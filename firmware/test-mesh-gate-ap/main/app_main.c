/*
 * rimba-halow-mesh-ap — all-ESP32 Mesh-gate: 802.11s mesh + co-channel SoftAP on ONE MM6108,
 * L2-BRIDGING traffic between them on ONE flat subnet (the all-ESP 802.11s gate).
 *
 * Bring-up order is MESH FIRST (mesh owns the primary vif) then the AP (secondary vif):
 *      mmhalow_init  ->  mmwlan_mesh_start  ->  mmwlan_ap_enable
 * then the L2 bridge wiring: a SECOND esp_netif for the AP vif, per-vif RX demux + TX tagging.
 * AP clients and mesh nodes share ONE flat 10.9.9.0/24:
 *      mesh netif  10.9.9.<gw>/24   (primary vif, MMWLAN_VIF_STA host-slot)
 *      AP   netif  10.9.9.1/24      (secondary vif, MMWLAN_VIF_AP; DHCP server for AP clients)
 *
 * The gate bridges at L2 (802.11s Address-Extension): an AP client's frame to a mesh node is
 * proxied into the mesh (S5c), a mesh node's frame to a client is delivered onto the AP vif (S5b),
 * broadcasts are bridged both ways (B1/B2), and proxy-ARP resolves across the bridge. So an AP
 * client is zero-config — it DHCPs a 10.9.9.x address and reaches any mesh node directly, with NO
 * ip_forward, NO second subnet, and NO static route. (This replaced the earlier L3 router: two
 * subnets + CONFIG_LWIP_IP_FORWARD + a MESH_GATE_IP return route.)
 *
 * PER-VIF WIRING (load-bearing, from the C-2 datapath review): morselib delivers mesh RX to the
 * MMWLAN_VIF_STA ext-cb slot and AP RX to MMWLAN_VIF_AP, and routes TX by metadata.vif. So the
 * mesh netif is fed from / tagged VIF_STA and the AP netif from / tagged VIF_AP. Getting this
 * backwards silently drops one direction.
 */

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_netif_types.h"
#include "esp_netif_defaults.h"

#include "mmhalow.h"
#include "mmpkt.h"
#include "umac/mesh/umac_mesh.h"

/* Local delivery needs a self-owned, contiguous RX pbuf (see gw_rx_deliver): esp_netif's zero-copy RX
 * pbuf is a non-contiguous PBUF_REF with a custom-free path, so we copy RX into a contiguous PBUF_RAM
 * and inject it directly via netif->input — avoiding the PBUF_REF free-ownership + header-prepend traps. */
#include "lwip/pbuf.h"
#include "lwip/netif.h"
#include "esp_netif_net_stack.h"

/* ---- Mesh params (match rimba-halow-mesh) ---------------------------------- */
#define MESH_ID         "rimba-mesh"
#define MESH_S1G_CHAN   27
#define MESH_MAX_PLINKS 16

/* ---- AP params (match rimba-halow-ap / rimba-halow-sta) -------------------- */
#define LINK_SSID       "rimba-ping"
#define LINK_PSK        "rimbahalow"
#define LINK_S1G_CHAN   27
#define LINK_OP_CLASS   68
#define LINK_MAX_STAS   4

#define AP_SUBNET_IP    "10.9.9.1"       /* the gate's AP-side host on the FLAT mesh subnet */
#define AP_SUBNET_MASK  "255.255.255.0"

static const char *TAG = "rimba-mesh-ap";

static uint8_t g_mesh_mac[6];
static uint8_t g_ap_bssid[6];   /* the AP vif's MAC — a frame to it is for us (local), not proxied (S5c) */
static esp_netif_t *g_mesh_netif;
static esp_netif_t *g_ap_netif;
static esp_netif_driver_base_t g_ap_driver_base;

/* S5b — the gate's associated AP clients. A proxied mesh AE frame whose eaddr1 (final DA) matches one of
 * these is delivered onto the AP vif (the mesh->AP leg of the L2 bridge, gate_ae_rx_cb below). */
#define GATE_MAX_CLIENTS 8
static uint8_t g_ap_clients[GATE_MAX_CLIENTS][6];
static volatile int g_ap_client_n;
/* g_ap_clients[]/g_ap_client_n are written by ap_sta_status_cb (AP status task) and read by
 * gate_is_ap_client from the mesh-RX task; a short portMUX critical section keeps a reader from seeing a
 * torn slot or a stale count mid-remove. No TX/blocking call runs inside it, so it cannot deadlock. */
static portMUX_TYPE g_ap_clients_mux = portMUX_INITIALIZER_UNLOCKED;

static void gw_arp_forget_mac(const uint8_t *mac); /* drop a departed host's proxy-ARP mapping (defined below) */

static bool gate_is_ap_client(const uint8_t *mac)
{
    bool found = false;
    taskENTER_CRITICAL(&g_ap_clients_mux);
    for (int i = 0; i < g_ap_client_n; i++)
        if (memcmp(g_ap_clients[i], mac, 6) == 0) { found = true; break; }
    taskEXIT_CRITICAL(&g_ap_clients_mux);
    return found;
}

