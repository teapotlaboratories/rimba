/*
 * test-raw-rps — the ESP advertises a RAW schedule, and the chip acts on it
 *
 * Covers three stages of the RAW (802.11ah Restricted Access Window) AP-side port, with two different
 * instruments that must not be confused:
 *
 *  S0b-3/S0b-4 (Q2)  Do the RPS element (EID 208) and the S1G Capabilities RAW Operation Support bit
 *                    reach the air BYTE-IDENTICAL to what a live Linux hostapd_s1g AP transmits, i.e.
 *                    does the MM6108 blob rewrite IEs in a host-supplied beacon? Evidence is OFF-AIR:
 *                    capture on chronium's morse0 and decode with tools/raw_s1g_beacon_decode.py.
 *
 *  S0b-5 (Q3b)       Does the chip's RAW engine ARM and GATE on an ESP-authored schedule? A capture can
 *                    only ever show what was ADVERTISED, so this comes from the chip's own firmware
 *                    counters -- mmwlan_get_morse_stats(core 1 = MAC), TLV tag 4210 -- reported on this
 *                    console.
 *
 *  S1 + S3           The element is now produced by a real encoder ported from morse_driver's raw.c
 *                    (morselib umac/ies/ie_rps.c), driven by ordinary AP configuration on
 *                    mmwlan_ap_args rather than a compile-time spike. RAW is OFF unless an application
 *                    asks for it, which is why no #ifdef guards it any more: MMWLAN_AP_ARGS_INIT zeroes
 *                    the config, so every other app is unaffected by construction.
 *
 * The encoder self-test below runs before any radio work and is the cheapest of the three checks.
 */

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "nvs_flash.h"

#include "mmhalow.h"
#include "mmwlan.h"

#include "test_report.h"

#define NAME  "raw-rps"
#define RIG   "one ESP board as AP + chronium morse0 monitor; a Linux STA for the S0b-5b load arm"

#define AP_SSID        "rimba-rawrps"
#define AP_PSK         "rimbahalow"
#define AP_S1G_CHAN    27
#define AP_OP_CLASS    68

/* Pinned so the capture is comparable with the Linux reference AP, whose tracked
 * docs/reference/captures/hostapd-rimba.conf uses beacon_int=100. */
#define AP_BEACON_INT_TUS  100

/* DELIBERATE DIVERGENCE from the reference AP's dtim_period=1, and the reason matters. The design doc
 * requires classifying the RPS per frame across a full DTIM cycle, because Linux's short-beacon IE
 * allowlist DOES include RPS -- so a capture that only sampled DTIM beacons could report Q2=PASS while
 * non-DTIM beacons carried nothing. With dtim_period=1 every beacon is a DTIM beacon and that check is
 * unrunnable. 3 puts both classes in one capture. Safe to diverge because Q2 is a byte-diff of an
 * element, not a timing measurement -- DTIM period does not enter the RPS bytes. */
#define AP_DTIM_PERIOD     3

/* Cap TX power as the other on-air fixtures do. The bench's documented RX-overload signature at
 * RSSI ~ -3 dBm produces retries; the reference captures sat at -36 dBm and that is the target. */
#define AP_TX_POWER_DBM    1

/* The reference AP's RAW schedule, so the emitted element stays byte-diffable against a live Linux
 * AP: one GENERIC assignment, AIDs 1-255, 2 slots of 10100 us, no start-time offset. */
#define AP_RAW_ENABLED     true
#define AP_RAW_START_AID   1
#define AP_RAW_END_AID     255
#define AP_RAW_NUM_SLOTS   2
#define AP_RAW_SLOT_US     10100

/* S0b-5 measurement window. 60 s at beacon_int 100 TU (102.4 ms) is ~586 beacons, comfortably enough to
 * tell "~1 assignment per beacon" from noise, and the run reports cumulative windows. */
#define RAW_WINDOW_MS      60000
/* Long enough to associate a Linux STA and run a ping flood inside the run (S0b-5b) without racing it. */
#define RAW_N_WINDOWS      10

/* S0b-5b needs the AP to TRANSMIT, not just beacon. aci_frames_delayed counts the local transmitter
 * withholding its OWN frames, so a ping flood at an AP with no IP produces pure uplink and leaves every
 * AP-side deferral counter at zero -- the run would measure nothing. A static IP lets the AP answer ICMP,
 * which is the downlink that gets deferred. */
