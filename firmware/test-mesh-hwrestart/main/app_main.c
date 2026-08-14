/*
 * test-mesh-hwrestart — can a mesh node survive a chip restart? (S1 of the hw_restart work)
 *
 * A mesh node cannot survive any hw_restart today. This fixture makes that REPRODUCIBLE rather than
 * something you wait for: it peers, forces a restart on demand, and lets you observe whether the mesh
 * comes back ON AIR.
 *
 * WHY IT HAS TO EXIST FIRST. The defect was found in a crash on 2026-07-15, and there is no way to
 * trigger a chip restart deliberately. Recovery code you can only exercise by waiting for a fault is
 * code you cannot verify -- so the reproducer is S1 and the two fixes (mesh recovery, the AP assert)
 * are S2/S3. Mechanism traced in docs/mesh-ap/rimba-mesh-hw-restart-design.md.
 *
 * ============================================================================================
 * THE VERDICT THIS APP EMITS IS *NOT* THE RECOVERY VERDICT. READ THIS BEFORE CHANGING ANYTHING.
 * ============================================================================================
 *
 * The first version of this fixture scored recovery on mmwlan_mesh_peer_count(), and it returned PASS
 * on a node that was stone deaf. mmwlan_mesh_peer_count() (umac_mesh.c:1404) walks mesh_peers[], a
 * HOST-SIDE array that mmdrv_deinit()/mmdrv_init() never touches: it reads 1 whether the node is
 * meshing or transmitting nothing at all. Measured 2026-08-05 -- peer_count held at 1 for the full 45 s
 * while the monitor saw the node emit ZERO frames. A fixture that certifies a broken node as healthy is
 * strictly worse than no fixture, so NO on-device probe scores recovery here, on purpose.
 *
 * The trap is structural, not an oversight: "silently deaf" is by definition the state in which every
 * host-side view still looks healthy. Any replacement metric that reads RAM on this board will fall
 * into it again.
 *
 * So the split is:
 *   - THIS APP asserts what the device can genuinely know: that the trigger really did cause a chip
 *     restart (umac_stats' hw_restart_counter, bumped by hw_restart_evt_handler() itself only AFTER
 *     mmdrv was torn down and re-inited), and that the DATAPATH to the peer works after it.
 *
 *     A ping IS a legitimate recovery probe, where the peer count is not, and the difference is worth
 *     being precise about: a reply cannot be manufactured by stale host memory. It requires this node
 *     to transmit, the peer to receive, decrypt, reply, and this node to receive and decrypt -- so it
 *     exercises the vif id, the per-peer chip state and the keys that the restart destroyed. What it
 *     does NOT prove is that the node is BEACONING again, which is why the on-air capture stays.
 *
 *     The reason ping was avoided in the first version was the documented "a source with no IP fails
 *     ping with an empty mpath" trap. That is handled by pinging BEFORE the restart too: a failing
 *     before-ping makes the run INCONCLUSIVE (the rig never carried traffic), never a FAIL.
 *   - THE RECOVERY VERDICT IS OFF-AIR: run tools/mesh_hwrestart_cap.py on chronium's morse0 across the
 *     run. It scores this board's own S1G beacons -- beacons before the restart, then whether they
 *     ever come back -- and uses the peer board's beacons as a liveness control so "no beacons" means
 *     silence rather than a monitor that lost the channel. Same shape as test-raw-rps, whose TEST|
 *     gates are also preconditions with the real verdict decoded from a capture.
 *
 * RIG: two ESP boards on the same mesh, plus chronium in monitor mode on ch27.
 *   board1 (this app) = node under test.  board0 = test-mesh-gate-node NO_PING=1 (cheapest responder;
 *   same MESH_ID "rimba-mesh" + ch27, no special build -- it only has to be there to peer and beacon).
 *   chronium: see docs/reference/rimba-linux-halow-monitor.md, then
 *     sudo python3 tools/mesh_hwrestart_cap.py <this board's mesh MAC> <peer mesh MAC>
 *   started BEFORE this board is reset. The MACs are printed below as TEST|INFO|mesh-mac / peer.
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
#include "ping/ping_sock.h"

#include "mmhalow.h"
#include "mmwlan.h"
#include "mmwlan_stats.h"
#include "umac/mesh/umac_mesh.h"

#include "test_report.h"

#define NAME "mesh-hwrestart"
#define RIG  "two ESP mesh nodes + chronium morse0 monitor; this board forces its own chip restart, " \
             "the RECOVERY verdict is off-air (tools/mesh_hwrestart_cap.py)"

#ifndef TEST_MESH_ID
#define MESH_ID "rimba-mesh"
#else
#define MESH_ID TEST_MESH_ID
#endif

#define MESH_S1G_CHAN    27
#define MESH_MAX_PLINKS  8
#define PEER_TIMEOUT_S   60
/* Beacon for a good while before triggering. The off-air scorer needs a solid pre-restart baseline of
 * this board's beacons to compare against; peering can complete less than a second after the first
 * beacon goes out, and triggering there leaves the baseline too thin to distinguish "went silent" from
 * "never really started". */
