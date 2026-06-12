# System Architecture & Technical Specification: Rimba. Power-Focused Sub-GHz Forest Mesh Network

## 1. Project Goal & Environmental Constraints

The objective is to deploy a scalable wireless sensor network of **up to 5,000 nodes** in a harsh forest environment with an **inter-node distance of roughly 1 km**.

* **Primary Constraint:** Absolute priority is maximizing battery life to ensure long-term, unattended field operation under limited solar conditions (canopy coverage).
* **Network Structure:** A hybrid architecture. The network functions natively as a decentralized, gateway-less Delay-Tolerant Network (DTN). However, it must seamlessly support both **intermittent data harvesting via mobile Data Mules (humans/drones)** and **fixed Border Gateways** wherever internet backhaul (LTE/Ethernet) is available.
* **The Anti-802.11s Pivot:** Standard 802.11s routing requires continuous carrier sensing and background peering traffic, keeping the radio in a constant `Rx` listen state drawing ~37 mA. This protocol abandons standard 802.11s in favor of a custom, deterministic Layer 2 framework utilizing connectionless frame injection to force the transceiver into deep sleep.

---

## 2. System Architecture & Topology

The topology is a **Hierarchical Clustered Mesh Architecture with Spatial Reuse**. Because nodes are physically separated by 1 km, they reside in small, distinct radio collision domains. This physical isolation allows thousands of nodes to reuse the same time slots simultaneously across the forest without causing global RF contention.

```
       [Cloud Server / Dashboard]
                   ▲
                   │ (IPv6 via Backhaul)
        ┌──────────┴──────────┐
        │ Fixed Border Router │◀──────────────────────────────┐
        └─────────────────────┘                               │
                   ▲                                          │
                   │ (Raw HaLow)                              │
        ┌──────────┴──────────┐                               │
        │  Relay Node (Hub)   │◀──────────────┐               │
        └─────────────────────┘               │               │
         ▲                   ▲                │               │
         │ (TWT Slot)        │ (TWT Slot)     │ (HaLow Dump)  │ (HaLow Dump)
┌────────┴────────┐ ┌────────┴────────┐       ▼               ▼
│ Leaf Sensor 01  │ │ Leaf Sensor 02  │   ( ( 📡 ) )      ( ( 📡 ) )
└─────────────────┘ └─────────────────┘    Node X          Node Y
                                              ▲               ▲
                                              │ (BLE Wake)    │ (Preamble Wake)
                                         [🚁 Drone / Human Data Mule]

```

### Node Role Classifications

1. **Leaf Nodes (Edge Sensors):** Ultra-low-power sensing devices. They never forward multi-hop traffic. They wake up exclusively to capture data, execute local schedule contracts with their parent, transmit their payload, and sleep.
2. **Relay Nodes (Cluster Hubs):** Equipped with expanded power profiles (larger solar panels/batteries). They manage local cluster schedules and act as regional data aggregators, forwarding telemetry hop-by-hop toward gateways or key clearings where a Data Mule is expected.
3. **Fixed Border Gateways (Root Sinks):** Linux-based SoM (System-on-Module) boundary routers. They act as stateless translation engines, turning compressed over-the-air raw layer 2 frames into standard IPv6 packets for cellular (LTE) upload.
4. **Data Mules (Dynamic Sinks):** Fast-moving collection platforms (Drones or Forest Rangers) that pass through the area asynchronously, executing a hardware-triggered "Wake and Flush" routine.

---

## 3. Protocol Timeline: Hybrid MAC Specification

The protocol structures time into repeating windows called **Epochs** (variable, extended duration e.g., 5 to 30 minutes to maximize battery life). Each Epoch contains a synchronized coordination period, followed by collision-free reserved slots, and a contention overflow valve.

```
├────────────────────────── Total Epoch Duration (e.g., 5–30 Min) ──────────────────────────┤
┌──────────────────────────────┬──────────────────────────────────────────────┬──────────────┐
│           CCW (5s)           │            TDMA / TWT Slots (50s)            │   SCW (5s)   │
└──────────────────────────────┴──────────────────────────────────────────────┴──────────────┘
 ▲                              ▲                                              ▲
 ├─ Synchronized Discovery      ├─ Reserved P2P Connections                    ├─ Slotted CSMA Fallback
 ├─ Neighbor Beacon Exchange    ├─ Non-overlapping Data Transmission           ├─ Target for Data Surges
 └─ Clock Drift Corrections     └─ Deep Sleep for Unassigned Channels          └─ Relays Keep Radio Awake

```

### A. Common Control Window (CCW)

* **Duration:** Typically 5 seconds at the start of the Epoch.
* **Mechanism:** All nearby cluster nodes wake up simultaneously. Nodes exchange short synchronization and discovery beacons using carrier sensing (CSMA/CA).
* **Time Synchronization Fallback:** Time-slots are aligned via GPS Pulse-Per-Second (PPS) hardware interrupts where available. In GPS-denied environments (heavy canopy), the cluster utilizes a **Distributed Clock Discipline Algorithm (PTP-style)** during the CCW: nodes monitor neighbor beacons, calculate clock offsets, and track timing deviations relative to the local lowest-MAC address node to prevent clock drift.

