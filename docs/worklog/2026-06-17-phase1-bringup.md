# Worklog — 2026-06-17 — Phase 1 bring-up start

**Author:** Aldwin
**Phase:** 1 — IBSS Foundation (BLOCKING)
**Goal of this session:** stand up the firmware project, the ESP-IDF toolchain,
and build + flash a first example firmware to the real hardware.

---

## Hardware / host environment

| Item | Value |
|---|---|
| Host | aarch64 Linux SBC (RK-series), 6 cores, 3.8 GB RAM, 53 GB free |
| Board | Seeed Studio **XIAO ESP32-S3 Plus** (ESP32-S3R8: 16 MB flash, 8 MB octal PSRAM) |
| Radio | Seeed **Wi-Fi HaLow** transceiver (Morse Micro **MM6108**) on the XIAO, SPI |
| Port | `/dev/ttyACM0` — board's **native USB-Serial-JTAG** (flash + console) |
| User | in `dialout` group → port accessible without sudo |

## Toolchain installed (out-of-tree, not committed)

- **ESP-IDF v5.2.3** at `~/esp/esp-idf` (`install.sh esp32s3`).
- ESP-IDF does **not** bundle `cmake`/`ninja` on Linux. No passwordless sudo here,
  so instead of `apt install`, they were installed into the IDF python env:
  `~/.espressif/python_env/idf5.2_py3.13_env/bin/pip install "cmake==3.30.5" ninja`.
  - **cmake pinned to 3.30.5 on purpose** — pip's latest is cmake 4.x, which
    drops pre-3.5 policy compatibility and breaks some ESP-IDF 5.2 component
    builds. Anyone reproducing this must avoid cmake 4.x with IDF 5.2.
- The repo's own Python venv (`~/Developments/venv/rimba`) is **not** used for the
  firmware build — ESP-IDF manages its own env.

---

## KEY FINDING — there is no supported ESP32 path for morselib

> **SUPERSEDED (same day, session 2):** Morse Micro *does* publish a maintained
> ESP32 port on the ESP Component Registry (`morsemicro/halow` +
> `morsemicro/firmware`). We consume that as a managed dependency — **no HAL port
> needed.** See [`2026-06-17-mm6108-halow-bringup.md`](2026-06-17-mm6108-halow-bringup.md).
> The analysis below is kept for history. It was correct that *the maintained
> `mm-iot-sdk` repo* has no ESP32 port; what it missed is the separately-published
> component.

This is the most important outcome of the session and it reshapes the Phase 1
plan, so it is recorded in full.

The development plan (Risk-01) assumes we drive the MM6108 from ESP32 via
`morselib` from **`mm-iot-sdk`**. On inspection:

- **`mm-iot-sdk` (v2.11.2)** — the current, maintained SDK — targets Morse Micro's
  own **STM32 reference boards only**. Its build platforms are
  `mm-ekh08-h753` (STM32H7), `u575`/`wb55` (STM32U5/WB), `mm6108-ekh05`,
  `mm8108-ekh05`. It bundles its own FreeRTOS, lwIP, mbedTLS. **There is no
  ESP32 port in it.**
- morselib is portable: it talks to the host through a **HAL** —
  `mmhal_wlan.h` (SPI/SDIO transport to the chip), `mmhal_os.h`, `mmhal_flash.h`,
  `mmosal.h`, etc. Porting to ESP32 means implementing these against ESP-IDF.
- The only ESP32 implementation of that HAL is the **deprecated**
  **`mm-iot-esp32`** repo (the user said: read it, don't depend on it). It is an
  "alpha port" of **mm-iot-sdk 2.10.4** to **ESP-IDF v5.1.1**, and it contains
  exactly the missing glue under `framework/mm_shims/`:
  `mmhal_wlan.c`, `mmhal_os.c`, `mmhal_core.c`, `mmosal_shim_freertos_esp32.c`,
  `mmhal_wlan_binaries.c`, `crypto_mbedtls_mm.c`, plus a `CMakeLists.txt` /
  `idf_component.yml` wrapping morselib + firmware blobs as an IDF component.

**Implication:** "Phase 1 task 1.1 — set up ESP-IDF + morselib build" is not a
one-day off-the-shelf step. Before any IBSS work (Risk-01) we must **port the
morselib HAL to ESP-IDF ourselves**, using `mm-iot-esp32/framework/mm_shims` as
the reference implementation but rehosting it on the current `mm-iot-sdk`
morselib (2.11.2) and a current ESP-IDF (5.2.3). This is new, real work that the
plan under-scoped. It does **not** change anything above L2 — the DTN/BPv7/mule/
routing stack is link-agnostic (see plan §RISK-01 fallback).

### Firmware blobs needed for the eventual port (present in mm-iot-sdk)

- `framework/morsefirmware/mm6108.mbin` — MM6108 firmware.
- `framework/morsefirmware/mm6108/bcfs/*.mbin` — Board Config Files. The Seeed
  HaLow module's correct BCF still needs to be identified (candidates include
  `bcf_mf08651_us.mbin` and `bcf_sx_sdmah.mbin`; **TBD — verify against the Seeed
  module before RF bring-up**, US 902–928 regulatory).

