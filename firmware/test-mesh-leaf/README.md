# test-mesh-leaf — Mesh leaf / single-hop mode (multihop opt-out)

**Status: new (2026-07-20) — developed on the pre-forward-port morselib 2.10.4 baseline; not yet
hardware-verified.** All role apps are `test-*`.

## Rig — the SAME 3-node forced line as `test-mesh-relay`, relay in leaf mode

| role | device | app |
|---|---|---|
| relay (support) | board2 — MANDATORY board2 (fully wired); calls `mmwlan_mesh_set_multihop(false)` | `test-mesh-leaf` |
| dest (support) | board1 | `test-mesh-leaf` |
| origin **[REPORTER]** | board0 | `test-mesh-leaf` |

## What it proves / does not prove

- **Proves:** `mmwlan_mesh_set_multihop(false)` makes a node a **leaf** — it keeps its 1-hop mesh plinks to both endpoints but does **not** forward origin↔dest, and does **not** black-hole (its peering survives). A shipped, on-air-A/B-verified runtime opt-out (P6d) that otherwise had no regression guard: a port-forward could silently re-enable relaying, or turn the opt-out into a black hole (drop the peering / go dark).
- **Does NOT prove:** the HWMP-invisibility of a leaf (its absence from other nodes' path selection) — only that it declines to forward and keeps peering. Path-selection invisibility needs a ≥3-hop topology this 3-board bench cannot form.

## Structural assertion — INVERTED from `test-mesh-relay`

Deterministic (not RF-bound): the forced allowlist makes the relay the ONLY path to the dest, so a leaf relay declining to forward → **exactly 0 replies**.

- **peering-survives:** the origin's sole mesh peer is still the relay (leaf kept the 1-hop plink — no black-hole / crash).
- **forwarding-blocked:** `replies == 0`.

PASS iff both. FAIL on **any** reply (relaying still enabled → leaf broken) or on lost peering (black-hole). A single reply is a real leak, not RF noise.

## How to run

```sh
make test-t2 TEST=mesh-leaf
```

Needs board2 powered (`tools/ppk2_hold.py`) as the relay; the orchestrator refuses the relay role on board0/board1.

## Line topology (the three MACs) — build-time arguments, not hardcoded

Identical scheme to `test-mesh-relay`: the symmetric app self-selects its role by comparing its own
mesh MAC to the three line MACs (`MESH_ORIGIN_MAC` / `MESH_DEST_MAC` / `MESH_RELAY_MAC`), which the
harness passes to every flash from the manifest `BENCH` registry. No MAC is hardcoded; retarget the
line by editing the boards' `mesh_mac` in `tools/regtest/manifest.py`, never the firmware.

## Firmware

`firmware/test-mesh-leaf/` (symmetric 3-node; role by MAC; relay calls `mmwlan_mesh_set_multihop(false)`;
origin inverts the assertion). Provenance: `tools/regtest/t2_tests.py` (`MESH_LEAF`), `make test-t2 TEST=mesh-leaf`.
