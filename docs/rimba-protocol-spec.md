# Rimba Protocol
## Technical Specification — Draft 0.28

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
14. [Over-The-Air Firmware Update (OTA)](#14-over-the-air-firmware-update-ota)
15. [Open Issues](#15-open-issues)
16. [Future Investigations](#16-future-investigations)

---

## 1. Introduction

### 1.1 Purpose

Rimba is a low-power wireless mesh networking protocol for battery- and solar-powered IoT sensor deployments. It operates in environments where connectivity to infrastructure is intermittent or absent, and where nodes may be separated by distances requiring multi-hop forwarding.

### 1.2 Design Goals

- **MCU-only**: All roles run on ESP32 or nRF54 class microcontrollers. No Linux required.
- **Uniform radio mode**: All nodes run 802.11ah IBSS mode. No AP or STA mode.
- **True mesh**: Any node communicates with any other node without a gateway or fixed root.
- **DTN-native**: Disruption-tolerant networking is a first-class concern, not an afterthought.
- **Low power**: Leaf nodes target multi-year battery life via custom sleep scheduling.
- **Secure**: All payloads are end-to-end encrypted. All links are hop-by-hop secured.
- **Scalable**: Supports 500–1000 relay nodes with additional leaf nodes per relay.

### 1.3 Non-Goals

- Rimba does not use 802.11s or any standard mesh protocol.
- Rimba does not use AP or STA mode on any node.
- Rimba does not run a full IPv6 stack inside the mesh.
- Rimba does not target real-time or low-latency applications.

### 1.4 Hardware Targets

| Component | Specification |
|---|---|
| Host MCU | Espressif ESP32-S3 or Nordic nRF54 series |
| Radio | Morse Micro MM6108 (Wi-Fi HaLow SoC) |
| **RTC** | **≤ 1 ppm temperature-compensated crystal RTC (mandatory)** |
| | Recommended: RV-3028-C7 (Micro Crystal, ±1 ppm, 40 nA standby) |
| | Alternative: RV-8803-C7 (Micro Crystal, ±1 ppm, 100 nA standby) |
| Host-Radio bus | SDIO or SPI |
| RTC bus | I2C |
| **GPS** (optional) | GNSS receiver, ≤ 5m CEP accuracy, UART or I2C output |
| | 1PPS output not required — position only, not used for timing |
| | Recommended for relay nodes in deployments > 200 nodes |
| | Examples: u-blox MAX-M10S (~$5), Quectel L86 (~$4) |
| | Interface: UART or I2C; powered only during initial fix |
| Radio firmware | mm6108.mbin (closed, provided by Morse Micro) |
| Host MAC/driver | [morselib](https://github.com/MorseMicro/mm-iot-sdk) (open source, Apache 2.0) |
| Radio mode | 802.11ah IBSS on all nodes |
| Frequency | 850–950 MHz (region-specific, license-exempt) |
| PHY standard | IEEE 802.11ah (Wi-Fi HaLow) |

---

## 2. Terminology

| Term | Definition |
|---|---|
| **Node** | Any device participating in a Rimba network |
| **Peer link** | A secured, bidirectional L2 link between two Rimba nodes |
| **IBSS** | Independent Basic Service Set — 802.11 adhoc mode used by all Rimba nodes |
| **OGM** | Originator Message — proactive routing advertisement (small deployments only) |
| **RREQ / RREP / RERR** | Route Request / Route Reply / Route Error — reactive routing messages |
| **Bundle** | A DTN protocol data unit as defined in BPv7 (RFC 9171) |
| **Custody** | Responsibility for delivering a bundle accepted by a node |
| **EID** | Endpoint Identifier — the bundle-layer address of a node (`ipn:` scheme) |
| **BCF** | Board Configuration File — MM6108 module RF calibration data |
| **Wake window** | The period during which a sleeping node is awake and reachable |
| **Superframe** | A repeating time grid used for local sleep coordination |

---

## 3. Network Architecture

### 3.1 Topology

Rimba forms a flat, fully decentralised IBSS mesh. All nodes are equal at the radio layer. Any node within radio range of another can exchange frames directly without any association or role negotiation.

```
        [GW]──────[R]──────[R]──────[R]
          \      / \      / \      /
          [R]──[R]──[R]──[R]──[R]──[R]
          /     \       \       \
        [L][L] [L][L]  [L][L]  [L][L]
                              
  All nodes: 802.11ah IBSS, same channel, same SSID
  All frames: raw Ethernet, EtherType 0x88B5
  All links:  peer-to-peer, ECDH-secured
  No AP. No STA. No root. No sink.
```

The network operates correctly with no gateway, no mule, and no fixed structure. Any node can communicate with any other node via multi-hop forwarding.

### 3.2 Node Roles

All roles run the same Rimba protocol. Role determines default power profile, routing participation level, and data flow behaviour — not a different protocol variant.

#### 3.2.1 Gateway

- Participates fully in the IBSS mesh as a standard Rimba node.
- Additionally provides internet or intranet connectivity via cellular modem or wired Ethernet.
- Acts as IPv6 border translator: bundles destined for internet EIDs are unpacked and forwarded as IPv6.
- Serves as a time source (NTP-synced) for the mesh. Multiple gateways may coexist; nodes select the best source per the acceptance rule in Section 11.2.
- Responds to RREQ messages targeting the **gateway anycast address** (`GATEWAY_ANYCAST_ID`, Section 8.11), enabling any mesh node to route toward the nearest gateway without prior knowledge of specific gateway Node IDs.
- Advertises current internet uplink load via `uplink_load_pct` in HELLO, enabling load-aware gateway selection when multiple gateways are reachable.
- Runs continuously on mains power.
- May be absent; the mesh operates correctly without it.

#### 3.2.2 Relay

- Full routing participant: re-floods routing messages, maintains forwarding table, stores and forwards bundles.
- May also source its own sensor data.
- Operates on solar panel with battery backup.
- Higher duty cycle than leaves — must be reachable to serve as a forwarding point.
- Establishes peer links with multiple neighbours.

#### 3.2.3 Leaf

- Sources sensor data only. Does not re-flood routing messages.
- Operates in deep sleep between measurement cycles.
- Powered by primary battery or small solar cell.
- Wakes on a configured schedule, sends pending bundles to the nearest peer relay, sleeps again.
- Establishes peer links with one or two nearby relays.
- Responds to RREQ for its own Node ID when awake. When asleep, the neighbouring relay responds on its behalf (see Section 8.9).

#### 3.2.4 Mule

A mule is a **relay that physically moves between zones**. It includes all relay capabilities plus custody transfer collection and mobility.

**Capabilities (superset of relay):**
- Participates fully in IBSS mesh as a standard Rimba node.
- Establishes backbone peer links (K neighbours) exactly as a relay does.
- Re-floods OGM and responds to RREQ/RREP — full routing participant.
- Maintains a DTN bundle store and forwards bundles via the mesh.
- Serves leaf nodes when stationary (TDMA schedule, receive windows).
- Accepts custody of bundles from isolated relays and leaves via the mule protocol (Section 9.5).
- Carries bundles physically across coverage gaps to zones with gateway connectivity.
- **May source its own sensor data** (e.g. GPS position, battery level, store occupancy) and bundle it for delivery to the gateway — useful for mule route logging and fleet management.
- Advertises the MULE flag (bit 5 of mesh header flags) in HELLO at all times, signalling to nearby isolated relays that custody transfer is available.

**Operating states:**

```
Stationary:  Behaves identically to a relay.
             Backbone peer links active.
             Serves leaves in TDMA schedule.
             Accepts custody transfers from nearby isolated relays.

Moving:      Mobile relay behaviour — backbone peer links break
             and reform as the mule traverses the mesh.
             DTN bundle store travels with the mule.
             Leaf orphaning and RREQ storms apply
             (Section 3.7.2 — mobile relay limitations).

At gateway:  Delivers collected bundles via custody transfer.
             Gateway deduplicates vs mesh-delivered bundles
             (Section 9.5.6).
```

**Role in the node hierarchy:**

```
Leaf     → sources data only
Relay    → routes, stores, forwards, serves leaves
Mule     → relay + moves + collects custody transfers   ← superset
Gateway  → relay + internet uplink
```

A mule is typically powered by a vehicle battery, drone battery, or rechargeable pack rather than a primary cell — battery life is not a primary constraint. GPS is recommended on mule nodes to enable geographic routing (Section 8.12) and position-triggered HELLO on movement.

**Mobile relay limitations apply when moving.** See Section 3.7.2 for the full description of leaf orphaning, RREQ storms, and the v1 operational workaround (drain leaves before departure). Relay handoff protocol (graceful leaf re-association) is deferred to v2.

### 3.3 Node Hardware

All nodes share a common hardware baseline. The RTC is mandatory on every node. GPS is optional and only relevant for relay nodes in large deployments.

```
┌─────────────────────────────────────────────────────┐
│                 NODE (any role)                      │
│                                                      │
│  ┌─────────────────┐   ┌──────────────────────────┐ │
│  │  ESP32-S3        │   │  MM6108                  │ │
│  │  or nRF54        ├───┤  802.11ah IBSS           │ │
│  │                  │   │  MORSE_CMD_INTERFACE_     │ │
│  │  SDIO or SPI     │   │  TYPE_ADHOC              │ │
│  └────────┬─────────┘   └──────────────────────────┘ │
│           │                                           │
│  ┌────────┴─────────┐   ┌──────────────────────────┐ │
│  │  RTC (mandatory) │   │  GPS (optional)           │ │
│  │  ≤ 1 ppm TCXO   │   │  Relay nodes only         │ │
│  │  RV-3028-C7 or  │   │  One-time fix at deploy   │ │
│  │  equivalent      │   │  Stored in flash          │ │
│  │  I2C · 40 nA    │   │  UART or I2C              │ │
│  └──────────────────┘   └──────────────────────────┘ │
│                                                      │
│  Rimba protocol stack                                │
│  BPv7 bundle engine                                  │
│  morselib (Apache 2.0, open source)                  │
└─────────────────────────────────────────────────────┘
```

**Component roles:**

| Component | Mandatory | Purpose |
|---|---|---|
| ESP32-S3 or nRF54 | Yes | Host MCU — runs Rimba stack, morselib, application |
| MM6108 | Yes | 802.11ah IBSS radio — single radio, all roles |
| RTC ≤ 1 ppm | **Yes** | Accurate timekeeping — bundle lifetime, BPSec nonce, leaf wake scheduling (Section 12) |
| GPS receiver | No | Geographic position for optional Tier 1/2 routing (Section 8.12). Relay nodes in deployments > 200 nodes. Power on once at deployment for fix, then off permanently. |

**Per-role hardware profile:**

| Role | MCU | MM6108 | RTC | GPS |
|---|---|---|---|---|
| Gateway | ESP32-S3 or nRF54 | ✅ | ✅ | Optional |
| Relay | ESP32-S3 or nRF54 | ✅ | ✅ | Optional (recommended for large deployments) |
| Mule | ESP32-S3 or nRF54 | ✅ | ✅ | Recommended (geographic routing + position-triggered HELLO) |
| Leaf | ESP32-S3 or nRF54 | ✅ | ✅ | Not needed |

### 3.4 Node Addressing

Each node has a globally unique 48-bit Node ID derived from the MM6108 MAC address (EUI-48).

```
Node ID:    6 bytes  (EUI-48)
Bundle EID: ipn:<node_id_decimal>.<service_number>

Service numbers:
  1 = sensor data
  2 = command / control
  3 = time sync
  4 = network management
```

Reserved Node IDs:

| Value | Meaning |
|---|---|
| `0x000000000000` | Null / unspecified |
| `0x000000000001` | `GATEWAY_ANYCAST_ID` — any gateway responds to RREQ; gateway is the decryption endpoint |
| `0x000000000002` | `CLOUD_ANYCAST_ID` — routed to any gateway, but decryptable only by the cloud backend (gateway forwards blind) |
| `0xFFFFFFFFFFFF` | Broadcast — delivered to all nodes |

### 3.5 Node Connectivity States

Every node maintains a connectivity state at runtime. The state is derived entirely from local observations — no coordination or election required — and drives routing and DTN behaviour automatically.

**States:**

```
  ┌─────────────────────────────────────────────────────────┐
  │  ISOLATED    No active backbone peer links.              │
  │              Neighbor table has no relay/GW/mule entries.│
  │              DTN buffering only. No routing possible.    │
  └──────────────────────┬──────────────────────────────────┘
                         │ first peer link established
                         ▼
  ┌─────────────────────────────────────────────────────────┐
  │  MESH_ONLY   Has backbone peers. No gateway route.       │
  │              Routes between mesh nodes work normally.    │
  │              Internet-destined bundles buffer in DTN.    │
  └──────────┬──────────────────────────┬───────────────────┘
             │ gateway route            │ all peer
             │ appears in              │ links lost
             │ routing table           │
             ▼                         ▼
  ┌─────────────────────┐       ┌──────────────┐
  │  CONNECTED          │       │  ISOLATED    │
  │  Has gateway route. │──────>│              │
  │  Full operation.    │  all  └──────────────┘
  └─────────────────────┘  peers
                            lost
```

**Transition triggers:**

| From | To | Trigger |
|---|---|---|
| ISOLATED | MESH_ONLY | First relay/gateway/mule peer link becomes ACTIVE |
| MESH_ONLY | CONNECTED | `GATEWAY_ANYCAST_ID` route appears in forwarding table (gateway RREP received or gateway OGM heard) |
| CONNECTED | MESH_ONLY | `GATEWAY_ANYCAST_ID` route expires from forwarding table |
| MESH_ONLY | ISOLATED | Last backbone peer link closes or times out |
| CONNECTED | ISOLATED | All peer links lost simultaneously |

**Behaviour per state:**

```
CONNECTED:
  Routing:  Normal RREQ/RREP or OGM forwarding.
  Bundles:  Forward internet-destined bundles (ipn:1.*)
            toward GATEWAY_ANYCAST_ID route (Section 8.11).
            Drain DTN store — dequeue oldest bundles first.
  Leaves:   Accept and forward normally.
  HELLO:    connectivity_state = 2

MESH_ONLY:
  Routing:  Normal mesh routing between nodes.
  Bundles:  Route mesh-destined bundles normally.
            Buffer internet-destined bundles in DTN store.
            Advertise store_pct to attract mule visits.
  Leaves:   Accept; buffer their uplink bundles.
  HELLO:    connectivity_state = 1

ISOLATED:
  Routing:  None. Forwarding table is irrelevant.
  Bundles:  DTN buffer only. Accept from own leaves.
            Refuse inter-relay custody (no path onward).
  Leaves:   Own leaves still accepted.
            Extend leaf scan window to ISOLATION_SCAN_WINDOW_MS.
  HELLO:    connectivity_state = 0
            Broadcast at base interval (no Trickle suppression —
            needs to be discoverable by passing mule).
```

**State is advertised in HELLO** so neighbours can make routing decisions. A relay in MESH_ONLY state is a valid forwarder for mesh traffic but not for internet-destined bundles. A relay in ISOLATED state should not be selected as a next-hop for any bundle requiring onward delivery.

### 3.6 Node Provisioning

#### 3.6.1 What Must Be Provisioned

A factory-fresh node has no network credentials and cannot participate in a Rimba deployment. Provisioning loads the minimum configuration required to join:

| Item | Size | Purpose |
|---|---|---|
| `network_id` | 4 bytes (uint32) | Determines IBSS SSID, BSSID, and channel grouping |
| `channel` | 1 byte | 802.11ah channel number for this deployment |
| `psk` | 16 bytes | Network pre-shared key — used for PEER_CONFIRM MIC |
| `root_secret` | 32 bytes | BPSec bundle key derivation input (HKDF IKM) |
| `role` | 1 byte | Node role: 0=gateway, 1=relay, 2=leaf, 3=mule (relay superset — see Section 3.2.4) |
| `leaf_sleep_ms` | 4 bytes | Sleep interval (optional, default: 900,000) |
| `min_tx_dbm` | 1 byte | Minimum TX power for adaptive power control (optional) |
| `max_tx_dbm` | 1 byte | Maximum TX power — must not exceed BCF regulatory limit |

Items derived automatically — **not** provisioned:

| Item | Source |
|---|---|
| Node ID | MM6108 MAC address (EUI-48) |
| Initial absolute time | TIME_SYNC from first gateway contact |
| GPS position | GPS fix at deployment (relay only, optional) |
| Routing table | Built dynamically |
| Peer link session keys | ECDH per peer link |
| `LEAF_INITIAL_JITTER_MS` | Random value generated on first boot |

#### 3.6.2 Provisioning Mechanisms

Two paths are supported. Both produce the same provisioning payload and store it in the same NVS location.

**Path A — USB/UART (bulk provisioning, staging):**

```
Laptop running rimba-provision CLI tool
    │ USB cable
    ▼
ESP32-S3 UART
    │
    NVS flash ← provisioning payload

Use case: factory pre-provisioning, test bench,
          provisioning 100s of nodes before deployment.
Command:  rimba-provision flash --config deployment.json \
              --port /dev/ttyUSB0
```

**Path B — BLE (field provisioning, individual nodes):**

```
Installer phone (running Rimba Provision app)
    │
    BLE (LE Secure Connections, encrypted)
    │
ESP32-S3 BLE stack (esp_prov component)
    │
    NVS flash ← provisioning payload

Use case: commissioning individual nodes in the field.
          No cables. Works with a phone.
```

#### 3.6.3 Commissioning Mode

When a node boots with no valid provisioning data in NVS, it enters **Commissioning Mode** and does not attempt to join the Rimba IBSS.

```
Commissioning Mode behaviour:
  BLE:   Advertise "Rimba-<serial>" with BLE public key fingerprint
         in manufacturer-specific data field.
         Accept LE Secure Connections pairing.
         Receive provisioning payload over encrypted BLE channel.

  USB:   Accept rimba-provision CLI connection on UART.
         Receive provisioning payload as length-prefixed CBOR frame.

  IBSS:  MM6108 stays powered off. Node does NOT join any IBSS.

  LED:   If present, blink at 1Hz (slow blink = needs provisioning).
         Solid ON = provisioning payload received, writing to flash.
         LED off = rebooting into normal operation.

  Timeout: No automatic timeout. Node stays in Commissioning Mode
           until provisioned or factory-reset.
```

#### 3.6.4 BLE Provisioning Security

The BLE channel is authenticated using the QR code printed on each node's label:

```
1. Node generates ephemeral ECDH key pair on first boot.
   Computes SHA-256 fingerprint of public key.
   Stores key pair and fingerprint in NVS (persists across
   commissioning attempts).

2. Node BLE advertisement includes:
     Device name:  "Rimba-<serial_number>"
     Mfr data:    fingerprint[0:8]  (first 8 bytes of SHA-256)

3. Node label (QR code) encodes:
     serial_number
     fingerprint (full 32 bytes)

4. Installer flow:
     a. Scan QR code with Rimba Provision app.
     b. App reads fingerprint from BLE advertisement.
     c. App compares advertisement fingerprint against QR fingerprint.
     d. If mismatch: ABORT. Do not provision this device.
        (Prevents provisioning a rogue node posing as this device.)
     e. If match: proceed with BLE Secure Connections pairing.
     f. Send provisioning payload over encrypted BLE channel.

Security properties:
  - BLE channel encrypted (LE Secure Connections, ECDH-based)
  - Node identity verified against physical label (QR code)
  - Attacker needs physical device access to learn fingerprint
  - Provisioning window is short (~30 seconds)
```

#### 3.6.5 Provisioning Payload Format

Delivered over BLE or USB as a CBOR-encoded structure:

```
{
  "magic":         <uint32: 0x52494D42  — "RIMB" as ASCII>,
  "version":       <uint8: 1>,
  "network_id":    <uint32>,
  "channel":       <uint8>,
  "psk":           <bytes[16]>,
  "root_secret":   <bytes[32]>,
  "role":          <uint8: 0–3>,
  "leaf_sleep_ms": <uint32: optional, default 900000>,
  "min_tx_dbm":    <int8:  optional>,
  "max_tx_dbm":    <int8:  optional>,
  "peer_key_lifetime_s": <uint32: optional, default 86400 (24h)>,
  "sleep_opt":       <uint8:  optional, 0=Continuous (default), 1=Scheduled>,

  // E2E encryption — MANDATORY for all nodes (Section 10.5)
  "e2e_profile":     <uint8:  0=HYBRID (default), 1=FULL_TRUE>,
  "hello_key_adv":   <uint8:  optional, 0=COLD, 1=WARM — Section 7.1;
                      default follows profile: HYBRID→COLD, FULL_TRUE→WARM>,
  "gateway_pub_key": <bytes[32]: gateway X25519 public key — endpoint for ipn:1.*>,
  "cloud_pub_key":   <bytes[32]: cloud backend X25519 public key — endpoint for ipn:2.*>,

  // Per-node identity — MANDATORY for all nodes (Section 7.5.5, 10.5)
  "node_x_priv":     <bytes[32]: this node's X25519 private key (encryption)>,
  "node_x_pub":      <bytes[32]: this node's X25519 public key>,
  "node_sign_priv":  <bytes[32]: this node's Ed25519 private key (signing)>,
  "node_sign_pub":   <bytes[32]: this node's Ed25519 public key>,
  "node_cert":       <bytes[~138]: deployment-CA certificate binding
                      node_id || node_x_pub || node_sign_pub || expires_at,
                      signed by the deployment CA (HKDF(root_secret,
                      "Rimba-v1-CA-signing"))>,

  "checksum":        <uint16: CRC-16/CCITT of all preceding bytes>
}
```

**Mandatory cryptographic provisioning.** Every node — regardless of role or
starting `e2e_profile` — is provisioned at staging with its own keypairs and a
deployment-CA certificate. This makes every node FULL_TRUE-capable from first
boot, even while the deployment runs as HYBRID (the capability is dormant, not
absent). It is the key operational decision that makes profile migration cheap:
because the keys and certificate are already in NVS, moving HYBRID → FULL_TRUE
is a policy flip rather than an over-the-air key-distribution campaign
(Section 10.5). There is no over-the-air certificate-issuance path — capability
is established once, at staging, for the life of the node.

```
Why mandatory, not optional:
  - Costs nothing while dormant (~270 B of NVS)
  - Eliminates the hard part of any future HYBRID → FULL_TRUE upgrade
  - Removes the need for a gateway-side CA-signing / OTA cert-delivery
    subsystem entirely (a significant simplification)
  - Makes HYBRID ⇄ FULL_TRUE a reversible config toggle
  - An isolated node is FULL_TRUE-capable immediately on deployment,
    with no dependency on reaching a gateway first
```

Optional fields use their documented defaults when omitted. `peer_key_lifetime_s` is stored as seconds in the payload for operator readability and converted to `PEER_KEY_LIFETIME_MS` internally. `sleep_opt` sets the initial value of the `relay_sleep_opt` / `leaf_sleep_opt` NVS key (Section 12.10 / 12.11); it may be changed at runtime thereafter. The mandatory cryptographic fields (`e2e_profile`, `gateway_pub_key`, `cloud_pub_key`, the four `node_*` keys, and `node_cert`) have no defaults — a provisioning payload missing any of them is rejected.

#### 3.6.6 Provisioning Data Storage

```
Storage: ESP32-S3 NVS (Non-Volatile Storage) flash partition
Encryption: AES-256 NVS encryption (enabled via ESP-IDF eFuse)
Namespace: "rimba_prov"

Keys stored:
  rimba_prov/network_id      uint32
  rimba_prov/channel         uint8
  rimba_prov/psk             blob[16]
  rimba_prov/root_secret     blob[32]
  rimba_prov/role            uint8
  rimba_prov/leaf_sleep_ms   uint32
  rimba_prov/min_tx_dbm      int8
  rimba_prov/max_tx_dbm      int8
  rimba_prov/jitter_applied  uint8  (0 = not yet, 1 = applied)
  rimba_prov/ble_privkey     blob   (ECDH private key for commissioning)
  rimba_prov/ble_fingerprint blob[32]

  E2E (mandatory):
  rimba_prov/e2e_profile     uint8  (0=HYBRID, 1=FULL_TRUE)
  rimba_prov/gateway_pub     blob[32]
  rimba_prov/cloud_pub       blob[32]
  rimba_prov/node_x_priv     blob[32]   (AES-256 NVS-encrypted)
  rimba_prov/node_x_pub      blob[32]
  rimba_prov/node_sign_priv  blob[32]   (AES-256 NVS-encrypted)
  rimba_prov/node_sign_pub   blob[32]
  rimba_prov/node_cert       blob[~138]
```

The private keys (`node_x_priv`, `node_sign_priv`, `root_secret`, `ble_privkey`)
are protected by AES-256 NVS encryption (ESP-IDF eFuse). They never leave the
device. The certificate and public keys are not secret and need no additional
protection beyond the NVS namespace.

The BLE key pair is generated once and persists across reboots. This ensures the QR code fingerprint remains valid for the node's lifetime.

#### 3.6.7 First-Boot Sequence

```
Power on
  ↓
Read NVS: provisioning data present and checksum valid?
  │
  ├── NO → Commissioning Mode (Section 3.6.3)
  │         ↓
  │         Receive valid provisioning payload
  │         Write to NVS
  │         Generate LEAF_INITIAL_JITTER_MS (random, store to NVS)
  │         Reboot
  │
  └── YES → Read provisioning data into RAM
            Check jitter_applied flag:
              0 → sleep for LEAF_INITIAL_JITTER_MS, then set flag=1
              1 → proceed immediately
            Join IBSS (using network_id, channel)
            Begin normal Rimba protocol operation
```

#### 3.6.8 Factory Reset

To return a node to Commissioning Mode:

**Hardware button (if present):**
```
Hold FACTORY_RESET_GPIO low for > 5 seconds during boot.
Node wipes provisioning NVS namespace.
Reboots into Commissioning Mode.
BLE key pair is preserved (QR code remains valid).
```

**Over-the-air (for remote nodes):**
```
Gateway sends a factory-reset bundle addressed to the target node:
  dst_eid: ipn:<node_id>.4  (network management service)
  payload: {cmd: "factory_reset", token: <32-byte auth token>}

The auth token must match a deployment-wide reset token
derived from: HKDF(root_secret, node_id, "Rimba-v1-reset")
This ensures only a party with the root_secret can trigger
remote factory reset.

Node receives bundle, verifies token, wipes NVS, reboots.
```

#### 3.6.9 Deployment Workflow

```
Pre-deployment (staging / office):
  1. Deployment operator generates deployment config:
       - Selects channel (check local spectrum conditions)
       - Generates network_id (random uint32)
       - Generates PSK (random 16 bytes)
       - Generates root_secret (random 32 bytes)
       - Creates deployment.json config file
       - Securely shares config with installers

  2. Bulk provisioning (optional):
       rimba-provision flash --config deployment.json \
           --role relay --port /dev/ttyUSB0
       Repeat for each node. Takes ~5 seconds per node.

  3. Print QR code labels for any nodes NOT bulk-provisioned
     in staging (will be field-provisioned via BLE).

Field deployment:
  4. Installer carries phone with Rimba Provision app
     loaded with deployment.json (PSK + root_secret included).

  5. For each node:
       a. Mount node at installation point.
       b. Power on node.
       c. If commissioning LED blinks: scan QR code on node label.
       d. App connects via BLE, verifies fingerprint, sends payload.
       e. Node reboots, LED stays off: provisioning complete.
       f. Wait for node to appear in network dashboard.

  6. If node was bulk-provisioned in staging:
       Skip steps c–e. Node goes directly to normal operation.
```

### 3.7 Mobile Node Limitations

Rimba is designed for **static sensor deployments**. Mobile nodes (leaves or relays that physically move) are a supported edge case for slow-moving assets but are not the primary design target. This section defines what works, what breaks, and what is explicitly out of scope for v1.

#### 3.7.1 Mobile Leaf

A mobile leaf is a leaf node attached to an asset that moves — an animal, worker, slow vehicle, or drone. The node's protocol behaviour is unchanged; what changes is whether a relay is in range on each scheduled wake.

**Speed limit**

The mobile leaf must find a relay within its wake window. The maximum sustainable speed depends on relay spacing and `LEAF_SLEEP_MS`:

```
Maximum speed = relay_spacing_m / (LEAF_SLEEP_MS / 1000 / 60) × 0.5
                                                              ↑
                                                 safety margin (50%)

At 500m relay spacing:
  LEAF_SLEEP_MS = 900,000ms (15 min):   ~2 km/h  (slow walk)
  LEAF_SLEEP_MS = 180,000ms  (3 min):   ~5 km/h  (brisk walk)
  LEAF_SLEEP_MS  = 60,000ms  (1 min):  ~15 km/h  (cycling)
  LEAF_SLEEP_MS  = 15,000ms (15 sec):  ~60 km/h  (vehicle)

For deployments with different relay spacing, scale proportionally.
```

**Battery impact of shortened wake interval**

Using the standard leaf power formula `avg_mA = 0.015 + 4.585 / T` (T in seconds):

```
LEAF_SLEEP_MS   Avg current   3,000 mAh battery life   Speed supported
────────────────────────────────────────────────────────────────────────
900,000ms (15m)   20 µA        17 years (no sensor)     ≤ 2 km/h
180,000ms  (3m)   41 µA         8.6 years               ≤ 5 km/h
 60,000ms  (1m)   91 µA         3.75 years              ≤ 15 km/h
 15,000ms (15s)  321 µA         1.1 years               ≤ 60 km/h
```

This cost is **unavoidable** — it is a consequence of physics (more wakes = more battery), not of protocol design.

**Additional protocol overhead on top of wake frequency**

Mobile leaves pay extra costs beyond the wake frequency increase:

```
Source of overhead           Extra current        Occurs when
─────────────────────────────────────────────────────────────────────
New relay ECDH exchange       +200ms at 12mA       First contact with
  (PEER_OPEN/CONFIRM)                              each new relay
Extended scan window          +1,800ms at 12mA     No relay found in
  (isolation backoff)                              200ms scan window
```

At 5 km/h (3-min wake interval), these add approximately:

```
ECDH (1 in 3 cycles):      200ms × 12mA / 3 / 180s =  4.4 µA
Scan extension (1 in 5):  1800ms × 12mA / 5 / 180s = 24.0 µA
─────────────────────────────────────────────────────────────
Protocol overhead total:                            ~28.4 µA
Wake frequency alone:                               ~21.0 µA
Combined (vs 20 µA static):                         ~72 µA

Battery life at 5 km/h with protocol overhead: ~4.8 years
  (vs 8.6 years from wake frequency alone — overhead adds ~45%)
```

The extended scan window (isolation backoff) dominates the protocol overhead. When a mobile leaf crosses a coverage gap between relays, it spends up to 2,000ms scanning at 12mA finding nothing. GPS-equipped leaves can avoid this by skipping scans when predicted position is between relays.

**What works today (no protocol changes required)**

```
✅ Isolation backoff (LEAF_SLEEP_MS doubles on consecutive misses)
✅ Extended scan window (grows to 2,000ms when isolated)
✅ RREQ/RREP re-discovers routes at each new relay
✅ DTN — relay stores bundles even if leaf doesn't return
✅ Leaf EID unchanged across all relays (MAC-derived Node ID)
✅ Alert sensor interrupt — wakes immediately regardless of location
```

**What breaks or degrades**

```
✗ Downlink to mobile leaf: bundle goes to last known relay.
  If leaf has moved to different relay, bundle is undelivered.
  No leaf location service exists in v1.

✗ Peer link ECDH per new relay: 200ms battery penalty each time.
  Mitigated by longer PEER_KEY_LIFETIME_MS (7 days) for revisited relays.

✗ TDMA schedule at old relay expires: relay removes leaf after
  RELAY_LEAF_MISS_MAX consecutive misses. No data loss — leaf re-registers
  at next contact — but adds ~200ms overhead for HELLO renegotiation.
```

**Mitigation: longer peer key lifetime**

For mobile leaves that follow predictable routes (animal territory, vehicle route), extending `PEER_KEY_LIFETIME_MS` reduces ECDH overhead on relay revisits:

```
Default PEER_KEY_LIFETIME_MS = 86,400,000 (24h)
Mobile leaf recommendation:    604,800,000 (7 days)

Leaf revisiting same relay within 7 days: uses cached key → 0ms overhead
Leaf visiting new relay: still pays ECDH once → cached for 7 days
```

Set at provisioning via `peer_key_lifetime_s` in the provisioning payload (Section 3.6.5).

**Mobile leaf deployment recommendations**

```
Asset speed      LEAF_SLEEP_MS   Battery (1× 18650)   Recommendation
────────────────────────────────────────────────────────────────────
< 2 km/h (walk)  900,000ms       ~5 years (w/ sensor)  Standard config
2–5 km/h         300,000ms       ~3 years              Reduce interval
5–15 km/h         60,000ms       ~1.5 years            Add solar or 2× 18650
15–60 km/h        15,000ms       < 1 year              Use vehicle power
> 60 km/h        Not viable      —                     Cellular IoT instead
```

**Mobile leaf in Continuous mode (powered assets)**

The limitations above apply only to Scheduled mode leaves. A leaf running Continuous mode (Section 12.11) eliminates most mobile leaf constraints at the cost of higher power draw (~14 mA). This is only practical for leaves with a vehicle battery, solar, or permanent power supply.

```
Limitation              Scheduled mobile    Continuous mobile
──────────────────────────────────────────────────────────────
Speed limit             ~2 km/h (15-min)    None — always listening,
                                            connects immediately on
                                            entering relay range
Battery penalty/speed   Severe — shorter    None — 14 mA regardless
                        wake = more power   of speed
Scan window overhead    Major (2,000ms at   Eliminated — no scan
  (no relay found)      12 mA each miss)    window concept
ECDH on new relay       Extra active time   No extra cost — already
                        per new encounter   at full power
Downlink reliability    Poor — bundle sent  Improved — leaf always
                        to last known relay associated with current relay
                        (may be wrong)      (but location service still
                                            needed for guaranteed delivery)
Isolation backoff       Yes — sleep         Not applicable
                        doubles on miss
Battery life            1.5–17 years        ~9 days (3,000 mAh)
Suitable for            Slow battery        Powered mobile assets
                        assets (animals,    (vehicles, powered drones,
                        workers)            tracked equipment)
```

Continuous mobile leaf deployments do not require adjusting `LEAF_SLEEP_MS` for speed. The node is always reachable and always exchanging data as soon as a relay is in range. The only remaining limitation is downlink delivery, which requires a leaf location service (out of scope, deferred — same as Scheduled mode).

#### 3.7.2 Mobile Relay

A mobile relay (vehicle-mounted, drone relay, repositionable node) is significantly more disruptive than a mobile leaf because it serves as infrastructure for leaves and backbone routing.

**What happens when a relay moves**

```
Relay R at position P (stationary, K=6 backbone peers, 50 leaves):

  R begins moving:
    → RSSI to backbone peers degrades
    → Adaptive TX power increases to compensate

  After 30–60 seconds of movement:
    → Backbone peer links fail (NEIGHBOR_TIMEOUT)
    → RERR sent for all routes through failed peers
    → connectivity_state: CONNECTED → MESH_ONLY → ISOLATED
    → 50 leaves are now orphaned (their relay is gone)

  At new location:
    → R broadcasts HELLO
    → New nearby relays establish PEER_OPEN
    → RREQ/RREP rediscovers routes through new backbone
    → connectivity_state rebuilds over 30–120 seconds
    → Leaves orphaned at old location must find new relays
      (1–5 wake cycles = 15–75 minutes of data gap)
```

**RREQ storms from topology change**

Each time a mobile relay disconnects from a backbone peer, RERR is generated for every route through that peer. With K=6 and 100 routes, a single topology change can generate up to 600 RERR messages. If the relay is moving continuously, this repeats every few minutes, causing significant routing overhead.

The gossip flood control (Section 8.7.1) and adaptive jitter (Section 13.3.2) reduce but do not eliminate this overhead.

**Geographic routing with moving relay**

If the relay has GPS, its position in other nodes' `position_table` becomes stale as it moves. Other relays may route toward the old position until the stale entry expires (`POSITION_MAX_AGE_S = 86,400s`).

Mitigation: mobile relay sends triggered HELLO when GPS detects > 50m movement since last HELLO, resetting Trickle and updating all nearby nodes' position tables. This conflicts with Trickle suppression (mobile relay continuously resets the Trickle doubling), so mobile relays should be configured to send HELLO at base interval without Trickle suppression.

**Why mobile relay is not recommended in v1**

The leaf orphaning problem has no clean solution in v1. When the mobile relay moves away from its 50 leaves, those leaves experience a 15–75 minute data gap while they discover new relays. Solving this requires a relay handoff protocol:

```
Relay handoff (out of scope v1):
  Mobile relay detects departure (GPS or signal degradation)
  Announces each peered leaf to nearby stationary relays:
    LEAF_HANDOFF {leaf_id, next_wake_ms, session_key_material}
  Nearby relay adds leaf to its TDMA schedule
  Leaf wakes → finds handoff relay → seamless transition

Why this is complex:
  - New LEAF_HANDOFF frame type required
  - Forwarding session key material breaks per-pair security model
    (requires re-keying or trusted relay-to-relay channel)
  - Mobile relay must detect it's moving (GPS required)
  - Handoff must complete before relay leaves radio range (~30s window)
  - 8–12 weeks implementation estimate
```

**Use a dedicated mule instead**

A mobile relay is essentially an uncoordinated mule. It carries its DTN bundle store as it moves and reconnects when it finds backbone coverage. However:

```
Mobile relay vs dedicated mule:
  Mobile relay: confuses routing, orphans leaves, causes RREQ storms
  Dedicated mule: clean custody protocol, no routing disruption,
                  no leaves to orphan, explicit handshake

For any use case that involves physically carrying data between
zones: use the dedicated mule protocol (Section 9.5).
Mobile relay is not recommended in v1.
```

#### 3.7.3 Summary

| | Mobile leaf (Scheduled, slow) | Mobile leaf (Scheduled, vehicle) | Mobile leaf (Continuous, powered) | Mobile relay |
|---|---|---|---|---|
| Speed | < 5 km/h | 5–60 km/h | Any speed | Any |
| Leaf sleep mode | Scheduled | Scheduled (shorter interval) | Continuous | N/A |
| Works? | ✅ Yes | ⚠️ Reduced battery | ✅ Yes (powered) | ❌ Not recommended |
| Speed limit | ~2 km/h (15-min) | ~60 km/h (15-sec) | None | N/A |
| Primary cost | Wake frequency | Wake frequency + overhead | 14 mA fixed draw | Leaf orphaning |
| Battery impact | 2–3× worse | 4–15× worse | ~9 days only | N/A (powered) |
| Downlink | ❌ Unreliable | ❌ Unreliable | ⚠️ Improved, not guaranteed | N/A |
| Protocol changes | None (param only) | None (param only) | None (mode switch only) | Handoff protocol |
| Requires | Battery | Battery + solar/2× cell | Vehicle/mains power | Dedicated mule instead |

**Out of scope (all mobile leaf modes):**

```
✗ Leaf location service (downlink routing to mobile leaf at current relay)
✗ GPS-aware scan skipping (skip scan when between relays — Scheduled only)
✗ Relay handoff protocol (leaf re-association on relay departure)
✗ Contact graph routing for predictable mobile paths (CGR)
✗ Fast roaming (< 1s handoff) for vehicular mesh
```

These are deferred or better handled by specialised protocols (LTE Cat-M1, NB-IoT) for high-speed mobile assets.

#### 3.7.4 Mobile Node Parameters at Provisioning

Mobile leaves must be provisioned with speed-appropriate parameters:

| Parameter | Static default | Mobile 5 km/h | Mobile 15 km/h |
|---|---|---|---|
| `LEAF_SLEEP_MS` | 900,000 (15 min) | 180,000 (3 min) | 60,000 (1 min) |
| `PEER_KEY_LIFETIME_MS` | 86,400,000 (24h) | 604,800,000 (7 days) | 604,800,000 (7 days) |
| `LEAF_SCAN_WINDOW_MS` | 200ms | 400ms | 600ms |
| Recommended battery | 1× 18650 | 2× 18650 | Vehicle power |
| Recommended solar | 0.5W+ | 0.5W+ | Not needed |

The provisioning app (Section 3.6) should offer a "mobile leaf" configuration template that sets these parameters based on the expected deployment speed. Continuous mode (Section 12.11) is an alternative to shortening `LEAF_SLEEP_MS` for powered mobile assets — it removes the speed limit entirely at the cost of higher current draw.

### 3.8 Data Flow Patterns

All communication in Rimba is bundle-based (BPv7). Every sensor reading,
command, alert, or status message is wrapped in a bundle before transmission.
Bundles are encrypted end-to-end (BPSec, source → destination) with additional
hop-by-hop link encryption on every peer link.

#### Network topology overview

```
                 ┌─────────────────────────────┐
                 │       CLOUD / INTERNET       │
                 └──────────────┬──────────────┘
                                │ uplink
                 ┌──────────────▼──────────────┐
                 │           Gateway           │
                 └──────┬───────────────┬──────┘
                        │               │
              OGM / RREQ backbone mesh  │
      ┌─────────────────┤               │
      │                 │               │
┌─────▼──────┐   ┌──────▼─────┐   ┌────▼───────────┐
│  Relay R1  │───│  Relay R2  │   │  Relay R3      │
│ CONNECTED  │   │ CONNECTED  │   │  ISOLATED      │
└──────┬─────┘   └──────┬─────┘   └────┬───────────┘
       │                │               │
  ┌────▼────┐      ┌────▼────┐     ┌────▼────┐
  │ Leaf L1 │      │ Leaf L2 │     │ Leaf L3 │
  └─────────┘      └─────────┘     └─────────┘

                    ┌──────────┐
                    │   Mule   │ sweeps between R3 and GW
                    └──────────┘
```

---

#### 3.8.1 Leaf → Gateway (Uplink)

**Path A — Connected relay**

```
  ┌──────┐  ① DATA   ┌──────────┐  ② forward   ┌─────────┐
  │ Leaf │──────────►│  Relay   │─────────────►│ Gateway │──► Internet
  └──────┘           │CONNECTED │               └─────────┘
                     └──────────┘
  ① Leaf wakes on RTC alarm. Sends DATA to peer relay.
  ② Relay looks up GATEWAY_ANYCAST_ID, forwards hop-by-hop.
  Latency: LEAF_SLEEP_MS + ~450ms   (default ~7.5 min)
```

**Path B — Multi-hop mesh**

```
  ┌──────┐   ┌────┐   ┌────┐   ┌────┐   ┌─────────┐
  │ Leaf │──►│ R1 │──►│ R2 │──►│ R3 │──►│ Gateway │──► Internet
  └──────┘   └────┘   └────┘   └────┘   └─────────┘

  Each relay forwards based on GATEWAY_ANYCAST_ID forwarding table.
  Routing: OGM (small mesh) or RREQ/RREP (large mesh).
  Latency: LEAF_SLEEP_MS + (hop_count × ~110ms)
```

**Path C — Isolated relay, mule delivery**

```
  ┌──────┐  ① DATA  ┌────────────┐
  │ Leaf │─────────►│   Relay    │
  └──────┘           │ (ISOLATED) │
                     │            │
                     │ ② DTN store│
                     └─────┬──────┘
                            │ ③ CUSTODY_REQ
                     ┌──────▼──────┐
                     │    Mule     │ sweeps past
                     └──────┬──────┘
                            │ ④ delivers
                     ┌──────▼──────┐
                     │   Gateway   │──► Internet
                     └─────────────┘

  ① Leaf deposits bundle at relay R (normal DATA frame).
  ② R is ISOLATED — stores bundle in DTN flash store.
  ③ Mule arrives: R sends CUSTODY_REQ MANIFEST (Section 9.5).
  ④ Mule physically carries bundles to gateway.
  Latency: mule sweep interval (hours–days)
```

**Path D — ALERT fast path**

```
  [Sensor]            ┌──────┐  ALERT   ┌───────┐  immediate  ┌─────────┐
  threshold ──IRQ────►│ Leaf │─────────►│ Relay │────────────►│ Gateway │
  breached            └──────┘  no wait  └───────┘  no queue   └─────────┘
                                no RTC
  Leaf wakes immediately from sensor GPIO interrupt (no sleep interval wait).
  ALERT flag bypasses aggregation and TDMA slot wait at every relay.
  Latency: < 500ms from sensor trigger (connected path)
  Latency: DTN store → mule (isolated, but ALERT never evicted, collected first)
```

---

#### 3.8.2 Gateway → Leaf (Downlink)

**Path A — Proxy RREP, Scheduled leaf**

```
              ① RREQ for leaf_id (flood)
  ┌─────────┐─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─►┌─────────┐
  │ Gateway │                           │  Relay  │◄── leaf peer link
  │         │◄─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ └────┬────┘
  │         │  ② proxy RREP                  │ ③ DTN buffer
  │         │──────────────────────────►      ▼
  └─────────┘  ③ DATA (dst=leaf_id)    ┌─────────┐
                                       │  Leaf   │ ← delivered on next wake
                                       └─────────┘

  ① Gateway issues RREQ for leaf node ID.
  ② Leaf's relay responds with proxy RREP (Section 8.3).
  ③ Gateway routes bundle to relay → relay buffers → leaf collects on wake.
  Latency: ~LEAF_SLEEP_MS (default ~7.5 min)
```

**Path B — Proxy RREP, Continuous leaf**

```
              ① RREQ for leaf_id
  ┌─────────┐─ ─ ─ ─ ─ ─►┌─────────┐
  │ Gateway │              │  Relay  │◄── always-on peer link
  │         │◄─ ─ ─ ─ ─ ─ └────┬────┘
  │         │ ② proxy RREP      │ ③ immediate delivery
  │         │──────────────►     ▼
  └─────────┘               ┌─────────┐
                             │  Leaf   │ (always listening)
                             └─────────┘

  Latency: RREQ discovery (~500ms) + routing hops
```

**Path C — Isolated relay, mule carries downlink**

```
  ┌─────────┐  ① no route    ┌──────────────────────┐
  │ Gateway │──to leaf relay──►  Gateway DTN store   │
  └────┬────┘                 └──────────────────────┘
       │ ② Mule departs                │
       │   collects from GW            ▼
  ┌────▼────┐               Mule carries bundle
  │  Mule   │──────────────────────────────────────►
  └────┬────┘
       │ ③ delivers to isolated relay
  ┌────▼──────────┐   ④ leaf wakes   ┌─────────┐
  │ Relay (isol.) │─────────────────►│  Leaf   │
  └───────────────┘                  └─────────┘

  Latency: mule sweep interval + LEAF_SLEEP_MS
```

---

#### 3.8.3 Relay → Relay

```
  ┌────────┐  OGM / RREQ   ┌────────┐  OGM / RREQ   ┌────────┐
  │ Relay  │──────────────►│ Relay  │──────────────►│ Relay  │
  │   R1   │               │   R2   │               │   R3   │
  └────────┘               └────────┘               └────────┘

  All relays are full IBSS participants and routing peers.
  Bundles forwarded toward best next-hop per forwarding table.
  Latency: ~110ms per hop
```

---

#### 3.8.4 Relay → Leaf (targeted delivery)

```
              ① RREQ for leaf_id (flood)
  ┌────────┐─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─►┌──────────┐
  │ Relay  │                           │  Relay   │◄── leaf peer
  │   R1   │◄─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ └────┬─────┘
  │        │  ② proxy RREP                  │ ③ buffer / deliver
  │        │──────────────────────────►      ▼
  └────────┘  ③ DATA (dst=leaf_id)    ┌──────────┐
                                      │   Leaf   │
                                      └──────────┘

  Any relay can reach any leaf — not just the leaf's own relay.
  Same proxy RREP mechanism as gateway downlink.
  Latency: LEAF_SLEEP_MS (Scheduled) or immediate (Continuous)
```

---

#### 3.8.5 Leaf → Leaf (peer-to-peer)

```
  ┌────────┐  ① DATA     ┌──────┐
  │ Leaf A │────────────►│  R_A │
  └────────┘  dst=leaf_B  └──┬───┘
                              │ ② RREQ for leaf_B_id
                    ─ ─ ─ ─ ─ ▼ ─ ─ ─ ─ ─►
                              mesh flood
                    ◄─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─
                              │ ③ proxy RREP from R_B
                         ┌────▼───┐   ④ DATA     ┌────────┐
                         │  R_A   │─────────────►│  R_B  │
                         └────────┘               └───┬────┘
                                                      │ ⑤ deliver
                                                 ┌────▼────┐
                                                 │ Leaf B  │
                                                 └─────────┘

  ① Leaf A sends bundle with dst = leaf_B_id to its relay R_A.
  ② R_A has no route to leaf_B → issues RREQ.
  ③ R_B (leaf B's relay) responds with proxy RREP (Section 8.3).
  ④ R_A routes bundle through mesh to R_B.
  ⑤ R_B delivers to Leaf B on next wake (Scheduled) or immediately (Continuous).
  Latency: up to 2 × LEAF_SLEEP_MS worst case (both leaves asleep)
```

---

#### 3.8.6 Relay/Leaf → Mule (custody transfer)

```
  ┌──────┐  DATA  ┌────────────┐  ① CUSTODY_REQ   ┌──────┐
  │ Leaf │───────►│   Relay    │  (MANIFEST)  ────►│      │
  └──────┘        │ (ISOLATED) │                   │ Mule │
                  │            │◄─────────────────  │      │
                  │            │  ② CUSTODY_ACK    │      │
                  │            │  (ACCEPT)         │      │
                  │            │                   │      │
                  │            │──DATA (bundle)───►│      │
                  │            │◄──CUSTODY_ACK──── │      │
                  │            │   (CONFIRM)       │      │
                  │ release*   │                   │      │
                  └────────────┘   repeat per      └──────┘
                                   bundle

  Priority: ALERT bundles first, then oldest NORMAL.
  * On CONFIRM the relay RELEASES the bundle per its retention mode
    (EAGER: delete now; RETAINED: keep until end-to-end ack). It never
    releases an unconfirmed bundle.
  If mule departs mid-transfer: relay retains unconfirmed bundles.
  See Section 9.5 (and 9.5.7 for EAGER/RETAINED) for full protocol.
```

---

#### 3.8.7 Mule → Gateway (delivery)

```
  ┌──────┐  ① CUSTODY_REQ  ┌─────────┐
  │      │  (MANIFEST:      │         │
  │ Mule │  all collected   │ Gateway │
  │      │  bundles)   ────►│         │
  │      │                  │         │
  │      │◄─────────────── │         │
  │      │  ② CUSTODY_ACK  │         │
  │      │  (ACCEPT)        │         │
  │      │                  │         │
  │      │──DATA (bundles)─►│         │──► Internet (deduplicated)
  │      │◄──CONFIRM──────  │         │
  └──────┘  per bundle      └─────────┘

  Gateway deduplicates against bundles already received via mesh.
  Mule's own telemetry (GPS trail, battery, store occupancy) drains here too.
  See Section 9.5.6 for deduplication.
```

---

#### 3.8.8 Summary Matrix

| Source | Destination | Diagram | Mechanism | Latency |
|---|---|---|---|---|
| Leaf | Gateway | 3.8.1 Path A | DATA → relay → GW | ~7.5 min |
| Leaf (ALERT) | Gateway | 3.8.1 Path D | GPIO wake → ALERT flag | < 500ms |
| Leaf (isolated) | Gateway | 3.8.1 Path C | DTN → mule → GW | Hours–days |
| Gateway | Leaf | 3.8.2 Path A | Proxy RREP → relay buffer | ~7.5 min |
| Gateway | Leaf (Continuous) | 3.8.2 Path B | Proxy RREP → immediate | < 1s |
| Gateway | Leaf (isolated) | 3.8.2 Path C | DTN → mule → relay → wake | Hours–days |
| Relay | Relay | 3.8.3 | OGM / RREQ mesh forwarding | ~110ms/hop |
| Relay | Leaf | 3.8.4 | Proxy RREP → buffer → wake | ~7.5 min |
| Leaf | Leaf | 3.8.5 | Proxy RREP via both relays | Up to ~30 min |
| Relay (isolated) | Mule | 3.8.6 | Custody transfer | While in range |
| Mule | Gateway | 3.8.7 | Custody transfer | While in range |
| Mule | Relay (downlink) | 3.8.7 | DATA delivery during sweep | While in range |
---

## 4. Protocol Stack

All nodes run the same protocol stack. Leaves use a reduced subset (marked below).

```
┌────────────────────────────────────────────────────────────┐
│  Application        CBOR-encoded sensor payload             │
├────────────────────────────────────────────────────────────┤
│  E2E Security       BPSec (RFC 9172) + COSE AEAD            │
│                     ChaCha20-Poly1305 or AES-128-CCM        │
│                     Source node → destination node          │
├────────────────────────────────────────────────────────────┤
│  DTN / Bundle       BPv7 (RFC 9171) · ipn: scheme           │
│                     Store-carry-forward · custody transfer  │
├────────────────────────────────────────────────────────────┤
│  Mesh Routing       Reactive RREQ/RREP (> 100 nodes)        │
│                     Proactive OGM (≤ 100 nodes, Appendix A) │
│                     [Leaf: DATA only, no re-flood]          │
├────────────────────────────────────────────────────────────┤
│  Mesh Frame         Custom header · EtherType 0x88B5        │
│                     src · dst · next_hop · TTL · seq · type │
├────────────────────────────────────────────────────────────┤
│  Peer Links         HELLO · PEER open/confirm               │
│                     Local TDMA sleep schedule               │
│                     [Leaf: minimal HELLO, 1–2 peer links]   │
├────────────────────────────────────────────────────────────┤
│  Link Security      Per-peer ECDH session key · AES-128-CCM │
│                     Hop-by-hop on all links                 │
├────────────────────────────────────────────────────────────┤
│  802.11ah IBSS      MM6108 · MORSE_CMD_INTERFACE_TYPE_ADHOC │
│                     Custom sleep scheduling (no TWT)        │
└────────────────────────────────────────────────────────────┘
```

---

## 5. Physical and Link Layer

### 5.1 IBSS Mode

All Rimba nodes operate in 802.11ah IBSS mode. There is no AP and no STA. Every node can exchange frames directly with every other node it can physically hear.

| Parameter | Value |
|---|---|
| Interface type | `MORSE_CMD_INTERFACE_TYPE_ADHOC` |
| SSID | `rimba-<network_id>` where network_id is a 32-bit deployment identifier |
| Channel | Single channel, deployment-configured |
| Channel bandwidth | 1 MHz (maximum range) or 2 MHz (balanced) |
| BSSID | Deterministic from network_id: `02:RI:MB:<network_id_bytes>` |

All nodes in a deployment share the same SSID, BSSID, and channel.

### 5.2 Channel and Region

| Region | Frequency | Bandwidth |
|---|---|---|
| US (FCC) | 902–928 MHz | 1 or 2 MHz |
| EU (ETSI) | 863–868 MHz | 1 MHz |
| AU (ACMA) | 915–928 MHz | 1 or 2 MHz |
| JP (MIC) | 916.5–927.5 MHz | 1 MHz |

Channel selection is deployment-specific and must comply with local regulations. All nodes in a deployment use the same channel.

### 5.3 Transmit Power and Range

| Bandwidth | Open field range | Dense urban range |
|---|---|---|
| 1 MHz MCS0 | > 1000 m | 200–400 m |
| 2 MHz MCS0 | 500–800 m | 150–300 m |

### 5.4 Multi-Hop Forwarding

Each hop is an independent IBSS frame exchange between peers. The Rimba mesh header carries end-to-end addressing; the 802.11ah MAC addresses change per hop.

```
Hop Continuous→Scheduled:  802.11ah src=A_mac, dst=B_mac
          Rimba header: src_node=A, dst_node=D, next_hop=B, TTL=n

B receives, looks up dst_node in forwarding table, re-sends:
Hop B→C:  802.11ah src=B_mac, dst=C_mac
          Rimba header: src_node=A, dst_node=D, next_hop=C, TTL=n-1
```

---

## 6. Mesh Frame Format

### 6.1 Ethernet Encapsulation

All Rimba frames are carried as Ethernet II frames with EtherType `0x88B5`:

```
 ┌─────────────────────────────────────┐
 │  Destination MAC  (6 bytes)          │  next-hop node's MAC
 ├─────────────────────────────────────┤
 │  Source MAC       (6 bytes)          │  this node's MAC
 ├─────────────────────────────────────┤
 │  EtherType = 0x88B5  (2 bytes)       │
 ├─────────────────────────────────────┤
 │  Rimba Mesh Header  (26 bytes)       │
 ├─────────────────────────────────────┤
 │  Payload            (variable)       │
 └─────────────────────────────────────┘
```

### 6.2 Rimba Mesh Header (26 bytes)

```
 0               1               2               3
 0 1 2 3 4 5 6 7 0 1 2 3 4 5 6 7 0 1 2 3 4 5 6 7 0 1 2 3 4 5 6 7
 ├───────────────┬───────────────┬───────────────┬───────────────┤
 │ Version (4)   │  Type (4)     │   Flags (8)   │    TTL (8)    │
 ├───────────────┴───────────────┴───────────────┴───────────────┤
 │              Sequence Number (16 bits)        │   Reserved    │
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
| 3 | `AGG` | Payload is an aggregate of multiple bundles (Section 9.7) |
| 2 | `ALERT` | High-priority bundle — do not aggregate, forward immediately (Section 9.8) |
| 1–0 | Reserved | Must be zero |

**TTL**: Default 16. Decremented each hop. Frame discarded at zero.

**Sequence Number**: Per-source, monotonically increasing. Frames with a `(src_node, seq)` pair seen within 60 seconds are discarded for loop prevention.

### 6.3 Frame Types

| Type | Name | Nodes | Description |
|---|---|---|---|
| `0x1` | HELLO | All | Neighbor discovery beacon |
| `0x2` | PEER_OPEN | All | Initiate peer link |
| `0x3` | PEER_CONFIRM | All | Accept peer link |
| `0x4` | PEER_CLOSE | All | Tear down peer link |
| `0x5` | RREQ | Relay/GW/Mule | Route Request |
| `0x6` | RREP | Relay/GW/Mule | Route Reply (also sent by relay as proxy for peered leaf — Section 8.3) |
| `0x7` | RERR | Relay/GW/Mule | Route Error (also sent by relay when leaf peer link is lost — Section 8.6) |
| `0x8` | DATA | All | Mesh data frame carrying a bundle |
| `0x9` | ACK | All | Hop-level acknowledgment |
| `0xA` | TIME_SYNC | GW (relays re-flood) | Time synchronisation originated by gateway, re-flooded hop-by-hop |
| `0xB` | CUSTODY_REQ | All | Request custody transfer |
| `0xC` | CUSTODY_ACK | All | Confirm custody transfer |
| `0xD` | KEY_REQ | Leaf/Relay | Request a node's public key from relay key cache |
| `0xE` | KEY_RESP | Relay | Return a public key with signature to requesting leaf |
| `0xF` | OTA_START | Relay/GW/Mule → receiver | Begin firmware stream: target_version, image_size, image_hash |
| `0x10` | OTA_CHUNK | Relay/GW/Mule → receiver | One 4 KB image fragment: seq_num + data + CRC-16 |
| `0x11` | OTA_EOF | Relay/GW/Mule → receiver | End of firmware stream; receiver finalises and verifies |
| `0x12` | OTA_ACK | receiver → sender | Per-chunk ACK and final result (SUCCESS / SIG_FAIL / FLASH_ERR) |
| `0x13` | OTA_NACK | receiver → sender | Chunk retransmit request (bad CRC or missing seq) |

Leaf nodes send and receive only: HELLO, PEER_OPEN, PEER_CONFIRM, PEER_CLOSE, DATA, ACK, CUSTODY_REQ, CUSTODY_ACK, KEY_REQ, KEY_RESP, and (as OTA receivers) OTA_START, OTA_CHUNK, OTA_EOF, OTA_ACK, OTA_NACK. They do not send or re-flood RREQ, RREP, RERR, or TIME_SYNC.

The OTA frames (`0xF`–`0x13`) operate at **L2 (Data Link)** — they are
point-to-point link frames between a sender (gateway, relay, or mule) and one
receiver, carried in the same 802.11ah IBSS frame (EtherType `0x88B5`) with
hop-by-hop AES-128-CCM as any other frame. They are **not** BPv7 bundles. The
firmware they carry is authenticated end-to-end by the image's Ed25519
signature (verified after `OTA_EOF`), not by BPSec. By contrast, the OTA
*announce* (`payload_announce`) IS a BPv7 bundle (L4) — see the OSI table in
Section 14.1d.

---

## 7. Neighbor Discovery and Peer Links

### 7.1 HELLO Frames

HELLO is Rimba's neighbour-discovery and link-state heartbeat. Every node
broadcasts it (to the broadcast MAC, `dst_node = 0xFFFFFFFFFFFF`) so nearby
nodes learn it exists and that it is still alive. A single HELLO carries the
coordination state that, on a conventional network, would be split across ARP,
neighbour discovery, routing hello packets, a liveness heartbeat, and a key
announcement:

HELLO operates at **L2 (Data Link)** in the Rimba stack (Section 9.0 OSI
mapping) — it is a link-local broadcast that is never forwarded or routed. It
is carried directly in an 802.11ah IBSS frame with the Rimba mesh header
(EtherType `0x88B5`), not inside a BPv7 bundle. Each HELLO is link-encrypted
hop-locally like any other frame; it reaches only direct radio neighbours and
is consumed there (no TTL, no re-broadcast). This is what makes it pure
neighbour discovery: it describes the sender to whoever can hear it directly,
and goes no further.

```
What HELLO conveys:
  1. Presence / liveness  — "I exist, I'm alive." A node drops from a
                            neighbour table if no HELLO arrives within
                            NEIGHBOR_TIMEOUT.
  2. Identity & role      — node_id, role (gateway/relay/leaf/mule).
  3. Link-state           — peers, battery, tx_power; receiver measures
                            RSSI and ETX from the HELLO itself.
  4. Sleep coordination   — TDMA schedule + wake window, so a relay knows
                            when a sleeping leaf will be awake.
  5. Network health       — store_pct, relay_load_pct, uplink_load_pct,
                            connectivity, time_stale.
  6. Position (GPS nodes) — lat_e6, lon_e6, pos_age_s for geo routing.
  7. E2E key distribution — node_pub_key, node_cert (per hello_key_adv).
  8. OTA campaign state   — sw_version, ota_pending, ota_ready, ota_error.
  9. Special signals      — MULE flag (custody available now), storage_kb
                            (free capacity), and mule-state signals for
                            genuine mule-side unavailability (storage full,
                            going offline for self-update). Window-closing
                            as a mule sweeps past is NOT advertised — the
                            relay infers it from the mule's HELLO RSSI trend
                            (Section 9.5.9).
```

**HELLO payload** (CBOR):

```
{
  "node_id":         <6-byte EUI-48>,
  "role":            <uint8: 0=gateway, 1=relay, 2=leaf, 3=mule (superset of relay)>,
  "seq":             <uint16>,
  "battery":         <uint8: 0–100%, 255=wired/solar>,
  "tx_power":        <int8: dBm>,
  "peers":           <list of node_ids currently peered with>,
  "flags":           <uint8: reserved, must be zero>,
  "time_stale":      <bool: true if now - last_timesync > TIMESYNC_STALE_S>,
  "store_pct":       <uint8: 0–100, bundle store utilisation>,
  "config_version":  <uint32: monotonic config version applied — ordering, Section 9.9>,
  "config_hash":     <bytes[8]: truncated SHA-256 of applied config — integrity>,

  // Geographic position (optional — GPS-equipped nodes only)
  "lat_e6":          <int32: latitude × 10⁶, e.g. 37.422° → 37422000>,
  "lon_e6":          <int32: longitude × 10⁶, e.g. -122.084° → -122084000>,
  "pos_age_s":       <uint32: seconds since GPS fix — omit if no GPS>,
  "connectivity":    <uint8: 0=ISOLATED, 1=MESH_ONLY, 2=CONNECTED>,
  "relay_load_pct":  <uint8: 0–100, relay radio+storage load — relay role only>,
  "uplink_load_pct": <uint8: 0–100, internet uplink utilisation — gateway role only>,

  // Local TDMA schedule (for sleep coordination)
  "tdma_frame_ms":   <uint16: superframe duration in ms>,
  "tdma_slot_ms":    <uint16: slot duration in ms>,
  "tdma_tx_slot":    <uint8: this node's transmit slot>,
  "tdma_rx_slots":   <list of uint8: slots this node listens>,
  "tdma_mgmt_slots": <list of uint8: shared management slots>,

  // Wake window (for sleeping nodes)
  "wake_interval_ms": <uint32: how often this node wakes>,
  "next_wake_ms":     <uint32: ms until next wake window>,
  "wake_duration_ms": <uint16: how long this node stays awake>,

  // E2E encryption — key advertising governed by hello_key_adv (below)
  "node_pub_key":  <bytes[32]: X25519 long-term public key — see policy below>,
  "node_cert":     <bytes[~138]: deployment-CA certificate (Section 7.5.5)
                    — always throttled: sent every Nth HELLO, not every one>
}
```

**Key advertising policy (`hello_key_adv`).** Whether `node_pub_key` rides in
every HELLO is a config governed by the deployment's encryption profile, not a
fixed behaviour. This reconciles HELLO overhead with the warm/cold trade-off in
Section 10.5:

```
hello_key_adv:
  WARM  — node_pub_key in EVERY HELLO. Relay caches stay continuously
          populated → leaf-to-leaf key discovery is instant.
          Default for FULL_TRUE deployments (caches MUST be warm for
          node-to-node ECIES). Cost: +32 B per HELLO.

  COLD  — node_pub_key advertised only every Nth HELLO (default N: every
          cert interval), or omitted and served on demand via KEY_REQ.
          Default for HYBRID deployments: leaf-to-leaf is soft, so other
          nodes' public keys are not needed for routine traffic, and the
          backend keys (gateway/cloud) are provisioned rather than learned
          from HELLO. Saves ~32 B per HELLO.

Profile-driven defaults:
  e2e_profile = HYBRID    → hello_key_adv = COLD  (key on demand)
  e2e_profile = FULL_TRUE → hello_key_adv = WARM  (key every HELLO)

A HYBRID deployment planning to upgrade to FULL_TRUE may set WARM early
so caches are already populated, making the eventual profile flip instant
(Section 10.5, migration).
```

The certificate (`node_cert`) is **always throttled** regardless of
`hello_key_adv` — it is sent only every Nth HELLO (or on KEY_REQ), never in
every frame, because at ~138 B it would otherwise dominate HELLO size. A
receiver that has a node's `node_pub_key` but not its `node_cert` requests the
cert via KEY_REQ before trusting the key for encryption.

`node_pub_key` and `node_cert` originate from the mandatory staging provisioning
(Section 3.6.5) — every node has them from first boot. Relays build a key cache
from received HELLOs: `key_cache[node_id] = {pub_key, cert, cached_at}`. The
certificate — signed by the deployment CA — lets a receiver verify the key
genuinely belongs to the advertising node before encrypting to it (Section
7.5.5), defeating key-substitution (MITM) by a malicious relay.

Leaf nodes populate the wake window fields. Relay nodes populate the TDMA fields. Both sets are optional; senders omit fields irrelevant to their role.

**HELLO intervals** (Trickle-suppressed per RFC 6206):

| Role | Base interval | Max interval |
|---|---|---|
| Gateway | 2 s | 128 s |
| Relay | 5 s | 320 s |
| Leaf | On wake only | — |
| Mule | 2 s | 32 s |

Leaf nodes broadcast one HELLO on each wake cycle. They do not maintain a Trickle timer.

**How Trickle adapts the interval.** The base and max are the two ends of an
adaptive range, not a fixed period. Trickle (RFC 6206) lowers HELLO overhead
when the neighbourhood is stable and raises responsiveness when something
changes:

```
Stable neighbourhood (nothing changing):
  Interval DOUBLES each period, base → ... → max.
  Relay: 5s → 10s → 20s → 40s → ... → 320s.
  A settled relay broadcasts HELLO only every ~5 minutes — idle
  channel cost stays minimal.

A change occurs → interval RESETS to base:
  HELLO speeds back up so neighbours learn the change quickly,
  then doubles again as the neighbourhood re-stabilises.

What counts as a "change" that resets Trickle:
  - a new neighbour appears, or a neighbour is lost (NEIGHBOR_TIMEOUT)
  - a peer link opens or closes
  - a significant state change worth propagating promptly
    (e.g. connectivity transition, store crossing a threshold,
     position move > 50 m for a mobile node, OTA state change)
```

The role differences follow node constraints: a powered gateway (2 s base)
must be found quickly and can afford frequent HELLOs; a relay (5 s base, 320 s
max) keeps a stable backbone quiet; a mule (2 s base, low 32 s max) moves and
changes neighbours often, so it never stays quiet for long while in motion;
a leaf is battery-critical, so it sends exactly one HELLO per wake (one every
`LEAF_SLEEP_MS`, default 15 min) and runs no Trickle timer at all.

### 7.2 Neighbor Table

Each node maintains a neighbor table for all nodes from which a HELLO was received within `NEIGHBOR_TIMEOUT`:

| Field | Description |
|---|---|
| node_id | Peer node ID |
| mac | Peer 802.11ah MAC address |
| role | Peer role |
| rssi | Last received RSSI |
| lq | Link quality (ETX-based, 0–255) |
| last_seen | Timestamp of last HELLO |
| peer_state | None / Opening / Active / Closing |
| wake_schedule | Next wake time and duration (for sleeping peers) |

`NEIGHBOR_TIMEOUT` = 3× the peer's advertised HELLO max interval. For leaf nodes this is set to 3× `wake_interval_ms`.

### 7.3 Local TDMA Schedule

Relay nodes manage a local superframe for sleep coordination with their backbone peers. It is a per-neighbourhood agreement — not a global network schedule.

```
Superframe example (relay with K=6 peers, 20 slots × 100ms = 2s):

  Slot  0:   This relay TX
  Slots 1–6: Peer RX (one per direct peer)
  Slots 7–9: MGMT (RREQ/RREP/HELLO)
  Slots 10–19: SLEEP

  Relay duty cycle: 10/20 = 50% → ~7 mA average on radio
```

Peers learn each other's slots from the `tdma_*` fields in HELLO and wake only during relevant slots. Leaf wake windows interleave naturally with relay TDMA: relays schedule a receive slot coinciding with the leaf's advertised wake window.

### 7.4 Peer Link Establishment

All Rimba links — relay-to-relay, relay-to-leaf, leaf-to-relay — use the same PEER protocol. WPA3-SAE is not used; link security is handled entirely by Rimba.

```
Node A                              Node B
  │── PEER_OPEN ──────────────────>│
  │   nonce_a, pubkey_a (X25519)   │
  │   network_id, version          │
  │                                 │
  │<── PEER_CONFIRM ───────────────│
  │    nonce_b, pubkey_b (X25519)   │
  │    MIC(PSK, nonce_a||nonce_b)  │
  │                                 │
  │  session_key = HKDF-SHA256(     │
  │    ECDH(priv_a, pub_b),         │
  │    nonce_a||nonce_b,            │
  │    "Rimba-v1-session")          │
  │  Peer link ACTIVE               │
```

The MIC is computed over the network-wide Pre-Shared Key (PSK). Both nodes must know the PSK to verify. The PSK is never transmitted.

All subsequent frames between these two nodes are encrypted with AES-128-CCM using the session key. The nonce includes a per-frame counter for replay protection.

**Leaf peer link key resumption**: Leaves establish peer links with 1–2 nearby relays on first contact. The session key is stored in RTC memory (survives deep sleep). On subsequent wakes, the leaf checks whether the key is still within `PEER_KEY_LIFETIME_MS` (24 hours). If valid, the leaf skips PEER_OPEN/CONFIRM and sends DATA directly using the existing session key. If expired or absent, PEER_OPEN/CONFIRM runs before any DATA exchange. This bounds the ECDH cost to once per 24 hours rather than once per wake cycle. See Section 12.3 step 3 for the wake-cycle decision point.

### 7.5 Key Discovery Protocol

This section defines how a node obtains another node's X25519 public key
before encrypting a bundle to it (Phase 2 true E2E, Section 10.5).

#### 7.5.1 Relay Key Cache

Each relay maintains a key cache populated from two sources:

```
Key cache population:

  ┌─────────────────────────────────────────────────────────────┐
  │                      Relay R_A                              │
  │                                                             │
  │  key_cache:                                                 │
  │  ┌──────────────────────────────────────────────────────┐  │
  │  │ node_id_L1 → { pub_key, cert, cached_at }            │  │
  │  │ node_id_L2 → { pub_key, cert, cached_at }            │  │
  │  │ node_id_R2 → { pub_key, cert, cached_at }            │  │
  │  │ node_id_L5 → { pub_key, cert, cached_at }            │  │
  │  └──────────────────────────────────────────────────────┘  │
  │         ▲                              ▲                    │
  │         │                              │                    │
  └─────────┼──────────────────────────────┼────────────────────┘
            │                              │
    Source 1: HELLO                  Source 2: RREP
    (link-local)                     (mesh-wide)
            │                              │
   ┌────────┴────────┐           ┌─────────┴────────┐
   │  Leaf L1 sends  │           │  RREQ issued for │
   │  HELLO with     │           │  node_id_L5      │
   │  node_pub_key   │           │  → RREP carries  │
   │                 │           │    L5's pub_key  │
   └─────────────────┘           └──────────────────┘

Source 1 coverage: nodes whose HELLO R_A has directly received
                   (leaves peered to R_A, neighbouring relays)
Source 2 coverage: any node reachable via RREQ across the mesh
```

```
Cache entry:
  key_cache[node_id] = {
    pub_key:   <bytes[32]: X25519 encryption public key>,
    cert:      <bytes[~138]: deployment CA certificate>,
    cached_at: <uint32: unix timestamp>
  }
```

Cache eviction: entries expire after `KEY_CACHE_LIFETIME_S` (default 604,800s,
7 days). Full cache evicts the oldest entry.

#### 7.5.2 KEY_REQ and KEY_RESP Frames

A leaf requests a public key from its relay using KEY_REQ (frame type 0xD).
The relay responds with KEY_RESP (frame type 0xE) if the key is in its cache.

```
KEY_REQ payload (CBOR):
{
  "target_node_id": <6-byte: node whose public key is needed>
}

KEY_RESP payload (CBOR):
{
  "target_node_id": <6-byte: echoed from KEY_REQ>,
  "pub_key":        <bytes[32]: X25519 public key>,
  "sig":            <bytes[64]: Ed25519 signature by target_node over its pub_key>,
  "status":         <uint8: 0=found, 1=not_found, 2=pending_rreq>
}
```

**Relay behaviour on receiving KEY_REQ:**

```
                 Leaf A               Relay R_A             Mesh
                   │                     │                    │
  ┌─ cache HIT ────┤                     │                    │
  │                │── KEY_REQ(B) ──────►│                    │
  │                │                     │ key_cache[B]? HIT  │
  │                │◄── KEY_RESP ─────────│ status=found      │
  │                │    {status=found,   │                    │
  │                │     pub_key, cert}  │                    │
  │                │ verify cert ✓       │                    │
  │                │ encrypt → send ✅   │                    │
  │                │                     │                    │
  ├─ cache MISS ───┤                     │                    │
  │                │── KEY_REQ(B) ──────►│                    │
  │                │                     │ key_cache[B]? MISS │
  │                │◄── KEY_RESP ─────────│ RREQ issued       │
  │                │    {status=         │                    │
  │                │     pending_rreq}   │──── RREQ(B) ──────►│
  │                │ [leaf stays awake   │◄─── RREP(B_pub) ───│
  │                │  if fast enough]    │ caches B's key     │
  │                │◄── KEY_RESP ─────────│ status=found      │
  │                │ verify cert ✓       │                    │
  │                │ encrypt → send ✅   │                    │
  │                │                     │                    │
  └─ RREQ timeout ─┤                     │                    │
                   │── KEY_REQ(B) ──────►│                    │
                   │                     │──── RREQ(B) ──────►│
                   │◄── KEY_RESP ─────────│ no RREP arrives   │
                   │    {status=         │                    │
                   │     not_found}      │                    │
                   │ store PENDING_KEY   │                    │
                   │ in RTC RAM + NVS    │                    │
                   │ [retry next wake]   │                    │
```

The leaf MUST verify the certificate in KEY_RESP before trusting the key
(Section 7.5.5). If verification fails: discard the key, do not encrypt.

#### 7.5.3 Leaf Local Key Directory

Each leaf maintains a small key directory in NVS:

```
NVS key:  "key_dir"
Format:   CBOR array of entries, each:
  {
    "node_id":   <6-byte>,
    "pub_key":   <bytes[32]>,
    "sig":       <bytes[64]>,
    "cached_at": <uint32: unix timestamp>
  }
Capacity:  10 entries (10 × ~106 bytes = ~1,060 bytes in NVS)
Eviction:  oldest entry when full
Expiry:    KEY_CACHE_LIFETIME_S (default 604,800s = 7 days)
```

The leaf checks its local key directory before sending KEY_REQ. If the key
is present and not expired, the leaf encrypts immediately without any relay
round-trip.

#### 7.5.4 Key Discovery Flows

**Decision tree — which flow applies:**

```
Leaf A wants to encrypt and send to Leaf B
                    │
                    ▼
        check local key_dir[B]
                    │
          ┌─────────┴──────────┐
         HIT                  MISS or
       (valid)                EXPIRED
          │                     │
          ▼                     ▼
  encrypt immediately     KEY_REQ to R_A
  send bundle ✅               │
                     ┌─────────┴──────────┐
                   HIT in              MISS in
                  R_A cache           R_A cache
                     │                    │
                     ▼                    ▼
               KEY_RESP(found)      R_A issues RREQ
               encrypt → send ✅         │
                                ┌────────┴─────────┐
                             RREP in            RREP after
                            < 230ms             > 230ms
                                │                   │
                                ▼                   ▼
                         KEY_RESP(found)    Leaf stores PENDING
                         encrypt → send ✅  in RTC RAM + NVS
                                                    │
                                         ┌──────────┴──────────┐
                                      RREP arrives          No RREP
                                      (B reachable)        (B isolated)
                                           │                    │
                                           ▼                    ▼
                                    KEY_RESP on          bundle pending
                                    next wake            until B reachable
                                    encrypt → send ✅    or expires ⚠️
```

**Case 1 — Key in leaf's local directory (fastest):**

```
Leaf A wakes:
  check key_dir[B] → found, not expired
  encrypt bundle with B's pub_key
  send to R_A → done
  Total: within 230ms wake window
```

**Case 2 — Key in relay cache (fast):**

```
Leaf A wakes:
  check key_dir[B] → miss or expired
  A → R_A: KEY_REQ{target=B}
  R_A: check key_cache[B] → HIT
  R_A → A: KEY_RESP{status=found, pub_key=B_pub, sig=B_sig}
  A: verify sig → store in key_dir → encrypt → send bundle
  Total: within 230ms wake window
```

**Case 3 — Key not in relay cache, RREQ fast (single wake):**

```
Leaf A wakes:
  A → R_A: KEY_REQ{target=B}
  R_A: MISS → issues RREQ for B → RREP arrives with B_pub (~100ms)
  R_A: caches B_pub → A: KEY_RESP{status=found, pub_key=B_pub, sig=B_sig}
  A: verify sig → store in key_dir → encrypt → send bundle
  Total: 230ms + RREQ/RREP round trip (~100-150ms) → extended wake
```

**Case 4 — RREQ response too slow (two wake cycles):**

```
Wake 1 (key discovery):
  A → R_A: KEY_REQ{target=B}
  R_A: MISS → issues RREQ → KEY_RESP{status=pending_rreq}
  [Leaf A's wake window expires]
  A stores in RTC RAM:
    { dst: B, payload_nvskey: "pending_B", status: PENDING_KEY }
  A stores payload in NVS under "pending_B"
  A goes to sleep

  R_A: RREP arrives → key_cache[B] populated

Wake 2 (send):
  A wakes → checks RTC RAM → PENDING_KEY for B
  A → R_A: KEY_REQ{target=B}  (cache hit this time)
  R_A → A: KEY_RESP{status=found}
  A: verify sig → encrypt payload from NVS → send bundle
  A: clear RTC RAM pending flag, delete NVS pending payload
  Total latency: one extra wake cycle (LEAF_SLEEP_MS = 15 min)
```

**Case 5 — Target completely isolated (bundle pending):**

```
Wake 1 (key discovery):
  A → R_A: KEY_REQ{target=B}
  R_A: RREQ → no RREP (B unreachable) → KEY_RESP{status=not_found}
  A: stores bundle as PENDING_KEY in NVS (with bundle lifetime)
  A goes to sleep

  R_A: retries RREQ every RREQ_RETRY_INTERVAL_S until B reachable
  OR: mule arrives at R_B, gets B's pub_key, carries KEY update to R_A's area

When B becomes reachable:
  R_A: gets key → delivers KEY_RESP on A's next wake
  OR: mule visits R_A → R_A learns B's key from mule's key_cache

Bundle expires if B remains unreachable beyond bundle lifetime.
```

#### 7.5.5 Verifying a Public Key Comes from a Trusted Source

Receiving a public key is not the same as trusting it. A self-signed key
proves the key and signature are internally consistent, but does not prove
the key genuinely belongs to node B. A malicious relay can substitute a
fake key with a matching fake signature and the self-check still passes.

**The attack:**

```
Honest KEY_RESP:
  { B_x_pub, B_sign_pub, sig = Ed25519_sign(B_x_pub, B_sign_priv) }
  Leaf A: Ed25519_verify(B_x_pub, sig, B_sign_pub) → PASS ✓
  But: Leaf A doesn't know if B_sign_pub really belongs to B.

Malicious KEY_RESP (evil relay substitutes everything):
  { fake_x_pub,
    fake_sign_pub,
    fake_sig = Ed25519_sign(fake_x_pub, fake_sign_priv) }
  Leaf A: Ed25519_verify(fake_x_pub, fake_sig, fake_sign_pub) → PASS ✓
  Leaf A encrypts to fake_x_pub → evil relay decrypts → MITM ✗
```

**Solution: Deployment CA certificate**

A CA (Certificate Authority) signs each node's public key at provisioning
time. Any receiver can verify the CA's signature to confirm the key is
genuine. Rimba derives the CA key from `deployment_root_secret`, which all
nodes already hold — no separate CA infrastructure is needed.

```
CA trust chain:

  deployment_root_secret  (provisioned to all nodes)
          │
          │ HKDF-SHA256("Rimba-v1-CA-signing")
          ▼
      ca_priv  ──────────────────────────────────────────────┐
      (Ed25519 private key)                                  │
          │                                         signs at │ provisioning
          │ Ed25519_pub()                                     │
          ▼                                                  │
       ca_pub  ◄── any node derives this locally            │
      (Ed25519 public key)                                   │
          │                                                  │
          │ verifies                          cert_sig ◄──────┘
          ▼                                      │
  Ed25519_verify(                               │
    node_id || x_pub || sign_pub || expires_at, │
    cert_sig,                                   │
    ca_pub                                      │
  ) → PASS or FAIL                              │
                                                │
  cert_sig was produced at provisioning:        │
    Ed25519_sign(node_id||x_pub||..., ca_priv) ─┘
```

```
CA key derivation (any node, deterministic):

  ca_priv = HKDF-SHA256(
    IKM  = deployment_root_secret,
    salt = "Rimba-v1-CA-signing",
    info = ""
  )                         ← Ed25519 private key (32 bytes)

  ca_pub = Ed25519_pub(ca_priv)  ← any node can compute this
```

**Certificate format:**

The provisioner (which knows root_secret) signs a certificate for each
node during deployment staging. Each node stores its own certificate in NVS.

```
Rimba Node Certificate (CBOR):
{
  "node_id":    <6-byte: node this certificate covers>,
  "x_pub":      <bytes[32]: X25519 encryption public key>,
  "sign_pub":   <bytes[32]: Ed25519 signing public key>,
  "expires_at": <uint32: unix timestamp>,
  "cert_sig":   <bytes[64]: Ed25519_sign(
                   node_id || x_pub || sign_pub || expires_at,
                   ca_priv
                 )>
}
~138 bytes per certificate, stored in NVS, distributed in KEY_RESP.
```

**Verification flow:**

```
Leaf A                  Relay R_A                    Node B's Relay
  │                        │                               │
  │ KEY_REQ{target=B}      │                               │
  │───────────────────────►│                               │
  │                        │──── RREQ ────────────────────►│
  │                        │◄─── RREP{B_x_pub, cert_B} ───│
  │                        │ caches key + cert             │
  │◄── KEY_RESP{           │                               │
  │      B_x_pub,          │                               │
  │      B_sign_pub,       │                               │
  │      expires_at,       │                               │
  │      cert_sig }        │                               │
  │                        │                               │
  │ Step 1: derive ca_pub  │                               │
  │   ca_priv = HKDF(root_secret, "Rimba-v1-CA-signing")  │
  │   ca_pub  = Ed25519_pub(ca_priv)                       │
  │                        │                               │
  │ Step 2: verify cert    │                               │
  │   Ed25519_verify(      │                               │
  │     msg = B_node_id || B_x_pub ||                      │
  │           B_sign_pub || expires_at,                    │
  │     sig = cert_sig,    │                               │
  │     key = ca_pub       │                               │
  │   ) → PASS ✓           │                               │
  │                        │                               │
  │ Step 3: check expiry   │                               │
  │   now < expires_at ✓   │                               │
  │                        │                               │
  │ Step 4: store + encrypt│                               │
  │   key_dir[B] = {x_pub, verified=true, expires_at}     │
  │   ECIES(payload, B_x_pub) → send bundle ✅            │
```

**Why this defeats MITM:**

```
Evil relay tries to substitute fake_x_pub:

  Needs a cert_sig that passes:
    Ed25519_verify(B_node_id || fake_x_pub || ..., cert_sig, ca_pub)

  To forge cert_sig needs ca_priv.
  To get ca_priv needs root_secret.
  If root_secret is secure → MITM is impossible. ✅

Leaf A's Step 2 verification FAILS on any forged certificate.
```

**KEY_RESP updated payload (full):**

```
KEY_RESP payload (CBOR):
{
  "target_node_id": <6-byte>,
  "x_pub":          <bytes[32]: X25519 encryption public key>,
  "sign_pub":       <bytes[32]: Ed25519 signing public key>,
  "expires_at":     <uint32: certificate expiry>,
  "cert_sig":       <bytes[64]: CA signature over all above fields>,
  "status":         <uint8: 0=found, 1=not_found, 2=pending_rreq>
}
```

**TOFU pinning after first verification:**

After first successful certificate verification, the leaf pins the key.
Unexpected key changes on future KEY_RESPs are flagged as suspicious.

```
First contact (pin established):

  Leaf A receives KEY_RESP{B_x_pub, cert_B}
  ├─ verify cert_B with ca_pub → PASS
  ├─ store in key_dir:
  │    key_dir[B] = { x_pub=B_x_pub,
  │                   cert_verified=true,
  │                   pinned_at=now,
  │                   expires_at=cert_B.expires }
  └─ encrypt with B_x_pub ✅

Second contact (same key expected):

  Leaf A receives KEY_RESP{B_x_pub, cert_B}  (same key)
  ├─ key_dir[B].x_pub == B_x_pub → matches pin ✓
  └─ encrypt with B_x_pub ✅

Second contact (key changed — legitimate rotation):

  Leaf A receives KEY_RESP{B_x_pub_new, cert_B_new}
  ├─ key_dir[B].x_pub ≠ B_x_pub_new  ← key changed!
  ├─ verify cert_B_new with ca_pub → PASS
  ├─ cert_B_new.expires_at > key_dir[B].pinned_at? YES ← newer cert
  ├─ update pin: key_dir[B].x_pub = B_x_pub_new
  └─ encrypt with B_x_pub_new ✅  (legitimate rotation accepted)

Second contact (key changed — possible MITM):

  Leaf A receives KEY_RESP{fake_x_pub, fake_cert}
  ├─ key_dir[B].x_pub ≠ fake_x_pub  ← key changed!
  ├─ verify fake_cert with ca_pub → FAIL ✗
  └─ reject key, alert ✅  (attack detected)
```

**Trust level summary:**

```
Level   Mechanism                      Defeats
──────────────────────────────────────────────────────────────────────
1       No verification                Nothing (never use)
2       Self-signature only            Passive key injection
        (key + sig consistent)         Does NOT defeat active MITM
3       Deployment CA (this section)   MITM, as long as root_secret
        CA derived from root_secret    is not physically compromised
4       Independent CA                 MITM even if root_secret
        (hardening plan Tier 4)        is compromised
```

**Certificate expiry and renewal:**

```
Default: CERT_LIFETIME_S = 31,536,000 (1 year)

Renewal: re-provision node, or gateway issues new certificate
via a unicast OTA config bundle (payload_type=1, target=node_id).
Expired certificate: receiver treats key as UNVERIFIED (Level 2 only).
```

---

## 8. Mesh Routing Protocol

### 8.0 Routing Architecture Overview

Rimba supports three routing mechanisms plus one delivery strategy that operates underneath all of them. Understanding the layering is essential before reading the individual protocol sections.

```
┌────────────────────────────────────────────────────────────┐
│  Mode 3: Geographic Greedy  (optional overlay, GPS only)   │
│  Per-bundle decision — activates when destination GPS      │
│  position is known. Runs on top of Mode 1 or Mode 2.       │
├──────────────────────────┬─────────────────────────────────┤
│  Mode 1: OGM Proactive   │  Mode 2: RREQ / RREP Reactive   │
│  Small networks          │  Large networks                  │
│  (< 30 originators)      │  (> 30 originators)             │
│  Routes always pre-built │  Routes discovered on demand     │
├──────────────────────────┴─────────────────────────────────┤
│  DTN Store-and-Forward — always active under all modes     │
│  No route found → store bundle → forward when path appears │
│  Mule contact → custody transfer regardless of mode        │
└────────────────────────────────────────────────────────────┘
```

See also: `rimba-routing-state-machine.svg` for the full transition diagram.

#### Mode 1 — OGM Proactive

Every relay floods a small "I exist" message (Originator Message, OGM) at regular intervals. Every other relay that hears an OGM records which neighbour it arrived from. Over time, each node builds a complete forwarding table: to reach node X, send to neighbour Y.

```
R1 sends OGM →
  R2 hears from R1: "best path to R1 is via R1" — records in table
  R2 re-floods OGM →
    R3 hears from R2: "best path to R1 is via R2" — records in table

Result: all nodes know a route to R1 without ever asking for one.
Zero discovery latency — route is always in the table.
Cost: OGMs flood continuously even when no data is moving.
```

**When to use**: small deployments (< 30 originators). Always the starting mode on first boot — networks begin sparse and scale up to Mode 2 as they grow.

#### Mode 2 — RREQ/RREP Reactive

Routes are discovered only when a bundle needs one. No continuous background flooding.

```
R1 needs to send to R7. No cached route.
  1. R1 floods RREQ (TTL=2) — gossip-controlled, jittered
  2. No RREP → R1 widens search (TTL=4)
  3. R7 receives RREQ → unicasts RREP back along reverse path
  4. Each relay along the path records the forward route to R7
  5. R1 receives RREP → route cached for 300 seconds
  6. Data traffic refreshes the route; 300s inactivity → expires
```

**When to use**: once the network has > 30 originators (switched automatically). No overhead when idle. Scales to 1,000+ nodes.

#### Mode 3 — Geographic Greedy (Optional Overlay)

Not a separate global mode — a **per-bundle decision** that runs on top of Mode 1 or 2 whenever GPS positions are available. For each bundle being forwarded, the relay checks if the destination's GPS coordinates are known and routes toward them using a quality-weighted distance score:

```
score_i = distance(neighbour_i.pos, destination.pos)
          × (256 / neighbour_i.link_quality)

Forward to neighbour with minimum score_i.

Void (no closer neighbour) → DTN buffer, fall back to Mode 1/2.
```

**When to use**: GPS-equipped relay nodes, when > 30% of neighbours have known positions. Eliminates RREQ flooding entirely for GPS-covered destinations. See Section 8.12.

#### DTN — Store and Forward (Base Layer)

Not a routing mode — a delivery guarantee layer beneath all three modes. When any routing decision fails, the bundle waits.

```
Any mode fails (timeout, void, ISOLATED state):
  → Bundle stored in relay flash (LittleFS, 256 KB)
  → Waiting for: route to appear (any mode)
                 OR mule to arrive (custody transfer)
  → Drains in ALERT-first, oldest-first order
```

DTN is not a fallback to be avoided. In a sparse sensor mesh where connectivity is intermittent by design, DTN is the primary delivery path for many nodes.

#### Topology discovery (Modes 1 and 2 — mutually exclusive)

These determine how the network learns which next-hop relay reaches a given destination. Only one is active at a time per node.

| Mode | Name | When active | How it works |
|---|---|---|---|
| 1 | OGM proactive | Small networks (< 30 originators) | Each node floods its own Originator Message. All nodes build a complete forwarding table from received OGMs. No per-route discovery latency. |
| 2 | RREQ/RREP reactive | Larger networks (> 30 originators) | No forwarding table maintained. Routes discovered on demand via RREQ flood + RREP unicast. DTN layer buffers bundles during discovery. |

#### Forwarding decision (Mode 3 — overlay on Modes 1 and 2)

This determines which specific neighbour to use for a given bundle. It operates per-bundle on top of whichever topology discovery mode is active.

| Mode | Name | When used | How it works |
|---|---|---|---|
| 3 | Geographic greedy | Optional — GPS-equipped relays with destination position known | Forward to quality-weighted geographically closest neighbour. Bypasses OGM table lookup and RREQ flood for this bundle. Falls back to Mode 1/2 on void. |

#### Special case: Gateway anycast

Not a separate mode. Uses the RREQ mechanism with a reserved destination address (`GATEWAY_ANYCAST_ID = 0x000000000001`). Any gateway responds to this RREQ. Route selection uses hop count then `uplink_load_pct`. See Section 8.11.

#### Routing state machine

```
First boot
  │
  ▼
┌─────────────────────────────────────┐
│         MODE 1: OGM PROACTIVE       │
│  Node floods OGMs, builds table.    │
│  Low latency — routes pre-built.    │
└────────────────────────────────────┘
  │                          ▲
  │ ogm_originator_count      │ ogm_originator_count
  │ > DENSE_OGM_ORIGINATORS   │ < SPARSE_OGM_ORIGINATORS
  │ (default 30)              │ for ROUTING_HYSTERESIS_S
  ▼                          │ (default 120 seconds)
┌─────────────────────────────────────┐
│       MODE 2: RREQ/RREP REACTIVE    │
│  OGMs stop. Routes on demand.       │
│  Existing entries valid until       │
│  lifetime expires (300 seconds).    │
└─────────────────────────────────────┘

MODE 3 (GEOGRAPHIC) operates as a per-bundle overlay on both:

  For any bundle being forwarded:
    Is destination GPS position in position_table?
    ├── YES → Is quality-filtered candidate available?
    │   ├── YES, own GPS known   → Tier 1 (full greedy, void detection)
    │   ├── YES, no own GPS      → Tier 2 (greedy, no void check)
    │   └── No usable candidates → Tier 3 (fall back to Mode 1 or 2)
    └── NO  → Tier 3 (use OGM table or RREQ — Mode 1 or 2)

DTN layer operates beneath all modes:
  No route found (any mode) → store bundle → forward when route appears
  Mule contact → custody transfer regardless of routing mode
```

#### Transition table

| Transition | Trigger | What happens |
|---|---|---|
| Start → Mode 1 | First boot (always) | Node begins flooding OGMs, builds forwarding table |
| Mode 1 → Mode 2 | `ogm_originator_count > 30` | OGM generation and re-flooding stops immediately. Existing routes remain valid for 300s. RREQ used for all new route needs. |
| Mode 2 → Mode 1 | `ogm_originator_count < 10` continuously for 120s | OGM generation resumes. One OGM sent immediately to re-announce. Neighbours update their metrics. |
| Any → Geo (per-bundle) | Destination GPS position found in `position_table` AND quality-filtered neighbour available | This bundle routed geographically. Other bundles continue using Mode 1 or 2. |
| Geo → Mode 1/2 (per-bundle) | Void detected OR destination position unknown | This bundle falls back to RREQ or OGM table. Geographic routing continues for other bundles. |
| Mode 1/2 → DTN | RREQ timeout OR `connectivity_state` = ISOLATED | Bundle stored in flash. Waits for route or mule. |
| DTN → Mode 1/2 | Route appears in forwarding table OR mule arrives | Bundle drains: ALERT-priority first, then oldest-first. |

#### Summary: which routing handles what

```
Question                              Answered by
──────────────────────────────────────────────────────────────────
Where is node X in the network?       OGM (Mode 1) or RREQ (Mode 2)
Which direction is node X's GPS?      Geographic (Mode 3, if GPS known)
How do I reach the nearest gateway?   Gateway anycast (RREQ variant)
What if I have no route at all?       DTN store-and-forward
What if the network is disconnected?  Mule custody transfer
```

#### Key design decisions

**Why OGM first?** Networks begin small. OGM provides zero-latency routing for the first weeks or months of a deployment. As more nodes are added, the protocol migrates to RREQ automatically without any reconfiguration.

**Why 30/10 as the thresholds?** At 30 originators, OGM traffic consumes roughly 30% of channel capacity. The 20-node hysteresis gap (30 down to 10) prevents oscillation at the boundary.

**Why geographic is per-bundle, not a global mode?** In a mixed GPS deployment, a global "geographic mode" would break routing to non-GPS destinations. Per-bundle selection means the protocol uses geographic routing where it can and falls back to RREQ where it cannot — no configuration, no edge cases.

**Why DTN is always active?** The routing modes assume connectivity. DTN handles the cases where connectivity doesn't exist — which in a sparse outdoor sensor mesh is frequent and expected. DTN is not a fallback; for many Rimba nodes it is the primary delivery path.

Routing mode is selected dynamically by each node independently, based on observed OGM traffic. No coordination or configuration is required.

### 8.1 Routing Mode Selection

**Measurement:**

Each node maintains a sliding-window count of unique OGM originators:

```
ogm_originator_count = number of distinct originator_ids
                       in OGMs received in the last
                       ROUTING_WINDOW_S seconds
```

**Transition rules:**

```
if ogm_originator_count > DENSE_OGM_ORIGINATORS:
    → Switch to REACTIVE mode

if ogm_originator_count < SPARSE_OGM_ORIGINATORS
   continuously for ROUTING_HYSTERESIS_S seconds:
    → Switch to PROACTIVE mode
```

**On first boot:** PROACTIVE mode. Networks start sparse and scale up to reactive as density grows.

**Mode behaviour:**

```
PROACTIVE mode:
  Node generates OGMs at intervals per Appendix A.
  Node re-floods received OGMs (TTL > 0).
  Forwarding table built from OGM-derived best-next-hop.

REACTIVE mode:
  Node stops generating and re-flooding OGMs.
  Routes discovered on demand via RREQ/RREP.
  Existing forwarding table entries remain valid
  until their lifetime expires.
  Leaves are unaffected — they never generate OGMs.
```

**Transition handling:**

```
PROACTIVE → REACTIVE:
  Stop OGM generation immediately.
  Existing routes remain valid until lifetime expires.
  Begin using RREQ for any new route needs.

REACTIVE → PROACTIVE:
  Resume OGM generation after ROUTING_HYSTERESIS_S.
  Send one OGM immediately on mode entry to re-announce.
  Neighbours receiving this OGM update their metrics.
```

**Hysteresis prevents oscillation**: the `ROUTING_HYSTERESIS_S` window (default 120s) ensures a node does not flip back to proactive the moment the OGM rate briefly dips.

### 8.2 Reactive Routing Overview

Routes are discovered on demand when a node has a bundle to forward and no cached route exists. The DTN layer buffers bundles during route discovery. Leaves do not initiate or re-flood RREQ — they send DATA to their nearest peer relay and let the relay handle routing onward.

### 8.3 Route Request (RREQ)

```
Node A needs route to Z, no cache entry:
  A broadcasts RREQ (TTL = RREQ_INITIAL_TTL, default: 2)

RREQ payload (CBOR):
{
  "src":     <6-byte>,   // originating node
  "dst":     <6-byte>,   // desired destination
  "rreq_id": <uint32>,   // unique per src, incremented each request
  "hop_cnt": <uint8>,    // 0 at origin
  "seq_src": <uint16>,
  "seq_dst": <uint16>    // last known, 0 if unknown
}

Each relay re-broadcasts if (src, rreq_id) not seen in 60s and TTL > 0.
Each relay records reverse path: "to reach src, via who sent this."
When RREQ reaches Z (or a node with fresh route to Z): send RREP.
```

#### Proxy RREP for peered leaves

A relay responds to RREQ on behalf of any leaf in its active peer link table. This enables downlink routing to leaves without the leaf being a full routing participant.

```
Relay R receives RREQ where dst = leaf_id L:

  If L is in R's peer link table:
    Continuous leaf (active peer link):
      Always respond with RREP.
      Route lifetime: LEAF_ROUTE_LIFETIME_S (default 300s).

    Scheduled leaf (miss_counter == 0):
      Respond with RREP.
      Route lifetime: min(LEAF_SLEEP_MS / 1000, LEAF_ROUTE_LIFETIME_S)
      Route expires before leaf's next expected sleep — forces
      re-discovery if leaf moves.

    Scheduled leaf (miss_counter > 0):
      Respond with RREP but with reduced lifetime:
      lifetime = max(60, LEAF_SLEEP_MS / 1000 - miss_counter × 30)
      Signals to the network that leaf presence is uncertain.

    Scheduled leaf (miss_counter >= RELAY_LEAF_MISS_MAX):
      Do not respond. Leaf has been removed from schedule.
      Leaf has likely moved — RREQ should not be satisfied.

  R sends RREP{src=RREQ.src, dst=leaf_id, hop_cnt=0,
               lifetime=<as above>}
```

This rule enables three communication patterns:

```
Pattern                  Path
──────────────────────────────────────────────────────────────────
Gateway → Leaf           GW → (mesh) → R (leaf's relay) → Leaf
  (downlink)             R responds to RREQ for leaf_id

Relay → Leaf             R1 → (mesh) → R2 (leaf's relay) → Leaf
  (any relay to leaf)    R2 responds to RREQ for leaf_id

Leaf → Leaf              Leaf A → R_A → (mesh) → R_B → Leaf B
  (peer-to-peer)         Leaf A sends to R_A.
                         R_A issues RREQ for leaf_B_id.
                         R_B responds with proxy RREP.
                         R_A routes bundle to R_B.
                         R_B delivers to Leaf B on next wake.
```

Leaves do not send RREQ or RREP themselves — the relay acts as their routing proxy. Leaf-to-leaf communication is supported but not latency-optimised: delivery to a Scheduled leaf takes up to one wake cycle (LEAF_SLEEP_MS) from the time the bundle reaches the leaf's relay.

### 8.4 Expanding Ring Search

```
Attempt   TTL    Retry after
──────────────────────────────────
1         2      500 ms
2         4      500 ms
3         8      1000 ms
4         16     2000 ms
5+        MAX    DTN buffers, retry after RREQ_RETRY_INTERVAL (60s)
```

### 8.5 Route Reply (RREP)

```
RREP payload (CBOR):
{
  "src":             <6-byte: was RREQ src>,
  "dst":             <6-byte: was RREQ dst>,
  "hop_cnt":         <uint8>,
  "seq_dst":         <uint16>,
  "lifetime":        <uint32: seconds, default 300>,

  // True E2E — optional, present when dst node has a registered pub key
  "dst_pub_key":     <bytes[32]: X25519 public key of dst node — optional>,
  "dst_pub_key_sig": <bytes[64]: Ed25519 sig of pub key by dst node — optional>
}
```

Each relay forwarding RREP installs a forward route to `dst`. If `dst_pub_key` is present, the requesting node caches it in its local key directory (Section 7.5.3) and can immediately encrypt bundles to `dst` without a separate KEY_REQ/KEY_RESP exchange. The signature MUST be verified before trusting the key. Route entries expire after `lifetime` seconds if not refreshed by data traffic.

#### 8.5.1 Multiple RREP — Route Selection

A single RREQ may elicit several RREPs (multiple paths to `dst` exist). The
originator installs the first RREP's route immediately so the buffered bundle
can flow, then evaluates later RREPs against the installed route:

```
On receiving an RREP for dst:

  if no route to dst installed:
    install this route (next_hop = RREP sender, hop_cnt, seq_dst)

  else (route already installed):
    install the new route ONLY if it is strictly better:
      newer:   RREP.seq_dst > installed.seq_dst        (fresher), OR
      shorter: RREP.seq_dst == installed.seq_dst
               AND RREP.hop_cnt < installed.hop_cnt     (same freshness,
                                                          fewer hops)
    otherwise: discard the RREP (do not disturb a working route)
```

This mirrors the forwarding-table update rule (Section 8.8.1) and the gateway
anycast rule (Section 8.11.1): freshness first, then hop count. It prevents a
late-arriving, longer-path RREP from replacing a good route already in use.

#### 8.5.2 Lost RREP Recovery

An RREP is a unicast frame traversing the reverse path. If it is lost (link
failure, sleeping intermediate node, collision), the originator never installs
a route and the buffered bundle cannot be sent. Recovery is handled without a
dedicated RREP-ACK:

```
Originator issued RREQ, started RREQ_RETRY_INTERVAL timer (60s):

  RREP arrives within interval:
    route installed, bundle sent, timer cancelled. ✅

  No RREP within RREQ_RETRY_INTERVAL:
    DTN layer still holds the bundle (it was never deleted).
    Originator re-issues RREQ with a new rreq_id and incremented TTL
    (expanding ring search, Section 8.4).
    Repeat up to RREQ_MAX_RETRIES (default 3) attempts.

  After RREQ_MAX_RETRIES with no RREP:
    Destination is treated as unreachable via mesh.
    Bundle remains in DTN store awaiting:
      - a future RREQ success (topology may heal), OR
      - mule custody transfer (Section 9.5.8), OR
      - bundle lifetime expiry (then dropped, E2E_CUSTODY_ACK{EXPIRED}
        emitted if the bundle was RETAINED — Section 9.5.7).
```

The key property: a lost RREP never loses data. The bundle stays buffered in
the DTN layer (it is only deleted on confirmed forward progress), so RREP loss
costs discovery latency, not delivery. Each RREQ retry uses a fresh `rreq_id`
so it is not suppressed by the duplicate cache (Section 8.7).

```
RREQ retry parameters:
  RREQ_RETRY_INTERVAL   60s   (wait before re-issuing RREQ)
  RREQ_MAX_RETRIES      3     (attempts before declaring mesh-unreachable)
  TTL escalation:       RREQ_INITIAL_TTL (2) → +2 per retry → mesh diameter
```

### 8.6 Route Error (RERR)

```
RERR payload (CBOR):
{
  "broken_dst": <list of 6-byte node IDs now unreachable>
}
```

Sent upstream when a link break is detected. The source's DTN layer buffers the affected bundle and re-initiates RREQ discovery. Recovery is DTN-driven: routing notifies, DTN layer retries.

**Precursor list and RERR propagation:**

"Upstream" is defined precisely by the precursor list. Each forwarding-table
entry records the set of neighbours that have used this node as their next hop
toward `dst`:

```
Forwarding table entry (extended):
  dst         → destination node ID
  next_hop    → neighbour to forward through
  hop_cnt     → distance to dst
  seq_dst     → freshness
  lifetime    → expiry
  precursors  → [neighbour IDs that route to dst VIA this node]
                (populated when this node forwards a DATA frame or
                 RREP on behalf of an upstream neighbour)
```

When a link to `next_hop` breaks, the relay does NOT broadcast RERR to all
neighbours. It unicasts RERR only to the precursors of the affected routes:

```
Link to next_hop N breaks (ACK timeout, peer link lost):

① Find all forwarding entries where next_hop == N
② Collect the union of their precursor lists
③ Mark those entries invalid (broken_dst list)
④ Unicast RERR{broken_dst} to each precursor
⑤ Each precursor receiving RERR:
     invalidates its own matching entries
     propagates RERR to ITS precursors (recursive, up the tree)
     stops when a node has no precursors for that dst (it was the origin)
```

This bounds RERR to the actual upstream path that was using the broken route,
rather than flooding the whole mesh. A node with no precursors for a broken
destination (typically the original source) does not propagate further — it
hands the failure to its DTN layer, which buffers the bundle and re-issues
RREQ (Section 8.5.2).

```
Example — RERR propagation up a path:

  Source S ──► R1 ──► R2 ──► R3 ──✗── R4 (dst)
                              │
                       link R3→R4 breaks

  R3: precursors for R4 = {R2}     → RERR to R2
  R2: precursors for R4 = {R1}     → RERR to R1
  R1: precursors for R4 = {S}      → RERR to S
  S:  no precursors (origin)       → DTN buffers, re-RREQ

  RERR travels exactly S◄R1◄R2◄R3 — the path that was in use.
  Nodes off this path never receive the RERR.
```

**RERR for leaf routes:**

When a relay loses a leaf peer link (leaf timed out, miss_counter reached RELAY_LEAF_MISS_MAX, or leaf explicitly closed peer link), the relay sends RERR for the leaf's node ID:

```
Relay R removes leaf L from peer link table:
  R sends RERR{broken_dst: [leaf_id_L]}
  All nodes that cached a route to leaf L via R remove that entry
  Next attempt to reach leaf L triggers fresh RREQ
  → Finds leaf's new relay (if leaf moved) or returns no route (if isolated)
```

This ensures stale leaf routes don't persist after the leaf has moved to a different relay.

### 8.7 Flood Control

```
1. Jittered rebroadcast:
   Relay waits random(0, RREQ_JITTER_MS) before re-flooding RREQ.
   RREQ_JITTER_MS is adaptive (Section 13.3.2):
     RREQ_JITTER_MS = max(RREQ_JITTER_BASE_MS,
                          relay_neighbor_count × RREQ_JITTER_PER_NEIGHBOR_MS)

2. Duplicate suppression:
   (src, rreq_id) seen within 60s → discard.

3. Rate limiting:
   Max 1 RREQ per destination per 10s.

4. Probabilistic gossip forwarding (Section 8.7.1):
   Re-flood with probability p that scales inversely with density.
```

#### 8.7.1 Gossip Forwarding

To prevent RREQ floods from saturating dense cells, a node re-floods a received RREQ only with probability `p`, computed from its local neighbour count:

```
p = min(1.0, GOSSIP_TARGET_FORWARDERS / relay_neighbor_count)

  neighbor_count = 4:   p = 0.75
  neighbor_count = 10:  p = 0.30
  neighbor_count = 30:  p = 0.10
  neighbor_count = 50:  p = 0.06
```

Target: approximately `GOSSIP_TARGET_FORWARDERS` (default 3) nodes re-broadcast each RREQ regardless of cell density. In sparse areas `p` approaches 1.0 (every node forwards); in dense areas `p` drops sharply.

**Mandatory forwarding overrides** — a node ALWAYS re-floods (p = 1.0) when any of these hold, to protect connectivity:

```
1. First-hop guarantee:
   Node is a direct neighbour of the RREQ originator.
   Ensures the request reliably escapes the originator's cell.

2. Articulation-point protection:
   Node has one or more neighbours reachable ONLY through it
   (no alternative route exists in its forwarding table).
   Such a node is a topological bridge and must forward.

3. Sparse safety:
   relay_neighbor_count <= NEIGHBOR_LOW_THRESHOLD (4).
```

**Delivery safety net**: probabilistic forwarding may occasionally fail to reach a destination. This is recovered by the existing expanding-ring search (Section 8.4) and DTN buffering — a failed discovery simply triggers a retry after `RREQ_RETRY_INTERVAL`. No data is lost; only discovery latency increases in rare cases.

Gossip applies only to RREQ flooding. OGM flooding (proactive mode) continues to use Trickle suppression, and RREP/RERR are unicast and unaffected.

### 8.8 Forwarding Table

| Field | Size | Description |
|---|---|---|
| dst_node | 6 bytes | Destination Node ID |
| next_hop | 6 bytes | Neighbour to send toward dst |
| hop_cnt | 1 byte | Distance metric |
| seq_dst | 2 bytes | Last known dst sequence |
| lifetime | 4 bytes | Expiry timestamp |
| precursors | variable | Neighbour IDs that route to dst via this node (for targeted RERR, Section 8.6) |

A neighbour is added to a route's precursor list when this node forwards a
DATA frame or an RREP toward `dst` on that neighbour's behalf. The list is
used to direct RERR only to upstream nodes actually using the route, rather
than broadcasting failure mesh-wide.

#### 8.8.1 Route Update Rule

When a node receives an RREP (or learns a route from an OGM) for a `dst_node`
that already has a forwarding table entry, it must decide whether to replace
the existing route. Rimba uses AODV-style sequence-and-metric comparison:

```
On receiving a route advertisement for dst with (new_seq_dst, new_hop_cnt):

  existing = forwarding_table[dst]

  Accept and replace the existing route if ANY of:
    1. No existing entry for dst, OR
    2. new_seq_dst is fresher than existing.seq_dst
       (using serial-number comparison, Section 8.8.2), OR
    3. new_seq_dst == existing.seq_dst AND new_hop_cnt < existing.hop_cnt
       (same freshness, but shorter path)

  Otherwise: ignore the advertisement (existing route is better or equal).

  When accepted:
    forwarding_table[dst] = { next_hop, new_hop_cnt, new_seq_dst,
                              lifetime = now + RREP.lifetime }
```

This guarantees loop-freedom (the AODV invariant): a node never installs a
route with a stale sequence number, and among equally-fresh routes prefers
the shorter one. Without this rule, two relays could install routes pointing
at each other, creating a forwarding loop that only TTL would eventually break.

```
Tie-breaking when new_seq_dst == existing.seq_dst AND new_hop_cnt == existing.hop_cnt:
  Keep the existing route (do not flap between equal-cost paths).
  Refresh its lifetime only if next_hop is identical.
  This prevents route oscillation between two equal-quality next-hops.
```

#### 8.8.2 Sequence Number Comparison and Wraparound

All sequence numbers in Rimba (`seq` in mesh header, `seq_dst` in routes,
`rreq_id`) are finite-width integers that eventually wrap around to zero.
Naïve comparison (`a > b`) breaks at the wrap boundary — a fresh post-wrap
value of 2 would appear "older" than a pre-wrap value of 65,534.

Rimba uses RFC 1982 serial number arithmetic for all sequence comparisons:

```
For 16-bit sequence numbers (seq, seq_dst):

  is_fresher(a, b):   // is a strictly newer than b?
    return (a != b) AND
           ((a > b AND a - b < 32768) OR
            (a < b AND b - a > 32768))

  Examples:
    is_fresher(5, 3)         → true   (normal case)
    is_fresher(2, 65534)     → true   (2 is post-wrap, newer)  ✅
    is_fresher(65534, 2)     → false  (65534 is pre-wrap, older) ✅
    is_fresher(40000, 100)   → false  (40000 - 100 > 32768, so 100 wrapped) 

For 32-bit sequence numbers (rreq_id):
  Same logic with wrap point 2^31 = 2,147,483,648.
```

The loop-prevention dedup cache (Section 6, `(src, seq)` seen within 60s)
uses exact-match on the pair, not ordering — so it is unaffected by
wraparound. Only the route freshness comparison (8.8.1) needs serial-number
arithmetic.

#### 8.8.3 Reboot and Incarnation Number

When a node reboots, its volatile sequence counter resets. Neighbours that
cached the node's pre-reboot sequence number would reject the node's fresh
(low) sequence numbers as stale or as replays, until the counter climbs back
past the cached value — potentially blackholing the node for a long time.

Rimba solves this with an **incarnation number** stored in NVS:

```
NVS field: "incarnation" (uint16, survives reboot, increments on each boot)

At boot:
  incarnation = NVS.read("incarnation") + 1
  NVS.write("incarnation", incarnation)

Effective sequence for freshness comparison:
  effective_seq = (incarnation << 16) | seq    // 32-bit combined value

  A reboot increments incarnation → effective_seq jumps far forward →
  neighbours immediately accept the rebooted node's advertisements as fresh.
```

```
HELLO and route advertisements carry both fields:
  "incarnation": <uint16>   ← from NVS, changes only on reboot
  "seq":         <uint16>   ← volatile, increments per message

Freshness comparison uses the combined 32-bit value:
  is_fresher( (inc_a << 16) | seq_a,
              (inc_b << 16) | seq_b )   // RFC 1982 on 32 bits

Reboot detection:
  if received.incarnation > cached.incarnation:
    Node rebooted. Reset cached seq for this node. Accept new advertisement.
    Clear any stale routes that used this node (it may have lost state).
```

This distinguishes "the node rebooted and legitimately restarted its counter"
(incarnation increased) from "an attacker is replaying old frames"
(incarnation unchanged, seq lower than cached → still rejected as replay).

The incarnation counter is 16-bit (65,536 reboots before it wraps). At one
reboot per day that is 179 years; wraparound is handled by the same RFC 1982
comparison.

### 8.9 Connectivity-Aware Forwarding

When forwarding a bundle, a node considers the connectivity state of candidate next-hops (from their HELLO advertisements):

```
To forward bundle destined for dst_node:

1. Look up dst_node in forwarding table.
2. For each candidate next_hop in order of metric:
     a. Check next_hop connectivity in neighbour table:
          ISOLATED → skip (no onward path)
          MESH_ONLY → use only for mesh-destined bundles
          CONNECTED → use for any bundle
     b. If acceptable: send. Done.
3. If no acceptable next_hop found:
     DTN buffer the bundle. Retry when routing table updates.
```

A node in MESH_ONLY state is a valid forwarder for node-to-node mesh traffic but should not be selected as next-hop for internet-destined bundles (those should wait for a CONNECTED relay or a mule).

### 8.10 Sleeping Node Handling

Leaves sleep most of the time and may miss incoming RREQ floods. A relay that has a sleeping leaf as a direct IBSS peer handles RREQ on its behalf:

```
RREQ arrives at Relay R1 with dst_node = Leaf L:
  R1 checks neighbor table: L is a direct IBSS peer
  L is currently sleeping (within NEIGHBOR_TIMEOUT but wake
  window not open)
  R1 sends RREP with hop_cnt=1, indicating L reachable via R1
  
When DATA frame for L arrives at R1:
  If L's wake window is open: deliver immediately via IBSS
  If L is asleep: buffer bundle in DTN store,
                  deliver on L's next wake window
```

This allows the mesh to route toward sleeping leaves without waiting for them to wake and participate directly.

### 8.11 Gateway and Cloud Anycast Routing

To support multiple gateways without requiring nodes to know specific gateway
Node IDs in advance, Rimba reserves two well-known anycast addresses. Both use
the **same routing mechanism** — any gateway answers RREQ for either — but they
declare **different cryptographic endpoints**:

```
GATEWAY_ANYCAST_ID = 0x000000000001   →  ipn:1.<service>
  Routed to the nearest gateway. Decryption endpoint is the GATEWAY.
  Bundle is encrypted to gateway_pub. The gateway can read the payload
  and act on it locally (alerting, aggregation, filtering).

CLOUD_ANYCAST_ID   = 0x000000000002   →  ipn:2.<service>
  Routed to the nearest gateway (identical anycast path). Decryption
  endpoint is the CLOUD. Bundle is encrypted to cloud_pub. The gateway
  CANNOT read the payload — it strips the Rimba transport and forwards
  the opaque ciphertext over the internet to the cloud backend.
```

**Routing and cryptographic destination are separate concerns:**

```
                    Routing destination        Crypto destination
                    (where it physically goes)  (who can decrypt)
  ──────────────────────────────────────────────────────────────────
  ipn:1.*           nearest gateway             that gateway
  ipn:2.*           nearest gateway             the cloud backend only

  Both route to "any gateway." The destination EID also declares which
  key the sender encrypted to. This is self-describing — the trust model
  is visible in the address, not negotiated over the air.
```

The sender selects the endpoint simply by choosing the address, and encrypts
to the matching key (both keys are in the provisioning payload, so neither
requires key discovery):

```
if dst == ipn:1.*  →  ECIES(payload, gateway_pub_key)
if dst == ipn:2.*  →  ECIES(payload, cloud_pub_key)
```

**Why a compromised gateway cannot cheat on `ipn:2.*` bundles:**

```
The destination EID is not a permission the gateway enforces — it is a
statement of which key was used to encrypt. A gateway receiving an
ipn:2.* bundle CANNOT decrypt it even if it wants to: the bundle is
encrypted to cloud_pub, and the gateway holds no cloud_priv. Enforcement
is mathematical (ECDH), not policy. A compromised gateway is, for
cloud-targeted bundles, exactly as powerless as a compromised relay.
```

This dissolves the gateway-key-sharing question for cloud traffic: gateways
hold no key capable of reading `ipn:2.*` payloads, so it does not matter
whether gateways share keys or how exposed they are.

#### 8.11.1 Gateway RREQ Response

A gateway answers RREQ for **both** anycast IDs identically (it is the routing
egress point for both):

```
Gateway G receives RREQ{dst=GATEWAY_ANYCAST_ID or CLOUD_ANYCAST_ID}:
  G sends RREP{src=RREQ.src, dst=<the requested anycast ID>,
               hop_cnt=0, lifetime=300s}
```

Multiple gateways may respond. The originator installs the route from the first RREP it receives (closest gateway). Subsequent RREPs are evaluated against the installed route:

```
Update rule on receiving second RREP for either anycast ID:
  if new_hop_cnt < current_hop_cnt:
    update route to new gateway  (closer found)
  elif new_hop_cnt == current_hop_cnt:
    if new_gateway.uplink_load_pct < current_gateway.uplink_load_pct:
      update route to new gateway  (same distance, less loaded)
  else:
    discard  (worse route)
```

The two anycast IDs maintain **separate forwarding entries** but are populated
by the same RREP mechanism. In practice a relay's next hop for both IDs is
usually the same gateway — the distinction matters only at the gateway, in how
the bundle is handled on arrival (8.11.3).

#### 8.11.2 Anycast Forwarding Table Entry

Each relay maintains a special anycast entry alongside normal per-node routes:

| Field | Value |
|---|---|
| dst_node | `GATEWAY_ANYCAST_ID` (0x000000000001) |
| next_hop | Next-hop toward the selected gateway |
| gw_node | Node ID of the selected gateway |
| hop_cnt | Hop count to the selected gateway |
| gw_load | `uplink_load_pct` from selected gateway's HELLO |
| lifetime | 300s (refreshed by data traffic or gateway re-advertisement) |

The anycast entry is populated when:
- A RREP for `GATEWAY_ANYCAST_ID` is received, OR
- The OGM routing table contains a gateway-role node (from its HELLO `role` field)

Only `role=0` (gateway) nodes respond to RREQ for `GATEWAY_ANYCAST_ID` and only gateway-role nodes are eligible as anycast destinations. Relay and mule nodes (`role=1` and `role=3`) are explicitly excluded as anycast targets — a mule has no internet uplink and must not appear as a gateway-destined bundle's final hop.

A mule node MAY appear as an **intermediate hop** in the anycast route. Since a mule participates in OGM/RREQ routing as a relay, when it is stationary and connected to the backbone, other nodes may route gateway-destined bundles *through* the mule toward the actual gateway:

```
Node A → Mule M → Gateway G

A's anycast table: next_hop = M, gw_node = G
M's anycast table: next_hop = G, gw_node = G

Bundle path: A → M → G   (mule is a transit hop, not the destination)
```

Gateway-destined bundles reaching a mule as a transit hop are forwarded normally. The mule does not take custody — it forwards in the same RREQ/RREP forwarding path as any relay.

**DTN path via mule custody transfer:**

When an isolated relay has no route to `GATEWAY_ANYCAST_ID`, its gateway-destined bundles enter the DTN store. The mule collects them via custody transfer (Section 9.5) and physically carries them to a gateway. The bundle destination (`ipn:1.*`) is unchanged — the mule is the physical carrier, not the network destination.

#### 8.11.3 Internet Bundle Forwarding and Gateway Handling

Relays forward bundles addressed to either anycast ID using the matching
forwarding entry — the routing logic is identical:

```
When a relay receives a bundle with dst EID ipn:1.* or ipn:2.*:

1. Look up the matching anycast ID in the forwarding table.
2. If entry exists and next_hop is CONNECTED: forward normally.
3. If entry exists but next_hop is MESH_ONLY: retain in DTN store.
   Bundle forwards when a CONNECTED relay becomes next-hop.
4. If no entry: initiate RREQ for that anycast ID.
   DTN buffers the bundle during discovery.
```

The behaviour **differs only at the gateway**, based on the destination EID:

```
Gateway receives bundle. Inspects dst EID:

  ┌──────────────────────────────────────────────────────────────┐
  │  dst = ipn:1.*  (GATEWAY_ANYCAST_ID — gateway endpoint)       │
  │                                                              │
  │   1. ECDH(gateway_priv, e_pub) → bundle_key                  │
  │   2. Decrypt payload                                         │
  │   3. Act locally if applicable:                             │
  │        - local alerting (no cloud round-trip)               │
  │        - aggregation / filtering before upload             │
  │        - operate during internet outage                    │
  │   4. Upload to cloud (plaintext over TLS, or re-encrypt)    │
  └──────────────────────────────────────────────────────────────┘

  ┌──────────────────────────────────────────────────────────────┐
  │  dst = ipn:2.*  (CLOUD_ANYCAST_ID — cloud endpoint)          │
  │                                                              │
  │   1. Do NOT attempt decryption (no cloud_priv on gateway)   │
  │   2. Strip Rimba transport: mesh header, link-layer AES-CCM │
  │   3. Keep the BPSec ciphertext block intact                 │
  │   4. Forward the opaque bundle to the cloud backend over    │
  │      the internet (TCPCLv4 or HTTPS)                        │
  │   5. Cloud: ECDH(cloud_priv, e_pub) → bundle_key → decrypt  │
  └──────────────────────────────────────────────────────────────┘
```

**The two paths side by side:**

```
GATEWAY endpoint (ipn:1.*):

  Leaf ──enc(gw_pub)──► [relays] ──► Gateway ──decrypt──► act locally
                                        │                  └──► upload to cloud
                                        ▼
                                   reads payload ✓

CLOUD endpoint (ipn:2.*):

  Leaf ──enc(cloud_pub)──► [relays] ──► Gateway ──forward──► Cloud ──decrypt
                                          │ blind            └──► reads payload ✓
                                          ▼
                                     cannot read ✗
                                     (no cloud_priv)
```

A single leaf can use both in the same deployment — e.g. an alert to
`ipn:1.alert` (gateway rings a local alarm immediately) and the full reading
to `ipn:2.telemetry` (only the cloud can read it). The choice is per-bundle.

#### 8.11.3a Internet-Side Delay Tolerance

For `ipn:2.*` bundles, the gateway → cloud link may itself be down (rural
backhaul, satellite outage). The gateway buffers the opaque ciphertext in its
DTN store until the cloud is reachable — exactly as a relay buffers for a
sleeping next-hop. The gateway stores bundles it cannot inspect; the bundle
lifetime clock must account for both the mesh transit delay and the internet
backhaul delay. If lifetime expires before the cloud is reached, the bundle is
dropped (and an E2E_CUSTODY_ACK{EXPIRED} is emitted for RETAINED bundles,
Section 9.5.7).

#### 8.11.4 Gateway Failover

When the selected gateway goes offline (route expires or RERR received):

```
1. Delete anycast forwarding entry.
2. DTN store retains all internet-destined bundles.
3. Re-initiate RREQ for GATEWAY_ANYCAST_ID.
4. Remaining online gateways respond → new route installed.
5. Buffered bundles drain toward the new gateway.

Failover latency ≈ RREQ discovery time (seconds).
```

#### 8.11.5 Cloud-Side Deduplication

When multiple gateways are operational, the same bundle may reach two gateways via different mesh paths and both upload it to the cloud endpoint. Rimba handles intra-gateway deduplication (mule + mesh path, Section 9.5.6). Cross-gateway deduplication must be handled at the cloud layer using the same key:

```
bundle_unique_key = src_eid || creation_ts_ms
```

Cloud endpoints must deduplicate on this key when receiving from multiple Rimba gateways.

### 8.12 Geographic Greedy Routing (Optional)

Geographic routing is a **forwarding-level overlay** (Mode 3) operating on top of whichever topology discovery mode is active (OGM or RREQ). It does not replace topology discovery — it bypasses it for individual bundles where the destination's GPS position is already known. See Section 8.0 for the full relationship between routing modes.

When relay nodes are equipped with a GPS receiver, Rimba can replace RREQ flooding with geographic greedy routing — a position-based forwarding algorithm that eliminates route discovery overhead entirely. This is an **optional capability**: the protocol falls back to RREQ/RREP when position information is unavailable.

#### 8.12.1 When to Use

```
Recommended when:
  - Deployment has > 200 relay nodes OR
  - Node density causes RREQ flood congestion (Section 13.3.6) OR
  - Deployment is static (relays don't move after installation)

Not needed when:
  - < 200 relay nodes and density is comfortable
  - Relay nodes are mobile (position changes frequently)
  - BOM cost of GPS module is a constraint
```

#### 8.12.2 Position Advertisement

GPS-equipped relay nodes include their position in HELLO frames using integer encoding (no floating-point parsing on MCU):

```
"lat_e6": <int32: latitude × 10⁶>
"lon_e6": <int32: longitude × 10⁶>
"pos_age_s": <uint32: seconds since GPS fix>

Resolution: 1 × 10⁻⁶ degree ≈ 0.11 metres
Accuracy required: ≤ 5m CEP (standard consumer GNSS)

Integer examples:
  37.422005° N → lat_e6 =  37422005
  122.084057° W → lon_e6 = -122084057
```

Non-GPS relays omit these fields. Their absence signals to neighbours that this node has no known position.

#### 8.12.3 Position Table

Every relay maintains a position table populated from received HELLO frames:

```
position_table[node_id] = {
  lat_e6:    int32,
  lon_e6:    int32,
  pos_age_s: uint32,
  last_rx_ms: uint64   // local timestamp when HELLO was received
}

Entry expires when:
  pos_age_s + ((now_ms - last_rx_ms) / 1000) > POSITION_MAX_AGE_S
```

Non-GPS nodes maintain this table for their GPS-equipped neighbours and can use it for Tier 2 forwarding decisions. A node without GPS has no self-entry in the table.

#### 8.12.4 Distance Calculation

For relay spacings up to ~50 km, a flat-Earth approximation is sufficient and avoids trigonometry on constrained MCUs:

```c
// Both positions in × 10⁶ degrees (int32)
// Returns approximate distance in metres
uint32_t approx_distance_m(int32_t lat1_e6, int32_t lon1_e6,
                            int32_t lat2_e6, int32_t lon2_e6) {
    int32_t dlat = lat2_e6 - lat1_e6;
    int32_t dlon = lon2_e6 - lon1_e6;
    // 111,000 m per degree latitude
    // longitude scale: cos(lat) ≈ use mid-latitude
    int32_t mid_lat_e6 = (lat1_e6 + lat2_e6) / 2;
    float cos_lat = cosf(mid_lat_e6 * 1e-6f * M_PI / 180.0f);
    float dy = dlat * 0.111f;          // metres × 10⁻³
    float dx = dlon * 0.111f * cos_lat;
    return (uint32_t)sqrtf(dx*dx + dy*dy);
}
```

For deployments spanning > 50 km, use the full haversine formula.

#### 8.12.5 Three-Tier Forwarding Algorithm

Geographic proximity alone is not sufficient for routing decisions. A physically close relay behind a wall or terrain obstacle may have a much poorer radio link than a farther relay with line-of-sight. The forwarding score combines geographic distance with link quality to account for this.

**Quality-weighted score:**

```
score_i = approx_distance_m(N_i.pos, D_pos) × (256 / N_i.lq)

  distance term:  geometrically closer to destination → lower
  quality term:   256 / lq → higher lq (better link) = lower cost

Pick N_j = argmin(score_i)

Example:
  N1: 50m from destination, lq=40  → score = 50 × 6.4  = 320
  N2: 150m from destination, lq=220 → score = 150 × 1.16 = 175 ← preferred
  N2 wins despite being farther: its link quality more than compensates.
```

**Full algorithm:**

```
On receiving bundle for destination D:

Step 1: Lookup D.node_id in position_table.
  Found → D_pos known → proceed to Step 2.
  Not found → TIER 3: use RREQ/RREP (Section 8.3–8.6).

Step 2: Build quality-filtered candidate set.
  candidates = {N_i : N_i in position_table AND
                      N_i is an active peer link AND
                      N_i.lq >= MIN_GEO_LINK_QUALITY}

  If candidates is empty:
    → No usable links. TIER 3: DTN buffer.

Step 3: Select best next-hop by combined score.
  For each candidate N_i:
    score_i = approx_distance_m(N_i.pos, D_pos) × (256 / N_i.lq)
  N_best = candidate with minimum score_i.

Step 4: Void check (GPS nodes only).
  If this node has own position:
    my_dist = approx_distance_m(my_pos, D_pos)
    best_dist = approx_distance_m(N_best.pos, D_pos)
    If best_dist >= my_dist:
      → Quality-void: no quality-filtered candidate makes
        geographic progress. TIER 3: DTN buffer.
    Else:
      → TIER 1: Forward to N_best.
  Else (no own position):
    → TIER 2: Forward to N_best. No void check.
              TTL and DTN guard against loops.
```

**Tier summary:**

| Tier | Condition | Quality filter | Void detection | Action |
|---|---|---|---|---|
| 1 | GPS + destination known | Yes | Yes | Quality-weighted greedy |
| 2 | No GPS + destination known | Yes | No | Quality-weighted, forward to best |
| 3 | Destination unknown or no candidates | N/A | N/A | RREQ/RREP or DTN buffer |

#### 8.12.6 Void Handling

A routing void occurs when a GPS relay detects it is the geographically closest node to the destination but cannot deliver directly (destination out of range, or not a direct peer).

```
Void detected at Node A:
  Bundle is handed to the DTN layer (Section 9).
  DTN stores the bundle.
  When a new neighbour appears that is closer to D than A:
    Geographic greedy resumes.
  When a mule arrives:
    Bundle transferred via custody protocol.
  When a new route appears via RREQ/RREP:
    Route used for delivery.
```

Rimba does **not** implement perimeter routing (right-hand rule around voids). The DTN layer is the void-handling mechanism. This is appropriate for a sparse-first protocol where disconnection is a first-class expectation, not an exception.

#### 8.12.7 Hybrid Mesh Behaviour

In a deployment with a mix of GPS and non-GPS relays:

```
GPS relay   → GPS relay:   Tier 1 (full greedy, void detection)
GPS relay   → non-GPS relay: Tier 1 decision at GPS relay,
                              non-GPS relay uses Tier 2 onward
Non-GPS relay → any:       Tier 2 if destination known,
                            Tier 3 if not
Any         → leaf:        Relay delivers locally (leaves don't
                            participate in geographic routing)
```

Non-GPS relays degrade gracefully — they participate in geographic forwarding using Tier 2 when destination positions are known, and fall back to RREQ/RREP when not. The mesh routes correctly whether GPS coverage is 10% or 100%.

**Recommended minimum GPS coverage for benefit:**

```
< 30% GPS relays:   Limited benefit. Most routes use RREQ/RREP.
  30–70% GPS:       Partial benefit. Dense clusters route geographically;
                    sparse edges use RREQ.
  > 70% GPS:        Full benefit. RREQ/RREP becomes rare exception.
  100% GPS:         RREQ floods effectively eliminated in connected mesh.
```

#### 8.12.8 GPS Power and Deployment Procedure

For static relay deployments, GPS is used once:

```
On first boot (or after factory reset):
  1. Power on GPS receiver (~12 mA active).
  2. Acquire fix: 30s–5 min (cold start, depends on visibility).
     Required accuracy: ≤ 5m CEP.
  3. Store (lat_e6, lon_e6) in protected flash.
  4. Store fix_timestamp in RTC.
  5. Power off GPS permanently.
  6. Set pos_age_s = time since fix_timestamp (increments forever).

Ongoing power draw from GPS: ~0 mA (powered off).
Average GPS energy over 3-year relay lifetime:
  12 mA × 5 min / (3yr × 525,960 min) ≈ 0 mA average.
```

If a relay is physically relocated, GPS fix procedure must be re-run manually (e.g., via a provisioning command over PEER link from a mobile device).

---

## 9. DTN Bundle Layer

### 9.0 What is DTN, and Why Not TCP/IP?

#### The TCP/IP assumption

TCP/IP — the protocol that underlies the internet — was designed for a world where both the sender and receiver are always reachable, and where a message sent now receives a reply within seconds. It makes three fundamental assumptions:

```
1. End-to-end connectivity exists at the moment of transmission.
   If no path is available, the send fails immediately.

2. Round-trip time is short — milliseconds to a few seconds.
   TCP retransmission timers and flow control are tuned for this.

3. Both endpoints are simultaneously available.
   A TCP connection requires a three-way handshake before any
   data can flow. If the destination is unreachable, the handshake
   cannot complete and no data is sent at all.
```

This works perfectly for laptops, phones, and servers connected to the internet. It fails completely for the kind of network Rimba is designed for.

#### Why TCP/IP fails for a sparse sensor mesh

```
A Rimba leaf wakes for 230ms every 15 minutes.
TCP/IP: "I need to open a connection to the gateway."

  1. Leaf sends SYN to gateway.
  2. Relay forwards it. But the relay's backbone peer is asleep.
  3. No SYN-ACK arrives within TCP retransmit timeout (~1 second).
  4. TCP declares the path broken.
  5. Leaf's wake window expires. Leaf goes back to sleep.
  6. No data delivered.

This happens every single wake cycle for every isolated node.
TCP/IP has no concept of "try again when the path opens later."
It only knows: connected right now, or failed.
```

Even on a connected path, TCP is the wrong tool. TCP streams are stateful — the connection must be maintained for the duration of transfer. A relay that sleeps would tear down the connection. TCP has no mechanism to resume where it left off after an interruption.

#### What DTN does differently

DTN (Delay-Tolerant Networking, RFC 4838) was designed for exactly this situation. It makes different assumptions:

```
1. End-to-end connectivity is NOT required right now.
   A message can be stored at an intermediate node until
   a path opens, then forwarded — potentially hours later.

2. Round-trip time may be very long — hours, days, or never.
   There is no real-time feedback loop. A bundle is sent and
   the sender trusts the network to deliver it eventually.

3. Endpoints do not need to be simultaneously available.
   A sender can deposit a bundle and go to sleep.
   The receiver can collect it whenever they next wake up.
```

The analogy is postal mail rather than a phone call. When you post a letter, you don't need the recipient to be home when you write it. You hand it to the postal system, it passes through several sorting depots, and it arrives at the destination whenever the postman makes their next round. The letter is a self-contained unit — it carries its own address, timestamp, and payload.

In Rimba terms: a sensor reading is posted to the relay (the local sorting depot). The relay holds it until a path to the gateway opens, then forwards it. If the path never opens, the mule (the postman) physically collects all waiting mail and carries it to the gateway.

#### The bundle: DTN's unit of transfer

Where TCP works with byte streams, DTN works with **bundles** — complete, self-contained messages. Each bundle carries:

```
Source EID:       who sent it (ipn:<node_id>.<service>)
Destination EID:  where it's going (ipn:1.<service>)
Creation timestamp + sequence number: unique identity
Lifetime:         when to discard if undelivered (e.g. 24 hours)
Payload block:    the actual sensor data (CBOR-encoded)
Security blocks:  BPSec authentication and encryption
```

Because a bundle is self-contained, it can be:
- Stored on flash for days
- Carried physically by a mule
- Forwarded through a chain of relays with different paths
- Deduplicated at the destination (same bundle arrived via two paths)

#### Custody transfer

TCP provides reliability through retransmission — if a packet is lost, the sender retransmits it. In a DTN network where round-trip times are hours, this is impractical.

DTN uses **custody transfer** instead. When a relay accepts custody of a bundle, it takes responsibility for delivery. The previous holder can then release its copy — though in Rimba *when* it releases depends on the retention mode (EAGER deletes on confirm; RETAINED keeps until end-to-end delivery is proven — Section 9.5.7). Responsibility is handed along the chain until the gateway (the final destination) confirms receipt.

```
TCP reliability:   sender retransmits until ACK received
                   requires sender to stay awake and connected

DTN reliability:   custody transferred hop by hop
                   sender can sleep after custody ACK received
                   each hop takes responsibility for the bundle
```

#### How Rimba uses DTN

Rimba implements BPv7 (Bundle Protocol Version 7, RFC 9171) as its DTN layer:

```
Application data (sensor reading, alert, command)
    ↓
BPv7 bundle (CBOR-encoded, timestamped, addressed)
    ↓
BPSec security blocks (ChaCha20-Poly1305 or AES-128-CCM)
    ↓
Mesh routing (OGM/RREQ → next hop → next hop → gateway)
    OR
DTN store → mule custody → gateway
```

The DTN layer is always active. Even when a connected mesh path exists, bundles are created as BPv7 units. If the path is available, they flow immediately. If not, they wait in the relay's flash store (LittleFS, 256 KB) until a path opens or a mule arrives.

```
TCP/IP model:           DTN model (Rimba):
  send → receive          send → store → forward → store → deliver
  (synchronous)           (asynchronous, may take hours)
  fails if no path        succeeds eventually (within bundle lifetime)
  connection-oriented     connectionless (bundle per message)
  stateful                stateless at each hop
  not suitable for        designed for intermittent, high-latency,
  intermittent networks   physically isolated nodes
```

The technical specification for Rimba's BPv7 implementation begins in Section 9.1.

#### Frame structure comparison

TCP/IP stacks layers where the outer headers change per hop and the inner headers stay the same end-to-end:

```
TCP/IP packet in transit:
┌────────────────────────────────────────────────────────────┐
│ Ethernet frame (L2)  — changes every hop                   │
│  src MAC = this router's MAC                               │
│  dst MAC = next router's MAC                               │
│  ┌──────────────────────────────────────────────────────┐  │
│  │ IP packet (L3)   — unchanged end-to-end              │  │
│  │  src IP = original sender IP                         │  │
│  │  dst IP = final receiver IP                          │  │
│  │  ┌────────────────────────────────────────────────┐  │  │
│  │  │ TCP segment (L4) — stateful stream             │  │  │
│  │  │  port, seq#, ack#                              │  │  │
│  │  │  ┌──────────────────────────────────────────┐  │  │  │
│  │  │  │ Application data (HTTP, MQTT, etc.)      │  │  │  │
│  │  │  └──────────────────────────────────────────┘  │  │  │
│  │  └────────────────────────────────────────────────┘  │  │
│  └──────────────────────────────────────────────────────┘  │
└────────────────────────────────────────────────────────────┘
```

Rimba adds a DTN layer and two encryption layers — one per-hop, one end-to-end:

```
Rimba frame in transit:
┌────────────────────────────────────────────────────────────┐
│ 802.11ah IBSS frame (L2) — changes every hop               │
│  src MAC = this relay's MAC                                │
│  dst MAC = next relay's MAC                                │
│  AES-128-CCM encrypted (link-layer, per-hop key)           │
│  ┌──────────────────────────────────────────────────────┐  │
│  │ Rimba mesh header — next_hop changes, src/dst stay   │  │
│  │  src      = originating node ID  (6 bytes, fixed)    │  │
│  │  dst      = final destination ID (6 bytes, fixed)    │  │
│  │  next_hop = immediate next relay (6 bytes, changes)  │  │
│  │  TTL, seq#, flags (DTN, ALERT, MULE...)              │  │
│  │  ┌────────────────────────────────────────────────┐  │  │
│  │  │ BPv7 bundle — unchanged end-to-end             │  │  │
│  │  │  src EID = leaf (ipn:<leaf_id>.1)              │  │  │
│  │  │  dst EID = gateway (ipn:1.1)                   │  │  │
│  │  │  creation timestamp, lifetime, sequence#       │  │  │
│  │  │  ┌──────────────────────────────────────────┐  │  │  │
│  │  │  │ BPSec blocks (E2E encryption to gateway) │  │  │  │
│  │  │  │  ┌────────────────────────────────────┐  │  │  │  │
│  │  │  │  │ Sensor payload (CBOR-encoded)      │  │  │  │  │
│  │  │  │  └────────────────────────────────────┘  │  │  │  │
│  │  │  └──────────────────────────────────────────┘  │  │  │
│  │  └────────────────────────────────────────────────┘  │  │
│  └──────────────────────────────────────────────────────┘  │
└────────────────────────────────────────────────────────────┘
```

The three-address mesh header (`src`, `dst`, `next_hop`) means every relay knows both where a frame ultimately came from and where it is ultimately going — without opening the bundle. This enables routing decisions without decrypting the payload.

**Two encryption layers serve different purposes:**

```
AES-128-CCM (link layer, per-hop):
  Between adjacent peer nodes only.
  Each relay decrypts to read the mesh header (routing info),
  then re-encrypts with the next peer's link key before forwarding.
  Prevents an RF observer from reading routing metadata.

BPSec (end-to-end, bundle layer):
  From originating leaf to gateway only.
  Intermediate relays cannot read the sensor payload.
  A compromised relay can forward bundles but cannot read their content.
```

In standard TCP/IP with TLS, IP and TCP headers are sent in plaintext — an observer between two routers can see source IP, destination IP, and ports. In Rimba, even the routing headers are encrypted at the link layer, providing stronger metadata privacy.

---

#### How data traverses the network

**TCP/IP — synchronous, path must exist end-to-end:**

```
Sender    Router1    Router2    Router3   Receiver
  │          │          │          │          │
  │──pkt────►│          │          │          │
  │          │──pkt────►│          │          │
  │          │          │──pkt────►│          │
  │          │          │          │──pkt────►│
  │◄──────────────────────────ACK────────────│
  │          │          │          │          │

  Each router: examine IP dst → look up next hop → forward immediately
  Each router: if no route → drop packet immediately
  Sender: if no ACK within timeout → retransmit from sender

  If Router2 goes offline:
  ┌──────────────────────────────────────────────┐
  │ Router1 drops packet                          │
  │ No ACK reaches sender                         │
  │ Sender retransmits (but Router2 still offline)│
  │ TCP connection eventually times out           │
  │ Sender must retry the entire connection       │
  └──────────────────────────────────────────────┘
```

**Rimba DTN — asynchronous, path only needed one hop at a time:**

```
Leaf      Relay1     Relay2     Gateway
  │          │          │          │
  │──bundle─►│          │          │  ← Relay2 currently unreachable
  │◄─ACK────│          │          │  ← Relay1 takes custody
  │ (sleeps) │[storing] │          │
  │          │          │          │
  │          │ (time passes — minutes, hours, or days)
  │          │          │          │
  │          │──bundle─►│          │  ← Relay2 comes online
  │          │[deleted] │[storing] │  ← Relay1 transfers custody
  │          │          │──bundle─►│
  │          │          │[deleted] │[deliver to cloud]

  Each relay: forward if next hop available,
              store in flash if not
  Leaf: sleeps after custody ACK — done
  No retransmission needed from leaf
  Bundle survives relay restarts (stored in flash)

  If Relay2 never comes online:
  ┌──────────────────────────────────────────────┐
  │ Mule physically travels to Relay1's area      │
  │ Relay1: CUSTODY_REQ → Mule                   │
  │ Mule physically carries bundle to Gateway     │
  │ Bundle delivered, possibly hours/days later   │
  └──────────────────────────────────────────────┘
```

**Key behavioural differences at each hop:**

```
Behaviour          TCP/IP router        Rimba relay
──────────────────────────────────────────────────────────────
On arrival         Forward or drop      Forward or store
Storage            None (no memory)     256 KB flash store
Encryption         Reads plaintext IP   Decrypts link layer,
                   header               re-encrypts for next hop
Payload access     Can read if no TLS   Cannot read (BPSec E2E)
On link failure    Drop packet          Store and wait
Sender notified?   Yes (ICMP / TCP RST) Not required (custody held)
Delivery guarantee None (best effort)   Yes (within bundle lifetime)
Address at L2      IP of next router    MAC of next relay peer
Address at L3/DTN  Final IP destination Final EID (unchanged)
```

---

#### Standards compliance

Rimba's DTN implementation follows established IETF standards at the bundle layer but uses a custom transport at the hop-by-hop layer.

```
Layer               Standard followed              Rimba status
──────────────────────────────────────────────────────────────────────
DTN architecture    RFC 4838                       Followed
Bundle Protocol     BPv7 (RFC 9171)                Followed — full bundle
                                                   format, CBOR encoding,
                                                   EID addressing
Bundle security     BPSec (RFC 9172)               Followed — BCB
                    + COSE (RFC 9052/9053)         (confidentiality) and
                                                   BIB (integrity)
EID addressing      ipn: scheme (RFC 9171)         Followed
CBOR encoding       RFC 8949                       Followed — bundles,
                                                   HELLO frames,
                                                   provisioning payloads
Trickle timer       RFC 6206                       Followed — HELLO and
                                                   OGM suppression
Convergence layer   TCPCLv4 (RFC 9174)             NOT followed
                    UDPCL, LTPCL, etc.             Custom convergence layer
                                                   (see below)
```

**What is a convergence layer?**

The DTN architecture (RFC 4838) separates the bundle layer from the underlying
transport using a "convergence layer" — an adapter that carries bundles over a
specific network technology. Standard convergence layers include:

```
TCPCLv4 (RFC 9174):  BPv7 over TCP/IP — used for internet DTN routing
UDPCL:               BPv7 over UDP — lightweight, lossy environments
LTPCL:               BPv7 over LTP — deep space (NASA)
```

Rimba uses a **custom convergence layer** that carries BPv7 bundles over
802.11ah IBSS using the Rimba mesh header (Section 6). This layer is not
standardised and is specific to Rimba.

```
Standard DTN stack:                 Rimba DTN stack:

  ┌─────────────────────┐             ┌─────────────────────┐
  │ BPv7 bundle layer   │             │ BPv7 bundle layer   │
  │ (RFC 9171)          │             │ (RFC 9171)          │  ← standard
  ├─────────────────────┤             ├─────────────────────┤
  │ Convergence layer   │             │ Rimba convergence   │
  │ e.g. TCPCLv4        │             │ layer               │  ← custom
  │ (RFC 9174)          │             │ (Section 6 + 7)     │
  ├─────────────────────┤             ├─────────────────────┤
  │ TCP / IP            │             │ 802.11ah IBSS       │  ← standard
  └─────────────────────┘             └─────────────────────┘
```

**Interoperability implications:**

Because Rimba follows BPv7 at the bundle layer, the bundles themselves are
standards-compliant. A gateway node can act as a bridge between the Rimba
custom convergence layer (mesh-facing) and a standard convergence layer
(internet-facing):

```
[Rimba mesh] ← custom CL → [Gateway] ← TCPCLv4 → [Internet DTN router]

The gateway strips the 802.11ah/Rimba transport, extracts the BPv7 bundle
(unchanged), and forwards it via TCP to a standard DTN endpoint on the internet.
The bundle format is identical on both sides.
```

This means Rimba sensor data can, in principle, be delivered to any BPv7-
capable endpoint on the internet — including DTN infrastructure at research
institutions, space agencies, or other Rimba deployments in different locations —
as long as the gateway performs convergence layer translation.

**What is NOT interoperable:**

A standard DTN node (e.g. running ION-DTN or Dtn7-rs) cannot join a Rimba
mesh directly. It would need to implement the Rimba convergence layer (IBSS
discovery, peer links, TDMA scheduling, mesh routing) to participate. The
bundle content would be compatible; the transport would not.

---

#### OSI layer mapping

The OSI model has seven layers. TCP/IP and Rimba map to them differently,
and BPv7 sits at a different level depending on how DTN is deployed.

```
OSI Layer          TCP/IP (standard internet)     Rimba (embedded DTN mesh)
────────────────────────────────────────────────────────────────────────────
7 — Application    HTTP, MQTT, CoAP               Sensor application
                   Application data               CBOR-encoded payload
                                                  (temperature, GPS, etc.)

6 — Presentation   TLS record layer               CBOR encoding / decoding
                   Data encoding (JSON, XML)       RFC 8949

5 — Session        TLS handshake                  BPSec security contexts
                   Socket session management       RFC 9172 + COSE RFC 9052
                                                  Ed25519 signing
                                                  ChaCha20-Poly1305 / AES-CCM

4 — Transport      TCP (reliable stream)           BPv7 bundle protocol
                   UDP (unreliable datagram)        RFC 9171
                                                   Store-carry-forward delivery
                                                   EID addressing
                                                   Bundle lifetime + custody

3 — Network        IP (RFC 791 / RFC 8200)         Rimba mesh routing
                   Routing protocols               OGM proactive (BATMAN-style)
                   (BGP, OSPF, RIP)                RREQ / RREP reactive (AODV)
                                                   Geographic greedy (GPSR)
                                                   Gateway anycast

2 — Data Link      Ethernet MAC (IEEE 802.3)       Rimba peer links
                   Wi-Fi MAC (IEEE 802.11)          HELLO neighbour discovery
                   ARP address resolution           X25519 ECDH key exchange
                                                   AES-128-CCM link encryption
                                                   802.11ah IBSS MAC

1 — Physical       Ethernet cable                  802.11ah PHY (Wi-Fi HaLow)
                   Wi-Fi radio (2.4 / 5 GHz)       900 MHz ISM band
                   Optical fibre                   MM6108 radio chip
                                                   Sub-1 GHz, 1–16 MHz channels
```

**Key observation: BPv7 sits at different OSI layers in different deployments.**

This is the most important distinction between internet DTN and Rimba:

```
Internet DTN (BPv7 over IP):       Rimba (BPv7 IS the transport):
┌────────────────────────────┐     ┌────────────────────────────┐
│ L7  BPv7 bundle layer      │     │ L4  BPv7 bundle layer      │
│     (application on top    │     │     (transport — no IP      │
│      of IP)                │     │      underneath)            │
├────────────────────────────┤     ├────────────────────────────┤
│     Convergence layer      │     │     Convergence layer      │
│     TCPCLv4 (RFC 9174)     │     │     Rimba CL (Section 6–7) │
├────────────────────────────┤     ├────────────────────────────┤
│ L4  TCP                    │     │ L3  Rimba mesh routing     │
├────────────────────────────┤     ├────────────────────────────┤
│ L3  IP                     │     │ L2  802.11ah IBSS MAC      │
│     (addresses, routing)   │     │     + Rimba peer links     │
├────────────────────────────┤     ├────────────────────────────┤
│ L2  Ethernet / Wi-Fi MAC   │     │ L1  802.11ah PHY           │
├────────────────────────────┤     │     (900 MHz HaLow radio)  │
│ L1  Physical medium        │     └────────────────────────────┘
└────────────────────────────┘
BPv7 is an application layer       BPv7 is the transport layer.
protocol running on top of IP.     There is no IP in Rimba.
IP handles L3 routing.             Rimba mesh routing is L3.
```

In internet DTN, IP addresses provide L3 routing and BPv7 EIDs provide
application-layer endpoint identification. The two address spaces coexist.

In Rimba, BPv7 EIDs (`ipn:<node_id>.<service>`) ARE the only addresses.
There are no IP addresses. The Rimba mesh header and routing protocol provide
L3 functionality that IP would normally handle.

**What Rimba has at L2 that TCP/IP doesn't:**

Standard 802.11 Wi-Fi at L2 provides MAC addressing and CSMA/CA access.
Rimba adds a significant amount of functionality at L2 that TCP/IP typically
pushes to higher layers:

```
Standard 802.11 L2:          Rimba L2 additions:
  MAC addressing               + Node authentication (X25519 ECDH)
  CSMA/CA collision avoidance  + Per-hop link encryption (AES-128-CCM)
  Association / disassociation + TDMA leaf scheduling
  (AP/STA mode)                + Trickle-suppressed HELLO discovery
                               + IBSS (no AP required)
```

This means Rimba's L2 is richer than a standard Wi-Fi L2, providing security
and scheduling that standard Wi-Fi leaves to higher layers or to infrastructure
(the access point).



---

### 9.1 Bundle Protocol

Rimba uses BPv7 (RFC 9171). All sensor data is encapsulated in bundles before transmission. The DTN layer provides store-carry-forward semantics across the mesh.

### 9.2 Endpoint Addressing

```
EID format:  ipn:<node_number>.<service_number>
node_number = decimal representation of the 48-bit Node ID
```

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

### 9.5 Mule Protocol

The mule protocol enables isolated relays and leaves to transfer bundles to a mobile courier when no backbone route to a gateway exists.

#### 9.5.0 How Custody Works (and What It Protects Against)

**The problem custody solves.** A bundle may sit in one node's flash for hours
or days waiting for a mule or a healed route. If that single copy is lost — node
fault, flash corruption, a mule that never delivers — the data is gone, silently,
with no notification to the source. Custody is the mechanism that prevents this
single point of loss by managing *who is responsible* for a bundle's onward
delivery and *when a previous holder may delete its copy*.

> **Note:** Rimba's custody is a Rimba-specific extension, NOT a BPv7 feature.
> BPv7 (RFC 9171) deliberately removed the custody-transfer mechanism that
> existed in BPv6 (RFC 5050). Plain BPv7 forwards best-effort with only a
> lifetime. Rimba re-introduces custody — in its own L2 frames with its own
> semantics — because mule-bridged sparse delivery genuinely needs single-point-
> of-loss protection. See the BPv7 conformance note in this section.

**Where the custody exchange sits in the OSI stack.** Custody is negotiated by
two dedicated L2 link frames — `CUSTODY_REQ` (`0xB`) and `CUSTODY_ACK` (`0xC`,
Section 6.3) — exchanged hop-locally between two peered nodes. The *bundle*
being transferred is a BPv7 unit (L4), but the custody *handshake* that governs
it is L2 signalling, like ACK or the peer-link frames.

```
Custody packet placement:

  L4 Transport    the BUNDLE being handed off (BPv7, RFC 9171)
                  — its payload is BPSec-encrypted (L5), unreadable to
                    the custodian
  L2 Data Link    CUSTODY_REQ (0xB) / CUSTODY_ACK (0xC) — the handshake
                  ACK (0x6) — per-bundle delivery acknowledgement
                  all in 802.11ah IBSS frames, hop-by-hop AES-128-CCM
  L1 Physical     900 MHz HaLow

  The end-to-end CUSTODY_ACK (E2E_CUSTODY_ACK, Section 9.5.7) is the
  exception: it is itself a small BPv7 BUNDLE (L4) addressed back to the
  source, because it must travel end-to-end, not hop-local.
```

So there are two distinct acknowledgement concepts, at two layers:

```
ACK (0x6, L2):              "I received this frame on this hop."
CUSTODY_ACK (0xC, L2):      "I accept RESPONSIBILITY for onward delivery —
                            you may delete your copy" (hop-local handoff).
E2E_CUSTODY_ACK (L4 bundle): "The bundle reached its final destination" —
                            propagates back to the source-side custodian
                            so a RETAINED copy can finally be deleted.
```

**The custody flow:**

```
 Isolated Relay R                         Mule M (sweeping)
   │                                          │
   │◄──── HELLO {MULE flag, storage_kb} ──────│  ① R detects a mule,
   │                                          │     verifies mule_id
   │                                          │
   │──── CUSTODY_REQ {MANIFEST: ─────────────►│  ② R offers its bundles
   │      bundle ids, sizes, priority} ───────│
   │                                          │  ③ M selects (ALERT first,
   │                                          │     then oldest) up to its
   │◄──── CUSTODY_ACK {ACCEPT: ───────────────│     free storage
   │      accepted_ids, storage_remaining} ───│
   │                                          │
   │──── DATA (bundle 1) ────────────────────►│  ④ transfer each accepted
   │◄──── ACK ────────────────────────────────│     bundle (L2 frame ACK)
   │◄──── CUSTODY_ACK {CONFIRM: id} ──────────│  ⑤ M takes RESPONSIBILITY
   │   ┌──────────────────────────────────┐   │
   │   │ EAGER mode:  R DELETES bundle 1   │   │  ⑥ retention mode decides
   │   │ RETAINED:    R KEEPS it, marks    │   │     whether R deletes now
   │   │   "awaiting E2E ack"              │   │     or waits for E2E ack
   │   └──────────────────────────────────┘   │
   │   ... repeat ④–⑥ for each bundle ...      │
   │                                          │
   │──── CUSTODY_REQ {DONE} ─────────────────►│  ⑦ R has no more
   │◄──── CUSTODY_ACK {DONE} ─────────────────│
   │                                          │
   │                          M sweeps onward ─┤  ⑧ M carries bundles toward
   │                                          │     a gateway / reachable node
   │                                          │
   │      (later, RETAINED mode only:)        │
   │◄═══ E2E_CUSTODY_ACK {DELIVERED} ═════════╡  ⑨ once the bundle reaches
   │   R now deletes its retained copy ✅     │     its destination, an
   │                                          │     end-to-end ack flows back
   │                                          │     (via mesh or next sweep)
```

**With custody vs without — what changes:**

```
WITHOUT custody (plain best-effort, as BPv7 alone would be):

  R holds bundle → mule arrives → R sends bundle → R deletes it
                                                    (no responsibility
                                                     handoff, no confirm)
  If the mule never delivers:
    - R already deleted its copy → bundle LOST
    - source never learns delivery failed → SILENT loss
    - no retry possible — no one has it anymore
  If the transfer is interrupted mid-send:
    - ambiguous: did the mule get it? R can't tell → either deletes
      too early (loss) or keeps duplicates (the gateway must dedup)

WITH Rimba custody:

  R holds bundle → mule arrives → CUSTODY handshake → explicit CONFIRM
    EAGER:    R deletes only after CONFIRM (never deletes an
              unconfirmed bundle — interrupted transfer = R keeps it)
    RETAINED: R keeps its copy until an END-TO-END ack proves delivery
              → even a mule that vanishes mid-journey loses nothing;
                R still has the bundle and can hand it to another mule
  Failure is never silent:
    - timeout / re-delegation (MULE_CUSTODY_TIMEOUT_S) if a mule stalls
    - E2E_CUSTODY_ACK {EXPIRED} tells the source if delivery ultimately
      failed, so it can re-issue
```

```
Side-by-side:

                          Without custody      With Rimba custody
  ──────────────────────────────────────────────────────────────────
  Delete timing           on send (blind)      on CONFIRM (EAGER) or
                                               E2E ack (RETAINED)
  Single point of loss    YES (mule is only    NO in RETAINED (source
                          copy)                keeps copy until proven)
  Silent data loss        YES                  NO (EXPIRED ack reports it)
  Interrupted transfer    ambiguous/loss/dup   safe — unconfirmed kept
  Retry after mule fails  impossible           yes (re-delegate / re-send)
  Cost                    none                 storage (RETAINED copies)
                                               + handshake frames
```

The trade is explicit: custody costs a little storage (RETAINED copies held
until confirmed) and a few handshake frames, in exchange for eliminating silent
single-point data loss — which, in a network where a bundle may depend on one
mule crossing a gap, is the difference between reliable and lossy delivery. The
EAGER/RETAINED split (Section 9.5.7) lets each bundle choose where it sits on
that trade: ALERT bundles default to RETAINED (lose nothing), routine telemetry
to EAGER (cheap).

#### 9.5.1 Overview

```
Relay (isolated)                        Mule (sweeping)
────────────────────────────────────────────────────────────────
                  ←── HELLO (MULE flag, storage_kb) ───
Relay detects mule. Verifies mule_id
against trusted list (Issue #9).

──── CUSTODY_REQ (MANIFEST) ──────────>
     {phase=MANIFEST, bundle list
      with id, size, created, priority}

                                        Mule selects bundles:
                                        1. ALERT priority first
                                        2. Then oldest-first
                                        Until storage_kb exhausted.

<─── CUSTODY_ACK (ACCEPT) ─────────────
     {phase=ACCEPT, accepted_ids[],
      storage_remaining_kb}

──── DATA (bundle 1) ─────────────────>
<─── ACK ──────────────────────────────
<─── CUSTODY_ACK (CONFIRM) ────────────
     {phase=CONFIRM, confirmed_id}      ← relay may now release this bundle
                                          (EAGER: delete now; RETAINED:
                                           keep until E2E ack — Section 9.5.7)

──── DATA (bundle 2) ─────────────────>
<─── ACK ──────────────────────────────
<─── CUSTODY_ACK (CONFIRM) ────────────
     ... repeat for all accepted bundles ...

──── CUSTODY_REQ (DONE) ─────────────>  ← relay has no more bundles
<─── CUSTODY_ACK (DONE or FULL) ────────
```

What a relay does on CUSTODY_ACK CONFIRM depends on the bundle's retention mode (Section 9.5.7). In **EAGER** mode the relay deletes its copy immediately on CONFIRM; in **RETAINED** mode it keeps its copy until an end-to-end CUSTODY_ACK proves the bundle reached its destination. In **both** modes, a relay never releases a bundle that has *not* been CONFIRMed: if the transfer is interrupted (the mule leaves without confirming), all unconfirmed bundles are retained. CONFIRM is therefore the earliest point at which a bundle *may* be deleted (EAGER), not a point at which it is always deleted.

#### 9.5.1a Handshake OSI Placement and Full Sequence

**Where the handshake sits.** The custody handshake is **L2 link signalling**,
exchanged hop-locally between the relay and the mule over the 802.11ah IBSS link
(EtherType `0x88B5`, hop-by-hop AES-128-CCM). Each step maps to a specific frame
type from the registry (Section 6.3):

```
Handshake step   Frame type           OSI layer    Carries
─────────────────────────────────────────────────────────────────────────
MANIFEST         CUSTODY_REQ  (0xB)   L2 Data Link  bundle list (ids, sizes,
                                                    priority, dst) — metadata
ACCEPT           CUSTODY_ACK  (0xC)   L2 Data Link  accepted_ids, storage_kb
DATA             DATA         (0x8)   L2 frame       the BUNDLE itself, which is
                                      carrying an    an L4 BPv7 unit (payload
                                      L4 bundle      BPSec-encrypted at L5)
ACK              ACK          (0x9)   L2 Data Link  hop-level "frame received"
CONFIRM          CUSTODY_ACK  (0xC)   L2 Data Link  confirmed_id, storage_kb

  All five ride in 802.11ah IBSS frames, hop-by-hop AES-128-CCM, L1 = 900 MHz.
```

The key distinction: the **handshake** (MANIFEST/ACCEPT/ACK/CONFIRM) is entirely
L2 control signalling — it governs *responsibility and flow*, not payload. Only
the **DATA** step carries an L4 bundle (and that bundle's payload stays
BPSec-encrypted end-to-end at L5; the mule, as custodian, moves it without
reading it). So custody is an L2 negotiation wrapped around an L4 payload
transfer:

```
  L4/L5   the bundle + its BPSec-encrypted payload (DATA step only)
  L2      MANIFEST · ACCEPT · ACK · CONFIRM  — the custody handshake
  L2      hop-by-hop AES-128-CCM on every frame
  L1      900 MHz HaLow
```

**Full sequence — mule arrival to departure:**

```
 Relay R (isolated, has bundles)            Mule M (sweeping through)
   │                                            │
   │                          ┌─ ARRIVAL ───────┤
   │◄──── HELLO {MULE flag, storage_kb} ─────────│  M enters range; RSSI
   │      (L2, broadcast, 0x1)                   │  rising. R detects M,
   │                                            │  verifies mule_id (Issue #9)
   │                                            │
   │                          ┌─ HANDSHAKE ─────┤
   │──── CUSTODY_REQ {MANIFEST: ids, ───────────►│  ① R offers its bundles
   │      sizes, priority, dst} (L2, 0xB) ───────│     (metadata only)
   │                                            │  ② M selects ALERT-first,
   │                                            │     then oldest, to fit
   │◄──── CUSTODY_ACK {ACCEPT: accepted_ids, ────│     storage_kb
   │      storage_kb} (L2, 0xC) ─────────────────│
   │                                            │
   │                          ┌─ TRANSFER ──────┤  (repeat per accepted bundle)
   │──── DATA (bundle 1) (0x8, carries ─────────►│  ③ R sends the L4 bundle
   │      L4 BPv7 bundle, payload BPSec) ────────│     (payload stays encrypted)
   │◄──── ACK (L2, 0x9) ─────────────────────────│  ④ M acks frame receipt
   │◄──── CUSTODY_ACK {CONFIRM: id, ─────────────│  ⑤ M takes RESPONSIBILITY
   │      storage_kb} (L2, 0xC) ─────────────────│
   │   ┌──────────────────────────────────────┐  │
   │   │ EAGER:    R deletes bundle 1          │  │  ⑥ retention decision
   │   │ RETAINED: R keeps it, awaits E2E ack  │  │     (Section 9.5.7)
   │   └──────────────────────────────────────┘  │
   │      ... repeat ③–⑥ for bundles 2..N ...     │
   │                                            │
   │                          ┌─ COMPLETION ────┤
   │──── CUSTODY_REQ {DONE} (L2, 0xB) ──────────►│  ⑦ R has no more (or M full)
   │◄──── CUSTODY_ACK {DONE} (L2, 0xC) ──────────│
   │                                            │
   │                          ┌─ DEPARTURE ─────┤
   │  (R observes M's HELLO RSSI past peak,      │  ⑧ window closing inferred
   │   falling → window closing, Section 9.5.9)  │     by R from RSSI trend
   │                                            │     (M makes no decision)
   │                              M sweeps away ─┤
   │  Unconfirmed bundles (if any) retained      │  ⑨ no loss: confirm-before-
   │  for the next mule pass.                     │     delete guarantees it
   │                                            │
   │   (later, RETAINED mode only:)              │
   │◄═══ E2E_CUSTODY_ACK {DELIVERED} ════════════╡  ⑩ once the bundle reaches
   │   (L4 BUNDLE, not L2 — travels end-to-end)  │     its destination, an
   │   R deletes its retained copy ✅            │     end-to-end ack returns
```

The only L4 elements in the whole exchange are the **bundle** carried in the
DATA step (③) and the **E2E_CUSTODY_ACK** (⑩), which is itself a bundle because
it must travel end-to-end back to the source. Everything else — MANIFEST,
ACCEPT, ACK, CONFIRM, DONE — is hop-local L2 signalling that never leaves the
relay↔mule link.

#### 9.5.2 CUSTODY_REQ Payload (CBOR)

```
{
  "phase":   <uint8: 0=MANIFEST, 1=DONE>,
  "bundles": [                    // phase=MANIFEST only
    {
      "id":       <bytes: src_eid || creation_ts_ms>,
      "size":     <uint32: bundle size in bytes>,
      "created":  <uint64: ms since epoch>,
      "priority": <uint8: 0=NORMAL, 1=ALERT>,
      "dst":      <string: destination EID>
    },
    ...
  ]
}
```

#### 9.5.3 CUSTODY_ACK Payload (CBOR)

```
{
  "phase":        <uint8: 0=ACCEPT, 1=CONFIRM, 2=DONE, 3=FULL, 4=BUSY>,
  "accepted_ids": [<bytes>, ...]  // phase=ACCEPT only
  "confirmed_id": <bytes>,        // phase=CONFIRM only
  "storage_kb":   <uint16>        // mule remaining storage after this ack
}
```

Phase `BUSY` (4) is returned when a relay sends a CUSTODY_REQ while the mule is
already in a custody session with another relay (Section 9.5.10). The requesting
relay backs off and retries; the mule serves one relay at a time.

#### 9.5.4 Bundle Selection (Mule)

On receiving a MANIFEST, the mule selects bundles to accept:

```
1. Sort manifest entries:
     primary key:   priority DESC  (ALERT before NORMAL)
     secondary key: created ASC    (oldest first within priority)

2. Accumulate from sorted list until:
     sum(size) >= storage_available_kb × 1024
   or manifest exhausted.

3. Return accepted subset in CUSTODY_ACK ACCEPT.
```

#### 9.5.5 Mule Departure (Window Closing)

Under the never-stops model (Section 9.5.9) there is no discrete "departure"
event the mule decides — a sweeping mule is always moving out of range. The
relay detects the closing window itself and wraps up cleanly:

```
Relay-inferred window closing (normal case):
  Relay tracks the mule's HELLO RSSI. Once it peaks and begins falling,
  the relay stops initiating new CUSTODY_REQ transfers it cannot finish
  and lets any in-flight transfer complete. Unconfirmed bundles are
  retained for the next pass. No flag from the mule is required.

Timeout fallback (link lost abruptly):
  Mule stops being heard. NEIGHBOR_TIMEOUT expires on the relay.
  Relay retains all bundles not yet CONFIRMed.
  No data loss: unconfirmed bundles stay in the relay store.

Genuine mule-side unavailability (distinct from sweeping past):
  A mule that is about to go offline for a REAL reason — storage full,
  or rebooting for self-update — MAY advertise that state in HELLO so a
  relay does not begin a doomed handshake. This is mule-side STATE, not
  a "leaving your range" declaration (which only the relay can determine).
```

Both the normal and fallback cases converge on the same safe outcome,
because the relay deletes a bundle only on CONFIRM (Section 9.5.0): whatever
was not confirmed before the window closed simply waits for the next mule pass.

#### 9.5.6 Gateway Deduplication

A bundle may arrive at the gateway via both the mule path and the mesh path (if connectivity was restored). Gateway deduplicates using the bundle unique key:

```
bundle_unique_key = src_eid || creation_ts_ms
```

Second arrival of a key already processed is silently dropped.

#### 9.5.7 Custody Reliability and Single-Point-of-Loss

The base custody protocol (9.5.1) deletes a relay's copy of a bundle as soon
as it receives CUSTODY_ACK CONFIRM from the mule. This is safe *during* the
transfer, but it creates a problem *after*: once the relay deletes its copy,
the bundle exists in only one place — the mule. If the mule never reaches a
node that can progress the bundle toward its destination, the bundle expires
and is lost, with no notification to the original source.

```
The single-point-of-loss problem:

  Relay R1                      Mule                    (intended) Gateway
    │                            │                            │
    │── bundle (custody) ───────►│                            │
    │◄─ CUSTODY_ACK CONFIRM ─────│                            │
    │  R1 DELETES its copy ✗     │                            │
    │  (bundle now exists ONLY   │                            │
    │   on the mule)             │                            │
    │                            │                            │
    │                     mule route changes,                 │
    │                     battery dies, flash                 │
    │                     corrupts, or lifetime               │
    │                     expires while parked                │
    │                            │                            │
    │                       bundle LOST ✗                     │
    │  (R1 already deleted it, trusting the mule)             │
```

Custody transfer **concentrated** the risk into a single mobile point of
failure. To address this, Rimba defines two custody retention modes:

```
Mode 1 — EAGER (base protocol, 9.5.1):
  Relay deletes its copy on CUSTODY_ACK CONFIRM.
  Lowest storage cost. Highest loss risk.
  Use when: store is under pressure, bundle is low-priority,
            or mule is known-reliable (e.g. wired-charging drone
            on a fixed dependable route).

Mode 2 — RETAINED (reliable custody):
  Relay marks the bundle "custody delegated, awaiting end-to-end ACK"
  but does NOT delete it. The relay keeps the copy until either:
    (a) an end-to-end CUSTODY_ACK propagates back confirming the
        bundle reached its destination, OR
    (b) the bundle lifetime expires (then delete — delivery failed), OR
    (c) store pressure forces eviction (9.6.2 — delegated-but-unconfirmed
        bundles are evicted only after all other categories).
  Higher storage cost. Near-zero loss risk.
  Use when: bundle is ALERT priority, or mule reliability is uncertain.
```

The retention mode is carried in the bundle's custody flag:

```
Bundle custody_mode field (in BPv7 block, 1 byte):
  0 = EAGER    (delete on CONFIRM)
  1 = RETAINED (keep until end-to-end ACK or expiry)

Default: ALERT bundles → RETAINED, NORMAL bundles → EAGER.
Configurable per deployment.
```

**End-to-end custody acknowledgement:**

For RETAINED bundles, the destination (or first node with a reliable path
to it) emits an end-to-end CUSTODY_ACK that propagates back toward the source:

```
  Source-side                                          Destination
  Relay R1          Mule          Relay R9          Gateway
    │                │              │                  │
    │── bundle ─────►│ (RETAINED:   │                  │
    │   R1 keeps copy│  R1 holds)   │                  │
    │                │── bundle ───►│                  │
    │                │              │── bundle ───────►│
    │                │              │                  │ delivers to cloud
    │                │              │◄─ E2E_CUSTODY_ACK│ {bundle_id, DELIVERED}
    │                │◄─ E2E_ACK ───│ (via mesh)       │
    │◄─ E2E_ACK ─────│ (next sweep, │                  │
    │  R1 NOW deletes│  or via mesh │                  │
    │  its copy ✅   │  if path     │                  │
    │                │  restored)   │                  │
```

The end-to-end CUSTODY_ACK travels back by whatever path is available —
through the mesh if connectivity was restored, or carried by the next mule
sweep. It is itself a small bundle (`ipn:<source>.custody_ack`) subject to
normal DTN delivery.

```
E2E_CUSTODY_ACK bundle payload (CBOR):
{
  "ack_type":     <uint8: 0=DELIVERED, 1=EXPIRED, 2=DROPPED>,
  "bundle_id":    <bytes: src_eid || creation_ts_ms of original bundle>,
  "delivered_at": <uint64: ms since epoch — when destination received it>,
  "via":          <6-byte: node_id that completed delivery>
}
```

**Custody timeout and re-delegation:**

A mule holding a RETAINED bundle that it cannot deliver should not hoard it
indefinitely. After `MULE_CUSTODY_TIMEOUT_S` (default 86,400s = 24h) without
progressing the bundle, the mule:

```
1. Continues carrying the bundle (does not delete — source still has copy)
2. Actively offers it to every relay it meets (not just gateways)
3. Will hand custody to another mule if that mule has a better route
   (mule-to-mule custody transfer, 9.5.8)
4. If lifetime expires: drops the bundle, emits E2E_CUSTODY_ACK{EXPIRED}
   so the source-side custodian learns delivery failed
```

This trades storage for reliability. Given the 256 KB store constraint
(9.6), the eviction policy (9.6.2) ensures RETAINED-but-unconfirmed bundles
are kept preferentially but still yield to store pressure as a last resort —
at which point the bundle reverts to EAGER semantics (the carrier becomes the
only copy).

#### 9.5.8 Downlink Mule Delivery (Bundles for a Relay or Leaf)

The uplink case (bundle destined for a gateway) is straightforward because
the gateway is an anycast destination — *any* gateway satisfies `ipn:1.*`.
Downlink bundles destined for a **specific relay or leaf** are harder: there
is exactly one valid destination, and for leaves that destination may be
asleep or mobile.

```
Destination type comparison:

  Uplink   ipn:1.*        ANY gateway works (anycast) — easy
  Downlink ipn:<relay>.*  ONE specific relay
           ipn:<leaf>.*   ONE specific leaf (asleep / possibly mobile)
```

**Key principle: the mule delivers to the destination's *relay*, not the leaf
directly, and only needs to reach *any node with a mesh path* to the
destination — not the destination itself.**

```
The mule bridges isolated clusters; the mesh does the final hops:

  ┌─ Cluster A (isolated) ─┐         ┌─ Cluster B ──────────────┐
  │                        │         │                          │
  │   Relay R_a            │         │   Relay R_b ─── Relay R_c │
  │      ▲                 │  mule   │      ▲            │       │
  │      │ ① mule arrives  │ travels │      │ ③ RREQ     ▼       │
  │      │   R_a: no route │ ──────► │      │   finds   Leaf L42 │
  │      │   to L42        │         │      │   route   (target) │
  │      │   mule RETAINS  │         │      │   to L42           │
  │      │   custody       │         │   ④ mule → R_b custody    │
  │                        │         │   ⑤ R_b → mesh → R_c →    │
  └────────────────────────┘         │      L42 on next wake ✅  │
                                      └──────────────────────────┘

  The mule never physically reached R_c (L42's relay).
  It only needed to reach R_b, which had a mesh path to L42.
```

**The "can you reach X?" custody query:**

Before transferring custody of a downlink bundle, the mule must confirm the
relay can actually progress it. This reuses RREQ, but initiated on behalf of
a stored bundle rather than live traffic:

```
Mule arrives at Relay R_x, holding a bundle for node X:

① Mule → R_x: CUSTODY_REQ MANIFEST (includes dst EID = X for each bundle)
② R_x evaluates each bundle's destination:
     Is X in my peer table?        → I can deliver directly (leaf) ✅
     Do I have a cached route to X? → I can forward via mesh ✅
     Issue RREQ for X → RREP comes back? → route exists ✅
     No route, X not my peer?       → I cannot progress this bundle ✗
③ R_x → Mule: CUSTODY_ACK ACCEPT, but ONLY for bundles it can progress
     accepted_ids = [bundles R_x has a path for]
     rejected_ids = [bundles R_x cannot reach]
④ Mule transfers only accepted bundles, RETAINS the rejected ones
     for the next relay it meets
```

This is the critical difference from uplink: a mule does **not** blindly dump
all bundles on the first relay. It transfers a downlink bundle only to a relay
that can demonstrably move it closer to the destination.

**Updated CUSTODY_ACK ACCEPT for downlink:**

```
CUSTODY_ACK (phase=ACCEPT) payload, extended:
{
  "phase":        <uint8: 0=ACCEPT>,
  "accepted_ids": [<bytes>, ...],   // bundles R_x can progress
  "rejected_ids": [<bytes>, ...],   // bundles R_x cannot reach (mule retains)
  "reach_status": [                 // why each was accepted (optional, for diagnostics)
    { "id": <bytes>, "reach": <uint8: 0=local_peer, 1=mesh_route, 2=rreq_confirmed> },
    ...
  ],
  "storage_kb":   <uint16>
}
```

**Leaf delivery handoff (mule → relay → leaf):**

A mule never waits for a sleeping leaf. It hands the bundle to the leaf's
relay, and the relay's proxy delivery (Section 8.3) completes the final hop:

```
① Mule reaches R_c (the relay leaf L42 is peered with)
   OR a relay with a mesh route to R_c
② Mule → R_c: custody transfer of L42-bound bundle
③ Mule departs — its job is done
④ R_c holds the bundle in its store
⑤ L42 wakes on its normal 15-min cycle
⑥ R_c delivers the bundle to L42 (proxy delivery, Section 8.3)
⑦ L42 sends bundle-level ACK
⑧ R_c emits E2E_CUSTODY_ACK{DELIVERED} back toward source
   (for RETAINED bundles — so the source-side custodian can delete)
```

**What happens if the target leaf has moved:**

```
Mule delivers bundle for L42 to R_c (L42's last-known relay).
L42 has since moved to R_d (different relay).

R_c behaviour:
  L42 no longer in peer table.
  R_c issues RREQ for L42 → if L42 is now peered with R_d and R_d
    is mesh-reachable → RREP → R_c forwards bundle to R_d → L42.
  If L42 unreachable (moved to an isolated cluster):
    R_c retains the bundle (RETAINED mode) until L42 reappears
    or bundle lifetime expires.
```

This reuses the RERR/RREQ machinery (Section 8.6) that already handles leaf
mobility for live traffic — the stored bundle simply triggers a fresh RREQ
when the leaf is not where expected.

**Downlink delivery failure:**

```
If no relay the mule ever meets has a path to the destination, AND
no future mule sweep finds one before lifetime expiry:

  Bundle expires → mule emits E2E_CUSTODY_ACK{EXPIRED} toward source.
  Gateway (original source of downlink command) learns the command
  was never delivered → can re-issue or escalate.

This is the same loss condition as uplink, but downlink is MORE
exposed because the destination is a single specific node, not an
anycast gateway. RETAINED mode (9.5.7) is therefore the recommended
default for all downlink bundles.
```

#### 9.5.9 Mule Contact Window and Non-Stopping Sweeps

**Design assumption: the mule never stops.** The custody protocol is designed
so that a mule (especially a drone) that simply *sweeps through* an isolated
cluster at speed, without pausing, still transfers data correctly. A mule that
*does* stop is treated as the favourable case — it widens the contact window and
lets more data move per pass — but stopping is never *required* for correctness.

```
The contact window is bounded by mule speed and link range:

  Drone at 10 m/s, ~1 km LoS range:  window ≈ 2 km / 10 = ~200 s (generous)
  Drone at 20 m/s, ~300 m NLoS:      window ≈ 600 m / 20 = ~30 s (typical)
  Fast pass, marginal link:          < 10 s usable (degraded SNR at edges)

The protocol must work across this whole range, degrading gracefully —
never assuming the upper end.
```

**Why a non-stopping sweep is safe.** The custody rules from 9.5.0/9.5.7 already
guarantee no loss on a truncated pass, because a node deletes a bundle *only* on
CONFIRM (EAGER) or end-to-end ack (RETAINED):

```
Drone sweeps in, transfers bundles 1–5, exits range before 6–10:

  Bundles 1–5:  received CONFIRM → safely with the mule (deleted in
                EAGER, or retained-pending in RETAINED)
  Bundles 6–10: NO CONFIRM → relay KEEPS them, untouched → no loss

A partial pass just makes partial progress. The unconfirmed remainder
waits for the NEXT mule pass. Large backlogs drain over MULTIPLE passes,
each pass taking what the window allows and confirming it. The mule never
has to stop; it transfers what it can per pass.
```

**Mechanisms that make short windows effective:**

```
1. Value-first, completable-first selection (extends 9.5.4):
   The mule already selects ALERT-first, then oldest-first. For a short
   window it should also prefer bundles it can TRANSFER AND CONFIRM within
   the estimated remaining contact time — so a truncated pass still moves
   the highest-value data. ALERT bundles cross on the earliest pass even
   if routine telemetry waits for a later one.

2. Contact-window estimation:
   A mule with GPS (speed/heading) and a HELLO-RSSI trend estimates
   remaining contact time and stops STARTING transfers it cannot finish,
   avoiding wasted partial DATA frames near the edge of range.

3. Per-bundle resume across passes (free, no per-byte state):
   Because custody is per-bundle with explicit CONFIRM, resumption is
   natural: on the next pass the relay re-offers only its still-unconfirmed
   bundles in the MANIFEST. The bundle is the unit of progress — no
   byte-level checkpointing needed.

4. Relay-inferred window-closing (NOT a mule-declared event):
   Under never-stops there is no "departing" decision for the mule to
   make — it is always moving, always approaching exit. The
   window-closing signal therefore comes from the RELAY observing the
   mule's HELLO RSSI trend: rising as the mule approaches, peaking at
   closest approach, falling as it leaves. Once RSSI is past peak and
   dropping, the relay knows contact time is running out and wraps up —
   completes in-flight CONFIRMs, stops starting new transfers, and
   retains the unconfirmed remainder for the next pass. This needs no
   decision or flag from the mule; it is pure link physics measured at
   the relay. A mule with GPS MAY advertise its speed/heading/position
   to let a relay (that knows its own position) estimate the window more
   precisely, but the mule never self-declares "departing" for a specific
   relay — it cannot know which relays are in range or when it passes out
   of any one's range.
```

**Stopping increases yield (the bonus case).** When a mule *can* stop — a drone
hovering or landing near a relay, a vehicle pausing — the contact window becomes
effectively unbounded, so:

```
  - the entire backlog can drain in a single pass (no multi-pass wait)
  - bulk transfers that don't fit a fast pass become feasible (see below)
  - more leaves in the cluster can be serviced within one visit

Deployments that can schedule mule stops at known isolated clusters will
see higher data yield per visit and lower end-to-end latency. This is an
operational optimisation, NOT a protocol requirement — the network is
correct either way.
```

**Bulk-transfer caveat.** Small bundles (telemetry, alerts) transfer in well
under a second of airtime and fit even a fast pass. But a *large* back-haul — an
OTA image being carried back, or a big accumulated data blob — may not fit a
single fast sweep. Such transfers require either a stopping mule, or multiple
passes using the chunked, resumable streaming path (OTA_CHUNK `seq_num`,
Section 14.4a) rather than the single-shot custody DATA transfer. The custody
layer moves whole bundles per pass; bulk streaming is the mechanism for payloads
larger than one contact window.

```
Summary:

  Mule never stops (baseline):  correct by design. Per-pass partial
                                transfer, confirm-before-delete, multi-pass
                                drainage, ALERT-first. No loss ever.
  Mule stops (bonus):           wider window → full drain per pass, bulk
                                transfers feasible, more leaves serviced.
                                Higher yield, lower latency. Optional.
```

#### 9.5.10 Multiple Relays Sharing One Mule

A mule sweeping into a cluster may be heard by several isolated relays at once,
all with bundles to hand off. A mule has **one radio** and custody transfer is a
**stateful pairwise session** (MANIFEST → ACCEPT → DATA → ACK → CONFIRM, Section
9.5.1) — it cannot run simultaneous sessions with several relays. The relays
therefore take turns. There is no central scheduler; turn-taking emerges from
three mechanisms.

```
                          ┌──────────────┐
                          │   Mule M     │  one radio → one custody
                          │ storage_kb=X │  session at a time
                          └──────┬───────┘
            broadcast HELLO {MULE flag, storage_kb}
            ┌────────────────────┼────────────────────┐
            ▼                    ▼                    ▼
      ┌──────────┐         ┌──────────┐         ┌──────────┐
      │ Relay R1 │         │ Relay R2 │         │ Relay R3 │
      └──────────┘         └──────────┘         └──────────┘
       all hear the same HELLO; all want to hand off bundles
```

```
1. CSMA/CA serializes session starts.
   Each relay's CUSTODY_REQ is an ordinary frame on the shared 802.11ah
   channel. Carrier-sense + random backoff means only one relay's
   CUSTODY_REQ wins the medium at a time; the others sense the channel
   busy and defer. This naturally orders who starts first.

2. The mule holds ONE session at a time (stateful lock).
   Once the mule sends CUSTODY_ACK ACCEPT to R1, it is "in session" with
   R1 and will not begin a MANIFEST exchange with R2/R3. A CUSTODY_REQ
   arriving from another relay mid-session is answered with CUSTODY_ACK
   BUSY (or simply not ACCEPTed); that relay backs off and retries. When
   R1's session ends (DONE, or the contact window closes), the mule is
   available again.

3. storage_kb advertises the shrinking budget in real time.
   Every CUSTODY_ACK carries storage_remaining_kb, and the mule's next
   HELLO advertises the reduced storage_kb. R2 and R3 see how much space
   is left before they start. When storage reaches 0, remaining relays
   retain everything for the next mule — no transfer is attempted.
```

```
Sequence across a 3-relay cluster (one mule, 200 KB free):

  M sweeps in → HELLO {MULE, storage=200KB}, heard by R1, R2, R3

  R1 wins medium → CUSTODY_REQ MANIFEST → M ACCEPT (R1 session)
     DATA/ACK/CONFIRM per bundle → M storage now 120 KB
     (R2, R3 sense busy → defer; a CUSTODY_REQ from them → BUSY)

  R1 DONE → M free again → next HELLO {storage=120KB}
  R2 wins medium → CUSTODY_REQ → M ACCEPT (R2 session)
     transfers → M storage now 60 KB

  R3 → CUSTODY_REQ → M ACCEPT what fits in 60 KB
     (R3 offers ALERT-first; rest retained for next mule)

  M window closes (RSSI past peak, 9.5.9) → M sweeps onward
  Anything not transferred stays in each relay (confirm-before-delete)
```

```
Properties:
  - Fair by contention: no relay is starved by design, though the relay
    that wins the medium first transfers first.
  - Storage-bounded: the mule fills up; later relays retain their
    remainder for the next pass. No loss (confirm-before-delete, 9.5.0).
  - No central coordination: turn-taking is emergent (CSMA + one-session
    lock + advertised storage), consistent with the rest of Rimba.
```

**Known limitation — no cross-relay priority (Issue #16).** Each relay offers
its *own* bundles ALERT-first, but the mule has no mechanism to prefer one
relay's ALERT bundle over another relay's routine telemetry. Whichever relay
wins the medium first transfers first, regardless of relative bundle priority
across relays. In a cluster where a low-priority relay wins the medium and fills
the mule's storage, a higher-priority ALERT bundle on another relay waits for
the next pass. A future refinement could have the mule run a short MANIFEST-only
collection round from all in-range relays before accepting any DATA, then accept
globally ALERT-first — at the cost of more handshake overhead in the contact
window. Recorded as Open Issue #16; not addressed in v1.

#### 9.5.11 Complete Flow — One Mule, Several Relays, Arrival to Departure

This is the full picture: a single mule sweeps into a cluster of three isolated
relays, services each one in turn (one custody session at a time), and leaves —
all within one bounded contact window, with no central coordinator. It ties
together the handshake (9.5.1a), the never-stops window model (9.5.9), and the
multi-relay turn-taking (9.5.10).

```
 Mule M (200 KB free)     Relay R1        Relay R2        Relay R3
   │                         │               │               │
 ══╪═════════════════════ ARRIVAL ═══════════╪═══════════════╪══════
   │                         │               │               │
   │── HELLO {MULE, 200KB} ─►│  (broadcast, all three hear it; RSSI rising)
   │───────────────────────────────────────► │               │
   │─────────────────────────────────────────────────────────►
   │                         │ verify mule_id │               │
   │                         │ (Issue #9)     │               │
   │                         │               │               │
 ══╪═══════════════════ R1 SESSION ══════════╪═══════════════╪══════
   │◄── CUSTODY_REQ MANIFEST ─│  R1 wins medium (CSMA)        │
   │── CUSTODY_ACK ACCEPT ───►│  M selects ALERT-first        │
   │   {accepted, 200KB}      │  (R2,R3 sense busy → defer;   │
   │                         │   if they try → BUSY)          │
   │◄── DATA (bundle) ────────│  ┐                            │
   │── ACK ──────────────────►│  │ per bundle:                │
   │── CUSTODY_ACK CONFIRM ──►│  │ transfer, ack, confirm     │
   │   {id, 160KB}            │  ┘ R1 deletes/retains          │
   │     ... repeat ...       │    (EAGER/RETAINED)           │
   │◄── CUSTODY_REQ DONE ─────│  R1 has no more               │
   │── CUSTODY_ACK DONE ─────►│  M now 120 KB                 │
   │                         │               │               │
 ══╪═══════════════════ R2 SESSION ══════════╪═══════════════╪══════
   │◄────── CUSTODY_REQ MANIFEST ─────────────│  R2 wins medium next
   │──────── CUSTODY_ACK ACCEPT {120KB} ─────►│
   │◄────── DATA / ACK / CONFIRM (per bundle) │  M now 60 KB
   │◄────── CUSTODY_REQ DONE ─────────────────│
   │──────── CUSTODY_ACK DONE ───────────────►│
   │                         │               │               │
 ══╪═══════════════════ R3 SESSION ══════════╪═══════════════╪══════
   │◄────────────── CUSTODY_REQ MANIFEST ─────────────────────│  R3's turn
   │─────────────── CUSTODY_ACK ACCEPT {60KB} ───────────────►│  only 60 KB left
   │◄────────────── DATA / ACK / CONFIRM ─────────────────────│  M accepts what
   │   (ALERT bundles first; rest exceed 60KB → NOT accepted)  │  fits, ALERT-first
   │─────────────── CUSTODY_ACK FULL ────────────────────────►│  M storage = 0
   │                         │               │   R3 RETAINS the remainder
   │                         │               │   for the next mule pass
   │                         │               │               │
 ══╪═══════════════════ DEPARTURE ═══════════╪═══════════════╪══════
   │  (each relay independently observes M's HELLO RSSI        │
   │   peak then fall → window closing, 9.5.9. M makes no      │
   │   "departing" decision — it just keeps moving.)           │
   │                         │               │               │
   │            M sweeps onward toward a gateway / next cluster │
   │                         │               │               │
   │  Carried now: R1's bundles + R2's bundles + R3's ALERT.   │
   │  Left behind (safely retained): R3's overflow → next pass. │
   │                         │               │               │
   │  (later, RETAINED bundles only:)                          │
   │  ◄═══ E2E_CUSTODY_ACK {DELIVERED} ═══  once each bundle    │
   │       reaches its destination, the ack returns to the     │
   │       origin relay, which deletes its retained copy.      │
```

```
What this illustrates:
  - ONE session at a time: M serializes R1 → R2 → R3 (CSMA picks order;
    BUSY defends the lock). Never parallel.
  - Storage drains across relays: 200 → 120 → 60 → 0 KB. Later relays
    see the shrinking budget in storage_kb and in M's HELLO.
  - Graceful saturation: when M fills (FULL), R3 keeps its overflow.
    No loss — confirm-before-delete (9.5.0) guarantees it.
  - No departure event: each relay infers the closing window from RSSI;
    M never declares it. Whatever wasn't transferred waits for next pass.
  - Per-relay retention modes are independent: R1 may run EAGER, R2/R3
    RETAINED — each relay's bundles follow that relay's policy.
  - End-to-end closure is asynchronous and per-bundle: E2E acks trickle
    back as bundles reach destinations, long after M has left the cluster.
```

The whole exchange needs no scheduler, no roster, and no coordination between
the relays — turn-taking is emergent (CSMA + one-session lock + advertised
storage), the window is self-detected (RSSI), and loss is structurally
impossible (delete only on CONFIRM). A mule that stops simply widens the window
so more — or all — of R3's overflow also fits in this pass (Section 9.5.9).

### 9.6 Relay Bundle Store Management

#### 9.6.1 Storage Thresholds

| Threshold | Value | Behaviour |
|---|---|---|
| `STORE_LOW_PCT` | 80% | Advertise `store_pct` in HELLO. Normal operation. |
| `STORE_HIGH_PCT` | 95% | Refuse inter-relay custody requests. Accept own-leaf bundles only. |
| `STORE_EVICT_PCT` | 99% | Begin eviction before accepting new bundles. |

#### 9.6.2 Eviction Order

When `STORE_EVICT_PCT` is reached, evict in this order:

```
1. Expired bundles (creation_ts + lifetime_ms < now)
2. Oldest NORMAL priority bundles (oldest created_ts first)
3. Oldest ALERT priority bundles
4. RETAINED-delegated bundles      ← last resort only
   (custody delegated to a mule, awaiting end-to-end ACK —
    the mule still has a copy, so eviction here reverts the
    bundle to EAGER semantics: the carrier becomes the only copy)
```

A bundle pending CUSTODY_ACK CONFIRM from a mule (transfer in progress) is
never evicted. RETAINED-delegated bundles (transfer complete, awaiting
end-to-end delivery confirmation, Section 9.5.7) are evicted only as the
final category — kept preferentially over nothing, but yielding to store
pressure before data loss of bundles that have no carrier at all.

#### 9.6.3 Store Fullness Signalling

HELLO payload gains a `store_pct` field:

```
"store_pct": <uint8: 0–100>
```

**Mule behaviour**: sort relay visit order by `store_pct` descending — visit fullest relays first.

**Neighbouring relay behaviour**: do not offer inter-relay custody to a relay with `store_pct >= STORE_HIGH_PCT`.

**Leaf behaviour**: leaves do not check `store_pct` (they only submit to their local relay). If the relay is full, the relay evicts older data to make room for the leaf's fresher reading.

### 9.7 Bundle Aggregation

To reduce frame count and channel contention in leaf-dense deployments, a relay combines multiple bundles destined for the same next-hop into a single mesh frame for one hop. The next-hop separates them back into individual bundles.

#### 9.7.1 Principle

Aggregation is a hop-by-hop transport optimisation only. It does not alter end-to-end bundle semantics:

```
Each bundle retains:
  - its own BPSec encryption (E2E to final destination)
  - its own lifetime and creation timestamp
  - its own custody status

The relay concatenates bundles sharing the same next_hop into
one mesh DATA frame. The aggregate frame is hop-encrypted with
the next-hop session key (AES-128-CCM). Inner bundles remain
E2E-encrypted and opaque to the forwarding relay.
```

Security is preserved: the outer frame is encrypted to the next hop, the inner bundles stay encrypted to their final destinations.

**Structural aggregation only — not semantic compression.**

Because bundles are E2E encrypted, a relay cannot read their payloads. Rimba's aggregation reduces the number of channel access events (frames on the air) but does not compress or summarise the underlying sensor data. This is a fundamental difference from protocols such as LEACH or Directed Diffusion, where the aggregating node can read, average, and compress readings.

```
LEACH cluster head:    reads 100 temperature values,
                       transmits 1 average → 100× data reduction

Rimba relay:           receives 100 encrypted bundles,
                       transmits them in ~5 frames → 20× fewer
                       channel access events, same data volume
```

Applications that require semantic aggregation (averaging, min/max, event counting) must do so at the application layer on the leaf node **before** bundle creation and BPSec encryption. The resulting single bundle then travels through the mesh as normal. Rimba provides no facility for in-network data reduction after encryption.

#### 9.7.2 Aggregate Frame Format

An aggregated mesh DATA frame carries a count and a length-prefixed list of bundles:

```
Mesh header (type=DATA, FRAG flag clear, AGG flag set)
┌──────────────────────────────────────────────────┐
│ agg_count    (uint8)   number of bundles          │
├──────────────────────────────────────────────────┤
│ bundle_1_len (uint16)  │ bundle_1 bytes           │
├──────────────────────────────────────────────────┤
│ bundle_2_len (uint16)  │ bundle_2 bytes           │
├──────────────────────────────────────────────────┤
│ ...                                                │
└──────────────────────────────────────────────────┘
```

A new flag bit is added to the mesh header (Section 6.2):

| Bit | Name | Meaning |
|---|---|---|
| 3 | `AGG` | Payload is an aggregate of multiple bundles |

(Previously reserved; bits 2–0 remain reserved.)

#### 9.7.3 Aggregation Trigger

A relay holds bundles for the same next-hop in an aggregation queue and sends the combined frame when ANY condition is met:

```
- Aggregate size would exceed AGG_MAX_BYTES (default 1400,
  staying under the 802.11ah MSDU limit)
- AGGREGATION_HOLD_MS elapsed since the first bundle entered
  the queue (default 2000ms)
- A frame with the `ALERT` flag (bit 2) arrives — flush entire
  queue immediately, then forward the ALERT frame standalone
- The next-hop is a mule whose contact window is closing (RSSI past
  peak, Section 9.5.9) or that signals genuine unavailability (flush immediately)
- The node enters a contact window that may close soon
```

The default `AGGREGATION_HOLD_MS` of 2000ms reflects the sparse-first assumption that latency is cheap. Deployments needing lower latency can reduce it at the cost of less aggregation benefit.

#### 9.7.4 De-aggregation

A node receiving a frame with the `AGG` flag set:

```
1. Decrypt the hop layer (AES-128-CCM, next-hop session key).
2. Read agg_count.
3. For each bundle: read bundle_N_len, extract bundle_N bytes.
4. Process each bundle independently:
     - if dst_node == self: deliver to DTN/app layer
     - else: route onward (may re-aggregate for the next hop)
```

Each extracted bundle re-enters the normal forwarding path and may be re-aggregated differently for its next hop, since bundles sharing a hop here may diverge later.

#### 9.7.5 Effectiveness

```
Leaf-dense example: relay serving 100 leaves, each sending
a 50-byte reading per interval, all destined for the gateway.

Without aggregation:
  100 separate mesh frames forwarded toward gateway.

With aggregation (1400-byte frames, ~60-byte bundles incl. overhead):
  ~23 bundles per aggregate frame
  100 bundles → ~5 frames
  ~95% reduction in frame count on the relay→gateway path
```

Aggregation is most effective close to sinks (gateways) and at relays serving many leaves, where many bundles share a common next-hop. It has little effect in the sparse interior of the mesh where bundles rarely share a next-hop.

### 9.8 Alert Bundle Pattern

Leaves operate on a fixed wake schedule optimised for battery life, which introduces an average latency of half the wake interval (7.5 minutes at the default 15-minute interval). For time-sensitive events — threshold breaches, intrusion detection, equipment faults — a leaf can bypass the sleep schedule and transmit immediately using a sensor interrupt wake.

#### 9.8.1 Sensor Interrupt Wake

The ESP32-S3 supports GPIO wakeup from deep sleep (`ESP_SLEEP_WAKEUP_EXT1`). A sensor that detects an alert condition asserts a GPIO pin, waking the MCU without waiting for the RTC timer.

```
Normal leaf operation:
  RTC timer fires every LEAF_SLEEP_MS → normal wake → NORMAL bundle

Alert condition:
  Sensor GPIO asserts → ESP32-S3 wakes (ESP_SLEEP_WAKEUP_EXT1)
  → detect wakeup cause
  → if EXT1: create ALERT bundle, set ALERT flag in mesh header
  → run same wake sequence (HELLO, peer check, DATA)
  → return to normal RTC sleep schedule (unchanged)

No change to LEAF_SLEEP_MS or RTC schedule.
The alert is an out-of-band event. Normal readings resume as scheduled.
```

**ESP-IDF implementation:**

```c
// Configure sensor GPIO as deep sleep wakeup source
esp_sleep_enable_ext1_wakeup(1ULL << SENSOR_ALERT_PIN,
                              ESP_EXT1_WAKEUP_ANY_HIGH);

// On wakeup: detect cause
esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
if (cause == ESP_SLEEP_WAKEUP_EXT1) {
    rimba_send_bundle(payload, len, PRIORITY_ALERT);   // ALERT flag set
} else {
    rimba_send_bundle(payload, len, PRIORITY_NORMAL);  // normal reading
}

// Return to sleep — schedule unchanged
esp_deep_sleep_start();
```

#### 9.8.2 ALERT Flag in Mesh Header

The ALERT flag (bit 2 of the mesh header flags byte, Section 6.2) is visible to every relay along the path. It signals high-priority handling at each hop without requiring any relay to decrypt the bundle payload.

```
ALERT flag effects:
  ✗ Do NOT add to aggregation buffer (flush immediately, Section 9.7.3)
  ✗ Do NOT wait for TDMA TX slot boundary (transmit at next CSMA/CA window)
  ✓ Forward before any queued NORMAL bundles at this relay
  ✓ Preserve in bundle store (never evict, Section 9.6.2)
  ✓ Transfer to mule before NORMAL bundles (Section 9.5.4)
  ✓ Bundle lifetime: 72 hours (vs 24 hours for NORMAL)
```

The ALERT flag is set by the originating leaf and preserved hop-by-hop to the gateway. Relays do not modify it.

#### 9.8.3 Relay Forwarding of ALERT Bundles

On receiving a DATA frame with the ALERT flag set:

```
1. Do not add to aggregation queue.
   Transmit immediately as a standalone DATA frame.

2. Bypass normal DTN queue ordering.
   Forward before any pending NORMAL bundles.

3. Continuous mode relays (MCU light sleep, MM6108 always-on):
   No change needed — MM6608 is always listening,
   relay can forward immediately.

4. Scheduled mode relays (TDMA sleep):
   ALERT received during sleep slot triggers early wake.
   MCU wakes via MM6108 interrupt, processes ALERT,
   forwards immediately without waiting for scheduled TX slot.
   Returns to TDMA schedule after forwarding.
```

#### 9.8.4 Latency Comparison

```
Data type          Wake mechanism     Avg latency to gateway
──────────────────────────────────────────────────────────────
Normal reading     RTC timer (15min)  ~7.5 minutes + 450ms TX
Alert event        Sensor GPIO IRQ    < 1 second
  (connected path)                    (TX latency only, no
                                       wake interval wait)

Alert event        Sensor GPIO IRQ    Hours to days
  (DTN, isolated)                     (stored until mule or
                                       route available)
```

For alert events, the leaf's full wake-transmit sequence takes:
- MM6108 boot: ~30ms
- HELLO + peer check: ~25ms
- ALERT DATA frame TX: ~10ms
- Relay receives + immediate forward (3 hops): ~330ms
- Gateway processes: ~50ms
- **Total: ~450ms from sensor trigger to gateway receipt (connected path)**

#### 9.8.5 Two-Tier Application Pattern

Applications requiring both efficient normal reporting and fast alert delivery use the two-tier pattern:

```
Tier 1 — Normal readings (battery-efficient):
  LEAF_SLEEP_MS = 900,000ms (15 minutes)
  MCU wakes on RTC timer
  Sends NORMAL priority bundle
  Average latency: 7.5 minutes
  Battery impact: baseline (5-8 years)

Tier 2 — Alert events (low latency):
  Sensor asserts GPIO when threshold exceeded
  MCU wakes on EXT1 interrupt
  Sends ALERT priority bundle immediately
  Average latency: < 1 second (connected)
  Battery impact: ~0 (alert events are rare; each
  alert adds ~230ms × 20mA = 4.6 mAs to the budget,
  equivalent to ~0.013% of one normal wake cycle)
```

The two tiers are independent. An alert wake does not affect the RTC schedule or subsequent normal readings.

---

### 9.9 Configuration Change Mechanism

Several operations — flipping `e2e_profile`, adjusting parameters, rotating a
backend key — require pushing a configuration change to deployed nodes. A
config change is **not a special control-plane primitive**: it is ordinary
application data carried in a BPv7 bundle, subject to the same routing, DTN
store-carry-forward, and delay tolerance as any other traffic.

#### 9.9.1 OSI Placement

```
A config change is a normal bundle, not a privileged fast-path:

  L7 Application  — config semantics: { set: "e2e_profile", value: 1 }
  L5 Security     — BPSec: signed by the operator key (authenticity);
                    optionally encrypted to the target
  L4 Transport    — BPv7 bundle (store-carry-forward, EID-addressed)
  L3 Network      — Rimba mesh routing (OGM / RREQ) reaches each node
  L2 Data Link    — 802.11ah IBSS, AES-128-CCM hop-by-hop
  L1 Physical     — 900 MHz HaLow

It therefore inherits all DTN delivery characteristics — propagation is
eventual, not instant or atomic. Reliability comes from local convergence
gossip rather than a central campaign (9.9.4).
```

#### 9.9.2 Config Bundle Format

```
Config bundle:
  dst EID = ipn:0.config           (fleet-wide — flooded)
            ipn:<node_id>.config   (targeted — routed/carried to one node)

  payload (CBOR, signed):
  {
    "config_version": <uint32: monotonic version, network-wide — ORDERING>,
    "changes":        [ { "key": "e2e_profile", "value": 1 }, ... ],
    "issued_at":      <uint64: ms since epoch>,
    "operator_sig":   <bytes[64]: Ed25519 over the above, by the operator key>
  }

  config_hash = SHA-256(canonical CBOR of the payload above), truncated to
  8 bytes for advertising in HELLO. It is the content identity / integrity
  check — two nodes with the same config_hash hold byte-identical config.
```

The `operator_sig` is signed by an operator authorisation key (for ordinary
config) or, for security downgrades, an offline operator key (Section 10.5
downgrade governance). A node verifies the signature before applying any
change and ignores unsigned or badly-signed config.

**Config size limit (`CONFIG_MAX_BYTES`).** In Rimba v1 a config payload is
capped at **256 bytes**. A config bundle whose payload exceeds this is rejected
(not applied, not re-served). This keeps config:

```
- single-frame: 256 B fits comfortably in one 802.11ah frame with all
  headers — no fragmentation, a single push delivers it
- trivially cacheable: every node caches the current config in NVS to
  re-serve it during convergence gossip (9.9.4); 256 B is negligible
- genuinely config-shaped: small key-value changes only. The cap
  prevents config from becoming a backdoor for large-payload delivery —
  anything large (maps, tables, firmware) must use the OTA streaming
  path (Section 14), which exists precisely because bundles cannot carry
  large payloads (Section 14.1a).
```

256 bytes holds roughly a dozen scalar changes, or a couple of key
rotations (a 32-byte key plus framing is ~40 B). This is sufficient for v1
config needs (e2e_profile, sleep timers, rollout parameters, key rotation).

> **Open issue (urgent):** the set of parameters config is permitted to change
> — and, critically, the provision-only set it must NOT change
> (`root_secret`, `network_id`, private keys, `channel`) — is not yet
> enumerated. See Open Issue #13. This must be defined before any config
> feature ships, as it is a security boundary, not just a completeness gap.

```
Forward compatibility: CONFIG_MAX_BYTES is a protocol parameter, not a
structural limit. A future Rimba version may raise it (toward the
single-frame ceiling ~1.3 KB, or beyond into multi-frame bundles up to
the 256 KB store limit) without changing the config mechanism — the
convergence gossip, version+hash, and bundle format all scale unchanged.
v1 deliberately starts conservative; the cap is the one value to bump.
```

#### 9.9.3 Version for Ordering, Hash for Integrity

Config carries two fields that do different jobs. `config_version` is a
monotonic counter that provides **ordering** — it answers "which config is
newer." `config_hash` is the truncated SHA-256 of the config, providing
**integrity and identity** — it answers "did I get the exact right bytes" and
"are two configs truly identical." A hash alone cannot drive convergence
because hashes are unordered (you cannot tell which of two differing hashes is
newer); the version supplies that direction.

Because the same config bundle may arrive multiple times and out of order
(flooded by several relays, re-delivered by a mule), application is idempotent
and order-independent:

```
On receiving a verified config bundle:
  if config_version > node's current config_version:
    verify SHA-256(payload) matches the advertised config_hash
    if hash mismatch → reject (corruption or wrong config delivered)
    else → apply changes, persist to NVS, adopt its config_version + config_hash
  elif config_version == current AND config_hash != current config_hash:
    ANOMALY — same version, different content. Do NOT auto-apply.
    Flag/report: indicates duplicate-version issuance or tampering.
  else:
    ignore (already applied this or a newer config)

Consequences:
  - A bundle re-flooded twice is applied once.
  - An older config (v41) arriving after a newer one (v42) is ignored.
  - A node that missed v41 but receives v42 jumps straight to v42
    (configs are cumulative state, not deltas to replay in order).
  - Two DIFFERENT configs issued under the same version are DETECTED
    (different hash) rather than silently treated as identical — a
    safety property a bare counter would miss.
```

#### 9.9.4 Convergence Model — No Central Roster

Config does not propagate as a centrally-tracked campaign. There is no roster
of "all devices," no central authority counting acknowledgements, and no
defined completion checkpoint. Instead, every node runs a single local rule,
and the network converges to a consistent config on its own — the same
local-rule / emergent-behaviour pattern as Trickle and OGM flooding.

```
The local convergence rule (run by every node):

  On hearing a neighbour's HELLO (carrying config_version + config_hash):

    if neighbour.config_version < my config_version:
        push my current config bundle to that neighbour
        (I am ahead — they need what I have)

    if neighbour.config_version > my config_version:
        request their config bundle, verify its config_hash on receipt
        (they are ahead — I need what they have)

    if same version, same hash:
        nothing to do (converged with this neighbour)

    if same version, DIFFERENT hash:
        anomaly (9.9.3) — flag, do not auto-resolve

No roster. No central tracker. No definition of "all devices."
A node reasons only about the neighbours it can directly hear.
The version gives direction; the hash verifies the bytes.
```

This is anti-entropy gossip: each node closes the version gap with every
neighbour it meets, and the config spreads peer-to-peer until the whole network
converges. The network is "converged" when no node has a neighbour with a
a different `config_version`/`config_hash` — an emergent property, not a tracked one.

**Every node caches the current config bundle.** For a node to push config to a
behind peer, it must hold the actual payload, not just the version number. The
config bundle is tiny (≤256 B, CONFIG_MAX_BYTES), so every node stores the current one in NVS and
can re-serve it to any neighbour. This removes any dependency on an origin node:
after issuance, any converged node can heal any behind neighbour.

#### 9.9.5 Why Convergence, Not a Roster

```
A roster-based campaign would require the network to know the full set
of devices, track per-node acknowledgements centrally, and compute a
completion state. The convergence model needs none of that, and is
strictly more robust:

  No global knowledge:   no node (not even the gateway) needs to know
                         the full device set. The "who is all devices"
                         question does not exist.

  Self-healing:          a node that was down returns advertising an old
                         config_version; its neighbours' rule fires and pushes
                         the current config. Same rule, no special path.

  New nodes for free:    a node added mid-deployment boots at the
                         config_version it was provisioned with; neighbours
                         bring it current. No registration step.

  No completion to get wrong: there is no "campaign complete" state to
                         maintain. Convergence is emergent — it happens
                         whether or not anyone is watching.

  Stronger guarantee:    "any two nodes that can eventually exchange
                         HELLOs (directly or via a mule path) converge to
                         the same config_version + config_hash." This is a property of the
                         protocol, not of an operator's bookkeeping — a
                         node cannot be "forgotten" because no one keeps
                         a list.
```

The unavoidable caveat (true of any network): a node that is never reachable
again (destroyed) never converges. No protocol can heal a dead node; the
convergence model does not pretend to. A node that simply returns later
converges normally.

#### 9.9.6 Propagation and Mule-Bridged Convergence

```
config_version=42 issued at the gateway (gateway sets its own config to v42):

  t0:  GW=42, everyone else=41
  t1:  GW's neighbours hear HELLO(42) → pull → 42
  t2:  THEIR neighbours hear HELLO(42) → pull → 42
  ...  ripples outward, hop by hop, by the local rule
  tN:  last connected node converges to 42

  Sleeping leaf:  converges on its next wake — hears its relay's
                  HELLO(42), rule fires, pulls 42.

  Isolated cluster: stays at 41 until a mule (now 42) sweeps in. The
                  mule's HELLO(42) is heard, the cluster pulls 42. On
                  return, if the mule meets a cluster that is somehow
                  ahead, it pulls — the mule is not a special "courier,"
                  just a mobile node running the same comparison rule.

Convergence time = time for gossip to reach the most remote node:
  bounded by network diameter (connected) or mule sweep period (isolated).
```

```
Config converging by local gossip (no central tracker anywhere):

   ┌─ Connected cluster ───────────────┐    ┌─ Isolated cluster ────┐
   │                                   │    │                       │
  GW(42) ─► R1 ─► R2 ─► R3             │    │   R7(41)  R8(41) L42(41)│
   │  push   push  push  push          │    │     ▲       ▲      ▲    │
   │  to     to    to    to            │    │     │ pull  │ pull │    │
   │  behind behind...                 │    │     └───────┴──────┘    │
   │  peers  ▼     ▼     ▼              │    │   mule(42) pushes when  │
   │       L1(42) L4(42) L9(42)        │    │   it sweeps through     │
   │       (each pulls on wake)        │    │                         │
   └───────────────────────────────────┘    └───────────────────────┘
              │                                        ▲
              │   mule hears GW cluster at 42,         │
              │   carries config bundle (42) onward ───┘
              │   — same local rule, mobile node

   Every arrow is the SAME local rule: "peer behind me → push;
   peer ahead of me → pull." Convergence is emergent. No roster,
   no campaign manager, no completion checkpoint.
```

#### 9.9.7 Optional Visibility

Telemetry is still available, but as *optional monitoring*, not a required
mechanism. A gateway may observe `config_version`/`config_hash` in the HELLOs it hears (and in
mule-carried telemetry from isolated clusters) to see how far convergence has
spread. This is purely for operator insight — convergence happens whether or
not anyone watches, because it is driven by the local rule at every node, not
by a central tracker acting on telemetry.

#### 9.9.8 Relationship to OTA Delivery

This version-comparison gossip is the same "who needs what I have" mechanism
that drives OTA delivery (Section 14.7a.2): a mule delivers firmware when
`target_version > node.sw_version`. Config and OTA share the pattern — a node
compares a version it advertises (`config_version` / `sw_version`) against a peer's
and acts on the gap. They differ only in how the payload is moved:

```
            "who needs it?" decision     payload movement
  ────────────────────────────────────────────────────────────────
  Config    config_version comparison    push the ≤256 B bundle inline
            (+ config_hash integrity)
  OTA       sw_version comparison         stream the 1–2 MB image (Section 14)

Same convergence logic; config just doesn't need the streaming machinery
because its payload fits in a single bundle.
```

---

## 10. Security

### 10.0 How End-to-End Encryption Works

Rimba uses **two independent encryption layers** that protect different things
and operate at different scopes. Understanding both is essential to understanding
the security model.

```
                             ← AES-128-CCM (hop-by-hop) →
                                                  ┌─────────────────────────────┐
Leaf ──────────── Relay 1 ──────────── Relay 2 ──────────── Gateway             │
                                                  │ ← BPSec ChaCha20 (E2E) →   │
                  └────────────────────────────────────────────────────────┘    │
                                                                                │
Layer 1: Link encryption    Protects each individual hop (peer link)            │
Layer 2: Bundle encryption  Protects the payload from leaf to gateway           │
```

---

#### Layer 1 — Hop-by-Hop Link Encryption (AES-128-CCM)

Every frame sent between two peer-linked nodes is encrypted using a **unique
session key** known only to those two nodes. This key is established during
the PEER_OPEN/CONFIRM handshake (Section 7.4) via X25519 Elliptic Curve
Diffie-Hellman.

```
How a frame moves between Relay 1 and Relay 2:

  Relay 1 sends:
    encrypt(frame, key=session_key_R1_R2, nonce=...) → ciphertext
    transmit ciphertext over 802.11ah radio

  Relay 2 receives:
    decrypt(ciphertext, key=session_key_R1_R2, nonce=...) → frame
    read mesh header: who sent this? where does it go next?
    discard link-layer decryption result
    encrypt(frame, key=session_key_R2_R3, nonce=...) → new ciphertext
    transmit to Relay 3
```

Each relay decrypts the incoming frame to read the **mesh header** (routing
info), then **re-encrypts** it with the next relay's key before forwarding.

```
What the link layer protects:
  ✅ Mesh header (src, dst, next_hop, TTL, flags)
  ✅ Bundle content (outer wrapper)
  ✅ Routing metadata — an RF observer cannot see where frames are going

What the link layer does NOT guarantee:
  ✗ A relay that has decrypted the frame can inspect the mesh header
    (this is intentional — relays need the header for routing)
  ✗ Does not protect against a compromised relay reading header fields
```

Key properties of the link layer:
- Algorithm: AES-128-CCM (authenticated encryption — detects tampering)
- Key: unique per pair, derived per session, stored in RAM only
- Nonce: `node_id (6B) || seq (2B) || timestamp (4B) || counter (1B)` — never reused
- Replay protection: frame rejected if counter ≤ last accepted counter from that peer

---

#### Layer 2 — End-to-End Bundle Encryption (BPSec BCB, ChaCha20-Poly1305)

The sensor payload inside the bundle is encrypted by the **originating leaf**
and decrypted only by the **destination**. Every relay in between carries an
encrypted blob it cannot read. The encryption is performed by the BPSec BCB
(Block Confidentiality Block, RFC 9172); ChaCha20-Poly1305 is the BCB's cipher.
The bundle key is derived either from `root_secret` (soft E2E) or via ECIES
(true E2E, Section 10.5) — in both cases the BCB does the encryption.

```
Bundle journey from leaf to gateway:

Leaf:
  sensor_data = {temp: 24.3, humidity: 67}
  bundle_key  = HKDF(root_secret, src_id, dst_id)
  ciphertext  = ChaCha20-Poly1305.encrypt(sensor_data, key=bundle_key)
  bundle      = [primary_block | BCB | ciphertext_block]
  ──────────────────────────────────────────────►
                              │
Relay 1 (sees):               │
  primary_block (src EID, dst EID, timestamps)  ← plaintext, needed for routing
  BCB (encryption metadata)                     ← plaintext, needed for security processing
  ciphertext_block                              ← ENCRYPTED, cannot read
  Relay forwards bundle as-is
                              │
Relay 2 (sees same thing):    │
  Same — still cannot read payload
                              │
                              ▼
Gateway:
  bundle_key  = HKDF(root_secret, src_id, dst_id)
  sensor_data = ChaCha20-Poly1305.decrypt(ciphertext, key=bundle_key)
  upload to cloud
```

**Key derivation — how the leaf and gateway agree on the key without transmitting it:**

```
bundle_key = HKDF-SHA256(
  IKM  = deployment_root_secret    ← 32 bytes, provisioned to all nodes
  salt = src_node_id || dst_node_id ← 12 bytes, from bundle EIDs
  info = "Rimba-v1-bundle"          ← context string
)

deployment_root_secret is never transmitted over the air.
Each src-dst pair produces a different bundle_key.
Leaf L1→GW and Leaf L2→GW have different keys.
The gateway derives the same key on receipt (knows root_secret + EIDs).
```

**BPSec block structure inside the bundle:**

```
BPv7 bundle (with BPSec):

  ┌────────────────────────────────────────────┐
  │ Primary Block (plaintext)                  │
  │   src EID: ipn:<leaf_id>.1                 │ ← routing, visible to all
  │   dst EID: ipn:1.1 (gateway)              │
  │   creation timestamp, lifetime, seq#       │
  ├────────────────────────────────────────────┤
  │ BIB — Block Integrity Block (plaintext)    │
  │   HMAC-SHA256 over primary block           │ ← any relay can verify
  │   (proves primary block not tampered)      │   integrity without decrypting
  ├────────────────────────────────────────────┤
  │ BCB — Block Confidentiality Block          │
  │   Security context: ChaCha20-Poly1305      │ ← encryption metadata
  │   Security target: payload block           │
  │   Nonce: timestamp(8B) || seq(4B)          │
  │   Authentication tag: 16 bytes             │
  ├────────────────────────────────────────────┤
  │ Payload Block (ciphertext)                 │
  │   CBOR(sensor_data) encrypted by BCB       │ ← only gateway can read
  └────────────────────────────────────────────┘
```

The BIB and BCB are BPSec standard blocks defined in RFC 9172. Any relay can
verify the BIB (proves the bundle header hasn't been modified in transit)
without being able to decrypt the payload (protected by BCB).

---

#### What each party can see

```
Party              Can see                         Cannot see
──────────────────────────────────────────────────────────────────────
RF observer        802.11ah ciphertext only        Everything (link encrypted)
  (passive         Cannot even read 802.11ah
   sniffer)        MAC addresses*

Relay (honest)     Mesh header (src, dst, next_hop) Sensor payload (BCB-encrypted)
                   BPv7 primary block               Bundle content
                   (src EID, dst EID, timestamps)

Relay              Same as above                   Sensor payload (still BCB)
  (compromised,                                    — UNLESS they extract
   keys only)                                        root_secret from NVS

Gateway            Everything (full decryption)    —

* In 802.11ah IBSS, the MAC header IS inside the AES-128-CCM ciphertext,
  so even MAC addresses are encrypted from an RF observer's perspective.
  A relay in range receives the frame and decrypts it — but this requires
  having the peer link session key, which only legitimate peers hold.
```

---

#### Leaf-to-Leaf Encryption

Leaf-to-leaf bundles use the **same BPSec mechanism** as leaf-to-gateway, but
with a different `dst_node_id` in the HKDF key derivation. No new protocol is
needed — the mechanism generalises automatically.

**Key derivation for leaf-to-leaf:**

```
Leaf A sends to Leaf B:

  bundle_key = HKDF-SHA256(
    IKM  = deployment_root_secret,        ← same on all nodes
    salt = leaf_A_node_id || leaf_B_node_id,  ← src and dst IDs
    info = "Rimba-v1-bundle"
  )

Leaf B decrypts on receipt:

  src_node_id ← read from BPv7 primary block (src EID)
  dst_node_id ← own node ID (known locally)

  bundle_key = HKDF-SHA256(
    IKM  = deployment_root_secret,        ← B also has root_secret
    salt = leaf_A_node_id || leaf_B_node_id,  ← from primary block
    info = "Rimba-v1-bundle"
  )
  → Same key → decrypts successfully
```

The key is **directional** — `salt = A || B` produces a different key from
`salt = B || A`. A bundle from Leaf A to Leaf B cannot be replayed as a bundle
from Leaf B to Leaf A.

**The full path with encryption layers shown:**

```
Leaf A                  Relay R_A           Relay R_B           Leaf B
  │                        │                   │                   │
  │ Create bundle:         │                   │                   │
  │  src EID = A           │                   │                   │
  │  dst EID = B           │                   │                   │
  │  payload = BPSec(data) │                   │                   │
  │────── AES-CCM ────────►│                   │                   │
  │       (A↔R_A key)      │                   │                   │
  │                        │──── AES-CCM ─────►│                   │
  │                        │    (R_A↔R_B key)  │                   │
  │                        │                   │──── AES-CCM ─────►│
  │                        │                   │    (R_B↔B key)    │
  │                        │                   │                   │
  │                        │                   │     Leaf B:       │
  │                        │                   │  bundle_key =     │
  │                        │                   │  HKDF(root,A,B)   │
  │                        │                   │  decrypt payload  │

AES-CCM changes at every hop (link keys).
BPSec payload block is unchanged from A to B (E2E key).
```

**What relays can see on a leaf-to-leaf bundle:**

```
Relay sees (plaintext):                 Relay cannot see:
  src EID: ipn:<leaf_A_id>.1            Payload content (BCB-encrypted)
  dst EID: ipn:<leaf_B_id>.1            What Leaf A is saying to Leaf B
  creation timestamp
  bundle lifetime
  BCB block presence (encryption used)

Privacy note: unlike leaf→gateway bundles where dst is always
the gateway (public knowledge), leaf-to-leaf bundles reveal
which specific leaf is the recipient. Relays can infer:
  "Leaf A is communicating with Leaf B"
but NOT what they are communicating.

For deployments where leaf communication relationships are
sensitive, consider routing through the gateway instead
(Leaf A → GW → Leaf B), which hides the direct relationship.
```

**Key discovery before encryption (Section 7.5):**

Before Leaf A can encrypt to Leaf B, it needs B's public key. The full discovery flow depends on what is already cached:

```
Fast path (key in leaf local directory or relay cache):
  A checks key_dir[B] → found → encrypt immediately
  OR A sends KEY_REQ to R_A → R_A returns B's key → encrypt
  Total: within single 230ms wake window

Slow path (key unknown, RREQ needed, > 230ms):
  Wake 1: A sends KEY_REQ → R_A issues RREQ → timeout
          A stores payload in NVS with PENDING_KEY status
  Wake 2: R_A has key from RREP → A gets key → encrypts → sends
  Cost: one extra wake cycle (15 min delay, one-time per new contact)
```

See Section 7.5.4 for all five key discovery cases including isolated
nodes and mule-assisted key delivery.

**Routing path (via proxy RREP, Section 8.3):**

```
① Leaf A creates bundle (dst = leaf_B EID), sends to Relay R_A
② R_A has no route to Leaf B → issues RREQ for leaf_B_node_id
③ Relay R_B (Leaf B's relay) responds with proxy RREP (carries B's pub_key)
④ R_A caches B's pub_key, returns it to Leaf A via KEY_RESP
⑤ Bundle routes: R_A → mesh → R_B
⑥ R_B delivers to Leaf B on next wake (Scheduled) or immediately (Continuous)
```

Leaf A does not need to know which relay Leaf B is on.
RREQ finds the right relay and delivers B's public key in the same exchange.
The payload remains encrypted throughout — R_B delivers without reading it.

**The same limitations apply as for leaf-to-gateway:**

```
Shared root_secret limitation:
  Any relay with root_secret can compute bundle_key(A→B)
  and decrypt or forge leaf-to-leaf bundles.

True leaf-to-leaf E2E (not currently implemented):
  Leaf A has Leaf B's public key
  Leaf A: ECIES.encrypt(data, key=leaf_B_public_key)
  Leaf B: ECIES.decrypt(ciphertext, key=leaf_B_private_key)
  No other node — not even a relay with root_secret — can read it.

For now: leaf-to-leaf encryption is as strong as leaf-to-gateway.
Both are robust against external attackers. Both share the same
risk from physically compromised nodes with root_secret access.
```

---

#### The shared root_secret limitation

The current design derives the bundle key from a shared `deployment_root_secret`
that every node holds. This provides strong protection against **external**
attackers (no key, no decryption). But it means a **physically compromised relay**
that extracts its NVS can derive bundle keys for any leaf:

```
Attacker captures Relay R, extracts deployment_root_secret from NVS:
  → Can compute bundle_key for any leaf in this deployment
  → Can decrypt all past and future bundles if they capture them
  → Can forge bundles that appear to come from any leaf

Attacker WITHOUT the root_secret (external RF observer):
  → Cannot decrypt anything
  → Cannot forge any bundle that passes BPSec verification
```

True E2E security — even against a compromised relay — would require
**per-leaf asymmetric encryption**:

```
True E2E (not currently implemented):
  Each leaf has a unique private key (leaf_private_key)
  Gateway has a known public key (gateway_public_key)
  Leaf: ECIES.encrypt(sensor_data, key=gateway_public_key)
  Gateway: ECIES.decrypt(ciphertext, key=gateway_private_key)

  A compromised relay cannot decrypt because:
    → It doesn't have gateway_private_key
    → leaf_private_key is not derivable from a shared secret
```

This is a known limitation documented in the hardening plan (Tier 1 — Security
Failure Paths). The current model assumes relays are **honest but potentially
observable**: a relay can forward a bundle without reading it, and physical
security of relay hardware is assumed. For deployments where physical relay
compromise is a realistic threat, the asymmetric key model above should be
implemented before production deployment.

---

#### Summary

```
Protection           Mechanism              Scope         Defeats
──────────────────────────────────────────────────────────────────────────
RF metadata hiding   AES-128-CCM            Per link      Passive sniffer,
                     (link layer)           (hop-by-hop)  traffic analysis

Payload              BPSec BCB              Source to     Relay reading
  confidentiality    ChaCha20-Poly1305      gateway       payload, mule
                     (bundle layer)         (E2E)         reading payload

Payload integrity    BPSec BIB              Source to     Bundle tampering
                     HMAC-SHA256            gateway       in transit
                     (bundle layer)         (E2E)

Replay prevention    AES-128-CCM nonce      Per link      Frame replay
  (link layer)       + strict counter       (hop-by-hop)  attacks

Replay prevention    BPv7 sequence number   End-to-end    Bundle replay
  (bundle layer)     + timestamp nonce      (E2E)         attacks
```



### 10.1 Threat Model

Rimba protects against passive eavesdropping, active frame tampering, replay attacks, rogue node injection, and mule data theft.

**Decryption capability by node tier** (with cloud-endpoint encryption,
`ipn:2.*`, Section 8.11):

```
Tier            Nodes              Can decrypt payload?
──────────────────────────────────────────────────────────────────────
Field forwarders relays, mules     NO — never hold any payload key
Gateway         gateway(s)         NO for ipn:2.* (no cloud_priv)
                                   YES for ipn:1.* (holds gateway_priv)
Cloud backend   cloud / HSM        YES — holds cloud_priv

Key consequence: for ipn:2.* traffic, NO physically-deployed node
(relay, mule, or gateway) can read payloads. The only decryption
point is the cloud backend, which is the most defensible location.
```

This tiering is the core of the security model: the assets most exposed to
physical compromise (field relays and mules, and to a lesser degree gateways)
are precisely the nodes given no ability to read payloads. A compromised relay,
mule, or gateway can forward, delay, or drop a cloud-targeted bundle, but never
read it.

**Backend protection is unconditional.** Both deployment encryption profiles
(HYBRID and FULL_TRUE — Section 10.5) use true E2E for backend-bound traffic.
There is no profile in which a compromised relay can read gateway- or
cloud-destined data. The profile choice affects only node-to-node (leaf-to-leaf)
traffic: HYBRID leaves that on soft encryption (readable by a compromised relay,
the rare path), FULL_TRUE extends true E2E to it as well. The guarantee
"no field node reads backend data" holds in every configuration.

### 10.2 Hop-by-Hop Security

All frames between peer-linked nodes are encrypted and authenticated using the session key from peer link establishment (Section 7.4). This applies to all links in the mesh — relay-to-relay, relay-to-leaf, and leaf-to-relay.

- **Algorithm**: AES-128-CCM
- **Key**: Per-pair session key (X25519 ECDH + HKDF)
- **Nonce**: `node_id (6) || seq (2) || timestamp (4) || counter (1)`
- **AAD**: src MAC || dst MAC || EtherType || frame type
- **Tag**: 8 bytes

Replay protection: a node rejects any frame whose nonce counter is not strictly greater than the last accepted value from that peer.

### 10.3 End-to-End Security (Bundle Layer)

Bundle payloads are protected source-to-destination using BPSec (RFC 9172):

- **Confidentiality**: COSE_Encrypt0 with ChaCha20-Poly1305
- **Key derivation**:
  ```
  bundle_key = HKDF-SHA256(
      ikm  = deployment_root_secret,
      salt = src_node_id || dst_node_id,
      info = "Rimba-v1-bundle"
  )
  ```
- **Nonce**: bundle creation timestamp (8B) || sequence number (4B)

A mule carries bundles but cannot read their contents without the bundle key.

### 10.4 Key Management

| Key | Storage | Notes |
|---|---|---|
| Deployment root secret | Protected flash (ESP32 eFuse NVS / nRF54 KMU) | Never transmitted |
| Network PSK | Protected flash | Used for peer link MIC |
| Session keys | RAM only | Renegotiated per peer link or on expiry |

Key rotation is out of scope for v1. See Open Issue #7.

### 10.5 True End-to-End Encryption (Phase 2)

The current design uses a shared `deployment_root_secret` for bundle key
derivation. Any node holding root_secret can decrypt any bundle. Section 10.0
describes this as "soft E2E." This section defines the upgrade path to
mathematically enforced E2E using per-node asymmetric keys.

> **ECIES does not replace BPSec — it operates within it.** BPSec (RFC 9172)
> is the framework: it defines the BIB (integrity) and BCB (confidentiality)
> blocks and how a receiver processes them. ECIES is the key-establishment
> method plugged into the BCB as a custom security context. The BCB performs
> the actual encryption — ChaCha20-Poly1305 — in **both** soft and true E2E
> modes; ECIES changes only *how the BCB's content-encryption key is derived*.
> The `e2e_mode` value is a BCB security-context parameter, carried inside the
> BPSec block. A Rimba bundle is therefore always a standards-compliant BPSec
> bundle regardless of mode — which is what keeps it interoperable with
> standard DTN endpoints (Section 9.0).

```
Where ECIES fits inside the BPSec block structure:

  ┌────────────────────────────────────┐
  │ Primary Block                      │ ← BPv7 (RFC 9171)
  ├────────────────────────────────────┤
  │ BIB — Block Integrity Block        │ ← BPSec (RFC 9172)
  ├────────────────────────────────────┤
  │ BCB — Block Confidentiality Block  │ ← BPSec (RFC 9172)
  │   security context: rimba-ecies    │ ← ECIES plugs in HERE
  │   params: e2e_mode, e_pub, nonce   │    (key establishment only)
  │   cipher: ChaCha20-Poly1305        │ ← the BCB does the encryption
  │   target: payload block            │
  ├────────────────────────────────────┤
  │ Payload Block (ciphertext)         │ ← encrypted by the BCB
  └────────────────────────────────────┘

  soft E2E  → bundle_key = HKDF(root_secret, src||dst)
  true E2E  → bundle_key = HKDF(ECDH(e_priv, recipient_pub), src||dst)
              ↑ ECIES changes only this derivation; the BCB is unchanged
```

#### Overview

The fundamental difference between soft and true E2E:

```
SOFT E2E (current — shared root_secret):

  Leaf A                 Relay                  Gateway
    │                      │                       │
    │ bundle_key =         │ ALSO has root_secret  │
    │ HKDF(root_secret,   │ ALSO can compute      │
    │      A_id, B_id)    │ bundle_key            │
    │                      │        ↓              │
    │──── encrypted ──────►│  COULD decrypt ✗      │
    │                      │──── encrypted ────────►│ decrypts ✓
    │                      │                       │

TRUE E2E (this section — asymmetric ECIES):

  Leaf A                 Relay                  Gateway
    │                      │                       │
    │ shared =             │ has only e_pub        │ has gateway_x_priv
    │ X25519(e_priv,       │ (visible in BCB)      │
    │   gateway_x_pub)    │        ↓              │
    │ bundle_key =         │ X25519(relay_x_priv,  │ X25519(gateway_x_priv,
    │ HKDF(shared,...)    │   e_pub) ≠ shared ✗   │   e_pub) = shared ✓
    │                      │ CANNOT decrypt ✅      │ decrypts ✓
    │──── encrypted ──────►│──── encrypted ────────►│
```

**Why the relay cannot decrypt — the ECDH property:**

```
ECDH (Diffie-Hellman on Curve25519) has one key property:
  X25519(A_priv, B_pub) = X25519(B_priv, A_pub) = shared_secret

Applied to ECIES:
  Sender:     X25519(e_priv,       gateway_x_pub) = shared ✓
  Gateway:    X25519(gateway_x_priv, e_pub)       = shared ✓  ← same!
  Relay:      X25519(relay_x_priv,   e_pub)       = DIFFERENT ✗

The relay's private key produces a different ECDH output.
It cannot derive the correct bundle_key.
It cannot decrypt. This is mathematically enforced, not by convention.
```

```
Current (soft E2E):
  bundle_key = HKDF(root_secret, src || dst)
  Any node with root_secret → can derive bundle_key → can decrypt

True E2E (this section):
  bundle_key = HKDF(ECDH(sender_ephemeral_priv, recipient_pub), src || dst)
  Only recipient_priv can produce the correct ECDH output → only recipient decrypts
  Intermediate relay with root_secret → CANNOT decrypt
```

#### Node Key Pairs

Every node is provisioned at staging with two key pairs and a deployment-CA
certificate (Section 3.6.5 — mandatory for all nodes). They are present in NVS
from first boot:

```
X25519 encryption key pair:
  x_priv   <bytes[32]>  ← NVS "node_x_priv" (AES-256 encrypted)
  x_pub    <bytes[32]>  ← NVS "node_x_pub"  (plaintext)

Ed25519 signing key pair:
  sign_priv <bytes[32]> ← NVS "node_sign_priv" (AES-256 encrypted)
  sign_pub  <bytes[32]> ← NVS "node_sign_pub"  (plaintext)

Deployment-CA certificate:
  node_cert <bytes[~138]> ← NVS "node_cert" (plaintext)
    binds node_id || x_pub || sign_pub || expires_at, signed by the
    deployment CA (Section 7.5.5). Proves these keys belong to this node.
```

Provisioning the keys (rather than generating them at first boot) means every
node is FULL_TRUE-capable immediately, with a CA-signed certificate, without
ever contacting a gateway — which is what makes profile migration a config flip
(Section 10.5, migration). Private keys never leave the device. Public keys and
the certificate are advertised via HELLO (Section 7.1) and distributed by relay
key caches.

#### Bundle Encryption (ECIES)

```
Sender encrypts to recipient:

  1. Generate ephemeral X25519 key pair per bundle:
       e_priv, e_pub = X25519_keygen()

  2. ECDH with recipient's long-term public key:
       shared = X25519(e_priv, recipient_x_pub)

  3. Derive bundle key:
       bundle_key = HKDF-SHA256(shared, src_id || dst_id || "rimba-v1-e2e")

  4. Encrypt:
       ciphertext, tag = ChaCha20-Poly1305(payload, key=bundle_key, nonce=timestamp||seq)

  5. Add to BCB:
       e_pub      (32 bytes — so recipient can do ECDH)
       ciphertext
       tag        (16 bytes)

  6. Discard e_priv immediately.

Recipient decrypts:

  7. Read e_pub from BCB.
  8. ECDH with own private key:
       shared = X25519(recipient_x_priv, e_pub)
       → same shared as step 2 (ECDH property)

  9. Derive same bundle key:
       bundle_key = HKDF-SHA256(shared, src_id || dst_id || "rimba-v1-e2e")

  10. Decrypt and verify tag.
```

**ECIES flow diagram:**

```
┌─────────────────────────────────────────────────────────────────────┐
│                        SENDER (Leaf A)                              │
│                                                                     │
│  ① X25519_keygen() ──► e_priv, e_pub   (ephemeral, per bundle)    │
│                              │                                      │
│  ② X25519(e_priv, B_x_pub) ─► shared   (32 bytes of randomness)   │
│                              │                                      │
│  ③ HKDF(shared, src||dst) ──► bundle_key                          │
│                              │                                      │
│  ④ ChaCha20(payload, bundle_key) ──► ciphertext + tag              │
│                                                                     │
│  ⑤ DISCARD e_priv  (forward secrecy — past bundles safe)          │
│                                                                     │
│  BCB carries: ┌──────────┬────────────────┬──────┐                │
│               │  e_pub   │   ciphertext   │  tag │                │
│               │  32 B    │   var length   │ 16 B │                │
│               └──────────┴────────────────┴──────┘                │
└────────────────────────────┬────────────────────────────────────────┘
                             │ bundle travels through mesh
                             ▼
┌─────────────────────────────────────────────────────────────────────┐
│                  RELAY (honest or compromised)                      │
│                                                                     │
│  Relay reads from bundle (plaintext):                              │
│    src EID, dst EID, lifetime, flags ← for routing                │
│    e_pub ← visible in BCB                                          │
│    ciphertext ← cannot read                                        │
│                                                                     │
│  Could relay try: X25519(relay_x_priv, e_pub) → ???               │
│  Result ≠ shared  (different private key → different output)       │
│  bundle_key = HKDF(???, ...) ≠ actual bundle_key                  │
│  Decryption fails. Tag verification fails. ✅                      │
└────────────────────────────┬────────────────────────────────────────┘
                             │ bundle continues
                             ▼
┌─────────────────────────────────────────────────────────────────────┐
│                      RECIPIENT (Leaf B / Gateway)                   │
│                                                                     │
│  ⑥ Read e_pub from BCB                                            │
│                                                                     │
│  ⑦ X25519(B_x_priv, e_pub) ──► shared  (SAME shared as ②)       │
│                                          ECDH property guarantees  │
│                                          this is identical         │
│  ⑧ HKDF(shared, src||dst) ──► bundle_key                         │
│                                                                     │
│  ⑨ ChaCha20.decrypt(ciphertext, bundle_key) + verify tag          │
│     ──► original payload ✓                                         │
└─────────────────────────────────────────────────────────────────────┘
```

#### Deployment Encryption Profiles

A deployment's encryption posture is a single provisioned setting,
`e2e_profile`, rather than a scatter of per-traffic flags. There are two
profiles. Pure soft encryption (root_secret for all traffic) is **not** an
available profile — every deployment protects its backend-bound data against
relay compromise.

```
Profile          Gateway/cloud traffic   Node-to-node traffic   KEY_REQ?
──────────────────────────────────────────────────────────────────────────
HYBRID (default) true E2E (ECIES)         soft (root_secret)     Never
FULL_TRUE        true E2E (ECIES)         true E2E (ECIES)       Yes (node-to-node)
```

```
e2e_profile provisioning value:
  0 = HYBRID     (default — strong where it is free)
  1 = FULL_TRUE  (opt-in — node-to-node relay-compromise resistance)
```

**HYBRID (default).** Backend-bound traffic (`ipn:1.*` gateway, `ipn:2.*`
cloud) uses ECIES to the provisioned gateway/cloud key — true end-to-end, no
key discovery, +32 B/bundle. Node-to-node traffic (leaf-to-leaf, the rare path)
uses the soft `root_secret` HKDF method — no KEY_REQ, lower overhead, but
readable by a compromised relay. This is the recommended default: the dominant
traffic is fully protected at zero discovery cost, and the only soft path is
the uncommon one.

**FULL_TRUE (opt-in).** Everything uses ECIES, including node-to-node. This
requires per-node keypairs, deployment-CA certificates, and working key
discovery (Section 7.5). Leaf-to-leaf first contact pays a KEY_REQ (cached for
`KEY_CACHE_LIFETIME_S`). Use when relays are physically exposed AND leaf-to-leaf
privacy matters enough to accept the key-discovery machinery.

**The backend invariant (both profiles):**

```
Backend-bound traffic (ipn:1.* and ipn:2.*) is ALWAYS true E2E (ECIES).
There is no profile in which a compromised relay can read data destined
for the gateway or cloud.

Consequence — a hard rule:
  A bundle addressed to ipn:1.* or ipn:2.* with e2e_mode=0 (soft) is
  INVALID. The receiving gateway MUST reject it. This closes the
  downgrade attack where an adversary forces backend data to soft mode.

Soft encryption (root_secret HKDF) is NOT removed from the system — it
remains the node-to-node method in HYBRID, and root_secret retains its
other roles (link-layer AES-CCM MAC, provisioning authentication, CA
key derivation). Soft is simply never applied to backend traffic.
```

**Profile is deployment-wide and consistent.** All nodes in a deployment run
the same `e2e_profile`. A sender's encryption must match what the receiver can
decrypt: if Leaf A were FULL_TRUE (encrypts leaf-to-leaf with ECIES) but Leaf B
were HYBRID (expects soft leaf-to-leaf), they could not communicate. The
per-bundle `e2e_mode` flag is the wire-level mechanism; the deployment profile
is the policy that drives it.

**Migration between profiles:**

Because keypairs and the deployment-CA certificate are **mandatory at
provisioning** (Section 3.6.5), every node is FULL_TRUE-capable from first boot.
Migration is therefore a policy flip, not a key-distribution campaign:

```
HYBRID → FULL_TRUE  (upgrade):
  Keys + certs are ALREADY in NVS on every node (provisioned at staging).
  The only prerequisite is that key discovery is warm — nodes advertising
  node_pub_key + cert in HELLO, relay caches populated (Section 7.5).

  1. Ensure nodes are advertising keys in HELLO (see "warm vs cold" below)
  2. Verify discovery coverage — campaign telemetry confirms every node's
     key + cert is published and resolvable across clusters
  3. Flip e2e_profile = FULL_TRUE (config bundle) → leaf-to-leaf uses ECIES

  No keypair generation, no certificate issuance, no OTA key delivery —
  the capability was established once at staging.

FULL_TRUE → HYBRID  (downgrade, trivial + instant):
  Flip e2e_profile = HYBRID. Leaf-to-leaf reverts to soft (root_secret,
  always present). Per-node keys stay in NVS, still used for backend
  traffic. Reduces security for leaf-to-leaf — deliberate, logged only.

Repeated toggling: cheap in BOTH directions. Capability is permanent
  (in NVS from staging); the profile is just a policy value. A deployment
  can downgrade during a key-management incident and re-upgrade later
  with no re-provisioning.
```

**Warm vs cold key advertising (`hello_key_adv`, defined in Section 7.1):**

```
Whether nodes advertise node_pub_key in every HELLO is the hello_key_adv
config. It governs how fast a HYBRID → FULL_TRUE flip is safe:

  WARM:  node_pub_key in every HELLO, cert periodically. Caches stay
    populated. HYBRID → FULL_TRUE is an INSTANT, safe flip — discovery
    already works. Cost: +32 B per HELLO. Default for FULL_TRUE.

  COLD:  node_pub_key advertised only every Nth HELLO (or on KEY_REQ).
    Saves HELLO bandwidth. On upgrade, nodes warm caches over a settling
    period before the flip is safe. Default for HYBRID.

(Keys and certs are ALWAYS in NVS regardless — hello_key_adv only
controls advertising frequency, not whether the node has the keys.)

Recommended: WARM for deployments that may toggle profiles; COLD for
deployments committed to HYBRID that want minimal HELLO overhead.
```

**The isolated-cluster limitation (unchanged by pre-provisioning):**

```
A node in a mule-only cluster is FULL_TRUE-CAPABLE immediately (it has
its keys + cert from staging). But for a DISTANT node to send leaf-to-leaf
to it, the distant node must LEARN the isolated node's public key — and
that key only crosses the mule boundary intermittently.

So: pre-provisioning makes each node ready, but cross-cluster leaf-to-leaf
key DISCOVERY in mule-dependent topologies remains mule-gated. For such
deployments, HYBRID is often the realistic ceiling for cross-cluster
leaf-to-leaf. Backend traffic (always true E2E) is unaffected — the cloud
key is provisioned everywhere.
```

```
Universal safe-ordering rule for any profile change:
  NEVER flip senders to a mode the receivers cannot yet decrypt.
  Receive-capability (firmware that can decrypt the new mode) must be
  deployed fleet-wide BEFORE the profile flip changes send behaviour.
  The per-bundle e2e_mode flag lets old and new coexist during rollout.
```

#### Scope of the Profile — What Counts as "Node-to-Node"

The profile governs exactly one category of traffic: payloads where a field
node is the original SOURCE and another field node is the final DESTINATION.
It does NOT govern forwarding, and it does NOT govern anything to or from the
backend. Getting this scope right matters, because most relay and leaf traffic
is forwarding, which the profile never touches.

```
Definition:
  NODE-TO-NODE  = both endpoints are field nodes (leaf, relay, or mule),
                  neither is the backend. The payload's source and final
                  destination are both in the field.
                  → governed by the profile (HYBRID=soft, FULL_TRUE=ECIES)

  BACKEND       = source or destination is the gateway (ipn:1.*) or cloud
                  (ipn:2.*). → ALWAYS true E2E, both profiles.

  FORWARDING    = a node passing a bundle THROUGH toward an endpoint it is
                  not. The node is a waypoint, not an endpoint.
                  → hop-by-hop AES-128-CCM only, profile-independent.
                    The forwarder cannot read the payload regardless.
```

Two separate encryption layers are in play, and the profile touches only one:

```
Hop-by-hop (AES-128-CCM): ALWAYS on, every link, every profile. Protects
  every frame between adjacent nodes — forwarding, HELLO, ACK, etc. The
  profile does not change this.

End-to-end (BPSec): the payload, source → final destination. THIS is what
  the profile governs, and only for node-to-node endpoints (backend is
  always true regardless).
```

Per node-pair, whether the profile applies depends on the ROLE in that
exchange (endpoint vs forwarder), not the node type:

```
Exchange                              Profile applies?   Encryption used
──────────────────────────────────────────────────────────────────────────
Leaf → gateway/cloud (through relays) No (backend)       Always true E2E
Relay forwarding any transit bundle   No (forwarding)    Hop-by-hop AES-CCM
Leaf → Leaf (both endpoints)          YES                HYBRID soft / FULL true
Leaf → Relay (relay is destination)   YES                HYBRID soft / FULL true
Relay → Leaf (relay is source)        YES                HYBRID soft / FULL true
Relay → Relay (both endpoints)        YES                HYBRID soft / FULL true
Relay ↔ Mule (custody endpoints)      YES                HYBRID soft / FULL true
Cloud → Leaf (downlink command)       No (backend)       Always true E2E
```

```
The common cases, stated plainly:
  - A leaf reporting to the backend → backend, always true E2E. Profile
    irrelevant. The relays it passes through only forward (hop-by-hop).
  - A relay forwarding that bundle → forwarding, profile irrelevant; the
    relay never reads the payload.
  - Field nodes talking to EACH OTHER as endpoints (leaf↔leaf, leaf↔relay,
    relay↔relay) → THIS is where the profile decides soft vs true.

"Node-to-node" therefore means all field-node-to-field-node endpoint
payloads — not just leaf-to-leaf. The migration behaviour, key-discovery
needs, and KEY_REQ cost of FULL_TRUE apply to every node-to-node pair, but
never to forwarding or backend traffic.
```

#### Encryption Endpoints (Gateway vs Cloud)


The destination EID a sender chooses determines both where the bundle is
routed AND which public key it is encrypted to. There are two uplink
endpoints (Section 8.11):

```
Destination EID    Encrypt to        Decrypted by      Use when
─────────────────────────────────────────────────────────────────────────
ipn:1.<service>    gateway_pub_key   the gateway       Gateway must read the
  (GATEWAY)                                            payload — local alerting,
                                                       aggregation, or operating
                                                       during internet outage.

ipn:2.<service>    cloud_pub_key     the cloud only    True end-to-end to the
  (CLOUD)                                              backend. No field node
                                                       (relay, mule, OR gateway)
                                                       can read the payload.
```

Both keys are distributed at provisioning, so neither endpoint requires key
discovery — a leaf can encrypt to either from first boot:

```
Provisioning payload carries both (Section 3.6.5):
  "gateway_pub_key": <bytes[32]>   endpoint for ipn:1.*
  "cloud_pub_key":   <bytes[32]>   endpoint for ipn:2.*

Sender logic (decided locally, never negotiated over the air):
  if dst == ipn:1.*  →  ECIES(payload, gateway_pub_key)
  if dst == ipn:2.*  →  ECIES(payload, cloud_pub_key)
```

**Why CLOUD is the stronger default:** the threat model that motivates true
E2E is the compromised field node. With `ipn:2.*`, the gateway joins relays
and mules in the "cannot decrypt" tier — the trust boundary moves to the
cloud backend, which is the most physically secure point (a datacenter / HSM,
not a field cabinet). With `ipn:1.*`, the gateway is a decryption point, which
is appropriate only when the gateway genuinely needs to act on the data.

**Gateway key model for ipn:1.\*:** because gateway anycast may deliver an
`ipn:1.*` bundle to any of several gateways, all gateways share a single
`gateway_pub_key` / `gateway_priv` so any of them can decrypt. This shared key
is acceptable specifically because it protects only against relay/mule
compromise (gateways are few and physically protected). Deployments where
gateway physical compromise is a realistic concern should prefer `ipn:2.*`
(cloud endpoint), which removes all decryption capability from the gateway
entirely and makes the shared-key question moot.

**Cloud key custody:** `cloud_priv` must live in a managed boundary — an HSM
or KMS (AWS KMS, GCP Cloud KMS, etc.) — never an ordinary cloud VM. Decryption
should occur inside the KMS boundary where the platform supports it. Otherwise
the model merely relocates the single point of trust from a field gateway to a
cloud VM, which is better but not the full benefit.

**Downlink (cloud → leaf):** symmetric. The cloud encrypts to the target
leaf's public key (node-to-node E2E, Section 7.5); the gateway forwards blind
into the mesh; only the target leaf decrypts.

#### Overhead per Bundle

```
Current BPSec BCB:
  nonce (12B) + tag (16B) = 28 bytes

True E2E BPSec BCB:
  e_pub (32B) + nonce (12B) + tag (16B) = 60 bytes

Delta: +32 bytes per bundle (ephemeral public key)

For 300-byte sensor bundle: +10.7% overhead — acceptable.
```

#### Forward Secrecy

```
Current (HKDF from root_secret):
  root_secret compromised → ALL past and future bundles decryptable
  No forward secrecy

True E2E (per-bundle ephemeral key):
  recipient_x_priv compromised:
    Future bundles: decryptable (attacker can do ECDH with future e_pub)
    Past bundles:   e_priv was discarded → CANNOT be decrypted ✅
  Forward secrecy per bundle.
```

**Forward secrecy timeline:**

```
Time ──────────────────────────────────────────────────────────────►

Bundle 1:   e_priv_1 generated → used → DISCARDED
Bundle 2:   e_priv_2 generated → used → DISCARDED
Bundle 3:   e_priv_3 generated → used → DISCARDED
                                             │
                              ◄──────────────┤ recipient_x_priv
                              Attacker        │ COMPROMISED here
                              captures it     │
                                             │
                              ▼              ▼
Can attacker decrypt Bundle 3? YES (e_pub_3 is in the bundle, e_priv_3 gone, but
  attacker can X25519(recipient_x_priv, e_pub_3) = shared_3 = correct ✗)
  Wait — no: e_priv_3 is gone. But X25519(recipient_x_priv, e_pub_3) = shared_3 ✓
  So Bundle 3 IS decryptable (it was in transit when key was compromised)

Can attacker decrypt Bundle 1 or 2? NO
  e_pub_1 and e_pub_2 are visible (in old bundle BCBs if captured)
  X25519(recipient_x_priv, e_pub_1) = shared_1 ✓... wait, YES they can.

Correction — what forward secrecy means here:
  Compromised recipient_x_priv: past AND future bundles decryptable
    (because recipient_x_priv is long-term, stays constant)

  TRUE forward secrecy per bundle requires rotating recipient_x_priv.
  OR: sender uses ONE-TIME public key (Signal protocol "pre-keys")

  In Rimba Phase 2: recipient_x_priv is long-term.
  Forward secrecy protection: rotate recipient key pair periodically.
  Key rotation interval = how far back an attacker can decrypt.

  Where Rimba IS forward secret: gateway private key.
  If a relay is compromised but gateway_x_priv is secure,
  past bundles stored at the relay (in ciphertext) cannot be decrypted
  by the relay even after it is captured — relay never had gateway_x_priv.
```

#### Implementation Phases

```
Phase 1 — Leaf → Gateway (highest priority):
  P1.1  Generate gateway key pair, add gateway_pub_key to provisioning
  P1.2  Leaf: generate ephemeral X25519 per bundle
  P1.3  ECDH + HKDF → bundle_key (replacing root_secret HKDF)
  P1.4  Update BCB encoder/decoder to carry e_pub (32 bytes)
  P1.5  Gateway: ECDH(gateway_x_priv, e_pub) → bundle_key → decrypt
  P1.6  Estimated effort: 2–3 weeks

Phase 2 — Node-to-Node (leaf-to-leaf, relay commands):
  P2.1  Keypairs + CA cert are provisioned at staging (Section 3.6.5,
        mandatory) — no first-boot generation needed
  P2.2  Add node_pub_key + node_cert to HELLO (Section 7.1)
  P2.3  Relay: build and maintain key cache from received HELLOs
  P2.4  Leaf: maintain local key directory in NVS (Section 7.5.3)
  P2.5  Implement KEY_REQ / KEY_RESP frames (Section 7.5.2)
  P2.6  Extend RREP to carry dst_pub_key (Section 8.5)
  P2.7  Sender: resolve recipient's pub key before bundle creation (Section 7.5.4)
  P2.8  Same ECIES as Phase 1 but using recipient's long-term x_pub
  P2.9  Estimated effort: 3–4 weeks
```

#### Transition from Soft E2E

During an initial rollout of ECIES firmware, both mechanisms must coexist. A
BCB parameter flag distinguishes them per bundle:

```
BCB security context parameter "e2e_mode":
  0 = root_secret HKDF (soft)
  1 = ECIES (true E2E)

During rollout (mixed old/new firmware):
  Nodes that cannot yet do ECIES: set e2e_mode = 0
  Nodes that can: prefer e2e_mode = 1
  Receivers accept both (the flag tells them which to apply)
```

**Steady-state rule once a profile is active (Section 10.5):**

```
Backend-bound traffic (ipn:1.*, ipn:2.*): e2e_mode MUST be 1.
  A backend bundle with e2e_mode=0 is INVALID and the gateway rejects
  it. This holds in BOTH profiles — backend traffic is never soft.

Node-to-node traffic:
  HYBRID:    e2e_mode = 0 (soft) — expected
  FULL_TRUE: e2e_mode = 1 (ECIES)

The e2e_mode=0 path therefore survives only for node-to-node traffic
in HYBRID deployments, and transiently during firmware rollout. It is
never valid for backend traffic in steady state.
```

---

## 11. Gateway and IPv6 Border Translation

### 11.1 Gateway Role

The gateway is a full Rimba mesh participant. It additionally bridges between the Rimba mesh and the IPv6 internet:

- Decrypts inbound bundles destined for internet EIDs.
- Re-encodes as IPv6/CoAP, MQTT, or HTTPS and forwards to cloud endpoints.
- Wraps downlink commands as bundles and injects into the mesh.
- Sends `CUSTODY_ACK` to confirm internet delivery.

### 11.2 Time Synchronisation

All Rimba nodes carry a mandatory ≤1 ppm external RTC (Section 1.4). This provides:

```
Absolute time error over time (≤1 ppm RTC):
  After 7 days:    ±0.6 seconds   ← TIMESYNC_STALE_S threshold (7 days)
  After 30 days:   ±2.6 seconds
  After 1 year:    ±31 seconds
  After 10 years:  ±310 seconds
```

With these error magnitudes, no protocol mechanism in Rimba is sensitive to time drift:

```
Consumer              Acceptable error    1 ppm reaches limit after
────────────────────────────────────────────────────────────────────
24h bundle lifetime   < 864s (1%)         27 years
7-day alert lifetime  < 6,048s (1%)       192 years
BPSec nonce           Monotonic only      Never (1ppm always ticks forward)
Bundle dedup key      Unique per bundle   Never (same leaf clock used)
```

TIME_SYNC is therefore needed only for **initial absolute time setting**, not for drift management.

```
TIME_SYNC payload (CBOR):
{
  "time":      <uint64: Unix timestamp, milliseconds>,
  "precision": <int8: log2 of precision in seconds>,
  "stratum":   <uint8: NTP stratum>,
  "src_node":  <6-byte: originating gateway Node ID>
}
```

#### When TIME_SYNC is sent

TIME_SYNC is **event-driven, not periodic**. Gateways send it only when something meaningful has changed or a node genuinely needs it:

```
Gateway sends TIME_SYNC when:
  a) A node with time_stale=true appears in neighbour table
     or in a HELLO received while processing a RREQ/RREP.

  b) Gateway establishes first contact with a new mesh segment
     (i.e. RREQ from a node with no prior route to this gateway).

  c) Gateway's own NTP/GPS time jumps by more than 1 second
     (clock correction or NTP step — re-anchor all nodes).

  d) A node explicitly requests time (no current mechanism defined
     — reserved for future TIME_REQ frame type if needed).

Gateway does NOT send TIME_SYNC:
  - On a periodic timer
  - In response to every HELLO received
  - When time_stale=false on all reachable neighbours
```

This means in a well-operating deployment, TIME_SYNC floods are extremely rare — essentially once at deployment per node, then never again unless a gateway's NTP clock steps or a node loses its RTC backup.

#### TIME_SYNC staleness

A node sets `time_stale=true` in its HELLO when:

```
now - last_timesync_received > TIMESYNC_STALE_S (7 days)
```

At 1 ppm this corresponds to ±0.6 seconds of accumulated drift — negligible for all protocol purposes. The 7-day threshold is deliberately conservative: it provides a safety margin for real-world RTC temperature variation (±3–5 ppm at extremes), gives early warning of RTC hardware failure, and keeps nodes freshly calibrated on a weekly cadence when gateway contact is available. Overhead per TIME_SYNC flood is ~0.03% of channel capacity — effectively zero.

#### Degraded operation without any TIME_SYNC

Nodes that have never received TIME_SYNC (e.g. fresh deployment not yet contacted by a gateway) operate in relative-time mode:

```
- Bundle creation_ts: monotonic counter from boot epoch (not wall clock)
- Bundle lifetime enforcement: skipped (bundles never expire in-network)
- BPSec nonce timestamp bytes: set to zero (sequence number handles uniqueness)
- time_stale flag: always true
- All other protocol functions: unaffected
```

#### Multi-gateway acceptance rule

When multiple gateways are present and each sends TIME_SYNC, every relay selects the best source:

```
Accept candidate if ANY of:
  1. No current accepted TIME_SYNC exists
  2. candidate.stratum < current.stratum
  3. Stratum equal AND candidate.precision > current.precision
  4. Stratum and precision equal AND
     candidate.src_node < current.src_node  (deterministic tiebreaker)

If accepted: update RTC, re-flood (precision - 1 per hop).
If not accepted: discard, do not re-flood.
```

Relays re-flood the accepted TIME_SYNC, decrementing `precision` by 1 per hop. Competing TIME_SYNC frames from lower-priority gateways are suppressed at each relay, preventing storms.

---

## 12. Power Management

### 12.1 No TWT in IBSS

802.11ah Target Wake Time (TWT) requires AP/STA infrastructure mode and is unavailable in IBSS. Rimba replaces it with two complementary mechanisms: advertised wake windows for leaves (Section 12.3) and local TDMA for relays (Section 12.6). Two relay sleep options are defined (Section 12.10); the recommended choice depends on solar availability and implementation complexity.

### 12.2 Protocol Parameters

| Parameter | Default | Description |
|---|---|---|
| `LEAF_BOOT_MS` | 30 | MM6108 IBSS boot + join target (to be validated) |
| `LEAF_SCAN_WINDOW_MS` | 200 | Listen window after HELLO broadcast |
| `LEAF_INITIAL_JITTER_MS` | random(0, `LEAF_SLEEP_MS`) | One-time random offset on first boot — staggers leaves across wake interval |
| `LEAF_SLEEP_MS` | 900,000 | Default sleep interval (15 minutes — sparse-first) |
| `LEAF_SLEEP_MAX_MS` | 3,600,000 | Maximum sleep interval (1 hour) |
| `LEAF_ISOLATION_THRESHOLD` | 3 | Consecutive missed contacts before extending interval |
| `LEAF_ISOLATION_FACTOR` | 2 | Sleep interval multiplier on isolation |
| `PEER_KEY_LIFETIME_MS` | 86,400,000 | Session key validity (24 hours) |
| `RELAY_DRIFT_RATE_PPM` | 1 | Required external RTC accuracy (≤ 1 ppm TCXO) |
| `RELAY_DRIFT_WINDOW_MS` | 50 | Receive window padding either side of prediction |
| `RELAY_DRIFT_INCREMENT_MS` | 1 | Window expansion per missed cycle (1 ppm × 5min = 0.3ms, rounded up) |
| `RELAY_DRIFT_WINDOW_MAX_MS` | 200 | Maximum drift window — dominated by boot time, not RTC drift |
| `TIMESYNC_STALE_S` | 604,800 | Time since last TIME_SYNC after which node flags for resync (7 days). Conservative by design: catches real-world RTC temperature variation, detects hardware failure early, and gives operators a clean weekly calibration cadence. Overhead is ~0.03% of channel capacity per flood. |
| `RELAY_LEAF_MISS_MAX` | 10 | Consecutive missed wake windows before removing from schedule |
| `LEAF_ROUTE_LIFETIME_S` | 300 | Route lifetime for proxy RREP entries (leaf routes) — shorter than relay routes to force rediscovery after mobile leaf moves |
| `RREQ_INITIAL_TTL` | 2 | Starting TTL for RREQ expanding ring search |
| `RREQ_RETRY_INTERVAL` | 60 | Seconds before retrying failed RREQ (DTN holds bundle during this period) |
| `RREQ_MAX_RETRIES` | 3 | RREQ attempts before destination declared mesh-unreachable (Section 8.5.2) |
| `OPTION_SWITCH_SETTLE_MS` | 4,000 | Settle period after Continuous→Scheduled mode switch — peers must learn new TDMA schedule before relay begins sleeping |
| `RELAY_DUTY_BUDGET` | 0.70 | Maximum relay duty cycle before leaves are turned away |
| `RELAY_REDIRECT_PCT` | 90 | relay_load_pct threshold at which relay stops accepting new leaves |
| `GATEWAY_ANYCAST_ID` | 0x000000000001 | Reserved Node ID — any gateway responds to RREQ targeting this address; gateway is the decryption endpoint |
| `CLOUD_ANYCAST_ID` | 0x000000000002 | Reserved Node ID — routed to any gateway, but decryptable only by the cloud backend (gateway forwards blind, Section 8.11) |
| `POSITION_MAX_AGE_S` | 86,400 | Position table entry expiry (24 hours — effectively infinite for static relays) |
| `GEO_MIN_GPS_COVERAGE` | 0.30 | Minimum fraction of relay neighbours with known position before attempting Tier 1/2 routing |
| `MIN_GEO_LINK_QUALITY` | 30 | Minimum lq (0–255) for a candidate to be included in geographic routing (below this ≈ 88% packet loss — not worth routing through) |
| `DENSE_OGM_ORIGINATORS` | 30 | OGM originator count above which node switches to REACTIVE routing |
| `SPARSE_OGM_ORIGINATORS` | 10 | OGM originator count below which node may revert to PROACTIVE routing |
| `ROUTING_WINDOW_S` | 60 | Sliding window for OGM originator count measurement |
| `ROUTING_HYSTERESIS_S` | 120 | Time SPARSE condition must hold before switching back to PROACTIVE |
| `STORE_LOW_PCT` | 80 | Bundle store threshold: advertise fullness in HELLO |
| `STORE_HIGH_PCT` | 95 | Bundle store threshold: refuse inter-relay custody |
| `STORE_EVICT_PCT` | 99 | Bundle store threshold: begin eviction |
| `MULE_CUSTODY_TIMEOUT_S` | 86,400 | Time a mule holds a RETAINED bundle before actively offering it to any relay / re-delegating (24h, Section 9.5.7) |
| `KEY_CACHE_LIFETIME_S` | 604,800 | Public key cache and key directory entry expiry (7 days, Section 7.5) |
| `CERT_LIFETIME_S` | 31,536,000 | Deployment CA node certificate validity (1 year, Section 7.5.5) |
| `OTA_WAKE_ATTEMPT_MAX` | 2 | Failed extended-wake OTA transfers before a leaf backs off (Section 14.8/14.13) |
| `OTA_BACKOFF_CYCLES` | 20 | Wake cycles a leaf ignores ota_pending after hitting the attempt cap (≈5h) |
| `CONFIG_MAX_BYTES` | 256 | Maximum config payload size, v1 (Section 9.9). Larger config rejected; raise in a future version. |
| `TARGET_NEIGHBOR_COUNT` | 10 | Desired relay-neighbour count for adaptive TX power |
| `NEIGHBOR_LOW_THRESHOLD` | 4 | Never reduce TX power below the level reaching this many peers |
| `POWER_STEP_DB` | 2 | TX power adjustment increment |
| `POWER_ADJUST_INTERVAL_S` | 30 | Minimum interval between TX power changes |
| `MIN_TX_POWER_DBM` | deployment-set | Lower bound for adaptive TX power |
| `MAX_TX_POWER_DBM` | BCF regulatory limit | Upper bound for adaptive TX power |
| `RREQ_JITTER_BASE_MS` | 20 | Base RREQ re-broadcast jitter |
| `RREQ_JITTER_PER_NEIGHBOR_MS` | 4 | Jitter added per relay neighbour |
| `GOSSIP_TARGET_FORWARDERS` | 3 | Target number of nodes to re-flood each RREQ |
| `AGGREGATION_HOLD_MS` | 2000 | Max time to hold bundles for aggregation |
| `AGG_MAX_BYTES` | 1400 | Max aggregate frame payload size (under MSDU limit) |

### 12.3 Leaf Wake Sequence

A leaf wakes via one of two triggers:

```
Trigger A — RTC timer (normal cycle):
  Fires every LEAF_SLEEP_MS. Sends NORMAL priority bundle.

Trigger B — Sensor GPIO interrupt (alert):
  Fires immediately on threshold breach. Sends ALERT priority bundle.
  Does not affect RTC schedule. Normal wake resumes as scheduled.
  See Section 9.8 for full alert bundle behaviour.
```

**Normal cycle (Trigger A):**

```
Step  Time (ms)   Action
────────────────────────────────────────────────────────────────────
0     (first boot only)
                  Draw LEAF_INITIAL_JITTER_MS = random(0, LEAF_SLEEP_MS).
                  Sleep for this duration before first wake.
                  Store "jitter applied" flag in flash.
                  This spreads all leaves uniformly across the
                  wake interval within one cycle, preventing
                  simultaneous wakes in dense deployments.
1     0           RTC fires. MCU wakes from deep sleep.
2     0–30        MM6108 powers on. IBSS join on provisioned
                  channel and BSSID. No scan — channel is fixed.
                  → Target: < 30ms (LEAF_BOOT_MS). Needs validation.

3     30          Check peer link key table (stored in RTC memory):
                  a) Key exists and not expired (< 24h old):
                     Use existing session key. Skip to step 5.
                  b) Key expired or absent:
                     Proceed to PEER_OPEN after HELLO (step 6a).

4     30          Broadcast HELLO:
                  {node_id, role=leaf, battery,
                   wake_interval_ms = LEAF_SLEEP_MS,
                   next_wake_ms     = LEAF_SLEEP_MS,
                   wake_duration_ms = LEAF_SCAN_WINDOW_MS}

5     30–230      SCAN_WINDOW: listen for relay response.

6     (within SCAN_WINDOW)
                  a) No prior key — on receiving relay HELLO:
                       Run PEER_OPEN / PEER_CONFIRM (~100ms).
                       If PEER exchange fits in window: proceed.
                       If not: sleep, retry next cycle.
                  b) Prior key valid — on receiving relay HELLO:
                       Proceed immediately to data exchange.
                  c) No relay response within SCAN_WINDOW:
                       miss_counter++. Goto step 9.

7     (within SCAN_WINDOW, after peer confirmed)
      Downlink:   Receive any DATA buffered by relay for this leaf.
                  Relay delivers highest-priority bundles first
                  within remaining SCAN_WINDOW time.
      Uplink:     Send pending uplink bundles to relay peer.
                  miss_counter = 0.

8     230         Exchange complete. Record wake_timestamp.
                  Power off MM6108.
                  Store session key in RTC memory for next wake.
                  MCU enters deep sleep for LEAF_SLEEP_MS.

9     (miss path) miss_counter >= LEAF_ISOLATION_THRESHOLD:
                    LEAF_SLEEP_MS = min(LEAF_SLEEP_MS × LEAF_ISOLATION_FACTOR,
                                        LEAF_SLEEP_MAX_MS)
                  Power off MM6108. MCU deep sleep for LEAF_SLEEP_MS.
```

**Total active time per cycle**: ~230ms (normal), ~330ms (with PEER_OPEN).

### 12.4 Within-Window Exchange Protocol

The exchange within the SCAN_WINDOW is ordered to maximise utility:

```
Leaf                                    Relay
────────────────────────────────────────────────────────────
── HELLO ──────────────────────────────>
   {node_id, wake_interval, next_wake,
    wake_duration, battery}

                                        [Relay: check leaf schedule,
                                         this is the expected window]

<─ HELLO ──────────────────────────────
   {node_id, role=relay, tdma_schedule}

[Both sides: verify session key valid]

── PEER_OPEN (if key absent/expired) ──>  [conditional]
<─ PEER_CONFIRM ────────────────────────  [conditional]
   [New session key derived]

<─ DATA × N ────────────────────────────  [relay → leaf, if any]
   [Highest-priority bundles first]

── ACK ─────────────────────────────────>

── DATA × N ────────────────────────────>  [leaf → relay]
   [All pending uplink bundles]

<─ ACK ─────────────────────────────────

[Leaf: MM6108 off, MCU deep sleep]        [Relay: update schedule,
                                           return to TDMA]
```

If the leaf's SCAN_WINDOW expires before all DATA has been exchanged, the relay retains undelivered downlink bundles for the next wake cycle. Uplink bundles from the leaf take priority: the leaf sends all its data before the window closes.

### 12.5 Relay Leaf Schedule Management

Each relay maintains a schedule table entry for every leaf it has contacted:

```
struct leaf_schedule_entry {
    node_id_t  leaf_id;
    uint8_t    leaf_mac[6];
    uint32_t   wake_interval_ms;    // from leaf HELLO
    uint32_t   wake_duration_ms;    // from leaf HELLO
    uint64_t   last_hello_ts;       // timestamp of last HELLO received
    uint64_t   predicted_next_ms;   // last_hello_ts + wake_interval_ms
    uint32_t   drift_window_ms;     // current receive window padding
    uint8_t    miss_count;          // consecutive missed contacts
}
```

**Table updates:**

On receiving leaf HELLO:
```
entry.last_hello_ts    = now
entry.predicted_next   = now + entry.wake_interval_ms
entry.drift_window_ms  = RELAY_DRIFT_WINDOW_MS (reset)
entry.miss_count       = 0
```

On predicted wake window expiring with no HELLO received:
```
entry.miss_count++
entry.drift_window_ms += RELAY_DRIFT_INCREMENT_MS

if entry.miss_count >= RELAY_LEAF_MISS_MAX:
    remove entry from active schedule
    retain peer link session key (leaf may reappear)
    log: "leaf <id> removed from schedule after N misses"
elif entry.drift_window_ms > RELAY_DRIFT_WINDOW_MAX_MS:
    cap at RELAY_DRIFT_WINDOW_MAX_MS
```

**Clock drift rationale:**

```
External RTC accuracy (mandatory): ≤ 1 ppm
Per 5-minute sleep cycle: 1 × 10⁻⁶ × 300,000ms = 0.3ms

RELAY_DRIFT_WINDOW_MS = 50ms each side (100ms total window).
This is dominated by LEAF_BOOT_MS (~30ms) not RTC drift.
Even after 100 missed cycles: 0.3 × 100 = 30ms accumulated drift
— well within the 50ms window.

Relay receive window total width = 2 × drift_window_ms = 100ms
  (1 superframe slot at 100ms per slot)
  Initial: ±50ms = 100ms total window
  After 10 misses: ±60ms — still 1-slot window
  Maximum: ±200ms = 2 superframe slots (covers extreme cases)
```

### 12.6 Relay TDMA Integration with Leaf Windows

The relay's local TDMA superframe (Section 7.3) must accommodate leaf receive windows without disrupting backbone peer slots:

```
Superframe with leaf windows (relay, K=4 backbone peers, 3 leaves):

Slots 0–4:   Backbone TX/RX (peer-dedicated slots)
Slots 5–7:   MGMT (RREQ/RREP/HELLO)
Slot  8:     Leaf window L1 (1 slot = 100ms covers ±50ms drift)
Slot  9:     Leaf window L2
Slot  10:    Leaf window L3
Slots 11–19: SLEEP

With 1 ppm RTC: leaf window = 2 × RELAY_DRIFT_WINDOW_MS = 100ms = 1 slot.
Previously (150 ppm internal RTC): 1000ms = 10 slots per leaf.
The 1 ppm RTC reduces the leaf window 10× — a single slot instead of ten.
```

Leaf windows are sparse events (every 5 minutes) relative to the 2-second superframe. The relay evaluates at the start of each superframe whether any leaf is predicted to wake during it, and stays awake only for relevant windows.

### 12.7 Maximum Leaves Per Relay

With wake staggering in place, leaves arrive at uniform intervals. The relay duty cycle from leaf windows grows linearly with leaf count and sets a practical capacity limit.

**Duty cycle model:**

```
Leaf arrival rate  = N_leaves / LEAF_SLEEP_MS
Leaf window width  = 2 × RELAY_DRIFT_WINDOW_MS = 100ms  (1 ppm RTC)
Leaf duty fraction = arrival_rate × window_width
                   = N_leaves × 100 / LEAF_SLEEP_MS

For N=100, LEAF_SLEEP_MS=300,000:
  leaf_fraction = 100 × 100 / 300,000 = 3.3%   ← vs 33% with internal RTC

Backbone duty fraction (K peers):
  = (1 + K + 3) / superframe_slots
  K=2: 30%   K=4: 40%   K=6: 50%

Total relay duty cycle = backbone_fraction + leaf_fraction
  K=4, N=100: 40% + 3.3% = 43.3%   ← vs 73% with internal RTC
  K=6, N=100: 50% + 3.3% = 53.3%   ← vs 83% with internal RTC
```

**Maximum leaves formula:**

```
max_leaves = (RELAY_DUTY_BUDGET - backbone_fraction)
             × LEAF_SLEEP_MS
             / (2 × RELAY_DRIFT_WINDOW_MS)

With RELAY_DUTY_BUDGET = 0.70 (70%), LEAF_SLEEP_MS = 300,000ms:
  K=2: max_leaves = 0.40 × 300,000 / 100 = 1,200
  K=4: max_leaves = 0.30 × 300,000 / 100 = 900
  K=6: max_leaves = 0.20 × 300,000 / 100 = 600

With sparse-first default LEAF_SLEEP_MS = 900,000ms (15 min):
  K=4: max_leaves = 0.30 × 900,000 / 100 = 2,700
  K=6: max_leaves = 0.20 × 900,000 / 100 = 1,800
```

The 1 ppm RTC delivers a 10× increase in max leaves compared to the internal RTC (from 90 to 900 at K=4, 5-min interval). For practical sensor deployments this ceiling is not a constraint.

A relay exceeding its `max_leaves` threshold should advertise this in its HELLO via `relay_load_pct` so new leaves associate with less-loaded relays instead.

**relay_load_pct calculation:**

```
radio_load_pct  = (current_leaf_count / max_leaves) × 100
relay_load_pct  = max(radio_load_pct, store_pct)
```

This combines both constraints — radio time and storage — into a single value that leaves use for relay selection.

**Leaf relay preference rule:**

When a leaf hears HELLOs from multiple relays:

```
1. Discard relays with:
     connectivity = ISOLATED (no onward path)
     relay_load_pct = 100     (fully at capacity)

2. Among remaining candidates:
     Select relay with lowest relay_load_pct.
     On tie: select strongest RSSI.

3. If current relay's relay_load_pct rises above
   RELAY_REDIRECT_PCT (90%) after association:
     Relay sets relay_load_pct = 100 in HELLO.
     Leaf sees this on next wake, searches for alternative.
     Leaf sends PEER_CLOSE to current relay.
     Leaf initiates PEER_OPEN with next-best relay.
```

Redirection is passive — relays advertise load, leaves decide. No explicit redirect message is needed.

**Further improvement with ATIM**: If IBSS ATIM power save (Issue #6) is supported, drift windows can be replaced with ATIM-synchronised wake times (~10ms windows). This would increase `max_leaves` by a further ~10× beyond the 1 ppm values above. At that point the constraint is channel capacity, not relay duty cycle.

### 12.8 Downlink Delivery During Predicted Window

When the relay has buffered downlink data for a leaf and the predicted wake window opens:

```
At (predicted_next_ms - drift_window_ms):
  Relay enters leaf receive mode
  Listens for leaf HELLO

On receiving leaf HELLO:
  Relay immediately responds with HELLO (confirms it is awake)
  Relay delivers buffered DATA frames within SCAN_WINDOW
  (Leaf has started its own 200ms SCAN_WINDOW at this point)

If (predicted_next_ms + drift_window_ms) passes with no HELLO:
  Relay closes leaf receive window
  Retains buffered DATA for next predicted wake
  Increments miss_count
```

If the relay has NO buffered data for a leaf, it still opens the receive window (to collect uplink DATA from the leaf) but does not keep the wider drift window open as urgently. Specifically, a relay with no pending downlink may reduce its leaf window to `wake_duration_ms + 200ms` (just enough for drift) to save power.

### 12.9 Power Budget

#### Leaf

```
15-min cycle, 230ms active (sparse-first default):

  Active (radio + MCU):  ~20 mA × 0.230s = 4.6 mAs
  Sleep (both chips):    ~15 µA × 899.77s = 13.5 mAs
  RTC (always on):       ~0.04 µA × 900s  = 0.04 mAs
  Per cycle total:        18.1 mAs
  Average current:        18.1 / 900 = ~20 µA

  3000 mAh battery (ignoring sensor): ~17 years
  Realistic (sensor + MCU overhead):   5–8 years
```

#### Relay (see Section 12.10 for sleep option definitions)

```
Continuous mode — MCU light sleep, MM6108 always in receive:
  MM6108 IBSS receive (continuous):  12.0 mA
  ESP32-S3 FreeRTOS light sleep:      2.0 mA
  RV-3028-C7 RTC:                    ~0.0 mA (40nA)
  ─────────────────────────────────────────────
  Total:                             ~14.0 mA

Scheduled mode — Full sleep, TDMA-based wake (K=6, 100 leaves, 15-min interval):

  Source of each value:
  ┌─────────────────────────────────────────────────────────┐
  │ MM6108 RX current: 12 mA (estimated — measure in Ph.1) │
  │ MCU light sleep:    2 mA (ESP32-S3 with SDIO clocked)  │
  │ 50% duty: K=6, 10/20 TDMA slots active                 │
  │ 1.1% leaf: 100 leaves × 100ms / 900,000ms (15-min)     │
  └─────────────────────────────────────────────────────────┘

  MM6108 active   (50%):    12 mA × 0.50  =  6.00 mA
  MM6108 sleep    (50%):     0.1 mA × 0.50 =  0.05 mA
  Leaf windows   (1.1%):    12 mA × 0.011 =  0.13 mA
  MCU active      (50%):     2.0 mA × 0.50 =  1.00 mA
  MCU deep sleep  (50%):    0.01 mA × 0.50 =  0.01 mA
  RTC (always):                              ~0.00 mA
  ─────────────────────────────────────────────────────
  Total:                                    ~7.2 mA

  Simplified: the dominant term is 12 mA × 50% = 6.0 mA.
  Everything else contributes ~1.2 mA combined.
```

#### Scheduled mode sensitivity: backbone peers K and leaf count

```
K peers   Backbone duty   Avg current (100 leaves, 15-min interval)
────────────────────────────────────────────────────────────────────
2         30%             4.3 mA
4         40%             5.7 mA
6         50%             7.2 mA
8         60%             8.6 mA
12+       ~80%            11.0 mA → Continuous mode simpler at this density

Leaf count impact at K=6, 1ppm RTC, 15-min leaf interval
(leaf window = 1.1% per 100 leaves at 15-min interval):
  Leaves   Leaf window   Additional   Total
  ────────────────────────────────────────
  0        0%            0.0 mA       7.1 mA
  100      1.1%          0.13 mA      7.2 mA
  300      3.3%          0.4 mA       7.5 mA
  900      10%           1.2 mA       8.3 mA

With 1ppm RTC and 15-min leaf interval, leaf count has
almost no impact on relay power — the dominant variable
is backbone peer count K.
```

Leaf count has minimal impact at low counts thanks to the mandatory 1 ppm RTC narrowing leaf windows to 100ms. Backbone peer count K is the dominant variable.

#### Gateway

Always on, mains powered.

### 12.10 Relay Sleep Mode

#### Overview

Unlike leaf nodes, which have a clear deep-sleep cycle between wake events, relay nodes face a dilemma: they must be available to receive frames from backbone peers and leaves, but they also need to conserve power. Two modes are defined:

```
Continuous mode: MM6108 always in receive. MCU light-sleeps between frames.
Scheduled mode:  Both MM6108 and MCU fully off between TDMA slots.
                 RTC alarm wakes both at each scheduled active slot.
```

The question each mode answers: **what does the relay do when nobody is talking to it?**

**Continuous mode** keeps the radio on at all times. The MM6108 listens continuously and taps the MCU on the shoulder (interrupt pin) when a frame arrives. The MCU wakes in microseconds, processes the frame, and returns to light sleep. Simple. Never misses a frame.

**Scheduled mode** powers both the radio and MCU completely off between scheduled TDMA active slots. An RTC alarm fires at each slot boundary — MCU wakes, powers on MM6108, MM6108 boots and joins IBSS, receives or transmits, then both power off again. Half the power consumption of Continuous mode, but complex and can miss frames during sleep slots.

```
                    Continuous mode    Scheduled mode
────────────────────────────────────────────────────────────────
Radio               Always in RX       Off between TDMA slots
MCU                 Light sleep        Deep sleep between slots
Avg power           ~14 mA             ~7.2 mA
Battery (2x18650)   ~15 days           ~29 days
Misses frames?      Never              Yes (during sleep slots)
Complexity          Low                High (TDMA schedule + boot)
RISK-02 dep?        No                 Yes (MM6108 boot < 100ms)
Status              Available          Requires investigation
                                         (see RISK-02 and note below)
```

The ~7 mA saving in Scheduled mode comes almost entirely from powering down the MM6108: it draws ~12 mA in receive mode, so switching it off for 48.9% of the time saves ~5.9 mA.

> **Note — Scheduled mode requires investigation before use.**
> Scheduled mode depends on MM6108 IBSS boot + join time being less than one TDMA slot
> width (100ms). This has not yet been measured on the target hardware (RISK-02 in the
> development plan). If boot time exceeds 100ms, Scheduled mode will silently drop frames
> that arrive before boot completes. Continuous mode has no such dependency and is
> recommended until RISK-02 is resolved. See Section 12.10 “Scheduled mode prerequisite”
> for full details and resolution options.

#### Option selection and switching

**The sleep option defaults to the firmware build-time flag but can be changed at runtime.**

```
Build-time default (sets initial value in NVS on first boot):
  CONFIG_RIMBA_RELAY_SLEEP_OPTION = CONTINUOUS   ← recommended default
  CONFIG_RIMBA_RELAY_SLEEP_OPTION = SCHEDULED   ← requires investigation (see note)

Runtime change: node reads current option from NVS on boot.
  Operator or OTA config bundle can update the NVS value.
  Change persists across reboots (stored in NVS).
```

**Runtime switching sequence:**

Peers learn the relay's sleep mode from its `tdma_rx_slots` field in HELLO.
Removing or adding this field is sufficient to notify all peers — no new
frame type is needed.

```
Switching Continuous mode → Scheduled mode (add TDMA sleep):

  1. Node computes its TDMA schedule (Section 12.6).
  2. Node begins advertising tdma_rx_slots in HELLO immediately.
  3. Node waits OPTION_SWITCH_SETTLE_MS (default 4,000ms —
     two superframe cycles) for peers to learn the new schedule.
  4. Node begins sleeping between active slots.

  Peers receive updated HELLO → honour new schedule.
  No explicit acknowledgement required.
  If a peer misses the updated HELLO and transmits during a sleep
  slot: 802.11ah exponential backoff retries until next active slot.

Switching Scheduled mode → Continuous mode (remove TDMA sleep):

  1. Node stops sleeping immediately.
  2. Node removes tdma_rx_slots from next HELLO.
  3. Peers receive updated HELLO → may send at any time.

  Simpler direction — always-on is immediately effective.
  No settle period required.
```

**NVS storage:**

```
NVS key:  "relay_sleep_opt"
Type:     uint8
Values:   0 = Continuous,  1 = Scheduled
Default:  set from CONFIG_RIMBA_RELAY_SLEEP_OPTION at first boot

Changed by:
  a) OTA config bundle (bundle addressed to this node)
  b) UART/USB debug command (development only)
  c) Re-provisioning via BLE (Section 3.6.2)
```

**Mule sleep mode during transit:**

A mule node is powered (vehicle or drone battery) and moves between relays.
Running Scheduled mode during transit adds up to 1,000ms wait at each relay while
the mule's MM6108 is off and waiting for its next active slot. Continuous mode
eliminates this wait.

```
Mule recommendation:
  Build default: CONFIG_RIMBA_RELAY_SLEEP_OPTION = CONTINUOUS
  Runtime:       switch to Continuous mode before departing on sweep
                 switch to Scheduled mode when stationary (saves 7 mA)

  Continuous mule + Continuous relay: ~50ms to first HELLO exchange
  Continuous mule + Scheduled relay: ≤ 1,050ms to first HELLO exchange
  Scheduled mule + Scheduled relay: ≤ 2,000ms (both may be sleeping)
```

**Recommended defaults:**

```
Node type    Build default    Runtime change?    Rationale
──────────────────────────────────────────────────────────────────
Relay        Continuous mode         Yes (NVS)          Scheduled mode requires
                                                 investigation (RISK-02)
Mule         Continuous mode         Yes (NVS)          Switch to Scheduled
                                                 when stationary (optional)
Gateway      Continuous mode         Yes (NVS)          Always-on by nature
Leaf         N/A              N/A                RTC alarm, not TDMA
```

#### Runtime switching — known issues and mitigations

**Issue 1: Trickle suppression during Continuous→Scheduled settle period**

The Continuous→Scheduled transition relies on all peers receiving the updated HELLO (with
`tdma_rx_slots`) within `OPTION_SWITCH_SETTLE_MS` (4,000ms). If Trickle has
backed off to a long interval (e.g. 16s on a stable network), no HELLO fires
during the settle window and peers start missing frames when the relay begins
sleeping.

```
Mitigation (required):
  On any sleep mode change: call rimba_trickle_reset() BEFORE
  starting the settle timer. This fires a HELLO immediately and
  resets the Trickle doubling interval to base rate, guaranteeing
  peers receive the updated schedule well within 4,000ms.

Sequence:
  1. Update NVS "relay_sleep_opt" to new value
  2. Update HELLO payload (add/remove tdma_rx_slots)
  3. rimba_trickle_reset()          ← fires HELLO immediately
  4. Start OPTION_SWITCH_SETTLE_MS timer
  5. On timer expiry: apply new sleep behaviour
```

**Issue 2: NVS flash wear for frequent mule switching**

If a mule writes to NVS at every relay stop (Continuous→Scheduled then Scheduled→Continuous), it accumulates
~80 NVS writes/day. Over years of operation this approaches flash endurance limits.

```
Mitigation:
  Distinguish two kinds of sleep mode change:

  Persistent change (operator config):
    Written to NVS. Survives reboots.
    Used for: deliberate deployment reconfiguration.
    Triggered by: OTA config bundle, BLE re-provision.

  Transient change (mule transit):
    RAM flag only. Reverts to NVS value on reboot.
    Used for: mule switching Continuous mode for a sweep,
              returning to Scheduled mode when stationary.
    Triggered by: application layer (sweep start/end).
    Zero NVS writes.

Firmware: rimba_set_sleep_option(opt, PERSIST_NVS)
          rimba_set_sleep_option(opt, TRANSIENT_RAM)
```

**Issue 3: Silent failure if Scheduled mode enabled without RISK-02 validation**

Scheduled mode requires MM6108 IBSS boot + join time < `TDMA_SLOT_MS` (100ms).
If a relay switches to Scheduled mode on hardware where boot time exceeds the slot
width, it silently drops frames that arrive before boot completes. No error
is raised.

```
Mitigation:
  Firmware measures MM6108 boot time during Phase 1 startup
  and stores the result in NVS ("mm6108_boot_us" key).

  Before allowing Continuous→Scheduled switch:
    if NVS["mm6108_boot_us"] not set:
      log warning: "RISK-02 not validated on this device"
      reject switch OR apply with explicit operator override
    if NVS["mm6108_boot_us"] >= TDMA_SLOT_MS * 1000:
      log error: "MM6108 boot time exceeds slot width"
      reject switch (Scheduled mode would cause silent frame loss)

  RISK-02 validation is a Phase 1 task (development plan).
  Scheduled mode MUST NOT be enabled in production until validated.
```


#### Continuous Mode — MCU Light Sleep

```
MM6108:   ──────────────────────────────────────  always in RX
MCU:      ████░░░░░████░░░░░████░░░░░████░░░░░   light sleep between events

Wake trigger: MM6108 asserts interrupt line to MCU when frame arrives.
MCU wakes in microseconds, processes frame, returns to light sleep.
Total: ~14 mA average.
```

**Advantages:**
- Simple to implement — standard ESP32 FreeRTOS light sleep
- Never misses an incoming frame
- No schedule enforcement required by peers
- No boot time dependency

**Implementation:**
```c
// MM6108 interrupt → wake MCU
gpio_wakeup_enable(MM6108_INT_PIN, GPIO_INTR_LOW_LEVEL);
esp_sleep_enable_gpio_wakeup();

// Main loop: light sleep between events
while (1) {
    esp_light_sleep_start();   // MCU sleeps, MM6108 keeps RX active
    // MCU woke due to MM6108 interrupt
    rimba_process_pending_frames();
}
```

The TDMA schedule (Section 12.6) is still used to coordinate WHEN peers send to this relay, but the relay does not power down its radio — it just lets the MCU rest between arrivals.

#### Scheduled Mode — Full Sleep, TDMA Wake

```
MM6108:   ████░░░░░░░████░░░░░░░████░░░░░░░████   off between active slots
MCU:      ████░░░░░░░████░░░░░░░████░░░░░░░████   deep sleep between slots

Wake trigger: MCU internal RTC timer fires at next active slot boundary.
MCU wakes, powers on MM6108, receives frames, both sleep again.
Total: ~7.2 mA (K=6, 100 leaves, 15-min leaf interval)

Derivation:
  Active 51.1% = 50% backbone + 1.1% leaf windows (100×100ms/900s)
  MM6108 active:    12 mA × 51.1% = 6.13 mA
  MCU light sleep:   2 mA × 51.1% = 1.02 mA
  MM6108 hibernate: 0.1 mA × 48.9% = 0.05 mA
  MCU deep sleep:  0.02 mA × 48.9% = 0.01 mA
  Total:                             7.21 mA ≈ 7.2 mA
```

**Wake timing mechanism:**

The 802.11ah IBSS TSF (Timing Synchronization Function) provides a shared 64-bit microsecond counter across all IBSS nodes, synchronised via beacon exchange. The relay uses this to know exactly when to wake:

```c
uint64_t tsf_us = mmwlan_get_tsf_us();
uint64_t current_slot = (tsf_us / SLOT_DURATION_US) % SLOTS_PER_FRAME;
uint64_t next_active_slot = find_next_active_slot(current_slot, my_schedule);
uint64_t sleep_duration_us = (next_active_slot - current_slot) * SLOT_DURATION_US;

esp_deep_sleep(sleep_duration_us);  // both MCU and MM6108 powered down
```

**Peer schedule enforcement:**

For Scheduled mode to work, all peers must honour the relay's TDMA schedule:

```
Relay R1 HELLO advertises: tdma_rx_slots = [1, 2, 3, 4, 5, 6]

Relay R2 wants to send to R1:
  1. Compute current_slot from local TSF counter
  2. Wait until current_slot ∈ R1.tdma_rx_slots
  3. Transmit

If R2 transmits during R1's sleep slot:
  → No ACK received
  → 802.11 exponential backoff and retry
  → Retries eventually land in an R1 active slot
  → OR: R2 reads R1's schedule from HELLO and waits
```

**Bootstrap — new peer without cached schedule:**

```
New node N encounters relay R1 for the first time:
  N broadcasts HELLO during any slot (no schedule yet)
  R1 is awake during its MGMT slots → hears N's HELLO
  R1 responds with HELLO → N learns R1's tdma_rx_slots
  N waits for R1's next RX slot before sending DATA
  Bootstrap delay: at most one superframe (default 2 seconds)
```

**Scheduled mode prerequisite — boot time validation (RISK-02):**

Scheduled mode requires MM6108 IBSS boot + join time < one slot duration (100ms). This must be measured in Phase 1 of development (see development plan). If boot time exceeds 100ms:

```
Resolution A: Increase TDMA_SLOT_MS to accommodate boot time
Resolution B: Keep MM6108 in doze/standby (not full power-off)
              between slots — partial power saving
Resolution C: Stay with Continuous mode if savings don't justify complexity
```

#### Battery and Solar Analysis

Using 2× 18650 in parallel (5,000 mAh effective at 3.3V):

```
Battery-only survival (no solar):
  Continuous mode (14.0 mA):  5,000 / 14.0 = 357h  = 14.9 days  (~2 weeks)
  Scheduled mode  (7.2 mA):  5,000 /  7.2 = 694h  = 28.9 days  (~4 weeks)

Solar sustainability — 2W panel, various irradiance conditions:
  (Daily harvest = 2W × peak_hours × 0.85 efficiency at 3.7V)

  Peak sun hrs   Harvest       Continuous mode net    Scheduled mode net
  ─────────────────────────────────────────────────────────────
  1h (overcast)  459 mAh/day   +123 mAh ✅     +279 mAh ✅
  2h (winter)    919 mAh/day   +583 mAh ✅     +739 mAh ✅
  3h (cloudy)  1,378 mAh/day  +1042 mAh ✅   +1198 mAh ✅
  5h (full sun) 2,297 mAh/day +1961 mAh ✅   +2117 mAh ✅

Both options are self-sustaining with a 2W panel even in
heavy overcast (1 peak sun hour). Solar panel choice matters
far more than sleep mode for relay longevity.

Minimum panel for net-zero operation:
  Continuous mode: 0.5W panel (0.29W theoretical minimum)
  Scheduled mode: 0.16W panel (tiny — any panel sustains Scheduled mode)
```

#### Design Recommendation

```
For v1 (deploy now):
  Use Continuous mode + 2W solar panel.
  Simple, reliable, no boot time dependency.
  Self-sustaining in all real-world outdoor conditions.
  Battery survives ~2 weeks of total solar failure.

Consider switching to Scheduled mode if all of the following are true:
    a) MM6108 IBSS boot time confirmed < 100ms (RISK-02 resolved)
    b) Relay operates in conditions where 7 mA saving is meaningful
       (battery-only, no solar, or extremely low-light environment)
    c) TDMA coordination tested at target node count

For high K deployments (K ≥ 10):
  Scheduled mode approaches Continuous mode current draw — use Continuous mode
  with a slightly larger solar panel instead.
```

### 12.11 Leaf Sleep Mode

The same Continuous and Scheduled mode naming applies to leaf nodes. However, the leaf's Scheduled mode is fundamentally different in scale from the relay's — the leaf sleeps for minutes at a time (LEAF_SLEEP_MS = 15 min) rather than the relay's 100ms TDMA slots.

#### Mode definitions

```
Leaf Continuous mode: MM6108 always in RX. MCU light-sleeps between events.
                      Wake on any incoming frame (interrupt) or sensor GPIO.

Leaf Scheduled mode:  MCU + MM6108 fully off between RTC alarm wake events.
                      Wakes every LEAF_SLEEP_MS. Active for ~230ms per cycle.
                      (Current default — same deep sleep described in Section 12.3)
```

```
                    Leaf Continuous mode    Leaf Scheduled mode
────────────────────────────────────────────────────────────────
Radio               Always in RX            Off between RTC wakes
MCU                 Light sleep             Deep sleep
Avg power           ~14 mA                  ~20 µA
Battery (3000 mAh)  ~9 days                 ~17 years
Wake trigger        Any frame or sensor     RTC alarm or sensor GPIO
Downlink reliable   Yes — always listening  No — relay must predict wake
Relay TDMA slot     Not needed              Required (one slot per leaf)
ALERT latency       ~10ms (no boot needed)  ~50ms (boot + TX)
Suitable for        Powered leaves only     Battery leaves
```

#### When to use Continuous mode on a leaf

```
Use Leaf Continuous mode when:
  - Leaf has a vehicle battery or permanent power supply
  - Reliable downlink delivery is required (remote config,
    firmware update bundles, actuation commands)
  - Lowest possible ALERT latency needed (~10ms vs ~50ms)
  - Leaf is effectively stationary and always in relay range

Do NOT use on battery-powered leaves:
  14 mA vs 20 µA = 700x more power.
  3000 mAh battery lasts 9 days instead of 17 years.
```

#### Effect on the relay when leaf runs Continuous mode

A leaf in Continuous mode behaves like a low-power relay from the network's perspective — it is always present as an IBSS peer.

```
Relay changes when serving a Continuous mode leaf:
  No TDMA slot allocation needed for this leaf.
    → Frees one slot in the relay's superframe.
    → Relay can serve more Scheduled mode leaves with freed slot.
  No drift window needed.
    → No predicted_next_wake_ms tracking for this leaf.
  Downlink: relay sends at any time, no buffering needed.
  Uplink: leaf sends when it has data, relay receives immediately.

Relay detects Continuous mode leaf via HELLO:
  Leaf in Continuous mode omits next_wake_ms and wake_duration_ms
  from its HELLO payload.
  Relay sees no wake schedule → treats leaf as always-available peer.
```

#### ALERT behaviour in Continuous mode

The sensor GPIO interrupt still wakes the MCU from light sleep when a threshold is breached, same as Scheduled mode. The difference is that MM6108 is already on — no boot delay.

```
Scheduled mode ALERT:  Sensor GPIO → MCU wakes → MM6108 boots (30ms)
                       → HELLO → DATA(ALERT) → total ~50ms

Continuous mode ALERT: Sensor GPIO → MCU wakes → DATA(ALERT) immediately
                       → total ~10ms (no boot needed)
```

#### Selection and switching

Same mechanism as relay sleep mode (Section 12.10):

```
Build-time default:  CONFIG_RIMBA_LEAF_SLEEP_OPTION = SCHEDULED
Runtime change:      NVS key "leaf_sleep_opt", same PERSISTENT/TRANSIENT
                     distinction as relay.
Switching:           Continuous → Scheduled: leaf sends HELLO with
                                             next_wake_ms field, then
                                             enters deep sleep on next cycle.
                     Scheduled → Continuous: leaf omits next_wake_ms from
                                             HELLO, stops calling deep sleep.
```

```
Node          Build default       Suitable for
──────────────────────────────────────────────────────────────
Leaf          Scheduled mode      Battery-powered (default)
              Continuous mode     Vehicle/mains-powered leaf
```


---

## 13. Dense and Sparse Deployment

### 13.1 Scale Guidelines

| Node count | Routing mode | Notes |
|---|---|---|
| ≤ 100 | OGM proactive (Appendix A) | Simple, low latency |
| 100–500 | OGM + aggressive Trickle | Monitor channel load |
| 500–1000 | Reactive RREQ/RREP | Standard mode for this spec |
| > 1000 | Zone routing required | Outside scope of v1 |

### 13.2 Channel Capacity at Target Scale

```
500 relay nodes + leaves, 1MHz channel (~150 kbps usable):

Routing overhead (reactive): ~15 kbps
Data traffic (multi-hop):    ~20 kbps
HELLO beacons (all nodes):   ~5 kbps (Trickle-suppressed)
Total backbone:              ~40 kbps  ✓ within limit

100 leaves per relay (local segment):
  Leaf HELLO on wake: 100 × 1/300s × 60B × 8 = 1.6 kbps
  Leaf DATA:          100 × 1/300s × 300B × 8 = 8 kbps
  With multi-hop relay forwarding (avg 3 hops): ~24 kbps
  Total local leaf traffic: ~26 kbps  ✓ within limit
```

### 13.3 Dense Deployment

Rimba is a sparse-first protocol. Dense deployments are supported via graceful degradation and adaptive self-organisation rather than dedicated dense-mode optimisation. Three adaptive mechanisms keep a densifying network healthy.

#### 13.3.1 Adaptive Transmit Power (Topology Control)

Each node independently adjusts TX power to target a manageable neighbour count. In dense areas this shrinks the effective radio cell, converting a contention problem into a (DTN-tolerable) extra-hop problem.

```
Every POWER_ADJUST_INTERVAL_S:

  n = count of relay/gateway/mule neighbours
      (last_seen < NEIGHBOR_TIMEOUT)

  if n > TARGET_NEIGHBOR_COUNT × 1.5:    # too dense
      tx_power -= POWER_STEP_DB
  elif n < TARGET_NEIGHBOR_COUNT × 0.5:  # too sparse
      tx_power += POWER_STEP_DB

  Clamp: MIN_TX_POWER_DBM ≤ tx_power ≤ MAX_TX_POWER_DBM

  Safety floor — never reduce power if ANY of:
    n ≤ NEIGHBOR_LOW_THRESHOLD
    any neighbour advertises connectivity = ISOLATED
    tx_power already at MIN_TX_POWER_DBM
```

Effect: in sparse areas power stays high (max range, fewest hops); in dense areas the cell shrinks until contention is manageable. Convergence is gradual (POWER_STEP_DB per interval) to avoid oscillation. The safety floor prevents the network from fragmenting into islands.

Secondary benefit: reduced collisions mean fewer retransmissions, which can lower net energy use in dense areas despite the added hops.

#### 13.3.2 Adaptive RREQ Jitter

RREQ re-broadcast jitter scales with local neighbour count so floods spread out more in denser cells:

```
RREQ_JITTER_MS = max(RREQ_JITTER_BASE_MS,
                     relay_neighbor_count × RREQ_JITTER_PER_NEIGHBOR_MS)

  5 neighbours:  max(20, 20)  = 20ms
  15 neighbours: max(20, 60)  = 60ms
  30 neighbours: max(20, 120) = 120ms
  50 neighbours: max(20, 200) = 200ms
```

At 50 neighbours with 200ms jitter, average gap between re-broadcasts is ~4ms versus ~1ms 802.11ah frame time — collisions drop sharply. Cost is higher route-discovery latency, acceptable under sparse-first/DTN assumptions.

#### 13.3.3 Trickle Suppression

HELLO and OGM intervals double when the network is stable (RFC 6206). In a fully-connected dense cell this is highly effective: the first re-flood of an OGM triggers mass suppression across all nodes that already heard it.

#### 13.3.4 Wake Collision Prevention

`LEAF_INITIAL_JITTER_MS` (Section 12.2) spreads leaves uniformly across the wake interval on first boot. Natural RTC drift maintains the spread thereafter.

#### 13.3.5 Relay Leaf Capacity

Each relay has a maximum leaf count from its duty-cycle budget (Section 12.7), advertised via `relay_load_pct`. Leaves migrate to less-loaded relays automatically.

With the mandatory 1 ppm RTC, leaf windows are 100ms (1 superframe slot), vs 1000ms (10 slots) with an internal MCU oscillator. The practical ceiling at 70% duty budget:

| Backbone peers K | Max leaves (5-min sleep) | Max leaves (15-min sleep) |
|---|---|---|
| 2 | 1,200 | 3,600 |
| 4 | 900 | 2,700 |
| 6 | 600 | 1,800 |

These ceilings exceed any expected sensor deployment. Leaf count is no longer a practical constraint with a 1 ppm RTC.

#### 13.3.6 Gossip-Based RREQ Flood Control

RREQ forwarding probability scales inversely with neighbour count (full definition in Section 8.7.1):

```
p = min(1.0, GOSSIP_TARGET_FORWARDERS / neighbor_count)
  = min(1.0, 3 / neighbor_count)

  4 neighbours:  p = 0.75  (sparse — forward most RREQs)
  10 neighbours: p = 0.30  (normal)
  20 neighbours: p = 0.15  (dense — only 15% re-broadcast)
  50 neighbours: p = 0.06  (very dense — 1 in 17 nodes forwards)
```

Combined with adaptive jitter (13.3.2), this keeps RREQ floods from saturating dense cells while ensuring adequate propagation in sparse ones. In sparse cells with ≤ 4 neighbours, gossip overrides ensure full re-broadcast regardless of p.

#### 13.3.7 Routing Mode Switching

The routing protocol switches between proactive (OGM) and reactive (RREQ/RREP) modes based on network scale (full definition in Section 8.1):

```
Detection metric: ogm_originator_count
  (unique OGM originators seen in a 60-second window)

  > 30 originators seen → switch to REACTIVE (RREQ/RREP)
  < 10 for 120 seconds → switch back to PROACTIVE (OGM)
  Hysteresis: ROUTING_HYSTERESIS_S = 120s prevents oscillation
```

In small sparse deployments, OGM builds a complete forwarding table proactively. In larger/denser deployments, reactive RREQ/RREP avoids the OGM flood overhead. The node makes this decision autonomously from its own observation of the mesh — no coordination required.

#### 13.3.8 Density Ceiling

| Nodes in mutual radio range | Behaviour |
|---|---|
| ≤ 20 | Excellent — no adaptation needed |
| 20–50 | Good — adaptive power/jitter engage |
| 50–100 | Degraded but working — more hops, higher latency |
| 100–200 | Heavy degradation — data flows via DTN, latency high |
| > 200 | Breaking — IBSS without RAW is the wrong tool at this density |

The > 200 ceiling is fundamental: IBSS relies on CSMA/CA, which has no scheduled access. Deployments requiring reliable operation above this density should use 802.11ah AP/STA mode with RAW (outside Rimba's scope). See Section 15 for candidate future mitigations.

### 13.4 Sparse Deployment

#### DTN operation

Nodes with no backbone path store bundles indefinitely (subject to lifetime). The relay's bundle store management policy (Section 9.6) governs eviction when storage fills.

#### Leaf isolation

Leaves extend scan window to 2000ms when isolated (`miss_counter >= LEAF_ISOLATION_THRESHOLD`). `LEAF_SLEEP_MS` doubles on each subsequent miss up to `LEAF_SLEEP_MAX_MS`. This preserves battery during extended isolation while remaining discoverable by a passing mule.

#### Mule sweeping

Mule sweep intervals should be shorter than the critical data bundle lifetime (default 24 hours for sensor readings, 72 hours for alerts). Mules prioritise visiting relays with the highest `store_pct` (Section 9.6.3). Full mule protocol in Section 9.5.

### 13.5 Density Self-Detection and Adaptation Summary

Every Rimba node continuously measures its local environment and adjusts behaviour automatically. No configuration change or operator intervention is required. The adjustments are driven by five passive metrics, all derived from normal protocol operation.

#### 13.5.1 Detection Metrics

```
Metric               Source               Updates every
────────────────────────────────────────────────────────────────────────
neighbor_count       HELLO neighbour       NEIGHBOR_TIMEOUT window
                     table                 (any HELLO arrival)

ogm_originator_count OGM routing table     60-second sliding window
                     (proactive mode only) (new OGM arrival)

connectivity_state   Route table +         Any route change
                     gateway anycast       (RREQ/RREP result,
                     entry status         route expiry)

relay_load_pct       Peer HELLO fields     Any HELLO from a relay peer

store_pct            Local bundle store    Any bundle stored/evicted
```

Each metric is **local observation only** — no node queries its neighbours for environment information. The picture emerges passively from normal traffic.

#### 13.5.2 Density Tiers

```
Tier          Definition                 Primary signal
──────────────────────────────────────────────────────────────────────────
ISOLATED      connectivity_state         No peers heard at all.
              = ISOLATED                 Node is on its own.

SPARSE        1 ≤ neighbor_count ≤ 4    Reachable but few peers.
              (NEIGHBOR_LOW_THRESHOLD)   DTN-first operation likely.

NORMAL        5 ≤ neighbor_count ≤ 15   Target operating zone.
              (TARGET_NEIGHBOR_COUNT)    All mechanisms at baseline.

DENSE         neighbor_count > 15        More neighbours than desired.
                                         Contention mitigations engage.

VERY DENSE    neighbor_count >           Approaching IBSS ceiling.
              TARGET_NEIGHBOR_COUNT×3    Adaptive power reducing cell.
              (> 30)
```

Tier is not a discrete state machine — each adaptation has its own threshold and engages gradually. A node with 12 neighbours is in NORMAL but still runs gossip (p = 0.25) and moderate jitter (48ms). Transitions are smooth, not stepped.

#### 13.5.3 Adaptation Table

All automatic adjustments a node makes in response to density:

| Mechanism | Sparse / Isolated | Normal | Dense | Spec section |
|---|---|---|---|---|
| **TX power** | Maximum (increase if n < 4) | TARGET_NEIGHBOR_COUNT goal | Reduce until n ≤ 15 | 13.3.1 |
| **RREQ jitter** | 20ms (minimum) | 20–60ms | Up to 200ms (n × 4) | 13.3.2 |
| **RREQ gossip p** | 1.0 (always forward) | 3/n | 3/n (auto-reduces) | 8.7.1, 13.3.6 |
| **Routing mode** | OGM proactive | OGM or RREQ | RREQ reactive | 8.1, 13.3.7 |
| **HELLO interval** | Trickle base rate | Trickle suppresses duplicates | Heavily suppressed | 7.1 |
| **Leaf sleep (LEAF_SLEEP_MS)** | Doubles on each miss (up to max) | Normal schedule | Normal schedule | 12.3, 13.4 |
| **Leaf scan window** | 2,000ms (extended isolation) | 200ms normal | 200ms normal | 12.3 |
| **Bundle forwarding** | Buffer (DTN store) | Forward if route exists | Forward if route exists | 9.1–9.4 |
| **Relay leaf migration** | N/A (no relays to migrate to) | Normal | relay_load_pct > 90% → migrate | 13.3.5 |
| **Geographic routing tier** | Tier 3 (RREQ fallback) | Tier 2 or 3 | Tier 1 (GPS, no flood) | 8.12 |
| **Wake staggering** | LEAF_INITIAL_JITTER_MS on boot | Applied once only | Applied once only | 12.3, 13.3.4 |

#### 13.5.4 How the Mechanisms Work Together

The adaptations are layered and complementary:

```
Density increases (more neighbours detected):
  1. TX power steps down  → radio cell shrinks → fewer neighbours see each frame
  2. RREQ jitter grows   → floods spread out temporally → fewer simultaneous TX
  3. Gossip p drops       → fewer nodes re-broadcast → flood volume falls
  4. Trickle suppresses   → HELLO/OGM beacons stop on stable nodes
  5. If very large:
     routing switches to RREQ → OGM floods stop entirely

Density decreases (fewer neighbours detected):
  1. TX power steps up   → radio cell grows → more nodes in range
  2. RREQ jitter falls   → floods propagate faster → lower discovery latency
  3. Gossip p rises      → all RREQs re-broadcast → no message loss in sparse cell
  4. Trickle fires again → HELLO/OGM resume at base rate
  5. If originator count < 10 for 120s:
     routing switches back to OGM → proactive forwarding resumes

Isolation (no neighbours at all):
  1. TX power at maximum → maintain maximum radio range
  2. Leaf scan window extends → 2,000ms to catch any relay in range
  3. LEAF_SLEEP_MS doubles on miss → battery preserved during isolation
  4. All data buffered in DTN store → waiting for any path to appear
  5. Mule contact: data drains via custody transfer on next sweep
```

#### 13.5.5 What Nodes Do NOT Adapt Automatically

The following are fixed at provisioning and do not adapt to density:

```
Fixed at provisioning:
  LEAF_SLEEP_MS      Deployment-wide policy, not density-driven.
                     (Only isolation backoff doubles it temporarily.)

  Bundle lifetime    Set per bundle type at creation; not adapted.

  Role               A relay does not become a leaf in sparse conditions.

  Channel            All nodes use the same channel for the lifetime
                     of the deployment.
```

These are intentional. Bundle lifetime in particular must not vary automatically — a 24-hour sensor bundle must expire at 24 hours regardless of how many neighbours the relay has.

### 13.6 Mobile Node Behaviour and Limitations

Mobile node behaviour (mobile leaf, mobile relay, speed limits, battery impact, and provisioning parameters) is fully specified in **Section 3.7**. Mobile support is summarised there because it is fundamentally an architectural property of the node role rather than a density adaptation.

Key points (see Section 3.7 for full detail):
- Mobile leaf (Scheduled mode): speed limited by `LEAF_SLEEP_MS` and relay spacing — ~2 km/h at 15-min interval. Faster requires shorter intervals and more battery.
- Mobile leaf (Continuous mode, Section 12.11): no speed limit, but ~14 mA draw — powered assets only.
- Mobile relay: not recommended — causes leaf orphaning and RREQ storms. Use a dedicated mule instead.
- Provisioning parameters for mobile leaves: Section 3.7.4.

## 14. Over-The-Air Firmware Update (OTA)

### 14.0 End-to-End Campaign Overview

Before the details, here is a complete OTA campaign from start to finish — the
gateway announcing a new version, through to a Scheduled leaf finishing its
update. This timeline ties together the announce (14.4), the relay staging and
state machine (14.5), the three delivery mechanisms (14.1e), and the leaf
streaming sequence (14.1c).

```
 Gateway              Relay (connected)         Scheduled Leaf
   │                       │                          │  (asleep, 15-min cycle)
   │ operator loads        │                          │
   │ signed image v1.2.0   │                          │
   │                       │                          │
 ① │── payload_announce ──►│                          │
   │   (BPv7 bundle, L4:    │                          │
   │    ver, size, hash,   │ verify announce sig      │
   │    Ed25519 sig)       │ ver > mine? yes          │
   │                       │                          │
 ② │◄── stream request ────│ relay enters RECEIVING   │
   │── OTA_START ─────────►│                          │
   │── OTA_CHUNK × N ──────►│ esp_ota_write to        │
   │   (+ NACK/retransmit) │ ota_stage               │
   │── OTA_EOF ───────────►│                          │
   │                       │ VERIFYING:              │
   │                       │  SHA-256 == hash? ✓     │
   │                       │  Ed25519 sig valid? ✓   │
   │                       │ → READY                 │
   │                       │                          │
   │                  ③    │ ota_pending=true in HELLO│
   │                       │                          │
   │                       │         leaf wakes ──────►│ ④ sees ota_pending
   │                       │◄─── HELLO reply ──────────│   verifies: authed
   │                       │     {ota_ready=true}     │   relay? signed
   │                       │                          │   announce? ver newer?
   │                       │                          │   → stay awake
   │                       │── OTA_START ────────────►│ ⑤ esp_ota_begin(ota_1)
   │                       │── OTA_CHUNK × N ─────────►│   write each chunk,
   │                       │   (+ NACK/retransmit)    │   discard from RAM
   │                       │── OTA_EOF ───────────────►│ ⑥ esp_ota_end()
   │                       │                          │   SHA-256 == hash? ✓
   │                       │                          │   Ed25519 valid? ✓
   │                       │◄── OTA_ACK{SUCCESS} ──────│ ⑦ set boot partition,
   │                       │                          │   reboot → v1.2.0,
   │                       │ mark leaf updated         │   mark_app_valid
   │                       │ (repeat ④–⑦ per leaf,    │
   │                       │  leaves before self)     │
   │                       │                          │
   │                  ⑧    │ all leaves done → DONE   │
   │                       │ relay self-updates,      │
   │                       │ reboots → v1.2.0         │
   │                       │                          │
   ⑨ both report new sw_version in HELLO; campaign converges
      (any node still behind is caught by version-comparison gossip,
       Section 14.1a — no central roster)
```

```
The same campaign for an ISOLATED relay swaps step ② for a mule:

 Gateway ──► Mule (loads image v1.2.0 + announce into SD on a gateway visit)
                │
                │ sweeps to isolated cluster (Mechanism B, 14.7)
                ▼
            Isolated Relay ── streams to its leaves (Mechanism C, identical
                              to steps ④–⑦ above)
                │
            Mule carries leaf/relay OTA_ACKs + new sw_versions back to the
            gateway on its return sweep (campaign telemetry, 14.7a)
```

```
Three phases, three layers:

  PHASE 1  announce        Gateway → relay     L4 bundle (control)
  PHASE 2  acquire image   stream or mule      L2 frames / mule carry (bulk)
  PHASE 3  push to leaves  relay → each leaf   L2 frames (bulk, on wake)

  Authenticity throughout: the Ed25519 image signature, verified by EVERY
  node (relay and each leaf) before flashing. The announce carries the
  version + hash; the signature is the end-to-end trust anchor.
```

The sections that follow detail each piece: design principles and why firmware
is streamed (14.1–14.1e), flash layout (14.2), HELLO fields (14.3), the announce
bundle (14.4), the session frame formats (14.4a), the relay state machine
(14.5), the three mechanisms (14.6–14.8), update ordering (14.9), security
(14.10), failure handling (14.11), and the attack surface (14.13).

---

### 14.1 Design Principles

OTA in Rimba must work across four node classes with very different constraints:

```
Node               Awake?     Reachable?         Key constraint
──────────────────────────────────────────────────────────────────────
Connected relay    Always     Direct backbone     None — streaming works
Isolated relay     Always     Mule only           Image carried physically
Continuous leaf    Always     Via relay           None — streaming works
Scheduled leaf     230ms/15m  Via relay           Wake window too short
                                                  for direct stream
```

The fundamental rule for Scheduled leaves:

> **A relay MUST have the complete, verified firmware image staged locally
> before signalling any leaf that an update is available. Partial transfers
> to leaves are not permitted.**

This separates delivery into two independent phases:
- **Phase 1**: Relay acquires complete image (may take minutes to days, no leaf involvement)
- **Phase 2**: Relay pushes complete image to each leaf during an extended wake window (local, guaranteed, no gateway dependency)

---

### 14.1a Why Firmware Is Streamed, Not Bundled

A reasonable question is why firmware is not delivered as a normal BPSec bundle
like all other data. The answer is a hard size constraint plus a set of
practical mismatches.

```
Bundle store:     256 KB (LittleFS, Section 9.6)
Firmware image:   1 MB (leaf) to 2 MB (relay)

A firmware image is 4–8× LARGER than the entire bundle store.
A BPSec bundle is an atomic unit — encrypted (BCB) and authenticated
as a whole — so it cannot be processed in pieces. A 2 MB atomic
bundle simply cannot fit a 256 KB store.
```

Bundle fragmentation (RFC 9171) could split the image, but it reintroduces
every cost of streaming plus BPSec overhead, and depends on a feature that is
explicitly out of scope (Open Issue #8):

```
Why fragmentation is not the answer:
  - Encrypt-then-fragment → receiver needs the whole 2 MB reassembled
    before it can decrypt/verify — no buffer for that.
  - Fragment-then-encrypt → each fragment becomes its own mini-bundle
    with its own BCB — that is just streaming chunks, but heavier.
  - Fragments still consume the 256 KB store one at a time → no gain.
  - Bundle reassembly fights the flash-write model: the leaf wants to
    write each chunk straight to its ota_1 partition as it arrives,
    never holding the whole image.
```

What streaming is, in effect, is fragmentation optimised for the one job that
matters — writing to flash incrementally without ever holding the whole image:

```
Streaming (current design):
  4 KB chunk arrives → AES-CCM decrypt (link key) → esp_ota_write()
  → discard chunk from RAM → next chunk
  Peak RAM: ~4 KB.  Peak storage: the purpose-built ota partition.
  Signature verified once, over the assembled flash image, at the end.
```

**Authenticity and confidentiality are not lost by skipping BPSec:**

```
Authenticity:   Ed25519 signature over the complete image (Section 14.10),
                verified by every node before flashing. Stronger than a
                generic BPSec BIB — it adds version-rollback protection
                and ties into ESP-IDF secure-boot eFuse.

Confidentiality: Firmware is PUBLIC to all nodes (every node runs the same
                image — nothing to hide from the nodes carrying it).
                Hop-by-hop AES-128-CCM already hides it from external RF
                observers in transit. BPSec end-to-end confidentiality
                would protect against a relay reading it — but the relay
                is going to run that exact firmware anyway.
```

```
The division of labour:
  BPSec bundles  → small, confidential, end-to-end payloads (sensor data)
  OTA streaming  → large, public, flash-destined payloads (firmware)

  The small announce bundle (~200 B) IS a normal bundle — it fits the
  store and bridges the two: a bundle that triggers a stream.
```

This mirrors the wider internet: DTN/BPv7 carries control and data messages,
while bulk transfer rides a separate optimised channel. In Rimba the announce
is the control bundle; the stream is the bulk channel.

**Version + hash, the same primitive as config.** OTA uses exactly the
version-and-hash pattern that config does (Section 9.9): `sw_version` /
`target_version` provides **ordering** (which firmware is newer, drives the
"who is behind" decision), and the announce's `payload_hash` (SHA-256) provides
**integrity** (the receiver verifies the assembled image against it before
flashing). The "who needs this" decision is the same decentralized local
comparison in both: any node — gateway, relay, or mule — that hears a peer
advertising a lower version than the known target acts on it (Section 14.7a.2),
with no central roster. The only difference from config is payload movement:
config pushes a ≤256 B bundle inline; OTA streams a 1–2 MB image (the rest of
this section). The image is additionally Ed25519-signed end-to-end — a
stronger authenticity guarantee than the hash alone, because firmware execution
is the highest-stakes action a node takes (Section 14.13).

### 14.1b OSI View — OTA Stream vs Normal Data Bundle

The two payload types live at different layers. A normal data bundle is a full
L4 BPv7 unit with end-to-end BPSec; an OTA chunk is an L2/application-framed
transfer protected only hop-by-hop, with authenticity supplied by the image
signature rather than a bundle security block.

```
                    Normal DATA bundle            OTA firmware chunk
────────────────────────────────────────────────────────────────────────────
L7 Application      sensor reading (CBOR)         firmware image (flash bytes)
                                                  — authenticity: Ed25519 sig
                                                    over WHOLE image (L7)

L5 Session/Security BPSec BIB + BCB               (none at bundle layer —
                    ChaCha20-Poly1305 E2E          no BPSec; image signature
                    key: ECIES or root_secret      provides end-to-end
                                                    authenticity instead)

L4 Transport        BPv7 bundle                   OTA session framing
                    store-carry-forward            OTA_START / CHUNK / EOF
                    EID addressing, lifetime       seq + 4 KB data + CRC-16
                                                  (NOT a BPv7 bundle)

L3 Network          Rimba mesh routing            Rimba mesh routing
                    (OGM / RREQ / geo)            (direct relay→leaf, 1 hop)

L2 Data Link        802.11ah IBSS                 802.11ah IBSS
                    AES-128-CCM (hop-by-hop)       AES-128-CCM (hop-by-hop)
                    ← protects in transit          ← protects in transit
                                                     (the ONLY encryption
                                                      the chunk gets)

L1 Physical         900 MHz HaLow radio           900 MHz HaLow radio
────────────────────────────────────────────────────────────────────────────

Key difference:
  Data bundle:  confidentiality + authenticity at L5 (BPSec), end-to-end.
                Relays cannot read it.
  OTA chunk:    authenticity at L7 (image signature), end-to-end.
                Confidentiality only at L2 (link), hop-by-hop.
                Relays CAN read it — and that is fine (public firmware).
```

```
On-the-wire framing comparison:

Normal DATA bundle (nested, E2E-encrypted payload):
  ┌─ 802.11ah frame · AES-128-CCM (per hop) ───────────────────┐
  │  ┌─ Rimba mesh header (src, dst, next_hop, TTL) ─────────┐  │
  │  │  ┌─ BPv7 bundle ──────────────────────────────────┐   │  │
  │  │  │  primary block (EIDs, lifetime)                │   │  │
  │  │  │  BIB (integrity)                               │   │  │
  │  │  │  BCB (confidentiality) ── ChaCha20-Poly1305    │   │  │
  │  │  │  payload (ciphertext) ← only endpoint decrypts │   │  │
  │  │  └────────────────────────────────────────────────┘   │  │
  │  └────────────────────────────────────────────────────────┘  │
  └────────────────────────────────────────────────────────────┘

OTA firmware chunk (flat, link-encrypted only):
  ┌─ 802.11ah frame · AES-128-CCM (per hop) ───────────────────┐
  │  ┌─ Rimba mesh header (src, dst=leaf, next_hop) ─────────┐  │
  │  │  OTA_CHUNK                                            │  │
  │  │    seq_num (4 B)                                      │  │
  │  │    data (4 KB of firmware image)  ← plaintext once    │  │
  │  │    crc16 (2 B)                       link-decrypted   │  │
  │  └────────────────────────────────────────────────────────┘  │
  └────────────────────────────────────────────────────────────┘
  (one chunk of many; the full image is Ed25519-signed as a whole)
```

### 14.1c OTA Streaming Sequence

The relay streams the staged image to one leaf at a time, over their existing
peer link, during the leaf's extended wake window. Each chunk is decrypted with
the ordinary relay↔leaf AES-128-CCM session key — there is no OTA-specific key.

```
Relay (READY, image staged)              Leaf (extended wake)
  │                                          │
  │◄──── HELLO reply {ota_ready=true} ───────│  leaf saw ota_pending,
  │                                          │  stays awake
  │                                          │
  │──── OTA_START ──────────────────────────►│  target_version, image_size,
  │     {ver, size, hash}                    │  image_hash
  │                                          │  leaf: esp_ota_begin(ota_1)
  │                                          │
  │──── OTA_CHUNK seq=0 ────────────────────►│  ┌─ per chunk: ────────────┐
  │◄─── ACK seq=0 ───────────────────────────│  │ AES-CCM decrypt (link)  │
  │──── OTA_CHUNK seq=1 ────────────────────►│  │ CRC-16 check            │
  │◄─── ACK seq=1 ───────────────────────────│  │ esp_ota_write(chunk)    │
  │──── OTA_CHUNK seq=2 ────────────────────►│  │ discard chunk from RAM  │
  │◄─── NACK seq=2 (bad CRC) ────────────────│  └─────────────────────────┘
  │──── OTA_CHUNK seq=2 (retransmit) ───────►│  bad chunk re-sent
  │◄─── ACK seq=2 ───────────────────────────│
  │            ... (≈54 s for 1 MB) ...       │
  │──── OTA_CHUNK seq=N ────────────────────►│
  │◄─── ACK seq=N ───────────────────────────│
  │──── OTA_EOF ────────────────────────────►│  leaf: esp_ota_end()
  │                                          │  ┌─ finalise: ─────────────┐
  │                                          │  │ SHA-256 == image_hash?  │
  │                                          │  │ Ed25519 signature valid?│
  │                                          │  └─────────────────────────┘
  │◄─── OTA_ACK {SUCCESS} ───────────────────│  if valid:
  │                                          │   esp_ota_set_boot_partition
  │  mark leaf updated                       │   reboot → new firmware
  │  (next leaf, or self-update if last)     │   mark_app_valid (within 60 s)
  │                                          │
  │  if OTA_ACK {SIG_FAIL}:                   │  if invalid:
  │  log, report to gateway,                 │   discard ota_1, stay on ota_0
  │  leaf stays on old firmware              │   resume normal sleep
```

Peak leaf RAM during the whole transfer is one chunk (~4 KB); the image
accumulates in the `ota_1` flash partition, never in memory. The signature is
checked once, after `OTA_EOF`, over the fully assembled flash image.

---

### 14.1d Where OTA Packets Sit in the OSI Stack

OTA involves two kinds of packet at two different layers. The announce is a
BPv7 bundle (L4); the streaming frames are link-layer frames (L2). This table
is the definitive placement for every OTA-related packet:

```
Packet              Frame/Bundle   OSI layer   Encryption        Authenticity
─────────────────────────────────────────────────────────────────────────────
payload_announce    BPv7 bundle    L4 transport hop-by-hop AES   Ed25519 sig
  (ipn:0/<id>       (DATA frame      (+ L5 BPSec   -CCM per hop   over announce
   .payload_         0x8 carries     if to backend)              fields (L7)
   announce)         the bundle)

OTA_START   (0xF)   L2 link frame  L2 data link  hop-by-hop AES   (none at frame
OTA_CHUNK   (0x10)  L2 link frame  L2 data link  -CCM per hop;    layer; image
OTA_EOF     (0x11)  L2 link frame  L2 data link  point-to-point   Ed25519 sig
OTA_ACK     (0x12)  L2 link frame  L2 data link  relay↔receiver   verified over
OTA_NACK    (0x13)  L2 link frame  L2 data link                   whole image
                                                                   after EOF (L7)

sw_version /        carried IN     L2 (HELLO is  hop-by-hop AES   (HELLO itself
ota_pending /       HELLO (0x1)    L2, Section    -CCM per hop     is link-auth;
config fields                      7.1)                            announce/image
                                                                   carry the sigs)
```

```
Why the split:
  - The ANNOUNCE is small (~200 B), needs DTN store-carry-forward to
    reach sleeping leaves and isolated clusters, and benefits from
    routing/dedup → it is a BPv7 bundle (L4). It is the CONTROL message.
  - The IMAGE STREAM is large (1–2 MB) and must be written to flash
    incrementally → it is a sequence of L2 link frames (the BULK channel),
    NOT bundles (Section 14.1a explains why bundling fails at 256 KB store).
  - OTA state in HELLO (sw_version, ota_pending) is L2 metadata that drives
    the decentralized "who is behind" comparison (Section 14.1a).

Authenticity is uniform regardless of layer: the image's Ed25519 signature
is the end-to-end guarantee, checked by every node before flashing. The
layer a packet sits at affects how it is MOVED, not how it is TRUSTED.
```

### 14.1e The Three Delivery Mechanisms — Unified View

OTA reaches every node class through three mechanisms that compose. They differ
only in how the image gets to the relay/leaf; all use the same OTA_START/CHUNK/
EOF frames and the same signature verification.

```
                          ┌─────────────┐
                          │   Gateway   │  has signed image, is image source
                          └──────┬──────┘
            ┌────────────────────┼────────────────────┐
            │ Mechanism A        │ Mechanism B         │
            │ (connected relay)  │ (isolated relay,    │
            │                    │  via mule)          │
            ▼                    ▼                     │
     ┌─────────────┐      ┌─────────────┐             │
     │  Relay R1   │      │    Mule     │ carries     │
     │ (backbone)  │      │ image in SD │ image +     │
     └──────┬──────┘      └──────┬──────┘ announce    │
            │ stream             │ sweeps to          │
            │ (A)                │ isolated cluster   │
            │                    ▼                    │
            │             ┌─────────────┐             │
            │             │  Relay R7   │ receives    │
            │             │ (isolated)  │ stream from │
            │             └──────┬──────┘ mule (B)    │
            │                    │                     │
            │   Mechanism C      │   Mechanism C       │
            │   (relay → leaf)   │   (relay → leaf)    │
            ▼                    ▼                     │
     ┌─────────────┐      ┌─────────────┐             │
     │  Leaf L1    │      │  Leaf L42   │             │
     │ (on wake)   │      │ (on wake)   │             │
     └─────────────┘      └─────────────┘             │
                                                       │
  Mechanism A:  Gateway ──stream──► directly-adjacent relay
  Mechanism A2: Relay ──stream──► downstream relay (backbone ripple,
                multi-hop propagation; each relay verifies then re-serves)
  Mechanism B:  Gateway ─► Mule ──carry+stream──► isolated relay
  Mechanism C:  Relay ──stream──► its leaves on their wake windows

  All four: OTA_START → OTA_CHUNK×N (+OTA_NACK/retransmit) → OTA_EOF
            → receiver verifies SHA-256 + Ed25519 → flash → OTA_ACK
```

```
Which mechanism applies, per node:

  Relay adjacent to gateway             → Mechanism A  (gateway streams)
  Relay reachable via other relays      → Mechanism A2 (upstream relay
                                           re-streams; ripples hop by hop)
  Relay with NO backbone path           → Mechanism B  (mule carries+streams)
  Any leaf                              → Mechanism C  (its relay streams,
                                           regardless of how the relay got
                                           the image — A, A2, or B)

The relay is always the one that streams to a leaf (Mechanism C). The only
question is how the RELAY obtained the image: from the gateway directly (A),
from an upstream relay (A2), or via mule (B). This is why a relay must hold
the complete verified image before signalling ota_pending — the leaf update
path is identical regardless of source.
```

---

### 14.2 Flash Partition Layout

All relay and mule nodes require a dedicated `ota_stage` partition. Leaves only need standard OTA partitions.

```
Relay / Mule flash layout (8 MB internal flash, default):

  0x000000  factory      256 KB   Fallback image (never overwritten)
  0x040000  ota_0          2 MB   Active running partition
  0x240000  ota_1          2 MB   OTA update target
  0x440000  ota_data       8 KB   Active partition selector
  0x442000  ota_stage      2 MB   Firmware staging area
                                  (receive from gateway/mule,
                                   push to leaves from here)
  0x642000  nvs           64 KB   Config, peer keys
  0x652000  rimba_store  256 KB   DTN bundle store (LittleFS)

Leaf flash layout (4 MB minimum):

  0x000000  factory      256 KB   Fallback image
  0x040000  ota_0          1 MB   Active partition
  0x140000  ota_1          1 MB   OTA update target
  0x240000  ota_data       8 KB   Active partition selector
  0x242000  nvs           64 KB   Config, peer keys
  (no ota_stage — leaves do not stage firmware for others)
```

#### 14.2.1 External Storage for ota_stage and Large Payloads

The `ota_stage` partition does not need to reside on internal flash. It can be
placed on external SPI/QSPI flash or an SD card. This is particularly relevant
for mule nodes, which may carry firmware images for many relays simultaneously,
and for any node where 8 MB internal flash is insufficient.

**Supported external storage options:**

| Storage type | Interface | Typical capacity | Typical speed | Suitable for |
|---|---|---|---|---|
| Internal flash | — | 4–16 MB | Up to 80 MB/s (QSPI) | Relays (default) |
| External SPI NOR flash | SPI / QSPI | 8–128 MB | 5–40 MB/s | Relays, gateways |
| SD card (SPI mode) | SPI | 4 GB–128 GB | ~4 MB/s | Mules, gateways |
| SD card (SDIO 4-bit) | SDIO | 4 GB–128 GB | ~10 MB/s | Mules |

ESP-IDF exposes all of these through its Virtual File System (VFS) and `spi_flash`
APIs. The `ota_stage` implementation addresses the staging area by path
(`/ota_stage/` on SD/external, or a dedicated partition on internal), making
the underlying storage transparent to the OTA protocol logic.

**Relay with external SPI flash:**

```
Common choice: W25Q128 (16 MB SPI NOR flash, ~$0.50/unit)
  Connected via HSPI at 40 MHz
  Provides 16 MB for ota_stage, leaving all internal flash free
  Mounted as /ext_flash/
  ota_stage path: /ext_flash/ota_stage.bin

Benefits:
  Leaves full 8 MB internal flash for firmware + bundle store growth
  Multiple firmware variants can be cached simultaneously
  Negligible cost addition per node
```

**Mule with SD card:**

```
Common choice: microSD via SPI (any class 10 or UHS-I card)
  ESP-IDF: esp_vfs_fat_sdmmc_mount() → FAT32 filesystem
  ota_stage path: /sd/ota/<node_id>/<version>.bin
  
Benefits:
  Mule can carry firmware images for hundreds of relay types
  Image library organised by node_id and version
  SD card also stores mule telemetry logs and route data
  Easily swapped for field updates to the mule's image library
  Capacity: 4 GB+ → no practical limit on simultaneous images
```

**General principle — large payload storage:**

The 256 KB DTN bundle store is sized for sensor data bundles (~300 bytes each).
Any payload larger than ~50 KB should be considered a "large payload" and handled
via external storage rather than the bundle store. This includes:

```
Payload type               Recommended storage   Size range
──────────────────────────────────────────────────────────────
Firmware images (OTA)      ota_stage (ext.)      1–4 MB
Large config datasets      /ext/config/          10 KB–1 MB
Geographic map data        /ext/maps/            100 KB–10 MB
  (relay position tables,
   routing hints for GPS)
Diagnostic logs            /ext/logs/            unbounded
Sensor data backlog        /ext/data/            unbounded
  (on mule or gateway)
```

Large payloads travel to nodes via the mule custody session or a direct gateway
streaming session — the same two mechanisms as OTA firmware. They do NOT pass
through the 256 KB DTN bundle store. The OTA_ANNOUNCE bundle pattern (Section 14.4)
can be generalised: a small announcement bundle (fits in DTN store) notifies nodes
that a large payload is available, and the actual payload is streamed separately.

---

### 14.3 HELLO Fields

Two new optional HELLO fields support OTA:

```
"sw_version":  <uint32>   Current firmware version, packed semver:
                          bits 31-22: major (10 bits)
                          bits 21-11: minor (11 bits)
                          bits 10-0:  patch (11 bits)
                          All nodes, always present.
                          Any node detects an out-of-date peer by
                          comparing sw_version in HELLO (same local
                          version-comparison rule as config, Section 9.9)
                          — a peer below the known target_version needs
                          the update. The gateway is the usual source of
                          new images, but the "who is behind" decision is
                          local to whichever node hears the HELLO, not a
                          central roster (a relay or mule applies the same
                          comparison — Section 14.7a.2).

"ota_pending": <bool>     Relay/mule only. True when ota_stage holds
                          a complete, verified image at a higher version
                          than current. Leaf MUST NOT sleep on a wake
                          cycle where relay HELLO has ota_pending=true.

"ota_ready":   <bool>     Leaf only. Sent in leaf HELLO reply when
                          leaf has seen ota_pending=true and is staying
                          awake for the transfer.

"ota_error":   <uint8>    Optional. Last OTA failure code. Present only
                          when reporting a failure to the gateway.
                          0x01=SIG_FAIL 0x02=FLASH_ERR 0x03=VERSION_ERR
```

---

### 14.4 Payload Announce Bundle

A small announcement bundle carries payload metadata. All nodes receive and
store it via normal DTN delivery. This is the only large-payload element that
travels through the 256 KB DTN bundle store.

**Targeting scope** is determined by the bundle destination EID:

```
Broadcast (fleet-wide):
  dst EID: ipn:0.payload_announce
  Routed to all nodes via normal DTN. Each node evaluates
  filters (target_version, min_hw_rev, rollout_pct) independently.

Unicast (single node):
  dst EID: ipn:<target_node_id>.payload_announce
  Routed to one specific node via RREQ/RREP.
  If target is a leaf: leaf's relay responds with proxy RREP
  (Section 8.3) and delivers on next leaf wake.
  No other node receives or stores this announcement.
```

The streaming session that follows is always peer-to-peer regardless of
whether the announcement was broadcast or unicast. A broadcast announcement
triggers individual streaming sessions — there is no multicast data transfer.

```
Bundle EID:   ipn:0.payload_announce              (broadcast)
              ipn:<target_node_id>.payload_announce (unicast)
Priority:     NORMAL
Lifetime:     604,800s (7 days)
Max size:     ~200 bytes (fits in 256 KB bundle store)

Payload (CBOR):
{
  "payload_type":    <uint8: 0=firmware, 1=config, 2=map_data,
                              3=diagnostic, 4=route_schedule>,
  "target_role":     <uint8: bitmask of roles that should act on this
                              bit 0 = gateway  (0x01)
                              bit 1 = relay    (0x02)
                              bit 2 = leaf     (0x04)
                              bit 3 = mule     (0x08)
                              absent or 0xFF   = all roles>,
  "target_version":  <uint32: packed semver — firmware only>,
  "payload_size":    <uint32: bytes>,
  "payload_hash":    <bytes[32]: SHA-256 of full payload>,
  "sig":             <bytes[64]: Ed25519 over all preceding fields>,
  "min_hw_rev":      <uint8: minimum hardware revision — optional>,
  "rollout_pct":     <uint8: 0-100, staged rollout — optional>
}
```

On receiving an announce bundle, a node acts if ALL of the following are true:
- Own role bit is set in `target_role` (if absent → all roles act)
- `payload_type` is applicable to this node role
- `target_version` > `sw_version` (firmware only)
- `min_hw_rev` ≤ own hardware revision (if present)
- `(node_id % 100) < rollout_pct` (if present; absent = 100%)

A node that fails the `target_role` check silently discards the bundle
after delivery — it does not re-flood it, does not log an error, and
does not signal any OTA activity.

For relay/mule nodes: prepare ota_stage, request payload from gateway or
wait for mule delivery. Set `ota_pending=true` in HELLO only after full
payload is staged and verified (Section 14.8).

**Targeting summary:**

| Scope | EID | `target_role` | Use case |
|---|---|---|---|
| All nodes | `ipn:0.payload_announce` | absent / 0xFF | Fleet firmware update |
| Relays only | `ipn:0.payload_announce` | 0x02 | Relay-only firmware |
| Leaves only | `ipn:0.payload_announce` | 0x04 | Leaf firmware |
| Relay + Mule | `ipn:0.payload_announce` | 0x0A | Infrastructure firmware |
| Gateway only | `ipn:0.payload_announce` | 0x01 | Gateway firmware |
| By hardware revision | `ipn:0.payload_announce` | + `min_hw_rev` | Hardware-specific update |
| Percentage rollout | `ipn:0.payload_announce` | + `rollout_pct` | Staged rollout |
| Single relay | `ipn:<relay_id>.payload_announce` | 0x02 | Node-specific config |
| Single leaf | `ipn:<leaf_id>.payload_announce` | 0x04 | Leaf threshold change |
| Single mule | `ipn:<mule_id>.payload_announce` | 0x08 | Mule route schedule |

**Role-based OTA flow differences:**

When `target_role` excludes leaves (e.g. relay-only firmware, `target_role=0x02`):
- Leaves receive the broadcast bundle but discard it after the role check
- Relay does NOT set `ota_pending=true` in HELLO (no leaf update needed)
- Relay skips the "leaves first" ordering (Section 14.9) — no leaves to update
- Relay drains its forwarding queue and updates itself directly
- This is the simplest OTA path: announce → stage → verify → self-update

---

### 14.4a OTA Session Frame Formats

The announce (14.4) is a BPv7 bundle. The actual image transfer uses five L2
link frames (types `0xF`–`0x13`, Section 6.3). Their payloads are defined here.
All are carried in the standard 802.11ah IBSS frame with the Rimba mesh header
and hop-by-hop AES-128-CCM, point-to-point between sender (gateway/relay/mule)
and one receiver.

```
OTA_START (0xF) — sender → receiver, opens a transfer session:
{
  "session_id":     <uint32: identifies this transfer; echoed in all
                             subsequent frames of the session>,
  "target_version": <uint32: packed semver of the image being sent>,
  "payload_type":   <uint8: 0=firmware, 1=config, ... (matches announce)>,
  "image_size":     <uint32: total bytes to be transferred>,
  "chunk_size":     <uint16: bytes per chunk, default 4096>,
  "total_chunks":   <uint32: ceil(image_size / chunk_size)>,
  "image_hash":     <bytes[32]: SHA-256 of the complete image — receiver
                             verifies the assembled image against this>
}
```

```
OTA_CHUNK (0x10) — sender → receiver, one fragment:
{
  "session_id":     <uint32: must match the open session>,
  "seq_num":        <uint32: 0-based chunk index>,
  "data":           <bytes: up to chunk_size bytes of image>,
  "crc16":          <uint16: CRC-16/CCITT over data — accidental-corruption
                             check; the image-wide SHA-256 + Ed25519 are the
                             real integrity/authenticity guarantees>
}
```

```
OTA_EOF (0x11) — sender → receiver, ends the transfer:
{
  "session_id":     <uint32: must match the open session>,
  "total_sent":     <uint32: chunk count the sender transmitted —
                             receiver cross-checks against total_chunks>
}
```

```
OTA_ACK (0x12) — receiver → sender:
{
  "session_id":     <uint32>,
  "ack_type":       <uint8: 0=CHUNK_ACK (per-chunk),
                            1=SESSION_RESULT (after EOF)>,
  "seq_num":        <uint32: chunk acknowledged — for ack_type=0>,
  "result":         <uint8: for ack_type=1 —
                            0=SUCCESS, 1=SIG_FAIL, 2=HASH_FAIL,
                            3=FLASH_ERR, 4=VERSION_FAIL, 5=ROLLBACK>
}
```

```
OTA_NACK (0x13) — receiver → sender, request retransmit:
{
  "session_id":     <uint32>,
  "seq_num":        <uint32: chunk to resend (bad CRC or never received)>,
  "reason":         <uint8: 0=bad_crc, 1=missing_seq, 2=out_of_order>
}
```

```
Field notes:
  - session_id ties a transfer together and lets a receiver reject stray
    frames from an aborted/old session (replay/confusion guard).
  - seq_num gives explicit ordering and is the unit of retransmit — a
    receiver NACKs a specific seq_num, the sender resends just that chunk.
  - crc16 is per-chunk and catches accidental corruption cheaply; it is
    NOT a security control (trivially forgeable). The image-wide SHA-256
    (checked against OTA_START.image_hash) and the Ed25519 signature
    (Section 14.10) are the integrity and authenticity guarantees, both
    verified after OTA_EOF before flashing.
  - These frames carry no BPSec — they are not bundles. Confidentiality in
    transit is the hop-by-hop AES-CCM link key; authenticity is the image
    signature (Section 14.1d).
```

---

### 14.5 Relay OTA State Machine

```
  IDLE ──────────────────────────────────────────────────────┐
    │ first chunk received                                     │
    ▼                                                          │
  RECEIVING ──── flash error ────────────────────────► FAILED │
    │ all bytes written                                        │
    ▼                                                          │
  VERIFYING ──── sig invalid / version fail ───────► FAILED ──┤
    │ sig valid, version ok                                    │
    ▼                                                          │
  READY (ota_pending=true in HELLO)                           │
    │ leaf wakes, sees ota_pending                            │
    ▼                                                          │
  PUSHING ──── flash error ────────────────────────► FAILED ──┤
    │ leaf ACK received; more leaves remain                    │
    │ (loop back to READY)                                     │
    │ last leaf ACK received                                   │
    ▼                                                          │
  DONE (ota_pending=false; queue self-update)                 │
    │ self-update + restart complete                          │
    └──────────────────────────────────────────────── IDLE ◄──┘
```

---

### 14.6 Mechanism A — Streaming to Connected Relay

```
① Gateway sees relay R sw_version < target_version in HELLO
② Gateway sends OTA_ANNOUNCE bundle via normal routing
③ Gateway opens OTA streaming session with R:
     R: esp_ota_begin() targeting ota_stage partition
     Gateway streams image in 4 KB chunks
     Each chunk: seq_num + data + CRC-16
     R: writes each chunk via esp_ota_write()
     R: sends OTA_NACK on bad CRC → gateway retransmits chunk
④ Gateway sends OTA_EOF marker
⑤ R: esp_ota_end() finalises ota_stage write
⑥ R verifies Ed25519 signature over full ota_stage content
   Signature valid AND target_version > sw_version:
     → READY state, ota_pending=true in HELLO
   Otherwise: FAILED, notify gateway via HELLO ota_error field
⑦ Leaves update (Mechanism C)
⑧ All leaves updated → relay self-update:
     Drain forwarding queue
     Copy ota_stage → ota_1
     esp_ota_set_boot_partition(ota_1)
     Announce imminent restart in HELLO
     Restart → boot from ota_1
     If boot succeeds within 60s:
       esp_ota_mark_app_valid_cancel_rollback()
     If not: auto-rollback to ota_0
```

---

### 14.6a Mechanism A2 — Relay-to-Relay Backbone Propagation

Mechanism A describes the gateway streaming to a *directly adjacent* relay. But
a relay several hops away across other relays cannot be streamed to directly —
the OTA frames (`OTA_START/CHUNK/EOF`) are L2 point-to-point between two
peer-linked nodes; they do not route multi-hop. A relay 4 hops out shares no
radio link with the gateway, so the gateway cannot open a streaming session
with it.

The image therefore propagates **store-and-forward along the backbone**: each
relay receives the complete image, verifies it, then becomes a source for its
downstream neighbours. Mechanism A is really the special case where the source
is the gateway; the general rule is "any relay holding the verified image is a
source."

```
Image rippling outward along the backbone:

  Gateway ──stream──► R1 ──stream──► R2 ──stream──► R3 ──stream──► R4
   (source)          stage         stage          stage          stage
                     verify        verify         verify         verify
                     re-serve ►    re-serve ►     re-serve ►     push to
                                                                 its leaves
  Each relay: receive complete image → VERIFY sig → become a source →
              re-stream to downstream neighbours → push to its own leaves
```

```
The recursive rule (a relay is a source once it holds the verified image):

  Relay R (ota_stage holds verified image v1.2.0) hears neighbour R'
  advertising sw_version < v1.2.0 in HELLO:

  ① R sees R' is behind — same version-comparison gossip as Section 14.1a
  ② R' has (or is sent) the payload_announce
  ③ R opens a streaming session with R' — R is now the source:
       R ──OTA_START──► R'     R': esp_ota_begin(), enters RECEIVING
       R ──OTA_CHUNK×N─► R'     R': esp_ota_write to ota_stage
                                R': OTA_NACK on bad CRC → R retransmits
       R ──OTA_EOF─────► R'
  ④ R': esp_ota_end(); verify SHA-256 == image_hash AND Ed25519 sig
  ⑤ R' → R: OTA_ACK{SUCCESS}; R' enters READY
  ⑥ R' is now ALSO a source: it re-streams to ITS downstream neighbours
     and pushes to its own leaves (Mechanism C)
```

**Mandatory: verify before re-serving.** A relay MUST verify the Ed25519
signature over the complete assembled image before it becomes a source for any
downstream relay. This guarantees a corrupt or tampered image stops at the first
relay that receives it and is never re-streamed onward:

```
If R' receives a bad image (corruption, or a malicious upstream relay
substituting content):
  R' verifies → SHA-256 or Ed25519 fails → R' discards ota_stage,
  stays on its current version, reports ota_error → does NOT become a
  source → the bad image propagates NO further.

Because every relay verifies independently, the signature is checked
at every backbone hop, not just once at the gateway. A compromised
relay in the middle of the chain cannot inject altered firmware to
relays downstream of it (it cannot forge the signature — Section 14.13
Vector 4).
```

```
Properties:
  - Same frames as Mechanism A (OTA_START/CHUNK/EOF) — no new wire format.
  - Same "who is behind" trigger as config convergence (14.1a): a relay
    streams to any neighbour advertising a lower sw_version.
  - No "gateway reach" requirement — a relay N hops out gets the image
    by ripple, hop by hop, as long as a backbone path of relays exists.
  - Ordering within a relay is unchanged: a relay re-serves to downstream
    relays and pushes to its leaves; it self-updates LAST (Section 14.9),
    after its leaves AND after it has finished serving as a source for
    downstream neighbours (so it doesn't reboot mid-stream and orphan a
    downstream relay's transfer).
  - A relay with no backbone path to any up-to-date relay falls back to
    Mechanism B (mule-carried).
```

This is why an OTA campaign reaches the entire connected backbone from a single
gateway origin: Mechanism A seeds the first relay, Mechanism A2 ripples it
across all reachable relays, and Mechanism C delivers from each relay to its
leaves. Isolated relays (no backbone path) are reached by Mechanism B.

---

### 14.7 Mechanism B — Mule-Carried Firmware (Isolated Relay)

```
① Gateway prepares signed firmware image for isolated relay R7
② Mule collects firmware from gateway during custody session:
     Firmware stored in mule's own flash (not ota_stage —
     mule carries images for multiple relays simultaneously)
     OTA_ANNOUNCE bundle also collected
③ Mule travels to R7, initiates direct OTA delivery session:
     Identifies correct image for R7 by node_id / hw_rev
     Streams image to R7's ota_stage (direct, not via DTN)
     Same 4 KB chunk + CRC-16 mechanism as Mechanism A
④ R7 verifies signature → READY → ota_pending=true
⑤ Leaves update via Mechanism C (mule may park and wait,
     or depart and collect ACKs on next sweep)
⑥ R7 self-update completes → bundles confirmation for mule
     or gateway delivery
```

---

### 14.7a Mule Participation in OTA Campaigns

Mechanism B describes a mule reactively carrying one image to one isolated
relay. In practice a mule is an **active campaign participant** — often the
only way an OTA campaign reaches clusters with no backbone path to a gateway.
A mule contributes in four distinct roles:

```
Role 1 — Image courier
  Carries the firmware image library (in its large SD/flash store) to
  isolated clusters the backbone cannot reach. Without the mule, an
  isolated relay stays on old firmware indefinitely.

Role 2 — Announce-bundle carrier
  Carries the small payload_announce bundle (~200 B, normal DTN bundle)
  into isolated clusters so their nodes learn an update exists, what
  version it is, and its hash — the same way it carries any bundle.

Role 3 — Campaign progress reporter
  Collects each visited node's current sw_version and any pending
  OTA_ACK results, and carries them back to the gateway. For isolated
  clusters this is the ONLY channel for campaign telemetry — the
  gateway's view of those regions is built entirely from mule reports.

Role 4 — Campaign target
  A mule runs firmware too (relay superset). target_role bit 3 (0x08)
  targets mules; 0x0A targets relay + mule (the infrastructure tier).
  The mule updates itself last — see self-update timing below.
```

#### 14.7a.1 Mule Image Library

The mule stores firmware images on external storage (SD card, Section 14.2.1),
not in `ota_stage` (which holds a single image being delivered). The library
is versioned, signature-validated, and refreshed at each gateway visit:

```
SD card layout:  /sd/ota/<target_role>/<version>.bin  +  .sig  +  .meta

At each gateway visit, the mule:
  1. Pulls any NEWER images the gateway advertises (higher target_version)
  2. Verifies each image's Ed25519 signature BEFORE storing
     (never propagate a corrupt or unsigned image across the network)
  3. Drops superseded images (a version now lower than the campaign's
     current target_version — avoids delivering stale firmware)
  4. Records the campaign's current target_version per target_role

Why validate at load time, not just at delivery:
  A mule that carried a corrupt image would waste an entire sweep
  delivering garbage to isolated clusters. Validate once, at the
  gateway, where a re-fetch is cheap.
```

A mule may carry images for multiple node classes simultaneously (a mixed
isolated cluster needs relay, leaf, and possibly mule images):

```
Example library on a mule serving mixed clusters:
  /sd/ota/relay/1.2.0.bin   (2 MB, target_role bit 1)
  /sd/ota/leaf/1.2.0.bin    (1 MB, target_role bit 2)
  /sd/ota/mule/1.2.0.bin    (2 MB, target_role bit 3)
  Total ~5 MB — trivial for an SD card.
```

#### 14.7a.2 Autonomous Delivery Decision

When a mule meets a node during its sweep, it decides independently whether to
deliver — no gateway round-trip needed:

```
Mule arrives at node X, reads X's HELLO {sw_version, role, hw_rev}:

① Search image library for a candidate where ALL hold:
     target_version > X.sw_version        (X is behind)
     target_role bit matches X.role        (image is for this class)
     min_hw_rev <= X.hw_rev                (hardware compatible)
② If a candidate exists AND X is eligible
   ((node_id % 100) < rollout_pct, not opted out):
     deliver payload_announce (if X hasn't seen it)
     stream image into X's ota_stage (relay/mule target)
       OR hand to X's relay for leaf targets (Section 14.8)
③ Record delivery: {node_id, version, timestamp}
④ Collect from X: pending OTA_ACKs, and the sw_version of X's
   leaves (campaign telemetry, Role 3)
⑤ Move to next node
```

#### 14.7a.3 Campaign Sweep Diagram

```
                 ┌─ Connected cluster (backbone to GW) ─┐
                 │                                       │
   ┌─────────┐   │   ┌─────┐      ┌─────┐      ┌─────┐   │
   │ Gateway │───┼──►│ R1  │─────►│ R2  │─────►│ R3  │   │
   └────┬────┘   │   └─────┘      └─────┘      └─────┘   │
        │        │   (these update over the backbone —   │
        │        │    Mechanism A, no mule needed)        │
        │        └───────────────────────────────────────┘
        │
        │ ① Mule visits gateway:
        │   - loads/refreshes image library (validate sigs)
        │   - picks up payload_announce bundle
        │   - drops off telemetry from LAST sweep
        ▼
   ┌─────────┐
   │  Mule   │  ② sweeps toward isolated cluster ───────────┐
   └─────────┘                                              │
                                                            ▼
                 ┌─ Isolated cluster (NO backbone path) ─────────┐
                 │                                               │
                 │   ┌─────┐      ┌─────┐      ┌─────┐           │
                 │   │ R7  │      │ R8  │      │ L42 │           │
                 │   └─────┘      └─────┘      └─────┘           │
                 │      ▲            ▲            ▲              │
                 │      │            │            │              │
                 │   ③ deliver    ③ deliver   ③ via R8's        │
                 │   announce +   announce +   relay staging    │
                 │   stream image stream image (Mechanism C)    │
                 │      │            │            │              │
                 │   ④ collect    ④ collect   ④ collect          │
                 │   sw_version,  ACKs         L42 sw_version    │
                 │   ACKs                                         │
                 └───────────────────────────────────────────────┘
                            │
                            │ ⑤ Mule returns to gateway next sweep,
                            ▼   delivers telemetry: who updated, who didn't
                   ┌─────────────────────────┐
                   │ Gateway campaign view:  │
                   │  R1,R2,R3 ✓ (backbone)  │
                   │  R7,R8 ✓ (via mule)     │
                   │  L42 ✓ (via mule→R8)    │
                   └─────────────────────────┘
```

#### 14.7a.4 Mule Self-Update Timing

A mule that is a campaign target must **not** update itself mid-sweep:

```
Why not mid-sweep:
  A mule mid-sweep is carrying firmware images AND custody bundles
  (DTN data destined for the gateway). A reboot to flash new firmware
  risks an interrupted update that could lose carried custody bundles
  or leave the mule in a bad state far from any gateway.

Rule: a mule updates itself only at a gateway, after handing off
  everything it carries — extending the "leaves before relay"
  ordering principle (Section 14.9) to the mule tier:

  ① Mule completes its sweep, returns to gateway
  ② Mule delivers all carried custody bundles to the gateway
  ③ Mule delivers all campaign telemetry
  ④ Mule receives its own image over the backbone (gateway streams
     to mule's ota_stage — it is in range and connected)
  ⑤ Mule verifies signature → self-updates → reboots
  ⑥ Mule re-validates its image library after reboot, then resumes
     sweeping with the new firmware

This guarantees a mule never reboots while it is the sole holder of
in-transit data.
```

#### 14.7a.5 Stale Image Handling

A sweep can take hours or days; the campaign may advance during that time:

```
If the campaign target advances (v1.2.0 → v1.2.1) while the mule is
mid-sweep carrying v1.2.0:

  - The mule continues delivering v1.2.0 to nodes still on older
    versions — v1.2.0 is still an improvement over what they have,
    and the announce bundle's signature is still valid.
  - On its next gateway visit, the mule refreshes to v1.2.1 and
    drops v1.2.0 from its library.
  - Nodes that received v1.2.0 will be caught by the next sweep
    carrying v1.2.1 (target_version > their now-current v1.2.0).

A mule never delivers an image OLDER than a node's current version
(the target_version > sw_version check, 14.7a.2), so stale-image
delivery can only ever move a node forward, never backward.
```

---

### 14.8 Mechanism C — Scheduled Leaf Update via Relay Staging

**Precondition**: Relay MUST be in READY state before ota_pending=true
appears in any HELLO. The relay's ota_stage contains a complete, signature-
verified image. No partial transfers to leaves are permitted.

**Anti-abuse precondition — verify before committing to the wake.** Extending
a leaf's wake window for ~54 seconds is expensive (≈3 weeks of normal battery).
A malicious relay that falsely advertises `ota_pending=true` could drain a
leaf's battery by making it wait for an update that never properly arrives
(see Section 14.13, Vector 2). To close this, the leaf performs cheap checks
*before* committing to the long transfer:

```
Before a leaf extends its wake window for OTA, it requires:

  1. Authenticated relay:
     ota_pending is honoured ONLY from a relay with which the leaf has
     an established, authenticated peer link (AES-128-CCM session key,
     Section 7.4). An unauthenticated or unknown node's ota_pending is
     ignored — the leaf does not stay awake.

  2. Signed announce first:
     The leaf requires a valid payload_announce (Ed25519 signature over
     {target_version, size, hash}, Section 14.4) BEFORE entering extended
     wake. Verifying the announce signature is cheap (one Ed25519 verify,
     ~ms) and happens within the normal wake window. A forged ota_pending
     with no validly-signed announce → leaf refuses to stay awake.

  3. Version check:
     target_version > current sw_version. A replayed old announce is
     rejected before any wake commitment.

Only after all three pass does the leaf enter extended wake (step ③).
This moves a cheap verification BEFORE the expensive commitment, instead
of only verifying the image at the end.
```

```
① Leaf L wakes on normal RTC schedule
② Leaf receives relay HELLO during scan window
   HELLO contains ota_pending=true
   ── Leaf verifies: authenticated relay? signed announce? version newer? ──
   ── If any check fails → ignore, return to normal sleep (no wake cost) ──
③ Leaf does NOT return to deep sleep (only after checks pass)
   Leaf sends HELLO reply with ota_ready=true
④ Relay sends OTA_START {
     "target_version": <uint32>,
     "image_size":     <uint32>,
     "image_hash":     <bytes[32]>   ← leaf verifies at end
   }
⑤ Leaf: esp_ota_begin() targeting ota_1 partition
⑥ Relay streams from ota_stage → leaf in 4 KB chunks
   Fully local — no gateway or mule dependency
   Leaf sends OTA_NACK on bad CRC → relay retransmits chunk
   ── Transfer attempt is bounded (see wake-budget cap below) ──
⑦ All chunks received: leaf calls esp_ota_end()
⑧ Leaf verifies SHA-256 (against image_hash from OTA_START)
   Leaf verifies Ed25519 signature
   If valid:
     esp_ota_set_boot_partition(ota_1)
     Leaf sends OTA_ACK{result=SUCCESS}
     Leaf restarts immediately
     On boot: esp_ota_mark_app_valid_cancel_rollback()
     Next HELLO: updated sw_version
   If invalid:
     Leaf sends OTA_ACK{result=SIG_FAIL}
     ota_1 discarded, leaf stays on ota_0
     Returns to normal sleep schedule
⑨ Relay receives OTA_ACK:
     SUCCESS: mark leaf updated, continue with remaining leaves
     FAIL: report to gateway, continue with remaining leaves
     When all leaves done: DONE state → self-update
```

**Wake-budget cap (anti-drain):** a leaf bounds how many times it will enter
extended OTA wake without a successful transfer:

```
OTA_WAKE_ATTEMPT_MAX = 2   (failed/incomplete transfers before backoff)

After OTA_WAKE_ATTEMPT_MAX failed transfers for the same target_version:
  - Leaf stops honouring ota_pending for OTA_BACKOFF_CYCLES (default 20
    wake cycles ≈ 5 hours at 15-min interval)
  - Leaf sets ota_error in its next HELLO so the relay/gateway sees the
    anomaly (campaign telemetry surfaces a leaf that cannot complete)
  - Protects against a relay that repeatedly burns the leaf's wake
    window with transfers that never finish
```

**Battery cost (1 MB leaf image, ~54 seconds):**

```
OTA session:   54s × 20mA  = 1,080 mAs = 0.30 mAh
Normal session: 0.23s × 20mA = 4.6 mAhs × 10⁻³

OTA = ~65 normal sessions ≈ 3 weeks of normal operation.
On 3,000 mAh battery: 0.01% — acceptable once or twice per year.
A successful update costs this ONCE. The wake-budget cap ensures a
malicious relay cannot inflict it repeatedly.
```

---

### 14.9 Update Ordering (Leaves Before Relay)

```
Why leaves first:
  If relay updates itself first and auto-rollback triggers:
    Relay reverts to old firmware
    Leaves already updated have newer firmware
    Risk of protocol version mismatch at leaf-relay boundary

  If leaves updated first:
    All leaves on new firmware, relay still on old
    Both should interoperate (backward-compatibility requirement:
    new firmware must be compatible with current - 1 minor version)
    Relay updates itself when all leaves confirm

Sequence:
  1. Relay READY
  2. Leaf L1 wakes → push → ACK ✓
  3. Leaf L2 wakes → push → ACK ✓
     ...
  N. Leaf LN wakes → push → ACK ✓ (all done)
  N+1. Relay drains forwarding queue
  N+2. Relay copies ota_stage → ota_1, sets boot partition
  N+3. Relay announces restart in HELLO
  N+4. Relay restarts → boots new firmware

Timeout: If not all leaves updated within LEAF_OTA_TIMEOUT_S
  (default 604,800s = 7 days), relay proceeds with self-update.
  Straggler leaves updated on the next rollout cycle.
```

---

### 14.10 Security

```
Requirement              Mechanism
──────────────────────────────────────────────────────────────────────
Image authenticity       Ed25519 signature over full image
                         Manufacturer signing key in ESP32-S3 eFuse
                         (ESP-IDF secure boot v2)
                         Verified by relay BEFORE entering READY state
                         AND by leaf BEFORE calling set_boot_partition

Version rollback         target_version > sw_version enforced at
  protection             relay (READY gate) and leaf (OTA_START check)
                         Nodes refuse images for equal or older version

Delivery authentication  OTA_ANNOUNCE via BPSec-protected bundle
                         OTA streaming session uses AES-CCM link crypto

A/B auto-rollback        New image must call
                         esp_ota_mark_app_valid_cancel_rollback()
                         within OTA_VALIDATE_TIMEOUT_S (default 60s)
                         Failure triggers automatic revert to ota_0

Chunk integrity          CRC-16 per 4 KB chunk (in-flight error detect)
                         SHA-256 over full image (ota_stage integrity)
                         Ed25519 signature (authenticity)

Signing key compromise   All nodes trust any image signed with the key
                         Recovery: physical re-flash to rotate key
                         Mitigation: per-deployment-batch signing sub-keys
                         (one key per batch — compromise is contained)
```

---

### 14.11 Failure Handling

```
Failure scenario         Behaviour
──────────────────────────────────────────────────────────────────────
Power loss during        ota_stage partially written
  RECEIVING              On reboot: ota_stage flagged invalid
                         Relay returns to IDLE
                         Gateway/mule re-delivers on next contact

Signature fails          ota_stage cleared → FAILED state
  after full receive     ota_error=SIG_FAIL in next HELLO
                         Gateway investigates, retries

Leaf moves mid-transfer  Transfer incomplete, esp_ota_end() not called
  (out of range)         Leaf: ota_1 incomplete, stays on ota_0
                         Relay: leaf marked as not-yet-updated
                         Retry: next leaf wake

Leaf signature fails     Leaf: OTA_ACK{result=SIG_FAIL}
                         Leaf stays on ota_0, resumes normal schedule
                         Relay: reports to gateway via bundle

Leaf auto-rollback       New firmware failed self-validation
                         ESP-IDF reverts to ota_0 automatically
                         Next HELLO shows old sw_version
                         Relay/gateway detect and flag for review

ota_stage flash error    Relay: FAILED → clear ota_stage
                         Relay operational on current firmware
                         ota_error field in HELLO
```

---

### 14.12 New Protocol Elements Summary

**New HELLO fields:**

| Field | Type | Nodes | When present |
|---|---|---|---|
| `sw_version` | uint32 | All | Always |
| `ota_pending` | bool | Relay/Mule | When READY state active |
| `ota_ready` | bool | Leaf | During OTA extended wake |
| `ota_error` | uint8 | All | When reporting OTA failure |

**New bundle type:**

| Bundle EID | Direction | Scope | Size | Description |
|---|---|---|---|---|
| `ipn:0.payload_announce` | GW → all | Broadcast | ~200B | Fleet-wide payload announcement |
| `ipn:<node_id>.payload_announce` | GW → node | Unicast | ~200B | Single-node payload announcement |

**New OTA session messages (direct streaming L2 link frames — not DTN bundles):**

| Type | Message | Direction | Description |
|---|---|---|---|
| `0xF` | `OTA_START` | Sender → receiver | Begin: target_version, image_size, image_hash |
| `0x10` | `OTA_CHUNK` | Sender → receiver | 4 KB data + seq_num + CRC-16 |
| `0x11` | `OTA_EOF` | Sender → receiver | All chunks sent |
| `0x12` | `OTA_ACK` | receiver → Sender | Result: SUCCESS / SIG_FAIL / FLASH_ERR / ROLLBACK |
| `0x13` | `OTA_NACK` | receiver → Sender | Chunk retransmit request (bad CRC) |

(Sender = gateway, relay, or mule depending on mechanism. Frame types are
registered in Section 6.3; OSI placement is in Section 14.1d.)

---

### 14.13 OTA Attack Surface

Because the firmware stream is protected hop-by-hop (AES-128-CCM) and
authenticated end-to-end by the image signature — but is NOT a BPSec
end-to-end-encrypted bundle — its attack surface differs from normal data.
This section enumerates the vectors and their mitigations so the model is
explicit rather than assumed.

```
Well-defended (image signature + version check):
  ✓ Flashing malicious firmware    — Ed25519 signature fails → not flashed
  ✓ Downgrade to vulnerable version — target_version > current rejects it
  ✓ Image tampering in transit      — signature covers the whole image

These are the CATASTROPHIC vectors, and they hold as long as the
signing key and verification implementation are sound (Vector 4).
```

**Vector 1 — Compromised relay reads the firmware.** Each relay decrypts every
chunk (hop-by-hop only), so a compromised relay sees the firmware in plaintext.

```
Exposure:   relay can reverse-engineer the image, extract baked-in
            secrets, or study it for vulnerabilities.
Severity:   LOW for public firmware (every node runs it anyway).
            HIGH only if firmware contains secrets / proprietary logic.
Mitigation: do not bake secrets into firmware images. For proprietary
            firmware, optional image-level encryption (encrypt-to-target)
            — niche, not in the base spec. BPSec bundling would also
            close this, at the cost described in Section 14.1a.
```

**Vector 2 — Battery-drain via false `ota_pending` (highest practical risk).**
A malicious relay falsely advertises an update to force a leaf to stay awake.

```
Exposure:   leaf burns ~54s of wake (≈3 weeks of battery) per false
            trigger; repeated → battery flattened in days.
Mitigation: announce-first verification (Section 14.8) — leaf verifies
            an authenticated relay + signed announce + version BEFORE
            committing to extended wake. Plus the wake-budget cap
            (OTA_WAKE_ATTEMPT_MAX) bounding repeated failed transfers.
```

**Vector 3 — Transfer disruption.** An on-link attacker corrupts chunks or
injects false NACKs to stretch the transfer indefinitely.

```
Exposure:   endless retransmit → battery drain, transfer never completes.
            CRC-16 catches accidental corruption, not malicious chunks
            (trivial to forge a valid CRC); the signature catches bad
            data only at the END, after the wasted transfer.
Mitigation: wake-budget cap (Section 14.8) — abort and report after
            OTA_WAKE_ATTEMPT_MAX failures rather than retransmitting
            forever.
```

**Vector 4 — Signing key / verification integrity (most catastrophic).**
Everything rests on the image signature. Compromise here defeats the fleet.

```
Exposure:   leaked signing key → attacker signs malicious firmware that
            every node accepts. A flawed verify routine → signature moot.
Severity:   CATASTROPHIC — fleet-wide code execution.
Mitigation: ESP-IDF secure-boot eFuse (signing key in hardware);
            per-deployment-batch signing sub-keys (limits blast radius);
            verify the COMPLETE image, never partial; no debug bypass of
            verification in production builds. Elevated to hardening
            plan Tier 1.
```

**Vector 5 — Announce manipulation / campaign censorship.** An attacker
replays, forges, or suppresses announce bundles.

```
Exposure:   replay old announce → rejected by version check (no effect).
            forge announce with fake hash → leaf streams, hash mismatch,
              rejects (wasted transfer = battery, no bad code).
            suppress announce in a cluster → nodes never learn of update
              → fleet fragmentation (availability attack, not code exec).
Mitigation: announce is Ed25519-signed (forgery fails). Version check
            defeats replay. Fragmentation is DETECTED by mule campaign
            telemetry (Section 14.7a) — the gateway sees which nodes
            never updated and can investigate.
```

**Vector 6 — Malicious relay staging-area substitution.** A compromised relay
controls what it pushes to leaves from its ota_stage.

```
Exposure:   push a different image → leaf signature check fails (reduces
              to Vector 4 — needs a valid signature attacker can't forge).
            push a valid OLD signed image → leaf version check rejects.
            selectively skip certain leaves → censorship (Vector 5).
Mitigation: leaf-side signature + version checks make code substitution
            infeasible without breaking Vector 4. Censorship is detected
            by campaign telemetry.
```

**Summary — where the real risk is:**

```
The streaming-vs-BPSec choice is NOT the main source of risk:
  - BPSec would close only Vector 1 (firmware confidentiality), which
    matters only for secret firmware.
  - The catastrophic vector (4) and the practical vectors (2, 3) are
    INDEPENDENT of transport — they would exist with BPSec too.

Priority order for defenders:
  1. Vector 4 (signing integrity)  — catastrophic, hardening Tier 1
  2. Vector 2 (battery drain)      — practical, mitigated in 14.8
  3. Vector 3 (disruption)         — practical, mitigated in 14.8
  4. Vectors 5/6 (censorship)      — detected via telemetry (14.7a)
  5. Vector 1 (confidentiality)    — low unless firmware is secret
```

**New parameters introduced by this section:**

| Parameter | Default | Description |
|---|---|---|
| `OTA_WAKE_ATTEMPT_MAX` | 2 | Failed extended-wake OTA transfers before a leaf backs off |
| `OTA_BACKOFF_CYCLES` | 20 | Wake cycles a leaf ignores ota_pending after hitting the attempt cap (≈5h) |

---

## 15. Open Issues

Issues marked **BLOCKING** must be resolved before firmware development begins.

---

### Issue #1 — IBSS Init Sequence Documentation **[BLOCKING]**

**Description**: The morselib IBSS init sequence using `MORSE_CMD_INTERFACE_TYPE_ADHOC` is undocumented. A community member confirmed it works on ESP32 (May 2026) but no reference implementation exists.

**Resolution**: Read morselib source (`morse_commands.h`, `mmwlan_sta.c`, `mmwlan_softap.c`), trace AP and STA init sequences, replicate for ADHOC. Write `rimba_ibss_init()` wrapper. Test on Seeed Xiao HaLow hardware. Upstream to Morse Micro or maintain as Rimba fork.

---

### Issue #2 — Leaf Peer Link Key Lifetime **[RESOLVED]**

**Resolution**: Session key stored in RTC memory, valid for `PEER_KEY_LIFETIME_MS` = 24 hours. On each wake, leaf checks key validity before running ECDH. Full PEER_OPEN/CONFIRM runs at most once per 24 hours. Specified in Section 7.4 and Section 12.3 step 3. The ECDH computation (~100ms on ESP32-S3) is absorbed into the wake window and does not require a dedicated wake cycle.

---

### Issue #3 — BCF File Dependency **[BLOCKING]**

**Description**: Every MM6108 module requires a vendor-supplied Board Configuration File for RF calibration. Production is blocked until the BCF for the chosen module is confirmed available.

**Resolution**: Confirm BCF availability before committing to a module. Recommended starting modules with community-verified BCFs: Seeed Xiao HaLow, AsiaRF MM610X-001.

---

### Issue #4 — RREQ Storm Validation at Scale **[HIGH]**

**Description**: Reactive routing flood control (jitter, deduplication, rate limiting, expanding ring) has not been empirically validated at 500–1000 node scale on 802.11ah IBSS.

**Resolution**: Simulate 500-node RREQ flood. Measure channel utilisation, route discovery latency, and delivery ratio. Tune `RREQ_JITTER_MS` and `RREQ_RATELIMIT` from results.

---

### Issue #5 — Leaf Power Without TWT **[HIGH]**

**Description**: All-IBSS removes hardware-managed leaf sleep. Custom wake scheduling (Section 12) gives ~30µA average vs ~15µA with TWT — approximately 2× higher. This reduces leaf battery life from ~7–10 years to ~3–5 years (3000 mAh cell, with sensor overhead).

**Resolution options**: (a) Accept the trade-off — 3–5 year battery life is sufficient for most deployments. (b) Reduce `LEAF_SCAN_WINDOW_MS` below 200ms once MM6108 IBSS boot time is validated. (c) Investigate IBSS ATIM power save in MM6108 firmware (Issue #6) — if supported, could approach TWT efficiency. (d) Reduce measurement frequency (longer `LEAF_SLEEP_MS`).

---

### Issue #6 — IBSS Power Save (ATIM) **[HIGH]**

**Description**: 802.11ah IBSS defines an ATIM (Announcement Traffic Indication Message) window for power-save coordination between peers — the IBSS equivalent of TWT. If the MM6108 firmware supports ATIM in IBSS mode, leaf power could approach TWT levels. Current support status is unknown.

**Resolution**: Check morselib source for ATIM window configuration. Test ATIM-based sleep on MM6108 IBSS. If supported, implement as an optional enhancement to the wake window scheme (Section 12.2). If not supported, document as a morselib enhancement target.

---

### Issue #7 — Key Rotation **[MEDIUM]**

**Description**: Deployment root secret and network PSK rotation are not defined.

**Resolution**: Define a key rotation protocol for v2. Use out-of-band physical provisioning as v1 mitigation.

---

### Issue #8 — Bundle Fragmentation **[MEDIUM]**

**Description**: BPv7 bundles exceeding the 802.11ah MSDU size may require fragmentation.

**Resolution**: Define mesh-layer fragmentation using the FRAG flag. Or rely on BPv7 bundle fragmentation (RFC 9171 Section 5.8).

---

### Issue #9 — Mule Authentication **[MEDIUM]**

**Description**: The custody protocol does not authenticate mules. A rogue mule could accept custody and discard bundles.

**Resolution**: Add a mule credential — either a deployment certificate or a pre-provisioned mule ID list held by relays.

---

### Issue #10 — OGM Metric Tuning **[LOW]**

**Description**: The OGM link cost function and intervals (Appendix A) need empirical calibration against MM6108 IBSS link quality measurements.

**Resolution**: Measure RSSI, packet loss, and ETX on MM6108 IBSS links at various distances. Calibrate accordingly.

---

### Issue #11 — Custody Single-Point-of-Loss **[MEDIUM, partially addressed]**

**Description**: In EAGER custody mode, a relay deletes its bundle copy on CUSTODY_ACK CONFIRM, leaving the mule as the only holder. If the mule never delivers, the bundle is lost with no source notification (Section 9.5.7).

**Resolution**: RETAINED custody mode (Section 9.5.7) keeps the source-side copy until an end-to-end CUSTODY_ACK confirms delivery. Default for ALERT bundles and recommended for all downlink. Remaining work: empirical tuning of `MULE_CUSTODY_TIMEOUT_S` and validation of end-to-end ACK propagation latency in a multi-mule deployment (Phase 5).

---

### Issue #12 — Downlink Mule Delivery Validation **[MEDIUM]**

**Description**: Downlink delivery to a specific relay or leaf via mule (Section 9.5.8) depends on the "can you reach X?" custody query and cluster-bridging behaviour. This is more failure-prone than uplink because the destination is a single specific node, not an anycast gateway.

**Resolution**: Validate in Phase 5 — bundle addressed to an isolated leaf, delivered via mule bridging two clusters. Confirm the mule correctly retains bundles that no encountered relay can progress, and that leaf-mobility (target moved to a different relay) triggers correct RREQ-based re-resolution.

---

### Issue #13 — Config-Changeable Parameter Scope **[HIGH — URGENT]**

**Description**: The config mechanism (Section 9.9) is fully specified —
propagation, convergence, version+hash, integrity — but *what config is
allowed to change* is not. The spec has one concrete driver (`e2e_profile`
migration), one real flow (certificate renewal, Section 7.5.5), and scattered
implied parameters (sleep timers, rollout_pct), but no authoritative list. This
is a security gap as well as a completeness gap: without an explicit
provision-only set, it is unstated whether security-critical fields
(`root_secret`, `network_id`, `node_x_priv`, `channel`) could be changed over
the air — which they MUST NOT be, or a compromised config channel becomes an
attack on the trust root.

**Resolution (urgent, before any config feature ships)**: Add a
"Config-Changeable Parameters" table to Section 9.9 defining three classes:
(1) config-changeable operational policy (`e2e_profile`, `hello_key_adv`,
`rollout_pct`, `leaf_sleep_ms`, `sleep_opt`, `downgrade_locked`); (2)
config-changeable rotatable keys (`gateway_pub_key`, `cloud_pub_key`,
`node_cert`); (3) provision-only / never-over-the-air (`root_secret`,
`network_id`, `node_x_priv`, `node_sign_priv`, `channel`, channel width —
see FI-3) — changeable only by
re-provisioning with physical access. Confirm the changeable set fits
`CONFIG_MAX_BYTES` (256 B). A node MUST reject a config bundle that attempts to
change a provision-only key.

---

### Issue #14 — target_version in HELLO (gossiped target hint) **[MEDIUM]**

**Description**: Currently a node learns the OTA `target_version` only from the
signed `payload_announce` bundle (Section 14.4). A node that misses the announce
(asleep, isolated, announce dropped) stays blind to the update even when its
neighbours know. Proposal: gossip the target in HELLO alongside `sw_version`, so
it propagates the same robust way as `config_version` and `sw_version`.

**Resolution (deferred)**: Add `target_version` + `target_hash` (~10 B) to HELLO
as a **discovery hint**, NOT an authoritative value. A node adopts a higher
hinted target only as a trigger to fetch the actual operator-signed announce
(matching `target_hash`) before acting on it — extending wake or pulling an
image still requires the verified signature. This preserves fast gossip
propagation while preventing the unauthenticated-target DoS (a malicious HELLO
advertising target_version=9999 would otherwise drain leaf batteries — same
class as Section 14.13 Vector 2). Unifies the three gossiped monotonic versions
(`sw_version`, `config_version`, `target_version`), each backed by a hash
binding to verified content.

---

### Issue #15 — OTA for Heterogeneous Hardware Types **[HIGH]**

**Description**: The OTA targeting model handles different *roles* (`target_role`)
and different *revisions of one board* (`min_hw_rev`), but NOT genuinely
different hardware *types* that need distinct, mutually-incompatible firmware
images (e.g. ESP32-S3 + sensor X vs + sensor Y, or different MCU variants).
`min_hw_rev` only expresses "this one image needs rev ≥ N"; it cannot express
"Board A gets image_A, Board B gets image_B." Any non-homogeneous fleet needs
this.

**Resolution (deferred)**: Introduce a `hw_type` identifier — provisioned at
staging (a physical fact about the board), advertised in HELLO, and embedded in
the **signed** image/announce metadata. Required additions:
(1) per-`hw_type` announce targeting (a mixed-fleet campaign is multiple
announces, one per type); (2) a **mandatory pre-flash `hw_type` match gate** —
a node refuses to flash an image whose signed `hw_type` ≠ its own, even if the
Ed25519 signature is valid (anti-brick: a correctly-signed wrong-hardware image
is still fatal, and the signature check alone won't catch it); (3) per-`hw_type`
version comparison in the convergence gossip (each type has its own
`target_version`; "behind" is relative to the target for THAT node's hw_type);
(4) mule image library keyed by `hw_type` (`/sd/ota/<hw_type>/<version>.bin`,
extends Section 14.7a). The pre-flash gate is the critical safety item — it
prevents bricking when the wrong image reaches a node by bug, attack, or
mis-tagged announce.

---

### Issue #16 — Cross-Relay Custody Priority **[MEDIUM]**

**Description**: When multiple relays share one mule (Section 9.5.10), turn-taking
is resolved by CSMA medium contention — whichever relay wins the channel first
transfers first. Each relay offers its own bundles ALERT-first, but there is no
mechanism for the mule to prefer one relay's ALERT bundle over another relay's
routine telemetry. A low-priority relay that wins the medium and fills the mule's
storage can leave a higher-priority ALERT bundle on another relay waiting for the
next pass.

**Resolution (deferred)**: A future refinement could have the mule run a short
MANIFEST-only collection round from all in-range relays before accepting any
DATA, then accept globally ALERT-first across the cluster — trading extra
handshake overhead within the contact window for correct cross-relay priority.
Needs evaluation against the contact-window budget (9.5.9), since the extra round
consumes time that could otherwise move data. Not addressed in v1.

---

*End of open issues.*

---

## 16. Future Investigations

---

### FI-1 — Relay Radio Hardware (Single vs Dual MM6108)

**Context**: In the current all-IBSS design, relay nodes require only one MM6108 in IBSS mode — the same as every other node. This is the simplest and lowest-cost relay hardware configuration.

However, a future two-tier architecture (where relay nodes simultaneously serve leaves via AP mode and participate in the relay backbone via IBSS) would require two radio interfaces per relay. Two hardware options exist for that scenario:

**Continuous mode — Single MM6108, Concurrent IBSS+AP**
- Requires morselib multi-VIF support for concurrent IBSS+AP.
- S1G Relay (2.11.2) demonstrates concurrent AP+STA on one chip.
- IBSS+AP combination not yet implemented or tested.
- Estimated effort if pursued: 2–4 weeks of morselib work.

**Scheduled mode — Dual MM6108**
- One in IBSS mode (SDIO), one in AP mode (SPI).
- Works today with no morselib changes.
- Additional BOM: ~$12–15 per relay.

**Status**: Not relevant to current all-IBSS design. Revisit if a two-tier architecture is adopted in a future version.

---

### FI-2 — Dense Deployment Beyond the IBSS Ceiling

**Context**: Rimba's density ceiling (~200 nodes in mutual radio range, Section 13.3.6) is set by CSMA/CA in IBSS mode, which has no scheduled channel access. Adaptive TX power and jitter extend the usable range but cannot eliminate the fundamental limit. The following are candidate mechanisms for deployments that genuinely need higher density, recorded for future evaluation:

**Candidate A — Multi-channel cell separation**
- Neighbouring dense clusters operate on different 802.11ah channels.
- Reduces co-channel contention via spatial/frequency reuse.
- Requires a channel-assignment mechanism (manual or distributed graph colouring) and a rendezvous channel for inter-cluster traffic and mules.
- Adds significant complexity; breaks the single-channel simplicity.

**Candidate B — Lightweight TDMA access scheduling**
- A node experiencing heavy contention proposes a local TDMA schedule to its neighbours (similar to the relay backbone TDMA in Section 7.3, but for channel access rather than sleep).
- Approximates RAW behaviour without AP mode.
- Works only within a fully-connected cluster; coordination across partially-overlapping cells is hard.

**Candidate C — IBSS ATIM power save (see Issue #6)**
- If MM6108 firmware supports ATIM, scheduled wake windows reduce simultaneous-awake contention.
- Helps both density and battery.
- Depends on firmware capability — needs validation.

**Candidate D — Gossip-based routing instead of flooding** *(implemented in v0.9, Section 8.7.1)*
- Replaces RREQ flooding with probabilistic gossip (re-broadcast with probability p < 1, scaled inversely with neighbour count).
- Reduces flood overhead in dense cells; rare misses recovered via DTN retry.
- Now part of the core spec. Remaining candidates below are for density beyond what gossip + aggregation + adaptive power can handle.

**Recommendation**: Gossip routing (D) and bundle aggregation (Section 9.7) are now implemented and are the primary dense-mitigation tools. If density still exceeds the ceiling after these, evaluate Candidate C (ATIM) next, then A (multi-channel). Candidate B (TDMA access) adds the most complexity and should be last.

---

### FI-3 — Dynamic Channel Width for Short Pairwise High-Bandwidth Bursts

**Context**: The deployment-wide channel width (1 MHz default, 2 MHz balanced;
Section 5) is treated as **provision-only** — changing it network-wide over the
air is unsafe, because config delivery is non-atomic (Section 9.9.4) and a
half-applied width change *partitions the network*: nodes that switched to 2 MHz
cannot hear nodes still on 1 MHz, and the corrective config cannot reach either
side. Channel width therefore belongs in the provision-only set alongside
`channel` and `network_id` (Open Issue #13). This makes a network-wide dynamic
width change a non-starter for v1.

**The narrower idea worth investigating**: a *temporary, pairwise* width
increase between **two specific nodes** that have already discovered each other
at the common base width — not a network-wide change. The motivating use case is
a **short high-bandwidth transfer**, most obviously an OTA firmware stream:

```
  Two nodes peer-linked at the 1 MHz base width (discovery always at base):
    R (has image) and R' (or a leaf) negotiate a temporary width bump
    for the duration of ONE transfer, then drop back to base.

  Leaf OTA at 1 MHz:  ~54 s for a 1 MB image
  Leaf OTA at 2 MHz:  potentially ~half that → less leaf wake → less
                      battery per update. The bandwidth helps exactly
                      where it hurts most (the long leaf wake window).
```

**Why pairwise sidesteps the partition risk**:

```
- Discovery and HELLO ALWAYS happen at the common base width (1 MHz), so
  nodes can always find each other — the network never loses connectivity.
- The width bump is scoped to ONE link, for ONE transfer, negotiated in
  the peer-link/OTA_START handshake. No global flag day, no config push.
- If the bump fails (interference, one node can't sustain the width),
  both nodes fall back to the base width — the transfer continues slower,
  never breaks.
- Only the two participating nodes are affected; the rest of the mesh is
  untouched and unaware.
```

**Open questions to investigate**:

```
1. Does the MM6108 support changing channel width on a per-session basis
   without a full radio reset? (If a width change requires a reconfigure
   that drops all links, the pairwise model is infeasible.)
2. Can two nodes hold a wider link while the rest of the cell operates at
   the base width on the same/adjacent center frequency without
   interfering? (Adjacent-channel and co-channel interference analysis.)
3. Negotiation: extend PEER_OPEN/CONFIRM or OTA_START to carry a proposed
   width + fallback rule. Both nodes must support the width and agree.
4. Timing: how does a wider pairwise link coexist with the relay TDMA
   backbone schedule (Section 7.3) and other nodes' channel access?
5. Mule transfers: a mule parked next to a relay for a bulk image handoff
   is the ideal candidate — both are (often) powered, stationary for the
   transfer, and isolated enough that a width bump is low-risk.

Recorded as a future feature, NOT for v1. v1 keeps channel width
provision-only and fixed. The pairwise-burst model is the only form of
"dynamic channel width" that does not risk partitioning the mesh, and
even it depends on MM6108 capabilities that must be verified first.
```

---

*Document status: Draft 0.28 — True E2E encryption (Section 10.5, ECIES); key discovery protocol (Section 7.5); deployment CA trust model; OTA firmware update (Section 14); DTN/TCP-IP explainer (Section 9.0); custody reliability with RETAINED mode and downlink mule delivery (Sections 9.5.7–9.5.8); routing gaps closed — multiple/lost RREP handling (8.5.1–8.5.2), RERR precursor-list propagation (8.6); CLOUD_ANYCAST_ID added — EID-selected encryption endpoint (gateway vs cloud), three-tier threat model (8.11, 10.1, 10.5); OTA streaming rationale, OSI comparison, and streaming sequence diagram (14.1a–14.1c); ECIES-within-BPSec clarification (10.5); mule OTA campaign participation — image library, autonomous delivery, telemetry, self-update timing (14.7a); OTA attack surface analysis (14.13) with announce-first verification and wake-budget anti-drain (14.8); deployment encryption profiles HYBRID (default) and FULL_TRUE — pure-soft profile removed, backend traffic unconditionally true E2E (10.5, 10.1); per-node keypairs + deployment-CA certificate now MANDATORY at provisioning (3.6.5) — every node FULL_TRUE-capable from first boot, migration is a config flip, no OTA cert-issuance path; profile scope clarified (10.5) — 'node-to-node' = all field-node-to-field-node endpoint payloads (leaf/relay/mule), forwarding and backend traffic excluded; HELLO L2 placement and Trickle adaptation documented (7.1); configuration change mechanism (9.9) — signed config bundles, config_version (ordering) + config_hash (integrity), decentralized convergence gossip (every node pushes config to behind peers; no roster, no central campaign), same-version-different-hash anomaly detection; OTA aligned to the same version (ordering) + hash (integrity) primitive and decentralized 'who is behind' comparison (14.1a, 14.7a.2, sw_version HELLO), image additionally Ed25519-signed; OTA frames (OTA_START/CHUNK/EOF/ACK/NACK) assigned frame types 0xF–0x13 in the registry (6.3), OSI placement table for all OTA packets (14.1d), and unified three-mechanism delivery diagram (14.1e); config payload capped at CONFIG_MAX_BYTES=256 B for v1 (single-frame, cacheable; raise in a future version), larger payloads must use the streaming path (9.9); Open Issue #13 raised [HIGH/URGENT] — config-changeable parameter scope not yet enumerated; OTA session frame formats (OTA_START/CHUNK/EOF/ACK/NACK) now fully field-defined (14.4a); end-to-end OTA campaign overview timeline added (14.0); Mechanism A2 — relay-to-relay backbone propagation (14.6a); Open Issues #14 (target_version as a signed-announce-backed HELLO hint) and #15 [HIGH] (OTA for heterogeneous hw_type — per-type targeting, mandatory pre-flash hw_type gate to prevent bricking, per-type version comparison) raised and deferred; FI-3 future investigation added — dynamic channel width for short pairwise high-bandwidth bursts (e.g. faster OTA), scoped to one link to avoid the network-partition risk of a width change; channel width added to the provision-only set (Issue #13); consolidated custody explainer (9.5.0) — what custody protects against, OSI placement (L2 CUSTODY_REQ/ACK handshake vs L4 E2E_CUSTODY_ACK bundle), full flow diagram, and with-vs-without comparison, plus the note that custody is a Rimba extension not a BPv7 feature; mule contact-window and non-stopping-sweep design (9.5.9) — custody assumes the mule NEVER stops (per-pass partial transfer, confirm-before-delete, multi-pass drainage, ALERT-first), with stopping as a bonus that widens the window for full drain/bulk transfer; 'departing' corrected — there is no mule-decided departure under never-stops, so the relay infers window-closing from the mule's HELLO RSSI trend (RSSI past peak), with a mule advertising only genuine mule-side unavailability (storage full / self-update reboot); 9.5.5 rewritten accordingly; multiple-relays-sharing-one-mule custody (9.5.10) — CSMA-serialized turn-taking, one-session lock with CUSTODY_ACK BUSY phase, storage_kb budget advertising, lossless retain-for-next-pass, plus Open Issue #16 [MEDIUM] for cross-relay ALERT priority; custody handshake OSI placement + full arrival-to-departure sequence diagram (9.5.1a) — MANIFEST/ACCEPT/ACK/CONFIRM are L2 control signalling (frame types 0xB/0xC/0x9), only DATA carries an L4 bundle and E2E_CUSTODY_ACK is an L4 bundle; complete unified flow (9.5.11) — one mule arriving, servicing three relays in turn (R1→R2→R3, storage draining 200→120→60→0 KB, graceful FULL saturation with overflow retained), and departing by RSSI-inferred window close, with asynchronous per-bundle E2E ack closure; corrected delete-on-CONFIRM diagrams/statements (3.8.6, 9.5.1, DTN explainer) to state that releasing a bundle on CONFIRM depends on retention mode (EAGER deletes now / RETAINED keeps until end-to-end ack), not unconditional deletion.*
*Changes from 0.24: Section 3.2.4 — mule is relay superset, data sourcing added. Section 12.10 — Continuous/Scheduled renamed to Continuous/Scheduled throughout spec; plain-English mode definitions added with comparison table (power, battery, complexity, frame-loss risk); runtime switching added (NVS persistent vs RAM transient); three switching mitigations documented: Trickle reset before settle timer, transient RAM flag for mule, RISK-02 boot-time validation gate before enabling Scheduled mode.*

---

## Appendix A — Proactive OGM Routing (Deployments ≤ 100 Nodes)

For small deployments, proactive OGM flooding provides lower route discovery latency than reactive RREQ/RREP.

**OGM payload** (CBOR):

```
{
  "originator": <6-byte node_id>,
  "seq":        <uint16: monotonically increasing>,
  "ttl":        <uint8: starts at 16>,
  "metric":     <uint8: cumulative path metric, starts at 0>
}
```

**OGM intervals**:

| Role | Interval |
|---|---|
| Gateway | 2 s |
| Relay | 10 s |
| Mule | 3 s |

Leaves do not generate OGMs.

**Propagation**: Each relay re-broadcasts received OGMs (TTL−1, metric + link_cost) unless `(originator, seq)` was seen within 60 seconds. Trickle suppression doubles the interval when stable.

**Link cost**: `link_cost = 256 / lq` where `lq` is link quality 0–255. Forwarding table stores the neighbour delivering the best (lowest metric) OGM per originator.

**Transition trigger**: Switch to reactive RREQ/RREP when node count exceeds 100 or OGM-induced channel load exceeds 20% of capacity.
