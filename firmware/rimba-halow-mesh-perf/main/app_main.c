/*
 * rimba-halow-mesh — 802.11s mesh bring-up (P1: mesh vif up + beacon).
 *
 * Brings up a single 802.11s mesh interface and self-beacons a mesh beacon
 * (zero-length SSID + Mesh ID + Mesh Configuration IEs), following morse_driver's
 * mesh BSS config flow (BSS_CONFIG -> BSSID_SET -> BSS_BEACON_CONFIG ->
 * MESH_CONFIG(START)). Peering / HWMP / forwarding come in later phases.
 *
 * Test: flash this on two boards. Each derives a unique MAC from its ESP32 efuse
 * MAC, beacons on the mesh channel, and the peer-beacon sniffer below logs the
 * OTHER node's beacon — proving the firmware beacons on air. See
 * docs/worklog/2026-06-25-mesh-p1-vif-beacon.md (P1).
 */

#include <inttypes.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"   /* esp_reset_reason (RTC-noinit cold-boot detection) */
#include "esp_attr.h"     /* RTC_NOINIT_ATTR (fwd-drop counters survive the crash-reboot) */
#include "nvs_flash.h"
#include "esp_netif.h"

#include "ping/ping_sock.h"
#include "lwip/etharp.h"

#include "mmhalow.h"
#include "umac/mesh/umac_mesh.h"

/* Keep in sync with the other rimba apps' link params. */
#define MESH_ID         "rimba-mesh"
#define MESH_S1G_CHAN   27   /* US 915.5 MHz, 1 MHz BW (global op-class 68) */
#define MESH_MAX_PLINKS 16

/* Linux-interop capture (temporary): bring this ESP up as an OPEN mesh peer (no allowlist) that
 * pings the Linux node (chronite, 10.9.9.2). Used to capture the Linux PREP/PERR on-air and to
 * verify the ESP's own PREQ/group frames against the live Linux device. Mutually exclusive with
 * the line-relay demo; uncomment for a Linux-interop on-air A/B capture. */
// #define MESH_LINUX_INTEROP 1

/* iperf throughput test (temporary): open peering (no allowlist), NO auto-ping, and an
 * esp_console UART REPL so `iperf -s` / `iperf -c <ip>` can be driven over the serial console.
 * Mesh IP is still pinned (10.9.9.<100+(mac[5]&0x3f)>). Mutually exclusive with the demos;
 * comment out for normal operation. */
#define MESH_IPERF 1

/* Multi-hop relay demo (temporary): force a 3-ESP line board1 -- board0(relay) -- board2 so
 * board0 must FORWARD board1<->board2 traffic (ESP as an intermediate hop). Each node sets a
 * peer allowlist by its own MAC. Comment out MESH_LINE_RELAY_DEMO for normal operation. */
#if !defined(MESH_LINUX_INTEROP) && !defined(MESH_IPERF)
#define MESH_LINE_RELAY_DEMO 1
#endif
/* For a multi-hop iperf measurement, MESH_IPERF reuses the line allowlist so board1<->board2
 * only reach each other via board0 (else, open-peering, they'd peer directly = single hop). */
#if defined(MESH_LINE_RELAY_DEMO) || defined(MESH_IPERF)
static const uint8_t MAC_B0[6] = { 0xe2, 0x72, 0xa1, 0xf8, 0xef, 0xa4 }; /* relay (10.9.9.136) */
static const uint8_t MAC_B1[6] = { 0xe2, 0x72, 0xa1, 0xf8, 0xf9, 0x40 }; /* endpoint (10.9.9.100) */
static const uint8_t MAC_B2[6] = { 0xe2, 0x72, 0xa1, 0xf8, 0xf0, 0x08 }; /* endpoint (10.9.9.108) */
static const uint8_t MAC_CHRONITE[6] __attribute__((unused)) =
    { 0x3c, 0x22, 0x7f, 0x37, 0x51, 0x38 }; /* Linux far endpoint (10.9.9.2); unused in the all-ESP line */
