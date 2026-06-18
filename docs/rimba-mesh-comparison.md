# Rimba Protocol — Mesh Communication Overview and Comparisons

---

## 1. How Rimba Mesh Communication Works

Rimba has three separate layers doing three distinct jobs. This is why it does not fit neatly into "TDMA" or "CSMA/CA" as a single label — each layer uses a different mechanism.

```
Layer               Mechanism              Job
──────────────────────────────────────────────────────────────
MAC / channel       CSMA/CA                When a node transmits,
access              (802.11ah standard)    how does it share the air?

Sleep               Local TDMA             When are nodes awake
coordination        (Rimba custom)         to receive?

Routing             OGM proactive /        How does a bundle find
                    RREQ/RREP reactive /   its way to the destination?
                    Geographic greedy
```

CSMA/CA handles the radio. Local TDMA handles the sleep schedule. Routing handles the path. These are independent concerns layered on top of each other.

### 1.1 A Bundle's Journey

```
1. Leaf wakes (scheduled by 1ppm RTC)

2. HELLO broadcast
   → relay hears it, opens its receive window

3. Leaf sends DATA frame
   → CSMA/CA: listen, random backoff if channel busy, transmit
   → 802.11ah IBSS unicast to relay MAC
   → Hop-by-hop AES-CCM encryption (Rimba peer link session key)

4. Relay receives bundle → DTN engine
   → Does relay have a route to destination?
        Yes (OGM/RREQ table) → forward next hop via CSMA/CA
        Yes (geographic)     → forward to quality-weighted closest peer
        No                   → store in bundle store, wait for route

5. Multi-hop: each relay repeats the same receive → forward or buffer

6. Bundle reaches gateway
   → BPSec decryption (source-to-gateway key)
   → IPv6 translation
   → Internet delivery
```

### 1.2 Channel Access: CSMA/CA

At the radio layer, Rimba is pure 802.11ah CSMA/CA — the same collision avoidance mechanism used in all Wi-Fi networks. When a node wants to transmit:

```
1. Sense the channel: is it idle?
2. If idle: wait a random backoff period (DIFS + CW slots)
3. Transmit. Await ACK.
4. If no ACK (collision or loss): double the contention window,
   retry. Up to 7 retries before dropping the frame.
```

Rimba adds gossip-based probabilistic flooding on top of this to reduce how many nodes simultaneously try to re-broadcast routing messages.

### 1.3 Sleep Coordination: Local TDMA

The local TDMA in Rimba is **not** for channel access control — it is purely for coordinating when nodes are awake. Each relay advertises a local superframe (typically 2 seconds, 20 slots of 100ms) in its HELLO. Peers learn which slots each neighbour is awake in, and schedule their own transmissions to coincide.

```
Example superframe (relay with 6 backbone peers):

Slot:   0    1    2    3    4    5    6    7    8    9  ...  19
      ┌────┬────┬────┬────┬────┬────┬────┬────┬────┬────┬────┐
      │ TX │ R1 │ R2 │ R3 │ R4 │ R5 │ R6 │MGMT│MGMT│MGMT│ zz │
      └────┴────┴────┴────┴────┴────┴────┴────┴────┴────┴────┘
       own  ←── peer receive slots ──→   RREQ  zz = sleep
       TX                                OGM

Relay duty cycle: 10/20 = 50%
Remaining 50%: radio off, MCU in light sleep
```

Leaf nodes operate on a much sparser schedule — waking every 15 minutes by default, advertising their wake window in HELLO, and sleeping the rest of the time.

### 1.4 Routing: Three Modes

```
Mode              When used               Mechanism
─────────────────────────────────────────────────────────────
OGM proactive     < 100 nodes             Periodic originator
                                          messages flooded,
                                          forwarding table built

RREQ/RREP         100–1000 nodes          On-demand route
reactive          (primary mode)          discovery via flooding
                                          + gossip control

Geographic        > 200 nodes with GPS    Per-hop forwarding to
greedy            relay hardware          quality-weighted
                                          geographically closest
                                          peer — no flooding
```

