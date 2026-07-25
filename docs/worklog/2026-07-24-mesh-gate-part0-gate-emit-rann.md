# 2026-07-24 — Mesh-gate Part 0: the production gate emits a discoverable RANN (S6 pre-step)

**Status:** ✅ **DONE + ON-AIR VERIFIED (gold-standard live-device A/B).** The production gate app
`firmware/rimba-halow-mesh-ap` now advertises itself as a discoverable 802.11s gate — it emits a
PROACTIVE_RANN with `RANN_FLAG_IS_GATE` every 5000 ms, byte-identical to the S1-verified RANN (and thus,
by transitivity from S1, to a live Linux gate). This is the standalone gate-emit change that S6 Case B
(a Linux node discovering the ESP gate) depends on; it lands as its own small PR before S6 proper. **All
three planned Part 0 checks PASS.** Uncommitted (held for the work-hours commit rule — Fri 09:12 PDT).

## Why (the load-bearing gap it closes)

The all-ESP mesh-gate L2 bridge learns `mpp` (mesh proxy paths) from **proxied data frames**, not from
RANN — so the production gate never needed to announce itself and never did (only the `test-mesh-gate*`
fixtures called `mmwlan_mesh_set_root_announcements`). **S6 Case B** (Linux node ↔ ESP gate) requires the
ESP gate to emit RANN+`IS_GATE` so a standards Linux node can *discover* it. This change adds exactly that,
and nothing else. See `docs/mesh-ap/rimba-mesh-ap-s6-interop-plan.md` (Part 0) + the S6 handoff.

## What changed (app-level only, +24 lines, no morselib change)

Two mirrored files (the T0 clone-mirror guard `0e01d8c` forces them byte-identical below the banner):

- **`firmware/rimba-halow-mesh-ap/main/app_main.c`** (the production gate) — after `mmwlan_mesh_start()`
  succeeds and before `mmwlan_ap_enable()`:
  ```c
  #define MESH_RANN_INTERVAL_MS 5000   /* RAW numeric on the wire; MUST equal the Linux bench gate's value */
  ...
  mmwlan_mesh_set_root_announcements(/*root_rann=*/true, /*is_gate=*/true, MESH_RANN_INTERVAL_MS);
  ESP_LOGI(TAG, "==> gate RANN announcements ON (IS_GATE, interval=%dms) on chan %d.", ...);
  ```
- **`firmware/test-mesh-gate-ap/main/app_main.c`** (the harness gate fixture, a body-clone) — the identical
  two hunks, to keep the T0 `_check_clone_mirrors` tripwire green (verified: bodies byte-identical again;
  `make test-unit` 46/46). This also correctly makes the harness's Case-C gate emit RANN too.

**Why additive/safe:** `g_mesh_multihop` defaults `true` (morselib `umac_mesh.c:566`) and neither app
disables it, so the setter is **not** a silent no-op — the concern the handoff flagged is resolved. The
existing MPP-from-proxied-frames datapath is untouched; RANN emission only *adds* standards gate discovery.
Verified no regression to the retired-L3 zero-config round-trip (check b below).

## ✅ VERIFICATION — 3-board bench, all three checks PASS

Rig: **board0** (`E0:72:A1:F8:EF:A4`, mesh `e2:72:a1:f8:ef:a4`) = gate `rimba-halow-mesh-ap`;
**board1** (`…F9:40`, mesh `…f9:40`) = mesh node; **board2** (`…F0:08`) = STA. **chronium** = `morse0`
monitor (`type monitor`, `freq 5560` = S1G ch27). Whole bench fw **1.17.8** (chronium dmesg fw size 480664).

### (a) Gate emits RANN with IS_GATE — on-air, PASS
`sudo python3 tools/mesh_rann_cap.py` on chronium captured board0's RANN (10 frames / 45 s):
```
sa=e2:72:a1:f8:ef:a4  flags=0x01  hop=0  ttl=31  rann_addr=e2:72:a1:f8:ef:a4  interval=5000  metric=0
action_hex=0d017e1501001fe272a1f8efa4  <rann_seq>  8813000000000000  00000000
full_hex   =d0000000 ffffffffffff e272a1f8efa4 e272a1f8efa4 9000 0d017e1501001f e272a1f8efa4 0a000000 8813000000000000 <FCS>
```
Byte-diff vs the **S1-verified** ESP RANN (`2026-07-21-mesh-gate-s1-rann.md`): every invariant field
IDENTICAL (FC `d000`, DA `ffffffffffff`, cat/act `0d01`, eid/len `7e15`, **flags `01`**, hop `00`,
ttl `1f`, interval `88130000`=5000, metric `00000000`); only SeqCtrl + rann_seq + FCS (per-frame counters)
differ. board0's serial confirms the new app-side marker:
`==> gate RANN announcements ON (IS_GATE, interval=5000ms) on chan 27.`, with the AP coming up concurrently
(`AP vif up (secondary) — CONCURRENT with mesh`, `AP netif up: 10.9.9.1/24 + DHCP server`) — no crash.

### (b) Zero-config round-trip still works (no retire-L3 regression) — PASS
board1 = `rimba-halow-mesh` (responder, static `10.9.9.100`); board2 = `rimba-halow-sta` (default DHCP+ping).
board2 serial:
```
associated to "rimba-ping"  ->  DHCP lease 10.9.9.2 — zero-config on the flat mesh subnet
pinging 10.9.9.100 ...       ->  reply from 10.9.9.100 seq=1..14  (23–68 ms, 0 drops, from the FIRST ping)
```
Identical to the retire-L3 baseline. Adding RANN emission did not perturb the L2-bridge datapath.

