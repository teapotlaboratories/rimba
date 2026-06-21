# Rimba

A sparse-first, delay-tolerant networking (DTN) mesh protocol for massive-scale
IoT deployment in remote and intermittently-connected areas.

Rimba carries CBOR sensor payloads inside BPv7 bundles (RFC 9171) over an
802.11ah (Wi-Fi HaLow) IBSS mesh, using store-carry-forward and mobile data
"mules" to bridge network partitions. Nodes are battery-optimized and
RTC-scheduled; traffic to the backend is always end-to-end encrypted.

## Status

**Draft 0.28 — design stage; Phase-1 IBSS foundation validated on hardware.**

Development started with the BLOCKING hardware-validation phase. **Before writing
any feature, read the [development plan](docs/rimba-development-plan.md)** — it
defines the phased build order and the IBSS-foundation phase everything depends on.

**RISK-01 (IBSS on the MM6108) is resolved and hardened**: 802.11ah ad-hoc bring-up,
bidirectional IP/`0x88B5` data, a 3-board full mesh, peer age-out + drop/rejoin, and
interop with a Linux `morse_driver` IBSS node on the same silicon — all derived from
and verified against the Linux implementation. See
[`docs/rimba-ibss-milestones.md`](docs/rimba-ibss-milestones.md) and the
[IBSS worklogs](docs/worklog/). The AP-STA fallback is not required. Next: Phase-2
link security.

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

## Building the firmware

The firmware lives under [`firmware/`](firmware/) and targets the ESP32-S3.
Several apps exist (select with `APP=`):
- **`rimba-hello`** — radio-free sanity check (toolchain + 8 MB PSRAM, RISK-04).
- **`rimba-halow-scan`** — boots the MM6108 HaLow radio over SPI and scans for
  APs. Confirmed working (firmware v1.17.6, chip ID 0x0306).
- **`rimba-halow-ap`** + **`rimba-halow-sta`** — a 2-board AP↔STA ping test.
  Confirmed on hardware: bidirectional IP, ~12 ms RTT over 802.11ah — the
  Phase-1 RISK-01 AP-STA link validated end-to-end.
- **`rimba-halow-ibss`** — the **IBSS / ad-hoc** mesh app (RISK-01 resolved). One
  binary on every node: N-node MAC-derived addressing, peer discovery, and pings
  to every peer. Validated on hardware — 3-board full mesh **and** interop with a
  Linux `morse_driver` IBSS node. See
  [`docs/rimba-ibss-milestones.md`](docs/rimba-ibss-milestones.md).

See [`firmware/README.md`](firmware/README.md) for per-example detail.

### Two separate SDKs (and why)

A Rimba node has two chips, each with its own SDK — don't confuse them:

