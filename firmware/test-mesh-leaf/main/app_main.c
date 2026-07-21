/*
 * test-mesh-leaf — mesh leaf / single-hop mode (mmwlan_mesh_set_multihop(false)).
 *
 * The SAME forced-line rig as test-mesh-relay (symmetric 3-node app, role self-selected by MAC):
 *
 *     origin(board0)  <-->  RELAY(board2)  <-->  dest(board1)
 *     allow=[relay]         allow=[origin,dest]   allow=[relay]
 *
 * The difference: the RELAY calls mmwlan_mesh_set_multihop(false) -- LEAF / single-hop mode. It keeps
 * its 1-hop plinks to both endpoints but must NOT forward origin<->dest (a shipped, on-air-A/B-
 * verified runtime opt-out, P6d). This test guards that opt-out: a port-forward could silently revert
 * it (re-enable relaying) or, worse, turn it into a black hole (drop the peering / go dark).
 *
 * The origin is the REPORTER, and its assertion is INVERTED from test-mesh-relay:
 *   - peering-survives: the origin's sole mesh peer is still the relay (leaf kept the 1-hop plink --
 *     it did NOT black-hole / crash);
 *   - forwarding-blocked: the origin gets 0 replies from the dest (the forced allowlist makes the
 *     relay the ONLY path, so a leaf relay declining to forward => no reply -- DETERMINISTIC, not
 *     RF-bound, so even one reply is a real leak).
 * PASS iff peering survives AND 0 replies. FAIL on any reply (relaying still on) or lost peering.
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
#include "mmwlan_stats.h"
#include "umac/mesh/umac_mesh.h"

#include "test_report.h"

#define NAME            "mesh-leaf"
#define RIG             "origin(board0)+RELAY(board2, multihop OFF)+dest(board1); forced line; origin reports"
#define MESH_ID         "rimba-mesh"
#define MESH_S1G_CHAN   27
#define PEER_WAIT_S     50
#define PING_COUNT      15
#define REPLY_FLOOR     8            /* >= this many of PING_COUNT => delivery OK (wide RF margin) */

/* Bench mesh MACs are BUILD-TIME arguments, not source constants (so swapping a bench board is a
 * manifest edit + rebuild, never a firmware edit). They arrive as CMake cache vars
 * TEST_MESH_ORIGIN/DEST/RELAY_MAC (the Makefile threads MESH_ORIGIN_MAC=/... into them; the T2
 * harness passes all three, derived from the manifest BENCH registry, to EVERY flash of this
 * symmetric app). Absent, these #ifndef defaults apply (the current board0/1/2 mesh MACs). */
#ifndef TEST_MESH_ORIGIN_MAC
#define TEST_MESH_ORIGIN_MAC "e2:72:a1:f8:ef:a4"   /* default: board0 -> 10.9.9.136 */
#endif
#ifndef TEST_MESH_DEST_MAC
#define TEST_MESH_DEST_MAC   "e2:72:a1:f8:f9:40"   /* default: board1 -> 10.9.9.100 */
#endif
#ifndef TEST_MESH_RELAY_MAC
#define TEST_MESH_RELAY_MAC  "e2:72:a1:f8:f0:08"   /* default: board2 -> 10.9.9.108 */
#endif

/* Origin=board0, dest=board1, RELAY=board2. Parsed from the build args at app_main start. */
static uint8_t MAC_ORIGIN[6], MAC_DEST[6], MAC_RELAY[6];
/* The dest's mesh IP is DERIVED from its MAC (10.9.9.<100+(mac[5]&0x3f)>, the bench convention this
 * app already uses for its own IP) -- so it is never a second thing to keep in sync. Filled early. */
static char DEST_IP[20];

static void parse_mac(const char *s, uint8_t out[6])
{
    unsigned v[6] = { 0 };
    if (sscanf(s, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) == 6) {
        for (int i = 0; i < 6; i++) out[i] = (uint8_t)v[i];
    }
}

static void mesh_ip_of(const uint8_t mac[6], char *buf, size_t n)
{
    snprintf(buf, n, "10.9.9.%u", 100u + (mac[5] & 0x3fu));
}

static uint8_t g_mesh_mac[6];
static volatile int s_replies = 0, s_timeouts = 0;
static volatile bool s_ping_done = false;

