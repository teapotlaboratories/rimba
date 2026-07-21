/*
 * test-c6-trigger — ESP32-C6 companion for board2's PS-measurement rig.
 *
 * The regression harness's D5 fixture: it fires the tp power ladder (DUT test-power) and wakes the
 * dscycle deep-sleep leaf (DUT test-deepsleep-cycle). It drives board2's D5 (XIAO ESP32-S3 GPIO6) over a
 * single wire from C6 GPIO20 (+ common GND). board2 is powered and measured by the PPK2, NOT the C6.
 * See docs/reference/rimba-bench-devices.md — "Measurement harness" and "board2 won't flash — un-wedge it".
 *
 * board2's ladder/deep-sleep DUT fw (test-power / test-deepsleep-cycle) reads D5:
 *   - at boot (pull-DOWN):  HIGH => flash-hold (flashable idle);  LOW/default => run
 *   - while idle/asleep (pull-UP): a LOW pulse => trigger one ladder run / wake from deep sleep
 *
 * SERIAL CONTROL (added 2026-07-21). The D5 behaviour is no longer fixed at compile time — it is a
 * runtime-latched MODE driven over the C6's UART0 console (the CP2102N on /dev/ttyUSB0). This lets the
 * harness COMMAND a wake pulse on demand (fire it at the right point in board2's short awake window)
 * instead of relying on a free-running pulse that races that window — the enabler for a dependable
 * dscycle wake. Line-based commands (newline-terminated, case-insensitive first token):
 *
 *   pulse [ms]        one LOW pulse now (default 120 ms) then Hi-Z rest; sets mode=hiz. THE on-demand wake.
 *   hiz               tri-state D5 (measurement-safe rest; board2 boots via its own pull-down = run)
 *   high              drive D5 HIGH steady  => board2 boots into FLASH-HOLD (remote reflash / wedge recovery)
 *   low               drive D5 LOW steady   => board2 boots into RUN
 *   trigger [period_s] free-running LOW pulse every period (default 30 s) — the legacy tp free-run
 *   toggle [half_ms]  square wave, flip every half_ms (default 500 ms = 1 Hz) — wiring / link test
 *   save              persist the current mode+params to NVS (survives a C6 reboot)
 *   default           clear NVS (revert to the compile-time MODE on next boot)
 *   status            report mode / pin level / params / nvs
 *   ping              -> C6|PONG (health check)
 *
 * Responses are prefixed "C6|" so the harness can confirm. Power-on default = NVS (if saved) else the
 * compile-time MODE below, so an un-commanded C6 behaves exactly as it did before (tp is unaffected).
 *
 * Build (standalone IDF — NOT the repo `make`, which is ESP32-S3 only):
 *   export IDF_PATH=<repo>/vendor/esp-idf && source $IDF_PATH/export.sh
 *   idf.py set-target esp32c6 build && idf.py -p /dev/ttyUSB0 flash monitor
 */
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"

/* Compile-time power-on default (used only when NVS has no saved mode). MODE_TRIGGER keeps the legacy
 * free-running tp behaviour for an un-commanded C6. */
#define MODE_TRIGGER   0
#define MODE_HOLD_HIGH 1
#define MODE_HOLD_LOW  2
#define MODE_TOGGLE    3
#define MODE_HIZ       4
#define MODE           MODE_TRIGGER      /* <-- default when NVS is empty */

#define PIN            GPIO_NUM_20       /* wired to board2 D5 / GPIO6 */
#define TRIG_PERIOD_S  30                /* default free-running trigger period */
#define TOGGLE_HALF_MS 500               /* default toggle half-period (1 Hz) */
#define PULSE_MS       120               /* default commanded pulse width */

#define UART            UART_NUM_0
#define LINE_MAX        64
#define NVS_NS          "c6trig"
#define NVS_KEY_MODE    "mode"
#define NVS_KEY_PARAM   "param"          /* period_s (trigger) or half_ms (toggle) */

