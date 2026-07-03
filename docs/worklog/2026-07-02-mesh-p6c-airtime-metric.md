# 2026-07-02 — P6c: mesh airtime link metric (port of net/mac80211 `airtime_link_metric_get`)

Replaces morselib's fixed per-hop HWMP cost (`MESH_PATH_LINK_METRIC = 100`) with a rate-derived airtime
metric ported line-for-line from `net/mac80211 airtime_link_metric_get`, so HWMP prefers higher-quality
paths instead of merely fewer hops. Implemented + builds + formula-unit-verified; the on-air routing test
is the remaining (bench-heavy) verification.

## What ships
- The **exact** mac80211 fixed-point ETT formula, re-verified byte-for-byte on chronite's pinned
  `rpi-linux` (`mesh_hwmp.c:338-381`; constants `TEST_FRAME_LEN=8192`/`MAX_METRIC=0xffffffff`/
  `ARITH_SHIFT=8` at `:14-16`).
- Its rate input **approximated** from the peer's last RX RSSI (mesh has no per-peer rate control — see
  below), on the *same metric scale* as a live Linux node's airtime, so a mixed ESP/Linux mesh
  accumulates comparably.
- Integrated at the two HWMP accumulation sites (PREQ + PREP) with the mac80211 overflow clamp.

## The formula (verified on both trees)
`airtime_link_metric_get` re-grepped on chronite: `rate = DIV_ROUND_UP(sta_get_expected_throughput,100)`;
`tx_time = device_constant + 10*test_frame_len/rate`; `estimated_retx = (1<<16)/(s_unit-err)`;
`result = (tx_time*estimated_retx) >> 16`; ESTAB-gate → `MAX_METRIC`. Ported verbatim at
`umac_mesh.c:2153` (`mesh_airtime_from_thr_kbps`). **Unit-checked** standalone: thr {300,1200,3000} Kbps
→ metric **{27307, 6827, 2731}** — matches the hand-computed Linux output.

## The rate input — deliberate approximation
mac80211 feeds the mmrc *learned* best-throughput rate (`sta_get_expected_throughput` →
`morse_get_expected_throughput`). morselib **never starts rate control for mesh peer stads** (mmrc's
`reference_table` stays NULL), so no learned rate exists. Instead (`mesh_thr_kbps_from_rssi`,
`umac_mesh.c:2170`) we seed the rate from the peer's RSSI using mmrc's *own* cold-start tiers
(`mmrc_init_rates`, `mmrc.c:1287`: ≥ −70 → MCS7, ≥ −85 → MCS3, else MCS0) and run it through the *same*
`mmrc_calculate_theoretical_throughput` (`mmrc.c:482`) morse's driver op uses, at the mesh operating
point (1 MHz / long GI / single stream) → 3000/1200/300 Kbps → per-hop **2731 / 6827 / 27307**. Every
per-hop cost is > 0, so adding a hop strictly increases the metric (monotonic / loop-free, as HWMP
requires). Per-frame RSSI is sampled onto the sender's peer from the very PREQ/PREP that traversed the
link (`umac_mesh_handle_action`, `umac_mesh.c:2588`), mirroring IBSS `umac_ibss_record_peer_rx`.

### Divergences (each deliberate)
1. **RSSI-seeded rate, not learned airtime** — no mesh RC; coarse 3-tier. Reuses mmrc's own cold-start
   seed so the estimate is derived exactly as morse would at cold-start.
2. **`err`/`fail_avg` structurally 0** — no per-peer PER/retry in morselib; the fail-ratio fallback
   (`mesh_hwmp.c:361-372`) never runs (we always synthesize rate > 0).
3. **Fixed 1 MHz BW** — mesh is single-channel; BW uniformly scales all metrics, cannot change path
   ordering.
4. **RSSI sampled at RX, not a TX-status EWMA** — no per-peer TX-status callback in morselib mesh; the
   3-tier quantization gives natural hysteresis (metric only re-evaluated at discovery).

