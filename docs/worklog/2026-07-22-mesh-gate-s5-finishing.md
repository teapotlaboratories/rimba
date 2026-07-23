# 2026-07-22 — Mesh-gate S5 finishing touches: ZERO-CONFIG L2 BRIDGE (broadcast/AE_A4 + double-delivery)

**Status:** ✅ **DONE + ON-AIR VERIFIED (PASS).** The gate L2 bridge now works **zero-config**: an AP client pings a
mesh node BY IP with **no static ARP** — the ARP resolves through the bridge (broadcast → mesh → reply) and the
bidirectional ping runs continuously. Two S5-finishing changes landed: **(A)** the double-delivery cleanup (the AE-rx
callback signals consumption so the gate no longer also ip_forwards a delivered frame) and **(B)** broadcast/proxy
bridging via multicast **AE_A4** (the last datapath piece for a zero-config bridge). morselib (`components/halow`) +
the gate app are UNCOMMITTED.

## Goal

Finish S5 (the gate L2 bridge) so it needs no per-node static ARP crutch — the milestone that proves it is a
**zero-config round-trip**: STA pings a mesh node's IP, ARP resolves across the bridge, both ways.

## What was implemented

### (A) Double-delivery cleanup — the AE-rx callback signals consumption
`mmwlan_mesh_ae_rx_cb_t` now RETURNS `bool`; `umac_mesh_ae_rx_deliver` returns it; the RX datapath does
`if (umac_mesh_ae_rx_deliver(...)) goto drop;` — so when the gate's `gate_ae_rx_cb` delivers a frame to an AP client
it returns true and the datapath SKIPS the normal local delivery. Before, the gate's own mesh netif ALSO saw the
frame and (with `CONFIG_LWIP_IP_FORWARD=y`) could spuriously re-inject/ARP it. A plain endpoint node registers no cb
(or returns false), so its local delivery — the round-trip's RX-deliver-eaddrs — is unchanged. `gate_ae_rx_cb`
returns true whenever `eaddr1` is one of our AP clients (WE own that frame, never ip_forward it); the observer cb in
`test-mesh-gate-rx` returns false.

### (B) Broadcast / proxy bridging — multicast AE_A4 (net/mac80211 semantics)
The zero-config insight: only the ARP **request** needs broadcast bridging; the mesh node's ARP **reply** and all
ICMP are unicast and ride the existing AE_A5_A6 + MPP paths (S5b/S5c + TX-auto-proxy).

**AP → mesh (originate).** New `mmwlan_mesh_tx_group_proxied(src, payload, len)` + `umac_mesh_build_group_proxied`:
a gate injects an AP client's broadcast/multicast into the mesh as a **group-addressed AE_A4** data frame — 3-address
fromDS (addr1=broadcast, addr2=addr3=us), Mesh Control flags=`0x01`, ttl, seqnum, **eaddr1 = the off-mesh source**,
then `[LLC/SNAP][L3]`. CCMP-encrypted under our MGTK via `umac_datapath_tx_mesh_group_frame(common_stad)` exactly
like a re-broadcast. The gate's `gw_ap_rx_cb` grew a broadcast branch (`do_group_proxy`): a group DA →
`tx_group_proxied`, then FALL THROUGH to local delivery too (a bridge floods to all ports; the gate's own stack may
want the broadcast — DHCP, ARP-for-gate).

**mesh RX (AE_A4).** The `ae == 0x01` strip branch now EXTRACTS `eaddr1` (bounds-checked), sets `rx_ae_a4`, and
`umac_mesh_mpp_learn(eaddr1, mesh_sa)` — so a node learns `mpp(off-mesh-source → gate)` for the return path. Local
delivery for an AE_A4 frame is `[dst=group (unchanged mesh DA)][src=eaddr1]` == net/mac80211
`ieee80211_strip_8023_mesh_hdr` (AE_A4: `h_source=eaddr1`, `h_dest` kept). So a mesh node sees the AP client's ARP as
`[dst=broadcast][src=client]` and can reply to it.

**re-broadcast preservation (multi-hop, group analog of S4c).** `mesh_rebcast_params` + `umac_mesh_build_rebcast`
carry an optional AE_A4 `eaddr1`; `umac_mesh_handle_group_data` gained `bool ae_a4, const uint8_t *eaddr1` so a
re-broadcast of a proxied multicast keeps `eaddr1` (a plain group frame stays byte-identical — flags=0, no eaddr).
Not exercised by the 2-node bench (single peer) but required for a 3+-node mesh; follows Linux (mesh header forwarded
verbatim).

## ✅ VERIFICATION — on-air, 3-node mesh+AP+STA, ZERO static ARP (PASS)

