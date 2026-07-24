/*
 * test-mesh-gate-sta — an AP client behind the gate, in one of two modes (selected at build time):
 *
 *  - DEFAULT (reporter): a DHCP client that pings a mesh node (10.9.9.100) zero-config across the flat
 *    L2 bridge (mesh-gate-bridge). A reply proves the whole retire-L3 bridge: DHCP, cross-bridge ARP (B1),
 *    S5b/S5c, round-trip, proxy-ARP. TTL==64 proves it is a pure L2 bridge (ip_forward would drop it to 63).
 *
 *  - STA_IP=x NO_PING=1 (responder, a SUPPORT role): a SILENT static AP-client at x that never originates
 *    traffic (so it never proactively teaches the gate its mapping). Used as the cold target a mesh node
 *    resolves in mesh-gate-b2 (Case D). No TEST| verdict -- the harness reads its "STA static IP" up_marker.
 *
 * All test roles the harness flashes are test-* apps (the split rule). Gate = test-mesh-gate-ap.
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
#include "lwip/ip4_addr.h"

#include "mmhalow.h"
#include "mmwlan.h"

#include "test_report.h"

/* The two build flags are coupled: the NO_PING responder branch needs assign_static_ip() (TEST_STATIC_IP)
 * and the DHCP reporter branch needs wait_for_dhcp_ip() (the #else) -- so exactly one of them defined is a
 * broken build. Fail loudly here instead of with an opaque "undefined reference". Valid: both (silent static
 * responder) or neither (DHCP pinger reporter). */
#if defined(TEST_NO_PING) ^ defined(TEST_STATIC_IP)
#error "test-mesh-gate-sta: pass STA_IP= and NO_PING=1 together (silent static responder), or neither (DHCP reporter)."
#endif

#define NAME       "mesh-gate-bridge"
#define RIG        "STA (DHCP) behind the gate's AP; pings a mesh node zero-config across the L2 bridge"

/* Match the gate's SoftAP (test-mesh-gate-ap / rimba-halow-mesh-ap). */
#define LINK_SSID  "rimba-ping"
#define LINK_PSK   "rimbahalow"
#define FAR_IP     "10.9.9.100"         /* the mesh node (reporter mode pings it across the L2 bridge) */
#define NETMASK    "255.255.255.0"

#define CONNECT_TIMEOUT_MS 40000
#define DHCP_TIMEOUT_MS    30000
#define PING_COUNT         15
#define PING_INTERVAL_MS   1000
#define REPLY_FLOOR        6            /* wide RF margin; the bridge + proxy-ARP add some loss */
#define EXPECT_TTL         64           /* L2 bridge does NOT decrement TTL (ip_forward would -> 63) */

static const char *TAG = "test-mesh-gate-sta";
static volatile bool s_connected = false;

static void sta_status_cb(enum mmwlan_sta_state st)
{
    if (st == MMWLAN_STA_CONNECTED) { s_connected = true; }
}

#ifndef TEST_NO_PING
static volatile int  s_replies = 0, s_timeouts = 0;
static volatile bool s_ping_done = false;
static volatile int  s_last_ttl = -1;

static void on_ping_success(esp_ping_handle_t hdl, void *args)
{
    uint16_t seqno; uint8_t ttl; uint32_t ms;
    esp_ping_get_profile(hdl, ESP_PING_PROF_SEQNO, &seqno, sizeof(seqno));
    esp_ping_get_profile(hdl, ESP_PING_PROF_TTL, &ttl, sizeof(ttl));
    esp_ping_get_profile(hdl, ESP_PING_PROF_TIMEGAP, &ms, sizeof(ms));
    s_replies++;
    s_last_ttl = ttl;
    TEST_INFO("reply from %s seq=%u ttl=%u time=%" PRIu32 " ms (across the L2 bridge)", FAR_IP, seqno, ttl, ms);
}
static void on_ping_timeout(esp_ping_handle_t hdl, void *args) { s_timeouts++; }
static void on_ping_end(esp_ping_handle_t hdl, void *args) { s_ping_done = true; }
#endif

#ifdef TEST_STATIC_IP
/* Pin a static IP (responder mode / a fixed-address test). gw=0 on the flat single subnet. */
static bool assign_static_ip(void)
{
    esp_netif_t *n = mmhalow_get_netif();
    if (n == NULL) { ESP_LOGE(TAG, "STA netif is NULL"); return false; }
    esp_netif_dhcpc_stop(n);
    esp_netif_ip_info_t ip = { 0 };
    ip.ip.addr = esp_ip4addr_aton(TEST_STATIC_IP);
    ip.netmask.addr = esp_ip4addr_aton(NETMASK);
    if (esp_netif_set_ip_info(n, &ip) != ESP_OK) { ESP_LOGE(TAG, "set_ip_info failed"); return false; }
    esp_netif_action_connected(n, NULL, 0, NULL);
    return true;
}
#else
/* mmhalow's STA netif is a DHCP client (AUTOUP + dhcpc on link-up), so wait for the gate's SoftAP to
 * hand out a real 10.9.9.x lease (non-zero, non-link-local). */
