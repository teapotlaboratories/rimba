/*
 * rimba-halow-ibss — RISK-01 IBSS / ad-hoc bring-up + data test.
 *
 * EXPERIMENTAL. Brings up the MM6108 in IBSS mode via the ported
 * mmwlan_ibss_enable():
 *     ADD_INTERFACE(ADHOC) -> SET_CHANNEL -> BSSID_SET -> BSS_CONFIG -> IBSS_CONFIG
 * (reverse-engineered from the MorseMicro Linux driver, morse_driver), beacons
 * and answers probe requests (mirroring net/mac80211/ibss.c), then pins a static
 * IP and pings the peer to exercise the IBSS data path (Phase-1 success gate).
 * See docs/worklog/2026-06-18-risk01-ibss-recon.md.
 *
 * ONE binary runs on both boards: the role (creator/IP .1 vs joiner/IP .2) is
 * chosen at runtime from the MAC, so no separate build per node. (This MAC split
 * is a 2-board bench heuristic; real role/merge would use IBSS TSF arbitration.)
 */

#include <inttypes.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "ping/ping_sock.h"

#include "mmhalow.h"
#include "mmwlan.h"

/* --- IBSS link parameters (identical on every node in the cell) -------------- */
#define LINK_SSID      "rimba-ibss"
#define LINK_S1G_CHAN  27             /* US 915.5 MHz, 1 MHz BW (global op-class 68) */
#define LINK_OP_CLASS  68
#define NETMASK        "255.255.255.0"
#define CREATOR_IP     "192.168.13.1"
#define JOINER_IP      "192.168.13.2"

/* IBSS BSSID — MUST be identical on every node. Locally-administered. */
static const uint8_t LINK_BSSID[6] = { 0x02, 0x12, 0x34, 0x56, 0x78, 0x9a };
/* ---------------------------------------------------------------------------- */

static const char *TAG = "rimba-ibss";

static void on_ping_success(esp_ping_handle_t hdl, void *args)
{
    uint16_t seqno;
    uint32_t elapsed_ms;
    ip_addr_t target = { 0 };
    esp_ping_get_profile(hdl, ESP_PING_PROF_SEQNO, &seqno, sizeof(seqno));
    esp_ping_get_profile(hdl, ESP_PING_PROF_TIMEGAP, &elapsed_ms, sizeof(elapsed_ms));
    esp_ping_get_profile(hdl, ESP_PING_PROF_IPADDR, &target, sizeof(target));
    ESP_LOGI(TAG, "reply from " IPSTR ": seq=%u time=%" PRIu32 " ms  <== IBSS DATA OK",
             IP2STR(&target.u_addr.ip4), seqno, elapsed_ms);
}

static void on_ping_timeout(esp_ping_handle_t hdl, void *args)
{
    uint16_t seqno;
    esp_ping_get_profile(hdl, ESP_PING_PROF_SEQNO, &seqno, sizeof(seqno));
    ESP_LOGW(TAG, "ping timeout seq=%u (peer not up yet?)", seqno);
}

static void start_static_ip_and_ping(const char *my_ip, const char *peer_ip)
{
    esp_netif_t *n = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (n == NULL) {
        ESP_LOGE(TAG, "netif WIFI_STA_DEF not found");
        return;
    }
    esp_netif_dhcpc_stop(n);    /* mmhalow's netif is a DHCP client; go static */

    esp_netif_ip_info_t ip = { 0 };
    ip.ip.addr = esp_ip4addr_aton(my_ip);
    ip.gw.addr = esp_ip4addr_aton(my_ip);
    ip.netmask.addr = esp_ip4addr_aton(NETMASK);
    ESP_ERROR_CHECK(esp_netif_set_ip_info(n, &ip));

    /* IBSS (like AP mode) doesn't fire a link-up event, so bring the netif up
     * explicitly or lwIP drops everything. */
    esp_netif_action_connected(n, NULL, 0, NULL);
    ESP_LOGI(TAG, "Static IP %s up; pinging peer %s (dynamic ARP)", my_ip, peer_ip);

    ip_addr_t target = { 0 };
    target.type = IPADDR_TYPE_V4;
    target.u_addr.ip4.addr = esp_ip4addr_aton(peer_ip);
    esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
    cfg.target_addr = target;
    cfg.count = ESP_PING_COUNT_INFINITE;
    cfg.interval_ms = 1000;
    esp_ping_callbacks_t cbs = {
        .on_ping_success = on_ping_success,
        .on_ping_timeout = on_ping_timeout,
    };
    esp_ping_handle_t ping;
    if (esp_ping_new_session(&cfg, &cbs, &ping) == ESP_OK) {
        esp_ping_start(ping);
    } else {
        ESP_LOGE(TAG, "failed to create ping session");
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_netif_init());

    /* mmhalow_init boots the chip + sets the channel list (no STA/AP interface),
     * and wires the netif TX/RX to mmwlan. */
    mmhalow_init(NULL);
    mmhalow_print_version_info();

    /* Pick role from our MAC so one binary serves both boards (bench heuristic). */
    uint8_t mac[6] = { 0 };
    mmwlan_get_mac_addr(mac);
    bool creator = ((mac[0] & 0x80) == 0);
    const char *my_ip = creator ? CREATOR_IP : JOINER_IP;
    const char *peer_ip = creator ? JOINER_IP : CREATOR_IP;
    ESP_LOGI(TAG, "MAC %02x:%02x:%02x:%02x:%02x:%02x -> role=%s ip=%s",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
             creator ? "CREATOR" : "JOINER", my_ip);

    struct mmwlan_ibss_args args = MMWLAN_IBSS_ARGS_INIT;
    memcpy(args.ssid, LINK_SSID, strlen(LINK_SSID));
    args.ssid_len = strlen(LINK_SSID);
    memcpy(args.bssid, LINK_BSSID, sizeof(args.bssid));
    args.op_class = LINK_OP_CLASS;
    args.s1g_chan_num = LINK_S1G_CHAN;
    args.creator = creator;
    args.start_beaconing = true;

    ESP_LOGI(TAG, "Calling mmwlan_ibss_enable() [%s]...", creator ? "CREATE" : "JOIN");
    enum mmwlan_status st = mmwlan_ibss_enable(&args);
    if (st != MMWLAN_SUCCESS) {
        ESP_LOGE(TAG, "==> mmwlan_ibss_enable FAILED status=%d", st);
        for (;;) { vTaskDelay(pdMS_TO_TICKS(5000)); }
    }
    ESP_LOGI(TAG, "==> IBSS up; bringing up IP + ping");

    vTaskDelay(pdMS_TO_TICKS(1500));
    start_static_ip_and_ping(my_ip, peer_ip);

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        ESP_LOGI(TAG, "alive (role=%s ip=%s)", creator ? "CREATOR" : "JOINER", my_ip);
    }
}