#endif
/* Runtime ping target (NULL = don't ping: relay + responder roles). */
static const char *g_ping_target;
/* Static ARP for the far endpoint (broadcast ARP can't traverse the relay yet). */
static const char *g_static_arp_ip;
static const uint8_t *g_static_arp_mac;
/* TEMP 2026-07-12 — board0 auto-blasts a UDP iperf client to this target once peered, so the S3 relay
 * RX-drop read runs WITHOUT an ESP console (opening it warm-resets the board). NULL = no auto-blast. */
static const char *g_iperf_blast_target;

/* TEMP 2026-07-12 — forward/RX-drop instrumentation storage (declared extern in umac/mesh/umac_mesh.h;
 * morselib increments these). RTC_NOINIT_ATTR keeps them in RTC RAM, which survives a SW reset / panic
 * reboot (the INT-WDT relay crash-loop is a panic->reboot, not a power cycle), so the accumulated counts
 * can be read AFTER the relay crashes. They are NOT auto-zeroed at startup — g_mesh_fwd_dbg_magic is a
 * canary: on a true power-on (or when the canary is garbage after a cold boot) we zero the array so each
 * test session starts clean; on a crash reboot the canary matches and the counts accumulate across boots. */
#define MESH_FWD_DBG_MAGIC 0x53334448u /* "S3DH" — BUMP each session where board0/board1 can't POWERON-cycle.
                                        * The dev-host hub's per-port `disable` cuts USB DATA only, NOT VBUS:
                                        * board0/board1 stay powered across a "cycle", so a reflash boots via a
                                        * warm/SW reset (reset_reason != POWERON) and would PRESERVE a stale
                                        * RTC capture (incl. a prior CCMP-ENC key + the "keep-first-only" latch,
                                        * which silently corrupts a fresh dual-side key read). A magic mismatch
                                        * vs the RTC-stored value is the only reliable force-zero for those
                                        * boards — bump this whenever you need a guaranteed-clean session on
                                        * board0/board1. board2 (PPK2) does POWERON-cycle, so it zeros anyway. */
RTC_NOINIT_ATTR uint32_t g_mesh_fwd_dbg[FDBG_COUNT];
RTC_NOINIT_ATTR uint32_t g_mesh_fwd_dbg_magic;

/* TEMP 2026-07-12 — FIX-1 verification telemetry (RTC_NOINIT; morselib increments attempts/completions via
 * the externs in umac_mesh.h). boot_count bumps each app_main entry — a crash-reboot loop climbs it, a
 * crash-free soft restart does not. See the DEFINITIVE-test note in umac_mesh.h. */
RTC_NOINIT_ATTR uint32_t g_hwr_boot_count;
RTC_NOINIT_ATTR uint32_t g_hwr_attempts;
RTC_NOINIT_ATTR uint32_t g_hwr_completions;

/* TEMP 2026-07-12 — mesh-plink flap telemetry (RTC_NOINIT; morselib increments via umac_mesh.h externs).
 * g_plink_estab==1 over a run => STABLE peer; climbing => FLAPPING. Used for the ESP<->ESP peering test. */
RTC_NOINIT_ATTR uint32_t g_plink_estab;
RTC_NOINIT_ATTR uint32_t g_plink_close;

/* TEMP 2026-07-12 — dual-side CCMP AAD/nonce/key capture for the multi-hop MIC failure. morselib
 * (ccmp.c mesh_ccmp_encrypt/decrypt) captures the FIRST multi-hop (A3!=A1) frame's crypto inputs into
 * these RTC buffers via mmwlan_ccmp_dbg_capture(). Layout: [0]=valid [1]=aad_len [2..9]=key[0..7]
 * [10..22]=nonce(13) [23..52]=aad(30). board0 populates _enc (encrypt), board2 populates _dec (decrypt
 * MIC-fail). Diff the two -> the differing byte pins the discrepancy. */
RTC_NOINIT_ATTR uint8_t g_ccmp_enc[56];
RTC_NOINIT_ATTR uint8_t g_ccmp_dec[56];
void mmwlan_ccmp_dbg_capture(int is_enc, const uint8_t *key, const uint8_t *aad, uint32_t aad_len,
                             const uint8_t *nonce)
{
    uint8_t *b = is_enc ? g_ccmp_enc : g_ccmp_dec;
    if (b[0]) { return; } /* keep the first capture only */
    b[0] = 1;
    b[1] = (uint8_t)aad_len;
    memcpy(&b[2], key, 8);
    memcpy(&b[10], nonce, 13);
    memcpy(&b[23], aad, aad_len > 30 ? 30 : aad_len);
}

