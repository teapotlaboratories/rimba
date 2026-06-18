# Rimba Protocol — RTC Selection and Comparison

**Companion to**: rimba-protocol-spec.md Draft 0.26  
**Document version**: 1.0

---

## 1. Why the RTC Matters

Rimba uses the RTC for three things:

```
Function                   Accuracy needed   Notes
────────────────────────────────────────────────────────────────────
Leaf wake scheduling        ±milliseconds     Relay opens receive window
  (relay drift window)      per cycle         at predicted leaf wake time

Bundle lifetime             ±seconds          24-hour bundle expires at
  enforcement                                 creation_ts + 24h

BPSec nonce timestamp      Monotonically     Used for anti-replay
  component                 increasing only   sequence numbers
```

The leaf wake scheduling function drives the design. RELAY_DRIFT_WINDOW_MS
sets how wide the relay's receive window is. Wider window = more relay awake
time = fewer leaves per relay:

```
max_leaves = (RELAY_DUTY_BUDGET − backbone_fraction)
             × LEAF_SLEEP_MS
             / (2 × RELAY_DRIFT_WINDOW_MS)
```

RTC accuracy directly determines RELAY_DRIFT_WINDOW_MS and therefore leaf
capacity.

---

## 2. Chip Comparison

### 2.1 Accuracy at Room Temperature

| Chip | Type | Best accuracy (25°C) | Temperature compensated |
|---|---|---|---|
| ESP32-S3 internal | RC oscillator | ±150 ppm | No |
| PCF85063TP (NXP) | Plain crystal | ±2 ppm (after trim) | No |
| DS3231 (Maxim/Analog) | TCXO | ±2 ppm | Yes |
| RV-3028-C7 (Micro Crystal) | TCXO | ±1 ppm | Yes |
| RV-8803-C7 (Micro Crystal) | TCXO | ±1 ppm | Yes |

### 2.2 Accuracy Over Temperature

A plain 32.768 kHz tuning-fork crystal (used in PCF85063TP) has a parabolic
temperature response with typical coefficient B ≈ −0.04 ppm/°C²:

```
drift_ppm(ΔT) = B × ΔT²    where ΔT = degrees from room temperature

At ΔT = 10°C:   −0.04 × 100 =  −4 ppm additional drift
At ΔT = 20°C:   −0.04 × 400 = −16 ppm additional drift
At ΔT = 30°C:   −0.04 × 900 = −36 ppm additional drift
```

This means a PCF85063TP calibrated to ±2 ppm at 25°C will drift to:
- ±6 ppm at 15°C or 35°C (±10°C swing — warm day indoors)
- ±18 ppm at 5°C or 45°C (±20°C swing — outdoor temperate)
- ±38 ppm at −5°C or 55°C (±30°C swing — outdoor cold climate)

Temperature-compensated chips (DS3231, RV-3028-C7) hold their rated
accuracy across the full operating range regardless of temperature swing.

```
Chip              0°C     25°C    40°C    −20°C   Outdoor?
──────────────────────────────────────────────────────────────
ESP32-S3 internal ~150ppm ~150ppm ~150ppm ~150ppm ✗ (too wide)
PCF85063TP         ~6ppm   ~2ppm   ~6ppm   ~34ppm  ⚠️ (climate-dependent)
DS3231             ±2ppm   ±2ppm   ±2ppm   ±2ppm   ✅
RV-3028-C7         ±1ppm   ±1ppm   ±1ppm   ±1ppm   ✅
```

### 2.3 Power Consumption

| Chip | Standby current | Impact on leaf (20 µA avg) |
|---|---|---|
| ESP32-S3 internal | 0 (built-in) | — |
| PCF85063TP | 250 nA | +1.25% |
| DS3231 | 110 nA (VCC) / 500 nA (bat) | +0.55% |
| RV-3028-C7 | 40 nA | +0.2% |
| RV-8803-C7 | 100 nA | +0.5% |

All external RTCs are negligible from a power perspective. Choice is driven
by accuracy and cost, not current.

### 2.4 Cost and Availability

| Chip | Approx unit cost | Interface | Package | Notes |
|---|---|---|---|---|
| ESP32-S3 internal | Free (built-in) | Internal | — | 150 ppm, no absolute time |
| PCF85063TP | ~$0.20–0.50 | I2C | TSSOP8 | Trim needed, no temp comp |
| DS3231 | ~$0.80–1.50 | I2C | SOIC16 | Common, large package |
| RV-3028-C7 | ~$1.50–3.00 | I2C | TDFN-8 | Spec recommendation |
| RV-8803-C7 | ~$1.80–3.50 | I2C | MICRO-8 | Alternative to RV-3028 |

