# 2026-07-02 — Mesh peer-table growth (UMAC_MESH_MAX_PEERS 4 → 8)

Doubles the ESP mesh peer table so a node can hold more than 4 direct neighbours — headroom for denser
meshes. A one-value change (Option A); the SAE bodies stay per-peer. Uncommitted → committed alongside
this worklog.

## Why
`UMAC_MESH_MAX_PEERS = 4` sizes the only peer array (`mesh_peers[MESH_MAX_PEERS]`, `umac_mesh.c:489`). It
caps **direct** (1-hop) neighbours, not mesh size (paths are separate — see the dynamic-path-table worklog).
In a mesh with >4 neighbours in RF range, an ESP hub could hold at most 4. 8 covers any near-term bench
topology (a 6-node mesh puts 5 peers on a hub).

## Decision — Option A (raise the count), not pool/defer the SAE bodies
`struct mesh_peer` is **636 B/entry** (nm-confirmed: `mesh_peers` bss = 0x9f0 = 2544 B / 4), dominated by
`sae_commit[192]` + `sae_confirm[192]` (`MESH_SAE_BODY_MAX=192`, `umac_mesh.c:83,480,482`). The tempting
saving is to pool/free those 384 B post-handshake — **but the assessment found they're read in the ESTAB
steady state**, not just during the handshake:
- `umac_mesh.c:1073-1077` — reauth restart-detect reads `peer->sae_commit`.
- `umac_mesh.c:1179-1181` — CONFIRMED-resync replays `peer->sae_confirm`.
- After ACCEPTED the bignum tmp is freed (`mesh_sae_clear_temp`, `umac_mesh.c:1151`), so the cached bytes
  **cannot be regenerated**.

So pooling/deferring would regress secure-mesh reauth/resync robustness for a 384 B/peer saving the RAM
budget doesn't need. The large transient SAE scratch (Dragonfly bignums) is **already heap** and already
freed at ACCEPTED — nothing left to pool. → keep the bodies inline; just raise the count.

## Edits
1. `umac_mesh.h:98` — `UMAC_MESH_MAX_PEERS (4) → (8)` (the single source of truth; `MESH_MAX_PEERS`
   aliases it at `umac_mesh.c:431`).
2. `umac_mesh.c:497` — `MESH_ALLOWLIST_MAX (4) → (8)` (an *independent* test-topology cap, not auto-scaled
   by the define; keep ≥ the peer count so all peers fit the allowlist).
3. `firmware/rimba-halow-mesh/main/app_main.c` — `#include "esp_system.h"` + a heap-free line in the 5 s
   heartbeat, to observe the runtime RAM headroom (telemetry).

**No struct change.** Every other count-sized site scales off the define automatically (verified): the
`mesh_peers` array, the peer-iteration loops (`:555,567,1244,1351,3174`), the index bound (`:656`), the
`estab_macs` copy in `mmwlan_mesh_peer_count` (`:1348-1357`), the datapath TX scheduler
(`umac_datapath.c:3380`), and the app's `peer_macs[UMAC_MESH_MAX_PEERS][6]` (`app_main.c:178`). The beacon
"0 peerings" byte (`:184`) is a cosmetic static template — unaffected.

## Verification
- **Build:** `make build APP=rimba-halow-mesh BOARD=proto1-fgh100m` — clean.
- **Static RAM delta (nm, authoritative):** `mesh_peers` **2544 → 5088 B** (8 × 636), `mesh_allowlist`
  **24 → 48 B**; ~+2.6 KB `.bss` total — negligible on the ESP32-S3. (16 is a drop-in alternative at
  +7.6 KB if a denser mesh is ever wanted.)
- **>4-peer establish (bench):** deferred to the next multi-node run — it piggybacks on the real-RC bench
  test (which needs a ≥6-node mesh anyway). Low-risk: this is a pure array-size bump; the per-peer plink
  FSM is unchanged, so >4 peers establish by construction. The heap telemetry will confirm runtime headroom.

## Divergence
No Linux analog — mac80211 has no such small static cap; this is an embedded static-array sizing choice.