#define AP_IP              "192.168.12.1"
#define AP_NETMASK         "255.255.255.0"

/* ---------------------------------------------------------------- S1: the RPS encoder self-test
 *
 * S1 replaced a hardcoded eight-byte RPS with a real encoder ported from morse_driver's raw.c. That
 * swap is exactly the kind of change that regresses silently: the element is still emitted, still the
 * right length, and only some field is subtly wrong -- which on air looks like a working AP right up
 * until a STA ignores the schedule.
 *
 * So the encoder is checked against bytes a live Linux AP actually transmitted. Those bytes are held
 * HERE rather than in morselib: if the expected value lived next to the encoder, the test would compare
 * the encoder against itself and pass for any self-consistent encoding.
 *
 * Derivation, for one GENERIC assignment with start_aid=1, end_aid=255, start_time 0, 2 slots of
 * 10100 us:
 *   D0     Element ID 208 (RPS)
 *   06     Length: assignment(3) + group(3); start time omitted because the offset is 0
 *   20     RAW Control: type GENERIC(0), GROUP_IND set unconditionally
 *   40 09  Slot Definition LE: cslot 80 (500 + 80*120 = 10100 us), 2 slots, format 0
 *   04 E0  RAW Group LE: page 0, start AID 1, low bits of end AID 255
 *   1F     end_aid >> 3
 */
extern uint32_t mmwlan_rps_build_reference(uint8_t *out, uint32_t out_len);

static const uint8_t k_rps_golden[8] = { 0xD0, 0x06, 0x20, 0x40, 0x09, 0x04, 0xE0, 0x1F };

static bool rps_encoder_selftest(void)
{
    uint8_t built[32] = { 0 };
    char got[3 * sizeof(built) + 1];
    char want[3 * sizeof(k_rps_golden) + 1];
    uint32_t len = mmwlan_rps_build_reference(built, sizeof(built));

    for (uint32_t i = 0; i < len && i < sizeof(built); i++)
    {
        snprintf(got + 3 * i, 4, "%02X ", built[i]);
    }
    got[len ? 3 * len - 1 : 0] = '\0';
    for (size_t i = 0; i < sizeof(k_rps_golden); i++)
    {
        snprintf(want + 3 * i, 4, "%02X ", k_rps_golden[i]);
    }
    want[3 * sizeof(k_rps_golden) - 1] = '\0';

    bool ok = (len == sizeof(k_rps_golden)) && (memcmp(built, k_rps_golden, len) == 0);

    TEST_INFO("RPS encoder built %lu bytes: %s", (unsigned long)len, got);
    TEST_INFO("live-Linux reference  : %s", want);
    TEST_STEP("rps-encoder-golden", ok,
              "ie_rps_build() output is byte-identical to the RPS a live Linux AP transmits");
    return ok;
}

/* ---------------------------------------------------------------- S0b-5: the chip's RAW counters
 *
 * mmwlan_get_morse_stats(core_num, reset) returns a malloc'd {buf,len} blob of the transceiver's own
 * statistics, freed with mmwlan_free_morse_stats(). core_num selects the core and the mapping is in the
 * driver (mmdrv_get_stats): 0 = HOST, 1 = MAC, 2 = UPHY. The RAW counters are MAC stats, so core 1. It
 * needs the radio started -- the driver returns MMWLAN_NOT_RUNNING otherwise, matching the Linux side
 * where morse_cli stats fails with the link down.
 *
 * The blob is a flat TLV sequence: u16 little-endian tag, u16 little-endian length, then the value.
 * (Same 4-byte convention morselib's own umac_stats.c append_tlv uses, and what morse_cli walks.)
 *
 * The tag numbers and value types below are NOT guesses and NOT copied from a doc: they were read out
 * of the firmware's own descriptor table, section .mac_offchip_stats in vendor/morse-firmware's ELF --
 * 164 records of {char type_str[50]; char name[50]; char key[100]; u32 format; u16 tag;}. That table is
 * how morse_cli resolves a tag to a name, and it puts tag 4210 as type "raw_stats_t", key "RAW".
 *
 * raw_stats_t is 15 x le32 = 60 bytes (morse_cli stats_format.h): assignments[8], then
 * assignments_truncated_from_tbtt, invalid_assignments, already_past_assignment, aci_frames_delayed,
 * bc_mc_frames_delayed, abs_frames_delayed, frame_crosses_slot_delayed.
 *
 * ⚠ WHY THIS WALKER REFUSES TO GUESS. If the tag or the offsets were wrong we would read zeros from the
 * wrong place, and "all counters zero" is exactly the reading that means "the RAW engine never armed" --
 * a false BLOCKED on the one question the stage exists to answer. So a missing tag 4210, or one whose
 * length is not 60, is reported as an ERROR with the full tag inventory dumped, never as zeros.
 */
