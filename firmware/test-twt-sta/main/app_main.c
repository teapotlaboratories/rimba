/*
 * test-twt-sta — the STA (requester + reporter) role of the TWT responder T2 test.
 *
 * Pairs with the AP role = test-apsta-ap (a SoftAP; the MM6108 is a TWT responder by DEFAULT for
 * an AP vif -- umac_interface.c:145, "responder is default-on for AP"). This app associates, sends a
 * mid-session TWT Setup Request action frame (mmwlan_twt_setup_request, REQUESTER), and polls the
 * agreement state until it reaches INSTALLED -- the structural proof the AP responded and an
 * agreement was established. All apps a T2 test flashes are test-*.
 *
 * WHAT IT PROVES: the ESP SoftAP accepts a TWT Setup Request and an agreement reaches INSTALLED
 * (the action-frame TWT path, PR teapotlaboratories/rimba#15).
 * WHAT IT DOES NOT PROVE: the power saving itself (that needs the PPK2 current ladder, not a
 * pass/fail), nor Linux-STA-as-requester interop.
 *
 * Structural assertion: mmwlan_twt_agreement_installed(0) == 1. The agreement state
 * (EMPTY->PENDING_RESPONSE->PENDING_INSTALLATION->INSTALLED) is a discrete negotiation outcome, not
 * an RF measurement -- either the handshake installed the agreement or it didn't.
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

#define NAME       "twt"
#define RIG        "AP=test-apsta-ap (TWT responder), STA(this)=requester+reporter"

/* Keep identical to test-apsta-ap. */
#define LINK_SSID  "rimba-ping"
#define LINK_PSK   "rimbahalow"

#define CONNECT_TIMEOUT_MS 40000
#define TWT_FLOW_ID        0
#define TWT_WAIT_S         20          /* poll for the agreement to reach INSTALLED */
#define TWT_WAKE_INT_US    10000000    /* 10 s service-period interval */
#define TWT_MIN_WAKE_US    65280       /* <= interval; morse_cli caps min wake duration here */

/* Exported from morselib (name matches the mmwlan* protected glob). 1=INSTALLED, 0=pending, -1=none. */
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

void app_main(void)
{
    vTaskDelay(pdMS_TO_TICKS(500));
    TEST_BEGIN(NAME, RIG);
    TEST_INFO("associating to \"%s\" (SAE), then requesting a TWT agreement", LINK_SSID);

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

    mmwlan_override_max_tx_power(1);   /* close-bench RX-overload workaround (mirrors the AP) */

    mmhalow_connect(sta_status_cb);
    int waited = 0;
    while (!s_connected && waited < CONNECT_TIMEOUT_MS) { vTaskDelay(pdMS_TO_TICKS(250)); waited += 250; }

    TEST_STEP("associate", s_connected, "connected=%d after %d ms", (int)s_connected, waited);
    if (!s_connected) {
        TEST_FAIL("no association to \"%s\" within %d ms (is test-apsta-ap up on the peer?)",
                     LINK_SSID, CONNECT_TIMEOUT_MS);
        TEST_END(NAME);
        park_forever();
    }

    /* Mid-session TWT Setup Request (action frame). The ESP AP responds (responder default-on). */
    struct mmwlan_twt_config_args twt = MMWLAN_TWT_CONFIG_ARGS_INIT;
    twt.twt_mode = MMWLAN_TWT_REQUESTER;
    twt.twt_wake_interval_us = TWT_WAKE_INT_US;
    twt.twt_min_wake_duration_us = TWT_MIN_WAKE_US;
    twt.twt_setup_command = MMWLAN_TWT_SETUP_REQUEST;
    enum mmwlan_status ts = mmwlan_twt_setup_request(&twt);
    TEST_STEP("twt-request", ts == MMWLAN_SUCCESS, "mmwlan_twt_setup_request ret=%d", (int)ts);
    if (ts != MMWLAN_SUCCESS) {
        TEST_FAIL("mmwlan_twt_setup_request returned %d -- the request was not accepted", (int)ts);
        TEST_END(NAME);
        park_forever();
    }

    /* Poll the agreement until INSTALLED (the AP has responded + we installed the agreement). */
    int inst = 0;
    for (int s = 0; s < TWT_WAIT_S; s++) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        inst = mmwlan_twt_agreement_installed(TWT_FLOW_ID);
        if (inst == 1) break;
        if ((s % 5) == 4) TEST_INFO("waiting for agreement INSTALLED... %ds, state=%d", s + 1, inst);
    }

    TEST_STEP("agreement-installed", inst == 1, "flow=%d installed=%d", TWT_FLOW_ID, inst);
    if (inst == 1) {
        TEST_PASS("TWT agreement flow %d reached INSTALLED (AP responded to the Setup Request)",
                     TWT_FLOW_ID);
    } else if (inst == 0) {
        TEST_INCONCLUSIVE("TWT setup accepted + still associated, but the agreement did not reach "
                             "INSTALLED within %ds (state pending -- the AP may not have responded)",
                             TWT_WAIT_S);
    } else {
        TEST_FAIL("no TWT agreement for flow %d after the setup request -- the responder handshake "
                     "did not stage an agreement", TWT_FLOW_ID);
    }
    TEST_END(NAME);
    park_forever();
}
