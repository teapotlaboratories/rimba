/*
 * rimba-halow-mesh-ap — all-ESP32 Mesh-gate: 802.11s mesh + co-channel SoftAP on ONE MM6108,
 * ROUTING traffic between them (the full §A3 gateway, all-ESP).
 *
 * Bring-up order is MESH FIRST (mesh owns the primary vif) then the AP (secondary vif):
 *      mmhalow_init  ->  mmwlan_mesh_start  ->  mmwlan_ap_enable
 * then the L3 gateway wiring: a SECOND esp_netif for the AP vif, per-vif RX demux + TX tagging,
 * and lwIP IP forwarding between the two subnets:
 *      mesh netif  10.9.9.<gw>/24   (primary vif, MMWLAN_VIF_STA host-slot)
 *      AP   netif  192.168.12.1/24  (secondary vif, MMWLAN_VIF_AP)
 *
 * PER-VIF WIRING (load-bearing, from the C-2 datapath review): in gateway mode morselib delivers
 * mesh RX to the MMWLAN_VIF_STA ext-cb slot and AP RX to MMWLAN_VIF_AP, and routes TX by
 * metadata.vif. So the mesh netif is fed from / tagged VIF_STA and the AP netif from / tagged
 * VIF_AP. Getting this backwards silently drops one direction.
 *
 * Needs the components/halow feat/mesh-ap-concurrency branch (Stage 1/2 + datapath Gaps A–D/P0)
 * and CONFIG_LWIP_IP_FORWARD=y (sdkconfig.defaults). A STA under the AP (rimba-halow-sta, static
 * 192.168.12.2 gw .1) can then ping a mesh node (10.9.9.x), traversing the gate; the 2nd mesh node
 * needs a return route `192.168.12.0/24 via 10.9.9.<gw>`.
 */

#include <inttypes.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_netif_types.h"
#include "esp_netif_defaults.h"

#include "mmhalow.h"
#include "mmpkt.h"
#include "umac/mesh/umac_mesh.h"

/* Gateway forwarding needs a FORWARDABLE RX pbuf (see gw_rx_deliver): esp_netif's zero-copy RX pbuf
 * is a non-contiguous PBUF_REF, which lwIP's pbuf_add_header refuses to prepend an L2 header onto,
 * so ip_forward silently drops it. We copy RX into a contiguous PBUF_RAM and inject it directly. */
#include "lwip/pbuf.h"
#include "lwip/netif.h"
#include "esp_netif_net_stack.h"

#include "test_report.h"   /* T2 mesh-ap: this is the GATE support role (no verdict; up-marker) */

#ifdef TEST_HW_RESTART
/* S3 stage 1b: force a chip restart on a LIVE gateway and score the AP-side restore. This is the only
 * fixture that CAN score it -- test-mesh-hwrestart has no AP netif, so its AP arm could only be
 * verified by beacons, which say nothing about keys or per-STA state. Here the AP vif carries real
 * IP traffic in both directions. */
#include "lwip/sockets.h"
#include "ping/ping_sock.h"

#define STA_IP          "192.168.12.2"    /* the client behind our AP (test-apsta-sta pins this) */
#define BCAST_IP        "192.168.12.255"  /* group-addressed => GTK-encrypted => the PN probe */
#define BCAST_PORT      9                 /* discard */
#define BCAST_PERIOD_MS 200
#define STA_WAIT_S      70
#define SETTLE_S        20
#define RECOVER_WAIT_S  45
#define PING_COUNT      5
#endif

/* ---- Mesh params (match rimba-halow-mesh) ---------------------------------- */
#define MESH_ID         "rimba-mesh"
#define MESH_S1G_CHAN   27
#define MESH_MAX_PLINKS 16

/* ---- AP params (match rimba-halow-ap / rimba-halow-sta) -------------------- */
#define LINK_SSID       "rimba-ping"
#define LINK_PSK        "rimbahalow"
#define LINK_S1G_CHAN   27
#define LINK_OP_CLASS   68
/* Left at 4 while the L2-bridge gate (rimba-halow-mesh-ap / test-mesh-gate-ap) moved to 16. This is
 * the SUPERSEDED L3-routing gate: a separate 192.168.12.0/24 behind the AP with lwIP ip_forward, no
 * proxy-ARP push and no client table, kept because two T2 tests still exercise the concurrency path
 * through it -- both with a single STA. Raising it here would buy nothing and would cost the ~900 B of
 * umac_sta_data per slot on an app that is not the shipped gate. */