#define SETTLE_S         20
/* Generous: the handler tears the driver down and re-inits it (mmdrv_deinit + mmdrv_init), which on a
 * healthy path also re-runs the firmware load. Scoring a recovery too early would report a defect that
 * is really just impatience. */
#define RECOVER_WAIT_S   45
#define PING_COUNT       5

#ifdef TEST_AP_VIF
/* S3 arm: bring an AP vif up alongside the mesh before triggering, reproducing the SHIPPED gate's
 * configuration (rimba-halow-mesh-ap = mesh primary + AP secondary on one MM6108). Same SSID/PSK/chan
 * as the gate so the reproduction is of the real thing, not an approximation. */
#define AP_SSID          "rimba-ping"
#define AP_PSK           "rimbahalow"
#define AP_OP_CLASS      68
#define AP_MAX_STAS      16
#endif

static const char *TAG = "hwrestart";
static uint8_t g_mesh_mac[6];

/* The one probe that says whether the RESTART ITSELF ran, independent of anything the mesh does after.
 * hw_restart_evt_handler() calls umac_stats_increment_hw_restart_counter() only after it has already
 * torn the driver down and re-inited it, so a bump is proof the handler executed to that point. It does
 * NOT say the mesh recovered -- that is what the off-air metric is for. Returns -1 if unreadable. */
static int hw_restart_count(void)
{
    struct mmwlan_stats_umac_data s = { 0 };
    if (mmwlan_get_umac_stats(&s) != MMWLAN_SUCCESS) return -1;
    return (int)s.hw_restart_counter;
}

/* HOST-SIDE ONLY -- reported as context, never scored. See the header comment: this reads 1 on a node
 * that is transmitting nothing. Kept in the log precisely so the staleness stays visible next to the
 * on-air truth, because that contrast is the finding. */
static int peer_count(void)
{
    uint8_t macs[UMAC_MESH_MAX_PEERS][6] = { { 0 } };
    int n = mmwlan_mesh_peer_count(macs);
    return (n < 0) ? 0 : n;
}

/* First established peer's MAC, for the data-flow check. Uses the same (stale-prone) table as
 * peer_count -- fine here, because it is only used to ADDRESS a ping, and the ping itself is what
 * proves anything. A stale MAC yields a failed ping, never a false success. */
static bool first_peer_mac(uint8_t out[6])
{
    uint8_t macs[UMAC_MESH_MAX_PEERS][6] = { { 0 } };
    int n = mmwlan_mesh_peer_count(macs);
    if (n <= 0) return false;
    memcpy(out, macs[0], 6);
    return true;
}

/* ---- data-flow check ------------------------------------------------------------------------
 *
 * Beacons returning proves the vif is back on air. It does NOT prove the DATAPATH came back: the peer
 * could be re-installed on the chip at the wrong vif id, or its keys could be missing, and the node
 * would beacon happily while every unicast to it failed. So the fixture also pings the peer.
 *
 * BEFORE and AFTER, and the BEFORE one is what makes the AFTER one readable. A bare post-restart ping
 * failure is ambiguous between "recovery is broken" and "this rig never carried traffic in the first
 * place" -- and the documented "a source with no IP fails ping with an empty mpath" trap makes the
 * second reading entirely plausible. So a failing before-ping is INCONCLUSIVE (fix the rig), never a
 * recovery verdict. Only a before-PASS/after-FAIL pair is evidence against the recovery.
 *
 * The peer's IP is derived from its MAC using the bench's flat-subnet convention (10.9.9.<100 + low
 * 6 bits>, as test-mesh-gate-node pins for itself), so nothing here hardcodes a bench address. */
