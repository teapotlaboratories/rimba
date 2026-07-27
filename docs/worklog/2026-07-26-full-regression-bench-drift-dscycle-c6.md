# 2026-07-26 — Full regression on the boottime-rig branch: bench drift, a flaky C6, and a silent-failure fix

**Goal.** Land the `test-ibss-boottime` fixture (RISK-02 rig) and prove nothing else broke, by running
every regression tier. What actually came out of it: two bench-doc defects, a recurring hardware fault,
a new `.ai` rule, and a harness bug that had been misreporting a bench failure as a DUT failure.

**Outcome.** All tiers green. Two PRs open (`47` boottime rig, `48` dscycle fix), three doc-only commits
on `main`. The C6 trigger needs a physical check — that one is not fixable in software.

---

## 1. The T0 manifest guard (the starting point)

`firmware/test-ibss-boottime/` existed in the tree but was absent from `tools/regtest/manifest.py`
`APPS`. `check_manifest_covers_tree` (`manifest.py:592` → `t0_build.py:204`) hard-fails T0 on any
tree↔APPS mismatch, so `make test-all` was red before anything else could run.

Fixed as a **real matrix entry**, not a `boards=()` stub — it is a normal esp32s3 app and should keep
compiling. Placed in its own "RISK-02 boot-time measurement fixture" block after `test-power`, which is
scored the same way (measured, not scored: no `TEST|RESULT`, the verdict is host-side).

Verified: `check_manifest_covers_tree('firmware')` → no drift (41 apps, 40 in the T0 matrix), and
`make build APP=test-ibss-boottime BOARD=proto1-fgh100m` → exit 0.

## 2. Code review of the rig — and a bug the review did not predict

Reviewed the branch diff. Findings applied:

| Finding | Resolution |
|---|---|
| `PEER_IP` duplicated the existing `PING_IP` (`rimba-halow-mesh` already uses `TEST_PING_IP`) | folded in — `IDF_EXTRA_D` is one global namespace, so a second spelling of one concept is a permanent cost |
| `CREATE` un-namespaced in that shared namespace | → `IBSS_CREATE` |
| default peer IP `192.168.13.164` was board0's real bench address | now a documented placeholder; a default **must** exist or the flagless T0 build breaks |
| `esp_ping_new_session` failure silently ignored | logs with `esp_err_to_name` |
| `mmwlan_get_mac_addr` return unchecked | checked; halts loudly (it returns `enum mmwlan_status`, `mmwlan.h:693`) |
| `setup_static_ip` delay asymmetry | **not a defect** — `rimba-halow-ibss:173` waits 1500 ms; the DUT omits it because it lands inside the measured `frame` phase. Documented. |
| no README | added, modelled on `test-power` |

**The bug the review missed.** While verifying the `PEER_IP`→`PING_IP` rename actually threaded through,
the IP literal was **missing from the ELF**. `compile_commands.json` showed `app_main.c` compiling with
`-DTEST_IBSS_CREATE=1` on a build that never passed it.

Cause: idf.py's `CMakeCache.txt` is persistent per `build/<app>/<board>/`, and **both roles share one
build directory**, so the role flag is *sticky*. `make flash … IBSS_CREATE=1` followed by a plain
`make flash` for the DUT silently reflashes the **creator** — two peers, no `BOOTTIME_MS` line, looks
exactly like dead RF.

This is a known footgun: `t2_onair.py:50-77` clears that cache and calls it "the cache-accumulation
footgun that once flashed wrong configs mid-bench". But that reset only runs for **harness-driven**
apps. This fixture is measured-not-scored so the harness never flashes it, and it is the only fixture
with a *role-selecting* flag — it falls straight through the gap.

The app cannot clear idf.py's cache from its own CMakeLists, so instead: each role logs
`ROLE=MEASURE (DUT)` / `ROLE=CREATE (peer)` as its first line, and the README leads with a
`make fullclean`-between-roles warning citing the `t2_onair.py` precedent.