/* 802.11 element IDs + beacon IE offset (24-byte PV0 header + ts/bcn-int/cap = 12). */
#define DOT11_IE_MESH_ID        (114)
#define BEACON_IE_OFFSET        (24 + 12)

static const char *TAG = "rimba-mesh";

/* This node's mesh MAC, derived from the ESP32 efuse MAC (unique per board). */
static uint8_t g_mesh_mac[6];

/* ---- peer-beacon RX sniffer (P1 on-air verification; basis for P2 peering) -----
 * morselib's raw-frame capture hook is exported via the mmwlan* symbol rule; the
 * API lives in src/internal, so declare it locally (layout must match
 * mmwlan_internal.h, same as firmware/rimba-halow-scan). */
enum mmwlan_frame_filter_flag {
    MMWLAN_FRAME_BEACON = 1 << 8,
};
struct mmwlan_rx_frame_info {
    enum mmwlan_frame_filter_flag frame_filter_flag;
    const uint8_t *buf;
    uint32_t buf_len;
    uint16_t freq_100khz;
    int16_t rssi_dbm;
    uint8_t bw_mhz;
};
typedef void (*mmwlan_rx_frame_cb_t)(const struct mmwlan_rx_frame_info *rx_info, void *arg);
extern enum mmwlan_status mmwlan_register_rx_frame_cb(uint32_t filter,
                                                      mmwlan_rx_frame_cb_t callback, void *arg);

static const uint8_t *find_ie(const uint8_t *ies, uint32_t len, uint8_t want, uint8_t *out_len)
{
    uint32_t i = 0;
    while (i + 2 <= len) {
        uint8_t id = ies[i], ie_len = ies[i + 1];
        if (i + 2 + ie_len > len) break;
        if (id == want) { *out_len = ie_len; return &ies[i + 2]; }
        i += 2 + ie_len;
    }
    return NULL;
}

static void peer_beacon_cb(const struct mmwlan_rx_frame_info *info, void *arg)
{
    if (info->buf_len <= BEACON_IE_OFFSET) return;
    const uint8_t *sa = info->buf + 10;   /* A2 / source address */
    if (memcmp(sa, g_mesh_mac, 6) == 0) return; /* ignore our own beacons */

    uint8_t id_len = 0;
    const uint8_t *id = find_ie(info->buf + BEACON_IE_OFFSET,
                                info->buf_len - BEACON_IE_OFFSET, DOT11_IE_MESH_ID, &id_len);
    bool ours = id && id_len == strlen(MESH_ID) && memcmp(id, MESH_ID, id_len) == 0;
    if (!id) return; /* not a mesh beacon */

    ESP_LOGI(TAG, "PEER MESH BEACON%s from %02x:%02x:%02x:%02x:%02x:%02x rssi=%d "
             "freq=%u00kHz MeshID=\"%.*s\"",
             ours ? " (rimba-mesh)" : "", sa[0], sa[1], sa[2], sa[3], sa[4], sa[5],
             info->rssi_dbm, info->freq_100khz, (int)id_len, (const char *)id);

    /* Same Mesh ID -> initiate a peer link (idempotent; morselib runs the MPM handshake
     * and ignores repeats once the peer is known). Mirrors mac80211 opening a plink on a
     * heard candidate beacon. */
    if (ours)
    {
        mmwlan_mesh_peer_open(sa);
    }
}

/* --- P4 data path bring-up: static mesh IP + ping a neighbour ------------------ */
static void on_ping_success(esp_ping_handle_t hdl, void *args)
{
    (void)args;
    uint16_t seqno; uint32_t elapsed_ms; ip_addr_t target = { 0 };
    esp_ping_get_profile(hdl, ESP_PING_PROF_SEQNO, &seqno, sizeof(seqno));
    esp_ping_get_profile(hdl, ESP_PING_PROF_TIMEGAP, &elapsed_ms, sizeof(elapsed_ms));
    esp_ping_get_profile(hdl, ESP_PING_PROF_IPADDR, &target, sizeof(target));
    ESP_LOGI(TAG, "reply from " IPSTR ": seq=%u time=%" PRIu32 " ms",
             IP2STR(&target.u_addr.ip4), seqno, elapsed_ms);
}

