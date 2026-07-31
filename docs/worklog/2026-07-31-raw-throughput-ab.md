# 2026-07-31 — RAW costs the AP ~22% of its downlink, with no contention to buy it back

**Goal.** RAW is being ported to fix EDCA contention for the mesh-gate. S0b-2 showed the AP's chip defers
its **own** downlink to honour the RAW window (`aci_frames_delayed = 92`). Since the gate is already the
throughput bottleneck, the obvious next question is whether RAW costs it more than the contention it
saves — and the design doc's own words are *"a GO that later has to be reversed on throughput is worse
than a slower decision."*

**Outcome.** With one STA and no contention, enabling RAW costs **~22% of downlink throughput**
(1.63 → 1.27 → 1.61 Mbit/s across an A-B-A control), and drives UDP loss from 0.12% to 7.2% at the same
offered rate. The AP deferred **37,897** of its own frames. **This measures cost only — the bench cannot
demonstrate RAW's benefit**, so the honest reading is a floor: RAW starts ~22% down on gate downlink and
has to earn that back before it is worth anything.

**No ESP code. Bench returned to radio silence on all three nodes. Nothing committed at write time.**

---

## 1. Rig — and the change that unblocked it

The blocker recorded in S0b-1/S0b-2 was that neither Pi Zero has `iperf3`, so there was no rate-controlled
generator. **A throughput measurement needs no sniffer**, which frees chronium — a Pi 5 that *does* have
`iperf3` — to be the STA:

| | |
|---|---|
| **chronite** | AP. `hostapd_s1g` from the tracked `hostapd-rimba.conf`, `iperf3 -s`. RAW driven directly by `morse_cli` |
| **chronium** | **STA** (not the sniffer, for once). `wpa_supplicant_s1g`, `raw_sta_priority=0`. Associated in 4 s, RSSI **−36 dBm** |
| chronogen, chronosalt | unused |
| ESP boards | **none** |

RAW config pinned exactly as in S0b-2, and confirmed via debugfs before measuring:

```
RAW ID: 1 (enabled: 1)
  start_time_us=0  aid: start=1 end=255
  slot info: x-slot=0 num=2 duration_us=10100
```

The window is therefore **20200 µs inside a 102400 µs beacon interval — 19.7% of the airtime.**

## 2. Calibrating the instrument first — and half of it failed

Before trusting any A/B I measured the baseline three times. That was worth doing:

| direction | baseline runs | verdict |
|---|---|---|
| **downlink** (AP→STA) | 1.68 / 1.52 / 1.68 Mbit/s | ~±10% — **usable** |
| **uplink** (STA→AP) | 104 / **0.00** / 156 Kbit/s | **unusable** |

**One uplink run transferred zero bytes.** You cannot measure a 20% effect against a baseline that
includes zero, so the uplink arm is dead as a TCP instrument.

UDP at a fixed offered rate confirmed it is the link, not TCP's reaction to it:

- uplink, 400 Kbit/s offered → 197 Kbit/s at **44% loss**, then 302 Kbit/s at **24% loss**
- downlink, 2 Mbit/s offered → 2.00 Mbit/s at **0.12% loss**

So this RF geometry has a badly asymmetric link. That is a bench property, **not** a RAW effect, and it is
why every number below is downlink.

**Downlink is also the direction that matters more for the driver use case:** the mesh-gate's bottleneck
is its own forwarding, and S0b-2 already showed the AP deferring its *own* TX.

## 3. The A-B-A result

Baseline, RAW on, then RAW off again — so a time-varying RF change cannot masquerade as the effect.

**TCP downlink (Mbit/s, receiver):**

| arm | runs | mean |
|---|---|---|
| **A** — RAW off | 1.68 · 1.52 · 1.68 | **1.63** |
| **B** — RAW on | 1.36 · 1.31 · 1.15 | **1.27** |
| **A′** — RAW off again | 1.62 · 1.68 · 1.52 | **1.61** |

**Throughput recovers when RAW is disabled**, so the drop is attributable to RAW. The ranges do not
overlap: the worst no-RAW run (1.52) still beats the best RAW run (1.36).

