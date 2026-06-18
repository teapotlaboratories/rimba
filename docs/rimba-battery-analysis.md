# Rimba Protocol — Power and Battery Analysis

**Hardware**: ESP32-S3 + MM6108 (802.11ah) + RV-3028-C7 RTC + RV-3028-C7 RTC (leaf & relay)  
**Companion to**: rimba-protocol-spec.md Draft 0.26

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

| Component | State | Current |
|---|---|---|
| MM6108 (802.11ah) | IBSS receive (always-on) | 12 mA |
| MM6108 | Hibernate / deep sleep | 0.1 mA |
| ESP32-S3 | FreeRTOS light sleep | 2 mA |
| ESP32-S3 | Deep sleep | 0.02 mA |
| RV-3028-C7 RTC | Always on (any mode) | 0.00004 mA (40 nA) |

---

## 3. Leaf Node Power Analysis

The leaf has a fundamentally different power profile from the relay. It spends 99.97% of its time in deep sleep and wakes for only ~230 ms per cycle.

### 3.1 Leaf Current Breakdown

```
15-minute cycle (LEAF_SLEEP_MS = 900,000 ms, sparse-first default):

Active phase (230 ms per cycle):
  MM6108 IBSS boot + TX/RX:   ~20 mA average
  ESP32-S3 active (brief):     included in 20 mA average
  Duration:                    0.230 s

Sleep phase (899.77 s per cycle):
  MM6108 hibernate:            ~0.10 mA
  ESP32-S3 deep sleep:          0.02 mA
  RV-3028-C7 RTC:               0.00004 mA
  Total sleep:                 ~0.12 mA

  Note: MM6108 hibernate current is the key unknown.
  The spec targets 15 µA total sleep — achievable if MM6108
  hibernate reaches ~10 µA. Measure in Phase 1 of dev plan.

Weighted average:
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

## 4. Relay Sleep Options

Two relay sleep modes are defined in the protocol spec (Section 12.10):

- **Continuous mode**: MCU in FreeRTOS light sleep, MM6108 always in IBSS receive mode. Simple. Never misses incoming frames.
- **Scheduled mode**: Both MCU and MM6108 powered down between TDMA active slots. Requires TSF-based wake timer and peer schedule coordination.

---

## 5. Continuous Mode — Current Derivation

```
MM6108 IBSS receive:    12.00 mA  (continuous, always on)
ESP32-S3 light sleep:    2.00 mA  (continuous, wakes on MM6108 interrupt)
RV-3028-C7 RTC:          0.00 mA  (40 nA, negligible)
─────────────────────────────────────────────────────────────────────
Total Continuous mode:          14.00 mA

This is independent of K (backbone peer count) or leaf count.
The relay is always drawing 14 mA regardless of traffic.
```

---

## 6. Scheduled Mode — Current Derivation

Scheduled mode has two phases. Every component is weighted by the fraction of time in each phase.

### 5.1 Phase fractions (K=6 backbone peers, 100 leaves, 15-min leaf interval)

```
Backbone TDMA duty    = (1 TX slot + K RX slots + 3 MGMT slots) / 20 total slots
                      = (1 + 6 + 3) / 20 = 50%

Leaf window duty      = N_leaves × leaf_window_ms / leaf_sleep_ms
                      = 100 × 100ms / 900,000ms = 1.1%

Active fraction       = backbone duty + leaf window duty = 51.1%
Sleep fraction        = 100% − 51.1% = 48.9%
```

### 5.2 Weighted current

```
During active phase (51.1% of time):
  MM6108 IBSS RX:          12 mA × 51.1% = 6.13 mA
  ESP32-S3 light sleep:     2 mA × 51.1% = 1.02 mA

During sleep phase (48.9% of time):
  MM6108 hibernate:        0.1 mA × 48.9% = 0.049 mA
  ESP32-S3 deep sleep:    0.02 mA × 48.9% = 0.010 mA

RTC (always):                               0.000 mA
─────────────────────────────────────────────────────────────────────
Total Scheduled mode (K=6, 100 leaves, 15-min):   7.21 mA ≈ 7.2 mA
```

**Note on MCU weighting**: The MCU draws 2 mA in light sleep (active phase) and 0.02 mA in deep sleep (sleep phase). It must be weighted by duty cycle. Counting it as a flat 2 mA across all time overstates the MCU contribution by ~1 mA.

### 5.3 Where the saving comes from

```
Continuous mode:  MM6108 12.0 mA (100%) + MCU 2.0 mA (100%) = 14.0 mA
Scheduled mode:  MM6108  6.1 mA (51%)  + MCU 1.0 mA (51%)  =  7.2 mA

