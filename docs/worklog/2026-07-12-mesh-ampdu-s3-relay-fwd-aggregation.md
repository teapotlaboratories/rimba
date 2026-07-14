# Worklog — 2026-07-12 — Mesh A-MPDU S3: relay forward-leg aggregation + FIX-2 (non-blocking forward)

**Author:** Aldwin
**Phase:** 802.11s mesh throughput — A-MPDU aggregation (S3, blocker B6) + the interrupt-WDT FIX-2
**Goal:** route the relay's **forwarded unicast** onto the aggregation-eligible data path so it A-MPDUs
per next-hop link (the dominant multi-hop-goodput lever), and in the same seam make the forward
**non-blocking** so it stops arming the relay interrupt-WDT crash. Follow net/mac80211
`ieee80211_rx_h_mesh_forward` (re-inject into the normal data TX path, TID preserved).
**Status:** **Code done, builds green, adversarially reviewed (3 findings found + fixed), NOT
bench-verified** (bench radio-silent; board2 PPK2-unpowered). Uncommitted, on the S1/S2 submodule branch.

This entry is **standalone**.

---

## 1. What shipped (5 files, morselib)

FIX-2 (crash trigger removal) and S3 (aggregation retag) live at one seam — the forward path — so they
landed together. A compile-time toggle `MESH_FWD_DATA_AGGREGATE` (default 1, `umac_mesh.h`) gates the S3
aggregation for bench A/B; FIX-2's non-blocking behaviour is unconditional (you never want to toggle back
into the crash).

| File | Change |
|---|---|
| `frames/frame_constructor.c` + `frames_common.h` | New `build_mesh_data_frame()` — allocates the forward frame on `MMDRV_PKT_CLASS_DATA_TID0+tid` instead of `MMDRV_PKT_CLASS_MGMT`. **Decisive:** MGMT-class frames route to `mgmt_q` (`pageset.c:202`, `tx_mgmt_handler`) which never A-MPDUs; DATA-TID class routes to the aggregating `tx_data_handler`. |
| `mesh/umac_mesh.h` | Toggle `MESH_FWD_DATA_AGGREGATE` (=1) + `MESH_FWD_TID` (=0). |
| `mesh/umac_mesh.c` | `umac_mesh_forward_data` allocs via `build_mesh_data_frame` (DATA class) when the toggle is on. |
| `datapath/umac_datapath.c` | `umac_datapath_tx_mesh_keyed_frame`: **FIX-2** non-blocking gate at the top; **S3** per-TID seqno + `aggr_check` + `tid=0` + A-MPDU-eligibility for the forwarded unicast; correct channel routing on enqueue. |

### The S3 aggregation retag (unicast forward only)

For a forwarded UNICAST (`key_type == PAIRWISE`; `stad` is already the next-hop peer), the keyed-frame TX
now mirrors the verified local-origin path (`umac_datapath.c:2209-2292`):

- **per-TID seqno** — `sta_data->tx_seq_num_spaces[MESH_FWD_TID=0]` on the next-hop stad (not the baseline
  space). Correct and *required*: TA is always our mesh MAC, so the next hop keeps **one** `(TA=us, TID 0)`
  reorder window; local-origin-to-N and forwarded-via-N on TID 0 must share one monotonic counter or N's
  reorder corrupts. (S2 already keyed everything on the next-hop stad — S3 rides that.)
- **`aggr_check(umacd, stad, MESH_FWD_TID, ssc)`** — opens/refreshes the originator BA session on the next
  hop (idempotent once established, `umac_ba.c:402-405`).
- **`tx_metadata->tid = 0`** and the A-MPDU-eligibility block (`tid_max_reorder_buf_size` +
  `MMDRV_TX_FLAG_AMPDU_ENABLED` via `umac_ba_is_ampdu_permitted(stad, 0)`).
- **channel** — `mmdrv_tx_frame(txbuf, /*is_mgmt=*/!fwd_aggregate)`: aggregating unicast → DATA channel;
  group + the S3-off fallback → MGMT channel.

Everything is consistent at **TID 0**: pkt-class `DATA_TID0`, metadata tid 0, seqno space [0], BA on
(next-hop, 0), and the wire QoS TID (`umac_mesh_build_forward` stamps `0x0100` = Mesh-Control-Present,
TID bits 0). Group re-broadcast (`key_type == GROUP`) is untouched: baseline seqno, no aggregation, MGMT
channel. SW-CCMP-per-MPDU is preserved and composes with A-MPDU (validated in S0/S1).

### FIX-2 (non-blocking forward — the interrupt-WDT trigger)

