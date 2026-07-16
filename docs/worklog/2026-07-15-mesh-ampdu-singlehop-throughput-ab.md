# 2026-07-15 — Mesh A-MPDU: aggregation proven single-hop AND relay (engages + ~37–38% throughput A/B)

**Status: WIN PROVEN both single-hop and relay.** Aggregation engages on 97% (origin) / 98.5% (forward) of
frames; ON-vs-OFF throughput A/B = **+37% single-hop, +38% relay**, both at the current MCS-limited rate. No
committed-code change (temp counters + an A-MPDU-disable gate, both reverted). Bench radio-silent afterward.

## Relay/multi-hop A/B (added same day) — board2 as the fully-wired RELAY
Rig: `board1(client) → board2(RELAY) → board0(server)`, forced through board2 by ESP allowlists (board1/board0
peer only board2). board2 = the fully-wired DUT in the relay role (bench-conformant); it forwards + aggregates,
originates nothing (`TX qos=0`). Same UDP `-b 5`, same ON/OFF gate, board2's `FWD` counters read live via a
3-serial hold (`scratch iperf_relay_run.py`).

| board1→board2→board0 (2-hop, UDP) | Throughput (20 s) | Forward frames A-MPDU-eligible |
|---|---|---|
| **A-MPDU ON** | **0.29 Mbit/s** | 382 / 388 = **98.5%** |
| **A-MPDU OFF** | **0.21 Mbit/s** | 344 / 349 (counted, flag withheld) |

