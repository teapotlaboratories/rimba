# test-mesh-ap — Mesh + AP concurrency on one MM6108 (the mesh-gate)

**Status: automated + hardware-verified** (2026-07-16, on the 3-board bench). All role apps are
`test-*`.

## Rig

| role | device | app |
|---|---|---|
| gate (support) | board2 — MANDATORY board2 (fully wired) | `test-mesh-ap-gate` |
| mesh_peer (support) | board1 | `test-mesh-ap-peer` |
| sta **[REPORTER]** | board0 | `test-mesh-ap-sta` |

## What it proves / does not prove

- **Proves:** Both vifs beacon concurrently on a single radio and a STA associated to the AP half routes through the mesh half (the all-ESP mesh-gate, hw-proven 2026-07-08/07-09).
- **Does NOT prove:** Gate throughput -- the recorded stress matrix found the gate IS the throughput bottleneck (cross-gate 30-62% loss vs mesh-to-mesh 3-13% under flood). That is a known characteristic, not a regression, so it must never be asserted as a pass/fail.

## Structural assertion

the STA behind the AP pings a far mesh node and observes **ttl=63** -- the gate's one `ip4_forward` decrement between the AP subnet (192.168.12.0/24) and the mesh subnet (10.9.9.0/24) proves exactly one gate hop. (TTL works here because the gate L3-routes, unlike the L2 mesh-relay.)

## How to run

```sh
make test-t2 TEST=mesh-ap
```

The orchestrator flashes each role (support roles first, verified up via a console up-marker), then
captures the reporter's `TEST|RESULT`, and returns every board to `rimba-hello` (radio-silent).

Needs board2 powered (`tools/ppk2_hold.py`) as the gate; the orchestrator refuses the gate role on board0/board1.


## Firmware

`firmware/test-mesh-ap-{gate,peer,sta}/` (the gate reuses the product mesh-gate logic; the peer's return route is repointed to the board2 gate 10.9.9.108; the sta reports).

Full catalogue + provenance: `tools/regtest/t2_tests.py` (`MESH_AP`), `make test-t2 TEST=mesh-ap`. DRY_RUN=1 Results: `docs/regression/rimba-regression-results.md`.