- **board0 = GATE** `rimba-halow-mesh-ap` (mesh `rimba-mesh` `e2:72:a1:f8:ef:a4` + AP `rimba-ping`).
- **board1 = D** `rimba-halow-mesh` — **no PEER_MAC / no static ARP**, IP `10.9.9.100`, ICMP responder.
- **board2 = STA** `rimba-halow-sta L2_DST_MAC=…` (round-trip mode) — IP `10.9.9.50`, **no static ARP**, pings
  `10.9.9.100` (ARP must resolve through the bridge).

STA: `reply from 10.9.9.100 seq=31..48 time=22–56 ms` — continuous, no drops, with **zero static ARP**.
Gate — the complete zero-config flow, explicitly logged when the STA (re)ARPs:
```
S5 AP->mesh bcast: 36 B from bc:2a:33:96:b2:33 to ff:ff:ff:ff:ff:ff        (STA's ARP request -> AE_A4 into the mesh)
S5b mesh->AP:      42 B to AP client bc:2a:33:96:b2:33 (src e2:72:a1:f8:f9:40)   (D's ARP REPLY bridged back)
S5c AP->mesh:     100 B from bc:2a:33:96:b2:33 to e2:72:a1:f8:f9:40         (ICMP echo, now unicast — ARP resolved)
S5b mesh->AP:     106 B to AP client bc:2a:33:96:b2:33 (src e2:72:a1:f8:f9:40)   (ICMP reply)
```
The `36 B` broadcast = 8 (LLC/SNAP) + 28 (ARP); D's `42 B` reply = 14 (eth) + 28 (ARP). Since the STA holds NO static
ARP, the only way it obtained D's MAC (to send the unicast ICMP the gate logs as `S5c … to …f9:40`) is the broadcast
AE_A4 bridge → D → the unicast ARP reply back. So the bridge is proven **both ways, zero-config**.

**Adversarial review** (4-lens workflow vs the Linux reference: Task-A consumption, AE_A4 RX, AE_A4 TX,
re-broadcast preservation): all returned CLEAN (no confirmed defects). *(Radio-silent cleanup + doc: see below.)*

## Not done (the true remainder)
- **mesh → AP broadcast bridging (Task B2):** the gate does NOT yet forward a MESH node's broadcast to AP clients
  (only AP→mesh). The zero-config round-trip doesn't need it (the ARP reply is unicast), but a mesh node's own
  broadcast (e.g. its ARP-for-a-client) won't reach AP clients. Symmetric to the AP→mesh branch: deliver a received
  group frame to the AP vif in the gate's mesh-RX path.
- **Retire `MESH_GATE_IP` + the L3 `ip_forward` gateway:** the L2 proxy is now primary; the L3 path still coexists.
- **S6:** live-Linux interop (AE_A5_A6-uniform vs Linux's AE_A4 for C→node; and multicast-AE interop).

## Footguns
- The RX AE_A4 delivery is `[dst=group][src=eaddr1]`; the AE_A5_A6 delivery is `[dst=eaddr1][src=eaddr2]` gated on
  `eaddr1==us`. Don't cross them.
- `mmwlan_mesh_tx_group_proxied` sends under the MGTK on the common stad (group key) — NOT a per-peer MTK. A group
  frame keyed to a peer's MTK would be undecryptable by other peers.
- The gate's group branch FALLS THROUGH to local delivery (doesn't `return` like the unicast proxy) — it must not
  `mmpkt_release` before `gw_rx_deliver` (which takes ownership). Only the unicast proxy path releases.
- AP delivery of a client's BROADCAST to `gw_ap_rx_cb` is REAL (confirmed on-air — the `S5 AP->mesh bcast` log) —
  morselib's AP hands client broadcasts to the VIF_AP ext-cb, same as it does unicast-to-non-BSSID (S5c).
- The re-broadcast AE preservation is untested on a 2-node bench (single peer → the re-broadcast is dropped as an
  own-echo). Verify it on a 3-node line before relying on multi-hop group AE.

## Files
- `components/halow/.../umac/datapath/umac_datapath.c` — AE_A4 RX extract + mpp_learn, AE_A4 local delivery,
  Task-A `goto drop` on consume, `handle_group_data` new args — **uncommitted**.
- `components/halow/.../umac/mesh/umac_mesh.c` + `.h` — `ae_rx_deliver` returns bool, `tx_group_proxied` +
  `build_group_proxied`, `build_rebcast`/`handle_group_data` AE_A4, typedef returns bool — **uncommitted**.
- `firmware/rimba-halow-mesh-ap/main/app_main.c` — `gate_ae_rx_cb` returns bool, `gw_ap_rx_cb` broadcast branch.
- `firmware/rimba-halow-sta/main/app_main.c` — zero-config round-trip mode (no static ARP; `PING_TARGET`).
- `firmware/test-mesh-gate-rx/main/app_main.c` — observer `ae_rx_cb` returns false.
- Memory: `mesh-gate-8021s-port-planned` (updated: S5 finishing touches + zero-config bridge verified).
