# Worklog — 2026-07-12 — Mesh A-MPDU: multi-hop RA-flapping root cause + the throttle ceiling

**Author:** Aldwin
**Phase:** 802.11s mesh throughput — A-MPDU aggregation (S2 follow-up, "clean multi-hop number")
**Goal:** get a clean *sustained* multi-hop A-MPDU measurement for S2 — which meant first
explaining the ~49% **RA=dest flapping** that had polluted the S2 multi-hop capture and
prevented sustained aggregation.
**Status:** **Root cause found + proven.** The flapping is **not** an HWMP timing bug — it is
the **ESP relay flapping under load** (the interrupt-WDT / GPIO-ISR instability), which
invalidates the path so the origin falls back to `RA = dest`. Swapping in a **stable Pi-5
relay (chronite)** pinned the origin's next hop to **100%** the relay (0% RA=dest). But even
then the multi-hop **origin** does not aggregate — because the 2-hop **downstream throttle**
keeps its first-leg queue too shallow. Verified this is inherent (not a BA/S2 defect): the same
origin aggregates **3-MPDU deep at 1.7 Mbit/s** on a *direct* single-hop flow. **Conclusion:
the multi-hop goodput lever is S3 (relay forward-leg aggregation), not S2.** Bench radio-silent
after; nothing committed.

This entry is **standalone**.

---

## 1. The symptom carried over from S2

The S2 multi-hop capture (board1 → board0-relay → board2, all-ESP forced line) showed board1's
QoS-Data split **~49% RA = board0 (the correct next hop)** vs **~49% RA = board2 (the final
destination)**. The RA=dest frames are wasted singletons (board2 drops them by allowlist) that
interleave with the good RA=board0 frames and starve A-MPDU. S2's BA-on-next-hop mechanism was
proven (the ADDBA reaches the next hop), but sustained aggregation never materialised. This
worklog explains why.

## 2. Where RA=dest comes from (code)

`umac_datapath_construct_80211_data_header_mesh` (`umac_datapath.c:3634-3639`):

```c
uint8_t next_hop[DOT11_MAC_ADDR_LEN];
if (!umac_mesh_lookup_next_hop(hdr_8023->dest_addr, next_hop))   /* no resolved mpath */
{
    mac_addr_copy(next_hop, hdr_8023->dest_addr);                /* FALLBACK: RA = dest */
    umac_mesh_start_discovery(hdr_8023->dest_addr);
}
mac_addr_copy(data_hdr->base.addr1, next_hop);                   /* RA = next hop (or dest) */
```

So **RA = dest whenever the mpath to the destination is momentarily absent**. Two facts rule out
the obvious "path expired" explanation:

- **Path lifetime is 30 s** (`MESH_PATH_LIFETIME_MS = 30000`, `umac_mesh.c:379`) with a
  **preemptive refresh** in the final 6 s (`MESH_PATH_REFRESH_MS = 6000`, mirroring mac80211
  `mesh_path_refresh`). The iperf test is **14 s** — far shorter than the lifetime.
- So ~49% RA=dest is **not** lifetime expiry. The path is being **repeatedly invalidated**
  mid-test.

## 3. Root cause — the ESP relay flaps under load

The path is invalidated when its next hop (the relay) is lost/PERR'd. board0, the **ESP relay**,
is exactly the node that crash-loops on an **interrupt-WDT in the ESP-IDF GPIO-ISR layer under
sustained relay load** (documented in the S2 worklog §5). Every relay hiccup drops board0's mesh
link → board1 invalidates the path via board0 → `RA = dest` until the next PREQ/PREP resolves it
→ path back → hiccup again. That cycle is the ~49% RA=dest.

**Proof — swap the relay.** Reconfigured the line to use a **stable Pi-5 relay** instead of the
ESP: **board1 (ESP) → chronite (Linux relay) → board0 (ESP)**, both ESP endpoints allowlisting
chronite (a temporary `MESH_RELAY_VIA_CHRONITE` app toggle; board2 was PPK2-power-gated dead
after the dev-host reboot, so board0 served as the 2nd endpoint). chronite runs the same
`rimba-mesh` Linux mesh as the interop test and forwards (`mesh_fwding = 1`).