static volatile int s_ping_ok;
static volatile bool s_ping_done;

static void on_ping_success(esp_ping_handle_t hdl, void *args) { (void)hdl; (void)args; s_ping_ok++; }
static void on_ping_end(esp_ping_handle_t hdl, void *args) { (void)hdl; (void)args; s_ping_done = true; }

/* Returns replies received (0 on any failure). */
static int ping_peer(const uint8_t *peer_mac, int count)
{
    char ip[16];
    snprintf(ip, sizeof(ip), "10.9.9.%u", 100u + (peer_mac[5] & 0x3fu));

    ip_addr_t target = { 0 };
    ipaddr_aton(ip, &target);

    esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
    cfg.target_addr = target;
    cfg.count = count;
    cfg.timeout_ms = 2000;
    cfg.interval_ms = 1000;

    esp_ping_callbacks_t cbs = { .on_ping_success = on_ping_success, .on_ping_end = on_ping_end };
    esp_ping_handle_t hdl = NULL;
    if (esp_ping_new_session(&cfg, &cbs, &hdl) != ESP_OK) return 0;

    s_ping_ok = 0;
    s_ping_done = false;
    esp_ping_start(hdl);
    for (int i = 0; i < (count * 3) + 10 && !s_ping_done; i++) vTaskDelay(pdMS_TO_TICKS(1000));
    esp_ping_stop(hdl);
    esp_ping_delete_session(hdl);
    TEST_INFO("ping %s: %d/%d replies", ip, s_ping_ok, count);
    return s_ping_ok;
}

/* Pin a static flat-subnet IP on the mesh netif, same convention as test-mesh-gate-node. Without an
 * address of our own the ping has no source and fails for a reason that has nothing to do with the
 * restart -- the exact trap this fixture's header warns about. */
static bool pin_static_ip(void)
{
    esp_netif_t *n = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (n == NULL) return false;
    for (int i = 0; i < 60 && !esp_netif_is_netif_up(n); i++) vTaskDelay(pdMS_TO_TICKS(500));
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_netif_dhcpc_stop(n);
    esp_netif_set_mac(n, g_mesh_mac);

    char ip[16];
    snprintf(ip, sizeof(ip), "10.9.9.%u", 100u + (g_mesh_mac[5] & 0x3fu));
    esp_netif_ip_info_t info = { 0 };
    info.ip.addr = esp_ip4addr_aton(ip);
    info.netmask.addr = esp_ip4addr_aton("255.255.255.0");
    if (esp_netif_set_ip_info(n, &info) != ESP_OK) return false;
    TEST_INFO("mesh IP %s", ip);
    return true;
}

