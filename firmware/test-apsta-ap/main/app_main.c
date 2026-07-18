/*
 * test-apsta-ap — the AP (support) role of the AP<->STA association+ping T2 test.
 *
 * A test-* app so the whole T2 rig is self-contained (no product rimba-* app in the loop).
 * Brings up an 802.11ah SoftAP (SSID "rimba-ping", SAE, PMF), pins the static IP 192.168.12.1 so
 * lwIP answers ICMP, and prints a TEST| up-marker the orchestrator waits on before starting the
 * STA. This is a SUPPORT role: it does not emit a RESULT (the STA reporter does) -- it just needs to
 * come up and stay up. Derived from firmware/rimba-halow-ap/main/app_main.c (the proven bring-up).
 *
 * Link params below MUST match firmware/test-apsta-sta/main/app_main.c.
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

#define NAME       "ap-sta-ping-ap"
#define RIG        "AP support role (SoftAP)"

/* Keep identical to test-apsta-sta. */
#define LINK_SSID      "rimba-ping"
#define LINK_PSK       "rimbahalow"
#define LINK_S1G_CHAN  27
#define LINK_OP_CLASS  68
#define LINK_MAX_STAS  4
#define AP_IP          "192.168.12.1"
#define NETMASK        "255.255.255.0"

static bool assign_static_ip(void)
{
    esp_netif_t *n = mmhalow_get_netif();
    if (n == NULL) { TEST_INFO("AP netif is NULL"); return false; }
    esp_netif_dhcpc_stop(n);   /* mmhalow's netif is a DHCP client; go static */

    esp_netif_ip_info_t ip = { 0 };
    ip.ip.addr = esp_ip4addr_aton(AP_IP);
    ip.gw.addr = esp_ip4addr_aton(AP_IP);
    ip.netmask.addr = esp_ip4addr_aton(NETMASK);
    if (esp_netif_set_ip_info(n, &ip) != ESP_OK) { TEST_INFO("set_ip_info failed"); return false; }

    /* In AP mode mmhalow never fires a link-up event, so bring the netif up explicitly, or
     * lwIP won't answer ICMP. This is the key fix that makes the AP reachable over IP. */
    esp_netif_action_connected(n, NULL, 0, NULL);
    TEST_INFO("AP static IP %s, netif up=%d", AP_IP, (int)esp_netif_is_netif_up(n));
    return true;
}

void app_main(void)
{
    vTaskDelay(pdMS_TO_TICKS(500));
    TEST_BEGIN(NAME, RIG);
    TEST_INFO("bringing up SoftAP \"%s\" (SAE) on S1G ch%d", LINK_SSID, LINK_S1G_CHAN);

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_netif_init());

    mmhalow_init(NULL);
    mmhalow_print_version_info();

    mmhalow_wifi_config_t cfg = { .ap = MMWLAN_AP_ARGS_INIT };
    memcpy((char *)cfg.ap.ssid, LINK_SSID, strlen(LINK_SSID));
    cfg.ap.ssid_len = strlen(LINK_SSID);
    memcpy(cfg.ap.passphrase, LINK_PSK, strlen(LINK_PSK));
    cfg.ap.security_type = MMWLAN_SAE;
    cfg.ap.pmf_mode = MMWLAN_PMF_REQUIRED;
    cfg.ap.s1g_chan_num = LINK_S1G_CHAN;
    cfg.ap.op_class = LINK_OP_CLASS;
    cfg.ap.max_stas = LINK_MAX_STAS;
    cfg.ap.sta_status_cb = NULL;
    ESP_ERROR_CHECK(mmhalow_set_config(WIFI_IF_AP, &cfg));

    /* Close-bench RX-overload workaround: cap AP TX to 1 dBm (mirrors the STA). */
    mmwlan_override_max_tx_power(1);

    mmhalow_wifi_start();
    vTaskDelay(pdMS_TO_TICKS(1500));   /* let the netif come up before pinning the IP */

    /* Emit the up-marker (Role.up_marker="ap-ready") HERE -- BEFORE assign_static_ip. On the current
     * components/halow build the app_main task stops producing console output the instant the AP netif
     * is brought up (esp_netif_action_connected), so a marker printed AFTER that never reaches the
     * orchestrator and the AP is wrongly judged "never came up". The SoftAP itself keeps serving --
     * proven on-bench: a STA associates (SAE) and gets 15/15 ICMP replies through it. The static IP is
     * pinned microseconds later, and the orchestrator only starts flashing the STA (tens of seconds)
     * after this marker, so the AP is answering ICMP long before any association. This is an interim
     * robustness fix; the underlying app_main-stall-after-netif-up is a separate firmware issue to
     * root-cause (see docs/worklog/2026-07-18-regression-run-and-apsta-ap-marker.md). Support roles do
     * NOT emit a RESULT -- the STA reporter's RESULT is the test verdict. */
    TEST_INFO("ap-ready: SoftAP up + pinning IP %s (answers ICMP)", AP_IP);

    if (!assign_static_ip()) {
        TEST_FAIL("AP came up but could not configure its netif");
        TEST_END(NAME);
        while (1) { vTaskDelay(pdMS_TO_TICKS(10000)); }
    }

    /* Stay up so the STA can associate + ping. Never sleep (keeps the USB reflashable). */
    while (1) { vTaskDelay(pdMS_TO_TICKS(10000)); }
}