/* Log AP client associations + maintain the client set (the status cb fires as a STA authorizes / leaves). */
static void ap_sta_status_cb(const struct mmwlan_ap_sta_status *st, void *arg)
{
    (void)arg;
    if (st == NULL) return;
    if (st->state == MMWLAN_AP_STA_AUTHORIZED) {
        taskENTER_CRITICAL(&g_ap_clients_mux);
        bool present = false;
        for (int i = 0; i < g_ap_client_n; i++)
            if (memcmp(g_ap_clients[i], st->mac_addr, 6) == 0) { present = true; break; }
        if (!present && g_ap_client_n < GATE_MAX_CLIENTS)
            memcpy(g_ap_clients[g_ap_client_n++], st->mac_addr, 6);
        int n = g_ap_client_n;
        taskEXIT_CRITICAL(&g_ap_clients_mux);
        ESP_LOGI(TAG, "AP client joined: " MACSTR " (%d total)", MAC2STR(st->mac_addr), n);
    } else if (st->state == MMWLAN_AP_STA_UNKNOWN) {
        taskENTER_CRITICAL(&g_ap_clients_mux);
        for (int i = 0; i < g_ap_client_n; i++)
            if (memcmp(g_ap_clients[i], st->mac_addr, 6) == 0) {
                memcpy(g_ap_clients[i], g_ap_clients[--g_ap_client_n], 6); /* swap-with-last remove */
                break;
            }
        int n = g_ap_client_n;
        taskEXIT_CRITICAL(&g_ap_clients_mux);
        gw_arp_forget_mac(st->mac_addr); /* stop pushing this client's now-stale IP->MAC mapping */
        ESP_LOGI(TAG, "AP client left:   " MACSTR " (%d total)", MAC2STR(st->mac_addr), n);
    }
}

/* ---- AP-side netif: a 2nd esp_netif bound to the AP (secondary) vif ---------
 * TX tags MMWLAN_VIF_AP so morselib egresses on the AP vif; RX is fed by the VIF_AP ext-cb. */
static esp_err_t gw_ap_transmit(void *h, void *buffer, size_t len)
{
    (void)h;
    if (mmwlan_tx_wait_until_ready(1000) != MMWLAN_SUCCESS) {
        ESP_LOGW(TAG, "AP netif TX blocked");
        return ESP_FAIL;
    }
    struct mmpkt *pkt = mmwlan_alloc_mmpkt_for_tx(len, 0);
    if (pkt == NULL) return ESP_ERR_NO_MEM;
    struct mmpktview *v = mmpkt_open(pkt);
    mmpkt_append_data(v, (const uint8_t *)buffer, len);
    mmpkt_close(&v);
    struct mmwlan_tx_metadata md = MMWLAN_TX_METADATA_INIT;
    md.vif = MMWLAN_VIF_AP;
    enum mmwlan_status txs = mmwlan_tx_pkt(pkt, &md);
    return (txs == MMWLAN_SUCCESS) ? ESP_OK : ESP_FAIL;
}

static esp_err_t gw_ap_transmit_wrap(void *h, void *buffer, size_t len, void *netstack_buf)
{
    (void)netstack_buf;
    return gw_ap_transmit(h, buffer, len);
}

static void gw_ap_free_rx(void *h, void *buffer)
{
    (void)h;
    struct mmpktview *v = (struct mmpktview *)buffer;
    struct mmpkt *pkt = mmpkt_from_view(v);
    mmpkt_close(&v);
    mmpkt_release(pkt);
}

static esp_err_t gw_ap_driver_post_attach(esp_netif_t *netif, void *args)
{
    esp_netif_driver_base_t *base = (esp_netif_driver_base_t *)args;
    base->netif = netif;
    esp_netif_driver_ifconfig_t ifcfg = {
        .handle = base,
        .transmit = gw_ap_transmit,
        .transmit_wrap = gw_ap_transmit_wrap,
        .driver_free_rx_buffer = gw_ap_free_rx,
    };
    return esp_netif_set_driver_config(netif, &ifcfg);
}

/* Per-vif RX LOCAL delivery: hand the frame to the netif for that vif (mesh->mesh netif, AP->AP netif)
 * for delivery into the gate's OWN lwIP stack — the DHCP server, and ARP/ping addressed to the gate.
 * The AP<->mesh BRIDGING is done at L2 by the S5b/S5c/B1/B2 branches in the RX cbs, not here.
 *
 * We DON'T use esp_netif_receive() here: it wraps the frame in a zero-copy, NON-CONTIGUOUS PBUF_REF
 * (esp_pbuf_allocate) with a custom-free callback. A contiguous, self-owned copy avoids two traps: (a)
 * lwIP's pbuf_add_header() refuses to prepend an L2 header onto a non-contiguous pbuf (pbuf.c:
 * `else`/`!force` -> return 1), and (b) the PBUF_REF custom-free path risks a double-free once we
 * already own the mmpkt.
 *
 * Fix: copy the frame into a CONTIGUOUS PBUF_RAM with link headroom and inject it via netif->input
 * (exactly what esp_netif's own wlanif_input does). We own the mmpkt (the ext-cb handed it over); we
 * copied it, so we free it exactly once and never pass it to esp_netif -> no double-free. */
static void gw_rx_deliver(struct mmpkt *mmpkt, esp_netif_t *esp_netif)
{
    struct mmpktview *v = mmpkt_open(mmpkt);
    uint32_t len = mmpkt_get_data_length(v);
    struct netif *lwip_netif = esp_netif_get_netif_impl(esp_netif);
    struct pbuf *p = NULL;
    if (lwip_netif != NULL && netif_is_up(lwip_netif) && len > 0 && len <= 0xFFFF) {
        /* PBUF_LINK reserves PBUF_LINK_HLEN (14 B) of headroom — PBUF_RAW_TX reserves only
         * PBUF_LINK_ENCAPSULATION_HLEN, which is 0 in this build. PBUF_RAM => contiguous. */
        p = pbuf_alloc(PBUF_LINK, (u16_t)len, PBUF_RAM);
        if (p != NULL) {
            memcpy(p->payload, mmpkt_get_data_start(v), len);
        }
    }
    /* Frame copied (or un-copyable); the mmpkt is ours -> free it exactly once. */
    mmpkt_close(&v);
    mmpkt_release(mmpkt);
    if (p != NULL && lwip_netif->input(p, lwip_netif) != ERR_OK) {
        pbuf_free(p);
    }
}