#define STATS_CORE_MAC          1
#define STATS_TLV_HDR_LEN       4

#define TAG_RAW                 4210    /* raw_stats_t, 60 B */
#define RAW_STATS_LEN           60
#define RAW_N_ASSIGNMENTS       8

struct tag_label
{
    uint16_t tag;
    const char *key;
};

/* TX + contention counters. Tag 4133 "TX Total" is the denominator for the deferral fraction; 4159 and
 * 4105 are direct contention observables, so a contention model can be calibrated rather than assumed. */
static const struct tag_label k_tx_tags[] = {
    { 4133, "TX Total" },
    { 4157, "TX requests" },
    { 4159, "TX average backoff slots" },
    { 4105, "DCF medium busy before TX" },
    { 4106, "DCF phy blocked before TX" },
    { 4143, "TX ACK valid" },
    { 4144, "TX ACK timeout" },
    { 4150, "TX ACK lost" },
};

/* The beacon-timing control. The RAW window is referenced to the END of the beacon carrying the RPS, so
 * a late or missing host-served beacon corrupts the time reference even with a perfect engine. Without
 * these, "the ESP cannot serve beacons punctually enough for RAW" is indistinguishable from "the FW has
 * no AP-direction engine" -- and only the first is fixable. Tag 4170 is the denominator that turns
 * assignments[] into a per-beacon rate instead of a bare count. */
static const struct tag_label k_beacon_tags[] = {
    { 4170, "Beacons TX" },
    { 4171, "Beacons missing from host" },
    { 4172, "Beacons late from host delay (average usec)" },
    { 4173, "Beacons late from host max delay (usec)" },
    { 4174, "Beacons late from host count (packets)" },
    { 4175, "Beacons expired before queuing" },
    { 4176, "Beacons TX queued late" },
    { 4177, "Beacons TX attempts delayed" },
    { 4178, "Beacons TX late" },
    { 4179, "Beacons TX failed count" },
    { 4180, "Beacons TX lifetime expired" },
};

static uint32_t rd_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Widths vary in this table (uint16_t/uint32_t/uint64_t all appear), so read by length, never assume 4. */
static uint64_t rd_uint(const uint8_t *p, uint16_t len)
{
    uint64_t v = 0;
    for (uint16_t i = 0; i < len && i < 8; i++)
    {
        v |= (uint64_t)p[i] << (8 * i);
    }
    return v;
}

/* Find a TLV by tag. Returns the value pointer and writes its length, or NULL if absent. */
static const uint8_t *tlv_find(const uint8_t *buf, uint32_t buf_len, uint16_t want, uint16_t *out_len)
{
    uint32_t off = 0;
    while (off + STATS_TLV_HDR_LEN <= buf_len)
    {
        uint16_t tag = (uint16_t)buf[off] | ((uint16_t)buf[off + 1] << 8);
        uint16_t len = (uint16_t)buf[off + 2] | ((uint16_t)buf[off + 3] << 8);
        off += STATS_TLV_HDR_LEN;
        if (len == 0 || off + len > buf_len)
        {
            break;      /* malformed -- stop rather than walk off the end */
        }
        if (tag == want)
        {
            *out_len = len;
            return buf + off;
        }
        off += len;
    }
    return NULL;
}

