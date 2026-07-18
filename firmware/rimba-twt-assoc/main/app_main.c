/*
 * rimba-twt-assoc — HaLow station that negotiates TWT power-save at association, then dozes.
 *
 * A low-power station example. It brings up the MM6108 radio, adds a TWT (Target Wake Time)
 * REQUESTER configuration *before* associating so the TWT request rides inside the
 * (re)association IEs — the path a HaLow AP's assoc-time TWT responder (e.g. hostapd's
 * he_twt_responder, default-on) actually answers — then associates over SAE and turns on
 * power-save. Once the agreement is up the radio only wakes for its scheduled service period
 * (~every wake interval) instead of every DTIM beacon, which is the battery win for a mostly
 * idle sensor node.
 *
 * Note: a mid-session mmwlan_twt_setup_request() action frame is NOT answered by every AP, so
 * the assoc-embedded path here is the portable way to get TWT up.
 *
 * Pair it with the rimba-halow-ap example (or any HaLow AP) on another board:
 *
 *   make flash APP=rimba-halow-ap    BOARD=proto1-fgh100m PORT=/dev/ttyACM0   # the AP
 *   make flash APP=rimba-twt-assoc   BOARD=proto1-fgh100m PORT=/dev/ttyACM1   # this TWT STA
 *   make monitor APP=rimba-twt-assoc BOARD=proto1-fgh100m PORT=/dev/ttyACM1
 *
 * Change LINK_SSID / LINK_PSK and the TWT interval below for your own network.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"

#include "mmhalow.h"
#include "mmwlan.h"

static const char *TAG = "rimba-twt-assoc";

/* Match the AP (rimba-halow-ap) — change for your network. */
#define LINK_SSID   "rimba-ping"
#define LINK_PSK    "rimbahalow"

/* TWT agreement requested at association: the station wakes for one service period every wake
 * interval. Widen the interval for deeper idle sleep, shorten it for lower downlink latency. */
#define TWT_WAKE_INTERVAL_US     10000000    /* 10 s between service periods */
#define TWT_MIN_WAKE_DURATION_US 65280       /* awake window per service period */

#define CONNECT_TIMEOUT_MS 30000
#define HEARTBEAT_MS       30000

static volatile bool s_connected = false;

static void sta_status_cb(enum mmwlan_sta_state st)
{
    if (st == MMWLAN_STA_CONNECTED) {
        s_connected = true;
        ESP_LOGI(TAG, "associated to \"%s\"", LINK_SSID);
    }
}

void app_main(void)
{
    vTaskDelay(pdMS_TO_TICKS(500));   /* let the USB-Serial-JTAG console attach */

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

    /* Assoc-embedded TWT: add the requester config BEFORE connecting so morselib carries the
     * TWT request in the (re)association IEs, where the AP's assoc-time responder handles it. */
    struct mmwlan_twt_config_args twt = MMWLAN_TWT_CONFIG_ARGS_INIT;
    twt.twt_mode = MMWLAN_TWT_REQUESTER;
    twt.twt_wake_interval_us = TWT_WAKE_INTERVAL_US;
    twt.twt_min_wake_duration_us = TWT_MIN_WAKE_DURATION_US;
    twt.twt_setup_command = MMWLAN_TWT_SETUP_REQUEST;
    enum mmwlan_status ts = mmwlan_twt_add_configuration(&twt);
    ESP_LOGI(TAG, "TWT requester queued for the assoc request (ret=%d, 0=OK)", ts);

    mmhalow_connect(sta_status_cb);
    int waited = 0;
    while (!s_connected && waited < CONNECT_TIMEOUT_MS) {
        vTaskDelay(pdMS_TO_TICKS(100));
        waited += 100;
    }
    if (!s_connected) {
        ESP_LOGE(TAG, "no association to \"%s\" within %d ms -- is the AP up?",
                 LINK_SSID, CONNECT_TIMEOUT_MS);
        while (1) vTaskDelay(pdMS_TO_TICKS(5000));
    }

    /* Enable radio power-save. With the TWT agreement up the radio dozes between service
     * periods instead of waking every DTIM beacon. */
    mmwlan_set_power_save_mode(MMWLAN_PS_ENABLED);
    ESP_LOGI(TAG, "power-save on — dozing on the TWT schedule (%d s service period)",
             (int)(TWT_WAKE_INTERVAL_US / 1000000));

    /* Stay associated and dozing; a periodic heartbeat shows the link is still up. */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(HEARTBEAT_MS));
        ESP_LOGI(TAG, "still associated, dozing on TWT");
    }
}
