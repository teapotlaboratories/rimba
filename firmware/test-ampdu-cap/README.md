# test-ampdu-cap — Firmware advertises the mesh A-MPDU capability (S0a)

**Status: automated + hardware-verified** (2026-07-16, on the 3-board bench). All role apps are
`test-*`.

## Rig

| role | device | app |
|---|---|---|
| dut | any single board | `test-ampdu-cap` |

## What it proves / does not prove

- **Proves:** The MM6108 firmware still reports the mesh AMPDU capability the S0 spike proved on 2026-07-11 -- i.e. the capability gate a stack bump could silently close is still open.
- **Does NOT prove:** That aggregation actually happens on the air, or any throughput figure. A-MPDU is FW-assembled; the capability bit is necessary, not sufficient. Real aggregation needs a peer + a capture (test-mesh-relay + the chronium monitor).

## Structural assertion

`mmwlan_ampdu_capability_advertised() == 1` -- binary/deterministic, the same bit the aggregation eligibility gate consumes. A stack bump that drops it fails here.

## How to run

```sh
make test-t2 TEST=ampdu-cap
```

The orchestrator flashes each role (support roles first, verified up via a console up-marker), then
captures the reporter's `TEST|RESULT`, and returns every board to `rimba-hello` (radio-silent).


## Firmware

`firmware/test-ampdu-cap/` (single board; brings up a mesh vif, reads `mmwlan_ampdu_capability_advertised()`).

Full catalogue + provenance: `tools/regtest/t2_tests.py` (`AMPDU_CAP`), `make test-t2 TEST=ampdu-cap`. DRY_RUN=1 Results: `docs/regression/rimba-regression-results.md`.
