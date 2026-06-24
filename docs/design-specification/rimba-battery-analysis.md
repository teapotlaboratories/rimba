# Rimba Protocol — Power and Battery Analysis

**Hardware**: ESP32-S3 + MM6108 (802.11ah) + RV-3028-C7 RTC (leaf & relay)  
**Companion to**: rimba-protocol-spec.md Draft 0.26

> **Baseline updated 2026-06-21** to the MM6108 datasheet currents
> (MM6108-MF08651-US Data Sheet — Listen/active and Table 6 sleep modes). The
> relay analysis now uses **26 mA** idle-RX (datasheet "Listen" at 1 MHz), not the
> earlier 12 mA estimate. See §2 and the note there. Sleep-mode reachability is in
> [`rimba-mm6108-powersave-analysis.md`](rimba-mm6108-powersave-analysis.md) §9.
>
> 📌 **SPEC UPDATE PENDING:** the protocol spec (`rimba-protocol-spec.md`,
> Section 12/13 power & §15 open issues) still carries the old relay/power
> figures and the "IBSS ATIM power save" open issue. Once the idle-RX current is
> measured on hardware, fold the final numbers (and the closed ATIM/Deep-sleep
> findings) back into the spec. Tracked in the development plan §7.

---

## 1. Battery Specification

```
2× Samsung 30Q 18650 in parallel

  Rated capacity:          2 × 3,000 mAh = 6,000 mAh at 3.7V
  Usable (3.0V cut-off):   ~5,500 mAh
  LDO regulator loss       3.3V / 3.7V = 89% efficiency
  Effective at 3.3V rail:  5,500 × 0.89 ≈ 4,900 mAh

  Used throughout this document: 5,000 mAh (rounded conservative)
```

---

## 2. Component Current Reference

MM6108 figures from the datasheet (MM6108-MF08651-US, Table 6 + Listen/active).

| Component | State | Current |
|---|---|---|
| MM6108 | **active RX / idle Listen** (1 MHz, always-on relay) | **26 mA** ¹ |
| MM6108 | active RX Listen (2–8 MHz) | 30–37 mA |
| MM6108 | active RX decode (MCS0–7) | 26–67 mA |
| MM6108 | **Snooze** (RC osc on, memory retained, timer wake) | 0.042 mA (42 µA) |
| MM6108 | **Deep sleep** (RC osc on, timer wake) | 0.001 mA (1 µA) |
| MM6108 | **Hibernate** (power off, ext-IRQ wake) | 0.00005 mA (0.05 µA) |
| ESP32-S3 | FreeRTOS light sleep | 2 mA |
| ESP32-S3 | Deep sleep (RTC retention) | 0.02 mA (~10–20 µA) |
| RV-3028-C7 RTC | Always on (any mode) | 0.00004 mA (40 nA) |

¹ **Idle-RX baseline = 26 mA** — the datasheet "Listen" current at the 1 MHz cell
bandwidth. An earlier version of this doc assumed 12 mA; that was optimistic and
unverified. 26 mA is now used for all relay numbers (§5–§12). A receiver *can*
idle below "active Listen," so the true figure could be lower — **measure idle-RX
at 1 MHz on hardware**; if it comes in under 26 mA, relay life and solar margins
improve linearly. This is the single highest-leverage measurement for relay BOM.

**Sleep-mode note:** Hibernate is **0.05 µA** (an earlier "~0.1 mA" guess was
2000× too high). A powered-off radio is therefore negligible in sleep; the leaf
sleep floor is **ESP32-bound** (~10–20 µA deep sleep + 40 nA RTC). The remaining
radio cost is the cold-boot **energy** per wake (RISK-02), not sleep current.

---

## 3. Leaf Node Power Analysis

The leaf has a fundamentally different power profile from the relay. It spends 99.97% of its time in deep sleep and wakes for only ~230 ms per cycle. **Leaf life is sleep-dominated, so it is unaffected by the idle-RX baseline** — the radio is only on (and only at active currents) during its brief wake window.

### 3.1 Leaf Current Breakdown