#define LINK_MAX_STAS   4

#define AP_SUBNET_IP    "192.168.12.1"
#define AP_SUBNET_MASK  "255.255.255.0"

static const char *TAG = "rimba-mesh-ap";

static uint8_t g_mesh_mac[6];
static esp_netif_t *g_mesh_netif;
static esp_netif_t *g_ap_netif;
static esp_netif_driver_base_t g_ap_driver_base;
static volatile uint32_t g_ap_rx, g_ap_tx, g_mesh_rx;

/* AP authorized-STA tracking (status cb fires during the assoc window; heartbeat logs it). */
#define TRACK_MAX 8
static uint8_t s_sta_macs[TRACK_MAX][MMWLAN_MAC_ADDR_LEN];
static volatile int s_sta_n;
#ifdef TEST_HW_RESTART
static volatile int s_auth_events;   /* AUTHORIZED transitions seen, ever */
#endif

static void ap_sta_status_cb(const struct mmwlan_ap_sta_status *st, void *arg)
{
    (void)arg;
    if (st == NULL) return;
    int idx = -1;
    for (int i = 0; i < s_sta_n; i++)
        if (memcmp(s_sta_macs[i], st->mac_addr, MMWLAN_MAC_ADDR_LEN) == 0) { idx = i; break; }
    if (st->state == MMWLAN_AP_STA_AUTHORIZED) {
        if (idx < 0 && s_sta_n < TRACK_MAX)
            memcpy(s_sta_macs[s_sta_n++], st->mac_addr, MMWLAN_MAC_ADDR_LEN);
#ifdef TEST_HW_RESTART
        /* Every AUTHORIZED transition is one association. The count is what tells a restored station
         * entry apart from a fresh one: if it advances across the restart the client re-associated,
         * and any traffic that then flows says nothing about whether the restore worked. Without this
         * the after-ping is a PASS the arm did not earn. */
        s_auth_events++;
#endif
    } else if (st->state == MMWLAN_AP_STA_UNKNOWN && idx >= 0) {
        for (int j = idx; j < s_sta_n - 1; j++)
            memcpy(s_sta_macs[j], s_sta_macs[j + 1], MMWLAN_MAC_ADDR_LEN);
        s_sta_n--;
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
    g_ap_tx++;
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

/* Per-vif RX: hand the frame to the netif for that vif (mesh->mesh netif, AP->AP netif).
 *
 * We DON'T use esp_netif_receive() here: it wraps the frame in a zero-copy, NON-CONTIGUOUS PBUF_REF
 * (esp_pbuf_allocate), and lwIP's pbuf_add_header() refuses to prepend an L2 header onto a
 * non-contiguous pbuf (pbuf.c: `else`/`!force` -> return 1). So when ip_forward re-transmits a
 * received frame on the OTHER vif, ethernet_output can't add the new 802.3/mesh header and drops it
 * (fw counted, but the frame never reaches halow_transmit). This is THE gateway forward-path bug.
 *
 * Fix: copy the frame into a CONTIGUOUS PBUF_RAM (forwardable — pbuf_add_header takes the contiguous
 * branch) with link headroom, and inject it via netif->input (exactly what esp_netif's own
 * wlanif_input does). Applies to BOTH directions (AP->mesh forward and mesh->AP reply) since both
 * cbs funnel through here. We own the mmpkt (the ext-cb handed it over); we copied it, so we free it
 * exactly once and never pass it to esp_netif -> no double-free with the PBUF_REF custom-free path. */
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

static void gw_mesh_rx_cb(struct mmpkt *mmpkt, const struct mmwlan_rx_metadata *md, void *arg)
{
    (void)md;
    g_mesh_rx++;
    gw_rx_deliver(mmpkt, (esp_netif_t *)arg);
}

static void gw_ap_rx_cb(struct mmpkt *mmpkt, const struct mmwlan_rx_metadata *md, void *arg)
{
    (void)md;
    g_ap_rx++;
    gw_rx_deliver(mmpkt, (esp_netif_t *)arg);
}

static void gw_setup_ap_netif(void)
{
    /* AP netif with a DHCP SERVER so STAs are zero-config (task #5 de-hardcode; superseded later by the
     * 802.11s L2-bridge port). The IP goes in the INHERENT config (like stock WIFI_AP) so the netif is
     * BORN with 192.168.12.1 and AUTOUP's auto-start of dhcps sees a valid server IP. (A 0.0.0.0 IP
     * makes dhcps fail — dhcpserver.c: ip4_addr_isany -> "could not obtain pcb" — and a later
     * set_ip_info would then abort with DHCP_NOT_STOPPED, boot-looping the gate.) So we do NOT call
     * set_ip_info here. Reuse the WIFI_STA netstack (generic ethernet L2 glue); dhcps is L3/UDP. */
    static esp_netif_ip_info_t ap_ip;              /* runtime-init: esp_ip4addr_aton is not const */
    ap_ip.ip.addr      = esp_ip4addr_aton(AP_SUBNET_IP);
    ap_ip.gw.addr      = esp_ip4addr_aton(AP_SUBNET_IP);   /* = router option offered to STAs */
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

    /* AP netif L2 addr = the AP vif BSSID, so ARP on the AP subnet resolves to the AP vif. */
    uint8_t bssid[MMWLAN_MAC_ADDR_LEN] = { 0 };
    if (mmwlan_ap_get_bssid(bssid) == MMWLAN_SUCCESS)
        esp_netif_set_mac(g_ap_netif, bssid);

    /* Create/attach the underlying lwIP netif + input path (mirrors mmhalow's wifi_start) and bring it
     * up. WITHOUT action_start, esp_netif_receive() would deref a NULL input fn (PC=0 on first RX).
     * AUTOUP + the inherent IP => dhcps auto-starts on 192.168.12.1. */
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
    ip.gw.addr = esp_ip4addr_aton("10.9.9.1");
    ip.netmask.addr = esp_ip4addr_aton("255.255.255.0");
    ESP_ERROR_CHECK(esp_netif_set_ip_info(n, &ip));
    ESP_LOGI(TAG, "mesh netif static IP %s (gateway host)", ipbuf);
    ESP_LOGI(TAG, "gateway ready: STA under the AP (192.168.12.0/24) routes to the mesh (10.9.9.0/24)");
    /* Up-marker the orchestrator waits on (Role.up_marker="gate-ready"). Support role: no RESULT --
     * the STA reporter emits the verdict. gate mesh IP = %s so the mesh peer's return route is set. */
    TEST_INFO("gate-ready: mesh+AP concurrent, gate mesh IP %s, AP 192.168.12.1", ipbuf);
    vTaskDelete(NULL);
}

#ifdef TEST_HW_RESTART
/* ---- S3 stage 1b: score the AP-side restore across a forced chip restart ------------------------
 *
 * Two probes, because neither alone can carry the verdict.
 *
 * PROBE A -- group-key PN continuity, and it is the one that does not depend on the client.
 * bcast_task() sends a UDP datagram to 192.168.12.255 five times a second. That is group-addressed,
 * so the AP encrypts it with the GTK held on the AP's common station -- the very keychain step 5 of
 * umac_ap_handle_hw_restarted() re-pushes. Off air, its CCMP packet number must keep CLIMBING across
 * the restart. A rewind to a low PN is precisely the defect S2 documented: connection_keys_install_key()
 * COPIES tx_seq out of the struct it is handed, so a REBUILT key restarts the PN at 0 while every peer
 * keeps its replay window -- every frame then silently dropped as a replay, every install returning
 * success. Scored by tools/ap_gtk_pn_cap.py on chronium; this fixture only has to keep the train running.
 *
 * PROBE B -- a unicast ping to the client across the AP link, which exercises the per-STA chip entry
 * and the pairwise key (step 4 + step 5's other half).
 * ⚠ It CANNOT stand alone. Measured 2026-08-26: a client deauths ~1.1 s into the outage and
 * re-associates ~22 s later, so an after-ping that succeeds may be talking to a FRESH association and
 * would prove nothing about the restore. s_auth_events is the discriminator, and the verdict below
 * returns INCONCLUSIVE rather than PASS whenever it advanced. Reporting that honestly is the point:
 * it converts stage 1's inference into a recorded measurement of why this rig cannot score it. */

static volatile int  s_ping_ok;
static volatile bool s_ping_done;
/* Flipped immediately before the trigger; switches the broadcast train to its longer payload so the
 * capture can tell pre-restart frames from post-restart ones by length alone. */
static volatile bool s_bcast_post;
static void on_ping_success(esp_ping_handle_t h, void *a) { (void)h; (void)a; s_ping_ok++; }
static void on_ping_end(esp_ping_handle_t h, void *a) { (void)h; (void)a; s_ping_done = true; }

/* Replies received, or 0 on any failure. Deliberately does NOT assert -- a dead client is a result. */
static int ping_sta(int count)
{
    ip_addr_t target = { 0 };
    ipaddr_aton(STA_IP, &target);

    esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
    cfg.target_addr  = target;
    cfg.count        = count;
    cfg.timeout_ms   = 2000;
    cfg.interval_ms  = 1000;
    esp_ping_callbacks_t cbs = { .on_ping_success = on_ping_success, .on_ping_end = on_ping_end };
    esp_ping_handle_t hdl = NULL;
    if (esp_ping_new_session(&cfg, &cbs, &hdl) != ESP_OK) return 0;

    s_ping_ok = 0;
    s_ping_done = false;
    esp_ping_start(hdl);
    for (int i = 0; i < (count * 3) + 10 && !s_ping_done; i++) vTaskDelay(pdMS_TO_TICKS(1000));
    esp_ping_stop(hdl);
    esp_ping_delete_session(hdl);
    TEST_INFO("ping %s: %d/%d replies", STA_IP, s_ping_ok, count);
    return s_ping_ok;
}

/* The group-addressed train probe A reads. Runs for the whole test, restart included: the gap in it
 * is the outage, and the PN either side of that gap is the measurement. A send failing during the
 * outage is expected and must not stop the task. */
static void bcast_task(void *arg)
{
    (void)arg;
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        TEST_INFO("bcast|socket() failed (%d) -- probe A has no train to measure", errno);
        vTaskDelete(NULL);
        return;
    }
    int on = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on));

    struct sockaddr_in dst = { 0 };
    dst.sin_family = AF_INET;
    dst.sin_port = htons(BCAST_PORT);
    dst.sin_addr.s_addr = ipaddr_addr(BCAST_IP);

    /* TWO payloads of DIFFERENT LENGTHS, switched at the trigger. This is how the capture tool finds
     * the restart, and it has to be length rather than content because the frames are GTK-encrypted --
     * the sniffer can see how big they are and nothing else.
     *
     * The first version of this arm had no marker and the tool looked for the largest gap in the train.
     * That is wrong here, and the bench proved it on the first run: the board's own REBOOT leaves a
     * bigger hole (3.33 s) than the chip restart does (~1 s), and a reboot legitimately installs a
     * fresh GTK at PN 1 -- so the tool read a textbook PASS as a FAIL. A restart has to be IDENTIFIED,
     * not guessed at from the shape of the trace. */
    static const char payload_pre[]  = "rimba-gtk-pn-probe";                 /* 18 B, before */
    static const char payload_post[] = "rimba-gtk-pn-probe-after-restart";   /* 32 B, after  */
    uint32_t n = 0;
    for (;;) {
        const char *p = s_bcast_post ? payload_post : payload_pre;
        size_t plen = s_bcast_post ? sizeof(payload_post) - 1 : sizeof(payload_pre) - 1;
        if (sendto(sock, p, plen, 0, (struct sockaddr *)&dst, sizeof(dst)) >= 0) {
            n++;
        }
        if ((n % 250) == 0 && n > 0)
            TEST_INFO("bcast|%" PRIu32 " group frames sent (phase %s)", n, s_bcast_post ? "post" : "pre");
        vTaskDelay(pdMS_TO_TICKS(BCAST_PERIOD_MS));
    }
}

