# Rimba regression-suite results

The living record of what the regression suite (`tools/regtest/`, `make test-t0|t1|t2`)
actually reports on the current tree. This is the **results** doc; the suite's design,
tiers, and how-to are in [`tools/regtest/README.md`](../../tools/regtest/README.md), and the
build-out is in the worklog
[`2026-07-16-regression-suite-and-fork-migration-plan.md`](../worklog/2026-07-16-regression-suite-and-fork-migration-plan.md).

**Update this doc whenever the suite is re-run** (especially after a firmware / morselib /
ESP-IDF bump or the fork migration). Each tier's machine-readable baseline is written to
`build/regtest/<tier>-latest.json`, stamped with the repo SHA + `components/halow` gitlink, so
every table below is regenerable and attributable to an exact tree state.

> **Firmware reshaping — 2026-07-17.** `firmware/` was split into curated user examples and a
> regtest fixture namespace. The radio-silent fixture is now **`test-idle`** (was `rimba-hello`,
> which stays as the bring-up example); the dscycle DUT is **`test-deepsleep-cycle`**; the C6
> trigger is **`test-c6-trigger`** (was `c6-harness`). The experiment apps `rimba-doze-hold`,
> `rimba-downlink-test`, `rimba-sleep-test`, `rimba-standby-test`, and `rimba-halow-mesh-perf` were
> removed (their coverage lives in the `tp`/`dscycle` tiers + `test-*`), and `rimba-halow-sta`
> was reworked from the PS-ladder rig into a clean join-AP + ping STA example (its ladder lives on
> as `test-power`).**

---

## Latest run — 2026-07-18 (reshaped + renamed `test-*` tree)

First full bench run after the reshape + the `regtest-`→`test-` rename. **All tiers green** except
dscycle (honest INCONCLUSIVE — flaky C6 wake, non-gating).

