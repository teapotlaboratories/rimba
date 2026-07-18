/*
 * test-power — the power-save ladder DUT for the `tp` (power) regression tier.
 *
 * A dedicated test-* copy of rimba-halow-sta's proven, C6-triggered 4-tier PS ladder. It is
 * the reporter for the `tp` tier, but — unlike a T2 reporter — it emits NO TEST|RESULT: the
 * verdict is a HOST-side PPK2 current measurement the firmware physically cannot see. What the
 * firmware DOES emit on the TEST| console is exactly what the host runner (tools/regtest/
 * tp_power.py) needs:
 *
 *   TEST|BEGIN                              once, at boot
 *   TEST|STEP|associated|...                once, after connect (the validity anchor)
 *   TEST|INFO|phase=N <name> start ...      per tier entry -> the host's segmentation timestamps
 *   TEST|STEP|twt-installed|...             P3, mmwlan_twt_agreement_installed (AP-dependent)
 *   TEST|STEP|wnm-accepted|...              P4, the _ext ret (false-PASS guard: a low mA with a
 *                                              failed structural step is INCONCLUSIVE, not PASS)
 *   TEST|INFO|phase=5 ladder-done ...       end of one run
 *
 * The rig (kept verbatim from rimba-halow-sta so "keep the C6 in the loop" holds):
 *   trigger : the C6 (GPIO20) pulls D5/GPIO6 LOW then releases it HIGH -> one ladder run
 *   marker  : phase N = N x 60 ms pulses on D5 at entry (the C6 / a PPK2 logic input can timestamp)
 *   phases  : 1 = No-PS  2 = Dyn-PS  3 = TWT(10 s SP)  4 = WNM + chip-powerdown  (all host-AWAKE)
 *   guard   : D5 HIGH at boot => flash-hold idle (always reflashable — the deep-tier USB-wedge fix)
 *   TX cap  : mmwlan_override_max_tx_power(1) — MANDATORY on the close bench (an uncapped TX
 *             saturates the AP RX -> retries keep the radio awake -> inflated doze current that
 *             MIMICS the fw-1.17.9 regression, a false positive). Remove for a real deployment.
 *
 * The DUT associates to SSID "rimba-ping"/SAE, which BOTH the ESP SoftAP (test-apsta-ap) and
 * the Linux hostapd_s1g AP (chronite hostapd-rimba.conf) advertise, so one firmware serves both
 * `tp` AP paths.
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
#include "esp_pm.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "mmhalow.h"
#include "mmwlan.h"
#include "test_report.h"

#define LINK_SSID  "rimba-ping"
#define LINK_PSK   "rimbahalow"
#define PHASE_PIN  GPIO_NUM_6          /* XIAO pad D5 — the single free pin shared with the C6 */
#define PHASE_S    18                  /* seconds per tier */
#define TWT_FLOW_ID 0
/* 0 = host AWAKE (the calibrated tp bands); 1 = ESP32 host light sleep (the tp --light-sleep variant,
 * a STRONGER secondary gate: light-sleep backfires to ~30 mA on 1.17.9 vs ~8-9 mA at 1.17.8, ~3-4x).
 * A build-time arg -- the Makefile threads HOST_LIGHT_SLEEP= -> -D TEST_HOST_LIGHT_SLEEP, which the
 * CMakeLists maps to this define. Default 0 so a plain build (T0/T1) is the host-awake ladder. */
#ifndef HOST_LIGHT_SLEEP
#define HOST_LIGHT_SLEEP 0
#endif

/* The uncommitted structural accessor test-twt also uses (mmwlan* glob, exported unmangled). */
extern int mmwlan_twt_agreement_installed(uint16_t flow_id);

static const char *TAG = "test-power";
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
 * Waits for LOW (assert) THEN HIGH again (release) so the C6 is off the line before we drive it. */
static void wait_for_trigger(void)
{
    gpio_reset_pin(PHASE_PIN);
    gpio_set_direction(PHASE_PIN, GPIO_MODE_INPUT);
    gpio_set_pull_mode(PHASE_PIN, GPIO_PULLUP_ONLY);
    TEST_INFO("idle: waiting for the C6 trigger on D5/GPIO%d", PHASE_PIN);
    while (gpio_get_level(PHASE_PIN) == 1) { vTaskDelay(pdMS_TO_TICKS(50)); }  /* assert (low)  */
    while (gpio_get_level(PHASE_PIN) == 0) { vTaskDelay(pdMS_TO_TICKS(10)); }  /* release (high) */
    vTaskDelay(pdMS_TO_TICKS(20));
    TEST_INFO("triggered uptime=%us — running 4x%ds ladder", up_s(), PHASE_S);
}

/* One fixed-schedule, all-host-AWAKE ladder; entry of each tier marked on D5 + the TEST| console.
 * The "phase=N <name> start" INFO line is the host's per-tier segmentation timestamp. */
