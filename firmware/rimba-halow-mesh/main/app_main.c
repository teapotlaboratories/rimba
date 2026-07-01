/*
 * rimba-halow-mesh — 802.11s mesh bring-up (P1: mesh vif up + beacon).
 *
 * Brings up a single 802.11s mesh interface and self-beacons a mesh beacon
 * (zero-length SSID + Mesh ID + Mesh Configuration IEs), following morse_driver's
 * mesh BSS config flow (BSS_CONFIG -> BSSID_SET -> BSS_BEACON_CONFIG ->
 * MESH_CONFIG(START)). Peering / HWMP / forwarding come in later phases.
 *
 * Test: flash this on two boards. Each derives a unique MAC from its ESP32 efuse
 * MAC, beacons on the mesh channel, and the peer-beacon sniffer below logs the
 * OTHER node's beacon — proving the firmware beacons on air. See
 * docs/worklog/2026-06-25-mesh-p1-vif-beacon.md (P1).
 */

#include <inttypes.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "nvs_flash.h"
#include "esp_netif.h"

#include "ping/ping_sock.h"
#include "lwip/etharp.h"

#include "mmhalow.h"
#include "umac/mesh/umac_mesh.h"

/* Keep in sync with the other rimba apps' link params. */
#define MESH_ID         "rimba-mesh"
#define MESH_S1G_CHAN   27   /* US 915.5 MHz, 1 MHz BW (global op-class 68) */
#define MESH_MAX_PLINKS 10

/* Linux-interop capture (temporary): bring this ESP up as an OPEN mesh peer (no allowlist) that
 * pings the Linux node (chronite, 10.9.9.2). Used to capture the Linux PREP/PERR on-air and to
 * verify the ESP's own PREQ/group frames against the live Linux device. Mutually exclusive with
 * the line-relay demo; uncomment for a Linux-interop on-air A/B capture. */
// #define MESH_LINUX_INTEROP 1

/* Dynamic-join test mode (optional; uncomment to use). NO peer allowlist — peer with ANYONE in RF range
 * (RF geometry, not policy, shapes the topology). Verified 2026-06-29: a node reset out of the mesh
 * REBOOTS and re-joins the running secured mesh on its own (re-SAE/AMPE/keys, pingable) with no restart of
 * the others. NOTE: RF-forcing a multi-hop LINE via low TX power did NOT work on this bench (boards too
 * close: at 1 dBm board1<->board2 is still ~-52 dBm, ~40 dB above sensitivity — needs physical separation;
 * board0 isn't between them so it just gets isolated). And the unconstrained full-mesh is flappier than the
 * forced 2-link topology (peering/path-selection churn) — a stability hardening follow-up. Use
 * MESH_LINE_RELAY_DEMO for a stable multi-hop demo. */
// #define MESH_DYNAMIC_RF 1
#ifdef MESH_DYNAMIC_RF
/* TX-power cap (dBm). 0 = no cap (full power; RF-forcing didn't work, see above). */
#define MESH_TX_POWER_DBM 0
#endif

/* Multi-hop relay demo (temporary): force a 3-ESP line board1 -- board0(relay) -- board2 so
 * board0 must FORWARD board1<->board2 traffic (ESP as an intermediate hop). Each node sets a
 * peer allowlist by its own MAC. Comment out MESH_LINE_RELAY_DEMO for normal operation. */
/* Default (no test mode defined) = a clean secured-mesh node: joins the mesh, gets its IP, is pingable
 * (responder), with NO forced allowlist and NO hardcoded peers. Uncomment MESH_LINE_RELAY_DEMO (or another
 * mode above) only for a bench-specific demo with hardcoded board MACs. */
// #define MESH_LINE_RELAY_DEMO 1
/* Leaf-node bench test (use WITH MESH_LINE_RELAY_DEMO): board0 calls mmwlan_mesh_set_multihop(false)
 * after start -> pure single-hop leaf, stops relaying board1<->board2. Uncomment to flash board0 as a
 * leaf (Test B); commented = board0 relays (Test A baseline). On-air A/B verified on chronium 2026-06-30. */
