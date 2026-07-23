/*
 * test-mesh-gate-relay — 802.11s multi-hop send-to-gates through a relay (S4c verification).
 *
 * A symmetric 3-role fixture (flash to all three ESPs). A forced line topology (peer allowlist +
 * filtered peer-open) makes the NODE reach the GATE ONLY via the RELAY:
 *
 *      NODE (board0) <--> RELAY (board1) <--> GATE (board2)
 *
 * The GATE floods a gate RANN; the RELAY re-floods it (S2); the NODE learns the GATE (known_gates) and
 * a path to it via the RELAY (root-confirmation PREQ). The NODE then send_to_gates() for an off-mesh
 * dest: it wraps the frame as a 6-address AE_A5_A6 frame to the GATE and sends it to the RELAY. The
 * RELAY relays it — and with S4c PRESERVES the AE endpoints (eaddr1/eaddr2) instead of stripping them.
 *
 * WHAT IT PROVES (S4c): the GATE, two hops away, receives the AE frame with eaddr1(DA)=02:..:cc +
 * eaddr2(SA)=02:..:dd INTACT — i.e. the intermediate relay kept the proxied endpoints (before S4c it
 * would strip them and the gate would get a plain frame). Verified via the GATE's AE-rx probe.
 *
 * BENCH: flash all three with NODE_MAC/RELAY_MAC/GATE_MAC (defaults board0/1/2). Radio-silent after.
 * Derived from test-mesh-relay (forced topology) + test-mesh-gate-fwd (send_to_gates).
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

/* The line topology (build-time, defaults board0/board1/board2). */
#ifndef TEST_NODE_MAC
#define TEST_NODE_MAC  "e2:72:a1:f8:ef:a4"   /* board0 — the send_to_gates node   */
#endif
#ifndef TEST_RELAY_MAC
#define TEST_RELAY_MAC "e2:72:a1:f8:f9:40"   /* board1 — the intermediate relay   */
#endif
#ifndef TEST_GATE_MAC
#define TEST_GATE_MAC  "e2:72:a1:f8:f0:08"   /* board2 — the gate (2 hops away)   */
#endif

/* The off-mesh endpoints the node's send_to_gates frame carries. */
static const uint8_t FINAL_DST[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0xcc }; /* eaddr1 (addr5) */
static const uint8_t SRC_HOST[6]  = { 0x02, 0x00, 0x00, 0x00, 0x00, 0xdd }; /* eaddr2 (addr6) */
static const uint8_t AE_PAYLOAD[8 + 20] = { 0xaa, 0xaa, 0x03, 0x00, 0x00, 0x00, 0x08, 0x00 };

static const char *TAG = "test-mesh-gate-relay";
static uint8_t g_mesh_mac[6], MAC_NODE[6], MAC_RELAY[6], MAC_GATE[6];
static uint8_t g_allow[2][6]; /* the neighbours this role may peer (forces the line) */
static uint8_t g_allow_n;

static void parse_mac(const char *s, uint8_t out[6])
{
    unsigned v[6] = { 0 };
    if (sscanf(s, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) == 6)
        for (int i = 0; i < 6; i++) out[i] = (uint8_t)v[i];
}

/* Auto-peer trigger, but ONLY toward an allowed neighbour (with the allowlist this forces the line). */
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
static bool is_allowed(const uint8_t *sa)
{
    for (uint8_t i = 0; i < g_allow_n; i++) if (memcmp(sa, g_allow[i], 6) == 0) return true;
    return false;
}
static void peer_beacon_cb(const struct mmwlan_rx_frame_info *info, void *arg)
{
    (void)arg;
    if (info->buf_len <= BEACON_IE_OFFSET) return;
    const uint8_t *sa = info->buf + 10;
    if (memcmp(sa, g_mesh_mac, 6) == 0 || !is_allowed(sa)) return;
    uint8_t id_len = 0;
    const uint8_t *id = find_ie(info->buf + BEACON_IE_OFFSET, info->buf_len - BEACON_IE_OFFSET,
                                DOT11_IE_MESH_ID, &id_len);
    if (id && id_len == strlen(MESH_ID) && memcmp(id, MESH_ID, id_len) == 0)
        mmwlan_mesh_peer_open(sa);
}

