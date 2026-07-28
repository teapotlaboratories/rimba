/*
 * test-ibss-boottime — RISK-02: measure cold-boot -> IBSS-joined -> first-frame-exchanged.
 *
 * Two build modes (one flat binary, role by a compile flag so the harness can flash either):
 *   - CREATE (TEST_IBSS_CREATE=1)  -> the peer/creator. Creates the provisioned cell, pins a static
 *     IP, and lwIP auto-responds to the DUT's pings. Always-on support role (board0). Prints its IP.
 *   - MEASURE (default)            -> the DUT (board2). JOINs the cell, times each cold-boot phase with
 *     esp_timer, pings the peer (TEST_PING_IP) as soon as its IP is up, and prints ONE summary line
 *     at the first ICMP reply:  BOOTTIME_MS app=.. fw=.. ibss=.. frame=.. total=..
 *
 * Phase clock: esp_timer_get_time() (us since the ESP-IDF timer started, i.e. early boot). The printed
 * `total` therefore EXCLUDES the power-on-edge -> timer-start gap (power ramp + ROM/2nd-stage
 * bootloader); that piece is measured separately, host-side, from the PPK2 power-on edge to first
 * current activity, and the host adds it to this number to get the true cold-boot figure.
 * Phases:
 *   app   = timer-start -> app_main entry (ESP-IDF startup before app_main)
 *   fw    = mmhalow_init (mmwlan_boot: MM6108 .mbin load over SPI + chip init) + netif
 *   ibss  = mmwlan_ibss_start (ADD ADHOC vif + cfg_ibss JOIN + beaconing = joined)
 *   frame = joined -> first ICMP reply from the peer (IP up + ARP + first exchange)
 *   total = timer-start -> first frame (= app + fw + ibss + frame)
 *
 * Cell params identical to rimba-halow-ibss (same BSSID/SSID/chan so a rimba-halow-ibss peer also works).
 */

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "ping/ping_sock.h"

#include "mmhalow.h"
#include "mmwlan.h"
#include "umac/ibss/umac_ibss.h"

#define LINK_SSID      "rimba-ibss"
#define LINK_S1G_CHAN  27
#define NETMASK        "255.255.255.0"
#define IP_PREFIX      "192.168.13."
static const uint8_t LINK_BSSID[6] = { 0x02, 0x12, 0x34, 0x56, 0x78, 0x9a };
static const char *TAG = "ibss-boottime";

/* MEASURE mode pings this peer (the CREATE node's IP); pass it at build time via PING_IP= .
 * The default exists ONLY so a bare `make build` (i.e. the T0 matrix, which passes no flags) still
 * compiles -- it is a placeholder, not a bench fact. Any real run must pass PING_IP=<creator's IP>;
 * the creator prints its own IP at boot for exactly this purpose. */
#ifndef TEST_PING_IP
#define TEST_PING_IP "192.168.13.1"
#endif

static void mac_to_ip(const uint8_t *mac, char *out, size_t outlen)
{
    unsigned octet = mac[5];
    if (octet == 0)   octet = 1;
    if (octet == 255) octet = 254;
    snprintf(out, outlen, IP_PREFIX "%u", octet);
}

static void setup_static_ip(const char *my_ip)
{
    esp_netif_t *n = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (n == NULL) { ESP_LOGE(TAG, "netif WIFI_STA_DEF not found"); return; }
    esp_netif_dhcpc_stop(n);
    esp_netif_ip_info_t ip = { 0 };
    ip.ip.addr = esp_ip4addr_aton(my_ip);
    ip.gw.addr = esp_ip4addr_aton(my_ip);
    ip.netmask.addr = esp_ip4addr_aton(NETMASK);
    ESP_ERROR_CHECK(esp_netif_set_ip_info(n, &ip));
    esp_netif_action_connected(n, NULL, 0, NULL);
}

#ifndef TEST_IBSS_CREATE   /* ===== MEASURE mode (the DUT) ===== */

static volatile int64_t s_t_fw_us;        /* esp_timer after mmhalow_init (fw loaded) */
static volatile int64_t s_t_ibss_us;      /* esp_timer at "joined" */
static volatile bool s_reported;

