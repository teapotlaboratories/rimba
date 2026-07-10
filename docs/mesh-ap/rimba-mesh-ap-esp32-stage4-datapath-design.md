# Stage 4 design — per-stad datapath dispatch (all-ESP mesh+AP gateway routing)

**Goal.** Route real traffic through the all-ESP Mesh-gate: an ESP STA under the SoftAP
→ gateway ESP (mesh+AP concurrent) → `ip_forward` → HaLow mesh → 2nd node, and the
return path. Stages 1–3 (concurrent vifs + per-vif beacon) are done + on-air-proven; this
is the **data-plane** piece, blocked on morselib's single global datapath mode.

**Status:** design only (assessed + anchored 2026-07-08). The cheap **RX demux is already
landed** (`umac_datapath.c:887`, on branch `feat/mesh-ap-concurrency`). The TX refactor
below is **not** implemented — it is a core-datapath change and wants its own bench
regression. All anchors grep-verified in this tree; paths relative to
`components/halow/components/mm-iot-sdk/framework/morselib/src/umac/`.

## Scope: L2 only

**This design is strictly L2 (the 802.3 ↔ 802.11 boundary). L3 and above are out of scope.**
The morselib datapath's boundary with the host is the **802.3 (Ethernet) frame**: TX consumes
a `struct umac_8023_hdr` (dest/src MAC + ethertype) from lwIP and builds the 802.11 frame; RX
rebuilds an 802.3 header (`umac_datapath_generate_8023_header`) and hands it to
`esp_netif_receive`. It inspects **ethertype** only (EAPOL, the ≥1500 SNAP threshold) and
**never IP addresses** — so Gaps A–D are protocol-agnostic above ethertype and carry IPv4,
IPv6, and any non-IP ethertype through the gateway for free.

Two distinct "forwardings" meet at this gateway; keep them separate:
- **802.11s mesh forwarding = L2, inside morselib (already works).** A mesh frame whose
  mesh-DA isn't us is relayed to the next hop by MAC/HWMP (`umac_datapath.c:867`,
  `umac_mesh_forward_data`). No IP involved.
- **Inter-subnet routing = L3, in lwIP, above this design (Gap E).** Routing an AP-subnet
  packet (`192.168.12.0/24`) to the mesh subnet (`10.9.9.0/24`) — the actual "gateway"
  behaviour — is `CONFIG_LWIP_IP_FORWARD` + two netifs + the routing table. morselib only ever
  sees the resulting per-netif 802.3 frames; it does not route, NAT, or read L3 headers.

So the gateway needs both halves, cleanly layered: Gaps A–D make the **L2** datapath per-vif
so each netif's traffic gets correct 802.11 encapsulation; **L3** forwarding sits entirely in
lwIP (Gap E). Anything above the 802.3 header is lwIP's concern, not the datapath's.

## The blocker, precisely

`struct umac_datapath_data` holds ONE global `const struct umac_datapath_ops *ops`
(`datapath/umac_datapath_data.h:82`), swapped wholesale by
`umac_datapath_configure_{sta,ap,mesh,ibss}_mode` (`datapath/umac_datapath.c:3200/3343/3527`
+ `umac_ap.c:227`, `umac_mesh.c:3126`). In a gateway, `mesh_start` sets mesh ops then
`ap_start` sets AP ops → **last writer wins, `ops = datapath_ops_ap`**. Worse,
`umac_datapath_process_tx_frame` (`datapath/umac_datapath.c:2067,2084`) builds the 802.11
header via `data->ops->construct_80211_data_header` and prepends the Mesh Control header
gated on the **global** `umac_mesh_is_active()` — applied to *every* stad. Net: an AP-client
downlink would get mesh 4-addr framing + a Mesh Control header, and a mesh-peer frame would
get AP 3-addr framing. Both wrong.

TX vif *routing* is otherwise fine: `umac_datapath_tx_frame` (`:2285`) looks up the stad by
destination/RA (`lookup_stad_by_tx_dest_addr`/`_by_peer_addr`), and `metadata->vif` is only
an active-check in `mmwlan_tx_pkt` (`umac.c:1406-1423`). So the fix is **per-stad framing
dispatch**, not API-level vif tagging.

## The enabler