### B. Target Wake Time (TWT) / Soft-TDMA Slots

* **Duration:** The primary block of the Epoch. Divided into discrete time-slices (e.g., 200 ms per node payload + guard band).
* **Mechanism:** Point-to-point transmission contracts. Outside the CCW, nodes sit in a microscopic microamp sleep state. A node's main processor and sub-GHz radio only boot up exactly when its negotiated TWT slot window arrives to transmit data to its target parent, eliminating idle-listening drain.
* **Adaptive Slot Allocation Engine:** Nodes evaluate their local radio environment by logging a rolling **Density Index ($D$)** based on unique neighbor beacon counts tracked during the CCW:
* *Aggressive Mode ($D < 5$, Sparse):* The node avoids heavy collision calculations, claims the first open slot in its neighbors' broadcasted maps, and shrinks its CCW time to return to deep sleep instantly.
* *Conservative Mode ($D \ge 5$, Dense):* The node implements a randomized backoff window across available slots to combat the Birthday Paradox effect and listens for multiple Epochs before claiming a slot to prevent collisions. Hysteresis rules check that a low density persists for at least 10 Epochs before down-shifting to Aggressive mode.



### C. Shared Contention Window (SCW)

* **Duration:** Final short window of the Epoch (e.g., 5 seconds).
* **Mechanism:** Utilizes slotted CSMA/CA directly on top of the TDMA structure. If a cluster's TDMA slots are entirely saturated, or a node undergoes an emergency data surge (e.g., sensor fire alarm), the device holds its overflow data in a flash queue, skips the TDMA phase, boots up during the SCW, and contends for airtime.
* **Asymmetric Power Allocation:** To protect battery life, battery-operated Leaf nodes completely power-down during the SCW if they have no overflow data to send. Only high-capacity Relay Nodes are required to keep their receivers active to capture incoming SCW bursts.

---

## 4. Delay-Tolerant Networking & Storage Strategy

Because there may be periods with no available fixed gateways, the network handles signal dropouts as a standard operational state via **Store-Carry-Forward (Mule-DTN)** mechanics.

* **Local Storage Array:** Telemetry data is handled using atomic, copy-on-write semantics managed by **LittleFS** deployed on external SPI Flash. This guarantees power-fail safety if a node experiences complete solar/battery brownout mid-write.
* **Staging Logic:** Sensor logs are bundled into binary chunks (using space-efficient serialization like Protocol Buffers or CBOR) and written to the `/bundles/` directory using an incremental naming scheme. Firmware updates are written sequentially to a singular `/fota/update.bin` file utilizing `fseek()` block offsets to minimize metadata wear-leveling overhead.
* **Custody Transfer:** Data is pushed down a localized routing gradient (the "Mule Metric"), migrating from the outer boundaries toward clearings, access paths, or active fixed gateways. A node only flushes its local LittleFS cache file once it receives a cryptographic link-layer ACK indicating successful custody transfer to the next hop or data sink.

---

## 5. Mobile Data Mule Intercept Protocol

To harvest data via a drone flying over at speeds up to 40 km/h, the network uses opportunistic discovery loops to bridge the tight line-of-sight communication window without forcing edge sensors to keep their radios constantly awake:

```
Drone Flight Path ──────────────────────────────🚁 (Streams Wake Signature)
                                               /  \
                                              /    \  (RF Coverage Cone)
                                             /      \
                                            ▼        ▼
                      ┌──────────────────────────────────────────────┐
                      │             nRF54L Node State                │
                      └──────────────────────┬───────────────────────┘
                                             │
                                             ▼
                               ┌───────────────────────────┐
                               │   DEEP POWER-DOWN SLEEP   │ (System drawing ~2 uA)
                               │  Co-processor Monitors    │
                               └─────────────┬─────────────┘
                                             │
                       ┌─────────────────────┴─────────────────────┐
                       ▼ (Method 1: BLE Intercept)                 ▼ (Method 2: HaLow Sniff)
         ┌───────────────────────────┐               ┌───────────────────────────┐
         │ Low Duty-Cycle BLE Active │               │  Sub-ms RF Preamble Sniff │
         │ Detects Drone Auth Token  │               │  Matches Long Carrier Wave│
         └─────────────┬─────────────┘               └─────────────┬─────────────┘
                       │                                           │
                       └─────────────────────┬─────────────────────┘
                                             │ (Hardware Interrupt Trigger)
                                             ▼
                               ┌───────────────────────────┐
                               │     ARM CORTEX-M33 BOOT   │
                               │   Bypass Soft-TDMA Cycles │
                               └─────────────┬─────────────┘
                                             │
                                             ▼
                               ┌───────────────────────────┐
                               │    BULK FLASH EXPULSION   │
                               │  Max MCS Rate via CSMA    │
                               └───────────────────────────┘

```

