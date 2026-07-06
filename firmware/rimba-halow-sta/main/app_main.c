/*
 * rimba-halow-sta — full power-save LADDER test (netif-free, zero traffic).
 * One run walks every PS mode with grep-able uptime markers so a single aligned PPK2 trace
 * yields the whole ladder:
 *   [connect]  -> NO-PS 20 s -> DYN-PS 20 s -> TWT(10 s SP) 40 s -> WNM(+powerdown) 40 s -> DONE
 * Run it against either AP (ESP32 board0 or Linux chronite) — same SSID/security on both.
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
#include "mmhalow.h"
#include "mmwlan.h"

#define LINK_SSID  "rimba-ping"
#define LINK_PSK   "rimbahalow"
static const char *TAG = "rimba-ladder";
static volatile bool s_connected = false;
static unsigned up_s(void) { return (unsigned)(esp_timer_get_time() / 1000000ULL); }

static void sta_status_cb(enum mmwlan_sta_state st)
{
    if (st == MMWLAN_STA_CONNECTED) { s_connected = true; }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Booting PS ladder test (ssid=\"%s\")...", LINK_SSID);
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_netif_init());

    mmhalow_init(NULL);
    mmhalow_print_version_info();   /* logs the morselib + CHIP FW version — verify rel_1_17_8 */
    mmhalow_wifi_config_t conf = { .sta = MMWLAN_STA_ARGS_INIT };
    memcpy(conf.sta.ssid, LINK_SSID, strlen(LINK_SSID));
    conf.sta.ssid_len = strlen(LINK_SSID);
    memcpy(conf.sta.passphrase, LINK_PSK, strlen(LINK_PSK));
    conf.sta.passphrase_len = strlen(LINK_PSK);
    conf.sta.security_type = MMWLAN_SAE;
    ESP_ERROR_CHECK(mmhalow_set_config(WIFI_IF_STA, &conf));

    ESP_LOGI(TAG, "Connecting...");
    mmhalow_connect(sta_status_cb);
    int waited = 0;
    while (!s_connected && waited < 60000) { vTaskDelay(pdMS_TO_TICKS(500)); waited += 500; }
    if (!s_connected) { ESP_LOGW(TAG, "link-up timeout"); return; }

    /* Phase 1 — NO PS (mmhalow.c:200 has PS disabled; make it explicit). Radio always on. */
    mmwlan_set_power_save_mode(MMWLAN_PS_DISABLED);
    ESP_LOGW(TAG, "=== P1 NOPS start uptime=%us ===", up_s());
    vTaskDelay(pdMS_TO_TICKS(20000));
    ESP_LOGW(TAG, "=== P1 NOPS end uptime=%us ===", up_s());

    /* Phase 2 — dynamic 802.11 PS (DTIM wakes, ~100 ms idle timeout). */
    mmwlan_set_power_save_mode(MMWLAN_PS_ENABLED);
    ESP_LOGW(TAG, "=== P2 DYNPS start uptime=%us ===", up_s());
    vTaskDelay(pdMS_TO_TICKS(20000));
    ESP_LOGW(TAG, "=== P2 DYNPS end uptime=%us ===", up_s());

    /* Phase 3 — TWT, 10 s service period (scheduled doze; PS stays enabled). */
    struct mmwlan_twt_config_args twt = MMWLAN_TWT_CONFIG_ARGS_INIT;
    twt.twt_mode = MMWLAN_TWT_REQUESTER;
    twt.twt_wake_interval_us = 10000000;
    twt.twt_min_wake_duration_us = 65280;
    twt.twt_setup_command = MMWLAN_TWT_SETUP_REQUEST;
    enum mmwlan_status st = mmwlan_twt_setup_request(&twt);
    ESP_LOGW(TAG, "=== P3 TWT start (setup ret=%d) uptime=%us ===", st, up_s());
    vTaskDelay(pdMS_TO_TICKS(40000));
    mmwlan_twt_teardown();
    ESP_LOGW(TAG, "=== P3 TWT end uptime=%us ===", up_s());
    vTaskDelay(pdMS_TO_TICKS(3000));

    /* Phase 4 — WNM sleep with chip powerdown (deepest radio tier). */
    struct mmwlan_set_wnm_sleep_enabled_args wnm = { .wnm_sleep_enabled = true,
                                                     .chip_powerdown_enabled = true };
    st = mmwlan_set_wnm_sleep_enabled_ext(&wnm);
    ESP_LOGW(TAG, "=== P4 WNM start (enter ret=%d) uptime=%us ===", st, up_s());
    vTaskDelay(pdMS_TO_TICKS(40000));
    wnm.wnm_sleep_enabled = false;
    st = mmwlan_set_wnm_sleep_enabled_ext(&wnm);
    ESP_LOGW(TAG, "=== P4 WNM end (exit ret=%d) uptime=%us ===", st, up_s());

    ESP_LOGW(TAG, "=== DONE uptime=%us ===", up_s());
}
