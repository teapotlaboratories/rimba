/*
 * rimba-deepsleep-cycle — deep-sleep as a usable PS leaf, measured against the Linux AP.
 *
 * Cycle:  connect (SAE) -> hold linked briefly -> DEEP SLEEP (MM6108 held in RESET_N-low, ESP32-S3 deep
 *         sleep ~0.35 mA) -> wake on the C6 trigger (D5 pulled LOW) or a 60 s backup timer -> cold boot ->
 *         RE-ASSOCIATE and log the reconnect latency -> sleep again.
 *
 * D5 / GPIO6 (single wire to C6 GPIO20):
 *   - FRESH power-on: flash-hold guard (pull-DOWN; HIGH => idle forever for reflash), then an 8 s host-awake
 *     flash window so rf_run can always catch it, then connect+sleep.
 *   - DEEP-SLEEP WAKE: wake cause = EXT1/timer => skip the guard, reconnect immediately (this is the leaf's
 *     wake path). During sleep D5 is pulled UP, so the C6's periodic LOW pulse (TRIGGER mode) wakes it.
 *
 * The reconnect latency (cold boot -> associated) is the deep-sleep "tax"; the PPK2 trace shows ~0.35 mA
 * between wakes with a brief boot+assoc spike per cycle.
 */
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "mmhalow.h"
#include "mmwlan.h"

#define LINK_SSID   "rimba-ping"
#define LINK_PSK    "rimbahalow"
#define D5          GPIO_NUM_6      /* single free pin, wired to C6 GPIO20 (trigger + guard + wake) */
#define MM_RESET    GPIO_NUM_1      /* MM6108 RESET_N — drive LOW to power the radio off through sleep */
#define SLEEP_BACKUP_S 60           /* backup timer wake if the C6 trigger is missed */

static const char *TAG = "ds-cycle";
static volatile bool s_connected = false;
static void sta_status_cb(enum mmwlan_sta_state st) { if (st == MMWLAN_STA_CONNECTED) s_connected = true; }

static void enter_deep_sleep(void)
{
    ESP_LOGW(TAG, "=== DEEP SLEEP (radio RESET_N low); wake on C6 trigger (D5 low) or %ds ===", SLEEP_BACKUP_S);
    vTaskDelay(pdMS_TO_TICKS(150));   /* flush log */

    /* Power the radio off: hold MM6108 RESET_N (GPIO1) LOW through deep sleep. */
    rtc_gpio_init(MM_RESET);
    rtc_gpio_set_direction(MM_RESET, RTC_GPIO_MODE_OUTPUT_ONLY);
    rtc_gpio_set_level(MM_RESET, 0);
    rtc_gpio_hold_en(MM_RESET);

    /* Wake on D5 going LOW (the C6 TRIGGER pulse) — pull D5 UP during sleep so a Hi-Z C6 keeps us asleep. */
    rtc_gpio_init(D5);
    rtc_gpio_set_direction(D5, RTC_GPIO_MODE_INPUT_ONLY);
    rtc_gpio_pullup_en(D5);
    rtc_gpio_pulldown_dis(D5);
    esp_sleep_enable_ext1_wakeup(1ULL << D5, ESP_EXT1_WAKEUP_ALL_LOW);
    esp_sleep_enable_timer_wakeup((uint64_t)SLEEP_BACKUP_S * 1000000ULL);

    esp_deep_sleep_start();   /* never returns; wakes -> cold boot -> app_main */
}

void app_main(void)
{
    vTaskDelay(pdMS_TO_TICKS(200));   /* console attach */

    /* Release any RTC-GPIO holds carried over from the previous deep-sleep cycle. */
    rtc_gpio_hold_dis(MM_RESET);
    rtc_gpio_deinit(MM_RESET);
    rtc_gpio_deinit(D5);

    esp_sleep_wakeup_cause_t wc = esp_sleep_get_wakeup_cause();
    bool from_deep = (wc == ESP_SLEEP_WAKEUP_EXT1 || wc == ESP_SLEEP_WAKEUP_TIMER);

    if (!from_deep) {
        /* FRESH power-on: flash-hold guard + a flash window (never reached via a deep-sleep wake). */
        gpio_reset_pin(D5);
        gpio_set_direction(D5, GPIO_MODE_INPUT);
        gpio_set_pull_mode(D5, GPIO_PULLDOWN_ONLY);
        vTaskDelay(pdMS_TO_TICKS(50));
        if (gpio_get_level(D5) == 1) {
            ESP_LOGW(TAG, "=== FLASH-HOLD: D5 HIGH at boot — idling host-awake for reflash ===");
            uint32_t h = 0;
            while (1) { vTaskDelay(pdMS_TO_TICKS(3000)); ESP_LOGW(TAG, "FLASH-HOLD %us", (unsigned)(3*++h)); }
        }
        ESP_LOGW(TAG, "fresh boot — 8 s flash window, then connect + deep-sleep cycle");
        for (int i = 0; i < 8; i++) vTaskDelay(pdMS_TO_TICKS(1000));
    } else {
        ESP_LOGW(TAG, "=== WOKE from deep sleep (cause=%d %s) — reconnecting ===",
                 wc, wc == ESP_SLEEP_WAKEUP_EXT1 ? "C6-trigger" : "backup-timer");
    }

    /* Reconnect timer starts here (cold boot -> associated is the deep-sleep tax). */
    uint64_t t0 = esp_timer_get_time();

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

    mmwlan_override_max_tx_power(1);   /* close-bench TX cap (matches the ladder) */
    s_connected = false;
    mmhalow_connect(sta_status_cb);
    int waited = 0;
    while (!s_connected && waited < 30000) { vTaskDelay(pdMS_TO_TICKS(100)); waited += 100; }

    if (s_connected) {
        uint32_t ms = (uint32_t)((esp_timer_get_time() - t0) / 1000ULL);
        ESP_LOGW(TAG, "=== RECONNECTED in %" PRIu32 " ms (wake_cause=%d) — associated to the AP ===", ms, wc);
    } else {
        ESP_LOGW(TAG, "=== reconnect TIMEOUT (30 s) wake_cause=%d — sleeping, retry next wake ===", wc);
    }

    /* Hold the link up a few seconds so the AP sees the association (and any 1 Hz downlink lands). */
    vTaskDelay(pdMS_TO_TICKS(4000));

    enter_deep_sleep();
}
