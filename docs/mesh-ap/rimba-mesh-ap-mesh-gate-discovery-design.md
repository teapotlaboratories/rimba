# Design: Proper (Linux-derived) 802.11s Mesh-Gate Discovery + Proxy — replacing the hard-coded default-gw hack

**Status: APPROVED 2026-07-10 — SIGN-OFF #1 (RANN+IS_GATE+learned MPP) and #2 (L2-bridge single-subnet)
are accepted; blocked only on SIGN-OFF #3 (a bench Linux gate for the byte-diff gold standard) + a code-map
re-pin to the 2.12.3 morselib tree (§2/§3) before any S-code is written.** Author: porting-lead
investigation, 2026-07-10. This is the mandated pre-port deliverable under `[[proper-fix-follow-linux]]`
+ `[[porting-ships-verified-codemap]]`. **⚠ Both anchor sets must be re-pinned before the §3 code-map is
treated as verified:** (1) the morselib anchors were grepped on the **pre-forward-port 2.10.4** tree — the
2.10.4→2.12.3 forward-port **moved the files into `umac/mesh/` + `umac/datapath/` subdirs and shifted every
line number** (e.g. `umac_mesh.c:353` is now `#define DOT11_IE_PREQ`, not the RANN defines), so re-grep them
on the current `7d7f76ad` tree; (2) the Linux `net/mac80211` anchors are from a fetched reference copy
(open80211s/Intel, ~6.12.x) and must be re-pinned to the bench's exact `rpi-linux`/`morse_driver` checkout.

Goal: let mesh nodes DISCOVER the gate and reach off-mesh hosts the standards way, replacing the mesh
node's static gw — already reduced to an **optional, commented-out** `#define MESH_GATE_IP "10.9.9.1"`
(`firmware/rimba-halow-mesh/main/app_main.c:39`, applied at `:70` under `#ifdef`) — with a gw driven
**dynamically from the discovered gate**. (The STA-side de-hardcode already happened via DHCP.)

---

## 0. The load-bearing correction — "GANN" is a misnomer for a follow-Linux port (SIGN-OFF #1)

Linux `net/mac80211` — our mandated reference — contains **zero** `gann`/`pxu`/`pxuc` code. It implements
the 802.11s mesh-gate + proxy with a deliberate subset:

- **Gate discovery** = the `RANN_FLAG_IS_GATE` bit (`= 1<<0`) carried inside a **RANN** element
  (`WLAN_EID_RANN = 126`) or a proactive PREQ — *not* a standalone GANN element (EID 125).
- **Gate advertisement** = the Mesh Configuration "Connected to Mesh Gate" bit (informational).
- **Proxy** = a **learned MPP table** (external-addr → proxying mesh STA) populated by snooping 6-address
  (Address-Extension) *data* frames — *never* PXU(137)/PXUC(138) management frames.

**Implication:** implementing the literal spec GANN/PXU/PXUC would (a) have **no Linux code to derive
from** → violates the project rules, and (b) be **un-verifiable on-air** — a Linux bench gate emits
RANN(126), so the required `[[verify-onair-chronium-monitor]]` byte-diff would fail against any invented
GANN frame. The GANN/PXU byte layouts are also not byte-authoritative in the spec (interval width, action
codes, proxy-info optionals unconfirmed).

**➤ SIGN-OFF #1:** "Proper follow-Linux gate" == **RANN + IS_GATE + learned MPP**. We will NOT ship
GANN(125)/PXU(137)/PXUC(138). Insisting on the literal IEEE elements voids the derive-from-mac80211
premise — treat that as a separate, explicitly-authorized project.

---

## 1. Architecture decision: L3-routing vs L2-bridge (SIGN-OFF #2)

A genuinely-proper 802.11s gate *is* the L2 model — the Linux gate mechanism (RANN → `known_gates` →
`send_to_gates` → `prepare_for_gate` → MPP) only closes end-to-end when the gate is an **L2 portal
bridging one L2 segment to the DS**.

### 1a. Why the current L3 gate can't just "add RANN"
Today: gate `rimba-halow-mesh-ap` = two `esp_netif`s (mesh `10.9.9.0/24` + AP `192.168.12.1/24`) joined by
lwIP `ip_forward` (`gw_rx_deliver`, app_main.c:156) — an IP **router**, not an 802.11s portal (no gate
advertisement on air at all). RANN carries the gate's **mesh MAC**, not its IP. Turning a discovered gate
MAC into an lwIP default route needs a MAC→IP mapping the mesh layer doesn't have — so "de-hardcode on L3"
just swaps one hack for another. Linux `send_to_gates` works **below IP**, but only fires if lwIP handed
the frame to the mesh netif, which a two-subnet L3 node won't (no route to `192.168.12.0/24`).

