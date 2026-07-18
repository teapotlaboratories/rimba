/*
 * rimba-deepsleep-cycle — a timer-woken deep-sleep battery leaf on 802.11ah HaLow.
 *
 * Cycle:  connect (SAE) -> hold linked briefly -> DEEP SLEEP (MM6108 held in RESET_N-low, so the
 *         radio is fully powered off; ESP32-S3 deep sleep ~0.35 mA) -> wake after SLEEP_S on the
 *         RTC timer -> cold boot -> RE-ASSOCIATE and log the reconnect latency -> sleep again.
 *
 * This models a low-duty-cycle sensor leaf: it wakes on a fixed interval, associates just long
 * enough to move a little traffic, then powers the radio down and sleeps. The reconnect latency
 * (cold boot -> associated) is the deep-sleep "tax"; a PPK2 trace shows ~0.35 mA between wakes
 * with a brief boot+assoc spike per cycle.
 *
 * Change LINK_SSID / LINK_PSK for your own network, and SLEEP_S for your duty cycle.
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

#define LINK_SSID   "rimba-ping"     /* change for your network */
#define LINK_PSK    "rimbahalow"     /* change for your network */
#define MM_RESET    GPIO_NUM_1       /* MM6108 RESET_N — drive LOW to power the radio off through sleep */
#define SLEEP_S     30               /* deep-sleep interval between wakes (seconds) */

static const char *TAG = "ds-cycle";
static volatile bool s_connected = false;
static void sta_status_cb(enum mmwlan_sta_state st) { if (st == MMWLAN_STA_CONNECTED) s_connected = true; }

static void enter_deep_sleep(void)
{
    ESP_LOGW(TAG, "=== DEEP SLEEP (radio RESET_N low); timer wake in %ds ===", SLEEP_S);
    vTaskDelay(pdMS_TO_TICKS(150));   /* flush log */

    /* Power the radio off: hold MM6108 RESET_N (GPIO1) LOW through deep sleep. */
    rtc_gpio_init(MM_RESET);
    rtc_gpio_set_direction(MM_RESET, RTC_GPIO_MODE_OUTPUT_ONLY);
    rtc_gpio_set_level(MM_RESET, 0);
    rtc_gpio_hold_en(MM_RESET);

    esp_sleep_enable_timer_wakeup((uint64_t)SLEEP_S * 1000000ULL);

    esp_deep_sleep_start();   /* never returns; wakes -> cold boot -> app_main */
}

void app_main(void)
{
    vTaskDelay(pdMS_TO_TICKS(200));   /* console attach */

    /* Release the RTC-GPIO hold on MM6108 RESET_N carried over from the previous deep-sleep cycle. */
    rtc_gpio_hold_dis(MM_RESET);
    rtc_gpio_deinit(MM_RESET);

    esp_sleep_wakeup_cause_t wc = esp_sleep_get_wakeup_cause();
    bool from_timer = (wc == ESP_SLEEP_WAKEUP_TIMER);
    ESP_LOGW(TAG, "=== %s — connecting ===", from_timer ? "WOKE from deep sleep" : "fresh boot");

    /* Reconnect timer starts here (cold boot -> associated is the deep-sleep tax). */
    uint64_t t0 = esp_timer_get_time();

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

    s_connected = false;
    mmhalow_connect(sta_status_cb);
    int waited = 0;
    while (!s_connected && waited < 30000) { vTaskDelay(pdMS_TO_TICKS(100)); waited += 100; }

    if (s_connected) {
        uint32_t ms = (uint32_t)((esp_timer_get_time() - t0) / 1000ULL);
        ESP_LOGW(TAG, "=== RECONNECTED in %" PRIu32 " ms — associated to the AP ===", ms);
    } else {
        ESP_LOGW(TAG, "=== reconnect TIMEOUT (30 s) — sleeping, retry next wake ===");
    }

    /* Hold the link up a few seconds so the AP sees the association (and any 1 Hz downlink lands). */
    vTaskDelay(pdMS_TO_TICKS(4000));

    enter_deep_sleep();
}
