# PPK2 power-save regression tier (`tp`) — plan + build log

**Status:** in progress (2026-07-16). Building a new `tp` (power) tier for the regression
suite that measures board2's PPK2 current ladder and gates on a *gross* regression (the kind
the fw-1.17.9 ~2× PS regression was), while always recording the raw mA.

This is the harness change requested after the power-saving audit
([2026-07-16-regression-suite-and-fork-migration-plan.md](2026-07-16-regression-suite-and-fork-migration-plan.md)
built T0/T1/T2; this adds T-power). The audit catalogue of *what power-saving Rimba has*
is the input; this doc is the *test* for it.

## Why a new tier (not a T2 test)

The suite's own contract forbids gating power in T2: `tools/regtest/README.md:19` — T2 "does
NOT prove throughput / power numbers — those are benchmarks", and `t2_tests.py:216` disclaims
PS as "needs the PPK2 current ladder, not a pass/fail test". T2's architecture also puts the
**verdict in the firmware** (`t2_onair.py:15-18`, a scraped `REGTEST|RESULT`). A power verdict
comes from a **host-side PPK2 stream the firmware physically cannot see**, so it can never be a
firmware `REGTEST|RESULT`. A separate `tp` tier keeps T2's clean model intact and reuses its
machinery (by-MAC port resolve, `require_wired` board2, radio-silence teardown, git-stamped
JSON baseline → the 1.17.9 regression becomes a JSON diff, not a bench session).

## Owner decisions (2026-07-16)

1. **AP reference: BOTH from the start** — ESP32 SoftAP (`regtest-apsta-ap` on board0, fully
   automated today) *and* Linux `hostapd_s1g` on chronite (the authoritative reference the
   1.17.9 table used: clean separation Dyn-PS 9.1 / WNM 5.1 mA). One DUT serves both — the
   Linux config `hostapd-rimba.conf` (on chronite, `ssid=rimba-ping`/SAE `rimbahalow`/
   `dtim_period=1`/PMF/ch27) matches the ESP AP and the DUT exactly.
