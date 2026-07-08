/*
 * rimba-halow-sta — TRIGGERED power-save ladder (C6-harness-ready, netif-free, zero traffic).
 *
 * Boots into a host-AWAKE idle and connects to the AP, then WAITS for a trigger on the one free
 * pin (GPIO6 / XIAO pad D5). A trigger runs ONE fixed-schedule ladder, marking each phase entry as
 * a pulse burst on that same pin (board2 drives it -> the C6 timestamps the edges), then returns to
 * idle. Because it never auto-runs into sleep, a fresh boot is ALWAYS host-awake -> USB stays
 * enumerated -> board2 is always reflashable. This is the fix for the deep-sleep-cycling wedge.
 *
 *   trigger : the C6 (or a jumper) pulls D5 LOW, then releases it HIGH
 *   phases  : 1 = No-PS   2 = Dyn-PS   3 = TWT(10 s SP)   4 = WNM + chip-powerdown   (all host-awake)
 *   marker  : phase N = N x 60 ms pulses on D5 at phase entry, then the tier runs 18 s
 *
 * Alignment: C6 trigger edge = t0; fixed 18 s phases; the burst count self-identifies each phase in
 * the PPK2 trace even if the USB console drops. Console markers are also logged (host is awake here).
 */
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "mmhalow.h"
#include "mmwlan.h"

#define LINK_SSID  "rimba-ping"
#define LINK_PSK   "rimbahalow"
#define PHASE_PIN  GPIO_NUM_6          /* XIAO pad D5 — the single free pin shared with the C6 */
#define PHASE_S    18                  /* seconds per tier */

static const char *TAG = "rimba-ladder";
static volatile bool s_connected = false;
static unsigned up_s(void) { return (unsigned)(esp_timer_get_time() / 1000000ULL); }

static void sta_status_cb(enum mmwlan_sta_state st)
{
    if (st == MMWLAN_STA_CONNECTED) { s_connected = true; }
}

/* board2 DRIVES D5: emit n 60 ms pulses to mark entry into phase n (captured on the C6 / PPK2). */
static void phase_mark(int n)
{
    gpio_set_direction(PHASE_PIN, GPIO_MODE_OUTPUT);
    for (int i = 0; i < n; i++) {
        gpio_set_level(PHASE_PIN, 1); vTaskDelay(pdMS_TO_TICKS(60));
        gpio_set_level(PHASE_PIN, 0); vTaskDelay(pdMS_TO_TICKS(60));
    }
    vTaskDelay(pdMS_TO_TICKS(150));    /* idle gap so the burst is clearly delimited */
}

/* Host-AWAKE idle. D5 = pulled-up input; the C6 (or a jumper to GND) pulls it LOW to trigger.
 * Waits for LOW (assert) THEN HIGH again (release) so the C6 is off the line before we drive it —
 * no bus contention, no series resistor needed. USB stays up the whole time => reflashable. */
static void wait_for_trigger(void)
{
    gpio_reset_pin(PHASE_PIN);
    gpio_set_direction(PHASE_PIN, GPIO_MODE_INPUT);
    gpio_set_pull_mode(PHASE_PIN, GPIO_PULLUP_ONLY);
    ESP_LOGW(TAG, "=== IDLE (host awake) — waiting for trigger on D5/GPIO%d ===", PHASE_PIN);
    while (gpio_get_level(PHASE_PIN) == 1) { vTaskDelay(pdMS_TO_TICKS(50)); }  /* assert (low)  */
    while (gpio_get_level(PHASE_PIN) == 0) { vTaskDelay(pdMS_TO_TICKS(10)); }  /* release (high) */
    vTaskDelay(pdMS_TO_TICKS(20));
    ESP_LOGW(TAG, "=== TRIGGERED uptime=%us ===", up_s());
}

