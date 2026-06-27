# 2026-06-27 — Mesh security Phase 1: firmware key-install boundary PROVEN on a MESH vif

Phase 1 of the mesh-security port (SAE+AMPE, following Morse's Linux stack) verified the pivotal
unknown on real hardware: **the MM6108 firmware accepts `SET_STA_STATE` + `INSTALL_KEY` for an
802.11s mesh peer** — it mints a per-peer station handle and installs the pairwise MTK + group
MGTK, all returning success. The −116 (`-ENETDOWN`) wall that blocks IBSS/ADHOC chip-CCMP does
**not** apply to the MESH vif, exactly as the `morse_driver` code-map predicted (mesh runs the AP
key path; `mac.c:4933` gates the AID bitmap on `AP || MESH_POINT`).

## Result (on-air-adjacent: board0 console, ESP↔ESP mesh, ch27 clear)

```
MESH-SEC sta_state aid=2 state=2 ret=0     # SET_STA_STATE AUTHENTICATED
MESH-SEC sta_state aid=2 state=3 ret=0     # SET_STA_STATE ASSOCIATED
MESH-SEC sta_state aid=2 state=4 ret=0     # SET_STA_STATE AUTHORIZED
MESH-SEC install MTK (pairwise) aid=2 ret=0   # INSTALL_KEY key_type=PTK
MESH-SEC install peer MGTK (group RX) aid=2 ret=0  # INSTALL_KEY key_type=GTK
netif up=1                                 # peer reached ESTAB
```

Every firmware command returns 0. The peer gets a real station handle (aid=2, self-assigned from
the mesh peer pool — Linux assigns it in wpa_supplicant `hostapd_get_aid`, `mesh_mpm.c:798`).

## The implementation (feature/mesh-security-phase1 in the submodule)

`umac/mesh/umac_mesh.c`: `struct mesh_peer.aid` (pool-index+1); `umac_mesh_peer_secure_estab()`
called at both ESTAB edges (`:1469`/`:1483`) — steps the peer AUTH→ASSOC→AUTHORIZED via
`mmdrv_update_sta_state`, then installs MTK (pairwise, key_idx 0) + peer MGTK (group, key_idx 1)
via `mmdrv_install_key`, mirroring hostap `mesh_mpm_plink_estab` (`mesh_mpm.c:928/938`). Keys are
static/shared (Phase 1); P2 derives the real MTK + exchanges MGTK via AMPE. Code-map:
`docs/mesh-ap/rimba-mesh-security-codemap.md`.

## Two findings that cost the debugging (both real, both fixed)

Getting here required fixing a peering regression that masked the test. Two independent causes:

### 1. The own-MGTK install at mesh start breaks OPEN MPM peering (architectural)
My first cut followed Linux literally and installed the node's own MGTK (group key, aid=0) at mesh
**start** (`__mesh_rsn_auth_init`, `mesh_rsn.c:266`). On hardware this **prevented ESTAB**. Clean
A/B, same clear channel: `PHASE1=1` → `netif up=0` (no peer); `PHASE1=0` → `netif up=1` (peers).
Removing the start-time install → peering returns. **Mechanism:** installing a group key flips the
firmware to expect protected frames; morselib's MPM peering is **OPEN** (unprotected Open/Confirm),
so those frames get dropped → no ESTAB. Linux gets away with the start-time install because its
peering is **AMPE-protected from the start**. So this is a forced divergence: with open MPM the own
MGTK must be **deferred** (it only belongs at start once AMPE-protected peering lands in P2). The
key-install boundary test doesn't need it — per-peer keys at ESTAB are enough.

### 2. The 4-node Linux secured mesh on ch27 saturates the channel
Separately, the live `rimba-smesh` (chronium-monitor + chronite/chronosalt/chronogen) continuously
beaconing/handshaking on ch27 **breaks the ESPs' sparse peering** (the action frames don't survive
the contention; beacons/discovery do). `PHASE1=0` peers only with the Linux mesh **stopped**. The
ESP-ESP test (which worked 2026-06-26 before this mesh existed) must run with ch27 clear, or on a
different channel. **Stop the Linux mesh during ESP mesh tests.**

### Bonus: morselib MMLOG is not on the ESP console
None of the `umac_mesh.c` `MMLOG_INF` lines (MESH FSM, MESH-SEC) reach the serial console — raising
`MMLOG_LEVEL_OVRD` didn't help (the "Morse Micro HaLow NetIF" lines are ESP_LOG from the netif glue,
not MMLOG). The MESH-SEC returns above were surfaced with a **temporary `printf`** (marked TEMP in
the diff). Revert before merge, or wire MMLOG to the console properly.

## State / cleanup TODO (uncommitted feature branch)
- TEMP debug to revert: `#include <stdio.h>` + the `printf("MESH-SEC …")` lines (were MMLOG_INF).
- Own-MGTK-at-start is deferred (commented) — revisit in P2 with AMPE-protected peering.
- A workflow misdiagnosed this as a "responder never sends OPEN" FSM bug — it trusted an unreliable
  on-air SA/category decode; the board-side console (`netif up`) + the clean PHASE1 A/B were ground
  truth. Lesson: for MPM, the node's own FSM log/state beats a lossy sniffer decode.

## Next (P1→P2)
P1 boundary is proven. Remaining for a *working* static-key encrypted mesh: install the own MGTK
(group TX) after first ESTAB (so broadcast encrypts) and confirm encrypted data on-air via the
chronium monitor. Then P2: replace static keys with AMPE (AES-SIV MIC + MTK derive + MGTK exchange).