static int wait_for_peers(int timeout_s)
{
    for (int i = 0; i < timeout_s; i++)
    {
        int n = peer_count();
        if (n > 0) return n;
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    return 0;
}

static void park_forever(void)
{
    while (1) { vTaskDelay(pdMS_TO_TICKS(10000)); }
}

void app_main(void)
{
    vTaskDelay(pdMS_TO_TICKS(500));
    TEST_BEGIN(NAME, RIG);

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
    args.max_plinks = MESH_MAX_PLINKS;

    if (mmwlan_mesh_start(&args) != MMWLAN_SUCCESS)
    {
        TEST_INCONCLUSIVE("mmwlan_mesh_start failed -- no mesh to restart, so nothing is measured");
        TEST_END(NAME);
        park_forever();
    }
    mmwlan_override_max_tx_power(1);
    /* Greppable: this is the SA the off-air scorer filters on. */
    TEST_INFO("mesh-mac|" MACSTR "|mesh_id=%s|chan=%d", MAC2STR(g_mesh_mac), MESH_ID, MESH_S1G_CHAN);
    ESP_LOGI(TAG, "mesh up on \"%s\" ch%d as " MACSTR, MESH_ID, MESH_S1G_CHAN, MAC2STR(g_mesh_mac));

#ifdef TEST_AP_VIF
    /* --- S3: an AP vif alongside the mesh, which is what the shipped gate runs ---------------- */
    struct mmwlan_ap_args ap_args = MMWLAN_AP_ARGS_INIT;
    memcpy((char *)ap_args.ssid, AP_SSID, strlen(AP_SSID));
    ap_args.ssid_len = strlen(AP_SSID);
    memcpy(ap_args.passphrase, AP_PSK, strlen(AP_PSK));
    ap_args.passphrase_len = strlen(AP_PSK);
    ap_args.security_type = MMWLAN_SAE;
    ap_args.pmf_mode = MMWLAN_PMF_REQUIRED;
    ap_args.s1g_chan_num = MESH_S1G_CHAN;
    ap_args.op_class = AP_OP_CLASS;
    ap_args.max_stas = AP_MAX_STAS;
    if (mmwlan_ap_enable(&ap_args) != MMWLAN_SUCCESS)
    {
        TEST_INCONCLUSIVE("mmwlan_ap_enable failed -- no AP vif, so this run does NOT exercise the S3 "
                          "assert. Needs CONFIG_HALOW_AP_MODE=y");
        TEST_END(NAME);
        park_forever();
    }
    TEST_INFO("ap-vif|up alongside the mesh (ssid=\"%s\" chan=%d) -- this is the gate's shape",
              AP_SSID, MESH_S1G_CHAN);
#endif

    /* --- baseline: we must be peered BEFORE the restart, or the after-measurement means nothing --- */
    int before = wait_for_peers(PEER_TIMEOUT_S);
    TEST_STEP("peer-before", before > 0, "estab_peers=%d (host-side; a precondition, NOT the metric)",
              before);
    if (before == 0)
    {
        TEST_INCONCLUSIVE("never peered within %ds -- the peer board is down or RF is bad. This says "
                          "nothing about hw_restart recovery; fix the rig and re-run", PEER_TIMEOUT_S);
        TEST_END(NAME);
        park_forever();
    }

    /* --- data-flow control: does traffic reach the peer BEFORE the restart? ------------------- */
    uint8_t peer_mac[6] = { 0 };
    bool have_peer = first_peer_mac(peer_mac);
    bool have_ip = pin_static_ip();
    int ping_before = (have_peer && have_ip) ? ping_peer(peer_mac, PING_COUNT) : 0;
    TEST_STEP("data-before", ping_before > 0, "%d/%d replies from the peer", ping_before, PING_COUNT);
#ifdef TEST_AP_VIF
    /* The S3 arm does NOT gate on the datapath. Bringing up an AP vif alongside the mesh gives this
     * fixture two vifs but none of the per-vif RX demux the real gate installs
     * (rimba-halow-mesh-ap's gw_mesh_rx_cb / gw_ap_rx_cb), so mesh RX is not wired here and the ping
     * cannot succeed. That is a property of this reproducer, not of the code under test.
     *
     * It does not matter: S3 is about hw_restart_evt_handler() asserting the moment it sees an AP vif,
     * which happens before any datapath is consulted. What the arm needs is mesh + AP up and a
     * trigger, and both are satisfied. The ping result is reported for the record and ignored. */
    TEST_INFO("s3-arm|ignoring the datapath gate (%d/%d): this arm has no per-vif RX demux, and the "
              "assert under test fires before the datapath is reached", ping_before, PING_COUNT);
    if (0)
    {
#else
    if (ping_before == 0)
    {
#endif
        TEST_INCONCLUSIVE("no traffic reached the peer BEFORE the restart (peer_mac=%d ip=%d) -- this "
                          "rig never carried data, so a post-restart failure would say nothing about "
                          "recovery. Fix the rig and re-run; the beacon capture is still valid",
                          (int)have_peer, (int)have_ip);
        TEST_END(NAME);
        park_forever();
    }

    /* Give the scorer a real pre-restart beacon baseline before we pull the rug out. */
    TEST_INFO("peered; beaconing for %ds to build the off-air baseline", SETTLE_S);
    vTaskDelay(pdMS_TO_TICKS(SETTLE_S * 1000));

    /* --- force the restart ------------------------------------------------------------------- */
    int rst_before = hw_restart_count();
    TEST_INFO("hw_restart_counter=%d before the trigger", rst_before);
#ifdef TEST_AP_VIF
    /* EXPECTED TO PANIC, and that is the deliverable. hw_restart_evt_handler() opens with
     *     if (umac_interface_get_vif_id(umacd, UMAC_INTERFACE_AP) != MMDRV_VIF_ID_INVALID)
     *         { MMLOG_ERR(...); MMOSAL_ASSERT(false); }
     * so with an AP vif up it dies before any recovery runs. The board asserts and reboots, which means
     * NO TEST| verdict can follow -- the panic itself is the result. Read the console for
     * "Unable to recover from hardware restart with AP interface active" plus the assert backtrace.
     * If the board instead survives and prints a verdict below, S3 is fixed. */
    TEST_INFO("s3-expect-panic|an AP vif is active, so the handler should assert before recovering. "
              "A reboot with no verdict after this line IS the S3 defect");
#endif
    TEST_INFO("restart-trigger|forcing a chip restart via mmwlan_force_hw_restart() with %d peer(s) "
              "established", before);
    if (mmwlan_force_hw_restart() != MMWLAN_SUCCESS)
    {
        TEST_INCONCLUSIVE("mmwlan_force_hw_restart() refused -- the WLAN subsystem is not active, so no "
                          "restart happened and nothing is measured");
        TEST_END(NAME);
        park_forever();
    }

    /* The call queues a umac event; the handler runs on the event loop and tears the driver down and
     * back up underneath us. Give it the full window before judging. */
    for (int i = 0; i < RECOVER_WAIT_S; i++)
    {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if ((i % 10) == 9)
        {
            TEST_INFO("  +%ds since restart: hw_restart_counter=%d (host-side estab_peers=%d -- "
                      "reads 1 even when deaf, see the header comment)",
                      i + 1, hw_restart_count(), peer_count());
        }
    }

    int rst_after = hw_restart_count();
    int after = peer_count();

    /* The ONLY thing scored on-device: did the trigger actually restart the chip? */
    bool restarted = (rst_before >= 0) && (rst_after > rst_before);
    TEST_STEP("restart-ran", restarted,
              "hw_restart_counter %d -> %d (bumped by hw_restart_evt_handler AFTER mmdrv_deinit + "
              "mmdrv_init, so a bump proves the restart path executed)", rst_before, rst_after);

    TEST_INFO("host-side estab_peers=%d (was %d) -- REPORTED, NOT SCORED. mesh_peers[] survives "
              "mmdrv_deinit/init untouched, so this number is meaningless as a recovery signal",
              after, before);

    /* --- did the DATAPATH come back? --------------------------------------------------------- */
    int ping_after = ping_peer(peer_mac, PING_COUNT);
    bool data_ok = (ping_after > 0);
    TEST_STEP("data-after", data_ok, "%d/%d replies (was %d/%d before the restart)",
              ping_after, PING_COUNT, ping_before, PING_COUNT);

    if (!restarted)
    {
        TEST_INCONCLUSIVE("hw_restart_counter did not advance (%d -> %d): mmwlan_force_hw_restart() "
                          "returned SUCCESS but the queued event never reached the handler. The "
                          "TRIGGER is broken, so the run says nothing about recovery -- fix the hook "
                          "before reading any capture from this run", rst_before, rst_after);
    }
    else if (!data_ok)
    {
        TEST_FAIL("the chip restarted (hw_restart_counter %d -> %d) but the DATAPATH did not come "
                  "back: %d/%d replies before, %d/%d after. Beaconing may still have resumed -- check "
                  "the capture -- but the peer is not reachable, so the vif/keys/peer state were not "
                  "fully restored", rst_before, rst_after, ping_before, PING_COUNT,
                  ping_after, PING_COUNT);
    }
    else
    {
        TEST_PASS("restarted (hw_restart_counter %d -> %d) with %d peer(s) and the datapath came "
                  "back: %d/%d replies after vs %d/%d before. NOTE this is the DATAPATH half of the "
                  "verdict -- whether the node is beaconing again is scored off-air by "
                  "tools/mesh_hwrestart_cap.py on chronium's morse0", rst_before, rst_after, before,
                  ping_after, PING_COUNT, ping_before, PING_COUNT);
    }

    TEST_END(NAME);
    park_forever();
}
