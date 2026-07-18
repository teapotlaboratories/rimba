/*
 * rimba-halow-ap-perf — HaLow SoftAP for AP-side iperf throughput.
 *
 * Brings up an 802.11ah SAE + PMF SoftAP and pins a STATIC IP (mmhalow runs no
 * DHCP server in HaLow AP mode, so lwIP needs a fixed address to carry IP
 * traffic), then starts an esp_console REPL so you can run `iperf -s` over the
 * serial console. Pair with rimba-halow-sta-perf, which associates and runs
 * `iperf -c 192.168.12.1` as the client — together they measure the HaLow link
 * throughput from the AP side.
 *
 *   LINK PARAMS BELOW MUST MATCH firmware/rimba-halow-sta-perf/main/app_main.c.
 */

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_console.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"

#include "iperf_cmd.h"
#include "ping_cmd.h"
#include "mmhalow.h"

/* --- Link / IP parameters (keep identical to rimba-halow-sta-perf) ------------
 * Change these for your own network. */
#define LINK_SSID      "rimba-ping"
#define LINK_PSK       "rimbahalow"   /* >= 8 chars, required for SAE */
#define LINK_S1G_CHAN  27             /* US 915.5 MHz, 1 MHz BW (global op-class 68) */
#define LINK_OP_CLASS  68
#define LINK_MAX_STAS  4              /* max associated stations */
#define AP_IP          "192.168.12.1" /* SoftAP static IP / gateway; the STA's iperf target */
#define NETMASK        "255.255.255.0"
/* ---------------------------------------------------------------------------- */

static const char *TAG = "rimba-ap";

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
     * netif up — leaving lwIP unable to carry IP traffic. Bring it up
     * explicitly. This is the key fix that makes the AP reachable over IP. */
    esp_netif_action_connected(n, NULL, 0, NULL);
    ESP_LOGI(TAG, "AP static IP " IPSTR ", netif up=%d",
             IP2STR(&ip.ip), (int)esp_netif_is_netif_up(n));
}

/* Start an esp_console REPL on the serial link and register the `iperf` (+ `ping`)
 * commands, so the throughput test is driven interactively: run `iperf -s` here to
 * act as the server; the STA connects with `iperf -c 192.168.12.1`. */
static void start_iperf_console(void)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = "iperf>";
#if CONFIG_ESP_CONSOLE_UART
    esp_console_dev_uart_config_t dev_config = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&dev_config, &repl_config, &repl));
#elif CONFIG_ESP_CONSOLE_USB_CDC
    esp_console_dev_usb_cdc_config_t dev_config = ESP_CONSOLE_DEV_CDC_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_cdc(&dev_config, &repl_config, &repl));
#elif CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    esp_console_dev_usb_serial_jtag_config_t dev_config = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_serial_jtag(&dev_config, &repl_config, &repl));
#endif
    app_register_iperf_commands();
    ping_cmd_register_ping();
    ESP_ERROR_CHECK(esp_console_start_repl(repl));
    ESP_LOGW(TAG, "iperf console ready: `iperf -s` (server; the STA is the client)");
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

    ESP_ERROR_CHECK(mmhalow_set_config(WIFI_IF_AP, &cfg));

    ESP_LOGI(TAG, "Starting SoftAP...");
    mmhalow_wifi_start();

    /* Give the netif a moment to come up, then pin the static IP. */
    vTaskDelay(pdMS_TO_TICKS(1500));
    assign_static_ip();

    ESP_LOGI(TAG, "SoftAP up at %s. Run `iperf -s` on this console (STA is the client).", AP_IP);

    start_iperf_console();
}
