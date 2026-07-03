# 2026-07-02 — Real per-peer rate control feeding the mesh airtime metric

Replaces P6c's RSSI-seeded rate in `mesh_last_hop_metric` with mmrc's **learned** best-throughput rate +
the **real success probability**, removing P6c divergences **#1** (RSSI-seed) and **#2** (`err` structurally
0). Implemented across 4 files, built, and **hardware-verified** (the metric now tracks the real per-link
loss). Backward-compatible: a cold/unconverged peer is byte-identical to the old behaviour.

## Feasibility — FEASIBLE in morselib (unlike A-MPDU)
Assessed 2026-07-02 (workflow + anchor-verify). The key result: unlike A-MPDU
([[mesh-no-ampdu-aggregation]]), the **tx-status signal reaches the mesh datapath ungated** — mesh
`get_sta_state` = `umac_datapath_get_state_ibss` returns CONNECTED unconditionally, so no
`MMWLAN_STA_CONNECTED` gate blocks feedback. Only two wirings were missing.

## The two gaps + the three edits
**GAP-1 — mmrc was never *started* for mesh peer stads** (so `reference_table` stayed NULL, no learned rate).
- **Edit 1** (`umac_mesh.c`): `umac_mesh_peer_rc_start()` → `umac_rc_start(peer->stad, 0, MMRC_MCS7)` at
  the two ESTAB transitions (after `umac_mesh_link_up_once`, outside the `#if MMWLAN_MESH_SEC_PHASE1` so
  the open build also learns); `umac_rc_stop(peer->stad)` in `mesh_peer_free` before the free (no-op if
  never started).

**GAP-2 — `datapath_ops_mesh.lookup_stad_by_aid` pointed at the STA variant** (`umac_datapath.c`), which
returns the single common (AID-0) stad and AID-mismatches every peer (1..N) → per-peer feedback dropped.
- **Edit 2** (`umac_datapath.c`): new `umac_datapath_lookup_stad_by_aid_mesh` → `aid==0` = common stad,
  else `umac_mesh_peer_stad_at(aid-1)` (mirrors `umac_ap_lookup_sta_by_aid`), wired into `datapath_ops_mesh`.
  Now `umac_datapath_process_tx_status_queue` resolves the per-peer stad and `umac_rc_feedback` lands in
  its `reference_table`.
- **Edit 2b** (`umac_datapath.c`): the keyed relay/group forward path pinned the mgmt (MCS0) table; switch
  it to `umac_rc_init_rate_table_data(stad, …, mmpkt_get_data_length(txbufview))` (mirrors the local-origin
  path) so a relay-dominated node also trains mmrc, not just MCS0. Auto-falls-back to mgmt until RC starts.

**Edit 3 — consume it.**
- New `umac_rc_get_learned_metric(stad, &thr_kbps, &prob)` (`umac_rc.c/h`): `umac_sta_data_get_rc` →
  `mmrc_sta_get_best_rate(reference_table)` → returns `false` if `reference_table==NULL` (RC not started)
  or `table[best.index].evidence==0` (no samples yet); else `thr_kbps =
  mmrc_calculate_theoretical_throughput(best)/1000` + `prob = table[best.index].prob`.
- `mesh_airtime_from_thr_kbps(thr_kbps, prob)` now takes the success probability: the hard-coded `err=0`
  becomes `err = s_unit − s_unit*prob/100` (the real Linux fail term, `mesh_hwmp.c:361-372`); `prob==0` →
  `MAX_METRIC` (== `fail_avg>=100`). **prob=100 ⇒ err=0 ⇒ byte-identical to the old path.**
- `mesh_last_hop_metric` is hybrid: learned-metric when converged, else the RSSI-seed tier with prob=100.

## Invariants held
- **Same metric scale as Linux airtime** — learned `thr_kbps` from the identical
  `mmrc_calculate_theoretical_throughput/1000` the RSSI path used, into the identical ETT. Mixed
  learned/RSSI-seed nodes stay comparable; no PREQ/PREP wire-format change.
- **Monotonic, strictly-positive per hop** — for `prob∈[1,100]`, `s_unit−err = s_unit*prob/100 ≥ 2 > 0`
  (no div-by-zero); every hop contributes ≥ 1. `prob==0`/`rate==0` short-circuit to `MAX_METRIC`.
- **Staged + reversible** — Edit 2(+2b) is inert without RC (feedback resolves a stad whose table is NULL,
  `umac_rc_feedback` no-ops); Edit 1 turns on learning; Edit 3 consumes it.

## Verification (hardware, 2026-07-02)
2-hop-capable mesh: board0 + board1 (secured) + chronite; chronite drove a sustained ping at board0 so
board0's TX (replies) trained mmrc for the chronite link. A TEMP `printf` in `mesh_last_hop_metric` logged
when the **learned** branch was taken:
```
RC-METRIC LEARNED sa=<chronite>  thr=3000k prob=93  metric=2934
RC-METRIC LEARNED sa=<chronite>  thr=900k  prob=100 metric=9103
RC-METRIC LEARNED sa=<board1>    thr=300k  prob=100 metric=27307
```
- **RC started + converged** (learned prints appear) — proves Edit 1 + the feedback path (Edit 2), since
  the accessor gates on `evidence>0`.
- **Real fail term** — chronite `prob=93` (not 100) raised the metric **2731 → 2934** vs the old RSSI-seed
  MCS7 (err=0); the metric now tracks the real ~7% link loss (divergence #2 gone).
- **Learned rate** — `thr` is mmrc's best-throughput (3000k/900k as it samples), not the 3-tier RSSI guess
  (divergence #1 gone).
- **Build:** clean across the 4 files. TEMP `printf`/`<stdio.h>` reverted after.

### On-air confirmation (chronium `morse0`, 2026-07-03)
Re-verified on a secured mesh (board0 + board1 + chronite/chronosalt/chronogen) with **chronium sniffing
`morse0`** (freq 5560, S1G ch27). Parsed the ESP HWMP **PREQ metric field** off the air: originated PREQs
carry `metric=0` (correct), forwarded PREQs accumulate the forwarder's learned per-link cost, and — the
decisive check — **non-tier metric values appear on the air (`3035`, `8441`)** that the RSSI-seed
*physically cannot* emit (it only produces 2731/6827/27307 at prob=100). So the learned rate + real prob
reach the **on-air** metric field, not just the local computation — the gold-standard confirmation. The
same run also closed the peer-table growth's deferred check: **board0 held 5 peers** (>4, the old cap).

## Rollout
Interop-safe: scalar ETT on a fixed scale, no wire change — a learned-rate node and an RSSI-seed node
compare on the same axis, no flag day. Directionality note (unchanged from P6c): `mesh_last_hop_metric` is
keyed by the transmitter and reads the local node's RC toward it, so a link is only as good as its
better-instrumented end.

## Code-map
Rows updated in `docs/mesh-ap/rimba-mesh-80211s-code-map.md` (P6c / RC section). Removes P6c worklog
divergences #1/#2 (`2026-07-02-mesh-p6c-airtime-metric.md`). Feasibility memory:
[[mesh-real-rc-feasible-design]].
