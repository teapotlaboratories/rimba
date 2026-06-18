# Rimba firmware

ESP32-S3 + Morse Micro MM6108 (Wi-Fi HaLow) firmware for the Rimba DTN mesh.
See [`../docs/rimba-development-plan.md`](../docs/rimba-development-plan.md) for
the phased build order. This tree currently holds the **Phase 1 bring-up**
example.

## Layout

```
Makefile            # (repo root) convenience wrapper around idf.py (build/flash/monitor)
build/<APP>/        # (repo root) out-of-source build output, e.g. build/rimba-hello/ (gitignored)
firmware/
  rimba-hello/      # Phase-1 bring-up example (toolchain + PSRAM/SRAM validation)
    main/rimba_hello.c
    sdkconfig.defaults[.esp32s3]
  README.md
vendor/mm-iot-sdk   # MM6108 morselib SDK (pinned submodule) — for the HaLow port
```

Builds are kept **out-of-source**: `make build APP=foo` writes to `build/foo/` at
the repo root, never inside `firmware/foo/`. Override with `BUILD_DIR=...`.

## Prerequisites

- **ESP-IDF v5.2.3**, installed out-of-tree (not committed here):
  ```bash
  git clone -b v5.2.3 --depth 1 --recursive https://github.com/espressif/esp-idf ~/esp/esp-idf
  ~/esp/esp-idf/install.sh esp32s3
  ```
- The MM6108 SDK submodule:
  ```bash
  git submodule update --init vendor/mm-iot-sdk
  ```

## Build / flash / monitor

The board (Seeed XIAO ESP32-S3 Plus) enumerates over its **native USB-Serial-JTAG**
as `/dev/ttyACM0`; that is used for both flashing and the console.

Run from the **repo root**:

```bash
make build                 # build rimba-hello
make flash                 # flash to /dev/ttyACM0
make monitor               # serial console (Ctrl-] to quit)
make flash-monitor         # flash then monitor
```

Override `APP`, `TARGET`, `PORT`, or `IDF_PATH` on the command line, e.g.
`make flash PORT=/dev/ttyACM1`. Run `make help` for the full list.

If you prefer raw `idf.py`:

```bash
. ~/esp/esp-idf/export.sh
cd firmware/rimba-hello && idf.py -p /dev/ttyACM0 flash monitor
```

## Hardware notes

- **MCU:** ESP32-S3R8 — 16 MB QIO flash, 8 MB **octal (OPI)** PSRAM. PSRAM
  config is mandatory (RISK-04); `rimba-hello` runs a read/write PSRAM test to
  confirm it.
- **Radio:** Seeed Wi-Fi HaLow (MM6108) over SPI. **Not yet driven** — the
  morselib HAL must be ported to ESP-IDF first. See the worklog under
  [`../docs/worklog/`](../docs/worklog/) for the status and plan.
