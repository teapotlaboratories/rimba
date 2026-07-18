# test-mesh-relay — Mesh multi-hop relay (SW-CCMP forwarding)

**Status: automated + hardware-verified** (2026-07-16, on the 3-board bench). All role apps are
`test-*`.

## Rig

| role | device | app |
|---|---|---|
| relay (support) | board2 — MANDATORY board2 (fully wired) | `test-mesh-relay` |
| dest (support) | board1 | `test-mesh-relay` |
| origin **[REPORTER]** | board0 | `test-mesh-relay` |

## What it proves / does not prove

- **Proves:** A frame originated on one node is forwarded by an ESP relay to a third node with host SW-CCMP crypto intact -- the multi-hop mesh claim, end to end.
- **Does NOT prove:** HW-crypto forwarding, which is a FIRMWARE limitation, not a bug to regress on: the MM6108 HW-crypto path emits a forwarded frame whose CCMP MIC does not verify under the relay's own group key (root-caused 2026-07-15 by offline MIC-verify). SW-CCMP (g_mesh_sw_crypto=true) is the shipping default and is what this test exercises.

## Structural assertion

sole mesh peer == the relay (topology proof: the forced allowlist means the origin can ONLY reach the dest via the relay, so any reply proves forwarding -- no TTL, since 802.11s relay is L2/one-subnet) + delivery >= floor + `datapath_rx_ccmp_failures` not dominant.

## How to run

```sh
source vendor/esp-idf/export.sh
python tools/regtest/run.py t2 --test mesh-relay
```

The orchestrator flashes each role (support roles first, verified up via a console up-marker), then
captures the reporter's `TEST|RESULT`, and returns every board to `rimba-hello` (radio-silent).

Needs board2 powered (`tools/ppk2_hold.py`) as the relay; the orchestrator refuses the relay role on board0/board1.

## Line topology (the three MACs) — build-time arguments, not hardcoded

The app is symmetric: every board runs the same firmware and self-selects its role (origin / dest /
relay) by comparing its own mesh MAC to the three line MACs. Those MACs are **build-time arguments**,
derived from the manifest `BENCH` registry — no MAC is hardcoded in the source. The harness passes
all three to **every** flash so the boards agree on the line:

```sh
# the harness does this for you (values from BENCH[board0/1/2].mesh_mac):
make flash APP=test-mesh-relay BOARD=proto1-fgh100m \
  MESH_ORIGIN_MAC=e2:72:a1:f8:ef:a4 MESH_DEST_MAC=e2:72:a1:f8:f9:40 MESH_RELAY_MAC=e2:72:a1:f8:f0:08 PORT=…
```

Absent, the app falls back to its `#ifndef` defaults (the current board0/1/2). The dest's mesh IP is
**derived** from `MESH_DEST_MAC` (`10.9.9.<100+(mac[5]&0x3f)>`, the bench convention), so it is never
a second value to keep in sync. To retarget the line, edit the boards' `mesh_mac` in
`tools/regtest/manifest.py` (or pass the `MESH_*_MAC` vars directly) — never the firmware.

> Note: a `-D` CMake cache var is sticky. If you *manually* build with custom `MESH_*_MAC`, a later
> bare `make build` in the same `build/` dir keeps the last value (standard CMake behaviour) — run
> `make fullclean APP=test-mesh-relay` to revert to the defaults. The harness always passes explicit
> values, so a real T2 run is unaffected.

## Firmware

`firmware/test-mesh-relay/` (symmetric 3-node; role by MAC; per-role peer allowlist forces the line; origin reports).

Full catalogue + provenance: `tools/regtest/t2_tests.py` (`MESH_RELAY`), `python tools/regtest/run.py
t2 --dry-run --test mesh-relay`. Results: `docs/regression/rimba-regression-results.md`.
