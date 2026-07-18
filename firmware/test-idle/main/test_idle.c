/*
 * test-idle — the radio-silent resting-state fixture (manifest.IDLE_APP).
 *
 * The regression harness flashes this to every ESP board after every hardware
 * test (tools/regtest: common.go_radio_silent), leaving the board radio-free.
 * The whole point is that nothing beacons, associates, peers, or monitors on
 * the air between tests: this app never brings the MM6108/HaLow radio up.
 *
 * Cloned from the rimba-hello bring-up example, trimmed to the fixture's one job.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_chip_info.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "test-idle";

void app_main(void)
{
    /* Let the USB-Serial-JTAG console attach before the first print. */
    vTaskDelay(pdMS_TO_TICKS(200));

    esp_chip_info_t info;
    esp_chip_info(&info);

    ESP_LOGI(TAG, "=============== test-idle: radio-silent ============");
    ESP_LOGI(TAG, "chip %s, %d core(s), idf %s  — no radio brought up",
             CONFIG_IDF_TARGET, info.cores, esp_get_idf_version());
    ESP_LOGI(TAG, "=======================================================");

    uint32_t beat = 0;
    while (true) {
        ESP_LOGI(TAG, "idle %" PRIu32 "  (uptime %" PRId64 " s, heap %u KB)",
                 beat++,
                 esp_timer_get_time() / 1000000,
                 (unsigned)(esp_get_free_heap_size() / 1024));
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