// #define MESH_LEAF_NODE 1
#if defined(MESH_LINE_RELAY_DEMO) || defined(MESH_DYNAMIC_RF)
static const uint8_t MAC_B0[6] = { 0xe2, 0x72, 0xa1, 0xf8, 0xef, 0xa4 }; /* relay (10.9.9.136) */
static const uint8_t MAC_B1[6] = { 0xe2, 0x72, 0xa1, 0xf8, 0xf9, 0x40 }; /* endpoint (10.9.9.100) */
static const uint8_t MAC_B2[6] = { 0xe2, 0x72, 0xa1, 0xf8, 0xf0, 0x08 }; /* endpoint (10.9.9.108) */
#endif
/* XV-CHRONOSALT test mode (optional; uncomment to use). board0 relays board1 <-> chronosalt (Linux Pi Zero,
 * mesh 10.9.9.3) via the forced allowlist. VERIFIED 2026-06-29: the #22 cross-vendor multi-hop fix works
 * with chronosalt too — 46 replies / 2 timeouts, board0 forwarding both ways, chronosalt's unprotected PREPs
 * handled — confirming #22 is not chronite-specific (generalises to any MFP=no Linux mesh peer). Bring up
 * chronosalt first: ssh chronosalt; the secured-mesh recipe with ~/wpa-interop.conf + `ip addr add 10.9.9.3`. */
// #define MESH_XV_CHRONOSALT 1
#ifdef MESH_XV_CHRONOSALT
static const uint8_t MAC_CHRONOSALT[6] = { 0x68, 0x24, 0x99, 0x44, 0x6a, 0x56 };
#endif
/* Runtime ping target (NULL = don't ping: relay + responder roles). */
static const char *g_ping_target;
/* Static ARP for the far endpoint (broadcast ARP can't traverse the relay yet). */
static const char *g_static_arp_ip;
static const uint8_t *g_static_arp_mac;

/* 802.11 element IDs + beacon IE offset (24-byte PV0 header + ts/bcn-int/cap = 12). */
#define DOT11_IE_MESH_ID        (114)
#define BEACON_IE_OFFSET        (24 + 12)

static const char *TAG = "rimba-mesh";

/* This node's mesh MAC, derived from the ESP32 efuse MAC (unique per board). */
static uint8_t g_mesh_mac[6];

/* ---- peer-beacon RX sniffer (P1 on-air verification; basis for P2 peering) -----
 * morselib's raw-frame capture hook is exported via the mmwlan* symbol rule; the
 * API lives in src/internal, so declare it locally (layout must match
 * mmwlan_internal.h, same as firmware/rimba-halow-scan). */
enum mmwlan_frame_filter_flag {
    MMWLAN_FRAME_BEACON = 1 << 8,
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
    if (info->buf_len <= BEACON_IE_OFFSET) return;
    const uint8_t *sa = info->buf + 10;   /* A2 / source address */
    if (memcmp(sa, g_mesh_mac, 6) == 0) return; /* ignore our own beacons */

    uint8_t id_len = 0;
    const uint8_t *id = find_ie(info->buf + BEACON_IE_OFFSET,
                                info->buf_len - BEACON_IE_OFFSET, DOT11_IE_MESH_ID, &id_len);
    bool ours = id && id_len == strlen(MESH_ID) && memcmp(id, MESH_ID, id_len) == 0;
    if (!id) return; /* not a mesh beacon */

    ESP_LOGI(TAG, "PEER MESH BEACON%s from %02x:%02x:%02x:%02x:%02x:%02x rssi=%d "
             "freq=%u00kHz MeshID=\"%.*s\"",
             ours ? " (rimba-mesh)" : "", sa[0], sa[1], sa[2], sa[3], sa[4], sa[5],
             info->rssi_dbm, info->freq_100khz, (int)id_len, (const char *)id);

    /* Same Mesh ID -> initiate a peer link (idempotent; morselib runs the MPM handshake
     * and ignores repeats once the peer is known). Mirrors mac80211 opening a plink on a
     * heard candidate beacon. */
    if (ours)
    {
        mmwlan_mesh_peer_open(sa);
    }
}