| relay | board1 QoS-Data RA distribution |
|---|---|
| board0 (ESP, flaps under load) | ~49% RA = next hop · ~49% RA = dest |
| **chronite (stable Pi 5)** | **954 / 954 = 100% RA = chronite · 0% RA = dest** |

A stable relay ⇒ the path never gets invalidated ⇒ **the flapping vanishes entirely**. That
pins the root cause on the ESP relay's load instability, not on HWMP.

## 4. But the multi-hop origin still doesn't aggregate — and that's inherent

With the path pinned to chronite, board1 → board0 (via chronite) still measured **0.24 Mbit/s,
0 aggregated PPDUs** on board1's origin leg. That is **not** a BA/S2 defect — it is the 2-hop
**downstream throttle** starving the origin's queue. Proven with a direct single-hop control:

| flow | throughput | board1 aggregation (board1 → chronite) |
|---|---|---|
| **multi-hop** board1 → board0 *via chronite* | 0.24 Mbit/s | 954 PPDUs, **0 aggregated** |
| **direct** board1 → chronite (single hop, `-b 3`) | **1.7 Mbit/s** | 769 PPDUs, **336 aggregated** (dist `433×1, 67×2, 268×3, 1×4`) |

board1's BA session to chronite **completes and aggregates 2–3 MPDUs deep** when the flow is a
high-rate single hop. In the 2-hop case the downstream bottleneck (relay forward leg) throttles
the end-to-end goodput to ~0.24 Mbit/s, so board1's first-leg TX queue rarely holds two frames
when a PPDU is built → single MPDUs. A-MPDU can only aggregate what is **already queued**; a
throttled origin has nothing to aggregate.

## 5. Takeaways

- **The flapping fix is a stable relay.** On this all-in-RF-range bench the only way to hold a
  multi-hop path steady is a relay that doesn't flap — a Linux node, or a fix for the ESP relay's
  interrupt-WDT under load.
- **S2 (multi-hop origin BA) is correct but rarely engages.** The origin is throttle-limited, so
  its A-MPDU-eligible flag seldom fires deep. A-MPDU's real payoff is on **high-rate single-hop
  legs** — proven 2–3 MPDU deep both ESP↔ESP and ESP↔Linux.
- **The multi-hop goodput lever is S3** (relay forward-leg aggregation): the **relay** holds the
  whole aggregate flow queued, so it — not the throttled origin — is where per-hop airtime gets
  amortised. Prioritise S3 over squeezing more from S2.
- **Follow-Linux fix worth doing:** the `RA = dest` fallback (`umac_datapath.c:3636`) is a
  divergence from net/mac80211 `mesh_nexthop_resolve`, which **queues the frame and discovers**
  instead of transmitting to the final destination. Adopting queue-and-discover would stop a
  non-peered multi-hop destination from emitting wasted singletons during path gaps.

## 6. Bench notes + teardown

- The **dev host rebooted mid-session** (tmpfs `/tmp` wiped: helper scripts + archived pcaps
  gone — findings preserved here), and **board2's PPK2 DUT-rail power dropped** on the reboot, so
  board0 stood in as the 2nd endpoint.
- Teardown radio-silent: board0 + board1 → `rimba-hello`; chronite `wpa_supplicant_s1g` killed +
  `wlan1` down; chronium `morse0`/`wlan1` down. The `MESH_RELAY_VIA_CHRONITE` / interop app
  toggles were reverted (tree `app_main.c` back to the `MESH_IPERF` default). **Nothing
  committed.** See [`../mesh-ap/rimba-mesh-ampdu-aggregation-design.md`](../mesh-ap/rimba-mesh-ampdu-aggregation-design.md)
  and the S2 worklog for the aggregation mechanism.
