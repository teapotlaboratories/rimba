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
  proto1/              # board config (Seeed XIAO ESP32-S3 Plus + HaLow add-on)
    sdkconfig.defaults
    sdkconfig.defaults.esp32s3
components/            # vendored Morse Micro components (git submodules), shared by apps
  halow/               #   morsemicro/halow  — morselib driver  (repo: mm-esp32-halow)
  firmware/            #   morsemicro/firmware — MM6108 blobs    (repo: mm-esp32-firmware)
firmware/
  rimba-hello/         # Phase-1 sanity example (toolchain + PSRAM/SRAM validation, no radio)
    main/rimba_hello.c
  rimba-halow-scan/    # MM6108 bring-up: boots the HaLow radio and scans for APs
    main/app_main.c
    CMakeLists.txt     # adds ../../components via EXTRA_COMPONENT_DIRS
  rimba-halow-ap/      # 2-board ping test: HaLow SoftAP node
  rimba-halow-sta/     # 2-board ping test: HaLow station node
  rimba-halow-ibss/    # IBSS / ad-hoc mesh (RISK-01): N-node addressing, peer discovery, ping
  README.md
vendor/
  esp-idf            # ESP-IDF toolchain (pinned submodule, v5.4.2) — the build uses this
  mm-iot-sdk         # Morse Micro SDK (pinned submodule) — reference only; the
                     # build uses the vendored components/, not this tree
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
  ./vendor/esp-idf/install.sh esp32s3
  # cmake/ninja are not bundled on Linux; pin cmake to 3.x (cmake 4 breaks IDF):
  ~/.espressif/python_env/idf5.4_py*_env/bin/pip install "cmake==3.30.5" ninja
  ```
  The Makefile's default `IDF_PATH` points at `vendor/esp-idf`, so you don't need
  to set it (override `IDF_PATH=...` to use a different install).
- The MM6108 components (`morsemicro/halow` + `morsemicro/firmware`) are
  **vendored as git submodules** under `components/` — not downloaded from the
  registry. Get them when cloning:
  ```bash
  git clone --recurse-submodules <repo>          # or, in an existing clone:
  git submodule update --init components/halow components/firmware
  ```
  The `rimba-halow-scan` project adds `components/` to the build via
  `EXTRA_COMPONENT_DIRS`. The `vendor/mm-iot-sdk` submodule is separate, optional
  reference (`git submodule update --init vendor/mm-iot-sdk`).

## Build / flash / monitor

The board (Seeed XIAO ESP32-S3 Plus) enumerates over its **native USB-Serial-JTAG**
as `/dev/ttyACM0`; that is used for both flashing and the console.

Run from the **repo root**. `APP` selects the example (default `rimba-halow-scan`)
and `BOARD` selects the board config under `boards/` (default `proto1`):

```bash
make build                           # rimba-halow-scan on board proto1 (defaults)
make flash                           # flash to /dev/ttyACM0
make monitor                         # serial console (Ctrl-] to quit)
make flash-monitor
make build APP=rimba-hello           # the radio-free sanity example
make build BOARD=proto2              # a different board (boards/proto2/)
```

Override `APP`, `BOARD`, `TARGET`, `PORT`, or `IDF_PATH` on the command line,
e.g. `make flash PORT=/dev/ttyACM1`. Run `make help` for the full list.

## The examples

### `rimba-hello`
Radio-free sanity check: chip info, an 8 MB PSRAM read/write test (RISK-04),
SRAM headroom, heartbeat. Use it to confirm the toolchain + flash pipeline.

### `rimba-halow-scan`
Boots the MM6108 over SPI (loads firmware + BCF), prints version/chip info, and
scans for HaLow APs. Built on the vendored `halow` component (`components/halow`).

Board-specific config lives under [`boards/<BOARD>/`](../boards/) — for the
default `proto1` board, [`boards/proto1/sdkconfig.defaults`](../boards/proto1/sdkconfig.defaults):

- **XIAO HaLow SPI/control pins** — the component's own defaults are a different
  board, so these overrides are required:
  `RESET_N=1 WAKE=2 BUSY=5 IRQ=3 CS=4 SCK=7 MISO=8 MOSI=9`.
- **BCF/firmware:** `CONFIG_MM_BCF_FILE="bcf_mf16858.mbin"` (the chip reports
  `mm6108-mf16858`), `CONFIG_MM_FW_FILE="mm6108.mbin"`.
- **Country:** `CONFIG_HALOW_COUNTRY_CODE="US"` — **required**; it defaults to
  `"??"` and without a valid code the radio won't set a channel list and scan
  fails. Valid: `AU CA EU GB IN JP KR NZ US`.

### `rimba-halow-ap` + `rimba-halow-sta` (2-board ping test)

A bidirectional ICMP ping over an 802.11ah AP↔STA link — the proven two-node
path (the MM6108 has no public IBSS in morselib). Flash one board as the SoftAP
and the other as the station, on separate ports:

```bash
make flash APP=rimba-halow-ap  PORT=/dev/ttyACM0    # SoftAP, static 192.168.12.1
make flash APP=rimba-halow-sta PORT=/dev/ttyACM1    # STA,    static 192.168.12.2
make monitor APP=rimba-halow-sta PORT=/dev/ttyACM1  # watch "reply from … time=N ms"
```

Both nodes ping each other (~12 ms RTT, US 915.5 MHz / 1 MHz BW). Two
non-obvious details, both handled in the apps:

- **Static IPs, no DHCP.** `mmhalow` gives even the AP a DHCP-*client* netif and
  runs no DHCP server, so each side pins a static IP rather than leasing one.
- **The AP must bring its netif up.** In AP mode `mmhalow` never fires a link-up
  event, so it never calls `esp_netif_action_connected` — the AP netif stays
  down and lwIP silently drops ICMP. `rimba-halow-ap` calls it explicitly; that
  one line is what makes the AP reachable over IP.

`rimba-halow-ap` carries one app-level override
([`firmware/rimba-halow-ap/sdkconfig.defaults`](rimba-halow-ap/sdkconfig.defaults)):
`CONFIG_HALOW_AP_MODE=y`. `rimba-halow-sta` needs none (STA is the default mode).

### `rimba-halow-ibss` (the IBSS / ad-hoc mesh)

The RISK-01 result: an **802.11ah IBSS** (peer-to-peer, no AP). The MM6108 has no
public IBSS API in morselib, so the IBSS support is added in the vendored
`components/halow` (adopted from `momentary-systems/esp-halow-ibss`; bring-up,
peer table + age-out, datapath). **One binary runs on every node** — the
create/join role is a MAC heuristic (bench), and each node derives its IP from its
MAC (`192.168.13.<mac[5]>`) and pings every discovered peer, so the same image
scales to N boards.

```bash
make flash APP=rimba-halow-ibss BOARD=proto1-fgh100m PORT=/dev/ttyACM0   # node 1
make flash APP=rimba-halow-ibss BOARD=proto1-fgh100m PORT=/dev/ttyACM1   # node 2  (… ACM2 = node 3)
make monitor APP=rimba-halow-ibss PORT=/dev/ttyACM0                      # "reply from 192.168.13.N … IBSS DATA OK"
```

Validated on hardware: 2- and 3-board **full mesh**, peer age-out + drop/rejoin
resilience, and **interop with a Linux `morse_driver` IBSS node** on the same
silicon. Background, the Linux-equivalence map, and the on-air debugging (the #17
beacon phantom-flood; #16 data-driven discovery) are in
[`../docs/ibss/rimba-ibss-milestones.md`](../docs/ibss/rimba-ibss-milestones.md),
[`../docs/ibss/rimba-ibss-test-plan.md`](../docs/ibss/rimba-ibss-test-plan.md), and
[`../docs/worklog/2026-06-20-ibss-adoption-interop-phantom.md`](../docs/worklog/2026-06-20-ibss-adoption-interop-phantom.md).

Key discovery detail worth knowing: peer discovery is **data-driven** (learned from
data-frame source addresses), *not* beacon-based — the current MM6108 firmware
(1.17.6) doesn't surface same-cell peer beacons to the host, and `morse_driver`
beacons carry `SA=BSSID`. This is firmware-dependent (see the milestones doc).

## Hardware notes

- **MCU:** ESP32-S3R8 — 16 MB QIO flash, 8 MB **octal (OPI)** PSRAM (mandatory,
  RISK-04).
- **Radio:** Seeed Wi-Fi HaLow (MM6108, module `mm6108-mf16858`) over SPI.
  Confirmed booting — firmware v1.17.6, chip ID 0x0306.
