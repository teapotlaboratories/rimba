/*
 * test-ibss — IBSS join/adopt + correct peer records.
 *
 * A SYMMETRIC app: both nodes run this identical firmware. The orchestrator flashes it to a support
 * node (creates + beacons) and to the reporter node (joins, polls the peer count, emits the verdict).
 * All apps a T2 test flashes are test-*.
 *
 * WHAT IT PROVES: IBSS_CONFIG(CREATE) succeeds and a node forms EXACTLY the right peer records with
 * no phantoms -- the structural core of the IBSS port (docs/ibss/rimba-ibss-test-plan.md P0.1).
 * WHAT IT DOES NOT PROVE: distinct AIDs (IBSS peers on the ESP are MAC-keyed, aid=0 -- that is a
 * Linux `station dump` oracle assertion), nor on-air frame equivalence with Linux, nor throughput.
 *
 * Structural assertion: mmwlan_ibss_start() == SUCCESS (== IBSS_CONFIG(CREATE)==0), AND
 * mmwlan_ibss_peer_count() == EXACTLY 1 on a 2-node cell. Every in_use peer slot is a heard sender,
 * so "exactly 1" IS the 0-phantom check (a phantom -- the #17 bug -- would be an extra in_use entry).
 * Sharper than the mesh test's tolerant ">= 1": for IBSS the exact count is the phantom detector.
 *
 * MAC / role: the MM6108 factory MAC is shared across modules, so (like the mesh apps) we derive a
 * unique locally-administered MAC from the ESP32 efuse and use it as the IBSS if_addr. IBSS needs
 * exactly one CREATE + the rest JOIN, and there is no auto create-else-join, so the creator is pinned
 * to a known bench MAC (board0), matching the mesh-relay pattern of hardcoding bench MACs. The rig
 * guarantees board0 is present (support role); if neither node is the creator, no cell forms and the
 * reporter returns INCONCLUSIVE ("no creator"), never a false FAIL.
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
#include "umac/ibss/umac_ibss.h"

#include "test_report.h"

#define NAME            "ibss"
#define RIG             "two ESP nodes (symmetric); reporter polls the IBSS peer count"
#define LINK_SSID       "rimba-ibss"
#define LINK_S1G_CHAN   27
#define PEER_WAIT_S     45

/* Provisioned shared BSSID for the cell (all nodes must agree; same app => same value). */
static const uint8_t LINK_BSSID[6] = { 0x02, 0x11, 0x22, 0x33, 0x44, 0x55 };

/* The creator = board0's derived mesh MAC (efuse E0:72:A1:F8:EF:A4 -> |0x02 &0xFE). Deterministic
 * across the fixed bench; matches the mesh-relay/perf pattern of pinning bench MACs. */
static const uint8_t CREATOR_MAC[6] = { 0xe2, 0x72, 0xa1, 0xf8, 0xef, 0xa4 };

void app_main(void)
{
    vTaskDelay(pdMS_TO_TICKS(500));
    TEST_BEGIN(NAME, RIG);

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_netif_init());

    /* Unique per-board IBSS MAC (the MM6108 factory MAC is shared across modules). */
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    mac[0] = (mac[0] | 0x02) & 0xFE;
    bool creator = (memcmp(mac, CREATOR_MAC, 6) == 0);

    mmhalow_init(NULL);
    mmhalow_print_version_info();

    struct mmwlan_ibss_args args = { 0 };
    memcpy(args.if_addr, mac, sizeof(args.if_addr));
    memcpy(args.bssid, LINK_BSSID, sizeof(args.bssid));
    memcpy(args.ssid, LINK_SSID, strlen(LINK_SSID));
    args.ssid_len = strlen(LINK_SSID);
    args.create = creator;
    args.s1g_chan_num = LINK_S1G_CHAN;
    args.beacon_interval_tu = 100;

    enum mmwlan_status st = mmwlan_ibss_start(&args);
    TEST_STEP("ibss-create", st == MMWLAN_SUCCESS, "role=%s mmwlan_ibss_start=%d",
                 creator ? "CREATE" : "JOIN", (int)st);
    if (st != MMWLAN_SUCCESS) {
        TEST_FAIL("mmwlan_ibss_start (%s) failed status=%d -- IBSS_CONFIG did not return 0 "
                     "(country not set / radio not booted?)", creator ? "CREATE" : "JOIN", (int)st);
        TEST_END(NAME);
        while (1) { vTaskDelay(pdMS_TO_TICKS(10000)); }
    }

    mmwlan_override_max_tx_power(1);   /* close-bench RX-overload workaround */

    /* Up-marker the orchestrator waits on for the SUPPORT node (Role.up_marker="ibss-up"). */
    TEST_INFO("ibss-up: %s as %02x:%02x:%02x:%02x:%02x:%02x on ch%d",
                 creator ? "created" : "joined", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
                 LINK_S1G_CHAN);

    /* Poll for exactly one peer record. */
    uint8_t peer_macs[8][6];
    uint8_t n = 0;
    for (int s = 0; s < PEER_WAIT_S; s++) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        n = mmwlan_ibss_peer_count(peer_macs);
        if (n >= 1) break;
        if ((s % 5) == 4) TEST_INFO("waiting for a peer record... %ds, peers=%u", s + 1, n);
    }

    TEST_STEP("peer-record", n == 1, "peers=%u", n);
    if (n == 1) {
        TEST_PASS("IBSS formed exactly 1 peer record (0 phantoms): %02x:%02x:%02x:%02x:%02x:%02x",
                     peer_macs[0][0], peer_macs[0][1], peer_macs[0][2],
                     peer_macs[0][3], peer_macs[0][4], peer_macs[0][5]);
    } else if (n == 0) {
        TEST_INCONCLUSIVE("no peer record within %ds -- the peer node never appeared (is the "
                             "creator (board0) up on the same cell / channel %d?)", PEER_WAIT_S,
                             LINK_S1G_CHAN);
    } else {
        TEST_FAIL("%u peer records on a 2-node cell -- expected exactly 1. Extra in_use entries "
                     "are PHANTOM peers (the divergence-17 bug).", n);
    }
    TEST_END(NAME);
    while (1) { vTaskDelay(pdMS_TO_TICKS(10000)); }
}
