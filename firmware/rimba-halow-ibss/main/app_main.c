/*
 * rimba-halow-ibss — RISK-01 IBSS / ad-hoc bring-up + data test.
 *
 * Uses the IBSS implementation adopted from the momentary-systems esp-halow-ibss
 * fork (mmwlan_ibss_start + the umac_ibss module): teardown-first bring-up that
 * mirrors the Linux flow (REMOVE_INTERFACE before ADD_INTERFACE(ADHOC)), so
 * IBSS_CONFIG(CREATE) returns 0 (no EEXIST), plus peer age-out and membership cb.
 * Our S1G-beacon source_addr fix (#16) is layered on top for Linux interop.
 *
 * ONE binary runs on every board: the create/join role is a MAC heuristic (bench
 * heuristic; real role/merge is backlog #4). Addressing is N-node: each node
 * derives its IP from its MAC (192.168.13.<octet(mac)>) and pings every discovered
 * peer, so the same binary scales to 3+ boards + the Linux node.
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
#include "umac/ibss/umac_ibss.h"

/* --- IBSS link parameters (identical on every node in the cell) -------------- */
#define LINK_SSID      "rimba-ibss"
#define LINK_S1G_CHAN  27             /* US 915.5 MHz, 1 MHz BW (global op-class 68) */
#define NETMASK        "255.255.255.0"
#define IP_PREFIX      "192.168.13."   /* N-node: host octet derived from the MAC */

static const uint8_t LINK_BSSID[6] = { 0x02, 0x12, 0x34, 0x56, 0x78, 0x9a };
static const char *TAG = "rimba-ibss";

static void mac_to_ip(const uint8_t *mac, char *out, size_t outlen)
{
    unsigned octet = mac[5];
    if (octet == 0)   octet = 1;
    if (octet == 255) octet = 254;
    snprintf(out, outlen, IP_PREFIX "%u", octet);
}

#define RIMBA_ETHERTYPE 0x88b5
static void send_rimba_88b5(const uint8_t *src_mac, unsigned seq)
{
    uint8_t frame[64];
    memset(frame, 0xff, 6);
    memcpy(frame + 6, src_mac, 6);
    frame[12] = (RIMBA_ETHERTYPE >> 8) & 0xff;
    frame[13] = RIMBA_ETHERTYPE & 0xff;
    int n = snprintf((char *)(frame + 14), sizeof(frame) - 14, "RIMBA-88B5 seq=%u", seq);
    unsigned len = 14 + (n > 0 ? (unsigned)n : 0);
    if (mmwlan_tx(frame, len) != MMWLAN_SUCCESS) {
        ESP_LOGW(TAG, "TX 0x88B5 seq=%u failed", seq);
    }
}

static void on_ping_success(esp_ping_handle_t hdl, void *args)
{
    uint16_t seqno; uint32_t elapsed_ms; ip_addr_t target = { 0 };
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
    ESP_LOGW(TAG, "ping timeout seq=%u", seqno);
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
    ESP_LOGI(TAG, "Static IP %s up (dynamic ARP)", my_ip);
}

static void start_ping(const char *peer_ip)
{
    ip_addr_t target = { 0 };
    target.type = IPADDR_TYPE_V4;
    target.u_addr.ip4.addr = esp_ip4addr_aton(peer_ip);
    esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
    cfg.target_addr = target;
    cfg.count = ESP_PING_COUNT_INFINITE;
    cfg.interval_ms = 1000;
    esp_ping_callbacks_t cbs = { .on_ping_success = on_ping_success, .on_ping_timeout = on_ping_timeout };
    esp_ping_handle_t ping;
    if (esp_ping_new_session(&cfg, &cbs, &ping) == ESP_OK) {
        esp_ping_start(ping);
        ESP_LOGI(TAG, "pinging peer %s", peer_ip);
    }
}

/* Peers we've already started a ping session for (each pinged exactly once). */
static uint8_t s_pinged[8][6];
static unsigned s_npinged;

/* Membership callback (fires on PEER_ADDED/REMOVED from the umac_ibss module). It
 * runs on the RX context, so keep it trivial: just record the peer MAC; the main
 * loop starts the ping session. */
static uint8_t s_pending[8][6];
static volatile unsigned s_npending;
static void peer_cb(const uint8_t *mac, enum mmwlan_ibss_peer_event ev, void *arg)
{
    if (ev != MMWLAN_IBSS_PEER_ADDED) return;
    if (s_npending < 8) { memcpy(s_pending[s_npending++], mac, 6); }
}

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_netif_init());

    mmhalow_init(NULL);
    mmhalow_print_version_info();

    uint8_t mac[6] = { 0 };
    mmwlan_get_mac_addr(mac);
    bool creator = ((mac[0] & 0x80) == 0);
    char my_ip[16];
    mac_to_ip(mac, my_ip, sizeof(my_ip));
    ESP_LOGI(TAG, "MAC %02x:%02x:%02x:%02x:%02x:%02x -> role=%s ip=%s",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
             creator ? "CREATOR" : "JOINER", my_ip);

    mmwlan_ibss_register_peer_cb(peer_cb, NULL);

    struct mmwlan_ibss_args args = { 0 };
    memcpy(args.bssid, LINK_BSSID, sizeof(args.bssid));
    memcpy(args.ssid, LINK_SSID, strlen(LINK_SSID));
    args.ssid_len = strlen(LINK_SSID);
    args.create = creator;
    args.s1g_chan_num = LINK_S1G_CHAN;
    args.beacon_interval_tu = 100;
    /* Set if_addr to our own MAC so our beacons carry source_addr = our MAC (a real
     * IBSS beacon, Linux-style). Peers then discover us from our beacon (#16). */
    memcpy(args.if_addr, mac, sizeof(args.if_addr));

    ESP_LOGI(TAG, "Calling mmwlan_ibss_start() [%s]...", creator ? "CREATE" : "JOIN");
    enum mmwlan_status st = mmwlan_ibss_start(&args);
    if (st != MMWLAN_SUCCESS) {
        ESP_LOGE(TAG, "==> mmwlan_ibss_start FAILED status=%d", st);
        for (;;) { vTaskDelay(pdMS_TO_TICKS(5000)); }
    }
    ESP_LOGI(TAG, "==> IBSS up; bringing up IP + ping");

    vTaskDelay(pdMS_TO_TICKS(1500));
    setup_static_ip(my_ip);

    unsigned seq = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(3000));
        send_rimba_88b5(mac, seq++);

        /* Start a ping for any newly-discovered peer (collected by peer_cb). */
        unsigned np = s_npending;
        for (unsigned i = 0; i < np && i < 8; i++) {
            bool seen = false;
            for (unsigned j = 0; j < s_npinged; j++) {
                if (memcmp(s_pinged[j], s_pending[i], 6) == 0) { seen = true; break; }
            }
            if (!seen && s_npinged < 8) {
                char peer_ip[16];
                mac_to_ip(s_pending[i], peer_ip, sizeof(peer_ip));
                ESP_LOGI(TAG, "peer %02x:%02x:%02x:%02x:%02x:%02x -> %s",
                         s_pending[i][0], s_pending[i][1], s_pending[i][2],
                         s_pending[i][3], s_pending[i][4], s_pending[i][5], peer_ip);
                start_ping(peer_ip);
                memcpy(s_pinged[s_npinged++], s_pending[i], 6);
            }
        }
        s_npending = 0;

        /* Age out peers idle > 30 s (caller-driven, per the umac_ibss API). */
        mmwlan_ibss_age_peers(30000);
    }
}