2. **Wide gross-multiple gate** — FAIL only at ~1.8–2× the *calibrated* 1.17.8 baseline;
   INCONCLUSIVE in the grey zone; raw mA always recorded; drawing *below* the band is never a
   FAIL. Honors the repo doctrine that doze *depth* is a benchmark. Catches the clear 1.17.9
   doubling (20.2/17.5) without flagging RF/thermal drift; a condition-sensitive milder case
   (Appendix A.4's 14.5/4.0) honestly lands INCONCLUSIVE, not red.
3. **Keep the C6 trigger in the loop** — reuse the existing C6 (GPIO20 → board2 D5) that pulses
   LOW every 30 s to fire one ladder run. No self-trigger firmware. The DUT keeps
   `wait_for_trigger()`; the host samples the PPK2 and segments phases off the console markers.

## Design

- **DUT firmware — `firmware/regtest-power/`** (new): a copy of the proven `rimba-halow-sta`
  4-tier C6-triggered ladder (No-PS → Dyn-PS → TWT(10 s SP) → WNM+chip-powerdown, 18 s each),
  but emitting the `REGTEST|` contract instead of raw `ESP_LOGW`: `BEGIN`; `STEP associated`;
  per-phase `INFO phase=N ...` markers (the host's segmentation timestamps); `STEP twt-installed`
  (`mmwlan_twt_agreement_installed`) and `STEP wnm-accepted` (the `_ext` ret) as the false-PASS
  guard; `INFO ladder-done`. **No `REGTEST|RESULT`** — the mA verdict is host-side. Keeps the
  D5 flash-hold guard, `wait_for_trigger` (C6), `phase_mark` (D5 pulses), SSID `rimba-ping`/SAE,
  and `mmwlan_override_max_tx_power(1)` (mandatory — uncapped TX saturates the close-bench AP RX
  and inflates doze current in a way that *mimics* the regression).
- **Runner — `tools/regtest/tp_power.py`** (new): owns the PPK2 (mirrors the proven
  `~/pwr_test/rf_run.py`/`ppk2_mon2.py`: kill prior holders → drain → `get_modifiers` with retry
  → `use_ampere_meter` → `set_source_voltage(5000)` [mandatory] → power-CYCLE the DUT rail →
  wait for board2 to enumerate → flash `regtest-power` in the fresh-boot window →
  `start_measuring()` in a thread accumulating µA@100 kHz with host timestamps). Brings up the AP
  (ESP `regtest-apsta-ap` on board0, or Linux `hostapd_s1g` via a new `linux_peer.bring_up_ap`),
  reads board2's console for the phase markers, segments the sample stream by the marker host
  timestamps (discard ~1 s settle, median of the plateau), scores, emits one `common.Result` per
  tier, then radio-silences.
- **Scoring — 4 measured, 2 scored.** No-PS (~65 mA) = *validity* check (associated + PS on +
  not RX-overloaded) and the tier-separation anchor; Dyn-PS + WNM+powerdown = the scored
  discriminators; TWT = recorded, not scored (a Linux AP ignores the mid-session setup → falls
  back to dyn-PS). Guard against a false PASS ("failed to doze" reads as a good low number):
  a low median with No-PS out of band, no tier separation, or a failed `STEP` → INCONCLUSIVE.
- **Tolerance model.** Per scored tier: `PASS ≤ ~1.4× baseline`; `FAIL ≥ ~1.8–2× baseline`
  (regression direction = higher); `INCONCLUSIVE` = grey zone, or any untrustworthy run.
  Bands live in a `manifest.POWER_TIERS` catalogue with per-tier `(pass_max, fail_min, band
  provenance)`; **calibrated on THIS rig at 1.17.8** in S4 (the memory anchors were a *Linux* AP
  and the two 1.17.9 passes disagree). Uses the existing 6-value status vocab unchanged;
  INCONCLUSIVE does not gate (`common.py:131-136`, green iff FAIL==0 and XPASS==0).
- **Integration:** new `tp` subparser + `cmd_tp` in `run.py`; `test-tp` Makefile target; `TP`
  tier in `report.py`; mA rides in `Result.detail` + `Result.meta` (median_ma / p10 / p90 /
  band / assoc / fw) through the existing generic renderer.

## Rig + preconditions

board2 ONLY (`require_wired`; board0/1 have WAKE/BUSY unwired → a PS reading there is
meaningless). PPK2 = board2's 5 V supply in ampere-meter mode; the runner owns the control CDC
start-to-finish and **power-cycles** the DUT at start (so a prior `ppk2_hold` take-over can't
leave a stale state — `rf_run.py` already does this). C6 powered + flashed + wired (GPIO20→D5).
TX-cap 1 dBm baked into the DUT. fw version logged per run (the gate is meaningless without it:
`mm6108.bin` SIZE 480664 = 1.17.8 vs 481040 = 1.17.9). PPK2 absent / board2 not enumerable /
no C6 trigger seen → SKIP or INCONCLUSIVE, never FAIL. Teardown: `rimba-hello` to board0+board2,
Linux AP down, board2 left powered (re-`ppk2_hold`) so it stays enumerated for other tests.

## Stages

- **S0** — manifest `App('regtest-power')` + `POWER_TIERS` skeleton + `tp` subparser/`cmd_tp` +
  `test-tp` + report `TP` tier + `tp_power.py` that cleanly SKIPs with no PPK2/board2. Verify T0
  still green (matrix builds `regtest-power`, drift check passes), `tp --dry-run` lists the rig.
- **S1** — the `regtest-power` firmware. Verify T0 build + a manual board2 flash shows the
  `REGTEST|` phase markers streaming at the 4×18 s cadence after a C6 trigger.
- **S2** — the PPK2 measure helper (take-over + threaded sample + window median). Verify against
  an idle board2 (~64 mA No-PS plateau) and that the take-over doesn't glitch board2.
- **S3** — end-to-end `run.py tp` (own PPK2, flash AP + DUT, sample, segment, 4 tier Results +
  HTML row), ~90–110 s.
- **S4** — CALIBRATE the bands at 1.17.8 (N runs, mean±σ for Dyn-PS + WNM), lock `POWER_TIERS`,
  commit as the golden baseline. Both AP paths.
- **S5** — validate the gate (optional 1.17.9 A/B, or a threshold sanity check).

## Build log

- 2026-07-16: plan written; decisions locked (see above).
- **S1 (firmware) DONE** — `firmware/regtest-power/` builds clean on `proto1-fgh100m` (17% partition
  free); a copy of the `rimba-halow-sta` C6-triggered ladder emitting the `REGTEST|` contract
  (`BEGIN`, `STEP associated/twt-installed/wnm-accepted`, per-phase `INFO phase=N`, no `RESULT`).
- **S0 (scaffolding) DONE** — `App('regtest-power')` in APPS (T0 drift check passes); `POWER_TIERS`
  + `POWER_NOPS_VALID_MA` + provisional per-AP `POWER_BANDS` in manifest; `tp` subparser + `cmd_tp`
  in run.py; `test-tp` Makefile target; `TP` tier + blurb in report.py. `tp --dry-run` lists the
  rig + tiers + bands for both AP paths; modules import; the report renders the TP section.
- **S2/S3 (sampler + end-to-end) DONE + validated on the bench.** `tp_power.py` owns the PPK2
  (mirrors `~/pwr_test/rf_run.py`: kill holders → drain → `get_modifiers` retry → `use_ampere_meter`
  → `set_source_voltage(5000)` → power-cycle → sample in a thread), brings up the AP (ESP
  `regtest-apsta-ap` on board0, or Linux `hostapd_s1g` on chronite via the new
  `linux_peer.bring_up_ap` + `docs/reference/captures/hostapd-rimba.conf`), flashes the DUT, segments
  the current stream by the `REGTEST|INFO|phase=N` markers, and scores. **Both AP paths ran
  end-to-end** (confirmed the C6 is alive: `c6-harness` banner + `TRIGGER pulse LOW` every 30 s):

  | tier | ESP AP (median, p10/p90) | Linux AP (median, p10/p90) | ref (good) 1.17.8 |
  |---|---|---|---|
  | No-PS (validity) | ~63 mA (PASS) | 62.7 mA (PASS) | ~65 |
  | Dyn-PS *(scored)* | 25.5 (20.8/61.2) | 23.1 (16.1/46.0) | Linux 9.1 / ESP ~15.3 |
  | TWT (recorded) | 22.5 | 22.5 | ~9.9 (Linux ignores mid-session) |
  | WNM+pd *(scored)* | 18.3 (12.8/23.5) | 14.5 (8.1/21.0) | Linux 5.1 / ESP ~4.2 |

  **Finding — the bench reads ~1.6–2× the documented reference on every doze tier, on BOTH APs.**
  No-PS validity passes (STA associated, ~63 mA), so the measurement is real; the offset is a
  **close-bench RX-overload** signature — the DUT caps its own TX to 1 dBm, but the AP blasts it at
  full power from cm away, so the radio keeps waking on retries (elevated p10 floor ~1.6× ref +
  spike-driven p90). Consistent across APs and runs (Dyn-PS 23–25, WNM 14–18), so **repeatable** —
  the wide gross-multiple gate calibrates to THIS rig's 1.17.8 numbers (their decision #2), it does
  NOT need to match the reference's absolute depth. The provisional bands (Linux-AP anchored) score
  the current rig as FAIL/INCONCLUSIVE — expected, they are pre-calibration placeholders.

  **Teardown bug found + fixed:** the first cut re-spawned `ppk2_hold` from the runner's `finally`;
  a bare restart races the just-closed PPK2 handle and lacks drain/retry (`ppk2_hold.py` has
  neither) → it crashed and left board2 unpowered. Fixed: `sampler.stop()` now drains the stream,
  and the teardown leaves board2 **latched-powered** (bench-devices.md:235-236) without re-spawning a
  fragile holder (the next `tp` take-over kills+drains+power-cycles regardless). Recovered board2 via
  `~/pwr_test/ppk2_power.py` (robust drain+retry+OFF/ON) and restored a standing `ppk2_hold`.

- **S4 (calibration) DONE + owner chose "calibrate now, harden later".** 3 runs/AP at 1.17.8 on
  this rig (median mA): **ESP** Dyn-PS 25.5 / 21.4 / 21.4, WNM 18.3 / 13.4 / 13.4 (run #1 a warmup
  excursion); **Linux** Dyn-PS 23.1 / 21.8 / 22.9, WNM 14.5 / 14.3 / 15.8 (tighter). No-PS held
  ~62–64 mA every run. Bands locked in `manifest.POWER_BANDS` (`POWER_BANDS_CALIBRATED = True`),
  pass_max ~1.3× the noisiest good run, fail_min ~1.9× baseline:
  ESP `dyn-ps (32, 42) / wnm (24, 32)`; Linux `dyn-ps (30, 40) / wnm (21, 28)`. A fresh 1.17.8 run
  scores **4 PASS / 0 FAIL** with margin (Dyn-PS 20.6 ≤ 32, WNM 12.8 ≤ 24).
- **S5 (gate validation) DONE — and it caught a real scoring bug.** Rather than flash 1.17.9 (out
  of scope for "calibrate now"; the bin isn't on the bench), I validated the scoring logic with a
  synthetic sweep through `_score`. It exposed a bug: the first cut gated scored tiers on a
  **tier-separation** check (`dyn-ps < 0.7 × No-PS`) as a validity guard — but a real regression
  pushes doze current UP toward No-PS, so that check FAILED exactly on a regression, scoring a ~2×
  regression **INCONCLUSIVE instead of FAIL** (defeating the gate's whole purpose). Fixed: validity
  is now just `associated + No-PS in window`; a high scored median is what `fail_min` is for. Re-run
  synthetic boundaries, all correct: good 1.17.8 (22/14) → PASS/PASS · regression ~2× (48/40) →
  **FAIL/FAIL** · not-dozing (63/55) → FAIL/FAIL · grey ~1.6× (37/28) → INCONCLUSIVE · low-WNM +
  `wnm-accepted=FAIL` → INCONCLUSIVE (false-doze guard) · unassociated → INCONCLUSIVE. Then a final
  **real** acceptance run with the shipped code = 4 PASS / 0 FAIL.

## Status: SHIPPED (uncommitted) + the v2 follow-up

The `tp` tier is complete and live: `make test-tp` / `python tools/regtest/run.py tp --ap esp|linux`.
New: `firmware/regtest-power/`, `tools/regtest/tp_power.py`, `manifest.POWER_*`,
`linux_peer.bring_up_ap` + `docs/reference/captures/hostapd-rimba.conf`, `run.py`/`Makefile`/
`report.py` wiring. All in the (already-uncommitted) regtest harness; nothing committed.

### ~~v2 — reduce the RX-overload offset~~ → REFUTED 2026-07-17 (this was a mis-diagnosis)

> **CORRECTION (2026-07-17, see `docs/worklog/2026-07-17-powersave-test-cases-batch.md`).** The claim
> below — that the ~1.6–2× elevation is RX overload from an uncapped AP — is **WRONG**. The **ESP AP
> already caps TX to 1 dBm** (`regtest-apsta-ap:86`) and **chronite's Linux AP is capped to 0 dBm**
> (`morse.conf tx_max_power_mbm=0`); `mmwlan_override_max_tx_power` is `uint16_t` (min 0 dBm) so it can go
> no lower; and both tp AP paths read *equal* (so AP TX isn't the differentiator). At ~0–1 dBm the link is
> **healthy** (No-PS ~63 mA — overload reads *wrong*, ~14 mA). So there is **no v2 TX-cap fix**; the ~2×
> vs the reference (same board2 / fw / cap) is an open measurement/config question, not overload. The
> wide bands calibrated to *this rig* stay valid. Below is the original (incorrect) note, kept for the
> record.

~~The gate is honest and works, but the absolute mA read ~1.6–2× the reference because the AP blasts
the DUT at full TX from cm away. v2: cap the AP's TX too — the ESP AP via `mmwlan_override_max_tx_power`,
and the Linux AP via the `morse` `tx_max_power_mbm` module param on chronite — for reference-like numbers,
then a tighter recalibration.~~ Optional hardening (still valid): the D5→PPK2-logic-input wire for inline
phase self-labeling.