**≈ 22% cost.**

**UDP downlink, 2 Mbit/s offered — receiver loss:**

| A (off) | B (on) | A′ (off) |
|---|---|---|
| 0.12% | **7.2%** | 0.96% |

Same shape: a ~60× loss increase under RAW, recovering when it is turned off.

**Counters after the RAW-on downlink load** (reset before the arm):

| | AP (chronite) | STA (chronium) |
|---|---|---|
| `assignments[0]` | 1030 | 989 |
| `invalid_assignments` | 0 | 0 |
| **`aci_frames_delayed`** | **37 897** | 4 502 |
| `frame_crosses_slot` | 383 | 81 |

The AP deferred **37,897 of its own downlink frames**. That is the mechanism, in the firmware's own words.

## 4. What the number means — and what it does not

**RAW is not hard-gating the AP.** The window is 19.7% of the beacon interval. If the AP's downlink were
strictly confined to it, throughput would fall by ~80%. It fell by 22%. So the chip is imposing a
*partial* cost — deferring frames that would cross into the restricted region, not refusing to transmit
outside the window.

**The bench cannot show the benefit, and this experiment does not attempt to.** RAW's payoff is reduced
collision loss when many STAs contend. With **one** STA there is nothing to save, so what is measured
here is pure overhead. The bench has two usable Linux radios plus a few ESPs — it cannot produce the
30-plus contending clients where RAW is supposed to pay for itself.

So the correct reading is a **floor, not a verdict**: *RAW starts ~22% down on gate downlink, and the
contention it saves has to exceed that before it is worth enabling.* Whether it does, at the client counts
the gate actually targets (16–32, per the scaling analysis), is **not answered here and cannot be answered
on this bench**.

**For the mesh-gate specifically that is a real caution.** The gate is already the throughput bottleneck;
this says RAW takes about a fifth off the top before delivering anything. It does not say RAW is wrong —
it says the case for it has to be made on contention, and that case remains unmeasured.

## 5. Verification status

- **On hardware, on air, two radios.** All figures from `iperf3` at the endpoints plus firmware counters.
  No ESP board involved, nothing flashed.
- **A-B-A design**, three TCP runs per arm plus a UDP arm, with the baseline measured *before* trusting it
  — which is what caught the unusable uplink.
- **On-air frame verification: not applicable.** Nothing new was transmitted; chronite emitted stock
  beacons plus the same RPS validated byte-for-byte in S0b-2, and no hand-built frame was involved. No
  capture was taken and none was needed.
- **Bench returned to radio silence** — RAW disabled + deleted (debugfs confirms `RAW disabled`), both
  daemons and `iperf3` killed, addresses flushed, `wlan1`/`morse0` down, `type managed` restored on all
  three nodes. Verified with `ps -eo comm=`, not `pgrep -f`.

## 6. Caveats

- **One STA, one channel, one geometry, one session.** No contention, so cost only.
- **Downlink only.** The uplink instrument failed calibration (§2) and no uplink conclusion is drawn.
- **The link is badly asymmetric** on this placement — 1.6 Mbit/s down vs ~0.1–0.3 Mbit/s up with 24–44%
  UDP loss. That asymmetry is unexplained and was not chased; it may itself distort how much of the
  downlink cost is deferral vs retransmission.
- **One RAW configuration** (2 slots × 10100 µs, all AIDs, start_time 0). The cost is presumably a
  function of window fraction and slot count; that curve was not measured.
- **TCP is a reactive instrument** — part of the 22% is TCP responding to added loss/latency, not raw
  airtime denial. The UDP arm (fixed offered rate, 7.2% loss) is the cleaner statement of the same effect.

## 7. Next

This does not change the S0b verdict — the gate passed on mechanism, and this is about value, not
feasibility. It does add a question the port should carry: **at what client count does RAW's contention
saving exceed its ~22% standing cost?** That needs either many-STA hardware the bench does not have, or a
model calibrated against these numbers.

The ESP arm (S0b-3/4/5) is unaffected and still the outstanding implementation work, with S0b-3 needing
the re-plan recorded in the S1G-caps worklog.