| | SDK | What it's for |
|---|---|---|
| **ESP32-S3** (host MCU) | **ESP-IDF** (Espressif) | The build system (`idf.py`, CMake/Ninja), FreeRTOS, drivers, crypto. **Everything compiles against this — it is required.** |
| **MM6108** (HaLow radio, over SPI) | **`morsemicro/halow`** (Morse Micro) | `morselib` driver + firmware blobs, from the [ESP Component Registry](https://components.espressif.com/components/morsemicro/halow). **Vendored as git submodules** under `components/` (forked at `2.10.4-esp32-2`) for reproducibility, not auto-downloaded. Requires ESP-IDF ≥ 5.4.2. |

```
ESP32-S3  (runs ESP-IDF) ──SPI──▶ MM6108  (driven by morselib from morsemicro/halow)
   host MCU                          HaLow radio
```

> The `vendor/mm-iot-sdk` submodule is **reference only** (full docs + the
> complete BCF set). The build does *not* use it — the radio code comes from the
> vendored `components/halow` + `components/firmware` submodules.

So **ESP-IDF is the mandatory toolchain**; it is vendored at `vendor/esp-idf`, and
the MM6108 radio code ships as git submodules under `components/` (init them when
cloning). Set up the vendored ESP-IDF first (below).

### 1. One-time toolchain setup

ESP-IDF is **vendored as a git submodule at `vendor/esp-idf`** (pinned at v5.4.2,
the minimum required by the `morsemicro/halow` component). Check it out with its
own submodules, then install the toolchain from it:

```bash
git submodule update --init vendor/esp-idf          # if not cloned --recurse-submodules
git -C vendor/esp-idf submodule update --init --recursive
./vendor/esp-idf/install.sh esp32s3

# cmake + ninja (ESP-IDF does not bundle them on Linux).
# Pin cmake to 3.x — cmake 4.x breaks the ESP-IDF build.
~/.espressif/python_env/idf5.4_py*_env/bin/pip install "cmake==3.30.5" ninja
# (or, if you have sudo: apt install cmake ninja-build)
```

The Makefile's default `IDF_PATH` points at `vendor/esp-idf`, so the build uses
the vendored toolchain automatically (override `IDF_PATH=...` for a different one).

### 2. Build / flash / monitor

Run from the **repo root** (the `Makefile` wraps `idf.py` and sources the ESP-IDF
environment for you). Make sure the `components/` submodules are checked out first
(`git submodule update --init components/halow components/firmware`).

`APP` selects the example (default `rimba-halow-scan`) and `BOARD` selects the
board config under `boards/` (default `proto1`):

```bash
make build                        # rimba-halow-scan on board proto1 (defaults)
make flash                        # flash to /dev/ttyACM0
make monitor                      # serial console (Ctrl-] to exit)
make flash-monitor
make build APP=rimba-hello        # the radio-free sanity example
```

Override defaults inline, e.g. `make flash PORT=/dev/ttyACM1`,
`make build BOARD=proto2`, or `make build IDF_PATH=/path/to/other/esp-idf`. Run
`make help` for the full list.

> The Seeed XIAO ESP32-S3 enumerates over its native USB-Serial-JTAG as
> `/dev/ttyACM0`, used for both flashing and the console. Your user must be in the
> `dialout` group to access it.

Prefer raw `idf.py`? `source vendor/esp-idf/export.sh`, then work inside
`firmware/rimba-halow-scan/` — but pass the board config yourself, since the
Makefile is what wires it: `idf.py -D SDKCONFIG_DEFAULTS=../../boards/proto1/sdkconfig.defaults build`.

## Documents

All documents live under `docs/`.

| File | What it is |
|---|---|
| [`docs/rimba-development-plan.md`](docs/rimba-development-plan.md) | **Phased implementation plan + risk register.** Start here to build. Phase 1 (IBSS foundation, BLOCKING) and the RISK-01 IBSS fallback strategy. |
| [`docs/rimba-protocol-spec.md`](docs/rimba-protocol-spec.md) | **The normative specification** (Draft 0.28). 16 sections: architecture, frames, routing, DTN/mule protocol, custody, security, power, OTA, open issues, future investigations. |
| [`docs/rimba-hardening-plan.md`](docs/rimba-hardening-plan.md) | Security hardening roadmap (Tier 0–4). |
| [`docs/rimba-ibss-milestones.md`](docs/rimba-ibss-milestones.md) | **RISK-01 IBSS bring-up milestones + the Linux-equivalence table** (each port file/symbol ↔ its `net/mac80211` / `morse_driver` counterpart). |
| [`docs/rimba-ibss-hardening-todo.md`](docs/rimba-ibss-hardening-todo.md) | IBSS hardening backlog + Findings (EEXIST, #16 data-driven discovery, #17 phantom flood, …). |
| [`docs/rimba-ibss-test-plan.md`](docs/rimba-ibss-test-plan.md) | IBSS validation plan + results (P0 multi-node, I.1–I.5 Linux interop). |
| [`docs/rimba-ibss-impl-comparison.md`](docs/rimba-ibss-impl-comparison.md) | Our port vs the `momentary-systems/esp-halow-ibss` fork (which we adopted). |
| [`docs/rimba-ibss-linux-interop-runbook.md`](docs/rimba-ibss-linux-interop-runbook.md) + [`docs/rimba-linux-node-setup.md`](docs/rimba-linux-node-setup.md) | Bring-up + interop commands for the Raspberry Pi + MM6108 Linux reference node. |
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