Each `struct umac_sta_data` already carries a `vif_id`
(`umac_sta_data_get/set_vif_id`, `data/umac_data.h:120,123`), and TX queues are **per-stad**
(`umac_sta_data_queue_pkt`, `datapath/umac_datapath.c:3164`) — only the dequeue *scan*
differs by mode (`_sta` scans AP clients `:3157`; `_mesh` scans common+peer stads
`:3410`/`mesh_get_next_tx_stad:3410`). Per-mode header builders already exist:
`_sta` (`:2002`), `_ibss` (`:3286`), `_mesh` (`:3462`). So a stad can self-describe its
framing; the refactor is to dispatch by stad, not by a global mode.

## Gaps → staged edits

**Gap A — AP-client stads are never vif-tagged. ✅ DONE (2026-07-08, branch
`feat/mesh-ap-concurrency`).** `umac_sta_data_set_vif_id` was called only by mesh
(`umac_mesh.c:602` → `mesh_ctx.vif_id`); the AP admit path never set it, so AP clients
defaulted to vif_id 0. **Implemented:** `umac_ap_add_sta` now sets
`umac_sta_data_set_vif_id(stad, data->vif_id)` alongside bssid/peer/security (`umac_ap.c:~585`),
and `umac_ap_start` tags `sta_common` right after the vif is resolved (`umac_ap.c:~226`).
`data->vif_id` is `ap_vif_id` when the AP is the concurrent secondary (Stage 1), else the
primary AP vif — correct for standalone AP too. Purely foundational: no code reads a stad's
vif_id yet (Gaps B/C consume it), so zero regression; builds clean.

*Linux grounding ([[proper-fix-follow-linux]], verified on chronite `~/halow`):* vif_id is a
property of the VIF, assigned at add-interface (`morse_driver/mac.c:3132`
`morse_cmd_add_if(&mors_vif->id,…)`; `struct morse_vif { u16 id; }` `morse.h:517`), and a TX
frame is tagged from its **egress** vif — `mac.c:978`
`tx_info->flags |= …VIF_ID_SET(mors_vif->id)` where `mors_vif = info->control.vif`
(`mac.c:2169`). A station belongs to its `sdata`/vif (`sta->sdata`); mac80211 builds the
3-addr/4-addr header per `sdata->vif.type` and the driver only stamps vif_id. morselib is both
layers, so the faithful analog is: stad carries its vif's id (this Gap), and framing is chosen
by that vif's **type** (Gap B).

**Gap B — framing keyed on global mode, not the vif. ✅ DONE (2026-07-08, branch
`feat/mesh-ap-concurrency`).** In Linux, framing is chosen by the egress vif's **type**
(mac80211 `ieee80211_build_hdr` switches on `sdata->vif.type` — `NL80211_IFTYPE_MESH_POINT`
builds the 4-addr + Mesh Control header, `…_AP` the 3-addr), and the driver only stamps vif_id
from that same egress vif (`morse_driver/mac.c:978`). **Implemented** that per-vif selection in
`umac_datapath_process_tx_frame`, guarded so it engages **only when a mesh AND a concurrent AP
are both active** — off the gateway path every branch keeps `umac_mesh_is_active()` byte-for-byte:
  - Exposed the two static builders (`umac_datapath_construct_80211_data_header_{ap,mesh}`,
    now declared in `umac_datapath_private.h`) so the common path can pick either.
  - Computed `tx_is_mesh_frame` = (in gateway) the stad's vif type via
    `umac_interface_get_vif_type_mask(umac_sta_data_get_vif_id(stad)) & MESH`, else
    `umac_mesh_is_active()`. Gateway calls `…_mesh` for mesh stads, `…_ap` otherwise; stamps
    `tx_metadata->vif_id = stad_vif_id` (mirrors `mac.c:978`).
  - Re-gated **every** per-frame mesh branch in the function on `tx_is_mesh_frame` (not the
    global): the Mesh Control header prepend, the QoS "Mesh Control Present" bit (`:2208`), the
    multi-hop next-hop key_stad lookup (`:2160`), and the mesh SW-CCMP path (`:2243`) — so an AP
    downlink stays a plain 3-addr HW-crypto frame while a mesh is up.
  Builds clean. **Not yet effective end-to-end** — mesh TX frames still won't reach this
  function until Gap C (the gateway dequeue/lookup) lands.

**Gap C — the ops set is single-mode; the gateway needs the WHOLE datapath demux (bigger than
first scoped; confirmed by a 16-agent adversarial review 2026-07-08).**

