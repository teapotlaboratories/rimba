# Rimba regression suite

The suite the backlog has asked for since the start (`docs/rimba-todo.md`,
`docs/mesh-ap/rimba-mesh-ap-milestones.md`, `docs/ibss/rimba-ibss-milestones.md`): a
way to tell whether a firmware / morselib / ESP-IDF bump silently regressed a milestone
that was working. It exists so that after a **stack bump** or a **fork migration**, "is
everything that worked yesterday still working?" has a runnable answer instead of a
bench session of rediscovery.

> **New here? Start with the illustrated guide:**
> [`docs/regression/rimba-regression-suite-guide.md`](../../docs/regression/rimba-regression-suite-guide.md)
> — the tiers, the bench devices, and every test explained with diagrams. This README is the terse
> how-to (commands + flags + layout).

## What it is (and is not)

Three tiers. Each is explicit about **what it proves and what it does not** — a test that
overclaims is worse than no test, because it gets trusted.

| Tier | Needs | Proves | Does NOT prove |
|---|---|---|---|
| **T0 build** | nothing (a laptop) | every app × board compiles via `make`, with a real country code baked in | anything at runtime — a T0-green tree can still have a dead radio |
| **T1 smoke** | one board | flash + boot + the radio really comes up (real chip id / fw / MAC / runtime country) | any on-air feature — a T1 board has no peer |
| **T2 on-air** | a real rig | the milestone claims that matter (assoc, IBSS, TWT, mesh peering/relay, mesh+AP, SW-CCMP…) | throughput / power numbers — those are benchmarks, not pass/fail gates |

A full on-air matrix over 30 apps is not realistic on a 3-board bench, so the suite is
**deliberately partial and honest about it** rather than fake-complete. Every T2 test
that is defined but not yet automated says so, with the concrete blocker, in its own
output (`--dry-run`) — see "Coverage" below.

## Requirements

**Software:**
- Run from the repo root, and drive everything through `make test-*` (never `python
  tools/regtest/run.py` directly, never bare `idf.py` — see "Building" below).
- `make` sources the vendored ESP-IDF env for you, so `pyserial` / `ppk2-api` are importable with **no
  `source export.sh` step**.

### Bench configuration — you pass it, nothing is stored

**The bench identity lives nowhere in the source** — no `manifest.py` registry. Every hardware tier
reads it from make variables (which make forwards to the harness), and errors telling you what to pass
if one is missing.

The easiest way to set them is the tracked template. Copy it once, fill in your bench's values, and
`source` it per shell — the file (`bench.env.sh`) is `.gitignore`d so your identity never gets committed:

```sh
cp bench.env.sh.example bench.env.sh   # from the repo root; then edit in your MACs/host
source bench.env.sh                    # exports BENCH_BOARD, BOARD*_MAC, WIRED_BOARD, LINUX_*
```

`bench.env.sh.example` (the committed template) documents every variable:

```sh
export BENCH_BOARD=proto1-fgh100m          # sdkconfig overlay for the ESP boards
export BOARD0_MAC=E0:72:A1:F8:EF:A4        # board0 efuse MAC  (light-load / spare slot)
export BOARD1_MAC=E0:72:A1:F8:F9:40        # board1 efuse MAC  (light-load / spare slot)
export BOARD2_MAC=E0:72:A1:F8:F0:08        # board2 efuse MAC  (fully-wired DUT/relay/gate slot)
export WIRED_BOARD=board2                  # which board is fully wired (WAKE+BUSY)
export LINUX_HOST=chronite                 # the Linux interop node (only T2 interop + tp --ap linux need it)
export LINUX_MAC=3c:22:7f:37:51:38         # its wlan1 MAC
export LINUX_IP=10.9.9.2                   # its mesh IP
```

You can also skip the file and append the variables on each `make` line — they're ordinary make
variables (e.g. `make test-t1 BOARD_NAME=board2 BENCH_BOARD=proto1-fgh100m BOARD2_MAC=… …`). Sourcing
once is just less to type.

`mesh_mac` / `mesh_ip` / `fully_wired` are **derived** from these — you never pass them. The current
bench's values are in [`docs/reference/rimba-bench-devices.md`](../../docs/reference/rimba-bench-devices.md).
`make test-bench` prints what it resolved. **Every `make test-*` example below assumes these are set**
(sourced, or appended to the command).

