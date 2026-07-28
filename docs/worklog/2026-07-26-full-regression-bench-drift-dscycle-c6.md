# 2026-07-26 — Full regression on the boottime-rig branch: bench drift, a USB hub fault, and a silent-failure fix

**Goal.** Land the `test-ibss-boottime` fixture (RISK-02 rig) and prove nothing else broke, by running
every regression tier. What actually came out of it: two bench-doc findings, a USB **hub** fault (first
misattributed to the C6 — see §7), a new `.ai` rule, and a harness bug that had been misreporting a
bench failure as a DUT failure.

**Outcome.** All tiers green. Two PRs open (`47` boottime rig, `48` dscycle fix), three doc-only commits
on `main`. The C6 trigger was initially blamed on hardware — **§7 records why that was wrong.**

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

**Finding 2 — the C6 port is pinned to a number that moves.** Doc `:76` said `/dev/ttyUSB0`; at the time
of checking it was on `ttyUSB1`. Three fixed references replaced with resolve-by-serial, including the
flash command, which now derives the port from `/dev/serial/by-id` (verified: resolves to the live port).
*(Revised — §7: the C6 is back on `ttyUSB0`, so the documented value was **fragile, not wrong**. The fix
stands, since the number demonstrably bounces; calling it "stale" was the overstatement.)*

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

**Root cause (as concluded at the time — see §7 for the revision): the C6 re-enumerated mid-run.** Its
`by-id` symlink is timestamped 19:02 and pointed at `ttyUSB1`; at session start (15:10) it was
`ttyUSB0`. dscycle ran 19:00:26→19:04. **⚠ This rests on the symlink mtime alone** — the kernel journal
only retains from 23:07:12, so there is no bus-level evidence for the 19:02 event either way. `_C6` opens the port
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

- **The C6 hardware is NOT implicated** — see §7. What is evidenced is a *hub* fault. Worth doing:
  move the C6 off the `7-1.4` hub it shares with the PPK2.
- **PRs `47` and `48` await `/review <PR #>`** before merge (per the rule updated this session).
- **The repo-wide docs audit is still outstanding** — the original goal of the session. Efficient shape
  is known: batched per-unit verification (~22 agents, not ~570 — this box has 6 cores so workflow
  concurrency is 4) plus a *scripted* verbatim-quote check. Tonight's two verified bench-doc findings
  are already fixed and should not be re-derived.
- **Bench left warm**: `ppk2_hold` running, board2 powered on `test-idle`, all ESPs radio-silent,
  chronite/chronosalt `wlan1` DOWN.
- `docs/worklog/2026-07-25-docs-audit-findings.md` remains untracked by choice (unverified list).

## 7. Correction (2026-07-27) — the C6 hardware was NOT at fault

§5 and §6 originally concluded "the C6 dropped off USB twice — a flaky cable, hub, or port" and called
for a physical check. **That was wrong**, and it was reached without looking at the kernel log. Prompted
by the owner pushing back ("C6 physical is always good, are you sure it's not something else?"), the
claim was re-verified against `journalctl -k`. Recorded here rather than edited away, because the wrong
call and the way it was reached are the useful part.

**What the kernel actually logged — one event, and it is the hub, not the C6** (2026-07-26 23:07:13):

```
usb 7-1.4: clear tt 1 (9082) error -71        <- x19 (EPROTO on the hub's transaction translator)
usb 7-1.4.2: USB disconnect, device number 16
cp210x ttyUSB1: cp210x converter now disconnected from ttyUSB1
usb 7-1.4: new high-speed USB device number 26   <- the HUB itself re-enumerated
usb 7-1.4.1: PPK2                                <- both children returned
usb 7-1.4.2: CP2102N -> ttyUSB0
```

`7-1.4` is a VIA VL812. The **PPK2 (`7-1.4.1`) and the C6 (`7-1.4.2`) are both behind it**, both
full-speed (12M) devices under a high-speed hub, so they share one Transaction Translator. Nineteen TT
protocol errors then a hub reset that took both children down. A single device with a bad cable does not
produce that signature. And the PPK2 on that hub is sourcing board2's 5 V DUT rail, so board2's radio
current pulls through the same port — this bench already has a documented power-margin problem
(chronosalt browns out on radio-up, §"At a glance" of the bench doc).

**Three specific claims in §5/§6 that do not hold:**

1. **"Dropped off USB twice" — unsupported.** There is exactly ONE USB-level event in the log. The
   `errno 5 Input/output error` during the later dscycle has **no corresponding disconnect**, and the
   journal *does* cover that window (checked 23:40–00:15: zero non-board2 USB events). The device never
   went away that time.
2. **The 19:02 re-enumeration has no kernel evidence either way** — the journal only retains from
   23:07:12. It was inferred purely from the `by-id` symlink mtime.
3. **"The C6 port is stale" (§4 Defect 2) was overstated.** The C6 is back on `ttyUSB0` — where the doc
   originally said. The documented value was *fragile*, not *wrong*. The doc fix (resolve by USB serial)
   still stands on its merits, since the number demonstrably bounces; only the justification was wrong.

**Hypotheses tested for the `errno 5`, all refuted:**

| Hypothesis | Result |
|---|---|
| Bus/hub event | Ruled out — kernel logged nothing on the C6 or its hub in that window |
| Handle contention (two processes on one tty) | **Refuted by experiment** — 6 pulses from a held handle while a second process repeatedly opened/wrote/closed the same tty: 0 write failures |
| USB autosuspend | Ruled out — `/sys/bus/usb/devices/7-1.4.2/power/control` = `on` (never suspends) |
| board2 re-enumeration storm | Weak — only 8 disconnect/connect cycles in the 35-minute window |

**Disposition.** The `errno 5` is **one unexplained, unreproduced transient** — stated as that rather
than replaced with a second guess. Not worth more bench time until it recurs, and PR `48` now makes a
recurrence self-diagnosing (it names the trigger, logs the exact errno, and marks the result a BENCH
FAULT rather than blaming the DUT). The earlier worry that `48` "masks a harness bug" is also refuted:
contention was the candidate bug, and it does not reproduce.

**The one real action:** the C6 and the PPK2 share hub `7-1.4`, and that hub demonstrably reset while
the PPK2 was driving board2's rail. Moving the C6 to a different hub or a root port costs nothing and
removes a proven single point of failure. That is a topology change, not a cable swap.

**Method note worth keeping:** the original diagnosis came from a `by-id` symlink timestamp and a
Python exception string. Neither is bus-level evidence. `journalctl -k` was available the whole time and
would have shown the hub reset on the first pass — check the kernel log before attributing a USB symptom
to a device.