### 1.5 Disruption Tolerance: BPv7 DTN

Every data message is wrapped in a BPv7 bundle. Bundles are stored at each relay until the next hop is reachable. A mule (drone, vehicle, or person) can physically carry bundles across gaps that radio cannot bridge. No data is ever simply discarded as "undeliverable" — it waits until a path appears.

---

## 2. Comparison: Strict TDMA

**Reference protocols**: IEEE 802.15.4e TSCH (6TiSCH), LoRaWAN Class B

Strict TDMA divides time into fixed slots and assigns specific slots to specific nodes. Only the designated node may transmit in its slot — no contention.

```
┌────┬────┬────┬────┬────┬────┐
│ A  │ B  │ C  │ A  │ B  │ C  │  deterministic, zero collision
└────┴────┴────┴────┴────┴────┘
  0ms  10ms 20ms 30ms 40ms 50ms
```

|                        | Strict TDMA             | Rimba                          |
|---|---|---|
| Collisions             | Zero — deterministic    | Possible (CSMA/CA), recovered via retry |
| Delivery latency       | Predictable, bounded by slot schedule | Variable, DTN-tolerant |
| Time sync required     | Microseconds (GPS or PTP mandatory) | Milliseconds (1 ppm RTC sufficient) |
| Topology change        | Slow — requires coordinator to reschedule | Fast — OGM/RREQ adapts dynamically |
| Dense deployment       | Excellent (zero collision) | Good with gossip/adaptive power (degrades above ~200 nodes in one cell) |
| Sleep efficiency       | Excellent (precise global slots) | Good (local TDMA for wakeup, CSMA/CA when awake) |
| Central coordinator    | Required                | Not required |
| DTN / disruption       | Not built in            | First-class (BPv7) |
| MCU implementation     | Moderate                | Moderate |
| Complexity             | Very high (slot manager, global sync) | Medium (distributed, no coordinator) |

**Key distinction**: Rimba's local TDMA controls only *when nodes are awake*. Once awake, they use CSMA/CA for channel access — the same as Wi-Fi. Strict TDMA eliminates contention entirely by assigning transmission slots globally.

**When strict TDMA wins**: Industrial control, factory automation, or safety-critical applications where deterministic latency and zero collision are non-negotiable.

**When Rimba wins**: Sensor mesh deployments where nodes are battery-powered, topology is dynamic, and occasional retransmission is acceptable.

---

## 3. Comparison: Pure CSMA/CA Mesh (802.11s)

**Reference protocol**: IEEE 802.11s (Wi-Fi Mesh), as used in home mesh routers and OpenWrt deployments.

802.11s operates at the standard 802.11 MAC layer with all nodes as equal Mesh Points. HWMP (Hybrid Wireless Mesh Protocol) handles path selection. All nodes are typically always-on and AC-powered.

|                        | 802.11s                 | Rimba                          |
|---|---|---|
| Radio layer            | 2.4 GHz or 5 GHz        | Sub-1 GHz (802.11ah)           |
| Range                  | 50–150 m                | 500 m – 1 km                  |
| Channel access         | CSMA/CA                 | CSMA/CA (identical mechanism)  |
| Path selection         | HWMP (standard)         | OGM / RREQ / Geographic (custom) |
| Sleep / battery        | Not designed for it     | Core design goal               |
| Node power             | 5–15 W (Linux router)   | ~8 mA relay, ~20 µA leaf      |
| DTN / disruption       | No                      | Yes (BPv7, mule custody)       |
| MCU support            | No (requires Linux)     | Yes (ESP32-S3 / nRF54)        |
| Routing overhead       | HWMP proactive + reactive | OGM + RREQ/RREP + geographic   |
| Security               | WPA3 link-layer         | BPSec E2E + per-peer ECDH     |
| Node count             | Hundreds                | 500–1000 relays + thousands of leaves |
| Maturity               | Very mature, standard   | Early stage                    |

