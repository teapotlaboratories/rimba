# Mesh single-hop / leaf toggle — disable multi-hop forwarding + HWMP at runtime

**Date:** 2026-06-30
**Area:** 802.11s mesh (morselib `umac/mesh`)
**Status:** implemented + on-air verified (chronium `morse0`), committed on `feature/mesh-forwarding-toggle` (independent of the SAE-hardening branch)

## Goal

Give a mesh node a runtime switch to opt out of multi-hop entirely and behave as a
pure single-hop **leaf**: it still forms direct 1-hop peer links, but it never relays
other nodes' traffic and never participates in path selection. Useful for an endpoint
device that should not spend CPU/airtime/power forwarding for the mesh.

## API

```c
/* umac/mesh/umac_mesh.h:152 */
void mmwlan_mesh_set_multihop(bool enabled);   /* default enabled (= unchanged behaviour) */
```

`mmwlan_mesh_set_multihop(false)` turns the node into a leaf. The flag is a file-static
bool (`g_mesh_multihop`, `umac_mesh.c:521`) kept **outside** `mesh_ctx` (which is
`memset` to 0 on every `mmwlan_mesh_start`), so it is settable **before or after** start
and survives a mesh restart. Default `true` — every gate below is a precise no-op when
enabled, so existing multi-hop behaviour is byte-for-byte unchanged.

## Why gating the data relay alone is NOT enough (the black-hole trap)

The obvious fix — "stop relaying data" — is wrong on its own. If the node stops
forwarding data but keeps answering/forwarding HWMP, it still **advertises itself as a
usable relay**: peers install paths whose next hop is this node and then unicast their
multi-hop traffic *to* it — which it now silently drops. That is a routing **black hole**
that breaks exactly the routes that should have detoured around it — worse than not
participating at all.

So a correct leaf must ALSO be invisible to HWMP path selection (emit no PREQ/PREP/PERR),
so peers never choose it as a next hop in the first place. Group/multicast re-broadcast is
a third, separate data path that no HWMP gate covers and must be disabled on its own.

## Code map — six gates off one bool

All in `components/halow/.../morselib/src/umac/mesh/umac_mesh.c`:

| Gate | Line | Function | Effect when leaf (false) |
|---|---|---|---|
| unicast relay | 2261 | `umac_mesh_forward_data` | early `return false` → never relay another node's unicast (RX caller drops) |
| group relay | 2394 | `umac_mesh_handle_group_data` | skip the re-broadcast block → never re-broadcast others' multicast (still delivers locally) |
| own next hop | 1809 | `umac_mesh_lookup_next_hop` | early `return false` → own TX always goes direct, never via a multi-hop path |
| HWMP TX | 1903 | `umac_mesh_tx_hwmp` | early return → emits no PREQ/PREP (originated **or** forwarded — this is the sole TX chokepoint for both) |
| PERR TX | 2014 | `umac_mesh_tx_perr` | early return → emits no PERR (on peer loss or propagated) |
| discovery | 1941 | `umac_mesh_start_discovery` | early return → never originates path discovery |

`tx_hwmp` (1903) and `tx_perr` (2014) are the *only* two TX chokepoints through which any
PREQ/PREP/PERR leaves the node — originated (start_discovery, PREP-as-target) or forwarded
(intermediate PREQ flood, PREP forward, PERR propagate). Gating them makes the node fully
HWMP-silent. Verified by an adversarial leak-hunt: no forwarded DATA or HWMP frame has any
egress path that bypasses these gates; there is no RANN/root-announce/proactive HWMP.

Direct 1-hop traffic is untouched: a frame addressed to us (mesh-DA == our MAC) never
reaches `forward_data` — it takes the local-delivery path — and MPM peering / SAE / AMPE
are not gated at all.

## Linux analogue