static void on_ping_timeout(esp_ping_handle_t hdl, void *args)
{
    (void)args;
    uint16_t seqno;
    esp_ping_get_profile(hdl, ESP_PING_PROF_SEQNO, &seqno, sizeof(seqno));
    ESP_LOGW(TAG, "ping timeout seq=%u", seqno);
}

/* Pin a static mesh IP once the netif is up (after a peer link establishes), then ping a
 * mesh neighbour (the Linux node 10.9.9.2) to exercise the mesh data path. */
static void mesh_net_task(void *arg)
{
    (void)arg;
    esp_netif_t *n = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (n == NULL) { ESP_LOGE(TAG, "mesh netif not found"); vTaskDelete(NULL); return; }

    for (int i = 0; i < 60 && !esp_netif_is_netif_up(n); i++) { vTaskDelay(pdMS_TO_TICKS(500)); }
    vTaskDelay(pdMS_TO_TICKS(500));

    esp_netif_dhcpc_stop(n);
    /* The mesh vif transmits with g_mesh_mac (the locally-administered mesh MAC); sync the
     * netif/ARP MAC to it so L2 (vif) and L3 (ARP) match — otherwise peers learn our IP against
     * the wrong MAC and replies never reach the mesh vif. */
    esp_netif_set_mac(n, g_mesh_mac);
    unsigned host = 100u + (g_mesh_mac[5] & 0x3fu); /* distinct per node, avoids .1/.2 */
    char ipbuf[20];
    snprintf(ipbuf, sizeof(ipbuf), "10.9.9.%u", host);
    const char *ping_to = g_ping_target ? g_ping_target : "10.9.9.2";
    esp_netif_ip_info_t ip = { 0 };
    ip.ip.addr = esp_ip4addr_aton(ipbuf);
    ip.gw.addr = esp_ip4addr_aton(ping_to);
    ip.netmask.addr = esp_ip4addr_aton("255.255.255.0");
    ESP_ERROR_CHECK(esp_netif_set_ip_info(n, &ip));
    ESP_LOGI(TAG, "mesh static IP %s (netif up=%d)%s%s", ipbuf, (int)esp_netif_is_netif_up(n),
             g_ping_target ? "; pinging " : " (no ping — relay/responder)",
             g_ping_target ? g_ping_target : "");

    if (g_static_arp_mac != NULL)
    {
        ip4_addr_t a = { .addr = esp_ip4addr_aton(g_static_arp_ip) };
        struct eth_addr e;
        memcpy(e.addr, g_static_arp_mac, 6);
        if (etharp_add_static_entry(&a, &e) == ERR_OK)
        {
            ESP_LOGI(TAG, "static ARP %s -> %02x:%02x:%02x:%02x:%02x:%02x", g_static_arp_ip,
                     g_static_arp_mac[0], g_static_arp_mac[1], g_static_arp_mac[2],
                     g_static_arp_mac[3], g_static_arp_mac[4], g_static_arp_mac[5]);
        }
    }

    if (g_ping_target == NULL)
    {
        vTaskDelete(NULL);
        return;
    }

    ip_addr_t target = { 0 };
    target.type = IPADDR_TYPE_V4;
    target.u_addr.ip4.addr = esp_ip4addr_aton(g_ping_target);
    esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
    cfg.target_addr = target;
    cfg.count = ESP_PING_COUNT_INFINITE;
    cfg.interval_ms = 1000;
    esp_ping_callbacks_t cbs = { .on_ping_success = on_ping_success,
                                 .on_ping_timeout = on_ping_timeout };
    esp_ping_handle_t ping;
    if (esp_ping_new_session(&cfg, &cbs, &ping) == ESP_OK) { esp_ping_start(ping); }
    else { ESP_LOGE(TAG, "ping session create failed"); }
    vTaskDelete(NULL);
}

