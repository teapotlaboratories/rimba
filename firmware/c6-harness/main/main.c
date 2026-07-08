/*
 * c6-harness — ESP32-C6 companion for board2's PS-measurement rig.
 *
 * Drives board2's D5 (XIAO ESP32-S3 GPIO6) over a single wire from C6 GPIO20 (+ common GND). board2 is
 * powered and measured by the PPK2, NOT the C6. See docs/reference/rimba-bench-devices.md — "Measurement
 * harness" and "board2 won't flash — un-wedge it".
 *
 * board2's rimba-halow-sta fw reads D5:
 *   - at boot (pull-DOWN):  HIGH => flash-hold (flashable idle);  LOW/default => run
 *   - while idle (pull-UP): a LOW pulse => trigger one ladder run
 *
 * MODE selects the behaviour (set it below):
 *   TRIGGER   - Hi-Z, then pulse LOW ~120 ms every TRIG_PERIOD_S  => fire one ladder run per period
 *   HOLD_HIGH - drive HIGH steady   => board2 boots into flash-hold (remote reflash / wedge recovery)
 *   HOLD_LOW  - drive LOW steady     => board2 boots into run
 *   TOGGLE    - 1 Hz square wave     => wiring / link test (board2's D5 monitor tracks it)
 *
 * Build (standalone IDF — NOT the repo `make`, which is ESP32-S3 only):
 *   export IDF_PATH=<repo>/vendor/esp-idf && source $IDF_PATH/export.sh
 *   idf.py set-target esp32c6 build && idf.py -p /dev/ttyUSB0 flash monitor
 */
#include <stdio.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"

#define MODE_TRIGGER   0
#define MODE_HOLD_HIGH 1
#define MODE_HOLD_LOW  2
#define MODE_TOGGLE    3
#define MODE           MODE_TRIGGER      /* <-- select the behaviour */

#define PIN            GPIO_NUM_20       /* wired to board2 D5 / GPIO6 */
#define TRIG_PERIOD_S  30

static const char *TAG = "c6-harness";
static void hiz(void)      { gpio_set_direction(PIN, GPIO_MODE_INPUT); }
static void drive(int lvl) { gpio_set_direction(PIN, GPIO_MODE_OUTPUT); gpio_set_level(PIN, lvl); }

void app_main(void)
{
    gpio_reset_pin(PIN);
#if MODE == MODE_TRIGGER
    hiz();   /* Hi-Z: board2 boots via its pull-down (=run) and idles via its pull-up (=high) */
    ESP_LOGW(TAG, "TRIGGER: GPIO%d Hi-Z; pulse LOW ~120ms every %ds", PIN, TRIG_PERIOD_S);
    uint32_t n = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(TRIG_PERIOD_S * 1000));
        drive(0);
        ESP_LOGW(TAG, "[%" PRIu32 "] TRIGGER pulse LOW", n++);
        vTaskDelay(pdMS_TO_TICKS(120));
        hiz();                            /* release -> board2 pull-up brings D5 back HIGH */
    }
#elif MODE == MODE_HOLD_HIGH
    drive(1);
    ESP_LOGW(TAG, "HOLD_HIGH: GPIO%d HIGH -> board2 boots into FLASH-HOLD (power-cycle board2)", PIN);
    while (1) vTaskDelay(pdMS_TO_TICKS(5000));
#elif MODE == MODE_HOLD_LOW
    drive(0);
    ESP_LOGW(TAG, "HOLD_LOW: GPIO%d LOW -> board2 boots into RUN", PIN);
    while (1) vTaskDelay(pdMS_TO_TICKS(5000));
#else /* MODE_TOGGLE */
    drive(1);
    ESP_LOGW(TAG, "TOGGLE: GPIO%d 1 Hz square wave (link test)", PIN);
    int lvl = 1;
    while (1) { lvl ^= 1; gpio_set_level(PIN, lvl); vTaskDelay(pdMS_TO_TICKS(1000)); }
#endif
}