**Key distinction**: 802.11s is designed for always-on infrastructure — home mesh routers, enterprise wireless backhaul. It makes no concessions for battery power. Rimba targets exactly the opposite: years of battery life with intermittent connectivity. A node running 802.11s firmware would drain a sensor battery in hours.

**When 802.11s wins**: Indoor enterprise environments, home mesh networking, and any deployment where AC power is available everywhere and low latency matters more than battery life.

**When Rimba wins**: Outdoor sensor deployments, remote monitoring, and any scenario where nodes are battery-powered and connectivity to a gateway is not guaranteed.

---

## 4. Comparison: Meshtastic

Meshtastic is the most directly comparable protocol — it is also designed for mesh networking over IoT-class hardware in remote or off-grid environments.

### 4.1 How Meshtastic Works

```
Radio: LoRa (Chirp Spread Spectrum)
  - Sub-1 GHz (868 MHz EU, 915 MHz US)
  - Very long range, very low bandwidth
  - Sleep current < 1 µA (excellent battery life)

Channel access: ALOHA-style
  - Listen briefly, then transmit
  - No collision avoidance (unlike CSMA/CA)
  - Collision recovery: sender re-transmits after timeout

Routing: Pure controlled flooding
  - Every message re-broadcast by every node
  - Probabilistic re-broadcast (each node re-sends with
    probability based on hop count)
  - No routing tables
  - No path selection

Delivery: Fire-and-forget
  - If the flood reaches destination: delivered
  - If not: message is lost
  - No DTN, no store-and-forward, no retry protocol

Security: AES-128 per-channel shared key
  - All nodes on a channel share one symmetric key
  - No per-node key derivation
  - No end-to-end per-source-destination encryption
```

### 4.2 Side-by-Side

|                        | Meshtastic              | Rimba                          |
|---|---|---|
| Radio technology       | LoRa (CSS)              | 802.11ah (OFDM)               |
| Frequency              | 868 / 915 MHz           | 850–950 MHz                   |
| Range                  | 3–20 km open field      | 500 m – 1 km open field       |
| Raw bandwidth          | 250 bps – 11 kbps       | ~150 kbps usable              |
| Practical throughput   | < 1 kbps                | ~100+ kbps                    |
| EU duty cycle limit    | 1% (36 s/hour TX max)   | None (802.11ah unlicensed)    |
| Channel access         | ALOHA-style             | CSMA/CA (802.11)              |
| Routing                | Pure flooding           | OGM / RREQ / Geographic       |
| Delivery guarantee     | No (fire-and-forget)    | Yes (BPv7 DTN custody)        |
| Store-and-forward      | No                      | Yes — bundles wait for route  |
| Mule / courier         | No                      | Yes (custody transfer)        |
| Security               | AES-128 shared channel  | BPSec COSE AEAD, per-pair ECDH|
| Encryption scope       | Channel (symmetric)     | E2E per source-destination     |
| Leaf sleep current     | < 1 µA (LoRa)           | ~15–20 µA average             |
| Leaf battery life      | 5–15 years (small cell) | 3–8 years                     |
| Relay power            | ~5 mA                   | ~8 mA                         |
| Max practical nodes    | ~50–200                 | 500–1000 relays (more with GPS) |
| Sensor data support    | Basic telemetry plugins | BPv7 bundles, CBOR, aggregation|
| Mobile app             | Yes (mature, iOS/Android)| Not yet                       |
| Maturity               | Mature, wide community  | Early stage (this spec)       |
| Hardware cost          | Low (~$15–30 complete)  | Moderate (~$30–60 complete)   |

### 4.3 Core Architecture Contrast

The fundamental difference is not radio or bandwidth — it is the delivery model.

