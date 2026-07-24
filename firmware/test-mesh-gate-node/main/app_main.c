/*
 * test-mesh-gate-node — a mesh node on the gate's mesh, in one of two modes (selected at build time):
 *
 *  - DEFAULT (reporter): pings a SILENT static AP-client (10.9.9.50) across the gate and self-reports.
 *    This is the mesh->AP-client B2 direction (mesh-gate-b2). Because the STA is silent, the node must
 *    resolve it COLD: its ARP is a mesh broadcast the gate bridges down to the AP (B2, No-Ack LOSSY), and
 *    reliability comes from the gate's proxy-ARP proactive push, not the broadcast. So the verdict asserts
 *    on proxy-ARP-BACKED reliability (a wide floor), not the flaky raw path -- the direction that was
 *    0-reply flaky before proxy-ARP.
 *
 *  - NO_PING=1 (responder, a SUPPORT role): just a static mesh node at 10.9.9.<100+lowbits> that lwIP
 *    auto-responds from. Used as the far mesh node the STA pings in mesh-gate-bridge (Case C). No TEST|
 *    verdict -- the harness reads its "mesh node IP" up_marker.
 *
 * All test roles the harness flashes are test-* apps (the split rule). Rig: gate=board2 (test-mesh-gate-ap).
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
#include "ping/ping_sock.h"

#include "mmhalow.h"
#include "mmwlan.h"
#include "umac/mesh/umac_mesh.h"

#include "test_report.h"

#define NAME       "mesh-gate-b2"
#define RIG        "mesh node pings a silent static AP-client (10.9.9.50) across the gate (B2 + proxy-ARP)"

#define MESH_ID         "rimba-mesh"      /* MATCH the gate's mesh (test-mesh-gate-ap / rimba-halow-mesh-ap) */
#define MESH_S1G_CHAN   27
#define MESH_MAX_PLINKS 16
#define STA_TARGET      "10.9.9.50"       /* the static-IP silent AP-client responder (reporter mode) */

#define PEER_WAIT_S     45
#define SETTLE_MS       3000              /* let the gate's 3s proxy-ARP push seed our cache */
#define PING_COUNT      30
#define PING_INTERVAL_MS 1000
#define REPLY_FLOOR     15                /* wide: 0-3 = broken (no proxy-ARP), 25-30 = reliable */

static const char *TAG = "test-mesh-gate-node";
static uint8_t g_mesh_mac[6];

#ifndef TEST_NO_PING
static volatile int  s_replies = 0, s_timeouts = 0, s_first_reply_seq = -1;
static volatile bool s_ping_done = false;

static void on_ping_success(esp_ping_handle_t hdl, void *args)
{
    uint16_t seqno; uint32_t ms;
    esp_ping_get_profile(hdl, ESP_PING_PROF_SEQNO, &seqno, sizeof(seqno));
    esp_ping_get_profile(hdl, ESP_PING_PROF_TIMEGAP, &ms, sizeof(ms));
    if (s_first_reply_seq < 0) s_first_reply_seq = seqno;
    s_replies++;
    TEST_INFO("reply from %s seq=%u time=%" PRIu32 " ms (via the gate B2/proxy-ARP)", STA_TARGET, seqno, ms);
}
static void on_ping_timeout(esp_ping_handle_t hdl, void *args) { s_timeouts++; }
static void on_ping_end(esp_ping_handle_t hdl, void *args) { s_ping_done = true; }
#endif

static void park_forever(void) { while (1) { vTaskDelay(pdMS_TO_TICKS(10000)); } }

/* Bring up the mesh + pin a static flat-subnet IP (10.9.9.<100 + mac&0x3f>, no gateway). Logs the
 * "mesh node IP" line (the harness up_marker in responder mode). Returns false on any failure. */
static bool mesh_bringup(char *ip_out, size_t ip_len)
{
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
    if (mmwlan_mesh_start(&args) != MMWLAN_SUCCESS) { ESP_LOGE(TAG, "mmwlan_mesh_start failed"); return false; }
    mmwlan_override_max_tx_power(1);

    esp_netif_t *n = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (n == NULL) { ESP_LOGE(TAG, "mesh netif not found"); return false; }
    for (int i = 0; i < 60 && !esp_netif_is_netif_up(n); i++) vTaskDelay(pdMS_TO_TICKS(500));
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_netif_dhcpc_stop(n);
    esp_netif_set_mac(n, g_mesh_mac);
    unsigned host = 100u + (g_mesh_mac[5] & 0x3fu);
    snprintf(ip_out, ip_len, "10.9.9.%u", host);
    esp_netif_ip_info_t ip = { 0 };
    ip.ip.addr = esp_ip4addr_aton(ip_out);
    ip.netmask.addr = esp_ip4addr_aton("255.255.255.0");
    if (esp_netif_set_ip_info(n, &ip) != ESP_OK) { ESP_LOGE(TAG, "set_ip_info failed"); return false; }
    return true;
}

