/*
 * test-mesh-gate-rx — 802.11s mesh-gate RANN RX + relay (S2 verification fixture).
 *
 * A plain (non-root) secured-mesh node that peers with a Linux mesh GATE (chronite, brought up as a
 * PROACTIVE_RANN gate), then exercises the S2 RX path: it receives the gate's Root Announcements,
 * LEARNS the gate (mmwlan_mesh_gate_count() -> 1), kicks a root-confirmation PREQ, and RE-FLOODS the
 * RANN (hop+1, ttl-1, accumulated metric) — a port of net/mac80211 hwmp_rann_frame_process. It also
 * advertises the "Connected to Mesh Gate" beacon formation-info bit once it knows a gate.
 *
 * WHAT IT PROVES (S2): the ESP receives + accepts a live Linux gate's RANN, records it as a gate, and
 * re-floods it byte-correctly — verified by (a) this node logging gate_count>0 and (b) capturing its
 * re-flooded RANN on chronium's morse0 monitor and byte-diffing it against the Linux origin RANN + a
 * Linux relay. On-air interop fixture (like test-mesh-linux); the verdict is the off-line byte-diff.
 *
 * BENCH: bring up chronite as a rimba-smesh gate (wpa_supplicant_s1g + `iw set mesh_param
 * mesh_hwmp_rootmode 4 / mesh_gate_announcements 1`); flash this to one ESP; monitor on chronium.
 * MUST use MESH_ID "rimba-smesh" + SAE "rimbamesh2026" (compiled into morselib) to peer with chronite.
 * Radio-silent cleanup after (reflash rimba-hello + `ip link set wlan1 down` on the Linux nodes).
 *
 * Derived from firmware/test-mesh-linux (peering) + firmware/test-mesh-gate (gate).
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

/* MUST match the Linux gate's wpa-smesh.conf so the ESP peers with it (SAE password is compiled into
 * morselib). Only the mesh ID differs from the ESP<->ESP tests — same as test-mesh-linux. */
#ifndef TEST_MESH_ID
#define TEST_MESH_ID    "rimba-smesh"   /* override to "rimba-mesh" to peer with the gate app (S5c) */
#endif
#define MESH_ID         TEST_MESH_ID
#define MESH_S1G_CHAN   27
#define MESH_MAX_PLINKS 16

static const char *TAG = "test-mesh-gate-rx";

/* This node's mesh MAC (unique per board). == the SA of its re-flooded RANNs on air. */
static uint8_t g_mesh_mac[6];

/* morselib's raw-frame beacon hook (exported via the mmwlan* symbol rule; declared locally, layout
 * matching mmwlan_internal.h — identical to test-mesh-linux / test-mesh-peering). Used to auto-open a
 * peer link to any neighbour beaconing our mesh ID (the proven MPM trigger). */
enum mmwlan_frame_filter_flag { MMWLAN_FRAME_BEACON = 1 << 8 };
struct mmwlan_rx_frame_info {
    enum mmwlan_frame_filter_flag frame_filter_flag;
    const uint8_t *buf; uint32_t buf_len; uint16_t freq_100khz; int16_t rssi_dbm; uint8_t bw_mhz;
};
typedef void (*mmwlan_rx_frame_cb_t)(const struct mmwlan_rx_frame_info *rx_info, void *arg);
extern enum mmwlan_status mmwlan_register_rx_frame_cb(uint32_t filter,
                                                      mmwlan_rx_frame_cb_t callback, void *arg);

/* 802.11 element IDs + beacon IE offset (24-byte PV0 header + ts/bcn-int/cap = 12). */
#define DOT11_IE_MESH_ID  114
#define BEACON_IE_OFFSET  (24 + 12)

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

/* S5 — the mesh->AP-bridge hook: the datapath hands us a received AE frame's proxied endpoints + payload
 * (which the plain mesh RX ext-cb can't see, since the AE Mesh Control is stripped first). A real gate
 * would deliver `payload` onto its AP vif addressed to eaddr1; here we log it to prove the app receives
 * the full proxied frame end to end. */
