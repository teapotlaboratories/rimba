# RISK-01 IBSS — open-link test plan

Validate that the **open 802.11ah IBSS foundation** (Phase 1, RISK-01) is robust
before Phase 2 (link security) is built on top of it. Companion to
[`rimba-ibss-milestones.md`](rimba-ibss-milestones.md) and the hardening backlog
[`rimba-ibss-hardening-todo.md`](rimba-ibss-hardening-todo.md).

Status legend: ☐ todo · ◐ in progress · ☑ passed · ✗ failed.

---

## 1. Coverage so far

Proven on a 2-board bench, close range, short (~20–30 s) captures:

- ☑ IBSS bring-up (creator + joiner roles)
- ☑ Host beacon + probe-request answering
- ☑ Bidirectional IP data path with dynamic ARP (~16–17 ms RTT)
- ☑ Raw Rimba `0x88B5` frame exchange (bidirectional)
- ☑ Per-peer station records form (one per peer, BSSID excluded)

Extended 2026-06-19 to **3 ESP32 nodes** (N-node MAC-octet addressing), close range:

- ☑ 3-node discovery — each node forms exactly 2 peer records, distinct AIDs (P0.1)
- ☑ All-pairs unicast — full triangle, 0 timeouts in window (P0.2)
- ☑ `0x88B5` broadcast reaches all nodes (P0.3)
- ☑ Concurrent multi-peer load — each node drives both peers at once (P0.5)
- ◐ Drop/rejoin — survivor link unaffected, dropped node rediscovered (P0.6; survivor
  re-acquisition under-tested — see §4)

This proves it **works at 3 nodes**. It does not yet prove it works **reliably under
sustained load, over long durations (soak), or against the Linux reference
implementation** — which is the remaining point of this plan (P0.5-interop, P1, P2).

---

## 2. Topology under test

| Node | Hardware | Stack | Port / addr |
|---|---|---|---|
| N0–N2 | XIAO ESP32-S3 + HaLow (MM6108) | esp-halow / morselib (our port) | `/dev/ttyACM0..2` |
| N3 | Same MM6108 module on Linux | `morse_driver` + mac80211 (reference) | host net iface |

**Common cell parameters — identical on every node** (no TSF merge yet, so the
BSSID must be pinned, see gaps §7):

| Param | Value |
|---|---|
| SSID | `rimba-ibss` |
| BSSID (fixed) | `02:12:34:56:78:9a` |
| S1G channel | 27 (op-class 68, 1 MHz BW) |
| Country / regdomain | US |
| Firmware | MM6108 `1.17.6` (match generations across nodes) |
| Security | OPEN (plaintext — Phase 1) |

**IP convention (all nodes, including Linux):** `192.168.13.<octet(mac)>`, where
`octet = mac[5]` (clamp 0→1, 255→254). Derived identically on every node so the
ping mesh is symmetric and the Linux node is "just another peer."

---

## 3. Prerequisite — N-node addressing (app change)

The current `rimba-halow-ibss` app hardcodes IP by role (creator→`.1`,
joiner→`.2`), which **collides at ≥3 nodes**. Generalise before multi-node tests:

- `my_ip = 192.168.13.<octet(my_mac)>`.
- Ping **every discovered peer**: for each entry in `mmwlan_ibss_get_peers()`,
  `peer_ip = 192.168.13.<octet(peer_mac)>`, ping it; log per-peer result.

This ties the L3 test to the L2 peer table — a ping only happens because the peer
was discovered — and makes the app topology-agnostic (2/3/N + Linux).

---

## 4. P0 — Multi-node ESP32 (3 boards)

