/*
 * test-mesh-gate-linux — ESP discovers a LIVE Linux 802.11s GATE via RANN (S6 Case B / discovery interop).
 *
 * The ESP joins a real Linux HaLow mesh whose node is configured as a PROACTIVE_RANN gate
 * (mesh_hwmp_rootmode=4 + mesh_gate_announcements=1 + mesh_hwmp_rann_interval=5000), which the harness
 * brings up over ssh (tools/regtest/linux_peer.bring_up_gate). The ESP peers with the Linux node
 * (MPM + SAE + AMPE), then confirms it LEARNED that node as a gate (mmwlan_mesh_gate_count() > 0) —
 * i.e. the ESP received, accepted, and recorded the live Linux gate's RANN. This is the S6 discovery
 * interop: the receive mirror of Part 0 (the ESP gate emitting RANN) and the gate-aware sibling of the
 * mesh-linux peering test. This is the reporter role; the Linux gate is a support role. All ESP apps a
 * T2 test flashes are test-* (the split rule).
 *
 * Mesh params MUST match the Linux reference config (docs/reference/captures/wpa-smesh.conf):
 *   mesh ID "rimba-smesh", SAE password "rimbamesh2026" (compiled into morselib), S1G ch27.
 *
 * WHAT IT PROVES (S6): an ESP node interoperates with a live mac80211 gate's RANN — it peers with the
 * Linux stack AND its S2 RX path records that node as a gate (the gate list / Connected-to-Gate path).
 * WHAT IT DOES NOT PROVE: the AE datapath through the gate (that is test-mesh-ae + `iw mpp dump`), or
 * on-air RANN byte-equivalence (that is the chronium-monitor byte-diff the on-air rule points at).
 *
 * Assertion:
 *   PASS         = an ESTAB peer whose MAC == the Linux node's wlan1 MAC appears AND known_gates>0.
 *   FAIL         = peered with the Linux node, but known_gates stayed 0 within the window — the ESP
 *                  peered but never learned it as a gate (the RANN gate-discovery interop regressed).
 *   INCONCLUSIVE = never got the Linux plink (gate down / RF / rig), not a discovery regression.
 */

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "nvs_flash.h"
#include "esp_netif.h"

#include "mmhalow.h"
#include "mmwlan.h"
#include "umac/mesh/umac_mesh.h"

#include "test_report.h"

#define NAME            "mesh-gate-linux"
#define RIG             "ESP mesh node (reporter) + a Linux mesh GATE (chronite, rootmode 4) over ssh"
#define MESH_ID         "rimba-smesh"       /* MUST match the Linux wpa-smesh.conf */
#define MESH_S1G_CHAN   27
#define PEER_WAIT_S     50                  /* SAE+AMPE peering against mac80211 can be slow-cold */
#define GATE_WAIT_S     25                  /* the Linux gate RANNs every 5s; allow several + re-flood */

/* The Linux peer's MAC is a BUILD-TIME ARGUMENT (not baked into the source): the ESP needs it to
 * recognise the SPECIFIC peer. It arrives as the compile define TEST_LINUX_MAC, which the harness
 * passes from the manifest LINUX_NODES registry (or a --linux-mac override). By hand:
 *     make flash APP=test-mesh-gate-linux BOARD=proto1-fgh100m LINUX_MAC=3c:22:7f:37:51:38 PORT=...
 * The default (chronite) applies only to a bare build with no argument (e.g. the T0 matrix). */
#ifndef TEST_LINUX_MAC
#define TEST_LINUX_MAC "3c:22:7f:37:51:38"   /* default: chronite wlan1 */
#endif

static uint8_t LINUX_MAC[6];

static void parse_mac(const char *s, uint8_t out[6])
{
    unsigned v[6] = { 0 };
    if (sscanf(s, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) == 6) {
        for (int i = 0; i < 6; i++) out[i] = (uint8_t)v[i];
    }
}

/* 802.11 element IDs + beacon IE offset (24-byte PV0 header + ts/bcn-int/cap = 12). */
#define DOT11_IE_MESH_ID  114
#define BEACON_IE_OFFSET  (24 + 12)

static uint8_t g_mesh_mac[6];

