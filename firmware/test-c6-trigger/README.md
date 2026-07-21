# test-c6-trigger — ESP32-C6 companion for the board2 PS-measurement rig

The regression harness's D5 trigger fixture. An **ESP32-C6-DevKitC-1** drives board2's **D5** (XIAO
ESP32-S3 GPIO6) over a single wire from **C6 GPIO20** (+ common GND) to fire board2's PS ladder (the
`tp` tier's `test-power` DUT), wake the deep-sleep leaf (the `dscycle` tier's `test-deepsleep-cycle`
DUT), and drive the flash-hold guard. board2 is powered and
measured by the **PPK2**, not the C6 — the C6 is the digital/control side (the PPK2's logic pins are
input-only; the C6 can *drive* the pin).

Wiring, integrity rules, and the flash-hold guard: `docs/reference/rimba-bench-devices.md` →
**"Measurement harness"** and **"board2 won't flash — un-wedge it"**.

## Serial control (no re-flash to change what the C6 does)

The D5 behaviour is a **runtime-latched mode driven over the C6's UART0 console** (the CP2102N on
`/dev/ttyUSB0`) — you no longer re-flash the C6 to change modes. This is what lets the `dscycle` tier
**command a wake pulse on demand** (fire it at the right instant in board2's short awake window) instead
of relying on a free-running pulse that raced that window. Commands are newline-terminated, the first
token is case-insensitive, and responses are prefixed `C6|`:

| Command | Effect |
|---|---|
| `pulse [ms]` | one LOW pulse now (default 120 ms) then Hi-Z rest; sets mode=hiz. **The on-demand wake.** |
| `hiz` | tri-state D5 (measurement-safe rest; board2 boots via its own pull-down = run) |
| `high` | drive D5 HIGH steady → board2 boots into **flash-hold** (remote reflash / wedge recovery) |
| `low` | drive D5 LOW steady → board2 boots into **run** |
| `trigger [period_s]` | free-running LOW pulse every period (default 30 s) — the legacy tp free-run |
| `toggle [half_ms]` | square wave, flip every half_ms (default 500 ms = 1 Hz) — wiring / link test |
| `save` | persist the current mode + params to NVS (survives a C6 reboot) |
| `default` | clear NVS → revert to the compile-time default on next boot |
| `status` | report mode / pin / params / nvs |
| `ping` | → `C6|PONG` (health check) |

**Power-on default = NVS (if `save`d) else the compile-time `MODE`** (`MODE_TRIGGER`), so an un-commanded
C6 behaves exactly as before — the `tp` tier's free-running trigger is unaffected. Opening the port can
auto-reset the C6 (DTR/RTS); the harness opens it once, tolerates one boot, and drops back to the Hi-Z
rest. Talk to it by hand with any serial console, e.g. `idf.py -p /dev/ttyUSB0 monitor` then type `status`.

## Build + flash

Standalone ESP-IDF — **not** the repo `make` (that targets ESP32-S3 only):

```sh
export IDF_PATH=<repo>/vendor/esp-idf && source $IDF_PATH/export.sh
idf.py set-target esp32c6 build
idf.py -p /dev/ttyUSB0 flash monitor        # C6 DevKitC-1 = CP210x on /dev/ttyUSB0
```

## Compile-time default (`MODE` in `main/main.c`)

Modes are selected at runtime over serial (table above); the compile-time `MODE` is only the **power-on
default when NVS has no saved mode** (`save` persists one). It maps 1:1 to the serial modes:

| `MODE` | serial equivalent | GPIO20 at boot |
|---|---|---|
| `MODE_TRIGGER` (default) | `trigger` | Hi-Z, pulse LOW ~120 ms every 30 s |
| `MODE_HOLD_HIGH` | `high` | driven HIGH (board2 → flash-hold) |
| `MODE_HOLD_LOW` | `low` | driven LOW (board2 → run) |
| `MODE_TOGGLE` | `toggle` | 1 Hz square wave (link test) |
| `MODE_HIZ` | `hiz` | tri-state |

## Verified

- **2026-07-07** — Link: board2 D5 tracks GPIO20 1:1 (`toggle`). Guard: `high` → board2 boots FLASH-HOLD;
  `low`/`hiz` → board2 runs. Trigger → ladder: a pulse runs board2's P1–P4 ladder, back to idle.
- **2026-07-21** — Serial control added: the `dscycle` tier commands `hiz` + `pulse` on demand (see its
  worklog). `ping`/`status`/`pulse`/`hiz`/`high`/`low`/`trigger`/`toggle`/`save`/`default` over UART0.
