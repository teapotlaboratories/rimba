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

> **⚠ Status update (2026-06-20): this comparison drove a decision we then acted on —
> we ADOPTED the momentary-systems implementation** (their `umac_ibss.c` + datapath,
> onto our `esp32-2` base) as the proper EEXIST fix (milestones **H2**). Two claims
> below were later *disproven on hardware* and are corrected inline:
> - "Ours is ahead via beacon discovery" rested on a **wrong measurement** (we thought
>   chronium's beacon carried the node MAC; it carries `SA=BSSID`). Beacon discovery is
>   moot on this hardware — discovery is **data-driven**, so we converged on *their*
>   model (#16). See [`worklog/2026-06-20-ibss-adoption-interop-phantom.md`](worklog/2026-06-20-ibss-adoption-interop-phantom.md).
> - Their datapath had a latent **#17 phantom-flood** bug (IBSS bound to the AP-mode
>   `frames_allowed_pre_association` list) that we inherited and then fixed.

## Where theirs is ahead (these map to our open TODOs)

| Area | momentary-systems (`5237495`) | Ours | Our backlog |
|---|---|---|---|
| Module structure | dedicated `umac/ibss/umac_ibss.c` (+ `.h`) | IBSS inline in `umac.c` | #12 |
| Peer age-out | ✅ `mmwlan_ibss_age_peers(threshold_ms)`, `last_rx_ms` per peer, LRU eviction at the 8-cap | ❌ grow-only, no free | #14 |
| Teardown / re-enable | ✅ `mmwlan_ibss_stop()` (tears down vif; `start` composes over boot STA) | ❌ assumes clean boot | #6 |
| Membership events | ✅ `mmwlan_ibss_register_peer_cb` (ADDED/REMOVED), `mmwlan_ibss_foreach_peer` | ❌ snapshot API only (`mmwlan_ibss_get_peers`) | — |
| Multicast | ✅ IPv4/IPv6 verified (`IP_ADD_MEMBERSHIP`) | basic IP only | — |
| Create/Join role | explicit `create` bool arg | MAC heuristic (provisioned-net role pick; make explicit) | #7 |
| S1G beacon header | parses `next_tbtt`/`cssid`/`ano` present flags | minimal | — |

Their `IBSS_ADOPTION.md` is a strong reference: security tradeoff analysis, an
app-layer-crypto recommendation with a perf budget, and honest caveats. `UMAC_IBSS_MAX_PEERS
= 8` (same as ours); per-peer state is RX-only (TX shares one stad/queue) — **identical to
our design**, so no per-peer Block-Ack on TX in either.

## What our Linux interop testing actually found (corrects the original "ours is ahead")

The original draft claimed our beacon-based discovery was a reference-correctness
advantage over their "drop beacons as another AP" handler. **On-air testing against the
real Linux node disproved this** — and we ended up adopting *their* model:

- **Their handler was effectively right.** `umac_datapath_process_s1g_beacon` dropped
  beacons whose `source_addr` looked like the BSSID. A `morse_driver` node's S1G beacon
  carries **`source_addr = BSSID`** (measured: `sa = 02:12:34:56:78:9a`, the BSSID — *not*
  the node MAC `3c:22:7f…` as the original draft wrongly stated; the morse RX path
  `dot11ah/rx_s1g_to_11n.c: morse_dot11ah_s1g_to_beacon` sets `mgmt->sa = mgmt->bssid`).
  So a morse beacon can't identify the sender at all.
- **Our #16 "mint a peer from `source_addr`" was the wrong move** — it minted the BSSID as
  a junk peer (and the firmware doesn't even surface same-cell *peer* ESP32 beacons to the
  host, so it discovered nothing real). We reverted #16 to **drop** beacons (H5): all
  discovery is **data-driven** (data-frame source addresses) — i.e. *their* design.
- **Net:** beacon-based IBSS discovery does no useful work on this firmware. Linux+morse is
  data-driven for the same reason (`ieee80211_ibss_rx_bss_info` keys `mgmt->sa`, which the
  morse RX path has set to the BSSID).

### Where we *did* add value (post-adoption)
- **#17 phantom-flood fix** — their datapath (and ours, after adoption) bound IBSS to the
  **AP-mode** `frames_allowed_pre_association` list, omitting `S1G_BEACON`; beacons then
  fell to the data-path `dot11_get_ta` mint and read the S1G **timestamp** as a peer →
  flood. We gave IBSS its own list with `S1G_BEACON`. *Their fork still has this latent
  bug* (only surfaces against a `morse_driver` node, which they never tested).
- **Linux interop validation** — I.1–I.3 + I.5 on-air against `morse_driver`/mac80211
  (test plan §5). Both forks were otherwise ESP32↔ESP32 only.
- **P0.6 drop/rejoin** validated using their age-out.

## Where they're the same
- **No TSF merge** — neither auto-merges; both pin the BSSID. (Theirs: explicit
  create/join arg; ours: MAC heuristic.) **This is now Rimba's deliberate design** —
  a provisioned mesh uses an agreed BSSID, so merge is out of scope (#4 closed
  2026-06-20). Their pinned-BSSID choice turned out to match ours.
- **No link-layer encryption** — both OPEN, both defer crypto to a higher layer (matches
  our Phase-2 software-shim decision). Their fork ships a `supplicant_shim` but does **not**
  wire IBSS-RSN ("plausibly 1000+ lines").
- **Firmware support** — both 1.17.6 + 1.17.9.
- **Discovery model** — both data-driven now (we converged on theirs via #16).

## Outcome (what we did)
**Adopted their implementation** (`umac_ibss.c` + datapath, milestones H2) for the proper
EEXIST fix and the robustness work — age-out (#14), teardown (#6), membership callbacks,
module factoring (#12). On top, we **fixed #17** (their latent phantom-flood bug) and
**validated against the Linux reference**. The original "keep our beacon discovery" advice
is **reversed** — beacon discovery is moot on this hardware; we use their data-driven model.
Residual cautions still apply: they self-describe the fork as "hackily forked… AI slop,"
they modify shared AP/STA files without re-verifying those modes, and the `mmwlan_ibss_*`
symbol names may collide with a future official Morse API.