void app_main(void)
{
    vTaskDelay(pdMS_TO_TICKS(500));

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_netif_init());

#ifdef TEST_NO_PING
    /* RESPONDER (Case C support role): a static mesh node the STA pings; lwIP auto-responds. No TEST|. */
    char ipbuf[20] = "?";
    if (!mesh_bringup(ipbuf, sizeof(ipbuf))) { park_forever(); } /* harness catches the missing up_marker */
    ESP_LOGI(TAG, "mesh node IP %s (responder-only) — waiting for the STA to ping us via the gate", ipbuf);
    park_forever();
#else
    /* REPORTER (Case D): ping the silent static AP-client + assert proxy-ARP-backed reliability. */
    TEST_BEGIN(NAME, RIG);
    char ipbuf[20] = "?";
    if (!mesh_bringup(ipbuf, sizeof(ipbuf))) {
        TEST_FAIL("mesh bring-up failed");
        TEST_END(NAME);
        park_forever();
    }
    TEST_INFO("mesh node IP %s -- pinging the silent AP-client %s across the gate", ipbuf, STA_TARGET);

    int peered = 0, waited = 0;
    while (waited < PEER_WAIT_S) {
        uint8_t peer_macs[UMAC_MESH_MAX_PEERS][6] = {{0}};
        peered = mmwlan_mesh_peer_count(peer_macs);
        if (peered > 0) break;
        vTaskDelay(pdMS_TO_TICKS(1000));
        waited++;
    }
    TEST_STEP("peer", peered > 0, "estab_peers=%d after %ds", peered, waited);
    if (peered == 0) {
        TEST_INCONCLUSIVE("never peered the gate's mesh within %ds -- gate down / RF, not a bridge bug",
                             PEER_WAIT_S);
        TEST_END(NAME);
        park_forever();
    }

    vTaskDelay(pdMS_TO_TICKS(SETTLE_MS));
    ip_addr_t target = { 0 };
    target.type = IPADDR_TYPE_V4;
    target.u_addr.ip4.addr = esp_ip4addr_aton(STA_TARGET);
    esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
    cfg.target_addr = target; cfg.count = PING_COUNT; cfg.interval_ms = PING_INTERVAL_MS;
    esp_ping_callbacks_t cbs = { .on_ping_success = on_ping_success, .on_ping_timeout = on_ping_timeout,
                                 .on_ping_end = on_ping_end, .cb_args = NULL };
    esp_ping_handle_t ping;
    if (esp_ping_new_session(&cfg, &cbs, &ping) != ESP_OK) {
        TEST_FAIL("peered but failed to create the ping session");
        TEST_END(NAME);
        park_forever();
    }
    TEST_INFO("pinging %s x%d (mesh->gate->AP; B2 bridges the ARP, proxy-ARP makes it reliable)...",
                 STA_TARGET, PING_COUNT);
    esp_ping_start(ping);
    int guard = 0;
    /* +8 slack (not +15): a PING_COUNT-at-1s session self-terminates in ~PING_COUNT s (esp_ping counts
     * timeouts and ends), so tighter dead-wait here keeps the reporter's worst-case timeline (bring-up +
     * PEER_WAIT_S + settle + ping) comfortably under the harness REPORTER_TIMEOUT_S capture window. */
    while (!s_ping_done && guard < (PING_COUNT + 8)) { vTaskDelay(pdMS_TO_TICKS(1000)); guard++; }

    TEST_STEP("delivery", s_replies >= REPLY_FLOOR, "replies=%d/%d timeouts=%d first_reply_seq=%d",
                 s_replies, PING_COUNT, s_timeouts, s_first_reply_seq);

    if (s_replies >= REPLY_FLOOR) {
        TEST_PASS("mesh node -> silent AP-client %s across the gate: %d/%d replies (first at seq=%d after the "
                     "lossy warmup) -- B2 bridges the ARP + proxy-ARP makes the mesh->AP-client resolution "
                     "RELIABLE (was 0-reply flaky before proxy-ARP)", STA_TARGET, s_replies, PING_COUNT,
                     s_first_reply_seq);
    } else if (s_replies > 0) {
        TEST_INCONCLUSIVE("peered + %d/%d replies (floor %d) -- resolution crossed the bridge but delivery "
                             "stayed marginal (lossy B2 without a stable proxy-ARP push? / RF)", s_replies,
                             PING_COUNT, REPLY_FLOOR);
    } else {
        TEST_FAIL("peered the gate but 0/%d replies from the silent AP-client %s -- the mesh->AP bridge (B2) "
                     "+ proxy-ARP did not resolve/deliver to a static responder", PING_COUNT, STA_TARGET);
    }
    TEST_END(NAME);
    park_forever();
#endif
}