static int hw_restart_count(void)
{
    struct mmwlan_stats_umac_data st = { 0 };
    if (mmwlan_get_umac_stats(&st) != MMWLAN_SUCCESS) return -1;
    return (int)st.hw_restart_counter;
}

static void hw_restart_arm_task(void *arg)
{
    (void)arg;

    xTaskCreate(bcast_task, "gtk_pn", 4096, NULL, 4, NULL);
    TEST_INFO("bcast|group-addressed train started to %s (%d ms) -- probe A reads its CCMP PN off air",
              BCAST_IP, BCAST_PERIOD_MS);

    /* A client must be associated BEFORE the restart, or probe B has nothing to restore and the whole
     * arm degrades to the beacon-only test we already have. */
    int waited = 0;
    while (s_sta_n == 0 && waited < STA_WAIT_S) { vTaskDelay(pdMS_TO_TICKS(1000)); waited++; }
    TEST_STEP("sta-before", s_sta_n > 0, "%d client(s) authorized after %ds (%d association event(s))",
              s_sta_n, waited, s_auth_events);
    if (s_sta_n == 0) {
        TEST_INCONCLUSIVE("no client associated to the AP within %ds, so the per-STA restore has "
                          "nothing to act on. Probe A (the group-key PN train) is still valid -- read "
                          "the capture -- but probe B says nothing this run", STA_WAIT_S);
        TEST_END("mesh-ap-gate");
        vTaskDelete(NULL);
        return;
    }

    int ping_before = ping_sta(PING_COUNT);
    TEST_STEP("data-before", ping_before > 0, "%d/%d replies from the client across the AP link",
              ping_before, PING_COUNT);
    if (ping_before == 0) {
        TEST_INCONCLUSIVE("no traffic crossed the AP link BEFORE the restart, so a post-restart "
                          "failure would say nothing about recovery. Fix the rig and re-run");
        TEST_END("mesh-ap-gate");
        vTaskDelete(NULL);
        return;
    }

    /* Give the capture a real pre-restart PN baseline before pulling the rug out. */
    TEST_INFO("settling %ds to build the off-air PN baseline", SETTLE_S);
    vTaskDelay(pdMS_TO_TICKS(SETTLE_S * 1000));

    int auth_before = s_auth_events;
    int rst_before = hw_restart_count();
    /* Mark the train BEFORE triggering: frames already in flight stay short, and everything the AP
     * sends from the restart onwards is long. The boundary between the two lengths is the event. */
    s_bcast_post = true;
    TEST_INFO("bcast|switching the group train to its post-restart payload length (the capture's marker)");
    TEST_INFO("restart-trigger|hw_restart_counter=%d, %d client(s) up, %d association event(s) so far",
              rst_before, s_sta_n, auth_before);
    if (mmwlan_force_hw_restart() != MMWLAN_SUCCESS) {
        TEST_INCONCLUSIVE("mmwlan_force_hw_restart() refused -- the WLAN subsystem is not active, so "
                          "no restart happened and nothing is measured");
        TEST_END("mesh-ap-gate");
        vTaskDelete(NULL);
        return;
    }

    for (int i = 0; i < RECOVER_WAIT_S; i++) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if ((i % 10) == 9)
            TEST_INFO("  +%ds since restart: hw_restart_counter=%d ap_stas=%d assoc_events=%d "
                      "ap_tx=%" PRIu32, i + 1, hw_restart_count(), s_sta_n, s_auth_events, g_ap_tx);
    }

    int rst_after = hw_restart_count();
    bool restarted = (rst_before >= 0) && (rst_after > rst_before);
    TEST_STEP("restart-ran", restarted, "hw_restart_counter %d -> %d", rst_before, rst_after);

    int ping_after = ping_sta(PING_COUNT);
    int auth_after = s_auth_events;
    bool reassociated = (auth_after > auth_before);
    TEST_STEP("data-after", ping_after > 0, "%d/%d replies (was %d/%d before)",
              ping_after, PING_COUNT, ping_before, PING_COUNT);
    TEST_INFO("assoc-churn|%d association event(s) before the restart, %d after -- %s",
              auth_before, auth_after,
              reassociated ? "the client RE-ASSOCIATED, so the after-ping does not test the restore"
                           : "no re-association: the after-ping crosses the RESTORED association");

    if (!restarted) {
        TEST_INCONCLUSIVE("hw_restart_counter did not advance (%d -> %d): the trigger returned SUCCESS "
                          "but the queued event never reached the handler. Nothing here is a verdict "
                          "on recovery -- fix the trigger first", rst_before, rst_after);
    } else if (reassociated) {
        TEST_INCONCLUSIVE("the chip restarted and the AP is serving clients again (%d/%d replies), but "
                          "the client RE-ASSOCIATED during the outage (%d -> %d association events). "
                          "That ping crossed a FRESH association, so it does NOT verify the per-STA "
                          "restore -- a working AP would pass this either way. Probe A (the group-key "
                          "PN train, scored off air) is unaffected and is the verdict for this run",
                          ping_after, PING_COUNT, auth_before, auth_after);
    } else if (ping_after == 0) {
        TEST_FAIL("the chip restarted (%d -> %d) and the client did NOT re-associate, so its chip "
                  "station entry and pairwise key are exactly what umac_ap_handle_hw_restarted() "
                  "re-pushed -- and no traffic crosses it: %d/%d replies before, %d/%d after. The "
                  "per-STA restore is broken. Check the console for 'AP: restore sta_state ... FAILED' "
                  "or 'AP: reinstall keychain FAILED'",
                  rst_before, rst_after, ping_before, PING_COUNT, ping_after, PING_COUNT);
    } else {
        TEST_PASS("the chip restarted (%d -> %d), the client held its association THROUGH the outage "
                  "(%d association events, unchanged), and traffic crosses it: %d/%d replies after vs "
                  "%d/%d before. The per-STA chip entry and pairwise key were genuinely restored. "
                  "Cross-check probe A off air for the group key",
                  rst_before, rst_after, auth_after, ping_after, PING_COUNT, ping_before, PING_COUNT);
    }
    TEST_END("mesh-ap-gate");
    vTaskDelete(NULL);
}
#endif /* TEST_HW_RESTART */

