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

/* iperf throughput test (temporary): associate, pin the IP, then skip the TWT test + ping and
 * start an esp_console REPL so `iperf -c 192.168.12.1` can be driven over serial. Comment out
 * for normal operation. */
#define IPERF 1

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

    /* Zero-config: DHCP from the gateway AP (IP + router). mmhalow starts a dhcpc on link-up; make sure
     * it's running and wait for the lease instead of pinning a static IP (task #5 de-hardcode). */
    esp_err_t de = esp_netif_dhcpc_start(n);
    if (de != ESP_OK && de != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED)
        ESP_LOGW(TAG, "dhcpc_start: %s", esp_err_to_name(de));

    esp_netif_ip_info_t ip = { 0 };
    for (int i = 0; i < 40 && ip.ip.addr == 0; i++) {
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_netif_get_ip_info(n, &ip);
    }
    if (ip.ip.addr == 0)
        ESP_LOGE(TAG, "DHCP: no lease after 20s");
    else
        ESP_LOGI(TAG, "DHCP lease " IPSTR " gw " IPSTR " mask " IPSTR " (up=%d)",
                 IP2STR(&ip.ip), IP2STR(&ip.gw), IP2STR(&ip.netmask), (int)esp_netif_is_netif_up(n));

#ifndef IPERF
    start_ping(ip.gw);
#endif
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

#ifdef IPERF
#include "esp_console.h"
#include "iperf_cmd.h"
#include "ping_cmd.h"
static void start_iperf_console(void)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = "iperf>";
#if CONFIG_ESP_CONSOLE_UART
    esp_console_dev_uart_config_t dev_config = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&dev_config, &repl_config, &repl));
#elif CONFIG_ESP_CONSOLE_USB_CDC
    esp_console_dev_usb_cdc_config_t dev_config = ESP_CONSOLE_DEV_CDC_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_cdc(&dev_config, &repl_config, &repl));
#elif CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    esp_console_dev_usb_serial_jtag_config_t dev_config = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_serial_jtag(&dev_config, &repl_config, &repl));
#endif
    app_register_iperf_commands();
    ping_cmd_register_ping();
    ESP_ERROR_CHECK(esp_console_start_repl(repl));
    ESP_LOGW(TAG, "iperf console ready: `iperf -c 192.168.12.1` (server is the AP)");
}
#endif

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

#ifdef IPERF
    /* iperf mode: wait for association, then start the console and skip the TWT test. */
    for (int w = 0; !s_connected && w < 60000; w += 500) { vTaskDelay(pdMS_TO_TICKS(500)); }
    ESP_LOGW(TAG, "iperf mode: link %s", s_connected ? "up" : "DOWN");
    start_iperf_console();
    return;
#endif

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
