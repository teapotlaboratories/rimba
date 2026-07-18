/*
 * rimba-halow-sta — join an 802.11ah HaLow SoftAP and ping it (the station example).
 *
 * The station side of the two-board HaLow demo. It brings up the MM6108 radio,
 * associates to a HaLow SoftAP over SAE, pins a static IP on the AP's subnet (the
 * SoftAP runs no DHCP server), and pings the AP continuously, printing the round-trip
 * time so you can watch the link on the console. Pair it with the rimba-halow-ap
 * example running on another board:
 *
 *   make flash APP=rimba-halow-ap  BOARD=proto1-fgh100m PORT=/dev/ttyACM0   # the AP
 *   make flash APP=rimba-halow-sta BOARD=proto1-fgh100m PORT=/dev/ttyACM1   # this STA
 *   make monitor APP=rimba-halow-sta BOARD=proto1-fgh100m PORT=/dev/ttyACM1
 *
 * Change LINK_SSID / LINK_PSK and the IPs below for your own network.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <inttypes.h>
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

static const char *TAG = "rimba-halow-sta";

/* Match the AP (rimba-halow-ap). */
#define LINK_SSID  "rimba-ping"
#define LINK_PSK   "rimbahalow"
#define AP_IP      "192.168.12.1"     /* the AP — our ping target and default gateway */
#define STA_IP     "192.168.12.2"
#define NETMASK    "255.255.255.0"

#define CONNECT_TIMEOUT_MS 40000
#define PING_INTERVAL_MS   2000

static volatile bool s_connected = false;

static void sta_status_cb(enum mmwlan_sta_state st)
{
    if (st == MMWLAN_STA_CONNECTED) {
        s_connected = true;
        ESP_LOGI(TAG, "associated to \"%s\"", LINK_SSID);
    }
}

static void on_ping_success(esp_ping_handle_t hdl, void *args)
{
    uint16_t seqno;
    uint32_t elapsed_ms;
    esp_ping_get_profile(hdl, ESP_PING_PROF_SEQNO, &seqno, sizeof(seqno));
    esp_ping_get_profile(hdl, ESP_PING_PROF_TIMEGAP, &elapsed_ms, sizeof(elapsed_ms));
    ESP_LOGI(TAG, "reply from %s seq=%u time=%" PRIu32 " ms", AP_IP, seqno, elapsed_ms);
}

static void on_ping_timeout(esp_ping_handle_t hdl, void *args)
{
    ESP_LOGW(TAG, "ping timeout");
}

/* mmhalow's STA netif is a DHCP client, but the SoftAP runs no DHCP server, so pin a
 * static IP on the AP's subnet (mirrors rimba-halow-ap's assign_static_ip). */
static bool assign_static_ip(void)
{
    esp_netif_t *n = mmhalow_get_netif();
    if (n == NULL) {
        ESP_LOGE(TAG, "STA netif is NULL");
        return false;
    }
    esp_netif_dhcpc_stop(n);

    esp_netif_ip_info_t ip = { 0 };
    ip.ip.addr      = esp_ip4addr_aton(STA_IP);
    ip.gw.addr      = esp_ip4addr_aton(AP_IP);
    ip.netmask.addr = esp_ip4addr_aton(NETMASK);
    if (esp_netif_set_ip_info(n, &ip) != ESP_OK) {
        ESP_LOGE(TAG, "set_ip_info failed");
        return false;
    }
    esp_netif_action_connected(n, NULL, 0, NULL);
    ESP_LOGI(TAG, "STA static IP %s (gw %s)", STA_IP, AP_IP);
    return true;
}

static void idle_forever(void)
{
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

void app_main(void)
{
    vTaskDelay(pdMS_TO_TICKS(500));   /* let the USB-Serial-JTAG console attach */
    ESP_LOGI(TAG, "joining HaLow AP \"%s\" (SAE), then pinging %s", LINK_SSID, AP_IP);

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

    mmhalow_connect(sta_status_cb);
    int waited = 0;
    while (!s_connected && waited < CONNECT_TIMEOUT_MS) {
        vTaskDelay(pdMS_TO_TICKS(250));
        waited += 250;
    }
    if (!s_connected) {
        ESP_LOGE(TAG, "no association to \"%s\" within %d ms -- is the AP (rimba-halow-ap) up?",
                 LINK_SSID, CONNECT_TIMEOUT_MS);
        idle_forever();
    }

    if (!assign_static_ip()) {
        idle_forever();
    }

    /* Ping the AP continuously so the console shows the live link. */
    ip_addr_t target = { 0 };
    target.type = IPADDR_TYPE_V4;
    target.u_addr.ip4.addr = esp_ip4addr_aton(AP_IP);

    esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
    cfg.target_addr = target;
    cfg.count = ESP_PING_COUNT_INFINITE;
    cfg.interval_ms = PING_INTERVAL_MS;

    esp_ping_callbacks_t cbs = {
        .on_ping_success = on_ping_success,
        .on_ping_timeout = on_ping_timeout,
        .on_ping_end = NULL,
        .cb_args = NULL,
    };
    esp_ping_handle_t ping;
    if (esp_ping_new_session(&cfg, &cbs, &ping) != ESP_OK) {
        ESP_LOGE(TAG, "failed to create the ping session");
        idle_forever();
    }
    ESP_LOGI(TAG, "pinging %s every %d ms...", AP_IP, PING_INTERVAL_MS);
    esp_ping_start(ping);

    idle_forever();
}
