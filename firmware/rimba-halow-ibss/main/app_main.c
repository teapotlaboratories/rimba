/*
 * rimba-halow-ibss — RISK-01 IBSS / ad-hoc command-acceptance test.
 *
 * EXPERIMENTAL. Brings up the MM6108 in IBSS mode via the ported
 * mmwlan_ibss_enable():
 *     ADD_INTERFACE(ADHOC) -> SET_CHANNEL -> BSSID_SET -> BSS_CONFIG -> IBSS_CONFIG
 * (sequence reverse-engineered from the MorseMicro Linux driver, morse_driver).
 *
 * Goal of THIS test: confirm the v1.17.6 chip firmware ACCEPTS the sequence
 * (especially IBSS_CONFIG, cmd 0x0035) from a clean state, and resolve the
 * EEXIST(-17) hit earlier. Watch the console for the per-command "IBSS:" status
 * lines. Beaconing, the IBSS datapath, and EtherType 0x88B5 frame exchange are
 * the NEXT increments. See docs/worklog/2026-06-18-risk01-ibss-recon.md.
 *
 * Two-node use: flash one board as CREATOR (default) and the other with
 * -DRIMBA_IBSS_CREATOR=0 to JOIN. Both MUST share the same LINK_BSSID/chan.
 */

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"

#include "mmhalow.h"
#include "mmwlan.h"

/* --- IBSS link parameters (keep identical across all nodes in the cell) ------ */
#define LINK_SSID      "rimba-ibss"
#define LINK_S1G_CHAN  27             /* US 915.5 MHz, 1 MHz BW (global op-class 68) */
#define LINK_OP_CLASS  68

/* The IBSS BSSID. MUST be identical on every node. Locally-administered
 * (bit 1 of octet 0 set), not derived per-node. */
static const uint8_t LINK_BSSID[6] = { 0x02, 0x12, 0x34, 0x56, 0x78, 0x9a };

/* Build the first board as CREATOR; flash the second with -DRIMBA_IBSS_CREATOR=0
 * to JOIN. (For the command-acceptance test, CREATE on both is also fine — each
 * board independently exercises the sequence.) */
#ifndef RIMBA_IBSS_CREATOR
#define RIMBA_IBSS_CREATOR 1
#endif
/* ---------------------------------------------------------------------------- */

static const char *TAG = "rimba-ibss";

void app_main(void)
{
    ESP_LOGI(TAG, "Booting HaLow IBSS command-acceptance test (ssid=\"%s\" chan=%d creator=%d)...",
             LINK_SSID, LINK_S1G_CHAN, RIMBA_IBSS_CREATOR);

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_netif_init());

    /* mmhalow_init boots the chip and sets the channel list, but adds NO STA/AP
     * interface — so we enter mmwlan_ibss_enable() from the clean state that
     * avoids the EEXIST(-17). */
    mmhalow_init(NULL);
    ESP_LOGI(TAG, "Wi-Fi HaLow initialised");
    mmhalow_print_version_info();

    struct mmwlan_ibss_args args = MMWLAN_IBSS_ARGS_INIT;
    memcpy(args.ssid, LINK_SSID, strlen(LINK_SSID));
    args.ssid_len = strlen(LINK_SSID);
    memcpy(args.bssid, LINK_BSSID, sizeof(args.bssid));
    args.op_class = LINK_OP_CLASS;
    args.s1g_chan_num = LINK_S1G_CHAN;
    args.creator = (RIMBA_IBSS_CREATOR != 0);
    args.start_beaconing = true;    /* host-generated IBSS beacon */

    ESP_LOGI(TAG, "Calling mmwlan_ibss_enable() [%s]...",
             args.creator ? "CREATE" : "JOIN");
    enum mmwlan_status st = mmwlan_ibss_enable(&args);
    if (st == MMWLAN_SUCCESS) {
        ESP_LOGI(TAG, "==> mmwlan_ibss_enable SUCCESS — firmware accepted the IBSS sequence");
    } else {
        ESP_LOGE(TAG, "==> mmwlan_ibss_enable FAILED status=%d (see the IBSS: lines above)", st);
    }

    /* Idle; the interesting output is the per-command "IBSS:" lines above. */
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        ESP_LOGI(TAG, "alive (creator=%d, ibss_status=%d)", RIMBA_IBSS_CREATOR, st);
    }
}
