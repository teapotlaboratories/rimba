/*
 * rimba-halow-meshscan — on-air verification for the 802.11s mesh node.
 *
 * Runs a looping scan (to keep the chip RX on across channels) and registers a
 * raw-frame callback for beacons. For every beacon it walks the information
 * elements looking for the Mesh ID element (IEEE 802.11 element ID 114). When it
 * finds one whose value matches our Mesh ID it logs an on-air match (BSSID, RSSI,
 * channel) and hexdumps the whole frame so the Mesh ID + Mesh Configuration (113)
 * IEs can be decoded and checked against what firmware/rimba-halow-mesh transmits.
 *
 * Pair this with firmware/rimba-halow-mesh on another board.
 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "mmhalow.h"
#include "mmwlan.h"
#include "nvs_flash.h"

static const char *TAG = "meshscan";

/* Must match firmware/rimba-halow-mesh's MESH_ID. */
#define MESH_ID "rimba-mesh"

/* 802.11 element IDs. */
#define DOT11_IE_MESH_CONFIGURATION (113)
#define DOT11_IE_MESH_ID            (114)

/* Beacon body fixed fields after the 24-byte PV0 management header:
 * timestamp(8) + beacon interval(2) + capability(2) = 12, then the IEs. */
#define BEACON_IE_OFFSET (24 + 12)

/* Raw-frame capture hook (exported via the mmwlan* symbol rule; the API lives in
 * morselib's src/internal, so declare it locally — layout must match
 * mmwlan_internal.h, same as firmware/rimba-halow-scan does). */
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

/* Return a pointer to the value of the first IE with id `want` in [ies, ies+len),
 * and write its length to *out_len. NULL if not present / malformed. */
static const uint8_t *find_ie(const uint8_t *ies, uint32_t len, uint8_t want, uint8_t *out_len)
{
    uint32_t i = 0;
    while (i + 2 <= len) {
        uint8_t id = ies[i];
        uint8_t ie_len = ies[i + 1];
        if (i + 2 + ie_len > len) {
            break; /* truncated */
        }
        if (id == want) {
            *out_len = ie_len;
            return &ies[i + 2];
        }
        i += 2 + ie_len;
    }
    return NULL;
}

static void sniff_cb(const struct mmwlan_rx_frame_info *info, void *arg)
{
    /* A3 (BSSID) is at offset 16 in the legacy mgmt header. */
    const uint8_t *bssid = (info->buf_len >= 22) ? info->buf + 16 : NULL;

    /* DIAG: log EVERY beacon so we can see what is on air and where the IEs sit. */
    ESP_LOGI(TAG, "BEACON len=%lu rssi=%d freq=%u00kHz bw=%uMHz "
             "BSSID=%02x:%02x:%02x:%02x:%02x:%02x",
             (unsigned long)info->buf_len, info->rssi_dbm, info->freq_100khz, info->bw_mhz,
             bssid ? bssid[0]:0, bssid ? bssid[1]:0, bssid ? bssid[2]:0,
             bssid ? bssid[3]:0, bssid ? bssid[4]:0, bssid ? bssid[5]:0);

    if (info->buf_len <= BEACON_IE_OFFSET) {
        return;
    }
    const uint8_t *ies = info->buf + BEACON_IE_OFFSET;
    uint32_t ies_len = info->buf_len - BEACON_IE_OFFSET;

    uint8_t mesh_id_len = 0;
    const uint8_t *mesh_id = find_ie(ies, ies_len, DOT11_IE_MESH_ID, &mesh_id_len);
    if (mesh_id != NULL) {
        bool ours = (mesh_id_len == strlen(MESH_ID)) &&
                    (memcmp(mesh_id, MESH_ID, mesh_id_len) == 0);
        uint8_t cfg_len = 0;
        const uint8_t *cfg = find_ie(ies, ies_len, DOT11_IE_MESH_CONFIGURATION, &cfg_len);
        ESP_LOGI(TAG, "  ^ MESH%s MeshID=\"%.*s\" MeshCfgIE=%s",
                 ours ? " <== rimba-mesh MATCH" : "",
                 (int)mesh_id_len, (const char *)mesh_id, cfg ? "present" : "MISSING");
    }

    /* Always hexdump so the IE layout can be decoded by hand. */
    ESP_LOG_BUFFER_HEXDUMP(TAG, info->buf, info->buf_len, ESP_LOG_INFO);
}

static void scan_cb(enum mmwlan_scan_state state, void *args)
{
    /* Quietly loop; the sniffer does the reporting. */
}

/* Per-scan-result callback — unused (the raw-frame sniffer does the work), but
 * provided as a non-NULL stub so the scan path never dereferences a null cb. */
static void scan_result_cb(const struct mmwlan_scan_result *entry, void *args)
{
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== 802.11s mesh on-air verification (looking for Mesh ID \"%s\") ===", MESH_ID);

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_netif_init());

    mmhalow_init(NULL);
    mmhalow_print_version_info();

    mmwlan_register_rx_frame_cb(MMWLAN_FRAME_BEACON, sniff_cb, NULL);

    struct mmhalow_scan_args scargs = {
        .complete_cb = scan_cb,
        .rx_cb = scan_result_cb,
    };

    /* Re-scan on a loop so non-interactive capture doesn't race a one-shot scan,
     * and so the chip keeps dwelling across channels to catch the mesh channel. */
    for (;;) {
        mmhalow_scan(&scargs);
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}
