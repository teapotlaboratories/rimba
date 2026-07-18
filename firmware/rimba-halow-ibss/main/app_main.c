/*
 * rimba-halow-ibss — IBSS / ad-hoc HaLow bring-up + peer data path.
 *
 * Brings up an IBSS (ad-hoc) cell with mmwlan_ibss_start() + the umac_ibss
 * module: teardown-first bring-up that mirrors the Linux flow (REMOVE_INTERFACE
 * before ADD_INTERFACE(ADHOC)) so IBSS_CONFIG(CREATE) returns 0 (no EEXIST),
 * plus peer age-out and a membership callback. Beacons carry source_addr = our
 * own MAC (a real IBSS beacon, Linux-style) so peers discover us from the beacon
 * and Linux HaLow nodes interoperate.
 *
 * ONE binary runs on every node. Addressing is N-node: each node derives its IP
 * from its MAC (192.168.13.<octet(mac)>) and pings every discovered peer, so the
 * same binary scales to as many boards as you flash it to (plus a Linux node).
 *
 * Design note — provisioned network, agreed BSSID, NO TSF merge:
 * Every node is deployed knowing the cell's BSSID (LINK_BSSID below — provision
 * this per deployment, don't ship the literal). IBSS TSF merge (see the Linux
 * net/mac80211/ibss.c ieee80211_rx_bss_info) exists to let *uncoordinated*
 * ad-hoc nodes that each rolled a random BSSID converge to one cell; with a
 * pre-shared BSSID there is only ever one cell, so merge is out of scope.
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

/* The agreed cell BSSID — same on every node (provisioned; no TSF merge — see the
 * design note above). Locally-administered (0x02). In production this comes from
 * provisioning, not a literal. */
static const uint8_t LINK_BSSID[6] = { 0x02, 0x12, 0x34, 0x56, 0x78, 0x9a };
static const char *TAG = "rimba-ibss";

/* Creator/joiner role selection. With a provisioned, pre-shared BSSID there is
 * only ever one cell, so this only decides who issues cfg_ibss CREATE first; the
 * rest simply JOIN. We pick by a MAC bit (mac[0] & CREATOR_MAC_BIT clear ->
 * CREATOR) so one flat binary self-assigns roles with no per-node config. If you
 * provision roles yourself, set `creator` from your own policy instead. */
#define CREATOR_MAC_BIT 0x80

static void mac_to_ip(const uint8_t *mac, char *out, size_t outlen)
{
    unsigned octet = mac[5];
    if (octet == 0)   octet = 1;
    if (octet == 255) octet = 254;
    snprintf(out, outlen, IP_PREFIX "%u", octet);
}

static void on_ping_success(esp_ping_handle_t hdl, void *args)
{
    uint16_t seqno; uint32_t elapsed_ms; ip_addr_t target = { 0 };
    esp_ping_get_profile(hdl, ESP_PING_PROF_SEQNO, &seqno, sizeof(seqno));
    esp_ping_get_profile(hdl, ESP_PING_PROF_TIMEGAP, &elapsed_ms, sizeof(elapsed_ms));
    esp_ping_get_profile(hdl, ESP_PING_PROF_IPADDR, &target, sizeof(target));
    ESP_LOGI(TAG, "reply from " IPSTR ": seq=%u time=%" PRIu32 " ms",
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
    /* Surface membership churn. The umac_ibss layer's own "IBSS new peer"/"aging
     * out" logs are MMLOG_INF, which is compiled out at morselib's default ERR
     * level — so log it app-side here. */
    ESP_LOGI(TAG, "peer_cb %s %02x:%02x:%02x:%02x:%02x:%02x",
             ev == MMWLAN_IBSS_PEER_ADDED ? "ADDED" : "REMOVED",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
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
    bool creator = ((mac[0] & CREATOR_MAC_BIT) == 0);
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
     * IBSS beacon, Linux-style). Peers then discover us from our beacon. */
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

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(3000));

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
