/*
 * rimba-halow-sta — HaLow station node for the 2-board ping test.
 *
 * Associates to the rimba-halow-ap SoftAP, assigns itself a STATIC IP (mmhalow
 * creates a DHCP-client netif but there is no DHCP server in HaLow AP mode, so
 * DHCP never completes), then pings the AP once a second and logs the RTT.
 *
 * The MM6108 has no public IBSS/ADHOC path in morselib, so AP<->STA is the
 * proven two-node link (the RISK-01 fallback).
 *
 *   LINK PARAMS BELOW MUST MATCH firmware/rimba-halow-ap/main/app_main.c.
 */

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "ping/ping_sock.h"

#include "mmhalow.h"

/* --- Link / IP parameters (keep identical to rimba-halow-ap) ----------------- */
#define LINK_SSID  "rimba-ping"
#define LINK_PSK   "rimbahalow"     /* >= 8 chars, required for SAE */
#define STA_IP     "192.168.12.2"
#define AP_IP      "192.168.12.1"   /* the AP / gateway — our ping target */
#define NETMASK    "255.255.255.0"
/* ---------------------------------------------------------------------------- */

static const char *TAG = "rimba-sta";
static SemaphoreHandle_t s_link_up;          /* consumed by net_task (DHCP/static-IP) */
static volatile bool s_connected = false;    /* sticky flag for app_main's action-frame seq */

static void on_ping_success(esp_ping_handle_t hdl, void *args)
{
    uint16_t seqno;
    uint8_t ttl;
    uint32_t elapsed_ms;
    ip_addr_t target = { 0 };
    esp_ping_get_profile(hdl, ESP_PING_PROF_SEQNO, &seqno, sizeof(seqno));
    esp_ping_get_profile(hdl, ESP_PING_PROF_TTL, &ttl, sizeof(ttl));
    esp_ping_get_profile(hdl, ESP_PING_PROF_TIMEGAP, &elapsed_ms, sizeof(elapsed_ms));
    esp_ping_get_profile(hdl, ESP_PING_PROF_IPADDR, &target, sizeof(target));
    ESP_LOGI(TAG, "reply from " IPSTR ": seq=%u ttl=%u time=%" PRIu32 " ms",
             IP2STR(&target.u_addr.ip4), seqno, ttl, elapsed_ms);
}

static void on_ping_timeout(esp_ping_handle_t hdl, void *args)
{
    uint16_t seqno;
    esp_ping_get_profile(hdl, ESP_PING_PROF_SEQNO, &seqno, sizeof(seqno));
    ESP_LOGW(TAG, "ping timeout, seq=%u", seqno);
}

static void start_ping(esp_ip4_addr_t target_ip)
{
    ip_addr_t target = { 0 };
    target.type = IPADDR_TYPE_V4;
    target.u_addr.ip4.addr = target_ip.addr;

    esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
    cfg.target_addr = target;
    cfg.count = ESP_PING_COUNT_INFINITE;
    cfg.interval_ms = 1000;

    esp_ping_callbacks_t cbs = {
        .on_ping_success = on_ping_success,
        .on_ping_timeout = on_ping_timeout,
        .on_ping_end = NULL,
        .cb_args = NULL,
    };

    esp_ping_handle_t ping;
    if (esp_ping_new_session(&cfg, &cbs, &ping) == ESP_OK) {
        ESP_LOGI(TAG, "Pinging AP " IPSTR " every %" PRIu32 " ms...", IP2STR(&target_ip), cfg.interval_ms);
        esp_ping_start(ping);
    } else {
        ESP_LOGE(TAG, "failed to create ping session");
    }
}

/* Runs once the link is up: pin a static IP, then start pinging the AP. */
static void net_task(void *arg)
{
    xSemaphoreTake(s_link_up, portMAX_DELAY);
    /* Let mmhalow's link-up handler (esp_netif_action_connected) settle first. */
    vTaskDelay(pdMS_TO_TICKS(1000));

    esp_netif_t *n = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (n == NULL) {
        ESP_LOGE(TAG, "netif WIFI_STA_DEF not found");
        vTaskDelete(NULL);
    }

    /* mmhalow auto-starts a DHCP client on link-up; stop it and go static. */
    esp_netif_dhcpc_stop(n);

    /* Derive a distinct static IP from this node's MAC (192.168.12.<mac[5]>) so several
     * STAs on one AP don't all land on .2. The AP derives the same address from the MAC
     * it tracks and pings each of us. */
    uint8_t mac[6] = { 0 };
    esp_netif_get_mac(n, mac);
    char ipbuf[20];
    snprintf(ipbuf, sizeof(ipbuf), "192.168.12.%u", (unsigned)mac[5]);

    esp_netif_ip_info_t ip = { 0 };
    ip.ip.addr = esp_ip4addr_aton(ipbuf);
    ip.gw.addr = esp_ip4addr_aton(AP_IP);
    ip.netmask.addr = esp_ip4addr_aton(NETMASK);
    ESP_ERROR_CHECK(esp_netif_set_ip_info(n, &ip));
    ESP_LOGI(TAG, "static IP %s (mac[5]=%u), gw " IPSTR " (up=%d)",
             ipbuf, (unsigned)mac[5], IP2STR(&ip.gw), (int)esp_netif_is_netif_up(n));

    start_ping(ip.gw);
    vTaskDelete(NULL);
}

