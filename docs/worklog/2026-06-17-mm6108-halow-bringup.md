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
maintained as managed components. So we consume that code instead of porting
anything — and have since **vendored it locally** rather than fetching from the
registry (see *Vendoring the Morse components* below). **No HAL port needed.**

Consequences:
- **ESP-IDF upgraded 5.2.3 → 5.4.2.** All published component versions
  (`2.9.7-esp32-3`, `2.10.4-esp32-1/-2`) require `idf >=5.4.2`. Installed
  v5.4.2 at `~/esp/esp-idf-5.4.2`; the Makefile default `IDF_PATH` now points
  there. (5.2.3 left in place but unused.)
- `vendor/mm-iot-sdk` (submodule) is now **reference-only** — the build pulls
  morselib + blobs from the vendored components, not this submodule. Kept for its
  full docs and the complete BCF set.

---

## What was built — `firmware/rimba-halow-scan/`

Based on the component's own `examples/scan` (`app_main.c`, clean high-level API:
`mmhalow_init()` / `mmhalow_scan()` / `mmhalow_print_version_info()`), with a
Seeed XIAO board overlay. (That overlay was later moved to `boards/proto1/` and
selected via the `BOARD` make var — see *Build/flash* below.)

The `halow` + `firmware` components are vendored under
`components/ (repo root)` (see *Vendoring* below), so
`main/idf_component.yml` no longer lists registry dependencies — IDF picks up the
local components automatically. `main` just declares `REQUIRES halow`.

### Board config that matters (`boards/proto1/sdkconfig.defaults`)

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

## Vendoring the Morse components

Instead of letting `idf_component_manager` fetch `morsemicro/halow` +
`morsemicro/firmware` from the ESP Component Registry into `managed_components/`
(gitignored, hash-guarded, clobbered on every `reconfigure`), the two packages are
**vendored as local components** under `components/ (repo root)`:

| Component | Path | Submodule remote | Pinned |
|---|---|---|---|
| `halow` (driver + morselib) | `components/halow` | `teapotlaboratories/mm-esp32-halow` | `2.10.4-esp32-2` |
| `firmware` (mm6108/mm8108 blobs) | `components/firmware` | `teapotlaboratories/mm-esp32-firmware` | `2.10.4-esp32-2` |

Each is its own git repo, tracked from the main repo as a submodule (first commit =
pristine upstream, so later commits are clean diffs of our changes). Why:

- **Reproducibility insurance** — the build no longer depends on the registry (or a
  specific published version) staying available; the exact source lives on remotes
  we control.
- **Track local modifications** — once we start patching morselib/mmhalow (e.g. for
  the IBSS / raw-L2 work), every edit is a reviewable commit against pristine
  upstream.

The repo-root `components/` dir is added to the build via `EXTRA_COMPONENT_DIRS`
in `firmware/rimba-halow-scan/CMakeLists.txt`; IDF then uses these in place of the
registry packages of the same name (a *local-component override* — confirmed in
the configure log: `Using component placed at .../components/firmware for
dependency "morsemicro/firmware"`). No network, no `managed_components/`. The
build output is byte-for-byte identical to the registry-fetched build.

`vendor/mm-iot-sdk` is a *different* git ref (the upstream SDK source) and is kept
only as reference; the vendored components above are the registry-packaged
`-esp32-` build, which is what actually compiles.

---

## Build/flash

```
git clone --recurse-submodules …       # or: git submodule update --init
make build                             # default APP rimba-halow-scan, BOARD proto1
make flash                             # /dev/ttyACM0
```
`rimba-halow-scan` is now the default `APP`, and `BOARD` (default `proto1`)
selects `boards/<BOARD>/sdkconfig.defaults` via `SDKCONFIG_DEFAULTS`; output goes
to `build/<APP>/<BOARD>/`. Image: `rimba_halow_scan.bin` ~1.23 MB (20% partition
free). The vendored `components/{halow,firmware}` must be checked out (submodules)
for the build to resolve.

## Next steps

1. **IBSS / RISK-01 (tasks 1.2–1.3):** a first pass through the vendored morselib
   source (`components/halow/.../morselib/include/mmwlan.h`) found:
   - **Raw L2 TX/RX is reachable** — `mmwlan_tx_pkt()` takes a packet that starts
     with an 802.3 header (`DST | SRC | ETHERTYPE | payload`, translated to 802.11),
     so we can set EtherType `0x88B5` directly; `mmwlan_register_rx_cb()` /
     `_rx_pkt_cb()` deliver RX frames with their 802.3 header, per-VIF. This is the
     Rimba datapath.
   - **No public ADHOC/IBSS** — the low-level `mmwlan` API exposes only STA
     (`mmwlan_sta_enable`) and AP (`mmwlan_ap_enable`). IBSS code exists only inside
     the bundled `wpa_supplicant` (hostap), with no evidence the Morse firmware
     exposes an ADHOC opmode. `mmwlan_boot()` only idles the chip; TX/RX needs an
     active VIF/BSS context.
   - **Implication:** raw `0x88B5` frames work, but ride on an associated STA↔AP
     link, not a peer IBSS — i.e. the **RISK-01 fallback (AP-STA) is the practical
     path** unless an ADHOC opmode can be reached. Next: confirm raw TX/RX over a
     live STA↔AP link before deciding whether to keep probing IBSS.
2. Two-board test: build a second node, attempt STA↔AP first (known to work) to
   confirm a live link, then pursue IBSS.
3. Measure MM6108 boot time (RISK-02 / task 1.4) once we control the boot path.

## Open question for Aldwin

- The Seeed wiki/schematic call the XIAO module "Quectel FGH100M-H", but the
  chip's own BCF reports `mm6108-mf16858` and that BCF works. Confirmed good
  enough for digital + scan. Flag only if RF range/regulatory later looks off.
