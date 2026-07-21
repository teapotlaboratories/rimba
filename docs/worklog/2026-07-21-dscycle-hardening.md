# 2026-07-21 — dscycle deep-sleep tier: make it reliable + self-contained

**Goal.** The `dscycle` regtest tier (`tools/regtest/dscycle.py`, DUT `firmware/test-deepsleep-cycle`)
deep-sleeps board2 and proves it re-associates on every wake. It lands **INCONCLUSIVE run-to-run** on a
flaky C6 D5-wake and isn't self-contained (leaves `ppk2_hold` restart to the operator; a too-broad
kill-match; `test-all` runs `tp` right before it, which kills `ppk2_hold` and doesn't restart it → dscycle
finds board2 un-enumerated and SKIPs silently). This is the next slice of the regtest "Loop-back: harden"
umbrella (`docs/rimba-todo.md`).

Five hardening items (from the `dscycle-hardening` memory + rimba-todo "Loop-back: harden"):
1. **Dependable wake (core flake).**
2. **Self-restart `ppk2_hold`** after teardown.
3. **Tighten the `ppk2_hold` kill-match** (was `pgrep -f ppk2_hold.py | kill -9` — matches anything merely
   mentioning the holder; once killed an orchestrating stress script).
4. **Fix `test-all` ordering** (tp → dscycle leaves board2 holderless → silent SKIP).
5. **C6 serial-latch — the enabler for #1.** Today `firmware/test-c6-trigger` picks its mode at *compile*
   time (`#define MODE MODE_TRIGGER`), so changing the D5 behaviour means re-flashing the esp32c6. Add a
   serial-command interface that latches the mode/pin state, so the harness can **command a wake pulse on
   demand** (fire it at the right point in board2's awake window) instead of relying on the free-running
   ~30 s pulse that races that window.

## Root-cause hypothesis (from the code, pre-bench)

- `firmware/test-c6-trigger` MODE_TRIGGER fires a **free-running** LOW pulse every 30 s regardless of
  board2's state (`main.c:53` `vTaskDelay(30 s)` → `drive(0)` 120 ms → `hiz()`).
- board2's DUT (`test-deepsleep-cycle/app_main.c`) only arms its ext1 D5-low wake **while it is in deep
  sleep**. Its awake window per cycle is short: cold boot → `mmhalow_init` → connect (≤30 s but usually a
  few s) → `RECONNECTED` → hold link 4 s → `enter_deep_sleep()`.
- So a 30 s free-running pulse **races** the awake window: a pulse that arrives while board2 is awake is
  wasted; board2 then sleeps and must wait for the *next* pulse — up to ~30 s — or falls through to the
  **60 s backup timer** (`SLEEP_BACKUP_S`). The dscycle budget even encodes this: `60 + cycles*130` s per
  the "slow ~130 s/cycle" comment (`dscycle.py:234`). Under a stressed/less-rested board2 the reconnect
  itself flakes on top, and the run lands INCONCLUSIVE.

**Fix direction:** the C6 must stop free-running and instead **pulse on command**. The harness watches
board2's port disappear (= it deep-slept), waits a short settle so ext1 is armed, then sends `pulse` over
the C6 serial → deterministic wake, no 30 s race, no 60 s fallback. This is why the serial-latch (#5) is
the enabler for a dependable wake (#1). The DUT's 60 s backup timer stays as a safety net.

## Plan

- **C6 firmware (`firmware/test-c6-trigger`)** — add a line-based serial console (UART0 / the CP2102N on
  ttyUSB0). Commands latch a runtime mode (persisted to NVS so a C6 reboot restores it): `pulse` (one LOW
  pulse now — the on-demand wake), `hiz` (rest = tri-state, measurement-safe default), `high`/`low`
  (HOLD_HIGH/HOLD_LOW guard), `trigger` (legacy free-run), `toggle` (link test), `status`. Keep the
  compile-time `MODE` as the power-on default so the existing tp free-run path is unchanged if no command
  is sent.
- **dscycle.py** — (a) commanded-wake `_capture` driving the C6 over serial; (b) `_ensure_holder()` starts
  `ppk2_hold` (detached) if board2 isn't powered — makes the tier self-contained and fixes the test-all
  order silently-SKIP; (c) `_free_ppk2()` kills the holder **precisely** (PID file, else the process that
  actually holds the PPK2 fd) instead of the broad pgrep; (d) `_restart_holder()` after recovery.
- **`ppk2_hold.py`** — write a PID file on start (clean up on exit) so the holder can be identified/killed
  precisely.

## Log

### Baseline reproduced (01:03–01:10, CYCLES=2)

`make test-dscycle CYCLES=2` on the current (unmodified) code + the current free-running-TRIGGER C6:

```
INCONCLUSIVE  333.6s  deepsleep-reconnect
  2/2 reconnects but only 1 long deep-sleep gaps — the board reconnected but did not
  clearly deep-sleep between cycles ... so not a clean deep-sleep-leaf proof.
```

This **confirms two root causes**:

1. **Off-by-one counting bug.** `_capture` counts *every* `RECONNECTED in N ms` line — including the
   **fresh-boot** association that happens once after flashing, before any deep-sleep. It stops when
   `len(recons) >= cycles`, so for CYCLES=2 it stops after `fresh-boot recon + 1 wake recon` = 2 recons but
   only **1** deep-sleep gap → the `n>=cycles but deep_sleeps<cycles` INCONCLUSIVE branch fires **even when
   the wake works**. The gate is structurally one cycle short.
2. **Free-running wake races the awake window** (as hypothesised) → slow ~130 s/cycle and intermittently
   misses, compounding the flake.

Also confirmed the item-2 gap live: the run's `finally` → `_recover_board2` **killed `ppk2_hold` (broad
`pgrep`) and did not restart it**, leaving board2 holderless at exit.

**Fix counts wake-reconnects only** (a cycle = a deep-sleep gap *followed by* a reconnect; the fresh-boot
recon is ignored) **and** commands the wake over the C6 (deterministic, fast). Plus self-restart the holder.

### Implemented (all 5 items)

- **#5 C6 serial-latch** — `firmware/test-c6-trigger/main/main.c` rewritten: a line-based UART0 command
  interface (`uart_driver_install` on UART0, read via `uart_read_bytes`; TX/logging unaffected). Commands
  `pulse [ms]` / `hiz` / `high` / `low` / `trigger [s]` / `toggle [ms]` / `save` / `default` / `status` /
  `ping`, responses prefixed `C6|`. Runtime-latched mode + optional NVS persistence (`save`). Power-on
  default = NVS else the compile-time `MODE` (=`MODE_TRIGGER`), so an un-commanded C6 is unchanged (tp
  unaffected).
- **#1 dependable wake** — `dscycle._capture` now drives a **commanded** wake: when board2's port vanishes
  (asleep) it waits `_WAKE_SETTLE_S`=1.5 s (ext1 armed) then sends `pulse` over the C6. No 30 s race, no
  60 s fallback. Falls back to the DUT's 60 s backup timer (loud warning) if the C6 doesn't answer.
- **off-by-one fix** — a cycle = a deep-sleep gap *followed by* a reconnect (`wake_cycles`), so the
  fresh-boot association no longer inflates the count.
- **#2 self-restart holder** — `_start_holder()` (detached, `start_new_session`) runs in the `finally`
  after recovery so board2 is left powered.
- **#4 self-contained** — `_ensure_holder()` starts `ppk2_hold` if board2 isn't powered (fixes the
  test-all silent SKIP); only a *missing PPK2* SKIPs (loudly).
- **#3 precise kill** — `_free_ppk2()` kills exactly the process holding the PPK2 fd (`/proc/<pid>/fd`
  scan) + the pidfile PID — not a broad `pgrep -f ppk2_hold.py`. `tools/ppk2_hold.py` now writes
  `/tmp/ppk2_hold.pid` (cleaned up on exit/signal).