/* --- P4 data path bring-up: static mesh IP + ping a neighbour ------------------ */
static void on_ping_success(esp_ping_handle_t hdl, void *args)
{
    (void)args;
    uint16_t seqno; uint32_t elapsed_ms; ip_addr_t target = { 0 };
    esp_ping_get_profile(hdl, ESP_PING_PROF_SEQNO, &seqno, sizeof(seqno));
    esp_ping_get_profile(hdl, ESP_PING_PROF_TIMEGAP, &elapsed_ms, sizeof(elapsed_ms));
    esp_ping_get_profile(hdl, ESP_PING_PROF_IPADDR, &target, sizeof(target));
    ESP_LOGI(TAG, "reply from " IPSTR ": seq=%u time=%" PRIu32 " ms",
             IP2STR(&target.u_addr.ip4), seqno, elapsed_ms);
}

static void on_ping_timeout(esp_ping_handle_t hdl, void *args)
{
    (void)args;
    uint16_t seqno;
    esp_ping_get_profile(hdl, ESP_PING_PROF_SEQNO, &seqno, sizeof(seqno));
    ESP_LOGW(TAG, "ping timeout seq=%u", seqno);
}

/* Pin a static mesh IP once the netif is up (after a peer link establishes), then ping a
 * mesh neighbour (the Linux node 10.9.9.2) to exercise the mesh data path. */
static void mesh_net_task(void *arg)
{
    (void)arg;
    esp_netif_t *n = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (n == NULL) { ESP_LOGE(TAG, "mesh netif not found"); vTaskDelete(NULL); return; }

    for (int i = 0; i < 60 && !esp_netif_is_netif_up(n); i++) { vTaskDelay(pdMS_TO_TICKS(500)); }
    vTaskDelay(pdMS_TO_TICKS(500));

    esp_netif_dhcpc_stop(n);
    /* The mesh vif transmits with g_mesh_mac (the locally-administered mesh MAC); sync the
     * netif/ARP MAC to it so L2 (vif) and L3 (ARP) match — otherwise peers learn our IP against
     * the wrong MAC and replies never reach the mesh vif. */
    esp_netif_set_mac(n, g_mesh_mac);
    unsigned host = 100u + (g_mesh_mac[5] & 0x3fu); /* distinct per node, avoids .1/.2 */
    char ipbuf[20];
    snprintf(ipbuf, sizeof(ipbuf), "10.9.9.%u", host);
    const char *ping_to = g_ping_target ? g_ping_target : "10.9.9.2";
    esp_netif_ip_info_t ip = { 0 };
    ip.ip.addr = esp_ip4addr_aton(ipbuf);
    ip.gw.addr = esp_ip4addr_aton(ping_to);
    ip.netmask.addr = esp_ip4addr_aton("255.255.255.0");
    ESP_ERROR_CHECK(esp_netif_set_ip_info(n, &ip));
    ESP_LOGI(TAG, "mesh static IP %s (netif up=%d)%s%s", ipbuf, (int)esp_netif_is_netif_up(n),
             g_ping_target ? "; pinging " : " (no ping — relay/responder)",
             g_ping_target ? g_ping_target : "");

    if (g_static_arp_mac != NULL)
    {
        ip4_addr_t a = { .addr = esp_ip4addr_aton(g_static_arp_ip) };
        struct eth_addr e;
        memcpy(e.addr, g_static_arp_mac, 6);
        if (etharp_add_static_entry(&a, &e) == ERR_OK)
        {
            ESP_LOGI(TAG, "static ARP %s -> %02x:%02x:%02x:%02x:%02x:%02x", g_static_arp_ip,
                     g_static_arp_mac[0], g_static_arp_mac[1], g_static_arp_mac[2],
                     g_static_arp_mac[3], g_static_arp_mac[4], g_static_arp_mac[5]);
        }
    }

    if (g_ping_target == NULL)
    {
        vTaskDelete(NULL);
        return;
    }

    ip_addr_t target = { 0 };
    target.type = IPADDR_TYPE_V4;
    target.u_addr.ip4.addr = esp_ip4addr_aton(g_ping_target);
    esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
    cfg.target_addr = target;
    cfg.count = ESP_PING_COUNT_INFINITE;
    cfg.interval_ms = 1000;
    esp_ping_callbacks_t cbs = { .on_ping_success = on_ping_success,
                                 .on_ping_timeout = on_ping_timeout };
    esp_ping_handle_t ping;
    if (esp_ping_new_session(&cfg, &cbs, &ping) == ESP_OK) { esp_ping_start(ping); }
    else { ESP_LOGE(TAG, "ping session create failed"); }
    vTaskDelete(NULL);
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== 802.11s mesh bring-up (P1: vif + beacon) ===");

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_netif_init());

    /* Unique per-board mesh MAC: ESP32 efuse MAC with the locally-administered bit
     * set (the MM6108 factory MAC is shared across modules, so it can't be used). */
    esp_read_mac(g_mesh_mac, ESP_MAC_WIFI_STA);
    g_mesh_mac[0] = (g_mesh_mac[0] | 0x02) & 0xFE;

    mmhalow_init(NULL);
    mmhalow_print_version_info();