#ifdef MESH_IPERF
#include "esp_console.h"
#include "iperf_cmd.h"
#include "iperf.h"        /* TEMP: iperf_start() for the board0 auto-blast (S3 relay RX-drop read) */
#include "ping_cmd.h"
/* Start an esp_console REPL on the console UART + register the `iperf` command (espressif/
 * iperf-cmd). Lets the host drive `iperf -s` / `iperf -c <ip>` over the serial port to measure
 * mesh goodput. Logs and the REPL share the console UART (as in the IDF iperf example). */
static void start_iperf_console(void)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = "iperf>";
#if CONFIG_ESP_CONSOLE_UART
    esp_console_dev_uart_config_t dev_config = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&dev_config, &repl_config, &repl));
#elif CONFIG_ESP_CONSOLE_USB_CDC
    esp_console_dev_usb_cdc_config_t dev_config = ESP_CONSOLE_DEV_CDC_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_cdc(&dev_config, &repl_config, &repl));
#elif CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    esp_console_dev_usb_serial_jtag_config_t dev_config = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_serial_jtag(&dev_config, &repl_config, &repl));
#endif
    app_register_iperf_commands();
    ping_cmd_register_ping();
    ESP_ERROR_CHECK(esp_console_start_repl(repl));
    ESP_LOGW(TAG, "iperf console ready: `iperf -s` (server) or `iperf -c <ip>` (client)");
}
#endif

/* TEMP 2026-07-12 — dump the forward/RX-drop counters (FWD-DBG + RX-DBG). `when` labels the call site
 * ("boot" = the surviving totals from before the crash-reboot; "5s" = the periodic in-run dump). */
