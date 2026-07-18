/*
 * rimba-halow-sta-perf — HaLow station node for the iperf throughput test.
 *
 * Associates to the rimba-halow-ap-perf SoftAP (SAE), takes an IP by DHCP, then
 * starts an esp_console REPL so you can drive `iperf -c <AP>` over the serial
 * console and measure real 802.11ah TCP/UDP throughput from the STA side.
 *
 *   LINK PARAMS BELOW MUST MATCH firmware/rimba-halow-ap-perf/main/app_main.c.
 */

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_console.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"

#include "iperf_cmd.h"
#include "ping_cmd.h"
#include "mmhalow.h"

/* --- Link parameters (keep identical to rimba-halow-ap-perf) ----------------- */
#define LINK_SSID  "rimba-ping"        /* change for your network (must match the AP) */
#define LINK_PSK   "rimbahalow"        /* >= 8 chars, required for SAE; must match the AP */
#define AP_IP      "192.168.12.1"      /* the AP = the iperf server; change for your network */
/* ---------------------------------------------------------------------------- */

static const char *TAG = "rimba-sta";
static SemaphoreHandle_t s_link_up;          /* consumed by net_task (DHCP) */
static volatile bool s_connected = false;    /* sticky flag app_main waits on before the console */

/* Runs once the link is up: take a DHCP lease so iperf has an IP to run over. */
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

    /* Zero-config: DHCP from the gateway AP (IP + router). mmhalow starts a dhcpc on link-up; make
     * sure it's running and wait for the lease instead of pinning a static IP. */
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
    ESP_LOGW(TAG, "iperf console ready: run `iperf -c %s` (the AP is the server)", AP_IP);
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

    ESP_LOGI(TAG, "Connecting to \"%s\"...", LINK_SSID);
    mmhalow_connect(sta_status_cb);

    /* Wait for association (net_task takes the DHCP lease in the background), then bring up
     * the console so `iperf -c <AP>` can be driven over serial. */
    for (int w = 0; !s_connected && w < 60000; w += 500) { vTaskDelay(pdMS_TO_TICKS(500)); }
    ESP_LOGW(TAG, "link %s", s_connected ? "up" : "DOWN");
    start_iperf_console();
}
