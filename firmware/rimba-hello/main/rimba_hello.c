/*
 * rimba-hello — Phase 1 bring-up firmware for the Seeed XIAO ESP32-S3 (Plus).
 *
 * Purpose: prove the ESP-IDF toolchain + build + flash + console pipeline on
 * the real hardware, and validate the items Phase 1 can check WITHOUT the
 * MM6108/morselib HaLow port:
 *
 *   - Chip identity / cores / silicon revision
 *   - PSRAM presence and a real allocation test   (RISK-04, dev-plan task 1.8)
 *   - SRAM/heap headroom                            (Phase 1 validation criteria)
 *
 * It deliberately does NOT touch the HaLow radio: the MM6108 morselib HAL has
 * to be ported to ESP-IDF first (see docs/worklog). This is the foundation that
 * everything in the development plan's Phase 1 builds on.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <inttypes.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_psram.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "rimba-hello";

/* Target from RISK-04: confirm the XIAO ESP32-S3 (Plus) 8 MB PSRAM is present
 * and usable, since Rimba's bundle store / routing table depend on it. */
#define PSRAM_TEST_BYTES (1 * 1024 * 1024) /* 1 MiB probe */

static void log_chip_info(void)
{
    esp_chip_info_t info;
    esp_chip_info(&info);

    uint32_t flash_size = 0;
    if (esp_flash_get_size(NULL, &flash_size) != ESP_OK) {
        flash_size = 0;
    }

    unsigned major = info.revision / 100;
    unsigned minor = info.revision % 100;

    ESP_LOGI(TAG, "chip      : %s, %d core(s), rev v%u.%u",
             CONFIG_IDF_TARGET, info.cores, major, minor);
    ESP_LOGI(TAG, "features  : WiFi2.4%s%s",
             (info.features & CHIP_FEATURE_BT) ? "/BT" : "",
             (info.features & CHIP_FEATURE_BLE) ? "/BLE" : "");
    ESP_LOGI(TAG, "flash     : %" PRIu32 " MB", flash_size / (1024 * 1024));
    ESP_LOGI(TAG, "idf       : %s", esp_get_idf_version());
}

/* Returns true if PSRAM is present and a real read/write test over PSRAM_TEST_BYTES passes. */
static bool validate_psram(void)
{
    if (!esp_psram_is_initialized()) {
        ESP_LOGE(TAG, "PSRAM    : NOT initialized — RISK-04 not satisfied on this board/config");
        return false;
    }

    size_t psram_total = esp_psram_get_size();
    size_t spiram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "PSRAM    : initialized, %u MB total, %u KB free heap",
             (unsigned)(psram_total / (1024 * 1024)), (unsigned)(spiram_free / 1024));

    uint8_t *buf = heap_caps_malloc(PSRAM_TEST_BYTES, MALLOC_CAP_SPIRAM);
    if (buf == NULL) {
        ESP_LOGE(TAG, "PSRAM    : failed to allocate %d KB from SPIRAM", PSRAM_TEST_BYTES / 1024);
        return false;
    }

    /* Write a pattern, read it back. Catches mis-wired/mis-configured octal PSRAM. */
    for (size_t i = 0; i < PSRAM_TEST_BYTES; i++) {
        buf[i] = (uint8_t)(i * 31u + 7u);
    }
    bool ok = true;
    for (size_t i = 0; i < PSRAM_TEST_BYTES; i++) {
        if (buf[i] != (uint8_t)(i * 31u + 7u)) {
            ok = false;
            break;
        }
    }
    heap_caps_free(buf);

    ESP_LOGI(TAG, "PSRAM    : %d KB read/write test %s",
             PSRAM_TEST_BYTES / 1024, ok ? "PASSED" : "FAILED");
    return ok;
}

static void log_memory(void)
{
    /* Internal SRAM holds time-critical morselib/FreeRTOS structures (dev plan). */
    size_t int_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t int_min  = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    ESP_LOGI(TAG, "SRAM     : %u KB free, %u KB min-ever  (Phase-1 gate: >50 KB)",
             (unsigned)(int_free / 1024), (unsigned)(int_min / 1024));
}

void app_main(void)
{
    /* Small delay so the USB-Serial-JTAG console is attached before we print. */
    vTaskDelay(pdMS_TO_TICKS(200));

    ESP_LOGI(TAG, "================ Rimba Phase-1 bring-up ================");
    log_chip_info();
    bool psram_ok = validate_psram();
    log_memory();
    ESP_LOGI(TAG, "PSRAM (RISK-04 / task 1.8): %s", psram_ok ? "OK" : "CHECK CONFIG");
    ESP_LOGI(TAG, "=======================================================");

    uint32_t beat = 0;
    while (true) {
        ESP_LOGI(TAG, "heartbeat %" PRIu32 "  (uptime %" PRId64 " s, heap %u KB)",
                 beat++,
                 esp_timer_get_time() / 1000000,
                 (unsigned)(esp_get_free_heap_size() / 1024));
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
