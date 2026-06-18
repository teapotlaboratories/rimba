# Rimba Protocol — Latency and Bandwidth Analysis

**Hardware**: ESP32-S3 + MM6108 (802.11ah HaLow)  
**Companion to**: rimba-protocol-spec.md Draft 0.26

---

## 1. Bandwidth

### 1.1 Raw Channel Capacity

Rimba operates on a single 802.11ah channel shared by all nodes in a deployment.

```
Channel bandwidth: 1 MHz (maximum range configuration)

Modulation / Coding   Raw bitrate   Typical range (open field)
──────────────────────────────────────────────────────────────
MCS0  (BPSK  1/2)      300 kbps      > 1,000 m
MCS1  (QPSK  1/2)      600 kbps        800 m
MCS4  (16QAM 3/4)    1,800 kbps        400 m
MCS7  (64QAM 5/6)    7,800 kbps        150 m

Rimba default: MCS0 (maximum range, minimum rate)
Usable after 802.11 overhead (headers, ACK, DIFS, backoff): ~150 kbps
```

### 1.2 Protocol Overhead Budget

```
Total usable channel:        150 kbps

Protocol overhead:
  RREQ/RREP routing:         ~15 kbps  (10%)
  HELLO beacons (Trickle):    ~5 kbps   (3%)
  OGM floods (proactive):    ~10 kbps   (7%)  [if active]
  ────────────────────────────────────────────
  Total overhead:             ~20 kbps  (13%)

Available for sensor data:  ~130 kbps  (87%)
```

Gossip-based RREQ forwarding (Section 8.7.1) and Trickle suppression (Section 7.1) keep routing overhead low even at scale. In a stable connected network, HELLO and routing overhead drops further as Trickle intervals double.

### 1.3 Per-Leaf Throughput

```
Single leaf, 1 reading per 15 minutes, 300-byte bundle:
  Raw data rate: 300 bytes × 8 bits / 900 s = 2.7 bps per leaf

100 leaves per relay:
  Leaf → relay:   100 × 2.7 bps          = 267 bps
  After bundle aggregation (Section 9.7):
    ~5 aggregate frames × 1,500 bytes / 900 s = 67 bps per relay
  Relay → backbone: 67 bps

500 relays total backbone traffic:
  500 × 67 bps = 33.5 kbps
  Well within 130 kbps data budget: 33.5 / 130 = 25.7% utilisation ✅
```

### 1.4 Channel Saturation Threshold

```
Channel fills when:
  total_data_bps > 130,000 bps

  total_data_bps = N_leaves × reading_size_bytes × 8 / reading_interval_s

Rearranging for maximum sustainable leaf count:
  N_leaves = 130,000 × reading_interval_s / (reading_size_bytes × 8)

At 300-byte readings:
  Reading interval   Max leaves (channel limited)
  ─────────────────────────────────────────────────
  1 minute           3,250
  5 minutes         16,250
  15 minutes        48,750
  1 hour           195,000

At 1,000-byte readings (rich sensor payload):
  15 minutes        14,625
  1 hour            58,500

Standard deployment (50,000 leaves, 300 bytes, 15 min): 25.7% channel ✅
```

The channel is not a constraint for typical sparse sensor deployments at the default 15-minute reading interval.

### 1.5 Comparison with Meshtastic

```
Protocol         Radio        Practical throughput   Notes
──────────────────────────────────────────────────────────────────────
Meshtastic       LoRa         < 1 kbps               EU 1% duty cycle
                                                      limits TX to 36s/hr
Rimba            802.11ah     ~130 kbps usable       No duty cycle limit
                                                      (unlicensed 900MHz)

Ratio: Rimba provides approximately 130× more data throughput.

Practical example — uploading 24 hours of sensor data per leaf
  (96 readings × 300 bytes = 28,800 bytes per leaf):

  Meshtastic (< 1 kbps):  > 230 seconds per leaf
  Rimba (130 kbps):        < 2 seconds per leaf
```