static void on_ping_success(esp_ping_handle_t hdl, void *args)
{
    uint16_t seqno; uint32_t ms;
    esp_ping_get_profile(hdl, ESP_PING_PROF_SEQNO, &seqno, sizeof(seqno));
    esp_ping_get_profile(hdl, ESP_PING_PROF_TIMEGAP, &ms, sizeof(ms));
    s_replies++;
    TEST_INFO("reply from %s seq=%u time=%" PRIu32 " ms (via relay)", DEST_IP, seqno, ms);
}
static void on_ping_timeout(esp_ping_handle_t hdl, void *args) { s_timeouts++; }
static void on_ping_end(esp_ping_handle_t hdl, void *args) { s_ping_done = true; }

static void park_forever(void) { while (1) { vTaskDelay(pdMS_TO_TICKS(10000)); } }

/* Pin the mesh netif's static IP (10.9.9.<100+(mac[5]&0x3f)>) + a static ARP for the dest so the
 * origin's ARP need not traverse the relay (deterministic). Mirrors rimba-halow-mesh-perf. */
static void setup_netif(bool origin)
{
    esp_netif_t *n = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (n == NULL) { TEST_INFO("mesh netif not found"); return; }
    for (int i = 0; i < 60 && !esp_netif_is_netif_up(n); i++) { vTaskDelay(pdMS_TO_TICKS(500)); }
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_netif_dhcpc_stop(n);
    esp_netif_set_mac(n, g_mesh_mac);
    char ipbuf[20];
    mesh_ip_of(g_mesh_mac, ipbuf, sizeof(ipbuf));
    esp_netif_ip_info_t ip = { 0 };
    ip.ip.addr = esp_ip4addr_aton(ipbuf);
    ip.gw.addr = esp_ip4addr_aton(DEST_IP);
    ip.netmask.addr = esp_ip4addr_aton("255.255.255.0");
    esp_netif_set_ip_info(n, &ip);
    TEST_INFO("mesh static IP %s (up=%d)", ipbuf, (int)esp_netif_is_netif_up(n));
    if (origin) {
        ip4_addr_t a = { .addr = esp_ip4addr_aton(DEST_IP) };
        struct eth_addr e;
        memcpy(e.addr, MAC_DEST, 6);
        if (etharp_add_static_entry(&a, &e) == ERR_OK) {
            TEST_INFO("static ARP %s -> dest MAC seeded", DEST_IP);
        }
    }
}