void app_main(void)
{
    TEST_BEGIN("mesh-ap-gate", "mesh + SoftAP + IP routing on one MM6108 (gate support role)");
    ESP_LOGI(TAG, "=== all-ESP Mesh-gate: mesh + SoftAP + IP routing on one MM6108 ===");

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_netif_init());

    esp_read_mac(g_mesh_mac, ESP_MAC_WIFI_STA);
    g_mesh_mac[0] = (g_mesh_mac[0] | 0x02) & 0xFE;

    mmhalow_init(NULL);                 /* creates the mesh netif (WIFI_STA_DEF) + boots morselib */
    mmhalow_print_version_info();
    g_mesh_netif = mmhalow_get_netif();
    /* This netif egresses on the MESH vif (the STA host-slot). Tag it so morselib's per-VIF datapath
     * routes gateway-forwarded packets out the mesh vif; without this the UNSPECIFIED tag is ambiguous
     * once the concurrent AP vif comes up and the forward is dropped/misrouted (STA->mesh path dead). */
    mmhalow_set_tx_vif(MMWLAN_VIF_STA);

    mmwlan_override_max_tx_power(1);     /* close-bench RX-overload guard; drop for range */

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

    /* --- 3) L3 gateway wiring: 2nd netif + per-vif RX demux + forwarding --- */
    gw_setup_ap_netif();
    /* Override mmhalow's single plain RX cb with per-vif ext cbs (gateway RX demux delivers mesh
     * as VIF_STA, AP clients as VIF_AP). */
    ESP_ERROR_CHECK(mmwlan_register_rx_pkt_ext_cb(MMWLAN_VIF_STA, gw_mesh_rx_cb, g_mesh_netif) !=
                    MMWLAN_SUCCESS);
    ESP_ERROR_CHECK(mmwlan_register_rx_pkt_ext_cb(MMWLAN_VIF_AP, gw_ap_rx_cb, g_ap_netif) !=
                    MMWLAN_SUCCESS);

    xTaskCreate(mesh_net_task, "mesh_net", 4096, NULL, 5, NULL);
#ifdef TEST_HW_RESTART
    xTaskCreate(hw_restart_arm_task, "hwrst_arm", 8192, NULL, 5, NULL);
#endif

    ESP_LOGI(TAG, "on-air identities: MESH SA=" MACSTR, MAC2STR(g_mesh_mac));

    uint32_t s = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (++s % 5 == 0) {
            uint8_t peer_macs[UMAC_MESH_MAX_PEERS][6] = {{0}};
            uint8_t n_peers = mmwlan_mesh_peer_count(peer_macs);
            ESP_LOGI(TAG, "alive uptime=%" PRIu32 "s  mesh_peers=%u ap_stas=%d  ap_rx=%" PRIu32 " ap_tx=%" PRIu32 " mesh_rx=%" PRIu32 "  heap=%" PRIu32,
                     s, (unsigned)n_peers, s_sta_n, g_ap_rx, g_ap_tx, g_mesh_rx, esp_get_free_heap_size());
        }
    }
}