---

## 2. Latency

Latency in Rimba has two distinct components that operate on very different timescales. Understanding which one dominates is key to reasoning about end-to-end delay.

### 2.1 Component 1: Leaf Wake Interval (Dominates)

```
The leaf wakes every LEAF_SLEEP_MS. Data generated during sleep
waits until the next scheduled wake.

LEAF_SLEEP_MS = 900,000ms (15 minutes, sparse-first default)

  Min latency:    0 s     (data generated exactly at wake moment)
  Max latency:    15 min  (data generated right after going to sleep)
  Average:        7.5 min

This is the dominant latency source for normal readings.
Transmission time (Section 2.2) is negligible by comparison.
```

### 2.2 Component 2: Transmission Latency (Once Leaf is Awake)

#### Connected path — cached route, K=6 relay, 3 backbone hops

```
Step                                      Duration
─────────────────────────────────────────────────────────────────
MM6108 IBSS boot + join                   30 ms  (LEAF_BOOT_MS target)
HELLO broadcast + relay response          20 ms
Peer link key check (no ECDH needed)       5 ms
DATA frame TX: leaf → relay               10 ms  (CSMA/CA + ACK)
Relay: route lookup, queue bundle          5 ms

Per backbone hop:
  TDMA slot wait (Continuous mode: none,          0–100 ms  (worst: 1 slot)
                  Scheduled mode: slot boundary)
  Frame TX + ACK                           10 ms
  Processing at each relay                  5 ms

3 backbone hops:
  Continuous mode (no slot wait):  3 × 15 ms   = 45 ms
  Scheduled mode (worst case):    3 × 115 ms  = 345 ms

Gateway: receive + IPv6 translate          50 ms
─────────────────────────────────────────────────────────────────
Total connected path, Continuous mode:   ~165 ms
Total connected path, Scheduled mode:   ~465 ms   (worst case, all slots wait)
Typical (average slot wait):      ~300 ms
```

#### Connected path — RREQ route discovery required

```
Same as above, plus route discovery:
  Expanding ring search (TTL=2):    500 ms  (RREQ_TIMEOUT)
  If no RREP → TTL=4:               500 ms
  RREP received + route installed:   50 ms

Route discovery adds:    ~600 ms – 2,000 ms
Total with discovery:    ~800 ms – 2,500 ms
```

#### Isolated relay — DTN path via mule

```
Relay stores bundle → waits for mule or gateway route

  Mule sweep interval (e.g. daily):         24 hours
  Mule travel to gateway:                   variable
  Gateway dedup + upload:                   ~1 second
  ─────────────────────────────────────────────────────
  Total DTN latency:                        24–48 hours (typical)

This is the expected operating mode in sparse deployments.
Latency is bounded by the mule schedule, not the protocol.
```

### 2.3 Alert Events: Sensor Interrupt Wake

For time-sensitive events, leaves bypass the sleep schedule entirely using a sensor GPIO interrupt (Section 9.8).

```
Alert latency (connected path):

Step                                      Duration
─────────────────────────────────────────────────────────────────
Sensor threshold breach → GPIO assert      <1 ms
ESP32-S3 wakes from deep sleep             ~5 ms
MM6108 IBSS boot + join                   ~30 ms
HELLO + peer check                        ~25 ms
ALERT DATA frame TX (no aggregation)      ~10 ms
Relay: receive ALERT, bypass TDMA slot    ~10 ms
3 backbone hops (ALERT priority):         ~45 ms  (Continuous mode)
Gateway processes:                        ~50 ms
─────────────────────────────────────────────────────────────────
Total: ~175 ms from sensor trigger to gateway receipt

Relay-to-gateway only (relay always on):
  3 hops × 15 ms + gateway 50 ms = ~95 ms
```

### 2.4 Full Latency Summary

