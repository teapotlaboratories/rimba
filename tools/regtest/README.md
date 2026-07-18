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

A full on-air matrix over 17 apps is not realistic on a 3-board bench, so the suite is
**deliberately partial and honest about it** rather than fake-complete. Every T2 test
that is defined but not yet automated says so, with the concrete blocker, in its own
output (`--dry-run`) — see "Coverage" below.

## Requirements

**Software (once per shell):**
- Run from the repo root. All builds go through `make` (never bare `idf.py` — see "Building" below).
- For any tier that touches a board (T1, T2), load the ESP-IDF python env so `pyserial` is importable:
  `source vendor/esp-idf/export.sh`. T0 (build-only) does not need it.

**Hardware, per tier:**

| Tier | Needs |
|---|---|
| **T0** build | nothing — a laptop. |
| **T1** smoke | one ESP board (default `board2`; `BOARD_NAME=board0` to use another). |
| **T2** on-air | depends on the test — see the table. |

Per-T2-test hardware (the orchestrator SKIPs a test whose devices aren't present, with the reason):

| T2 test | Boards / nodes |
|---|---|
| `swccmp`, `ampdu-cap` | any 1 ESP board |
| `ap-sta-ping`, `ibss`, `twt`, `mesh-peering` | board0 **+** board1 |
| `mesh-relay`, `mesh-ap` | board0 + board1 **+ board2** (board2 is the relay/gate — `require_wired`) |
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

    LINUX_NODES registry  ──(or)──  --linux-mac / --linux-ip
              └────────────┬────────────────┘
             make LINUX_MAC=… LINUX_IP=…  →  -D TEST_LINUX_MAC/IP  →  compiled into test-mesh-linux

| What | Where | Note |
|---|---|---|
| the node registry | `tools/regtest/manifest.py` → `LINUX_NODES` | **single source of truth (Python)**: `host → {mesh_ip, mesh_mac, role}`. `make test-bench` prints it. |
| which node a test uses | `tools/regtest/t2_tests.py` → the test's `Role(device="linux:<host>")` | e.g. `mesh-linux` uses `linux:chronite` |
| the peer MAC + IP the ESP checks | **passed to the build** as `TEST_LINUX_MAC` / `TEST_LINUX_IP` | harness derives them from the test's `linux:<host>` registry row and (re)builds the reporter against them; `app_main.c` falls back to a chronite `#ifndef` default only if nothing is passed |

So a plain `python tools/regtest/run.py t2 --test mesh-linux` already builds the reporter against
whatever `LINUX_NODES` says for its `linux:<host>` role — no manual step. Override the peer for one run
without touching the registry:

```sh
python tools/regtest/run.py t2 --test mesh-linux --linux-mac 68:24:99:44:6b:80 --linux-ip 10.9.9.4
```

or drive the build directly:

```sh
make flash APP=test-mesh-linux BOARD=proto1-fgh100m LINUX_MAC=68:24:99:44:6b:80 LINUX_IP=10.9.9.4 PORT=…
```

**Easiest of all — don't look the MAC up at all.** `flash-interop` reads the peer's MAC (and mesh IP)
off the node live over ssh, so you only pass the host and the board's port:

```sh
python tools/regtest/run.py flash-interop --host chronite --port /dev/ttyACM0
# add --run to also bring the node onto the rimba-smesh mesh, capture the board's PASS/FAIL,
# and take both sides off the air afterwards (the whole interop loop, one command):
python tools/regtest/run.py flash-interop --host chronite --port /dev/ttyACM0 --run
```

| flag | meaning |
|---|---|
| `--host` (req) | ssh host of the Linux node; its `wlan1` MAC is read live |
| `--port` (req) | the ESP board's serial port to flash |
| `--ip` | override the peer mesh IP (default: the node's live `wlan1` IPv4, else its manifest entry) |
| `--linux-mac` | skip the query and use this MAC (e.g. the node is off but you know it) |
| `--run` | bring the mesh up first, capture the verdict, then silence both sides |
| `--monitor` | print the board's `TEST\|` output after flashing (implied by `--run`) |

A run-time cross-check (`linux_peer.mesh_mac_matches`) still reads the node's **live** `wlan1` MAC and
**warns** if it differs from what was compiled in — catching a stale build against a re-flashed node.

**To point `mesh-linux` at a different existing node** (e.g. `chronogen`): pass
`--linux-mac`/`--linux-ip` for a one-off, or change the role to `device="linux:chronogen"` in
`t2_tests.py` to make it the default (the MAC/IP then come from that node's `LINUX_NODES` row
automatically). No firmware edit either way.

**To add a brand-new Linux node:** give it an `~/.ssh/config` entry and add a `LinuxNode(...)` row to
`LINUX_NODES` in `manifest.py`. That's it — the build-arg plumbing picks the MAC/IP up from there.

### The same pattern for ESP-only topology (the `mesh-relay` line)

`mesh-relay` is symmetric — all three boards run `test-mesh-relay` and each self-selects its role
(origin / dest / relay) by MAC — so **every** flash needs the whole line's MACs. Those are also
**build-time arguments derived from the manifest**, not firmware constants: each relay `Role` declares
a `build_mac_var` (`MESH_ORIGIN_MAC` / `MESH_DEST_MAC` / `MESH_RELAY_MAC`), and the orchestrator feeds
each one its **assigned board's** `BENCH[...].mesh_mac`, passing all three to every flash. Retarget the
line by editing the boards' `mesh_mac` in `manifest.py` (or `make ... MESH_ORIGIN_MAC=… MESH_DEST_MAC=…
MESH_RELAY_MAC=…`); the dest's mesh IP is *derived* from `MESH_DEST_MAC`, so it's never a second value
to sync. `python tools/regtest/run.py t2 --test mesh-relay --dry-run` prints the resolved MACs.

> A `-D` cache var is sticky (both here and for the Linux peer): a *manual* custom build pins the value
> until `make fullclean APP=<app>`. The harness always passes explicit values, so real T2 runs are
> unaffected — this only bites a hand-run bare rebuild in a dir you previously custom-built.

## Quick start

```sh
source vendor/esp-idf/export.sh     # once per shell (for T1/T2)

make test-bench      # what hardware is present right now (run this first)
make test-t0         # build matrix — no hardware, ~25–35 min cold / minutes warm
make test-t1         # smoke on board2 (needs a board; BOARD_NAME=board0 to pick another)
make test-t2         # all on-air feature tests (needs the rigs above)
make test-tp         # power-save tier: PPK2 current ladder (board2 + PPK2 + C6 + an AP)
make test            # t0 + t1
make test-silence    # return every ESP to the radio-free idle app
make test-report     # (re)generate the HTML report (build/regtest/report.html) from the baselines
```

**Power-save + newer tests (added 2026-07):**

```sh
python tools/regtest/run.py tp --ap esp|linux    # PPK2 current ladder (4 tiers, 2 scored)
python tools/regtest/run.py tp --light-sleep     # a HOST_LIGHT_SLEEP build variant (separate bands)
python tools/regtest/run.py dscycle              # deep-sleep duty-cycle reconnect gate (board2 + C6)
python tools/regtest/run.py t2 --test twt-assoc  # assoc-embedded TWT -> INSTALLED (universal path, Linux AP)
python tools/regtest/run.py t2 --test multi-twt  # 2 STAs both reach TWT INSTALLED concurrently (plain AP)
python tools/regtest/run.py t2 --test mesh-ap-multi-twt  # 2 TWT STAs behind the MESH GATE (concurrency)
```

**T2 now has 12 feature tests** (was 9): the original 8 all-ESP + `mesh-linux`, plus `twt-assoc`,
`multi-twt`, and `mesh-ap-multi-twt`. Combination coverage of TWT: single-STA (`twt`, both APs via
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
python tools/regtest/run.py t2 --test mesh-linux     # ESP <-> Linux interop
python tools/regtest/run.py t2 --test mesh-ap        # the mesh-gate
python tools/regtest/run.py t2 --dry-run             # list every T2 test + its rig, no hardware
```

Exit code is `0` only when nothing FAILed (SKIP / XFAIL / INCONCLUSIVE don't fail the run).

Or directly (the tiers that touch a board need the IDF python env for pyserial —
`source vendor/esp-idf/export.sh` first):

```sh
python tools/regtest/run.py t0   [--app NAME ...] [--board NAME ...]
python tools/regtest/run.py t1   [--board-name board2] [--include-sleep-apps]
python tools/regtest/run.py t2   [--test SLUG ...] [--dry-run]
python tools/regtest/run.py bench
python tools/regtest/run.py silence
```

Every run writes a JSON baseline to `build/regtest/<tier>-latest.json`, stamped with the
repo SHA and the `components/halow` gitlink — so a baseline is always attributable to a
specific tree state. Diff two baselines to see what a bump changed.

### Reports — three views of the same results

- **Terminal** — a live `[PASS]/[FAIL]/[SKIP]` line per test as it runs.
- **HTML** (`build/regtest/report.html`) — a **self-contained, theme-aware page** for humans, auto-
  generated at the end of every tier run. Summary cards per tier + a per-test table with status
  badges, timing, detail, and collapsible `evidence`. Open it in a browser or share the single file.
  Regenerate anytime from the existing baselines (no re-run) with `make test-report` /
  `python tools/regtest/run.py report`. It shows the **last run per tier**, so run `make test-t2`
  to populate all of T2.
- **JSON** (`build/regtest/<tier>-latest.json`) — machine-readable, for diffing across bumps.

**The curated narrative snapshot** (hand-maintained, with provenance) lives in
[`docs/regression/rimba-regression-results.md`](../../docs/regression/rimba-regression-results.md).

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

`BOARD=proto1` cannot build **any** app: `boards/proto1/sdkconfig.defaults` asks for
`bcf_mf16858.mbin`, which the pinned `vendor/morse-firmware` (1.17.8) does not ship —
every `proto1` build fails at CMake configure. This predates the suite; it was found by
the suite's first run. The whole bench is `proto1-fgh100m`, so nobody had noticed. It is
recorded in `manifest.KNOWN_BROKEN_BOARDS` with the full reason, so `proto1` builds report
XFAIL and the gate stays meaningful. **Owner decision:** restore an `mf16858` BCF, or
retire `boards/proto1`. See `docs/worklog/2026-07-16-regression-suite-and-fork-migration-plan.md`.

## Bench facts the harness enforces (so you don't have to remember them)

These are wired into `manifest.py` / `common.py`, not left to the operator:

- **Ports are resolved by efuse MAC**, never a cached `ttyACM*` (they re-enumerate).
- **board0 / board1 have WAKE + BUSY unwired** — they are light-load endpoints only.
  Any load / relay / power-save DUT role **must** be board2. The manifest marks this and
  T2 pins board2 for those roles; using board0 there tests missing solder, not code.
- **board2 is PPK2-powered** and only enumerates while `tools/ppk2_hold.py` runs.
- **Sleep apps wedge the USB** — T1 skips them by default (they need a PPK2 power-cycle to
  recover; `--include-sleep-apps` only on board2).
- **Radio-silence after every hardware test** is automatic: T1/T2 flash `test-idle`
  back to every board they touched. The Linux half (`ip link set wlan1 down`) is still
  the operator's, and each test's README says so.

## Coverage — what's automated vs defined

```sh
python tools/regtest/run.py t2 --dry-run    # the full T2 catalogue, with provenance
```

**All 9 T2 tests are automated** (8 all-ESP + 1 ESP↔Linux interop), every ESP role app `test-*`,
all passing on the bench:
- **`swccmp`** — RFC-3610 CCM KAT (no radio, deterministic).
- **`ampdu-cap`** — FW advertises the mesh A-MPDU capability (single board, mesh vif).
- **`ap-sta-ping`** — AP↔STA association + ping (`test-apsta-ap` + `test-apsta-sta`). 15/15.
- **`ibss`** — IBSS join + exactly-1-peer/0-phantom (symmetric `test-ibss`).
- **`twt`** — TWT responder agreement → INSTALLED (`test-apsta-ap` + `test-twt-sta`).
- **`mesh-peering`** — SAE+AMPE peering to ESTAB, ESP↔ESP (symmetric `test-mesh-peering`).
- **`mesh-relay`** — mesh multi-hop SW-CCMP forwarding, forced line, **relay = board2**.
- **`mesh-ap`** — mesh + AP concurrency (the gate), STA pings a far node, **ttl=63**, **gate = board2**.
- **`mesh-linux`** — **ESP↔Linux mesh interop**: the ESP peers with + pings a real Linux node
  (chronite), brought up + torn down over ssh by `linux_peer.py`. The gold standard.

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
  manifest.py     apps, boards, bench nodes, known-broken, T2 catalogue metadata
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