static bool ae_rx_cb(const uint8_t *e1, const uint8_t *e2, const uint8_t *payload, uint32_t len, void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "AE-rx CB: eaddr1(DA)=%02x:%02x:%02x:%02x:%02x:%02x eaddr2(SA)=%02x:%02x:%02x:%02x:%02x:%02x"
             " payload_len=%" PRIu32 " payload[0..3]=%02x%02x%02x%02x",
             e1[0], e1[1], e1[2], e1[3], e1[4], e1[5], e2[0], e2[1], e2[2], e2[3], e2[4], e2[5],
             len, len > 0 ? payload[0] : 0, len > 1 ? payload[1] : 0, len > 2 ? payload[2] : 0,
             len > 3 ? payload[3] : 0);
    return false; /* observer only — do NOT consume; let the normal local delivery proceed */
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== 802.11s mesh-gate RANN RX + relay (S2) ===");

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
    args.max_plinks = MESH_MAX_PLINKS;

    ESP_LOGI(TAG, "joining mesh \"%s\" (SAE) as a plain relay node, mac=%02x:%02x:%02x:%02x:%02x:%02x",
             MESH_ID, g_mesh_mac[0], g_mesh_mac[1], g_mesh_mac[2], g_mesh_mac[3], g_mesh_mac[4],
             g_mesh_mac[5]);
    enum mmwlan_status st = mmwlan_mesh_start(&args);
    if (st != MMWLAN_SUCCESS)
    {
        ESP_LOGE(TAG, "==> mmwlan_mesh_start FAILED status=%d", (int)st);
        return;
    }
    /* Match the bench TX power the other mesh fixtures use (close-range, avoid RX overload). */
    mmwlan_override_max_tx_power(1);
    /* Auto-open a plink to any neighbour beaconing our mesh ID (peers us with the Linux gate). */
    mmwlan_register_rx_frame_cb(MMWLAN_FRAME_BEACON, peer_beacon_cb, NULL);
    mmwlan_mesh_register_ae_rx_cb(ae_rx_cb, NULL); /* S5: mesh->AP bridge hook (here: log proxied frames) */
#ifdef MESH_GATE_MODE
    /* S4b test rig: become a PROACTIVE_RANN gate so a peer node learns us (known_gates) and can
     * send_to_gates through us. We then receive its AE frames (the AE rx probe above shows them). */
    mmwlan_mesh_set_root_announcements(true, true, 5000);
    ESP_LOGI(TAG, "==> mesh vif up; GATE MODE (emitting gate RANNs). Watching for AE frames via me.");
#else
    ESP_LOGI(TAG, "==> mesh vif up; NOT a root (relays gate RANNs). Waiting to learn a gate...");
#endif

    /* Heartbeat: report peers + learned gates. Latch the first gate-discovery so it is unmissable. */
    uint32_t s = 0;
    bool announced_gate = false;
    for (;;)
    {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (++s % 5 != 0)
        {
            continue;
        }
        uint8_t peer_macs[UMAC_MESH_MAX_PEERS][6] = {{0}};
        uint8_t n_peers = mmwlan_mesh_peer_count(peer_macs);
        uint8_t n_gates = mmwlan_mesh_gate_count();
        /* S3 probe: report any received 6-address (AE_A5_A6) mesh frames + the proxied endpoints, so an
         * AE sender (test-mesh-ae) can be verified without a Linux node (the datapath RX log is MMLOG,
         * not on the UART). Confirms the FW delivered the AE frame + the host parsed the AE Mesh Control. */
        uint8_t ae1[6] = {0}, ae2[6] = {0};
        uint32_t ae_rx = mmwlan_mesh_ae_rx_probe(ae1, ae2);
        if (ae_rx > 0)
        {
            ESP_LOGI(TAG, "  AE rx count=%" PRIu32 "  last eaddr1(DA)=%02x:%02x:%02x:%02x:%02x:%02x "
                     "eaddr2(SA)=%02x:%02x:%02x:%02x:%02x:%02x", ae_rx, ae1[0], ae1[1], ae1[2], ae1[3],
                     ae1[4], ae1[5], ae2[0], ae2[1], ae2[2], ae2[3], ae2[4], ae2[5]);
            /* S4 — confirm the MPP table LEARNED the proxied source (eaddr2) via the frame's mesh SA:
             * mmwlan_mesh_mpp_lookup(eaddr2) should return the sender's mesh MAC (the proxy node). */
            uint8_t mpp[6] = {0};
            if (mmwlan_mesh_mpp_lookup(ae2, mpp))
            {
                ESP_LOGI(TAG, "  MPP learned: %02x:%02x:%02x:%02x:%02x:%02x via %02x:%02x:%02x:%02x:%02x:%02x",
                         ae2[0], ae2[1], ae2[2], ae2[3], ae2[4], ae2[5],
                         mpp[0], mpp[1], mpp[2], mpp[3], mpp[4], mpp[5]);
            }
        }
        ESP_LOGI(TAG, "uptime=%" PRIu32 "s  estab_peers=%u  known_gates=%u", s, (unsigned)n_peers,
                 (unsigned)n_gates);
        for (uint8_t pi = 0; pi < n_peers; pi++)
        {
            ESP_LOGI(TAG, "  peer[%u]=%02x:%02x:%02x:%02x:%02x:%02x", pi,
                     peer_macs[pi][0], peer_macs[pi][1], peer_macs[pi][2],
                     peer_macs[pi][3], peer_macs[pi][4], peer_macs[pi][5]);
        }
        if (n_gates > 0 && !announced_gate)
        {
            announced_gate = true;
            ESP_LOGI(TAG, "==> GATE DISCOVERED via RANN (known_gates=%u) — S2 RX path works, "
                     "now relaying + advertising Connected-to-Gate.", (unsigned)n_gates);
        }
    }
}
