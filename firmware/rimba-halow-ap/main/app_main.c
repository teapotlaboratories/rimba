/*
 * rimba-halow-ap — HaLow SoftAP node for the 2-board ping test.
 *
 * Brings up an 802.11ah SoftAP, assigns its netif a STATIC IP (so lwIP answers
 * ICMP), and also pings the STA back — giving a bidirectional check. Pair with
 * rimba-halow-sta, which associates, pins a static IP on the same subnet, and
 * pings this node.
 *
 * mmhalow creates a DHCP-client netif (no DHCP server in HaLow AP mode), so we
 * stop the client and set a static address. The MM6108 has no public IBSS path
 * in morselib, so AP<->STA is the proven two-node link (the RISK-01 fallback).
 *
 *   LINK PARAMS BELOW MUST MATCH firmware/rimba-halow-sta/main/app_main.c.
 */

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "ping/ping_sock.h"

#include "mmhalow.h"

/* --- Link / IP parameters (keep identical to rimba-halow-sta) ---------------- */
#define LINK_SSID      "rimba-ping"
#define LINK_PSK       "rimbahalow"   /* >= 8 chars, required for SAE */
#define LINK_S1G_CHAN  27             /* US 915.5 MHz, 1 MHz BW (global op-class 68) */
#define LINK_OP_CLASS  68
#define LINK_MAX_STAS  4              /* allow multiple leaves (multi-STA TWT test) */
#define AP_IP          "192.168.12.1"
#define STA_IP         "192.168.12.2"   /* the STA — our ping target */
#define NETMASK        "255.255.255.0"
/* ---------------------------------------------------------------------------- */

static const char *TAG = "rimba-ap";

/* --- multi-STA TWT validation: track + periodically report authorized stations.
 * The per-STA status callback fires during the association window (when the USB-CDC
 * console is frozen), so it only updates state here; a separate task logs the list
 * every few seconds once the console is alive again. Seeing >1 authorized STA proves
 * the AP admits multiple leaves concurrently (each a TWT requester). ----------- */
#define TRACK_MAX 8
static uint8_t s_sta_macs[TRACK_MAX][MMWLAN_MAC_ADDR_LEN];
static volatile int s_sta_n;
static bool s_pinged[256];                 /* one ping session per STA, keyed by mac[5] */
static void start_ping_octet(uint8_t octet);

static void ap_sta_status_cb(const struct mmwlan_ap_sta_status *st, void *arg)
{
    (void)arg;
    if (st == NULL) return;
    int idx = -1;
    for (int i = 0; i < s_sta_n; i++)
        if (memcmp(s_sta_macs[i], st->mac_addr, MMWLAN_MAC_ADDR_LEN) == 0) { idx = i; break; }
    if (st->state == MMWLAN_AP_STA_AUTHORIZED) {
        if (idx < 0 && s_sta_n < TRACK_MAX)
            memcpy(s_sta_macs[s_sta_n++], st->mac_addr, MMWLAN_MAC_ADDR_LEN);
    } else if (st->state == MMWLAN_AP_STA_UNKNOWN && idx >= 0) {
        for (int j = idx; j < s_sta_n - 1; j++)
            memcpy(s_sta_macs[j], s_sta_macs[j + 1], MMWLAN_MAC_ADDR_LEN);
        s_sta_n--;
    }
}

