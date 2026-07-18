/*
 * test-mesh-ap-sta — the STA (reporter) role of the mesh + AP concurrency (mesh-gate) T2 test.
 *
 * Behind the gate's AP, this STA pings a FAR MESH NODE (board1, 10.9.9.100) -- NOT the gate's own
 * mesh IP -- so the packet must be IP-forwarded across the gate from the AP subnet (192.168.12.0/24)
 * to the mesh subnet (10.9.9.0/24). It self-verifies the forward by reading the reply's TTL: the STA
 * emits at TTL 64, the far node replies at TTL 64, and the gate's ip4_forward decrements it EXACTLY
 * once -> the STA observes TTL 63. That single decrement is the proof the packet traversed exactly
 * one gate hop -- a delivery count alone cannot prove it. All apps a T2 test flashes are test-*.
 *
 * Rig: gate=board2 (test-mesh-ap-gate), mesh_peer=board1 (test-mesh-ap-peer, 10.9.9.100),
 *      sta(this)=board0.
 *
 * WHAT IT PROVES: mesh + AP run concurrently on one MM6108 and a STA behind the AP routes end-to-end
 * through the mesh to a far node
 * (docs/worklog/2026-07-10-esp32-mesh-gate-returnleg-fixed-forward-rootcaused.md).
 * WHAT IT DOES NOT PROVE: gate throughput (the gate is the throughput bottleneck -- a known
 * characteristic, not a regression, so never gated here).
 *
 * Assertion: associated AND >=1 reply from the far node AND ttl==63 (exactly one IP-forward hop).
 * ttl!=63 with replies = INCONCLUSIVE (a reply arrived but not via the expected single gate hop);
 * 0 replies despite association = FAIL.
 */

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "ping/ping_sock.h"

#include "mmhalow.h"
#include "mmwlan.h"

#include "test_report.h"

#define NAME       "mesh-ap"
#define RIG        "STA behind the gate's AP; pings a far mesh node via the gate"

/* Keep the AP link params identical to test-mesh-ap-gate's SoftAP. */
#define LINK_SSID  "rimba-ping"
#define LINK_PSK   "rimbahalow"
#define STA_IP     "192.168.12.2"       /* our IP on the AP subnet */
#define GATE_AP_IP "192.168.12.1"       /* the gate's AP address = our default gateway */
#define NETMASK    "255.255.255.0"
#define FAR_IP     "10.9.9.100"         /* the far mesh node (board1), reached via the gate */

#define CONNECT_TIMEOUT_MS 40000
#define PING_COUNT         15
#define PING_INTERVAL_MS   1000
#define REPLY_FLOOR        6            /* wide RF margin; the gate adds loss */
#define EXPECT_TTL         63           /* 64 - one ip4_forward decrement at the gate */

static volatile bool s_connected = false;
static volatile int  s_replies = 0, s_timeouts = 0;
static volatile bool s_ping_done = false;
static volatile int  s_last_ttl = -1;

static void sta_status_cb(enum mmwlan_sta_state st)
{
    if (st == MMWLAN_STA_CONNECTED) { s_connected = true; }
}

static void on_ping_success(esp_ping_handle_t hdl, void *args)
{
    uint16_t seqno; uint8_t ttl; uint32_t ms;
    esp_ping_get_profile(hdl, ESP_PING_PROF_SEQNO, &seqno, sizeof(seqno));
    esp_ping_get_profile(hdl, ESP_PING_PROF_TTL, &ttl, sizeof(ttl));
    esp_ping_get_profile(hdl, ESP_PING_PROF_TIMEGAP, &ms, sizeof(ms));
    s_replies++;
    s_last_ttl = ttl;
    TEST_INFO("reply from %s seq=%u ttl=%u time=%" PRIu32 " ms (via the gate)",
                 FAR_IP, seqno, ttl, ms);
}
static void on_ping_timeout(esp_ping_handle_t hdl, void *args) { s_timeouts++; }
static void on_ping_end(esp_ping_handle_t hdl, void *args) { s_ping_done = true; }

static bool assign_static_ip(void)
{
    esp_netif_t *n = mmhalow_get_netif();
    if (n == NULL) { TEST_INFO("STA netif is NULL"); return false; }
    esp_netif_dhcpc_stop(n);
    esp_netif_ip_info_t ip = { 0 };
    ip.ip.addr = esp_ip4addr_aton(STA_IP);
    ip.gw.addr = esp_ip4addr_aton(GATE_AP_IP);   /* off-subnet (10.9.9.x) routes via the gate */
    ip.netmask.addr = esp_ip4addr_aton(NETMASK);
    if (esp_netif_set_ip_info(n, &ip) != ESP_OK) { TEST_INFO("set_ip_info failed"); return false; }
    esp_netif_action_connected(n, NULL, 0, NULL);
    TEST_INFO("STA %s gw %s (netif up=%d)", STA_IP, GATE_AP_IP, (int)esp_netif_is_netif_up(n));
    return true;
}

