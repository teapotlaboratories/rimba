# firmware/ reshape — regtest fixtures vs curated examples

**2026-07-17 · shipped in the working tree (uncommitted)**

Split `firmware/` into two clean buckets: **`test-*`** = the regression suite plus every app
the harness *functionally flashes as a fixture*, and **everything else** = a small, curated set of
clean **example** apps a real user would copy (join an AP, run a mesh node, TWT power-save, the
mesh-gate). End-state rule: **no non-regtest app is load-bearing**, and the surviving examples carry
no test scaffolding.

## Bottom line

- **Only 3 non-regtest apps were ever load-bearing** (a fixture the harness flashes/runs): `rimba-hello`
  (the `IDLE_APP` radio-silence app), `rimba-deepsleep-cycle` (the dscycle DUT), and `c6-harness` (the
  tp/dscycle D5 trigger). Every other non-regtest app was **T0-build + T1-smoke coverage only**.
- Those 3 moved into the regtest namespace: **`test-idle`** (clone of hello, keeps hello as the
  example) · **`test-deepsleep-cycle`** (verbatim dscycle DUT) · **`test-c6-trigger`** (rename of
  c6-harness).
- **5 experiment apps removed** (`git rm`): `rimba-doze-hold`, `rimba-downlink-test`, `rimba-sleep-test`,
  `rimba-standby-test`, `rimba-halow-mesh-perf` — their coverage now lives in the `tp`/`dscycle` tiers or
  a `test-*` test.
- **`rimba-halow-sta` was mislabeled** — it was the C6-triggered PS-ladder measurement rig (already cloned
  verbatim into `test-power`), *not* a STA example. Rewritten as a clean join-AP + ping STA.
- **11 curated examples** cleaned of scaffolding, each with a new README.
- **Verified:** T0 build of all 13 changed s3 apps = **14 pass / 0 fail / 0 skip**; manifest drift check
  green; zero AI attribution; T0 matrix 30 → 27 apps.

## Method — two analysis/cleanup workflows

The decision surface was 17 non-regtest apps × (is-it-a-fixture? · is-it-example-worthy? · is-it superseded
by a regtest test? · what edits keep the suite green?), so it was fanned out:

1. **19-agent analysis workflow** — one agent per app read the source and reported test-scaffolding with
   `file:line`, example-worthiness, the superseding `test-*` test, and the exact `manifest.py`/`Makefile`/
   docs edits each choice needs; plus a lineage agent (mapped every `test-*` app to the `rimba-*` it was
   cloned from and confirmed each carries disqualifying harness hooks) and a doc-reference agent (split every
   doc mention into **living** = edit vs **historical worklog** = leave alone).
2. Owner picked the disposition (all four decisions = the recommended option): `git rm` the removals ·
   strip scaffolding **+ add a README** to each survivor (no renames this pass) · keep a cleaned `ap-perf` +
   `sta-perf` iperf pair and `git rm` `mesh-perf` · **clone** hello → `test-idle` and keep hello.
3. **11-agent cleanup workflow** — one agent per surviving example stripped the cited scaffolding (removing
   dead callers/decls/includes together), grep-verified no removed symbol remained, and wrote the README.

## The key finding — `rimba-halow-sta` was not a STA example

`rimba-halow-sta`'s header read *"TRIGGERED power-save ladder (C6-harness-ready, netif-free, zero traffic)"* —
it associated then ran a fixed 4-tier PS ladder marking each phase as D5 GPIO pulses for the PPK2/C6, with a
flash-hold guard and a `mmwlan_override_max_tx_power(1)` bench cap (line 153: *"Remove for a real deployment"*).
That ladder had already been cloned verbatim into `test-power` (the tp DUT, `manifest.POWER_DUT_APP`). Yet
the radio-silent runbook and `rimba-linux-node-setup.md` still told users to flash `rimba-halow-sta` as
*"station, static 192.168.12.2"* — a **stale mismatch**. So the app was removed-and-recreated: a clean
join-AP-over-SAE + static-IP + continuous-ping STA (derived from `test-apsta-sta` minus the `TEST|`
hooks). This simultaneously filled the "no clean STA example" gap and *fixed* those stale docs.

## Execution — 5 stages, each verified before the next

**Stage 1 — harness fixtures (the load-bearing part).** Created `test-idle` (radio-free clone of
`rimba-hello`) and `test-deepsleep-cycle` (verbatim clone with every rig hook kept — C6/D5 EXT1 wake,
flash-hold, the exact `RECONNECTED in <ms>` line dscycle scrapes); `git mv c6-harness → test-c6-trigger`.
Rewired `manifest.py` (`APPS` + `IDLE_APP = test-idle`), `dscycle.py:_DUT_APP = test-deepsleep-cycle`,
and the c6 references in `t1_smoke.py`/`tp_power.py`. **Checkpoint:** `check_manifest_covers_tree` = no drift;
all harness modules import; on-disk↔APPS parity exact.

