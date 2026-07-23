/*
 * test-mesh-gate — 802.11s mesh-gate RANN emitter (S1 verification fixture).
 *
 * Brings up a single 802.11s secured mesh node (identical bring-up to rimba-halow-mesh) and then
 * turns on PROACTIVE_RANN root + gate announcements via mmwlan_mesh_set_root_announcements(). The
 * node floods a Root Announcement (RANN, WLAN_EID_RANN=126) with RANN_FLAG_IS_GATE set every
 * RANN_INTERVAL_MS, exactly as a Linux mesh gate (dot11MeshHWMPRootMode=PROACTIVE_RANN +
 * dot11MeshGateAnnouncementProtocol=1) does. See docs/mesh-ap/rimba-mesh-ap-mesh-gate-discovery-design.md.
 *
 * WHAT IT PROVES (S1): the ESP emits a RANN whose bytes are identical to a live Linux gate's RANN —
 * verified by capturing both on chronium's morse0 monitor and byte-diffing (the mesh-gate discovery
 * TX half). This is an on-air interop fixture (like test-mesh-linux), NOT a self-checking T2 test:
 * the verdict is the off-line byte-diff, so it emits no TEST| line.
 *
 * BENCH: flash to one ESP; set RANN_INTERVAL_MS to the SAME numeric value the bench Linux gate uses
 * (default dot11MeshHWMPRannInterval=5000) so the interval field bytes match. The node logs its mesh
 * MAC (== rann_addr) at boot and a "MESH RANN (gate) sn=.. from <mac>" line (morselib MMLOG) each
 * emission. Radio-silent cleanup after (reflash rimba-hello + `ip link set wlan1 down` on Linux).
 *
 * Derived from firmware/rimba-halow-mesh/main/app_main.c.
 */

#include <inttypes.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_netif.h"

#include "mmhalow.h"
#include "umac/mesh/umac_mesh.h"

/* Keep in sync with rimba-halow-mesh / the other mesh fixtures' link params. */
#define MESH_ID          "rimba-mesh"
#define MESH_S1G_CHAN    27   /* US 915.5 MHz, 1 MHz BW (global op-class 68) */
#define MESH_MAX_PLINKS  16

/* RANN period. MUST match the bench Linux gate's dot11MeshHWMPRannInterval numeric value so the
 * interval field on the wire is byte-identical (Linux puts the raw value on-air; it is NOT ms->TU
 * converted). Linux default = 5000. */
#define RANN_INTERVAL_MS 5000
#define RANN_IS_GATE     true   /* set RANN_FLAG_IS_GATE (0x01): advertise this root as a gate */

static const char *TAG = "test-mesh-gate";

/* This node's mesh MAC, derived from the ESP32 efuse MAC (unique per board). == rann_addr on air. */
static uint8_t g_mesh_mac[6];

/* Pin a static mesh IP once the netif is up (distinct per node, derived from the mesh MAC). Not
 * strictly needed to emit RANN, but keeps the node a well-formed mesh member for a peer capture. */
static void mesh_net_task(void *arg)
{
    (void)arg;
    esp_netif_t *n = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (n == NULL) { ESP_LOGE(TAG, "mesh netif not found"); vTaskDelete(NULL); return; }

    for (int i = 0; i < 60 && !esp_netif_is_netif_up(n); i++) { vTaskDelay(pdMS_TO_TICKS(500)); }
    vTaskDelay(pdMS_TO_TICKS(500));

    esp_netif_dhcpc_stop(n);
    esp_netif_set_mac(n, g_mesh_mac);
    unsigned host = 100u + (g_mesh_mac[5] & 0x3fu);
    char ipbuf[20];
    snprintf(ipbuf, sizeof(ipbuf), "10.9.9.%u", host);
    esp_netif_ip_info_t ip = { 0 };
    ip.ip.addr = esp_ip4addr_aton(ipbuf);
    ip.netmask.addr = esp_ip4addr_aton("255.255.255.0");
    ESP_ERROR_CHECK(esp_netif_set_ip_info(n, &ip));
    ESP_LOGI(TAG, "mesh static IP %s (netif up=%d) — gate RANN emitter", ipbuf,
             (int)esp_netif_is_netif_up(n));
    vTaskDelete(NULL);
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== 802.11s mesh-gate RANN emitter (S1) ===");

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_netif_init());

    /* Unique per-board mesh MAC: ESP32 efuse MAC with the locally-administered bit set. */
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

    ESP_LOGI(TAG, "Starting mesh (id=\"%s\" chan=%d rann_addr=%02x:%02x:%02x:%02x:%02x:%02x)...",
             MESH_ID, MESH_S1G_CHAN, g_mesh_mac[0], g_mesh_mac[1], g_mesh_mac[2],
             g_mesh_mac[3], g_mesh_mac[4], g_mesh_mac[5]);
    enum mmwlan_status st = mmwlan_mesh_start(&args);
    if (st != MMWLAN_SUCCESS)
    {
        ESP_LOGE(TAG, "==> mmwlan_mesh_start FAILED status=%d", (int)st);
        return;
    }

    /* S1: become a PROACTIVE_RANN root + gate. The morselib RANN tick (armed at mesh start) now
     * emits a RANN every RANN_INTERVAL_MS with RANN_FLAG_IS_GATE set. Mirrors the Linux mesh config
     * dot11MeshHWMPRootMode=PROACTIVE_RANN + dot11MeshGateAnnouncementProtocol=1. */
    mmwlan_mesh_set_root_announcements(true, RANN_IS_GATE, RANN_INTERVAL_MS);
    ESP_LOGI(TAG, "==> mesh vif up; PROACTIVE_RANN root%s, interval=%dms — flooding RANN on chan %d.",
             RANN_IS_GATE ? " + GATE (flags=0x01)" : "", RANN_INTERVAL_MS, MESH_S1G_CHAN);

    xTaskCreate(mesh_net_task, "mesh_net", 4096, NULL, 5, NULL);

    uint32_t s = 0;
    for (;;)
    {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (++s % 5 == 0)
        {
            uint8_t peer_macs[UMAC_MESH_MAX_PEERS][6] = {{0}};
            uint8_t n_peers = mmwlan_mesh_peer_count(peer_macs);
            ESP_LOGI(TAG, "gate alive, uptime=%" PRIu32 "s  estab_peers=%u  (RANN every %dms)",
                     s, (unsigned)n_peers, RANN_INTERVAL_MS);
            for (uint8_t pi = 0; pi < n_peers; pi++)
            {
                ESP_LOGI(TAG, "  peer[%u]=%02x:%02x:%02x:%02x:%02x:%02x", pi,
                         peer_macs[pi][0], peer_macs[pi][1], peer_macs[pi][2],
                         peer_macs[pi][3], peer_macs[pi][4], peer_macs[pi][5]);
            }
        }
    }
}
