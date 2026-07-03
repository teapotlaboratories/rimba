# 2026-07-02 — Mesh dynamic path table (dest-MAC hash index, lift the 8-path ceiling)

Replaces morselib's fixed 8-entry linear-scan mesh path table with a **64-entry pool + a dest-MAC hash
index** (O(1) lookup, `int16_t`-chained), lifting the hard cap on reachable destinations. Implemented,
host-tested, built, and bench-verified (peering + 0%-loss datapath). All changes are file-local to
`umac_mesh.c`. Uncommitted pending review.

## Why
`#define MESH_MAX_PATHS (8)` backed `mesh_paths[8]` (one entry per reachable **destination**, so it grows
with total mesh size, not local neighbours). On the 9th distinct destination `mesh_path_get_or_add`
silently evicted the soonest-to-expire entry — any-to-any traffic across >8 nodes **thrashed paths**. That
8 was the real network-size wall.

## Scope — path table only (assessed all three tables)
- **PATH = the ceiling → scaled.** Small entry (~40 B), grows with mesh size. Target: capacity a
  compile-time knob, default **64**, with O(1) hashed lookup so the raise is free on the hot path
  (every forward / `umac_mesh_lookup_next_hop` / PREQ / PREP).
- **PEER = neighbour-bounded → left at 4.** `UMAC_MESH_MAX_PEERS (4)` (`umac_mesh.h:98`). Capped by
  physical RF neighbours + the plink policy, not mesh size. Also the **memory hog**: `struct mesh_peer`
  carries `sae_commit[192]` + `sae_confirm[192]` ≈ 630 B/entry — the lever there is shrinking those SAE
  bodies, not adding a hash. Linear `mesh_peer_find` over ≤~8 is trivial.
- **RMC = fine fixed cache → left.** 16-slot FIFO ring, 2 s dedup window (`umac_mesh.c:2531-2556`).

## Design decision — fixed pool + `int16_t`-chained hash
Chosen: a static entry **pool** (`mesh_paths[64]`, BSS) + an integer-indexed **chained hash** keyed by dest
MAC. Chain links are `int16_t` **pool indices** (not pointers) — position-independent (survive `memset`),
2 B each, zero heap.

| Option | Verdict | Reason |
|---|---|---|
| Fixed pool + index-chained hash | ✅ chosen | Zero heap / zero fragmentation, all BSS. O(1) find on the hot path; the soonest-to-expire eviction is preserved as a cold O(cap) scan. Trivially host-testable. |
| Dynamic alloc-per-entry | ❌ | Heap fragmentation on the ESP's small heap under constant path install/evict churn; adds an alloc failure path to the datapath. |
| Full rhashtable + RCU (Linux) | ❌ | Unportable/overkill — resize machinery, `rcu_head`/`kfree_rcu`, four spinlocks, per-entry timers, the parallel walk list. The single umac event loop has **no concurrent readers/writers**, so none of it earns its RAM. |

**Chaining, not open addressing:** eviction reuses a slot, so a dest must leave its bucket — chaining
unlinks in one step; open-addressing deletion needs tombstones/backshift over the fixed pool.

## Implementation (`umac_mesh.c`, all file-local — no external refs to the changed symbols)
1. `MESH_MAX_PATHS 8 → 64`; add `MESH_PATH_BUCKETS (64)` (pow2 ≥ cap) + `MESH_PATH_NIL ((int16_t)-1)` (`:367`).
2. `struct mesh_path_entry`: add `int16_t hnext` (bucket chain link); add `mesh_path_bucket[64]` (`:1801-1815`).
3. New helpers: `mesh_path_hash` (FNV-1a over the 6 MAC bytes → bucket), `mesh_path_hash_insert` /
   `_remove` (chain by pool index), `mesh_path_tbl_reset` (`~:1816`).
4. `mesh_path_find` — hashed bucket walk, **same signature** (`:1821`).
5. `mesh_path_get_or_add` — hashed find + the **byte-identical** soonest-to-expire eviction, plus unlink
   the evicted dest / index the new one, **same signature** (`:1833`).