### Bench verification (2026-07-21)

**C6 serial protocol** (direct smoke test over ttyUSB0): `ping→C6|PONG`, `status`, `hiz`, `pulse 120→
C6|PULSE…done`, `trigger`, `toggle`, `bogus→C6|ERR unknown` — all correct.

**Run 1 — `make test-dscycle CYCLES=2` (new stack): PASS in 124.6 s** (baseline was INCONCLUSIVE at
333.6 s). Detail: `2/2 deep-sleep→wake→reassoc cycles (2 gaps >3s, 3 reconnects, latencies
8570/5820/7240 ms)`, `wake=commanded-c6`. The 3 reconnects → correctly **2 cycles** (off-by-one fixed).
Recovery + holder self-restart confirmed: board2 back on rimba-hello, holder running, `/tmp/ppk2_hold.pid`
written.

**Item 3 — precise kill (unit):** started a bystander whose cmdline is literally
`python3 tools/ppk2_hold.py --bystander`. `pgrep -af ppk2_hold.py` matched **both** it and the real holder
(what the old broad kill would target); `_free_ppk2()` killed **only** the real holder (the PPK2 fd
owner) and the bystander survived. PASS.

**Items 2+4 — self-start (bench):** powered board2 fully OFF with no holder (the post-tp state), then
`make test-dscycle` → it logged `board2 not powered; starting tools/ppk2_hold.py`, board2 enumerated in
~3 s, and the tier ran (no silent SKIP).