```
15-minute cycle (LEAF_SLEEP_MS = 900,000 ms, sparse-first default):

Active phase (230 ms per cycle):
  MM6108 cold-boot + IBSS join + TX/RX:  ~20 mA average
    (boot-dominated: SPI firmware load draws less than active RX;
     the brief 26–37 mA RX/TX bursts are included in this blend)
  ESP32-S3 active (brief):                included in the 20 mA average
  Duration:                               0.230 s

Sleep phase (899.77 s per cycle):
  MM6108 hibernate (datasheet):  0.00005 mA  (0.05 µA)
  ESP32-S3 deep sleep:           0.02 mA     (~10–20 µA, dominant)
  RV-3028-C7 RTC:                0.00004 mA
  Total sleep:                  ~0.015–0.020 mA  (ESP32-bound)

  The MM6108 hibernate "key unknown" is settled by the datasheet (0.05 µA,
  negligible). The sleep floor is the ESP32-S3 deep sleep. The 15 µA total-sleep
  target is met if the ESP32 reaches ~10 µA (achievable on the S3 with RTC-only
  retention); at a conservative 20 µA the leaf 15-min average is ~25 µA.

Weighted average (using ~15 µA sleep, optimized ESP32):
  Active:  20 mA    × 0.230 / 900    = 0.0051 mA =   5.1 µA
  Sleep:   0.015 mA × 899.77 / 900   = 0.015 mA  =  15.0 µA
  ─────────────────────────────────────────────────────────────
  Total:                                            ~20.1 µA
```

### 3.2 Battery Life Formula

The average leaf current as a function of wake interval T (seconds):

```
avg_mA = 0.015 + 4.585 / T

Where:
  0.015 = sleep current (mA)
  4.585 = (active_mA − sleep_mA) × active_duration
        = (20 − 0.015) × 0.230 = 4.596 mAs per cycle ≈ 4.585

This is the asymptotic formula: as T → ∞, avg → 0.015 mA (sleep only).
As T decreases, the active fraction dominates.
```

### 3.3 Battery Life by Wake Interval

Using a 3,000 mAh leaf battery at 3.6V (e.g. single 18650 or 2× AA):

```
Wake interval   Avg current   Battery life (no sensor)
───────────────────────────────────────────────────────────────
15 min (900s)    20.1 µA      3,000,000 / 20.1 = 149,254h = 17.0 years
5 min  (300s)    30.3 µA      3,000,000 / 30.3 =  99,010h = 11.3 years
1 min   (60s)    91.4 µA      3,000,000 / 91.4 =  32,818h =  3.75 years
30 sec           168 µA       3,000,000 / 168  =  17,857h =  2.04 years
10 sec           474 µA       3,000,000 / 474  =   6,329h =  0.72 years

Diminishing returns: halving the wake interval roughly doubles
average current beyond ~5 minutes, rapidly draining the battery.
```

### 3.4 Sensor Overhead Impact

The theoretical 17-year figure (Section 3.3, 15-min interval) assumes only radio + MCU power. Real sensor hardware adds to the active phase.

```
Realistic estimate from spec: 5–8 years (3,000 mAh, 15-min interval)

To achieve 5-year life:
  Required avg current = 3,000,000 µAh / (5 × 8,760h) = 68.5 µA
  Sensor overhead = 68.5 − 20.1 = 48.4 µA additional average
  Per-cycle energy = 48.4 µA × 900s  = 43.6 mC = 43.6 mAs

What 43.6 mAs looks like in practice:
  I2C/SPI sensor read (5 mA for 8.7 s): 5 × 8.7 = 43.5 mAs ← matches
  OR MCU active processing at 40 mA for 1.1 s extra per cycle

Sensor current benchmark (3,000 mAh battery, 15-min interval):
  Sensor draw   Avg overhead   Realistic life
  ────────────────────────────────────────────────
  1 mA sensor   1.1 µA        16.1 years
  5 mA sensor   5.6 µA        14.3 years
  20 mA sensor  22 µA          9.9 years
  40 mA sensor  44 µA          6.7 years
  60 mA sensor  66 µA          4.9 years

Sensor draw is measured only during active period (active_s per cycle).
Overhead = sensor_mA × active_s / cycle_s
```

### 3.5 Alert Event Battery Impact

Alert events (sensor GPIO interrupt wake, Section 9.8 of spec) are additional wake cycles outside the RTC schedule.