---

## What was actually built this session

`firmware/rimba-hello/` — a deliberately radio-free Phase-1 sanity firmware that
proves the toolchain + flash + console pipeline and validates what we *can*
validate without the HaLow port:

- Chip identity, cores, silicon revision, flash size, IDF version.
- **PSRAM presence + 1 MiB read/write test** → covers **RISK-04 / task 1.8**
  (8 MB octal PSRAM is mandatory for the bundle store / routing table).
- Internal SRAM free / min-ever headroom → Phase-1 gate "> 50 KB SRAM free".
- 5 s heartbeat with uptime + heap.

Project structure created:

```
Makefile                   # (repo root) idf.py wrapper: build / flash / monitor / size / ...
firmware/
  README.md
  rimba-hello/
    CMakeLists.txt, sdkconfig.defaults[.esp32s3]
    main/{rimba_hello.c, CMakeLists.txt}
vendor/mm-iot-sdk          # pinned submodule @ b61d4548 (v2.11.2)
docs/worklog/              # this log
.gitignore
```

> Update (same session): the `Makefile` was moved from `firmware/` to the **repo
> root** at Aldwin's request (`APP_DIR := firmware/$(APP)`), so `make build/flash/
> monitor` now run from the repo root. The root `README.md` gained a "Building the
> firmware" section (full from-scratch compile steps) and a "Two separate SDKs"
> table clarifying ESP-IDF (ESP32-S3 host MCU, mandatory) vs `mm-iot-sdk` (MM6108
> radio, not yet used). No firmware source changed.
>
> Update 2 (same session): builds are now **out-of-source**. The Makefile passes
> `idf.py -B <repo-root>/build/$(APP)`, so output goes to `build/rimba-hello/`
> (gitignored) instead of `firmware/rimba-hello/build/`. The stale in-app build dir
> was removed.

Key sdkconfig choices for the XIAO ESP32-S3 Plus: `ESP32S3` target, 16 MB QIO
flash, **octal PSRAM** (`SPIRAM_MODE_OCT`), console on **USB-Serial-JTAG**
(that's what `/dev/ttyACM0` is — no external UART bridge).

### Build / flash results — PASS

- `make build` → OK (`rimba_hello.bin` 0x314f0 = ~200 KB, 81% app partition free).
- `make flash` over `/dev/ttyACM0` → OK.
- esptool on connect independently confirmed: `ESP32-S3 (QFN56) rev v0.2`,
  `Embedded PSRAM 8MB`, `USB mode: USB-Serial/JTAG`, MAC `e0:72:a1:f8:ef:a4`.
- Console (captured from USB-Serial-JTAG):

```
rimba-hello: chip      : esp32s3, 2 core(s), rev v0.2
rimba-hello: flash     : 16 MB
rimba-hello: idf       : v5.2.3
rimba-hello: PSRAM    : initialized, 8 MB total, 8189 KB free heap
rimba-hello: PSRAM    : 1024 KB read/write test PASSED
rimba-hello: SRAM     : 374 KB free, 374 KB min-ever  (Phase-1 gate: >50 KB)
rimba-hello: PSRAM (RISK-04 / task 1.8): OK
rimba-hello: heartbeat 0  (uptime 1 s, heap 8532 KB)
```

**Phase-1 items cleared this session:** toolchain/build/flash/console pipeline
working on real hardware; **RISK-04 / task 1.8 (PSRAM)** validated in silicon and
software; SRAM headroom gate (>50 KB) passed (374 KB free).

---

## Next steps (recommended order)

1. ~~Confirm `rimba-hello` runs on hardware~~ — **DONE.** Flashed and verified
   live on the board (re-confirmed after the Makefile move; binary unchanged).
   PSRAM test PASSED, SRAM 374 KB free. Closes RISK-04 / task 1.8 empirically.
2. **Decide the morselib port strategy** (see KEY FINDING). Proposed: create
   `firmware/components/morselib/` as an ESP-IDF component that pulls morselib
   source from `vendor/mm-iot-sdk`, and `firmware/components/mm_shims/` holding
   our ESP-IDF HAL port (seeded from the deprecated `mm-iot-esp32` shims, updated
   to morselib 2.11.2 + IDF 5.2.3).
3. **Bring up the SPI link to the MM6108** and load `mm6108.mbin` + the correct
   Seeed BCF — first milestone is `mmwlan` init + a scan, mirroring the
   `mm-iot-esp32` `scan` example (then STA, then the ADHOC/IBSS reverse-engineering
   of Risk-01 task 1.3).
4. Only after a frame round-trips (task 1.7) do we measure IBSS boot time
   (Risk-02 / task 1.4).

## Open questions for Aldwin

- **Port scope:** OK to spend the up-front effort porting the morselib HAL to
  ESP-IDF (rehosting `mm-iot-esp32` shims onto mm-iot-sdk 2.11.2), vs. pinning to
  the older 2.10.4 that the deprecated ESP32 port already matches? Newer = more
  work but supported; older = faster but matches a deprecated alpha.
- **BCF:** which exact Seeed HaLow module variant is this (to pick the right
  `bcf_*.mbin` and confirm US regulatory)?