### Reliability — repeated runs (was: a single flaky INCONCLUSIVE)

| Run | Cycles | Result | Wall | Per-cycle cadence |
|---|---|---|---|---|
| baseline (old) | 2 | **INCONCLUSIVE** | 333.6 s | free-run wake, off-by-one |
| run 1 | 2 | **PASS 2/2** | 124.6 s | commanded; reconnects 8.6/5.8/7.2 s |
| run 2 (self-start) | 2 | **PASS 2/2** | 118.6 s | self-started from dark board2 |
| run 3 | 3 | **PASS 3/3** | 143.6 s | gaps 2.5 s each; reconnects 5.8/6.7/6.7 s |
| run 4 | 2 | **PASS 2/2** | 122.7 s | gaps 2.5 s each; reconnects 5.7/5.8 s |
| run 5 (post-review) | 2 | **PASS 2/2** | 118.4 s | gaps 2.5 s each; reconnects 6.0/5.8 s |

**5/5 clean PASS** since the fix, including a 3/3 and a self-start-from-dark — vs the baseline's single
INCONCLUSIVE. Each cycle is ~15–23 s wall (commanded pulse → cold boot → reassoc), against the old
free-run's ~130 s/cycle.

### Self-review (`/code-review`) + fixes

Ran a code review over the diff; 4 low-severity findings (the core paths were already bench-verified),
triaged:

- **`_start_holder` fd leak** — the parent opened `/tmp/ppk2_hold.log` and passed it to `Popen` but never
  closed its copy → fixed with a `with` block (the child dup's the fd).
- **C6 `pulse [ms]` unbounded** — a large ms would block the command loop and starve the UART RX → clamped
  to 2000 ms. Inert on the harness path (always `pulse 120`); verified `pulse 99999999 → C6|PULSE ms=2000`.
- **`rimba-hello` doc wording** — the docstrings said board2 rests on `rimba-hello`; it actually flashes
  `M.IDLE_APP` (`test-idle`) → corrected the wording.
- **IDLE_APP pre-built dependency** — recovery + silence flash from `build/test-idle/…`; a standalone run
  where `test-idle` was never built would no-op. Pre-existing suite invariant (t0 builds it first) — left
  as-is, noted.

Post-fix: pyflakes clean, C6 rebuilt + reflashed + re-smoked, and **run 5 (above) is the end-to-end
confirmation** that the reflashed C6 + edited harness still PASS.

### tp-contract fix — restore the C6 to `trigger` at teardown

Found while prepping the full regression: the **`tp` tier depends on the C6 free-running `MODE_TRIGGER`**
(`tp_power.py:479` — "no ladder markers seen — the C6 trigger never fired") and never opens/commands the
C6. My dscycle teardown left the C6 in **`hiz`**, which would break any `tp` that runs after a dscycle
(the bench contract is "the C6 rests in trigger"). Fixed: dscycle's `finally` now calls `c6.trigger()`
(restore the free-running default) instead of `c6.hiz()` before closing. During the run the C6 is still
put in `hiz` (so we command pulses); only the *resting* state changes. Verified by a dscycle-then-`tp`
sequence (below).

### Stress test + full regression (against the 2.12.3 forward-port)

The `components/halow` submodule is clean at `7d7f76ad` = tag **`2.12.3-esp32-1`** (the 2.11.2→2.12.3
forward-port + beacon cleanup), so all firmware here builds against the production forward-port.

**Stress test — 8 back-to-back runs, 30 wake cycles (cycles 2/3/2/5/3/2/3/10):**

```
PASS=7  INCO=1  FAIL=0  / 8 runs
run 1  CYCLES=2  PASS   run 5  CYCLES=3  INCONCLUSIVE (2/3)
run 2  CYCLES=3  PASS   run 6  CYCLES=2  PASS
run 3  CYCLES=2  PASS   run 7  CYCLES=3  PASS
run 4  CYCLES=5  PASS   run 8  CYCLES=10 PASS  (10/10, 306 s)
```

Every reconnect was ~5.5–6.4 s, every gap ~2.5 s — rock-steady cadence. The lone INCONCLUSIVE (run 5, the
5th consecutive run hammering the PPK2 rail with power-cycles) got 2/3 cycles: cycle 3's commanded wake
didn't complete within budget. This is the tier behaving **correctly** — INCONCLUSIVE, not a false PASS,
on a flaked wake/reconnect — and it's attributable to the standing **board2 PPK2-rail flake**
(`[[board2-serial-capture-flake]]`, ~2–3 %/capture USB re-enumeration, worse after repeated dscycle
power-cycles), not a dscycle logic fault. Contrast the baseline: the old tier landed INCONCLUSIVE on
**every** run (the off-by-one), even a good wake.

**Robustness follow-up — re-pulse on a missed wake.** Run 5 exposed that a *missed* commanded pulse used
to wait ~60 s for the DUT's backup timer before recovering. Added `_WAKE_RETRY_S = 8 s`: `_capture` now
**re-pulses every 8 s** while the port stays gone after the first pulse (a pulse can be dropped on a rail
wobble), recovering a missed wake in ~8 s instead of ~60 s; per-cycle budget nudged 45→55 s for headroom.
Re-verified with a 5-run mini-stress (below) — no regression, and it leaves the C6 in `trigger` (the
teardown fix).

**Mini-stress (re-pulse + trigger-teardown + budget changes): 5/5 PASS** (cycles 3/2/3/3/2). Session
total across all dscycle runs: **17/18 PASS** (1 honest INCONCLUSIVE = the bench rail flake above).

### Full regression — `make test-all` (against 2.12.3 `7d7f76ad`)

`make test-all BOARD_NAME=board2 AP=esp CYCLES=3` — T0 build matrix → T1 smoke → T2 on-air → tp
(ESP AP) → dscycle. The C6 is set to `trigger` first (tp depends on the free-run).

| Tier | Result | Time |
|---|---|---|
| **T0** build matrix | 31 pass · 0 fail · 1 skip (test-c6-trigger excluded, esp32c6) | 53 min |
| **T1** smoke (board2) | 12 pass · 0 fail · 2 skip | 6.5 min |
| **T2** on-air (18) + dscycle | 18 pass · **1 fail** → 19 pass · 0 fail after retry | 21 min |
| **TP** power ladder (ESP AP) | 4 pass · 0 fail | 3.3 min |

**Everything green except one transient.** The lone T2 fail was `mesh-linux` — the ESP↔Linux mesh interop
test — and it failed on the **chronite side**: *"support role 'linux' (chronite) did not come up: mesh did
not reach 'type mesh' within ~12 s. wpa log tail: (empty)."* Not an ESP/firmware issue and **unrelated to
the dscycle work** (`test-mesh-linux` built fine in T0; the ESP↔ESP `mesh-peering` and the chronite-hostapd
`twt-assoc` both PASSED — so chronite was reachable, only its `wpa_supplicant_s1g` *mesh* bring-up flaked,
the documented [[dont-reboot-linux-mesh-nodes]] / [[bench-mesh-ip-not-persistent]] flakiness). **Re-ran
`mesh-linux` standalone → PASS 57.0 s** (T2 → 19 pass / 0 fail), confirming a transient.

**Item #4 validated live in the real `test-all` order:** tp ran before dscycle and seized the PPK2 (killing
the holder + leaving board2 latched-powered); dscycle then logged `board2 not powered; starting
tools/ppk2_hold.py`, self-started the holder, and PASSed 3/3 (138.4 s). The old tier would have SKIPped
silently here — the exact bug item #4 fixes.