static void dump_fwd_dbg(const char *when)
{
    uint32_t fd[FDBG_COUNT];
    mmwlan_mesh_get_fwd_dbg(fd);
    ESP_LOGW(TAG, "FWD-DBG[%s] rx_reached=%" PRIu32 " fwd_entry=%" PRIu32 " lookup_miss=%" PRIu32
                  " nhstad_null=%" PRIu32 " build_null=%" PRIu32 " keyed=%" PRIu32
                  " txready_to=%" PRIu32 " encfail=%" PRIu32 " mmdrv_fail=%" PRIu32 " mmdrv_ok=%" PRIu32,
             when, fd[FDBG_RX_FWD_REACHED], fd[FDBG_FWD_ENTRY], fd[FDBG_LOOKUP_MISS], fd[FDBG_NHSTAD_NULL],
             fd[FDBG_BUILD_NULL], fd[FDBG_KEYED_ENTRY], fd[FDBG_TXREADY_TIMEOUT], fd[FDBG_ENCRYPT_FAIL],
             fd[FDBG_MMDRV_FAIL], fd[FDBG_MMDRV_OK]);
    ESP_LOGW(TAG, "RX-DBG[%s] mesh_seen=%" PRIu32 " allowlist=%" PRIu32 " plaintext=%" PRIu32
                  " hw_ccmp_fail=%" PRIu32 " sw_ccmp_fail=%" PRIu32 " (mic=%" PRIu32 " replay=%" PRIu32
                  ") no_decrypt=%" PRIu32 " decrypt_ok=%" PRIu32,
             when, fd[FDBG_RX_MESH_SEEN], fd[FDBG_RX_ALLOWLIST], fd[FDBG_RX_PLAINTEXT], fd[FDBG_RX_HW_CCMP_FAIL],
             fd[FDBG_RX_SW_CCMP_FAIL], fd[FDBG_RX_SW_MIC_FAIL], fd[FDBG_RX_SW_REPLAY_FAIL],
             fd[FDBG_RX_NO_DECRYPT], fd[FDBG_RX_DECRYPT_OK]);
    ESP_LOGW(TAG, "TX-DBG[%s] multihop=%" PRIu32 " nh_null=%" PRIu32 " nh_eq_stad=%" PRIu32 " nh_ok=%" PRIu32
                  "  (nh_null/eq_stad => wrong per-link key)",
             when, fd[FDBG_TX_MULTIHOP], fd[FDBG_TX_NH_NULL], fd[FDBG_TX_NH_EQ_STAD], fd[FDBG_TX_NH_OK]);
    {
        static char cchx[170];
        for (int i = 0; i < 53; i++) { snprintf(&cchx[i * 3], 4, "%02x ", g_ccmp_enc[i]); }
        ESP_LOGW(TAG, "CCMP-ENC[%s] %s", when, cchx);
        for (int i = 0; i < 53; i++) { snprintf(&cchx[i * 3], 4, "%02x ", g_ccmp_dec[i]); }
        ESP_LOGW(TAG, "CCMP-DEC[%s] %s (layout: [0]valid [1]aadlen [2..9]key8 [10..22]nonce13 [23..52]aad30)",
                 when, cchx);
    }
    ESP_LOGW(TAG, "HWR-DBG[%s] boot_count=%" PRIu32 " restart_attempts=%" PRIu32 " restart_completions=%" PRIu32,
             when, g_hwr_boot_count, g_hwr_attempts, g_hwr_completions);
    ESP_LOGW(TAG, "PLINK-DBG[%s] estab=%" PRIu32 " close=%" PRIu32 "  (stable=1/0, flap=climbs)",
             when, g_plink_estab, g_plink_close);
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== 802.11s mesh bring-up (P1: vif + beacon) ===");

    /* TEMP 2026-07-12 — RTC-noinit fwd/RX-drop counters: zero them on a true power-on (or when the RTC
     * canary is garbage after a cold boot), else keep accumulating across the INT-WDT crash-reboot loop.
     * Then dump the SURVIVING totals immediately — so even a boot that crash-loops before the 5 s periodic
     * dump still surfaces the counts read from RTC RAM. reflash_hello (PPK2 power cycle) => power-on => clean. */
    esp_reset_reason_t rr = esp_reset_reason();
    if (rr == ESP_RST_POWERON || g_mesh_fwd_dbg_magic != MESH_FWD_DBG_MAGIC)
    {
        memset(g_mesh_fwd_dbg, 0, sizeof(g_mesh_fwd_dbg));
        g_mesh_fwd_dbg_magic = MESH_FWD_DBG_MAGIC;
        g_hwr_boot_count = 0;
        g_hwr_attempts = 0;
        g_hwr_completions = 0;
        g_plink_estab = 0;
        g_plink_close = 0;
        memset(g_ccmp_enc, 0, sizeof(g_ccmp_enc));
        memset(g_ccmp_dec, 0, sizeof(g_ccmp_dec));
        ESP_LOGW(TAG, "FWD-DBG counters ZEROED (reset_reason=%d, fresh session)", (int)rr);
    }
    else
    {
        ESP_LOGW(TAG, "FWD-DBG counters PRESERVED across reset_reason=%d (crash-reboot)", (int)rr);
    }
    g_hwr_boot_count++;   /* bumps each app_main entry: a crash-reboot loop climbs it; a soft restart does not */
    dump_fwd_dbg("boot");

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_netif_init());

    /* Unique per-board mesh MAC: ESP32 efuse MAC with the locally-administered bit
     * set (the MM6108 factory MAC is shared across modules, so it can't be used). */
    esp_read_mac(g_mesh_mac, ESP_MAC_WIFI_STA);
    g_mesh_mac[0] = (g_mesh_mac[0] | 0x02) & 0xFE;

    mmhalow_init(NULL);
    mmhalow_print_version_info();

    struct mmwlan_mesh_args args = { 0 };
    memcpy(args.if_addr, g_mesh_mac, sizeof(g_mesh_mac));
    memcpy(args.mesh_id, MESH_ID, strlen(MESH_ID));
    args.mesh_id_len = strlen(MESH_ID);
    args.s1g_chan_num = MESH_S1G_CHAN;
    args.beacon_interval_tu = 100;
    args.max_plinks = MESH_MAX_PLINKS;

    /* Forced-topology allowlist MUST be configured BEFORE mmwlan_mesh_start. mesh_start begins beaconing
     * and RX immediately; an empty allowlist (count==0) allows-all (mesh_peer_allowed()), so any neighbour
     * beacon/OPEN arriving in the window between start and a later allowlist call could form a direct plink
     * to a node we intend to reach multi-hop. That window WAS the S3 diagnosis confound: board0 would
     * sometimes peer chronite directly (all nodes in RF range) and grab a chronite MTK, muddying the
     * forward-key measurement. Setting the allowlist first closes the race — board0 can then ONLY reach
     * chronite via the board2 relay, giving a clean true-multi-hop line. */
