/*
 * rimba-halow-mesh-monitor — an 802.11s mesh-beacon monitor for MM6108 / HaLow.
 *
 * Why this exists: there is no usable external HaLow sniffer here — the Linux
 * morse_driver's monitor mode delivers no frames, and the MM6108 firmware only
 * surfaces *foreign* (other-BSSID) beacons to the host when the local vif is in
 * MESH mode (it does NOT surface IBSS or AP beacons — so this is a mesh monitor,
 * not a universal sniffer; see README.md). This app brings a mesh vif up on a
 * chosen channel and registers morselib's promiscuous monitor hook
 * (mmwlan_register_monitor_cb), which fires for every frame the firmware delivers
 * — *before* the datapath's BSSID filter discards it. It then prints a one-line
 * summary of each frame (type, addresses, SSID / Mesh ID, RSSI).
 *
 * The RX hook runs on the driver task, so it does the bare minimum (parse a few
 * fields, enqueue) and a separate task does the printing.
 */

#include <inttypes.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "nvs_flash.h"
#include "esp_netif.h"

#include "mmhalow.h"
#include "umac/mesh/umac_mesh.h"

/* morselib promiscuous monitor hook. Declared locally (the API lives in morselib's
 * src/internal and is link-visible via the mmwlan* symbol rule, exactly like
 * mmwlan_register_rx_frame_cb is used by rimba-halow-scan). Layout MUST match
 * src/internal/mmwlan_internal.h. */
struct mmwlan_monitor_rx_info {
    const uint8_t *buf;
    uint32_t buf_len;
    uint16_t freq_100khz;
    int16_t rssi_dbm;
    uint8_t bw_mhz;
};
typedef void (*mmwlan_monitor_rx_cb_t)(const struct mmwlan_monitor_rx_info *info, void *arg);
extern enum mmwlan_status mmwlan_register_monitor_cb(mmwlan_monitor_rx_cb_t callback, void *arg);

/* ---- configuration ---------------------------------------------------------- */
#define MON_S1G_CHAN   27           /* S1G channel to listen on (US 915.5 MHz, 1 MHz). */
#define MON_MESH_ID    "rimba-mon"  /* mesh ID for our listening vif (see README). */

static const char *TAG = "meshmon";

/* ---- 802.11 frame parsing --------------------------------------------------- */
#define FC_TYPE(fc)    (((fc) >> 2) & 0x3)
#define FC_SUBTYPE(fc) (((fc) >> 4) & 0xf)
#define DOT11_IE_SSID    (0)
#define DOT11_IE_MESH_ID (114)

/* Compact, queue-friendly summary of one received frame. */
struct mon_evt {
    uint8_t  a2[6];
    uint8_t  a3[6];
    int16_t  rssi;
    uint16_t freq_100khz;
    uint16_t len;
    uint8_t  type;
    uint8_t  subtype;
    uint8_t  id_kind;   /* 0 none, 1 SSID, 2 Mesh ID */
    uint8_t  id_len;
    char     id[33];
};

static QueueHandle_t mon_q;
static uint8_t g_mon_mac[6];

/* mgmt-frame subtype -> short label. */
static const char *mgmt_label(uint8_t subtype)
{
    switch (subtype) {
        case 0:  return "AssocReq";
        case 1:  return "AssocResp";
        case 2:  return "ReassocReq";
        case 3:  return "ReassocResp";
        case 4:  return "ProbeReq";
        case 5:  return "ProbeResp";
        case 8:  return "Beacon";
        case 9:  return "ATIM";
        case 10: return "Disassoc";
        case 11: return "Auth";
        case 12: return "Deauth";
        case 13: return "Action";
        default: return "Mgmt?";
    }
}

static const char *frame_label(uint8_t type, uint8_t subtype)
{
    switch (type) {
        case 0: return mgmt_label(subtype);
        case 1: return "Control";
        case 2: return "Data";
        case 3: return (subtype == 1) ? "S1G-Beacon" : "Ext?";
        default: return "?";
    }
}

/* Find IE `want` in [ies, ies+len); return value pointer + length via out_len. */
static const uint8_t *find_ie(const uint8_t *ies, uint32_t len, uint8_t want, uint8_t *out_len)
{
    uint32_t i = 0;
    while (i + 2 <= len) {
        uint8_t id = ies[i], il = ies[i + 1];
        if (i + 2 + il > len) break;
        if (id == want) { *out_len = il; return &ies[i + 2]; }
        i += 2 + il;
    }
    return NULL;
}

