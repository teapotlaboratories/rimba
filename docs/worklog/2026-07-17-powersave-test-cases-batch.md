# Power-saving test cases — a 5-feature batch (fw-pin, assoc-TWT, multi-TWT, deep-sleep, LS ladder)

**Status:** shipped (uncommitted), 2026-07-17. Five of the "add next" power-saving test cases from
the verified matrix (docs/worklog/2026-07-16-ppk2-power-regression-tier.md + the artifact) built and
verified on the bench. Each is a delta against what the `tp` tier + T2 `twt` already covered.

The five, in build order (easiest first): **#1 FW-blob version-pin** (T0 tripwire), **#2 assoc-embedded
TWT → INSTALLED** (T2 gate, both APs), **#10 multi-STA TWT — both INSTALLED** (T2 gate + a multi-reporter
harness), **#9 deep-sleep duty-cycle reconnect** (a flapping-port runner), **#8 HOST_LIGHT_SLEEP ladder
variant** (a `tp` build-arg + a second calibration).

## #1 — FW-blob version-pin (T0, no hardware) ✅ PASS

A build-time tripwire for the exact regression the whole `tp` tier exists to catch: a silent bump to
fw 1.17.9 roughly doubles STA power-save current. T0 now asserts, before any board is flashed, that
`vendor/morse-firmware/firmware/mm6108.bin` is exactly the pinned 1.17.8 blob — **size `480664 B` +
sha256 `ce2702b7…`** (`manifest.EXPECTED_FW_SIZE`/`EXPECTED_FW_SHA256`; check in `t0_build.py`
`_check_fw_blob`). Verified: PASS on the pinned blob; FAIL when pointed at the thin-LMAC blob
(`mm6108-tlm.bin`, `473812 B`). Highest value/effort of the batch — catches the regression at the
cheapest choke point and enforces the whole-bench-1.17.8 rule.

## #2 — Assoc-embedded TWT → INSTALLED (T2 `twt-assoc`) ✅ PASS

`firmware/regtest-twt-assoc-sta`: requests TWT via `mmwlan_twt_add_configuration()` **before** connect
(the TWT IE in the assoc request), not the mid-session action the `twt` test uses. This is the
**universal path** that engages on BOTH the ESP SoftAP and a Linux `hostapd_s1g` AP (a Linux AP ignores
the mid-session action), so it's genuinely new coverage.

- **A source caveat was refuted on-bench.** `umac_twt_add_configuration` only *stores* the config
  (`umac_twt.c:339`) and `umac_twt_process_ie` needs a pre-existing agreement (`:252`), so an earlier
  audit pass claimed the assoc-embedded path has *no* INSTALLED accessor. On the bench it DOES:
  `mmwlan_twt_agreement_installed(0)==1` after an assoc-embedded setup, against **both** the ESP AP and
  chronite's hostapd (INSTALLED on flow 0, associated in 3.5 s). The reporter scans flows 0..3 to be
  safe.
- The T2 test uses the **Linux AP** as the discriminator (`Role(device="linux:chronite",
  linux_setup="hostapd-ap")`). Needed a new orchestrator branch: `t2_onair._bring_up_linux` now handles
  `hostapd-ap` (→ `linux_peer.bring_up_ap`), and the `_resolve_build_vars`/dry-run peer lines are guarded
  so a hostapd-ap role (STA associates by SSID) doesn't get the mesh-interop MAC/IP. Verified: `twt-assoc`
  PASS end-to-end through the harness (chronite hostapd bring-up → board1 reporter → INSTALLED → teardown).

## #10 — Multi-STA TWT: both INSTALLED (T2 `multi-twt`) ✅ PASS

Two STAs concurrently negotiating TWT against one ESP SoftAP — the "2 authorized STAs" concurrency the
single-STA `twt` test asserts but never runs. **No new firmware** (reuses `regtest-twt-sta` on board1 +
board2). The work was a **multi-reporter harness extension**: `t2_onair._run_orchestrated` now collects
*all* `reporter=True` roles, flashes+captures each (the first stays associated while the next joins, so
both coexist), and `_record_multi_verdict` ANDs their verdicts (PASS iff every reporter PASSed; any
FAIL/no-RESULT → FAIL). The single-reporter path is preserved (re-ran `twt` = PASS after the refactor).
Verified: `multi-twt` PASS — `2/2 reporters INSTALLED concurrently: sta1@board1=PASS; sta2@board2=PASS`.

