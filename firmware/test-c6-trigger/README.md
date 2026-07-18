# test-c6-trigger — ESP32-C6 companion for the board2 PS-measurement rig

The regression harness's D5 trigger fixture. An **ESP32-C6-DevKitC-1** drives board2's **D5** (XIAO
ESP32-S3 GPIO6) over a single wire from **C6 GPIO20** (+ common GND) to fire board2's PS ladder (the
`tp` tier's `test-power` DUT), wake the deep-sleep leaf (the `dscycle` tier's `test-deepsleep-cycle`
DUT), and drive the flash-hold guard. board2 is powered and
measured by the **PPK2**, not the C6 — the C6 is the digital/control side (the PPK2's logic pins are
input-only; the C6 can *drive* the pin).

Wiring, integrity rules, and the flash-hold guard: `docs/reference/rimba-bench-devices.md` →
**"Measurement harness"** and **"board2 won't flash — un-wedge it"**.

## Build + flash

Standalone ESP-IDF — **not** the repo `make` (that targets ESP32-S3 only):

```sh
export IDF_PATH=<repo>/vendor/esp-idf && source $IDF_PATH/export.sh
idf.py set-target esp32c6 build
idf.py -p /dev/ttyUSB0 flash monitor        # C6 DevKitC-1 = CP210x on /dev/ttyUSB0
```

## Modes (set `MODE` in `main/main.c`)

| Mode | GPIO20 | board2 effect |
|---|---|---|
| `TRIGGER` (default) | Hi-Z, pulse LOW ~120 ms every 30 s | fires one ladder run per period |
| `HOLD_HIGH` | driven HIGH | boots into **flash-hold** (remote reflash / wedge recovery) |
| `HOLD_LOW` | driven LOW | boots into **run** |
| `TOGGLE` | 1 Hz square wave | wiring / link test (board2's D5 monitor tracks it) |

## Verified 2026-07-07 (via this harness)

- **Link**: board2 D5 tracks GPIO20 1:1 (TOGGLE mode).
- **Guard**: HOLD_HIGH → board2 boots FLASH-HOLD; HOLD_LOW/Hi-Z → board2 runs.
- **Trigger → ladder**: TRIGGER pulse → board2 runs P1–P4 (No-PS / Dyn-PS / TWT / WNM+powerdown), back to idle.

## Future

A host-controllable version (trigger on command over the C6 USB, and timestamp board2's phase-marker
pulses on D5) plus host-side alignment with the PPK2 current stream — see the bench doc's harness notes.