Mirrors the *intent* of Linux `dot11MeshForwarding` (`mesh_fwding`) but is **stricter**:
`mesh_fwding=0` only stops forwarding-for-others (the node still does path selection for
its own traffic), whereas this toggle also disables the node's own multi-hop use and all
HWMP origination — a true single-hop-only node. Not a line-by-line port (the HWMP here is
morselib's own hand-rolled implementation, not `net/mac80211/mesh_hwmp.c`); it is an
original gate set with a Linux conceptual analogue.

## On-air verification (chronium `morse0`, 2026-06-30)

**Bench:** 3 ESP boards in the forced-line topology (`MESH_LINE_RELAY_DEMO`: the peer
allowlist blocks board1↔board2 direct, so board1↔board2 traffic must traverse board0).
board1 auto-pings board2 (10.9.9.108). chronium sniffs `morse0`.

- board0 = `e2:72:a1:f8:ef:a4` (relay, 10.9.9.136)
- board1 = `e2:72:a1:f8:f9:40` (endpoint, 10.9.9.100)
- board2 = `e2:72:a1:f8:f0:08` (endpoint, 10.9.9.108)

Monitor recipe (chronium): `sudo iw dev wlan1 set type monitor` + `set freq 5560`
(S1G ch27), then `sudo python3 ~/halow-mon.py <secs> e272a1f8efa4` (filter to board0's TA).

| Metric (board0 = `ef:a4`) | Test A — relay (multihop ON) | Test B — leaf (multihop OFF) |
|---|---|---|
| DATA-4addr forwarded (board0 TA) | 37 / 18 s | **0** / 30 s |
| HWMP (PREQ/PREP) from board0 | originating PREQs | **0** |
| board0 beacons | yes (alive) | yes — 820 beacons (alive + peered) |
| board1 → board2 ping | **0% loss** (replies 46–72 ms) | **100% loss** (34/34 timeouts) |
| board1 ↔ board0 direct link | up | up (re-peered, got mesh IP) |

The instant board0 became a leaf it stopped forwarding 4-addr data AND went HWMP-silent,
while still beaconing and holding its direct peer links. board1 could no longer reach
board2 (100% loss) but stayed directly peered with board0 — exactly a pure leaf.

**Reachability tradeoff observed live:** a leaf is HWMP-invisible, so when board0 dropped
out as a relay, board1 fell back to trying board2 *directly* (datapath direct-fallback,
seen as `TA=f9:40 A1=f0:08` in the capture). Since the allowlist never peered board1↔board2,
board2 dropped those → the 100% loss. Consequence for interop: a peer that resolves *all*
unicast via HWMP (e.g. Linux `mac80211`, which queues unicast behind path resolution) may be
unable to reach a leaf even directly. ESP peers reach it fine (they direct-fallback on a
path-lookup miss). If a leaf must stay HWMP-reachable, a softer variant would keep only the
PREP-as-target reply un-gated (answers "I'm here" without ever forwarding for others).

## Files / commits

- Submodule `feat/mesh-forwarding-toggle` (off `894dcd9f`, the merged secured-mesh base):
  `fe2ac1ef` — `umac_mesh.c` (+46/−5), `umac_mesh.h` (+8).
- Parent `feature/mesh-forwarding-toggle` (off `main`): app demo hook (`MESH_LEAF_NODE`,
  default-off) + this worklog + milestones/code-map updates + submodule gitlink bump.

Independent of the SAE-hardening branch (different functions; branched off the pre-SAE base).

## Verification status

- Build: green on the pre-SAE base (`rimba-halow-mesh`, proto1-fgh100m).
- On-air: **gold-standard is N/A** — this feature adds no new on-air frame to byte-diff
  against Linux; it *suppresses* existing frames. Verified instead by the A/B *absence*
  capture above (board0 emits 37→0 forwards, PREQ→0) plus end-to-end ping (0%→100% loss).

## Follow-ups

- App wiring: currently a default-off `MESH_LEAF_NODE` bench demo hook in `rimba-halow-mesh`
  (matching the existing demo-macro convention). Decide final surface (console command vs
  demo macro vs API-only).
- Optional "reachable leaf" mode (keep PREP-as-target) if Linux peers must reach a leaf.
