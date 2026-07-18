/*
 * test-mesh-linux — ESP <-> LINUX 802.11s mesh interoperability (the gold standard).
 *
 * The ESP joins a real Linux HaLow mesh (mac80211 + morse_driver + wpa_supplicant_s1g on chronite),
 * peers with it (MPM + SAE + AMPE), and pings it -- proving the ESP interoperates with a genuine
 * Linux stack, not just another ESP running the same morselib. This is the reporter role; the Linux
 * node is a support role the orchestrator brings up over ssh (tools/regtest/linux_peer.py). All ESP
 * apps a T2 test flashes are test-*.
 *
 * Mesh params MUST match the Linux reference config (docs/reference/captures/wpa-smesh.conf):
 *   mesh ID "rimba-smesh" (NOT the ESP-internal "rimba-mesh"), SAE password "rimbamesh2026"
 *   (compiled into morselib), S1G ch27. Only the mesh ID differs from the ESP<->ESP tests.
 *
 * WHAT IT PROVES: an ESP completes SAE+AMPE mesh peering against a Linux node AND exchanges data
 * (ICMP) with it -- byte-level interop of the peering handshake + the secured data path.
 * WHAT IT DOES NOT PROVE: multi-hop relay through a Linux node, or on-air frame equivalence.
 *
 * Assertion: an ESTAB peer whose MAC == the Linux node's wlan1 MAC appears (peering interop), AND
 * ICMP replies come back from the Linux node's mesh IP (data interop). Peering is binary/structural;
 * the reply count is RF-bound (wide floor + INCONCLUSIVE band).
 */

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "lwip/etharp.h"
#include "ping/ping_sock.h"

#include "mmhalow.h"
#include "mmwlan.h"
#include "umac/mesh/umac_mesh.h"

#include "test_report.h"

#define NAME            "mesh-linux"
#define RIG             "ESP mesh node (reporter) + a Linux mesh node (chronite) brought up over ssh"
#define MESH_ID         "rimba-smesh"       /* MUST match the Linux wpa-smesh.conf */
#define MESH_S1G_CHAN   27
#define PEER_WAIT_S     50
#define PING_COUNT      15
#define REPLY_FLOOR     6

/* The Linux peer's identity is a BUILD-TIME ARGUMENT -- the ESP needs the peer's MAC + IP at compile
 * time to recognise the specific peer and ping it, but they are NOT baked into this source. They come
 * from the compile defines TEST_LINUX_MAC / TEST_LINUX_IP, which the harness passes from the
 * manifest LINUX_NODES registry (or a `--linux-mac` / `--linux-ip` override). By hand:
 *     make flash APP=test-mesh-linux BOARD=proto1-fgh100m \
 *          LINUX_MAC=3c:22:7f:37:51:38 LINUX_IP=10.9.9.2 PORT=...
 * The defaults below (chronite) only apply to a bare build with no argument (e.g. the T0 matrix). */
#ifndef TEST_LINUX_MAC
#define TEST_LINUX_MAC "3c:22:7f:37:51:38"   /* default: chronite wlan1 */
#endif
#ifndef TEST_LINUX_IP
#define TEST_LINUX_IP  "10.9.9.2"            /* default: chronite mesh IP */
#endif
#define LINUX_IP  TEST_LINUX_IP

/* Parsed from TEST_LINUX_MAC at boot (see app_main). */
static uint8_t LINUX_MAC[6];

static void parse_mac(const char *s, uint8_t out[6])
{
    unsigned v[6] = { 0 };
    if (sscanf(s, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) == 6) {
        for (int i = 0; i < 6; i++) out[i] = (uint8_t)v[i];
    }
}

/* 802.11 element IDs + beacon IE offset (24-byte PV0 header + ts/bcn-int/cap = 12). */
#define DOT11_IE_MESH_ID  114
#define BEACON_IE_OFFSET  (24 + 12)

static uint8_t g_mesh_mac[6];
static volatile int s_replies = 0, s_timeouts = 0;
static volatile bool s_ping_done = false;

enum mmwlan_frame_filter_flag { MMWLAN_FRAME_BEACON = 1 << 8 };
struct mmwlan_rx_frame_info {
    enum mmwlan_frame_filter_flag frame_filter_flag;
    const uint8_t *buf; uint32_t buf_len; uint16_t freq_100khz; int16_t rssi_dbm; uint8_t bw_mhz;
};
typedef void (*mmwlan_rx_frame_cb_t)(const struct mmwlan_rx_frame_info *rx_info, void *arg);
extern enum mmwlan_status mmwlan_register_rx_frame_cb(uint32_t filter,
                                                      mmwlan_rx_frame_cb_t callback, void *arg);

