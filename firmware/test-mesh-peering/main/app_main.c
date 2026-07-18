/*
 * test-mesh-peering — 802.11s secured-mesh peering (SAE + AMPE) reaches ESTAB.
 *
 * A SYMMETRIC app: both mesh nodes run this identical firmware. The T2 orchestrator flashes it to
 * a support node (which just comes up and beacons) and to the reporter node (which polls the ESTAB
 * peer count and emits the TEST| verdict). All apps a T2 test flashes are test-*.
 *
 * WHAT IT PROVES: two ESP nodes complete the mesh peering handshake (MPM + SAE + AMPE) against each
 * other -- with no Linux anchor -- and the plink reaches ESTAB. This is the foundation every mesh
 * milestone sits on.
 * WHAT IT DOES NOT PROVE: forwarding, multi-hop, or on-air byte-equivalence with Linux (ESTAB only
 * proves a tolerant peer completed the handshake).
 *
 * Structural assertion: mmwlan_mesh_peer_count() >= 1. ESTAB is binary -- SAE+AMPE either completed
 * or it didn't -- and it is independent of RF quality (a fading link changes timing, not whether the
 * handshake closes). Derived from firmware/rimba-halow-mesh/main/app_main.c.
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

#define NAME            "mesh-peering"
#define RIG             "two ESP mesh nodes (symmetric); reporter polls ESTAB peer count"
#define MESH_ID         "rimba-mesh"
#define MESH_S1G_CHAN   27
#define MESH_MAX_PLINKS 16
#define PEER_WAIT_S     45          /* how long to wait for the first ESTAB peer */

/* 802.11 element IDs + beacon IE offset (24-byte PV0 header + ts/bcn-int/cap = 12). */
#define DOT11_IE_MESH_ID  114
#define BEACON_IE_OFFSET  (24 + 12)

static uint8_t g_mesh_mac[6];

/* morselib's raw-frame capture hook (exported via the mmwlan* symbol rule; API lives in
 * src/internal, declared locally, layout matching mmwlan_internal.h -- same as rimba-halow-mesh). */
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

/* On hearing a same-Mesh-ID beacon, open a peer link (idempotent; morselib also auto-peers on S1G
 * mesh beacons in its datapath -- this app hook mirrors mac80211 and is belt-and-suspenders). */
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

static void park_forever(void)
{
    while (1) { vTaskDelay(pdMS_TO_TICKS(10000)); }
}

void app_main(void)
{
    vTaskDelay(pdMS_TO_TICKS(500));
    TEST_BEGIN(NAME, RIG);

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_netif_init());

    /* Unique per-board mesh MAC (the MM6108 factory MAC is shared across modules). */
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

    enum mmwlan_status st = mmwlan_mesh_start(&args);
    if (st != MMWLAN_SUCCESS) {
        TEST_FAIL("mmwlan_mesh_start failed status=%d -- mesh vif did not come up "
                     "(country not set / radio not booted?)", (int)st);
        TEST_END(NAME);
        park_forever();
    }

    /* Close-bench RX-overload workaround: cap TX to 1 dBm (both nodes). */
    mmwlan_override_max_tx_power(1);
    mmwlan_register_rx_frame_cb(MMWLAN_FRAME_BEACON, peer_beacon_cb, NULL);

    /* Up-marker the orchestrator waits on for the SUPPORT node (Role.up_marker="mesh-up"). */
    TEST_INFO("mesh-up: vif up + beaconing on ch%d as %02x:%02x:%02x:%02x:%02x:%02x",
                 MESH_S1G_CHAN, g_mesh_mac[0], g_mesh_mac[1], g_mesh_mac[2],
                 g_mesh_mac[3], g_mesh_mac[4], g_mesh_mac[5]);

    /* Poll for the first ESTAB peer. */
    uint8_t peer_macs[MESH_MAX_PLINKS][6];
    uint8_t n = 0;
    for (int s = 0; s < PEER_WAIT_S; s++) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        n = mmwlan_mesh_peer_count(peer_macs);
        if (n >= 1) break;
        if ((s % 5) == 4) TEST_INFO("waiting for ESTAB peer... %ds, estab=%u", s + 1, n);
    }

    TEST_STEP("peer-estab", n >= 1, "estab_peers=%u", n);
    if (n >= 1) {
        TEST_PASS("mesh peering (MPM+SAE+AMPE) reached ESTAB: %u peer(s), first=%02x:%02x:%02x:"
                     "%02x:%02x:%02x", n, peer_macs[0][0], peer_macs[0][1], peer_macs[0][2],
                     peer_macs[0][3], peer_macs[0][4], peer_macs[0][5]);
    } else {
        TEST_FAIL("no ESTAB mesh peer within %ds -- MPM/SAE/AMPE did not complete (is the peer "
                     "node up on the same Mesh ID \"%s\" / channel %d?)", PEER_WAIT_S,
                     MESH_ID, MESH_S1G_CHAN);
    }
    TEST_END(NAME);
    park_forever();
}
