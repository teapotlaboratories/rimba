# Mesh peer-link liveness fix — follow-Linux code-map

**Date:** 2026-07-12
**Reference:** `rpi-linux` (mac80211 mesh) @ `372414fd4`, on chronite `~/halow/rpi-linux`. **Every Linux
`file:line` below re-grepped in that checkout — verified 2026-07-12** (not cited from memory).
**Scope:** Make the ESP32/morselib 802.11s mesh hold an established peer link as robustly as Linux
mac80211, so marginal RX (a brief fade dropping a few beacons) no longer tears the plink down and
forces a fresh SAE re-peer. **No new keepalive traffic** — Linux sends none; the fix is the same two
mechanisms Linux actually uses (a long inactivity window + counting every RX as activity).

## Problem (committed `7fdba0c9` behaviour, not introduced by S2/S3)

`umac_mesh_plink_tick` tears down an ESTAB peer when `now - peer->last_rx_ms > MESH_PLINK_INACTIVITY_MS`
(`umac_mesh.c:1346`). Two divergences from Linux made that fire spuriously:

1. **`MESH_PLINK_INACTIVITY_MS = 6000` (6 s)** — ~300× shorter than Linux's `plink_timeout` (1800 s).
   At ~100 ms beacons that tolerates only ~60 missed beacons; a brief RX fade tears the link down.
2. **`last_rx_ms` was refreshed only by the peer's beacon + peering/SAE frames — never by received
   DATA frames.** Linux refreshes `last_rx` on *every* received frame. So a peer actively sending us
   data, whose beacons we happen to miss, could still expire.

Under user-MPM secured mesh, teardown silently destroys the sta (no Close) → recovery requires a full
fresh SAE handshake, i.e. the observed "establish → drop → re-SAE" flap.

## Linux mechanism (verified file:line on chronite `~/halow/rpi-linux`)

| Aspect | Linux | file:line |
|---|---|---|
| Inactivity teardown | `ieee80211_mesh_housekeeping` calls `ieee80211_sta_expire(sdata, plink_timeout * HZ)`; expire tests `last_active + exp_time` | `mesh.c:911`/`:917`, `sta_info.c:1616`/`:1630` |
| Timeout default | `MESH_DEFAULT_PLINK_TIMEOUT = 1800` (seconds) | `net/wireless/mesh.c:26`, applied `:84` |
| Housekeeping cadence | every 60 s (`IEEE80211_MESH_HOUSEKEEPING_INTERVAL`) | `mesh.h:221` |
| "last_active" | max(last_rx, last_ack) | `sta_info.c:2857` |
| last_rx ← peer beacon | `mesh_sta_info_init()` sets `last_rx = jiffies` for the (existing) sta on every matching mesh beacon; chain `rx_bcn_presp → mesh_neighbour_update → mesh_sta_info_get` | `mesh_plink.c:446`, `mesh.c:1456`/`:1507` |
| last_rx ← any frame (incl. data) | fast-RX (`:4810`) + general RX path (`:1770`) set `last_rx = jiffies` | `rx.c:4810`, `rx.c:1770` |
| Periodic keepalive on ESTAB | **NONE** — the plink timer is a no-op in LISTEN/ESTAB ("Ignoring timer … timer deleted") | `mesh_plink.c:696` |
| Teardown (user-MPM) | silent destroy → `__mesh_plink_deactivate`, Close sent only in `!user_mpm` | `mesh_plink.c:410`/`:414` |

Math: 1800 s / ~1.024 s-beacon ≈ **~1758 beacons** of slack (Linux) vs ~60 (ESP 6 s @ 100 ms) — Linux
rides through minutes of loss.

## The fix — new code ↔ Linux mapping

| # | ESP change | Mirrors Linux |
|---|---|---|
| 1 | `MESH_PLINK_INACTIVITY_MS`: `6000` → `1800u*1000u` (1800 s) + corrected provenance comment (`umac_mesh.c:415-418`) | `MESH_DEFAULT_PLINK_TIMEOUT = 1800` (`net/wireless/mesh.c:26`) |
| 2 | New `umac_mesh_note_peer_rx(ta)` (`umac_mesh.c`, decl `umac_mesh.h`) — find peer by TA, set `last_rx_ms`; called from the mesh data-RX path after successful decrypt (`umac_datapath.c`, post `FDBG_RX_DECRYPT_OK`, with `dot11_get_ta(header)`) | `last_rx = jiffies` on every received frame (`rx.c:4810`) |

Already-correct (kept): the peer **beacon** already refreshes `last_rx_ms` (`umac_mesh.c:1414`,
`umac_mesh_handle_peer_beacon`), mirroring `mesh_plink.c:446` — so the ESP already counts beacons as
activity; only data-frame activity + the timeout scale were wrong.

**Deliberately NOT done:** a periodic null-data/QoS-Null keepalive on ESTAB. Linux sends none
(`mesh_plink.c:692-699`); adding one would diverge from Linux (and violate "follow-Linux, no
symptom-hacks"). Liveness comes from beacons + data activity + the long window, exactly as in Linux.

## Risk / behaviour notes

- A genuinely-dead peer now lingers up to 1800 s (as in Linux). Benign here: HWMP **paths** expire
  independently after `MESH_PATH_LIFETIME_MS = 30000` (30 s) and re-discover, and a returning peer's
  Close/re-SAE frees the slot immediately. So routing is unaffected by the longer peer hold.
- Strictly more stable: both changes only *extend* how long a link is kept / *add* activity signals;
  neither can cause a spurious teardown.
- The ESP evaluates the inactivity check on its ~300–500 ms plink tick (finer than Linux's 60 s
  housekeeping); with an 1800 s threshold it simply fires promptly once truly silent for 1800 s.

## Verification

Flash board2, cold-boot, chronite mesh, confirm sustained stable peering (already stable pre-fix in a
fresh session — the fix hardens the marginal-RX / under-load cases). Best signal: a data blast through
board2 (relay) — pre-fix, sustained load that starves beacon RX could expire the peer at 6 s; post-fix
it should not. Radio-silent after (per bench rule).
