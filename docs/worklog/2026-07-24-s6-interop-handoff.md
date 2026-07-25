# Handoff — resume S6 (ESP mesh-gate ↔ live-Linux 802.11s interop)

**Date:** 2026-07-24 ~05:00 PDT (written just before a user-requested context clear).
**Task the fresh context must do:** continue **S6** — the last mesh-gate stage: live-Linux 802.11s
interop. Start with **Part 0** (a standalone gate-emit change), then S6 proper.

**Authoritative execution playbook (READ IT FIRST, self-contained):**
[`docs/mesh-ap/rimba-mesh-ap-s6-interop-plan.md`](../mesh-ap/rimba-mesh-ap-s6-interop-plan.md). This
handoff captures current state + the immediate next step; that doc is the full plan (Part 0, P0–P3).
Design + code-map: [`rimba-mesh-ap-mesh-gate-discovery-design.md`](../mesh-ap/rimba-mesh-ap-mesh-gate-discovery-design.md),
[`rimba-mesh-80211s-code-map.md`](../mesh-ap/rimba-mesh-80211s-code-map.md).

---

## Repo state at handoff (clean, everything landed)

- On `main`, HEAD `0e01d8c` **locally** — but **remote `main` is `e16082fa`** (PR #45 merged, below).
  **This sandbox's git remote is SSH (`git@github.com:teapotlaboratories/rimba`) with NO key → `git
  fetch`/`push` FAIL.** `gh` works (HTTPS token, scopes `repo`). So: read PRs / merge via `gh`; local
  `main` stays behind until the user runs `git pull` from an SSH-enabled shell. Don't fight the SSH failure.
- `components/halow` gitlink `8a70405c` — mesh-gate **S1–S5c + round-trip + broadcast-both-ways +
  proxy-ARP + retire-L3 ALL LANDED** (rimba #42/#43/#44; mm-esp32-halow #27). ESP↔ESP mesh-gate is fully
  proven on-air. See [[mesh-gate-8021s-port-planned]].
- Uncommitted: only two untracked worklogs (`2026-07-23-regression-full-sweep-screen-handoff.md` + this
  file). No code in flight.
- **This session's work (done):** re-ran the full regression sweep `make test-all` — **ALL GREEN**
  (T0 40✓/1skip · T1 12✓/2skip · T2 22✓ incl. full chronite interop · TP 4✓ · dscycle PASS); the one
  `mesh-linux` INCONCLUSIVE was the [[board2-serial-capture-flake]], re-ran standalone → PASS. Then
  reviewed + **MERGED PR #45** (regtest T2 unit tests, `make test-unit` 46→53). Bench left SAFE: all 3
  boards `test-idle`, **board2 powered OFF** (`ppk2_hold` released), chronite **`wlan1` DOWN**.

## THE IMMEDIATE NEXT STEP — Part 0: make the ESP gate a discoverable 802.11s gate (own PR, do FIRST)

**Why:** the production gate app `firmware/rimba-halow-mesh-ap` never advertises as a gate on-air (the
ESP↔ESP bridge learns MPP from proxied data frames, not RANN). Case B (a Linux node discovering the ESP
gate) needs it to emit RANN+`IS_GATE`. This is additive/safe — RANN emission doesn't change the existing
MPP-from-proxied-frames datapath.

**Code (verified anchors, 2026-07-24):**
- `firmware/rimba-halow-mesh-ap/main/app_main.c:657` calls `mmwlan_mesh_start(&mesh_args)` and does **NOT**
  call the setter. Add, right after `mmwlan_mesh_start(...)` succeeds:
  ```c
  mmwlan_mesh_set_root_announcements(/*root_rann=*/true, /*is_gate=*/true, /*interval_ms=*/5000);
  ```
- Setter sig: `components/halow/.../umac/mesh/umac_mesh.h:211`
  `void mmwlan_mesh_set_root_announcements(bool root_rann, bool is_gate, uint32_t interval_ms);`
- **Mirror the reference impl** `firmware/test-mesh-gate/main/app_main.c:116`
  (`mmwlan_mesh_set_root_announcements(true, RANN_IS_GATE, RANN_INTERVAL_MS)`; there `RANN_INTERVAL_MS`
  default = 5000).
- **Gate:** the setter is gated on `g_mesh_multihop` being true — **confirm the gate app enables multihop**
  (check `mesh_args` / a `mmwlan_mesh_set_multihop(true)` near line 657) or the call is a silent no-op.
- **`5000` is deliberate:** the RANN interval is a **RAW numeric on the wire (no ms→TU convert)**, so the
  ESP gate and the Linux bench gate MUST use the SAME value (S1 footgun). Linux bench default + ESP
  fixtures = 5000.

**Bench verify** (board0=gate `rimba-halow-mesh-ap`, board1=mesh node, board2=STA):
1. Gate emits RANN — capture on **chronium** `morse0` monitor with `tools/mesh_rann_cap.py`, confirm
   `rann_flags=01` (IS_GATE).
2. The existing **zero-config round-trip** (STA DHCP → ping a mesh node by IP) still works (no regression
   to retired-L3).
3. An ESP mesh node reports `mmwlan_mesh_gate_count() > 0` (learned the ESP gate via RANN).
Radio-silent after.

**Ship:** branch `feat/gate-emit-rann` → `/code-review` → rebase-merge (via `gh`, API-side — see the SSH
note; agent-initiated `gh pr merge` is BLOCKED by the auto-mode classifier and needs the user's explicit
"merge it"). Worklog + render + a MEMORY.md index card.

## After Part 0 — S6 proper (full detail in the S6 plan doc)

- **P0** code-map re-pin (no bench): re-grep every §3 anchor on the CURRENT trees, stamp "verified <date>"
  (morselib `7d7f76ad`/2.12.3, files now under `umac/mesh/` + `umac/datapath/`; Linux `chronium:~/halow/
  rpi-linux` `372414fd4`/6.12.21, **re-`scp` each session**). Per [[porting-ships-verified-codemap]].
- **P1 Case A — Linux gate ↔ ESP node (THE WILDCARD):** build + validate the *Linux-side* L2 bridge
  (`mesh-br` bridging `wlan1`↔`eth0`, flat `10.9.9.0/24`) — never done on this bench; unknown whether the
  morse driver permits bridging the mesh vif. Expect to iterate.
- **P2 Case B — ESP gate ↔ Linux node (needs Part 0):** Linux discovers the ESP gate's RANN; the
  S3-deferred AE/MPP leg (`make flash APP=test-mesh-ae LINUX_MAC=<linux wlan1 MAC>`, verify `iw dev wlan1
  mpp dump`); full bridge datapath both ways.
- **P3** byte-diff sign-off (RANN both directions via `tools/mesh_rann_cap.py` with the S1 normalized
  mask; gate bit via `tools/mesh_beacon_cap.py`) + ship the re-pinned code-map.

## Bench + footguns (fresh, for S6)

- **Whole bench MUST be fw 1.17.8** — check at session start (`dmesg` fw SIZE `480664`=1.17.8; `morse_cli
  version` mis-reports). Cross-device byte-diffs are invalid under skew. [[morse-fw-same-version]].
- **Bench env (export or pass to make):** `BENCH_BOARD=proto1-fgh100m BOARD0_MAC=E0:72:A1:F8:EF:A4
  BOARD1_MAC=E0:72:A1:F8:F9:40 BOARD2_MAC=E0:72:A1:F8:F0:08 WIRED_BOARD=board2`. **board2 is PPK2-power-gated
  — start a hold first** (`nohup /home/quartz/.espressif/python_env/idf5.4_py3.13_env/bin/python
  tools/ppk2_hold.py &`) so it enumerates as a `/dev/ttyACM*`; identify ESPs by efuse MAC, not ttyACM.
- **chronite** was reachable + healthy tonight (morse loaded, wlan1 present, fw 1.17.8, `10.9.9.2` /
  `3c:22:7f:37:51:38`). It is the Linux GATE candidate but was flaky at S3 (mesh bring-up); fallback
  chronogen. **chronium = the sniffer** (`morse0` monitor, freq 5560; can't be sniffer AND mesh member).
- **Linux mesh only via `wpa_supplicant_s1g`** on the SECURED `rimba-smesh` (pw `rimbamesh2026`, ch27),
  never bare `iw mesh join`; gate mode layered via `iw set mesh_param` after `MESH-GROUP-STARTED`.
  [[morse-linux-mesh-via-supplicant]]. **Don't reboot Linux nodes** (config in tmpfs) —
  [[dont-reboot-linux-mesh-nodes]]. Kill the daemon with `pkill -9 -x wpa_supplicant_s1g`, NEVER
  `pkill -f wpa_supplicant_s1g` (self-kills the ssh session).
- **Build via `make ... BOARD=proto1-fgh100m`**, never bare `idf.py` ([[build-via-make-board]]). `make
  flash` doesn't clear stale `TEST_*` CMake cache vars — `rm build/<app>/<board>/CMakeCache.txt` before
  reflashing a changed test config. `LINUX_MAC=`/`STA_IP=` need CMakeLists `target_compile_definitions`.
- **Long bench runs:** tracked `run_in_background` tasks get REAPED here — use a detached `screen`, poll
  via FOREGROUND `vmstat`-timed wait-blocks (foreground `sleep` is BLOCKED). `pgrep -af`/`pkill -f` on a
  pattern self-matches the running shell — kill by exact PID / `kill -0`.
- **Radio-silent after every test** ([[radio-silent-after-every-test]]); power board2 OFF when done
  (`ppk2_hold.py off`); `ip link set wlan1 down` on any Linux node used. On-air verify on chronium's
  monitor, byte-diff vs a LIVE Linux transmission ([[verify-onair-chronium-monitor]]).
- Commit rules: never auto-commit ([[commit-attribution]] — no AI trailers); no `git commit`/`push`
  Mon–Fri 09:00–16:59 local ([[no-push-work-hours]]) — verify `date` first.

## Next step for the fresh context
1. Read the S6 plan doc (`docs/mesh-ap/rimba-mesh-ap-s6-interop-plan.md`) — it's the authoritative playbook.
2. Verify the bench (fw 1.17.8 everywhere; start `ppk2_hold`; `make test-conn` / `run.py bench`; check
   `ssh chronite`).
3. Do **Part 0** (gate-emit RANN) end-to-end: edit → build → 3-board bench verify → ship `feat/gate-emit-rann`.
4. Then S6 P0→P3.
