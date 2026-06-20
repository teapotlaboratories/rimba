# IBSS implementation comparison — ours vs `momentary-systems/esp-halow-ibss`

Comparison of our host-side IBSS port against the other public ESP32/MM6108 IBSS fork
(timothyb89 / momentary-systems), referenced from the Morse community thread
[*Firmware support for IBSS/ad-hoc on FGH100M*](https://community.morsemicro.com/t/firmware-support-for-ibss-ad-hoc-on-fgh100m/1653/5).

**Their repo:** `https://github.com/momentary-systems/esp-halow-ibss`
**Branch:** `ibss-support` · **Commit pinned for this comparison:**
`5237495da41d35c03a6405d8db4aa53942f28e0f` (2026-06-02). Recorded 2026-06-20.

Both forks share the same premise: a fork of `morsemicro/halow`, adding host-side IBSS
to morselib on stock MM6108 firmware (both verified on 1.17.6 / 1.17.9), OPEN /
no link-layer encryption. The differences are in maturity vs reference-correctness.

## Where theirs is ahead (these map to our open TODOs)

| Area | momentary-systems (`5237495`) | Ours | Our backlog |
|---|---|---|---|
| Module structure | dedicated `umac/ibss/umac_ibss.c` (+ `.h`) | IBSS inline in `umac.c` | #12 |
| Peer age-out | ✅ `mmwlan_ibss_age_peers(threshold_ms)`, `last_rx_ms` per peer, LRU eviction at the 8-cap | ❌ grow-only, no free | #14 |
| Teardown / re-enable | ✅ `mmwlan_ibss_stop()` (tears down vif; `start` composes over boot STA) | ❌ assumes clean boot | #6 |
| Membership events | ✅ `mmwlan_ibss_register_peer_cb` (ADDED/REMOVED), `mmwlan_ibss_foreach_peer` | ❌ snapshot API only (`mmwlan_ibss_get_peers`) | — |
| Multicast | ✅ IPv4/IPv6 verified (`IP_ADD_MEMBERSHIP`) | basic IP only | — |
| Create/Join role | explicit `create` bool arg | MAC heuristic (bench hack) | #7 |
| S1G beacon header | parses `next_tbtt`/`cssid`/`ano` present flags | minimal | — |

Their `IBSS_ADOPTION.md` is a strong reference: security tradeoff analysis, an
app-layer-crypto recommendation with a perf budget, and honest caveats. `UMAC_IBSS_MAX_PEERS
= 8` (same as ours); per-peer state is RX-only (TX shares one stad/queue) — **identical to
our design**, so no per-peer Block-Ack on TX in either.

## Where ours is ahead — reference-implementation (Linux) correctness

**The decisive difference.** Both forks were tested ESP32↔ESP32 only — *except ours*, which
we validated on-air against the real Linux `morse_driver`/mac80211 IBSS node (P0.5,
2026-06-20). That exposed a divergence in their design:

Their S1G beacon handler `umac_datapath_process_s1g_beacon` (datapath `umac_datapath.c:156`):
```c
if (!umac_connection_addr_matches_bssid(umacd, s1g_header->source_addr)) {
    MMLOG_DBG("Beacon received from another AP.\n");   /* -> drop */
    return;
}
```
It assumes a beacon's `source_addr == BSSID`. That holds for the **ESP32's own
non-standard self-beacon** (the `SA=BSSID` / SW-4741 quirk) but **not** for a standard
802.11 IBSS beacon, where `SA = the beaconing node's own MAC`. The Linux `morse_driver`
emits the standard form — we *measured* chronium's beacon `source_addr = 3c:22:7f…` (the
node MAC, not the BSSID). So their handler would **reject the reference implementation's
beacons as "from another AP,"** i.e. no passive beacon discovery against Linux (it would
fall back to data-frame discovery, which still works).

Our #16 fix takes the opposite, IBSS-correct path: read `source_addr` and **mint a peer
from it**; the BSSID-exclusion drops only our own cell's self-beacon. Result (verified
on-air): the ESP32 discovers the Linux node **passively from its beacon**, 1 clean peer, 0
phantoms, bidirectional ping. This is a direct payoff of our governing rule — derive and
verify against the Linux side — vs their ESP32-only testing.

> Note: their behaviour against Linux is *inferred from their code*, not measured by us.
> Ours against Linux is measured (test plan §5, I.1/I.2/I.3 pass).

## Where they're the same
- **No TSF merge** (#4) — neither auto-merges; both pin the BSSID. (Theirs: explicit
  create/join arg; ours: MAC heuristic.)
- **No link-layer encryption** — both OPEN, both defer crypto to a higher layer (matches
  our Phase-2 software-shim decision). Their fork ships a `supplicant_shim` but does **not**
  wire IBSS-RSN ("plausibly 1000+ lines").
- **Firmware support** — both 1.17.6 + 1.17.9.

## Recommendation
Borrow their **robustness work** — age-out (#14), teardown (#6), membership callbacks, and
the `umac_ibss.c` factoring (#12); their `umac_ibss.c` is a strong template that would
accelerate that backlog. **Keep our Linux-verified beacon discovery** (their `SA=BSSID`
beacon assumption must not be adopted). Cautions: they self-describe the fork as "hackily
forked… AI slop," they modify shared AP/STA files without re-verifying those modes, and the
`mmwlan_ibss_*` symbol names may collide with a future official Morse API.
