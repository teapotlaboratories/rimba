/*
 * rimba-twt-assoc — retest TWT against the Linux AP via the ASSOC-EMBEDDED path.
 *
 * Earlier tests used mmwlan_twt_setup_request() (a MID-SESSION action frame after associating), which the
 * Linux hostapd_s1g does not answer. This app instead calls mmwlan_twt_add_configuration() BEFORE connecting
 * — morselib then negotiates TWT in the (re)association IEs, which is what hostapd's beacon/assoc-time TWT
 * responder (he_twt_responder, default-on) actually handles.
 *
 * Watch for: the TWT SP-cadence signature in the PPK2 trace (radio wakes ~every wake-interval instead of
 * every DTIM) = TWT engaged. Host-awake first; flip HOST_LIGHT_SLEEP for the rare-wake deep win.
 */
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_pm.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "mmhalow.h"
#include "mmwlan.h"

#define LINK_SSID   "rimba-ping"
#define LINK_PSK    "rimbahalow"
#define GUARD_PIN   GPIO_NUM_6
#define WAKE_INT_US 10000000        /* 10 s TWT service-period interval */
#define HOLD_S      60
#define HOST_LIGHT_SLEEP 0

static const char *TAG = "twt-assoc";
static volatile bool s_connected = false;
static volatile int  s_disassoc  = 0;
static void sta_status_cb(enum mmwlan_sta_state st)
{
    bool now = (st == MMWLAN_STA_CONNECTED);
    if (s_connected && !now) s_disassoc++;
    s_connected = now;
}

void app_main(void)
{
    vTaskDelay(pdMS_TO_TICKS(200));

    gpio_reset_pin(GUARD_PIN);
    gpio_set_direction(GUARD_PIN, GPIO_MODE_INPUT);
    gpio_set_pull_mode(GUARD_PIN, GPIO_PULLDOWN_ONLY);
    vTaskDelay(pdMS_TO_TICKS(50));
    if (gpio_get_level(GUARD_PIN) == 1) {
        ESP_LOGW(TAG, "=== FLASH-HOLD: D5 HIGH at boot — idling for reflash ===");
        uint32_t h = 0;
        while (1) { vTaskDelay(pdMS_TO_TICKS(3000)); ESP_LOGW(TAG, "FLASH-HOLD %us", (unsigned)(3*++h)); }
    }
    ESP_LOGW(TAG, "fresh boot — 6 s flash window");
    for (int i = 0; i < 6; i++) vTaskDelay(pdMS_TO_TICKS(1000));

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

    /* ASSOC-EMBEDDED TWT: add the config BEFORE connecting so it goes in the (re)association IEs
       (the path hostapd's beacon/assoc-time responder handles) — NOT a mid-session action frame. */
    struct mmwlan_twt_config_args twt = MMWLAN_TWT_CONFIG_ARGS_INIT;
    twt.twt_mode = MMWLAN_TWT_REQUESTER;
    twt.twt_wake_interval_us = WAKE_INT_US;
    twt.twt_min_wake_duration_us = 65280;
    twt.twt_setup_command = MMWLAN_TWT_SETUP_REQUEST;
    enum mmwlan_status ts = mmwlan_twt_add_configuration(&twt);
    ESP_LOGW(TAG, "=== mmwlan_twt_add_configuration ret=%d (0=OK) — TWT in assoc IEs ===", ts);

    mmwlan_override_max_tx_power(1);
    uint64_t t0 = esp_timer_get_time();
    mmhalow_connect(sta_status_cb);
    int waited = 0;
    while (!s_connected && waited < 30000) { vTaskDelay(pdMS_TO_TICKS(100)); waited += 100; }
    if (!s_connected) { ESP_LOGW(TAG, "connect timeout"); while (1) vTaskDelay(pdMS_TO_TICKS(5000)); }
    ESP_LOGW(TAG, "=== CONNECTED in %" PRIu32 " ms ===", (uint32_t)((esp_timer_get_time()-t0)/1000));

    mmwlan_set_power_save_mode(MMWLAN_PS_ENABLED);
#if HOST_LIGHT_SLEEP
    esp_pm_config_t pm = { .max_freq_mhz = 160, .min_freq_mhz = 40, .light_sleep_enable = true };
    esp_pm_configure(&pm);
    ESP_LOGW(TAG, "host light sleep enabled");
#endif

    ESP_LOGW(TAG, "=== assoc-embedded TWT (%d s SP) — holding %ds; watch PPK2 for SP-cadence doze ===",
             (int)(WAKE_INT_US/1000000), HOLD_S);
    for (int t = 0; t < HOLD_S; t++) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if ((t % 15) == 14) ESP_LOGW(TAG, "    TWT +%ds: connected=%d disassoc=%d", t+1, (int)s_connected, s_disassoc);
    }
    ESP_LOGW(TAG, "=== TWT hold done: disassoc=%d connected=%d ===", s_disassoc, (int)s_connected);
    while (1) vTaskDelay(pdMS_TO_TICKS(5000));
}
