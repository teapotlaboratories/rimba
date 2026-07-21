# Rimba — master TODO / roadmap

A single high-level view of outstanding work. **This is an index, not a backlog** —
the detailed, authoritative TODO lists live in the per-area docs linked below.
Update *those*; keep this page to one line per item with a pointer. Status:
✅ done · ◐ in progress · ☐ todo · 🔒 blocked.

**Where we are:** Phase 1 (IBSS foundation) is validated on hardware; we're in the
**L2-decision sub-phase** — building both candidate link layers (IBSS and the
Mesh-gate) to compare before committing.

---

## Now — current focus

### L2 link layer — pick IBSS vs Mesh-gate (build both, compare)
- ◐ **IBSS hardening** → [`ibss/rimba-ibss-milestones.md`](ibss/rimba-ibss-milestones.md) (TODO section). Headline open items: hop-by-hop **CCMP** (#3, 🔒 on the firmware station-handle), **dynamic create-else-join** (#7), beacon contention (#5).
- ◐ **Mesh-gate (Mesh + AP)** → [`mesh-ap/rimba-mesh-ap-milestones.md`](mesh-ap/rimba-mesh-ap-milestones.md) (TODO section). The 802.11s mesh half is **built (P0–P6b ✅)** and **mesh + AP concurrency on one radio (A3) is ✅ done** (both vifs beacon concurrently; STA→gate→mesh→far-node forwards end-to-end; app `firmware/rimba-halow-mesh-ap`). **The remaining structural item — and the next big task — is the [802.11s Mesh-gate port](mesh-ap/rimba-mesh-ap-mesh-gate-discovery-design.md) (S1–S6, ~12–19 d, approved 2026-07-10, not started):** follow-Linux gate discovery = RANN + IS_GATE + learned MPP, L2-bridged single subnet. Other open items: **RAW** AP-side port, **AID ≥ 64** on-air validation, Linux STA as TWT requester. *(Action-frame TWT now ✅ — [PR #15](https://github.com/teapotlaboratories/rimba/pull/15).)*
- ✅ **802.11s mesh port (P0–P6b)** → [`mesh-ap/rimba-mesh-ap-milestones.md`](mesh-ap/rimba-mesh-ap-milestones.md) (§802.11s mesh point) + worklog [`worklog/2026-06-26-mesh-mpm-peering-frames.md`](worklog/2026-06-26-mesh-mpm-peering-frames.md). An ESP32 joins a Linux HaLow mesh, pings, originates + **relays** multi-hop, group-forwards, and does **PERR** broken-link teardown.
- ✅ **Mesh throughput (P6c partial)** — **airtime metric** ✅ (byte-exact `airtime_link_metric_get` port, 2026-07-02) · **AMPE/SAE + CCMP** ✅ · **iperf throughput test** ✅ · **real per-peer rate control** ✅ (**~2.2×**) · **A-MPDU aggregation S0–S3** ✅ (**~37% single-hop, ~38% relay**; teapotlaboratories/mm-esp32-halow#23 + #33, 2026-07-15). Mesh multi-hop went **~0.03–0.06 → ~0.5–1.7 Mbit/s**; the ceiling is now the **bench RF link (RX overload)**, not the code. **P6c remainder:** RANN/root + proxy/gate (`mpp`) — both are the Mesh-gate port above — and **mesh power save**.
- ☐ **Mesh hw-restart recovery** → mesh-ap backlog. **A mesh node cannot survive any `hw_restart` today** — no `umac_mesh_handle_hw_restarted`, so a chip reset wipes the vifs and the node goes **silently deaf** (bench-proven 2026-07-15, both A/B arms). Prerequisite for revisiting FIX-1 (removed; archived at halow tag `archive/fix1-implementation`).
- ☐ **RAW (Restricted Access Window) — AP-side, port from Linux** → [`mesh-ap/rimba-mesh-ap-milestones.md`](mesh-ap/rimba-mesh-ap-milestones.md) (TODO, RAW item). Big standalone feature: port the RPS IE + AID grouping + slot definition from `morse_driver` `raw.c` (1742 lines) + `page_slicing.c`, follow Linux exactly, write the new-code↔Linux map. **Recon/feasibility pass first** (does the fw accept the RAW config cmd; minimum-viable scope). Its own branch + PR.
- ☐ **Decision:** choose the L2 after the comparison (or define when each applies). Trade-off table in both milestone docs.

### Power-save (early focus — the battery-leaf model rests on it)
- ✅ **RISK-02** radio cold-boot-to-joined time — measured ≈1.39 s (2026-06-21).
- ☐ **RTC-scheduled "Scheduled mode"** prototype (ESP32 + RTC drives the duty cycle; chip has no IBSS radio PS) → ibss TODO #8; context in [`design-specification/rimba-mm6108-powersave-analysis.md`](design-specification/rimba-mm6108-powersave-analysis.md).

### Cross-cutting
- ✅ **Regression suite** across every built feature so fw/morselib bumps don't silently regress milestones — harness `tools/regtest/` (T0 build / T1 smoke / T2 on-air + `tp` power + `dscycle`), see [`tools/regtest/README.md`](../tools/regtest/README.md) + [`regression/rimba-regression-results.md`](regression/rimba-regression-results.md). **Full bench run 2026-07-18 all-green: T0 28/0/1-skip/27-xfail · T1 12/0/2-skip · T2 15/0 · tp 4/0** (dscycle INCONCLUSIVE = flaky C6 wake, non-gating). First run found the pre-existing `BOARD=proto1` break (missing `bcf_mf16858` BCF, XFAIL). Lands via **PR [#35](https://github.com/teapotlaboratories/rimba/pull/35)** (+ submodule accessors **teapotlaboratories/mm-esp32-halow#24**, merge that first). Worklogs: [`2026-07-16-…`](worklog/2026-07-16-regression-suite-and-fork-migration-plan.md), [`2026-07-18-regression-run-and-apsta-ap-marker.md`](worklog/2026-07-18-regression-run-and-apsta-ap-marker.md).
  - ✅ **firmware/ reshape + `test-*` rename** (2026-07-17/18): split `firmware/` into `test-*` (the suite + the fixtures the harness flashes: `test-idle`/`test-deepsleep-cycle`/`test-c6-trigger`) vs **11 curated `rimba-*` example apps** (scaffolding stripped, README each; 5 experiment apps removed; `rimba-halow-sta` rebuilt clean). Then renamed the app prefix `regtest-`→`test-` (dirs + `project()` + the `TEST|` console contract), keeping the `tools/regtest/` dir + `make test-*` targets. Worklog [`2026-07-17-firmware-regtest-split.md`](worklog/2026-07-17-firmware-regtest-split.md).
  - ◐ **Suite hardening + 3 new mesh T2 tests** (2026-07-20, developed on the pre-forward-port morselib 2.10.4 baseline, UNCOMMITTED). **Batch A** — `manifest._validate_bench()` config-validation guards (WIRED_BOARD typo → no wired DUT; malformed `BOARDx_MAC`; two boards colliding on the derived `10.9.9.x`) + a `make test-unit` target with 10 stdlib-unittest tests. **3 new T2 tests** reusing the mesh-relay forced-line rig + existing accessors (T2 12→15): `mesh-large-frame` (1400 B payload → FW fragmentation → the defrag-before-decrypt RX path), `mesh-leaf` (relay `mmwlan_mesh_set_multihop(false)`; origin inverts → PASS iff peering survives AND 0 replies), `mesh-relay-nocrash` (relay is the reporter, asserts `hw_restart_counter` unchanged across a forwarding window — the interrupt-WDT crash). All build green on 2.10.4; **NEXT** = bench-verify → 2.12.3 forward-compat → land. First slice of the "Loop-back: harden" umbrella below. Worklog [`2026-07-20-regtest-hardening-new-testcases.md`](worklog/2026-07-20-regtest-hardening-new-testcases.md).
    - ☐ **Deferred pre-merge-review follow-ups (non-blocking)**: (a) `mesh-relay-nocrash` gates PASS on a peer-count load-proxy (`peer_count>=2`), so a rare origin `esp_ping_new_session` failure could false-PASS under zero forwarding load — corroborate real load with a datapath forwarded-frame counter, not just the plink count; (b) the negative-assertion tests (`mesh-leaf` 0-reply, `mesh-relay-nocrash` no-crash) can't distinguish "correctly blocked / survived" from "rig silently under-delivered" when run in isolation — mitigated by the full suite (`mesh-relay` exercises the same dest/relay setup with a positive delivery assert). (Fixed in the same pass: `_validate_bench` now full-shape-validates every MAC, not just octets 0/5.)
  - ✅ **`make test-conn` preflight** — check a device is reachable before a test run (`PORT=`/`MAC=`/`NODE=`/`HOST=`; esptool chip_id for ESPs, ssh for Linux nodes; no args → probe all).
  - ✅ **`tp` power-save tier (PPK2)** — meters board2's PS ladder off the PPK2 + gates a gross current regression (the fw-1.17.9 ~2× kind), raw mA always recorded. `make test-tp AP=esp|linux`. Calibrated to this rig's 1.17.8 baseline; fresh run = **4 PASS**. Worklog [`2026-07-16-ppk2-power-regression-tier.md`](worklog/2026-07-16-ppk2-power-regression-tier.md).
    - ☐ **v2 (harden):** cap the AP's TX (`mmwlan_override_max_tx_power` on the ESP AP; `morse` `tx_max_power_mbm` on chronite) for reference-like numbers, then a tighter recalibration that catches a smaller regression.
  - ✅ **5 more power-save test cases** (2026-07-17): FW-blob version-pin (T0), assoc-embedded TWT → INSTALLED (T2 `twt-assoc`, both APs), multi-STA TWT (T2 `multi-twt`), deep-sleep reconnect (`make test-dscycle`), and a `test-tp LIGHT_SLEEP=1` variant. Worklog [`2026-07-17-powersave-test-cases-batch.md`](worklog/2026-07-17-powersave-test-cases-batch.md). (deep-sleep lands honest INCONCLUSIVE on a stressed board2 — a clean 3/3 needs a rested board2; the LS "stronger gate" needs v2.)
  - ☐ **`test-apsta-ap` app_main stall after AP netif-up** — the SoftAP support app's `app_main` task stops producing console output the instant the AP netif comes up (`esp_netif_action_connected`); the SoftAP itself is fine (a STA associates + pings 15/15 through it), so the `ap-ready` up-marker was moved *before* that call as an interim. Worked in the 2026-07-17 14:00 run, surfaced by a clean rebuild against the currently-modified `components/halow` — suspect a main-task stack overflow or an esp_netif/morselib AP-path change. Root-cause it and revert the interim. Worklog [`2026-07-18-regression-run-and-apsta-ap-marker.md`](worklog/2026-07-18-regression-run-and-apsta-ap-marker.md).
  - ✅ **Assoc-embedded TWT requester — FIXED (a Hop-1 regression; first mis-triaged as pre-existing, then root-caused 2026-07-19).** T2 `twt-assoc` went **PASS (2.10.4) → INCONCLUSIVE (2.11.2) → PASS (fixed)**. Root cause: the hostap 2.11.2 forward-port tightened the assoc-time TWT-seed guard in `wpa_supplicant.c` (2 sites, ~1450/8310) from `#ifdef CONFIG_S1G_TWT` to `#if defined(CONFIG_S1G_TWT) && defined(CONFIG_DRIVER_NL80211_MORSE)`. The ESP uses the `mmwlan` driver and defines **`MM_IOT`**, not `CONFIG_DRIVER_NL80211_MORSE`, so the whole assoc-time TWT-config block **compiled OUT** → the `.twt_conf` driver op (`mmwpas_twt_conf`, driver.c) never fired → no `PENDING_RESPONSE` agreement seeded → `ie_twt_build` skipped the assoc-req TWT IE → no negotiation → INCO. It compiles either way, which is why compile-verify missed it — the exact "268-file hostap update = bench risk" the fork plan flagged. **Fix:** guard `+= || defined(MM_IOT)` (the codebase's own idiom, `driver.h:32`). Verified via serial probes (`.twt_conf` fires → agreement seeded → assoc-req IE emitted → AP grant → INSTALLED on flow 0) + harness `twt-assoc` PASS. Detail: [`worklog/2026-07-19-sdk-hop1-2.11.2-report.md`](worklog/2026-07-19-sdk-hop1-2.11.2-report.md).
  - ✅ **Serial-capture retry hardening** (2026-07-18, uncommitted) — stress-testing the harness (loop the hardware tiers, no retries) characterized the T1 flake as a **standing bench issue**: board2's PPK2-powered USB port occasionally **re-enumerates mid-capture** (~2–3 %/capture, ~1 in 4 runs, not app-specific; 0 on bus-powered board0/board1), which the harness treated as a hard gating FAIL. Fix in `tools/regtest/common.py` (`_capture`/`_await_port`): re-resolve the port by efuse MAC + recapture once on a mid-capture `SerialException`, still re-raising (FAIL) for a persistently-dead board; retry is logged. Unit-tested (both branches) + 120 post-fix captures 0-fail (no regression); a *live* transient recovery went unobserved (honest caveat). Worklog [`2026-07-18-bench-stress-and-capture-retry.md`](worklog/2026-07-18-bench-stress-and-capture-retry.md).
    - ☐ **Loud board2-not-powered signal** — a board2 tier currently **SKIPs silently** when `ppk2_hold` isn't running (reads as "fine" in an automated run); warn loudly (or optionally auto-start the holder). Also `dscycle`'s `pgrep -f ppk2_hold.py | kill -9` is broad enough to kill any process merely *mentioning* the holder — tighten the match.
  - ☐ **Loop-back: harden the test-cases + the harness** — a dedicated robustness pass once the current features are landed. The green-baseline runs proved *coverage*; this pass is about making the suite *trustworthy under bench flakiness* so a re-run gives the same verdict. Gathers the open items above + the gaps this session surfaced:
    - **Run-history / flake ledger.** Baselines are latest-wins (`build/regtest/<tier>-latest.json`), so a passing retry **silently overwrites** a failed run — a flake leaves no durable trace (this is what erased the first `rimba-halow-sta` FAIL). Keep a rolling per-tier history + a flake log so retried transients are *recorded*, not lost, and the flake rate stays visible.
    - **Lint gate on harness edits.** A `bench`-out-of-scope edit passed `py_compile` and only failed on the bench (10/12 FAIL). Wire **pyflakes** (undefined names) into the preflight / a pre-run check so that class of bug is caught off-bench.
    - **Live-verify the serial-capture retry.** The [[board2-serial-capture-flake]] fix is unit-tested + regression-clean, but a *live* transient recovery on-bench went **unobserved**; deliberately inject one (e.g. a brief PPK2-rail glitch mid-capture) to confirm the re-resolve+recapture path fires for real, and close the caveat.
    - **Reduce the board2 flake at source, not just tolerate it.** The retry masks a ~2–3 %/capture USB re-enumeration; root-cause the PPK2-rail wobble (power/cabling/hold-script) to lower the underlying rate.
    - **Robustness audit of each T2 test.** Review every feature test for flaky assertions, too-tight timeouts, and RF-noise tolerance (wide floors / binary proxies, not equalities) so an on-air test fails only on a real regression.
    - **Make `dscycle` reliable + self-contained.** The deep-sleep gate lands INCONCLUSIVE on a flaky C6 wake; give it a dependable wake (rested board2 / better trigger) and have it **restart `ppk2_hold` itself** after teardown instead of leaving it to the operator.
    - Also rolls up the standing sub-items: `tp` v2 (cap AP TX + recalibrate), the `test-apsta-ap` app_main stall, and the loud board2-not-powered / tighten-dscycle-kill item above. Worklog [`2026-07-18-bench-stress-and-capture-retry.md`](worklog/2026-07-18-bench-stress-and-capture-retry.md).
    - **PR [#36](https://github.com/teapotlaboratories/rimba/pull/36) review findings (2026-07-18)** — filed here (deferred, not merge-blocking; none is a false-PASS). Fix in the hardening pass:
      - **`make test-all` never runs dscycle** (`Makefile:159`): it runs `test-tp` right before `test-dscycle`, but tp kills `ppk2_hold` and doesn't restart it, so dscycle finds board2 un-enumerated and SKIPs (non-gating, so `test-all` still reads green). Restart the holder between the tiers, or reorder, or have dscycle self-power board2 (overlaps the "make dscycle reliable" item).
      - **`require_linux` accepts a missing `LINUX_MAC`** (`manifest.py:174`): `_build_linux` needs only HOST+IP, but `require_linux` errors only on an empty registry — so `LINUX_MAC=""` passes and mesh-linux builds with an empty peer MAC silently. Make the builder+validator agree.
      - **`WIRED_BOARD` typo not validated** (`manifest.py:160`): an out-of-set value (e.g. `board3`) yields a bench with **no** `fully_wired` board yet `require_bench` passes; error loudly instead.
      - **Malformed `BOARDx_MAC` silently accepted** (`manifest.py:152`): only checked non-empty, so a typo'd MAC yields empty derived `mesh_mac`/`mesh_ip` and a board that never resolves; validate the MAC shape.
      - **Capture retry can false-FAIL the contention sub-case** (`common.py:347`): the retry assumes re-enumeration reset the board and it replays; for a pure "multiple access on port" (board still alive, verdict already printed in the discarded buffer) the fresh window sees nothing → spurious FAIL. Fails-closed (never a false PASS), so low priority.
      - **`linux:<host>` role label ignored** (`t2_onair.py:305`): all `linux:` roles now resolve to `DEFAULT_LINUX_PEER` (=`LINUX_HOST`), so a non-default host is silently redirected. Harmless today (every role is `linux:chronite`); revisit if a 2nd Linux node is configured.
      - **Derived `mesh_ip` can collide** (`manifest.py:138`): two boards sharing `MAC[5]&0x3f` map to the same `10.9.9.x` (the old hardcoded table was distinct). Detect + error on a collision at build time.
- ☐ **Validate PSRAM** memory headroom (RISK-04) → [`rimba-development-plan.md`](rimba-development-plan.md) task 1.8.
- ◐ **Stack bump** (MM6108 fw / morselib / ESP-IDF) — a real port-forward, keep parity with the Linux node → ibss TODO #15. The **morselib forward-port 2.10.4 → 2.12.3 is DONE** (landed 2026-07-20 — see the sub-item below); the **MM6108 fw + ESP-IDF** bumps remain.
  - ✅ **SDK forward-port morselib 2.10.4 → 2.12.3 — DONE + landed 2026-07-20.** The "fork migration" **pivoted** from replaying our work onto `teapotlaboratories/esp-halow` (that fork was deleted) to **bringing MorseMicro's upstream SDK INTO our repo**: forward-ported the `components/halow` submodule (`teapotlaboratories/mm-esp32-halow`) morselib **2.10.4 → 2.11.2 → 2.12.3** in two per-hop tagged commits, keeping our self-contained vendored layout + 24 PRs. Hop 1 = per-VIF datapath re-arch + 6 on-air-verified fixes; Hop 2 = additive relay subsystem (off) + DPP QR; **full 4-tier regression green each hop.** Landed via **`teapotlaboratories/mm-esp32-halow#25`** (tags `2.11.2-esp32-1` / `2.12.3-esp32-1`, merge-committed to keep the SHAs on main) + superproject **[#37](https://github.com/teapotlaboratories/rimba/pull/37)** (gitlink → `85252daf`). Detail: worklog [`2026-07-19-sdk-hop1-2.11.2-report.md`](worklog/2026-07-19-sdk-hop1-2.11.2-report.md) + plan [`2026-07-18-sdk-forwardport-2.12.3-plan.md`](worklog/2026-07-18-sdk-forwardport-2.12.3-plan.md) + memory `fork-migration-esp-halow`.
    - ✅ **beacon.c notify-variant cleanup — DONE 2026-07-20** (`teapotlaboratories/mm-esp32-halow#26` + gitlink bump [#38](https://github.com/teapotlaboratories/rimba/pull/38)): the beacon IRQ handler now uses the plain `driver_task_notify_event` (`morse_beacon_irq_handle` runs in the `morse_spi_irq_main` **task** context, not a hard ISR), matching `morse_hw_irq_handle`'s sibling notifies + `morse_beacon_start`. No functional change; compile-verified.

---

## Next — phased build (above L2, link-agnostic)

The phased plan + per-phase tasks live in [`rimba-development-plan.md`](rimba-development-plan.md) §4.

- ☐ **Phase 2 — Peer links + link security** (Phase-2 crypto shim; pin the SDK, RISK-06).
- ☐ **Phase 3 — Routing & mesh forwarding** (OGM/RREQ, custody-aware).
- 🔒 **Phase 4 — DTN bundle layer (BPv7)** — **RISK-03 BLOCKING**: no ESP32 BPv7 lib; RFC 9171 subset (dev-plan 4.1–4.4).
- ☐ **Phase 5 — Full integration & validation** (incl. adaptive TX power, RISK-05).
- ☐ **Phase 6 — Optional: geographic routing.**

---

## Security & hardening (spans all phases)

Roadmap + tiers in [`design-specification/rimba-hardening-plan.md`](design-specification/rimba-hardening-plan.md); spec issues in
[`design-specification/rimba-protocol-spec.md`](design-specification/rimba-protocol-spec.md) §15–§16.

- ☐ **Issue #13 — config-changeable-parameter scope** **[HIGH / URGENT]** — must be defined before any config-convergence work (spec §15, Issue #13).
- ☐ **Issue #9 — mule custody authentication** **[HIGH]** (hardening §2.4).
- ☐ **Tier 0 threat model → Tier 1 security-failure paths** (highest priority) → hardening-plan; start with "the one thing to do first" (§8).
- ☐ **Tiers 2–4** (DoS / resource exhaustion, protocol edge cases, operational hardening).

---

## Housekeeping

- ✅ Fixed the pre-existing broken worklog link in `worklog/2026-06-22-mesh-ap-twt.md` (the never-created `2026-06-21-i4-beacon-source-addr-firmware-wall.md` → redirected to `2026-06-21-phase1-validation-complete.md` + dev-plan RISK-02). The `ibss/rimba-ibss-milestones.md:349` mention is honest prose ("referenced but never created"), not a link — left as-is.

---

## Authoritative backlogs (edit these — this page only points to them)

| Area | Doc | What it holds |
|---|---|---|
| IBSS L2 | [`ibss/rimba-ibss-milestones.md`](ibss/rimba-ibss-milestones.md) | Milestones, Linux maps, fork comparison, **IBSS TODO**, findings |
| Mesh-gate L2 | [`mesh-ap/rimba-mesh-ap-milestones.md`](mesh-ap/rimba-mesh-ap-milestones.md) | Milestones, Linux maps, **Mesh-gate TODO** |
| Phased plan + risks | [`rimba-development-plan.md`](rimba-development-plan.md) | Phases 1–6, risk register, per-phase tasks |
| Security | [`design-specification/rimba-hardening-plan.md`](design-specification/rimba-hardening-plan.md) | Threat model + Tier 0–4 |
| Spec | [`design-specification/rimba-protocol-spec.md`](design-specification/rimba-protocol-spec.md) | §15 Open Issues, §16 Future Investigations |
| Validation results | [`ibss/rimba-ibss-test-plan.md`](ibss/rimba-ibss-test-plan.md) | P0 / I.1–I.5 results |
