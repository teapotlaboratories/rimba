/*
 * test-ampdu-cap — the firmware advertises the mesh A-MPDU capability (S0a).
 *
 * Single board, no peer, no traffic, no capture: A-MPDU is FW-assembled and the capability is a
 * pure advertisement read. Brings up a mesh vif (capabilities are populated per-vif at interface-add
 * time, so a vif must exist) and reads the same bit the aggregation eligibility gate consults
 * (umac_datapath.c: MORSE_CAP_SUPPORTED(..., AMPDU)). A stack bump that silently drops that bit
 * fails here. Reports via the TEST| console contract.
 *
 * WHAT IT PROVES: the MM6108 firmware still advertises the mesh AMPDU capability the S0 spike proved
 * on 2026-07-11 (docs/worklog/2026-07-11-mesh-ampdu-s0-fw-capability-spike.md).
 * WHAT IT DOES NOT PROVE: that aggregation actually happens on the air, or any throughput figure --
 * the capability bit is necessary, not sufficient (real aggregation needs a peer + a capture).
 */

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
#include "umac/mesh/umac_mesh.h"

#include "test_report.h"

#define NAME  "ampdu-cap"
#define RIG   "any single ESP mesh node; no peer, no capture"
#define MESH_ID  "rimba-mesh"
#define MESH_S1G_CHAN  27

/* Exported from morselib (name matches the mmwlan* protected glob). 1=advertised, 0=not, -1=uninit. */
extern int mmwlan_ampdu_capability_advertised(void);

void app_main(void)
{
    vTaskDelay(pdMS_TO_TICKS(500));
    TEST_BEGIN(NAME, RIG);
    TEST_INFO("bringing up a mesh vif, then reading the FW-advertised A-MPDU capability");

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_netif_init());

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    mac[0] = (mac[0] | 0x02) & 0xFE;

    mmhalow_init(NULL);
    mmhalow_print_version_info();

    struct mmwlan_mesh_args args = { 0 };
    memcpy(args.if_addr, mac, sizeof(mac));
    memcpy(args.mesh_id, MESH_ID, strlen(MESH_ID));
    args.mesh_id_len = strlen(MESH_ID);
    args.s1g_chan_num = MESH_S1G_CHAN;
    args.beacon_interval_tu = 100;
    args.max_plinks = 16;

    enum mmwlan_status st = mmwlan_mesh_start(&args);
    if (st != MMWLAN_SUCCESS) {
        /* No vif -> the capability read is untrustworthy (reads 0). Not a capability regression;
         * most likely a country-code / radio-boot problem. */
        TEST_INCONCLUSIVE("mesh vif did not come up (mmwlan_mesh_start=%d) -- cannot read the "
                             "per-vif capability (country not set / radio not booted?)", (int)st);
        TEST_END(NAME);
        while (1) { vTaskDelay(pdMS_TO_TICKS(10000)); }
    }

    int cap = mmwlan_ampdu_capability_advertised();
    TEST_STEP("fw-ampdu-cap", cap == 1, "mmwlan_ampdu_capability_advertised=%d", cap);

    if (cap == 1) {
        TEST_PASS("FW advertises the mesh AMPDU capability (AMPDU_cap=1) -- the same bit the "
                     "aggregation eligibility gate consumes");
    } else if (cap == 0) {
        TEST_FAIL("FW does NOT advertise the mesh AMPDU capability (AMPDU_cap=0) -- the "
                     "aggregation gate is now closed; A-MPDU eligibility is lost. Suspect a "
                     "firmware/stack bump that dropped MORSE_CAPS_AMPDU.");
    } else {
        TEST_INCONCLUSIVE("morselib not initialised (cap=%d)", cap);
    }
    TEST_END(NAME);

    /* Never sleep (keeps the native USB enumerated so the board stays reflashable). */
    while (1) { vTaskDelay(pdMS_TO_TICKS(10000)); }
}