*C-1 DONE + reviewed CLEAN in isolation:* the egress vif is threaded through the TX path
(`umac_datapath_tx_frame` gained `enum mmwlan_vif vif`; `mmwlan_tx_pkt` passes `metadata->vif`,
internal callers pass UNSPECIFIED; the mesh netif uses the STA host-slot; the STA active-check
also accepts a mesh vif). `umac_datapath_gateway_lookup()` scopes the TX stad lookup by vif
(VIF_AP→AP clients, VIF_STA→mesh, UNSPECIFIED→dest-MAC). The review verified **non-gateway paths
are byte-identical** and the lookup logic is correct.

*But the review found the feature is not gateway-functional yet — TWO must land before C-2 works:*

- **P0 — shared-static aliasing (CRITICAL, pre-existing; FIXED 2026-07-08).** `umac_data.c` had ONE
  file-scope `static struct umac_sta_data` shared by the STA/AP/IBSS/mesh "common" stads — safe only
  while those were mutually exclusive. In a gateway, `mmwlan_ap_enable`'s `umac_sta_data_alloc_static`
  memset+reconfigured the **mesh** common stad as AP (its keys/BSSID/vif_id), silently corrupting the
  mesh data AND control plane (HWMP/peering TX all go through `mesh_ctx.common_stad`). Not caught by
  the Stage-3 beacon test (beacons don't use `common_stad`). **Fix:** dedicated
  `umac_sta_data_alloc_static_mesh()` backing for the mesh common; mesh common now also carries its
  vif_id explicitly.
- **The gateway demux is whole-datapath, not just TX dequeue.** Every `data->ops`-mediated lookup is
  AP-only in a gateway (mesh peer stads are heap objects in `mesh_ctx.peers[]`, never in
  `data->stas[]`), so C-2 must cover ALL of them, else mesh RX/RC are dead the moment the AP is up:
  - **RX ingress lookup** `umac_datapath.c:1840` `data->ops->lookup_stad_by_peer_addr(ta)` → a mesh
    peer's TA isn't found → frame dropped at `:1848` **before** Gap D's demux. **Mesh RX dead.** (also
    the RX-dedup pre-check `:1657`.)
  - **TX dequeue** `process_tx` (AP dequeue scans only AP stads) → mesh TX never drained +
    `num_pkts_queued` leak.
  - **TX-status** `:2785` `lookup_stad_by_aid` → mesh peer frames get no `umac_rc_feedback`
    (regresses the just-landed P6c airtime metric whenever an AP is co-active).
  - **enqueue** must dispatch by the stad's vif (AP→`umac_ap_queue_pkt`, mesh→`_tx_queue_frame_sta`).
  - **process_rx_mgmt** by ingress vif (= C-3; fixes the AUTH mesh-peer-vs-AP-client ambiguity).

  **C-2 (revised) — IMPLEMENTED + builds (2026-07-08; under adversarial re-review before Gap E).**
  A `datapath_ops_gateway` is installed by `configure_ap_mode` when `umac_datapath_gateway_active()`
  (mesh already up). Every stad-facing op dispatches by the stad's vif TYPE via
  `umac_datapath_stad_is_mesh()`: combined `lookup_stad_by_peer_addr`/`_by_tx_dest_addr` (AP-client
  else mesh-peer — fixes the RX-ingress drop @1840 + dedup @1657), `dequeue` over BOTH sets,
  `enqueue` by stad vif (AP→`umac_ap_queue_pkt`, mesh→`_tx_queue_frame_sta`), `get_sta_state` /
  `is_stad_tx_paused` / `set_stad_sleep_state` per vif, and `process_rx_mgmt_frame` routed by the
  **ingress** `rx_metadata->vif_id` (= folds in C-3; fixes the AUTH mesh-peer-vs-AP-client overlap).
  Pre-assoc allow-list is the AP∪mesh union. tx-status routes `lookup_stad_by_aid` by the stamped
  `tx_metadata->vif_id` (AP/mesh AIDs collide). `umac_datapath_gateway_active` +
  `umac_datapath_configure_gateway_mode` exported; the `_ap` builders/mgmt-handler de-static'd.

  *C-2 review (20-agent workflow, 2026-07-08) — 1 CRITICAL + 3 fixed, 2 deferred:* **CRITICAL (fixed)**
  `umac_datapath_tx_mgmt_frame_ap` never stamped `tx_metadata->vif_id`, so AP probe/auth/assoc/action
  responses egressed on the mesh primary vif (0) → client association (esp. secured, via per-vif
  HW-crypto) broken; fixed by stamping `umac_ap_get_vif_id(umacd)` when an AP is active (IBSS/standalone
  stay vif 0; also fixes tx-status misrouting). **Fixed:** same fn's NULL-addr common-stad lookup
  returned the *mesh* common in a gateway → resolve to `umac_ap_lookup_sta_by_addr` when
  `gateway_active`; dequeue was strict AP-first (mesh starves) → alternate AP/mesh; PROBE_REQ always
  routed to the AP handler. **Deferred (non-blocking, tracked):** gateway `mmwlan_mesh_stop` doesn't
  free the host-side AP (`umacd->ap`/supplicant) → SoftAP unrestartable + leak (ties to `mmwlan_ap_disable`);
  gateway RX peer-lookup never returns NULL for an unknown unicast TA. Verified CLEAN: P0 static split,
  non-gateway byte-identical, per-vif framing/demux, tx-status vif-scoping, beacon concurrency.

  **⚠️ GAP-E WIRING (load-bearing):** in gateway mode mesh RX is delivered to the **VIF_STA** ext_cb
  slot (vs VIF_AP in mesh-only). The app MUST register the mesh netif under `MMWLAN_VIF_STA` + the AP
  netif under `MMWLAN_VIF_AP`, and the mesh netif TX must tag `metadata.vif = MMWLAN_VIF_STA`, or mesh
  RX is silently dropped.

  ---
  *(original C-2 spec, for reference — now implemented above):* `data->ops` is one struct (AP's, last
  writer), but AP and mesh differ in five
