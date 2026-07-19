# Stress-testing the regression harness + a serial-capture retry hardening

**2026-07-18 · bench-flow robustness — a characterized board2 USB-reenumeration flake + the fix**

After the reshaped/renamed suite came up green (see
[`2026-07-18-regression-run-and-apsta-ap-marker.md`](2026-07-18-regression-run-and-apsta-ap-marker.md)),
one T1 run had failed on a single serial-capture glitch that vanished on retry. Rather than wave it
away, I **stress-tested the harness itself** — looping the hardware-touching tiers many times with **no
retries**, to measure the real flake rate and decide whether it was a one-off or a standing bench issue.
It is a standing (low-rate) issue; this worklog characterizes it and adds a targeted fix.

## What triggered this

The full `make test-all BOARD_NAME=board2 AP=esp CYCLES=2` run was effectively all-green (T0 28/28,
T2 15/15, tp 4/4) **except** one T1 FAIL:

```
[FAIL]   25.4s  rimba-halow-sta @ proto1-fgh100m
        serial capture failed: device reports readiness to read but returned no data
        (device disconnected or multiple access on port?)
```

Re-running `make test-t1 BOARD_NAME=board2` → `rimba-halow-sta` **PASS**, T1 12/12. So the FAIL was a
transient, not a code fault — but "passed on retry" is not the same as "understood," and a passing retry
**silently overwrites** the failing baseline (`build/regtest/T1-latest.json` is latest-wins, no history),
so a flake leaves no durable trace. Worth pinning down.

## The stress loop

A driver (scratch `stress.sh`) that loops the bench-touching tiers and **counts every raw outcome, never
retrying** — the point is to *measure* flakiness, not to pass. Job mix: `t1:board2 ×8`, `t1:board0 ×1`,
`t1:board1 ×1`, `dscycle ×3` (13 iterations). Each T1 run flashes ~11 apps and does one serial capture
per flash.

**Clean-rail result: 0 flakes across the first 8 T1 runs (~96 serial captures on board2 + board0/board1).**
The specific app that flaked (`rimba-halow-sta`) did not reproduce. That alone shows the flash → boot →
serial-capture → port-reresolve → radio-silence pipeline is fundamentally sound.

**Then the flakes appeared** — but only *after* a dscycle iteration ran. Follow-up runs on a
dscycle-disturbed rail flaked repeatedly, on **different, random apps** each time:

| When | App that flaked | Note |
|---|---|---|
| Full `test-all` run | `rimba-halow-sta` | first sighting |
| Fill run 1 (post-dscycle) | `rimba-halow-mesh-ap` | different app |
| Fill run 2 (post-dscycle) | `rimba-hello` | the **radio-free** do-nothing app |
| Settled-rail run 2 | `rimba-halow-sta` | recurred even after the rail quieted |

Across ~14 board2 T1 runs (~168 captures): **~4 serial-capture flakes on 4 distinct app instances** —
roughly **2–3 % per capture, ~1 in 4 runs hitting at least one**. **Not app-specific** (it hit the
trivial `rimba-hello` as readily as the radio apps).

## Root cause — board2 transient USB re-enumeration

The error string is **pyserial's own** `SerialException`, raised from `read()` when `select()` reports
the fd readable but the read returns 0 bytes — the textbook signature of a USB device **disconnecting and
re-enumerating** mid-read. I caught one directly: board2's `/dev/ttyACM4` node was recreated (new inode /
timestamp) at idle with no test running.

- **board2 is PPK2-powered** (the DUT rail, held by `tools/ppk2_hold.py`). A brief rail wobble resets the
  ESP → its USB-serial-JTAG drops and re-enumerates (often **renumbered** to a different `/dev/ttyACM*`).
- **board0/board1 are bus-powered** and showed 0 flakes — consistent with the rail being the variable.
- **Correlated with dscycle.** `dscycle` power-cycles the PPK2 rail (its own docstring,
  `tools/regtest/dscycle.py:20`, warns power-cycling *"destabilised the rail"*). 0/96 flakes on the
  long-settled initial holder; clustered flakes right after dscycle churned the supply. It is not
  *exclusively* post-dscycle (settled-rail run 2 flaked too) — the base rate is low but nonzero.

At idle the node is otherwise stable (same inode for 100 s), so this is an occasional transient, not
continuous churn.

## The harness gap