static const char *TAG = "test-c6-trigger";

/* Latched runtime state. */
static int      g_mode   = MODE;
static uint32_t g_param  = TRIG_PERIOD_S;   /* trigger period_s or toggle half_ms, per mode */
static int      g_level  = 1;               /* last driven level for toggle */
static int64_t  g_next_us = 0;              /* next free-running action (trigger/toggle) */

/* ---- D5 pin control ---------------------------------------------------- */
static void hiz(void)      { gpio_set_direction(PIN, GPIO_MODE_INPUT);
                             gpio_set_pull_mode(PIN, GPIO_FLOATING); }  /* true Hi-Z: no pull onto board2's D5 */
static void drive(int lvl) { gpio_set_direction(PIN, GPIO_MODE_OUTPUT); gpio_set_level(PIN, lvl); }

static const char *mode_name(int m)
{
    switch (m) {
    case MODE_TRIGGER:   return "trigger";
    case MODE_HOLD_HIGH: return "high";
    case MODE_HOLD_LOW:  return "low";
    case MODE_TOGGLE:    return "toggle";
    case MODE_HIZ:       return "hiz";
    default:             return "?";
    }
}

/* Apply the resting pin state for a mode and (re)arm the free-running schedule. */
static void enter_mode(int m, uint32_t param)
{
    g_mode = m;
    switch (m) {
    case MODE_HOLD_HIGH: drive(1); break;
    case MODE_HOLD_LOW:  drive(0); break;
    case MODE_TOGGLE:    g_param = param ? param : TOGGLE_HALF_MS; g_level = 1; drive(1); break;
    case MODE_TRIGGER:   g_param = param ? param : TRIG_PERIOD_S;  hiz();       break;
    case MODE_HIZ:
    default:             g_mode = MODE_HIZ; hiz(); break;
    }
    g_next_us = esp_timer_get_time();   /* schedule the next free-running action from now */
    if (m == MODE_TRIGGER) g_next_us += (int64_t)g_param * 1000000LL;
    else if (m == MODE_TOGGLE) g_next_us += (int64_t)g_param * 1000LL;
}

/* One commanded LOW pulse, then back to Hi-Z rest (the on-demand wake). */
static void pulse_low(uint32_t ms)
{
    drive(0);
    vTaskDelay(pdMS_TO_TICKS(ms ? ms : PULSE_MS));
    hiz();
    g_mode = MODE_HIZ;                   /* pulse leaves D5 at a measurement-safe rest */
}

/* ---- NVS persistence --------------------------------------------------- */
static void nvs_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    int32_t m = -1, p = 0;
    esp_err_t em = nvs_get_i32(h, NVS_KEY_MODE, &m);
    nvs_get_i32(h, NVS_KEY_PARAM, &p);
    nvs_close(h);
    if (em == ESP_OK && m >= 0 && m <= MODE_HIZ) {
        g_mode  = (int)m;
        g_param = (uint32_t)p;
        ESP_LOGW(TAG, "restored mode=%s param=%" PRIu32 " from NVS", mode_name(g_mode), g_param);
    }
}

static void nvs_save(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) { printf("C6|ERR nvs open\n"); return; }
    nvs_set_i32(h, NVS_KEY_MODE, g_mode);
    nvs_set_i32(h, NVS_KEY_PARAM, (int32_t)g_param);
    nvs_commit(h);
    nvs_close(h);
    printf("C6|SAVED mode=%s param=%" PRIu32 "\n", mode_name(g_mode), g_param);
}

static void nvs_clear(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) { printf("C6|ERR nvs open\n"); return; }
    nvs_erase_all(h);
    nvs_commit(h);
    nvs_close(h);
    printf("C6|CLEARED (compile default '%s' on next boot)\n", mode_name(MODE));
}