### 1b. Why L2-bridge is the proper target
One subnet (e.g. `10.9.9.0/24`) spanning mesh **and** AP clients; gate **bridges** AP↔mesh. Node → AP-STA:
STA on-link → node ARPs → no mesh path to STA MAC → `send_to_gates` wraps it **AE_A5_A6** (eaddr1 = STA
MAC, eaddr2 = orig SA) toward a discovered gate → gate strips the mesh header and **bridges** A5/A6 out the
AP netif. Reverse: gate snoops the STA's SA, originates AE frames; every node **learns** `STA-MAC → gate`
in its MPP table. No hard-coded gw, no MAC→IP kludge, single subnet, every mechanism a direct Linux port,
byte-diffable against a live Linux gate.

### 1c. Trade-offs

| Axis | L3-retain (ip_forward + dynamic route) | **L2-bridge (recommended)** |
|---|---|---|
| Standards-faithful | Partial (discovery only) | **Full** (RANN + MPP + AE + send_to_gates) |
| De-hardcode clean | No — needs a MAC→IP shim (new hack) | **Yes** — gate learned at L2 |
| Subnets | Two | **One** (AP clients get `10.9.9.x`) |
| AP addressing change | None | **Yes** — renumber onto the mesh subnet |
| External-MAC transparency | No (NAT-like L3) | **Yes** (A5/A6 preserved) |
| Touches the just-built concurrency `umac_datapath.c` AE path | No | **Yes** (regression risk) |
| Effort | Lower (S1–S2 + shim) | Higher (S1–S5) |
| On-air Linux interop provable | Discovery only | **End-to-end** |

### 1d. Recommendation
**Target the L2-bridge model, staged.** Land the *inert, additive* discovery signalling first (RANN TX/RX +
gate list + beacon bit, S1/S2) — independently on-air-verifiable, near-zero regression risk to the proven
mesh+AP concurrency — then the AE datapath (S3) and MPP proxy + `send_to_gates` (S4, the hard 20%), then
flip the app to an L2 bridge and delete the hard-coded gw (S5).

**Fallback (if the single-subnet/bridge change is rejected):** ship **S1+S2 only** on the existing L3 gate
— makes the ESP a real, discoverable, Linux-interoperable on-air gate, but the node still needs a
route/default-gw to *use* it (a dynamic-default-route shim, **not** standards-way reachability).

**➤ SIGN-OFF #2:** Adopt the **L2-bridge single-subnet** target (AP clients renumber onto `10.9.9.0/24`,
gate becomes an L2 bridge/portal instead of an ip_forward router)?
**➤ SIGN-OFF #3:** A live Linux node configured as a **gate** (`mesh_gate_announcements 1` →
`PROACTIVE_RANN`) for the gold-standard RANN + AE byte-diff — today's bench Linux nodes are plain mesh
points, so there is no gold-standard frame until this is actioned.

---

## 2. Staged port plan (each stage independently on-air-verifiable)

morselib edit sites are under
`components/halow/components/mm-iot-sdk/framework/morselib/src/umac/` — post-forward-port specifically
`umac/mesh/umac_mesh.c` and `umac/datapath/umac_datapath.c` (the flat `umac/` paths **and** the line numbers
below predate the 2.12.3 forward-port and **must be re-grepped** on the current tree). Linux refs re-pin to
the bench checkout before the code-map ships.