/* Runs on the driver (RX) task — keep it minimal: parse a few fields, enqueue. */
static void monitor_cb(const struct mmwlan_monitor_rx_info *info, void *arg)
{
    (void)arg;
    if (mon_q == NULL || info->buf_len < 2) return;

    uint16_t fc = (uint16_t)(info->buf[0] | (info->buf[1] << 8));
    struct mon_evt e = { 0 };
    e.type = FC_TYPE(fc);
    e.subtype = FC_SUBTYPE(fc);
    e.rssi = info->rssi_dbm;
    e.freq_100khz = info->freq_100khz;
    e.len = (uint16_t)info->buf_len;

    if (info->buf_len >= 22) {
        memcpy(e.a2, info->buf + 10, 6);
        memcpy(e.a3, info->buf + 16, 6);
    }

    /* Beacons / probe responses carry SSID + (for mesh) Mesh ID after the 24-byte
     * legacy header + 12 bytes of fixed fields (timestamp/interval/capability). */
    if (e.type == 0 && (e.subtype == 8 || e.subtype == 5) && info->buf_len > 36) {
        const uint8_t *ies = info->buf + 36;
        uint32_t ielen = info->buf_len - 36;
        uint8_t l = 0;
        const uint8_t *mid = find_ie(ies, ielen, DOT11_IE_MESH_ID, &l);
        if (mid && l) {
            e.id_kind = 2; e.id_len = l > 32 ? 32 : l; memcpy(e.id, mid, e.id_len);
        } else {
            const uint8_t *ssid = find_ie(ies, ielen, DOT11_IE_SSID, &l);
            if (ssid) { e.id_kind = 1; e.id_len = l > 32 ? 32 : l; memcpy(e.id, ssid, e.id_len); }
        }
    }

    (void)xQueueSend(mon_q, &e, 0); /* drop if full — never block the driver task */
}

static void monitor_print_task(void *arg)
{
    (void)arg;
    struct mon_evt e;
    uint32_t n = 0;
    for (;;) {
        if (xQueueReceive(mon_q, &e, portMAX_DELAY) != pdTRUE) continue;
        const char *idkind = e.id_kind == 2 ? "mesh" : e.id_kind == 1 ? "ssid" : "";
        ESP_LOGI(TAG,
                 "#%" PRIu32 " %-10s A2=%02x:%02x:%02x:%02x:%02x:%02x "
                 "A3=%02x:%02x:%02x:%02x:%02x:%02x rssi=%-4d %u00kHz len=%-3u %s%s%.*s%s",
                 ++n, frame_label(e.type, e.subtype),
                 e.a2[0], e.a2[1], e.a2[2], e.a2[3], e.a2[4], e.a2[5],
                 e.a3[0], e.a3[1], e.a3[2], e.a3[3], e.a3[4], e.a3[5],
                 e.rssi, e.freq_100khz, e.len,
                 idkind, e.id_kind ? "=\"" : "", (int)e.id_len, e.id, e.id_kind ? "\"" : "");
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== HaLow 802.11s mesh-beacon monitor (chan %d) ===", MON_S1G_CHAN);

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_netif_init());

    /* Unique per-board MAC (the MM6108 factory MAC is shared across modules). */
    esp_read_mac(g_mon_mac, ESP_MAC_WIFI_STA);
    g_mon_mac[0] = (g_mon_mac[0] | 0x02) & 0xFE;

    mmhalow_init(NULL);
    mmhalow_print_version_info();

    mon_q = xQueueCreate(64, sizeof(struct mon_evt));
    xTaskCreate(monitor_print_task, "mon_print", 4096, NULL, 4, NULL);

    /* Bring up a mesh vif: this is the mode in which the MM6108 firmware surfaces
     * foreign beacons to the host (it does not in STA/AP/IBSS). The vif self-beacons
     * with MON_MESH_ID; pick an ID distinct from the network under test. */
    struct mmwlan_mesh_args args = { 0 };
    memcpy(args.if_addr, g_mon_mac, sizeof(g_mon_mac));
    memcpy(args.mesh_id, MON_MESH_ID, strlen(MON_MESH_ID));
    args.mesh_id_len = strlen(MON_MESH_ID);
    args.s1g_chan_num = MON_S1G_CHAN;
    args.beacon_interval_tu = 100;
    args.max_plinks = 4;

    enum mmwlan_status st = mmwlan_mesh_start(&args);
    if (st != MMWLAN_SUCCESS) {
        ESP_LOGE(TAG, "==> mmwlan_mesh_start FAILED status=%d", (int)st);
        return;
    }

    mmwlan_register_monitor_cb(monitor_cb, NULL);
    ESP_LOGW(TAG, "==> monitoring on chan %d. Note: addresses are often zeroed by the "
                  "firmware's S1G->legacy beacon conversion (see README).", MON_S1G_CHAN);

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