/* ---- command handling -------------------------------------------------- */
static void handle_line(char *line)
{
    char *tok = strtok(line, " \t");
    if (!tok) return;
    char *arg = strtok(NULL, " \t");
    uint32_t n = arg ? (uint32_t)strtoul(arg, NULL, 10) : 0;

    if (!strcasecmp(tok, "pulse")) {
        uint32_t ms = n ? n : PULSE_MS;
        if (ms > 2000) ms = 2000;   /* clamp: a long blocking pulse would starve the UART RX */
        printf("C6|PULSE ms=%" PRIu32 "\n", ms);
        pulse_low(ms);
        printf("C6|PULSE done -> hiz\n");
    } else if (!strcasecmp(tok, "hiz")) {
        enter_mode(MODE_HIZ, 0);       printf("C6|MODE hiz\n");
    } else if (!strcasecmp(tok, "high")) {
        enter_mode(MODE_HOLD_HIGH, 0); printf("C6|MODE high (board2 boots FLASH-HOLD)\n");
    } else if (!strcasecmp(tok, "low")) {
        enter_mode(MODE_HOLD_LOW, 0);  printf("C6|MODE low (board2 boots RUN)\n");
    } else if (!strcasecmp(tok, "trigger")) {
        enter_mode(MODE_TRIGGER, n);   printf("C6|MODE trigger period=%" PRIu32 "s\n", g_param);
    } else if (!strcasecmp(tok, "toggle")) {
        enter_mode(MODE_TOGGLE, n);    printf("C6|MODE toggle half=%" PRIu32 "ms\n", g_param);
    } else if (!strcasecmp(tok, "save")) {
        nvs_save();
    } else if (!strcasecmp(tok, "default")) {
        nvs_clear();
    } else if (!strcasecmp(tok, "status")) {
        printf("C6|STATUS mode=%s param=%" PRIu32 " pin=GPIO%d\n", mode_name(g_mode), g_param, PIN);
    } else if (!strcasecmp(tok, "ping")) {
        printf("C6|PONG\n");
    } else {
        printf("C6|ERR unknown: %s\n", tok);
    }
}

/* Service the free-running modes (trigger/toggle) on schedule; hiz/high/low rest after enter_mode. */
static void service_mode(void)
{
    int64_t now = esp_timer_get_time();
    if (g_mode == MODE_TRIGGER) {
        if (now >= g_next_us) {
            pulse_low(PULSE_MS);
            g_mode = MODE_TRIGGER;                 /* pulse_low set hiz/HIZ; stay in trigger */
            g_next_us = esp_timer_get_time() + (int64_t)g_param * 1000000LL;
        }
    } else if (g_mode == MODE_TOGGLE) {
        if (now >= g_next_us) {
            g_level ^= 1;
            drive(g_level);
            g_next_us = now + (int64_t)g_param * 1000LL;
        }
    }
}

void app_main(void)
{
    /* NVS (for the latched mode) — tolerate a fresh/So partition. */
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    /* UART0 RX driver so we can read command lines from the console (TX/logging is unaffected). */
    uart_driver_install(UART, 256, 0, 0, NULL, 0);

    gpio_reset_pin(PIN);
    nvs_load();
    enter_mode(g_mode, g_param);

    ESP_LOGW(TAG, "test-c6-trigger ready — serial control on UART0. mode=%s param=%" PRIu32
                  " (send 'status' / 'ping' / 'pulse' / 'hiz' / 'trigger' ...)",
             mode_name(g_mode), g_param);
    printf("C6|READY mode=%s param=%" PRIu32 "\n", mode_name(g_mode), g_param);

    char line[LINE_MAX];
    int len = 0;
    while (1) {
        uint8_t c;
        int r = uart_read_bytes(UART, &c, 1, pdMS_TO_TICKS(20));
        if (r == 1) {
            if (c == '\n' || c == '\r') {
                if (len > 0) { line[len] = '\0'; handle_line(line); len = 0; }
            } else if (len < LINE_MAX - 1) {
                line[len++] = (char)c;
            } else {
                len = 0;   /* overflow -> drop the line */
            }
        }
        service_mode();
    }
}
