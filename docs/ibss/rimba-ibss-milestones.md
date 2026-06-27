# RISK-01 — IBSS / ad-hoc on the MM6108: milestones

The single doc for the **802.11ah (Wi-Fi HaLow) IBSS / ad-hoc** L2 on **ESP32-S3 +
Morse Micro MM6108** (`mm-iot-sdk`/morselib, which exposes no public IBSS API): the
milestones, the new-code ↔ Linux comparison, the **fork comparison** (vs
momentary-systems), the **TODO / open items**, and the **findings & decisions**. For
the blow-by-blow (commands, captures, diagnoses) see the worklogs
([`worklog/2026-06-18-risk01-ibss-recon.md`](../worklog/2026-06-18-risk01-ibss-recon.md),
[`…2026-06-20-ibss-adoption-interop-phantom.md`](../worklog/2026-06-20-ibss-adoption-interop-phantom.md))
and the validation results in [`rimba-ibss-test-plan.md`](rimba-ibss-test-plan.md).

**Governing requirement:** the implementation is derived from the **Linux side**
— MorseMicro's `morse_driver` (mac80211) for the firmware command sequence, and
`net/mac80211/ibss.c` (MorseMicro kernel fork) for the IBSS beacon/probe-resp IE
set, merge, and ATIM — not improvised from morselib's AP path.

**Hardware:** up to 3× Seeed XIAO ESP32-S3 + HaLow module (reports
`mm6108-mf16858`, chip `0x0306`, firmware **v1.17.6**) **plus a Raspberry Pi +
MM6108 Linux reference node** (`morse_driver`/mac80211, same silicon — the interop
oracle, see [`reference/rimba-linux-node-setup.md`](../reference/rimba-linux-node-setup.md)); board config
`boards/proto1-fgh100m` (`bcf_fgh100mhaamd`); US 915.5 MHz, 1 MHz BW, S1G channel
27 / op-class 68 (the Linux `iw … ibss join` frequency for ch27 is **5560** — the
5 GHz-model channel that `dot11ah` maps ch27 onto; on-air is still 915.5 MHz).

---

## Milestones

### M1 — Reconnaissance & feasibility ✅
Reverse-engineered the IBSS firmware-command sequence from `morse_driver`
(`ADD_INTERFACE(ADHOC) → SET_CHANNEL → BSSID_SET → BSS_CONFIG → IBSS_CONFIG`) and
located the injection points in the ESP32 morselib. Pinned a coherent version
matrix (vendored firmware is already the forum-proven v1.17.6; `mm-iot-sdk`
pinned to 2.10.4 for parity). Found the two IBSS commands (`IBSS_CONFIG 0x35`,
`BSSID_SET 0x52`) are undeclared in the ESP32 SDK header but present in the chip
firmware.

### M2 — Firmware accepts the IBSS command sequence ✅
Added the ADHOC interface type + the two missing commands to morselib and a
`mmwlan_ibss_enable()` that drives the clean-state sequence. On hardware the chip
**accepts** `ADD_INTERFACE(ADHOC)`, `BSSID_SET`, `BSS_CONFIG`, and recognises
`IBSS_CONFIG` (returns `EEXIST(-17)` — "already created" — which is the exact
error seen on the Morse forum, now understood and handled, not a wrong command).

### M3 — Stable ad-hoc bring-up + host-generated beacon ✅
The board brings up IBSS and runs a host-generated **long S1G beacon** without
crashing (`ibss_status=0`). Beacon contents corrected against
`net/mac80211/ibss.c`: IBSS capability bit, **IBSS Parameter Set element**
(ATIM=0), S1G Capabilities, S1G Operation, no TIM.

### M4 — RF link + sniffer validated ✅
Built a raw-frame sniffer on the second board
(`mmwlan_register_rx_frame_cb`) and validated it against a known-good SoftAP:
frames captured at 915.5 MHz from the AP's BSSID. Established that the chip
surfaces **probe responses** (not beacons) to the host during an active scan, so
discovery requires the node to **answer probe requests**.

### M5 — Genuine IBSS link proven on air ✅
Added an IBSS datapath (mirroring `ieee80211_ibss_rx_probe_req`) that answers
probe requests with a probe response. A second board now detects the ad-hoc node
as a **genuine IBSS**:

```
rimba-ibss   BSSID 02:12:34:56:78:9a   Capability 0x0002 (IBSS set, ESS clear)
RXFRAME flag=PROBE_RSP len=77 freq=915500kHz  A2=A3=02:12:34:56:78:9a
```

This confirms the MM6108 in ADHOC mode on the ESP32 **transmits and receives on
air** and is advertised as a real IBSS — the central risk of RISK-01 is retired.

### M6 — IBSS data path: bidirectional IP, dynamic ARP ✅
Un-stubbed the data plane (single shared peer `stad`, STA-style enqueue/dequeue,
OPEN-plaintext TX, IBSS addressing) and fixed the real blocker: the RX **data**
path resolved the VIF as STA/AP only and dropped `UMAC_INTERFACE_ADHOC` frames
(`umac_datapath.c` "Invalid RX VIF"); added an ADHOC→`MMWLAN_VIF_AP` case. Two
boards then exchange **bidirectional IP over IBSS with dynamic ARP** (no static
entry):