/* ---- Gate proxy-ARP -------------------------------------------------------------------------------
 * The lossy part of the L2 bridge is broadcast ARP resolution across it: a host ARPs for a peer on the
 * OTHER side, and the request (broadcast, No-Ack, lossy) must cross the bridge AND the reply must come
 * back — so resolution is hit-or-miss (the flaky mesh->AP-client case). Proxy-ARP fixes it: the gate
 * snoops IP->MAC on both sides and, when it sees an ARP REQUEST for a host it knows on the OTHER side,
 * answers directly with that host's REAL MAC over a reliable link (a unicast on the AP; a proxied
 * unicast on the mesh via mmwlan_mesh_tx_proxied). Answering with the real MAC (not the gate's) keeps
 * the L2-bridge datapath (S5b/S5c) carrying the actual traffic — the requester still addresses the true
 * peer, the gate just short-circuits the lossy resolution. */
#define GW_ARP_MAX 24
enum { GW_SIDE_AP = 0, GW_SIDE_MESH = 1 };
struct gw_arp_entry { uint32_t ip; uint8_t mac[6]; uint8_t side; bool used; uint32_t last_ms; };
static struct gw_arp_entry g_gw_arp[GW_ARP_MAX];
#define GW_ARP_TTL_MS (5u * 60u * 1000u)  /* forget a host not seen/refreshed within this window */

static uint32_t gw_now_ms(void) { return (uint32_t)xTaskGetTickCount() * portTICK_PERIOD_MS; }

static void gw_arp_learn(uint32_t ip, const uint8_t *mac, uint8_t side)
{
    if (ip == 0 || (mac[0] & 0x01)) return; /* skip 0.0.0.0 + broadcast/multicast MACs */
    uint32_t now = gw_now_ms();
    int free_i = -1, lru_i = -1;
    for (int i = 0; i < GW_ARP_MAX; i++) {
        if (g_gw_arp[i].used) {
            if (g_gw_arp[i].ip == ip) {
                memcpy(g_gw_arp[i].mac, mac, 6); g_gw_arp[i].side = side; g_gw_arp[i].last_ms = now; return;
            }
            if (lru_i < 0 || (int32_t)(g_gw_arp[i].last_ms - g_gw_arp[lru_i].last_ms) < 0) lru_i = i;
        } else if (free_i < 0) { free_i = i; }
    }
    int slot = (free_i >= 0) ? free_i : lru_i; /* a free slot, else evict the least-recently-used entry */
    if (slot < 0) return;
    g_gw_arp[slot].ip = ip; memcpy(g_gw_arp[slot].mac, mac, 6);
    g_gw_arp[slot].side = side; g_gw_arp[slot].last_ms = now;
    g_gw_arp[slot].used = true; /* publish last, after the fields are populated */
}
static void gw_arp_forget_mac(const uint8_t *mac)
{
    for (int i = 0; i < GW_ARP_MAX; i++)
        if (g_gw_arp[i].used && memcmp(g_gw_arp[i].mac, mac, 6) == 0) g_gw_arp[i].used = false;
}
static bool gw_arp_lookup(uint32_t ip, uint8_t *mac_out, uint8_t *side_out)
{
    uint32_t now = gw_now_ms();
    for (int i = 0; i < GW_ARP_MAX; i++)
        if (g_gw_arp[i].used && g_gw_arp[i].ip == ip) {
            if ((uint32_t)(now - g_gw_arp[i].last_ms) > GW_ARP_TTL_MS) { g_gw_arp[i].used = false; return false; }
            memcpy(mac_out, g_gw_arp[i].mac, 6); *side_out = g_gw_arp[i].side; return true;
        }
    return false;
}
/* Learn the sender's IP->MAC from a frame (ARP or IPv4), so the gate can proxy/announce it. `side` = the
 * vif it arrived on. Snooping IPv4 (not just ARP) lets the gate learn a host from ANY of its traffic
 * (e.g. a ping), instead of waiting up to 60 s for its next gratuitous ARP. */
static void gw_arp_snoop(const uint8_t *eth, uint32_t elen, uint8_t side)
{
    if (elen < 14) return;
    if (eth[12] == 0x08 && eth[13] == 0x06 && elen >= 14 + 28) {          /* ARP */
        uint32_t spa; memcpy(&spa, eth + 14 + 14, 4);                     /* ARP sender IP */
        gw_arp_learn(spa, eth + 14 + 8 /* ARP sha */, side);             /* ARP sender MAC */
    } else if (eth[12] == 0x08 && eth[13] == 0x00 && elen >= 14 + 20) {   /* IPv4 */
        uint32_t sip; memcpy(&sip, eth + 14 + 12, 4);                     /* IP header source addr */
        gw_arp_learn(sip, eth + 6 /* Ethernet source MAC */, side);
    }
}
/* Fill a 42-byte Ethernet ARP REPLY: to `req_mac`/`req_ip`, answering "tpa is-at tpa_mac". */
static void gw_arp_build_reply(uint8_t *out, const uint8_t *req_mac, uint32_t req_ip, uint32_t tpa,
                               const uint8_t *tpa_mac)
{
    memcpy(out, req_mac, 6);        /* eth dst = requester */
    memcpy(out + 6, tpa_mac, 6);    /* eth src = the answered host (as if IT replied) */
    out[12] = 0x08; out[13] = 0x06; /* ethertype ARP */
    out[14] = 0x00; out[15] = 0x01; /* htype ethernet */
    out[16] = 0x08; out[17] = 0x00; /* ptype IPv4 */
    out[18] = 6; out[19] = 4;       /* hlen plen */
    out[20] = 0x00; out[21] = 0x02; /* oper = REPLY */
    memcpy(out + 22, tpa_mac, 6);   /* sha = answered host MAC */
    memcpy(out + 28, &tpa, 4);      /* spa = answered host IP */
    memcpy(out + 34, req_mac, 6);   /* tha = requester MAC */
    memcpy(out + 38, &req_ip, 4);   /* tpa = requester IP */
}
static void gw_tx_ap_frame(const uint8_t *frame, uint32_t len)
{
    struct mmpkt *pkt = mmwlan_alloc_mmpkt_for_tx(len, 0);
    if (pkt == NULL) return;
    struct mmpktview *tv = mmpkt_open(pkt);
    mmpkt_append_data(tv, frame, len);
    mmpkt_close(&tv);
    struct mmwlan_tx_metadata tmd = MMWLAN_TX_METADATA_INIT;
    tmd.vif = MMWLAN_VIF_AP;
    (void)mmwlan_tx_pkt(pkt, &tmd);
}
/* If `eth` is an ARP REQUEST for a host we know on `!from_side`, answer it (real MAC) over the reliable
 * link and return true (the caller then skips the lossy bridge for this frame). */