#ifdef MESH_LINUX_INTEROP
    /* No allowlist -> peer with anyone (incl. the Linux node). Ping chronite so we originate a
     * PREQ for it (-> Linux PREP) and so chronite builds a path to us (-> Linux PERR on break). */
    g_ping_target = "10.9.9.2";
    ESP_LOGW(TAG, "MESH role: Linux interop -> open peering + ping chronite (10.9.9.2)");
#endif
#ifdef MESH_IPERF
    /* Line allowlist (board1 -- board0(relay) -- board2) so a board1->board2 iperf is forced
     * multi-hop via board0 (open peering would peer them directly = 1 hop). No auto-ping; the
     * IP is still pinned. Drive `iperf` over the console: board2 `iperf -s`, board1
     * `iperf -c 10.9.9.108`. A direct board0<->board1 iperf also works (they're allowed peers). */
    /* S3 bench-verify 2026-07-12 (fwd-drop instrumentation): RELAY=board2 (only fully-wired ESP); far
     * endpoint = chronite (Linux, peers reliably). Line: board0 (client, iperf -c 10.9.9.2) -> board2
     * (relay/DUT) -> chronite (Linux iperf -s). Revert to board0-relay after the test. */
    /* S3 RC-fix verify 2026-07-13: PURE ALL-ESP forced line board0 (client) -> board2 (relay/DUT) ->
     * board1 (far endpoint). ESP<->ESP peering is far more reliable than the cross-vendor ESP<->Linux
     * SAE (chronite would not re-peer), and this isolates the RC-on-key_stad fix cleanly: board0's
     * origin frames to board1 are multi-hop (A1=board2 != A3=board1), the exact A3!=A1 shape that was
     * MCS0-fragmented. Read board2 RX-DBG decrypt_ok. */
    if (memcmp(g_mesh_mac, MAC_B2, 6) == 0)
    {
        uint8_t allow[12];
        memcpy(allow, MAC_B0, 6);
        memcpy(allow + 6, MAC_B1, 6);
        mmwlan_mesh_set_peer_allowlist(allow, 2);
        ESP_LOGW(TAG, "MESH role: iperf RELAY (board2, fully-wired) board0 <-> board1 (all-ESP)");
    }
    else if (memcmp(g_mesh_mac, MAC_B0, 6) == 0)
    {
        mmwlan_mesh_set_peer_allowlist(MAC_B2, 1);
        /* Auto-blast board1 (10.9.9.100) via the board2 relay: board0->board2 is the A3!=A1 multi-hop
         * origin leg whose MCS0 fragmentation the RC fix targets. Static ARP so no broadcast ARP needed. */
        g_static_arp_ip = "10.9.9.100";
        g_static_arp_mac = MAC_B1;
        g_iperf_blast_target = "10.9.9.100";
        ESP_LOGW(TAG, "MESH role: iperf endpoint board0 -> `iperf -c 10.9.9.100` (board1 via board2)");
    }
    else if (memcmp(g_mesh_mac, MAC_B1, 6) == 0)
    {
        mmwlan_mesh_set_peer_allowlist(MAC_B2, 1); /* far endpoint: peers ONLY board2 (forced 2-hop) */
        ESP_LOGW(TAG, "MESH role: iperf FAR endpoint board1 (10.9.9.100) via board2");
    }