static void dump_tag_inventory(const uint8_t *buf, uint32_t buf_len)
{
    uint32_t off = 0;
    int n = 0;
    while (off + STATS_TLV_HDR_LEN <= buf_len)
    {
        uint16_t tag = (uint16_t)buf[off] | ((uint16_t)buf[off + 1] << 8);
        uint16_t len = (uint16_t)buf[off + 2] | ((uint16_t)buf[off + 3] << 8);
        off += STATS_TLV_HDR_LEN;
        if (len == 0 || off + len > buf_len)
        {
            TEST_INFO("  TLV walk stopped at offset %lu: tag=%u len=%u (malformed or truncated)",
                      (unsigned long)(off - STATS_TLV_HDR_LEN), tag, len);
            break;
        }
        TEST_INFO("  tag=%-5u len=%-3u", tag, len);
        off += len;
        n++;
    }
    TEST_INFO("  (%d TLVs in %lu bytes)", n, (unsigned long)buf_len);
}

/* Reads the MAC stats and reports the RAW + beacon-timing counters. `reset` zeroes them after reading,
 * which is how a clean measurement window is established. Returns false if the read itself failed. */
static bool report_raw_counters(const char *label, bool reset, bool inventory)
{
    struct mmwlan_morse_stats *stats = mmwlan_get_morse_stats(STATS_CORE_MAC, reset);
    if (stats == NULL)
    {
        TEST_INFO("[%s] mmwlan_get_morse_stats(core %d) returned NULL -- the read FAILED. This is not "
                  "a zero reading: no conclusion about the RAW engine may be drawn from it.",
                  label, STATS_CORE_MAC);
        return false;
    }

    /* ⚠ stats->len OVERSTATES the TLV payload by 4. mmdrv_get_stats sets buf = resp->data (already past
     * the 4-byte resp->status) but len = resp->hdr.len, and hdr.len counts everything after the 12-byte
     * command header -- status included. driver.c synthesises an empty response as
     * hdr.len = sizeof(resp->status), which pins it: the payload is hdr.len - 4. Walking the full
     * stats->len reads four bytes of the next field as a phantom TLV header. */
    uint32_t tlv_len = (stats->len >= sizeof(uint32_t)) ? stats->len - sizeof(uint32_t) : 0;

    TEST_INFO("[%s] MAC stats blob: %lu bytes reported, %lu bytes of TLV payload",
              label, (unsigned long)stats->len, (unsigned long)tlv_len);
    if (inventory)
    {
        dump_tag_inventory(stats->buf, tlv_len);
    }

    uint16_t len = 0;
    const uint8_t *raw = tlv_find(stats->buf, tlv_len, TAG_RAW, &len);
    if (raw == NULL)
    {
        TEST_INFO("[%s] *** TAG %d (RAW) ABSENT from the stats blob *** -- the firmware did not emit "
                  "the RAW counter set. Do NOT read this as 'the engine did not arm'; it means the "
                  "instrument is missing. Tag inventory follows.", label, TAG_RAW);
        dump_tag_inventory(stats->buf, tlv_len);
        mmwlan_free_morse_stats(stats);
        return false;
    }
    if (len != RAW_STATS_LEN)
    {
        TEST_INFO("[%s] *** TAG %d has length %u, expected %d *** -- raw_stats_t layout differs from "
                  "the firmware descriptor table this was built against. Field offsets would be wrong, "
                  "so no values are reported.", label, TAG_RAW, len, RAW_STATS_LEN);
        mmwlan_free_morse_stats(stats);
        return false;
    }

    uint32_t a[RAW_N_ASSIGNMENTS];
    for (int i = 0; i < RAW_N_ASSIGNMENTS; i++)
    {
        a[i] = rd_le32(raw + 4 * i);
    }
    uint32_t truncated  = rd_le32(raw + 32);
    uint32_t invalid    = rd_le32(raw + 36);
    uint32_t past       = rd_le32(raw + 40);
    uint32_t aci_delay  = rd_le32(raw + 44);
    uint32_t bcmc_delay = rd_le32(raw + 48);
    uint32_t abs_delay  = rd_le32(raw + 52);
    uint32_t crosses    = rd_le32(raw + 56);

    TEST_INFO("[%s] RAW assignments[] = %lu %lu %lu %lu %lu %lu %lu %lu", label,
              (unsigned long)a[0], (unsigned long)a[1], (unsigned long)a[2], (unsigned long)a[3],
              (unsigned long)a[4], (unsigned long)a[5], (unsigned long)a[6], (unsigned long)a[7]);
    TEST_INFO("[%s] RAW truncated_from_tbtt=%lu invalid=%lu already_past=%lu", label,
              (unsigned long)truncated, (unsigned long)invalid, (unsigned long)past);
    TEST_INFO("[%s] RAW delayed: aci=%lu bc_mc=%lu abs=%lu frame_crosses_slot=%lu", label,
              (unsigned long)aci_delay, (unsigned long)bcmc_delay, (unsigned long)abs_delay,
              (unsigned long)crosses);

    uint64_t tx_total = 0;
    for (size_t i = 0; i < sizeof(k_tx_tags) / sizeof(k_tx_tags[0]); i++)
    {
        uint16_t tlen = 0;
        const uint8_t *v = tlv_find(stats->buf, tlv_len, k_tx_tags[i].tag, &tlen);
        if (v == NULL)
        {
            TEST_INFO("[%s] tx tag %u (%s): ABSENT", label, k_tx_tags[i].tag, k_tx_tags[i].key);
            continue;
        }
        uint64_t val = rd_uint(v, tlen);
        if (k_tx_tags[i].tag == 4133)
        {
            tx_total = val;
        }
        TEST_INFO("[%s] %s = %llu", label, k_tx_tags[i].key, (unsigned long long)val);
    }

    if (tx_total > 0)
    {
        TEST_INFO("[%s] ==> aci_frames_delayed/TX Total = %lu/%llu = %lu.%02lu %%",
                  label, (unsigned long)aci_delay, (unsigned long long)tx_total,
                  (unsigned long)((uint64_t)aci_delay * 100 / tx_total),
                  (unsigned long)(((uint64_t)aci_delay * 10000 / tx_total) % 100));
    }

    uint64_t beacons_tx = 0;
    for (size_t i = 0; i < sizeof(k_beacon_tags) / sizeof(k_beacon_tags[0]); i++)
    {
        uint16_t blen = 0;
        const uint8_t *v = tlv_find(stats->buf, tlv_len, k_beacon_tags[i].tag, &blen);
        if (v == NULL)
        {
            TEST_INFO("[%s] beacon tag %u (%s): ABSENT", label, k_beacon_tags[i].tag,
                      k_beacon_tags[i].key);
            continue;
        }
        uint64_t val = rd_uint(v, blen);
        if (k_beacon_tags[i].tag == 4170)
        {
            beacons_tx = val;
        }
        TEST_INFO("[%s] %s = %llu", label, k_beacon_tags[i].key, (unsigned long long)val);
    }

    /* The headline number. On the Linux arm assignments[0] climbed at roughly one per beacon, which is
     * what "the chip parsed the RPS out of its own outgoing beacon" looks like. Tag 4170 gives the exact
     * denominator instead of inferring it from elapsed time. */
    if (beacons_tx > 0)
    {
        TEST_INFO("[%s] ==> assignments[0]/Beacons TX = %lu/%llu", label,
                  (unsigned long)a[0], (unsigned long long)beacons_tx);
    }

    mmwlan_free_morse_stats(stats);
    return true;
}