static bool gw_proxy_arp(const uint8_t *eth, uint32_t elen, uint8_t from_side)
{
    if (elen < 14 + 28 || eth[12] != 0x08 || eth[13] != 0x06) return false; /* not ARP */
    const uint8_t *arp = eth + 14;
    if (arp[6] != 0x00 || arp[7] != 0x01) return false;                     /* not a REQUEST */
    uint32_t tpa; memcpy(&tpa, arp + 24, 4);
    uint8_t tmac[6], tside;
    if (!gw_arp_lookup(tpa, tmac, &tside) || tside == from_side) return false; /* unknown / same side */
    uint32_t spa; memcpy(&spa, arp + 14, 4);
    const uint8_t *req_mac = arp + 8;
    if (from_side == GW_SIDE_AP) {
        uint8_t reply[42]; /* per-call (was a shared static): no cross-task race with the announce push */
        gw_arp_build_reply(reply, req_mac, spa, tpa, tmac);
        gw_tx_ap_frame(reply, sizeof(reply));
    } else { /* mesh requester: proxy a unicast ARP reply back to it (eaddr1=requester, eaddr2=answered) */
        uint8_t snap[8 + 28]; /* per-call buffer — no shared-static race */
        snap[0] = 0xaa; snap[1] = 0xaa; snap[2] = 0x03;
        snap[3] = 0x00; snap[4] = 0x00; snap[5] = 0x00;
        snap[6] = 0x08; snap[7] = 0x06;                 /* ARP ethertype */
        uint8_t *a = snap + 8;
        a[0] = 0; a[1] = 1; a[2] = 0x08; a[3] = 0; a[4] = 6; a[5] = 4; a[6] = 0; a[7] = 2; /* reply */
        memcpy(a + 8, tmac, 6);  memcpy(a + 14, &tpa, 4);          /* sha/spa = answered host */
        memcpy(a + 18, req_mac, 6); memcpy(a + 24, &spa, 4);       /* tha/tpa = requester */
        (void)mmwlan_mesh_tx_proxied(req_mac, tmac, snap, sizeof(snap));
    }
    ESP_LOGI(TAG, "proxy-ARP: told %s " MACSTR " that 10.9.9.%u is-at " MACSTR,
             from_side == GW_SIDE_AP ? "AP" : "mesh", MAC2STR(req_mac), ((const uint8_t *)&tpa)[3],
             MAC2STR(tmac));
    return true;
}

/* Proactive push (the reliable fix): reactive proxy-ARP still needs the requester's lossy broadcast ARP to
 * reach us, and gets masked by passive gratuitous-ARP learning. Instead, periodically TEACH each learned
 * host about every learned host on the OTHER side over a RELIABLE UNICAST (AP downlink / proxied mesh
 * unicast, both acked). Then neither side ever needs to broadcast-ARP across the bridge — resolution stops
 * depending on lossy No-Ack multicast. (With TRUST_IP_MAC on, an unsolicited ARP reply populates the cache.) */
static void gw_arp_push_to_mesh(uint32_t spa, const uint8_t *smac, const uint8_t *dst_mac, uint32_t dpa)
{
    uint8_t snap[8 + 28]; /* per-call buffer — no shared-static race with the RX-context proxy-ARP */
    snap[0] = 0xaa; snap[1] = 0xaa; snap[2] = 0x03;
    snap[3] = 0x00; snap[4] = 0x00; snap[5] = 0x00;
    snap[6] = 0x08; snap[7] = 0x06;
    uint8_t *a = snap + 8;
    a[0] = 0; a[1] = 1; a[2] = 0x08; a[3] = 0; a[4] = 6; a[5] = 4; a[6] = 0; a[7] = 2; /* reply */
    memcpy(a + 8, smac, 6);  memcpy(a + 14, &spa, 4);   /* sha/spa = the announced host */
    memcpy(a + 18, dst_mac, 6); memcpy(a + 24, &dpa, 4);/* tha/tpa = the taught node    */
    (void)mmwlan_mesh_tx_proxied(dst_mac, smac, snap, sizeof(snap));
}
static void gw_arp_announce_once(void)
{
    uint32_t now = gw_now_ms();
    for (int i = 0; i < GW_ARP_MAX; i++) /* age out departed/stale hosts before pushing */
        if (g_gw_arp[i].used && (uint32_t)(now - g_gw_arp[i].last_ms) > GW_ARP_TTL_MS)
            g_gw_arp[i].used = false;
    int pushed = 0;
    for (int a = 0; a < GW_ARP_MAX; a++) {
        if (!g_gw_arp[a].used) continue;
        for (int b = 0; b < GW_ARP_MAX; b++) {
            if (!g_gw_arp[b].used || g_gw_arp[a].side == g_gw_arp[b].side) continue; /* other side only */
            /* teach host `a` that host `b` is-at b.mac, over a's reliable link */
            if (g_gw_arp[a].side == GW_SIDE_AP) {
                uint8_t reply[42]; /* per-call buffer (was a shared static) */
                gw_arp_build_reply(reply, g_gw_arp[a].mac, g_gw_arp[a].ip,
                                   g_gw_arp[b].ip, g_gw_arp[b].mac);
                gw_tx_ap_frame(reply, sizeof(reply));                   /* AP downlink (acked) */
            } else {
                gw_arp_push_to_mesh(g_gw_arp[b].ip, g_gw_arp[b].mac,
                                    g_gw_arp[a].mac, g_gw_arp[a].ip);   /* proxied mesh unicast */
            }
            pushed++;
        }
    }
    if (pushed > 0)
        ESP_LOGI(TAG, "proxy-ARP push: refreshed %d cross-bridge ARP mapping(s) via reliable unicast",
                 pushed);
}
static void gw_arp_announce_task(void *arg)
{
    (void)arg;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(3000));
        gw_arp_announce_once();
    }
}