---

## 3. Impact on Rimba Protocol Parameters

### 3.1 RELAY_DRIFT_WINDOW_MS formula

```
RELAY_DRIFT_WINDOW_MS = max(LEAF_BOOT_MS,
                            RELAY_DRIFT_RATE_PPM
                            × LEAF_SLEEP_MS / 1,000,000
                            × accumulated_cycles)

For single-cycle drift only (before any misses):
  1 ppm,   5-min:  max(30, 1×300,000/1,000,000)  = max(30, 0.3ms)  = 30ms
  2 ppm,   5-min:  max(30, 2×300,000/1,000,000)  = max(30, 0.6ms)  = 30ms
  20 ppm,  5-min:  max(30, 20×300,000/1,000,000) = max(30, 6ms)    = 30ms
  150 ppm, 5-min:  max(30, 150×300,000/1,000,000)= max(30, 45ms)   = 45ms

Boot time (LEAF_BOOT_MS = 30ms) dominates at all accuracies for single cycles.
The difference appears when cycles are missed (leaf silent for N cycles):
```

### 3.2 Effect of missed cycles

```
After 10 missed leaf cycles (5-min interval):

  RTC accuracy    Accumulated drift    Drift window needed
  ─────────────────────────────────────────────────────────
  1 ppm           0.3ms × 10 = 3ms    30ms   (boot dominates)
  2 ppm           0.6ms × 10 = 6ms    36ms   (boot + drift)
  5 ppm (trim)    1.5ms × 10 = 15ms   45ms   (manageable)
  20 ppm (cold)   6ms   × 10 = 60ms   90ms   (boot + drift)
  150 ppm (int.)  45ms  × 10 = 450ms  480ms  (wide)
```

### 3.3 Max leaves per relay by RTC choice

```
K=4 backbone peers, 70% duty budget, 5-min leaf interval:

  max_leaves = 0.30 × 300,000 / (2 × drift_window_ms)

  RTC choice      Drift window   Max leaves   vs. RV-3028
  ─────────────────────────────────────────────────────────
  RV-3028-C7      50ms           900          baseline
  DS3231          72ms           625          −31%
  PCF85063TP      90ms (outdoor) 500          −44%
  PCF85063TP      50ms (indoor)  900          same as RV-3028
  ESP32-S3 int.   530ms          85           −91%
```

Note: PCF85063TP matches RV-3028-C7 in stable indoor environments but
degrades in outdoor deployments with temperature variation.

---

## 4. PCF85063TP: When It Works and When It Doesn't

### 4.1 Where PCF85063TP is acceptable

```
Deployment type                        Temperature range   PCF85063TP ok?
────────────────────────────────────────────────────────────────────────────
Development / bench testing            20–25°C             ✅  Yes
Indoor deployment (office, factory)    15–35°C (±10°C)     ✅  Yes (~6ppm)
Tropical outdoor (stable climate)      25–40°C (±10°C)     ✅  Yes (~6ppm)
Temperate outdoor (seasonal change)    0–40°C  (±20°C)     ⚠️  Marginal (~18ppm)
Cold climate outdoor                  −20–40°C (±30°C)    ❌  No  (~38ppm)
```

### 4.2 Software temperature compensation option

The PCF85063TP offset register can be updated by software to compensate for
temperature drift — NXP provides application note AN10652 for this:

```
Required hardware:  PCF85063TP + temperature sensor (e.g. NCP18 thermistor
                    or Si7021) near the crystal
Required software:  ESP32-S3 reads temperature periodically
                    Computes offset: offset = B × ΔT²
                    Writes offset register to PCF85063TP
Update interval:    Every 1–5 minutes (temperature changes slowly)
Achievable accuracy: ±2 ppm over −20°C to +60°C with good implementation
```

With software compensation, PCF85063TP can approach ±2 ppm over a wide range,
but this adds complexity, an extra sensor, and more firmware code. At that point
the RV-3028-C7 is simpler and more reliable for only $1–2 more per node.

### 4.3 PCF85063TP configuration

If deploying with PCF85063TP, set `RELAY_DRIFT_RATE_PPM` in provisioning:

```
Indoor deployment (±10°C):   RELAY_DRIFT_RATE_PPM = 6
Outdoor temperate (±20°C):   RELAY_DRIFT_RATE_PPM = 20
Cold climate (±30°C):        RELAY_DRIFT_RATE_PPM = 40
```

The protocol auto-adjusts RELAY_DRIFT_WINDOW_MS based on this value.

---

## 5. Development: Using ESP32-S3 Internal RTC + NTP

During development (bench testing without an external RTC), the ESP32-S3
internal RTC combined with NTP provides an acceptable substitute.

