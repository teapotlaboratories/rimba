/*
 * rimba-halow-mesh — 802.11s secured-mesh node (SAE + AMPE + CCMP).
 *
 * Brings up a single 802.11s mesh interface (Mesh ID + Mesh Configuration IEs, following
 * morse_driver's mesh BSS config flow), joins the secured mesh, pins a static mesh IP, and
 * acts as a responder. Peering (MPM / SAE / AMPE), forwarding, and HWMP path selection are
 * handled inside morselib — the peer link forms automatically when a neighbour is heard.
 *
 * Flash on multiple boards (and/or alongside Linux mesh nodes): each derives a unique mesh MAC
 * from its ESP32 efuse MAC and joins the mesh on the shared channel. The heartbeat logs the
 * node's ESTAB peer count + peer MACs. iperf/ping and forced-topology multi-hop demos live in
 * the dedicated rimba-halow-mesh-perf app.
 */

#include <inttypes.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h" /* esp_get_free_heap_size / esp_get_minimum_free_heap_size */
#include "nvs_flash.h"
#include "esp_netif.h"

#include "mmhalow.h"
#include "umac/mesh/umac_mesh.h"

/* Keep in sync with the other rimba apps' link params. */
#define MESH_ID         "rimba-mesh"
#define MESH_S1G_CHAN   27   /* US 915.5 MHz, 1 MHz BW (global op-class 68) */
#define MESH_MAX_PLINKS 16

/* 802.11 element IDs + beacon IE offset (24-byte PV0 header + ts/bcn-int/cap = 12). */
#define DOT11_IE_MESH_ID        (114)
#define BEACON_IE_OFFSET        (24 + 12)

static const char *TAG = "rimba-mesh";

/* This node's mesh MAC, derived from the ESP32 efuse MAC (unique per board). */
static uint8_t g_mesh_mac[6];

/* ---- peer-beacon RX sniffer (on-air telemetry) --------------------------------
 * morselib's raw-frame capture hook is exported via the mmwlan* symbol rule; the API lives in
 * src/internal, so declare it locally (layout must match mmwlan_internal.h, same as
 * firmware/rimba-halow-scan). Note: S1G mesh peer beacons are handled directly in morselib's
 * datapath (which drives peering); this app-level hook only surfaces legacy beacons. */
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
    (void)arg;
    if (info->buf_len <= BEACON_IE_OFFSET) return;
    const uint8_t *sa = info->buf + 10;   /* A2 / source address */
    if (memcmp(sa, g_mesh_mac, 6) == 0) return; /* ignore our own beacons */

    uint8_t id_len = 0;
    const uint8_t *id = find_ie(info->buf + BEACON_IE_OFFSET,
                                info->buf_len - BEACON_IE_OFFSET, DOT11_IE_MESH_ID, &id_len);
    if (!id) return; /* not a mesh beacon */
    bool ours = id_len == strlen(MESH_ID) && memcmp(id, MESH_ID, id_len) == 0;

    ESP_LOGI(TAG, "PEER MESH BEACON%s from %02x:%02x:%02x:%02x:%02x:%02x rssi=%d "
             "freq=%u00kHz MeshID=\"%.*s\"",
             ours ? " (rimba-mesh)" : "", sa[0], sa[1], sa[2], sa[3], sa[4], sa[5],
             info->rssi_dbm, info->freq_100khz, (int)id_len, (const char *)id);

    /* Same Mesh ID -> initiate a peer link (idempotent; morselib runs the MPM/SAE/AMPE handshake
     * and ignores repeats once the peer is known). Mirrors mac80211 opening a plink on a heard
     * candidate beacon. */
    if (ours)
    {
        mmwlan_mesh_peer_open(sa);
    }
}

/* Pin a static mesh IP once the netif is up (distinct per node, derived from the mesh MAC). */
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
    esp_netif_ip_info_t ip = { 0 };
    ip.ip.addr = esp_ip4addr_aton(ipbuf);
    ip.gw.addr = esp_ip4addr_aton("10.9.9.1");
    ip.netmask.addr = esp_ip4addr_aton("255.255.255.0");
    ESP_ERROR_CHECK(esp_netif_set_ip_info(n, &ip));
    ESP_LOGI(TAG, "mesh static IP %s (netif up=%d) — responder", ipbuf, (int)esp_netif_is_netif_up(n));
    vTaskDelete(NULL);
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== 802.11s secured mesh node ===");

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

    /* Log any peer mesh beacons we hear (S1G peer beacons are handled in morselib's datapath). */
    mmwlan_register_rx_frame_cb(MMWLAN_FRAME_BEACON, peer_beacon_cb, NULL);

    /* Once a peer link is up, pin a static mesh IP. Peering (MPM/SAE/AMPE), forwarding, and HWMP
     * are handled in morselib — the peer link forms automatically when a neighbour is heard/opens. */
    xTaskCreate(mesh_net_task, "mesh_net", 4096, NULL, 5, NULL);

    uint32_t s = 0;
    for (;;)
    {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (++s % 5 == 0)
        {
            uint8_t peer_macs[UMAC_MESH_MAX_PEERS][6] = {{0}};
            uint8_t n_peers = mmwlan_mesh_peer_count(peer_macs);
            ESP_LOGI(TAG, "mesh alive, uptime=%" PRIu32 "s  estab_peers=%u", s, (unsigned)n_peers);
            ESP_LOGI(TAG, "  heap free=%" PRIu32 " min_ever=%" PRIu32,
                     esp_get_free_heap_size(), esp_get_minimum_free_heap_size());
            for (uint8_t pi = 0; pi < n_peers; pi++)
            {
                ESP_LOGI(TAG, "  peer[%u]=%02x:%02x:%02x:%02x:%02x:%02x", pi,
                         peer_macs[pi][0], peer_macs[pi][1], peer_macs[pi][2],
                         peer_macs[pi][3], peer_macs[pi][4], peer_macs[pi][5]);
            }
        }
    }
}