Verified after: clean build of each role compiles the right one (MEASURE sees only `-DTEST_PING_IP`,
CREATE sees both), and the peer IP literal reaches the DUT ELF.

## 3. Full regression

Ran every tier on the branch. Bench: board0 + board1 present, board2 PPK2-gated (I started
`tools/ppk2_hold.py` to power it), C6 on a CP2102N, chronite reachable with `wlan1` DOWN.

| Tier | Result | Notes |
|---|---|---|
| T0 build | **42 PASS / 0 FAIL / 1 SKIP** | 43 entries. SKIP = `test-c6-trigger`, excluded by design (esp32c6 target). `test-ibss-boottime x proto1-fgh100m` PASS. fw-blob pin PASS: `mm6108.bin` 1.17.8, 480664 B, sha `ce2702b7…` |
| T1 smoke | **12 PASS / 0 FAIL / 2 SKIP** | board2. SKIPs are the sleep apps (flashing them wedges USB), covered by dscycle |
| T2 on-air | **22 PASS / 0 FAIL** | all 19 tests + radio-silent restores, **0 INCONCLUSIVE** — notable on this bench, where RF-marginal runs normally land there |
| TP power | **4 PASS / 0 FAIL** | `AP=esp`, full ladder incl. both gated tiers (Dyn-PS, WNM+chip-powerdown) |
| dscycle | **INCONCLUSIVE** → see §5 | regressed from 6/6 PASS (07-21 → 07-24) |

**Two process notes worth keeping.**

- **Do not pipe a long tier through `tail`.** It buffers until exit, so a killed run loses the entire
  log. The first T0 run was reaped after 72 min and its console output was empty — the results survived
  only because the harness writes `build/regtest/T0-latest.json` independently. That JSON is the durable
  record; read it, not the console.
- **Harness background tasks get reaped.** T0 and T1 were both stopped mid-run. `setsid nohup`-detached
  processes survive (same mechanism that keeps `ppk2_hold` alive); the remaining tiers ran that way.
- **dscycle writes its result into the T2 baseline**, so `T2-latest.json` shows 22 PASS + 1 dscycle
  entry. Reading T2 before dscycle finishes gives a different answer than reading it after — I reported
  "T2 zero INCONCLUSIVE" and had to correct it.

## 4. Bench vs. the inventory doc — two real defects

Checked `docs/reference/rimba-bench-devices.md` against live state. Matches: all three ESP efuse MACs,
PPK2 present, fw 1.17.8 confirmed by T0's blob pin, all four Linux nodes reachable.

**Defect 1 — the doc contradicts itself on mesh IPs, and the diagram is the wrong one.**
Prose (`:62`, `:66`, `:73`): chronite `10.9.9.2`, chronosalt **`.3`**, chronogen **`.4`**, mesh spans
`.2`–`.4`. ASCII diagram (`:124-125`): chronosalt **`.4`**, chronogen **`.5`**. Resolved against the live
node rather than by preference — chronosalt reports `wlan1 DOWN 10.9.9.3/24`. **The prose is right.**
The diagram also breaks the doc's own stated `.2`–`.4` range.

**Defect 2 — the C6 port is stale.** Doc `:76` said `/dev/ttyUSB0`; it was on `ttyUSB1`. Three fixed
references replaced with resolve-by-serial, including the flash command, which now derives the port from
`/dev/serial/by-id` (verified: resolves to the live port).

**Not defects:** board2 on `ttyACM2` (doc says `ttyACM4`) and PPK2 on `ttyACM3/4` (doc says `ttyACM2/3`).
The doc explicitly flags these as a snapshot that re-enumerates.

**New rule** in `.ai/AGENTS.md` + `CLAUDE.md`: run `make test-bench` and reconcile against the inventory
before every hardware tier. Splits volatile (`ttyACM*`/`ttyUSB*` numbers, `10.9.9.x` mesh IPs, whether
board2 is powered, which app is flashed) from stable (efuse MAC, USB serial, hostname). A mismatch beyond
port numbering is stop-and-reconcile, and the doc gets fixed in the same session.

