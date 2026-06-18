# Rimba

A sparse-first, delay-tolerant networking (DTN) mesh protocol for massive-scale
IoT deployment in remote and intermittently-connected areas.

Rimba carries CBOR sensor payloads inside BPv7 bundles (RFC 9171) over an
802.11ah (Wi-Fi HaLow) IBSS mesh, using store-carry-forward and mobile data
"mules" to bridge network partitions. Nodes are battery-optimized and
RTC-scheduled; traffic to the backend is always end-to-end encrypted.

## Status

**Draft 0.28 — design stage, entering hardware bring-up.**

Development is starting with the BLOCKING hardware-validation phase. **Before
writing any feature, read the [development plan](docs/rimba-development-plan.md)**
— it defines the phased build order and, critically, the IBSS-foundation phase
that everything else depends on (plus the fallback strategy if IBSS doesn't work
on the MM6108).

See Section 15 (Open Issues) and Section 16 (Future Investigations) in the spec
for tracked work, including the urgent config-changeable-parameter-scope security
boundary (Issue #13).

## Where to start

1. **[`docs/rimba-development-plan.md`](docs/rimba-development-plan.md)** — start here if you are building. Phased plan, risk register, and the Phase 1 IBSS-foundation tasks. Includes the RISK-01 fallback strategy (what to do if IBSS doesn't work, incl. the AP-STA path and its HELLO/discovery implications).
2. **[`docs/rimba-protocol-spec.md`](docs/rimba-protocol-spec.md)** — the normative specification. Read for protocol details.
3. The companion analyses (below) — background that informed the design.

## Hardware baseline

- **MCU:** ESP32-S3
- **Radio:** Morse Micro MM6108 (802.11ah HaLow, single radio per node)
- **RTC:** RV-3028-C7 (≤1 ppm), mandatory for scheduled wake (deferrable to Phase 4 via NTP dev-mode — see development plan)
- **PHY:** sub-GHz (902–928 MHz US), ~1 km range, 1–2 MHz channels

## Node roles

- **Leaf** — battery sensor; sleeps, wakes on RTC schedule, sends/receives on wake.
- **Relay** — always-on forwarder; backbone, DTN store, leaf scheduling.
- **Mule** — mobile relay superset; physically carries bundles between partitions. Designed to work without stopping (a stop is a bonus that widens the contact window).
- **Gateway** — backbone egress to the backend (`ipn:1.*`); cloud is `ipn:2.*`.

## Protocol stack

```
CBOR payload
  → BPSec / COSE  (E2E encryption, RFC 9172 / 9052 / 9053)
  → BPv7 bundle   (DTN store-carry-forward, RFC 9171)
  → mesh routing  (OGM / RREQ / geo)
  → custom convergence layer
  → 802.11ah IBSS (EtherType 0x88B5, AES-128-CCM hop-by-hop)
```

## Documents

All documents live under `docs/`.

| File | What it is |
|---|---|
| [`docs/rimba-development-plan.md`](docs/rimba-development-plan.md) | **Phased implementation plan + risk register.** Start here to build. Phase 1 (IBSS foundation, BLOCKING) and the RISK-01 IBSS fallback strategy. |
| [`docs/rimba-protocol-spec.md`](docs/rimba-protocol-spec.md) | **The normative specification** (Draft 0.28). 16 sections: architecture, frames, routing, DTN/mule protocol, custody, security, power, OTA, open issues, future investigations. |
| [`docs/rimba-hardening-plan.md`](docs/rimba-hardening-plan.md) | Security hardening roadmap (Tier 0–4). |
| [`docs/rimba-mesh-comparison.md`](docs/rimba-mesh-comparison.md) | Comparison vs other mesh protocols. |
| [`docs/rimba-routing-comparison.md`](docs/rimba-routing-comparison.md) | Routing-approach analysis and tradeoffs. |
| [`docs/rimba-battery-analysis.md`](docs/rimba-battery-analysis.md) | Power budget and battery-life analysis. |
| [`docs/rimba-latency-bandwidth.md`](docs/rimba-latency-bandwidth.md) | Latency and bandwidth analysis. |
| [`docs/rimba-mesh-topology.md`](docs/rimba-mesh-topology.md) | Topology and density analysis. |
| [`docs/rimba-rtc-comparison.md`](docs/rimba-rtc-comparison.md) | RTC selection and drift analysis (incl. NTP dev-mode for Phases 1–3). |
| [`docs/halow-mesh-dtn-spec.md`](docs/halow-mesh-dtn-spec.md) | Superseded original spec (kept for history; see deprecation banner inside). |
| [`CHANGELOG.md`](CHANGELOG.md) | Per-draft change history. |

## Standards followed

BPv7 (RFC 9171), BPSec (RFC 9172), COSE (RFC 9052/9053), CBOR (RFC 8949),
DTN architecture (RFC 4838), Trickle (RFC 6206). The convergence layer is
custom (not TCPCLv4); custody is a Rimba extension (BPv7 removed BPv6 custody).
