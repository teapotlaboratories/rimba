# Worklog — 2026-06-20: IBSS adoption, Linux interop, and the beacon phantom-flood

Covers the day's arc: adopting the momentary-systems IBSS implementation (proper
EEXIST fix) → validating drop/rejoin (P0.6) → mixed-cell Linux interop (I.5) →
root-causing and fixing the **#17 beacon phantom-flood** → correcting our beacon-
discovery understanding (**#16**, now data-driven). All of it landed on `main`.

Companion docs updated the same day:
[`rimba-ibss-milestones.md`](../rimba-ibss-milestones.md) (H1–H5 + the Linux-
equivalence table), [`rimba-ibss-hardening-todo.md`](../rimba-ibss-hardening-todo.md)
(Findings + #16/#17), [`rimba-ibss-test-plan.md`](../rimba-ibss-test-plan.md) (P0.6,
I.5), [`rimba-ibss-impl-comparison.md`](../rimba-ibss-impl-comparison.md) (corrected).

---

## 1. Adoption — proper EEXIST fix (H2)

Our inline IBSS bring-up got `IBSS_CONFIG(CREATE)` → **EEXIST(-17)** because it
skipped the Linux teardown step (morselib de-inits the whole driver on last-
interface-remove, so the lone boot vif made the firmware report "already created").
Rather than tolerate it, we **adopted the public `momentary-systems/esp-halow-ibss`
implementation** — `umac/ibss/umac_ibss.c` (+ `datapath_ops_ibss`) — onto our
`esp32-2` base. It does the Linux-faithful **teardown-first** bring-up
(`REMOVE_INTERFACE` before `ADD_INTERFACE(ADHOC)`, mirroring `ieee80211_do_stop` →
morse `morse_mac_ops_remove_interface`), so `IBSS_CONFIG` returns 0. Free wins:
peer age-out (#14), teardown (#6), membership callbacks (#12), module factor-out
(#12).

Verified: no EEXIST; pure 2-ESP32 and **3-ESP32 full mesh** (3/3 bidirectional);
Linux interop data 2/2.

## 2. P0.6 drop/rejoin — age-out unblocks survivor rediscovery (H3)

Age-out made testable a case that was structurally impossible before (records
never expired): drop a node, survivors free it (~30 s) and re-acquire it as a
**fresh record** on return. Added `peer_cb` membership logging to the app (the
layer's own add/age-out logs are `MMLOG_INF`, compiled out at the default `ERR`
level). Result: both survivors logged `PEER_REMOVED` then `PEER_ADDED` for the
dropped node, link unaffected, bidirectional data restored. Mirrors
`ieee80211_ibss_sta_expire`. **P0.6 → ☑.**

## 3. Mixed-cell Linux interop (I.5) — the #17 phantom flood

Brought the Raspberry Pi MM6108 (`wlan1`, `morse_driver`) into the cell. Two
setup notes worth recording:
- `iw … ibss join` for S1G **ch27** needs frequency **5560** (the 5 GHz-model
  channel `dot11ah` maps ch27 onto — `s1g_channels_rules.c: CHANS1GHZ(27,…,112)`,
  5 GHz ch112 = 5560 MHz). On-air is still 915.5 MHz. `915500` is rejected.
- `morse_cli` needs `sudo` (CAP_NET_ADMIN); non-login SSH drops `/usr/sbin`, so
  `iw` needs an explicit PATH.

**First I.5 run FAILED.** The ESP32s flooded with **hundreds of phantom peers**
(`48:xx:xx:xx:00:d5`), evicting real peers — ACM0 was starved to **0 replies**.
chronium → ESP32 worked fine (4/4, clean `station dump`); only ESP32 ← chronium
*beacons* was broken. This corrected an earlier mischaracterisation (we had called
the mixed-cell degradation "airtime contention, not a blocker" — wrong).

### Root cause (pinned by on-air probe)

Instrumented the RX-filter mint site and captured chronium's beacon:
```
vts=0x001c ta=48:93:01:00:00:d5 fc=1c00 a1=02123456789a o10=4893010000d5
```
- `vts=0x1c` **is** `S1G_BEACON`; `a1` (source_addr @ offset 4) = the **BSSID**;
  `ta = dot11_get_ta` = the **time_stamp** @ offset 10 → the phantom.
- It fired *inside the filter's mint block*, i.e. `rx_frame_allowed_pre_association`
  returned **false for a beacon**.

The adopted `datapath_ops_ibss` reused the **AP-mode**
`frames_allowed_pre_association` list (`{PROBE_REQ, AUTH, ASSOC_REQ}`), which omits
`S1G_BEACON` (an AP never receives beacons from clients). So beacons weren't
"allowed," fell to the data-path `dot11_get_ta` mint, and that helper — written for
normal frames where addr2 is at offset 10 — read the S1G beacon's timestamp instead.
(This is the same class as #16, in a second code location. #16 fixed the *handler*;
the *filter* runs first.) **The bug was present in the upstream momentary-systems
fork too** — same `ap_mode` binding — but never surfaced there because they only
tested ESP32↔ESP32 and never against a `morse_driver` node.

### Fix (#17)

Give IBSS its own allowed-list including `S1G_BEACON` (like STA mode — IBSS has no
association). Beacons now skip the mint and route to `process_s1g_beacon`.

```c
static const uint16_t frames_allowed_pre_association_ibss[] = {
    DOT11_VER_TYPE_SUBTYPE(0, EXT,  S1G_BEACON),   // the fix
    DOT11_VER_TYPE_SUBTYPE(0, MGMT, PROBE_RSP),
    DOT11_VER_TYPE_SUBTYPE(0, MGMT, ACTION),
    UINT16_MAX,
};
```

After the fix: **full all-pairs reachability in the mixed 4-node cell, 0 phantoms.**
**I.5 → ☑.** Linux equivalent: a beacon goes to `net/mac80211/ibss.c`
`ieee80211_ibss_rx_bss_info`, never the unknown-sender data path.

## 4. Beacon discovery is data-driven — the #16 correction (H5)

Followed up with a `DBG-SA` probe in `process_s1g_beacon` (dump each received
beacon's `source_addr`) on a **pure 2-ESP32** cell, then with chronium up:

| | DBG-SA result |
|---|---|
| pure 2-ESP32 | **0** beacons reach `process_s1g_beacon` (cell still forms over data) |
| + chronium | 113 beacons, **all** `sa = 02:12:34:56:78:9a` (BSSID), `==BSSID? 0` |

Two findings:
1. **The mm6108 firmware (1.17.6) does not surface same-cell peer beacons to the
   host.** A firmware/lower-MAC decision, upstream of morselib — the driver never
   sees them. So ESP32↔ESP32 discovery is 100 % data-driven.
2. **morse beacons carry `source_addr = BSSID`**, not the sender MAC. Confirmed in
   the Linux driver too: `dot11ah/rx_s1g_to_11n.c: morse_dot11ah_s1g_to_beacon`
   copies the single S1G address into **both** `mgmt->sa` and `mgmt->bssid`. So
   Linux+morse can't identify a peer from a beacon either — it's data-driven there
   as well (`ieee80211_ibss_rx_bss_info` keys `mgmt->sa`, which = the BSSID).

Also corrected a wrong belief: `umac_connection_addr_matches_bssid()` returns
**false** for our own BSSID (the connection's BSSID field isn't set to the cell
BSSID in IBSS — a latent sub-bug), so #16 had been **minting the BSSID as a junk
peer** (→ IP `.154`), not "dropping it." 

**Change:** `process_s1g_beacon` now **drops** beacons (no peer minting), with a
comment marking it **morse hardware/firmware dependent (mm6108.mbin 1.17.6)** and a
pointer to revisit only if a future firmware surfaces real per-node beacons. This
converges our design with the momentary-systems (data-driven) model. Re-verified
on the full 4-node cell: full reachability, 0 phantoms, **no junk `.154` peer**.

## 5. Firmware vs driver (who owns the behaviour)

- **Peer beacons not surfaced** → the closed **firmware** (`mm6108.mbin`), not the
  driver. morselib only acts on what the firmware forwards.
- **SA=BSSID rewrite** → firmware on the ESP32; on Linux it's the `morse_driver`
  (`morse_dot11ah_beacon_to_s1g`, SW-4741).

So a future firmware could change both; the host-side driver can't.

---

## File changes (this session)

Submodule **`mm-esp32-halow`** (`components/halow`):
- `…/umac/datapath/umac_datapath.c`
  - **#17:** added `frames_allowed_pre_association_ibss[]` (with `S1G_BEACON`);
    bound it on `datapath_ops_ibss` (was `frames_allowed_pre_association_ap_mode`).
  - **#16:** `umac_datapath_process_s1g_beacon` no longer mints peers from beacons
    (drops them) — with the morse hardware/firmware-dependency comment.
- (H2 adoption: `umac/ibss/umac_ibss.c` (+ `.h`), `datapath_ops_ibss`, interface
  plumbing — landed earlier in the session.)

Superproject **`rimba`**:
- `firmware/rimba-halow-ibss/main/app_main.c` — `peer_cb` membership logging (P0.6
  observability).
- `docs/` — milestones, hardening-todo, test-plan, impl-comparison, this worklog.

## Linux-equivalence quick map (see milestones table for the full set)

| This port | Linux (`net/mac80211/ibss.c` unless noted) |
|---|---|
| `umac_ibss.c: mmwlan_ibss_stop` (teardown-first) | `iface.c: ieee80211_do_stop`; morse `mac.c: morse_mac_ops_remove_interface` |
| `umac_ibss.c: mmwlan_ibss_age_peers` | `ieee80211_ibss_sta_expire` |
| `lookup_stad_by_peer_addr_ibss` (data-driven discovery) | `sta_info_get(mgmt->sa)` → `ieee80211_ibss_add_sta` |
| `process_s1g_beacon` (drops; #16) | `ieee80211_ibss_rx_bss_info` (keys `mgmt->sa`) |
| `frames_allowed_pre_association_ibss[]` (#17) | beacons route to `ieee80211_ibss_rx_bss_info`, not the data path |
| morse `SA=BSSID` (firmware) | `dot11ah/rx_s1g_to_11n.c: morse_dot11ah_s1g_to_beacon` |

---

## Git / PR state (end of session)

Both PRs **MERGED** to `main`:
- `mm-esp32-halow` **#2** (adoption), **#3** (#17 fix + #16 simplification) →
  submodule `main` @ `d817582`.
- `rimba` **#3** (adoption), **#4** (#17 + #16 + P0.6/docs) → `main` @ `dcfb9fc`,
  gitlink → `d817582` (verified consistent).
- Branches cleaned up (local + remote). Commits attributed to the owner only.

## Open threads (tracked in the backlog)
- `umac_connection_addr_matches_bssid` doesn't recognise the IBSS cell BSSID
  (latent; moot now that beacons don't mint peers).
- #4 IBSS merge / TSF — would tighten the remaining Linux-fidelity gaps (real
  BSSID/SA handling, scan→join-else-create).
- P0.4 per-peer dedup — **done 2026-06-20** (forced cross-peer probe: 208 cross-peer
  seq collisions correctly accepted, 9 genuine dedup drops; see test plan §4).
