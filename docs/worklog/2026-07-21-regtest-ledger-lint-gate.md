# 2026-07-21 — regtest: run-history flake ledger + pyflakes lint gate (make the suite unattended)

**Goal.** Two slices of the regtest "Loop-back: harden" umbrella (`docs/rimba-todo.md`), driven by one
requirement from the owner: **the harness must run fully automatically — nobody, not even the agent,
should have to monitor a run.** For that to be true you have to be able to launch it, walk away, and trust
the durable record afterwards. Two gaps stood in the way:

1. **Run-history / flake ledger** — the per-tier `build/regtest/<tier>-latest.json` baselines are
   *latest-wins*: a passing retry silently overwrites a failed run, so a flake leaves **no trace** (this is
   what erased the first `rimba-halow-sta` FAIL, and it's why the `mesh-linux` fail→retry-pass earlier
   today would have vanished). Nobody watching = nobody catches the transient.
2. **pyflakes lint gate on harness edits** — a `bench`-out-of-scope edit once passed `py_compile` and only
   failed **on the bench, 10/12 FAIL**, deep into a run. `py_compile` does not catch undefined names;
   pyflakes does, in ~0.1 s. Without an automatic gate you only find out mid-run — exactly when you're not
   watching.

## Design

- **Ledger hooks the one chokepoint.** `common.Reporter.add()` already persists crash-safely after every
  result — the natural place. It now also **appends one JSONL line per result** to
  `build/regtest/history.jsonl` (append-only, never overwritten; keyed by a per-Reporter `run_id` so a
  run's results group together; git SHA cached per run so it isn't a `git` shell-out per result). New
  `tools/regtest/ledger.py` holds the schema + the flake view: a test that produced **both** good
  (PASS/SKIP/XFAIL) and bad (FAIL/INCONCLUSIVE/XPASS) outcomes across runs is **flaky**; always-bad is
  **broken** (a real defect, not a flake). `run.py flakes` / `make test-flakes` print it; the HTML report
  carries a "Flake ledger" section.
- **Lint gate runs before the bench, automatically.** New `tools/regtest/lint.py` runs the pyflakes API
  over `tools/regtest/*.py` + `tools/*.py`. `run.py` calls it at the top of every run-producing command
  (`t0/t1/t2/tp/dscycle/all/flash-interop`) and `make test-all` runs it first — a real finding aborts with
  a non-zero exit **before any hardware is touched**, no bypass. A missing pyflakes only *warns* (never
  blocks the bench on a missing dev tool). `make test-lint` runs it on demand.
- **`persist` flag closes a latent dry-run bug.** `Reporter(persist=False)` skips both the crash-safe
  write and the ledger append. Threaded through `t2`/`tp`'s dry runs, so a `DRY_RUN=1` preview no longer
  **clobbers the real baseline** (the per-`add()` crash-safe write had been defeating run.py's dry-run
  guard) or pollutes the ledger.

## The gate immediately earned its keep

Turning the gate on flagged **6 pre-existing findings** the harness had been carrying — including the two
`# noqa`-suppressed "reshape later" patterns the #39 batch had deferred (pyflakes ignores `# noqa`, so
they need real fixes), and a `redefinition of unused 'ap_teardown'`. All fixed:

- `t1_smoke.py` — drop unused `INCONCLUSIVE` import.
- `t2_onair.py` — drop unused `capture_serial` import; two placeholder-less f-strings → plain strings.
- `tp_power.py` — drop unused `REPO_ROOT` import; the `import ppk2_api  # noqa` availability probe →
  `importlib.util.find_spec("ppk2_api")` (no unused import); the `None`-default-then-conditional-`def`
  `ap_teardown` → a named helper assigned to the variable (an assignment, not a def-redefinition — the
  pattern pyflakes was flagging; not a real bug, but now clean).

## Verification (off-bench + a fire-and-forget bench demo)

**Unit tests — 26 pass** (`make test-unit`, now run under the IDF python so pyflakes is present):
- `test_ledger.py` — flake detection (flip=flaky, always-pass≠flaky, always-bad=broken, INCONCLUSIVE
  counts as bad, SKIP/XFAIL are good), append-is-additive round-trip, torn-final-line tolerance, and the
  **real `Reporter.add()` → ledger hook** (temp dirs, so it never touches `build/regtest`) + a
  dry-run-records-nothing test.
- `test_lint.py` — pyflakes catches an undefined name + an unused import, passes clean code, **the real
  harness is clean**, and the gate degrades (skips) gracefully when pyflakes is absent.

**Dogfood:** `make test-lint` = *clean, 19 harness files* (after the 6 fixes).

**Fire-and-forget bench demo (no monitoring):** launched `2× make test-t1 BOARD_NAME=board2` detached,
then wrote these docs + fixed the review findings below *without watching it*, and read the durable ledger
afterward. Run 1 recorded **12 results** to `history.jsonl` (each with `run_id` / `git 7addc14-dirty` /
`halow_gitlink` / the radio read-back detail); `make test-flakes` → *"12 results across 1 run · FLAKY:
none — every test that ran more than once gave a consistent verdict."* Exactly the launch-and-check-after
workflow the change exists to enable.