| Tier | Result |
|---|---|
| **T0 build** | ✅ 28 PASS / 0 FAIL / 1 SKIP (`test-c6-trigger` esp32c6-excluded). *(This run predated retiring `proto1`; that board's 27 XFAIL are gone — the matrix is now the one bench board.)* |
| **T1 smoke** (board2) | ✅ 12 PASS / 0 FAIL / 2 SKIP (the 2 sleep apps) |
| **T2 on-air** | ✅ 15 PASS / 0 FAIL (12 feature tests + 3 silence) |
| **tp** power (`--ap esp`) | ✅ 4 PASS — No-PS 62.7 mA anchor · Dyn-PS 23.9 ≤ 32 · TWT 23.2 (recorded) · WNM+pd 17.1 ≤ 24 |
| **dscycle** | ⚠ INCONCLUSIVE — 1/2 wake cycles (C6 D5-wake flaky → 60 s backup timer); non-gating |

**One fix landed mid-run.** T2 first showed 3 FAILs — `ap-sta-ping` / `twt` / `multi-twt`, all with
"AP support role never came up: `ap-ready` not seen". Root cause: a **false-FAIL** — `test-apsta-ap`'s
`app_main` task stops emitting console output the instant the AP netif comes up, so the up-marker never
reached the orchestrator. The SoftAP is fully alive (a real STA associated over SAE and got **15/15**
ICMP replies through the "hung" AP). Fix: emit `ap-ready` *before* the stalling netif call → T2 15/15;
the same fix unblocked `tp --ap esp` and dscycle's AP. The underlying `app_main` stall is a separate
firmware follow-up (`rimba-todo.md` + worklog `2026-07-18-regression-run-and-apsta-ap-marker.md`).

### Harness robustness — the board2 serial-capture flake, characterized + hardened (2026-07-18)

Stress-testing the harness (looping the hardware tiers with **no retries** to measure the real flake
rate) characterized the one T1 transient seen above. It is a **standing, low-rate bench issue, not a
code fault**:

- **Rate ~2–3 % per capture (~1 in 4 board2 T1 runs).** ~4 serial-capture flakes across ~168 board2
  captures, on **4 distinct apps** (`rimba-halow-sta`, `rimba-halow-mesh-ap`, and the radio-free
  `rimba-hello`) — so it is the **capture**, not any app. **0 flakes** on bus-powered board0/board1.
- **Root cause:** board2 is PPK2-powered; a brief rail wobble resets the ESP → its USB-serial-JTAG
  **re-enumerates** (often renumbered) mid-read, which pyserial raises as `device reports readiness to
  read but returned no data … multiple access on port?`. Clustered right after `dscycle` power-cycles the
  rail (`dscycle.py:20` warns power-cycling *"destabilised the rail"*); base rate low but nonzero.
- **Fix (uncommitted, code → branch+PR):** `tools/regtest/common.py` — a retry-aware `_capture()` +
  `_await_port()` re-resolves the port by efuse MAC and recaptures a fresh window on a mid-capture
  disconnect; **exhausting the retry re-raises so a genuinely dead board still FAILs**; the retry is
  logged, not silent. `efuse_mac` threaded through `t1_smoke.py:159` + `t2_onair.py:152,236,345`.
- **Verified:** unit test covers the recovery + persistent-fail branches; 10 post-fix runs = **120
  captures, 0 fail** (no regression). **Unverified (honest):** no *live* transient fired in the post-fix
  window, so the fix catching a real on-bench event is **unobserved** — only the mock holds; forcing one
  needs invasive rail-glitching. Full detail: worklog `2026-07-18-bench-stress-and-capture-retry.md`.

---

## Prior run — 2026-07-16

| Tree | Value |
|---|---|
| superproject | `905d1a7` (working tree, +uncommitted suite) |
| `components/halow` gitlink | `5832469d` |
| morselib / fw / chip | 2.10.4 / 1.17.8 / `0x0306` (read off the radio at T1) |
| bench | **all 3 ESP nodes** — board0 + board1 + board2 (via `tools/ppk2_hold.py`); Linux mesh nodes not used |

### Summary

| Tier | Result | What it establishes |
|---|---|---|
| **T0 build** | ✅ **green** — 28 PASS / 0 FAIL / 28 XFAIL / 1 SKIP | every app builds on the bench board, with a real country code baked in |
| **T1 smoke** | ✅ **green** — 14 PASS / 0 FAIL / 4 SKIP | every radio app brings the MM6108 up on real silicon (chip/fw/MAC/country) |
| **T2 on-air** | ✅ **9 of 9 automated** — 8 all-ESP **PASS**; `mesh-linux` verified PASS earlier today but **SKIP** in the latest baseline (chronite went offline — a bench issue, not a regression) | SW-CCMP, AP↔STA, mesh peering, TWT, A-MPDU-cap, IBSS, mesh multi-hop relay, the mesh-gate all PASS; ESP↔Linux interop passes when the Linux node is up |

**Headline:** the tree is green on everything the suite can check, and **T2 now covers every
milestone claim** — all 9 on-air feature tests are automated and passing on the bench (board0 +
board1 + board2 via the PPK2 hold), **including ESP↔Linux mesh interop** (the ESP peers with and
pings a real Linux mac80211/morse_driver node, chronite, brought up + torn down over ssh). The one
red-adjacent signal at the time — `BOARD=proto1` (a pre-existing missing-BCF break, quarantined as
XFAIL) — has since been resolved by **retiring `boards/proto1`**. Nothing the suite tests has regressed.

---

## T0 — build matrix

`make test-t0` · 28 apps on the bench board `proto1-fgh100m` · no hardware · ~25–35 min wall (a
morselib change forces a full relink) · [proves: compiles + real country code; does **not** prove:
any runtime behaviour].

| Board | Result | Detail |
|---|---|---|
| **proto1-fgh100m** (the bench board) | **28 / 28 PASS** | every app compiles; each generated `sdkconfig` carries `CONFIG_HALOW_COUNTRY_CODE="US"` and `CONFIG_IDF_TARGET="esp32s3"` |
| **test-c6-trigger** | 1 SKIP | esp32c6 / standalone project; excluded from the S3 matrix by design |

The 28 are the 16 product `rimba-*` apps plus 12 `test-*` T2 harness apps (`swccmp`,
`apsta-ap`, `apsta-sta`, `mesh-peering`, `twt-sta`, `ampdu-cap`, `ibss`, `mesh-relay`,
`mesh-ap-gate`, `mesh-ap-peer`, `mesh-ap-sta`, `mesh-linux`).

**How the 28 were confirmed green** (an honest note): each app builds green on `proto1-fgh100m` —
verified per-app by `make build` producing a valid `.bin` + `CONFIG_HALOW_COUNTRY_CODE="US"` in the
generated sdkconfig (all 28 present), plus partial full-matrix runs that were each green on every
app they reached. A single uninterrupted `make test-t0` was **not** captured in this session: a
morselib change forces all 28 apps to relink (~30 min total), which repeatedly exceeded a
background-task time limit in this environment. The result is not in doubt (28/28 artifacts green),
but the baseline JSON reflects the last completed partial run, not one clean 28-app pass. The four
`mmwlan*` accessors added this session (`twt_agreement_installed`, `ampdu_capability_advertised`,
`ibss_peer_count`, + the `esp_mesh_ccm_selftest` export) are purely additive, so they cannot break
an app that does not call them — the 6 newest apps built directly against the current morselib.

### Pre-existing break — `BOARD=proto1` — RESOLVED (retired 2026-07-18)

T0's first run surfaced a pre-existing break: `boards/proto1/sdkconfig.defaults` required a
`bcf_mf16858.mbin` the pinned `vendor/morse-firmware` (1.17.8) doesn't ship, so every `proto1` build
failed at CMake configure (`BCF ELF 'bcf_mf16858.bin' not found`). The BCF was silently dropped when
the 2026-06-24 vendor-sourcing change moved firmware to the submodule; the whole bench is
`proto1-fgh100m`, so nobody had noticed. It was quarantined as XFAIL (non-gating).

**Owner decision taken: `boards/proto1` was retired** (deleted). The bench is `proto1-fgh100m`, so no
board needs the `mf16858` BCF. T0 now builds the one bench board; there is no XFAIL board today (the
XFAIL / XPASS machinery remains for the next documented-broken board).

---

## T1 — smoke (flash + boot + radio up)

`make test-t1 BOARD_NAME=board0` · **14 PASS / 0 FAIL / 4 SKIP** · 462 s ·
[proves: the radio boots on real silicon; does **not** prove: any on-air feature — a T1 board
has no peer].

Ran on **board0** (board2, the fully-wired DUT, was not powered). Valid because T1 exercises
only bring-up (light load), where board0's unwired WAKE/BUSY do not matter. **Every one of the
12 radio apps brought the MM6108 up cleanly**, reporting real values (not line-presence — a dead
radio prints an uninitialised struct, which T1 rejects):

```
country=US   chip=0x0306   fw=1.17.8   mac=68:24:99:44:6b:b7   (morselib 2.10.4)
```

| App | Result | App | Result |
|---|---|---|---|
| rimba-hello (radio-free) | ✅ banner + zero HaLow lines | rimba-halow-ibss | ✅ radio up |
| rimba-halow-scan | ✅ radio up | rimba-halow-mesh | ✅ radio up |
| rimba-halow-ap | ✅ radio up | rimba-halow-mesh-ap | ✅ radio up |
| rimba-halow-sta | ✅ radio up | rimba-halow-mesh-perf | ✅ radio up |
| rimba-halow-ap-perf | ✅ radio up | rimba-twt-assoc | ✅ radio up |
| rimba-halow-sta-perf | ✅ radio up | rimba-downlink-test | ✅ radio up |

**Asserted per app:** runtime country == the board overlay's `US` (catches both the "??"
dead-radio trap and a valid-but-wrong region); chip id == `0x0306`; fw contains `1.17.8`; MAC ≠
`00:00:00:00:00:00`; and the `mmwlan_get_version` call did **not** log its error path. board0
was auto-returned to `rimba-hello` (radio-silent) at the end.