**✅ S1 DONE + ON-AIR VERIFIED (2026-07-21)** — ESP gate RANN byte-identical to a live Linux gate on chronium
`morse0` (worklog `2026-07-21-mesh-gate-s1-rann.md`). **✅ S2 DONE + ON-AIR VERIFIED (2026-07-22)** — ESP learns a
live Linux gate's RANN (`known_gates=1`), re-floods it byte-exactly (hop+1/ttl−1/metric-accum, origin preserved),
and advertises the Connected-to-Gate beacon bit (formation-info `0x03`); worklog `2026-07-22-mesh-gate-s2-rann-rx.md`.
**✅ S3 DONE + ON-AIR VERIFIED (2026-07-22)** — the ESP builds + parses a 6-address `AE_A5_A6` mesh data frame and
the MM6108 FW delivers/accepts it (ESP↔ESP eaddr round-trip byte-exact), resolving the §4 FW-AE-delivery risk;
worklog `2026-07-22-mesh-gate-s3-ae-datapath.md`. **KEY FINDING: the §4 "CCMP offset/AAD re-derivation" risk does
NOT apply — with host SW-CCMP the Mesh Control (incl. AE eaddrs) is in the encrypted body and the AAD is over the
MAC header only, so AE is crypto-transparent (ccmp.c untouched).** **✅ S4 DONE + ON-AIR VERIFIED (2026-07-22)** —
MPP table + learning (`mpp_path_add` on AE RX) + `send_to_gates`/`prepare_for_gate` (a node with no path to an
off-mesh dest wraps the frame as an AE frame via a discovered gate); verified ESP↔ESP with a direct-peer gate;
worklogs `2026-07-22-mesh-gate-s4a-mpp-learning.md` + `-s4b-send-to-gates.md`. **✅ S4c DONE + ON-AIR VERIFIED
(2026-07-22)** — the relay now preserves AE (`umac_mesh_forward_data` carries eaddrs), so **multi-hop gates work**;
verified 3-node (NODE↔RELAY↔GATE, gate 2 hops away got the endpoints intact); worklog
`2026-07-22-mesh-gate-s4c-ae-relay-preserve.md`. **✅ S5a DONE + ON-AIR VERIFIED (2026-07-22)** — the mesh→AP bridge
hook `mmwlan_mesh_register_ae_rx_cb` gives the app a received AE frame's (eaddr1, eaddr2, payload), which the plain
RX ext-cb can't (stripped with the Mesh Control); worklog `2026-07-22-mesh-gate-s5a-ae-rx-app-hook.md`. **✅ S5b DONE
+ ON-AIR VERIFIED (2026-07-22)** — the gate app (`rimba-halow-mesh-ap`) bridges a proxied mesh AE frame onto its AP
toward the target client (3-node mesh+AP+STA; **mesh+AP concurrency confirmed working on 2.12.3**); worklog
`2026-07-22-mesh-gate-s5b-gate-egress.md`. **✅ S5c DONE + ON-AIR VERIFIED (2026-07-22)** — the gate proxies an AP
client's frame INTO the mesh (new `mmwlan_mesh_tx_proxied`; `gw_ap_rx_cb`), the mesh node learns `mpp(client→gate)`;
**so the GATE now BRIDGES BOTH DIRECTIONS** (3-node mesh+AP+STA); worklog `2026-07-22-mesh-gate-s5c-ap-ingress.md`.
Remaining = L2-bridge finishing touches (bidirectional round-trip + single-subnet/retire `MESH_GATE_IP` + proxy-ARP
via multicast AE). S1–S5c uncommitted in `components/halow`; S5b/S5c in the (tracked) gate app. Verified 2.12.3 anchors live in the S1–S4 code comments (the §3 table below still
shows stale 2.10.4 line numbers — re-pin pending).

**S1 — RANN element + HWMP builder extension + proactive-root timer (GATE side).** Gate periodically
broadcasts a 21-octet RANN with `RANN_FLAG_IS_GATE`; no node behaviour change (fully additive). Edits:
`umac_mesh.c:353` (RANN defines), `:2063` (builder `MPATH_RANN` case emitting `{flags,hopcount,ttl,addr[6],
le32 seq, le32 interval, le32 metric}`), **new** root-mode timer (morselib is pure reactive today),
broadcast unprotected via the group-privacy exemption. Linux: `mesh_hwmp.c:146,1434,1440`, `mesh.c:1760/691/702`.
Verify: byte-diff ESP RANN vs a live Linux gate's RANN. **1–2 days.**

**S2 — RANN RX + gate list + connected-to-gate beacon bit (ALL nodes).** Edits: `umac_mesh.c:1815`
(`mesh_path_entry` += is_gate/is_root/rann_snd_addr/rann_metric), `:2335` (RANN RX case → port
`hwmp_rann_frame_process`: freshness gate, `mesh_path_add_gate` hook), **new** `known_gates`/`mesh_gate_num`,
`:184` (Formation-Info bit0). Linux: `mesh_hwmp.c:914,992,968`, `mesh_pathtbl.c:337,397,89`, `mesh.c:261`.
Verify: a live Linux node adds the ESP gate (`num_gates>0`, `iw mpath dump`). **2–3 days.**