## #9 — Deep-sleep duty-cycle reconnect (`dscycle`) — implemented + logic-verified

`tools/regtest/dscycle.py` (a `run.py dscycle` command): flashes `rimba-deepsleep-cycle` on board2, and
over N cycles proves the leaf rejoins on every wake — counting both the `RECONNECTED` lines and the
**long port-gone gaps** that prove genuine deep-sleep (vs reset-cycling). Structural gate, no PPK2.

- **Two subtleties, both handled honestly.** (a) Re-opening the flapping native USB-JTAG **resets the
  just-woken board**, so the firmware's `wake_cause` reads UNDEFINED and "WOKE" is missed — so the runner
  proves deep-sleep from the *port-gone duration* (>4 s = a real deep-sleep wait; a bare reset returns in
  ~2 s), which can't be faked by the reset. (b) The sleep app wedges the USB; reliable recovery is a **5 s**
  PPK2 power-off (2.5 s leaves board2 dark) + a tight `esptool` flash of rimba-hello in the fresh-boot window
  (embedded in `_recover_board2`).
- **Verdict + verification.** First run: a clean **2 reconnects + 2 consistent deep-sleep gaps** (logic
  verified). Later runs on the (repeatedly wedge/recovered, over-stressed) board2 landed **INCONCLUSIVE
  (2/3)** — the wakes fell flaky, and the gate **correctly returned INCONCLUSIVE, never a false PASS**
  (the suite doctrine working). A clean 3/3 PASS needs a healthy board2 + reliable C6 wakes; on a stressed
  board2 the honest INCONCLUSIVE is the right result. The test is code-complete, the recovery works, and
  the logic (reconnect + deep-sleep-gap counting → PASS/INCONCLUSIVE/FAIL) is verified.

## #8 — HOST_LIGHT_SLEEP ladder variant (`tp --light-sleep`) ✅ PASS, with a finding

`HOST_LIGHT_SLEEP` is now a **build-arg** (`#ifndef` guard in `regtest-power/main/app_main.c`; the
Makefile threads `HOST_LIGHT_SLEEP=1` → `-D REGTEST_HOST_LIGHT_SLEEP` → the CMake maps it to the define).
`tp --light-sleep` builds the DUT with it and scores against `manifest.POWER_BANDS_LS`. Calibrated on-rig
(3 runs): LS Dyn-PS **~25.8 mA**, WNM **~22–25 mA** (both APs near-identical), all tight. Bands locked
`dyn-ps (35, 48)` / `wnm (35, 46)`; a fresh run PASSes, a gross regression FAILs, a grey lands
INCONCLUSIVE (synthetic-verified); the host-awake bands are untouched.

- **Finding — the "stronger gate" advantage is masked on this rig.** In the clean reference, light-sleep
  backfires WNM 1.17.8 ~4 mA → 1.17.9 ~30 mA (~7×). But our LS 1.17.8 already reads ~25.8 mA, so the
  multiple vs a 1.17.9-style ~30 mA is no bigger than the host-awake gate. The variant is implemented +
  works + gives tight repeatable numbers, but the reference's dramatic advantage doesn't reproduce here.

### v2 turned out to be a mis-diagnosis (verified)

The plan (and the `tp` worklog) blamed the ~2× elevation on RX overload — "the AP blasts the DUT at full
TX from cm away" — and proposed v2 = cap the AP TX. **That is wrong, verified 2026-07-17:**

- The **ESP AP already caps TX to 1 dBm** (`regtest-apsta-ap/main/app_main.c:86`), and **chronite's Linux
  AP is capped to 0 dBm** (`/etc/modprobe.d/morse.conf` `tx_max_power_mbm=0`). Both APs + the DUT are all
  at ~0–1 dBm.
