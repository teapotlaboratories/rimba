# RISK-01 IBSS — hardening to-do log

Running backlog to take the IBSS implementation from "two boards exchange IP"
(RISK-01 resolved, see [`rimba-ibss-milestones.md`](rimba-ibss-milestones.md)) to
something robust enough for the Rimba mesh. **Governing rule still applies: derive
behaviour from the Linux side** (`net/mac80211/ibss.c`, `morse_driver`) — see
memory `ibss-base-on-linux-implementation`.

Status: ☐ todo · ◐ in progress · ☑ done. Update this log as items move.

## Known limitations from the bring-up (the starting point)
The working implementation deliberately took shortcuts to prove the link:
- **Single shared peer `stad`** for the whole cell — RX dedup/sequence tracking
  is one space shared across all peers; correct for 2 nodes, wrong for >2.
- **No IBSS merge** — agreed (pinned) BSSID, role is a MAC heuristic in the app; no
  TSF-based merge. *Now a deliberate design choice, not a shortcut* — Rimba is a
  provisioned network, see #4 (out of scope) and the Findings decision note.
- **Open/plaintext only** — peer marked `MMWLAN_OPEN`; no encryption.
- **No ATIM / power save** — ATIM window 0 (always awake).
- **Verified with IP (0x0800)**, not the literal Rimba EtherType `0x88B5`.
- **No teardown/disable path**; bring-up assumes a clean boot state.

## Backlog (prioritised)