static const uint8_t *find_ie(const uint8_t *ies, uint32_t len, uint8_t want, uint8_t *out_len)
{
    uint32_t i = 0;
    while (i + 2 <= len) {
        uint8_t id = ies[i], ie_len = ies[i + 1];
        if (i + 2 + ie_len > len) break;
        if (id == want) { *out_len = ie_len; return &ies[i + 2]; }
        i += 2 + ie_len;
    }
    return NULL;
}

static void peer_beacon_cb(const struct mmwlan_rx_frame_info *info, void *arg)
{
    (void)arg;
    if (info->buf_len <= BEACON_IE_OFFSET) return;
    const uint8_t *sa = info->buf + 10;
    if (memcmp(sa, g_mesh_mac, 6) == 0) return;
    uint8_t id_len = 0;
    const uint8_t *id = find_ie(info->buf + BEACON_IE_OFFSET, info->buf_len - BEACON_IE_OFFSET,
                                DOT11_IE_MESH_ID, &id_len);
    if (!id) return;
    if (id_len == strlen(MESH_ID) && memcmp(id, MESH_ID, id_len) == 0) {
        mmwlan_mesh_peer_open(sa);
    }
}

static void on_ping_success(esp_ping_handle_t hdl, void *args)
{
    uint16_t seqno; uint32_t ms;
    esp_ping_get_profile(hdl, ESP_PING_PROF_SEQNO, &seqno, sizeof(seqno));
    esp_ping_get_profile(hdl, ESP_PING_PROF_TIMEGAP, &ms, sizeof(ms));
    s_replies++;
    TEST_INFO("reply from Linux %s seq=%u time=%" PRIu32 " ms", LINUX_IP, seqno, ms);
}
static void on_ping_timeout(esp_ping_handle_t hdl, void *args) { s_timeouts++; }
static void on_ping_end(esp_ping_handle_t hdl, void *args) { s_ping_done = true; }

static void park_forever(void) { while (1) { vTaskDelay(pdMS_TO_TICKS(10000)); } }

static int has_linux_peer(uint8_t macs[][6], uint8_t n)
{
    for (uint8_t i = 0; i < n; i++)
        if (memcmp(macs[i], LINUX_MAC, 6) == 0) return 1;
    return 0;
}

