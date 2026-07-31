/*
 * test-raw-rps — does a host-authored RPS element survive onto the air? (RAW S0b-3 / S0b-4, Q2)
 *
 * S0b-2 already passed the gate all-Linux: the MM6108 blob parses a Restricted Access Window schedule
 * out of a host-authored beacon and gates its own TX to it, and a STA self-restricts on receiving one.
 * Everything left is ESP-host-side, and this fixture is the beacon end of it.
 *
 * WHAT IT PROVES: whether the RPS element (EID 208) that morselib's host beacon builder splices in
 * reaches the air BYTE-IDENTICAL to what a live Linux hostapd_s1g AP transmits -- i.e. whether the blob
 * rewrites or strips IEs in a host-supplied beacon. Same question for the S1G Capabilities RAW
 * Operation Support bit (octet[6] bit 3), which Linux emits only while RAW is actually running.
 *
 * WHAT IT DOES NOT PROVE: that the chip ACTS on the ESP's schedule. That is S0b-5, and it is read from
 * the chip's own tag-4210 RAW counters, not from this capture.
 *
 * The board is a beacon source and nothing else -- no STA, no traffic, no peer. All the evidence is
 * off-air, captured on chronium's morse0 monitor and decoded with tools/raw_s1g_beacon_decode.py.
 * The console output here is orientation for the operator, not the measurement.
 *
 * THE SPIKE IS COMPILE-GATED. The two source edits it depends on live in shared morselib
 * (umac_ap.c's umac_ap_build_beacon, s1g_capabilities.c's ie_s1g_capabilities_build_ap) and are wrapped
 * in `#ifdef RIMBA_RAW_S0B_SPIKE`. That define is set build-globally by THIS app's CMakeLists.txt and
 * nowhere else, so the other 26 apps that set CONFIG_HALOW_AP_MODE=y are untouched. Build this fixture
 * with the morselib edits absent and you get the NO-HOOK BASELINE from the identical binary otherwise --
 * same SSID, same BSSID, same beacon interval -- which is the control the byte-diff needs.
 */

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "nvs_flash.h"

#include "mmhalow.h"
#include "mmwlan.h"

#include "test_report.h"

#define NAME  "raw-rps"
#define RIG   "one ESP board as AP + chronium morse0 monitor; optionally chronite as the Linux RAW AP"

#define AP_SSID        "rimba-rawrps"
#define AP_PSK         "rimbahalow"
#define AP_S1G_CHAN    27
#define AP_OP_CLASS    68

/* Pinned so the capture is comparable with the Linux reference AP, whose tracked
 * docs/reference/captures/hostapd-rimba.conf uses beacon_int=100. */
#define AP_BEACON_INT_TUS  100

/* DELIBERATE DIVERGENCE from the reference AP's dtim_period=1, and the reason matters. The design doc
 * requires classifying the RPS per frame across a full DTIM cycle, because Linux's short-beacon IE
 * allowlist DOES include RPS -- so a capture that only sampled DTIM beacons could report Q2=PASS while
 * non-DTIM beacons carried nothing. With dtim_period=1 every beacon is a DTIM beacon and that check is
 * unrunnable. 3 puts both classes in one capture. It is safe to diverge here because S0b-4 is a
 * byte-diff of an element, not a timing measurement -- DTIM period does not enter the RPS bytes.
 * (Source says the ESP cannot have this failure at all: morselib has no short-beacon TX path, and
 * umac_ap_build_beacon() is the single unconditional builder for every beacon. This measures it.) */
#define AP_DTIM_PERIOD     3

/* Cap TX power as the other on-air fixtures do. The bench's documented RX-overload signature at
 * RSSI ~ -3 dBm produces retries; the reference captures sat at -36 dBm and that is the target. */
#define AP_TX_POWER_DBM    1