/* Pin a static IP so the AP answers ICMP. Modelled on rimba-halow-ap: mmhalow's netif is a DHCP client,
 * and in AP mode mmhalow never fires a link-up event, so the netif has to be brought up by hand or lwIP
 * silently never answers. */
static void assign_static_ip(void)
{
    esp_netif_t *n = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (n == NULL)
    {
        TEST_INFO("netif WIFI_STA_DEF not found -- the AP will not answer ICMP, so the S0b-5b "
                  "deferral counters cannot move. S0b-5a is unaffected.");
        return;
    }
    esp_netif_dhcpc_stop(n);

    esp_netif_ip_info_t ip = { 0 };
    ip.ip.addr = esp_ip4addr_aton(AP_IP);
    ip.gw.addr = esp_ip4addr_aton(AP_IP);
    ip.netmask.addr = esp_ip4addr_aton(AP_NETMASK);
    ESP_ERROR_CHECK(esp_netif_set_ip_info(n, &ip));
    esp_netif_action_connected(n, NULL, 0, NULL);

    TEST_INFO("AP static IP %s netmask %s, netif up=%d -- answers ICMP, so a STA ping flood produces "
              "the AP downlink that aci_frames_delayed counts",
              AP_IP, AP_NETMASK, (int)esp_netif_is_netif_up(n));
}