static void sta_status_cb(enum mmwlan_sta_state st)
{
    switch (st) {
        case MMWLAN_STA_DISABLED:   ESP_LOGW(TAG, "STA disabled"); break;
        case MMWLAN_STA_CONNECTING: ESP_LOGI(TAG, "STA connecting..."); break;
        case MMWLAN_STA_CONNECTED:  ESP_LOGI(TAG, "STA link up"); s_connected = true; xSemaphoreGive(s_link_up); break;
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Booting HaLow STA (ssid=\"%s\")...", LINK_SSID);

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_netif_init());

    s_link_up = xSemaphoreCreateBinary();
    xTaskCreate(net_task, "net", 4096, NULL, 5, NULL);

    mmhalow_init(NULL);
    ESP_LOGI(TAG, "Wi-Fi HaLow initialised");
    mmhalow_print_version_info();

    mmhalow_wifi_config_t conf = { .sta = MMWLAN_STA_ARGS_INIT };

    memcpy(conf.sta.ssid, LINK_SSID, strlen(LINK_SSID));
    conf.sta.ssid_len = strlen(LINK_SSID);

    memcpy(conf.sta.passphrase, LINK_PSK, strlen(LINK_PSK));
    conf.sta.passphrase_len = strlen(LINK_PSK);
    conf.sta.security_type = MMWLAN_SAE;

    ESP_ERROR_CHECK(mmhalow_set_config(WIFI_IF_STA, &conf));

    /* Action-frame TWT test. Connect WITHOUT assoc-IE TWT (so we get a flat-RTT, awake
     * baseline), then negotiate TWT *mid-session* via a TWT-Setup ACTION frame
     * (mmwlan_twt_setup_request) — the association-preserving path. Later tear it down with
     * a TWT-Teardown action frame. Requester role, ~1 s interval, ~65 ms min wake. */
    struct mmwlan_twt_config_args twt = MMWLAN_TWT_CONFIG_ARGS_INIT;
    twt.twt_mode = MMWLAN_TWT_REQUESTER;
    twt.twt_wake_interval_us = 1000000;        /* wake ~every 1 s (test: < ping 2 s timeout) */
    twt.twt_min_wake_duration_us = 65536;      /* ~65 ms awake per service period */
    twt.twt_setup_command = MMWLAN_TWT_SETUP_REQUEST;

    ESP_LOGI(TAG, "Connecting to \"%s\" (no assoc-IE TWT; action-frame test)...", LINK_SSID);
    mmhalow_connect(sta_status_cb);

    /* After association, exercise the mid-session action-frame path. Poll the sticky flag
     * (net_task owns the s_link_up semaphore, so we must not take it here). */
    int waited_ms = 0;
    while (!s_connected && waited_ms < 60000)
    {
        vTaskDelay(pdMS_TO_TICKS(500));
        waited_ms += 500;
    }
    if (s_connected)
    {
        vTaskDelay(pdMS_TO_TICKS(12000));   /* baseline window — expect flat ~10 ms AP->STA RTT */
        enum mmwlan_status st = mmwlan_twt_setup_request(&twt);
        ESP_LOGW(TAG, "=== TWT Setup ACTION frame sent -> %d (0=OK); expect doze RTT now ===", st);
        vTaskDelay(pdMS_TO_TICKS(30000));   /* doze window — expect RTT rising toward ~1 s */
        st = mmwlan_twt_teardown();
        ESP_LOGW(TAG, "=== TWT Teardown ACTION frame sent -> %d (0=OK) ===", st);
    }
    else
    {
        ESP_LOGW(TAG, "link-up timeout; skipped action-frame test");
    }
}