```
Energy per alert wake:
  Same 230ms active sequence = 20 mA × 0.230s = 4.6 mAs = 0.00128 mAh

Impact of alerts on annual budget (3,000 mAh battery):
  Alert frequency   Annual energy   % of budget   Battery impact
  ─────────────────────────────────────────────────────────────────
  1 per day         0.47 mAh        0.016%         < 1 week reduction
  10 per day        4.7 mAh         0.16%           < 3 weeks reduction
  100 per day       47 mAh          1.6%            ~3 months reduction
  1,000 per day    467 mAh          15.6%           ~2.6 years reduction

For typical sensor applications (< 10 alerts/day):
  Alert events are effectively free — negligible battery impact.
```

### 3.6 Battery Selection by Deployment

```
Target life    Battery option            Capacity   Comments
─────────────────────────────────────────────────────────────────────
1–2 years      CR123A (single)           1,500 mAh  Compact, field-replaceable
               AA cells × 1              2,500 mAh

3–5 years      CR123A × 2 in parallel    3,000 mAh  Practical for most IoT
               18650 single              3,000 mAh  Common, cheap
               AA cells × 2             5,000 mAh  Very common, cheap

5–10 years     18650 × 2 in parallel    6,000 mAh  Bulkier but long-lived
               AA cells × 4            10,000 mAh  Best value for longevity

10+ years      D cells × 2             32,000 mAh  Large form factor
               Custom Li-SOCl₂ pack    Varies      Best energy density,
                                                    higher cost

Recommended for most deployments: single 18650 (3,000 mAh)
  → 5–8 years realistic → covers most IoT replacement cycles
  → Same cell family as relay (simplifies spare parts)
  → Widely available, known chemistry

If form factor is constrained: 2× CR123A in parallel (3,000 mAh total)
  → Same capacity, shorter and wider than 18650
  → Field-replaceable without tools
```

### 3.7 Battery Life Summary (Leaf, by interval and battery)

```
3,000 mAh leaf battery, sensor adds ~50 µA overhead:

Wake interval   No sensor    With sensor (~50 µA overhead)
──────────────────────────────────────────────────────────
15 min (900s)   17.0 years   5–8 years   ← spec default
5 min  (300s)   11.3 years   4–6 years
1 min   (60s)    3.75 years  2–3 years
30 sec           2.04 years  1–2 years
10 sec           0.72 years  0.5–1 year

5,000 mAh leaf battery (2× AA), with sensor overhead:
15 min          ~9–14 years  → covers full deployment lifecycle
5 min           ~7–10 years  → good for frequent-read deployments
```

---

## 3A. Chip sleep modes — per-node viability (datasheet-grounded)

The MM6108 datasheet defines three hardware sleep modes; the firmware/host
analysis (`rimba-mm6108-powersave-analysis.md`) determined which are reachable on
an IBSS (ADHOC) vif. Mapping that onto Rimba roles:

| Chip mode (typ current) | IBSS-reachable? | Leaf | Relay / Gateway / Mule |
|---|---|---|---|
| Active RX (26–37 mA) | yes | ✗ far too high | ✓ **Continuous mode** — must hear peers |
| **Snooze** (42 µA) | ✓ via `CONFIG_PS` patch | ✗ (~2.9 mA in-cell, see below) | ✗ deaf to peers (no ATIM) |
| **Deep sleep** (1 µA) | ✗ STA-only in firmware | — | — |
| **Hibernate / power-off** (0.05 µA) | ✓ via `RESET_N` | ✓ **RTC-scheduled leaf** | ✓ **Scheduled-mode** sleep slots |

**Only two modes have a production role:** Active RX (always-on nodes) and
Hibernate/power-off (RTC-scheduled — both leaf sleep and relay Scheduled-mode
slots). Snooze and Deep sleep fit no Rimba role.

### Why Snooze is not a leaf mode

Could a leaf stay in the cell and use radio Snooze (dynamic PS) instead of fully
powering off? A node that stays in the IBSS cell must keep the ESP32 awake enough
to hold TSF/serve the link (light sleep, 2 mA) **and** TX its own S1G beacon every
beacon interval (100 TU = 102.4 ms → 9.77/s):

```
  ESP32 light sleep (must stay in-cell):          2.00 mA
  Beacon TX  (~30 mA × ~3 ms × 9.77/s):           0.88 mA
  MM6108 Snooze floor between beacons:            0.04 mA
  ──────────────────────────────────────────────────────
  Snooze-leaf average:                          ~ 2.9 mA
  Battery life (3,000 mAh): 3,000 / 2.9 = 1,034 h ≈ 43 days
```

