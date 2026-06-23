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
static SemaphoreHandle_t s_link_up;

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

    esp_netif_ip_info_t ip = { 0 };
    ip.ip.addr = esp_ip4addr_aton(STA_IP);
    ip.gw.addr = esp_ip4addr_aton(AP_IP);
    ip.netmask.addr = esp_ip4addr_aton(NETMASK);
    ESP_ERROR_CHECK(esp_netif_set_ip_info(n, &ip));
    ESP_LOGI(TAG, "static IP " IPSTR ", gw " IPSTR " (up=%d)",
             IP2STR(&ip.ip), IP2STR(&ip.gw), (int)esp_netif_is_netif_up(n));

    start_ping(ip.gw);
    vTaskDelete(NULL);
}

static void sta_status_cb(enum mmwlan_sta_state st)
{
    switch (st) {
        case MMWLAN_STA_DISABLED:   ESP_LOGW(TAG, "STA disabled"); break;
        case MMWLAN_STA_CONNECTING: ESP_LOGI(TAG, "STA connecting..."); break;
        case MMWLAN_STA_CONNECTED:  ESP_LOGI(TAG, "STA link up"); xSemaphoreGive(s_link_up); break;
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

    /* Request a TWT agreement BEFORE associating (morselib requires
     * mmwlan_twt_add_configuration to be called before mmwlan_sta_enable, which
     * mmhalow_connect drives). TWT is the AP/STA HaLow power-save that is NOT
     * available in IBSS (STA/AP-only) — as a STA on chronium's AP it becomes usable.
     * Requester role, ~5 s service-period interval, ~65 ms min wake duration. */
    struct mmwlan_twt_config_args twt = MMWLAN_TWT_CONFIG_ARGS_INIT;
    twt.twt_mode = MMWLAN_TWT_REQUESTER;
    twt.twt_wake_interval_us = 1000000;        /* wake ~every 1 s (test: < ping 2s timeout) */
    twt.twt_min_wake_duration_us = 65536;      /* ~65 ms awake per service period */
    twt.twt_setup_command = MMWLAN_TWT_SETUP_REQUEST;
    enum mmwlan_status twt_st = mmwlan_twt_add_configuration(&twt);
    ESP_LOGI(TAG, "TWT add_configuration (requester, 5s/65ms) -> %d (0=OK)", twt_st);

    ESP_LOGI(TAG, "Connecting to \"%s\"...", LINK_SSID);
    mmhalow_connect(sta_status_cb);
}