Saving:    MM6108 saves 5.9 mA   MCU saves 1.0 mA    = 6.8 mA total

The saving is almost entirely the MM6108 being off during sleep slots.
```

---

## 7. K Sensitivity (Backbone Peer Count)

The backbone peer count K determines the active fraction. The formula for any K:

```
active_fraction = (1 + K + 3) / 20     [for 20-slot superframe]

Scheduled mode average = (12 + 2) mA × active_fraction
                 + (0.1 + 0.02) mA × (1 − active_fraction)

Simplified (dominant terms):
  Scheduled mode ≈ 14 mA × active_fraction + 0.12 mA × sleep_fraction
```

| K peers | Active fraction | Scheduled mode avg | Continuous mode | Saving |
|---|---|---|---|---|
| 2 | 30% | 4.3 mA | 14 mA | 9.7 mA |
| 4 | 40% | 5.7 mA | 14 mA | 8.3 mA |
| 6 | 50% | 7.2 mA | 14 mA | 6.8 mA |
| 8 | 60% | 8.6 mA | 14 mA | 5.4 mA |
| 10 | 70% | 10.0 mA | 14 mA | 4.0 mA |
| 12 | 80% | 11.3 mA | 14 mA | 2.7 mA |
| 14+ | ≥ 85% | ≥ 12.0 mA | 14 mA | ≤ 2 mA → use Continuous mode |

*Includes 100 leaves at 15-min interval (+1.1% active fraction, +0.13 mA)*

---

## 8. Leaf Count Sensitivity (K=6 fixed)

With the mandatory 1 ppm RTC, leaf windows are 100ms wide (vs 1000ms with internal MCU RTC). This makes leaf count a minor variable.

```
Leaf window fraction = N_leaves × 100ms / 900,000ms

Per 100 leaves: 100 × 100 / 900,000 = 1.11% additional active fraction
Additional current per 100 leaves: 14 mA × 0.0111 = 0.16 mA
```

| Leaves | Leaf window % | Additional current | Total (K=6) |
|---|---|---|---|
| 0 | 0.0% | 0.00 mA | 7.1 mA |
| 100 | 1.1% | 0.13 mA | 7.2 mA |
| 300 | 3.3% | 0.40 mA | 7.5 mA |
| 500 | 5.6% | 0.67 mA | 7.8 mA |
| 900 | 10.0% | 1.20 mA | 8.3 mA |

Leaf count barely matters. Even at 900 leaves the increase is only 1.2 mA. Backbone peer count K is the dominant variable.

---

## 9. Battery-Only Life (Relay) (No Solar)

```
Battery life = 5,000 mAh / average_current_mA

Continuous mode:  5,000 / 14.0  =  357 h  = 14.9 days  ≈ 2 weeks
Scheduled mode (K=6, 100 leaves):
           5,000 / 7.2   =  694 h  = 28.9 days  ≈ 4 weeks

Scheduled mode by K:
  K=2:  5,000 / 4.3  = 1,163 h = 48.5 days  ≈ 7 weeks
  K=4:  5,000 / 5.7  =   877 h = 36.5 days  ≈ 5 weeks
  K=6:  5,000 / 7.2  =   694 h = 28.9 days  ≈ 4 weeks
  K=8:  5,000 / 8.6  =   581 h = 24.2 days  ≈ 3.5 weeks
  K=10: 5,000 / 10.0 =   500 h = 20.8 days  ≈ 3 weeks
```

---

## 10. Solar Sustainability (Relay)

### 9.1 Daily energy balance

```
Solar assumptions:
  Panel: 2W
  System efficiency (MPPT charger × battery): 85%
  Daily harvest = 2W × peak_sun_hours × 0.85 / 3.7V

Daily consumption:
  Continuous mode:  14.0 mA × 24h = 336 mAh/day
  Scheduled mode:   7.2 mA × 24h = 173 mAh/day

