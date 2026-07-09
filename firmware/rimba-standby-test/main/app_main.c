/*
 * rimba-standby-test — feasibility test of MMWLAN_STANDBY (deprecated): the Morse chip keeps the
 * association ALIVE (DTIM PS + keep-alives, offloaded) while the ESP32 host sleeps, so the host doesn't
 * service per-DTIM wakes. This is the morselib mechanism for "deep-sleep without losing the link" (Q2).
 *
 * This first pass keeps it honest + bounded: connect -> mmwlan_standby_enter() -> host LIGHT sleep (keeps
 * RAM so no cold-boot-resume problem) -> hold, watching the link -> mmwlan_standby_exit(). It answers:
 *   (1) does standby_enter even succeed on the ESP/FGH100M build?  (2) does the STA stay associated?
 *   (3) what's the power with the chip autonomously holding the link + the host light-sleeping?
 * The full win (host DEEP sleep, chip wakes it via GPIO) needs a resume path that skips mmhalow_init's chip
 * reset — a separate integration, not attempted here.
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
#define HOLD_S      45
#define HOST_LIGHT_SLEEP 0

static const char *TAG = "standby";
static volatile bool s_connected = false;
static volatile int  s_disassoc  = 0;
static volatile int  s_exit_cb   = 0;
static void sta_status_cb(enum mmwlan_sta_state st)
{
    bool now = (st == MMWLAN_STA_CONNECTED);
    if (s_connected && !now) s_disassoc++;
    s_connected = now;
}
static void standby_exit_cb(uint8_t reason, void *arg)
{
    s_exit_cb++;
    ESP_LOGW(TAG, "=== standby EXIT callback: reason=%d ===", reason);
}

void app_main(void)
{
    vTaskDelay(pdMS_TO_TICKS(200));
    gpio_reset_pin(GUARD_PIN);
    gpio_set_direction(GUARD_PIN, GPIO_MODE_INPUT);
    gpio_set_pull_mode(GUARD_PIN, GPIO_PULLDOWN_ONLY);
    vTaskDelay(pdMS_TO_TICKS(50));
    if (gpio_get_level(GUARD_PIN) == 1) {
        ESP_LOGW(TAG, "=== FLASH-HOLD ===");
        while (1) vTaskDelay(pdMS_TO_TICKS(3000));
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

    mmwlan_override_max_tx_power(1);
    mmhalow_connect(sta_status_cb);
    int waited = 0;
    while (!s_connected && waited < 30000) { vTaskDelay(pdMS_TO_TICKS(100)); waited += 100; }
    if (!s_connected) { ESP_LOGW(TAG, "connect timeout"); while (1) vTaskDelay(pdMS_TO_TICKS(5000)); }
    ESP_LOGW(TAG, "=== CONNECTED — entering standby (chip holds the link, host will sleep) ===");

    mmwlan_set_power_save_mode(MMWLAN_PS_ENABLED);

    /* Enter standby — the chip takes over keeping the association alive. */
    struct mmwlan_standby_enter_args args = { .standby_exit_cb = standby_exit_cb, .standby_exit_arg = NULL };
    enum mmwlan_status st = mmwlan_standby_enter(&args);
    ESP_LOGW(TAG, "=== mmwlan_standby_enter ret=%d (0=OK, else standby unsupported/failed) ===", st);

#if HOST_LIGHT_SLEEP
    esp_pm_config_t pm = { .max_freq_mhz = 160, .min_freq_mhz = 40, .light_sleep_enable = true };
    esp_pm_configure(&pm);
#endif

    ESP_LOGW(TAG, "=== STANDBY hold %ds (chip autonomous; host light-sleeping) ===", HOLD_S);
    for (int t = 0; t < HOLD_S; t++) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if ((t % 15) == 14)
            ESP_LOGW(TAG, "    standby +%ds: connected=%d disassoc=%d exit_cb=%d", t+1,
                     (int)s_connected, s_disassoc, s_exit_cb);
    }

    enum mmwlan_status xt = mmwlan_standby_exit();
    ESP_LOGW(TAG, "=== mmwlan_standby_exit ret=%d — connected=%d disassoc=%d exit_cb=%d ===",
             xt, (int)s_connected, s_disassoc, s_exit_cb);
    while (1) vTaskDelay(pdMS_TO_TICKS(5000));
}