That is ~150× worse than the RTC power-off leaf (20 µA, ~17 years, §3.3). The
42 µA Snooze floor is irrelevant — the cost is *staying in the cell at all* (MCU
awake + mandatory beaconing). Powering **off** (Hibernate/`RESET_N`) drops the
node out of the cell entirely, which is why the RTC design wins. Snooze only helps
a node that is already always-on *and* never needs to receive unsolicited peer
traffic — no Rimba role fits, so Snooze stays a bench curiosity (the `CONFIG_PS`
patch) for send-only tests, not a production mode.

---

## 4. Relay Sleep Options

Two relay sleep modes are defined in the protocol spec (Section 12.10):

- **Continuous mode**: MCU in FreeRTOS light sleep, MM6108 always in IBSS receive mode. Simple. Never misses incoming frames.
- **Scheduled mode**: Both MCU and MM6108 powered down between TDMA active slots. Requires TSF-based wake timer and peer schedule coordination.

---

## 5. Continuous Mode — Current Derivation

```
MM6108 IBSS receive (idle Listen, 1 MHz):  26.00 mA  (continuous, always on)
ESP32-S3 light sleep:                        2.00 mA  (wakes on MM6108 interrupt)
RV-3028-C7 RTC:                              0.00 mA  (40 nA, negligible)
─────────────────────────────────────────────────────────────────────
Total Continuous mode:                      28.00 mA

This is independent of K (backbone peer count) or leaf count.
The relay draws ~28 mA regardless of traffic — dominated by the
always-on receiver. (At 12 mA idle-RX this was 14 mA; the figure
scales linearly with the measured idle-RX current.)
```

---

## 6. Scheduled Mode — Current Derivation

Scheduled mode has two phases. Every component is weighted by the fraction of time in each phase.

### 6.1 Phase fractions (K=6 backbone peers, 100 leaves, 15-min leaf interval)

```
Backbone TDMA duty    = (1 TX slot + K RX slots + 3 MGMT slots) / 20 total slots
                      = (1 + 6 + 3) / 20 = 50%

Leaf window duty      = N_leaves × leaf_window_ms / leaf_sleep_ms
                      = 100 × 100ms / 900,000ms = 1.1%

Active fraction       = backbone duty + leaf window duty = 51.1%
Sleep fraction        = 100% − 51.1% = 48.9%
```

### 6.2 Weighted current

```
During active phase (51.1% of time):
  MM6108 active RX:        26 mA × 51.1% = 13.29 mA
  ESP32-S3 light sleep:     2 mA × 51.1% =  1.02 mA

During sleep phase (48.9% of time):
  MM6108 hibernate:    0.00005 mA × 48.9% = 0.00002 mA  (negligible)
  ESP32-S3 deep sleep:    0.02 mA × 48.9% = 0.010 mA

RTC (always):                               0.000 mA
─────────────────────────────────────────────────────────────────────
Total Scheduled mode (K=6, 100 leaves, 15-min):   14.32 mA ≈ 14.3 mA
```

**Note on MCU weighting**: The MCU draws 2 mA in light sleep (active phase) and 0.02 mA in deep sleep (sleep phase). It must be weighted by duty cycle. The sleep-phase radio term is negligible (Hibernate 0.05 µA), so Scheduled mode is dominated entirely by the **active-phase RX**.

### 6.3 Where the saving comes from

```
Continuous mode:  MM6108 26.0 mA (100%) + MCU 2.0 mA (100%) = 28.0 mA
Scheduled mode:  MM6108 13.3 mA (51%)  + MCU 1.0 mA (51%)  = 14.3 mA

Saving:    MM6108 saves 12.7 mA   MCU saves 1.0 mA    = 13.7 mA total

The saving is almost entirely the MM6108 being off during sleep slots.
```

---

## 7. K Sensitivity (Backbone Peer Count)

The backbone peer count K determines the active fraction. The formula for any K:

```
active_fraction = (1 + K + 3) / 20     [for 20-slot superframe]

Scheduled mode average = (26 + 2) mA × active_fraction
                 + (0.00005 + 0.02) mA × (1 − active_fraction)

Simplified (sleep term ≈ 0.01 mA, negligible):
  Scheduled mode ≈ 28 mA × active_fraction
```