/* B2 — mesh->AP broadcast bridging (symmetric to gw_ap_rx_cb's AP->mesh group branch): a GROUP-addressed
 * mesh frame (e.g. an ARP request from a mesh node for an AP client's IP) is ALSO injected onto the AP vif
 * so the AP clients see mesh broadcasts. The datapath already prepended the 802.3 header (for an AE_A4
 * proxied multicast that is [dst=group][src=eaddr1]; for a plain mesh multicast [dst=group][src=mesh_sa]),
 * so we re-emit those exact bytes on the AP. No loop: this fires only on mesh RX (the gate's own AP->mesh
 * TX never returns here), and the mesh RMC + own-SA drop already suppress the gate's own re-broadcasts. */
static uint8_t s_mesh2ap[14 + 1514];
static void gw_mesh_rx_cb(struct mmpkt *mmpkt, const struct mmwlan_rx_metadata *md, void *arg)
{
    (void)md;
    struct mmpktview *v = mmpkt_open(mmpkt);
    const uint8_t *eth = mmpkt_get_data_start(v);
    uint32_t elen = mmpkt_get_data_length(v);
    uint32_t buflen = 0;
    if (eth != NULL && elen >= 14 && elen <= sizeof(s_mesh2ap)) {
        memcpy(s_mesh2ap, eth, elen); /* copy the frame, then close the rx view before any TX */
        buflen = elen;
    }
    mmpkt_close(&v);
    bool answered = false;
    uint32_t n = 0;
    if (buflen > 0) {
        gw_arp_snoop(s_mesh2ap, buflen, GW_SIDE_MESH);            /* learn this mesh node's IP->MAC */
        answered = gw_proxy_arp(s_mesh2ap, buflen, GW_SIDE_MESH); /* answer its ARP for an AP client */
        if (!answered && (s_mesh2ap[0] & 0x01) && g_ap_client_n > 0) {
            n = buflen; /* group frame, not proxy-answered -> B2-bridge it to the AP */
        }
    }
    if (answered) {
        mmpkt_release(mmpkt); /* consumed via a reliable proxy reply — don't bridge or locally deliver */
        return;
    }
    if (n > 0) {
        struct mmpkt *pkt = mmwlan_alloc_mmpkt_for_tx(n, 0);
        if (pkt != NULL) {
            struct mmpktview *tv = mmpkt_open(pkt);
            mmpkt_append_data(tv, s_mesh2ap, n);
            mmpkt_close(&tv);
            struct mmwlan_tx_metadata tmd = MMWLAN_TX_METADATA_INIT;
            tmd.vif = MMWLAN_VIF_AP;
            uint16_t et = ((uint16_t)s_mesh2ap[12] << 8) | s_mesh2ap[13];
            if (mmwlan_tx_pkt(pkt, &tmd) == MMWLAN_SUCCESS) {
                if (et == 0x0806 && n >= 14 + 28)   /* ARP: show op + who-has/is-at */
                    ESP_LOGI(TAG, "B2 mesh->AP bcast: %u B from " MACSTR " ARP op=%u spa=10.9.9.%u tpa=10.9.9.%u",
                             (unsigned)n, MAC2STR(s_mesh2ap + 6), s_mesh2ap[14 + 7], s_mesh2ap[14 + 17],
                             s_mesh2ap[14 + 27]);
                else
                    ESP_LOGI(TAG, "B2 mesh->AP bcast: %u B from " MACSTR " to " MACSTR " et=0x%04x",
                             (unsigned)n, MAC2STR(s_mesh2ap + 6), MAC2STR(s_mesh2ap), et);
            }
        }
    }
    gw_rx_deliver(mmpkt, (esp_netif_t *)arg); /* also deliver locally (the gate's own stack) */
}

/* S5c — AP->mesh bridge ingress: an AP client's UNICAST frame destined to a MESH node (not the gate
 * itself, not broadcast/multicast) is PROXIED into the mesh as an AE_A5_A6 frame (eaddr2 = the client,
 * eaddr1 = the dst) via mmwlan_mesh_tx_proxied. The Ethernet payload [dst][src][ethertype][L3] is
 * re-encapsulated as [LLC/SNAP][L3] for the mesh. Frames to the gate itself (DHCP, ping-to-gate) +
 * broadcast fall through to the existing L3 path. The mesh->AP return leg is S5b (gate_ae_rx_cb). */