functions that all matter concurrently. A `datapath_ops_gateway` (installed by
`configure_ap_mode` when mesh is already active — mesh-first is mandated) must override:
  1. `lookup_stad_by_tx_dest_addr` — unicast: `umac_ap_lookup_sta_by_addr` (returns NULL for a
     non-client) → else `…_lookup_stad_by_tx_dest_addr_mesh` (peer / HWMP next-hop / group).
  2. `lookup_stad_by_peer_addr` (RX/TX-status TA) — AP client by MAC → else mesh peer.
  3. `enqueue_tx_frame` — dispatch by the stad's vif (Gap A): AP client → `umac_ap_queue_pkt`
     (keeps `num_pkts_queued` accounting the AP dequeue relies on); mesh stad →
     `umac_datapath_tx_queue_frame_sta`.
  4. `dequeue_tx_frame` — serve `umac_ap_tx_dequeue_frame` then
     `umac_datapath_tx_dequeue_frame_mesh` (both drain per-stad queues; AP-priority is fine for
     the demo, note the fairness caveat).
  5. `process_rx_mgmt_frame` — **the hard one.** Mesh peering (`ACTION` MPM/AMPE + `AUTH` SAE)
     and AP client bring-up (`ASSOC`/`AUTH`/`PROBE`) both arrive; `AUTH` overlaps, so it can NOT
     be dispatched by subtype. Needs the **ingress vif** to route (mesh mgmt handler vs AP).
  Visibility: the AP funcs are public (`umac_ap.h`); the mesh funcs are `static` in
  `umac_datapath.c` (`:3405/3419/3461`) — define the gateway ops there and expose/extern as
  needed. Anchors: AP dequeue `umac_ap.c:840`, AP lookup `umac_ap.c:469/493`, mesh scanner
  `umac_datapath.c:3410`.

*Two ambiguities that force a design choice.* The current TX/RX path only has the **dest/TA
MAC**, not the vif:
  - **TX broadcast** (ARP on the AP subnet vs the mesh subnet) can't be attributed to a vif by
    MAC, so a combined lookup routes all broadcast to one path.
  - **RX AUTH** (above) can't be attributed to mesh-peering vs AP-client.
  Linux never has this problem: mac80211 always knows the vif — TX carries `info->control.vif`
  (→ `morse_driver/mac.c:978` stamps `mors_vif->id`) and RX is delivered per `sdata`. **The
  faithful fix is to thread the vif through the morselib TX/RX path** rather than guess from
  frame content:
  - TX: pass `metadata->vif` from `mmwlan_tx_pkt` (`umac.c:1425`) into `umac_datapath_tx_frame`
    (`:2336`) and scope the lookup/framing to it (an explicit `MMWLAN_VIF_MESH` enum, or reuse
    the STA slot as the RX demux already does for mesh). Also fixes Gap B's gateway guard and
    lets broadcast pick the right vif.
  - RX mgmt: route by the frame's vif (the FW tags RX with a vif id —
    `MORSE_RX_STATUS_FLAGS_VIF_ID`, mirrored on the ESP RX metadata) instead of subtype.
  **Recommendation:** do the vif-threading version — it's the Linux model, resolves both
  ambiguities, and avoids fragile heuristics. A narrower "unicast-only + static ARP + peering
  established before AP" path could demo routing without it, but it's fragile (breaks on
  re-peering / any broadcast) and not worth shipping. Gap C is a deliberate, bench-regressed
  pass (pure STA / AP / mesh + multi-hop must all still pass), not an incremental add.