static void sta_monitor_task(void *arg)
{
    (void)arg;
    for (;;) {
        int n = s_sta_n;
        ESP_LOGI(TAG, "=== authorized STAs: %d (max %d) ===", n, LINK_MAX_STAS);
        for (int i = 0; i < n; i++) {
            ESP_LOGI(TAG, "    sta[%d] " MACSTR, i, MAC2STR(s_sta_macs[i]));
            uint8_t octet = s_sta_macs[i][5];   /* STA IP = 192.168.12.<mac[5]> */
            if (!s_pinged[octet]) { s_pinged[octet] = true; start_ping_octet(octet); }
        }
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}

static void on_ping_success(esp_ping_handle_t hdl, void *args)
{
    uint16_t seqno;
    uint8_t ttl;
    uint32_t elapsed_ms;
    ip_addr_t target = { 0 };
    esp_ping_get_profile(hdl, ESP_PING_PROF_SEQNO, &seqno, sizeof(seqno));
    esp_ping_get_profile(hdl, ESP_PING_PROF_TTL, &ttl, sizeof(ttl));
    esp_ping_get_profile(hdl, ESP_PING_PROF_TIMEGAP, &elapsed_ms, sizeof(elapsed_ms));
    esp_ping_get_profile(hdl, ESP_PING_PROF_IPADDR, &target, sizeof(target));
    ESP_LOGI(TAG, "reply from " IPSTR ": seq=%u ttl=%u time=%" PRIu32 " ms",
             IP2STR(&target.u_addr.ip4), seqno, ttl, elapsed_ms);
}

static void on_ping_timeout(esp_ping_handle_t hdl, void *args)
{
    uint16_t seqno;
    esp_ping_get_profile(hdl, ESP_PING_PROF_SEQNO, &seqno, sizeof(seqno));
    ESP_LOGW(TAG, "ping timeout, seq=%u (STA not up yet?)", seqno);
}

/* Ping one STA (192.168.12.<octet>) continuously. Times out until that STA configures
 * its static IP, then replies — confirming the AP->STA (downlink) direction, and showing
 * the TWT doze signature once the STA establishes a TWT agreement. One session per STA. */
static void start_ping_octet(uint8_t octet)
{
    char ipbuf[20];
    snprintf(ipbuf, sizeof(ipbuf), "192.168.12.%u", (unsigned)octet);

    ip_addr_t target = { 0 };
    target.type = IPADDR_TYPE_V4;
    target.u_addr.ip4.addr = esp_ip4addr_aton(ipbuf);

    esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
    cfg.target_addr = target;
    cfg.count = ESP_PING_COUNT_INFINITE;
    cfg.interval_ms = 1000;

    esp_ping_callbacks_t cbs = {
        .on_ping_success = on_ping_success,
        .on_ping_timeout = on_ping_timeout,
        .on_ping_end = NULL,
        .cb_args = NULL,
    };

    esp_ping_handle_t ping;
    if (esp_ping_new_session(&cfg, &cbs, &ping) == ESP_OK) {
        ESP_LOGI(TAG, "Pinging STA %s every %" PRIu32 " ms...", ipbuf, cfg.interval_ms);
        esp_ping_start(ping);
    } else {
        ESP_LOGE(TAG, "failed to create ping session for %s", ipbuf);
    }
}

static void assign_static_ip(void)
{
    esp_netif_t *n = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (n == NULL) {
        ESP_LOGE(TAG, "netif WIFI_STA_DEF not found");
        return;
    }
    esp_netif_dhcpc_stop(n);   /* mmhalow's netif is a DHCP client; go static */

    esp_netif_ip_info_t ip = { 0 };
    ip.ip.addr = esp_ip4addr_aton(AP_IP);
    ip.gw.addr = esp_ip4addr_aton(AP_IP);
    ip.netmask.addr = esp_ip4addr_aton(NETMASK);
    ESP_ERROR_CHECK(esp_netif_set_ip_info(n, &ip));

    /* In AP mode mmhalow never fires a link-up event, so it never brings the
     * netif up — leaving lwIP unable to answer ICMP. Bring it up explicitly.
     * This is the key fix that makes the AP reachable over IP. */
    esp_netif_action_connected(n, NULL, 0, NULL);
    ESP_LOGI(TAG, "AP static IP " IPSTR ", netif up=%d — answers ICMP",
             IP2STR(&ip.ip), (int)esp_netif_is_netif_up(n));
}

void app_main(void)
{
    ESP_LOGI(TAG, "Booting HaLow SoftAP (ssid=\"%s\" chan=%d)...", LINK_SSID, LINK_S1G_CHAN);

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_netif_init());

    mmhalow_init(NULL);
    ESP_LOGI(TAG, "Wi-Fi HaLow initialised");
    mmhalow_print_version_info();

    mmhalow_wifi_config_t cfg = { .ap = MMWLAN_AP_ARGS_INIT };

    memcpy((char *)cfg.ap.ssid, LINK_SSID, strlen(LINK_SSID));
    cfg.ap.ssid_len = strlen(LINK_SSID);

    memcpy(cfg.ap.passphrase, LINK_PSK, strlen(LINK_PSK));
    cfg.ap.security_type = MMWLAN_SAE;
    cfg.ap.pmf_mode = MMWLAN_PMF_REQUIRED;

    cfg.ap.s1g_chan_num = LINK_S1G_CHAN;
    cfg.ap.op_class = LINK_OP_CLASS;
    cfg.ap.max_stas = LINK_MAX_STAS;
    cfg.ap.sta_status_cb = ap_sta_status_cb;

    ESP_ERROR_CHECK(mmhalow_set_config(WIFI_IF_AP, &cfg));

    ESP_LOGI(TAG, "Starting SoftAP...");
    mmhalow_wifi_start();

    /* Give the netif a moment to come up, then pin the static IP. */
    vTaskDelay(pdMS_TO_TICKS(1500));
    assign_static_ip();

    ESP_LOGI(TAG, "SoftAP up. Pinging each authorized STA and answering their pings.");

    /* sta_monitor_task starts one ping session per authorized STA (at 192.168.12.<mac[5]>). */
    xTaskCreate(sta_monitor_task, "stamon", 3072, NULL, 4, NULL);
}