#### Worked example — a full run from a clean shell

```sh
# 1. Load your bench identity (once per shell).
source bench.env.sh

# 2. Confirm every device answers before spending time on the suite.
make test-conn

# 3. Run the whole suite — build (T0) + smoke (T1) + on-air (T2) + power (tp) + deep-sleep cycle,
#    then render the HTML report. BOARD_NAME/AP/CYCLES are the only per-run knobs.
make test-all BOARD_NAME=board2 AP=esp CYCLES=2

# …or run one tier at a time, e.g. just the on-air checks:
make test-t2

# open the report
xdg-open tools/regtest/report.html
```

Everything hardware-related in that `test-all` line beyond `BOARD_NAME`/`AP`/`CYCLES` (the three ESP
MACs, which board is wired, the Linux peer host/MAC/IP) came from the `source bench.env.sh` in step 1.

**Hardware, per tier:**

| Tier | Needs |
|---|---|
| **T0** build | nothing — a laptop. |
| **T1** smoke | one ESP board — pass `BOARD_NAME=board0\|board1\|board2` (**required**, no default). |
| **T2** on-air | depends on the test — see the table. |

Per-T2-test hardware (the orchestrator SKIPs a test whose devices aren't present, with the reason):

| T2 test | Boards / nodes |
|---|---|
| `swccmp`, `ampdu-cap` | any 1 ESP board |
| `ap-sta-ping`, `ibss`, `twt`, `mesh-peering` | board0 **+** board1 |
| `mesh-relay`, `mesh-large-frame`, `mesh-leaf`, `mesh-relay-nocrash`, `mesh-ap` | board0 + board1 **+ board2** (board2 is the relay/gate — `require_wired`) |
| `mesh-linux` | board2 **+ a reachable Linux node** (chronite over ssh) |

**Bench setup notes:**
- **board2** is powered by the PPK2 rail and only enumerates while `python tools/ppk2_hold.py` is
  running (in the background). Start it before any test that needs board2. `make test-bench` shows
  what's currently present.
- **Ports are resolved by efuse MAC** automatically (never a cached `ttyACM*`), so hotplug order
  doesn't matter.
- **`mesh-linux`** additionally needs the Linux node reachable by ssh (`ssh chronite` — resolved via
  `~/.ssh/config`, key auth, passwordless sudo). The harness pushes the mesh config, brings the node
  up, and tears it back down for you.
- **Radio-silence is automatic:** T1/T2 flash `test-idle` back to every ESP they touched, and the
  Linux node is taken down after `mesh-linux`.

## Linux nodes as interop peers (the `mesh-linux` test)

A Linux node is a **support role** in an interop test (`device="linux:<host>"`) — the harness brings
it up as a mesh peer over ssh, the ESP is the reporter, and the node is torn down afterwards. What a
node must satisfy, and how to point a test at one:

**Requirements for a node to be an interop peer:**
1. **Reachable by ssh, by hostname** — an entry in `~/.ssh/config` (User + HostName + key auth), so
   `ssh <host>` works with **no password**, and **passwordless `sudo`** on the node (the bring-up runs
   `iw` / `wpa_supplicant_s1g` under sudo). Never a raw IP.
2. **The HaLow stack, matched to the bench** — an **MM6108 radio on `wlan1`** and the Morse software
   at **1.17.8**: the `morse` + `dot11ah` kernel modules, `mm6108.bin` firmware, and
   `wpa_supplicant_s1g` + `iw` on PATH (`/usr/sbin`). A node is built per
   [`docs/reference/rimba-linux-node-setup.md`](../../docs/reference/rimba-linux-node-setup.md); the
   bench inventory is [`docs/reference/rimba-bench-devices.md`](../../docs/reference/rimba-bench-devices.md).
3. **Matching mesh parameters** — mesh ID `rimba-smesh`, `key_mgmt=SAE` / `sae_password=rimbamesh2026`,
   S1G ch27, `dtim_period=1`. **You don't configure these** — the harness pushes
   `docs/reference/captures/wpa-smesh.conf` to the node's `/tmp` if it's missing (tmpfs is wiped by a
   reboot) and starts the mesh from it. They already match the ESP morselib's compiled-in values.

**Where the Linux device info lives (how it's "passed"):**

The ESP reporter still needs the peer's MAC + IP compiled in (it has to recognise and ping the peer),
but they are **build-time arguments, not source constants** — you never edit the firmware. They flow:

    LINUX_HOST / LINUX_MAC / LINUX_IP  (make variables — the interop node)
              └────────────┬────────────────┘
             make … LINUX_MAC=… LINUX_IP=…  →  -D TEST_LINUX_MAC/IP  →  compiled into test-mesh-linux

| What | Where | Note |
|---|---|---|
| the interop node | the make variables `LINUX_HOST` / `LINUX_MAC` / `LINUX_IP` | **you pass these** (exported or per-command) — nothing is stored in the source. `make test-bench` prints what it resolved. |
| which node a test uses | `tools/regtest/t2_tests.py` → the test's `Role(device="linux:<host>")` | there's one configured peer, so every `linux:` role resolves to your `LINUX_HOST` |
| the peer MAC + IP the ESP checks | **passed to the build** as `TEST_LINUX_MAC` / `TEST_LINUX_IP` | derived from your `LINUX_MAC` / `LINUX_IP` and compiled into the reporter; `app_main.c` has a `#ifndef` fallback only if nothing is passed |

So with `LINUX_HOST`/`LINUX_MAC`/`LINUX_IP` set, `make test-t2 TEST=mesh-linux` builds the reporter
against your node — no manual step. Override the peer for one run:

```sh
make test-t2 TEST=mesh-linux LINUX_MAC=68:24:99:44:6b:80 LINUX_IP=10.9.9.4
```

or drive the build directly:

```sh
make flash APP=test-mesh-linux BOARD=proto1-fgh100m LINUX_MAC=68:24:99:44:6b:80 LINUX_IP=10.9.9.4 PORT=…
```

**Easiest of all — don't look the MAC up at all.** `flash-interop` reads the peer's MAC (and mesh IP)
off the node live over ssh, so you only pass the host and the board's port:

```sh
make test-interop HOST=chronite PORT=/dev/ttyACM0
# add GO=1 to also bring the node onto the rimba-smesh mesh, capture the board's PASS/FAIL,
# and take both sides off the air afterwards (the whole interop loop, one command):
make test-interop HOST=chronite PORT=/dev/ttyACM0 GO=1
```

| var | meaning |
|---|---|
| `HOST=` (req) | ssh host of the Linux node; its `wlan1` MAC is read live |
| `PORT=` (req) | the ESP board's serial port to flash |
| `IP=` | override the peer mesh IP (default: the node's live `wlan1` IPv4, else its manifest entry) |
| `LINUX_MAC=` | skip the query and use this MAC (e.g. the node is off but you know it) |
| `GO=1` | bring the mesh up first, capture the verdict, then silence both sides |
| `MONITOR=1` | print the board's `TEST\|` output after flashing (implied by `GO=1`) |

A run-time cross-check (`linux_peer.mesh_mac_matches`) still reads the node's **live** `wlan1` MAC and
**warns** if it differs from what was compiled in — catching a stale build against a re-flashed node.

**To use a different Linux node** (e.g. `chronogen`): give it an `~/.ssh/config` entry (key auth,
passwordless sudo) and set `LINUX_HOST=chronogen LINUX_MAC=… LINUX_IP=…`. That's the whole change —
no source or firmware edit; the build-arg plumbing picks up the MAC/IP you passed.

### The same pattern for ESP-only topology (the `mesh-relay` line)

`mesh-relay` is symmetric — all three boards run `test-mesh-relay` and each self-selects its role
(origin / dest / relay) by MAC — so **every** flash needs the whole line's MACs. Those are
**build-time arguments, not firmware constants**: each relay `Role` declares a `build_mac_var`
(`MESH_ORIGIN_MAC` / `MESH_DEST_MAC` / `MESH_RELAY_MAC`), and the orchestrator feeds each one its
**assigned board's** mesh MAC — which is *derived* from that board's `BOARD*_MAC` you passed — to every
flash. The dest's mesh IP is derived from its MAC too, so it's never a second value to sync.
`make test-t2 TEST=mesh-relay DRY_RUN=1` prints the resolved MACs.

> A `-D` cache var is sticky (both here and for the Linux peer): a *manual* custom build pins the value
> until `make fullclean APP=<app>`. The harness always passes explicit values, so real T2 runs are
> unaffected — this only bites a hand-run bare rebuild in a dir you previously custom-built.

## Quick start

```sh
make test-bench                                      # what hardware is present right now (run this first)

make test-all BOARD_NAME=board2 AP=esp CYCLES=2      # EVERY tier: t0->t1->t2->tp->dscycle + report (full rig)

# ...or a tier at a time:
make test-t0                                         # build matrix — no hardware, ~25–35 min cold / minutes warm
make test-t1 BOARD_NAME=board2                       # smoke (BOARD_NAME required: board0|board1|board2)
make test-t2                                         # all on-air feature tests (needs the rigs above)
make test-tp AP=esp                                  # power-save PPK2 ladder (AP required: esp|linux)
make test-dscycle CYCLES=2                           # deep-sleep reconnect gate
make test    BOARD_NAME=board2                       # just t0 + t1
make test-silence                                    # return every ESP to the radio-free idle app
make test-report                                     # (re)generate build/regtest/report.html from the baselines
```

`make test-all` runs every tier in order even if one FAILs, regenerates the report, and exits
non-zero if any tier had a real FAIL (SKIP / INCONCLUSIVE don't gate). It needs the full rig
(board2 on the PPK2 + the C6, plus board0/board1 for the multi-board T2 tests).

> **Test-run parameters have no defaults.** You always state the board (`BOARD_NAME` / `--board-name`),
> the AP (`AP` / `--ap`), and the cycle count (`--cycles`) so a run never silently picks one for you —
> a missing one errors with a hint instead of quietly running on the wrong device.

**Power-save + newer tests (added 2026-07):**

```sh
make test-tp AP=esp|linux         # PPK2 current ladder (4 tiers, 2 scored; AP required)
make test-tp AP=linux LIGHT_SLEEP=1   # a HOST_LIGHT_SLEEP build variant (separate bands)
make test-dscycle CYCLES=2   # deep-sleep duty-cycle reconnect gate (CYCLES required; board2 + C6)
make test-t2 TEST=twt-assoc  # assoc-embedded TWT -> INSTALLED (universal path, Linux AP)
make test-t2 TEST=multi-twt  # 2 STAs both reach TWT INSTALLED concurrently (plain AP)
make test-t2 TEST=mesh-ap-multi-twt  # 2 TWT STAs behind the MESH GATE (concurrency)
```

**T2 now has 15 feature tests** (was 12): the original 8 all-ESP + `mesh-linux`, then `twt-assoc`,
`multi-twt`, and `mesh-ap-multi-twt`, plus the three mesh-hardening tests `mesh-large-frame`,
`mesh-leaf`, and `mesh-relay-nocrash`. Combination coverage of TWT: single-STA (`twt`, both APs via
`twt-assoc`), multi-STA on a plain AP (`multi-twt`), and multi-STA **behind the mesh gate**
(`mesh-ap-multi-twt`).

T0 also carries a **FW-blob version-pin** — it asserts `vendor/morse-firmware/firmware/mm6108.bin` is the
pinned 1.17.8 blob (size 480664 B + sha256) before any board is flashed, catching a silent bump to the
~2×-power-regressing 1.17.9. `tp`/`dscycle` need board2 on the PPK2 rail (`tools/ppk2_hold.py`); a
deep-sleep-wedged board2 recovers with a **5 s** PPK2 power-off (2.5 s leaves it dark) + a tight esptool
flash of `test-idle`.

Every `make test-*` run auto-refreshes `build/regtest/report.html` (a self-contained page you can
open in a browser); `make test-report` regenerates it from the existing baselines without re-running.

To run **one** T2 test (e.g. just the Linux interop, or just the mesh-gate):

```sh
make test-t2 TEST=mesh-linux     # ESP <-> Linux interop
make test-t2 TEST=mesh-ap        # the mesh-gate
make test-t2 DRY_RUN=1             # list every T2 test + its rig, no hardware
```

Exit code is `0` only when nothing FAILed (SKIP / XFAIL / INCONCLUSIVE don't fail the run).

**Always drive the suite through `make test-*`** — never `python tools/regtest/run.py` directly.
`make` sources the vendored ESP-IDF env for you (so `pyserial` / `ppk2-api` are importable), so no
`source export.sh` is ever needed. Extra knobs are make variables: `TEST=<slug>` / `DRY_RUN=1` on
`test-t2`, `LIGHT_SLEEP=1` / `DRY_RUN=1` on `test-tp`, `INCLUDE_SLEEP=1` on `test-t1`. Run `make help`
to list the targets.

Every run writes a JSON baseline to `build/regtest/<tier>-latest.json`, stamped with the
repo SHA and the `components/halow` gitlink — so a baseline is always attributable to a
specific tree state. Diff two baselines to see what a bump changed.

### Reports — three views of the same results

- **Terminal** — a live `[PASS]/[FAIL]/[SKIP]` line per test as it runs.
- **HTML** (`build/regtest/report.html`) — a **self-contained, theme-aware page** for humans, auto-
  generated at the end of every tier run. Summary cards per tier + a per-test table with status
  badges, timing, detail, and collapsible `evidence`. Open it in a browser or share the single file.
  Regenerate anytime from the existing baselines (no re-run) with `make test-report` /
  `make test-report`. It shows the **last run per tier**, so run `make test-t2`
  to populate all of T2.
- **JSON** (`build/regtest/<tier>-latest.json`) — machine-readable, for diffing across bumps. This is
  **latest-wins**: a re-run overwrites it, so it only shows the *last* run per tier.
- **Flake ledger** (`build/regtest/history.jsonl`) — the durable counterpart to the latest-wins JSON:
  **every result is appended, never overwritten**, so a transient a retry masked is still recorded.
  `make test-flakes` lists which tests flip verdict run-to-run; the HTML report shows the same as a
  "Flake ledger" section. See below.

**The curated narrative snapshot** (hand-maintained, with provenance) lives in
[`docs/regression/rimba-regression-results.md`](../../docs/regression/rimba-regression-results.md).

## Unattended runs — the lint gate + the flake ledger

The suite is built to run **fire-and-forget**: kick it off, walk away, and read the durable record
afterwards — you don't watch it live. Two pieces make that trustworthy:

**Lint gate (off-bench, automatic).** Every run-producing command runs **pyflakes over the harness first**
(`tools/regtest/*.py` + the bench `tools/*.py`), so an undefined-name / bad-edit bug fails in ~0.1 s
*before any hardware is touched* — not 40 min into a run (`py_compile` does **not** catch undefined names;
this once cost a 10/12-FAIL bench run). It runs automatically ahead of `t0/t1/t2/tp/dscycle` and at the top
of `make test-all`; run it on its own with **`make test-lint`**. A real finding aborts with a non-zero
exit and no bypass; a missing pyflakes only warns (it never blocks the bench on a missing dev tool).

**Unit tests (off-bench).** The harness's own logic — the flake ledger, the `manifest` / `APPS`
drift check, the trend math, the lint gate — is covered by **37 host unit tests**; run them with
**`make test-unit`** (no hardware, no bench). They gate the harness itself the way T0–T2 gate the
firmware.

**Flake ledger (durable, automatic).** Because the per-tier JSON is latest-wins, a passing retry used to
**silently erase** a failed run — a flake left no trace. Now `Reporter.add` **appends every result** to
`build/regtest/history.jsonl` (append-only, crash-safe, one line per result, grouped by a per-run id). A
test that *flips* verdict across runs (both good and bad outcomes) is surfaced as **flaky** — a
bench/RF/rig transient, not a code regression — while an always-bad test is flagged **consistently
failing** (a real defect). `make test-flakes` for the CLI view; the HTML report carries the same section.

```sh
# fire-and-forget: launch detached, come back later — nothing to monitor
make test-all BOARD_NAME=board2 AP=esp CYCLES=3 >/tmp/run.log 2>&1 &
# … later …
make test-flakes     # what flipped verdict run-to-run (the durable history)
make test-report     # the HTML (already regenerated by the run) — incl. the flake section
```

**Numbers that drift (trend + gitlink diff).** Pass/fail catches a break; it does **not** catch a *number
that drifts while the test still passes* — the tp No-PS current creeping up (the fw-1.17.9 ~2× power-save
regression was exactly this), a dscycle reconnect getting slower, throughput sagging. The ledger now
persists each result's **numeric `meta`** (tp `median_ma`, dscycle `latencies_ms`/`wake_cycles`, …), so:

```sh
make test-trend                       # every recorded metric over the last runs (sparkline + % change)
make test-trend TEST=deepsleep-reconnect
make test-trend DIFF="7d7f76ad 88606f94"   # how each metric MOVED across two halow SDK gitlinks (>20% flagged)
```

`DIFF` is the one to run after a `components/halow` bump: it turns "still passes" into "median_ma
+100% ⟵ >20% MOVE" — the drift the suite exists to catch.

## Exit codes and statuses

Exit is **0 only when nothing FAILed**. The status vocabulary is deliberately richer than
pass/fail, because conflating an honest non-result with a regression is how a suite gets
ignored:

- **PASS / FAIL** — as expected.
- **SKIP** — could not run (board absent, test not implemented). Not a failure.
- **INCONCLUSIVE** — ran, but the measurement can't be trusted as pass/fail (e.g. a noisy
  RF number outside its margin). Distinct from FAIL on purpose: a fading link is not a
  code regression.
- **XFAIL** — failed, and it was *already known* to fail for a documented reason
  (`manifest.KNOWN_BROKEN_BOARDS`). Counted and printed; does **not** gate.
- **XPASS** — a known-broken case unexpectedly passed. **Gates the run** — it means the
  documented reason is stale and the manifest entry must be removed, otherwise that entry
  silently hides every future regression on that board.

## Known-broken today (XFAIL)

**None.** The previously-broken `proto1` overlay (it asked for a `bcf_mf16858.mbin` the pinned
`vendor/morse-firmware` doesn't ship, so every `proto1` build failed at CMake configure) was
**retired** — `boards/proto1` is deleted and the whole bench is `proto1-fgh100m`. The XFAIL / XPASS
machinery stays (`manifest.KNOWN_BROKEN_BOARDS`, currently empty) for the next time a board has a
documented, non-gating breakage.

## Bench facts the harness enforces (so you don't have to remember them)

These are wired into the harness / `common.py`, not left to the operator:

- **Ports are resolved by efuse MAC** (the `BOARD*_MAC` you pass), never a cached `ttyACM*` (they re-enumerate).
- **The non-`WIRED_BOARD` boards have WAKE + BUSY unwired** — they are light-load endpoints only.
  Any load / relay / power-save DUT role **must** be the wired board (normally board2). T2 pins it
  for those roles; using a light-load board there tests missing solder, not code.
- **board2 is PPK2-powered** and only enumerates while `tools/ppk2_hold.py` runs.
- **A mid-capture USB re-enumeration is tolerated, not a FAIL.** board2's PPK2 rail occasionally
  wobbles → the ESP resets and its serial port drops + re-enumerates (often renumbered) mid-read;
  pyserial surfaces this as `device reports readiness to read but returned no data … multiple access on
  port?`. The capture path (`common._capture`) catches it **once**, re-resolves the port by efuse MAC,
  and recaptures a fresh window — logging `[capture] … dropped mid-read … re-resolving`. A *persistent*
  fault still FAILs (the retry re-raises), so a genuinely dead board is not masked. Measured rate ~2–3 %
  per capture on board2, ~0 on the bus-powered boards.
- **Sleep apps wedge the USB** — T1 skips them by default (they need a PPK2 power-cycle to
  recover; `--include-sleep-apps` only on board2).
- **Radio-silence after every hardware test** is automatic: T1/T2 flash `test-idle`
  back to every board they touched. The Linux half (`ip link set wlan1 down`) is still
  the operator's, and each test's README says so.

## Coverage — what's automated vs defined

```sh
make test-t2 DRY_RUN=1    # the full T2 catalogue, with provenance
```

**All 15 T2 tests are automated** (13 all-ESP + 2 ESP↔Linux interop), every ESP role app `test-*`,
all passing on the bench:
- **`swccmp`** — RFC-3610 CCM KAT (no radio, deterministic).
- **`ampdu-cap`** — FW advertises the mesh A-MPDU capability (single board, mesh vif).
- **`ap-sta-ping`** — AP↔STA association + ping (`test-apsta-ap` + `test-apsta-sta`). 15/15.
- **`ibss`** — IBSS join + exactly-1-peer/0-phantom (symmetric `test-ibss`).
- **`twt`** — TWT responder agreement → INSTALLED (`test-apsta-ap` + `test-twt-sta`).
- **`mesh-peering`** — SAE+AMPE peering to ESTAB, ESP↔ESP (symmetric `test-mesh-peering`).
- **`mesh-relay`** — mesh multi-hop SW-CCMP forwarding, forced line, **relay = board2**.
- **`mesh-large-frame`** — a large (FW-fragmented) frame forwarded origin→relay→dest with SW-CCMP
  intact (the defrag-before-decrypt RX path), forced line, **relay = board2**.
- **`mesh-leaf`** — the P6d single-hop/leaf opt-out: the relay keeps its 1-hop plinks but declines to
  forward (`mmwlan_mesh_set_multihop(false)`) — exactly 0 replies, no black-hole, **relay = board2**.
- **`mesh-relay-nocrash`** — the relay forwards a sustained load with **no silent hw-restart**
  (`hw_restart_counter` unchanged), guarding the interrupt-WDT crash, **relay = board2** (reporter).
- **`mesh-ap`** — mesh + AP concurrency (the gate), STA pings a far node, **ttl=63**, **gate = board2**.
- **`mesh-linux`** — **ESP↔Linux mesh interop**: the ESP peers with + pings a real Linux node
  (chronite), brought up + torn down over ssh by `linux_peer.py`. The gold standard.
- **`twt-assoc`** — assoc-embedded TWT reaches INSTALLED (flow 0..3) against a **Linux AP** — the
  universal path (the second ESP↔Linux interop test).
- **`multi-twt`** — two STAs both reach TWT INSTALLED **concurrently** on a plain ESP AP.
- **`mesh-ap-multi-twt`** — two TWT STAs **behind the mesh gate** both reach INSTALLED (mesh+AP
  concurrency + per-STA TWT).

`mesh-relay`/`mesh-ap` need board2 powered (`tools/ppk2_hold.py`); the orchestrator refuses their
`require_wired` roles on board0/board1. `mesh-linux` needs chronite reachable by ssh. Three small
`mmwlan*` accessors were added to morselib so reporters can self-verify structural facts
(`mmwlan_twt_agreement_installed`, `mmwlan_ampdu_capability_advertised`, `mmwlan_ibss_peer_count`).

### The T2 orchestrator (`t2_onair.py`)

Multi-board tests declare structured **roles** in `t2_tests.py`: each role maps to a bench device
and a firmware app, and exactly one is the **reporter** (its `TEST|RESULT` is the verdict). The
orchestrator resolves every device by efuse MAC, flashes support roles first and confirms each came
up (via an `up_marker` in its console), then flashes + captures the reporter, records the verdict,
and returns every board it touched to `test-idle`. It **enforces the wiring rule in code**: a role
marked `require_wired` refuses to run on board0/board1 (unwired WAKE/BUSY) rather than produce a
false bug. A **Linux-peer helper** (`linux_peer.py`, ssh) brings a real Linux node onto the air for
interop tests: it pushes the reference mesh config if missing, starts `wpa_supplicant_s1g` in mesh
mode, and tears the node back down afterwards — hardware-verified via `mesh-linux` (a role with
`device="linux:<host>"`, `linux_setup="mesh-peer"`). The verdict still comes from an ESP reporter
(the orchestrator scrapes ESP `TEST|` lines); a Linux node is a support role only.

**Adding a multi-board test is declarative:** write the reporter firmware (copy
`test-apsta-sta`), declare the test's `roles`, register the app in `manifest.py`. For a Linux
interop test, add a `linux:<host>` support role — no new firmware on the Linux side.

## Layout

```
tools/regtest/
  manifest.py     apps, known-broken, power bands, T2 catalogue metadata (bench identity comes from make, built here from env)
  t2_tests.py     the T2 test definitions (rig + expectations + provenance)
  common.py       ports, make, serial capture, results, radio-silence
  t0_build.py     T0 tier
  t1_smoke.py     T1 tier
  t2_onair.py     T2 tier (scrapes the TEST| console contract)
  run.py          CLI
firmware/
  test-common/include/test_report.h   the TEST| verdict contract
  test-<slug>/                            one app per T2 feature + its README
```

## Adding a test

1. Add the app to `firmware/test-<slug>/` (copy `test-swccmp` as a skeleton;
   report via `test_report.h`).
2. Add a `T2Test` to `t2_tests.py` with expected values **and their `file:line`
   provenance** — assert the *structural* fact, never the RF number (see the module
   docstring for why).
3. Register the app in `manifest.py` `APPS` (T0's drift check **fails** otherwise —
   an unregistered app is an untested app), and remove its `T2_NOT_IMPLEMENTED` entry.
4. Write `firmware/test-<slug>/README.md` for the human + agent runner.