**Gap D — RX demux (DONE).** `umac_datapath.c:887` now maps mesh-vs-AP RX by
`mesh_ctrl_present` when both are active → `MMWLAN_VIF_STA` (mesh) / `MMWLAN_VIF_AP` (AP), so
two host netifs can be fed. Backward-compatible (only diverges when both active).

**Gap E — app + lwIP (app-level, not morselib).** In `firmware/rimba-halow-mesh-ap`:
  - create a 2nd `esp_netif` for the AP; register `mmwlan_register_rx_pkt_ext_cb(MMWLAN_VIF_STA,
    …mesh netif)` and `(MMWLAN_VIF_AP, …AP netif)` (overrides mmhalow's single plain cb);
    each netif's `transmit` → `mmwlan_tx_pkt` (dest-driven routing, `vif=UNSPECIFIED` ok).
  - mesh netif `10.9.9.gw`, AP netif `192.168.12.1`; `CONFIG_LWIP_IP_FORWARD=y`.
  - 2nd mesh node needs a return route `192.168.12.0/24 via 10.9.9.gw` (Linux §A3 gotcha).

## Recommended shape

Add an explicit per-stad datapath mode (mesh / infra-BSS) set at stad creation (Gaps A/B),
and a `_gateway` ops triple (lookup/dequeue that scan both stad sets) selected when
`ap_vif_id != INVALID` and mesh is active (Gap C). Non-gateway nodes keep the exact current
single-mode ops — zero behaviour change off the gateway path.

## Risk + verification

Core TX path — regressions hit **all** traffic. Must re-run the full matrix on bench before
trusting: (1) pure STA assoc+ping, (2) pure AP + STA leaf, (3) pure mesh peer+ping+multi-hop
relay (the [[mesh-status-p4-esp-linux-ping]] + [[mesh-security-port]] regressions), then (4)
the gateway end-to-end: STA leaf → AP → mesh → 2nd node, both directions, with a
`morse0` capture confirming AP-client frames are 3-addr (no Mesh Control) and mesh frames are
4-addr. Watch CCMP (mesh per-pair MTK vs AP PTK), per-vif sequence-number spaces
(`mmdrv_set_seq_num_spaces`, `:3035`), and PS/sleep-state (AP buffers for dozing clients vs
mesh). Est. multi-day; same risk class as [[mesh-no-ampdu-aggregation]] /
[[mesh-real-rc-feasible-design]]. Follow [[proper-fix-follow-linux]] (net/mac80211 routes per
`sdata`/vif — the model to mirror) + ship a code-map ([[porting-ships-verified-codemap]]).

---

## Runtime bring-up diagnosis (2026-07-09): AP downlink blocked in FW secondary vif

On-air result: a `rimba-halow-sta` associates to the gateway AP (`rimba-halow-mesh-ap`),
gets its static IP, but **every ping times out** — the AP→STA (fromDS) direction never
reaches air. Uplink (STA→AP, toDS) works: encrypted data decrypts, `ap_rx` increments.

### What was proven (instrumented counters, on `feat/mesh-ap-concurrency`)

Temporary debug getters added to `umac_datapath.c` (marked "remove after diagnosis"),
exposed to the app because the mangler keeps only `mmwlan_*`-prefixed symbols public
(`framework/tools/buildsystem/librarymangler.py` → internal `umac_*` become `mmint_umac_*`):
`mmwlan_dbg_get_txcounters()` (TX-build drop points) and `mmwlan_dbg_get_txstatus()`
(FW tx-status verdict).

The host TX path is verified correct **end to end** for the AP downlink:

| Stage | Evidence |
|-------|----------|
| lwIP → `gw_ap_transmit` → `mmwlan_tx_pkt` | returns SUCCESS (`ap_tx_fail=0`) |
| gateway `tx_frame` stad lookup | finds the AP client (no `MMWLAN_NOT_FOUND`) |
| `process_tx_frame` build | `proc==submit` (43/43), `dropRA=0` |
| 802.11 header | fromDS, `addr2`=e0: (AP vif BSSID, = beacon BSSID the STA associated to), `addr1`=STA |
| HW crypto | `dropKEY=0 swccmpFail=0`; pairwise key installed on `ap_vif_id` (`driver_ap.c:411` → `umac_ap_get_vif_id`) |
| `mmdrv_tx_frame` submit | returns 0 (no host error), `submit=43` |

FW verdict on those 43 submitted frames:
- **chronium morse0 monitor: `fromDS(AP→STA data) = 0`** in every window (`toDS = 41`).
- tx-status: `ps_filt=0 fail=0 unacked=0` — **not** PS-buffered, **not** a TX failure.
- `acked` stuck at 6 = the one-time assoc/EAPOL mgmt frames (does not grow ~1/s with pings).
- The data frames produce essentially no *data* tx-status — they vanish inside the FW.
- Beacons **do** radiate on the secondary vif (Stage 3 concurrency proof).

### Conclusion + next step

The MM6108 FW radiates *beacons* but silently drops *host-queued unicast data* submitted on
the **secondary** AP vif (`ap_vif_id`). This is not a host/morselib datapath bug — every host
stage is correct. It is **not** a hard FW limitation either: memory
[[mesh-ap-concurrency-1179-regression]] records that the Linux `morse_driver` routes data
through a secondary AP vif (mesh on primary `wlan1` + AP on secondary `ap0`). So the FW
supports secondary-vif data TX; the port is **missing a `morse_driver` → FW command/config**
that enables the secondary vif's data path (beyond `mmdrv_cfg_bss` + `mmdrv_start_beaconing`).

Next (task #16): diff `morse_driver`'s AP-on-secondary-vif bring-up (add_interface /
`bss_info_changed` / set-channel / start-AP / any per-vif TX-queue-mapping or data-enable
opcode) against morselib's `umac_interface_add` secondary branch + `umac_ap_start`; port the
missing command. Derive line-by-line from the Linux side ([[proper-fix-follow-linux]]);
ship a code-map ([[porting-ships-verified-codemap]]).

### Follow-up (2026-07-09 pm): deepened + candidate commands empirically ruled out

An ultracode multi-agent diff (morse_driver vs morselib, driver on chronite) proposed two
per-vif FW commands the AP path omits vs the working mesh path: `mmdrv_set_bssid` and
`mmdrv_config_beacon_timer`. Both were rejected:

- **`set_bssid`** — adversarially refuted: Linux `ieee80211_start_ap` (cfg.c) does NOT include
  `BSS_CHANGED_BSSID`, so `morse_cmd_set_bssid` never fires on AP bring-up (only on reconfig,
  util.c:1912). The AP vif's BSSID is bound via `add_if` (its own MAC), symmetric for primary
  (umac_interface.c:312) and secondary (:330). Adding it would DIVERGE from Linux.
- **`config_beacon_timer`** — EMPIRICALLY TESTED on-air (added to `umac_ap_start`): no effect,
  fromDS still 0. It is beacon-timer-only (mbssid.c:57-60 issues it `(false)` on a still-data-
  active non-tx AP vif; struct is a single enable bit). Reverted.
- **channel** — `mmdrv_set_channel` and `morse_cmd_set_channel` are both device-global (no
  vif_id); ruled out.

Instrumented tx-status (getters on the branch) hardened the diagnosis: the host stamps
`tx_metadata->vif_id = 1` (ap-stamp counter: `host_stamped_vif=1`, 463 frames) and skbq.c:838
encodes `MORSE_TX_CONF_FLAGS_VIF_ID_SET` unconditionally, so the wire descriptor carries vif 1.
Yet those 463 AP data frames produce ~zero tx-status and zero radiation (chronium: 0 frames of
any type/addr addressed to the STA). `__skbq_data_tx_finish` (skbq.c:674) writes only
`status_flags`, never `vif_id` — so the stray `vif=0/aid=1` tx-status observed is a separate
AP-generated path (QoS-Null/PS), not the data. Conclusion stands and is stronger: the FW drops
secondary-vif AP unicast data at the deepest level.

**Decisive next experiment (static analysis exhausted):** (A) reproduce mesh-primary+AP-secondary
on a Linux node (same 1.17.8 fw) and capture the morse_driver→FW command stream, byte-diff vs
morselib; OR (B) confirm the ESP `mm6108.mbin` supports 2 concurrent DATA vifs (the Linux result
used the Linux firmware image, possibly a different build at the same version); OR (C) on-bench:
bring up a mesh PEER and verify mesh DATA flows on vif 0 in the gateway — if it does and AP data
on vif 1 doesn't, the FW only services data on the primary vif.

### Linux command capture (2026-07-09): the definitive driver-vs-morselib diff

Built + deployed an instrumented **1.17.8** morse_driver on chronite (added an unconditional
`dev_info("MORSECAP id=0x%04x vif=%u")` at `command.c:186` logging every FW command's id +
target vif — dynamic_debug is absent on this kernel so `dev_dbg` is a no-op, hence the promotion).
Ran the proven `mesh_gate_up.py` (mesh on `wlan1`=vif0 + AP on `ap0`=vif1), associated the ESP STA.

**Reverse-validation (decisive):** the *same* ESP STA (`bc:2a:33:96:b2:9f`) that receives 0
downlink from its own ESP AP pings the Linux secondary-vif AP **11/0**. So the ESP RX is fine and
the Linux driver's secondary-vif AP data path works — the bug is entirely the ESP's AP-side
downlink TX on the secondary vif.

**Command diff (vif=1, the secondary AP):**

| morse_driver on AP vif | morselib AP path | match? |
|---|---|---|
| SET_CHANNEL (0x01) | mmdrv_set_channel (global) | ✓ |
| BSS_CONFIG (0x06) | mmdrv_cfg_bss | ✓ **byte-identical struct** (beacon_int/dtim/cssid) |
| SET_STA_STATE (0x14) | mmdrv_update_sta_state | ✓ |
| INSTALL_KEY (0x0a) / DISABLE_KEY (0x0b) | mmdrv_install_key / disable | ✓ |
| GET_CAPABILITIES / GET_CHANNEL_FULL / HW_SCAN | reads | ✓ |
| **MCAST_FILTER (0x3c) — 13×** | **(none — not defined or sent)** | ✗ **ONLY DELTA** |
| (relies on FW/host beacon; no START_BEACONING cmd) | START_BEACONING (host beacon engine) | arch diff |

So the command SETS are equivalent except **`MORSE_CMD_ID_MCAST_FILTER`**, which morselib does not
define or send. Payload = `{count, hw_addr[]}` (a multicast RX address list; driver
`morse_cmd_cfg_multicast_filter`, sent from the per-vif filter callback `mac.c:4295`).
Mechanistically RX-only, but it may gate whether the FW delivers the STA's **broadcast ARP request**
to the host on a secondary vif — if it doesn't, ARP never resolves (matches the symptom).

**Next:** port `mmdrv_cfg_multicast_filter` (+ the CMD id/struct) into morselib and wire it into the
AP configure-filter path (mirror `mac.c:4295`); rebuild `rimba-halow-mesh-ap`; test whether fromDS
AP→STA data / ARP now works. If not, the delta is sub-command (data-TX descriptor or command
timing) and needs data-plane instrumentation, not the command channel.

Build recipe + gotchas recorded in task #16 (checkout 1.17.8 tag; `-DCONFIG_MORSE_SPI` etc. via
KCPPFLAGS; swap BOTH morse.ko + dot11ah.ko for symbol-CRC; churn wedges the SPI chip → reboot
GPIO-resets it). Bench restored to stock 1.17.8 + radio-silent.

