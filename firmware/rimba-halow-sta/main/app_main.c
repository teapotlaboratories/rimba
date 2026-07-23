/*
 * rimba-halow-sta — join an 802.11ah HaLow SoftAP and ping across it (the station example).
 *
 * Brings up the MM6108 radio, associates to a HaLow SoftAP over SAE, and pings continuously,
 * printing the round-trip time. By DEFAULT it is a DHCP client: against the all-ESP mesh-gate
 * (rimba-halow-mesh-ap) whose SoftAP serves DHCP on the flat 10.9.9.0/24, it leases a 10.9.9.x and
 * pings a mesh node zero-config (the gate L2-bridges + proxy-ARP resolves) — no static IP, no route.
 * For an AP with no DHCP server (the plain rimba-halow-ap demo) pass a static IP:
 *
 *   # against the mesh-gate (default: DHCP + ping mesh node 10.9.9.100):
 *   make flash APP=rimba-halow-sta BOARD=proto1-fgh100m PORT=/dev/ttyACM2
 *   # against a plain rimba-halow-ap (static IP, ping the AP):
 *   make flash APP=rimba-halow-sta ... STA_IP=192.168.12.2 PING_IP=192.168.12.1
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <inttypes.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "ping/ping_sock.h"
#include <stdio.h>
#include "lwip/etharp.h"

#include "mmhalow.h"
#include "mmwlan.h"

static const char *TAG = "rimba-halow-sta";

/* Match the AP (rimba-halow-ap / the mesh-gate SoftAP). */
#define LINK_SSID  "rimba-ping"
#define LINK_PSK   "rimbahalow"

/* IP acquisition. The mesh-gate SoftAP serves DHCP on the flat 10.9.9.0/24, so by DEFAULT this STA is a
 * DHCP CLIENT — zero-config: it leases a 10.9.9.x and reaches any mesh node directly (the gate L2-bridges
 * + proxy-ARP resolves). For an AP with NO DHCP server (the plain rimba-halow-ap demo) or a fixed-address
 * test, pass -D TEST_STATIC_IP="a.b.c.d" (make ... STA_IP=a.b.c.d). Ping target defaults to mesh node D
 * (rimba-halow-mesh derives 10.9.9.100); override with -D TEST_PING_IP="a.b.c.d" (make ... PING_IP=...). */
#ifdef TEST_PING_IP
#define PING_TARGET TEST_PING_IP
#else
#define PING_TARGET "10.9.9.100"
#endif
#ifdef TEST_STATIC_IP
#define STA_IP  TEST_STATIC_IP
#define NETMASK "255.255.255.0"
#endif

#define CONNECT_TIMEOUT_MS 40000
#define PING_INTERVAL_MS   2000

static volatile bool s_connected = false;

static void sta_status_cb(enum mmwlan_sta_state st)
{
    if (st == MMWLAN_STA_CONNECTED) {
        s_connected = true;
        ESP_LOGI(TAG, "associated to \"%s\"", LINK_SSID);
    }
}

static void on_ping_success(esp_ping_handle_t hdl, void *args)
{
    uint16_t seqno;
    uint32_t elapsed_ms;
    ip_addr_t raddr;
    esp_ping_get_profile(hdl, ESP_PING_PROF_SEQNO, &seqno, sizeof(seqno));
    esp_ping_get_profile(hdl, ESP_PING_PROF_TIMEGAP, &elapsed_ms, sizeof(elapsed_ms));
    esp_ping_get_profile(hdl, ESP_PING_PROF_IPADDR, &raddr, sizeof(raddr)); /* the actual responder */
    ESP_LOGI(TAG, "reply from %s seq=%u time=%" PRIu32 " ms", ip4addr_ntoa(&raddr.u_addr.ip4), seqno,
             elapsed_ms);
}

static void on_ping_timeout(esp_ping_handle_t hdl, void *args)
{
    ESP_LOGW(TAG, "ping timeout");
}

#ifdef TEST_STATIC_IP
/* Pin a STATIC IP — for an AP with no DHCP server (the plain rimba-halow-ap demo) or a fixed-address
 * test. mmhalow's STA netif is a DHCP client, so stop it first. On the flat single subnet there is no
 * L3 gateway, so gw = 0. */
