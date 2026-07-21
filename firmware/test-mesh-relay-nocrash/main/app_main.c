/*
 * test-mesh-relay-nocrash — relay stability under sustained forwarding (no hw-restart).
 *
 * The SAME forced-line rig as test-mesh-relay, but the ROLES are re-cast so the RELAY is the reporter
 * (only a reporter's console is scraped, and hw_restart_counter is the relay's OWN stat):
 *
 *     origin(board0)          RELAY(board2)               dest(board1)
 *     SUPPORT: drives pings    REPORTER: forwards, then    SUPPORT: responder
 *     through the relay        checks hw_restart_counter
 *
 * The origin + dest are SUPPORT roles (they boot first); the origin drives a long ping burst so the
 * relay is under sustained forwarding load. The RELAY is flashed LAST (the reporter): it forwards,
 * then asserts its hw_restart_counter did NOT increment over the window -- i.e. the SPI-host-teardown
 * interrupt-WDT crash (root-caused in the mesh-relay-intwdt worklog) did not silently fire. This
 * catches a crash-AND-recover that a delivery check alone can MISS.
 *
 * Assertion (relay/reporter): forwarding-path (BOTH endpoints peered, so there was real load -- else
 * INCONCLUSIVE, never a false PASS) AND no-hw-restart (hw_restart_counter unchanged across the
 * window). PASS on both; FAIL if the counter climbed (the relay hw-restarted under load).
 *
 * THE RELAY MUST BE board2: the interrupt-WDT crash was only ever seen on a relay whose WAKE+BUSY are
 * wired (board2); board0/board1 as relay produce a DIFFERENT false crash (the missing-solder trap).
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

#define NAME            "mesh-relay-nocrash"
#define RIG             "origin(board0)+RELAY(board2)+dest(board1); forced line; RELAY reports (no hw-restart)"
#define MESH_ID         "rimba-mesh"
#define MESH_S1G_CHAN   27
/* Sizing note: the relay is the reporter, and REPORTER_TIMEOUT_S is 130 s from its flash to TEST|END.
 * boot (~25 s) + RELAY_PEER_WAIT_S + FORWARD_WINDOW_S must stay under that. */
#define RELAY_PEER_WAIT_S   45       /* relay waits for BOTH endpoints (already up) before measuring */
#define FORWARD_WINDOW_S    40       /* relay samples hw_restart_counter across this forwarding window */
#define ORIGIN_PEER_WAIT_S  150      /* origin waits for the relay (flashed LAST, ~60-90 s after us) */
#define ORIGIN_PING_COUNT   90       /* origin drives pings ~90 s to load the relay across its window */

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
    bool is_dest   = (memcmp(g_mesh_mac, MAC_DEST, 6) == 0);
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

    /* Up-marker the orchestrator waits on for the SUPPORT roles (origin, dest). */
    TEST_INFO("mesh-up: role=%s as %02x:%02x:%02x:%02x:%02x:%02x on ch%d", role,
                 g_mesh_mac[0], g_mesh_mac[1], g_mesh_mac[2], g_mesh_mac[3], g_mesh_mac[4],
                 g_mesh_mac[5], MESH_S1G_CHAN);

    if (is_relay) {
        /* ===== RELAY = the reporter (nocrash) ===== */
        setup_netif(false);   /* the relay forwards; it neither originates nor answers the pings */

        /* Both endpoints must peer before the check means anything: a relay under NO load cannot
         * crash-under-load, so no-peers => INCONCLUSIVE, never a false PASS. */
        uint8_t peer_macs[16][6];
        uint8_t np = 0;
        bool both_peered = false;
        for (int s = 0; s < RELAY_PEER_WAIT_S; s++) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            np = mmwlan_mesh_peer_count(peer_macs);
            both_peered = (np >= 2);
            if (both_peered) break;
            if ((s % 10) == 9) TEST_INFO("relay waiting for both endpoints... %ds, peers=%u", s + 1, np);
        }
        TEST_STEP("forwarding-path", both_peered, "peers=%u (need origin AND dest)", np);

        struct mmwlan_stats_umac_data st0 = { 0 };
        (void)mmwlan_get_umac_stats(&st0);
        uint16_t restarts_before = st0.hw_restart_counter;

        /* Forwarding window: the origin drives pings through us for FORWARD_WINDOW_S. A relay that
         * hw-restarts under this load (the SPI-host-teardown interrupt-WDT crash) increments the
         * counter -- which a delivery check alone can MISS if the relay crash-and-recovers inside a
         * ping window. */
        TEST_INFO("relay forwarding window: %ds (origin driving traffic)...", FORWARD_WINDOW_S);
        for (int s = 0; s < FORWARD_WINDOW_S; s++) { vTaskDelay(pdMS_TO_TICKS(1000)); }

        struct mmwlan_stats_umac_data st = { 0 };
        (void)mmwlan_get_umac_stats(&st);
        uint16_t restarts = st.hw_restart_counter;
        bool no_crash = (restarts == restarts_before);
        TEST_STEP("no-hw-restart", no_crash, "hw_restart_counter=%u (was %u at window start)",
                     restarts, restarts_before);

        if (!both_peered) {
            TEST_INCONCLUSIVE("the relay never peered both endpoints (peers=%u) -- no forwarding "
                                 "load, so a crash-under-load cannot be judged (bench, not a code "
                                 "regression)", np);
        } else if (no_crash) {
            TEST_PASS("relay forwarded for %ds with NO hw-restart (hw_restart_counter stayed %u) -- "
                         "the interrupt-WDT SPI-host-teardown crash did not fire under load",
                         FORWARD_WINDOW_S, restarts);
        } else {
            TEST_FAIL("relay hw_restart_counter climbed %u->%u during forwarding -- the relay "
                         "silently hw-restarted under load (interrupt-WDT SPI-host-teardown crash)",
                         restarts_before, restarts);
        }
        TEST_END(NAME);
        park_forever();
    }

    if (is_dest) {
        /* ===== DEST = support responder ===== */
        setup_netif(false);
        park_forever();
    }

    /* ===== ORIGIN = support that DRIVES the forwarding load (does NOT report) ===== */
    setup_netif(true);

    /* The relay is flashed LAST (it is the reporter), so wait for its plink, then fire a long ping
     * burst to keep the relay under sustained forwarding load across its measurement window. The
     * origin does not self-verify -- the relay is the reporter. */
    uint8_t peer_macs[16][6];
    for (int s = 0; s < ORIGIN_PEER_WAIT_S; s++) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        uint8_t n = mmwlan_mesh_peer_count(peer_macs);
        if (n == 1 && memcmp(peer_macs[0], MAC_RELAY, 6) == 0) break;
        if ((s % 10) == 9) TEST_INFO("origin waiting for relay plink... %ds, peers=%u", s + 1, n);
    }
    ip_addr_t target = { 0 };
    target.type = IPADDR_TYPE_V4;
    target.u_addr.ip4.addr = esp_ip4addr_aton(DEST_IP);
    esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
    cfg.target_addr = target; cfg.count = ORIGIN_PING_COUNT; cfg.interval_ms = 1000;
    esp_ping_callbacks_t cbs = { .on_ping_success = on_ping_success, .on_ping_timeout = on_ping_timeout,
                                 .on_ping_end = on_ping_end, .cb_args = NULL };
    esp_ping_handle_t ping;
    if (esp_ping_new_session(&cfg, &cbs, &ping) == ESP_OK) {
        TEST_INFO("origin driving %dx pings to load the relay...", ORIGIN_PING_COUNT);
        esp_ping_start(ping);
    }
    park_forever();
}