### Skipped (4) — with reason

The 4 sleep apps (`rimba-doze-hold`, `rimba-deepsleep-cycle`, `rimba-sleep-test`,
`rimba-standby-test`) are **skipped by default**: a sleep app powers the ESP32-S3 native USB
down and re-enumerates it constantly, so esptool can never land the board in download mode —
every later flash fails "No serial data received" and recovery needs a PPK2 power-cycle, which
board0/board1 do not have. Run them with `--include-sleep-apps` on **board2** only.

### Incidental — the modules self-report `mf16858`

The BCF board description on the bench modules reads `mf16858`, even though the build uses the
`bcf_fgh100mhaamd` BCF. So the silicon identifies as mf16858 but runs clean on the fgh100m BCF.
This would **not** have rescued `boards/proto1` (since retired): that board asked for a
`bcf_mf16858.mbin` *file* the pinned firmware doesn't ship — a build-time file-resolution failure,
independent of which BCF the radio prefers at runtime.

---

## T2 — on-air feature tests

`make test-t2` · **all 8 feature tests PASS** (+ auto radio-silence) · [proves: a milestone claim
still holds on the air; does **not** prove: throughput / power numbers — those are benchmarks, not
gates].

T2 is driven by a **multi-role orchestrator** (`tools/regtest/t2_onair.py`): each test declares
*roles* (role → bench device → firmware app, one marked the **reporter**); the orchestrator
resolves devices by efuse MAC, flashes each role, verifies support roles came up (via an
up-marker), then scrapes the reporter's `TEST|RESULT` verdict and returns every board to
`test-idle`. It enforces the wiring rule in code — a role marked `require_wired` refuses to run
on board0/board1 rather than produce a false bug. **Every app a T2 test flashes is `test-*`**
(no product `rimba-*` app in the loop), so the tests are self-contained harnesses.

