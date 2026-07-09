/*
 * rimba-sleep-test — measure board2's lowest achievable power.
 *
 * Sequence at boot:
 *   1. FLASH-HOLD GUARD (first, always): read D5/GPIO6 (pull-down). If HIGH -> sit in a host-awake idle so
 *      the host can reflash. This is ALSO the deep-sleep wake landing: driving D5 high wakes board2 and
 *      lands right here. D5 LOW / default -> proceed to the sleep test.
 *   2. Power off the HaLow radio: hold the MM6108 in reset via RESET_N (GPIO1) LOW, held through sleep.
 *   3. Deep sleep, waking on D5/GPIO6 HIGH (RTC ext1). D5 is pulled down during sleep so only an actively
 *      driven HIGH (C6 in HOLD_HIGH mode) wakes it -> cold boot -> guard -> flashable.
 *
 * So board2 can sit at its hardware floor indefinitely and still be recovered/reflashed remotely by driving
 * D5 high (no BOOT button, no cable). Measure the PPK2 current for the deep-sleep floor.
 */
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"

static const char *TAG = "sleep-test";
#define D5        GPIO_NUM_6      /* flash-hold guard + deep-sleep wake (wired to C6 GPIO20) */
#define MM_RESET  GPIO_NUM_1      /* MM6108 RESET_N — drive LOW to hold the radio off */

/* 0 = A: radio OFF + host DEEP SLEEP  -> the ~0.35 mA floor (default).
 * 1 = C: radio OFF (RESET_N low) + host AWAKE (WFI idle, no esp_pm) -> the "radio-off, host-awake"
 *        baseline used to DIFFERENCE the associated PS tiers and isolate the radio's own draw. */
#define AWAKE_HOLD 0

void app_main(void)
{
    vTaskDelay(pdMS_TO_TICKS(200));   /* console attach */

    /* Release any RTC-GPIO holds carried over from a previous deep-sleep cycle. */
    rtc_gpio_hold_dis(MM_RESET);
    rtc_gpio_deinit(MM_RESET);
    rtc_gpio_deinit(D5);

    /* === 1. FLASH-HOLD GUARD (and deep-sleep-wake landing) === */
    gpio_reset_pin(D5);
    gpio_set_direction(D5, GPIO_MODE_INPUT);
    gpio_set_pull_mode(D5, GPIO_PULLDOWN_ONLY);
    vTaskDelay(pdMS_TO_TICKS(50));
    if (gpio_get_level(D5) == 1) {
        ESP_LOGW(TAG, "=== FLASH-HOLD: D5/GPIO%d HIGH at boot — idling host-awake for reflash ===", D5);
        uint32_t h = 0;
        while (1) {
            vTaskDelay(pdMS_TO_TICKS(3000));
            ESP_LOGW(TAG, "FLASH-HOLD idling %us — reflash now (drive D5 low + reset to sleep-test)",
                     (unsigned)(3 * ++h));
        }
    }

    ESP_LOGW(TAG, "sleep-test (reset reason=%d): 10s flash window, then radio OFF + deep sleep. Drive D5 HIGH to wake.",
             esp_reset_reason());
    /* 10 s host-awake window so a fresh-boot-window reflash can always catch board2 before it sleeps
     * (test-rig convenience; the floor is measured AFTER this window — remove for a real deployment). */
    for (int i = 0; i < 10; i++) { vTaskDelay(pdMS_TO_TICKS(1000)); }

#if AWAKE_HOLD
    /* === C: radio OFF (RESET_N low) + host AWAKE — the baseline for isolating the radio ===
     * Regular GPIO (no rtc hold needed since we never sleep); host stays awake in a WFI-idle loop, the
     * SAME idle state as the associated PS ladder, so (ladder tier) - (this) = the radio's own draw. */
    gpio_reset_pin(MM_RESET);
    gpio_set_direction(MM_RESET, GPIO_MODE_OUTPUT);
    gpio_set_level(MM_RESET, 0);
    ESP_LOGW(TAG, "=== AWAKE-HOLD (C): radio OFF (RESET_N low) + host AWAKE — measuring the host-awake floor ===");
    uint32_t sec = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        ESP_LOGW(TAG, "awake-hold %us — radio off, host awake (read the PPK2 plateau)", (unsigned)(5 * ++sec));
    }
#endif

    /* === 2. Power off the radio: hold MM6108 RESET_N (GPIO1) LOW through deep sleep === */
    rtc_gpio_init(MM_RESET);
    rtc_gpio_set_direction(MM_RESET, RTC_GPIO_MODE_OUTPUT_ONLY);
    rtc_gpio_set_level(MM_RESET, 0);
    rtc_gpio_hold_en(MM_RESET);

    /* === 3. Deep sleep, wake on D5 HIGH; pull D5 down during sleep so only a driven HIGH wakes it === */
    rtc_gpio_init(D5);
    rtc_gpio_set_direction(D5, RTC_GPIO_MODE_INPUT_ONLY);
    rtc_gpio_pulldown_en(D5);
    rtc_gpio_pullup_dis(D5);
    esp_sleep_enable_ext1_wakeup(1ULL << D5, ESP_EXT1_WAKEUP_ANY_HIGH);

    vTaskDelay(pdMS_TO_TICKS(200));   /* flush the log */
    /* NB: no need to disable the USB-Serial-JTAG here — in deep sleep the digital-domain power-down already
     * turns it off (board2's port disappears), and measurement showed disabling the pad changes nothing
     * (floor stays ~0.6 mA — that residual is board regulator/module quiescent, not the USJ). */
    esp_deep_sleep_start();           /* never returns; wakes -> cold boot -> guard */
}
