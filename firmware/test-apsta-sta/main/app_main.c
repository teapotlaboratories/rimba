/*
 * test-apsta-sta — the STA (reporter) role of the AP<->STA association+ping T2 test.
 *
 * Pairs with the AP role, which is the EXISTING `rimba-halow-ap` app (unmodified): it brings up
 * an 802.11ah SoftAP (SSID "rimba-ping", SAE), pins the static IP 192.168.12.1, and answers ICMP.
 * This app is the mirror: it associates to that AP, pins 192.168.12.2, pings the AP, and reports
 * a machine-readable verdict on the console via the TEST| contract. The T2 orchestrator
 * (tools/regtest/t2_onair.py) flashes rimba-halow-ap to the AP board and this to the STA board,
 * then scrapes THIS board's TEST|RESULT line as the test verdict.
 *
 * Link params below MUST match firmware/rimba-halow-ap/main/app_main.c.
 *
 * WHAT IT PROVES: an ESP STA completes the SAE association to an ESP SoftAP and IP data flows
 * (ICMP round-trips) over the HaLow link.
 * WHAT IT DOES NOT PROVE: throughput, power-save, or the AID>=64 ceiling (that needs 64+ STAs).
 *
 * Assertion strategy (structural, not RF-noisy): association is the binary structural fact;
 * the reply COUNT is RF-bound, so PASS needs association + a WIDE floor of replies, a partial
 * count is INCONCLUSIVE (marginal link, not a code regression), and zero replies despite
 * association is a FAIL (the IP/ICMP path is broken, not merely lossy — at close bench range the
 * recorded link is ~0% loss, so 0/15 is a real break).
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

#define NAME       "ap-sta-ping"
#define RIG        "AP=rimba-halow-ap on one board, STA(this)=other board"

/* Keep identical to rimba-halow-ap. */
#define LINK_SSID  "rimba-ping"
#define LINK_PSK   "rimbahalow"
#define AP_IP      "192.168.12.1"     /* the AP — our ping target */
#define STA_IP     "192.168.12.2"
#define NETMASK    "255.255.255.0"

#define CONNECT_TIMEOUT_MS 40000
#define PING_COUNT         15
#define PING_INTERVAL_MS   1000
#define REPLY_FLOOR        8          /* >= this many of PING_COUNT => PASS (wide RF margin) */

static volatile bool s_connected = false;
static volatile int  s_replies = 0;
static volatile int  s_timeouts = 0;
static volatile bool s_ping_done = false;

static void sta_status_cb(enum mmwlan_sta_state st)
{
    if (st == MMWLAN_STA_CONNECTED) { s_connected = true; }
}

static void on_ping_success(esp_ping_handle_t hdl, void *args)
{
    uint16_t seqno; uint32_t elapsed_ms;
    esp_ping_get_profile(hdl, ESP_PING_PROF_SEQNO, &seqno, sizeof(seqno));
    esp_ping_get_profile(hdl, ESP_PING_PROF_TIMEGAP, &elapsed_ms, sizeof(elapsed_ms));
    s_replies++;
    TEST_INFO("reply from %s seq=%u time=%" PRIu32 " ms", AP_IP, seqno, elapsed_ms);
}

static void on_ping_timeout(esp_ping_handle_t hdl, void *args)
{
    s_timeouts++;
    TEST_INFO("ping timeout (%d so far)", s_timeouts);
}

static void on_ping_end(esp_ping_handle_t hdl, void *args)
{
    uint32_t sent = 0, recv = 0;
    esp_ping_get_profile(hdl, ESP_PING_PROF_REQUEST, &sent, sizeof(sent));
    esp_ping_get_profile(hdl, ESP_PING_PROF_REPLY, &recv, sizeof(recv));
    TEST_INFO("ping session end: sent=%" PRIu32 " recv=%" PRIu32, sent, recv);
    s_ping_done = true;
}

/* mmhalow's STA netif is a DHCP client, but the SoftAP runs no DHCP server, so pin a static IP
 * on the same subnet as the AP (mirrors rimba-halow-ap:assign_static_ip). */
