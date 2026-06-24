# P0 multi-node bench — 3× ESP32 IBSS (2026-06-19)

First real multi-node exercise of the open 802.11ah IBSS foundation: 3 ESP32-S3 +
HaLow boards in one cell, validating the per-peer station records (#14) that are
invisible with 2 boards. Companion to the test plan
([`rimba-ibss-test-plan.md`](../ibss/rimba-ibss-test-plan.md) §4) — results recorded
there; this is the blow-by-blow.

## Setup

- 3× XIAO ESP32-S3 + HaLow (MM6108) on `/dev/ttyACM0..2`, `boards/proto1-fgh100m`.
- One binary: `firmware/rimba-halow-ibss` with the new **N-node addressing** —
  `my_ip = 192.168.13.<octet(mac)>` (octet = `mac[5]`, clamp 0→1 / 255→254), and a
  ping to **every** peer from `mmwlan_ibss_get_peers()` (was hardcoded `.1`/`.2`,
  which collides at ≥3 nodes). The IBSS create/join role stays a MAC heuristic.
- Cell params: SSID `rimba-ibss`, BSSID `02:12:34:56:78:9a`, S1G ch27 / 1 MHz /
  op-class 68 / US, OPEN.

MAC → IP that formed:

| MAC | IP | Port (this run) |
|---|---|---|
| `68:24:99:44:6b:b7` | `.183` | ACM0 |
| `bc:2a:33:96:b2:9f` | `.159` | ACM1 |
| `68:24:99:44:6a:56` | `.86`  | ACM2 |

## Results

**P0.1 discovery ☑** — every node formed **exactly 2** peer records, correct MACs,
distinct AIDs (1 & 2), no self/BSSID/bogus entries.

**P0.2 all-pairs unicast ☑** — full triangle reachable; **165 ping replies,
0 timeouts** in the capture window. RTT 16–57 ms (vs ~16 ms on the 2-board bench —
the spread is the added 3-node contention, all well-behaved).

**P0.3 broadcast ☑** — each node received the raw `0x88B5` broadcast (13–18
frames/node).

**P0.5 concurrent multi-peer load ☑** — each node drove **both** peers' unicast
pings + the `0x88B5` broadcast simultaneously; all flows held, **0 errors/asserts**
on any board.

**P0.4 per-peer dedup ◐** — per-peer AIDs are independent and the concurrent 2-peer
flows stayed healthy, but per-peer **seq/dup counter** independence was only
inferred from clean flows, not isolated as its own assertion. Revisit with a forced
duplicate / seq-wrap probe.

**P0.6 drop/rejoin ◐ (core PASS, survivor-side caveat)** — dropped N2 (`.86`) by
re-flashing it (radio off through the flash = clean departure), survivors capturing
throughout:

- **Survivor link unaffected** — ACM0↔ACM1 (`.183`↔`.159`) ran **130 + 73
  continuous replies, 0 timeouts, 0 crashes** across the drop.
- **Rejoin** — N2 rebooted, rejoined, rediscovered **both** peers with stable AIDs,
  restarted pings to both (fresh empty `pinged[]`), and had **bidirectional data
  back ~6.1 s after app start** (first reply 53 ms; a one-off 718 ms first packet =
  ARP resolution, then steady ~30 ms).
- **Caveat (test-harness, not IBSS):** the **survivor side** of re-acquisition was
  under-tested. Survivors logged **0 new pings** when `.86` returned, because the
  app's `pinged[]` dedup still had `.86` from the first run → no fresh session for a
  peer that returns with the **same MAC**. This is the no-age-out gap (test plan
  §8.1) surfacing at the app layer: survivors never free the stale peer record, and
  the ping app conflates "discovered once" with "ping started once." The **protocol
  path is unaffected** — real Rimba DATA is driven off the live peer table every
  loop, not a one-shot ping session. To test survivor re-acquisition properly the
  ping app needs **membership-driven** sessions (start/stop with peer-table
  presence), which is blocked on peer age-out (§8.1, backlog #14 follow-on).

**P0.7 multi-creator convergence ☑** — all three boards (whatever role the MAC
heuristic assigned) shared one BSSID/cell; no split-cell.

## Takeaways

- The N-node addressing change works exactly as intended; the per-peer records
  (#14) hold up at 3 nodes — independent AIDs, no cross-peer false dedup, healthy
  concurrent flows.
- The no-age-out gap (§8.1) now has a concrete observed consequence at the app
  layer (survivors can't cleanly re-acquire a returned same-MAC peer). Reinforces
  that **peer age-out** is the next robustness item before survivor-side reliability
  testing is meaningful.

## Next

- **P0.5-interop** ★ — Linux MM6108 (`morse_driver`) 4th node; the reference-impl
  check (riskiest divergence: our beacon IE set vs `morse_driver`).
- **P1 reliability** — throughput, loss/jitter, MTU sweep, overnight soak (this run
  was minutes, not hours).

## Bench state

All three boards returned to radio-silent (`rimba-hello`) after the run.
