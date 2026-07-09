/*
 * rimba-doze-hold — "deep-sleep without powering off the radio": keep the STA ASSOCIATED while the radio
 * dozes (TWT, then WNM+powerdown), so there is NO re-association tax — the opposite tradeoff to
 * rimba-deepsleep-cycle (which powers the radio off, hits 0.37 mA, and pays ~5 s to re-associate on wake).
 *
 * Constraint that makes this real: to stay associated the ESP32-S3 HOST must only LIGHT-sleep (deep sleep
 * loses morselib's association state), so the floor is the light-sleep floor (~4 mA), not 0.37 mA. This app
 * runs host-AWAKE (already ~5 mA WNM at 1.17.8); flip HOST_LIGHT_SLEEP for the extra ~1 mA.
 *
 * Sequence: connect -> TWT doze hold (HOLD_S) -> WNM+powerdown doze hold (HOLD_S) -> report. The
 * sta_status_cb counts any DISASSOC during the holds; the AP side (chronite station dump) should show board2
 * present the whole time. On the Linux AP TWT does not engage (STA stays at dyn-PS ~10 mA but still
 * ASSOCIATED); WNM+powerdown deeply dozes the radio at ~5 mA and stays associated.
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
#define GUARD_PIN   GPIO_NUM_6      /* D5 flash-hold guard (pull-down; HIGH at boot => flashable idle) */
#define HOLD_S      45              /* seconds to hold each doze mode */
#define HOST_LIGHT_SLEEP 0         /* 0 = host awake (~5 mA WNM); 1 = + host light sleep (~4 mA) */

static const char *TAG = "doze-hold";
static volatile bool s_connected = false;
static volatile int  s_disassoc  = 0;   /* count CONNECTED -> not-CONNECTED transitions during a hold */

static void sta_status_cb(enum mmwlan_sta_state st)
{
    bool now = (st == MMWLAN_STA_CONNECTED);
    if (s_connected && !now) s_disassoc++;   /* dropped the link */
    s_connected = now;
}

/* Hold one doze mode for HOLD_S seconds, watching the association stays up. */
static void doze_hold(const char *what)
{
    int base = s_disassoc;
    ESP_LOGW(TAG, "=== %s doze — holding %ds (should stay ASSOCIATED, no re-assoc) ===", what, HOLD_S);
    for (int t = 0; t < HOLD_S; t++) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if ((t % 15) == 14)
            ESP_LOGW(TAG, "    %s +%ds: connected=%d disassoc_events=%d", what, t + 1,
                     (int)s_connected, s_disassoc - base);
    }
    ESP_LOGW(TAG, "=== %s doze done: %d disassoc event(s), connected=%d ===",
             what, s_disassoc - base, (int)s_connected);
}

void app_main(void)
{
    vTaskDelay(pdMS_TO_TICKS(200));

    /* flash-hold guard (D5 HIGH at boot => idle host-awake forever for reflash) */
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
    mmhalow_print_version_info();   /* verify chip fw rel_1_17_8 */
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
    while (!s_connected && waited < 30000) { vTaskDelay(pdMS_TO_TICKS(100)); waited += 100; }
    if (!s_connected) { ESP_LOGW(TAG, "connect timeout — nothing to hold"); }
    else ESP_LOGW(TAG, "=== CONNECTED — beginning radio-doze holds (associated the whole time) ===");

    /* REQUIRED: mmhalow_init() force-disables PS, so the radio stays fully on (No-PS ~64 mA) even with a TWT
     * request. Enable PS so TWT/dyn-PS actually doze the radio. (WNM+chip_powerdown powers the chip down
     * regardless, but TWT dozing needs this.) */
    mmwlan_set_power_save_mode(MMWLAN_PS_ENABLED);

#if HOST_LIGHT_SLEEP
    esp_pm_config_t pm = { .max_freq_mhz = 160, .min_freq_mhz = 40, .light_sleep_enable = true };
    esp_pm_configure(&pm);
    ESP_LOGW(TAG, "host light sleep enabled");
#endif

    /* 1) TWT doze — on the Linux AP this does NOT install a schedule (stays dyn-PS) but remains ASSOCIATED. */
    struct mmwlan_twt_config_args twt = MMWLAN_TWT_CONFIG_ARGS_INIT;
    twt.twt_mode = MMWLAN_TWT_REQUESTER;
    twt.twt_wake_interval_us = 10000000;
    twt.twt_min_wake_duration_us = 65280;
    twt.twt_setup_command = MMWLAN_TWT_SETUP_REQUEST;
    mmwlan_twt_setup_request(&twt);
    doze_hold("TWT");
    mmwlan_twt_teardown();

    /* 2) WNM+powerdown doze — deeply dozes the radio (chip powerdown) while STAYING ASSOCIATED. */
    struct mmwlan_set_wnm_sleep_enabled_args wnm = { .wnm_sleep_enabled = true, .chip_powerdown_enabled = true };
    mmwlan_set_wnm_sleep_enabled_ext(&wnm);
    doze_hold("WNM+powerdown");
    wnm.wnm_sleep_enabled = false;
    mmwlan_set_wnm_sleep_enabled_ext(&wnm);

    ESP_LOGW(TAG, "=== DONE — total disassoc events across both holds: %d (0 = never re-associated) ===",
             s_disassoc);
    /* idle host-awake so board2 stays enumerated + reflashable */
    while (1) vTaskDelay(pdMS_TO_TICKS(5000));
}