static uint32_t wait_for_dhcp_ip(void)
{
    esp_netif_t *n = mmhalow_get_netif();
    if (n == NULL) { ESP_LOGE(TAG, "STA netif is NULL"); return 0; }
    esp_netif_ip_info_t ip = { 0 };
    for (int i = 0; i < DHCP_TIMEOUT_MS / 500; i++) {
        if (esp_netif_get_ip_info(n, &ip) == ESP_OK && ip.ip.addr != 0 &&
            !ip4_addr_islinklocal((const ip4_addr_t *)&ip.ip)) {
            return ip.ip.addr;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    return 0;
}
#endif

static void park_forever(void) { while (1) { vTaskDelay(pdMS_TO_TICKS(10000)); } }

static bool associate(void)
{
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
    return s_connected;
}

void app_main(void)
{
    vTaskDelay(pdMS_TO_TICKS(500));

#ifdef TEST_NO_PING
    /* RESPONDER (Case D support role): a SILENT static AP-client; lwIP auto-responds. No TEST| verdict. */
    if (!associate()) { park_forever(); }        /* harness catches the missing up_marker */
    if (!assign_static_ip()) { park_forever(); }
    ESP_LOGI(TAG, "STA static IP %s (responder-only, no ping) — waiting for a mesh node to reach us",
             TEST_STATIC_IP);                     /* up_marker */
    park_forever();
#else
    /* REPORTER (Case C): DHCP a lease, then ping the mesh node zero-config across the L2 bridge. */
    TEST_BEGIN(NAME, RIG);
    TEST_INFO("associating to the gate AP \"%s\", DHCP, then pinging mesh node %s across the bridge",
                 LINK_SSID, FAR_IP);
    if (!associate()) {
        TEST_FAIL("no association to the gate AP \"%s\" within %d ms (is the gate test-mesh-gate-ap up "
                     "on board2?)", LINK_SSID, CONNECT_TIMEOUT_MS);
        TEST_END(NAME);
        park_forever();
    }

    uint32_t lease = wait_for_dhcp_ip();
    TEST_STEP("dhcp", lease != 0, "lease=" IPSTR, IP2STR((esp_ip4_addr_t *)&lease));
    if (lease == 0) {
        TEST_FAIL("associated but no DHCP lease within %d ms -- is the gate serving DHCP on 10.9.9.x?",
                     DHCP_TIMEOUT_MS);
        TEST_END(NAME);
        park_forever();
    }

    vTaskDelay(pdMS_TO_TICKS(3000));  /* let proxy-ARP settle the cross-bridge mapping */
    ip_addr_t target = { 0 };
    target.type = IPADDR_TYPE_V4;
    target.u_addr.ip4.addr = esp_ip4addr_aton(FAR_IP);
    esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
    cfg.target_addr = target; cfg.count = PING_COUNT; cfg.interval_ms = PING_INTERVAL_MS;
    esp_ping_callbacks_t cbs = { .on_ping_success = on_ping_success, .on_ping_timeout = on_ping_timeout,
                                 .on_ping_end = on_ping_end, .cb_args = NULL };
    esp_ping_handle_t ping;
    if (esp_ping_new_session(&cfg, &cbs, &ping) != ESP_OK) {
        TEST_FAIL("associated + leased but failed to create the ping session");
        TEST_END(NAME);
        park_forever();
    }
    TEST_INFO("pinging mesh node %s x%d (zero-config across the L2 bridge + proxy-ARP)...", FAR_IP, PING_COUNT);
    esp_ping_start(ping);
    int guard = 0;
    while (!s_ping_done && guard < (PING_COUNT + 12)) { vTaskDelay(pdMS_TO_TICKS(1000)); guard++; }

    bool ttl_ok = (s_last_ttl == EXPECT_TTL);
    TEST_STEP("delivery", s_replies >= REPLY_FLOOR, "replies=%d/%d timeouts=%d", s_replies, PING_COUNT, s_timeouts);
    TEST_STEP("ttl", ttl_ok, "observed_ttl=%d expected=%d (L2 bridge, no ip_forward)", s_last_ttl, EXPECT_TTL);

    if (s_replies >= REPLY_FLOOR && ttl_ok) {
        TEST_PASS("STA DHCP -> mesh node %s zero-config across the L2 bridge: %d/%d replies, ttl=%d "
                     "(pure L2 bridge -- no ip_forward hop); proves DHCP + B1 + S5b/S5c + round-trip + proxy-ARP",
                     FAR_IP, s_replies, PING_COUNT, s_last_ttl);
    } else if (s_replies > 0 && !ttl_ok) {
        TEST_INCONCLUSIVE("replies=%d but ttl=%d != %d -- a reply arrived, but not via a plain L2 bridge "
                             "(ip_forward re-added?)", s_replies, s_last_ttl, EXPECT_TTL);
    } else if (s_replies > 0) {
        TEST_INCONCLUSIVE("leased + ttl=%d correct but only %d/%d replies (floor %d) -- marginal RF / "
                             "proxy-ARP resolution across the bridge", s_last_ttl, s_replies, PING_COUNT, REPLY_FLOOR);
    } else {
        TEST_FAIL("associated + DHCP-leased but 0/%d replies from mesh node %s -- the L2 bridge / proxy-ARP "
                     "did not deliver the zero-config round-trip", PING_COUNT, FAR_IP);
    }
    TEST_END(NAME);
    park_forever();
#endif
}