#ifdef MESH_DYNAMIC_RF
    /* Optional EIRP cap (MESH_TX_POWER_DBM>0). RF-forcing multi-hop via low power didn't work here (boards
     * too close), so default 0 = full power; the mode is for dynamic-join verification (no allowlist). */
    if (MESH_TX_POWER_DBM > 0)
    {
        enum mmwlan_status pst = mmwlan_override_max_tx_power(MESH_TX_POWER_DBM);
        ESP_LOGW(TAG, "MESH DYNAMIC_RF: TX power capped to %d dBm (st=%d)", MESH_TX_POWER_DBM, (int)pst);
    }
    else
    {
        ESP_LOGW(TAG, "MESH DYNAMIC_RF: full TX power (no cap)");
    }
#endif

    struct mmwlan_mesh_args args = { 0 };
    memcpy(args.if_addr, g_mesh_mac, sizeof(g_mesh_mac));
    memcpy(args.mesh_id, MESH_ID, strlen(MESH_ID));
    args.mesh_id_len = strlen(MESH_ID);
    args.s1g_chan_num = MESH_S1G_CHAN;
    args.beacon_interval_tu = 100;
    args.max_plinks = MESH_MAX_PLINKS;

    /* Set the forced-topology allowlist BEFORE mmwlan_mesh_start: the allowlist is plain file-static
     * state (no mesh needed), and setting it after start races the first beacons -> a node could peer
     * with a non-allowed neighbour (e.g. board1<->board2 directly) in the gap and keep that direct
     * key+path, defeating the forced topology. */
#ifdef MESH_LINE_RELAY_DEMO
    if (memcmp(g_mesh_mac, MAC_B0, 6) == 0)
    {
        uint8_t allow[12];
        memcpy(allow, MAC_B1, 6);
#ifdef MESH_XV_CHRONOSALT
        memcpy(allow + 6, MAC_CHRONOSALT, 6); /* board0 relays board1 <-> chronosalt(Linux) */
        mmwlan_mesh_set_peer_allowlist(allow, 2);
        ESP_LOGW(TAG, "MESH role: RELAY (peers board1+chronosalt, forwards between them)");
#else
        memcpy(allow + 6, MAC_B2, 6);
        mmwlan_mesh_set_peer_allowlist(allow, 2);
        ESP_LOGW(TAG, "MESH role: RELAY (peers board1+board2, forwards between them)");
#endif
    }
    else if (memcmp(g_mesh_mac, MAC_B1, 6) == 0)
    {
        mmwlan_mesh_set_peer_allowlist(MAC_B0, 1);
#ifdef MESH_XV_CHRONOSALT
        g_ping_target = "10.9.9.3"; /* chronosalt(Linux), reachable only via board0 */
        ESP_LOGW(TAG, "MESH role: endpoint board1 -> ping chronosalt (10.9.9.3) via relay");
#else
        g_ping_target = "10.9.9.108"; /* board2, reachable only via board0 (ARP now relayed) */
        ESP_LOGW(TAG, "MESH role: endpoint board1 -> ping board2 (10.9.9.108) via relay");
#endif
    }
    else if (memcmp(g_mesh_mac, MAC_B2, 6) == 0)
    {
        mmwlan_mesh_set_peer_allowlist(MAC_B0, 1);
        ESP_LOGW(TAG, "MESH role: endpoint board2 (responder)");
    }
