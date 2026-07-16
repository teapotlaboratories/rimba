# 2026-07-15 — HWMP path-flapping: verified a TEST ARTIFACT, not a production bug (+ D1/D4 residuals)

**Status: RESOLVED — no production HWMP flapping bug.** A 3-lens adversarial workflow comparing morselib's
HWMP against the pinned Linux `net/mac80211/mesh_hwmp.c` reference settled the long-standing question. No code
changed (pure analysis); two real, separate, non-flapping residuals (D1 small / D4 bigger) recorded as backlog.

## The question
The `mesh-p6c-airtime-and-hwmp-flapping` memory says the HWMP multi-path flapping bug (per-reply `target_sn` +
no PREQ reply-dedup) was **fixed 2026-07-02** (halow `b677d9a3`). Yet the A-MPDU **S2** work (2026-07-11,
worklog `2026-07-11-mesh-ampdu-s2-multihop-nexthop-stad.md` §4) still saw board1's next-hop to board2
alternate (~319 RA=board0 correct-relay / ~221 RA=board2 the final dest), and called it "the known flapping
bug." Real residual, or a test artifact?

## Method
3 independent lenses (port-fidelity vs Linux · artifact-vs-real-bug judge · self-healing-gap analyst) reading
the actual files — morselib `umac_mesh.c` + `umac_datapath.c` and a local copy of Linux `mesh_hwmp.c` /
`mesh_pathtbl.c` / `mesh.h` (scp'd from chronite's `~/halow/rpi-linux`) — then a synthesis that verified the
load-bearing anchors and **self-corrected** one lens.

## Verdict — the 07-11 flapping is a FORCED-TOPOLOGY TEST ARTIFACT (proven)
1. **Anti-flap logic is a faithful Linux port.** `mesh_path_update` (`umac_mesh.c:1984`) == `hwmp_route_info_get`:
   stale-SN reject + adopt only via same-next-hop OR when the metric beats by 10/9 (~11%) hysteresis, applied
   independently of the SN test. Two genuine similar-metric paths **cannot** flap through this compare.
2. **The ESTAB metric gate already exists** (this corrects the "artifact-steelman" lens, which wrongly claimed
   there was none). `mesh_last_hop_metric` (`:2339`) returns `MESH_METRIC_MAX` for a **non-peer** transmitter,
   exactly like Linux `airtime_link_metric_get` non-ESTAB → MAX. So a route learned via a non-peer gets a
   saturated metric and **loses** the compare to any real relay path.
3. **The oscillation is manufactured by the bench allowlist.** `mesh_peer_allowed` (`:517`) is a bench-only
   gate (returns true for everyone when `count==0`, i.e. off in production). It creates "in-RF-range +
   HWMP-heard + not a data peer," which cannot occur in an auto-peering mesh (in-range → MPM-peers → real
   1-hop path; out-of-range → never hears the PREP). The proximate RA-flip is the **origin `RA=dest`
   fallback** (`umac_datapath.c:3761`), not route-learning.

## Two real, separate residuals (backlog — NOT the flapping)
- **D1 (small).** The local-origin header builder `umac_datapath_construct_80211_data_header_mesh`
  (`umac_datapath.c:3761-3766`) transmits `RA=dest` when `umac_mesh_lookup_next_hop` fails, instead of
  buffer+PREQ like Linux `mesh_nexthop_resolve` (mesh_hwmp.c:1239-1287). Correct only when dest is a direct
  peer (`umac_mesh_get_peer_stad(dest)!=NULL`); for a multi-hop-only dest it wastes airtime blasting an
  unreachable RA + loses first packets before discovery. The **forward** path already drops-and-discovers —
  only the origin path diverges. Fix = mirror it (peer-check → `RA=dest`; else drop/queue + discover); the
  void in-place header builder means the fix touches the caller (`:2180`), so a small refactor. Not yet
  bench-confirmed as production-impacting (reasoned + worklog §4 only).
- **D4 (bigger, separate hardening).** No tx-status → path-invalidation in the mesh datapath;
  `umac_mesh_invalidate_paths_via` (`:2260`, the `mesh_plink_broken` body → PERR) fires only on MPM peer-link
  teardown. No analog of Linux `ieee80211s_update_metric` → `fail_avg` EWMA → `LINK_FAIL_THRESH(95)` →
  `mesh_plink_broken` self-heal → a silent black-hole/asymmetric next-hop never self-heals. Feasible: hook at
  `umac_datapath_process_tx_status_queue` (`:2938`) beside `umac_rc_feedback` (`:2986`) — add a per-peer
  `fail_avg` EWMA (port Linux smoothing exactly or it self-thrashes under normal MCS0 retries), invalidate on
  >95. Bonus: makes P6c's structurally-0 airtime `err` term honest. **Would NOT cure the 07-11 flapping** (the
  allowlist-dropped frames hardware-ACK above the drop → tx-status = SUCCESS → no fail-avg fires). Needs a
  convergence bench.

## D1 — IMPLEMENTED + bench-verified (same day; UNCOMMITTED)
Added `umac_datapath_mesh_frame_undeliverable(h8023, is_eapol)` in `umac_datapath.c` (+36 lines), gating BOTH
mesh TX paths in `umac_datapath_process_tx_frame`: a mesh **unicast** frame that is **not EAPOL**, **not
multicast**, has **no resolved HWMP path** (`!umac_mesh_lookup_next_hop`), AND whose dest is **not a direct
peer** (`umac_mesh_get_peer_stad(dest)==NULL`) is now **dropped** (kick discovery → `goto error` → release +
return `MMWLAN_SUCCESS`=consumed) instead of falling through to the builder's `RA=dest`. Direct-peer dests still
get `RA=dest` via the builder's own fallback (correct, single-hop). Follows Linux `mesh_nexthop_resolve`
(we drop rather than buffer — no mesh pending queue).

**Verification:**
- **Regression — single-hop board2→board0 (direct peers):** iperf 0.46 Mbit/s. Peering completed + direct
  traffic flows → D1 does NOT drop EAPOL / direct-peer / has-path frames. ✅
- **Multi-hop origination board1→board0→board2 (board1 originates to a non-peer):** works, ramps to **1.66
  Mbit/s** (0.70 avg). Dropping the artifact `RA=dest` frames lets board1 hold the relay path cleanly — D1
  incidentally mitigates the forced-rig oscillation on the origin side. ✅

**Status:** UNCOMMITTED in the halow submodule working tree (`umac_datapath.c`, +36) — for review + commit
(never auto-commit). No app change. Bench radio-silent afterward.

## Disposition
- No product fix needed for the *flapping* (artifact). D1 (the origin RA=dest weakness the bench surfaced) is
  now fixed. The S2/S3 A-MPDU multi-hop bench should still use a **genuine out-of-RF far node**, not the
  in-range allowlist (which manufactures the HWMP-reachable-but-data-forbidden inconsistency).
- **D4** (tx-status→PERR self-heal) = separate hardening, own convergence bench — stays backlog.
- Recorded: milestones Backlog (HWMP routing-residuals item, D1 marked done) + the Forced-topology test note;
  memory `mesh-p6c-airtime-and-hwmp-flapping`. Workflow run `wf_4d9e1f2c-337`.
