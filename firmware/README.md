# Rimba firmware

ESP32-S3 + Morse Micro MM6108 (Wi-Fi HaLow) firmware for the Rimba DTN mesh.
See [`../docs/rimba-development-plan.md`](../docs/rimba-development-plan.md) for
the phased build order, and [`../docs/worklog/`](../docs/worklog/) for the
bring-up log.

## Layout

```
Makefile             # (repo root) convenience wrapper around idf.py (build/flash/monitor)
build/<APP>/         # (repo root) out-of-source build output, e.g. build/rimba-hello/ (gitignored)
firmware/
  rimba-hello/       # Phase-1 sanity example (toolchain + PSRAM/SRAM validation, no radio)
    main/rimba_hello.c
  rimba-halow-scan/  # MM6108 bring-up: boots the HaLow radio and scans for APs
    main/app_main.c
    main/idf_component.yml   # pulls morsemicro/halow + morsemicro/firmware
  README.md
vendor/mm-iot-sdk    # Morse Micro SDK (pinned submodule) — reference only; the
                     # build uses the published components, not this tree
```

Builds are kept **out-of-source**: `make build APP=foo` writes to `build/foo/` at
the repo root, never inside `firmware/foo/`. Override with `BUILD_DIR=...`.

## Prerequisites

- **ESP-IDF v5.4.2**, installed out-of-tree (not committed here). v5.4.2 is the
  minimum required by the `morsemicro/halow` component.
  ```bash
  git clone -b v5.4.2 --depth 1 --recursive https://github.com/espressif/esp-idf ~/esp/esp-idf-5.4.2
  ~/esp/esp-idf-5.4.2/install.sh esp32s3
  # cmake/ninja are not bundled on Linux; pin cmake to 3.x (cmake 4 breaks IDF):
  ~/.espressif/python_env/idf5.4_py*_env/bin/pip install "cmake==3.30.5" ninja
  ```
- The MM6108 components are fetched automatically by the ESP-IDF component
  manager on first build (from each app's `main/idf_component.yml`). No manual
  step. The `vendor/mm-iot-sdk` submodule is optional reference:
  `git submodule update --init vendor/mm-iot-sdk`.

## Build / flash / monitor

The board (Seeed XIAO ESP32-S3 Plus) enumerates over its **native USB-Serial-JTAG**
as `/dev/ttyACM0`; that is used for both flashing and the console.

Run from the **repo root** (`APP` selects which example):

```bash
make build APP=rimba-halow-scan      # build (default APP is rimba-hello)
make flash APP=rimba-halow-scan      # flash to /dev/ttyACM0
make monitor                         # serial console (Ctrl-] to quit)
make flash-monitor APP=rimba-halow-scan
```

Override `APP`, `TARGET`, `PORT`, or `IDF_PATH` on the command line, e.g.
`make flash PORT=/dev/ttyACM1`. Run `make help` for the full list.

## The examples

### `rimba-hello`
Radio-free sanity check: chip info, an 8 MB PSRAM read/write test (RISK-04),
SRAM headroom, heartbeat. Use it to confirm the toolchain + flash pipeline.

### `rimba-halow-scan`
Boots the MM6108 over SPI (loads firmware + BCF), prints version/chip info, and
scans for HaLow APs. Built on the published `morsemicro/halow` component.

Board-specific config lives in [`rimba-halow-scan/sdkconfig.defaults`](rimba-halow-scan/sdkconfig.defaults):

- **XIAO HaLow SPI/control pins** — the component's own defaults are a different
  board, so these overrides are required:
  `RESET_N=1 WAKE=2 BUSY=5 IRQ=3 CS=4 SCK=7 MISO=8 MOSI=9`.
- **BCF/firmware:** `CONFIG_MM_BCF_FILE="bcf_mf16858.mbin"` (the chip reports
  `mm6108-mf16858`), `CONFIG_MM_FW_FILE="mm6108.mbin"`.
- **Country:** `CONFIG_HALOW_COUNTRY_CODE="US"` — **required**; it defaults to
  `"??"` and without a valid code the radio won't set a channel list and scan
  fails. Valid: `AU CA EU GB IN JP KR NZ US`.

## Hardware notes

- **MCU:** ESP32-S3R8 — 16 MB QIO flash, 8 MB **octal (OPI)** PSRAM (mandatory,
  RISK-04).
- **Radio:** Seeed Wi-Fi HaLow (MM6108, module `mm6108-mf16858`) over SPI.
  Confirmed booting — firmware v1.17.6, chip ID 0x0306.
