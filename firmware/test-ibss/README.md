# test-ibss — IBSS join/adopt + peer-table correctness

**Status: automated + hardware-verified** (2026-07-16, on the 3-board bench). All role apps are
`test-*`.

## Rig

| role | device | app |
|---|---|---|
| support (support) | board0 | `test-ibss` |
| reporter **[REPORTER]** | board1 | `test-ibss` |

## What it proves / does not prove

- **Proves:** IBSS_CONFIG(CREATE) succeeds and a node forms exactly the right peer records with no phantom entries -- the structural core of the IBSS port. (Exactly-1-peer on a 2-node cell IS the 0-phantom check.)
- **Does NOT prove:** On-air frame equivalence with Linux (I.4 was never done -- the monitor was compiled out at the time; docs/ibss/rimba-ibss-test-plan.md:172), nor IBSS throughput (P1.1-P1.3 have no recorded number at all -- 'numbers TODO', :285).

## Structural assertion

`mmwlan_ibss_start() == SUCCESS` (IBSS_CONFIG(CREATE)==0) AND **exactly 1** peer record on a 2-node cell -- the exact count IS the 0-phantom (divergence-17) check.

## How to run

```sh
make test-t2 TEST=ibss
```

The orchestrator flashes each role (support roles first, verified up via a console up-marker), then
captures the reporter's `TEST|RESULT`, and returns every board to `rimba-hello` (radio-silent).


## Firmware

`firmware/test-ibss/` (symmetric; both nodes run it, creator pinned to board0's MAC; reporter polls `mmwlan_ibss_peer_count()`).

Full catalogue + provenance: `tools/regtest/t2_tests.py` (`IBSS`), `make test-t2 TEST=ibss`. DRY_RUN=1 Results: `docs/regression/rimba-regression-results.md`.