**S3 — 6-address Address-Extension datapath, TX+RX (the hard 20%).** Emit/parse AE_A5_A6; stop discarding
eaddr on RX. Edits: TX `umac_datapath.c:2138` + `:3592` (`construct_80211_data_header_mesh` → A5/A6) +
`umac_mesh.c:2540`; RX `umac_datapath.c:833` (extract eaddr1/eaddr2 instead of skip). Linux: `mesh.c:884,851`,
`tx.c:2726`, `rx.c:2875`. **⚠ CCMP offset/AAD math** (`umac_datapath.c` ~528–628) must be re-derived for the
+12 AE bytes or proxied frames MIC-fail on relay. Verify: AE-frame byte-diff vs Linux; confirm the **MM6108
FW delivers RX AE frames to the host cb** (open risk, §4). **3–5 days.**

**S4 — MPP proxy table + learning + `prepare_for_gate` + `send_to_gates` fallback.** New MPP table
(`mpp_path_add`/`_lookup`), RX learning at `umac_datapath.c:833`, fallback hook at `umac_mesh.c:1995`
(next-hop miss → walk `known_gates`, rewrite via a `prepare_for_gate`-equiv). Linux: `mesh_pathtbl.c:722,274,134,969`,
`mesh_hwmp.c:1425`, `rx.c:2889`. **⚠ FW A4≠TA withhold** (`[[gateway-e2e-forward-mesh-unicast-gap]]`;
`umac_mesh.c:138–142`) — re-verify a 6-addr forward delivers or the P5 host-SW-CCMP still covers it.
**3–4 days.**

**S5 — App: L2 bridge on the gate + single subnet + de-hardcode the node.** Replace the two-netif
`ip_forward` (`rimba-halow-mesh-ap/app_main.c:156`) with an L2 bridge/portal (AP-client MACs proxied into
the mesh); delete `rimba-halow-mesh/app_main.c:124` gw. L3-retain fallback: keep `ip_forward`, install a
dynamic default route from the discovered gate (documented shim). **2–3 days.**

**S6 — On-air verify vs a live Linux gate + ship the code-map.** Whole bench matched Morse 1.17.8
(`[[morse-fw-same-version]]`); byte-diff every new ESP frame vs live Linux (`[[verify-onair-chronium-monitor]]`);
bidirectional interop (Linux gate↔ESP node + ESP gate↔Linux node); radio-silent after
(`[[radio-silent-after-every-test]]`). **1–2 days.**

**Effort roll-up:** ~**12–19 session-days** for the full L2 target; ~**4–7 days** for the S1+S2
discovery-only fallback (which does *not* by itself deliver standards-way reachability).

---

## 3. Function-level code-map skeleton (ship per `[[porting-ships-verified-codemap]]`)

Re-verify every Linux file:line against the **bench-pinned** checkout before this is treated as verified.

