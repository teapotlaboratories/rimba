/*
 * test-mesh-ae — 802.11s 6-address Address-Extension (AE_A5_A6) datapath probe (S3 verification).
 *
 * Peers with a Linux mesh node (chronite) and periodically injects a PROXIED AE_A5_A6 mesh data frame
 * to it via mmwlan_mesh_send_ae_test(): mesh DA = chronite, and the Mesh Control carries two proxied
 * endpoints — eaddr1 (final DA / addr5) + eaddr2 (original SA / addr6, a synthetic "off-mesh host").
 *
 * WHAT IT PROVES (S3): the ESP builds an AE frame the MM6108 FW accepts for TX, the FW on the peer
 * DELIVERS it to the host, and the peer decrypts + parses the AE Mesh Control correctly. The gold-standard
 * check runs on the Linux side: `iw dev wlan1 mpp dump` on chronite must list eaddr2 (the proxied source)
 * as reachable via THIS node's mesh MAC — i.e. mac80211's ieee80211_rx_mesh_data ran mpp_path_add() on our
 * frame, which only happens if the AE bytes are exactly what Linux expects. That resolves the "does the FW
 * deliver/accept AE frames" open risk AND proves byte-correctness against the reference implementation.
 * (mesh data is encrypted, so a passive monitor can't byte-diff the plaintext Mesh Control — the receiver
 * decoding it is the verification.)
 *
 * BENCH: bring chronite up as a plain rimba-smesh mesh node (wpa_supplicant_s1g, NO gate mode needed);
 * flash this to one ESP; then `ssh chronite 'iw dev wlan1 mpp dump'`. MUST use MESH_ID "rimba-smesh" + SAE
 * "rimbamesh2026" (compiled into morselib) to peer. Radio-silent cleanup after.
 *
 * Derived from firmware/test-mesh-gate-rx.
 */

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "nvs_flash.h"
#include "esp_netif.h"

#include "mmhalow.h"
#include "mmwlan.h"
#include "umac/mesh/umac_mesh.h"

#ifndef TEST_MESH_ID
#define TEST_MESH_ID    "rimba-smesh"   /* override to "rimba-mesh" to target the gate app */
#endif
#define MESH_ID         TEST_MESH_ID
#define MESH_S1G_CHAN   27
#define MESH_MAX_PLINKS 16
/* eaddr1 (the proxied final DA) is overridable so the S5b test can set it to the gate's AP client MAC. */
#ifndef TEST_EADDR1
#define TEST_EADDR1     "02:00:00:00:00:bb"
#endif

/* The Linux mesh peer (mesh DA of the AE frame). Build-time overridable (defaults to chronite). */
#ifndef TEST_LINUX_MAC
#define TEST_LINUX_MAC "3c:22:7f:37:51:38"   /* chronite wlan1 */
#endif

/* Synthetic proxied endpoints carried in the AE Mesh Control. Locally-administered so they can't collide
 * with a real bench MAC. eaddr2 (the proxied SA) is what chronite's `iw mpp dump` should show via us. */
static uint8_t EADDR1_DA[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0xbb }; /* addr5 — proxied final DA (TEST_EADDR1) */
static const uint8_t EADDR2_SA[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0xaa }; /* addr6 — proxied source   */

/* A benign payload: LLC/SNAP (IP ethertype) + 20 dummy bytes, so the receiver's 802.11->802.3 conversion
 * has a well-formed SNAP after the Mesh Control (mpp learning happens on the mesh header regardless). */
static const uint8_t AE_PAYLOAD[8 + 20] = { 0xaa, 0xaa, 0x03, 0x00, 0x00, 0x00, 0x08, 0x00 };

static const char *TAG = "test-mesh-ae";
static uint8_t g_mesh_mac[6];
static uint8_t LINUX_MAC[6];

static void parse_mac(const char *s, uint8_t out[6])
{
    unsigned v[6] = { 0 };
    if (sscanf(s, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) == 6)
        for (int i = 0; i < 6; i++) out[i] = (uint8_t)v[i];
}

/* Auto-peer trigger: open a plink to any neighbour beaconing our mesh ID (== test-mesh-gate-rx). */
enum mmwlan_frame_filter_flag { MMWLAN_FRAME_BEACON = 1 << 8 };
struct mmwlan_rx_frame_info {
    enum mmwlan_frame_filter_flag frame_filter_flag;
    const uint8_t *buf; uint32_t buf_len; uint16_t freq_100khz; int16_t rssi_dbm; uint8_t bw_mhz;
};
typedef void (*mmwlan_rx_frame_cb_t)(const struct mmwlan_rx_frame_info *rx_info, void *arg);
extern enum mmwlan_status mmwlan_register_rx_frame_cb(uint32_t filter,
                                                      mmwlan_rx_frame_cb_t callback, void *arg);
