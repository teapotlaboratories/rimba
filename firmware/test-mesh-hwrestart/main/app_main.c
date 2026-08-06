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
 *   - THIS APP asserts only what the device can genuinely know: that the trigger really did cause a
 *     chip restart (umac_stats' hw_restart_counter, bumped by hw_restart_evt_handler() itself only
 *     AFTER mmdrv was torn down and re-inited). That validates the FIXTURE, not the fix.
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

    /* Give the scorer a real pre-restart beacon baseline before we pull the rug out. */
    TEST_INFO("peered; beaconing for %ds to build the off-air baseline", SETTLE_S);
    vTaskDelay(pdMS_TO_TICKS(SETTLE_S * 1000));

    /* --- force the restart ------------------------------------------------------------------- */
    int rst_before = hw_restart_count();
    TEST_INFO("hw_restart_counter=%d before the trigger", rst_before);
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

    if (!restarted)
    {
        TEST_INCONCLUSIVE("hw_restart_counter did not advance (%d -> %d): mmwlan_force_hw_restart() "
                          "returned SUCCESS but the queued event never reached the handler. The "
                          "TRIGGER is broken, so the run says nothing about recovery -- fix the hook "
                          "before reading any capture from this run", rst_before, rst_after);
    }
    else
    {
        TEST_PASS("trigger verified: the chip really was restarted (hw_restart_counter %d -> %d) with "
                  "%d peer(s) established. THIS IS NOT A RECOVERY VERDICT -- score recovery off-air "
                  "with tools/mesh_hwrestart_cap.py on chronium's morse0; it reports whether this "
                  "board's beacons ever return", rst_before, rst_after, before);
    }

    TEST_END(NAME);
    park_forever();
}