* **Method 1 (Dual-Radio BLE Wake):** The nRF54L’s ultra-low-power network co-processor operates a background, highly efficient Bluetooth LE scanning profile. The incoming drone continuously broadcasts a cryptographically signed BLE wake-up token. Upon intercept, an internal hardware interrupt wakes the primary ARM application processor and initializes the heavy sub-GHz MM6108 radio.
* **Method 2 (HaLow Low-Power Preamble Sensing):** The sub-GHz MM6108 radio performs ultra-fast, sub-millisecond RF energy sniffing loops every few seconds. The drone streams an extended Layer 2 discovery preamble. Once an energy signature match occurs, the node fully wakes up.
* **Bulk Dump Phase:** Once the drone/mule connection is established, the node completely suspends normal Soft-TDMA epoch constraints, transitions into an unslotted bulk transfer state, and dumps its compiled LittleFS sensor blocks at maximum hardware Modulation and Coding Scheme (MCS) data rates using CSMA/CA before the drone exits the line-of-sight cone.

---

## 6. Stateless IPv6 Boundary Translation

To make internet cloud ingestion seamless without loading a heavy IP stack onto memory-constrained, battery-restricted edge devices, the protocol implements stateless compression mirroring the principles of **SCHC (Static Context Header Compression - RFC 8724)**.

* **The Over-The-Air Frame Layout:** Over-the-air packets contain no 40-byte IPv6 headers. The packet simply packs short, highly compressed Layer 2 values into raw 802.11ah injected frames:
```
+───────────────────+─────────────────+───────────────────+───────────────────────+
|    Epoch ID (1B)  | Source ID (2B)  | Destination (2B)  | Compressed Header (1B)|
+───────────────────+─────────────────+───────────────────+───────────────────────+
| Replay Control    | Source Local    | Target Relay or   | Static Context Rule / |
| & Time Sync Index | Hardware ID     | Anycast Gateway   | Port Definition Identifier
+───────────────────+─────────────────+───────────────────+───────────────────────+

```


* **Stateless Expansion At the Gateway:** Gateways or Base Stations are assigned a global `/64` IPv6 prefix by the backhaul network provider (e.g., `2001:db8:forest:mesh::/64`). When a fixed boundary gateway intercept occurs, it runs a deterministic expansion algorithm to generate valid IPv6 traffic without tracking live runtime socket sessions:
* **Network Prefix:** The first 64 bits are statically filled using the gateway's hardcoded network context prefix.
* **Interface Identifier (IID):** The trailing 64 bits are synthesized deterministically by mapping the 16-bit or 32-bit Over-the-Air `Source ID` into the lower bits of the IPv6 address array.
* **Result:** Cloud servers receive standardized, globally routable IPv6 packets directly from each deep-forest sensor node, enabling targeted diagnostics and anycast network failovers without requiring DHCPv6 handshakes or active routing tables at the edge.



---

## 7. Security Architecture

Because devices are placed in unattended wilderness environments, security is split into an asymmetric identity phase and a hardware-accelerated session phase to ensure forward secrecy without straining power resources.

1. **Hardware-Rooted Identity (Join-Time):** Every node contains a unique **ECC Private Key ($K_{priv}$)** injected into a secure storage container (such as the nRF54L’s isolated TrustZone vault) alongside an immutable corporate **Root of Trust Public Key**. When establishing a connection contract during the CCW, nodes generate an ECDSA digital signature over a random challenge nonce to prove their identity offline without a verification server.
2. **Symmetric Session Isolation (Data-Time):** Once a link identity is cryptographically verified, the two nodes execute an **Ephemeral ECDH (Elliptic Curve Diffie-Hellman)** handshake to establish a localized Symmetric Session Key. This key is stored in volatile RAM and is used to wrap subsequent data transfers using hardware-accelerated **AES-GCM (Galois/Counter Mode)**, which yields authenticated, encrypted payloads at near-zero CPU cycle costs.
3. **Physical Protection:** Nodes run with mandatory **Secure Boot** and **Flash Encryption** enabled. If an edge device is stolen from the field, its external SPI flash data is unreadable, and any attempts to inject modified malicious code will trigger a boot failure. A stolen node's identity is stripped from the network via a decentralized **Distributed Revocation List (DRL)** bundle that propagates throughout the forest using an offline gossip protocol.

---

## INSTRUCTION FOR THE ANALYZING AI:

Review the multi-tier protocol detailed above. Focus your evaluation on:

1. The edge state transitions required to bridge the gap between deterministic TWT deep sleep slots and sudden, high-speed Data Mule discovery sequences.
2. The concurrency management of the Adaptive Slot Engine when handling local temporal synchronization Fallbacks in the absence of a global GPS clock source.
3. Identifying potential edge-case race conditions within the LittleFS bundle staging architecture when a custody transfer is abruptly truncated during a bulk dump phase.