- `mmwlan_override_max_tx_power` is `uint16_t` — **min 0 dBm**, so a firmware TX cap can go no lower.
- At these levels the link is **healthy** (No-PS ~63 mA — an *overloaded* link reads wrong, ~14 mA per the
  audit's mechanism-#20 A/B), and both tp AP paths read **equal** (ESP 20.6/12.8 ≈ Linux 20.2/11.8) — if
  AP TX drove the elevation, the two paths would differ.

⇒ There is **no v2 firmware change** — the AP TX is already as low as it goes, and the elevation isn't
overload. The ~2× vs the reference (same board2 / same fw / same cap) is an **open question**: most likely
our `tp` *measurement* (ladder segmentation, DTIM1 wake averaging) differs from the reference doc's method,
or a beacon/DTIM config differs. The wide bands calibrated to *this rig* remain valid; a future item is to
reconcile the tp measurement against the reference method (or accept the rig-calibrated numbers as the
baseline, which is what the gate already does).

## Follow-on — `mesh-ap-multi-twt` (completing the combination matrix) ✅ PASS

A coverage gap surfaced: nothing combined the **mesh gate + multi-STA TWT**. `mesh-ap` runs the gate with
ONE non-TWT STA; `multi-twt` runs multi-STA TWT against a PLAIN AP. New T2 test `mesh-ap-multi-twt` closes
it — **no new firmware**: gate = board2 (`regtest-mesh-ap-gate`, whose AP half is SSID `rimba-ping`, same
as the TWT STAs) + two `regtest-twt-sta` STAs (board0 + board1), both `reporter=True` so the multi-reporter
harness ANDs them. Verified: **`2/2 reporters INSTALLED concurrently`** — the gate's SECONDARY AP vif does
serve multiple concurrent TWT agreements while the mesh vif is up (the per-STA responder table under
mesh+AP concurrency), a path nothing had exercised. Full T2 is now **15 PASS / 0 FAIL** (12 feature tests).

**Harness bug fixed here:** `cmd_t2`/`cmd_tp` called `rep.write()` even on `--dry-run`, so a dry run
clobbered the real baseline with empty results (and poisoned a later `--append` seed — it bit this very
test). Guarded so a dry run never writes the baseline.

## Files

- **Firmware:** `firmware/regtest-twt-assoc-sta/` (new); `firmware/regtest-power/` (the
  `HOST_LIGHT_SLEEP` `#ifndef`/CMake build-arg).
- **Harness:** `tools/regtest/t0_build.py` (`_check_fw_blob`); `manifest.py`
  (`FW_BLOB_PATH`/`EXPECTED_FW_SIZE`/`EXPECTED_FW_SHA256`, `App('regtest-twt-assoc-sta')`,
  `POWER_BANDS_LS`); `t2_tests.py` (`TWT_ASSOC`, `MULTI_TWT`); `t2_onair.py` (multi-reporter +
  `_record_multi_verdict` + `hostapd-ap` linux role + build-var/dry-run guards); `linux_peer.py`
  (`bring_up_ap` — already present from the tp tier); `dscycle.py` (new) + `run.py dscycle`;
  `tp_power.py` + `run.py` (`--light-sleep`); `Makefile` (`HOST_LIGHT_SLEEP` thread).
- **Docs:** this worklog (+ HTML render).

## Net + follow-ups

All five in the (already-uncommitted) regtest harness; nothing committed. T0 stays green (fw-blob PASS,
drift clean, the new/changed apps build); the **full T2 = 14 PASS / 0 FAIL** (all 11 feature tests incl.
the new `twt-assoc` + `multi-twt`, one baseline). Follow-ups (revised): **the ~2× elevation is NOT an
AP-TX / overload issue** (see above — both APs already capped, link healthy) — the open item is to
reconcile the `tp` measurement against the reference doc's method (or accept the rig-calibrated bands, as
the gate already does); and a clean **3/3 `dscycle`** needs the C6 D5-wake path to be reliable (it's
marginal right now, so the gate honestly lands INCONCLUSIVE — the test working, not a code gap).