| K peers | Active fraction | Scheduled mode avg | Continuous mode | Saving |
|---|---|---|---|---|
| 2 | 30% | 8.7 mA | 28 mA | 19.3 mA |
| 4 | 40% | 11.5 mA | 28 mA | 16.5 mA |
| 6 | 50% | 14.3 mA | 28 mA | 13.7 mA |
| 8 | 60% | 17.1 mA | 28 mA | 10.9 mA |
| 10 | 70% | 19.9 mA | 28 mA | 8.1 mA |
| 12 | 80% | 22.7 mA | 28 mA | 5.3 mA |
| 14 | 90% | 25.5 mA | 28 mA | 2.5 mA |
| 15+ | ≥ 95% | ≥ 26.9 mA | 28 mA | ≤ 1 mA → use Continuous mode |

*Includes 100 leaves at 15-min interval (+1.1% active fraction, +0.31 mA). The
Scheduled-mode advantage now persists to higher K than at 12 mA idle-RX (the
crossover moved from K≈14 to K≈15) because the per-slot RX saving is larger.*

---

## 8. Leaf Count Sensitivity (K=6 fixed)

With the mandatory 1 ppm RTC, leaf windows are 100ms wide (vs 1000ms with internal MCU RTC). This makes leaf count a minor variable.

```
Leaf window fraction = N_leaves × 100ms / 900,000ms

Per 100 leaves: 100 × 100 / 900,000 = 1.11% additional active fraction
Additional current per 100 leaves: 28 mA × 0.0111 = 0.31 mA
```

| Leaves | Leaf window % | Additional current | Total (K=6) |
|---|---|---|---|
| 0 | 0.0% | 0.00 mA | 14.0 mA |
| 100 | 1.1% | 0.31 mA | 14.3 mA |
| 300 | 3.3% | 0.93 mA | 14.9 mA |
| 500 | 5.6% | 1.56 mA | 15.6 mA |
| 900 | 10.0% | 2.80 mA | 16.8 mA |

Leaf count barely matters. Even at 900 leaves the increase is only 2.8 mA. Backbone peer count K is the dominant variable.

---

## 9. Battery-Only Life (Relay) (No Solar)

```
Battery life = 5,000 mAh / average_current_mA

Continuous mode:  5,000 / 28.0  =  179 h  =  7.4 days  ≈ 1 week
Scheduled mode (K=6, 100 leaves):
           5,000 / 14.3  =  350 h  = 14.6 days  ≈ 2 weeks

Scheduled mode by K:
  K=2:  5,000 / 8.7  =   575 h = 23.9 days  ≈ 3.4 weeks
  K=4:  5,000 / 11.5 =   435 h = 18.1 days  ≈ 2.6 weeks
  K=6:  5,000 / 14.3 =   350 h = 14.6 days  ≈ 2 weeks
  K=8:  5,000 / 17.1 =   292 h = 12.2 days  ≈ 1.7 weeks
  K=10: 5,000 / 19.9 =   251 h = 10.5 days  ≈ 1.5 weeks
```

---

## 10. Solar Sustainability (Relay)

### 10.1 Daily energy balance

```
Solar assumptions:
  Panel: 2W
  System efficiency (MPPT charger × battery): 85%
  Daily harvest = 2W × peak_sun_hours × 0.85 / 3.7V
                = 459.5 mAh per peak-sun-hour

Daily consumption:
  Continuous mode:  28.0 mA × 24h = 672 mAh/day
  Scheduled mode:   14.3 mA × 24h = 343 mAh/day

Net daily balance (harvest − consumption):

  Peak sun   Harvest      Continuous mode net    Scheduled mode net
  ──────────────────────────────────────────────────────────────
  1h           459 mAh    −213 mAh  ❌    +116 mAh  ✅
  2h           919 mAh    +247 mAh  ✅    +576 mAh  ✅
  3h         1,378 mAh    +706 mAh  ✅  +1,035 mAh  ✅
  4h         1,838 mAh  +1,166 mAh  ✅  +1,495 mAh  ✅
  5h         2,297 mAh  +1,625 mAh  ✅  +1,954 mAh  ✅

With a 2W panel, Scheduled mode self-sustains even at 1 peak sun hour;
Continuous mode needs ~1.5 peak sun hours per day to break even.
```

### 10.2 Minimum panel for net-zero