6. Table reset (`:2981`): bare `memset(mesh_paths,…)` → `mesh_path_tbl_reset()`.

**Untouched (verified):** `mesh_path_update` (the shipped P6c/HWMP fresh-info + 10/9 hysteresis),
`umac_mesh_lookup_next_hop` (+ preemptive refresh), discovery, PERR teardown, `invalidate_paths_via`
— all call the same-signature find/get_or_add or iterate the pool (bound `MESH_MAX_PATHS`, now 64).

### Two correctness details
- **Reset must NIL the buckets** — a bare `memset` leaves every bucket head + `hnext == 0`, i.e. all
  pointing at entry 0, so `mesh_path_find` would **cycle**. `mesh_path_tbl_reset` sets buckets + `hnext`
  to `NIL` after the zero. It runs at every mesh start (`:2981`, with the ctx/peer/rmc resets), before any
  lookup; BSS-zero is never walked.
- **Hash/`used` invariant holds** — a path is in the hash **iff** `used==true`, and `used` only changes
  via `get_or_add`'s eviction (which `hash_remove`s first). Audited every path clear: expiry (`:2009`),
  PERR/teardown (`:2228,:2479`), `invalidate_paths_via` (`:2216`) all set `active=false` (+`sn++`) and
  **keep `used=true`** → the entry stays validly linked. No path is ever `used=false` outside eviction
  (the only `used=false`, `:598`, is the peer table). So the stale-bucket-link corruption path is
  impossible.

## Verification
- **Host test** (standalone C, exact ported logic) — **ALL PASS**: (T1) 20 distinct inserts past the old
  cap 8 → 20/20 findable, chains sound; (T2) fill to 64 (load 1.0, chaining collisions guaranteed) →
  64/64 findable, **no cycles**; (T3) 65th insert evicts the min-expiry entry → evicted dest gone, new
  dest present, other 64 intact, used stays 64; (T4) idempotent `get_or_add` → same slot, no dup.
- **Build:** `make build APP=rimba-halow-mesh BOARD=proto1-fgh100m` — `umac_mesh.c` compiles clean, no
  warnings, image built. ✓
- **Live mesh (behaviour-neutral):** board0 ↔ board1 secured-peer (ESTAB); chronite brought up as a mesh
  peer (2 stations) and **ping board0 + board1 → 0% loss both** — each ESP resolves a path back via the
  hashed `get_or_add` and forwards. The refactor is behaviour-neutral for ≤8 paths (identical eviction +
  the untouched `mesh_path_update`); the >8 capability is what the host test exercises (a bench mesh
  can't reach >8 nodes here).
- **Edge-case audit:** the `used`/`active` invariant above.

## Divergences from mac80211 (each deliberate)
1. **Fixed pool + hash index, not `rhashtable`** — no resize/RCU; single event loop has no concurrent access.
2. **Cap 64, not `MESH_MAX_MPATHS = 1024`** (`mesh.h:229`) — embedded RAM; ~2.7 KB at 64 (was ~288 B),
   knob-able to 32 (~1.3 KB). Raise if a deployment needs it.
3. **FNV-1a over 6 bytes, no per-table seed** — vs `mesh_table_hash` `jhash_1word(last4, seed)`
   (`mesh_pathtbl.c:22-34`); no adversarial rehash concern on a private mesh.
4. **Eviction = soonest-expiry victim** (unchanged existing policy) — vs Linux GC by `mesh_path_expire`
   (`mesh_pathtbl.c:1095`) + the 1024 admission cap.

## Code-map
Rows added to `docs/mesh-ap/rimba-mesh-80211s-code-map.md` (Path-table section); Linux refs re-grepped on
chronite's pinned `rpi-linux` (`mesh_pathtbl.c:22/680`, `mesh.h:105/229`, `ieee80211_i.h:700`).