Net daily balance (harvest − consumption):

  Peak sun   Harvest      Continuous mode net    Scheduled mode net
  ──────────────────────────────────────────────────────────────
  1h           459 mAh    +123 mAh  ✅    +286 mAh  ✅
  2h           919 mAh    +583 mAh  ✅    +746 mAh  ✅
  3h         1,378 mAh  +1,042 mAh  ✅  +1,205 mAh  ✅
  4h         1,837 mAh  +1,501 mAh  ✅  +1,664 mAh  ✅
  5h         2,297 mAh  +1,961 mAh  ✅  +2,124 mAh  ✅

Both options are self-sustaining with a 2W panel even in
heavy overcast with only 1 peak sun hour per day.
```

### 9.2 Minimum panel for net-zero

```
Required harvest = daily consumption
Required harvest = panel_W × 5h × 0.85 / 3.7V
Solving for panel_W:

Continuous mode: panel_W = 336 × 3.7 / (5 × 0.85) = 0.29W → use 0.5W panel
Scheduled mode: panel_W = 173 × 3.7 / (5 × 0.85) = 0.15W → use 0.2W panel

At 3 peak sun hours (conservative mid-latitude):
Continuous mode: 0.49W → 0.5W panel still sufficient
Scheduled mode: 0.25W → 0.3W panel sufficient
```

### 9.3 Solar failure survival (panel blocked, e.g. snow / fault)

How many days does the battery last if the panel produces nothing?

```
This is the same as Section 8 (battery-only life):
  Continuous mode: ~2 weeks before battery exhausted
  Scheduled mode: ~4 weeks before battery exhausted

Practical note: a 2W panel at 10% of rating (heavy cloud, shade)
still produces 46 mAh/day. Scheduled mode (173 mAh/day consumption)
is not sustained at this level. Continuous mode (336 mAh/day) is not
sustained either. The battery absorbs the deficit in both cases.
```

### 9.4 Extended cloudy season analysis

How many consecutive cloudy days can the relay survive starting from a full battery?

```
Scenario: 2W panel, very poor sun (0.5 peak hours/day, heavy overcast)
  Daily harvest:      0.5h × 2W × 0.85 / 3.7V = 230 mAh/day

Net per day:
  Continuous mode: 230 − 336 = −106 mAh/day (draining at 106 mAh/day)
  Scheduled mode: 230 − 173 =  +57 mAh/day (still surplus — self-sustaining!)

Starting from full battery (5,000 mAh):
  Continuous mode: 5,000 / 106 = 47 days until battery exhausted
  Scheduled mode: never exhausted — net positive even at 0.5h/day sun

Scheduled mode becomes self-sustaining at:
  173 mAh/day = 2W × h × 0.85 / 3.7V → h = 0.40 peak hours/day

A 2W panel with Scheduled mode sustains the relay with as little as
24 minutes of useful sun per day — indoor window or deep forest.
```

---

## 11. Summary Table

| Scenario | Continuous mode | Scheduled mode (K=6) |
|---|---|---|
| Average current | 14.0 mA | 7.2 mA |
| Battery only | ~2 weeks | ~4 weeks |
| Min panel (5h sun) | 0.3W | 0.16W |
| Min panel (3h sun) | 0.5W | 0.26W |
| Self-sustaining sun threshold (2W panel) | 0.40 h/day | 0.20 h/day |
| Recommended panel | 2W | 1W |
| Implementation effort | Low | Medium-High |
| Boot time dependency | None | MM6108 < 100ms |

---

## 12. Recommendation

```
For v1 deployment with solar:
  Continuous mode + 2W panel.
  14 mA average, self-sustaining from 0.4 peak sun hours per day.
  ~2 weeks battery backup during total solar failure.
  No implementation risk — simple FreeRTOS light sleep.

For v2 or battery-only deployment:
  Scheduled mode + 1W panel.
  7.2 mA average, self-sustaining from 0.2 peak sun hours per day.
  ~4 weeks battery backup during total solar failure.
  Requires MM6108 IBSS boot time < 100ms (measure in Phase 1).

For K ≥ 12 deployments:
  Scheduled mode saving is < 2 mA — not worth the complexity.
  Use Continuous mode with a slightly larger panel.
```

---

*Document version: 1.0*  
*Companion spec: rimba-protocol-spec.md Draft 0.26*