| Stage | New/edited morselib symbol | morselib file:line | Linux reference | Linux file:line |
|---|---|---|---|---|
| S1 | `DOT11_IE_RANN`/`RANN_FLAG_IS_GATE`/`HWMP_RANN_IE_LEN` | `umac_mesh.c:353` | `WLAN_EID_RANN` / `ieee80211_rann_ie` | `ieee80211.h` |
| S1 | HWMP builder `MPATH_RANN` case | `umac_mesh.c:2063` | `mesh_path_sel_frame_tx` RANN branch | `mesh_hwmp.c:146` |
| S1 | `umac_mesh_tx_root_frame` (new) + root timer | *new* | `mesh_path_tx_root_frame` / `ieee80211_mesh_rootpath` | `mesh_hwmp.c:1434` / `mesh.c:1760` |
| S2 | `mesh_path_entry` += is_gate/is_root/rann_* | `umac_mesh.c:1815` | `struct mesh_path` | `mesh.h` |
| S2 | RANN RX → `hwmp_rann_process` port | `umac_mesh.c:2335` | `hwmp_rann_frame_process` (gate hook :992) | `mesh_hwmp.c:914` |
| S2 | `mesh_path_add_gate`/`mesh_gate_num`/`known_gates` | *new* (near `:1829`) | same | `mesh_pathtbl.c:337/397/89` |
| S2 | Formation-Info bit0 / Capability bit3 | `umac_mesh.c:184/185` | `mesh_add_meshconf_ie` | `mesh.c:261` |
| S3 | TX AE mesh-control build | `umac_datapath.c:2138` | `ieee80211_new_mesh_header` | `mesh.c:884` |
| S3 | TX 80211 addr (A5/A6) | `umac_datapath.c:3592` | `ieee80211_fill_mesh_addresses` | `mesh.c:851` |
| S3 | forwarded-unicast AE | `umac_mesh.c:2540` | (forward path) | `tx.c:2726` |
| S3 | RX AE extract (stop skip) | `umac_datapath.c:833` | AE parse | `rx.c:2875` |
| S4 | `mpp_path_add`/`mpp_path_lookup` (new table) | *new* | same | `mesh_pathtbl.c:722/274` |
| S4 | `prepare_for_gate`-equiv | *new* | `prepare_for_gate` | `mesh_pathtbl.c:134` |
| S4 | `send_to_gates` fallback hook | `umac_mesh.c:1995` | `mesh_path_send_to_gates` (+trigger) | `mesh_pathtbl.c:969` / `mesh_hwmp.c:1425` |
| S4 | RX MPP learning | `umac_datapath.c:833` | `mpp_path_add(proxied, h_source)` | `rx.c:2889` |
| S5 | gate L2 bridge (replace ip_forward) | `rimba-halow-mesh-ap/app_main.c:156` | portal concept | — |
| S5 | drive gw from the discovered gate (retire the optional `MESH_GATE_IP`) | `rimba-halow-mesh/app_main.c:39/70` | — | — |

---

## 4. Risks / the hard 20%

1. **Regression to the just-proven mesh+AP concurrency.** S3/S4 edit `umac_datapath.c` on the uncommitted
   `feat/mesh-ap-concurrency` datapath — A/B every AE change vs the concurrency + host-SW-CCMP baseline.
2. **CCMP offset/AAD math** — AE_A5_A6 adds 12 bytes ahead of the payload; re-derive the SW-CCMP header/AAD
   length (`umac_datapath.c` ~528–628) or proxied frames MIC-fail on relay.
3. **FW A4≠TA withhold** (`umac_mesh.c:138–142`) — a 6-addr AE frame changes the header the MM6108 FW gates
   on; **probe on-air** that the FW both delivers received AE frames to the host cb (it silently dropped
   foreign-BSSID S1G beacons before — `[[s1g-vs-legacy-beacon-rx-paths]]`) and accepts AE forwards, before S3/S4.
4. **Keying class** — RANN is group-addressed (unprotected group-privacy exemption). The root-confirm
   unicast PREQ (`mesh_hwmp.c:981`) + AE unicast forwards must use the per-peer PTK/MTK (the exact keying
   that broke multi-hop relay before). Wrong = silent drops.
5. **Metric** — RANN freshness uses `airtime_link_metric_get`; confirm it's the same p6c airtime metric or
   root ranking mis-orders.
6. **Load** — root-confirm PREQ per accepted RANN + periodic RANN flood → rate-limit via the existing
   `MESH_PREQ_MIN_GAP_MS` gate (`umac_mesh.c:385`).

Live-Linux A/B is **mandatory** at S1 (RANN), S3 (AE), S6 (interop). **Blocker: no gold-standard frame
exists until a bench Linux node is put in gate mode (SIGN-OFF #3).**

---

## 5. Open decisions the next session cannot start without
- **SIGN-OFF #1 — ✅ APPROVED** (RANN+MPP, not literal GANN/PXU).
- **SIGN-OFF #2 — ✅ APPROVED** — L2-bridge single-subnet target; the L3-retain fallback stays documented
  as the safety valve if S3–S5 regress the concurrency datapath.
- **SIGN-OFF #3 — ☐ OUTSTANDING** — a live Linux gate configured on the bench for the byte-diff gold
  standard (the one true blocker left, alongside the code-map re-pin above).
- Root/gate mode to emit: `PROACTIVE_RANN` (recommended) vs proactive-PREQ-with-gate-bit; align RX + byte-diff
  target with what the Linux bench gate emits by default.
- Re-pin the Linux reference tree to the bench's exact `rpi-linux`/`morse_driver` commit before the §3
  code-map cites line numbers.