```
RXDATA dst=<self> src=<peer> eth=0x0800   reply from 192.168.13.2: time=17 ms
counts: ok=18  rxdata=36  timeout=0  senderr=0   (~16-17 ms RTT)
```

**The Phase-1 data gate is met** — two battery-class nodes exchange L2/IP frames
with no infrastructure. The literal Rimba EtherType `0x88B5` rides this same path
(it's just a different EtherType than the `0x0800` used here; sending raw `0x88B5`
is an app-level `mmwlan_tx`, not a datapath change).

### RISK-01 — RESOLVED
All five layers (interface/commands, bring-up, beacon, probe-answering, data
path) work end-to-end on two boards, derived from and verified against the Linux
implementation. IBSS is a viable L2 for Rimba on the MM6108; the RISK-01 fallback
(AP-STA) is not required.

---

## Post-RISK-01 — Phase-1 hardening & validation

These extend the proven link toward a robust mesh. Detail:
[`worklog/2026-06-20-ibss-adoption-interop-phantom.md`](../worklog/2026-06-20-ibss-adoption-interop-phantom.md),
plus the **TODO / open items** and **Findings & decisions** sections below, and
[`rimba-ibss-test-plan.md`](rimba-ibss-test-plan.md) (P0/I results).

### H1 — N-node addressing + 3-board bench (P0) ✅
One binary on every board; each derives its IP from its MAC
(`192.168.13.<mac[5]>`) and pings every discovered peer. 3 boards form a full
mesh (P0.1–0.7); per-peer `sta_data`+AID so RX dedup is isolated per sender.