static void park_forever(void)
{
    while (1)
    {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

void app_main(void)
{
    vTaskDelay(pdMS_TO_TICKS(500));
    TEST_BEGIN(NAME, RIG);
    TEST_INFO("RAW via mmwlan_ap_args (S3): enabled=%d AIDs %d-%d, %d slots of %d us",
              AP_RAW_ENABLED, AP_RAW_START_AID, AP_RAW_END_AID, AP_RAW_NUM_SLOTS, AP_RAW_SLOT_US);

    /* Run before any radio work: it needs no hardware, and if the encoder is wrong there is no
     * point spending a bench slot capturing its output. */
    rps_encoder_selftest();

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_netif_init());

    mmhalow_init(NULL);
    mmhalow_print_version_info();

    mmhalow_wifi_config_t cfg = { .ap = MMWLAN_AP_ARGS_INIT };
    memcpy((char *)cfg.ap.ssid, AP_SSID, strlen(AP_SSID));
    cfg.ap.ssid_len = strlen(AP_SSID);
    memcpy(cfg.ap.passphrase, AP_PSK, strlen(AP_PSK));
    cfg.ap.passphrase_len = strlen(AP_PSK);
    cfg.ap.security_type = MMWLAN_SAE;
    cfg.ap.pmf_mode = MMWLAN_PMF_REQUIRED;
    cfg.ap.s1g_chan_num = AP_S1G_CHAN;
    cfg.ap.op_class = AP_OP_CLASS;
    cfg.ap.max_stas = 4;
    cfg.ap.beacon_interval_tus = AP_BEACON_INT_TUS;
    cfg.ap.dtim_period = AP_DTIM_PERIOD;

    /* S3: RAW is now ordinary AP configuration rather than a compile-time spike. It is off unless
     * asked for (MMWLAN_AP_ARGS_INIT zeroes it), so every other app is unaffected without needing a
     * guard. This schedule reproduces the Linux reference AP's, keeping the element byte-diffable. */
    cfg.ap.raw.enabled = AP_RAW_ENABLED;
    cfg.ap.raw.start_aid = AP_RAW_START_AID;
    cfg.ap.raw.end_aid = AP_RAW_END_AID;
    cfg.ap.raw.num_slots = AP_RAW_NUM_SLOTS;
    cfg.ap.raw.slot_duration_us = AP_RAW_SLOT_US;
    cfg.ap.raw.cross_slot_boundary = false;
    cfg.ap.raw.start_time_us = 0;

    if (mmhalow_set_config(WIFI_IF_AP, &cfg) != ESP_OK)
    {
        TEST_INCONCLUSIVE("mmhalow_set_config(AP) failed -- no beacons, so nothing to capture");
        TEST_END(NAME);
        park_forever();
    }

    /* Strictly between set_config and wifi_start, as the working precedent does (test-apsta-ap): the
     * override can only REDUCE below the regulatory max and returns MMWLAN_UNAVAILABLE once the WLAN
     * subsystem is active, so calling it after start would silently do nothing. */
    if (mmwlan_override_max_tx_power(AP_TX_POWER_DBM) != MMWLAN_SUCCESS)
    {
        TEST_INFO("mmwlan_override_max_tx_power(%d) REFUSED -- the AP is at the regulatory max. Read "
                  "the RSSI in the capture before trusting it; ~-3 dBm is the RX-overload signature",
                  AP_TX_POWER_DBM);
    }

    mmhalow_wifi_start();
    assign_static_ip();

    /* Let the vif come up before reporting the MAC: the capture is filtered on it, and a run whose
     * source address the operator guessed is a run that can attribute the wrong beacons to the ESP.
     *
     * ⚠ THE BEACONS DO NOT CARRY THE CHIP MAC. The AP vif transmits from the locally-administered
     * variant -- bit 1 of octet 0 set -- so a chip MAC of 68:24:99:44:6b:b7 beacons as
     * 6a:24:99:44:6b:b7 (observed on air 2026-07-31, and mmwlan_get_mac_addr reports the former).
     * Filtering a capture on the chip MAC returns zero frames, which reads exactly like "the ESP
     * never beaconed" -- so print BOTH and say plainly which one the decoder wants. */
    vTaskDelay(pdMS_TO_TICKS(3000));

    uint8_t mac[6] = { 0 };
    if (mmwlan_get_mac_addr(mac) == MMWLAN_SUCCESS)
    {
        uint8_t sa[6];
        memcpy(sa, mac, sizeof(sa));
        sa[0] |= 0x02;

        TEST_INFO("chip MAC %02x:%02x:%02x:%02x:%02x:%02x -- NOT what the beacons carry",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        TEST_INFO("beacon SA %02x:%02x:%02x:%02x:%02x:%02x <-- FILTER THE CAPTURE ON THIS",
                  sa[0], sa[1], sa[2], sa[3], sa[4], sa[5]);
        TEST_INFO("ssid=\"%s\" chan=%d beacon_int=%d TU dtim=%d tx_power=%d dBm",
                  AP_SSID, AP_S1G_CHAN, AP_BEACON_INT_TUS, AP_DTIM_PERIOD, AP_TX_POWER_DBM);
        TEST_INFO("next: sudo tcpdump -i morse0 -w cap.pcap on chronium, then "
                  "python3 tools/raw_s1g_beacon_decode.py cap.pcap "
                  "--sa %02x:%02x:%02x:%02x:%02x:%02x --beacon-int-tu %d",
                  sa[0], sa[1], sa[2], sa[3], sa[4], sa[5], AP_BEACON_INT_TUS);
    }
    else
    {
        TEST_INFO("could not read the AP MAC -- run the decoder without --sa and identify the ESP by "
                  "its IE chain (it carries SSID \"%s\" first, unlike a Linux AP)", AP_SSID);
    }

    /* Scored on "is this board beaconing", which is the only precondition the capture has. The verdict
     * itself is off-air and belongs to the decoder, so there is deliberately no TEST| gate on the RPS. */
    TEST_STEP("ap-beaconing", true,
              "AP vif up on ch%d; capture on chronium morse0 and decode with "
              "tools/raw_s1g_beacon_decode.py", AP_S1G_CHAN);

    /* ---- S0b-5 (Q3b): does the chip's RAW engine act on THIS board's schedule? ----
     *
     * Read once with reset=true to establish a clean window (and to prove the read path works at all,
     * with a full tag inventory), then again after a measurement window. Everything is cumulative from
     * the reset, so the second read IS the measurement.
     *
     * The zero-STA arm is deliberate and is the decisive one. On the Linux AP the engine armed on the
     * ADVERTISEMENT ALONE -- assignments climbing with every Delayed counter still at zero, before any
     * STA associated. So a board beaconing by itself already answers "does the engine arm from an
     * ESP-authored beacon"; associating a STA and driving traffic is what moves the deferral counters,
     * and that is a separate question about gating, not about arming. */
    TEST_INFO("S0b-5: reading MAC stats and resetting them to open the measurement window");
    bool read_ok = report_raw_counters("reset", true, true);
    TEST_STEP("stats-read", read_ok,
              "mmwlan_get_morse_stats(core %d) returned a parseable blob containing tag %d",
              STATS_CORE_MAC, TAG_RAW);

    if (!read_ok)
    {
        TEST_INCONCLUSIVE("the counter read failed or tag %d was absent -- the instrument is not "
                          "working, so Q3b is NOT evaluable. This is not a negative result.", TAG_RAW);
        TEST_END(NAME);
        park_forever();
    }

    /* Report cumulatively so the operator can watch it climb rather than waiting on one number, and so
     * a run that is interrupted still yields data. */
    for (int window = 1; window <= RAW_N_WINDOWS; window++)
    {
        vTaskDelay(pdMS_TO_TICKS(RAW_WINDOW_MS));
        char label[16];
        snprintf(label, sizeof(label), "t+%ds", window * (RAW_WINDOW_MS / 1000));
        report_raw_counters(label, false, false);
    }

    TEST_INFO("S0b-5 verdict rules: assignments[] climbing at ~1 per beacon with invalid=0 means the "
              "chip parsed the RPS out of its OWN outgoing beacon => the engine arms with the ESP as "
              "AP. assignments[] flat with the element provably on air and beacon lateness small is a "
              "host-format mismatch, NOT a blocked verdict. assignments[] flat with beacon lateness "
              "near a slot duration is a beacon-punctuality problem, which is a different and more "
              "fixable one.");

    TEST_END(NAME);
    park_forever();
}
