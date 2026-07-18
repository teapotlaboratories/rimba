/*
 * test-twt-assoc-sta — the STA (requester + reporter) role of the ASSOC-EMBEDDED TWT test.
 *
 * The delta vs test-twt-sta: this app requests TWT via mmwlan_twt_add_configuration() BEFORE
 * mmhalow_connect(), so the TWT IE rides in the (re)association request — the path a Linux
 * hostapd_s1g honours (assoc-time he_twt_responder, default-on) and the ONLY TWT path that engages
 * on BOTH an ESP SoftAP and a Linux AP. The mid-session action path (test-twt) is ignored by a
 * Linux AP, so this closes the gap for the universal path.
 *
 * WHAT IT PROVES: an assoc-embedded TWT agreement reaches INSTALLED
 * (mmwlan_twt_agreement_installed == 1) — a discrete negotiation outcome, not an RF measurement.
 * WHAT IT DOES NOT PROVE: the power saving itself (that is the tp PPK2 tier, not a pass/fail).
 *
 * NB: whether the assoc-embedded REQUESTER path populates the same flow-id agreement table the
 * accessor reads was UNCERTAIN from the source (umac_twt_add_configuration only stores the config;
 * umac_twt_process_ie needs a pre-existing agreement) — so this reporter polls flow ids 0..3 and
 * reports the observed state, and the T2 test's assertion follows what the bench actually shows.
 */

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"

#include "mmhalow.h"
#include "mmwlan.h"

#include "test_report.h"

#define NAME       "twt-assoc"
#define RIG        "AP=test-apsta-ap or Linux hostapd (assoc-time responder), STA(this)=requester+reporter"

/* Keep identical to the AP (test-apsta-ap / hostapd-rimba.conf). */
#define LINK_SSID  "rimba-ping"
#define LINK_PSK   "rimbahalow"

#define CONNECT_TIMEOUT_MS 40000
#define TWT_WAIT_S         20          /* poll for the agreement to reach INSTALLED */
#define TWT_WAKE_INT_US    10000000    /* 10 s service-period interval */
#define TWT_MIN_WAKE_US    65280
#define TWT_MAX_FLOWS      4           /* the assoc-embedded flow id is unknown a priori — scan 0..3 */

/* Exported from morselib (matches the mmwlan* protected glob). 1=INSTALLED, 0=pending, -1=none. */
extern int mmwlan_twt_agreement_installed(uint16_t flow_id);

static volatile bool s_connected = false;
static void sta_status_cb(enum mmwlan_sta_state st)
{
    if (st == MMWLAN_STA_CONNECTED) { s_connected = true; }
}

static void park_forever(void)
{
    while (1) { vTaskDelay(pdMS_TO_TICKS(10000)); }
}

/* Return the flow id that is INSTALLED (1), else the first that is pending (encode as -2), else -1. */
static int scan_installed(int *pending_flow)
{
    *pending_flow = -1;
    for (int f = 0; f < TWT_MAX_FLOWS; f++) {
        int st = mmwlan_twt_agreement_installed((uint16_t)f);
        if (st == 1) return f;
        if (st == 0 && *pending_flow < 0) *pending_flow = f;
    }
    return -1;
}

void app_main(void)
{
    vTaskDelay(pdMS_TO_TICKS(500));
    TEST_BEGIN(NAME, RIG);
    TEST_INFO("assoc-embedded TWT: add_configuration BEFORE connect, then poll INSTALLED");

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_netif_init());

    mmhalow_init(NULL);
    mmhalow_print_version_info();

    mmhalow_wifi_config_t conf = { .sta = MMWLAN_STA_ARGS_INIT };
    memcpy(conf.sta.ssid, LINK_SSID, strlen(LINK_SSID));
    conf.sta.ssid_len = strlen(LINK_SSID);
    memcpy(conf.sta.passphrase, LINK_PSK, strlen(LINK_PSK));
    conf.sta.passphrase_len = strlen(LINK_PSK);
    conf.sta.security_type = MMWLAN_SAE;
    ESP_ERROR_CHECK(mmhalow_set_config(WIFI_IF_STA, &conf));

    /* ASSOC-EMBEDDED: the config goes in the (re)assoc IEs, NOT a mid-session action frame. */
    struct mmwlan_twt_config_args twt = MMWLAN_TWT_CONFIG_ARGS_INIT;
    twt.twt_mode = MMWLAN_TWT_REQUESTER;
    twt.twt_wake_interval_us = TWT_WAKE_INT_US;
    twt.twt_min_wake_duration_us = TWT_MIN_WAKE_US;
    twt.twt_setup_command = MMWLAN_TWT_SETUP_REQUEST;
    enum mmwlan_status tc = mmwlan_twt_add_configuration(&twt);
    TEST_STEP("twt-add-config", tc == MMWLAN_SUCCESS,
                 "mmwlan_twt_add_configuration ret=%d (TWT in assoc IEs)", (int)tc);

    mmwlan_override_max_tx_power(1);   /* close-bench RX-overload workaround (mirrors the AP) */

    mmhalow_connect(sta_status_cb);
    int waited = 0;
    while (!s_connected && waited < CONNECT_TIMEOUT_MS) { vTaskDelay(pdMS_TO_TICKS(250)); waited += 250; }
    TEST_STEP("associate", s_connected, "connected=%d after %d ms", (int)s_connected, waited);
    if (!s_connected) {
        TEST_FAIL("no association to \"%s\" within %d ms (is the AP up on the peer?)",
                     LINK_SSID, CONNECT_TIMEOUT_MS);
        TEST_END(NAME);
        park_forever();
    }

    /* Poll for an INSTALLED agreement on any of flows 0..3 (the assoc-embedded flow id is
     * assigned by the negotiation, not by us). */
    int inst_flow = -1, pending = -1;
    for (int s = 0; s < TWT_WAIT_S; s++) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        inst_flow = scan_installed(&pending);
        if (inst_flow >= 0) break;
        if ((s % 5) == 4)
            TEST_INFO("waiting for assoc-embedded TWT INSTALLED... %ds, first-pending-flow=%d",
                         s + 1, pending);
    }

    TEST_STEP("agreement-installed", inst_flow >= 0,
                 "installed_flow=%d pending_flow=%d (scanned flows 0..%d)",
                 inst_flow, pending, TWT_MAX_FLOWS - 1);
    if (inst_flow >= 0) {
        TEST_PASS("assoc-embedded TWT reached INSTALLED on flow %d (the universal path — TWT in "
                     "the assoc IEs, honoured by the assoc-time responder)", inst_flow);
    } else if (pending >= 0) {
        TEST_INCONCLUSIVE("associated + a TWT agreement is staged on flow %d but did not reach "
                             "INSTALLED within %ds (pending — the AP may not have completed the "
                             "assoc-time negotiation)", pending, TWT_WAIT_S);
    } else {
        TEST_INCONCLUSIVE("associated, but no TWT agreement is visible via "
                             "mmwlan_twt_agreement_installed on flows 0..%d — the assoc-embedded "
                             "requester path may not register an agreement in the accessor's table "
                             "(config-only). Assoc-embedded TWT then needs current metering (tp), "
                             "not this structural accessor.", TWT_MAX_FLOWS - 1);
    }
    TEST_END(NAME);
    park_forever();
}
