# test-raw-cap — Firmware advertises RAW / page slicing (RAW S0a)

**Status: hardware-verified, NOT wired into a scored tier** (2026-07-30, board0, fw 1.17.8). This is a
**feasibility probe**, not a regression test — see "How it is scored" below before adding it to a tier.

## Rig

| role | device | app |
|---|---|---|
| dut | any single board | `test-raw-cap` |

No peer, no sniffer, no Linux reference, no beacon hook.

## What it proves / does not prove

- **Proves:** whether the shipped `.mbin` claims `MORSE_CAPS_RAW` / `MORSE_CAPS_PAGE_SLICING`.
- **Does NOT prove:** that the chip acts on an **AP-authored** RAW schedule. The capability word
  carries **no direction**, and STA-side RAW was already believed working — so a set bit is
  **necessary, not sufficient**. A **clear** bit is the decisive outcome: strong evidence RAW is
  absent from the shipped image.

Do not read "enforcement" into RAW: 802.11ah has no AP primitive that polices peers — a STA restricts
*itself* on parsing the RPS, and the chip's own RAW counters only ever count the **local** transmitter
deferring its own frames. A test written against AP-policing reports failure for a feature that works.

## How it is scored — the READ, not the value

`TEST_STEP` passes when both accessors return `>= 0`, i.e. **morselib was initialised and the read is
valid**. It does **not** assert the bits are set.

That is deliberate. *"FW does not advertise RAW"* is a legitimate feasibility answer, not something
that broke, so a clear bit reports **PASS** with a loud `BLOCKED-SIGNAL` info line rather than FAIL.
Scoring a correct negative as a failure trains everyone to ignore the suite.

**The rule this establishes:** score the READ while the value is unknown; score the VALUE once it is
an established baseline. Now that S0a has fixed `MORSE_CAPS_RAW=1` as the baseline on fw 1.17.8, a
stack/SDK/blob bump that silently clears it *is* a regression — so converting this to a value-scored
T2 test (the `test-ampdu-cap` pattern) is the right follow-up, and only then does it belong in a tier.

## Why an AP vif

The capabilities are **device-global**, not per-vif — `mmdrv_get_capabilities()` is called with
`MMDRV_VIF_ID_INVALID` and `umac_interface_get_capabilities()` takes the global `umac_data` — so any
vif would do for the read. An AP vif is used anyway: RAW is an AP-direction feature, and if that
global-vs-per-vif reading is ever wrong, an AP vif still returns the right answer where a mesh vif
might not. Requires `CONFIG_HALOW_AP_MODE=y`.

Some vif must be up for the fetch to have happened, so the probe reads **after** `mmhalow_wifi_start()`.

## How to run

```sh
make build APP=test-raw-cap BOARD=proto1-fgh100m
make flash-monitor APP=test-raw-cap BOARD=proto1-fgh100m PORT=/dev/ttyACM0
```

Not in a `make test-t2` tier (see above). Return the board to `rimba-hello` afterwards — the automatic
radio-silencing only applies to harness-driven runs.

## Result on fw 1.17.8

```
TEST|INFO|MORSE_CAPS_RAW=1  MORSE_CAPS_PAGE_SLICING=1
TEST|STEP|cap-read|PASS|raw=1 page_slicing=1
TEST|RESULT|PASS|FW advertises MORSE_CAPS_RAW — S0a answered, S0b is justified
```

## Firmware

`firmware/test-raw-cap/` — single board; brings up an AP vif, reads
`mmwlan_{raw,page_slicing}_capability_advertised()` (added in the `components/halow` submodule).

Design doc: `docs/design-specification/rimba-raw-apside-design.md`.
