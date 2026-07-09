# Rimba firmware

ESP32-S3 + Morse Micro MM6108 (Wi-Fi HaLow) firmware for the Rimba DTN mesh.
See [`../docs/rimba-development-plan.md`](../docs/rimba-development-plan.md) for
the phased build order, and [`../docs/worklog/`](../docs/worklog/) for the
bring-up log.

## Layout

```
Makefile               # (repo root) convenience wrapper around idf.py (build/flash/monitor)
build/<APP>/<BOARD>/   # (repo root) out-of-source build output, per app+board (gitignored)
boards/
  proto1/              # Seeed XIAO ESP32-S3 Plus + HaLow add-on
  proto1-fgh100m/      # Seeed XIAO ESP32-S3 + FGH100M (MM6108) — the current HaLow/PS bench board
components/            # vendored Morse Micro components (git submodules), shared by apps
  halow/               #   morsemicro/halow    — morselib driver  (repo: mm-esp32-halow)
firmware/
  # --- bring-up / sanity ---
  rimba-hello/         # Phase-1 sanity: toolchain + PSRAM/SRAM validation, no radio. Also the radio-silence image.
  rimba-halow-scan/    # MM6108 bring-up: boots the HaLow radio and scans for APs
  # --- AP <-> STA link + throughput ---
  rimba-halow-ap/      # HaLow SoftAP (2-board ping AP; also the ESP32-AP-under-test in the PS study)
  rimba-halow-sta/     # TRIGGERED power-save ladder — the PS DUT app (netif-free, C6-triggered)
  rimba-halow-ap-perf/ # iperf throughput — SoftAP side
  rimba-halow-sta-perf/# iperf throughput / 2-board ping — STA side
  # --- mesh ---
  rimba-halow-mesh/    # 802.11s secured mesh node (SAE + AMPE + host SW-CCMP)
  rimba-halow-mesh-perf/# iperf throughput — mesh
  rimba-halow-ibss/    # 802.11ah IBSS / ad-hoc mesh (RISK-01): N-node addressing, peer discovery, ping
  # --- power-save & sleep characterization (see the PS reference doc) ---
  rimba-twt-assoc/     # TWT via the assoc-embedded path (engages on both Linux + ESP APs)
  rimba-doze-hold/     # keep the STA associated while the radio dozes (TWT then WNM+powerdown)
  rimba-standby-test/  # MMWLAN_STANDBY feasibility (deprecated — loses to WNM)
  rimba-deepsleep-cycle/# deep-sleep as a duty-cycled leaf (RESET_N-low + re-associate on wake)
  rimba-downlink-test/ # downlink-while-dozing: a pingable dozing STA (§3b)
  rimba-sleep-test/    # board2 lowest-power floor (radio in hardware reset + ESP32 deep sleep)
  # --- measurement harness ---
  c6-harness/          # ESP32-C6 companion that drives board2's D5 trigger (own README)
  README.md
vendor/
  esp-idf            # ESP-IDF toolchain (pinned submodule, v5.4.2) — the build uses this
  morse-firmware     # MM6108 firmware blobs (pinned submodule) — mm6108.bin is ELF->.mbin at build time
```

Builds are kept **out-of-source**: `make build APP=foo BOARD=bar` writes to
`build/foo/bar/` at the repo root, never inside `firmware/foo/`. Each app+board
gets its own build dir and generated `sdkconfig`, so switching boards never
reuses a stale config.

## Prerequisites

- **ESP-IDF v5.4.2**, vendored as a git submodule at `vendor/esp-idf` (v5.4.2 is
  the minimum required by the `morsemicro/halow` component). Check it out with its
  own submodules, then install the toolchain from it:
  ```bash
  git submodule update --init vendor/esp-idf          # if not cloned --recurse-submodules
  git -C vendor/esp-idf submodule update --init --recursive
  ./vendor/esp-idf/install.sh esp32s3 esp32c6         # esp32c6 target only needed for c6-harness
  # cmake/ninja are not bundled on Linux; pin cmake to 3.x (cmake 4 breaks IDF):
  ~/.espressif/python_env/idf5.4_py*_env/bin/pip install "cmake==3.30.5" ninja
  ```
  The Makefile's default `IDF_PATH` points at `vendor/esp-idf`, so you don't need
  to set it (override `IDF_PATH=...` to use a different install).