```
Meshtastic:
  "Send a message and flood it everywhere.
   If enough nodes re-broadcast, it probably arrives.
   If not, resend manually."

  Data loss is expected and accepted.
  The protocol makes no commitment to delivery.

Rimba:
  "Every bundle is stored until confirmed delivered.
   Routes are discovered intelligently, not flooded.
   A mule can physically carry data across coverage gaps.
   No bundle is ever simply lost — it waits for a path."

  Delivery is the protocol's responsibility, not the operator's.
```

### 4.4 Security Architecture Contrast

```
Meshtastic:
  One AES-128 key per channel.
  Everyone on the channel can decrypt everything.
  A compromised node exposes all traffic on that channel.

Rimba:
  Hop-by-hop: per-pair ECDH session keys.
    Each link encrypted independently.
    A compromised relay exposes only its own hop.

  End-to-end: BPSec COSE AEAD with per-source-destination keys.
    Derived from a deployment root secret + src/dst IDs.
    A compromised relay cannot read bundle payloads.
    A mule carrying bundles sees only encrypted blobs.
```

### 4.5 When Each Wins

```
Scenario                                     Better choice
─────────────────────────────────────────────────────────────
Hikers communicating over 10+ km             Meshtastic
Disaster relief text messaging               Meshtastic
Emergency comms with no infrastructure       Meshtastic
Very sparse nodes (> 5 km apart)             Meshtastic
Low-cost, fast deployment, community tools   Meshtastic

Dense sensor network (agriculture, city)     Rimba
High-volume sensor data (> 1 kbps/node)      Rimba
Guaranteed delivery required                 Rimba
Isolated nodes needing mule/drone courier    Rimba
500+ node mesh                               Rimba
E2E security per sensor node                 Rimba
Sensor data aggregation and routing          Rimba
```

Meshtastic and Rimba occupy different points in the design space. Meshtastic is LoRa-native and optimised for sparse long-range human communication with a focus on simplicity and community tooling. Rimba is HaLow-native and optimised for dense-to-moderate sensor networks with guaranteed delivery, intelligent routing, and strong per-node security.

---

## 5. Summary

```
                  Strict TDMA    CSMA/CA Mesh   Meshtastic     Rimba
                  (TSCH)         (802.11s)       (LoRa)         (HaLow IBSS)
────────────────────────────────────────────────────────────────────────────
Range             Medium         Short           Very long      Long
                  (depends on    (50–150m)       (3–20km)       (500m–1km)
                  radio)

Bandwidth         Medium         High            Very low       High
                                 (100+ Mbps)     (<1 kbps)      (~150 kbps)

Battery life      Good           Poor            Excellent      Good
                  (scheduled     (always-on)     (<1µA sleep)   (~20µA leaf)
                  slots)

Delivery          Deterministic  Best-effort     Best-effort    Guaranteed
guarantee         (slot timing)  (retry at MAC)  (flood or      (DTN custody)
                                                 lose)

DTN support       No             No              No             Yes

Collisions        Zero           Yes             Yes (ALOHA)    Yes (CSMA/CA)

Central coord     Required       No              No             No

Sleep mgmt        Excellent      Poor            Excellent      Good

Scalability       Limited by     Hundreds        50–200         500–1000+
                  slot count     nodes           nodes          relays

MCU support       Yes            No (Linux)      Yes            Yes

Security          Link-layer     WPA3 link       Shared AES-128 BPSec E2E
                                                                per-pair ECDH

Maturity          Mature         Very mature     Mature         Early
                  (niche)        (enterprise)    (community)    (this spec)
```

Rimba sits between CSMA/CA mesh and Meshtastic in the design space: longer range and lower power than 802.11s, higher bandwidth and stronger delivery guarantees than Meshtastic. Its core value proposition is **guaranteed delivery for sensor data across intermittently-connected, battery-powered meshes** — a use case none of the three comparison protocols addresses directly.