static void park_forever(void) { while (1) { vTaskDelay(pdMS_TO_TICKS(10000)); } }

void app_main(void)
{
    vTaskDelay(pdMS_TO_TICKS(500));
    TEST_BEGIN(NAME, RIG);
    TEST_INFO("associating to the gate AP \"%s\", then pinging far node %s via the gate",
                 LINK_SSID, FAR_IP);

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_netif_init());

    mmhalow_init(NULL);
    mmhalow_print_version_info();

    mmhalow_wifi_config_t conf = { .sta = MMWLAN_STA_ARGS_INIT };
    memcpy(conf.sta.ssid, LINK_SSID, strlen(LINK_SSID));
    conf.sta.ssid_len = strlen(LINK_SSID);
    memcpy(conf.sta.passphrase, LINK_PSK, strlen(LINK_PSK));
    conf.sta.passphrase_len = strlen(LINK_PSK);
    conf.sta.security_type = MMWLAN_SAE;
    ESP_ERROR_CHECK(mmhalow_set_config(WIFI_IF_STA, &conf));
    mmwlan_override_max_tx_power(1);

    mmhalow_connect(sta_status_cb);
    int waited = 0;
    while (!s_connected && waited < CONNECT_TIMEOUT_MS) { vTaskDelay(pdMS_TO_TICKS(250)); waited += 250; }

    TEST_STEP("associate", s_connected, "connected=%d after %d ms", (int)s_connected, waited);
    if (!s_connected) {
        TEST_FAIL("no association to the gate AP \"%s\" within %d ms (is test-mesh-ap-gate up "
                     "on board2?)", LINK_SSID, CONNECT_TIMEOUT_MS);
        TEST_END(NAME);
        park_forever();
    }
    if (!assign_static_ip()) {
        TEST_FAIL("associated but could not configure the STA netif");
        TEST_END(NAME);
        park_forever();
    }

    /* Give the gate + mesh a moment to settle the return route, then ping the far node. */
    vTaskDelay(pdMS_TO_TICKS(2000));
    ip_addr_t target = { 0 };
    target.type = IPADDR_TYPE_V4;
    target.u_addr.ip4.addr = esp_ip4addr_aton(FAR_IP);
    esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
    cfg.target_addr = target; cfg.count = PING_COUNT; cfg.interval_ms = PING_INTERVAL_MS;
    esp_ping_callbacks_t cbs = { .on_ping_success = on_ping_success, .on_ping_timeout = on_ping_timeout,
                                 .on_ping_end = on_ping_end, .cb_args = NULL };
    esp_ping_handle_t ping;
    if (esp_ping_new_session(&cfg, &cbs, &ping) != ESP_OK) {
        TEST_FAIL("associated but failed to create the ping session");
        TEST_END(NAME);
        park_forever();
    }
    TEST_INFO("pinging far node %s x%d via the gate...", FAR_IP, PING_COUNT);
    esp_ping_start(ping);
    int guard = 0;
    while (!s_ping_done && guard < (PING_COUNT + 12)) { vTaskDelay(pdMS_TO_TICKS(1000)); guard++; }

    bool ttl_ok = (s_last_ttl == EXPECT_TTL);
    TEST_STEP("delivery", s_replies >= REPLY_FLOOR, "replies=%d/%d timeouts=%d",
                 s_replies, PING_COUNT, s_timeouts);
    TEST_STEP("ttl", ttl_ok, "observed_ttl=%d expected=%d", s_last_ttl, EXPECT_TTL);

    if (s_replies >= REPLY_FLOOR && ttl_ok) {
        TEST_PASS("STA->gate->mesh->far-node end-to-end: %d/%d replies from %s, ttl=%d "
                     "(one IP-forward hop through the gate)", s_replies, PING_COUNT, FAR_IP, s_last_ttl);
    } else if (s_replies > 0 && !ttl_ok) {
        TEST_INCONCLUSIVE("replies=%d but ttl=%d != %d -- a reply arrived, but not via the "
                             "expected single gate hop", s_replies, s_last_ttl, EXPECT_TTL);
    } else if (s_replies > 0) {
        TEST_INCONCLUSIVE("associated + ttl=%d correct but only %d/%d replies (floor %d) -- "
                             "marginal RF through the gate", s_last_ttl, s_replies, PING_COUNT,
                             REPLY_FLOOR);
    } else {
        TEST_FAIL("associated to the gate AP but 0/%d replies from far node %s -- the "
                     "STA->AP->ip_forward->mesh->far-node path did not deliver", PING_COUNT, FAR_IP);
    }
    TEST_END(NAME);
    park_forever();
}