## 5. The dscycle failure — root cause, and a harness bug

dscycle went INCONCLUSIVE: *"board2 associated (1 reconnect, latencies 4360ms) but completed 0/2
deep-sleep→wake cycles (0 gaps) — did it deep-sleep and get woken?"*, after many
`commanding C6 wake pulse (retry)` lines. History: **6/6 PASS** 07-21 → 07-24, so this was a real change.

**Root cause: the C6 re-enumerated mid-run.** Its `by-id` symlink is timestamped 19:02 and pointed at
`ttyUSB1`; at session start (15:10) it was `ttyUSB0`. dscycle ran 19:00:26→19:04. `_C6` opens the port
once and holds it (`dscycle.py:99`), and `cmd()` caught every write exception returning `[]` — so every
wake pulse after 19:02 went into a **dead file descriptor**. board2 was never woken.

The C6 itself was fine: it answered `C6|READY mode=trigger param=30`. A re-run passed **2/2**, confirming
the transient.

**The harness bug.** A dead fd and "the C6 answered nothing" were indistinguishable, so a *bench* fault
was reported as a *DUT* fault. Fixed (PR `48`):

- `_C6Gone` — a write that **raises** means the fd is dead and every later command is a no-op. Different
  fact from "no reply" → different signal. `ping()` still swallows it: a prober should answer `False`.
- `_C6.reopen()` — re-resolve by USB serial (the `ttyUSB` number moves) and reopen. The wake loop
  self-heals: on `_C6Gone` it reopens once and re-issues the pulse.
- **Every** non-PASS verdict names the trigger when it died, and `c6_lost` goes into the result `meta`.
- A no-reconnect result is **INCONCLUSIVE, not FAIL**, when the trigger died — a DUT that was never woken
  has not failed.
- Teardown reopens if needed to restore the free-running trigger the `tp` tier depends on.

**The fix's own first version was incomplete, and the bench proved it.** I wired the bench-fault message
into only the 0-cycles branch. A verification run then hit the C6 dying *again* — a hard
`errno 5 Input/output error` — completed 1/2 cycles, landed in the **"flaky link/wake"** branch, and said
nothing about the dead C6. Same class of misdiagnosis, different branch. Also, the unrecoverable path set
`c6_lost` without printing, so the console went silent exactly when it mattered. Both fixed.

**Verification.** Reproduced the failure against the real device — force-closed the fd under a live
handle and confirmed: dead-handle pulse raises (old code returned silently), `ping()` returns `False`
without raising, `reopen()` recovers, pulse works again, trigger restored. The recovery path then fired
**unprompted** on a real run. Happy path re-confirmed after the change: **dscycle PASS 2/2**
(latencies 4490 / 7141 / 6180 ms), `T2: 23 pass, 0 fail, 0 skip`, `meta c6_lost: None`.
`make test-lint` clean, `make test-unit` 53/53.

## 6. Open / next

- **The C6 needs a physical check.** It dropped off USB **twice in one evening** — once moving
  `ttyUSB0`→`ttyUSB1`, once a hard `errno 5`. dscycle was 6/6 PASS 07-21→07-24 and unreliable all night.
  PR `48` makes the harness *report* this correctly; it cannot cure a flaky cable, hub, or port.
- **PRs `47` and `48` await `/review <PR #>`** before merge (per the rule updated this session).
- **The repo-wide docs audit is still outstanding** — the original goal of the session. Efficient shape
  is known: batched per-unit verification (~22 agents, not ~570 — this box has 6 cores so workflow
  concurrency is 4) plus a *scripted* verbatim-quote check. Tonight's two verified bench-doc findings
  are already fixed and should not be re-derived.
- **Bench left warm**: `ppk2_hold` running, board2 powered on `test-idle`, all ESPs radio-silent,
  chronite/chronosalt `wlan1` DOWN.
- `docs/worklog/2026-07-25-docs-audit-findings.md` remains untracked by choice (unverified list).