static uint8_t s_ap2mesh[8 + 1514];
static void gw_ap_rx_cb(struct mmpkt *mmpkt, const struct mmwlan_rx_metadata *md, void *arg)
{
    (void)md;
    struct mmpktview *v = mmpkt_open(mmpkt);
    const uint8_t *eth = mmpkt_get_data_start(v);
    uint32_t elen = mmpkt_get_data_length(v);
    if (eth != NULL && elen >= 14) {
        gw_arp_snoop(eth, elen, GW_SIDE_AP);              /* learn this AP client's IP->MAC */
        if (gw_proxy_arp(eth, elen, GW_SIDE_AP)) {        /* answer its ARP for a mesh node directly */
            mmpkt_close(&v); mmpkt_release(mmpkt); return; /* resolved reliably — skip the lossy bridge */
        }
    }
    bool do_unicast_proxy = false;   /* S5c: unicast to a mesh node -> AE_A5_A6 to that node */
    bool do_group_proxy = false;     /* S5:  broadcast/multicast    -> AE_A4 flooded into the mesh */
    uint8_t dst[6], src[6];
    uint32_t l3 = 0;
    if (eth != NULL && elen >= 14) {
        l3 = elen - 14;
        bool is_group = (eth[0] & 0x01);                                 /* broadcast/multicast DA */
        bool for_gate = (memcmp(eth, g_ap_bssid, 6) == 0 || memcmp(eth, g_mesh_mac, 6) == 0);
        if (l3 <= sizeof(s_ap2mesh) - 8) {
            memcpy(dst, eth, 6);
            memcpy(src, eth + 6, 6);
            s_ap2mesh[0] = 0xaa; s_ap2mesh[1] = 0xaa; s_ap2mesh[2] = 0x03; /* LLC/SNAP */
            s_ap2mesh[3] = 0x00; s_ap2mesh[4] = 0x00; s_ap2mesh[5] = 0x00;
            s_ap2mesh[6] = eth[12]; s_ap2mesh[7] = eth[13];               /* ethertype -> SNAP */
            memcpy(s_ap2mesh + 8, eth + 14, l3);
            if (is_group)            do_group_proxy = true;               /* ARP request, etc. */
            else if (!for_gate)      do_unicast_proxy = true;             /* to a mesh node */
        }
    }
    mmpkt_close(&v);
    if (do_unicast_proxy) {
        if (mmwlan_mesh_tx_proxied(dst, src, s_ap2mesh, 8 + l3)) {
            ESP_LOGI(TAG, "S5c AP->mesh: %u B from " MACSTR " to " MACSTR, (unsigned)(8 + l3),
                     MAC2STR(src), MAC2STR(dst));
            mmpkt_release(mmpkt); /* proxied into the mesh (payload copied) — free once */
            return;
        }
        /* No mesh route for this unicast (e.g. dst is another AP client or unresolvable): fall through to
         * local delivery / L3 forwarding instead of silently dropping it. */
    }
    if (do_group_proxy) {
        /* Bridge an AP client's broadcast/multicast INTO the mesh as a proxied AE_A4 group frame, so mesh
         * nodes see it (e.g. an ARP request) + learn mpp(client->us). Then FALL THROUGH to local delivery
         * too — a bridge floods to all ports, and the gate's own stack may want this broadcast (DHCP, ARP
         * for the gate itself). */
        if (mmwlan_mesh_tx_group_proxied(src, s_ap2mesh, 8 + l3)) {
            uint16_t et = ((uint16_t)s_ap2mesh[6] << 8) | s_ap2mesh[7];
            if (et == 0x0806 && l3 >= 28) /* ARP */
                ESP_LOGI(TAG, "S5 AP->mesh bcast: %u B from " MACSTR " ARP op=%u spa=10.9.9.%u tpa=10.9.9.%u",
                         (unsigned)(8 + l3), MAC2STR(src), s_ap2mesh[8 + 7], s_ap2mesh[8 + 17],
                         s_ap2mesh[8 + 27]);
            else
                ESP_LOGI(TAG, "S5 AP->mesh bcast: %u B from " MACSTR " to " MACSTR " et=0x%04x",
                         (unsigned)(8 + l3), MAC2STR(src), MAC2STR(dst), et);
        }
    }
    gw_rx_deliver(mmpkt, (esp_netif_t *)arg); /* local: DHCP / ping-to-gate / broadcast */
}

/* S5b — mesh->AP bridge egress: a received proxied AE frame whose eaddr1 is one of our AP clients is
 * delivered onto the AP vif as an Ethernet II frame (dst=eaddr1, src=eaddr2). The AE payload is
 * [LLC/SNAP][L3]; recover the ethertype from the SNAP and build [dst][src][ethertype][L3], then hand it to
 * the AP vif TX (morselib re-adds LLC/SNAP + CCMP-encrypts to the client). The direct use of the S5a
 * mmwlan_mesh_register_ae_rx_cb hook. Runs in the mesh RX task; the TX is non-blocking (no tx_wait). */
static uint8_t s_ae2ap[14 + 1514];
static bool gate_ae_rx_cb(const uint8_t *eaddr1, const uint8_t *eaddr2, const uint8_t *payload,
                          uint32_t len, void *arg)
{
    (void)arg;
    if (!gate_is_ap_client(eaddr1)) return false; /* not for our AP — leave the normal mesh delivery/relay */
    /* This proxied frame's final DA is one of OUR AP clients: WE own it — deliver it onto the AP vif and
     * tell the datapath we consumed it (return true) so it does NOT also deliver it into the gate's own
     * lwIP stack (that would spuriously re-inject the frame into the mesh — the S5 double-delivery fix). */
    if (len >= 8 && payload[0] == 0xaa && payload[1] == 0xaa && payload[2] == 0x03) { /* LLC/SNAP */
        uint32_t l3 = len - 8;
        if (l3 <= sizeof(s_ae2ap) - 14) {
            memcpy(s_ae2ap, eaddr1, 6);      /* dst = the AP client (final DA) */
            memcpy(s_ae2ap + 6, eaddr2, 6);  /* src = the off-mesh source      */
            s_ae2ap[12] = payload[6];        /* ethertype recovered from the SNAP */
            s_ae2ap[13] = payload[7];
            memcpy(s_ae2ap + 14, payload + 8, l3);
            gw_arp_snoop(s_ae2ap, 14 + l3, GW_SIDE_MESH); /* proxy-ARP: learn this mesh node's IP->MAC */
            struct mmpkt *pkt = mmwlan_alloc_mmpkt_for_tx(14 + l3, 0);
            if (pkt != NULL) {
                struct mmpktview *v = mmpkt_open(pkt);
                mmpkt_append_data(v, s_ae2ap, 14 + l3);
                mmpkt_close(&v);
                struct mmwlan_tx_metadata md = MMWLAN_TX_METADATA_INIT;
                md.vif = MMWLAN_VIF_AP;
                if (mmwlan_tx_pkt(pkt, &md) == MMWLAN_SUCCESS)
                    ESP_LOGI(TAG, "S5b mesh->AP: %u B to AP client " MACSTR " (src " MACSTR ")",
                             (unsigned)(14 + l3), MAC2STR(eaddr1), MAC2STR(eaddr2));
            }
        }
    }
    return true; /* owned by us (delivered to the AP, or dropped) — never fall through to local delivery */
}