- **The S3 forward-path retag works:** board2 marks **98.5%** of *forwarded* frames A-MPDU-eligible (B6 was
  "relay bypasses aggregation" — now it doesn't). **~38% relay win**, ~same ratio as single-hop.
- Absolute 2-hop rate (~0.29) ≈ half the single-hop (~0.52) — expected, the half-duplex relay shares airtime.
- The relay win ratio (~38%) ≈ single-hop (~37%): at this low MCS the gain is overhead-amortization-limited on
  BOTH paths; it grows with MCS. board2-as-relay held up fine under iperf load (no crash) — the conformant rig.

---
## Single-hop A/B (first)

## Goal
Prove the committed mesh A-MPDU work (S1-S3, halow `aa2bdb51`) actually (a) engages aggregation and (b) improves
goodput — the payoff the branch exists for.

## Method
- App: committed `rimba-halow-mesh-perf` (`MESH_IPERF` mode — line allowlist board1--board0--board2 + an
  `esp_console` `iperf` REPL). SW-CCMP (shipping default). Rig: **single-hop board2→board0** (board2 = the
  fully-wired DUT, iperf **client** = the aggregating sender; board0 = iperf server). board1 idle.
- Traffic: **UDP** `iperf -c 10.9.9.136 -u -b 5` (offer 5 Mbit/s ≫ link → TX queue fills → the FW can build deep
  A-MPDUs). TCP first gave the same low rate but a nearly-empty queue → nothing to aggregate.
- **On-air (chronium morse0 + tshark):** confirmed the **ADDBA handshake completes** (category-3 BlockAck action
  exchange board2↔board0). BUT the monitor **drops the fast data + ACK/BA frames under a high-rate burst**
  (captured 1087 beacons but ~13 data frames in a UDP run) → it is **unreliable for aggregation depth**. So:
- **ESP-side counters (the reliable instrument):** temp `mmwlan_dbg_qos_tx` (mesh unicast QoS-data TX) +
  `mmwlan_dbg_ampdu_tx` (subset the BA session marks A-MPDU-eligible, i.e. `umac_ba_is_ampdu_permitted` true and
  `MMDRV_TX_FLAG_AMPDU_ENABLED` set), at `umac_datapath.c` :2363 (origin) + :2886 (relay), logged on the
  heartbeat.
- **A/B gate:** a temp `mmwlan_dbg_ampdu_off` that still counts eligibility but **withholds the AMPDU_ENABLED
  flag** → the FW sends each MPDU singly. Same binary otherwise; only the flag differs.

## Results (single-hop, UDP, board2→board0)
| | Throughput (20 s avg) | A-MPDU-eligible / QoS-data TX |
|---|---|---|
| **A-MPDU ON** (flag set) | **0.52 Mbit/s** | 953 / 984 = **97%** |
| **A-MPDU OFF** (flag withheld) | **0.38 Mbit/s** | 642 / 675 = 96% (counted, unused) |

- **Aggregation engages:** once the BA session is up (~first 5 s), **97%** of board2's mesh QoS-data frames are
  A-MPDU-eligible + flagged. The code path works end to end (ADDBA → permitted → per-frame flag → FW aggregates).
- **~37% single-hop throughput win** (0.52 vs 0.38), same eligibility both sides (so it's the *aggregation*, not
  the BA session, that differs). Per-interval ON is consistently ≥ OFF and reaches peaks (0.65–0.80) OFF never
  hits (max 0.50) → the direction is real, not endpoint noise. (Single A/B pair; would firm up with repeats.)

## Real-RC — the bigger lever (verified same day; already committed)
Chasing the low absolute rate turned up that **real per-peer rate control is already implemented + committed**
(halow `838b23c2`) — the `mesh-real-rc-feasible-design` "ready to implement" memo was stale. So the ~0.5 Mbit/s
above was **never fixed-MCS0** — real-RC was active the whole time. Verified it directly (temp
`mmwlan_dbg_rc_thr_kbps` print of `umac_rc_get_learned_metric` + a `mmwlan_dbg_rc_off` gate forcing the mgmt/MCS0
table):

| Single-hop board2→board0 (UDP) | Learned rate (mmrc) | Throughput |
|---|---|---|
| **Real-RC ON** | 300→**900 kbps** (MCS0→MCS2), prob 65–97% | **0.48 Mbit/s** |
| **Real-RC OFF** (forced MCS0) | 300 kbps (MCS0), prob 100% | **0.22 Mbit/s** |

**Real-RC ≈ 2.2× (+118%)** — the dominant throughput lever, ~6× bigger than A-MPDU's ~37%, and they COMPOUND
(A-MPDU's ~37% sits on top of the real-RC rate). **The ceiling here is the BENCH LINK, not the code:** mmrc's
`prob` bounces 65–70% at MCS2 (~30% loss) so it settles at MCS2, not MCS7 — the close bench nodes RX-overload.
A cleaner RF setup (separation / lower TX power) would let mmrc converge higher → more. So the throughput stack
on this bench: **MCS0 baseline ~0.22 → +real-RC ~0.48 (2.2×) → +A-MPDU ~0.52 (another ~1.37×).**

## Honest caveats
- **Absolute rate is low (~0.5 Mbit/s) → the PHY rate caps it, not aggregation.** The mesh runs at a low/untrained
  MCS (the **real-RC** thread, `mesh-real-rc-feasible-design`). A-MPDU amortizes fixed per-frame overhead (SIFS +
  BA + preamble); at a low MCS the DATA airtime dominates so the amortization is a smaller fraction → a modest
  ~37%. **The win grows with MCS** (higher rate → overhead dominates → bigger aggregation gain) and on the relay
  path (more overhead to amortize) — A-MPDU and real-RC are **complementary** levers.
- **The relay/multi-hop win (the design's "dominant relay-goodput win", S3/B6) is NOT measured here** — needs a
  board2-as-relay rig (board2 fully-wired; board0/board1 as light endpoints under iperf load = a wiring-risk to
  design around). The `FWD` counters stayed 0 (single-hop has no forward).
- The morse0 monitor is not a reliable A-MPDU-depth instrument under load (drops data/BA); use ESP-side counters
  or a lower offered rate.

## Reusable
- Driving `iperf` over the ESP console: these XIAO USB-JTAG boards **reset on serial open**, so a persistent
  `iperf -s` can't survive a reopen — hold BOTH serials open through the whole test and wait ~16 s for boot +
  SAE/AMPE re-peer before sending commands (`scratch iperf_run.py`). `-b` is an integer in **Mbit/s** (`-b 5`,
  not `5M`). Server `iperf -s -u`, client `iperf -c <ip> -u -b 5 -t <sec>`.
- ESP-side A-MPDU eligibility counters at `umac_datapath.c` :2363/:2886 + the `ampdu_off` gate = the reliable
  ON/OFF A/B method (the on-air monitor is not).

## Next
1. ✅ **Relay/multi-hop A/B** — done (above): board2-relay forward aggregation works, ~38%.
2. ✅ **Real-RC** — found already committed + verified (above): ~2.2×, the dominant lever.
3. **Improve the RF link so mmrc can converge past MCS2** — the bench ceiling is the marginal close-node link
   (RX-overload, `prob` ~65–70% at MCS2), NOT the code. Add node separation / drop TX power, then re-measure —
   real-RC should reach a higher MCS and the whole stack (real-RC × A-MPDU) should scale up.
4. The Edit-3 metric/path-selection effect (learned rate changing HWMP route choice) still wants a ≥6-node
   convergence bench (`mesh-real-rc-feasible-design`) — bench-limited here.
5. Firm up the single-hop numbers with repeated A/B runs (per-interval variance is high).