#### MCAST_FILTER eliminated — it's not a command difference

Follow-up: the one command delta (`MCAST_FILTER` 0x3C) is morse_driver's multicast **whitelist**,
gated by module param `enable_mcast_whitelist` (=Y on chronite; `prepare_multicast` returns
`count=0` when off). So the Linux AP applies a *restrictive* multicast RX whitelist and still
works, while the ESP sends no filter (receives **all** multicast) and fails. A restrictive
multicast-RX filter cannot cause a *unicast* downlink drop. (It was ported to morselib to test,
then reverted once its whitelist nature was confirmed.)

**Net conclusion of the capture:** the secondary-vif AP downlink drop is **not** a missing/different
FW command — the command sets are equivalent. It is a **data-plane or beacon-architecture** difference:
1. **Data TX descriptor** — morse_driver may set a field on AP downlink data frames (vif=1) that
   morselib doesn't; needs DATA-channel instrumentation (MORSECAP is command-channel only).
2. **Beacon architecture** — morse_driver uses FW-offloaded beaconing (BSS_CONFIG + beacon-channel
   template; no START_BEACONING command), morselib uses a host beacon engine
   (`mmdrv_start_beaconing`); the FW may require FW-offloaded beaconing to originate data on a
   secondary vif. (config_beacon_timer alone was tested → no effect.)