static void run_ladder(void)
{
    phase_mark(1);
    mmwlan_set_power_save_mode(MMWLAN_PS_DISABLED);
    TEST_INFO("phase=1 no-ps start uptime=%us", up_s());
    vTaskDelay(pdMS_TO_TICKS(PHASE_S * 1000));

    phase_mark(2);
    mmwlan_set_power_save_mode(MMWLAN_PS_ENABLED);
    TEST_INFO("phase=2 dyn-ps start uptime=%us", up_s());
    vTaskDelay(pdMS_TO_TICKS(PHASE_S * 1000));

    phase_mark(3);
    struct mmwlan_twt_config_args twt = MMWLAN_TWT_CONFIG_ARGS_INIT;
    twt.twt_mode = MMWLAN_TWT_REQUESTER;
    twt.twt_wake_interval_us = 10000000;
    twt.twt_min_wake_duration_us = 65280;
    twt.twt_setup_command = MMWLAN_TWT_SETUP_REQUEST;
    enum mmwlan_status ts = mmwlan_twt_setup_request(&twt);
    TEST_INFO("phase=3 twt start uptime=%us (setup ret=%d)", up_s(), (int)ts);
    vTaskDelay(pdMS_TO_TICKS(2000));   /* give the responder a moment to answer before polling */
    int inst = mmwlan_twt_agreement_installed(TWT_FLOW_ID);
    /* AP-dependent: the ESP AP installs a mid-session agreement; a Linux hostapd IGNORES the
     * mid-session action (STA stays at dyn-PS). So this STEP is a recorded fact, not a hard gate —
     * the TWT tier is measured-not-scored. It still guards a false PASS on the ESP path. */
    TEST_STEP("twt-installed", inst == 1, "flow=%d installed=%d (AP-dependent)", TWT_FLOW_ID, inst);
    vTaskDelay(pdMS_TO_TICKS(PHASE_S * 1000 - 2000));
    mmwlan_twt_teardown();

    phase_mark(4);
    struct mmwlan_set_wnm_sleep_enabled_args wnm = { .wnm_sleep_enabled = true,
                                                     .chip_powerdown_enabled = true };
    enum mmwlan_status ws = mmwlan_set_wnm_sleep_enabled_ext(&wnm);
    TEST_INFO("phase=4 wnm-powerdown start uptime=%us (enter ret=%d)", up_s(), (int)ws);
    /* The structural half of the deepest tier: WNM-Sleep-Enter must be accepted for the low mA to
     * mean "dozing while associated" and not "failed to doze". A low mA with this FAILED -> the host
     * runner reports INCONCLUSIVE, never PASS. */
    TEST_STEP("wnm-accepted", ws == MMWLAN_SUCCESS, "enter ret=%d chip_powerdown=1", (int)ws);
    vTaskDelay(pdMS_TO_TICKS(PHASE_S * 1000));
    wnm.wnm_sleep_enabled = false;
    mmwlan_set_wnm_sleep_enabled_ext(&wnm);

    phase_mark(5);   /* end-of-run marker */
    TEST_INFO("phase=5 ladder-done uptime=%us", up_s());
}

void app_main(void)
{
    /* Let the USB-Serial-JTAG console attach before we print. */
    vTaskDelay(pdMS_TO_TICKS(200));

    /* === flash-hold guard (checked FIRST, before any radio/NVS init) ===
     * D5/GPIO6 HIGH at boot => do NOT run; idle host-AWAKE forever so the host can always reflash
     * board2 (the deep-tier USB-wedge escape hatch). Default pull-DOWN => LOW/floating = normal run. */
    gpio_reset_pin(PHASE_PIN);
    gpio_set_direction(PHASE_PIN, GPIO_MODE_INPUT);
    gpio_set_pull_mode(PHASE_PIN, GPIO_PULLDOWN_ONLY);
    vTaskDelay(pdMS_TO_TICKS(50));
    if (gpio_get_level(PHASE_PIN) == 1) {
        ESP_LOGW(TAG, "FLASH-HOLD: D5/GPIO%d HIGH at boot — idling host-awake for reflash", PHASE_PIN);
        uint32_t held = 0;
        while (1) {
            vTaskDelay(pdMS_TO_TICKS(3000));
            ESP_LOGW(TAG, "FLASH-HOLD idling %us — safe to reflash (drive D5 low + reset to run)",
                     (unsigned)(3 * ++held));
        }
    }

    TEST_BEGIN("power",
                  "board2 STA on the PPK2 rail; the C6 triggers a 4-tier PS ladder; the host samples current");
    TEST_INFO("D5/GPIO%d low at boot -> normal run; ssid=\"%s\"", PHASE_PIN, LINK_SSID);

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_netif_init());

    mmhalow_init(NULL);
    mmhalow_print_version_info();   /* logs morselib + CHIP FW version — the gate is meaningless without it */
    mmhalow_wifi_config_t conf = { .sta = MMWLAN_STA_ARGS_INIT };
    memcpy(conf.sta.ssid, LINK_SSID, strlen(LINK_SSID));
    conf.sta.ssid_len = strlen(LINK_SSID);
    memcpy(conf.sta.passphrase, LINK_PSK, strlen(LINK_PSK));
    conf.sta.passphrase_len = strlen(LINK_PSK);
    conf.sta.security_type = MMWLAN_SAE;
    ESP_ERROR_CHECK(mmhalow_set_config(WIFI_IF_STA, &conf));

    mmwlan_override_max_tx_power(1);   /* close-bench RX-overload workaround — see the header comment */
    TEST_INFO("connecting (TX capped to 1 dBm for the close bench)...");
    mmhalow_connect(sta_status_cb);
    int waited = 0;
    while (!s_connected && waited < 60000) { vTaskDelay(pdMS_TO_TICKS(500)); waited += 500; }
    TEST_STEP("associated", s_connected, "connected=%d after %d ms ssid=%s tx_cap=1dBm",
                 (int)s_connected, waited, LINK_SSID);
    if (!s_connected) {
        TEST_INFO("link-up timeout — the ladder will still run on trigger, but a valid power gate "
                     "needs association (the host marks an unassociated run INCONCLUSIVE)");
    }

#if HOST_LIGHT_SLEEP
    esp_pm_config_t pm = { .max_freq_mhz = 160, .min_freq_mhz = 40, .light_sleep_enable = true };
    esp_pm_configure(&pm);
    TEST_INFO("host light sleep enabled (160/40 MHz)");
#endif

    /* idle -> triggered run -> idle, forever. The host captures one complete P1..ladder-done cycle. */
    while (1) {
        wait_for_trigger();
        run_ladder();
    }
}