static bool assign_static_ip(void)
{
    esp_netif_t *n = mmhalow_get_netif();
    if (n == NULL) {
        ESP_LOGE(TAG, "STA netif is NULL");
        return false;
    }
    esp_netif_dhcpc_stop(n);

    esp_netif_ip_info_t ip = { 0 };
    ip.ip.addr      = esp_ip4addr_aton(STA_IP);
    ip.gw.addr      = 0;
    ip.netmask.addr = esp_ip4addr_aton(NETMASK);
    if (esp_netif_set_ip_info(n, &ip) != ESP_OK) {
        ESP_LOGE(TAG, "set_ip_info failed");
        return false;
    }
    esp_netif_action_connected(n, NULL, 0, NULL);
    ESP_LOGI(TAG, "STA static IP %s", STA_IP);
    return true;
}
#else
/* DHCP client (default): mmhalow's STA netif is ESP_NETIF_DEFAULT_WIFI_STA (AUTOUP + dhcpc) and mmhalow
 * calls esp_netif_action_connected on link-up, so the DHCP client is already running once associated —
 * just wait for the gate's SoftAP to hand out a 10.9.9.x lease. */
static bool wait_for_dhcp_ip(void)
{
    esp_netif_t *n = mmhalow_get_netif();
    if (n == NULL) {
        ESP_LOGE(TAG, "STA netif is NULL");
        return false;
    }
    esp_netif_ip_info_t ip = { 0 };
    for (int i = 0; i < 60; i++) {   /* up to ~30 s */
        if (esp_netif_get_ip_info(n, &ip) == ESP_OK && ip.ip.addr != 0) {
            ESP_LOGI(TAG, "DHCP lease " IPSTR " — zero-config on the flat mesh subnet", IP2STR(&ip.ip));
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    ESP_LOGE(TAG, "no DHCP lease within 30 s — is the gate (rimba-halow-mesh-ap) up + serving DHCP?");
    return false;
}
#endif

static void idle_forever(void)
{
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

void app_main(void)
{
    vTaskDelay(pdMS_TO_TICKS(500));   /* let the USB-Serial-JTAG console attach */
    ESP_LOGI(TAG, "joining HaLow AP \"%s\" (SAE), then pinging %s", LINK_SSID, PING_TARGET);

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

    mmhalow_connect(sta_status_cb);
    int waited = 0;
    while (!s_connected && waited < CONNECT_TIMEOUT_MS) {
        vTaskDelay(pdMS_TO_TICKS(250));
        waited += 250;
    }
    if (!s_connected) {
        ESP_LOGE(TAG, "no association to \"%s\" within %d ms -- is the AP (rimba-halow-ap) up?",
                 LINK_SSID, CONNECT_TIMEOUT_MS);
        idle_forever();
    }

#ifdef TEST_STATIC_IP
    if (!assign_static_ip()) {
        idle_forever();
    }
#else
    if (!wait_for_dhcp_ip()) {
        idle_forever();
    }
#endif

#ifdef TEST_NO_PING
    /* B2 test — responder-only: associate + hold a static IP, but DON'T originate any traffic (so this
     * client never ARPs and never teaches a mesh node its address). A mesh node then INITIATES to us,
     * forcing the gate to bridge the mesh node's broadcast ARP down to the AP (mesh->AP bridging = B2). */
    ESP_LOGI(TAG, "responder-only (no ping) — waiting for a mesh node to reach us");
    idle_forever();
#else
    /* Ping continuously so the console shows the live link (the AP, or in round-trip mode a mesh node). */
    ip_addr_t target = { 0 };
    target.type = IPADDR_TYPE_V4;
    target.u_addr.ip4.addr = esp_ip4addr_aton(PING_TARGET);

    esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
    cfg.target_addr = target;
    cfg.count = ESP_PING_COUNT_INFINITE;
    cfg.interval_ms = PING_INTERVAL_MS;

    esp_ping_callbacks_t cbs = {
        .on_ping_success = on_ping_success,
        .on_ping_timeout = on_ping_timeout,
        .on_ping_end = NULL,
        .cb_args = NULL,
    };
    esp_ping_handle_t ping;
    if (esp_ping_new_session(&cfg, &cbs, &ping) != ESP_OK) {
        ESP_LOGE(TAG, "failed to create the ping session");
        idle_forever();
    }
    ESP_LOGI(TAG, "pinging %s every %d ms (zero-config via the gate L2 bridge + proxy-ARP)...",
             PING_TARGET, PING_INTERVAL_MS);
    esp_ping_start(ping);

    idle_forever();
#endif
}