- The MM6108 components (`morsemicro/halow` + `morse-firmware`) are **vendored as
  git submodules** — not downloaded from the registry. Get them when cloning:
  ```bash
  git clone --recurse-submodules <repo>          # or, in an existing clone:
  git submodule update --init components/halow vendor/morse-firmware
  ```
  The firmware image is generated at build time from `vendor/morse-firmware`
  (ELF → `.mbin`, see `cmake/mm-fw-gen/`). **Chip firmware version is set by that
  submodule** — the bench standard is **1.17.8** (`mm6108.bin` = `fd41e1c`); keep
  it matched to the Linux nodes (see
  [`../docs/reference/rimba-bench-devices.md`](../docs/reference/rimba-bench-devices.md)).

## Build / flash / monitor

The board enumerates over its **native USB-Serial-JTAG** as `/dev/ttyACM*`; that is
used for both flashing and the console. Run from the **repo root**. `APP` selects
the example and `BOARD` selects the board config under `boards/`:

```bash
make build APP=rimba-halow-scan BOARD=proto1-fgh100m       # build
make flash APP=rimba-halow-scan BOARD=proto1-fgh100m PORT=/dev/ttyACM0
make monitor APP=rimba-halow-scan PORT=/dev/ttyACM0        # serial console (Ctrl-] to quit)
make build APP=rimba-hello                                 # the radio-free sanity example
```

`BOARD=proto1-fgh100m` is the current HaLow bench board (XIAO ESP32-S3 + FGH100M).
Override `APP`, `BOARD`, `TARGET`, `PORT`, or `IDF_PATH` on the command line,
e.g. `make flash PORT=/dev/ttyACM1`. Run `make help` for the full list.

## The apps

### Bring-up / sanity

- **`rimba-hello`** — radio-free sanity check: chip info, an 8 MB PSRAM read/write
  test (RISK-04), SRAM headroom, heartbeat. Confirms the toolchain + flash pipeline,
  and is the **radio-silence image** flashed to the ESPs after every bench test.
- **`rimba-halow-scan`** — boots the MM6108 over SPI (loads firmware + BCF), prints
  version/chip info, and scans for HaLow APs. Board-specific SPI pins, BCF/firmware,
  and country code live under [`boards/<BOARD>/`](../boards/); **`CONFIG_HALOW_COUNTRY_CODE`
  is required** (defaults to `"??"`, which fails the scan — valid: `AU CA EU GB IN JP KR NZ US`).

### AP ↔ STA link + throughput

A bidirectional ICMP ping / iperf over an 802.11ah AP↔STA link — the proven
two-node path. Flash one board as the SoftAP and the other as a station:

```bash
make flash APP=rimba-halow-ap  BOARD=proto1-fgh100m PORT=/dev/ttyACM0   # SoftAP, static 192.168.12.1
make flash APP=rimba-halow-sta-perf BOARD=proto1-fgh100m PORT=/dev/ttyACM1   # STA + iperf
```

Two non-obvious details, both handled in the apps:
- **Static IPs, no DHCP.** `mmhalow` gives even the AP a DHCP-*client* netif and runs
  no DHCP server, so each side pins a static IP.
- **The AP must bring its netif up.** In AP mode `mmhalow` never fires a link-up event,
  so `rimba-halow-ap` calls `esp_netif_action_connected` explicitly — that one line is
  what makes the AP reachable over IP.

`rimba-halow-ap` carries `CONFIG_HALOW_AP_MODE=y`; STA apps need none (STA is the
default). The **`-perf`** variants (`rimba-halow-ap-perf`, `rimba-halow-sta-perf`,
`rimba-halow-mesh-perf`) add an **iperf** server/client for throughput measurement.

> Note: `rimba-halow-sta` itself is **not** a ping STA — it is the power-save ladder
> (below). Use `rimba-halow-sta-perf` for the plain link/throughput test.

### Mesh