#endif
#ifdef MESH_DYNAMIC_RF
    /* No allowlist -> peer with ANYONE in RF range (RF geometry, not policy, shapes the topology).
     * board1 pings board2; if board1<->board2 is out of range, board0 must relay it (dynamic multi-hop,
     * no restart). Path forms via HWMP from the live beacons/PREQs alone. */
    if (memcmp(g_mesh_mac, MAC_B1, 6) == 0)
    {
        g_ping_target = "10.9.9.108"; /* board2 */
        ESP_LOGW(TAG, "MESH DYNAMIC_RF: board1 -> ping board2 (10.9.9.108), NO allowlist (RF-forced line)");
    }
    else if (memcmp(g_mesh_mac, MAC_B2, 6) == 0)
    {
        ESP_LOGW(TAG, "MESH DYNAMIC_RF: board2 responder, NO allowlist");
    }
    else if (memcmp(g_mesh_mac, MAC_B0, 6) == 0)
    {
        ESP_LOGW(TAG, "MESH DYNAMIC_RF: board0 (mid) responder, NO allowlist");
    }
#endif

    ESP_LOGI(TAG, "Starting mesh (id=\"%s\" chan=%d mac=%02x:%02x:%02x:%02x:%02x:%02x)...",
             MESH_ID, MESH_S1G_CHAN, g_mesh_mac[0], g_mesh_mac[1], g_mesh_mac[2],
             g_mesh_mac[3], g_mesh_mac[4], g_mesh_mac[5]);
    enum mmwlan_status st = mmwlan_mesh_start(&args);
    if (st != MMWLAN_SUCCESS)
    {
        ESP_LOGE(TAG, "==> mmwlan_mesh_start FAILED status=%d", (int)st);
        return;
    }
    ESP_LOGI(TAG, "==> mesh vif up; firmware beaconing periodically on chan %d.", MESH_S1G_CHAN);

#if defined(MESH_LEAF_NODE) && defined(MESH_LINE_RELAY_DEMO)
    /* Bench test (Variant A): make board0 a pure single-hop leaf. It still peers directly with
     * board1 and board2 but stops relaying their traffic and emits no HWMP -- so board1->board2
     * (forced through board0 by the allowlist) must now fail. Runtime-settable; here right after start. */
    if (memcmp(g_mesh_mac, MAC_B0, 6) == 0)
    {
        mmwlan_mesh_set_multihop(false);
        ESP_LOGW(TAG, "MESH role: board0 LEAF -- multi-hop/forwarding DISABLED (will not relay)");
    }
#endif

    /* Log any peer mesh beacons we hear on the channel. */
    mmwlan_register_rx_frame_cb(MMWLAN_FRAME_BEACON, peer_beacon_cb, NULL);

#ifdef MESH_LINUX_INTEROP
    /* No allowlist -> peer with anyone (incl. the Linux node). Ping chronite so we originate a
     * PREQ for it (-> Linux PREP) and so chronite builds a path to us (-> Linux PERR on break). */
    g_ping_target = "10.9.9.2";
    ESP_LOGW(TAG, "MESH role: Linux interop -> open peering + ping chronite (10.9.9.2)");
#endif

    /* P4: once a peer link is up, pin a static mesh IP and ping a neighbour. */
    xTaskCreate(mesh_net_task, "mesh_net", 4096, NULL, 5, NULL);

    /* P2: mesh peering (MPM) is handled in morselib — we answer received Mesh Peering
     * Open frames with Open+Confirm and reach ESTAB (see umac_mesh_handle_action). The
     * peer link forms automatically once a neighbour (e.g. a Linux mesh node) opens to us. */
    uint32_t s = 0;
    for (;;)
    {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (++s % 5 == 0)
        {
            ESP_LOGI(TAG, "mesh alive, uptime=%" PRIu32 "s", s);
        }
    }
}
