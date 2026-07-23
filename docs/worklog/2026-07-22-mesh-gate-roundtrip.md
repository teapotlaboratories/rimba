# 2026-07-22 — Mesh-gate L2 bridge: BIDIRECTIONAL ROUND-TRIP VERIFIED (STA ↔ mesh node through the gate)

**Status:** ✅ **DONE + ON-AIR VERIFIED (PASS).** An AP client behind the gate and a HaLow mesh node now hold a
**continuous bidirectional ping** across the gate's L2 bridge — the STA pings the mesh node's real IP and gets ICMP
replies back, seq after seq, with no drops. This closes the L2-bridge core datapath: S5b (mesh→AP) + S5c (AP→mesh) +
MPP + the two datapath completions below. morselib (`components/halow`) is UNCOMMITTED (S1–S5c + round-trip).

## Goal

A *true* bidirectional round-trip through the gate L2 bridge (the finishing piece flagged in the S5c worklog): an AP
client `C` pings a mesh node `D` by IP, `D` replies, and the reply reaches `C` — both legs over the 6-address AE
bridge, not the old L3 `ip_forward`. This exercises two datapath completions the one-way S5b/S5c legs did not:

1. **RX-deliver-eaddrs** — when a proxied `AE_A5_A6` frame is delivered to the *local* stack, hand lwIP an 802.3
   header of `[dst=eaddr1][src=eaddr2]` (the proxied endpoints) so the receiving node replies to the real off-mesh
   source `C`, not to the mesh forwarder/gate. Mirrors `net/mac80211` `ieee80211_strip_8023_mesh_hdr`.
2. **TX-auto-proxy** — when a mesh node originates a unicast to a dst with no mesh path but a **known MPP entry**
   (reachable via a gate), auto-proxy it into the mesh as an AE frame (`eaddr1=dst`, `eaddr2=us`) toward the gate,
   instead of dropping it. This is how `D`'s reply finds its way back to `C`.

## What was implemented (morselib `umac_datapath.c`, uncommitted)

**RX-deliver-eaddrs** (local-delivery 802.3-header build, `umac_datapath_process_rx_data_frame_after_reorder`):
```c
bool deliver_ae = rx_ae && umac_interface_addr_matches_mac_addr(stad, rx_eaddr1);
umac_datapath_generate_8023_header(deliver_ae ? rx_eaddr1 : dot11_get_da(header),
                                   deliver_ae ? rx_eaddr2 : dot11_get_sa_data(data_hdr),
                                   llc_ethertype, &header_8023);
```
**Load-bearing detail — the `eaddr1==us` gate.** A design workflow caught that this line *also* runs on the GATE for
its S5b mesh→AP frames (`mesh_da(addr3)==us` but `eaddr1=client!=us`, and the relay branch doesn't fire for
`addr3==us`, so it falls through to local delivery). An **ungated** `rx_ae ? …` rewrite would have changed the gate's
proven `gw_mesh_rx_cb` netif header from `[dst=addr3=G][src=addr4=D]` to `[dst=C][src=D]`, disturbing its
`CONFIG_LWIP_IP_FORWARD=y` path. Gating on `eaddr1==us` (verified: `mmwlan_mesh_tx_proxied` sets `addr3=`on-mesh
target and `eaddr1=`final_dst, so at the true endpoint `eaddr1==addr3==us`, while at the gate `eaddr1=C!=addr3=G==us`)
means **only the true final endpoint rewrites**; the gate's bytes stay identical → zero regression. `rx_eaddr1/2` are
read only when `rx_ae` (they're populated + bounds-checked only for a valid `AE_A5_A6` frame).

**TX-auto-proxy** (`umac_datapath_mesh_proxy_offmesh` + `umac_datapath_mesh_frame_undeliverable`): on an
undeliverable mesh unicast (no next-hop path, not a direct peer), proxy **only if** `mmwlan_mesh_mpp_lookup(dst)`
HITS — re-encapsulate `[802.3][L3]` → `[LLC/SNAP][L3]` and `mmwlan_mesh_tx_proxied(dst, us, …)`. On an MPP **miss**
it returns false and falls through to `umac_mesh_start_discovery(dst)`. **This gating is the safety property:** a
real-but-not-yet-discovered mesh dest (no path/peer/MPP) is NOT wrapped to a gate — it kicks HWMP discovery exactly as
before, so plain multi-hop mesh unicast is unregressed.

## Bench config (single subnet, so reachability is by MAC via MPP)

- **board0 = GATE** — `rimba-halow-mesh-ap` (mesh `rimba-mesh` MAC `e2:72:a1:f8:ef:a4` + AP `rimba-ping`; S5b+S5c).
- **board1 = D (mesh node)** — `rimba-halow-mesh PEER_MAC=bc:2a:33:96:b2:33`. IP `10.9.9.100` (derived from the mesh
  MAC `e2:72:a1:f8:f9:40`), an ICMP responder. `PEER_MAC` pins a **static ARP `10.9.9.50 → the STA's MAC`** so `D`
  can reply to the off-mesh client without a broadcast ARP into the mesh (not yet bridged) — the reply's L2 dst = the
  STA's MAC, which fires TX-auto-proxy (`mpp(STA→gate)` was learned from the STA's inbound AE frame).