void app_main(void)
{
    parse_mac(TEST_NODE_MAC, MAC_NODE);
    parse_mac(TEST_RELAY_MAC, MAC_RELAY);
    parse_mac(TEST_GATE_MAC, MAC_GATE);

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_netif_init());

    esp_read_mac(g_mesh_mac, ESP_MAC_WIFI_STA);
    g_mesh_mac[0] = (g_mesh_mac[0] | 0x02) & 0xFE;
    bool is_node  = (memcmp(g_mesh_mac, MAC_NODE, 6) == 0);
    bool is_relay = (memcmp(g_mesh_mac, MAC_RELAY, 6) == 0);
    bool is_gate  = (memcmp(g_mesh_mac, MAC_GATE, 6) == 0);
    const char *role = is_node ? "NODE" : is_relay ? "RELAY" : is_gate ? "GATE" : "?";

    /* Allowed neighbours per role -> the line NODE<->RELAY<->GATE. */
    if (is_relay) { memcpy(g_allow[0], MAC_NODE, 6); memcpy(g_allow[1], MAC_GATE, 6); g_allow_n = 2; }
    else if (is_node)  { memcpy(g_allow[0], MAC_RELAY, 6); g_allow_n = 1; }
    else               { memcpy(g_allow[0], MAC_RELAY, 6); g_allow_n = 1; } /* gate peers only the relay */

    ESP_LOGI(TAG, "=== S4c multi-hop send-to-gates: role=%s mac=%02x:%02x:%02x:%02x:%02x:%02x ===",
             role, g_mesh_mac[0], g_mesh_mac[1], g_mesh_mac[2], g_mesh_mac[3], g_mesh_mac[4], g_mesh_mac[5]);

    mmhalow_init(NULL);
    mmhalow_print_version_info();

    struct mmwlan_mesh_args args = { 0 };
    memcpy(args.if_addr, g_mesh_mac, sizeof(g_mesh_mac));
    memcpy(args.mesh_id, MESH_ID, strlen(MESH_ID));
    args.mesh_id_len = strlen(MESH_ID);
    args.s1g_chan_num = MESH_S1G_CHAN;
    args.beacon_interval_tu = 100;
    args.max_plinks = MESH_MAX_PLINKS;

    /* Allowlist BEFORE mesh_start (a late one lets a direct plink form + defeats the line). */
    if (g_allow_n == 2) { uint8_t a[12]; memcpy(a, g_allow[0], 6); memcpy(a + 6, g_allow[1], 6);
                          mmwlan_mesh_set_peer_allowlist(a, 2); }
    else                { mmwlan_mesh_set_peer_allowlist(g_allow[0], 1); }

    if (mmwlan_mesh_start(&args) != MMWLAN_SUCCESS) { ESP_LOGE(TAG, "mesh_start FAILED"); return; }
    mmwlan_override_max_tx_power(1);
    mmwlan_register_rx_frame_cb(MMWLAN_FRAME_BEACON, peer_beacon_cb, NULL);

    if (is_gate)
    {
        mmwlan_mesh_set_root_announcements(true, true, 5000); /* GATE emits gate RANNs */
        ESP_LOGI(TAG, "==> GATE up (RANN + gate). Watching for the AE frame relayed via the RELAY.");
    }
    else
    {
        ESP_LOGI(TAG, "==> %s up.", role);
    }

    uint32_t s = 0;
    bool announced = false;
    for (;;)
    {
        vTaskDelay(pdMS_TO_TICKS(1000));
        s++;
        uint8_t peer_macs[UMAC_MESH_MAX_PEERS][6] = {{0}};
        uint8_t n_peers = mmwlan_mesh_peer_count(peer_macs);
        uint8_t n_gates = mmwlan_mesh_gate_count();

        /* NODE: once it has learned the (2-hop) gate, send_to_gates every 3s. */
        if (is_node)
        {
            if (n_gates > 0 && !announced)
            {
                announced = true;
                ESP_LOGI(TAG, "==> NODE learned the 2-hop gate (known_gates=%u); send_to_gates every 3s.",
                         (unsigned)n_gates);
            }
            if (n_gates > 0 && (s % 3 == 0))
            {
                bool ok = mmwlan_mesh_send_to_gates(FINAL_DST, SRC_HOST, AE_PAYLOAD, sizeof(AE_PAYLOAD));
                ESP_LOGI(TAG, "send_to_gates: %s (known_gates=%u)", ok ? "sent" : "NO GATE/PATH",
                         (unsigned)n_gates);
            }
        }

        /* GATE (and RELAY): report received AE frames — the GATE's count advancing with eaddr1=02:..:cc
         * proves the proxied endpoints survived the 2-hop relay (S4c). */
        uint8_t ae1[6] = {0}, ae2[6] = {0};
        uint32_t ae_rx = mmwlan_mesh_ae_rx_probe(ae1, ae2);
        if (s % 5 == 0)
        {
            ESP_LOGI(TAG, "role=%s uptime=%" PRIu32 "s peers=%u gates=%u ae_rx=%" PRIu32, role, s,
                     (unsigned)n_peers, (unsigned)n_gates, ae_rx);
            if (ae_rx > 0)
                ESP_LOGI(TAG, "  last AE: eaddr1(DA)=%02x:%02x:%02x:%02x:%02x:%02x "
                         "eaddr2(SA)=%02x:%02x:%02x:%02x:%02x:%02x", ae1[0], ae1[1], ae1[2], ae1[3],
                         ae1[4], ae1[5], ae2[0], ae2[1], ae2[2], ae2[3], ae2[4], ae2[5]);
        }
    }
}
