# RISK-01 IBSS — open-link test plan

Validate that the **open 802.11ah IBSS foundation** (Phase 1, RISK-01) is robust
before Phase 2 (link security) is built on top of it. Companion to
[`rimba-ibss-milestones.md`](rimba-ibss-milestones.md) (milestones, the hardening
backlog/TODO, and the findings).

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
- ☑ Per-peer dedup independence — forced cross-peer probe (P0.4, 2026-06-20)
- ☑ Drop/rejoin — survivors unaffected, dropped node aged out + rediscovered (P0.6,
  unblocked by the adopted age-out)

### Foundation validation — COMPLETE (2026-06-21)
The open IBSS foundation is now validated end-to-end:
- **Multi-node (P0.1–0.7) ☑** — 3-board full mesh, per-peer state, drop/rejoin.
- **Linux interop (I.1–I.3, I.5) ☑** — talks to a real `morse_driver`/mac80211 node on
  the same silicon; mixed 4-node all-pairs after the #17 fix.
- **Reliability (P1.4 recovery, P1.5 soak) ☑** — mid-stream node drop/recovery, and a
  **~6.5 h 4-node soak: 0 reboots, 0 asserts, no heap leak**, RTT stable to the end.
- **One caveat — I.4 ✗ (not done):** on-wire frame diff needs the morse driver rebuilt with
  `CONFIG_MORSE_MONITOR` (a build flag, *not* external gear; rebuild attempted + reverted);
  the ESP32's own beacon `source_addr` stays unverified on-wire (#11). Not a functional
  blocker.
- Not collected: P1.1–P1.3 throughput/jitter/MTU *numbers* (stability + recovery +
  soak — the load-bearing reliability tests — pass).

**Verdict: Phase-1 open-IBSS foundation is robust and ready for Phase-2 (link
security) to build on it.**

---

## 2. Topology under test

| Node | Hardware | Stack | Port / addr |
|---|---|---|---|
| N0–N2 | XIAO ESP32-S3 + HaLow (MM6108) | esp-halow / morselib (our port) | `/dev/ttyACM0..2` |
| N3 | Same MM6108 module on Linux | `morse_driver` + mac80211 (reference) | host net iface |