- **`rimba-halow-mesh`** — an **802.11s secured-mesh** node: brings up a mesh vif
  (Mesh ID + Mesh Config IEs, following `morse_driver`'s mesh BSS flow), joins the
  secured mesh (**SAE + AMPE**, host software-CCMP), pins a static mesh IP, and
  forwards (HWMP + group-addressed). Single- and multi-hop relay are on-air verified.
  Background + the line-by-line Linux code-map are in
  [`../docs/mesh-ap/rimba-mesh-ap-milestones.md`](../docs/mesh-ap/rimba-mesh-ap-milestones.md).
- **`rimba-halow-ibss`** — the RISK-01 **802.11ah IBSS** (peer-to-peer, no AP). IBSS
  support is added in the vendored `components/halow` (adopted from
  `momentary-systems/esp-halow-ibss`). **One binary runs on every node** — the
  create/join role is a MAC heuristic and each node derives its IP from its MAC
  (`192.168.13.<mac[5]>`), so the same image scales to N boards. Validated: 2- and
  3-board full mesh, peer age-out + rejoin, and interop with a Linux `morse_driver`
  IBSS node. Peer discovery is **data-driven** (learned from data-frame source
  addresses), not beacon-based. See
  [`../docs/ibss/rimba-ibss-milestones.md`](../docs/ibss/rimba-ibss-milestones.md).

### Power-save & sleep characterization

The apps behind the STA power-save study — full method, numbers, and topology in
[`../docs/reference/rimba-halow-ps-esp32-vs-linux-ap.md`](../docs/reference/rimba-halow-ps-esp32-vs-linux-ap.md).
board2 (the DUT) is powered + metered by a Nordic PPK2 and triggered over one wire
by the `c6-harness`. **STA pitfall shared by all of these:** `mmhalow_init()`
force-disables power-save (`mmhalow.c:200`) — every app re-enables it with
`mmwlan_set_power_save_mode(MMWLAN_PS_ENABLED)` after connect.

| App | What it measures |
|---|---|
| `rimba-halow-sta` | the **triggered PS ladder** — No-PS → Dynamic PS → TWT → WNM+powerdown, fixed 18 s phases, C6-triggered; `HOST_LIGHT_SLEEP` flag toggles §3a (awake) vs §3c (host light-sleep) |
| `rimba-twt-assoc` | **TWT via the assoc-embedded path** (`mmwlan_twt_add_configuration` before connect) — engages on the Linux AP too, unlike the mid-session action |
| `rimba-doze-hold` | "deep-sleep without powering off the radio" — hold the STA **associated** while it dozes (TWT, then WNM + chip-powerdown) |
| `rimba-standby-test` | `MMWLAN_STANDBY` feasibility (**deprecated** — chip stays active, loses to WNM+powerdown) |
| `rimba-deepsleep-cycle` | deep-sleep as a **duty-cycled leaf**: hold the radio in `RESET_N`-low + ESP32 deep sleep, wake on the C6 trigger, **re-associate** (~5.1 s) |
| `rimba-downlink-test` | **downlink-while-dozing** (§3b): board2 becomes a pingable STA (`192.168.12.51`), dozes in Dynamic PS while the AP pings at 1 Hz |
| `rimba-sleep-test` | board2's **lowest-power floor** (`RESET_N`-low + ESP32 deep sleep ≈ 0.35–0.6 mA) |

### Measurement harness

- **`c6-harness`** — an **ESP32-C6** companion (not the S3): drives board2's D5 pin
  to trigger the PS ladder and time the phase markers over a single wire. Built for
  the `esp32c6` target; has its own [`c6-harness/README.md`](c6-harness/README.md).

## Hardware notes

- **MCU:** ESP32-S3R8 — 16 MB QIO flash, 8 MB **octal (OPI)** PSRAM (mandatory, RISK-04).
- **Radio:** Morse Micro MM6108 over SPI (`proto1-fgh100m` = XIAO ESP32-S3 + FGH100M;
  `proto1` = XIAO + Seeed HaLow add-on `mm6108-mf16858`), chip ID `0x0306`.
- **Chip firmware:** bench standard **1.17.8** (`mm6108.bin` = `fd41e1c`), bundled from
  `vendor/morse-firmware` and kept matched to the Linux nodes — see
  [`../docs/reference/rimba-bench-devices.md`](../docs/reference/rimba-bench-devices.md).