#endif
#ifdef MESH_LINE_RELAY_DEMO
    if (memcmp(g_mesh_mac, MAC_B0, 6) == 0)
    {
        uint8_t allow[12];
        memcpy(allow, MAC_B1, 6);
        memcpy(allow + 6, MAC_B2, 6);
        mmwlan_mesh_set_peer_allowlist(allow, 2);
        ESP_LOGW(TAG, "MESH role: RELAY (peers board1+board2, forwards between them)");
    }
    else if (memcmp(g_mesh_mac, MAC_B1, 6) == 0)
    {
        mmwlan_mesh_set_peer_allowlist(MAC_B0, 1);
        g_ping_target = "10.9.9.108"; /* board2, reachable only via board0 (ARP now relayed) */
        ESP_LOGW(TAG, "MESH role: endpoint board1 -> ping board2 (10.9.9.108) via relay");
    }
    else if (memcmp(g_mesh_mac, MAC_B2, 6) == 0)
    {
        mmwlan_mesh_set_peer_allowlist(MAC_B0, 1);
        ESP_LOGW(TAG, "MESH role: endpoint board2 (responder)");
    }
#endif

    ESP_LOGI(TAG, "Starting mesh (id=\"%s\" chan=%d mac=%02x:%02x:%02x:%02x:%02x:%02x)...",
             MESH_ID, MESH_S1G_CHAN, g_mesh_mac[0], g_mesh_mac[1], g_mesh_mac[2],
             g_mesh_mac[3], g_mesh_mac[4], g_mesh_mac[5]);
    enum mmwlan_status st = mmwlan_mesh_start(&args);
    if (st != MMWLAN_SUCCESS)
    {
        ESP_LOGE(TAG, "==> mmwlan_mesh_start FAILED status=%d", (int)st);
        return;
    }
    ESP_LOGI(TAG, "==> mesh vif up; firmware beaconing periodically on chan %d.", MESH_S1G_CHAN);

    /* Log any peer mesh beacons we hear on the channel. */
    mmwlan_register_rx_frame_cb(MMWLAN_FRAME_BEACON, peer_beacon_cb, NULL);

    /* P4: once a peer link is up, pin a static mesh IP and ping a neighbour. */
    xTaskCreate(mesh_net_task, "mesh_net", 4096, NULL, 5, NULL);

#ifdef MESH_IPERF
    start_iperf_console();
#endif

    /* P2: mesh peering (MPM) is handled in morselib — we answer received Mesh Peering
     * Open frames with Open+Confirm and reach ESTAB (see umac_mesh_handle_action). The
     * peer link forms automatically once a neighbour (e.g. a Linux mesh node) opens to us. */
    uint32_t s = 0;
#ifdef MESH_IPERF
    bool blast_started = false;
#endif
    for (;;)
    {
        vTaskDelay(pdMS_TO_TICKS(1000));
#ifdef MESH_IPERF
        /* TEMP 2026-07-12 — board0 auto-blast: once peered + IP settled (~15 s), fire ONE UDP iperf
         * client at chronite via the board2 relay so the S3 relay RX-drop counters accumulate WITHOUT an
         * ESP console (opening it warm-resets the board). RTC-noinit counters survive even if board2 crashes. */
        if (g_iperf_blast_target != NULL && !blast_started && s >= 15 &&
            mmwlan_mesh_peer_count(NULL) > 0)
        {
            blast_started = true;
            iperf_cfg_t cfg;
            memset(&cfg, 0, sizeof(cfg));
            cfg.type = IPERF_IP_TYPE_IPV4;
            cfg.flag = IPERF_FLAG_CLIENT | IPERF_FLAG_UDP;
            cfg.destination_ip4 = esp_ip4addr_aton(g_iperf_blast_target);
            cfg.sport = IPERF_DEFAULT_PORT;
            cfg.dport = IPERF_DEFAULT_PORT;
            cfg.interval = IPERF_DEFAULT_INTERVAL;
            cfg.time = 60;
            cfg.bw_lim = 5; /* Mbit/s — enough to congest the half-duplex relay */
            ESP_LOGW(TAG, "AUTO-BLAST: UDP iperf client -> %s @ 5 Mbit 60 s (S3 relay RX-drop read)",
                     g_iperf_blast_target);
            (void)iperf_start(&cfg);
        }
#endif
        if (++s % 5 == 0)
        {
            ESP_LOGI(TAG, "mesh alive, uptime=%" PRIu32 "s", s);
            /* TEMP 2026-07-12 — forward/RX-drop instrumentation dump (relay diagnosis). */
            dump_fwd_dbg("5s");
        }
    }
}