Next experiment: re-instrument the Linux driver's DATA TX path (log `tx_info` descriptor for AP
downlink data on vif=1) and byte-diff vs morselib `convert_tx_metadata_to_tx_info` (skbq.c:838).

#### Descriptor + rate eliminated — the frame is well-formed; FW drops it on vif 1

ESP-side instrumentation (`mmwlan_dbg_get_aprate` in `process_tx_frame`) shows the AP downlink
frame is fully well-formed: `host_stamped_vif=1`, `rate0=MCS0 bw=0 UNUSED=0` (a valid transmittable
rate, not `MMRC_MCS_UNUSED`), HW-encrypt + valid key, valid tid. `skbq.c:838` encodes VIF_ID
unconditionally. So the ESP submits a perfect, correctly-tagged frame (`mmdrv_tx_frame` returns 0)
and the FW **silently drops it on vif 1** (no tx-status, no radiation) — while the identical frame +
commands work on Linux vif 1 (ESP STA pings it 11/0) and on the ESP's own primary vif 0 (standalone
AP works).

**Exhaustive elimination of host→FW causes:** commands equivalent (BSS_CONFIG byte-identical),
MCAST_FILTER = whitelist red-herring, SET_CHANNEL device-global, descriptor well-formed (valid
vif/rate/key/HW_ENC). The difference is **not visible at the host→FW command/descriptor interface.**
The MM6108 originates data on vif 0 but not vif 1 under morselib; morse_driver enables vif 1 via
something below the command channel.