```
Required harvest = daily consumption
Required harvest = panel_W × sun_h × 0.85 / 3.7V
Solving for panel_W:

At 5 peak sun hours:
  Continuous mode: 672 × 3.7 / (5 × 0.85 × 1000) = 0.59W → use 1W panel
  Scheduled mode:  343 × 3.7 / (5 × 0.85 × 1000) = 0.30W → use 0.5W panel

At 3 peak sun hours (conservative mid-latitude):
  Continuous mode: 0.97W → ~1W panel
  Scheduled mode:  0.50W → ~0.5W panel
```

### 10.3 Solar failure survival (panel blocked, e.g. snow / fault)

How many days does the battery last if the panel produces nothing?

```
This is the same as Section 9 (battery-only life):
  Continuous mode: ~1 week before battery exhausted
  Scheduled mode: ~2 weeks before battery exhausted

Practical note: a 2W panel at 10% of rating (heavy cloud, shade)
still produces 46 mAh/day. Neither mode is sustained at this level
(Continuous 672, Scheduled 343 mAh/day consumption); the battery
absorbs the deficit in both cases.
```

### 10.4 Extended cloudy season analysis

How many consecutive cloudy days can the relay survive starting from a full battery?

```
Scenario: 2W panel, very poor sun (0.5 peak hours/day, heavy overcast)
  Daily harvest:      0.5h × 459.5 = 230 mAh/day

Net per day:
  Continuous mode: 230 − 672 = −442 mAh/day (draining)
  Scheduled mode: 230 − 343 = −113 mAh/day (draining, but slower)

Starting from full battery (5,000 mAh):
  Continuous mode: 5,000 / 442 = 11.3 days until exhausted
  Scheduled mode: 5,000 / 113 = 44.2 days until exhausted

Self-sustaining sun threshold (2W panel):
  Continuous mode:  672 / 459.5 = 1.46 peak hours/day (~88 min)
  Scheduled mode:   343 / 459.5 = 0.75 peak hours/day (~45 min)

At the 26 mA idle-RX baseline neither mode is self-sustaining on a 2W panel at
0.5h/day (unlike the old 12 mA estimate, where Scheduled survived 0.4h/day).
Scheduled now needs ~45 min of useful sun per day on a 2W panel; Continuous ~88
min. A larger panel restores the margin (see §10.2 and §12).
```

---

## 11. Summary Table

| Scenario | Continuous mode | Scheduled mode (K=6) |
|---|---|---|
| Average current | 28.0 mA | 14.3 mA |
| Battery only | ~1 week | ~2 weeks |
| Min panel (5h sun) | 0.6W → use 1W | 0.3W → use 0.5W |
| Min panel (3h sun) | 1.0W | 0.5W |
| Self-sustaining sun threshold (2W panel) | 1.46 h/day | 0.75 h/day |
| Recommended panel | 3W | 2W |
| Implementation effort | Low | Medium-High |
| Boot time dependency | None | MM6108 < 100ms |

> All relay figures use the **26 mA datasheet idle-RX baseline** (§2). If a
> hardware measurement shows idle-RX is lower, battery life and solar margins
> improve linearly — this is the key relay measurement to take.

---

## 12. Recommendation

```
For v1 deployment with solar:
  Continuous mode + 3W panel.
  28 mA average, self-sustaining from ~1.5 peak sun hours per day.
  ~1 week battery backup during total solar failure.
  No implementation risk — simple FreeRTOS light sleep.
  (2W panel is acceptable only in good-sun regions, ≥1.5h/day.)

For v2 or battery-only / low-sun deployment:
  Scheduled mode + 2W panel.
  14.3 mA average, self-sustaining from ~0.75 peak sun hours per day.
  ~2 weeks battery backup during total solar failure.
  Requires MM6108 IBSS boot time < 100ms (measure in Phase 1).

For K ≥ 15 deployments:
  Scheduled mode saving is < ~1 mA — not worth the complexity.
  Use Continuous mode with a slightly larger panel.

Before committing panel BOM:
  Measure idle-RX current at 1 MHz on hardware. The 26 mA datasheet figure
  drives panel size and battery backup; a lower measured value relaxes both.
```

---

*Document version: 1.1 (2026-06-21 — rebaselined to datasheet MM6108 currents)*  
*Companion spec: rimba-protocol-spec.md Draft 0.26*