```
Scenario                     Wake trigger      Total latency
──────────────────────────────────────────────────────────────────
Leaf → GW, connected,        RTC timer         7.5 min avg
  normal reading, cached rt  (15-min interval) + ~300 ms TX
                                               ≈ 7.5 minutes

Leaf → GW, connected,        RTC timer         7.5 min avg
  RREQ discovery             (15-min interval) + ~1.5 s discovery
                                               + ~300 ms TX
                                               ≈ 7.5 minutes

Relay → GW, connected        Always-on relay   ~150 ms
  (no leaf wake interval)                       (3 hops)

Leaf → GW, alert event,      Sensor GPIO IRQ   ~175 ms
  connected path                               (no wake interval)

Leaf → GW, isolated,         RTC timer         24–48 hours
  DTN via mule                                  (mule schedule)
```

### 2.5 Latency Tuning

The leaf wake interval is the primary latency knob:

```
LEAF_SLEEP_MS   Average latency   Leaf battery life   Use case
────────────────────────────────────────────────────────────────
900,000  (15m)  7.5 minutes       5–8 years           Environmental
300,000  (5m)   2.5 minutes       2–3 years           Agricultural
60,000   (1m)   30 seconds        ~6 months           Industrial monitor
10,000   (10s)  5 seconds         ~1 month            Near-real-time

Alert (sensor IRQ):  < 200 ms                         Any, rare events
```

For most IoT sensor applications, the two-tier pattern (15-minute normal + sensor interrupt for alerts) delivers both battery life and sub-200ms alert latency simultaneously.

### 2.6 Relay-Only Path Latency

When a relay is the data source (not a leaf), the wake interval does not apply. Relay-to-gateway latency is transmission-only:

```
Continuous mode (always-on relay):
  Route cached, 3 hops: ~150 ms
  RREQ discovery + 3 hops: ~1.5 s

Scheduled mode (TDMA relay):
  Route cached, 3 hops: ~465 ms worst case / ~300 ms average
  RREQ discovery + 3 hops: ~2 s

Geographic routing (GPS equipped, no RREQ needed):
  3 hops, quality-weighted forwarding: ~150 ms (same as Continuous mode)
  Eliminates RREQ discovery latency for known destinations.
```

---

## 3. Comparison with Meshtastic

```
                    Meshtastic (LoRa)          Rimba (802.11ah)
────────────────────────────────────────────────────────────────
Throughput          < 1 kbps                   ~130 kbps data
Per-message latency 5–30 seconds               < 200 ms (alert)
                    (flood timing)             7.5 min (normal, 15-min leaf)
                                               ~300 ms (relay, connected)
Wake interval       N/A (always on)            Configurable (10s – 15min)
Delivery guarantee  No (fire and forget)       Yes (DTN custody)
DTN path            No                         Hours to days via mule
Alert latency       5–30 s (same as normal)    < 200 ms (sensor IRQ)
```

Meshtastic's advantage: no wake interval latency because all nodes are always-on. Rimba's tradeoff: leaves sleep for battery life, creating an average 7.5-minute normal latency, but alert events bypass this entirely via sensor interrupt.

---

## 4. Summary

```
Bandwidth:
  Channel capacity:       ~150 kbps (MCS0, 1MHz)
  Data available:         ~130 kbps after protocol overhead
  Per leaf (15-min, 300B): 2.7 bps
  500 relays × 100 leaves: 33.5 kbps total — 25.7% channel utilisation
  Saturation at 15-min:   ~48,750 leaves (far beyond any deployment)

Latency:
  Normal leaf reading:    0–15 min (wake interval) + ~300 ms TX
  Alert event:            < 200 ms end-to-end (sensor IRQ bypass)
  Relay → gateway:        ~150–500 ms (no leaf wake interval)
  DTN / mule delivery:    hours to days (sparse deployment)

The dominant latency for normal sensor data is the leaf wake
interval, not transmission time. Alert events eliminate this
entirely via sensor GPIO interrupt wake (Section 9.8).
```

---

*Document version: 1.0*  
*Companion spec: rimba-protocol-spec.md Draft 0.26*