### 5.1 How it works

```
Development setup:
  ESP32-S3 built-in 2.4 GHz WiFi → connects to development network
  SNTP client (esp_sntp.h) → syncs absolute time from pool.ntp.org
  ESP32-S3 internal RTC → stores synced time (150 ppm accuracy)

MM6108 HaLow → runs Rimba IBSS mesh (completely separate radio)

These are independent RF paths — no conflict.
NTP WiFi and HaLow IBSS operate simultaneously.
```

### 5.2 ESP-IDF implementation

```c
// In rimba_dev_ntp.c — development-only code, excluded from production

#include "esp_sntp.h"
#include "esp_wifi.h"

void rimba_dev_ntp_sync(void) {
    // Connect ESP32-S3 internal WiFi to dev network
    wifi_config_t cfg = {
        .sta = { .ssid = DEV_WIFI_SSID, .password = DEV_WIFI_PASS }
    };
    esp_wifi_set_config(WIFI_IF_STA, &cfg);
    esp_wifi_start();
    esp_wifi_connect();

    // Wait for IP
    // ... (event loop or semaphore)

    // Sync time via SNTP
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();

    // Wait for sync
    time_t now = 0;
    struct tm timeinfo = {};
    while (timeinfo.tm_year < (2020 - 1900)) {
        vTaskDelay(100 / portTICK_PERIOD_MS);
        time(&now);
        localtime_r(&now, &timeinfo);
    }

    // Disconnect WiFi — MM6108 takes over
    esp_wifi_disconnect();
    esp_wifi_stop();

    // Internal RTC now has accurate absolute time (150 ppm accuracy)
    // RELAY_DRIFT_RATE_PPM = 150 in development config
}
```

### 5.3 Development config differences from production

```
Parameter                  Production (RV-3028)   Development (NTP + internal)
────────────────────────────────────────────────────────────────────────────
RELAY_DRIFT_RATE_PPM       1                       150
RELAY_DRIFT_WINDOW_MS      50ms (derived)           45ms→530ms (derived)
Max leaves per relay       900 (K=4)                ~85 (K=4)
TIMESYNC_STALE_S           604,800 (7 days)         3,600 (1 hour) — resync often
TIME_SYNC source           Gateway                  NTP on boot
time_stale flag            Set after 7 days         Set after 1 hour (resync)
```

Max leaves drops from 900 to ~85 per relay in development mode, which is fine
for bench testing with a small number of nodes. Deploy 3–5 nodes in development
and leaf capacity is not a concern.

### 5.4 When to add the real RTC

Add the external RTC before testing:
- Leaf sleep scheduling at 15-minute intervals (internal RTC drift accumulates)
- Multi-day test runs (without NTP resync, internal drifts significantly)
- Power consumption measurements (need accurate timing)
- Production readiness testing

---

## 6. RTC Selection Decision Guide

```
Question                                      Recommendation
──────────────────────────────────────────────────────────────────────
Development/bench testing only?               ESP32-S3 internal + NTP
                                              (no external RTC needed)

Production, indoor or stable temp?            PCF85063TP (cheap)
  Cost is more important than temp range      Set RELAY_DRIFT_RATE_PPM = 6
  Temperature swing < ±15°C

Production, outdoor temperate climate?        RV-3028-C7 (recommended)
  Full spec compliance required               Meets ≤1ppm requirement
  Temperature swing > ±15°C

Production, cold climate (below 0°C)?         RV-3028-C7 or RV-8803-C7
                                              TCXO essential here

Existing design with DS3231?                  Acceptable
                                              ±2ppm TCXO, set
                                              RELAY_DRIFT_RATE_PPM = 2
```

---

## 7. Summary

```
RTC chip          PPM (worst case)  Leaves/relay  Cost/node  Recommended for
──────────────────────────────────────────────────────────────────────────────
ESP32-S3 internal ±150 ppm          ~85            $0         Development only
PCF85063TP        ±6 ppm (indoor)   900            ~$0.35     Indoor production
                  ±38 ppm (cold)    ~185                       (stable temp only)
DS3231            ±2 ppm            625            ~$1.00     Existing designs
RV-3028-C7        ±1 ppm            900            ~$2.00     ← Spec recommended
RV-8803-C7        ±1 ppm            900            ~$2.50     Alternative

The $1.65 difference between PCF85063TP and RV-3028-C7 per node costs
$825 for a 500-relay deployment. For outdoor IoT in variable climates,
this is worth it — the alternative is 44% fewer leaves per relay and
unpredictable drift behaviour in winter.
```

---

*Document version: 1.0*  
*Companion spec: rimba-protocol-spec.md Draft 0.26*