static void gw_setup_ap_netif(void)
{
    /* AP netif with a DHCP SERVER so AP clients are zero-config on the FLAT mesh subnet: the gate hands
     * out 10.9.9.x and the L2 bridge + proxy-ARP let a client reach any mesh node directly (no route).
     * The IP goes in the INHERENT config (like stock WIFI_AP) so the netif is BORN with 10.9.9.1 and
     * AUTOUP's auto-start of dhcps sees a valid server IP. (A 0.0.0.0 IP makes dhcps fail — dhcpserver.c:
     * ip4_addr_isany -> "could not obtain pcb" — and a later set_ip_info would then abort with
     * DHCP_NOT_STOPPED, boot-looping the gate.) So we do NOT call set_ip_info here. The dhcps pool starts at
     * 10.9.9.2 and spans CONFIG_LWIP_DHCPS_MAX_STATION_NUM leases (default 8 -> .2-.9), so at the default it
     * stays clear of the mesh nodes' 10.9.9.100-163 static range; if that Kconfig is raised toward ~100, pin
     * the range explicitly (esp_netif_dhcps_option) to keep the pool below .100. Reuse the WIFI_STA netstack
     * (generic ethernet L2 glue); dhcps is L3/UDP. */
    static esp_netif_ip_info_t ap_ip;              /* runtime-init: esp_ip4addr_aton is not const */
    ap_ip.ip.addr      = esp_ip4addr_aton(AP_SUBNET_IP);
    ap_ip.gw.addr      = 0; /* no router option: a pure L2 bridge doesn't route off-subnet, so don't hand
                             * clients a default route to 10.9.9.1 that would only black-hole their traffic */
    ap_ip.netmask.addr = esp_ip4addr_aton(AP_SUBNET_MASK);
    esp_netif_inherent_config_t ap_base_cfg = {
        .flags = (esp_netif_flags_t)(ESP_NETIF_DHCP_SERVER | ESP_NETIF_FLAG_AUTOUP),
        .ip_info = &ap_ip,
        .get_ip_event = 0,
        .lost_ip_event = 0,
        .if_key = "HALOW_GWAP",
        .if_desc = "gw-ap",
        .route_prio = 50,
        .bridge_info = NULL,
    };
    esp_netif_config_t ap_cfg = {
        .base = &ap_base_cfg,
        .driver = NULL,
        .stack = ESP_NETIF_NETSTACK_DEFAULT_WIFI_STA,
    };
    g_ap_netif = esp_netif_new(&ap_cfg);
    assert(g_ap_netif != NULL);

    g_ap_driver_base.post_attach = gw_ap_driver_post_attach;
    ESP_ERROR_CHECK(esp_netif_attach(g_ap_netif, &g_ap_driver_base));

    /* AP netif L2 addr = the AP vif BSSID, so ARP on the AP subnet resolves to the AP vif. Fail fast if the
     * BSSID is unavailable: both the netif MAC and the S5c for_gate test (g_ap_bssid) require a valid one —
     * a zero g_ap_bssid would misclassify a frame to the gate's own AP MAC as client->mesh and mis-proxy it. */
    uint8_t bssid[MMWLAN_MAC_ADDR_LEN] = { 0 };
    ESP_ERROR_CHECK(mmwlan_ap_get_bssid(bssid) == MMWLAN_SUCCESS ? ESP_OK : ESP_FAIL);
    esp_netif_set_mac(g_ap_netif, bssid);
    memcpy(g_ap_bssid, bssid, 6); /* S5c: the AP-side MAC (frames to it are local, not proxied) */

    /* NOTE — two esp_netifs (this AP + the mesh netif in mesh_net_task) share 10.9.9.0/24. That is safe:
     * dhcps is netif-bound, the AP<->mesh datapath is L2-bridged on an explicit md.vif (neither uses lwIP
     * routing), and lwIP answers a gate-addressed frame on its INPUT netif — so a client's ping to 10.9.9.1
     * is replied on the AP vif (bench-verified). The gate originates no lwIP L3 to a subnet host, so the
     * route_prio ambiguity between the two same-subnet netifs is never exercised. */

    /* Create/attach the underlying lwIP netif + input path (mirrors mmhalow's wifi_start) and bring it
     * up. WITHOUT action_start, esp_netif_receive() would deref a NULL input fn (PC=0 on first RX).
     * AUTOUP + the inherent IP => dhcps auto-starts on 10.9.9.1. */
    esp_netif_action_start(g_ap_netif, NULL, 0, NULL);
    esp_netif_action_connected(g_ap_netif, NULL, 0, NULL); /* no link event in AP */
    ESP_LOGI(TAG, "AP netif up: %s/24 + DHCP server, BSSID/MAC=" MACSTR, AP_SUBNET_IP, MAC2STR(bssid));
}