## Integration (what changed, `umac_mesh.c`)
- `:2232` (PREQ) + `:2289` (PREP): `new_metric = metric + mesh_last_hop_metric(frame_sa)` + overflow clamp
  `if (new_metric < metric) new_metric = MESH_METRIC_MAX` (`mesh_hwmp.c:480` — previously absent; the
  constant +100 couldn't overflow, airtime hops can).
- New per-peer `int16_t last_rssi_dbm` (`:447`), init `MESH_RSSI_NONE` (`:575`), recorded from the HWMP
  frame (`:2588`).
- `mesh_last_hop_metric` (`:2187`): ESTAB gate → RSSI → airtime, keyed by the immediate transmitter.
- `mesh_path_update` fresh-info compare (`:1859`, `sn == p->sn && metric < p->metric`) **unchanged** —
  lower-is-better already fits airtime; it's what the verification exercises.
- Removed `MESH_PATH_LINK_METRIC`; added constants `MESH_METRIC_*` + RSSI tiers (`:369-378`); added
  `#include "mmrc.h"`.

## Verification
- **Formula (unit):** standalone C → {27307, 6827, 2731} == hand-computed Linux `airtime_link_metric_get`. ✓
- **Build:** `make build APP=rimba-halow-mesh BOARD=proto1-fgh100m` — `umac_mesh.c` compiles, links
  (`mmrc_calculate_theoretical_throughput` resolved), esp32s3 image created. ✓
- **Hardware (3-board line, no RF arrangement, 2026-07-02): ✓** flashed board1 — board0(relay) — board2
  (`MESH_LINE_RELAY_DEMO` allowlist; board1 pings board2 forced 2-hop via board0). board1's resolved path
  metric was read off the console via a TEMP `printf` (morselib `MMLOG` isn't on the UART):
  - **Run 1 (real RSSI, both links strong):** `P6C-PATH to board2 via board0 metric=5462` = 2×2731 (both
    MCS7) — the true airtime metric, **not** the old fixed 200. Confirms real RSSI is sampled → tier →
    airtime and accumulated across 2 hops. Ping 0% loss (~45 ms).
  - **Run 2 (board0↔board2 forced weak, −90 dBm via a TEMP override):** `metric=30038` = 27307 (MCS0 weak
    hop) + 2731 (MCS7 strong hop) — the metric **responds to per-link quality** with correct asymmetric
    accumulation. Ping stayed 0% loss.
  Deterministic, no antenna/attenuator work (the override forces RSSI in software). TEMP scaffolding
  (`printf`, `<stdio.h>`, the override, the app's `MESH_IPERF`→line-demo toggle) reverted after.
- **Full-RF routing-decision test (future, optional):** a 4th node giving two competing multi-hop paths so
  airtime picks a *different next-hop* than the fixed metric would. Not required to validate the new code —
  the fresh-info selection it exercises (`mesh_path_update:1859`, lower-metric-wins) is unchanged existing
  logic; P6c only changed the metric *value*, now hardware-verified above.

## Routing-decision test (triangle) — surfaced a pre-existing HWMP bug
Beyond the single-path metric-value tests above, I ran the "gold-standard" decision test: a 3-board
triangle (all peered), override forcing the **direct** board1↔board2 link weak (−90 → 27307) while the
board1→board0→board2 relay stays strong (2731+2731 = 5462). Airtime *should* route board1→board2 via
board0 despite board2 being a direct peer.

**Result:** the relay path **is** discovered + installed (via board0, 5462), but the installed path
**flaps** direct(27307)↔relay(5462) across discovery rounds (4 direct : 2 relay installs in a 40 s
window). Root cause is **HWMP-layer, not the metric**: `umac_mesh.c:2264` replies to a PREQ with
`target_sn = ++mesh_hwmp_sn` (per-reply SN increment) and there is **no received-PREQ reply-dedup** — so
in a multi-path topology board2 replies to *both* the direct PREQ and the relayed PREQ, each with a
different, incrementing SN. board1's fresh-info rule (`mesh_path_update:1872`) then selects by **newer SN,
short-circuiting the `metric <` compare** → flapping, and selection is SN-arrival-order-dependent rather
than metric-dependent.

mac80211 avoids this: a target replies **once** per discovery (a duplicate PREQ — same orig_sn — is stale
and not re-replied via `hwmp_route_info_get`). morselib lacks that dedup. **This is separate from P6c** —
the airtime metric itself is correct (values verified above); it simply can't cleanly *decide* a
multi-path route until HWMP reply-dedup lands. Filed as the backlog item **"HWMP multi-path
reply-dedup / per-reply SN"**. This finding is exactly what the decision test existed to catch — the
single-path tests could not have surfaced it.

**Update (same day): the HWMP bug was FIXED** (worklog `2026-07-02-mesh-hwmp-multipath-dedup-sn.md`,
following mac80211's time-gated target SN + freshness gate). Re-running this exact triangle, board1→board2
now **settles on the relay (via board0, 5462)** each discovery and holds stable ~20 s with no flapping — so
the airtime routing *decision* is now hardware-verified end-to-end, not just the metric values.

## Interop / rollout
All morselib mesh nodes must be flashed **together** — a not-yet-upgraded node still emits +100/hop
(incomparable scale). Linux nodes already emit compatible stock airtime. Note: morselib's preemptive
path refresh is *time-based* (`MESH_PATH_REFRESH_MS`), not the metric-threshold `hwmp_is_mpath_optimal`
`253` path, so airtime doesn't collide with it today; if that metric-based check is ever ported, `253`
(min single-hop ~280 for airtime) must be re-derived.

## Code-map
Rows in `docs/mesh-ap/rimba-mesh-80211s-code-map.md` (P6c section); Linux refs re-grepped on chronite's
pinned `rpi-linux` / `morse_driver`.