### (c) An ESP mesh node learns the gate via RANN — PASS
board1 reflashed = `test-mesh-gate-rx MESH_ID=rimba-mesh` (peers with the gate; mesh SAE password
`rimbamesh2026` is compiled into morselib and identical across `rimba-mesh`/`rimba-smesh`, so the Mesh-ID
override is sufficient to peer). board1 serial at uptime 5 s and held for 30 s:
```
joining mesh "rimba-mesh" (SAE) ... estab_peers=1  peer[0]=e2:72:a1:f8:ef:a4  known_gates=1
==> GATE DISCOVERED via RANN (known_gates=1) — S2 RX path works ...
```
So the production gate's RANN is well-formed enough for a real ESP node to peer, receive, and record it as
a gate (`mmwlan_mesh_gate_count() == 1`). (Bonus: board1 concurrently logged AE-rx + `MPP learned … via
e2:72:a1:f8:ef:a4` — the gate's full proxy machinery runs alongside RANN emission.)

## Verification tier
**Live-device gold standard** for (a) (on-air capture + byte-diff vs the S1 reference) and (c) (a real ESP
node learns the gate); on-air functional PASS for (b). Per `[[verify-onair-chronium-monitor]]` /
`milestones.md:991-1011`.

## Cleanup (radio-silent, done)
`rimba-hello` flashed to board0/1/2; **board2 powered OFF** (`ppk2_hold.py off` — ttyACM2 gone, PPK2 DUT
off); chronium `morse0` down + `wlan1` restored to `type managed`. Per `[[radio-silent-after-every-test]]`.

## Review
`/code-review` (xhigh) over the working diff: **no correctness findings** (additive, minimal,
bench-verified). One low-severity maintainability note — the `5000` interval is now a third independent copy
of the bench-wide RANN interval (also `test-mesh-gate-ap` mirror + `test-mesh-gate`'s `RANN_INTERVAL_MS`)
with no single source of truth; a future bench-interval change must update every copy **and** the Linux gate
together or the S6 P3 byte-diff breaks. Mitigated by the T0 clone-mirror guard (forces the two `-ap` apps
equal) + the in-code comment; consistent with the repo's per-app-constant pattern (`MESH_ID`,
`MESH_S1G_CHAN` are likewise duplicated). Left as-is.

## Footguns hit / confirmed
- **T0 clone-mirror guard (`0e01d8c`)** — editing the production `rimba-halow-mesh-ap/main/app_main.c`
  silently drifts its body-clone `test-mesh-gate-ap`; the T0 `_check_clone_mirrors` tripwire FAILs unless
  the same body edit is mirrored. Mirror both, re-run `make test-unit` (46/46), diff the bodies (identical).
- **`make flash` keeps stale `TEST_*` CMake cache** — `rm build/<app>/<board>/CMakeCache.txt` before
  reflashing a changed test config (did so for `rimba-halow-mesh`, `rimba-halow-sta`, `test-mesh-gate-rx`);
  confirmed `TEST_MESH_ID=\"rimba-mesh\"` reached `build.ninja` (the silent-`-D`-drop footgun).
- **Detached bench daemons over ssh** — `sudo nohup … &` over ssh holds the channel; the local `timeout`
  killed the ssh but the remote capture kept running (verified via `pgrep`). `ppk2_hold` started with
  `setsid nohup … & disown`.
- board2 is PPK2-power-gated → enumerated as ttyACM2 only after `ppk2_hold`; boards resolved by efuse MAC
  via `tools/regtest/common.resolve_port` (not `/dev/ttyACM*`).

## Files
- `firmware/rimba-halow-mesh-ap/main/app_main.c` (+12) — the production gate emits RANN+IS_GATE.
- `firmware/test-mesh-gate-ap/main/app_main.c` (+12) — the mirrored clone (T0 guard).
- Memory: `mesh-gate-8021s-port-planned` (Part 0 done — production gate is a discoverable RANN gate).

## Ship (NOT done — no auto-commit; held for work-hours rule)
Branch `feat/gate-emit-rann`, superproject-only (no submodule change). No `git commit`/`push` Mon–Fri
09:00–16:59 local ([[no-push-work-hours]]) — it is Fri 09:12 PDT, so **hold until after 17:00** (or the user
commits). Never auto-commit / no AI trailers ([[commit-attribution]]). PR merge needs the user's explicit
"merge it". Then: HTML render of this worklog + a MEMORY.md index card.

## Next — S6 proper
- **P0** (no bench): re-pin the §3 code-map anchors on the current trees (morselib `7d7f76ad`/2.12.3 under
  `umac/mesh/` + `umac/datapath/`; Linux `chronium:~/halow/rpi-linux` `372414fd4`/6.12.21 — re-`scp`).
- **P1 Case A (the wildcard):** build + validate the Linux-side L2 bridge (`mesh-br` = `wlan1`↔`eth0`, flat
  `10.9.9.0/24`) on chronite; ESP node reaches an off-mesh DS host through the Linux gate.
- **P2 Case B (needs this change):** Linux node discovers the ESP gate's RANN + the S3-deferred AE/MPP leg.
- **P3:** RANN both-directions byte-diff + gate-bit + ship the re-pinned code-map.
