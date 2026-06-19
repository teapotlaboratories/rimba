/*
 * Copyright 2026 Morse Micro
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "mmhalow.h"
#include "mmutils.h"
#include "mmwlan.h"
#include "nvs_flash.h"

static const char *TAG = "scan";

/* --- Raw-frame sniffer (RISK-01 diagnostic) ---------------------------------
 * morselib has an internal raw-frame capture hook, mmwlan_register_rx_frame_cb,
 * exported via the `mmwlan*` symbol rule. We use it alongside the scan (which
 * keeps the chip RX on across channels) to see whether an IBSS node's BEACON is
 * actually on air — the scanner itself only reports probe responses. Declared
 * locally because the API lives in morselib's src/internal (not the public
 * header). Layout MUST match mmwlan_internal.h. */
enum mmwlan_frame_filter_flag {
    MMWLAN_FRAME_PROBE_REQ = 1 << 4,
    MMWLAN_FRAME_PROBE_RSP = 1 << 5,
    MMWLAN_FRAME_BEACON    = 1 << 8,
};
struct mmwlan_rx_frame_info {
    enum mmwlan_frame_filter_flag frame_filter_flag;
    const uint8_t *buf;
    uint32_t buf_len;
    uint16_t freq_100khz;
    int16_t rssi_dbm;
    uint8_t bw_mhz;
};
typedef void (*mmwlan_rx_frame_cb_t)(const struct mmwlan_rx_frame_info *rx_info, void *arg);
extern enum mmwlan_status mmwlan_register_rx_frame_cb(uint32_t filter,
                                                      mmwlan_rx_frame_cb_t callback, void *arg);

/* Our IBSS BSSID (matches firmware/rimba-halow-ibss). */
static const uint8_t IBSS_BSSID[6] = { 0x02, 0x12, 0x34, 0x56, 0x78, 0x9a };

static void sniff_cb(const struct mmwlan_rx_frame_info *info, void *arg)
{
    /* 802.11 mgmt header: FC(2) Dur(2) A1(6) A2(6) A3=BSSID@16(6). */
    const uint8_t *a3 = (info->buf_len >= 22) ? info->buf + 16 : NULL;
    const uint8_t *a2 = (info->buf_len >= 16) ? info->buf + 10 : NULL;
    bool ours = a3 && (memcmp(a3, IBSS_BSSID, 6) == 0);
    ESP_LOGI(TAG, "RXFRAME flag=0x%x len=%lu freq=%u00kHz rssi=%d bw=%uMHz%s "
             "A2=%02x:%02x:%02x:%02x:%02x:%02x A3=%02x:%02x:%02x:%02x:%02x:%02x",
             (unsigned)info->frame_filter_flag, (unsigned long)info->buf_len,
             info->freq_100khz, info->rssi_dbm, info->bw_mhz,
             ours ? " <== IBSS MATCH" : "",
             a2 ? a2[0]:0, a2 ? a2[1]:0, a2 ? a2[2]:0, a2 ? a2[3]:0, a2 ? a2[4]:0, a2 ? a2[5]:0,
             a3 ? a3[0]:0, a3 ? a3[1]:0, a3 ? a3[2]:0, a3 ? a3[3]:0, a3 ? a3[4]:0, a3 ? a3[5]:0);
    /* For our own IBSS frames, hexdump the whole frame so the IEs can be decoded
     * and verified against the Linux beacon/probe-resp (net/mac80211/ibss.c). */
    if (ours) {
        ESP_LOG_BUFFER_HEXDUMP(TAG, info->buf, info->buf_len, ESP_LOG_INFO);
    }
}

static void scan_cb(enum mmwlan_scan_state state, void *args)
{
    ESP_LOGI(TAG, "Scan finished");
}

static int akm_str_len(uint32_t akm_suite)
{
    switch (akm_suite) {
        case MM_AKM_SUITE_NONE:
            return 4;
        case MM_AKM_SUITE_OTHER:
            return 5;
        default:
            return 3;
    }
}

static void rx_cb(const struct mmwlan_scan_result *entry, void *args)
{
    struct mm_rsn_information rsn = {0};
    mm_parse_rsn_information(entry->ies, entry->ies_len, &rsn);

    ESP_LOGI(TAG, "\n"
        "%.*s" "\n"
        "\t" "Operating BW: %d MHz" "\n"
        "\t" "BSSID: %02x:%02x:%02x:%02x:%02x:%02x" "\n"
        "\t" "RSSI: %3d" "\n"
        "\t" "Beacon Interval (TUs): %u" "\n"
        "\t" "Capability Info: 0x%04x" "\n"
        "\t" "Security: %.*s %.*s" "\n"
        ,
        entry->ssid_len,
        entry->ssid,
        entry->op_bw_mhz,
        entry->bssid[0],
        entry->bssid[1],
        entry->bssid[2],
        entry->bssid[3],
        entry->bssid[4],
        entry->bssid[5],
        entry->rssi,
        entry->beacon_interval,
        entry->capability_info,
        rsn.num_akm_suites ? akm_str_len(rsn.akm_suites[0]) : 4,
        rsn.num_akm_suites ? mm_akm_suite_to_string(rsn.akm_suites[0]) : "None",
        rsn.num_akm_suites > 1 ? akm_str_len(rsn.akm_suites[1]) : 0,
        rsn.num_akm_suites > 1 ? mm_akm_suite_to_string(rsn.akm_suites[1]) : ""
    );
}

void app_main(void)
{
    ESP_LOGI(TAG, "Booted. Initializing Wi-Fi HaLow...");

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_netif_init());

    mmhalow_init(NULL);

    ESP_LOGI(TAG, "Wi-Fi HaLow Initialised");

    mmhalow_print_version_info();

    /* Capture raw beacons/probes over the air (RISK-01 IBSS diagnostic). */
    mmwlan_register_rx_frame_cb(MMWLAN_FRAME_BEACON | MMWLAN_FRAME_PROBE_RSP |
                                MMWLAN_FRAME_PROBE_REQ, sniff_cb, NULL);

    struct mmhalow_scan_args scargs = {
        .complete_cb = scan_cb,
        .rx_cb = rx_cb,
    };

    /* Re-scan on a loop so non-interactive capture doesn't have to race a
     * one-shot boot scan. */
    for (;;) {
        mmhalow_scan(&scargs);
        vTaskDelay(pdMS_TO_TICKS(4000));
    }
}