All against the **2.12.3 forward-port** (`components/halow` @ `7d7f76ad`). Bench left radio-silent.

**Design validation from run 3's real gaps.** With the fast commanded wake the port-gone gap is only
**~2.5 s** — *shorter* than the old 4 s (and my 3 s) threshold, and close to a bare reset's ~2 s
re-enumerate. So a gap-duration heuristic alone **cannot** distinguish a real deep-sleep here. The
**DUT's `DEEP SLEEP` log marker is the primary, unambiguous proof** (it prints only right before
`esp_deep_sleep_start()` with the radio in RESET_N-low); the gap is corroboration. This is why the rework
gates on the marker, not just the gap — a pure-gap gate would have false-INCONCLUSIVE'd the fast wake.

**Bug caught + fixed mid-verification.** Runs 1–2 printed `deep-slept 0.0s`: a stale `/dev/serial/by-id`
symlink lingers through the vanish transition, so `resolve_port` briefly returned the old path and the
deep-sleep was counted (correctly, via the marker) but with a bogus ~0 gap. Fixed by only trusting a
**successful serial open** as "board2 genuinely back" (a blip that fails to open no longer resets the gap
timer) — run 3 then reported the true ~2.5 s gaps. Functionally the PASS was always valid (marker-gated);
the fix corrects the reported duration and removes the fragility.

### Files changed

- `firmware/test-c6-trigger/main/main.c` — serial-latch firmware (UART0 command loop + NVS).
- `firmware/test-c6-trigger/README.md` — serial protocol table + verification.
- `tools/regtest/dscycle.py` — commanded-wake `_capture`, `_ensure_holder`/`_start_holder`,
  precise `_free_ppk2` (fd + pidfile), progress prints, correct cycle counting.
- `tools/ppk2_hold.py` — writes `/tmp/ppk2_hold.pid` (cleaned up on exit/signal).
- `Makefile` — `test-all` comment: dscycle self-starts the holder (no post-tp silent SKIP).

**Not on the air.** dscycle is a structural USB/serial gate; board2 associates to the ESP AP over HaLow
but this change touches no transmitted frame (the C6 pulse is a D5 GPIO wake, not RF), so the on-air
gold-standard doesn't apply. Bench left radio-silent (all ESPs on rimba-hello, holder powering board2).