static void park_forever(void)
{
    while (1)
    {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

void app_main(void)
{
    vTaskDelay(pdMS_TO_TICKS(500));
    TEST_BEGIN(NAME, RIG);

#ifdef RIMBA_RAW_S0B_SPIKE
    TEST_INFO("built WITH RIMBA_RAW_S0B_SPIKE -- morselib splices the golden RPS into every beacon "
              "and asserts the S1G-caps RAW bit, IF the guarded morselib edits are present in the tree");
#else
    TEST_INFO("built WITHOUT RIMBA_RAW_S0B_SPIKE -- plain AP beacons, this is the no-hook baseline");
#endif

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_netif_init());

    mmhalow_init(NULL);
    mmhalow_print_version_info();

    mmhalow_wifi_config_t cfg = { .ap = MMWLAN_AP_ARGS_INIT };
    memcpy((char *)cfg.ap.ssid, AP_SSID, strlen(AP_SSID));
    cfg.ap.ssid_len = strlen(AP_SSID);
    memcpy(cfg.ap.passphrase, AP_PSK, strlen(AP_PSK));
    cfg.ap.passphrase_len = strlen(AP_PSK);
    cfg.ap.security_type = MMWLAN_SAE;
    cfg.ap.pmf_mode = MMWLAN_PMF_REQUIRED;
    cfg.ap.s1g_chan_num = AP_S1G_CHAN;
    cfg.ap.op_class = AP_OP_CLASS;
    cfg.ap.max_stas = 4;
    cfg.ap.beacon_interval_tus = AP_BEACON_INT_TUS;
    cfg.ap.dtim_period = AP_DTIM_PERIOD;

    if (mmhalow_set_config(WIFI_IF_AP, &cfg) != ESP_OK)
    {
        TEST_INCONCLUSIVE("mmhalow_set_config(AP) failed -- no beacons, so nothing to capture");
        TEST_END(NAME);
        park_forever();
    }

    /* Strictly between set_config and wifi_start, as the working precedent does (test-apsta-ap): the
     * override can only REDUCE below the regulatory max and returns MMWLAN_UNAVAILABLE once the WLAN
     * subsystem is active, so calling it after start would silently do nothing. */
    if (mmwlan_override_max_tx_power(AP_TX_POWER_DBM) != MMWLAN_SUCCESS)
    {
        TEST_INFO("mmwlan_override_max_tx_power(%d) REFUSED -- the AP is at the regulatory max. Read "
                  "the RSSI in the capture before trusting it; ~-3 dBm is the RX-overload signature",
                  AP_TX_POWER_DBM);
    }

    mmhalow_wifi_start();

    /* Let the vif come up before reporting the MAC: the capture is filtered on it, and a run whose
     * BSSID the operator guessed is a run that can attribute the wrong beacons to the ESP. */
    vTaskDelay(pdMS_TO_TICKS(3000));

    uint8_t mac[6] = { 0 };
    if (mmwlan_get_mac_addr(mac) == MMWLAN_SUCCESS)
    {
        TEST_INFO("AP BSSID %02x:%02x:%02x:%02x:%02x:%02x ssid=\"%s\" chan=%d beacon_int=%d TU dtim=%d "
                  "tx_power=%d dBm",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
                  AP_SSID, AP_S1G_CHAN, AP_BEACON_INT_TUS, AP_DTIM_PERIOD, AP_TX_POWER_DBM);
    }
    else
    {
        TEST_INFO("could not read the AP MAC -- filter the capture on SSID \"%s\" instead", AP_SSID);
    }

    /* Scored on "is this board beaconing", which is the only precondition the capture has. The verdict
     * itself is off-air and belongs to the decoder, so there is deliberately no TEST| gate on the RPS. */
    TEST_STEP("ap-beaconing", true,
              "AP vif up on ch%d; capture on chronium morse0 and decode with "
              "tools/raw_s1g_beacon_decode.py", AP_S1G_CHAN);

    TEST_INFO("next: sudo tcpdump -i morse0 -w cap.pcap on chronium, then "
              "python3 tools/raw_s1g_beacon_decode.py cap.pcap --sa <BSSID above> --beacon-int-tu %d",
              AP_BEACON_INT_TUS);

    TEST_END(NAME);
    park_forever();
}