static void on_ping_success(esp_ping_handle_t hdl, void *args)
{
    if (s_reported) return;
    s_reported = true;
    int64_t t_frame = esp_timer_get_time();
    int64_t app_us  = (int64_t)(intptr_t)args;     /* t0 (esp_timer @ app_main entry) */
    int64_t t_fw    = s_t_fw_us;
    int64_t t_ibss  = s_t_ibss_us;
    /* Phase deltas in ms (float, 1 dp). */
    double app  = app_us / 1000.0;
    double fw   = (t_fw   - app_us) / 1000.0;
    double ibss = (t_ibss - t_fw)   / 1000.0;
    double frame= (t_frame- t_ibss) / 1000.0;
    double total= t_frame / 1000.0;
    uint16_t seqno; uint32_t elapsed;
    esp_ping_get_profile(hdl, ESP_PING_PROF_SEQNO, &seqno, sizeof(seqno));
    esp_ping_get_profile(hdl, ESP_PING_PROF_TIMEGAP, &elapsed, sizeof(elapsed));
    /* The single machine-parseable result line. */
    printf("BOOTTIME_MS app=%.1f fw=%.1f ibss=%.1f frame=%.1f total=%.1f (peer=%s seq=%u rtt=%" PRIu32 "ms)\n",
           app, fw, ibss, frame, total, TEST_PING_IP, seqno, elapsed);
    fflush(stdout);
    ESP_LOGI(TAG, "==> first frame exchanged: total cold-boot(CPU)->first-reply = %.1f ms", total);
}

void app_main(void)
{
    int64_t t0 = esp_timer_get_time();     /* CPU-active reference */

    /* Announce the compiled role FIRST. idf.py's CMakeCache under build/<app>/<board>/ is persistent,
     * so a -D TEST_IBSS_CREATE from an earlier `make ... IBSS_CREATE=1` SURVIVES a later build that
     * omits the flag -- the same cache-accumulation footgun tools/regtest/t2_onair.py:50 clears for
     * harness-driven apps. This fixture is driven BY HAND, so nothing clears it for us: print the role
     * so a stale-cache misflash is obvious here instead of looking like a dead link. */
    ESP_LOGW(TAG, "ROLE=MEASURE (DUT) -- will JOIN and time the boot; peer=%s", TEST_PING_IP);

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_netif_init());

    mmhalow_init(NULL);                    /* mmwlan_boot: MM6108 .mbin load + chip init + netif */
    s_t_fw_us = esp_timer_get_time();
    ESP_LOGI(TAG, "fw phase (mmhalow_init) done @ %.1f ms", (s_t_fw_us - t0) / 1000.0);

    uint8_t mac[6] = { 0 };
    /* A failure here leaves mac all-zero, which derives a bogus if_addr AND a bogus IP -- on the
     * bench that reads as "the cell never formed" rather than "we never got our MAC". Say so. */
    if (mmwlan_get_mac_addr(mac) != MMWLAN_SUCCESS) {
        ESP_LOGE(TAG, "mmwlan_get_mac_addr FAILED -- cannot derive if_addr/IP; halting");
        for (;;) vTaskDelay(pdMS_TO_TICKS(5000));
    }
    char my_ip[16];
    mac_to_ip(mac, my_ip, sizeof(my_ip));

    /* Refuse to measure against ourselves. mac_to_ip can derive ANY host in this /24 (it clamps to
     * .1/.254), so a DUT whose mac[5] lands on the configured peer would ping its own address: lwIP
     * answers locally and instantly, and we would print a perfectly plausible BOOTTIME_MS whose
     * `frame` phase never touched the air. A wrong number that looks right is worse than no number. */
    if (strcmp(my_ip, TEST_PING_IP) == 0) {
        ESP_LOGE(TAG, "peer PING_IP=%s is THIS board's own derived IP -- the ping would never leave "
                      "the host and the boot time would be fiction. Pass the creator's IP via PING_IP=.",
                 TEST_PING_IP);
        for (;;) vTaskDelay(pdMS_TO_TICKS(5000));
    }

    struct mmwlan_ibss_args args = { 0 };
    memcpy(args.bssid, LINK_BSSID, sizeof(args.bssid));
    memcpy(args.ssid, LINK_SSID, strlen(LINK_SSID));
    args.ssid_len = strlen(LINK_SSID);
    args.create = false;                   /* the DUT always JOINs the peer's cell */
    args.s1g_chan_num = LINK_S1G_CHAN;
    args.beacon_interval_tu = 100;
    memcpy(args.if_addr, mac, sizeof(args.if_addr));

    enum mmwlan_status st = mmwlan_ibss_start(&args);
    if (st != MMWLAN_SUCCESS) {
        ESP_LOGE(TAG, "mmwlan_ibss_start FAILED status=%d", st);
        for (;;) vTaskDelay(pdMS_TO_TICKS(5000));
    }
    s_t_ibss_us = esp_timer_get_time();
    ESP_LOGI(TAG, "ibss JOINED @ %.1f ms (my ip %s); pinging peer %s ...",
             (s_t_ibss_us - t0) / 1000.0, my_ip, TEST_PING_IP);

    /* No settle delay here, deliberately: rimba-halow-ibss waits 1500 ms before this call, but that
     * wait would land inside the measured `frame` phase and inflate the very number we are after.
     * The netif already exists (mmhalow_init created it in the `fw` phase), so assigning the address
     * is safe this early; the ping retry loop below absorbs however long the link takes to carry. */
    setup_static_ip(my_ip);

    /* Ping the peer immediately (retries every 500 ms until the link carries it). First reply = the
     * first frame exchanged -> on_ping_success stamps the total. */
    ip_addr_t target = { 0 };
    target.type = IPADDR_TYPE_V4;
    target.u_addr.ip4.addr = esp_ip4addr_aton(TEST_PING_IP);
    esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
    cfg.target_addr = target;
    cfg.count = ESP_PING_COUNT_INFINITE;
    cfg.interval_ms = 500;
    cfg.timeout_ms  = 500;
    esp_ping_callbacks_t cbs = { .on_ping_success = on_ping_success,
                                 .cb_args = (void *)(intptr_t)t0 };
    /* Without this branch a failed session is indistinguishable on the console from a link that never
     * came up: no BOOTTIME_MS line, no error, just an idle board. */
    esp_ping_handle_t ping;
    esp_err_t perr = esp_ping_new_session(&cfg, &cbs, &ping);
    if (perr == ESP_OK) {
        esp_ping_start(ping);
    } else {
        ESP_LOGE(TAG, "esp_ping_new_session FAILED (%s) -- no BOOTTIME_MS line will be printed",
                 esp_err_to_name(perr));
    }

    for (;;) vTaskDelay(pdMS_TO_TICKS(5000));   /* the PPK2 power-cycles us for the next sample */
}