The forward runs in the **umac-core evtloop task** (see the root-cause worklog's Stage-1 correction — it is
*not* the driver-task `bus_lock`). Its old blocking `wait_for_tx_ready(1000 ms)` stalled the evtloop under
congestion. FIX-2 = flip that wait to non-blocking (`calculate_tx_timeout_ms(umacd, false)` = drop-on-full),
and — per the review below — **hoist the gate to the top of the function** so a paused forward drops
*before* stamping a seqno or opening a BA session. Mirrors net/mac80211's qdisc drop for a forwarded frame.

## 2. Adversarial review — 3 findings, all fixed

A 3-lens review workflow (aggregation correctness · crypto/buffer/non-blocking · follow-Linux) + adversarial
verification surfaced three real issues in the first cut; all were verified against code and fixed:

1. **Blocking ADDBA inline in the forward.** The new `aggr_check` fires ADDBA via the *blocking*
   `umac_datapath_tx_mgmt_frame` (`:2663` wait, `:2677` `mmdrv_tx_frame(...,true)`), and on a paused-TX
   failure the session is left retryable (`umac_ba.c:275-282`) → every subsequent forward re-blocks the
   evtloop. **Fix:** hoisting the non-blocking drop-gate to the top means a paused forward drops *before*
   reaching `aggr_check` — no blocking ADDBA under congestion.
2. **`mmdrv_tx_frame`'s 2nd arg is `is_mgmt` (channel select), not "blocking."** The first cut flipped it
   `true→false` unconditionally, routing the GROUP broadcast to the ack-expecting DATA queue (no
   `NO_ACK` flag set). **Fix:** `mmdrv_tx_frame(txbuf, !fwd_aggregate)` — group + fallback stay on MGMT.
   (Also corrected a comment that mislabelled the boolean as "non-blocking.")
3. **SN burned on congestion-drop.** The seqno was stamped before the drop gate → a dropped forward left a
   next-hop reorder gap. **Fix:** same hoist — the gate now precedes the seqno stamp, so a drop consumes no SN.

**Open items deliberately deferred** (noted, not defects): TID preservation is flattened to 0 (Linux
preserves `skb->priority`; acceptable — the wire was already TID 0); `IMMEDIATE_REPORT` + `AMPDU_ENABLED`
coexist on the forward (bench A/B whether it inhibits aggregation); in-place-forward (design §S3 calls it
independent — still copy-forwards).

## 3. Build

`idf.py build` of `firmware/rimba-halow-mesh-perf` (esp32s3, IDF v5.4.2): **Project build complete** —
compiles + links + packages a valid image, **zero warnings on any changed file**. (The debug image is 1.52 MB;
the default 1 MB single-app partition overflows — a pre-existing packaging config, unrelated to this change;
built green with `SINGLE_APP_LARGE`.)

## 4. Bench validation plan (not yet run — bench radio-silent)

Restore: repower board2 (`tools/ppk2_hold.py`), fw **1.17.8** everywhere, bring up board1 → board0(relay) →
board2 (or a Linux relay). Then:

- **S3 aggregation on the relay egress:** capture the relay's forward leg on chronium `morse0`; expect
  consecutive-seq QoS-Data MPDUs sharing a PPDU TSFT (the A-MPDU signature) — where today's mgmt-class forward
  shows only singletons. 2-hop iperf before/after (`MESH_FWD_DATA_AGGREGATE` 0 vs 1 A/B in the same RF window).
- **FIX-2 crash:** load-sweep the relay; expect no `hw_restart` / INT_WDT where the blocking forward previously
  crash-looped. Instrument via RTC-noinit `hw_restart_count` + forward-wait-timeout counters (the relay has no
  usable console — see the root-cause worklog §7).
- **Correctness:** ping/iperf end-to-end through the relay must stay lossless-ish (drops only under genuine
  congestion, by design); confirm the GROUP re-broadcast (ARP/bcast) still reaches the mesh (MGMT channel intact).

**After every test:** radio-silent — `rimba-hello` to all ESPs + `ip link set wlan1 down` on Linux nodes.

## 5. Provenance / links

- Follow-Linux anchor: `ieee80211_rx_h_mesh_forward` (chronite `~/halow/rpi-linux` @ `372414fd4`,
  `net/mac80211/rx.c` ~:2930-2991: `set_qos_hdr` + re-inject into the data TX path, TID preserved).
- Design: `docs/mesh-ap/rimba-mesh-ampdu-aggregation-design.md` §5 S3 / §(c) / B6.
- The interrupt-WDT root cause + why FIX-2 matters: `2026-07-12-mesh-relay-intwdt-rootcause.md`.
- S1 (BLOCK_ACK RX routing + MFP=no) and S2 (multi-hop next-hop stad) worklogs — the BA machinery S3 rides.

**Nothing committed.** All edits are on the `feat/mesh-ampdu-s1-blockack-rx` submodule working tree
(stacked S1 → S2 → S3), plus the app-level rimba branch `feat/mesh-ampdu-s1`.
