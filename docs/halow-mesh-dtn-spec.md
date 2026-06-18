# HaLow Mesh DTN Protocol (HMDP)
## Technical Specification — Draft 0.2

> **⚠️ SUPERSEDED — historical reference only.**
> This is the original dual-radio HMDP draft. It has been superseded by the
> **Rimba Protocol Specification** (`rimba-protocol-spec.md`, Draft 0.26+),
> which uses a single-radio IBSS-only architecture. Key differences: Rimba is
> single MM6108 per node (no AP+STA dual-radio), names sleep modes
> Continuous/Scheduled, defines mule as a relay superset, and adds proxy RREP
> leaf routing. Do not use this document for implementation — refer to
> rimba-protocol-spec.md instead.

---

## Table of Contents

1. [Introduction](#1-introduction)
2. [Terminology](#2-terminology)
3. [Network Architecture](#3-network-architecture)
4. [Protocol Stack](#4-protocol-stack)
5. [Physical and Link Layer](#5-physical-and-link-layer)
6. [Mesh Frame Format](#6-mesh-frame-format)
7. [Neighbor Discovery and Peer Links](#7-neighbor-discovery-and-peer-links)
8. [Mesh Routing Protocol](#8-mesh-routing-protocol)
9. [DTN Bundle Layer](#9-dtn-bundle-layer)
10. [Security](#10-security)
11. [Gateway and IPv6 Border Translation](#11-gateway-and-ipv6-border-translation)
12. [Power Management](#12-power-management)
13. [Dense and Sparse Deployment](#13-dense-and-sparse-deployment)
14. [Open Issues](#14-open-issues)

---

## 1. Introduction

### 1.1 Purpose

The HaLow Mesh DTN Protocol (HMDP) defines a low-power wireless mesh networking protocol targeting battery- and solar-powered IoT sensor deployments. It is designed to operate in environments where connectivity to infrastructure is intermittent or absent and where nodes may be separated by distances requiring multi-hop forwarding.

### 1.2 Design Goals

- **MCU-only**: All roles run on ESP32 or nRF54 class microcontrollers. No Linux required.
- **True mesh**: Any node can communicate with any other node without requiring a gateway or fixed root.
- **DTN-native**: Disruption-tolerant networking is a first-class concern, not an afterthought.
- **Low power**: Leaf nodes target multi-year battery life. Relay nodes target solar-sustainable operation.
- **Secure**: All payloads are end-to-end encrypted. All links are hop-by-hop secured.
- **Scalable**: Supports 500–1000 relay nodes with up to 100 leaf nodes per relay.
- **Simple gateway translation**: Internet translation occurs only at the gateway border.

### 1.3 Non-Goals

- HMDP does not implement 802.11s or any other standard mesh protocol.
- HMDP does not run a full IPv6 stack inside the mesh.
- HMDP does not target real-time or low-latency applications. Latency from seconds to hours is acceptable.

### 1.4 Hardware Targets

| Component | Specification |
|---|---|
| Host MCU | Espressif ESP32-S3 or Nordic nRF54 series |
| Radio (relay backbone) | Morse Micro MM6108, SDIO interface |
| Radio (leaf attachment) | Morse Micro MM6108, SPI interface |
| Radio firmware | mm6108.mbin (closed, provided by Morse Micro) |
| Host MAC/driver | morselib (open source, Apache 2.0, mm-iot-sdk) |
| Frequency | 850–950 MHz (region-specific, license-exempt) |
| PHY standard | IEEE 802.11ah (Wi-Fi HaLow) |

**Note**: Relay nodes require two MM6108 radios. See Section 3.3.

---

## 2. Terminology

| Term | Definition |
|---|---|
| **Node** | Any device participating in an HMDP network |
| **Relay cluster** | One relay node plus its associated leaf nodes |
| **Relay backbone** | The IBSS mesh network formed by all relay nodes |
| **Leaf attachment** | The AP/STA infrastructure link between a relay and its leaves |
| **Peer link** | A secured, bidirectional L2 link between two relay backbone nodes |
| **OGM** | Originator Message — used in small deployments (< 100 relays) only |
| **RREQ / RREP / RERR** | Route Request / Route Reply / Route Error — reactive routing messages |
| **Bundle** | A DTN protocol data unit as defined in BPv7 (RFC 9171) |
| **Custody** | Responsibility for delivering a bundle accepted by a node |
| **EID** | Endpoint Identifier — the bundle-layer address of a node |
| **IBSS** | Independent Basic Service Set — 802.11 adhoc mode, used for relay backbone |
| **TWT** | Target Wake Time — 802.11ah AP/STA power save feature, used for leaf attachment |
| **RAW** | Restricted Access Window — 802.11ah contention control, used for leaf attachment |
| **BCF** | Board Configuration File — MM6108 module RF calibration data |

---

## 3. Network Architecture

### 3.1 Two-Tier Topology

HMDP uses a two-tier architecture that separates relay backbone communication from leaf attachment. Each tier uses the most appropriate 802.11ah operating mode.

```
═══════════════════════════════════════════════════════════════
  TIER 1: RELAY BACKBONE MESH  (802.11ah IBSS)
═══════════════════════════════════════════════════════════════

        [GW]──────[R1]──────[R2]──────[R3]
          \      / \       / \       /
          [R4]──[R5]──────[R6]──────[R7]──[R8]
                |                    |
               ...                  ...
           500 relay nodes total, any-to-any mesh,
           reactive RREQ/RREP routing, no root

═══════════════════════════════════════════════════════════════
  TIER 2: LEAF CLUSTERS  (802.11ah AP+STA with TWT)
═══════════════════════════════════════════════════════════════

   Each relay runs an AP for its leaf cluster:

         [Rx as AP]
        /  | | | | \
       L  L L L L  L   ...up to 100 leaf STAs per relay
                        TWT sleep, RAW contention control,
                        standard 802.11ah association
```

**Design rationale**: 50,000 leaf nodes cannot share a single IBSS channel — HELLO beacons alone would exceed 400 kbps at 50,000 nodes, which exceeds 802.11ah 1MHz channel capacity (~150 kbps usable). The two-tier split confines leaf traffic to local clusters and reserves the backbone channel for inter-relay routing only.

### 3.2 Node Roles

#### 3.2.1 Gateway

- Provides internet or intranet connectivity via cellular modem or wired Ethernet.
- Runs continuously on mains power.
- Participates in the relay backbone as an IBSS node.
- Acts as IPv6 border translator: bundles addressed to internet destinations are unpacked and forwarded as IPv6.
- Serves as the authoritative time source (NTP-synced) for the mesh.
- Does not need to run a leaf AP (optional).
- May be absent; the mesh operates correctly without it.

#### 3.2.2 Relay

- Forms the backbone mesh via Tier 1 IBSS links.
- Serves up to ~100 leaf nodes via a Tier 2 AP interface.
- Stores and forwards bundles between tiers and across the backbone.
- Powered by solar panel with battery backup.
- Runs two MM6108 radios simultaneously (see Section 3.3).

#### 3.2.3 Leaf

- Sources sensor data only. Does not participate in Tier 1 backbone mesh.
- Operates in deep sleep between measurement cycles using TWT.
- Powered by primary battery or small solar cell.
- Associates as a standard 802.11ah STA to its local relay AP.
- Single MM6108, STA mode only.

#### 3.2.4 Mule

- A mobile node (drone, vehicle, or person) that sweeps the mesh collecting buffered bundles.
- Participates in Tier 1 as an IBSS node when in range of relays.
- Accepts custody of bundles via the DTN custody protocol (Section 9.5).
- Uploads bundles when reaching internet connectivity.
- Identified by a MULE capability flag in HELLO frames.

### 3.3 Relay Hardware Architecture

A relay node requires two MM6108 radios to simultaneously serve both tiers:

```
                    ┌──────────────────────────┐
                    │      ESP32-S3 MCU         │
                    │                           │
  Tier 1 IBSS  ─── │ SDIO ──── MM6108 #1       │
  (backbone)        │           (IBSS mode)     │
                    │                           │
  Tier 2 AP    ─── │ SPI  ──── MM6108 #2        │
  (leaf cluster)    │           (AP mode)       │
                    │                           │
                    │  DTN bundle engine        │
                    │  Mesh routing (RREQ/RREP) │
                    │  Peer link manager        │
                    └──────────────────────────┘
```

- **MM6108 #1** (SDIO): Runs IBSS mode for relay-to-relay backbone mesh. Uses `MORSE_CMD_INTERFACE_TYPE_ADHOC` (morselib, confirmed working in community as of May 2026).
- **MM6108 #2** (SPI): Runs AP mode for leaf attachment. TWT and RAW are managed by this radio.
- **Bundle engine**: Software layer that receives bundles from both interfaces and routes them appropriately.

**Dual-radio is the defined relay hardware for this version.** Single-radio concurrent IBSS+AP is out of scope and listed as a future discovery item (see Section 14, Future Investigations).

### 3.4 Node Addressing

Each node has a globally unique 48-bit Node ID derived from the MM6108 MAC address (EUI-48).

```
Node ID:    6 bytes  (EUI-48, e.g. 0x001122334455)
Bundle EID: ipn:<node_id_decimal>.<service_number>
            e.g.  ipn:18838586676.1
```

Reserved Node ID `0xFFFFFFFFFFFF` denotes mesh broadcast.

For bundle routing, leaf nodes are addressed individually, but inter-relay routing uses relay-level destinations. When a bundle is destined for a leaf, it is routed to that leaf's relay, which delivers it locally via the AP interface.

---

## 4. Protocol Stack

### 4.1 Tier 1 — Relay Backbone

```
┌──────────────────────────────────────────────────────────────┐
│  Application        CBOR-encoded sensor payload               │
├──────────────────────────────────────────────────────────────┤
│  E2E Security       BPSec (RFC 9172) + COSE AEAD              │
│                     ChaCha20-Poly1305 or AES-128-CCM          │
├──────────────────────────────────────────────────────────────┤
│  DTN / Bundle       BPv7 (RFC 9171) · ipn: scheme            │
│                     Store-carry-forward, custody transfer     │
├──────────────────────────────────────────────────────────────┤
│  Mesh Routing       Reactive RREQ/RREP (< 100 nodes: OGM)    │
│                     Route cache, expanding ring search        │
├──────────────────────────────────────────────────────────────┤
│  Mesh Frame         Custom header, EtherType 0x88B5           │
│                     src_node · dst_node · next_hop · TTL      │
├──────────────────────────────────────────────────────────────┤
│  Peer Link Security Per-peer ECDH session key · AES-128-CCM  │
├──────────────────────────────────────────────────────────────┤
│  802.11ah IBSS      MM6108 #1 · SDIO · local TDMA sleep      │
└──────────────────────────────────────────────────────────────┘
```

### 4.2 Tier 2 — Leaf Cluster

```
┌──────────────────────────────────────────────────────────────┐
│  Application        CBOR-encoded sensor payload               │
├──────────────────────────────────────────────────────────────┤
│  E2E Security       BPSec + COSE AEAD (same as Tier 1)        │
├──────────────────────────────────────────────────────────────┤
│  DTN / Bundle       BPv7 · leaf sends bundles to relay AP     │
├──────────────────────────────────────────────────────────────┤
│  Link security      WPA3-SAE (standard 802.11ah AP/STA)       │
├──────────────────────────────────────────────────────────────┤
│  Power save         TWT — hardware native, relay AP manages   │
│  Dense control      RAW — hardware native, up to 100 STAs     │
├──────────────────────────────────────────────────────────────┤
│  802.11ah AP/STA    MM6108 #2 (relay: AP) · SPI               │
│                     MM6108 (leaf: STA) · SDIO or SPI          │
└──────────────────────────────────────────────────────────────┘
```

---

## 5. Physical and Link Layer

### 5.1 Tier 1 — Relay Backbone (IBSS)

All relay and gateway nodes operate in 802.11ah IBSS mode on MM6108 #1.

| Parameter | Value |
|---|---|
| Interface type | `MORSE_CMD_INTERFACE_TYPE_ADHOC` |
| SSID | `hmdp-bb-<network_id>` (backbone) |
| Channel | Backbone channel — deployment-configured |
| Channel bandwidth | 1 MHz (max range) or 2 MHz (balanced) |
| BSSID | Deterministic from network_id |
| Max relay nodes | ~500 (reactive routing limit) |

All relay nodes in a deployment share the same backbone SSID, BSSID, and channel. Any relay can directly exchange frames with any other relay it can physically hear. The mesh routing layer (Section 8) handles multi-hop delivery.

### 5.2 Tier 2 — Leaf Cluster (AP+STA)

Each relay runs MM6108 #2 in AP mode for its leaf cluster.

| Parameter | Value |
|---|---|
| Interface type | AP mode (`mmwlan_softap_enable()`, mm-iot-sdk 2.10.4+) |
| SSID | `hmdp-<relay_node_id>` |
| Channel | Leaf channel — assigned per relay (see Section 5.3) |
| Channel bandwidth | 1 MHz |
| Max leaf STAs | ~100 (802.11ah TIM practical limit) |
| Power save | TWT per STA, scheduled by AP |
| Dense contention | RAW, relay configures windows for up to 100 STAs |

Leaves use standard 802.11ah STA mode. TWT association is negotiated at link setup. No HMDP-specific power management is required on the leaf — TWT handles it natively.

### 5.3 Channel Planning

Two separate channel pools are used to prevent inter-tier interference:

```
Backbone channel (Tier 1):
  One channel shared by all 500 relays.
  Example: Ch 36 (920 MHz, US)
  Reactive routing keeps overhead ~10–15% of capacity.

Leaf channels (Tier 2):
  Each relay AP operates on a leaf channel.
  Adjacent relays must use different channels to avoid
  AP beacon collisions and STA confusion.
  
  Frequency reuse pattern (US example):
    ~13 non-overlapping 1MHz channels in 902–928 MHz
    Use 4–7 channels for leaf tier
    Assign channels via graph colouring of relay adjacency
    Distant relays may reuse the same leaf channel

  Channel assignment: provisioned at deployment time.
  Stored in each relay's configuration.
```

### 5.4 Transmit Power and Range

| Bandwidth | Open field range | Dense urban range |
|---|---|---|
| 1 MHz MCS0 | > 1000 m | 200–400 m |
| 2 MHz MCS0 | 500–800 m | 150–300 m |

### 5.5 Multi-Hop Forwarding

Multi-hop forwarding is implemented at the mesh frame layer, not at the 802.11ah MAC layer. Each hop is an independent IBSS frame exchange:

```
Hop A→B:  802.11ah IBSS frame, src=A_mac, dst=B_mac
          Mesh header: src_node=A, dst_node=D, next_hop=B, TTL=n

B receives, consults routing table, re-transmits:
Hop B→C:  802.11ah IBSS frame, src=B_mac, dst=C_mac
          Mesh header: src_node=A, dst_node=D, next_hop=C, TTL=n-1
```

MAC addresses change per hop. Node IDs in the mesh header remain fixed end-to-end.

---

## 6. Mesh Frame Format

### 6.1 Ethernet Encapsulation

All HMDP Tier 1 frames are carried as Ethernet II frames with EtherType `0x88B5`:

```
 ┌─────────────────────────────────────┐
 │  Destination MAC  (6 bytes)          │  next-hop MAC
 ├─────────────────────────────────────┤
 │  Source MAC       (6 bytes)          │  this node's MAC
 ├─────────────────────────────────────┤
 │  EtherType = 0x88B5  (2 bytes)       │
 ├─────────────────────────────────────┤
 │  HMDP Mesh Header (26 bytes)         │
 ├─────────────────────────────────────┤
 │  HMDP Payload     (variable)         │
 └─────────────────────────────────────┘
```

### 6.2 HMDP Mesh Header (26 bytes)

```
 0               1               2               3
 0 1 2 3 4 5 6 7 0 1 2 3 4 5 6 7 0 1 2 3 4 5 6 7 0 1 2 3 4 5 6 7
 ├───────────────┬───────────────┬───────────────┬───────────────┤
 │ Version (4)   │  Type (4)     │   Flags (8)   │    TTL (8)    │
 ├───────────────┴───────────────┼───────────────┴───────────────┤
 │              Sequence Number (16 bits)                         │
 ├───────────────────────────────────────────────────────────────┤
 │              Source Node ID (48 bits / 6 bytes)                │
 ├───────────────────────────────────────────────────────────────┤
 │              Destination Node ID (48 bits / 6 bytes)           │
 ├───────────────────────────────────────────────────────────────┤
 │              Next Hop Node ID (48 bits / 6 bytes)              │
 ├───────────────────────────────────────────────────────────────┤
 │              Payload Length (16 bits)                          │
 └───────────────────────────────────────────────────────────────┘
```

**Version**: `0x1`.

**Flags**:

| Bit | Name | Meaning |
|---|---|---|
| 7 | `DTN` | Payload is a BPv7 bundle |
| 6 | `FRAG` | Mesh-layer fragment |
| 5 | `MULE` | Sender is a mule node |
| 4 | `ACK_REQ` | Hop-level ACK requested |
| 3–0 | Reserved | Must be zero |

**TTL**: Default 16. Decremented each hop. Discard at zero.

**Sequence Number**: Per-source, monotonically increasing. Frames with a `(src_node, seq)` pair seen within 60 seconds are discarded (loop prevention).

### 6.3 Frame Types

| Type | Name | Description |
|---|---|---|
| `0x1` | HELLO | Neighbor discovery beacon (backbone) |
| `0x2` | PEER_OPEN | Initiate peer link |
| `0x3` | PEER_CONFIRM | Accept peer link |
| `0x4` | PEER_CLOSE | Tear down peer link |
| `0x5` | RREQ | Route Request — flooded toward destination |
| `0x6` | RREP | Route Reply — unicast back to source |
| `0x7` | RERR | Route Error — notify upstream of broken link |
| `0x8` | DATA | Mesh data frame carrying a BPv7 bundle |
| `0x9` | ACK | Hop-level acknowledgment |
| `0xA` | TIME_SYNC | Time synchronisation from gateway |
| `0xB` | CUSTODY_REQ | Request custody transfer (mule) |
| `0xC` | CUSTODY_ACK | Confirm custody transfer |

---

## 7. Neighbor Discovery and Peer Links

### 7.1 HELLO Frames (Tier 1 only)

Relay and gateway nodes broadcast HELLO frames on the backbone channel. Leaves do NOT participate in the backbone HELLO protocol.

**HELLO payload** (CBOR):

```
{
  "node_id":          <6-byte EUI-48>,
  "role":             <uint8: 0=gateway, 1=relay, 2=leaf, 3=mule>,
  "seq":              <uint16>,
  "battery":          <uint8: 0-100%, 255=wired/solar>,
  "tx_power":         <int8: dBm>,
  "peers":            <list of node_ids currently peered with>,
  "flags":            <uint8: capability flags>,

  // Local TDMA schedule (for sleep coordination)
  "tdma_frame_ms":    <uint16: superframe duration>,
  "tdma_slot_ms":     <uint16: slot duration>,
  "tdma_my_tx_slot":  <uint8: this node's TX slot number>,
  "tdma_rx_slots":    <list of uint8: slots this node listens>,
  "tdma_mgmt_slots":  <list of uint8: shared management slots>
}
```

**HELLO intervals** (Trickle-suppressed per RFC 6206):

| Role | Base interval | Max interval |
|---|---|---|
| Gateway | 2 s | 128 s |
| Relay | 5 s | 320 s |
| Mule | 2 s | 32 s |

### 7.2 Local TDMA Schedule

Each relay independently manages a local TDMA superframe for sleep coordination with its backbone peers. This is not a global network schedule — it is a per-neighbourhood agreement.

```
Superframe example (relay with K=6 backbone peers):
  Total slots: 20, slot duration: 100ms, frame: 2s

  Slot  0:   This relay TX
  Slots 1-6: Peer RX (one per peer, relay listens)
  Slots 7-9: MGMT (RREQ/RREP/HELLO shared)
  Slots 10-19: SLEEP

  Relay backbone duty cycle: 10/20 = 50%
  Average backbone radio current: ~7 mA
```

Neighbours learn each other's slots from the TDMA fields in HELLO frames and wake accordingly. This eliminates the need for ATIM or any firmware-level power-save coordination.

### 7.3 Peer Link Establishment

Peer links secure backbone communication between relay nodes. Leaf-to-relay security is handled by WPA3-SAE at the 802.11ah level (standard association).

**Establishment** (backbone peers only):

```
Node A                              Node B
  │── PEER_OPEN ──────────────────>│
  │   nonce_a, pubkey_a (X25519)   │
  │                                 │
  │<── PEER_CONFIRM ───────────────│
  │    nonce_b, pubkey_b,           │
  │    MIC(PSK, nonce_a||nonce_b)  │
  │                                 │
  │  session_key = HKDF(             │
  │    ECDH(priv_a, pub_b),         │
  │    nonce_a||nonce_b,            │
  │    "HMDP-v1-session")           │
  │  Peer link ACTIVE               │
```

All subsequent backbone frames between these two nodes are encrypted with AES-128-CCM using the session key. Nonce includes a per-frame counter for replay protection.

---

## 8. Mesh Routing Protocol

### 8.1 Routing Mode Selection

The routing protocol scales with deployment size:

| Relay count | Routing mode | Reason |
|---|---|---|
| < 100 relays | Proactive OGM flooding | Simple, low latency, affordable overhead |
| 100–500 relays | OGM + aggressive Trickle | Marginal, monitor channel utilisation |
| > 500 relays | **Reactive RREQ/RREP** | OGM flooding saturates channel |

**Primary mode for target deployments (500–1000 relays): Reactive routing.**

The OGM protocol (described in Appendix A) is retained for small deployments only and is not detailed in the main body of this specification.

### 8.2 Reactive Routing Overview

HMDP uses an on-demand route discovery protocol based on AODV (RFC 3561) adapted for the DTN mesh context. Routes are discovered only when a node has data to send and no cached route exists. The DTN layer buffers bundles during route discovery without any protocol-level timeout.

### 8.3 Route Request (RREQ)

When node A needs a route to node Z and has no cached route:

```
Step 1: A broadcasts RREQ with TTL=RREQ_INITIAL_TTL (default: 2)

RREQ frame payload (CBOR):
{
  "src":      <6-byte: originating node>,
  "dst":      <6-byte: desired destination>,
  "rreq_id":  <uint32: unique per src, incremented per request>,
  "hop_cnt":  <uint8: 0 at origin, incremented each hop>,
  "seq_src":  <uint16: src sequence number>,
  "seq_dst":  <uint16: last known dst sequence, 0 if unknown>
}

Step 2: Each relay re-broadcasts RREQ if:
  - (src, rreq_id) not seen before (duplicate suppression)
  - TTL > 0 after decrement
  - Records reverse path: "to reach src, send to who sent this"

Step 3: If RREQ reaches Z (or a node with a fresh route to Z):
  Z (or the intermediate) sends RREP unicast back along
  the reverse path.

Step 4: Each relay forwarding RREP installs a forward route to Z.
  A receives RREP → route installed → DTN engine releases
  buffered bundles.
```

### 8.4 Expanding Ring Search

To avoid full-network floods for nearby destinations:

```
Attempt  TTL    Wait before retry
───────────────────────────────────────
1        2      RREQ_TIMEOUT = 500ms
2        4      RREQ_TIMEOUT = 500ms
3        8      RREQ_TIMEOUT = 1000ms
4        16     RREQ_TIMEOUT = 2000ms
5+       MAX    DTN buffer, retry after RREQ_RATELIMIT
```

If no RREP is received at TTL=MAX, the destination is currently unreachable. The bundle is stored by the DTN layer and route discovery is retried after `RREQ_RETRY_INTERVAL` (default: 60s).

### 8.5 Route Reply (RREP)

```
RREP frame payload (CBOR):
{
  "src":       <6-byte: route origin (was RREQ src)>,
  "dst":       <6-byte: route destination (was RREQ dst)>,
  "hop_cnt":   <uint8: distance from dst to here>,
  "seq_dst":   <uint16: dst sequence number>,
  "lifetime":  <uint32: route valid for N seconds>
}
```

Default `lifetime` = 300s. Refreshed by data traffic. Expired routes are removed from the cache.

### 8.6 Route Error (RERR)

When a relay detects a broken link (no ACK after retries) while forwarding:

```
RERR frame payload (CBOR):
{
  "broken_dst": <list of 6-byte node IDs unreachable via broken link>
}
```

RERR is sent upstream toward the bundle source. The source's DTN layer buffers the bundle and initiates fresh RREQ discovery. This is DTN-driven recovery: the routing layer notifies, the DTN layer retries. No routing-layer fast reroute is attempted.

### 8.7 Flood Control

Three mechanisms prevent RREQ storms:

```
1. Jittered rebroadcast:
   Each relay waits random(0, RREQ_JITTER_MS=20) before
   re-flooding a RREQ. Staggers the flood wave.

2. Duplicate suppression:
   (src, rreq_id) seen within 60s → discard. One table
   entry per originator.

3. Rate limiting:
   Max 1 RREQ per destination per RREQ_RATELIMIT=10s.
   Prevents storms toward unreachable destinations.
```

### 8.8 Forwarding Table

Each relay maintains:

| Field | Size | Description |
|---|---|---|
| dst_node | 6 bytes | Destination Node ID |
| next_hop | 6 bytes | Neighbour to forward toward dst |
| hop_cnt | 1 byte | Distance metric |
| seq_dst | 2 bytes | Last known dst sequence number |
| lifetime | 4 bytes | Route expiry timestamp |

Entries expire after `lifetime`. No proactive refresh — routes expire naturally when unused.

---

## 9. DTN Bundle Layer

### 9.1 Bundle Protocol

HMDP uses BPv7 as defined in RFC 9171. All sensor data is encapsulated in bundles. The DTN layer is the integration point between Tier 1 (backbone routing) and Tier 2 (leaf delivery).

### 9.2 Endpoint Addressing

```
EID format:  ipn:<node_number>.<service_number>

node_number   = decimal of 48-bit Node ID
service_number:
  1 = sensor data
  2 = command/control
  3 = time sync
  4 = network management
```

Leaf bundles destined for nodes on other relay clusters are routed: leaf → relay AP (Tier 2) → relay DTN engine → backbone routing to destination relay (Tier 1) → destination relay AP → destination leaf (Tier 2).

### 9.3 Bundle Storage

| Role | Minimum flash storage |
|---|---|
| Gateway | 512 KB |
| Relay | 256 KB |
| Leaf | 32 KB |
| Mule | 1 MB |

### 9.4 Bundle Lifetime

| Data type | Default lifetime |
|---|---|
| Sensor reading | 24 hours |
| Alert / event | 72 hours |
| Command | 1 hour |

### 9.5 Custody Transfer (Mule Protocol)

When a mule arrives in range of a relay:

1. Mule announces itself via HELLO with `MULE` flag set.
2. Relay detects mule in neighbour table.
3. Relay sends `CUSTODY_REQ` listing buffered bundle IDs.
4. Mule accepts bundles, responds with `CUSTODY_ACK`.
5. Relay deletes its copies only after receiving `CUSTODY_ACK`.
6. Mule carries bundles and uploads at connectivity point.
7. Gateway deduplicates on arrival (bundle source EID + creation timestamp as unique key).

---

## 10. Security

### 10.1 Threat Model

HMDP protects against passive eavesdropping, active frame tampering, replay attacks, rogue node injection, and mule data theft.

### 10.2 Hop-by-Hop Security (Tier 1 Backbone)

All backbone frames are encrypted with per-peer session keys:

- **Algorithm**: AES-128-CCM
- **Key**: X25519 ECDH + HKDF per peer link (Section 7.3)
- **Nonce**: `node_id (6) || seq (2) || timestamp (4) || counter (1)`
- **AAD**: src MAC || dst MAC || EtherType || frame type
- **Tag**: 8 bytes

### 10.3 Hop-by-Hop Security (Tier 2 Leaf Attachment)

Standard WPA3-SAE via the 802.11ah AP/STA association. Handled entirely by the MM6108 and morselib. No HMDP-specific code required.

### 10.4 End-to-End Security (Bundle Layer)

Bundle payloads are protected source-to-destination using BPSec (RFC 9172):

- **Confidentiality**: COSE_Encrypt0 with ChaCha20-Poly1305
- **Key derivation**:
  ```
  bundle_key = HKDF-SHA256(
      ikm  = deployment_root_secret,
      salt = src_node_id || dst_node_id,
      info = "HMDP-v1-bundle"
  )
  ```
- **Nonce**: bundle creation timestamp (8B) || sequence (4B)

A mule carries bundles but cannot read their contents without the bundle key.

### 10.5 Key Management

| Key | Where stored | Rotation |
|---|---|---|
| Deployment root secret | Protected flash (ESP32 eFuse NVS / nRF54 KMU) | Out of scope v1 |
| Network PSK (peer link MIC) | Protected flash | Out of scope v1 |
| Session keys (per peer link) | RAM only | Each peer link setup |
| WPA3-SAE keys (leaf) | Managed by morselib | Standard 802.11ah |

---

## 11. Gateway and IPv6 Border Translation

### 11.1 Gateway Role

The gateway participates in the relay backbone as an IBSS node with full routing capability. When internet connectivity is available, it additionally:

- Unpacks inbound bundles destined for internet EIDs
- Re-encodes as IPv6/CoAP, MQTT, or HTTPS
- Forwards to cloud endpoints
- Wraps downlink commands as bundles and injects into backbone

### 11.2 Time Synchronisation

```
TIME_SYNC payload (CBOR):
{
  "time":      <uint64: Unix timestamp, milliseconds>,
  "precision": <int8: log2 of precision in seconds>,
  "stratum":   <uint8: NTP stratum>
}
```

Relays re-flood TIME_SYNC, decrementing precision by 1 per hop. Leaves accept the first TIME_SYNC received on wake via their relay AP.

---

## 12. Power Management

### 12.1 Tier 2 Leaf Sleep (TWT — Hardware Native)

Leaves use 802.11ah Target Wake Time, negotiated at association with their relay AP. No HMDP-specific sleep code is required on the leaf.

```
TWT configuration (provisioned at AP):
  Wake interval: 300,000 ms  (5 minutes, configurable)
  Wake duration: 100 ms
  
Leaf power profile:
  Active (radio Tx/Rx + MCU): ~15 mA for 100ms
  Sleep (MM6108 hibernate + nRF54 deep sleep): ~10 µA
  
  Average current:
    (15,000 µA × 0.1/300) + (10 µA × 299.9/300)
    = 5 µA + 10 µA = ~15 µA
  
  3000 mAh battery (ignoring sensor):  ~22 years
  Realistic (sensor + MCU overhead):   3–7 years
```

RAW (Restricted Access Window) is configured by the relay AP to manage contention among up to 100 simultaneous TWT wake events. Standard 802.11ah feature, no HMDP code required.

### 12.2 Tier 1 Relay Backbone Sleep (Local TDMA)

Relay backbone radios (MM6108 #1) use the local neighbourhood TDMA schedule advertised in HELLO frames (Section 7.2).

```
Relay backbone duty cycle with K backbone peers:

  Awake slots = 1 (own TX) + K (peer RX) + 3 (MGMT)
  Total slots = 20 (configurable)

  K=4 peers:  8/20 = 40% → ~5.6 mA average on backbone radio
  K=6 peers: 10/20 = 50% → ~7.0 mA average
  K=10 peers: 14/20 = 70% → ~9.8 mA average
  K>12 peers: always-on becomes simpler (~14 mA)
```

### 12.3 Tier 2 AP Radio (MM6108 #2)

The leaf AP radio is mostly idle between TWT windows:

```
100 leaves × TWT 100ms/5min = 3.3% leaf traffic
AP beacon overhead: ~5%
Total AP radio active: ~8%

Average AP radio current:
  12 mA × 0.08 + 0.05 × 0.92 ≈ ~1.0 mA
```

### 12.4 Total Relay Power Budget

```
Component                Current
─────────────────────────────────────────────────
MM6108 #1 IBSS (K=6):   ~7.0 mA  (50% duty TDMA)
MM6108 #2 AP:           ~1.0 mA  (8% leaf traffic)
ESP32-S3 light sleep:   ~2.0 mA
─────────────────────────────────────────────────
Total:                  ~10 mA average

Solar panel needed:     ~35 mW → 0.5W panel sufficient
                        (small, practical for field deployment)
```

---

## 13. Dense and Sparse Deployment

### 13.1 Scale Guidelines

| Scale | Relay count | Routing mode | Notes |
|---|---|---|---|
| Small | < 100 | OGM proactive (Appendix A) | Simple, low latency |
| Medium | 100–500 | OGM + aggressive Trickle | Monitor channel load |
| Large (target) | 500–1000 | Reactive RREQ/RREP | Standard mode for this spec |
| Beyond | > 1000 | Zone routing required | Outside scope of v1 |

### 13.2 Channel Capacity at Scale

```
Target deployment: 500 relays, 100 leaves each

Tier 1 backbone channel (1MHz, ~150 kbps usable):
  Routing overhead (reactive): ~15 kbps average
  Data traffic (multi-hop):    ~20 kbps average
  Total per segment:           ~35 kbps  ✓ well within limit

Tier 2 leaf channels (per relay cluster):
  100 leaves × 1 reading/5min × 300 bytes = 500 bps
  Well within 1MHz channel capacity per cluster  ✓
```

### 13.3 Dense Leaf Deployment

Up to 100 leaves per relay is within 802.11ah design parameters:

- TWT schedules individual wake times preventing simultaneous contention.
- RAW divides leaves into access windows for remaining contention cases.
- The relay AP can serve up to ~8191 associated STAs per the 802.11ah standard; 100 is conservative.

### 13.4 Sparse Deployment

In sparse deployments where relays may be temporarily isolated:

- DTN bundles buffer indefinitely (subject to lifetime).
- Mule sweeps scheduled shorter than the critical data lifetime.
- Leaves extend their TWT wake window to `ISOLATION_SCAN_WINDOW=2000ms` when their AP disappears (re-association scanning).

---

## 14. Open Issues

Issues are listed in priority order. Issues marked **BLOCKING** must be resolved before hardware or firmware development begins.

---

---

### Issue #1 — IBSS Init Sequence Documentation **[BLOCKING]**

**Status**: Open

**Description**: The morselib IBSS initialisation sequence using `MORSE_CMD_INTERFACE_TYPE_ADHOC` has no official documentation. One community member confirmed it works on ESP32 (May 2026) after fixing their init sequence, but no reference implementation is published.

**Resolution**: Read morselib source (`morse_commands.h`, `mmwlan_sta.c`, `mmwlan_softap.c`), trace how AP and STA modes are initialised, replicate for ADHOC mode. Write a clean `mmwlan_ibss_enable()` wrapper and test on Seeed Xiao HaLow hardware. Upstream to Morse Micro or maintain as HMDP fork.

---

### Issue #2 — BCF File Dependency **[BLOCKING]**

**Status**: Open

**Description**: Every MM6108 module requires a vendor-supplied Board Configuration File (BCF) for RF calibration. BCFs are not user-modifiable and must be obtained from the module vendor. Production hardware is blocked until the BCF for the chosen module is confirmed available.

**Resolution**: Confirm BCF availability from module vendor before committing to a module. Recommended starting modules: Seeed Xiao HaLow, AsiaRF MM610X-001 (both have community-verified BCFs).

---

### Issue #3 — RREQ Storm Validation at Scale **[HIGH]**

**Status**: Open

**Description**: The reactive routing flood control mechanisms (jitter, duplicate suppression, rate limiting, expanding ring) have not been empirically validated at 500–1000 node scale on 802.11ah.

**Resolution**: Simulate 500-node RREQ flood with realistic traffic patterns (1 reading/5min per node). Measure channel utilisation, route discovery latency, and delivery ratio. Tune `RREQ_JITTER_MS` and `RREQ_RATELIMIT` based on results.

---

### Issue #4 — Key Rotation **[MEDIUM]**

**Status**: Open

**Description**: Deployment root secret and network PSK rotation are not defined. Critical for long-lived deployments where node compromise is a concern.

**Resolution**: Define a key rotation protocol for v2. Consider out-of-band provisioning (physical access to relay) as v1 mitigation.

---

### Issue #5 — Bundle Fragmentation **[MEDIUM]**

**Status**: Open

**Description**: BPv7 bundles exceeding the 802.11ah MSDU size may require fragmentation. 802.11ah's large MSDU support (~1500 bytes) reduces frequency but does not eliminate it.

**Resolution**: Define mesh-layer fragmentation in the FRAG flag and associated fragment header extension. Or rely on BPv7 bundle fragmentation (RFC 9171 Section 5.8).

---

### Issue #6 — OGM Metric Tuning **[MEDIUM]**

**Status**: Open

**Description**: The OGM link cost function and intervals (Appendix A) need empirical calibration against actual MM6108 IBSS link quality measurements.

**Resolution**: Measure RSSI, packet loss, and ETX on MM6108 IBSS links at various distances and obstruction levels. Calibrate `link_cost = 256/lq` accordingly.

---

### Issue #7 — Mule Authentication **[MEDIUM]**

**Status**: Open

**Description**: The custody protocol does not authenticate mules. A rogue mule could accept custody of bundles and discard them.

**Resolution**: Add a mule credential — either a deployment-level certificate or a pre-shared mule ID list held by relays. Only credentialed mules are offered custody.

---

### Issue #8 — Dense Leaf Contention Without RAW **[LOW]**

**Status**: Open

**Description**: RAW is available in AP/STA mode (Tier 2) but the specific RAW configuration for 100 leaves with varied TWT schedules needs validation. Theoretical design is sound; empirical testing at 50–100 STAs has not been done.

**Resolution**: Test MM6108 AP mode with 50–100 simulated STA connections using TWT and RAW. Measure actual contention and throughput.

---

### Issue #9 — Dual MM6108 SDIO Arbitration **[LOW]**

**Status**: Open

**Description**: ESP32-S3 has one SDIO controller (used by MM6108 #1, IBSS) and multiple SPI controllers (used by MM6108 #2, AP). Both radios generating interrupts simultaneously requires careful ISR and DMA arbitration in firmware.

**Resolution**: Prototype dual-radio relay on ESP32-S3. Validate that simultaneous SDIO (IBSS) and SPI (AP) interrupt handling is stable at expected traffic rates. nRF54 uses SPI for both — verify two separate SPI peripherals work concurrently.

---

---

## 15. Future Investigations

Items in this section are explicitly out of scope for v1. They are recorded here to avoid re-litigating the decisions and to provide a starting point for future versions.

---

### FI-1 — Single MM6108 Concurrent IBSS+AP

**Potential benefit**: Reduce relay BOM by ~$12–15 per node by eliminating the second MM6108. At 500 relays this is ~$6,000–$7,500.

**Background**: A relay currently requires two MM6108 radios — one in IBSS mode (Tier 1 backbone) and one in AP mode (Tier 2 leaf cluster). A single MM6108 running both roles concurrently would eliminate this.

**Why it may be feasible**:
- The S1G Relay feature (mm-iot-sdk 2.11.2) already demonstrates concurrent AP+STA on one MM6108. The chip can handle two simultaneous MAC contexts.
- IBSS is architecturally simpler than STA for the second context — no association state machine, no TWT negotiation toward a parent.
- The Linux MM6108 driver includes an `IBSS_bridge_support` patch suggesting Morse Micro has explored this direction.
- morselib is open source (Apache 2.0); the UMAC can be extended.

**What is needed to pursue**:
1. Add `mmwlan_ibss_enable()` as a proper public API (foundation: `MORSE_CMD_INTERFACE_TYPE_ADHOC` in `morse_commands.h`).
2. Extend morselib to support two concurrent VIF contexts (IBSS + AP), modelled on S1G Relay.
3. Verify that `mm6108.bin` chip firmware supports the IBSS+AP combination at the LMAC level.
4. Resolve TBTT scheduling conflict: AP beacon (hard deadline) vs IBSS beacon contention (soft, can be deferred).

**Estimated effort**: 2–4 weeks of morselib engineering. Risk: medium — chip firmware support unconfirmed until tested.

**Decision trigger**: Pursue when relay unit volume exceeds 200 units, or when BOM cost is a blocking constraint.

---

*Document status: Draft 0.2 — Two-tier architecture, reactive routing, and dual-radio relay hardware incorporated.*
*Changes from 0.1: Two-tier architecture, reactive RREQ/RREP routing replacing flat OGM, dual MM6108 relay hardware, TWT for leaf power (replacing custom TDMA), local neighbourhood TDMA for backbone sleep, channel planning, updated power budget, concurrent IBSS+AP moved to Future Investigations (FI-1), open issues renumbered.*

---

## Appendix A — Proactive OGM Routing (Small Deployments < 100 Relays)

For deployments below 100 relay nodes, the simpler proactive OGM routing protocol may be used in place of reactive RREQ/RREP.

**OGM frame payload** (CBOR):
```
{
  "originator": <6-byte node_id>,
  "seq":        <uint16: monotonically increasing>,
  "ttl":        <uint8: starts at 16, decremented per hop>,
  "metric":     <uint8: cumulative, starts at 0>
}
```

**OGM intervals**:

| Role | Interval |
|---|---|
| Gateway | 2 s |
| Relay | 10 s |
| Mule | 3 s |

**Propagation**: Each relay re-broadcasts received OGMs (TTL-1, metric + link_cost) unless `(originator, seq)` was seen within 60 seconds. Trickle suppression doubles the interval when the network is stable.

**Forwarding table**: Each relay stores the neighbour that delivered the best (lowest metric) OGM for each originator. `link_cost = 256 / lq` where `lq` is link quality 0–255.

**Transition to reactive**: When relay count exceeds 100, or when OGM-induced channel load exceeds 20% of capacity, switch to reactive RREQ/RREP (Section 8).
