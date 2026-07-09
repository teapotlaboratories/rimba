/*
 * rimba-downlink-test (§3b) — downlink-while-dozing. board2 becomes a pingable STA at 192.168.12.51,
 * dozes in Dynamic PS, and the AP pings it at 1 Hz. Confirms the AP buffers + delivers downlink to a
 * dozing STA (at DTIM), and measures the STA current under a 1 Hz downlink load.
 *   - ESP32 AP (board0): built-in pinger already targets 192.168.12.<mac[5]> = .51.
 *   - Linux AP (chronite): run  ping -i 1 192.168.12.51  by hand.
 * board2 MAC bc:2a:33:96:b2:33 -> .51.
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
#include "driver/gpio.h"
#include "mmhalow.h"
#include "mmwlan.h"

#define LINK_SSID "rimba-ping"
#define LINK_PSK  "rimbahalow"
#define GUARD_PIN GPIO_NUM_6
#define STA_IP    "192.168.12.51"
#define GW_IP     "192.168.12.1"
#define NETMASK   "255.255.255.0"
#define HOLD_S    50

static const char *TAG = "downlink";
static volatile bool s_connected = false;
static void sta_status_cb(enum mmwlan_sta_state st) { s_connected = (st == MMWLAN_STA_CONNECTED); }

static void assign_static_ip(void)
{
    esp_netif_t *n = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!n) { ESP_LOGE(TAG, "no WIFI_STA_DEF netif"); return; }
    esp_netif_dhcpc_stop(n);
    esp_netif_ip_info_t ip = { 0 };
    ip.ip.addr = esp_ip4addr_aton(STA_IP);
    ip.gw.addr = esp_ip4addr_aton(GW_IP);
    ip.netmask.addr = esp_ip4addr_aton(NETMASK);
    ESP_ERROR_CHECK(esp_netif_set_ip_info(n, &ip));
    esp_netif_action_connected(n, NULL, 0, NULL);
    ESP_LOGW(TAG, "=== static IP %s — pingable by the AP ===", STA_IP);
}

void app_main(void)
{
    vTaskDelay(pdMS_TO_TICKS(200));
    gpio_reset_pin(GUARD_PIN);
    gpio_set_direction(GUARD_PIN, GPIO_MODE_INPUT);
    gpio_set_pull_mode(GUARD_PIN, GPIO_PULLDOWN_ONLY);
    vTaskDelay(pdMS_TO_TICKS(50));
    if (gpio_get_level(GUARD_PIN) == 1) { ESP_LOGW(TAG, "FLASH-HOLD"); while (1) vTaskDelay(pdMS_TO_TICKS(3000)); }
    ESP_LOGW(TAG, "fresh boot — 6 s flash window");
    for (int i = 0; i < 6; i++) vTaskDelay(pdMS_TO_TICKS(1000));

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_netif_init());
    mmhalow_init(NULL);
    mmhalow_print_version_info();
    mmhalow_wifi_config_t conf = { .sta = MMWLAN_STA_ARGS_INIT };
    memcpy(conf.sta.ssid, LINK_SSID, strlen(LINK_SSID)); conf.sta.ssid_len = strlen(LINK_SSID);
    memcpy(conf.sta.passphrase, LINK_PSK, strlen(LINK_PSK)); conf.sta.passphrase_len = strlen(LINK_PSK);
    conf.sta.security_type = MMWLAN_SAE;
    ESP_ERROR_CHECK(mmhalow_set_config(WIFI_IF_STA, &conf));

    mmwlan_override_max_tx_power(1);
    mmhalow_connect(sta_status_cb);
    int w = 0; while (!s_connected && w < 30000) { vTaskDelay(pdMS_TO_TICKS(100)); w += 100; }
    if (!s_connected) { ESP_LOGW(TAG, "connect timeout"); while (1) vTaskDelay(pdMS_TO_TICKS(5000)); }
    ESP_LOGW(TAG, "=== CONNECTED ===");

    assign_static_ip();
    vTaskDelay(pdMS_TO_TICKS(1000));

    mmwlan_set_power_save_mode(MMWLAN_PS_ENABLED);
    ESP_LOGW(TAG, "=== DYNAMIC PS + 1 Hz downlink — holding %ds (AP pinging .51) ===", HOLD_S);
    for (int t = 0; t < HOLD_S; t++) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if ((t % 10) == 9) ESP_LOGW(TAG, "    dyn-PS+downlink +%ds connected=%d", t+1, (int)s_connected);
    }
    ESP_LOGW(TAG, "=== downlink hold done, connected=%d ===", (int)s_connected);
    while (1) vTaskDelay(pdMS_TO_TICKS(5000));
}
