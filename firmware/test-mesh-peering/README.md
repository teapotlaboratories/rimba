# test-mesh-peering — 802.11s secured-mesh peering (SAE+AMPE) → ESTAB

**Status: automated + hardware-verified** (board0+board1, 2026-07-16: reporter reached 1 ESTAB
peer = the other board's mesh MAC → PASS).

A **symmetric** test: both mesh nodes run this identical app. The orchestrator flashes it to a
support node (comes up + beacons) and to the reporter node (polls the ESTAB peer count).

## Rig — two roles (both this app)

| role | device | notes |
|---|---|---|
| **support** | board0 | comes up, beacons on Mesh ID `rimba-mesh`; up-marker `"mesh-up"` |
| **reporter** | board1 | polls `mmwlan_mesh_peer_count()` until ≥ 1 ESTAB peer, emits the verdict |

ESP↔ESP peering needs **no Linux anchor** (memory: `esp-esp-mesh-peers-no-anchor`). Light load, so
board0/board1 are fine for the handshake.

## What it proves / does not prove

- **Proves:** two ESP nodes complete the mesh peering handshake (MPM + SAE + AMPE) against each
  other and the plink reaches **ESTAB** — the foundation every mesh milestone sits on.
- **Does NOT prove:** forwarding, multi-hop, or on-air byte-equivalence with Linux (ESTAB only
  proves a tolerant peer completed the handshake).

## Assertion

`mmwlan_mesh_peer_count() >= 1`. ESTAB is binary — SAE+AMPE either closed or it didn't — and is
independent of RF quality (a fading link changes timing, not whether the handshake completes).

## How to run

```sh
make test-t2 TEST=mesh-peering
```

## Expected console (reporter / board1)

```
TEST|INFO|mesh-up: vif up + beaconing on ch27 as e2:72:a1:f8:f9:40
TEST|STEP|peer-estab|PASS|estab_peers=1
TEST|RESULT|PASS|mesh peering (MPM+SAE+AMPE) reached ESTAB: 1 peer(s), first=e2:72:a1:f8:ef:a4
```

Needs `CONFIG_HALOW_AP_MODE=y` (mesh reuses the AP-type interface + beacon path), set in this app's
`sdkconfig.defaults`.
