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
- **No IBSS merge** — BSSID is hardcoded and the role is a MAC heuristic in the
  app; no TSF-based merge / join-existing-cell discovery.
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
4. ☐ **IBSS merge (TSF).** Discover peers via beacons and converge on a common
   BSSID/TSF (higher-TSF wins), per `ieee80211_rx_bss_info`. Replaces the
   hardcoded-BSSID + MAC-role bench heuristic with real create/join semantics
   (scan → join existing, else create). **Pairs with #14 (age-out): merge makes
   peer membership a real state machine, and add/remove are two sides of it.**
5. ☐ **Beacon contention.** Real IBSS has distributed beaconing (a node suppresses
   its beacon if it heard one for the interval). Currently every node beacons
   every interval unconditionally — fine for 2, a storm risk as density grows.
6. ☐ **Teardown / disable / re-enable.** `mmwlan_ibss_disable()` (stop beaconing,
   `IBSS_CONFIG STOP`, remove interface, free the stad); handle re-enable and
   param/channel change cleanly. *Template:* the momentary-systems fork has
   `mmwlan_ibss_stop()` (tears down the vif; `start` composes over the boot STA) —
   see [`rimba-ibss-impl-comparison.md`](rimba-ibss-impl-comparison.md).
7. ☐ **Drop bench heuristics.** Remove the MAC-based role pick + hardcoded IPs in
   the app once merge + proper create/join land.
14. ☐ **Peer age-out / free (follow-on to #2).** Per-peer records are currently
    heap-allocated on first RX and **never freed** — a departed peer lingers; the
    table caps at 8 then overflows to the shared record, and the heap is grow-only.
    Stamp `last_rx` per peer, sweep on the beacon/work timer, and free the record +
    release its AID when idle past a limit. **Linux ref (governing rule):**
    `net/mac80211/ibss.c` `ieee80211_ibss_sta_expire()` on a timer, dropping peers
    idle past `IEEE80211_IBSS_INACTIVITY_LIMIT` (60 s). Best built **with #4**, since
    proper join semantics already need peer add *and* remove. **Why it matters:**
    (a) bounds the heap and unblocks >8-node cells; (b) it is the prerequisite for
    honest **survivor-side** drop/rejoin testing — observed in the P0.6 bench
    (2026-06-19), where survivors could not re-acquire a returned same-MAC peer
    because the record never expired (see test plan §8.1 and the P0 worklog). Until
    age-out exists, "rediscovered as a fresh record" is impossible on the survivor
    side by construction. *Template:* the momentary-systems fork implements this
    (`mmwlan_ibss_age_peers(threshold_ms)`, per-peer `last_rx_ms`, LRU eviction at the
    8-cap; caller-driven, no background timer) — usable as a reference. See
    [`rimba-ibss-impl-comparison.md`](rimba-ibss-impl-comparison.md).

### P3 — power & de-risking
8. ☐ **ATIM / IBSS power save (RISK-06-ish, Rimba leaves).** Battery leaves need
   doze; implement the ATIM window + buffered-traffic announcement. Check whether
   the MM6108 firmware exposes IBSS PS at all (Open Issue #6).
9. ☐ **RISK-02 — measure IBSS boot/join time.** Dev plan targets `LEAF_BOOT_MS=30`;
   never measured. GPIO-toggle / timestamp from power-on to first beacon
   heard/joined. Gates Scheduled mode.
10. ☐ **>2-node test.** Validate beacons, discovery, and data with 3+ nodes (needs
    more boards) — depends on #2.
11. ☐ **Verify the S1G beacon on-wire.** Only the probe response was decoded; the
    actual `EXT/S1G_BEACON` frame's framing (compat IE, etc.) is chip-side and
    unverified — capture via a monitor interface.

### P4 — code quality
12. ☐ **Factor IBSS out of `umac.c`** into `umac_ibss.c` (beacon, probe-resp,
    datapath ops, bring-up) for separation and easier upstream-diffing. *Template:*
    the momentary-systems fork (`esp-halow-ibss` @ `5237495`) already factors exactly
    this into `umac/ibss/umac_ibss.c` (+ `.h`) with a clean `mmwlan_ibss_*` public API
    — strong reference for the module split. Reuse its structure, but **keep our
    Linux-verified beacon discovery** (their `SA=BSSID` beacon assumption is wrong for
    the reference `morse_driver`). See [`rimba-ibss-impl-comparison.md`](rimba-ibss-impl-comparison.md).
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
