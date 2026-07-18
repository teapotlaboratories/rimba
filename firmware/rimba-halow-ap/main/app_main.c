/*
 * rimba-halow-ap — 802.11ah (Wi-Fi HaLow) SoftAP example.
 *
 * Brings up an MM6108 SoftAP secured with SAE + PMF, gives its netif a STATIC IP
 * (mmhalow runs no DHCP server in AP mode) so lwIP answers ICMP, and logs each
 * station as it joins or leaves. Pair it with the rimba-halow-sta example on a
 * second board, which associates and pings this AP:
 *
 *   make flash APP=rimba-halow-ap  BOARD=proto1-fgh100m PORT=/dev/ttyACM0   # this AP
 *   make flash APP=rimba-halow-sta BOARD=proto1-fgh100m PORT=/dev/ttyACM1   # the STA
 *   make monitor APP=rimba-halow-ap BOARD=proto1-fgh100m PORT=/dev/ttyACM0
 *
 * This example carries a high-density AP config — up to 255 associated stations,
 * with the per-STA state routed to PSRAM (see sdkconfig.defaults). Change
 * LINK_SSID / LINK_PSK and the IP below for your own network.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "nvs_flash.h"

#include "mmhalow.h"

/* --- Link / IP parameters (change these for your own network) ---------------- */
#define LINK_SSID      "rimba-ping"     /* SoftAP SSID — keep in sync with rimba-halow-sta */
#define LINK_PSK       "rimbahalow"     /* >= 8 chars, required for SAE (change it) */
#define LINK_S1G_CHAN  27               /* US 915.5 MHz, 1 MHz BW (global op-class 68) */
#define LINK_OP_CLASS  68
#define LINK_MAX_STAS  255              /* admit up to 255 STAs — matches CONFIG_HALOW_AP_MAX_STAS
                                           in sdkconfig.defaults (also the max of max_stas) */

#define AP_IP          "192.168.12.1"   /* SoftAP static IP / gateway (change for your network) */
#define NETMASK        "255.255.255.0"
/* ---------------------------------------------------------------------------- */

static const char *TAG = "rimba-ap";

/* Fires as stations join and leave the SoftAP. Here we just log each event; a real
 * application would track its connected stations from this callback. */
static void ap_sta_status_cb(const struct mmwlan_ap_sta_status *st, void *arg)
{
    (void)arg;
    if (st == NULL) return;
    if (st->state == MMWLAN_AP_STA_AUTHORIZED)
        ESP_LOGI(TAG, "STA " MACSTR " authorized (aid=%u)", MAC2STR(st->mac_addr), (unsigned)st->aid);
    else if (st->state == MMWLAN_AP_STA_UNKNOWN)
        ESP_LOGI(TAG, "STA " MACSTR " left", MAC2STR(st->mac_addr));
}

static void assign_static_ip(void)
{
    esp_netif_t *n = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (n == NULL) {
        ESP_LOGE(TAG, "netif WIFI_STA_DEF not found");
        return;
    }
    esp_netif_dhcpc_stop(n);   /* mmhalow's netif is a DHCP client; go static */

    esp_netif_ip_info_t ip = { 0 };
    ip.ip.addr = esp_ip4addr_aton(AP_IP);
    ip.gw.addr = esp_ip4addr_aton(AP_IP);
    ip.netmask.addr = esp_ip4addr_aton(NETMASK);
    ESP_ERROR_CHECK(esp_netif_set_ip_info(n, &ip));

    /* In AP mode mmhalow never fires a link-up event, so it never brings the
     * netif up — leaving lwIP unable to answer ICMP. Bring it up explicitly.
     * This is the key fix that makes the AP reachable over IP. */
    esp_netif_action_connected(n, NULL, 0, NULL);
    ESP_LOGI(TAG, "AP static IP " IPSTR ", netif up=%d — answers ICMP",
             IP2STR(&ip.ip), (int)esp_netif_is_netif_up(n));
}

void app_main(void)
{
    ESP_LOGI(TAG, "Booting HaLow SoftAP (ssid=\"%s\" chan=%d)...", LINK_SSID, LINK_S1G_CHAN);

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_netif_init());

    mmhalow_init(NULL);
    ESP_LOGI(TAG, "Wi-Fi HaLow initialised");
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
    cfg.ap.sta_status_cb = ap_sta_status_cb;

    ESP_ERROR_CHECK(mmhalow_set_config(WIFI_IF_AP, &cfg));

    ESP_LOGI(TAG, "Starting SoftAP...");
    mmhalow_wifi_start();

    /* Give the netif a moment to come up, then pin the static IP. */
    vTaskDelay(pdMS_TO_TICKS(1500));
    assign_static_ip();

    ESP_LOGI(TAG, "SoftAP up at %s — answering ICMP, waiting for stations.", AP_IP);
}