### All 9 automated + passing

| Test | Rig (ESP apps all `test-*`) | Result | Evidence |
|---|---|---|---|
| **`swccmp`** — host SW-CCMP correctness | 1 board, no radio | ✅ **PASS** | RFC-3610 Packet-Vector-1 KAT + decrypt round-trip: `rc=0`, ~750 µs |
| **`ampdu-cap`** — FW advertises mesh A-MPDU | board2, 1 board, mesh vif | ✅ **PASS** | `mmwlan_ampdu_capability_advertised()==1` (the same bit the aggregation gate reads) |
| **`ap-sta-ping`** — AP↔STA assoc + ping | `test-apsta-ap` + `test-apsta-sta` (reporter) | ✅ **PASS** | associated (SAE) 11 s, **15/15 ICMP replies** (RTT 10–37 ms), 0 timeouts |
| **`ibss`** — IBSS join + peer records | board0+board1, symmetric `test-ibss` | ✅ **PASS** | joined; **exactly 1 peer record** = the creator's MAC (0-phantom check) |
| **`twt`** — TWT responder agreement | `test-apsta-ap` (responder) + `test-twt-sta` (reporter) | ✅ **PASS** | setup ret=0, **agreement flow 0 → INSTALLED** |
| **`mesh-peering`** — mesh SAE+AMPE → ESTAB | board0+board1, symmetric `test-mesh-peering` | ✅ **PASS** | reporter reached ESTAB (1 peer solo; 2 in the full suite — see note), no Linux anchor |
| **`mesh-linux`** — **ESP↔Linux mesh interop** | **chronite** (real Linux node, over ssh) + board2 `test-mesh-linux` (reporter) | ✅ PASS (verified 3× earlier) / ◻ SKIP now | ESP peered (SAE+AMPE) with chronite `3c:22:7f:37:51:38` + **13/15 ICMP replies** from 10.9.9.2. Currently SKIP: chronite is offline (unreachable over ssh) — the orchestrator SKIPs an unreachable Linux node rather than failing the ESP code |
| **`mesh-relay`** — mesh multi-hop (SW-CCMP) | origin board0 + **relay board2** + dest board1 | ✅ **PASS** | forced line (allowlist), **sole peer = relay**, 12/15 forwarded, `ccmp_failures` not dominant |
| **`mesh-ap`** — mesh + AP concurrency (gate) | **gate board2** + peer board1 + sta board0 (reporter) | ✅ **PASS** | STA→AP→ip_forward→mesh→far-node, 14/15 replies, **ttl=63** (one gate hop) |

Highlights of how each self-verifies structurally (not on RF numbers):

- **`swccmp`** — deterministic KAT; PASS ⇒ the CCM ciphertext + MIC are byte-exact (mesh MICs
  interoperate with Linux/mac80211). A regression here is the "mesh drops ~99% of forwards" failure.