void app_main(void)
{
    vTaskDelay(pdMS_TO_TICKS(500));
    TEST_BEGIN(NAME, RIG);

    /* Resolve the line topology from the build args (defaults = board0/1/2). */
    parse_mac(TEST_MESH_ORIGIN_MAC, MAC_ORIGIN);
    parse_mac(TEST_MESH_DEST_MAC, MAC_DEST);
    parse_mac(TEST_MESH_RELAY_MAC, MAC_RELAY);
    mesh_ip_of(MAC_DEST, DEST_IP, sizeof(DEST_IP));

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_netif_init());

    esp_read_mac(g_mesh_mac, ESP_MAC_WIFI_STA);
    g_mesh_mac[0] = (g_mesh_mac[0] | 0x02) & 0xFE;
    bool is_origin = (memcmp(g_mesh_mac, MAC_ORIGIN, 6) == 0);
    bool is_relay  = (memcmp(g_mesh_mac, MAC_RELAY, 6) == 0);
    const char *role = is_origin ? "origin" : is_relay ? "RELAY" : "dest";

    mmhalow_init(NULL);
    mmhalow_print_version_info();

    struct mmwlan_mesh_args args = { 0 };
    memcpy(args.if_addr, g_mesh_mac, sizeof(g_mesh_mac));
    memcpy(args.mesh_id, MESH_ID, strlen(MESH_ID));
    args.mesh_id_len = strlen(MESH_ID);
    args.s1g_chan_num = MESH_S1G_CHAN;
    args.beacon_interval_tu = 100;
    args.max_plinks = 16;

    /* Forced-topology allowlist MUST be set BEFORE mmwlan_mesh_start (mesh_start beacons + RX
     * immediately; a late allowlist lets a direct plink form in the window and defeats the line). */
    if (is_relay) {
        uint8_t allow[12];
        memcpy(allow, MAC_ORIGIN, 6);
        memcpy(allow + 6, MAC_DEST, 6);
        mmwlan_mesh_set_peer_allowlist(allow, 2);      /* relay peers both endpoints, forwards */
    } else if (is_origin) {
        mmwlan_mesh_set_peer_allowlist(MAC_RELAY, 1);  /* origin peers ONLY the relay */
    } else {
        mmwlan_mesh_set_peer_allowlist(MAC_RELAY, 1);  /* dest peers ONLY the relay */
    }

    enum mmwlan_status st = mmwlan_mesh_start(&args);
    if (st != MMWLAN_SUCCESS) {
        TEST_FAIL("mmwlan_mesh_start (%s) failed status=%d (country not set / radio not booted?)",
                     role, (int)st);
        TEST_END(NAME);
        park_forever();
    }
    mmwlan_override_max_tx_power(1);

    /* Up-marker the orchestrator waits on for the SUPPORT roles (relay, dest). */
    TEST_INFO("mesh-up: role=%s as %02x:%02x:%02x:%02x:%02x:%02x on ch%d", role,
                 g_mesh_mac[0], g_mesh_mac[1], g_mesh_mac[2], g_mesh_mac[3], g_mesh_mac[4],
                 g_mesh_mac[5], MESH_S1G_CHAN);

    if (!is_origin) {
        /* Support roles (relay, dest). The RELAY disables multi-hop forwarding (leaf / single-hop
         * mode): it keeps its 1-hop plinks to both endpoints but must NOT relay origin<->dest. The
         * origin then proves forwarding is blocked while the peering survives. */
        if (is_relay) {
            mmwlan_mesh_set_multihop(false);
            TEST_INFO("LEAF MODE: relay multihop DISABLED (peers but will not forward)");
        }
        setup_netif(false);
        park_forever();
    }

    /* ===== origin = reporter ===== */
    setup_netif(true);

    /* Wait until the ONLY peer is the relay (topology proof). */
    uint8_t peer_macs[16][6];
    uint8_t n = 0;
    bool peer_is_relay = false;
    for (int s = 0; s < PEER_WAIT_S; s++) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        n = mmwlan_mesh_peer_count(peer_macs);
        peer_is_relay = (n == 1 && memcmp(peer_macs[0], MAC_RELAY, 6) == 0);
        if (peer_is_relay) break;
        if ((s % 5) == 4) TEST_INFO("waiting for relay plink... %ds, peers=%u", s + 1, n);
    }
    TEST_STEP("topology", peer_is_relay, "peers=%u sole_peer_is_relay=%d", n, (int)peer_is_relay);
    if (!peer_is_relay) {
        TEST_FAIL("origin's sole mesh peer is not the relay (peers=%u) -- the forced line did "
                     "not form, so a reply would not prove forwarding", n);
        TEST_END(NAME);
        park_forever();
    }

    /* Ping the dest through the relay. */
    ip_addr_t target = { 0 };
    target.type = IPADDR_TYPE_V4;
    target.u_addr.ip4.addr = esp_ip4addr_aton(DEST_IP);
    esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
    cfg.target_addr = target; cfg.count = PING_COUNT; cfg.interval_ms = 1000;
    esp_ping_callbacks_t cbs = { .on_ping_success = on_ping_success, .on_ping_timeout = on_ping_timeout,
                                 .on_ping_end = on_ping_end, .cb_args = NULL };
    esp_ping_handle_t ping;
    if (esp_ping_new_session(&cfg, &cbs, &ping) != ESP_OK) {
        TEST_FAIL("failed to create the ping session");
        TEST_END(NAME);
        park_forever();
    }
    TEST_INFO("pinging dest %s x%d via the relay...", DEST_IP, PING_COUNT);
    esp_ping_start(ping);
    int guard = 0;
    while (!s_ping_done && guard < (PING_COUNT + 12)) { vTaskDelay(pdMS_TO_TICKS(1000)); guard++; }

    /* Leaf mode: peering is already proven (the topology check above required the relay as our sole
     * peer, so leaf mode kept the 1-hop plink -- no black-hole). Success now = forwarding is BLOCKED:
     * 0 replies, because the forced allowlist makes the relay the ONLY path to the dest and a leaf
     * relay must decline to forward. ANY reply means the relay forwarded -> leaf/single-hop mode is
     * broken (relaying still enabled). This is DETERMINISTIC (no path => no reply), not RF-bound, so
     * a single reply is a real leak, not noise. */
    TEST_STEP("peering-survives", peer_is_relay, "sole peer is the relay in leaf mode (no black-hole)");
    TEST_STEP("forwarding-blocked", s_replies == 0, "replies=%d/%d (expect 0 with multihop OFF)",
                 s_replies, PING_COUNT);

    if (s_replies == 0) {
        TEST_PASS("leaf mode correct: the relay stayed 1-hop peered (sole peer) but forwarded nothing "
                     "(0/%d replies over the forced line) -- neither relays nor black-holes", PING_COUNT);
    } else {
        TEST_FAIL("the relay forwarded %d/%d replies with mmwlan_mesh_set_multihop(false) -- leaf / "
                     "single-hop mode is BROKEN (relaying is still enabled)", s_replies, PING_COUNT);
    }
    TEST_END(NAME);
    park_forever();
}