- **board2 = STA (AP client C)** — `rimba-halow-sta L2_DST_MAC=e2:72:a1:f8:f9:40`. IP `10.9.9.50` (same subnet as
  `D`), static ARP `10.9.9.100 → D's mesh MAC`, pings `10.9.9.100`. Because the ping's L2 dst = `D` (not the gate),
  it MUST traverse the S5c L2-bridge proxy, not the L3 `ip_forward` (which only fires for frames addressed to the
  gate) — so a reply is unambiguous proof of the L2 bridge, both ways.

## ✅ VERIFICATION — on-air, 3-node mesh+AP+STA (PASS)

**STA** — continuous replies from the mesh node (12+ consecutive, no drops), ~27–77 ms round-trip:
```
I (85663) rimba-halow-sta: reply from 10.9.9.100 seq=41 time=34 ms
I (87673) rimba-halow-sta: reply from 10.9.9.100 seq=42 time=47 ms
...
I (107703) rimba-halow-sta: reply from 10.9.9.100 seq=52 time=77 ms
```
**GATE** — both legs, every 2 s:
```
S5c AP->mesh: 100 B from bc:2a:33:96:b2:33 to e2:72:a1:f8:f9:40      (STA's echo, AP→mesh proxy)
S5b mesh->AP: 106 B to AP client bc:2a:33:96:b2:33 (src e2:72:a1:f8:f9:40)   (D's reply, mesh→AP delivery)
```
**D** — `estab_peers=1 peer[0]=e2:72:a1:f8:ef:a4` (peered with the gate), replying to the STA.

The full chain proven: STA→(gate S5c ingress, AE `eaddr2=STA/eaddr1=D`)→D; **D delivers it locally as
`[dst=D][src=STA]` (RX-deliver-eaddrs) and its lwIP replies to the STA**; D's reply→(TX-auto-proxy, `mpp(STA→gate)`
hit)→AE to the gate→(gate S5b egress)→STA. The `eaddr1==us` gate is validated live: the gate keeps logging S5b
correctly (its path untouched) **and** D rewrites so it answers the STA, not the gate.

**Radio-silent cleanup done:** board0/1/2 → `rimba-hello`; chronium `morse0`/`wlan1` down (no Linux node was used —
this was a pure ESP↔ESP↔ESP flow).

## Not done — remaining L2-bridge finishing touches
- **Gate double-delivery (cosmetic):** D's reply AE also reaches the gate's *own* mesh netif (line-975 local
  delivery, non-AE header since `eaddr1!=us`); with `ip_forward` on, the gate may emit a spurious/duplicate
  ARP/forward for the STA's IP. Benign for ping (the real reply is delivered via S5b). Clean fix = have
  `umac_mesh_ae_rx_deliver` return "handled" and skip local delivery when a cb consumed it (only fires on the gate,
  which is the sole registrant) — deferred to keep the round-trip change surface minimal.
- **Proxy-ARP / broadcast bridging:** this test uses static ARPs on both ends. A zero-config flow needs the gate to
  bridge broadcast/multicast AP↔mesh — the **multicast AE (`AE_A4`)** path (out of S3 scope). Last datapath piece.
- **Retire `MESH_GATE_IP` + the L3 `ip_forward` gateway:** the L2 proxy is now the primary path; the L3 path still
  coexists in `rimba-halow-mesh-ap`.
- **S6:** live-Linux interop. NOTE the AE-mode nuance carried since S3 — this port uses `AE_A5_A6` uniformly
  (self-consistent ESP↔ESP); Linux uses `AE_A4` for the C→mesh-node direction. An interop concern, not blocking
  ESP↔ESP.

## Footguns
- The RX rewrite MUST stay gated on `eaddr1==us` (not `rx_ae` alone, not `mesh_da==us`) — both are true on the gate.
- TX-auto-proxy MUST stay gated on an MPP **hit** — proxying any undeliverable unicast would mis-route a
  not-yet-discovered mesh dest to a gate instead of kicking HWMP discovery.
- `D` needs a **static ARP for the client** (no `ETHARP_TRUST_IP_MAC`), else its reply is addressed to the gate's
  mesh MAC and never fires TX-auto-proxy.
- Both C and D must be on **one subnet** (`10.9.9.0/24` here) — a subnet mismatch makes the round-trip silently fail
  at L3 even while S5c ingress logs success.
- Keep `D` in default `multihop=true` (a leaf node can't originate a proxied reply).
- MPP entries age out (`MESH_PATH_LIFETIME_MS`); bidirectional ping keeps `mpp(C→gate)` fresh. Asymmetric/keepalive
  flows could see the reply leg miss after expiry (no gate fallback on the MPP-miss path).

## Files
- `components/halow/.../umac/datapath/umac_datapath.c` — RX-deliver-eaddrs (gated) + TX-auto-proxy — **uncommitted**.
- `firmware/rimba-halow-mesh/main/app_main.c` (+ `CMakeLists.txt`) — `TEST_PEER_MAC` static-ARP (round-trip D).
- `firmware/rimba-halow-sta/main/app_main.c` — round-trip mode under `TEST_L2_DST_MAC` (STA on `10.9.9.50`, pings
  `RT_DST_IP=10.9.9.100`; `on_ping_success` now logs the real responder via `ESP_PING_PROF_IPADDR`).
- `Makefile` — threads `PEER_MAC=` → `TEST_PEER_MAC`.
- Memory: `mesh-gate-8021s-port-planned` (updated: round-trip verified).