## Adversarial review (multi-agent) + 5 fixes

Ran a 4-lens adversarial review (flake-logic · reporter-hook · lint-gate · an "is it *truly* unattended?"
completeness critic) + synthesis. It found **5 real defects**, all fixed:

1. **(HIGH) SKIP was classified as a GOOD outcome** — so a rig-gated test that SKIPs when its rig is absent
   and FAILs 100% when it *does* run (`{SKIP, FAIL}`) was scored `good>0 and bad>0` → **flaky**, and
   reported as "a bench transient, not a code regression" — *inverting the ledger's purpose on exactly the
   on-air/rig tests it protects*. Fix: **SKIP is now neutral** (`_GOOD = {PASS, XFAIL}`); `SKIP+FAIL` →
   **broken** (a real defect), `SKIP+PASS` → still not flaky.
2. **(MED) The "not a code regression" label was git-blind** — a flip that straddles a commit is a
   *possible regression*, not a confirmed transient. Fix: `summarize` now tracks the git SHAs of good vs
   bad outcomes; a flip at one SHA is a **same-code transient**, a flip across SHAs is flagged **"flipped
   across code changes — verify before dismissing."** (CLI + report).
3. **(MED) A reaped run read as a silent false-green** — the crash-safe baseline of a run killed mid-way
   (all-PASS prefix) was indistinguishable from a complete green run on the report the operator trusts.
   Fix: a **`complete` marker** — the crash-safe per-result writes set `complete=False`; a new
   `Reporter.finalize()` (used for the final write in `run.py`) sets `True`; the report shows a loud
   **⚠ INCOMPLETE** banner otherwise.
4. **(LOW) `load_history` could crash `summarize`** on a shape-valid non-object line (`null`/`123`/`[…]`).
   Fix: skip non-dict lines (matches the "tolerant read" contract).
5. **(LOW) All-time aggregation never decays** — a once-flaked test stays flagged forever. Fix (light): a
   **`recent[…]`** column (last few outcomes, so a resolved flake is visually distinct from a live one) +
   a documented `rm build/regtest/history.jsonl` reset.

Re-verified after the fixes: **29 unit tests pass** (+3: SKIP-neutral incl. `SKIP+FAIL→broken`,
same-code-vs-cross-commit, non-dict tolerance, the `finalize()` complete marker), lint gate clean.

## Update — "numbers that drift" (metric trend + gitlink diff)

The flake ledger catches pass↔fail flips; it does **not** catch a *number that drifts while the test still
passes* — the tp No-PS current creeping up (the fw-1.17.9 ~2× power-save regression was exactly this), a
dscycle reconnect getting slower, throughput sagging. That's the biggest regression *class* the suite
misses, and the highest-leverage attack is infrastructure, not one test: the ledger already flows through
`Reporter.add`, and tp/dscycle already record rich numeric `meta` (`median_ma`/`p10_ma`/`p90_ma`,
`latencies_ms`/`wake_cycles`) — but the ledger threw it away.

- **Persist the numeric meta.** `ledger.append_result` now stores a **bounded** `meta` (`_ledger_meta`:
  scalar numbers, short strings, numeric lists ≤32) in each `history.jsonl` line, so a recorded number is
  durable + trendable, not just the verdict.
- **Trend + diff view.** `run.py trend` / `make test-trend`: every metric over the last N runs with a
  sparkline + % change; a numeric list trends as its mean. `make test-trend DIFF="<glA> <glB>"` compares
  each metric's value at two **halow gitlinks** (SHA prefixes accepted) and flags a **>20% move** — the
  command to run after a `components/halow` bump, turning "still passes" into "`median_ma +100% ⟵ >20%
  MOVE`".
- Verified: **7 more unit tests** (36 total) — meta persistence + bounding, scalar/list-mean extraction,
  trend ordering, and the canonical **2× No-PS-mA diff across two SDK gitlinks** — plus a real-data demo
  (`2× dscycle` fire-and-forget → `make test-trend` shows the reconnect-latency trend). The RC/A-MPDU
  *numbers* (survey's next targets) need a public `mmwlan_*` accessor first (only the internal
  `umac_rc_get_rc_stats(umac_sta_data*)` exists) — deferred as a submodule follow-on.

## Files

- New: `tools/regtest/ledger.py` (flake ledger + metric trend/diff), `tools/regtest/lint.py`,
  `tools/regtest/tests/test_ledger.py`, `test_lint.py`, `test_trend.py`.
- Changed: `tools/regtest/common.py` (persist flag + run_id + ledger hook + `finalize`/`complete`),
  `run.py` (lint gate + `lint`/`flakes`/`trend` commands), `report.py` (flake section + incomplete-run
  banner), `t2_onair.py`/`tp_power.py` (`persist=not dry_run` + the lint fixes), `t1_smoke.py` (lint fix),
  `Makefile` (`test-lint`/`test-flakes`/`test-trend`; `test-all` lints first; `test-unit` under the IDF
  python), `README.md`.

No hardware behaviour changed — this is host-side harness plumbing (unit-testable, no radio). The fixes to
the 3 tier files are lint-only (unused imports / string prefixes / a name-binding reshape), verified by
the suite still importing + the demo run.
