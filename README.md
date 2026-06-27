# Rimba

A sparse-first, delay-tolerant networking (DTN) mesh protocol for massive-scale
IoT deployment in remote and intermittently-connected areas.

Rimba carries CBOR sensor payloads inside BPv7 bundles (RFC 9171) over an
802.11ah (Wi-Fi HaLow) IBSS mesh, using store-carry-forward and mobile data
"mules" to bridge network partitions. Nodes are battery-optimized and
RTC-scheduled; traffic to the backend is always end-to-end encrypted.

## Status

**Draft 0.28 — Phase-1 foundation validated on hardware; currently in the L2-layer
decision phase.**

Rimba's link layer has two viable options on the MM6108, and **both are being built on
hardware and compared** before committing to one:

- **IBSS / ad-hoc** — symmetric, infrastructure-free. Foundation resolved, hardened, and
  soaked; the open question is leaf power-save (this firmware has no IBSS radio power-save).
  → [`docs/ibss/rimba-ibss-milestones.md`](docs/ibss/rimba-ibss-milestones.md)
- **Mesh-gate (802.11s mesh + AP)** — relays mesh; leaves TWT-sleep under a relay-AP that
  buffers their downlink. AP mode, the TWT responder + leaf power-save, STA-count scaling to
  255, and the **802.11s mesh point** (an ESP32 joins a Linux HaLow mesh, pings, relays
  multi-hop, group-forwards, PERR teardown — P0–P6b) are all proven; ESP32 mesh+AP concurrency
  on one radio is the remaining open item.
  → [`docs/mesh-ap/rimba-mesh-ap-milestones.md`](docs/mesh-ap/rimba-mesh-ap-milestones.md)

Neither L2 is chosen yet — making them comparable on the same silicon is the point of this phase.

For what's left, the phased plan, and the per-milestone history, see
[`docs/rimba-todo.md`](docs/rimba-todo.md) (roadmap),
[`docs/rimba-development-plan.md`](docs/rimba-development-plan.md) (phases + risk register),
and the per-L2 milestone docs above.

## Where to start

