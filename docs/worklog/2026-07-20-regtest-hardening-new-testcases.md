# Regression-suite hardening + new T2 test cases (developed on the 2.10.4 baseline)

**2026-07-20 · in progress.** Harden the `tools/regtest` suite and add on-air (T2) test cases,
**developed against the pre-forward-port `components/halow` baseline** (morselib 2.10.4 @ `3c8ded0d`)
as the known-good reference: green the new tests on a battle-tested build first, so a later failure —
including against the 2.12.3 forward-port — is a real regression. Follow-up to the SDK forward-port
(now landed on `main`, gitlink `7d7f76ad`).

> ⚠ The submodule is checked out at the baseline `3c8ded0d` for this work (detached; `main`'s gitlink
> is untouched). **Restore it to `7d7f76ad` (`git submodule update`) before any superproject commit**,
> or a commit would downgrade the gitlink.

## How the work was scoped

A coverage-gap analysis (current T2 coverage vs the validated-feature inventory vs the recorded
"does-not-prove" gaps) produced a ranked candidate list. Owner picked the **mesh-relay-rig batch** —
three tests that all reuse the existing 3-node forced-line rig and only existing accessors (zero
forward-port cost) — and chose **three separate apps** over build-var modes on one app.

## Batch A — harness config-validation guards (host-side, unit-tested)

`tools/regtest/manifest.py` gained `_validate_bench()`, called from the `require_bench()` gate, to
catch **present-but-wrong** bench config that previously built a silently-broken bench:

- `WIRED_BOARD` naming a board that doesn't exist → no fully-wired DUT/relay/PS slot.
- a malformed `BOARDx_MAC` (non-empty but not a valid MAC) → empty derived `mesh_mac`/`mesh_ip`.
- two boards deriving the same `10.9.9.x` mesh IP → indistinguishable on-air.

Import-safety preserved: *absent* config still yields `{}` (so no-hardware tiers import fine); only a
tier that calls `require_bench()` validates. Verified by `tools/regtest/tests/test_manifest.py` (10
stdlib-`unittest` tests) + a new `make test-unit` target — all green; the real bench identity passes
and each bad-config class is rejected with an actionable message. New files are `pyflakes`-clean.