/* One fixed-schedule, all-host-AWAKE ladder; entry of each tier marked on D5 + console. */
static void run_ladder(void)
{
    phase_mark(1);
    mmwlan_set_power_save_mode(MMWLAN_PS_DISABLED);
    ESP_LOGW(TAG, "=== P1 NO-PS start uptime=%us ===", up_s());
    vTaskDelay(pdMS_TO_TICKS(PHASE_S * 1000));

    phase_mark(2);
    mmwlan_set_power_save_mode(MMWLAN_PS_ENABLED);
    ESP_LOGW(TAG, "=== P2 DYN-PS start uptime=%us ===", up_s());
    vTaskDelay(pdMS_TO_TICKS(PHASE_S * 1000));

    phase_mark(3);
    struct mmwlan_twt_config_args twt = MMWLAN_TWT_CONFIG_ARGS_INIT;
    twt.twt_mode = MMWLAN_TWT_REQUESTER;
    twt.twt_wake_interval_us = 10000000;
    twt.twt_min_wake_duration_us = 65280;
    twt.twt_setup_command = MMWLAN_TWT_SETUP_REQUEST;
    enum mmwlan_status st = mmwlan_twt_setup_request(&twt);
    ESP_LOGW(TAG, "=== P3 TWT start (ret=%d) uptime=%us ===", st, up_s());
    vTaskDelay(pdMS_TO_TICKS(PHASE_S * 1000));
    mmwlan_twt_teardown();

    phase_mark(4);
    struct mmwlan_set_wnm_sleep_enabled_args wnm = { .wnm_sleep_enabled = true,
                                                     .chip_powerdown_enabled = true };
    st = mmwlan_set_wnm_sleep_enabled_ext(&wnm);
    ESP_LOGW(TAG, "=== P4 WNM+powerdown start (ret=%d) uptime=%us ===", st, up_s());
    vTaskDelay(pdMS_TO_TICKS(PHASE_S * 1000));
    wnm.wnm_sleep_enabled = false;
    mmwlan_set_wnm_sleep_enabled_ext(&wnm);

    phase_mark(5);   /* end-of-run marker */
    ESP_LOGW(TAG, "=== LADDER DONE uptime=%us — returning to idle ===", up_s());
}

void app_main(void)
{
    /* Let the USB-Serial-JTAG console attach before we log. */
    vTaskDelay(pdMS_TO_TICKS(200));

    /* === SPECIAL: flash-hold guard (checked FIRST, before any radio/NVS init) ===
     * If D5/GPIO6 is HIGH at boot, do NOT run the app — just idle host-AWAKE forever so the host can always
     * reflash board2. This is the wedge escape hatch: to recover a board stuck in a bad fw, hold D5 HIGH
     * (drive it from the C6, or jumper D5 -> 3V3), power-cycle, and it boots straight into this flashable
     * idle instead of the app. Default is a pull-DOWN, so D5 LOW / floating = normal run; HIGH = the special
     * hold state. Pure GPIO — no radio touched. */
    gpio_reset_pin(PHASE_PIN);
    gpio_set_direction(PHASE_PIN, GPIO_MODE_INPUT);
    gpio_set_pull_mode(PHASE_PIN, GPIO_PULLDOWN_ONLY);
    vTaskDelay(pdMS_TO_TICKS(50));               /* let the level settle */
    if (gpio_get_level(PHASE_PIN) == 1) {
        ESP_LOGW(TAG, "=== FLASH-HOLD: D5/GPIO%d HIGH at boot — idling host-awake for reflash ===", PHASE_PIN);
        uint32_t held = 0;
        while (1) {
            vTaskDelay(pdMS_TO_TICKS(3000));
            ESP_LOGW(TAG, "FLASH-HOLD idling %us — safe to reflash (drive D5 low + reset to run)",
                     (unsigned)(3 * ++held));
        }
    }
    ESP_LOGI(TAG, "D5/GPIO%d low at boot -> normal run.", PHASE_PIN);

    ESP_LOGI(TAG, "Booting TRIGGERED PS ladder (ssid=\"%s\")...", LINK_SSID);
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_netif_init());

    mmhalow_init(NULL);
    mmhalow_print_version_info();   /* logs morselib + CHIP FW version — verify rel_1_17_9 */
    mmhalow_wifi_config_t conf = { .sta = MMWLAN_STA_ARGS_INIT };
    memcpy(conf.sta.ssid, LINK_SSID, strlen(LINK_SSID));
    conf.sta.ssid_len = strlen(LINK_SSID);
    memcpy(conf.sta.passphrase, LINK_PSK, strlen(LINK_PSK));
    conf.sta.passphrase_len = strlen(LINK_PSK);
    conf.sta.security_type = MMWLAN_SAE;
    ESP_ERROR_CHECK(mmhalow_set_config(WIFI_IF_STA, &conf));

    /* Close-bench RX-overload workaround: cap TX so the AP's receiver isn't saturated (applies on the
       channel switch during connect). Earlier this dropped chronite's RX to a healthy ~-28 dBm and let
       board2 hold a stable ladder. Remove for a real deployment. */
    mmwlan_override_max_tx_power(1);
    ESP_LOGI(TAG, "Connecting (TX capped to 1 dBm for the close bench)...");
    mmhalow_connect(sta_status_cb);
    int waited = 0;
    while (!s_connected && waited < 60000) { vTaskDelay(pdMS_TO_TICKS(500)); waited += 500; }
    if (!s_connected) { ESP_LOGW(TAG, "link-up timeout (idle anyway; retry on next trigger)"); }

    /* Host-AWAKE forever: idle -> triggered run -> idle. Never self-sleeps -> never wedges its USB. */
    while (1) {
        wait_for_trigger();
        run_ladder();
    }
}