### P1 — protocol-critical for Rimba
1. ☑ **Raw `0x88B5` frame exchange.** Done 2026-06-19. App broadcasts raw EtherType
   `0x88B5` L2 frames via `mmwlan_tx()` (bare 802.3 frame); `halow_rx` surfaces
   received `0x88B5` (lwIP would drop the unknown type). Verified bidirectional on
   the 2-board bench (each node RXes the peer's `RIMBA-88B5 seq=N`), concurrent
   with IP ping. The literal Phase-1 gate passes over the same IBSS data path as IP.
2. ☑ **Per-peer station records (multi-node).** Done 2026-06-19. Replaced the
   single shared `stad` with a `peers[]` table: each peer discovered on first RX
   (get-or-add keyed by transmitter MAC, mirroring `ieee80211_ibss_add_sta`) gets
   its own `umac_sta_data` + AID, so RX dedup/seq is now per-peer. The BSSID is
   excluded (S1G beacons carry SA=BSSID and aren't caught by the legacy
   `(MGMT,BEACON)` pre-assoc filter → would otherwise mint a bogus peer). TX still
   uses one shared queue (per-peer TX keying moves with CCMP). New public API
   `mmwlan_ibss_get_peers()` snapshots the table. Verified on the bench: each node
   forms exactly one peer record for the other (stable AID), data path healthy.
   **Finding (for #3):** registering the peer with the firmware via SET_STA_STATE
   (to get a keyable station handle) returns **-116 on the ADHOC interface**, and
   since `morse_cmd_tx` blocks, calling it from the RX/core-task context stalls
   the datapath. So firmware-station registration is deferred to #3 and must run
   off the hot path with the -116 understood first. Full verification of >2 nodes
   still needs ≥3 boards.
3. ◐ **Hop-by-hop encryption (AES-128-CCM).** Rimba spec wants CCMP hop-by-hop;
   move off OPEN. **Attempted 2026-06-19 — blocked, needs a different approach
   (see below). Reverted to working plaintext.**

### P2 — robustness / correctness
16. ☑ **Fix peer-extraction from the reference S1G beacon (Linux interop).** *Found +
    fixed 2026-06-20 (test plan I.1/I.2).* The IBSS RX filter
    (`umac_datapath_rx_frame_filter`, `src/umac/datapath/umac_datapath.c`) read the
    transmitter via `dot11_get_ta()` (legacy `addr2` offset) for **all** frames — but
    `morse_driver`'s **S1G beacons** use the compressed `dot11_s1g_beacon_hdr`, where
    `source_addr` is at offset 4 and the **`time_stamp`** sits at the `addr2` offset. So
    every reference beacon minted a fresh phantom peer from the ticking timestamp
    (observed `48:63:3d:08:00:d5`, `48:f3:3e:…`), flooding the 8-slot table and crowding
    out the real Linux MAC. (ESP32↔ESP32 was unaffected: the existing BSSID check in
    `umac_ibss_get_or_add_peer` drops our own `SA=BSSID` beacons — but only once the
    *right* field is read.) **Fix:** read `source_addr` for `(EXT, S1G_BEACON)` frames.
    Verified on-air: 1 clean peer for the Linux MAC, 0 phantoms, passive beacon
    discovery, bidirectional ping. Side benefit: the data path was already fine, so this
    only touched discovery.
    **Superseded 2026-06-20:** the `DBG-SA` follow-up (Findings → "Beacon-based peer
    discovery does no real work") showed the firmware doesn't surface peer beacons and
    morse beacons carry `source_addr=BSSID`, so beacon-based discovery earns nothing.
    `process_s1g_beacon` now **drops** beacons (no peer minting); discovery is fully
    data-driven. Revisit only if a future firmware surfaces real per-node beacons.
17. ☑ **Chronium beacon phantom flood (mixed-cell interop).** *Found + fixed 2026-06-20
    (I.5).* The adopted datapath bound IBSS to the **AP-mode**
    `frames_allowed_pre_association` list, which omits `S1G_BEACON` (an AP never receives
    beacons from clients). So every S1G beacon failed the pre-assoc check and fell to the
    RX filter's `dot11_get_ta` mint (`umac_datapath.c:1303`) = the beacon `time_stamp` →
    a phantom per beacon, flooding the 8-slot table and evicting real peers (ACM0 starved
    to 0 replies; a morse_driver node beacons every 100 ms with SA=BSSID). **Fix:** give
    IBSS its own allowed list incl. `S1G_BEACON` (like STA mode); beacons now skip the
    mint and route to `process_s1g_beacon` (#16 `source_addr` discovery, drops SA=BSSID).
    **Verified:** mixed 4-node cell full all-pairs reachability, **0 phantoms** (was
    hundreds). Same mint was the source of pure-cell discovery flakiness too. See
    Findings → "BUG #17".
4. ✗ **IBSS merge (TSF) — OUT OF SCOPE (decided 2026-06-20).** *Rimba is a
   **provisioned** network*: every node is deployed knowing the mesh's BSSID (like
   Wi-Fi credentials), so all nodes are already on one agreed cell. TSF merge
   (`ieee80211_rx_bss_info`, higher-TSF wins) exists to let *uncoordinated* ad-hoc
   nodes — each having rolled its own **random** BSSID — converge to a single cell.
   With a pre-shared BSSID there are never two BSSIDs to reconcile, so merge is
   unnecessary. The adopted momentary-systems fork made the same call (pinned BSSID,
   explicit create/join, no merge), as does Linux when pointed at a fixed BSSID. The
   still-useful part of the old #4 (unify create/join, drop the role heuristic) is
   folded into #7. **Revisit only if a deployment ever needs coordinator-free cell
   formation** (nodes that cannot pre-agree on a BSSID). See the Findings decision
   note below.
5. ☐ **Beacon contention.** Real IBSS has distributed beaconing (a node suppresses
   its beacon if it heard one for the interval). Currently every node beacons
   every interval unconditionally — fine for 2, a storm risk as density grows.
6. ◐ **Teardown / disable / re-enable.** Partly done 2026-06-20 via the adoption:
   `mmwlan_ibss_stop()` exists (tears down the vif; `start` composes over the boot
   STA) and the teardown-first bring-up is what fixes the EEXIST. **Remaining:**
   exercise/verify stop→re-enable and param/channel change at runtime (not yet tested).
   See [`rimba-ibss-impl-comparison.md`](rimba-ibss-impl-comparison.md).
7. ◐ **Dynamic create-else-join (`ieee80211_sta_find_ibss`).** The role must **not**
   be provisioned — field relays are all equal and don't know who's "first". Decide
   it at boot by **active scan against the agreed BSSID/SSID**: probe → got a probe
   response (cell exists) → `cfg_ibss JOIN`; silence → `cfg_ibss CREATE`. Whoever
   powers on first creates; everyone after joins. Identical logic on every node, no
   per-node config. We already have the parts: nodes answer probe requests (M5) and
   the chip surfaces probe responses during an active scan (M4). The current
   `mac[0] & 0x80` heuristic is a bench stand-in for this scan. **Pairs with #18:**
   JOIN is also what makes a late node sync to the cell's TSF (CREATE starts a fresh
   clock), so this is the vehicle for TSF sync, not just role selection. Also fix the
   latent `umac_connection_addr_matches_bssid` bug here (connection BSSID not set to
   the cell BSSID in IBSS). NB this is *not* merge (#4) — same known BSSID throughout;
   "does the one cell exist yet?", not "reconcile two BSSIDs." IPs are MAC-derived
   (`192.168.13.<mac[5]>`), fine for a mesh.
14. ☑ **Peer age-out / free (follow-on to #2).** Done 2026-06-20 via the adoption:
    `mmwlan_ibss_age_peers(threshold_ms)` frees per-peer records (+ stad) idle past a
    threshold and fires `PEER_REMOVED`; per-peer `last_rx_ms`, LRU eviction at the
    8-cap; caller-driven (the app calls it every loop @ 30 s). Mirrors the intent of
    `net/mac80211/ibss.c` `ieee80211_ibss_sta_expire()` (Linux: 60 s on a timer; ours
    is caller-driven, no background timer). **Verified P0.6 (2026-06-20):** survivors
    aged out a dropped peer (`PEER_REMOVED` on both) and rediscovered it as a **fresh
    record** on return (`PEER_ADDED` on both) — the survivor-side re-acquisition that
    was structurally impossible before. This (a) bounds the heap / unblocks >8-node
    cells and (b) unblocked honest drop/rejoin testing (test plan §4 P0.6). See
    [`rimba-ibss-impl-comparison.md`](rimba-ibss-impl-comparison.md).

### P3 — power & de-risking
> **PRIORITY CHANGE (2026-06-20): power-save is an early focus, not deferred to
> Phase 3/4.** Rimba's whole battery-leaf model (RTC-scheduled wake) rests on it, and
> it's better to prove it's achievable now than to discover at Phase 3 that the
> firmware can't. Power-save is **host/RTC-driven** (#8), since the firmware has no IBSS
> radio PS. Near-term sequence: finish the small Phase-1 validation (~~P0.4 dedup ☑~~,
> I.4 frame diff, P1.5 soak) → **#9 RISK-02 boot-time** (the gating number) → **#8**
> RTC-scheduled-mode prototype → **#7** create-else-join (fast rejoin). #9 runs *after*
> the other Phase-1 points. (Dev-plan task 1.12a updated to match.)

18. ☐ **TSF sync — DEMOTED (no longer gating; the RTC is the schedule clock).** *Reframed
    2026-06-20.* Power-save scheduling now rides the **RTC** (#8 RTC-scheduled mode), not
    the radio TSF — so TSF sync is **not** a power-save prerequisite anymore. Keep it on
    the list only as a *nice-to-have* (fine intra-window timing, or if we ever want
    radio-level scheduling). The experiment below stays valid if/when we want it, but it is
    **not** Phase-1-blocking; #9 (boot time) is the gating power-save number instead.
    Scheduled wake needs every node to agree on the cell's TSF clock. TSF *sync*
    (intra-cell, "adopt the highest TSF heard") is **separate from merge** (#4) and is
    a firmware/lower-MAC behaviour — so this **starts as an experiment**, not a code
    change:
    1. **Can the host read the TSF?** Find/expose a `GET_TSF` in `morse_commands`
       (Linux: `drv_get_tsf`) — needed both to verify sync and to schedule wake.
    2. **Do same-BSSID nodes actually sync?** Measure two ESP32 TSFs over time — do
       they converge? (One data point suggests chronium's stayed *independent*, but its
       `fixed-freq` join may disable sync, so inconclusive for ESP32↔ESP32.)
    3. **Does JOIN sync to the cell while CREATE starts fresh?** (Confirms #7 delivers
       sync.)
    If the firmware syncs intra-cell, even today's all-CREATE nodes may already
    converge and we mainly need the readout to prove it; if it doesn't and exposes no
    controls, that's a firmware-capability limit we document (like beacon-surfacing).
    Linux ref: `net/mac80211/ibss.c` TSF handling + `ieee80211_rx_bss_info`.
8. ☐ **RTC-scheduled radio power-cycling = Rimba "Scheduled mode" (EARLY FOCUS;
   PRIORITY after the small Phase-1 items).** *Approach decided 2026-06-20.* Instead of
   relying on chip-level IBSS power-save (which doesn't exist — see below), drive the
   whole duty cycle from the **ESP32 + the precise RTC** (RV-3028-C7, ≤1 ppm): the RTC
   alarm wakes the ESP32, which **powers the MM6108 on, joins the pinned cell, exchanges
   for the window, powers the radio off, and deep-sleeps** until the next alarm. Both
   chips off between windows → lowest power.

   **Why this is the right model (it bypasses the firmware gaps):**
   - **No chip IBSS power-save needed** — we hard power-cycle the radio, not chip-doze it.
   - **No radio TSF sync needed** — the **RTC is the shared clock**, not the chip TSF.
     This *demotes #18* (TSF sync) from "gating prerequisite" to nice-to-have.
   - **Fits Rimba's DTN nature** — delay-tolerant, so "events wait for the next window"
     is acceptable (leaves buffer + send on their slot). RTC is already mandated in the
     spec *for* scheduled wake.
   - **Keeps the all-IBSS topology** — pure-IBSS leaves can be low-power without TWT, so
     the AP-STA-for-power argument (below) is weakened, not forced.

   **The gating measurement → RISK-02 (#9): radio cold-boot-to-IBSS-joined time.** Every
   window pays an MM6108 boot (firmware blob over SPI) + IBSS formation before data flows.
   That single number sets the viable wake period + the power budget; if it's tens of ms,
   great, if hundreds of ms–seconds, the duty cycle is inefficient. **Also check for a
   faster *resume* (firmware-retained reset) vs full cold boot.** This is THE Phase-1
   power-save experiment — measure it on the bench (GPIO toggle: power-on → first
   frame exchanged).

   **Other costs to design for:** clock discipline (RTCs drift — ~60 µs/min at 1 ppm, so a
   few-ms guard band covers many minutes; need initial + periodic re-sync, e.g. distribute
   canonical time **in-band during the window**); window = boot + join + data + guard; all
   nodes on the same schedule (provisioned period + re-sync); latency floor = the wake
   period (async alerts wait — DTN-acceptable).

   **Hardware pins (XIAO carrier, `boards/proto1-fgh100m`)** for the cycle: `RESET_N`=GPIO1
   (power-cycle the chip), **`WAKE`=GPIO2** (host→chip, `mmdrv_set_wake_enabled()`),
   **`BUSY`=GPIO5** (chip→host), **`SPI_IRQ`=GPIO3** (chip→host — wakes the ESP32). RTC
   alarm → ESP32 deep-sleep wake. The wiring exists (dev-plan 1.9–1.11).

   **Why not chip power-save (the investigation that led here):**
   - *morselib:* `mmwlan_set_power_save_mode` + `Standby` exist but are **STA/DTIM-only**;
     no ATIM, no IBSS-PS code.
   - *morse Linux driver (firmware oracle):* rich PS — `ps.c` (bus/dynamic PS), **`twt.c`
     (TWT, 802.11ah scheduled wake)**, `yaps.c` — but **structurally STA/AP-only** (every
     TWT path gates on `IFTYPE_STATION`/`AP`; no ADHOC; no ATIM). So the firmware almost
     certainly has **no IBSS radio power-save**. See the Findings note.
   - *Continuous mode* (radio on, MCU sleeps on the GPIO3 IRQ) remains the simple fallback
     and is independently useful for always-on relays.
   - *TWT works in AP-STA* — only relevant if the RTC-scheduled approach proves
     insufficient; then AP-STA leaf↔relay links become the lever. Not the plan.

   **Depends on #7 (create-else-join, so a woken node rejoins fast) + #9 (RISK-02 boot
   time, the gating number).** Supersedes the earlier "implement ATIM" framing.
9. ☐ **RISK-02 — measure radio cold-boot-to-IBSS-joined time (GATING for #8; PRIORITY
   after the small Phase-1 items).** This is the number that decides whether RTC-scheduled
   mode (#8) is viable. Dev plan targets `LEAF_BOOT_MS=30`; never measured. GPIO-toggle /
   timestamp from MM6108 power-on → firmware loaded → IBSS joined → **first frame
   exchanged**. Also measure a *resume* path (firmware-retained reset) if one exists — a
   fast resume vs a full cold boot changes the power budget a lot. Run on the bench
   (boards we have). **This is the Phase-1 power-save experiment** (replaces the earlier
   "does the chip do IBSS PS" framing — we now expect it doesn't).
10. ☐ **>2-node test.** Validate beacons, discovery, and data with 3+ nodes (needs
    more boards) — depends on #2.
11. ☐ **Verify the S1G beacon on-wire.** Only the probe response was decoded; the
    actual `EXT/S1G_BEACON` frame's framing (compat IE, etc.) is chip-side and
    unverified — capture via a monitor interface.

### P4 — code quality
12. ☑ **Factor IBSS out of `umac.c`** into `umac_ibss.c` (beacon, probe-resp,
    datapath ops, bring-up). Done 2026-06-20 by adopting the momentary-systems
    `umac/ibss/umac_ibss.c` (+ `.h`) with the `mmwlan_ibss_*` public API, **keeping our
    Linux-verified beacon discovery** layered on top (#16 — their `SA=BSSID` beacon
    assumption is wrong for the reference `morse_driver`). See
    [`rimba-ibss-impl-comparison.md`](rimba-ibss-impl-comparison.md).
13. ☐ **Review all STA/AP-only assumptions** in morselib for other ADHOC drops
    (the RX-VIF bug was one; audit for siblings).
15. ☐ **Bump the ESP32 stack to latest (fw / SDK / IDF).** Current pins:
    MM6108 firmware **1.17.6**, `morsemicro/halow` (morselib + mm-iot-sdk) fork
    **`2.10.4-esp32-2`**, ESP-IDF **v5.4.2**. Move each to the latest stable.
    Caveats / cost (this is not a plain version-bump):
    - **The IBSS port patches morselib** (the ADHOC interface, IBSS commands,
      beacon/probe-resp, datapath RX-VIF fix live on the `mm-esp32-halow` fork). A
      morselib/mm-iot-sdk bump means **re-applying and re-validating those patches**
      on the new base — budget for a real port-forward, not a fast-forward.
    - **Keep generation parity with the Linux reference node** (P0.5 interop, test
      plan §2): bump the ESP32 *and* the chronium `morse_driver`/firmware **together**
      to the same MM release, or pin both deliberately — a one-sided bump invalidates
      the interop result. See `rimba-ibss-linux-interop-runbook.md`.
    - **Toolchain pin:** keep cmake on **3.x** (4.x breaks the ESP-IDF build, per
      README) when moving IDF.
    - **Regress after any bump:** re-run the 3-board P0 bench (and the AP-STA ping)
      before trusting the new base.

## Findings

### IBSS_CONFIG EEXIST(-17) — root cause + why we still tolerate it (2026-06-20)
Our `mmwlan_ibss_enable` bring-up gets `IBSS_CONFIG(CREATE)` → **EEXIST(-17)** from the
firmware, which we tolerate (treat as "already created"). Investigated against the other
public fork, **momentary-systems/esp-halow-ibss** (`5237495`, see
[`rimba-ibss-impl-comparison.md`](rimba-ibss-impl-comparison.md)).

- **Confirmed it is NOT firmware/command-bytes:** the `mm6108.mbin` blob is byte-identical
  (same SHA256) between the forks, and `mmdrv_add_if` / `mmdrv_cfg_bss` / `mmdrv_cfg_ibss` /
  `morse_cmd_tx` are byte-identical. Matching every command *arg* (cssid, DTIM,
  probe_filtering, interface MAC = node-MAC, skipping BSSID_SET) in our fork did **not**
  remove the EEXIST.
- **Root cause = a divergence from the Linux flow (governing rule).** Their
  `mmwlan_ibss_start` **removes any active interface (boot STA/SCAN/AP) before
  `ADD_INTERFACE(ADHOC)`** (`umac_interface_remove` loop) — and that teardown is exactly
  what Morse's Linux stack does. In mac80211 you cannot switch an iface to IBSS while it is
  up: `ieee80211_if_change_type` returns `-EBUSY` (kernel `net/mac80211/iface.c`), so
  `iw set type ibss` requires `ip link down` first, which runs `ieee80211_do_stop()` →
  driver `.remove_interface` → **`morse_mac_ops_remove_interface` → `morse_cmd_rm_if` →
  `MORSE_CMD_ID_REMOVE_INTERFACE`** (`morse_driver/mac.c:3566`, `command.c:916`). i.e. Linux
  removes the old firmware interface, then adds the new ADHOC one. **Our bring-up skips the
  `REMOVE_INTERFACE` step** — we `ADD_INTERFACE(ADHOC)` on top of the live boot interface, so
  the stale firmware BSS context makes `IBSS_CONFIG(CREATE)` report already-exists → EEXIST.
  Their fork (and Linux) returns `0`. So the teardown is not their trick; it's the
  Linux-mandated behaviour we should derive per the governing rule.
- **Why we still tolerate EEXIST for now (precise mechanism):** instrumented our bring-up —
  before `ADD_INTERFACE(ADHOC)` the active set is `active_interface_types = 0x0001` =
  **`UMAC_INTERFACE_NONE`**, the boot vif that `mmwlan_boot` creates (`umac.c:778`). That
  lone NONE interface is what makes `IBSS_CONFIG(CREATE)` return EEXIST. But
  `umac_interface_remove` only issues the firmware `mmdrv_rm_if` (REMOVE_INTERFACE) **when
  `active_interface_types` reaches 0** (`umac_interface.c:23-27`) — so the teardown removes
  NONE → active hits 0 → **`mmdrv_rm_if(boot vif)`** fires → the chip/data path is torn down,
  and re-adding ADHOC does not re-establish the netif/datapath binding our `mmhalow` relies
  on. Result: porting *only* the teardown **killed the EEXIST but regressed the data path**
  (discovery/RX perfect — 3 peers, 0 phantoms — but all pings failed, chronium↔ESP32 0/3).
  Their fork re-establishes the binding in `umac_ibss_start`; ours doesn't. Reverted → data
  path works again (chronium→ESP32 2/2, 4-node mixed cell OK). **Proven fixable:** building +
  flashing their *full* fork on our exact board gives both no-EEXIST *and* a working data path.
- **Surgical fix attempt 2 (2026-06-20) — teardown + RX re-bind, still insufficient.** Added
  the teardown (EEXIST gone, `IBSS_CONFIG`=0 ✓) **and** an `mmhalow_rebind_rx()` re-registering
  the netif RX/link-state callbacks after the re-init. Result: **raw L2 TX works** (`0x88B5`
  broadcast sends, 0 failures) but **ARP/ICMP still fail** (data RX broken — ARP replies never
  arrive → ping never resolves). So the chip re-init loses **more** data-path state than just
  the RX callback (chip-side data RX config / netif↔driver attach / PS / channel). Reverted.
  **Conclusion: surgical reconstruction isn't converging — the validated fix is to adopt the
  momentary-systems integrated bring-up (their `umac_ibss.c` + mmhalow), i.e. the #12 factor-out
  — which re-establishes the full post-boot data path holistically and is proven on our HW.**
- **Backlog (future clean-up, Linux-faithful):** do **both** halves of the Linux sequence —
  (1) `REMOVE_INTERFACE` the active interface before `ADD_INTERFACE(ADHOC)` (mirrors
  `morse_mac_ops_remove_interface`/`ieee80211_do_stop`), **and** (2) re-bind the netif/datapath
  to the new ADHOC vif afterwards (mirrors their `umac_ibss_start` host setup / Linux's
  `ip link up` re-attach). Until both land, the EEXIST is benign and handled. Relates to
  #6 (teardown/disable) and #7.

### Adoption validated on HW (2026-06-20)
Adopted the momentary-systems `umac_ibss.c` + integrated bring-up (WIP branches:
super `wip/ibss-adopt-refactor`, submodule `wip/adopt-momentary-ibss`). Bench results:
- **EEXIST gone** — `IBSS_CONFIG(CREATE)` returns 0 (teardown-first bring-up mirrors
  the Linux `REMOVE_INTERFACE`→`ADD_INTERFACE(ADHOC)` order). Root-caused, not tolerated.
- **Pure ESP32↔ESP32 (2 nodes, no Linux node): works** — ACM0↔ACM1 discover + ping
  bidirectionally.
- **Pure 3-ESP32 (no Linux node): full mesh, 3/3 bidirectional** (2026-06-20) — each
  node pings + gets replies from *both* others. Same node count as the mixed cell
  minus chronium → isolates node-count from the Linux node. This is the
  deployment-relevant case (cells are all-rimba) and it works completely.
- **Linux interop: works** — chronium ↔ all 3 ESP32 data 2/2; beacon-based discovery
  of the Linux node via the #16 `source_addr` fix.
- **Gained** peer age-out (#14), teardown (#6), membership callbacks (#12).
- **BUG #17 — mixed-cell ESP32↔ESP32 discovery breaks: chronium beacon phantom flood
  (root-caused 2026-06-20, I.5).** *Earlier framing (airtime contention / "not a
  blocker") was WRONG — corrected here.* In a mixed cell the ESP32s under-discover
  each other because **chronium's `morse_driver` S1G beacon mints a phantom peer per
  beacon** (~every 100 ms): MACs `48:xx:xx:xx:00:d5` — the #16 timestamp-as-MAC
  signature. ~hundreds of phantoms in 40 s flood the 8-slot peer table and **LRU-evict
  the real ESP32 peers** → ACM0 got **0 replies** (fully starved), ACM2 lost chronium.
  chronium→ESP32 is fine (3–4/4 replies, clean `station dump`); only ESP32←chronium
  beacon parse is broken.
  - **Mechanism (pinned by on-air probe).** Instrumented the filter mint site and
    captured chronium's beacon: `vts=0x001c ta=48:93:01:00:00:d5 fc=1c00 a1=02123456789a
    o10=4893010000d5`. So the beacon **is** classified `S1G_BEACON` (`vts=0x1c`), `a1`
    (source_addr, offset 4) = the **BSSID**, and `ta = dot11_get_ta` = the **time_stamp**
    at offset 10 → the phantom. Crucially it fired *inside the filter's mint block*, i.e.
    `rx_frame_allowed_pre_association` returned **false for a beacon**. Cause: IBSS reused
    the **AP-mode** `frames_allowed_pre_association` list (`{PROBE_REQ, AUTH, ASSOC_REQ}`),
    which omits `S1G_BEACON` — so beacons aren't allowed pre-assoc and fall to the TA-mint.
    (The #16 `source_addr` fix in `process_s1g_beacon` is correct but runs *after* the
    filter, which already minted the phantom.)
  - **Fix (one targeted change):** give IBSS its own list incl. `S1G_BEACON` (mirrors STA
    mode — IBSS has no association). Beacons skip the filter mint and reach
    `process_s1g_beacon`, which reads `source_addr` (peer MAC → discover; BSSID → drop).
    `mm-esp32-halow` `fix/ibss-beacon-phantom-flood` (`0cf0a97`).
  - **Verified on HW (mixed 4-node, 3 ESP32 + Linux):** full all-pairs reachability,
    **0 phantoms** (was hundreds), chronium 4/4 to each ESP32, station dump = 3 peers.
  - **Ruled out:** IBSS merge / BSSID split (same pinned BSSID, no TSF merge); it was never
    a classification miss or airtime contention.
  - **Severity:** broke **Linux interop** discovery (and degraded pure cells — ESP32 peer
    beacons hit the same mint). Not a hard pure all-ESP32 blocker (data-path discovery
    masked it), but the fix makes both cells clean.

### Beacon-based peer discovery does no real work — discovery is data-driven (2026-06-20)
Followed up on #17 with an on-air `DBG-SA` probe in `process_s1g_beacon` (dump each
received beacon's `source_addr`). Result settles whether #16 beacon discovery earns its
keep — it doesn't:
- **Pure 2-ESP32:** the two boards communicate fine, but **0 beacons reached
  `process_s1g_beacon`** on either. The **mm6108 firmware (1.17.6) does not surface
  same-cell peer beacons to the host** — a firmware/lower-MAC decision, upstream of the
  driver (morselib never sees them). So ESP32↔ESP32 discovery is 100% data-driven.
- **With chronium:** 113 beacons reached the handler, **all** `source_addr =
  02:12:34:56:78:9a` (the BSSID), and `umac_connection_addr_matches_bssid()` returned
  **false** for it — so #16 was actually **minting the BSSID as a junk peer** (→ IP
  `.154`), not "dropping it" as previously thought. (Sub-bug: the connection's BSSID
  field isn't set to our cell BSSID in IBSS; moot once we stop using beacons.)
- **Conclusion / fix:** `process_s1g_beacon` no longer mints peers from beacons (#16
  reverted to a drop, with a comment marking it **morse hardware/firmware dependent**).
  Real discovery is the data-frame path (`lookup_stad_by_peer_addr_ibss` on the real TA),
  which is what Linux+morse effectively does too (`net/mac80211/ibss.c` keys on
  `mgmt->sa`, but morse RX sets `mgmt->sa = bssid`). Converges our design with the
  momentary-systems (data-driven) model. Revisit #16 only if a future firmware surfaces
  peer beacons with a real per-node `source_addr`. `mm-esp32-halow` `d7cfa18`.

### DECISION — Rimba is a provisioned network: no IBSS merge (2026-06-20)
Closes #4. **Rimba is a provisioned mesh** — every node is deployed already knowing the
network's BSSID (provisioned at staging, like Wi-Fi credentials), so all nodes share one
**agreed BSSID** and are on a single cell from boot.

- **What merge is for:** IBSS has no AP to define the cell. If nodes start "the same"
  network (same SSID) but each rolls its own **random** BSSID, you get two same-named cells
  with different IDs that can't talk. TSF merge (`net/mac80211/ibss.c:
  ieee80211_rx_bss_info`, higher-TSF wins) is the coordinator-free coalescing that collapses
  them into one — *only needed when nodes can't pre-agree on a BSSID*.
- **Why Rimba doesn't need it:** a pre-shared BSSID means there are never two BSSIDs to
  reconcile. Standard for a managed deployment; sidesteps the firmware-capability risks
  (runtime BSSID change, TSF readout) merge would require.
- **Consistency:** the adopted momentary-systems fork already chose pinned-BSSID + explicit
  create/join + no merge; Linux pointed at a fixed BSSID behaves the same.
- **Reversal trigger:** only a deployment that must form cells with *no* pre-agreed BSSID
  (truly uncoordinated nodes). Not anticipated. The merge beacon-input is actually available
  (foreign-TSF beacons *are* surfaced — cf. the data-driven finding above), so this is a
  product decision, not a hardware limitation.

What remains is #7 (dynamic create-else-join against the agreed BSSID), not #4.

### DECISION — role is dynamic (not provisioned) + power-save is an early focus (2026-06-20)
Two related decisions:
- **No provisioned create/join role.** Field relays are all equal and can't know who's
  "first," so the role can't be a per-node config. It's decided at boot by **active scan
  against the agreed BSSID** (`ieee80211_sta_find_ibss`): cell exists → JOIN, else CREATE.
  See #7. The current `mac[0]&0x80` heuristic is a bench stand-in for that scan.
- **Power-save pulled early — via RTC scheduling, not chip power-save.** The morse driver
  has no IBSS radio power-save (TWT is STA/AP-only — see the Finding), so Rimba's
  battery-leaf Scheduled mode is driven by the **ESP32 + the precise RTC**: wake on the RTC
  alarm, power the radio on, join, exchange, power off, deep-sleep (#8). This **bypasses
  both the missing chip IBSS-PS and the TSF-sync question** (the RTC is the schedule clock,
  so #18 is demoted). The gating unknown is **radio cold-boot-to-joined time** (#9 /
  RISK-02) — that one number sets the viable wake period and the power budget.
  **Sequence:** small Phase-1 validation (P0.4 ☑, I.4, P1.5 soak) → **#9 RISK-02 boot-time
  measurement** (the gating number) → #8 RTC-scheduled-mode prototype → #7 create-else-join
  (fast rejoin per window). The boot-time measurement runs *after* the other Phase-1
  points. Dev-plan task 1.12a updated to match.

### FINDING — morse driver has no IBSS radio power-save; TWT is STA/AP-only (2026-06-20)
Read the morse Linux driver (`~/halow/morse_driver` on chronium) to gauge firmware
power-save capability. It has a *rich* power-save stack — `ps.c` (bus/dynamic PS),
**`twt.c` (Target Wake Time — the 802.11ah scheduled-wake mechanism, not legacy ATIM)**,
`yaps.c` — **but it is structurally STA/AP-only:** every TWT path gates on
`IFTYPE_STATION`/`IFTYPE_AP` (no ADHOC branch), `morse_cmd_set_ps` is driven from STA
(`cfg80211` power_mgmt) + AP paths, and there is **no ATIM and no IBSS/ADHOC on-air
power-save path anywhere**. Since the driver mirrors firmware features, the firmware very
likely has **no IBSS radio power-save** — an IBSS node's radio stays awake to hear peers.

**Consequence — resolved by going host-driven (RTC), not a blocker after all:**
- *Continuous mode* (radio on, **MCU** sleeps, wakes on the GPIO3 IRQ) is achievable now.
- *Scheduled mode* doesn't need chip power-save: **drive it from the ESP32 + RTC** —
  power-cycle the radio per the RTC schedule (#8). The RTC is the shared clock, so we
  don't need TWT/ATIM *or* radio TSF sync. This keeps the all-IBSS topology.
- **TWT works in AP-STA** — held in reserve only if RTC-scheduled cycling proves too
  power-hungry (e.g. radio cold-boot is too slow per #9); then AP-STA leaf↔relay links
  become the lever. Not the current plan.
Full detail + the gating experiment (#9 boot time) in #8. Still worth confirming with
Morse whether any IBSS PS exists undocumented (as the IBSS_CONFIG opcodes did).

### Beacon interval = 100 TU, matches Linux (2026-06-20)
Q: is the ESP32 beacon interval different from Linux? **No.** The app sets
`beacon_interval_tu = 100`; the Linux node config uses `beacon_int=100`
(`rimba-linux-node-setup.md`). 100 TU (≈102.4 ms) is the standard IEEE 802.11 /
mac80211 IBSS default — both sides match. S1G nuance: 802.11ah adds short beacons +
an "S1G Short Beacon Interval" IE, but Linux IBSS always emits **long** beacons
(`beacon.c:388` — `if (type==ADHOC) short_beacon=false`), and our `umac_ibss` beacon
builder does the same, so we follow Linux there too.

### CCMP attempt (2026-06-19) — blocked on the chip's key model
Tried the simplest Rimba-aligned model: one shared **GROUP** CCMP key (the
deployment key) for all frames, installed via `umac_keys_install_key` (cipher is
always `AES_CCM`; `umac_keys_init` is required first since `umac_sta_data_alloc_static`
only memsets). Made IBSS unicast use the group key too. Encrypted ping failed
(`ok=0`, some sends succeed but no replies). Two hard blockers identified:
- **Group key can't decrypt unicast.** HW CCMP RX uses the *pairwise* key for
  unicast; a unicast frame encrypted with the group key isn't decrypted by the
  peer (some TX went out, zero replies).
- **Pairwise keys need an AID.** `mmdrv_install_key(vif_id, aid, ...)` keys a
  pairwise key to a STA's AID. IBSS has **no association → no AID**, so a static
  pairwise key can't be keyed to a peer.

**Root cause (revised after reading the Linux side, 2026-06-19).** It is *not* a
chip limitation. How Linux does IBSS CCMP (`wpa_supplicant/ibss_rsn.c` +
`net/mac80211`):
- A **`sta_info` per peer**, keyed by **MAC address** (`ieee80211_ibss_add_sta`).
  No association, but the station record still exists.
- Per pair, a **4-way handshake** runs; the PTK from the handshake **initiated by
  the higher-MAC node** is the one used (`ibss_rsn.c:163-168` tie-break).
- The **PTK is installed against the peer's MAC** (`supp_set_key` →
  `wpa_drv_set_key(addr=peer->addr, key_idx=0)`) → unicast uses it. Each node has
  its own **GTK** for broadcast; RX broadcast decrypts with the *sender's* GTK.
- The driver maps `sta_info` → a hardware **station handle** for the key table.

On the MM6108 that station handle is the `aid` field. The firmware already exposes
the two primitives Linux uses, and morselib already implements both — but only
drives them from the **AP association** path:
- `MORSE_CMD_ID_SET_STA_STATE` (0x14): binds **`sta_addr` (MAC) + `aid` + state**
  (NONE→AUTH→ASSOC→AUTHORIZED — the `ieee80211_sta_state` enum). This is the
  `morse_op_sta_state` reflection. (`driver.c:858`, used by `umac_connection.c`.)
- `MORSE_CMD_ID_INSTALL_KEY` (0x0A): PTK/GTK keyed by that `aid` + key_idx.

So the shared-group-key attempt failed because **IBSS has no per-peer station
record** → no `aid` to bind a pairwise key to. The `aid` is just a station handle,
exactly like Linux's sta_info→key-slot mapping; it does **not** require a real
association AID.

**Update 2026-06-19 (from #2):** the host-side per-peer records + AID assignment
now exist (#2 done). But the firmware half — `SET_STA_STATE(peer_mac, aid,
AUTHORIZED)` — **returns -116 on the ADHOC interface**, so the firmware does not
mint a station handle for an IBSS peer the way it does on association. Until that
is understood, there is no `aid` the firmware will accept an `INSTALL_KEY` against,
so even static per-peer CCMP is blocked at the firmware boundary. Next concrete
step for #3: investigate why SET_STA_STATE rejects ADHOC peers (interface state?
a different/added command? unsupported for adhoc on this fw build?), driving it
off the RX hot path. Check `morse_driver`'s `morse_op_sta_state` for the adhoc
sequence the kernel driver uses.

**Corrected path (CCMP once the firmware station handle works):**
1. **Per-peer station records (#2 / P1.2)** — DONE host-side. The firmware
   `SET_STA_STATE(peer_mac, aid, AUTHORIZED)` (the `ieee80211_ibss_add_sta` +
   driver `sta_state` cb mirror) currently returns -116 on adhoc; resolve first.
2. **Then CCMP**, two flavours:
   - **(a) static pre-shared** — install a pre-shared PTK against each peer's `aid`
     (unicast) + a shared GTK (broadcast). No handshake; minimal; non-standard but
     uses the real per-peer key mechanism.
   - **(b) full IBSS RSN** — drive the bundled `ibss_rsn.c` (per-peer 4-way
     handshake, higher-MAC = authenticator) → `INSTALL_KEY` per peer. Standards-
     correct; the work is glue (driver `set_key`→morselib, peer add/remove, EAPOL
     routing). 
This reorders the backlog: **#2 is the prerequisite for #3.** Reverted the WIP;
tree is back to working plaintext + dynamic ARP.

## Done
- ☑ RISK-01 core: IBSS bring-up, beacon, probe-answering, IP data path with
  dynamic ARP (see milestones doc).
- ☑ Raw `0x88B5` L2 exchange verified bidirectional (P1.1, 2026-06-19).
- ☑ Per-peer station records — one `sta_data`+AID per discovered peer, BSSID
  excluded, `mmwlan_ibss_get_peers()` API (P1.2, 2026-06-19).
- ☑ Stop tracking generated `*.mbin.o` build artifacts.