- **`ampdu-cap`** — reads the FW-advertised capability via a new accessor after a mesh vif is up; the
  same bit the aggregation eligibility gate (`umac_datapath.c`) consumes. Single board, no peer.
- **`ibss`** — symmetric app; the reporter asserts **exactly 1** peer record on a 2-node cell, which
  *is* the 0-phantom (divergence-17) check — sharper than a tolerant `≥1`.
- **`twt`** — a new accessor `mmwlan_twt_agreement_installed()` lets the reporter poll the agreement
  to **INSTALLED**, a discrete negotiation outcome rather than a PPK2 doze inference.
- **`mesh-relay`** — a per-role peer allowlist forces a line topology; the reporter proves forwarding
  by construction (its *only* mesh peer is the relay, so any reply from the dest went through it —
  no TTL, since 802.11s relay is L2/one-subnet) + `datapath_rx_ccmp_failures` not dominant.
- **`mesh-ap`** — the STA behind the gate's AP pings a far mesh node and reads **ttl=63**: the gate's
  single `ip4_forward` decrement proves the packet crossed exactly one gate hop between the two
  subnets (a delivery count alone cannot). TTL works here because the gate *does* L3-route between
  192.168.12.0/24 and 10.9.9.0/24 — the opposite of mesh-relay.
- **`mesh-linux`** — the **gold standard**: the ESP joins the same `rimba-smesh` mesh as a real Linux
  node (chronite) and asserts an ESTAB peer whose MAC == the Linux node's, then pings it. This proves
  the SAE+AMPE handshake and the secured data path interoperate with genuine `mac80211` +
  `morse_driver`, not just another ESP running the same morselib. The **Linux side is fully
  orchestrated** by `tools/regtest/linux_peer.py` over ssh: it pushes the reference `wpa-smesh.conf`
  if missing, starts `wpa_supplicant_s1g` in mesh mode, and **tears the node back down** (wlan1 DOWN)
  after — verified. Mesh ID `rimba-smesh` and SAE `rimbamesh2026` match the ESP morselib; only the
  mesh ID differs from the ESP↔ESP tests.

Two small **morselib accessors** were added to make the last structural facts self-verifiable
(all in the `mmwlan*` public glob, so exported unmangled): `mmwlan_twt_agreement_installed`,
`mmwlan_ampdu_capability_advertised`, and `mmwlan_ibss_peer_count`. These sit in the
`components/halow` submodule alongside the earlier `esp_mesh_ccm_selftest` export.

**Adding another test is declarative:** write the reporter firmware (copy the nearest existing
`test-*`), declare its `roles` in `t2_tests.py`, register the app in `manifest.py` — plus a
small morselib accessor if a structural fact isn't in the public API.