**Common cell parameters — identical on every node** (no TSF merge — provisioned
mesh, agreed BSSID, by design; #4 out of scope — so the
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
| P0.4 | ☑ | Per-peer dedup correctness | with 2 peers each, **no cross-peer false dedup** (the bug #14 prevents) — sequence/dup counters per peer are independent |
| P0.5 | ☑ | Concurrent multi-peer load | N0 drives N1 and N2 simultaneously; both flows hold |
| P0.6 | ☑ | Partial-failure resilience | power-cycle N2 → N0↔N1 unaffected; N2 rejoins → rediscovered as a fresh record |
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
- **P0.4 ☑ — isolated 2026-06-20 with a forced cross-peer probe.** Instrumented the
  dedup site (`umac_datapath_is_rx_frame_duplicate`) to count, across a 256-frame
  ring spanning all peers, accepted frames whose `(tid, seq)` matched a recent frame
  from a *different* peer. Drove ACM0 with a chronium unicast flood (8464 frames,
  0.06 % loss) + concurrent ACM1/ACM2 pings (the other peers). Result over 9400
  frames: **`xpeer_collisions_ACCEPTED = 208`** (208 same-`(tid,seq)`-different-peer
  collisions correctly *accepted*, not deduped — proves the seq space is per-peer; a
  shared space would have false-dropped them) and **`dedup_drops = 9`** (the
  mechanism still drops genuine same-peer retransmits). Dedup state lives in the
  per-peer `sta_data->rx_seq_num_spaces`, so independence is structural; the probe
  confirms it on-air. Probe reverted after the run.
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
- **P0.6 ☑ — re-run 2026-06-20, caveat resolved by the age-out adoption.** With the
  momentary-systems `umac_ibss` age-out merged, the survivor side now works. Dropped
  N2 (`.86` = `68:24:99:44:6a:56`) by re-flash (radio off); survivors (instrumented
  `peer_cb` logging, since the layer's own add/age-out logs are `MMLOG_INF`, compiled
  out at the default ERR level) showed: **(1) link unaffected** — N0↔N1 replies
  continued throughout; **(2) age-out** — *both* survivors logged `peer_cb REMOVED
  68:24:99:44:6a:56` ~30 s after N2 went silent (record freed); **(3) rediscovery as
  a fresh record** — on N2's return *both* logged `peer_cb ADDED 68:24:99:44:6a:56`
  (only possible because age-out freed the old record); **(4) bidirectional data to
  the returned node restored** — N2 receives replies from *both* survivors (so
  survivor→N2 and N2→survivor both flow). Remaining app-only caveat (#7): the test
  app pings each peer once and never clears `pinged[]` on `REMOVED`, so the survivor's
  *own* ping session to the returned peer isn't re-displayed — the link is proven via
  the reverse direction. Fix: membership-driven ping sessions in the app.
- **P0.7 ☑** — all three boards (whatever role the MAC heuristic assigned) shared
  one BSSID/cell; no split-cell.

---

## 5. P0.5 — Linux HaLow interop (4th node) ★

The reference-implementation correctness check: our port talks to real Linux
`morse_driver`/mac80211 IBSS on the **same silicon**. Also closes backlog #11
(verify the S1G beacon on-wire), since the Linux node can capture/decode frames.

> **Bring-up runbook:** [`rimba-linux-node-setup.md`](rimba-linux-node-setup.md) §12 (IBSS interop)
> — concrete commands for I.1–I.5, the pinned cell params, and the open question of
> the S1G IBSS *join* syntax (no vendor-documented IBSS recipe exists). Resolve the
> working join command there and back-fill the `<S1G_FREQ_FOR_CH27>` placeholder below.

| # | St | Test | Pass criteria |
|---|---|---|---|
| I.1 | ☑ | Mutual discovery | Linux `iw dev wlan0 scan` sees us / our probe-answer responds; we form a peer record for the Linux MAC; `iw dev wlan0 station dump` lists our nodes |
| I.2 | ☑ | Beacon interop (both ways) | Linux ingests our hand-built S1G beacons without error; we ingest Linux beacons without garbage/crash — **riskiest divergence point** (our IE set vs `morse_driver`) |
| I.3 | ☑ | Data path | ESP32 ↔ Linux ping both directions + throughput; validates addressing (A1=DA/A2=SA/A3=BSSID), plaintext data frames |
| I.4 | ✗ | On-air frame diff | **Not done 2026-06-21 — monitor mode is *compiled out* of our morse driver (`CONFIG_MORSE_MONITOR`), not a hardware limit.** Needs the driver rebuilt with that flag (capture on `morse0` per APPNOTE-36), or the ESP32 raw-frame hook. Rebuild attempted + reverted (see note). |
| I.5 | ☑ | Mixed 4-node cell | 3 ESP32 + 1 Linux: all-pairs reachability + broadcast reaches everyone across both implementations |

**Run 2026-06-20 (I.5)** — chronium (`wlan1`, MM6108, `.66`) joined the pinned cell
(`iw … ibss join rimba-ibss 5560 fixed-freq 02:12:34:56:78:9a`; S1G ch27 maps to the
5 GHz-model freq **5560** per `dot11ah` `CHANS1GHZ(27,…,112)`, on-air 915.5 MHz) + 3 ESP32.
- **First run: FAILED** — phantom-peer flood (#17): chronium's beacons minted hundreds of
  `:00:d5` timestamp-phantoms, evicting real peers → ACM0 starved to 0 replies.
- **After the #17 fix: PASS** — full all-pairs reachability, **0 phantoms**; each ESP32
  reaches the other two + chronium; chronium 4/4 to each ESP32, `station dump` = 3 peers.

**Run 2026-06-20** — 1 ESP32 (`rimba-halow-ibss`, ACM0, MAC `…6b:b7`→.183) + Linux
node `chronium` (RPi 5 + MM6108, `morse_driver`/mac80211, all components 1.17.8 —
see [`rimba-linux-node-setup.md`](rimba-linux-node-setup.md)). Linux joined via
`iw dev wlan1 ibss join rimba-ibss 5560 fixed-freq 02:12:34:56:78:9a` (the **5 GHz
ch112 = S1G ch27 / 915.5 MHz / 1 MHz** mapping; `morse_cli channel` confirmed 1 MHz),
IP `192.168.13.66`.

- **Preceded by AP-STA interop ☑** (`hostapd_s1g` AP ↔ `rimba-halow-sta`): WPA3-SAE
  associated, bidirectional ping ~12 ms — proved the radios interoperate on-air and
  de-risked framing before IBSS. (See the Linux-node worklog.)
- **I.3 data path ☑** — **Linux → ESP32 ping 3/3, 0% loss** (`192.168.13.183`); the
  ESP32 receives Linux data frames *and replies*, so plaintext IBSS data interoperates
  bidirectionally between the two implementations.
- **I.1 discovery ◐** — Linux **correctly discovers the ESP32** (`iw station dump` →
  `68:24:99:44:6b:b7`, −31 dBm). The **ESP32 does NOT correctly discover Linux**: see I.2.
- **I.2 beacon interop ◐ — the predicted divergence, confirmed.** Linux ingests the
  ESP32's beacons cleanly (created the IBSS, sees the ESP32, no dmesg errors). But the
  **ESP32 misparses Linux's S1G beacons**: its peer-discovery extracts the transmitter
  MAC from the wrong offset — landing in the beacon **TSF timestamp** — so every Linux
  beacon mints a fresh phantom peer (`48:63:3d:08:00:d5`, `48:f3:3e:…`, 3rd byte
  climbing with the timestamp). The 8-slot table floods with garbage, Linux's real MAC
  (`3c:22:7f`) is never recorded, and the ESP32's own pings go to non-existent IPs (all
  time out). **Data path is unaffected** (I.3 works); the bug is isolated to the ESP32
  IBSS RX **peer-extraction vs the reference S1G (compressed/EXT) MAC-header layout**.
  Tracked as hardening backlog **#16**. Fix is ESP32-side, then re-run I.1/I.2.
- **I.5** ☑ done (mixed 4-node, after the #17 fix — see §5).
- **I.4 ✗ BLOCKED (2026-06-21) — morse monitor mode captures nothing on-wire.** Put
  chronium's `wlan1` in monitor mode on the right S1G channel (`morse_cli` confirmed
  **915500 kHz** = ch27, 1 MHz) with ACM0 beaconing as a lone IBSS creator, and captured
  via a raw `AF_PACKET` socket. **Zero frames** — `rx_packets` delta was **0** over 6 s,
  unchanged by promiscuous mode, with no `morse_cli` monitor/sniff command and no relevant
  `dmesg`. (Not IBSS-specific — an ESP32 SoftAP beacon also captured 0 frames.)
  **ROOT CAUSE FOUND 2026-06-21 (forum + driver source) — NOT a hardware limit:** the
  chronium `morse_driver` was compiled **without `CONFIG_MORSE_MONITOR`**. The `morse0`
  monitor netdev (`monitor.c: morse_mon_init`) and the RX->monitor path are `#ifdef
  CONFIG_MORSE_MONITOR`; our build lacks the flag (no `morse0`, 0 `morse_mon` symbols). Per
  the forum + APPNOTE-36 you capture on the **`morse0`** interface (not `mon0`), which only
  exists with that flag. **So I.4 is unblockable by a driver rebuild, no external sniffer.**
  **Rebuild attempt 2026-06-21 — partial:** rebuilt with `CONFIG_MORSE_MONITOR=y` (monitor.o
  compiled, 24 `morse_mon` syms) but the reverse-engineered flag set (+ a `-Wno-error` hack)
  produced a module that **didn't register the `morse_spi` driver** -> chip never probed ->
  no `wlan1`; reverted, chronium restored. Proper fix: rebuild with the *exact original
  recipe* + `CONFIG_MORSE_MONITOR`. **Alternative:** the ESP32 has a raw-frame hook
  (`mmwlan_register_rx_frame_cb` + `MMWLAN_FRAME_BEACON`) giving raw bytes. What we
  *do* know of the framing comes from instrumentation, not a clean capture: chronium's
  beacon `source_addr = BSSID` (DBG-SA + the morse `morse_dot11ah_beacon_to_s1g` source),
  the data header layout (DBG17: A1=RA@4, A2=TA@10), and the M5 probe-response decode. The
  one thing still *unverified* on-wire is **our ESP32's own beacon `source_addr`** (MAC vs
  BSSID) — it needs either a monitor-enabled morse build (`morse0` capture) or the ESP32
  raw-frame hook.

**Net (initial run):** IBSS data interoperates on-air with the reference implementation
(I.3 ✓); the one defect was the ESP32's beacon-based peer discovery against real
`morse_driver` S1G beacons (I.2) — exactly the risk this test was built to surface.

**Fix verified (2026-06-20, #16 done).** Root cause: the IBSS RX filter read the
transmitter via `dot11_get_ta()` (legacy `addr2` offset) even for S1G beacons, whose
compressed `dot11_s1g_beacon_hdr` puts `source_addr` at offset 4 and the **time_stamp**
at the addr2 offset — so every reference beacon minted a phantom peer from the ticking
timestamp. Fixed in `umac_datapath.c` to read `source_addr` for S1G beacons (the
existing BSSID check then drops our own `SA=BSSID` beacons while admitting a real
peer's). Re-run: ESP32 forms **exactly 1 clean peer** for the Linux MAC (`3c:22:7f…`,
**0 phantoms**), discovers it **passively from its beacon**, and pings bidirectionally —
**ESP32→Linux 30/30 replies, Linux→ESP32 4/4 0% loss** (~12 ms). **I.1, I.2, I.3 now
pass.** Remaining: I.4 (on-air frame diff / #11) and I.5 (mixed 4-node cell).

### Linux-side bring-up (reference template — adapt to the Luckfox worklog)

```sh
# morse_driver loaded, firmware (mm6108 1.17.6) + BCF in place, iface = wlan0.
sudo iw reg set US
sudo ip link set wlan0 down
sudo iw dev wlan0 set type ibss
sudo ip link set wlan0 up

# Join OUR cell with the pinned BSSID, on the S1G channel matching ch27/1MHz/op-class 68.
# The exact freq/channel arg follows morse_driver's S1G convention (see Luckfox worklog);
# fixed-freq + BSSID is required because neither side does TSF merge (by design).
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
| P1.4 | Recovery ☑ | power-cycle a node mid-stream → link + data resume (done in the 2026-06-20 stress test §"P1.4") |
| P1.5 | Soak (overnight) ☑ | hours of continuous traffic → no asserts/crashes, stable RTT, no heap growth |

**P1.5 soak ☑ — 2026-06-21, ~6.5 h, 4-node cell (3 ESP32 + chronium).** Continuous
traffic (chronium 2/s to each ESP32 + the ESP32s' all-pairs cross-pings); ESP32 firmware
instrumented with a 30 s heap/uptime telemetry line. Result on all three boards: **uptime
6 h 31–32 m, 0 reboots, 0 asserts/panics/backtraces**, data flowing to the end (chronium
`icmp_seq ~47 000`, **12–19 ms RTT**, no degradation). **No heap leak** — `heap_min` over
6.5 h: ACM0 **Δ0 B**, ACM1 −108 B, ACM2 −548 B (transient noise, not a growing leak; a leak
would be 100s of KB). The merged stack (adoption + #16/#17 + data-driven discovery) is
stable for long-duration continuous operation. *(P1.1–P1.3 throughput/jitter/MTU numbers
still uncollected; stability + recovery + soak are the load-bearing ones and pass.)*

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
   (P2.2). Merge (#4) is out of scope (provisioned mesh); the remaining cleanup is
   unifying create/join against the agreed BSSID (#7).

---

## 9. Suggested execution order

1. ☑ Make the **N-node addressing** app change (§3) + a multi-node/reliability build.
2. ☑ Bench the **3 ESP32 nodes** (ACM0/1/2) → run **P0** (P0.1–0.7).
3. ☑ Bring up the **Linux node**, pin the matching config (§5) → run **P0.5/I.x**
   (discovery → beacon → ping → mixed cell). I.4 on-air diff ✗ (needs external sniffer).
4. ☑ **P1 reliability** — recovery (P1.4) + **~6.5 h soak (P1.5)**. P1.1–1.3 numbers TODO.
5. ☐ **P2 RF/edge** as time / physical access allows (range, etc.).

Record results inline (flip ☐ → ☑/✗ with a one-line note) as each test runs.
**Status (2026-06-21): foundation validation complete — see §1. Next: #9 boot-time
(power-save gating), then Phase 2.**
