/*
 * test-raw-cap — does the loaded MM6108 firmware advertise RAW / page slicing? (RAW S0a)
 *
 * S0a of the RAW AP-side feasibility spike (docs/design-specification/rimba-raw-apside-design.md).
 * The whole RAW port's viability rests on whether the MM6108 blob acts on a Restricted Access Window
 * schedule at all, and this is the cheapest signal available: one board, no sniffer, no Linux
 * reference, no beacon hook. If the capability is absent, the on-air spike should not be built at all.
 *
 * WHAT IT PROVES: whether the shipped .mbin claims MORSE_CAPS_RAW / MORSE_CAPS_PAGE_SLICING.
 * WHAT IT DOES NOT PROVE: that the chip acts on an AP-AUTHORED schedule. The capability word carries
 * no direction, and STA-side RAW was already believed working -- so a set bit is necessary, not
 * sufficient, and that question stays on-air. A CLEAR bit is the decisive outcome.
 *
 * Do not read "enforcement" into RAW: 802.11ah has no AP primitive that polices peers -- a STA
 * restricts ITSELF on parsing the RPS, and the chip's own RAW counters only ever count the local
 * transmitter deferring its own frames. A test written against AP-policing reports failure for a
 * feature that works. See README.md.
 *
 * Brings up an AP vif rather than a mesh one. The capabilities are device-global (fetched with
 * MMDRV_VIF_ID_INVALID during the first interface-add), so any vif would do for the read -- but RAW
 * is an AP-direction feature, and if that global-vs-per-vif reading is ever wrong, an AP vif still
 * returns the right answer where a mesh vif might not.
 *
 * Reports via the TEST| console contract but is NOT a regression test: "FW does not advertise RAW"
 * is a legitimate finding, not something that broke. It is scored on whether the READ succeeded, and
 * the GO/BLOCKED interpretation is stated in the output for a human to act on.
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

#define NAME  "raw-cap"
#define RIG   "any single ESP board; no peer, no sniffer, no Linux reference"

#define AP_SSID        "rimba-rawcap"
#define AP_PSK         "rimbahalow"
#define AP_S1G_CHAN    27
#define AP_OP_CLASS    68

/* Exported from morselib (names match the mmwlan* glob in protected_syms.txt).
 * 1 = advertised, 0 = not advertised or no vif yet, -1 = morselib not initialised. */
extern int mmwlan_raw_capability_advertised(void);
extern int mmwlan_page_slicing_capability_advertised(void);

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
    TEST_INFO("bringing up an AP vif, then reading the FW-advertised RAW / page-slicing bits");

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

    if (mmhalow_set_config(WIFI_IF_AP, &cfg) != ESP_OK)
    {
        TEST_INCONCLUSIVE("mmhalow_set_config(AP) failed -- cannot bring a vif up, so the "
                          "capability read would be untrustworthy");
        TEST_END(NAME);
        park_forever();
    }
    mmhalow_wifi_start();

    /* The capability fetch happens during interface-add, so give the AP vif a moment to exist
     * before reading. Without a vif both accessors legitimately return 0, which would be
     * indistinguishable from "the FW does not advertise it". */
    vTaskDelay(pdMS_TO_TICKS(2000));

    int raw = mmwlan_raw_capability_advertised();
    int slicing = mmwlan_page_slicing_capability_advertised();

    TEST_INFO("MORSE_CAPS_RAW=%d  MORSE_CAPS_PAGE_SLICING=%d", raw, slicing);

    /* Score the READ, not the value -- a clear bit is an answer, not a regression. */
    TEST_STEP("cap-read", raw >= 0 && slicing >= 0,
              "raw=%d page_slicing=%d (>=0 means morselib was initialised and the read is valid)",
              raw, slicing);

    if (raw < 0 || slicing < 0)
    {
        TEST_INCONCLUSIVE("morselib not initialised (raw=%d slicing=%d) -- the radio did not boot; "
                          "suspect country code or SPI wiring, not a capability change", raw, slicing);
        TEST_END(NAME);
        park_forever();
    }

    if (raw == 1)
    {
        TEST_INFO("S0a => GO-CANDIDATE: the blob claims RAW. This is NECESSARY BUT NOT SUFFICIENT "
                  "-- the capability word carries no direction and STA-side RAW is already known to "
                  "work, so AP-direction enforcement is still unproven. Proceed to S0b: hand-build "
                  "an RPS IE into the beacon, capture on chronium morse0, byte-diff against a live "
                  "Linux RAW AP, and measure whether a STA confines TX to its slot.");
        TEST_PASS("FW advertises MORSE_CAPS_RAW (page_slicing=%d) -- S0a answered, S0b is justified",
                  slicing);
    }
    else
    {
        TEST_INFO("S0a => BLOCKED-SIGNAL: the blob does not claim RAW. Do not build the S0b harness "
                  "on this firmware. Escalate as a vendor/FW ask, and do not ship a RAW "
                  "advertisement the AP cannot enforce.");
        TEST_PASS("read valid: FW does NOT advertise MORSE_CAPS_RAW (page_slicing=%d). This is a "
                  "feasibility ANSWER, not a regression -- see the INFO line above", slicing);
    }

    TEST_END(NAME);
    park_forever();
}