### H2 — Adopt the momentary-systems IBSS implementation (proper EEXIST fix) ✅
Replaced our inline IBSS with the public `momentary-systems/esp-halow-ibss`
implementation (`umac_ibss.c` module + `datapath_ops_ibss`), adopted onto our
`esp32-2` base. This is the **Linux-faithful teardown-first bring-up**: it removes
the active boot interface before `ADD_INTERFACE(ADHOC)` (mirrors
`ieee80211_do_stop` → morse `REMOVE_INTERFACE`), so `IBSS_CONFIG(CREATE)` returns
**0** — the `EEXIST(-17)` is fixed at the root, not tolerated. Brings peer age-out
(#14), teardown (#6), and membership callbacks (#12). Validated on HW: no EEXIST,
pure 2-/3-ESP32 full mesh, Linux interop data 2/2.

### H3 — Drop/rejoin resilience, P0.6 (age-out unblocks survivor rediscovery) ✅
With age-out merged, a survivor now frees a departed peer's record (~30 s) and
re-acquires it as a **fresh record** on return — the survivor-side test that was
structurally impossible before. Mirrors `ieee80211_ibss_sta_expire`.

### H4 — Linux interop in a mixed cell, I.5 (the #17 phantom-flood bug) ✅
3 ESP32 + 1 Linux `morse_driver` node on one cell. First run **failed**: the
adopted datapath bound IBSS to the **AP-mode** `frames_allowed_pre_association`
list, which omits `S1G_BEACON`. So every received S1G beacon fell through to the
RX filter's `dot11_get_ta()` peer-mint — which on an S1G beacon reads the
**`time_stamp`** field (addr2 offset), minting a fresh phantom peer per beacon,
flooding the 8-slot table and evicting real peers (a survivor was starved to 0
replies). **Fix:** give IBSS its own allowed-list including `S1G_BEACON`, so
beacons skip the mint and route to `process_s1g_beacon`. After the fix: **full
all-pairs reachability, 0 phantoms.** This bug was present in the upstream
momentary-systems fork too (same `ap_mode` binding) — surfaced only by our Linux
interop testing.

### H5 — Beacon discovery is data-driven (the #16 correction) ✅
On-air `DBG-SA` probing settled how peers are actually discovered, and **corrected
an earlier wrong belief**: beacon-based discovery does *no* useful work on this
hardware/firmware.
- The **mm6108 firmware (1.17.6) does not surface same-cell peer beacons to the
  host** — a pure 2-ESP32 cell communicates fine but **0** beacons reach
  `process_s1g_beacon`. So ESP32↔ESP32 discovery is entirely data-driven.
- A `morse_driver` node's S1G beacon carries **`source_addr = BSSID`** (not the
  sender's MAC), confirmed both on-air and in the Linux driver
  (`morse_dot11ah_s1g_to_beacon` copies the single S1G address into *both*
  `mgmt->sa` and `mgmt->bssid`). So even surfaced beacons can't identify a peer.

`process_s1g_beacon` now **drops** beacons (no peer minting; it was creating a junk
`BSSID` peer). All real discovery is the **data-frame** path
(`lookup_stad_by_peer_addr_ibss` on a frame's real TA) — which is what Linux+morse
effectively does too (`ieee80211_ibss_rx_bss_info` keys on `mgmt->sa`, but morse
sets `mgmt->sa = bssid`). Marked **morse hardware/firmware dependent** in the
driver; revisit only if a future firmware surfaces real per-node beacons.

### H6 — Phase-1 foundation validation COMPLETE ✅ (2026-06-21)
Full suite passed — multi-node mesh, Linux `morse_driver` interop (same silicon), per-peer
dedup, recovery, and a ~6.5 h 4-node soak (0 reboots, no heap leak, RTT stable). **Per-test
results (P0.1–0.7, I.1–I.5, P1.4/P1.5) and the I.4 caveat are tabulated in the canonical
[`rimba-ibss-test-plan.md`](rimba-ibss-test-plan.md) §1** (don't restate them here). The
open-IBSS foundation is robust and ready for **Phase 2 (link security)**. RISK-02
cold-boot-to-IBSS-joined time has since been **measured ≈1.39 s** (2026-06-21), the gating
number for RTC-scheduled power-save (#9).

---

## Code development overview & Linux comparison

The port was built bottom-up, each layer mapped to (and derived from) its Linux
source and verified on hardware: firmware commands → bring-up sequence → host
beacon → datapath/probe-answering. The MM6108 firmware is the same silicon Linux
drives, so the firmware-command layer matches `morse_driver` directly; the
upper-layer behaviour (beacon IEs, probe answering, addressing, merge) matches
`net/mac80211/ibss.c`.

Path shorthands:
- **port** = `components/halow/components/mm-iot-sdk/framework/morselib/` (+ a few
  superproject files under `firmware/`, `boards/`).
- **kernel** = `MorseMicro/linux` (fork branch `mm/linux-6.12.81/1.17.x`).
- **morse_driver** = `MorseMicro/morse_driver` (out-of-tree mac80211 driver).

| Concern | This port (file : symbol) | Linux equivalent (file : symbol) |
|---|---|---|
| ADHOC interface type + add | port `src/internal/mmdrv.h` : `MMDRV_INTERFACE_TYPE_ADHOC`; `src/driver/driver.c` : `mmdrv_add_if` ADHOC case | morse_driver `mac.c` : `interface_modes` `BIT(ADHOC)`@7211; `command.c` : `morse_cmd_add_if` (`NL80211_IFTYPE_ADHOC`→`MORSE_CMD_INTERFACE_TYPE_ADHOC`) |
| IBSS commands + structs (`IBSS_CONFIG` 0x35, `BSSID_SET` 0x52) | port `src/common/morse_commands.h` : `morse_cmd_req_ibss_config`, `…_bssid_set`; `src/driver/driver.c` : `mmdrv_cfg_ibss`, `mmdrv_set_bssid` | morse_driver `morse_commands.h` : `morse_cmd_req_ibss_config`; `command.c` : `morse_cmd_cfg_ibss`@2228, `morse_cmd_set_bssid` |
| Bring-up sequence + create/join, EEXIST handling | port `src/umac/umac.c` : `mmwlan_ibss_enable`, `umac_ibss_do_start` | kernel `net/mac80211/ibss.c` : `__ieee80211_sta_join_ibss`; morse_driver `mac.c` : `morse_mac_join_ibss`@5308, `bss_info_changed` IBSS branch@4127 |
| Host IBSS beacon (IBSS cap bit, IBSS Param Set, S1G Cap/Op, no TIM, long) | port `src/umac/umac.c` : `umac_ibss_build_beacon`, `umac_ibss_get_beacon`, `umac_ibss_append_s1g_ies`; dispatch `src/umac/umac_mmdrv_shim.c` : `mmdrv_host_get_beacon` | kernel `net/mac80211/ibss.c` : `ieee80211_ibss_build_presp`@38 (IBSS Param Set@133, cap `WLAN_CAPABILITY_IBSS`@475); 11n→S1G conv morse_driver `dot11ah/tx_11n_to_s1g.c` : `morse_dot11ah_beacon_to_s1g`@819 (`EXT\|S1G_BEACON`@831) + `beacon.c` : long-only for ADHOC@382 |
| Probe-request answering | port `src/umac/umac.c` : `umac_ibss_process_rx_mgmt_frame`, `umac_ibss_build_probe_resp`, `datapath_ops_ibss` | kernel `net/mac80211/ibss.c` : `ieee80211_ibss_rx_queued_mgmt`@1584 → `ieee80211_rx_mgmt_probe_req`@1490 |
| IBSS data 802.11 addressing (ToDS=FromDS=0; A1=DA, A2=SA, A3=BSSID) | port `src/umac/umac.c` : `umac_ibss_construct_80211_data_header` | kernel `net/mac80211/tx.c` : `ieee80211_build_hdr` (`NL80211_IFTYPE_ADHOC` case) |
| Beacon-aware discovery (workaround) | port `src/umac/datapath/umac_datapath.c` : `…_process_rx_mgmt_frame_sta` BEACON case | n/a — ESP32 `hw_scan` only surfaces probe responses to the host, so we answer probes (as Linux does) and also feed beacons to the scanner |
| Public API / app entry | port `include/mmwlan.h` : `mmwlan_ibss_enable`, `mmwlan_ibss_args`; app `firmware/rimba-halow-ibss/main/app_main.c` | userspace: `iw dev … ibss join`; `wpa_supplicant` (`mode=1`) |
| IBSS merge (TSF tiebreak) | **intentionally omitted** — Rimba is a *provisioned* mesh (agreed BSSID); merge is for coordinator-free cell formation, not needed (#4 out of scope, 2026-06-20) | kernel `net/mac80211/ibss.c` : `ieee80211_rx_bss_info`@1081 (TSF tiebreak@1160) — used only with a random BSSID |
| Data-plane TX/RX (`0x88B5` + IP) | ✅ working — port `src/umac/datapath/umac_datapath.c` : `umac_datapath_process_rx_data_frame`, `…tx_dequeue_frame_ibss` | kernel `net/mac80211/tx.c` + `rx.c`; morse_driver data path |

### Post-RISK-01 hardening — adoption + #16/#17 (2026-06-20)

After H2, IBSS moved out of `src/umac/umac.c` into the adopted module
`src/umac/ibss/umac_ibss.c` (+ `datapath_ops_ibss` in `umac_datapath.c`). New/changed
mappings from this phase:

| Concern | This port (file : symbol) | Linux equivalent (file : symbol) |
|---|---|---|
| IBSS module + public API (factor-out #12) | `src/umac/ibss/umac_ibss.c` : `mmwlan_ibss_start`/`_stop`, `umac_ibss_get_or_create_peer_stad` | kernel `net/mac80211/ibss.c` : `__ieee80211_sta_join_ibss`, `ieee80211_ibss_add_sta`@581 |
| Teardown-first bring-up (EEXIST fix, H2) | `umac_ibss.c` : `mmwlan_ibss_stop` → `REMOVE_INTERFACE` before `ADD_INTERFACE(ADHOC)` | kernel `net/mac80211/iface.c` : `ieee80211_do_stop`; morse_driver `mac.c` : `morse_mac_ops_remove_interface` |
| Peer age-out / free (#14) | `umac_ibss.c` : `mmwlan_ibss_age_peers(threshold_ms)`, per-peer `last_rx_ms`, LRU at 8-cap | kernel `net/mac80211/ibss.c` : `ieee80211_ibss_sta_expire` (`IEEE80211_IBSS_INACTIVITY_LIMIT` 60 s) |
| Peer discovery (the real path) — **data-driven** | `umac_datapath.c` : `umac_datapath_lookup_stad_by_peer_addr_ibss` (get-or-create on a data frame's real TA) | kernel `net/mac80211/ibss.c` : `sta_info_get(mgmt->sa)` → `ieee80211_ibss_add_sta`; `rx.c` sta lookup |
| Beacon RX handler — **drops, no peer mint (#16)** | `umac_datapath.c` : `umac_datapath_process_s1g_beacon` | kernel `net/mac80211/ibss.c` : `ieee80211_ibss_rx_bss_info`@968 (keys `mgmt->sa`) — see "morse beacon SA=BSSID" below |
| IBSS allowed-pre-assoc list (**#17 phantom-flood fix**) | `umac_datapath.c` : `frames_allowed_pre_association_ibss[]` (incl. `S1G_BEACON`), bound on `datapath_ops_ibss` | kernel design: beacons route to `ieee80211_ibss_rx_bss_info`, never minted as "unknown sender" via the data path; cf. `frames_allowed_pre_association_sta_mode` which also lists `S1G_BEACON` |
| morse S1G beacon **SA=BSSID** (why beacon discovery is moot) | firmware (mm6108.mbin 1.17.6) lower-MAC TX rewrite + non-surfacing of peer beacons | morse_driver `dot11ah/rx_s1g_to_11n.c` : `morse_dot11ah_s1g_to_beacon` (sets `mgmt->sa = mgmt->bssid = s1g.sa`); TX `dot11ah/tx_11n_to_s1g.c` : `morse_dot11ah_beacon_to_s1g` (SW-4741) |

### Key divergences from Linux (and why)
1. **S1G beacon conversion location.** On Linux the 11n→S1G beacon conversion is
   *host-side* (`morse_driver/dot11ah/tx_11n_to_s1g.c`). On ESP32 the **chip
   firmware** performs it (proven by the working SoftAP). So we feed an
   11n-form PV0 beacon body (incl. the S1G Cap/Op IEs, as hostapd does for AP)
   and rely on the chip to emit the S1G `EXT/S1G_BEACON` per interface type.
2. **State-machine scope.** Linux mac80211 runs the full IBSS state machine
   (merge, TSF adoption, ATIM, per-peer `sta_info`). This port implements the
   bring-up, beacon, probe-answering, **and data plane** (all proven). **TSF merge
   is intentionally omitted** — Rimba is a *provisioned* network with an agreed
   BSSID, so coordinator-free cell coalescing isn't needed (#4 out of scope, decided
   2026-06-20; see the hardening-todo decision note). ATIM is also omitted (window
   0 = always awake; per `ibss.c` it is power-save-only).
3. **No association objects.** IBSS has no association, so the datapath `stad`
   lookups return `NULL` and frames are admitted via
   `frames_allowed_pre_association`. **The adopted code reused the AP-mode list
   here, which omits `S1G_BEACON` — that was the #17 phantom-flood bug** (a beacon
   isn't "allowed," so it fell to the data-path `dot11_get_ta` mint and read the
   beacon timestamp as a transmitter). Fixed by an IBSS-specific list that allows
   `S1G_BEACON` (like STA mode). Linux never has this problem: a beacon goes to
   `ieee80211_ibss_rx_bss_info`, not the unknown-sender data path.
4. **Discovery is data-driven, not beacon-based** (H5/#16). On this firmware,
   peer beacons aren't surfaced to the host and morse beacons carry `SA=BSSID`,
   so peers are learned from data-frame source addresses. This matches Linux+morse
   in practice (`ieee80211_ibss_rx_bss_info` keys `mgmt->sa`, but the morse RX
   path sets `mgmt->sa = bssid`). Morse hardware/firmware dependent.

---

## Implementation comparison — vs the `momentary-systems` fork

The other public ESP32/MM6108 IBSS fork is **`momentary-systems/esp-halow-ibss`**
(branch `ibss-support`, commit `5237495` 2026-06-02; from the Morse community thread
[*Firmware support for IBSS/ad-hoc on FGH100M*](https://community.morsemicro.com/t/firmware-support-for-ibss-ad-hoc-on-fgh100m/1653/5)).
Both fork `morsemicro/halow` and add host-side IBSS on stock fw, OPEN/no crypto.
**We adopted their implementation** (`umac_ibss.c` + datapath, milestones **H2**) as
the proper EEXIST fix + the robustness work, then fixed their latent #17 bug and
validated against the Linux reference.

Where theirs was ahead (these became our backlog, mostly now done via adoption):

| Area | momentary-systems (`5237495`) | Ours (pre-adoption) | Item |
|---|---|---|---|
| Module structure | dedicated `umac/ibss/umac_ibss.c` | inline in `umac.c` | #12 ☑ |
| Peer age-out | `mmwlan_ibss_age_peers`, LRU at 8-cap | grow-only | #14 ☑ |
| Teardown / re-enable | `mmwlan_ibss_stop()` | assumed clean boot | #6 ◐ |
| Membership events | ADDED/REMOVED cb + `foreach_peer` | snapshot API only | — |
| Multicast | IPv4/IPv6 verified | basic IP | — |
| Create/Join role | explicit `create` arg | MAC heuristic | #7 ☐ |

What our Linux-interop testing changed: the original "our beacon discovery is ahead"
claim was **wrong** (we thought chronium's beacon carried the node MAC; it carries
`SA=BSSID`), so beacon discovery is moot — we converged on **their data-driven model**
(#16 reverted to drop, H5). Where *we* added value: the **#17 phantom-flood fix** (their
latent bug — IBSS bound to the AP-mode pre-assoc list omitting `S1G_BEACON`; *their fork
still has it*), Linux interop validation (I.1–I.5), and P0.6 drop/rejoin via their age-out.
Same in both: no TSF merge (both pin the BSSID), no link-layer crypto (both OPEN),
data-driven discovery, `MAX_PEERS = 8`, TX shares one stad/queue. Residual cautions on
the adopted code: they self-describe it as "hackily forked… AI slop," modify shared
AP/STA files without re-verifying those modes, and `mmwlan_ibss_*` may collide with a
future official Morse API.

---

## TODO / open items

The single IBSS backlog. ☐ todo · ◐ in progress. Done items are the milestones above.
**Governing rule:** derive from Linux (`net/mac80211/ibss.c`, `morse_driver`).

**Protocol-critical**
- ◐ **#3 Hop-by-hop CCMP (AES-128-CCM).** Move off OPEN. **Blocked at the firmware
  boundary:** IBSS has no association → no AID to bind a pairwise key to, and
  `SET_STA_STATE(peer_mac, aid, AUTHORIZED)` **returns -116 on the ADHOC interface**, so
  the firmware won't mint a station handle for an IBSS peer (details in Findings → CCMP).
  Next: understand the -116 (off the RX hot path); check `morse_driver`'s
  `morse_op_sta_state` adhoc sequence. Then static-PSK-PTK or full IBSS-RSN (`ibss_rsn.c`).

**Robustness / correctness**
- ☐ **#5 Beacon contention.** Distributed beaconing (suppress own beacon if one was heard
  this interval). Every node currently beacons unconditionally — fine for 2, a storm risk
  as density grows.
- ◐ **#6 Teardown / re-enable.** `mmwlan_ibss_stop()` exists (adoption); **verify**
  stop→re-enable and runtime param/channel change (not yet tested).
- ☐ **#7 Dynamic create-else-join** (`ieee80211_sta_find_ibss`). Role must not be
  provisioned — decide at boot by **active scan against the agreed BSSID**: probe → got a
  response → JOIN; silence → CREATE. Replaces the `mac[0]&0x80` bench heuristic; also the
  vehicle for TSF sync (JOIN syncs to the cell). Fix the latent
  `umac_connection_addr_matches_bssid` (connection BSSID not set in IBSS) here.
- ☐ **#13 Audit STA/AP-only assumptions** in morselib for other ADHOC drops (the RX-VIF
  bug was one).
- ☐ **#20 Drop/rejoin survivor-side rediscovery gap** (found in the 2026-06-23 regression/stress
  test). In a mixed 4-node cell (3 ESP32 + chronium), dropping then rejoining one ESP32: the
  rejoined node fully recovers (pings all peers, 0 timeouts) and survivors still **serve** its
  traffic (reply to its pings), but the survivors did **not re-add it to their own active-ping
  set** within ~90 s — i.e. survivor→returned-peer discovery didn't re-fire even though the RX
  path sees the peer's frames. P0.6 claimed survivor re-acquisition works; re-confirm and fix
  (likely the get-or-add-on-RX path vs the app ping-list). See
  [`worklog/2026-06-23-regression-stress-test.md`](../worklog/2026-06-23-regression-stress-test.md).

**Power & de-risking** *(early focus — Rimba's battery-leaf model rests on it)*
- ☐ **#9 RISK-02 — measure radio cold-boot-to-IBSS-joined time (GATING, NEXT).** The number
  that decides whether RTC-scheduled mode is viable. GPIO-toggle from MM6108 power-on →
  firmware loaded → IBSS joined → **first frame exchanged**; also measure a *resume* path.
  Dev plan targets `LEAF_BOOT_MS=30`; never measured (RISK-02 has since been measured ≈1.39 s,
  2026-06-21 — confirm/refine).
- ☐ **#8 RTC-scheduled radio power-cycling = "Scheduled mode".** Drive the duty cycle from
  ESP32 + the precise RTC (RV-3028-C7, ≤1 ppm): alarm wakes ESP32 → power MM6108 on → join
  the pinned cell → exchange → power radio off → deep-sleep. Bypasses the missing chip
  IBSS-PS *and* TSF sync (RTC is the shared clock). Pins (`boards/proto1-fgh100m`):
  `RESET_N`=GPIO1, `WAKE`=GPIO2, `BUSY`=GPIO5, `SPI_IRQ`=GPIO3. Depends on #7 + #9.
- ☐ **#18 TSF sync — DEMOTED** to nice-to-have (the RTC is the schedule clock, not the radio
  TSF). Keep only for fine intra-window timing. Experiment: expose `GET_TSF`; do same-BSSID
  ESP32s converge; does JOIN sync while CREATE starts fresh.
- ☐ **#10 >2-node test** beyond the 3-board bench (needs more boards) — beacons/discovery/data.
- ☐ **#11 Verify the S1G beacon on-wire (I.4).** Only the probe-resp was decoded; the
  `EXT/S1G_BEACON` framing is chip-side + unverified. **Root cause of the failed capture:**
  chronium's `morse_driver` is built **without `CONFIG_MORSE_MONITOR`** (the `morse0` monitor
  netdev is `#ifdef`'d out — a build flag, not hardware). Close via a rebuild with that flag,
  or the ESP32 raw-frame hook (`mmwlan_register_rx_frame_cb` + `MMWLAN_FRAME_BEACON`). Open
  Q: our ESP32 beacon's `source_addr` (MAC vs BSSID).
- ☐ **#20 Beacon RX source-address: re-open the "firmware wall" (surfaced by the mesh P2
  work, 2026-06-25).** H5 concluded "the firmware does not surface same-cell peer beacons"
  from **0 beacons reaching `process_s1g_beacon`**. But the mesh P2 work found foreign
  beacons **do** reach the host — as **legacy MGMT/Beacon frames at
  `umac_datapath_rx_frame_filter`** (FC `0x80`, `A2/A3 = 00:00`), a path that bypasses
  `process_s1g_beacon` entirely. So beacons *are* surfaced; H5's "0 surfaced" was measured
  only at the S1G/EXT handler and **missed the legacy path** — worth re-instrumenting the
  IBSS case the same way (tap `rx_frame_filter`, not just `process_s1g_beacon`). **Why it
  matters more for mesh than IBSS:** the S1G beacon carries a single address = the BSSID; for
  IBSS that's the *shared* cell BSSID (useless for per-node ID → data-driven discovery is
  correct regardless), but for **mesh** the BSSID = the node's *own* MAC, so that single
  address IS the peer's identity. **Open questions:** (a) is the address truly stripped by the
  firmware before morselib, or is it recoverable (e.g. could the mesh vif be made to receive
  raw `EXT/S1G_BEACON` so `process_s1g_beacon` reads `source_addr`)? (b) does a Linux mesh
  node (morse_driver `s1g_to_beacon` → `sa = bssid = source_addr`) see the real per-node MAC,
  confirming the address is on-air? Verify by rebuilding `morse_driver` with
  `CONFIG_MORSE_MONITOR` (see #11) and capturing a mesh beacon's addresses. If recoverable,
  normal beacon-based mesh discovery works and provisioned peer MACs aren't needed. The old
  `i4-beacon-source-addr-firmware-wall` worklog (referenced but never created) called this a
  firmware wall — this re-opens it. See `docs/worklog/2026-06-25-mesh-p1-vif-beacon.md`.
  - **UPDATE (2026-06-25): the mesh half of this is RESOLVED — and it was a host bug, not a
    firmware wall.** The mesh vif built beacons with `mesh_mac = 00:00` because the bring-up
    used `umac_interface_get_vif_mac_addr()` (STA/AP-roles only → returns zero for mesh).
    Fixed to use the vif's real `if_addr` / `get_device_mac_addr()`. **Verified on Linux**:
    rebuilt chronium's `morse_driver` with `CONFIG_MORSE_MONITOR=y` and captured on `morse0` —
    after the fix the mesh beacons carry the real per-node MAC (`SA = BSSID = e2:72:…`); before
    the fix Linux saw `00:00` too (confirming we were *transmitting* zeros). **For IBSS this is
    DIFFERENT and likely fine:** IBSS builds beacons from `args->if_addr` (not the broken
    getter), and the beacon source is the *shared cell BSSID* by design — so IBSS's
    data-driven discovery is correct regardless. **Still worth doing for IBSS:** (a) confirm
    the IBSS beacon's on-air SA with the now-working `morse0` monitor (is it the real
    per-node MAC or the shared BSSID?); (b) re-check the "0 beacons surfaced" claim by tapping
    `rx_frame_filter` (legacy path), not just `process_s1g_beacon`.
- ☐ **#21 Verify IBSS on-air behavior with the working `morse0` monitor.** chronium's
  `morse_driver` is now rebuilt with `CONFIG_MORSE_MONITOR=y`, so the `morse0` raw-monitor
  netdev finally delivers S1G frames (this was the blocker behind #11 and the I.4 gap). With an
  IBSS cell running on ch27, capture on `morse0` and confirm, on-wire:
  1. **Beacon source address** — is the IBSS beacon's SA the **shared cell BSSID**
     (`02:12:34:56:78:9a`, as H5 claims) or a real per-node MAC? Settles whether IBSS has any
     residual of the mesh `00:00` bug (it shouldn't — IBSS builds from `args->if_addr`).
  2. **"0 beacons surfaced" recheck** — H5 measured this only at `process_s1g_beacon`; the mesh
     work showed beacons also arrive as **legacy MGMT frames at `rx_frame_filter`**. Confirm
     whether same-cell IBSS peer beacons reach the host at all (legacy path included), which
     would refine the "firmware doesn't surface peer beacons" finding.
  3. **Frame formats** — decode the `EXT/S1G_BEACON` framing and the probe-resp on-wire (the
     original I.4 goal, #11) now that capture works.
  Does **not** change the IBSS data-driven-discovery decision (source = shared BSSID is correct
  regardless) — it's verification + closing #11/I.4. Reference sniffer setup:
  `docs/worklog/2026-06-25-mesh-p1-vif-beacon.md`; build cmd in
  `reference/rimba-linux-node-setup.md` + `CONFIG_MORSE_MONITOR=y`.

**Code quality / maintenance**
- ☐ **#15 Bump the ESP32 stack** (fw / SDK / IDF) from MM6108 fw **1.17.6**, `morsemicro/halow`
  **`2.10.4-esp32-2`**, ESP-IDF **v5.4.2**. *Not a fast-forward:* the IBSS port patches morselib
  (ADHOC iface, IBSS commands, beacon/probe-resp, RX-VIF fix) → re-apply + re-validate; keep
  **generation parity with the chronium Linux node** (a one-sided bump invalidates interop);
  keep cmake on 3.x; re-run the 3-board P0 bench + AP-STA ping after any bump.
- ☐ **#19 Re-audit the adopted `momentary-systems` fork** (we adopted it at H2 — see the
  "Implementation comparison" section above). Three checks:
  - **Missing features:** re-diff against their current `ibss-support` branch — is anything
    still in their repo that we haven't adopted? (the original gap table is mostly closed,
    but they may have moved on since `5237495`.)
  - **Linux fidelity:** does *their* implementation actually follow Morse's Linux
    `morse_driver` + `net/mac80211/ibss.c`? We adopted it for the EEXIST fix, but they
    self-describe it as "hackily forked… AI slop" and modify shared AP/STA files without
    re-verifying those modes — audit the adopted paths against the reference (governing rule).
  - **Test:** validate any adopted / changed behaviour on hardware — 3-board P0 bench +
    Linux interop — per the verify rule.
- ☐ **Regression suite** across every built feature (hello / scan / AP-STA / IBSS / TWT /
  Mesh+AP) so firmware/morselib bumps don't silently regress earlier milestones.

---

## Findings & decisions

Conclusions that shaped the implementation. Blow-by-blow in the worklogs
([`…2026-06-20-ibss-adoption-interop-phantom.md`](../worklog/2026-06-20-ibss-adoption-interop-phantom.md),
[`…2026-06-21-mm6108-powersave-decompile.md`](../worklog/2026-06-21-mm6108-powersave-decompile.md)).

**EEXIST(-17) on `IBSS_CONFIG(CREATE)` — root cause (2026-06-20).** Not firmware/command
bytes (the `mm6108.mbin` + the `mmdrv_*` command code are byte-identical to the
momentary-systems fork). **Root cause = a divergence from the Linux flow:** Linux *removes*
the active interface before switching to IBSS (`iw set type ibss` needs `ip link down` →
`ieee80211_do_stop` → `morse_mac_ops_remove_interface` → `MORSE_CMD_ID_REMOVE_INTERFACE`).
Our bring-up did `ADD_INTERFACE(ADHOC)` on top of the live boot vif, so the stale BSS context
made `IBSS_CONFIG(CREATE)` report already-exists. **Why we tolerated it for a while:** porting
*only* the teardown killed the EEXIST but **regressed the data path** (the boot vif's
netif/datapath binding is lost when `active_interface_types` hits 0 and `mmdrv_rm_if` fires);
even teardown + RX re-bind left ARP/ICMP broken. **Proper fix = adopt the momentary-systems
integrated bring-up** (H2), which re-establishes the full post-boot data path holistically —
proven on HW (no EEXIST *and* working data path).

**DECISION — Rimba is a provisioned network → no IBSS merge (#4 closed, 2026-06-20).** Every
node is deployed knowing the mesh's BSSID (provisioned, like Wi-Fi credentials), so all nodes
share one agreed cell. TSF merge (`ieee80211_rx_bss_info`, higher-TSF wins) only exists to
coalesce *uncoordinated* nodes that each rolled a random BSSID — moot with a pre-shared BSSID.
Matches the momentary-systems choice and Linux pointed at a fixed BSSID. Reversal trigger: a
deployment that must form cells with no pre-agreed BSSID (not anticipated). What remains is #7
(dynamic create-else-join), not merge.

**DECISION — role is dynamic, and power-save is an early focus via RTC (2026-06-20).** Field
relays are equal and can't know who's "first," so the create/join role is decided at boot by
active scan against the agreed BSSID (#7), not per-node config. Power-save is pulled early but
driven by **ESP32 + the precise RTC**, not chip power-save — because:

**FINDING — the morse driver has no IBSS radio power-save; TWT is STA/AP-only (2026-06-20).**
The driver has a rich PS stack (`ps.c`, `twt.c` = 802.11ah TWT, `yaps.c`) but **every TWT path
gates on `IFTYPE_STATION`/`IFTYPE_AP` — no ADHOC branch, no ATIM, no IBSS on-air PS**. Since the
driver mirrors firmware features, the firmware almost certainly has no IBSS radio power-save. So
Scheduled mode hard power-cycles the radio on the RTC schedule (#8); the RTC is the shared clock,
which **demotes #18 (TSF sync)** and bypasses the missing chip PS. (TWT *does* work in AP-STA —
held in reserve, and the basis for the **Mesh-gate** alternative, see
[`rimba-mesh-ap-milestones.md`](../mesh-ap/rimba-mesh-ap-milestones.md).) The gating number is #9 (boot time).

**FINDING — beacon discovery does no real work; discovery is data-driven (2026-06-20, H5).**
On-air `DBG-SA` probing showed (a) the mm6108 fw (1.17.6) **does not surface same-cell peer
beacons to the host** (0 beacons reached the handler in a pure 2-ESP32 cell), and (b) a
`morse_driver` beacon carries `source_addr = BSSID` (the morse RX path sets `mgmt->sa = bssid`),
so even surfaced beacons can't identify a peer. `process_s1g_beacon` now **drops** beacons;
discovery is the data-frame path (`lookup_stad_by_peer_addr_ibss` on the real TA) — which is what
Linux+morse effectively does too. Marked morse hardware/firmware dependent.

**FINDING — CCMP is blocked on the chip key model (2026-06-19, #3).** HW CCMP RX uses the
*pairwise* key for unicast, keyed by `aid`; IBSS has no association → no AID. Linux does IBSS
CCMP via a `sta_info` per peer (keyed by MAC) + a per-pair 4-way handshake (higher-MAC =
authenticator, `ibss_rsn.c`), installing the PTK against the peer's MAC; the driver maps
`sta_info` → a HW station handle (the `aid`). morselib has the primitives (`SET_STA_STATE` 0x14,
`INSTALL_KEY` 0x0A) but drives them only from AP association — and `SET_STA_STATE` **returns -116
on ADHOC**. So per-peer CCMP is blocked at that firmware boundary (#3) until the -116 is
understood. Host-side per-peer records + AID already exist (#2).

**Other:** beacon interval = **100 TU**, matches the Linux node (`beacon_int=100`); Linux IBSS
always emits *long* beacons (`beacon.c:388`, `if (type==ADHOC) short_beacon=false`) and our
`umac_ibss` builder does the same.

---

## Build / test

```bash
make build APP=rimba-halow-ibss BOARD=proto1-fgh100m
# one binary on every node — flash 1..3 boards:
for p in 0 1 2; do make flash APP=rimba-halow-ibss BOARD=proto1-fgh100m PORT=/dev/ttyACM$p; done
make monitor APP=rimba-halow-ibss PORT=/dev/ttyACM0      # "reply from 192.168.13.N … IBSS DATA OK"
```

Linux interop (the 4th node) — Raspberry Pi + MM6108, `morse_driver`/mac80211; bring-up
in [`reference/rimba-linux-node-setup.md`](../reference/rimba-linux-node-setup.md) §12 (IBSS interop). Join the
same pinned cell with **frequency 5560** (S1G ch27 in the 5 GHz model; on-air 915.5 MHz):

```bash
sudo iw dev wlan1 ibss join rimba-ibss 5560 fixed-freq 02:12:34:56:78:9a
sudo ip addr add 192.168.13.66/24 dev wlan1     # octet from the node MAC, like the ESP32s
```

> Bench note: XIAO USB-Serial-JTAG re-enumerates on reset and a beaconing board
> resists esptool reset; for non-interactive capture, flash/reset then wait ~2–3 s
> before opening a continuous `cat` on the port. Return idle boards to the
> radio-free `rimba-hello` between tests.
