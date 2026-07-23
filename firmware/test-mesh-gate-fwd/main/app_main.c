/*
 * test-mesh-gate-fwd — 802.11s send-to-gates TX fallback (S4b verification).
 *
 * A mesh node that peers, LEARNS a gate from its RANN (S2 known_gates), then reaches an off-mesh
 * destination THROUGH the gate via mmwlan_mesh_send_to_gates(): it wraps the frame as a 6-address
 * AE_A5_A6 frame (mesh DA = gate, eaddr1 = final off-mesh DA, eaddr2 = original source) and sends it to
 * the gate — the port of net/mac80211 mesh_path_send_to_gates + prepare_for_gate.
 *
 * WHAT IT PROVES (S4b): a node with no path to an off-mesh dest falls back to a discovered gate and emits
 * a correct AE frame to it. Verified two ways: (1) THIS node logs `send_to_gates: sent (gates=N)`;
 * (2) the GATE node (test-mesh-gate-rx GATE=1) logs the received AE frame via its AE-rx probe with
 * eaddr1(DA)=02:..:cc, eaddr2(SA)=02:..:dd, and learns mpp(02:..:dd via us).
 *
 * BENCH: flash the gate to one ESP (`make flash APP=test-mesh-gate-rx GATE=1 ...`) and this to another;
 * both rimba-smesh (SAE rimbamesh2026). Radio-silent cleanup after. Derived from test-mesh-ae.
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

#define MESH_ID         "rimba-smesh"
#define MESH_S1G_CHAN   27
#define MESH_MAX_PLINKS 16

/* The off-mesh endpoints the send_to_gates frame carries (locally-administered synthetic MACs). */
static const uint8_t FINAL_DST[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0xcc }; /* eaddr1 (addr5) — off-mesh DA */
static const uint8_t SRC_HOST[6]  = { 0x02, 0x00, 0x00, 0x00, 0x00, 0xdd }; /* eaddr2 (addr6) — original SA */
static const uint8_t AE_PAYLOAD[8 + 20] = { 0xaa, 0xaa, 0x03, 0x00, 0x00, 0x00, 0x08, 0x00 };

static const char *TAG = "test-mesh-gate-fwd";
static uint8_t g_mesh_mac[6];

/* Auto-peer trigger (== test-mesh-gate-rx / test-mesh-ae). */
enum mmwlan_frame_filter_flag { MMWLAN_FRAME_BEACON = 1 << 8 };
struct mmwlan_rx_frame_info {
    enum mmwlan_frame_filter_flag frame_filter_flag;
    const uint8_t *buf; uint32_t buf_len; uint16_t freq_100khz; int16_t rssi_dbm; uint8_t bw_mhz;
};
typedef void (*mmwlan_rx_frame_cb_t)(const struct mmwlan_rx_frame_info *rx_info, void *arg);
extern enum mmwlan_status mmwlan_register_rx_frame_cb(uint32_t filter,
                                                      mmwlan_rx_frame_cb_t callback, void *arg);
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
    if (id && id_len == strlen(MESH_ID) && memcmp(id, MESH_ID, id_len) == 0)
        mmwlan_mesh_peer_open(sa);
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== 802.11s send-to-gates TX fallback (S4b) ===");

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

    ESP_LOGI(TAG, "joining \"%s\" as mac=%02x:%02x:%02x:%02x:%02x:%02x", MESH_ID,
             g_mesh_mac[0], g_mesh_mac[1], g_mesh_mac[2], g_mesh_mac[3], g_mesh_mac[4], g_mesh_mac[5]);
    if (mmwlan_mesh_start(&args) != MMWLAN_SUCCESS) { ESP_LOGE(TAG, "mesh_start FAILED"); return; }
    mmwlan_override_max_tx_power(1);
    mmwlan_register_rx_frame_cb(MMWLAN_FRAME_BEACON, peer_beacon_cb, NULL);
    ESP_LOGI(TAG, "mesh up; waiting to learn a gate, then send_to_gates for an off-mesh dest...");

    uint32_t s = 0;
    bool announced = false;
    for (;;)
    {
        vTaskDelay(pdMS_TO_TICKS(1000));
        s++;
        uint8_t peer_macs[UMAC_MESH_MAX_PEERS][6] = {{0}};
        uint8_t n_peers = mmwlan_mesh_peer_count(peer_macs);
        uint8_t n_gates = mmwlan_mesh_gate_count();
        if (n_gates > 0 && !announced)
        {
            announced = true;
            ESP_LOGI(TAG, "==> learned a gate (known_gates=%u); starting send_to_gates every 3s "
                     "(final DA 02:..:cc, src 02:..:dd).", (unsigned)n_gates);
        }
        if (n_gates > 0 && (s % 3 == 0))
        {
            bool ok = mmwlan_mesh_send_to_gates(FINAL_DST, SRC_HOST, AE_PAYLOAD, sizeof(AE_PAYLOAD));
            ESP_LOGI(TAG, "send_to_gates: %s (known_gates=%u)", ok ? "sent" : "NO GATE/PATH",
                     (unsigned)n_gates);
        }
        if (s % 5 == 0)
            ESP_LOGI(TAG, "uptime=%" PRIu32 "s estab_peers=%u known_gates=%u", s, (unsigned)n_peers,
                     (unsigned)n_gates);
    }
}