**Scope calls:** the `LINUX_MAC` guard was dropped (an empty Linux MAC is a *supported* "read live at
bring-up" mode, per `LinuxNode.mesh_mac`); the **pyflakes gate** was deferred — running pyflakes
surfaced 7 pre-existing findings, 2 of which are *intentional* patterns that must be reshaped before a
strict gate can go green (`tp_power.py:400 import ppk2_api` = an availability probe; `tp_power.py:448
ap_teardown` = a defensive `None` used in teardown).

## Three new T2 tests (each = a copy of `test-mesh-relay` + its one change)

All reuse the forced-line rig (origin=board0, relay=board2/wired, dest=board1) and the three
build-time mesh MACs; registered in `manifest.py` APPS + `t2_tests.py` (+ the `T2_TESTS` tuple);
drift-clean. T2 grew **12 → 15**.

| test | change | assertion | guards |
|---|---|---|---|
| **mesh-large-frame** | origin pings a 1400-byte payload | same topology + delivery-floor + ccmp-not-dominant, but over a FW-fragmented frame | the **defrag-before-decrypt** RX path (its inverse was reverted) |
| **mesh-leaf** | relay calls `mmwlan_mesh_set_multihop(false)` | INVERTED + deterministic: PASS iff sole peer still the relay (peering survives) AND **exactly 0** replies (forwarding blocked) | the shipped-but-unguarded leaf/single-hop opt-out (P6d) |
| **mesh-relay-nocrash** | roles re-cast: RELAY is the reporter, origin drives a 90-ping burst | relay forwards, then asserts `hw_restart_counter` unchanged across a ~40s window; INCONCLUSIVE if it never peers both endpoints (no load ⇒ no false PASS) | the interrupt-WDT SPI-host-teardown relay crash |

### Harness facts that shaped `nocrash`

`t2_onair._run_orchestrated` brings SUPPORT roles up first (waits each `up_marker`), then flashes
reporters and **blocks per-reporter** (`capture_until "TEST|END"`) in declaration order. So a
relay-then-origin multi-reporter would deadlock (the relay's `hw_restart_counter` verdict needs the
origin's traffic, but the origin isn't flashed until after the relay's verdict). Resolution: the relay
is the **sole reporter** and the origin is a **support role that drives traffic**. `REPORTER_TIMEOUT_S
= 130 s` caps the relay's flash→`TEST|END`, so the windows are sized boot(~25) + peer-wait(45) +
forward(40) < 130.

## Build verification (2.10.4 baseline)

All three apps **build green** via `make build APP=… BOARD=proto1-fgh100m` against `3c8ded0d`
(`test_mesh_large_frame.bin`, `test_mesh_leaf.bin`, `test_mesh_relay_nocrash.bin`). One real compile
error was caught + fixed: `nocrash` used `if (is_dest)` but the app only declared `is_origin`/`is_relay`
(dest was the `else`); added the `is_dest` declaration.

Reaper note: harness-backgrounded builds/runs get reaped ~1–4 min → all builds + the bench run were
launched **detached** (`nohup … & disown`) and tracked with the Monitor tool.

## Bench verification (2.10.4 baseline)

Bench ready: `ppk2_hold` running (board2 → ttyACM4), 3/3 ESP nodes present, fw 1.17.8, ch27; no Linux
node needed (all three tests are ESP-only). Ran `make test-t2 TEST="mesh-large-frame mesh-leaf
mesh-relay-nocrash"` then `make test-silence` (radio-silent per `.ai/radio-silent-workflow.md`).

**Result: ALL GREEN (2026-07-20) — `T2: 6 pass, 0 fail, 0 skip (693s)`:**

| test | verdict | wall-clock |
|---|---|---|
| mesh-large-frame | PASS | 189.3 s |
| mesh-leaf | PASS | 182.8 s |
| mesh-relay-nocrash | PASS | 207.6 s |

No window/timing tuning was needed: the `nocrash` relay peered both endpoints and read
`hw_restart_counter` within the 130 s reporter cap, and `mesh-leaf`'s deterministic 0-reply held. The
run's radio-silence sweep (board0/1/2 → `test-idle`, all PASS) confirms the bench was left silent. ⇒
**the three new tests are bench-verified green on the known-good 2.10.4 baseline** — so they PASS on a
build where every underlying behavior is battle-tested, which is the whole point of developing them
here first.

## `.ai`-rule conformance for this effort

- **Build** via `make … BOARD=proto1-fgh100m` (harness) — never bare `idf.py`.
- **Verify:** Batch A = unit tests; the 3 apps = this hardware bench-verify (evidence = the captured
  `TEST|RESULT` lines).
- **On-air byte-diff:** N/A — these are *test-harness* apps exercising mesh behaviors already on-air-
  verified when the features were built (forwarding/defrag, `set_multihop`, relay stability); they do
  not alter what the ESP puts on the wire.
- **Radio-silent** after via the harness `go_radio_silent` + an explicit `make test-silence`.

## Disposition — LANDED 2026-07-21

1. ✅ Bench-verify on 2.10.4 — 3/3 PASS, no tuning needed (above).
2. ✅ Forward-compat — restored the submodule to `7d7f76ad`; all three rebuilt **clean against 2.12.3**
   (the accessors survived the forward-port).
3. ✅ Landed via **[#39](https://github.com/teapotlaboratories/rimba/pull/39)** (rebase-merge, gitlink
   unchanged at `7d7f76ad`). The pre-merge review returned SAFE TO MERGE; two cheap findings were fixed
   in a second commit — `_validate_bench` now full-shape-validates every MAC (a bad middle octet used to
   slip through) + a test fixup — and two non-blocking robustness caveats deferred as tracked TODOs
   (`nocrash` peer-count-as-load proxy; negative-assertion isolation), see `docs/rimba-todo.md`.

The suite is now at **15 T2 tests** + a `make test-unit` host tier, on `main`.
