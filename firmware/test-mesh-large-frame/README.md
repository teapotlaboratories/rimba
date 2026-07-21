# test-mesh-large-frame — Large-frame mesh forwarding (FW-fragmented SW-CCMP)

**Status: new (2026-07-20) — developed on the pre-forward-port morselib 2.10.4 baseline; not yet
hardware-verified.** All role apps are `test-*`.

## Rig — the SAME 3-node forced line as `test-mesh-relay`

| role | device | app |
|---|---|---|
| relay (support) | board2 — MANDATORY board2 (fully wired) | `test-mesh-large-frame` |
| dest (support) | board1 | `test-mesh-large-frame` |
| origin **[REPORTER]** | board0 | `test-mesh-large-frame` |

## What it proves / does not prove

- **Proves:** a LARGE mesh frame (ICMP payload `PING_SIZE` = 1400 B) survives multi-hop forwarding with host SW-CCMP intact. The payload exceeds the 1 MHz / MCS0 single-PPDU limit, so the MM6108 FW fragments the *already host-encrypted* frame over the air, and the RX must reassemble the raw fragments **before** decrypting: the host computed one CCMP MIC over the whole frame, and the FW leaves the CCMP header only on fragment 0 and the MIC only on the last, so no fragment is independently decryptable (encrypt→fragment). This exercises the **defrag-before-decrypt** RX path — whose inverse (host-side fragment→encrypt) was bench-broken and reverted (`docs/worklog/2026-07-14-mesh-defrag-before-decrypt-PASS.md`) — that the small-frame `test-mesh-relay` never touches.
- **Does NOT prove:** HW-crypto forwarding (a FIRMWARE limitation, not a bug to regress on), nor throughput.

## Structural assertion

The same three proofs as `test-mesh-relay`, but over a fragmented frame: sole mesh peer == the relay (topology proof — the forced allowlist means the origin can ONLY reach the dest via the relay, so any reply proves forwarding; no TTL, since 802.11s relay is L2/one-subnet) + delivery >= floor (a defrag/decrypt-order regression MIC-fails the reassembled frame → the frame is dropped → ~0 replies) + `datapath_rx_ccmp_failures` not dominant.

## How to run

```sh
make test-t2 TEST=mesh-large-frame
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
make flash APP=test-mesh-large-frame BOARD=proto1-fgh100m \
  MESH_ORIGIN_MAC=e2:72:a1:f8:ef:a4 MESH_DEST_MAC=e2:72:a1:f8:f9:40 MESH_RELAY_MAC=e2:72:a1:f8:f0:08 PORT=…
```

Absent, the app falls back to its `#ifndef` defaults (the current board0/1/2). The dest's mesh IP is
**derived** from `MESH_DEST_MAC` (`10.9.9.<100+(mac[5]&0x3f)>`, the bench convention), so it is never
a second value to keep in sync. To retarget the line, edit the boards' `mesh_mac` in
`tools/regtest/manifest.py` (or pass the `MESH_*_MAC` vars directly) — never the firmware.

> Note: a `-D` CMake cache var is sticky. If you *manually* build with custom `MESH_*_MAC`, a later
> bare `make build` in the same `build/` dir keeps the last value (standard CMake behaviour) — run
> `make fullclean APP=test-mesh-large-frame` to revert to the defaults. The harness always passes
> explicit values, so a real T2 run is unaffected.

## Firmware

`firmware/test-mesh-large-frame/` (symmetric 3-node; role by MAC; per-role peer allowlist forces the
line; origin reports; large ping payload → the FW fragments the encrypted mesh frame).

Full catalogue + provenance: `tools/regtest/t2_tests.py` (`MESH_LARGE_FRAME`), `make test-t2 TEST=mesh-large-frame`. Results: `docs/regression/rimba-regression-results.md`.