#define DOT11_IE_MESH_ID  114
#define BEACON_IE_OFFSET  (24 + 12)

static const uint8_t *find_ie(const uint8_t *ies, uint32_t len, uint8_t want, uint8_t *out_len)
{
    uint32_t i = 0;
    while (i + 2 <= len) {
        uint8_t id = ies[i], ie_len = ies[i + 1];
        if (i + 2 + ie_len > len) break;
        if (id == want) { *out_len = ie_len; return &ies[i + 2]; }
        i += 2 + ie_len;
    }
    return NULL;
}
static void peer_beacon_cb(const struct mmwlan_rx_frame_info *info, void *arg)
{
    (void)arg;
    if (info->buf_len <= BEACON_IE_OFFSET) return;
    const uint8_t *sa = info->buf + 10;
    if (memcmp(sa, g_mesh_mac, 6) == 0) return;
    uint8_t id_len = 0;
    const uint8_t *id = find_ie(info->buf + BEACON_IE_OFFSET, info->buf_len - BEACON_IE_OFFSET,
                                DOT11_IE_MESH_ID, &id_len);
    if (id && id_len == strlen(MESH_ID) && memcmp(id, MESH_ID, id_len) == 0)
        mmwlan_mesh_peer_open(sa);
}

static int has_peer(uint8_t macs[][6], uint8_t n, const uint8_t *want)
{
    for (uint8_t i = 0; i < n; i++) if (memcmp(macs[i], want, 6) == 0) return 1;
    return 0;
}

void app_main(void)
{
    parse_mac(TEST_LINUX_MAC, LINUX_MAC);
    parse_mac(TEST_EADDR1, EADDR1_DA); /* S5b: eaddr1 = the gate's AP client MAC (else the default) */
    ESP_LOGI(TAG, "=== 802.11s AE probe — mesh=\"%s\" peer=%s eaddr1=%s ===", MESH_ID, TEST_LINUX_MAC,
             TEST_EADDR1);

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_netif_init());

    esp_read_mac(g_mesh_mac, ESP_MAC_WIFI_STA);
    g_mesh_mac[0] = (g_mesh_mac[0] | 0x02) & 0xFE;

    mmhalow_init(NULL);
    mmhalow_print_version_info();

    struct mmwlan_mesh_args args = { 0 };
    memcpy(args.if_addr, g_mesh_mac, sizeof(g_mesh_mac));
    memcpy(args.mesh_id, MESH_ID, strlen(MESH_ID));
    args.mesh_id_len = strlen(MESH_ID);
    args.s1g_chan_num = MESH_S1G_CHAN;
    args.beacon_interval_tu = 100;
    args.max_plinks = MESH_MAX_PLINKS;

    ESP_LOGI(TAG, "joining \"%s\" as mac=%02x:%02x:%02x:%02x:%02x:%02x", MESH_ID,
             g_mesh_mac[0], g_mesh_mac[1], g_mesh_mac[2], g_mesh_mac[3], g_mesh_mac[4], g_mesh_mac[5]);
    if (mmwlan_mesh_start(&args) != MMWLAN_SUCCESS) { ESP_LOGE(TAG, "mesh_start FAILED"); return; }
    mmwlan_override_max_tx_power(1);
    mmwlan_register_rx_frame_cb(MMWLAN_FRAME_BEACON, peer_beacon_cb, NULL);
    ESP_LOGI(TAG, "mesh up; waiting to peer with the Linux node, then injecting AE frames...");

    uint32_t s = 0;
    bool peered = false;
    for (;;)
    {
        vTaskDelay(pdMS_TO_TICKS(1000));
        s++;
        uint8_t peer_macs[UMAC_MESH_MAX_PEERS][6] = {{0}};
        uint8_t n_peers = mmwlan_mesh_peer_count(peer_macs);
        bool have_linux = has_peer(peer_macs, n_peers, LINUX_MAC);
        if (have_linux && !peered)
        {
            peered = true;
            ESP_LOGI(TAG, "==> peered with the Linux node; now injecting AE_A5_A6 frames every 3s. "
                     "Check `iw dev wlan1 mpp dump` on the Linux node for 02:00:00:00:00:aa via me.");
        }
        if (peered && (s % 3 == 0))
        {
            bool ok = mmwlan_mesh_send_ae_test(LINUX_MAC, EADDR1_DA, EADDR2_SA,
                                               AE_PAYLOAD, sizeof(AE_PAYLOAD));
            ESP_LOGI(TAG, "AE tx to Linux: %s (eaddr1=02:..:bb DA, eaddr2=02:..:aa SA)",
                     ok ? "sent" : "FAILED (not peered?)");
        }
        if (s % 5 == 0)
            ESP_LOGI(TAG, "uptime=%" PRIu32 "s estab_peers=%u linux_peered=%d", s,
                     (unsigned)n_peers, (int)peered);
    }
}