**Stage 2 — removals.** `git rm` the 5 experiment apps + their `manifest.py APPS` entries. Replaced
`rimba-halow-sta`'s ladder `app_main.c` with the clean STA. **Checkpoint:** drift clean, T0 matrix 30 → 27.

**Stage 3 — clean survivors + READMEs** (the 11-agent workflow). Stripped, per app: the close-bench TX cap,
`TEST|` hooks, D5/flash-hold guards, the `#define IPERF 1` experiment gate + dead ping/TWT code, the
RISK-01 raw-frame sniffers (`mmwlan_register_rx_frame_cb` re-declarations), bench packet counters/soak loops,
hardcoded bench MAC/IP peers, and internal jargon (`RISK-04`, `dev-plan task 1.8`). Kept every real feature.
`rimba-hello`'s T1 banner (`Rimba Phase-1 bring-up`) was deliberately preserved.

**Stage 4 — living docs.** Updated the regression suite guide (`.md` + `.html`), `tools/regtest/README.md`,
`rimba-bench-devices.md`, the PS reference doc, and the results doc: renamed the trigger, repointed the moved
ladder to `test-power`, noted the retired probes (**measurements preserved**), and fixed the counts
(30 → 27 apps, 4 → 2 sleep apps). Historical worklogs were left untouched.

**Stage 5 — verify.** `run.py t0 --board proto1-fgh100m --app …` over all 13 changed/new s3 apps.

## Verification

```
T0 -- build matrix (every app x board via make; asserts the country code)
  [PASS]  fw-blob version-pin
  [PASS]  rimba-hello / -halow-scan / -halow-ap / -halow-sta / -halow-ap-perf / -halow-sta-perf
  [PASS]  rimba-halow-ibss / -halow-mesh / -halow-mesh-ap / -twt-assoc / -deepsleep-cycle
  [PASS]  test-idle / test-deepsleep-cycle
T0: 14 pass, 0 fail, 0 skip  (1262s)
```

- **Every changed/new s3 app compiles** via the harness `make` path — including the hand-written
  `rimba-halow-sta`, the two new fixtures, the heavily-stripped `mesh-ap`, and the iperf pair.
- **Manifest drift: none** · all harness modules import · `IDLE_APP=test-idle` ·
  `dscycle._DUT_APP=test-deepsleep-cycle`.
- **Zero AI attribution** anywhere · all 11 READMEs present · `mesh-ap` datapath functions intact.
- `test-c6-trigger` (esp32c6, `boards=()`, hand-built target) is a cosmetic rename only (project name +
  `TAG` + comments, zero logic) — verified by inspection; it is outside the `make`/T0 matrix.

## Final `firmware/` layout

**11 curated examples** (each cleaned + README): `rimba-hello`, `rimba-halow-scan`, `rimba-halow-ap`,
`rimba-halow-sta` (rebuilt clean), `rimba-halow-ibss`, `rimba-halow-mesh`, `rimba-halow-mesh-ap`,
`rimba-twt-assoc`, `rimba-deepsleep-cycle`, `rimba-halow-ap-perf`, `rimba-halow-sta-perf`.

**Regtest fixtures** (no non-regtest app is load-bearing anymore): `test-idle` (silence),
`test-deepsleep-cycle` (dscycle DUT), `test-c6-trigger` (tp/dscycle trigger), plus the existing
`test-*` test suite.

## Open follow-ups

- **Bench re-verify `rimba-deepsleep-cycle` (example).** Its wake path was changed from the bench C6 EXT1
  trigger to a plain RTC timer (`esp_sleep_enable_timer_wakeup(SLEEP_S)`) so it's a portable battery-leaf
  example. Confirm cold-boot reconnect after each `SLEEP_S` wake + the ~0.35 mA sleep floor on the PPK2. The
  *fixture* twin (`test-deepsleep-cycle`) is the untouched verbatim clone, so the dscycle tier itself is
  not at risk.
- **Pre-existing suite doc nit** (left untouched): `test-ap-sta-ping/README` and `test-apsta-sta`'s
  header say the T2 AP role is `rimba-halow-ap`, but the rig actually flashes `test-apsta-ap`
  (`t2_tests.py`) — worth reconciling.

## Footguns for the next session

- `manifest.check_manifest_covers_tree` **hard-fails T0** on any tree↔`APPS` mismatch — every add / remove /
  rename must mirror `manifest.py APPS`. Kept in lockstep and re-checked after each structural stage.
- The **T0/T1 build-matrix over the examples stays** (do not move it under `test-*`) — building the shipped
  examples is exactly the coverage that catches a stack bump breaking a user-facing example. Only the counts
  changed.
- Everything is in the **working tree, uncommitted** (nothing committed — a code change lands as one
  branch + PR, held past weekday work hours). The whole `tools/regtest/` + `firmware/test-*` tree was
  already uncommitted active work, so it and this reshaping land together.
- No board was flashed (compile-only), so the bench is untouched / radio-silent.