enum mmwlan_frame_filter_flag { MMWLAN_FRAME_BEACON = 1 << 8 };
struct mmwlan_rx_frame_info {
    enum mmwlan_frame_filter_flag frame_filter_flag;
    const uint8_t *buf; uint32_t buf_len; uint16_t freq_100khz; int16_t rssi_dbm; uint8_t bw_mhz;
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

/* Peer with any node advertising our mesh ID (the Linux gate). Same discovery shim as test-mesh-linux:
 * S1G mesh beacons bypass the app rx cb, so peer-open is driven from the beacon frame filter. */
static void peer_beacon_cb(const struct mmwlan_rx_frame_info *info, void *arg)
{
    (void)arg;
    if (info->buf_len <= BEACON_IE_OFFSET) return;
    const uint8_t *sa = info->buf + 10;
    if (memcmp(sa, g_mesh_mac, 6) == 0) return;
    uint8_t id_len = 0;
    const uint8_t *id = find_ie(info->buf + BEACON_IE_OFFSET, info->buf_len - BEACON_IE_OFFSET,
                                DOT11_IE_MESH_ID, &id_len);
    if (!id) return;
    if (id_len == strlen(MESH_ID) && memcmp(id, MESH_ID, id_len) == 0) {
        mmwlan_mesh_peer_open(sa);
    }
}

static void park_forever(void) { while (1) { vTaskDelay(pdMS_TO_TICKS(10000)); } }

static int has_linux_peer(uint8_t macs[][6], uint8_t n)
{
    for (uint8_t i = 0; i < n; i++)
        if (memcmp(macs[i], LINUX_MAC, 6) == 0) return 1;
    return 0;
}

void app_main(void)
{
    parse_mac(TEST_LINUX_MAC, LINUX_MAC);   /* the peer MAC is a build-time argument */

    vTaskDelay(pdMS_TO_TICKS(500));
    TEST_BEGIN(NAME, RIG);
    TEST_INFO("joining Linux mesh \"%s\" (SAE); Linux gate = %02x:%02x:%02x:%02x:%02x:%02x "
              "(from TEST_LINUX_MAC=%s); expecting to learn it as a gate via RANN", MESH_ID,
              LINUX_MAC[0], LINUX_MAC[1], LINUX_MAC[2], LINUX_MAC[3], LINUX_MAC[4], LINUX_MAC[5],
              TEST_LINUX_MAC);

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_netif_init());

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
    args.max_plinks = 16;

    enum mmwlan_status st = mmwlan_mesh_start(&args);
    if (st != MMWLAN_SUCCESS) {
        TEST_FAIL("mmwlan_mesh_start failed status=%d (country not set / radio not booted?)", (int)st);
        TEST_END(NAME);
        park_forever();
    }
    mmwlan_override_max_tx_power(1);
    mmwlan_register_rx_frame_cb(MMWLAN_FRAME_BEACON, peer_beacon_cb, NULL);

    /* 1) Peering interop: wait for an ESTAB peer that IS the Linux node. */
    uint8_t peer_macs[16][6];
    uint8_t np = 0;
    bool linux_peered = false;
    for (int s = 0; s < PEER_WAIT_S; s++) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        np = mmwlan_mesh_peer_count(peer_macs);
        linux_peered = has_linux_peer(peer_macs, np);
        if (linux_peered) break;
        if ((s % 5) == 4) TEST_INFO("waiting for the Linux plink... %ds, peers=%u", s + 1, np);
    }
    TEST_STEP("peer-linux", linux_peered, "peers=%u linux_estab=%d", np, (int)linux_peered);
    if (!linux_peered) {
        TEST_INCONCLUSIVE("no ESTAB peer matching the Linux gate MAC within %ds (peers=%u) -- the "
                          "Linux gate is down / RF / rig, not a gate-discovery regression", PEER_WAIT_S, np);
        TEST_END(NAME);
        park_forever();
    }

    /* 2) Gate-discovery interop: the ESP must LEARN the Linux node as a gate (its S2 RX path recorded
     * the RANN with RANN_FLAG_IS_GATE). Poll known_gates -- the Linux gate RANNs every 5s. */
    uint8_t n_gates = 0;
    for (int s = 0; s < GATE_WAIT_S; s++) {
        n_gates = mmwlan_mesh_gate_count();
        if (n_gates > 0) break;
        vTaskDelay(pdMS_TO_TICKS(1000));
        if ((s % 5) == 4) TEST_INFO("peered; waiting to learn the gate via RANN... %ds, known_gates=%u",
                                    s + 1, n_gates);
    }
    TEST_STEP("learn-gate", n_gates > 0, "known_gates=%u", n_gates);

    if (n_gates > 0) {
        TEST_PASS("ESP<->Linux gate discovery: peered (SAE+AMPE) with the Linux node + learned it as a "
                  "gate via RANN (known_gates=%u)", n_gates);
    } else {
        TEST_FAIL("peered (SAE+AMPE) with the Linux node but known_gates stayed 0 within %ds -- the ESP "
                  "never recorded the live Linux gate's RANN (S2 RX interop regressed, or the Linux node "
                  "was not in gate mode)", GATE_WAIT_S);
    }
    TEST_END(NAME);
    park_forever();
}
