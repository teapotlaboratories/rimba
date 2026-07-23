# 2026-07-22 — Mesh-gate proxy-ARP: reliable mesh↔AP-client resolution (fixes the lossy-broadcast flakiness)

**Status:** ✅ **DONE + ON-AIR VERIFIED.** The gate now keeps both sides' ARP caches populated so a mesh node ↔
an AP client resolve each other **reliably**, instead of depending on lossy No-Ack broadcast ARP crossing the
bridge (the flaky mesh→AP-client case from the B2 worklog). Gate app only (`rimba-halow-mesh-ap`); no morselib
change. UNCOMMITTED.

## Problem (from the B2 investigation)

The L2 bridge carries unicast reliably, but *resolution* crossed the bridge as a broadcast ARP — and mesh
broadcasts are No-Ack (lossy). So a mesh node reaching an AP client (its ARP request had to reach the far host
AND the reply come back, both over lossy multicast) was hit-or-miss: it "worked" only via the client's periodic
gratuitous ARP (`GARP_TMR_INTERVAL=60 s` + `TRUST_IP_MAC`), i.e. up to 60 s and never for a silent host.

## What was implemented — two layers, both in the gate app

**1) IP↔MAC snoop table.** The gate learns each host's IP↔MAC from every frame it already handles — from ARP
*and* IPv4 (so it learns a host from ANY traffic, e.g. a ping, not just a once-a-minute gratuitous ARP). Learned
in `gw_ap_rx_cb` (AP side), `gw_mesh_rx_cb` (mesh group frames), and `gate_ae_rx_cb`/S5b (mesh nodes, from the
reliable AE frames). Each entry is tagged AP-side or mesh-side.

**2a) Reactive proxy-ARP.** When the gate receives an ARP REQUEST for a host it knows on the OTHER side, it
answers directly with that host's **REAL MAC** (not the gate's) over a reliable link — a unicast on the AP, a
proxied unicast (`mmwlan_mesh_tx_proxied`) on the mesh — and skips the lossy bridge for that frame. Answering with
the real MAC keeps the L2-bridge datapath (S5b/S5c) carrying the actual traffic; the gate only short-circuits the
resolution. (Caveat: reactive proxy-ARP still needs the requester's broadcast ARP to reach the gate, and is often
*masked* by passive gratuitous-ARP learning — so on its own it's only a partial win. Hence layer 2b.)

**2b) Proactive push (the reliable fix).** A 3 s task (`gw_arp_announce_task`) teaches every learned host about
every learned host on the OTHER side over **reliable unicast** (AP downlink / proxied mesh unicast, both acked).
So neither side ever needs to broadcast-ARP across the bridge — resolution stops depending on lossy multicast.
(With `TRUST_IP_MAC` on, an unsolicited ARP reply populates the receiver's cache.)

## ✅ VERIFICATION — on-air, 3-node (mesh node D pings AP client STA, the flaky direction)

- board0 = GATE `rimba-halow-mesh-ap` (+ proxy-ARP). board1 = D `rimba-halow-mesh PING_IP=10.9.9.50` (mesh node
  initiating). board2 = STA `rimba-halow-sta …NO_PING` (AP-client responder at 10.9.9.50).
- Settle so the gate learns both sides, then **reset D** (empty cache) to force a fresh resolution.

Gate: `proxy-ARP push: refreshed 2 cross-bridge ARP mapping(s) via reliable unicast` — every 3 s (16× in the
capture): the gate knows the STA (AP) + D (mesh) and proactively pushes BOTH mappings over reliable unicast.
D: **`reply from 10.9.9.50` from seq 3, 18 replies / 2 timeouts** — D reached the STA fast (~5 s after reset) and
reliably, no lossy-broadcast dependency. (Earlier, silent-host / missed-GARP runs were 0-reply.)

## Notes / follow-ups
- The reactive path is masked by passive learning; the proactive push is what provides the reliability guarantee.
- Snoop table is 24 entries, plain overwrite when full — fine for a small mesh; size it for larger deployments.
- The push is O(AP × mesh) per 3 s — trivial at bench scale; for a large mesh, rate-limit / only push changed
  entries.
- Proxy-ARP + the L2 bridge assume a SINGLE subnet (AP clients + mesh nodes on 10.9.9.x). The gate app still runs
  the AP on 192.168.12.1 with a DHCP server (two subnets); "retire MESH_GATE_IP / single subnet" makes proxy-ARP
  the zero-config default rather than needing a static-IP client. Tested here with the STA static on 10.9.9.50.

## Files
- `firmware/rimba-halow-mesh-ap/main/app_main.c` — snoop table + `gw_arp_snoop` (ARP+IPv4) + reactive `gw_proxy_arp`
  (hooked in `gw_ap_rx_cb`/`gw_mesh_rx_cb`) + `gw_arp_snoop` in `gate_ae_rx_cb` + proactive `gw_arp_announce_task`.
  **Uncommitted.**
- Memory: `mesh-gate-8021s-port-planned` (proxy-ARP done — mesh↔AP-client reliable).
