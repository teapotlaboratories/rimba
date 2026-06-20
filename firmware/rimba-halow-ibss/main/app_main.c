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
 * ONE binary runs on every board. The IBSS create/join role is still chosen at
 * runtime from the MAC (a 2-board bench heuristic; real role/merge would use IBSS
 * TSF arbitration), but addressing is now N-node: each node derives its own IP
 * from its MAC (192.168.13.<octet(mac)>) and pings *every* discovered peer, so the
 * same binary scales to 3+ boards without IP collisions. The ping target set is
 * driven by the L2 peer table (mmwlan_ibss_get_peers) — a ping only happens
 * because the peer was discovered.
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
#define IP_PREFIX      "192.168.13."   /* N-node: host octet derived from the MAC */

/* IBSS BSSID — MUST be identical on every node. Locally-administered. */
static const uint8_t LINK_BSSID[6] = { 0x02, 0x12, 0x34, 0x56, 0x78, 0x9a };
/* ---------------------------------------------------------------------------- */

static const char *TAG = "rimba-ibss";

/* Derive the host octet from a MAC the same way on every node: octet = mac[5],
 * clamped off the network/broadcast addresses (0 -> 1, 255 -> 254). Identical
 * derivation everywhere (incl. the Linux node) makes the ping mesh symmetric. */
static void mac_to_ip(const uint8_t *mac, char *out, size_t outlen)
{
    unsigned octet = mac[5];
    if (octet == 0)   octet = 1;
    if (octet == 255) octet = 254;
    snprintf(out, outlen, IP_PREFIX "%u", octet);
}

/* Rimba L2 frame type. The Phase-1 gate is exchanging these raw, not just IP. */
#define RIMBA_ETHERTYPE 0x88b5

/* Broadcast one raw EtherType-0x88B5 L2 frame to the cell via mmwlan_tx(), which
 * takes a bare 802.3 frame: [DST(6)][SRC(6)][ethertype(2,BE)][payload]. Goes out
 * over the same IBSS data path as IP (plaintext for now). The peer surfaces it in
 * halow_rx ("RX 0x88B5 ..."); lwIP would otherwise drop the unknown EtherType. */
static void send_rimba_88b5(const uint8_t *src_mac, unsigned seq)
{
    uint8_t frame[64];
    memset(frame, 0xff, 6);                       /* DST = broadcast            */
    memcpy(frame + 6, src_mac, 6);                /* SRC = our MAC              */
    frame[12] = (RIMBA_ETHERTYPE >> 8) & 0xff;    /* EtherType 0x88B5 (BE)      */
    frame[13] = RIMBA_ETHERTYPE & 0xff;
    int n = snprintf((char *)(frame + 14), sizeof(frame) - 14, "RIMBA-88B5 seq=%u", seq);
    unsigned len = 14 + (n > 0 ? (unsigned)n : 0);

    enum mmwlan_status st = mmwlan_tx(frame, len);
    if (st != MMWLAN_SUCCESS) {
        ESP_LOGW(TAG, "TX 0x88B5 seq=%u failed status=%d", seq, st);
    } else {
        ESP_LOGI(TAG, "TX 0x88B5 broadcast seq=%u len=%u", seq, len);
    }
}

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

static void setup_static_ip(const char *my_ip)
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
    ESP_LOGI(TAG, "Static IP %s up (dynamic ARP)", my_ip);
}

/* Start an infinite 1 Hz ping session to one peer. One session per peer; the
 * callbacks log the per-target result, so concurrent sessions don't interleave
 * ambiguously. */
static void start_ping(const char *peer_ip)
{
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
        ESP_LOGI(TAG, "pinging peer %s", peer_ip);
    } else {
        ESP_LOGE(TAG, "failed to create ping session for %s", peer_ip);
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

    /* Pick the IBSS create/join role from our MAC so one binary serves every board
     * (bench heuristic). Addressing is independent: my IP is derived from my MAC. */
    uint8_t mac[6] = { 0 };
    mmwlan_get_mac_addr(mac);
    bool creator = ((mac[0] & 0x80) == 0);
    char my_ip[16];
    mac_to_ip(mac, my_ip, sizeof(my_ip));
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
    setup_static_ip(my_ip);

    /* Track which peer MACs we've already started a ping session for, so each new
     * peer gets pinged exactly once as it's discovered (and a power-cycled peer
     * that returns with the same MAC isn't double-pinged). */
    uint8_t pinged[8][6];
    unsigned npinged = 0;

    unsigned seq = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(3000));
        send_rimba_88b5(mac, seq++);   /* exercise the raw Rimba 0x88B5 path */

        /* Dump the per-peer station table (multi-node IBSS): each discovered peer
         * shows its MAC, assigned AID, and the firmware SET_STA_STATE result. Start
         * a ping to any peer we haven't pinged yet (target IP derived from its MAC,
         * same convention as ours), tying the L3 test to the L2 peer table. */
        struct mmwlan_ibss_peer_info peers[8];
        unsigned np = mmwlan_ibss_get_peers(peers, 8);
        ESP_LOGI(TAG, "IBSS peers: %u", np);
        for (unsigned i = 0; i < np && i < 8; i++) {
            const uint8_t *pmac = peers[i].mac_addr;
            ESP_LOGI(TAG, "  peer[%u] %02x:%02x:%02x:%02x:%02x:%02x aid=%u",
                     i, pmac[0], pmac[1], pmac[2], pmac[3], pmac[4], pmac[5],
                     peers[i].aid);

            bool seen = false;
            for (unsigned j = 0; j < npinged; j++) {
                if (memcmp(pinged[j], pmac, 6) == 0) { seen = true; break; }
            }
            if (!seen && npinged < 8) {
                char peer_ip[16];
                mac_to_ip(pmac, peer_ip, sizeof(peer_ip));
                start_ping(peer_ip);
                memcpy(pinged[npinged++], pmac, 6);
            }
        }
    }
}