1. **[`docs/rimba-todo.md`](docs/rimba-todo.md)** — the master TODO / roadmap. One high-level page indexing all outstanding work, pointing to the detailed backlogs. Start here for "what's left."
2. **[`docs/rimba-development-plan.md`](docs/rimba-development-plan.md)** — start here if you are building. Phased plan, risk register, and the Phase 1 IBSS-foundation tasks. Includes the RISK-01 fallback strategy (what to do if IBSS doesn't work, incl. the AP-STA path and its HELLO/discovery implications).
3. **[`docs/design-specification/rimba-protocol-spec.md`](docs/design-specification/rimba-protocol-spec.md)** — the normative specification. Read for protocol details.
4. The companion analyses (below) — background that informed the design.

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
  [`docs/ibss/rimba-ibss-milestones.md`](docs/ibss/rimba-ibss-milestones.md).

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

> The MM6108 radio code comes from the vendored `components/halow` submodule
> (morselib). The firmware image is **generated at build time** from the
> `vendor/morse-firmware` submodule (ELF → `.mbin`, see `cmake/mm-fw-gen/`) — no
> firmware blobs are vendored.

So **ESP-IDF is the mandatory toolchain**; it is vendored at `vendor/esp-idf`, and
the MM6108 radio code ships as git submodules under `components/` (init them when
cloning). Set up the vendored ESP-IDF first (below).

### 1. One-time toolchain setup

> **Install [Git LFS](https://git-lfs.com) before cloning** (`git lfs install`) — some docs (e.g.
> `docs/reference/MM_APPNOTE-24_Linux_Porting_Guide.pdf`) are stored via LFS. If you cloned without it, run
> `git lfs install && git lfs pull` to fetch the real files (otherwise you get small pointer stubs).

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
environment for you). Make sure the submodules are checked out first
(`git submodule update --init components/halow vendor/morse-firmware`).

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
| [`docs/rimba-todo.md`](docs/rimba-todo.md) | **Master TODO / roadmap** — one high-level index of all outstanding work, pointing to the per-area backlogs. |
| [`docs/rimba-development-plan.md`](docs/rimba-development-plan.md) | **Phased implementation plan + risk register.** Start here to build. Phase 1 (IBSS foundation, BLOCKING) and the RISK-01 IBSS fallback strategy. |
| [`docs/design-specification/rimba-protocol-spec.md`](docs/design-specification/rimba-protocol-spec.md) | **The normative specification** (Draft 0.28). 16 sections: architecture, frames, routing, DTN/mule protocol, custody, security, power, OTA, open issues, future investigations. |
| [`docs/design-specification/rimba-hardening-plan.md`](docs/design-specification/rimba-hardening-plan.md) | Security hardening roadmap (Tier 0–4). |
| [`docs/ibss/rimba-ibss-milestones.md`](docs/ibss/rimba-ibss-milestones.md) | **IBSS — the single doc.** RISK-01 bring-up milestones + hardening (H1–H6), the Linux-equivalence table (each port file/symbol ↔ its `net/mac80211` / `morse_driver` counterpart), the **fork comparison** (vs `momentary-systems`), the **TODO / open items**, and the **findings & decisions** (EEXIST, data-driven discovery, phantom-flood, no-IBSS-power-save, CCMP block). |
| [`docs/ibss/rimba-ibss-test-plan.md`](docs/ibss/rimba-ibss-test-plan.md) | IBSS validation plan + results (P0 multi-node, I.1–I.5 Linux interop). |
| [`docs/reference/rimba-linux-node-setup.md`](docs/reference/rimba-linux-node-setup.md) | Bring-up + interop commands for the Raspberry Pi + MM6108 Linux reference node (AP test §11; IBSS interop §12). |
| [`docs/mesh-ap/rimba-mesh-ap-milestones.md`](docs/mesh-ap/rimba-mesh-ap-milestones.md) | **Mesh-gate (Mesh + AP) — the single doc.** The L2 alternative to IBSS: milestones (AP → TWT power-save → STA-count scaling 63→255 → multi-node validation), the **new-code ↔ Linux `morse_driver`/`dot11ah` porting maps** (TWT responder + multi-block S1G TIM), the IBSS-vs-Mesh-gate trade-off, and the **Mesh-gate TODO** — plus the **full 802.11s mesh-point status + Linux/ESP32 feature comparison** (P0–P6b: peering, HWMP, forwarding, PERR), consolidated here. |
| [`docs/design-specification/rimba-mesh-comparison.md`](docs/design-specification/rimba-mesh-comparison.md) | Comparison vs other mesh protocols. |
| [`docs/design-specification/rimba-routing-comparison.md`](docs/design-specification/rimba-routing-comparison.md) | Routing-approach analysis and tradeoffs. |
| [`docs/design-specification/rimba-battery-analysis.md`](docs/design-specification/rimba-battery-analysis.md) | Power budget and battery-life analysis. |
| [`docs/design-specification/rimba-latency-bandwidth.md`](docs/design-specification/rimba-latency-bandwidth.md) | Latency and bandwidth analysis. |
| [`docs/design-specification/rimba-mesh-topology.md`](docs/design-specification/rimba-mesh-topology.md) | Topology and density analysis. |
| [`docs/design-specification/rimba-rtc-comparison.md`](docs/design-specification/rimba-rtc-comparison.md) | RTC selection and drift analysis (incl. NTP dev-mode for Phases 1–3). |
| [`CHANGELOG.md`](CHANGELOG.md) | Per-draft change history. |

## Standards followed

BPv7 (RFC 9171), BPSec (RFC 9172), COSE (RFC 9052/9053), CBOR (RFC 8949),
DTN architecture (RFC 4838), Trickle (RFC 6206). The convergence layer is
custom (not TCPCLv4); custody is a Rimba extension (BPv7 removed BPv6 custody).
