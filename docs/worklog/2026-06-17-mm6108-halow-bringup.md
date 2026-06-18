# Worklog — 2026-06-17 (session 2) — MM6108 HaLow bring-up

**Author:** Aldwin (with Claude Code)
**Phase:** 1 — IBSS Foundation (BLOCKING), task 1.1 (SDK + morselib build)
**Goal:** pull real MM6108 code into a firmware and get the radio to boot.

**Result: SUCCESS.** The MM6108 boots over SPI on the XIAO ESP32-S3, loads its
firmware + BCF, and runs a HaLow scan.

---

## The key correction to session 1

Session 1 concluded there was *no supported ESP32 path* for morselib and that we
would have to port the HAL ourselves. **That is now superseded.** Aldwin found
that Morse Micro publishes the ESP32 port on the **ESP Component Registry**:

- https://components.espressif.com/components/morsemicro/halow  (the HAL/driver)
- https://components.espressif.com/components/morsemicro/firmware (the blobs)

This is the same code as the deprecated `mm-iot-esp32` repo, now packaged and
maintained as managed components. So we consume it as a dependency instead of
porting anything. **No HAL port needed.**

Consequences:
- **ESP-IDF upgraded 5.2.3 → 5.4.2.** All published component versions
  (`2.9.7-esp32-3`, `2.10.4-esp32-1/-2`) require `idf >=5.4.2`. Installed
  v5.4.2 at `~/esp/esp-idf-5.4.2`; the Makefile default `IDF_PATH` now points
  there. (5.2.3 left in place but unused.)
- `vendor/mm-iot-sdk` (submodule) is now **reference-only** — the build pulls
  morselib + blobs from the component, not the submodule. Kept for its full docs
  and the complete BCF set.

---

## What was built — `firmware/rimba-halow-scan/`

Based on the component's own `examples/scan` (`app_main.c`, clean high-level API:
`mmhalow_init()` / `mmhalow_scan()` / `mmhalow_print_version_info()`), with a
Seeed XIAO board overlay in `sdkconfig.defaults`.

`main/idf_component.yml`:
```yaml
dependencies:
  morsemicro/halow:    { version: "^2.10.4-esp32-2" }
  morsemicro/firmware: { version: "^2.10.4-esp32-2" }
```

### Board config that matters (sdkconfig.defaults)

- **XIAO HaLow SPI/control pins** (from the Seeed `mm-iot-esp32` fork's Kconfig
  defaults — the *upstream component* defaults are a different board, each pin
  +1, so these overrides are REQUIRED):

  | RESET_N | WAKE | BUSY | IRQ | CS | SCK | MISO | MOSI |
  |---|---|---|---|---|---|---|---|
  | GPIO1 | GPIO2 | GPIO5 | GPIO3 | GPIO4 | GPIO7 | GPIO8 | GPIO9 |

  `CONFIG_MM_RESET_N=1 … CONFIG_MM_SPI_MOSI=9`
- **BCF:** `CONFIG_MM_BCF_FILE="bcf_mf16858.mbin"`, FW `mm6108.mbin`,
  `CONFIG_MMHAL_CHIP_TYPE_MM6108=y`. **Empirically confirmed correct** — the chip
  reports `{"module": "mm6108-mf16858"}`. (The board is NOT the FGH100M variant
  the Seeed wiki mentions; the silicon's own BCF says mf16858.)
- **Country:** `CONFIG_HALOW_COUNTRY_CODE="US"`. This was the one non-obvious
  gotcha — it defaults to `"??"`, and without a valid code `mmhalow_init()` logs
  `Channel list not set`, version readback returns garbage, and scan fails.
  Valid codes in the bundled regdb: AU CA EU GB IN JP KR NZ US.
- `CONFIG_PARTITION_TABLE_SINGLE_APP_LARGE=y` (the 1.2 MB image doesn't fit the
  default app partition), console on USB-Serial-JTAG, 16 MB QIO flash, octal PSRAM.

`REQUIRES` for `main`: `halow nvs_flash esp_wifi` (component name is `halow`).

---

## Verified on hardware (console capture)

```
scan: Booted. Initializing Wi-Fi HaLow...
Morse Micro HaLow NetIF: Setting Channel List US
  BCF API version:         12.1.0
  BCF board description:   {"module": "mm6108-mf16858", ...
  Morselib version:        2.10.4
  Morse firmware version:  1.17.6
  Morse chip ID:           0x0306
Wi-Fi MAC address: 68:24:99:44:6b:b7
scan: Scan finished
```

All readbacks valid: stable chip ID `0x0306`, real firmware (1.17.6) + morselib
(2.10.4) versions, real MAC, US channel list. Scan completed with no APs found
(expected — nothing else is transmitting HaLow nearby). This proves: SPI wiring
correct, firmware+BCF load over SPI, regulatory domain configured, scan subsystem
functional.

**Phase-1 task 1.1 (ESP-IDF + morselib build) is effectively done**, and we have a
working `mmwlan` boot — the foundation for tasks 1.2–1.3 (the ADHOC/IBSS work).

---

## Build/flash

```
make build APP=rimba-halow-scan      # uses IDF 5.4.2 by default
make flash APP=rimba-halow-scan      # /dev/ttyACM0
```
Image: `rimba_halow_scan.bin` ~1.23 MB (20% partition free).

## Next steps

1. **IBSS / RISK-01 (tasks 1.2–1.3):** the published component exposes STA/AP
   (`mmhalow_connect`, `mmhalow_wifi_start`) but not ADHOC. Investigate whether
   morselib (now we have it as source under `managed_components/`) exposes
   `MORSE_CMD_INTERFACE_TYPE_ADHOC`, and whether we can reach raw L2 frame TX/RX
   (EtherType 0x88B5) — the real Phase-1 goal. If ADHOC isn't reachable, this is
   where the RISK-01 fallback (AP-STA) decision gets made.
2. Two-board test: build a second node, attempt STA↔AP first (known to work) to
   confirm a live link, then pursue IBSS.
3. Measure MM6108 boot time (RISK-02 / task 1.4) once we control the boot path.

## Open question for Aldwin

- The Seeed wiki/schematic call the XIAO module "Quectel FGH100M-H", but the
  chip's own BCF reports `mm6108-mf16858` and that BCF works. Confirmed good
  enough for digital + scan. Flag only if RF range/regulatory later looks off.