`common.capture_serial` / `capture_until` did a bare `s.readline()` loop
(`tools/regtest/common.py`); a mid-capture `SerialException` propagated straight up and the tier recorded
a **hard, gating FAIL**. But a genuinely dead radio fails *deterministically* (fails again on retry),
whereas this transient **always clears on the next attempt**. So the harness could not distinguish "the
board glitched on the USB bus" from "the firmware is broken" — and defaulted to failing the run.

## The fix — tolerate one re-enumeration, log it, still FAIL on a real fault

`tools/regtest/common.py`: a shared retry-aware `_capture()` core (used by both `capture_serial` and
`capture_until`), plus `_await_port()`:

- On a mid-capture `SerialException`/`OSError`, **re-resolve the port by efuse MAC** (`_await_port` waits
  for the node to reappear — re-enumeration renumbers it, so the old path is stale) and **recapture a
  fresh full window** (re-enumeration resets the ESP, so the boot banner / radio-up line / `TEST|END`
  verdict prints again).
- **Exhausting the retry budget re-raises** → a persistently-unstable board still FAILs. One retry
  tolerates *one* transient, not a standing fault.
- The retry is **logged, not silent**: `[capture] <port> dropped mid-read … re-resolving` +
  `re-enumerated: <old> -> <new>` to stderr. Papering over the flake would hide the real re-enumeration
  rate; this keeps it countable.

`efuse_mac` is threaded through the callers so the re-resolve has a MAC to use:
`tools/regtest/t1_smoke.py:159` (added as a `_smoke_one` parameter) and
`tools/regtest/t2_onair.py:152,236,345`. The param is optional, so the `tp`/`dscycle`/`run.py` callers are
untouched and backward-compatible.

## Verification — honest ledger

| Check | Result |
|---|---|
| **Unit test** (mock pyserial) | recovery path re-resolves `ttyACM4→ttyACM7` + finds marker ✅; persistent failure still raises (FAIL preserved) ✅ |
| **No-regression, real bench** | 10 board2 T1 runs post-fix = **120 captures, 0 fail, 0 false behaviour** ✅ |
| **Live transient recovery on-bench** | **NOT observed** — the ~2–3 % flake simply never fired in the post-fix window, even on a deliberately dscycle-disturbed rail |

So the fix is **correct** (unit-tested, both branches) and **regression-free** (120 clean captures), but I
was **unable to catch a real transient to watch the fix recover live** — only the mocked one holds. Per
the verify-or-say-why rule: forcing a live transient would mean invasive fault injection (glitching the
PPK2 rail mid-capture), which I judged not worth the risk. The mechanism is understood and the logic is
proven; the next natural transient will recover-and-log instead of failing the run.

### A self-inflicted regression the bench caught

The first post-fix validation batch came back **2 pass / 10 fail** on every run —
`serial capture failed: name 'bench' is not defined`. My `t1_smoke.py` edit referenced `bench.efuse_mac`,
but that variable is not in `_smoke_one`'s scope; `py_compile` passed because it is a *runtime* NameError,
not a syntax error. Fixed by threading `efuse_mac` in as a parameter. **Lesson:** `py_compile` doesn't
catch undefined names — I installed **pyflakes** and confirmed the touched files are clean of undefined
names. This is exactly why the change was validated on the bench and not trusted on a green unit test
alone.

### Incidental finding — dscycle's kill is broad

`dscycle` frees the PPK2 by `pgrep -f ppk2_hold.py | kill -9` (`tools/regtest/dscycle.py:136-137`). That
pattern matches **any** process whose command line merely *mentions* the holder — it killed my
orchestrating stress-script (whose text contained the string `ppk2_hold.py`). Not in scope for this fix,
but worth tightening the match if we care. It also means, by design, **dscycle leaves board2 unpowered and
the operator must restart `ppk2_hold.py`** — which is why the earlier dscycle iteration silently left the
following board2 jobs to SKIP.

## Secondary gap noted (not fixed)

A board2-targeted tier **silently SKIPs** when the holder isn't up (`board2 not enumerated`) rather than
warning loudly. In an automated run a SKIP reads as "fine" when board2 was never actually tested. A louder
`board2 not powered — start ppk2_hold` (or an optional auto-start) would be an honest improvement — backlog.

## Bench state

Left radio-silent: all three ESPs on `test-idle` (zero HaLow lines), chronite `wlan1` DOWN, board2 re-held
on the PPK2 rail by a single `ppk2_hold.py` (a duplicate holder was collapsed to one to avoid PPK2 serial
contention). All changes are in the working tree, **uncommitted** (weekday no-commit window); the
`common.py`/`t1_smoke.py`/`t2_onair.py` edits are code → a branch + PR when landed.