The first real exercise of the per-peer records (#14); invisible with 2 boards.

| # | St | Test | Pass criteria |
|---|---|---|---|
| P0.1 | ☑ | 3-node discovery | each node forms **exactly 2** peer records — correct MACs, distinct AIDs, no self/BSSID/bogus entries (`mmwlan_ibss_get_peers` dump) |
| P0.2 | ☑ | All-pairs unicast | N0↔N1, N0↔N2, N1↔N2 all ping with low loss |
| P0.3 | ☑ | Broadcast reaches all | one node's broadcast `0x88B5` is received by **both** others |
| P0.4 | ◐ | Per-peer dedup correctness | with 2 peers each, **no cross-peer false dedup** (the bug #14 prevents) — sequence/dup counters per peer are independent |
| P0.5 | ☑ | Concurrent multi-peer load | N0 drives N1 and N2 simultaneously; both flows hold |
| P0.6 | ◐ | Partial-failure resilience | power-cycle N2 → N0↔N1 unaffected; N2 rejoins → rediscovered as a fresh record |
| P0.7 | ☑ | Multi-creator convergence | however the MAC role-heuristic assigns roles across 3 boards, all share one BSSID/cell |

**Run 2026-06-19** — 3× XIAO ESP32-S3 on ACM0/1/2, one binary (N-node MAC-octet
addressing), `boards/proto1-fgh100m`. MAC→IP: `…6b:b7`→.183, `bc…b2:9f`→.159,
`…6a:56`→.86.

- **P0.1 ☑** — every node formed exactly 2 records, correct MACs, distinct AIDs
  (1 & 2), no self/BSSID/bogus entries.
- **P0.2 ☑** — full triangle reachable; **165 replies, 0 timeouts** in the window
  (RTT 16–57 ms, the spread is the added contention vs the 2-board ~16 ms).
- **P0.3 ☑** — `0x88B5` broadcast received by all (13–18 frames/node).
- **P0.5 ☑** — each node drove **both** peers' unicast pings + the `0x88B5`
  broadcast concurrently; all flows held, 0 errors/asserts on any board.
- **P0.4 ◐** — per-peer AIDs are independent and concurrent 2-peer flows stayed
  healthy, but the per-peer **seq/dup counter** independence wasn't isolated as its
  own assertion (only inferred from clean concurrent flows). Revisit with a forced
  duplicate/seq-wrap probe.
- **P0.6 ◐** — drop N2 (`.86`) by re-flash (radio off), survivors capturing:
  **N0↔N1 link unaffected** (130 + 73 continuous replies, 0 timeouts, 0 crashes);
  **N2 rejoined**, rediscovered **both** peers with stable AIDs, restarted pings to
  both, bidirectional data back **~6.1 s after app start**. *Caveat:* the
  **survivor side** of re-acquisition was under-tested — survivors logged 0 new
  pings for the returned peer because the app's `pinged[]` dedup × the no-age-out
  gap (§8.1) suppress re-pinging a peer that returns with the **same MAC**. Protocol
  layer is unaffected (real DATA is driven off the live peer table each loop, not a
  one-shot ping session); the **ping test app** needs membership-driven sessions to
  test survivor re-acquisition properly. Blocked on age-out (§8.1).
- **P0.7 ☑** — all three boards (whatever role the MAC heuristic assigned) shared
  one BSSID/cell; no split-cell.

---

## 5. P0.5 — Linux HaLow interop (4th node) ★

The reference-implementation correctness check: our port talks to real Linux
`morse_driver`/mac80211 IBSS on the **same silicon**. Also closes backlog #11
(verify the S1G beacon on-wire), since the Linux node can capture/decode frames.

> **Bring-up runbook:** [`rimba-ibss-linux-interop-runbook.md`](rimba-ibss-linux-interop-runbook.md)
> — concrete commands for I.1–I.5, the pinned cell params, and the open question of
> the S1G IBSS *join* syntax (no vendor-documented IBSS recipe exists). Resolve the
> working join command there and back-fill the `<S1G_FREQ_FOR_CH27>` placeholder below.

| # | Test | Pass criteria |
|---|---|---|
| I.1 | Mutual discovery | Linux `iw dev wlan0 scan` sees us / our probe-answer responds; we form a peer record for the Linux MAC; `iw dev wlan0 station dump` lists our nodes |
| I.2 | Beacon interop (both ways) | Linux ingests our hand-built S1G beacons without error; we ingest Linux beacons without garbage/crash — **riskiest divergence point** (our IE set vs `morse_driver`) |
| I.3 | Data path | ESP32 ↔ Linux ping both directions + throughput; validates addressing (A1=DA/A2=SA/A3=BSSID), plaintext data frames |
| I.4 | On-air frame diff | Linux in **monitor mode** captures our beacon/probe/data; decode and diff against Linux-emitted frames (closes #11) |
| I.5 | Mixed 4-node cell | 3 ESP32 + 1 Linux: all-pairs reachability + broadcast reaches everyone across both implementations |

### Linux-side bring-up (reference template — adapt to the Luckfox worklog)

```sh
# morse_driver loaded, firmware (mm6108 1.17.6) + BCF in place, iface = wlan0.
sudo iw reg set US
sudo ip link set wlan0 down
sudo iw dev wlan0 set type ibss
sudo ip link set wlan0 up

# Join OUR cell with the pinned BSSID, on the S1G channel matching ch27/1MHz/op-class 68.
# The exact freq/channel arg follows morse_driver's S1G convention (see Luckfox worklog);
# fixed-freq + BSSID is required because neither side does TSF merge yet.
sudo iw dev wlan0 ibss join rimba-ibss <S1G_FREQ_FOR_CH27> fixed-freq 02:12:34:56:78:9a

# Static IP per the MAC-octet convention (matches the ESP32 derivation).
MAC5=$(cut -d: -f6 /sys/class/net/wlan0/address)
sudo ip addr add 192.168.13.$((16#$MAC5))/24 dev wlan0

# Verify:
iw dev wlan0 station dump      # should list the ESP32 peers
ping 192.168.13.<esp32 octet>  # bidirectional data
```

**Compatibility risk points to watch:** S1G beacon IE set/framing (hand-built vs
`morse_driver`) · BSSID pinning (no merge) · exact channel/op-class/regdomain ·
firmware-version parity.

---

## 6. P1 — Reliability (run in the multi-node topology)

We have latency numbers but **no throughput, loss-rate, jitter, or MTU data**, and
no long-duration evidence.

| # | Test | Measure / pass criteria |
|---|---|---|
| P1.1 | Throughput | sustained TX-flood (sender blasts, receiver counts) → packets/s + Mbps per direction |
| P1.2 | Loss + jitter under load | high-rate traffic → loss % and RTT distribution (p50/p99) |
| P1.3 | MTU sweep | ping payload 64→1500 B; find the fragmentation/MTU cliff |
| P1.4 | Recovery | power-cycle a node mid-stream → link + data resume without manual intervention |
| P1.5 | Soak (overnight) | hours of continuous traffic → no asserts/crashes, stable RTT, no heap growth (watch the no-age-out gap, §7) |

---

## 7. P2 — RF / edge

| # | Test | Notes |
|---|---|---|
| P2.1 | Range / RSSI-vs-distance | move nodes apart; behaviour at link margin |
| P2.2 | Role / BSSID boundary cases | both-creator, both-joiner, two creators same BSSID |
| P2.3 | Channel / width variants | beyond ch27 / 1 MHz — try 2/4 MHz widths, other channels |

---

## 8. Known gaps these tests will probe

1. **No peer age-out / free** (#14) — per-peer records are heap-allocated and never
   freed; a departed peer lingers (table caps at 8, then overflows to the shared
   record). The drop/rejoin (P0.6) and soak (P1.5) tests stress this.
2. **No teardown / disable path** (backlog #6) — bring-up assumes a clean boot;
   can't cleanly cycle IBSS down/up without a reboot.
3. **Role = MAC heuristic** — untested when multiple nodes pick the same role
   (P2.2); real create/join + merge is backlog #16.

---

## 9. Suggested execution order

1. Make the **N-node addressing** app change (§3) + a multi-node/reliability build.
2. Bench the **3 ESP32 nodes** (ACM0/1/2) → run **P0**.
3. Bring up the **Linux node**, pin the matching config (§5) → run **P0.5**
   (discovery → beacon → ping → on-air diff → mixed cell).
4. **P1 reliability** across the mixed topology; **soak overnight**.
5. **P2 RF/edge** as time / physical access allows.

Record results inline (flip ☐ → ☑/✗ with a one-line note) as each test runs.