#else   /* ===== CREATE mode (the peer / creator; board0, always-on) ===== */

void app_main(void)
{
    /* See the MEASURE-side note: the role flag is sticky in idf.py's persistent CMakeCache, so state
     * which role actually got compiled in. */
    ESP_LOGW(TAG, "ROLE=CREATE (peer) -- will CREATE the cell and answer pings, not measure anything");

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_netif_init());

    mmhalow_init(NULL);
    mmhalow_print_version_info();

    uint8_t mac[6] = { 0 };
    /* A failure here leaves mac all-zero, which derives a bogus if_addr AND a bogus IP -- on the
     * bench that reads as "the cell never formed" rather than "we never got our MAC". Say so. */
    if (mmwlan_get_mac_addr(mac) != MMWLAN_SUCCESS) {
        ESP_LOGE(TAG, "mmwlan_get_mac_addr FAILED -- cannot derive if_addr/IP; halting");
        for (;;) vTaskDelay(pdMS_TO_TICKS(5000));
    }
    char my_ip[16];
    mac_to_ip(mac, my_ip, sizeof(my_ip));

    struct mmwlan_ibss_args args = { 0 };
    memcpy(args.bssid, LINK_BSSID, sizeof(args.bssid));
    memcpy(args.ssid, LINK_SSID, strlen(LINK_SSID));
    args.ssid_len = strlen(LINK_SSID);
    args.create = true;                    /* the peer CREATEs the provisioned cell */
    args.s1g_chan_num = LINK_S1G_CHAN;
    args.beacon_interval_tu = 100;
    memcpy(args.if_addr, mac, sizeof(args.if_addr));

    ESP_LOGI(TAG, "IBSS CREATE: mac=%02x:%02x:%02x:%02x:%02x:%02x  ip=%s (the peer the DUT pings)",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], my_ip);
    enum mmwlan_status st = mmwlan_ibss_start(&args);
    if (st != MMWLAN_SUCCESS) { ESP_LOGE(TAG, "ibss_start FAILED %d", st); for (;;) vTaskDelay(pdMS_TO_TICKS(5000)); }

    vTaskDelay(pdMS_TO_TICKS(1000));
    setup_static_ip(my_ip);
    ESP_LOGI(TAG, "==> IBSS peer up @ %s — lwIP will auto-reply to the DUT's pings. PEER_IP=%s", my_ip, my_ip);

    for (;;) vTaskDelay(pdMS_TO_TICKS(10000));
}

#endif