void app_main(void)
{
    parse_mac(TEST_LINUX_MAC, LINUX_MAC);   /* the peer MAC is a build-time argument */

    vTaskDelay(pdMS_TO_TICKS(500));
    TEST_BEGIN(NAME, RIG);
    TEST_INFO("joining Linux mesh \"%s\" (SAE), peer = Linux %02x:%02x:%02x:%02x:%02x:%02x @ %s "
                 "(from TEST_LINUX_MAC=%s)", MESH_ID, LINUX_MAC[0], LINUX_MAC[1], LINUX_MAC[2],
                 LINUX_MAC[3], LINUX_MAC[4], LINUX_MAC[5], LINUX_IP, TEST_LINUX_MAC);

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_netif_init());

    esp_read_mac(g_mesh_mac, ESP_MAC_WIFI_STA);
    g_mesh_mac[0] = (g_mesh_mac[0] | 0x02) & 0xFE;

    mmhalow_init(NULL);
    mmhalow_print_version_info();

    struct mmwlan_mesh_args args = { 0 };
    memcpy(args.if_addr, g_mesh_mac, sizeof(g_mesh_mac));
    memcpy(args.mesh_id, MESH_ID, strlen(MESH_ID));
    args.mesh_id_len = strlen(MESH_ID);
    args.s1g_chan_num = MESH_S1G_CHAN;
    args.beacon_interval_tu = 100;
    args.max_plinks = 16;

    enum mmwlan_status st = mmwlan_mesh_start(&args);
    if (st != MMWLAN_SUCCESS) {
        TEST_FAIL("mmwlan_mesh_start failed status=%d (country not set / radio not booted?)",
                     (int)st);
        TEST_END(NAME);
        park_forever();
    }
    mmwlan_override_max_tx_power(1);
    mmwlan_register_rx_frame_cb(MMWLAN_FRAME_BEACON, peer_beacon_cb, NULL);

    /* Pin the mesh netif IP so we can ping the Linux node, + a static ARP for it. */
    esp_netif_t *n = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (n) {
        for (int i = 0; i < 30 && !esp_netif_is_netif_up(n); i++) vTaskDelay(pdMS_TO_TICKS(500));
        esp_netif_dhcpc_stop(n);
        esp_netif_set_mac(n, g_mesh_mac);
        unsigned host = 100u + (g_mesh_mac[5] & 0x3fu);
        char ipbuf[20];
        snprintf(ipbuf, sizeof(ipbuf), "10.9.9.%u", host);
        esp_netif_ip_info_t ip = { 0 };
        ip.ip.addr = esp_ip4addr_aton(ipbuf);
        ip.gw.addr = esp_ip4addr_aton(LINUX_IP);
        ip.netmask.addr = esp_ip4addr_aton("255.255.255.0");
        esp_netif_set_ip_info(n, &ip);
        ip4_addr_t a = { .addr = esp_ip4addr_aton(LINUX_IP) };
        struct eth_addr e; memcpy(e.addr, LINUX_MAC, 6);
        etharp_add_static_entry(&a, &e);
        TEST_INFO("mesh IP %s, static ARP for the Linux node seeded", ipbuf);
    }

    /* Wait for an ESTAB peer that IS the Linux node (peering interop). */
    uint8_t peer_macs[16][6];
    uint8_t np = 0;
    bool linux_peered = false;
    for (int s = 0; s < PEER_WAIT_S; s++) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        np = mmwlan_mesh_peer_count(peer_macs);
        linux_peered = has_linux_peer(peer_macs, np);
        if (linux_peered) break;
        if ((s % 5) == 4) TEST_INFO("waiting for the Linux plink... %ds, peers=%u", s + 1, np);
    }
    TEST_STEP("peer-linux", linux_peered, "peers=%u linux_estab=%d", np, (int)linux_peered);
    if (!linux_peered) {
        TEST_FAIL("no ESTAB peer matching the Linux node MAC within %ds (peers=%u) -- SAE+AMPE "
                     "peering with the Linux stack did not complete", PEER_WAIT_S, np);
        TEST_END(NAME);
        park_forever();
    }

    /* Ping the Linux node (data interop). */
    ip_addr_t target = { 0 };
    target.type = IPADDR_TYPE_V4;
    target.u_addr.ip4.addr = esp_ip4addr_aton(LINUX_IP);
    esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
    cfg.target_addr = target; cfg.count = PING_COUNT; cfg.interval_ms = 1000;
    esp_ping_callbacks_t cbs = { .on_ping_success = on_ping_success, .on_ping_timeout = on_ping_timeout,
                                 .on_ping_end = on_ping_end, .cb_args = NULL };
    esp_ping_handle_t ping;
    if (esp_ping_new_session(&cfg, &cbs, &ping) != ESP_OK) {
        TEST_FAIL("peered with Linux but failed to create the ping session");
        TEST_END(NAME);
        park_forever();
    }
    TEST_INFO("pinging the Linux node %s x%d...", LINUX_IP, PING_COUNT);
    esp_ping_start(ping);
    int guard = 0;
    while (!s_ping_done && guard < (PING_COUNT + 12)) { vTaskDelay(pdMS_TO_TICKS(1000)); guard++; }

    TEST_STEP("data", s_replies >= REPLY_FLOOR, "replies=%d/%d timeouts=%d",
                 s_replies, PING_COUNT, s_timeouts);
    if (s_replies >= REPLY_FLOOR) {
        TEST_PASS("ESP<->Linux mesh interop: peered (SAE+AMPE) with the Linux node + %d/%d ICMP "
                     "replies from %s", s_replies, PING_COUNT, LINUX_IP);
    } else if (s_replies > 0) {
        TEST_INCONCLUSIVE("peered with Linux but only %d/%d replies (floor %d) -- marginal RF, "
                             "not a clear interop regression", s_replies, PING_COUNT, REPLY_FLOOR);
    } else {
        TEST_FAIL("peered (SAE+AMPE) with the Linux node but 0/%d ICMP replies -- the secured "
                     "data path to the Linux stack did not carry ICMP", PING_COUNT);
    }
    TEST_END(NAME);
    park_forever();
}
