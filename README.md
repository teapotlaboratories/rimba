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

## Building the firmware

The firmware lives under [`firmware/`](firmware/) and targets the ESP32-S3. Two
examples exist (select with `APP=`):
- **`rimba-hello`** — radio-free sanity check (toolchain + 8 MB PSRAM, RISK-04).
- **`rimba-halow-scan`** — boots the MM6108 HaLow radio over SPI and scans for
  APs. Confirmed working (firmware v1.17.6, chip ID 0x0306).

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

So **ESP-IDF is the mandatory toolchain**; the MM6108 components ship as git
submodules under `components/` (init them when cloning). Install ESP-IDF first.

### 1. One-time toolchain setup

ESP-IDF is installed out-of-tree (it is not committed to this repo). v5.4.2 is
the minimum required by the `morsemicro/halow` component:

```bash
git clone -b v5.4.2 --depth 1 --recursive https://github.com/espressif/esp-idf ~/esp/esp-idf-5.4.2
~/esp/esp-idf-5.4.2/install.sh esp32s3

# cmake + ninja (ESP-IDF does not bundle them on Linux).
# Pin cmake to 3.x — cmake 4.x breaks the ESP-IDF build.
~/.espressif/python_env/idf5.4_py*_env/bin/pip install "cmake==3.30.5" ninja
# (or, if you have sudo: apt install cmake ninja-build)
```

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
`make build BOARD=proto2`, or `make build IDF_PATH=~/esp/esp-idf-5.4.2`. Run
`make help` for the full list.

> The Seeed XIAO ESP32-S3 enumerates over its native USB-Serial-JTAG as
> `/dev/ttyACM0`, used for both flashing and the console. Your user must be in the
> `dialout` group to access it.

Prefer raw `idf.py`? `source ~/esp/esp-idf-5.4.2/export.sh`, then work inside
`firmware/rimba-halow-scan/` — but pass the board config yourself, since the
Makefile is what wires it: `idf.py -D SDKCONFIG_DEFAULTS=../../boards/proto1/sdkconfig.defaults build`.

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