**Note on run order (not a bug):** the suite silences every board it touched **once, at the end**
(efficient — no re-flash between tests), so a test's leftover radio state can persist into a later
test. It's harmless here because the tests are order-robust by design: `mesh-peering` asserts
`≥1` peer (it saw 2 in the full run — board0 also heard board2's leftover `ampdu-cap` mesh vif),
and the forced-topology tests (`mesh-relay`, `mesh-ap`) use peer allowlists that reject any stray
peer (so `mesh-relay` still saw *exactly* the relay). If strict per-test isolation is ever wanted,
silence inside the per-test loop instead of the end — at the cost of extra flashes.

---

## What this run did and did not cover

| Covered | Not covered (and why) |
|---|---|
| Every app compiles on the bench board (T0) | any runtime behaviour — a T0-green tree can still have a dead radio |
| Every radio app boots the MM6108 (T1) | power-save doze depth (needs the PPK2 current ladder, not a pass/fail) |
| SW-CCMP crypto + FW A-MPDU capability (T2) | AID ≥ 64 (needs 64+ associated STAs — structurally impossible on 3 boards) |
| AP↔STA ping + IBSS peer records (T2) | HW-crypto mesh forwarding (a firmware limitation, not a regression target) |
| Mesh peering + TWT agreement (T2) | (none — Linux-interop is now covered by `mesh-linux`) |
| Mesh multi-hop relay + the mesh-gate, board2 (T2) | on-air byte-diff vs Linux (a separate manual capture the suite points at, not replaces) |
| **ESP↔Linux mesh interop — peer + ping a real Linux node (T2)** | multi-hop *through* a Linux node (only ESP↔Linux single-hop is covered) |

---

## TP — power-save tier (PPK2), added 2026-07-16

`make test-tp` / `make test-tp AP=esp|linux` · **a new tier, not a T2 test** — a
power verdict comes from a host-side PPK2 current stream the firmware cannot see, so it can never be a
firmware `TEST|RESULT` (T2's model). [proves: the STA PS tiers didn't silently ~2× (a fw/stack bump
regressing power-save); does **not** prove: absolute doze depth — that is a benchmark, deliberately not
a gate]. Full build + design in the worklog
[`2026-07-16-ppk2-power-regression-tier.md`](../worklog/2026-07-16-ppk2-power-regression-tier.md).

The runner owns the PPK2 (mirrors `~/pwr_test/rf_run.py`), power-cycles board2, brings up an AP (ESP
SoftAP `test-apsta-ap` on board0, **or** Linux `hostapd_s1g` on chronite via `linux_peer.bring_up_ap`),
flashes the C6-triggered `test-power` ladder, segments the current stream by the DUT's
`TEST|INFO|phase=N` markers, and scores **Dyn-PS + WNM** against calibrated per-AP bands (No-PS =
validity anchor; TWT recorded, not scored). A **fresh 1.17.8 run = 4 PASS / 0 FAIL**.

**Calibrated to this rig, not the reference.** The bench reads **~1.6–2× the documented reference** on
every doze tier (both APs) but **repeatable**, so the wide gross-multiple gate is calibrated to *this
rig's* 1.17.8 numbers (3 runs/AP: ESP Dyn-PS 21–25 / WNM 13–18; Linux Dyn-PS 22–23 / WNM 14–16). Bands
(`manifest.POWER_BANDS`, `CALIBRATED=True`): `PASS ≤ ~1.3×` the noisiest good run, `FAIL ≥ ~1.9×`
baseline, grey between. A 1.17.9-style ~2–3× regression FAILs; a milder ~1.5× lands INCONCLUSIVE (honest
per the doctrine). The scoring was validated by a synthetic sweep through `_score` (which caught + fixed
a tier-separation guard that was masking regressions as INCONCLUSIVE).

> **The ~2× is NOT RX overload / an uncapped AP (mis-diagnosis REFUTED 2026-07-17).** Both APs are
> already TX-capped — ESP AP 1 dBm (`test-apsta-ap:86`), chronite 0 dBm (`morse.conf
> tx_max_power_mbm=0`) — `mmwlan_override_max_tx_power` is `uint16_t` (min 0 dBm), and both tp paths read
> *equal* (so AP TX isn't the differentiator). At ~0–1 dBm the link is *healthy* (No-PS ~63 mA; overload
> reads *wrong*, ~14 mA). So there is **no v2 TX-cap fix**; the ~2× vs the reference (same board2 / fw /
> cap) is an open MEASUREMENT/config question (our ladder segmentation / DTIM1 averaging vs the ref
> method). The rig-calibrated bands remain valid.

**Rig:** board2 (`require_wired`) on the PPK2 + the **C6** (`firmware/test-c6-trigger` MODE_TRIGGER, pulses
every 30 s, must be powered + wired GPIO20→D5) + an AP. PPK2/board2/C6/AP absent → SKIP or INCONCLUSIVE,
never FAIL. Teardown leaves board2 latched-powered; recover a wedged PPK2 with a **5 s** power-off (2.5 s
leaves a deep-sleep-wedged board2 dark) + a tight esptool flash of test-idle.

## 2026-07-17 — six more test cases + full T2 re-run

Added six "add next" cases (worklog
[`2026-07-17-powersave-test-cases-batch.md`](../worklog/2026-07-17-powersave-test-cases-batch.md)), and
re-ran the full T2 into one baseline = **15 PASS / 0 FAIL** (all **12** feature tests, incl. the new ones):

| test | tier | how to run | result |
|---|---|---|---|
| **FW-blob version-pin** | T0 | `make test-t0` | ✅ PASS (asserts mm6108.bin = 480664 B / sha `ce2702b7…` = 1.17.8 before any flash; FAIL on drift) |
| **`twt-assoc`** | T2 | `make test-t2 TEST=twt-assoc` | ✅ PASS — assoc-embedded TWT → INSTALLED on a **Linux hostapd AP** (the universal path both APs honour) |
| **`multi-twt`** | T2 | `make test-t2 TEST=multi-twt` | ✅ PASS — 2 STAs both INSTALLED (a new multi-reporter harness) |
| **`mesh-ap-multi-twt`** | T2 | `make test-t2 TEST=mesh-ap-multi-twt` | ✅ PASS — the **mesh-gate serving 2 concurrent TWT STAs** (mesh+AP concurrency + per-STA TWT responder table), a combination no other test covered |
| **`deepsleep-reconnect`** | `dscycle` | `make test-dscycle CYCLES=2` | ◐ INCONCLUSIVE on-rig — implemented + logic-verified, but the C6 D5-wake is marginal so the leaf reconnects too slowly (the gate correctly reports INCONCLUSIVE, never a false PASS) |
| **`tp --light-sleep`** | `tp` | `make test-tp AP=linux LIGHT_SLEEP=1` | ✅ PASS — a HOST_LIGHT_SLEEP build-arg variant + `POWER_BANDS_LS` (calibrated ~25.8 mA); the reference's "~7× stronger gate" doesn't reproduce here (same open question as the tp ~2×) |

**Harness bug fixed while building `mesh-ap-multi-twt`:** `cmd_t2`/`cmd_tp` used to call `rep.write()` even
on `--dry-run`, so a dry run clobbered the real baseline with its empty results (and poisoned a later
`--append` seed). Guarded so a dry run never writes the baseline.

**T2 coverage is now 12 feature tests** (was 9): the original 8 all-ESP + `mesh-linux`, plus `twt-assoc`,
`multi-twt`, and `mesh-ap-multi-twt`. Combination coverage: single-STA TWT (`twt`, both APs via
`twt-assoc`), multi-STA TWT on a plain AP (`multi-twt`), and multi-STA TWT **behind the mesh gate**
(`mesh-ap-multi-twt`).

---

## How to reproduce

```sh
make test-bench                          # what hardware is present now
make test-t0                             # build matrix (no hardware)
make test-t1 BOARD_NAME=board0           # smoke (board2 if its PPK2 rail is up)
make test-t2                             # on-air: all 9 feature tests (needs board2 via ppk2_hold + ssh to chronite)
make test-tp AP=esp                      # power-save tier: measure board2's PS ladder off the PPK2 (needs the C6 + an AP)
make test-t2 TEST=mesh-linux    # just one test (the ESP<->Linux interop)
make test-t2 DRY_RUN=1            # the catalogue, no hardware
make test-silence                        # return every ESP to test-idle
```

**Outputs of a run:**
- `build/regtest/{T0,T1,T2}-latest.json` — machine-readable baselines (git/gitlink stamped),
  written **incrementally after each test** (crash-safe: a killed run still leaves what completed).
- `build/regtest/report.html` — a self-contained, theme-aware **HTML report** for humans, auto-
  generated at the end of each run (summary cards + per-test tables with status, timing, and
  collapsible evidence). `make test-report` regenerates it from the baselines without re-running.

**Building a full-suite baseline across short runs.** This environment reaps long-running processes,
so a single `make test-t2` (~13 min) can be killed mid-run. Use `APPEND=1` to accumulate onto the
existing baseline in short, kill-safe invocations (each re-run of a test supersedes its old result):

```sh
make test-t2 TEST="swccmp ampdu-cap ap-sta-ping"          # fresh
make test-t2 APPEND=1 TEST="ibss twt"
make test-t2 APPEND=1 TEST="mesh-peering mesh-linux"
make test-t2 APPEND=1 TEST="mesh-relay"
make test-t2 APPEND=1 TEST="mesh-ap"
```

To record a new run in *this* doc, re-run the tiers and refresh the tables above with the new counts
+ the JSON's git/gitlink stamp.