static bool assign_static_ip(void)
{
    esp_netif_t *n = mmhalow_get_netif();
    if (n == NULL) { TEST_INFO("STA netif is NULL"); return false; }
    esp_netif_dhcpc_stop(n);

    esp_netif_ip_info_t ip = { 0 };
    ip.ip.addr = esp_ip4addr_aton(STA_IP);
    ip.gw.addr = esp_ip4addr_aton(AP_IP);
    ip.netmask.addr = esp_ip4addr_aton(NETMASK);
    if (esp_netif_set_ip_info(n, &ip) != ESP_OK) { TEST_INFO("set_ip_info failed"); return false; }
    esp_netif_action_connected(n, NULL, 0, NULL);
    TEST_INFO("STA static IP %s, netif up=%d", STA_IP, (int)esp_netif_is_netif_up(n));
    return true;
}

static void park_forever(void)
{
    /* Never sleep: keeps the native USB enumerated so the board stays reflashable. */
    while (1) { vTaskDelay(pdMS_TO_TICKS(10000)); }
}

void app_main(void)
{
    vTaskDelay(pdMS_TO_TICKS(500));
    TEST_BEGIN(NAME, RIG);
    TEST_INFO("associating to SSID \"%s\" (SAE), then pinging the AP at %s", LINK_SSID, AP_IP);

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

    /* Close-bench RX-overload workaround: cap TX to 1 dBm so the AP's receiver isn't saturated
     * (mirrors rimba-halow-ap + rimba-halow-sta). Remove for a real deployment. */
    mmwlan_override_max_tx_power(1);

    mmhalow_connect(sta_status_cb);
    int waited = 0;
    while (!s_connected && waited < CONNECT_TIMEOUT_MS) { vTaskDelay(pdMS_TO_TICKS(250)); waited += 250; }

    TEST_STEP("associate", s_connected, "connected=%d after %d ms", (int)s_connected, waited);
    if (!s_connected) {
        TEST_FAIL("no association to \"%s\" within %d ms -- SAE/link failed (is the AP "
                     "(rimba-halow-ap) up on the peer board?)", LINK_SSID, CONNECT_TIMEOUT_MS);
        TEST_END(NAME);
        park_forever();
    }

    if (!assign_static_ip()) {
        TEST_FAIL("associated but could not configure the STA netif -- IP path unavailable");
        TEST_END(NAME);
        park_forever();
    }

    /* Ping the AP. */
    ip_addr_t target = { 0 };
    target.type = IPADDR_TYPE_V4;
    target.u_addr.ip4.addr = esp_ip4addr_aton(AP_IP);
    esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
    cfg.target_addr = target;
    cfg.count = PING_COUNT;
    cfg.interval_ms = PING_INTERVAL_MS;
    esp_ping_callbacks_t cbs = {
        .on_ping_success = on_ping_success,
        .on_ping_timeout = on_ping_timeout,
        .on_ping_end = on_ping_end,
        .cb_args = NULL,
    };
    esp_ping_handle_t ping;
    if (esp_ping_new_session(&cfg, &cbs, &ping) != ESP_OK) {
        TEST_FAIL("associated but failed to create the ping session");
        TEST_END(NAME);
        park_forever();
    }
    TEST_INFO("pinging %s x%d @ %d ms...", AP_IP, PING_COUNT, PING_INTERVAL_MS);
    esp_ping_start(ping);

    int guard = 0;
    while (!s_ping_done && guard < (PING_COUNT + 10)) { vTaskDelay(pdMS_TO_TICKS(1000)); guard++; }

    TEST_STEP("ping", s_replies >= REPLY_FLOOR, "replies=%d/%d timeouts=%d",
                 s_replies, PING_COUNT, s_timeouts);

    if (s_replies >= REPLY_FLOOR) {
        TEST_PASS("associated (SAE) + %d/%d ICMP replies from %s", s_replies, PING_COUNT, AP_IP);
    } else if (s_replies > 0) {
        TEST_INCONCLUSIVE("associated but only %d/%d replies (floor %d) -- marginal RF link, "
                             "not a clear code regression", s_replies, PING_COUNT, REPLY_FLOOR);
    } else {
        TEST_FAIL("associated but 0/%d ICMP replies -- the IP/ICMP path is broken (at close "
                     "bench range the link is ~0%% loss, so 0 replies is not mere RF loss)",
                     PING_COUNT);
    }
    TEST_END(NAME);
    park_forever();
}