Remaining candidates (both require going below the command channel):
1. **Command payload/ordering** not yet byte-compared — extend the driver MORSECAP capture to
   HEXDUMP full command bodies for AP-vif commands (esp. ADD_INTERFACE + SET_STA_STATE), byte-diff
   vs morselib. Cheapest decisive next check.
2. **Beacon architecture** — morse_driver may load the AP beacon template into the FW (offloaded,
   FW transmits autonomously = "live BSS") while morselib host-serves each beacon
   (`mmdrv_start_beaconing`); the FW may originate data only on an offloaded-beacon BSS on a
   non-primary vif. Needs beacon-SKB-channel capture (not commands).

#### Firmware eliminated — same 1.17.8 image; QoS/EDCA tested (no effect)

Per-vif QoS/EDCA was implemented + tested (morselib sets QoS with `UNKNOWN_VIF_ID` vs morse_driver
`vif=0`; added explicit per-AP-vif EDCA in `umac_ap_start`) → **no effect** on the downlink drop.
Reverted.

Then ruled out the firmware entirely: the ESP `mm6108.mbin` (MMFW, 401076 B) is the **same 1.17.8
firmware** as Linux `mm6108.bin` (ELF, 480664 B). `cmake/mm-fw-gen/morse_firmware.cmake` converts the
vendored upstream ELF (`vendor/morse-firmware` = "the single source of truth") to `.mbin` via
`convert-bin-to-mbin.py`; both are `rel_1_17_8`. (The SDK-default `morsefirmware/mm6108.mbin` is 1.17.6
and **unused** — the build overrides it.) So ESP and Linux run **identical firmware** with different
host stacks → the bug is a host-side morselib difference and **is fixable**.

Everything at the command-ID / BSS_CONFIG / SET_STA_STATE / descriptor level is eliminated. Remaining
host candidates: (1) unverified command **payloads** (ADD_INTERFACE, INSTALL_KEY — only BSS_CONFIG +
SET_STA_STATE byte-compared); (2) command ordering; (3) beacon architecture (FW-offloaded template vs
host-served). Next: extend the driver MORSECAP capture to hexdump full AP-vif command bodies and
byte-diff vs morselib.