/* Pin the mesh netif's static IP once it is up (mmhalow's netif = the primary/mesh vif). */
static void mesh_net_task(void *arg)
{
    (void)arg;
    esp_netif_t *n = g_mesh_netif;
    if (n == NULL) { ESP_LOGE(TAG, "mesh netif not found"); vTaskDelete(NULL); return; }

    for (int i = 0; i < 60 && !esp_netif_is_netif_up(n); i++) { vTaskDelay(pdMS_TO_TICKS(500)); }
    vTaskDelay(pdMS_TO_TICKS(500));

    esp_netif_dhcpc_stop(n);
    esp_netif_set_mac(n, g_mesh_mac);
    unsigned host = 100u + (g_mesh_mac[5] & 0x3fu);
    char ipbuf[20];
    snprintf(ipbuf, sizeof(ipbuf), "10.9.9.%u", host);
    esp_netif_ip_info_t ip = { 0 };
    ip.ip.addr = esp_ip4addr_aton(ipbuf);
    ip.gw.addr = 0; /* flat single subnet — the gate is an L2 bridge, not an L3 gateway */
    ip.netmask.addr = esp_ip4addr_aton("255.255.255.0");
    ESP_ERROR_CHECK(esp_netif_set_ip_info(n, &ip));
    ESP_LOGI(TAG, "mesh netif static IP %s (L2-bridge gate host)", ipbuf);
    ESP_LOGI(TAG, "L2 bridge ready: AP clients + mesh nodes share 10.9.9.0/24 (DHCP + proxy-ARP, no ip_forward)");
    vTaskDelete(NULL);
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== all-ESP Mesh-gate: mesh + SoftAP L2-bridged on one MM6108 (flat 10.9.9.0/24) ===");

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_netif_init());

    esp_read_mac(g_mesh_mac, ESP_MAC_WIFI_STA);
    g_mesh_mac[0] = (g_mesh_mac[0] | 0x02) & 0xFE;

    mmhalow_init(NULL);                 /* creates the mesh netif (WIFI_STA_DEF) + boots morselib */
    mmhalow_print_version_info();
    g_mesh_netif = mmhalow_get_netif();

    /* --- 1) MESH on the PRIMARY vif --------------------------------------- */
    struct mmwlan_mesh_args mesh_args = { 0 };
    memcpy(mesh_args.if_addr, g_mesh_mac, sizeof(g_mesh_mac));
    memcpy(mesh_args.mesh_id, MESH_ID, strlen(MESH_ID));
    mesh_args.mesh_id_len = strlen(MESH_ID);
    mesh_args.s1g_chan_num = MESH_S1G_CHAN;
    mesh_args.beacon_interval_tu = 100;
    mesh_args.max_plinks = MESH_MAX_PLINKS;

    ESP_LOGI(TAG, "Starting MESH (id=\"%s\" chan=%d mac=" MACSTR ")...",
             MESH_ID, MESH_S1G_CHAN, MAC2STR(g_mesh_mac));
    enum mmwlan_status st = mmwlan_mesh_start(&mesh_args);
    if (st != MMWLAN_SUCCESS) { ESP_LOGE(TAG, "mesh_start FAILED %d", (int)st); return; }
    ESP_LOGI(TAG, "==> MESH vif up (primary).");

    /* --- 2) SoftAP on the SECONDARY vif ----------------------------------- */
    struct mmwlan_ap_args ap_args = MMWLAN_AP_ARGS_INIT;
    memcpy((char *)ap_args.ssid, LINK_SSID, strlen(LINK_SSID));
    ap_args.ssid_len = strlen(LINK_SSID);
    memcpy(ap_args.passphrase, LINK_PSK, strlen(LINK_PSK));
    ap_args.passphrase_len = strlen(LINK_PSK);
    ap_args.security_type = MMWLAN_SAE;
    ap_args.pmf_mode = MMWLAN_PMF_REQUIRED;
    ap_args.s1g_chan_num = LINK_S1G_CHAN;
    ap_args.op_class = LINK_OP_CLASS;
    ap_args.max_stas = LINK_MAX_STAS;
    ap_args.sta_status_cb = ap_sta_status_cb;

    ESP_LOGI(TAG, "Starting SoftAP (ssid=\"%s\" chan=%d) alongside the mesh...", LINK_SSID, LINK_S1G_CHAN);
    st = mmwlan_ap_enable(&ap_args);
    if (st != MMWLAN_SUCCESS) { ESP_LOGE(TAG, "ap_enable FAILED %d (mesh stays up)", (int)st); return; }
    ESP_LOGI(TAG, "==> AP vif up (secondary) — CONCURRENT with mesh.");

    /* --- 3) L2 bridge wiring: 2nd netif + per-vif RX demux + TX tagging --- */
    gw_setup_ap_netif();
    /* Override mmhalow's single plain RX cb with per-vif ext cbs (gateway RX demux delivers mesh
     * as VIF_STA, AP clients as VIF_AP). */
    ESP_ERROR_CHECK(mmwlan_register_rx_pkt_ext_cb(MMWLAN_VIF_STA, gw_mesh_rx_cb, g_mesh_netif) !=
                    MMWLAN_SUCCESS);
    ESP_ERROR_CHECK(mmwlan_register_rx_pkt_ext_cb(MMWLAN_VIF_AP, gw_ap_rx_cb, g_ap_netif) !=
                    MMWLAN_SUCCESS);
    /* S5b — mesh->AP bridge: deliver a proxied AE frame (eaddr1 = one of our AP clients) onto the AP vif.
     * The plain VIF_STA ext-cb can't see the AE endpoints (stripped with the Mesh Control), so this uses
     * the S5a AE-rx hook. A proxied frame to an AP client goes via this AP-inject path; other mesh RX
     * reaches the mesh netif for local delivery (gw_rx_deliver). */
    mmwlan_mesh_register_ae_rx_cb(gate_ae_rx_cb, NULL);

    xTaskCreate(mesh_net_task, "mesh_net", 4096, NULL, 5, NULL);
    /* Proxy-ARP proactive push: keep both sides' ARP caches populated over reliable unicast so a mesh node
     * ↔ AP client never depends on a lossy broadcast ARP crossing the bridge. */
    xTaskCreate(gw_arp_announce_task, "gw_arp", 4096, NULL, 4, NULL);

    ESP_LOGI(TAG, "on-air identities: MESH SA=" MACSTR, MAC2STR(g_mesh_mac));
}
