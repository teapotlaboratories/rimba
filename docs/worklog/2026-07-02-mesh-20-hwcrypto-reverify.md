# 2026-07-02 — #20 HW-crypto A4≠TA mesh forward: re-verified on the ESP, isolated to the host stack, and proven NOT a driver→FW command difference

Re-opened the #20 backlog ("in HW-crypto mode the MM6108 FW withholds a forwarded 4-address
frame whose A4 [mesh-SA] ≠ TA") after the §#25/§#26 verdict in
`2026-06-27-mesh-security-phase3-sae.md` was found to be **self-contradictory**: §#25 proved the
*same* FW binary HW-decrypts + delivers the A4≠TA forward on Linux (including running the ESP's exact
blob on chronite), then §#26 concluded "it's the firmware" — a logical inversion of its own strongest
datum. A reliable, independent re-run overturns the "closed-FW, unpinnable" framing.

## Bottom line
1. **The A4≠TA-forward withhold is REAL on the ESP** — re-confirmed with a *reliable* RX-entry probe.
   §#26's linchpin ("RX fired 0×") came from a probe §#26 *itself* called unreliable (it never
   captured even a positive control — a USB-CDC reset-on-open artifact).
2. **It is the HOST STACK (morselib), not the FW and not the BCF.** Excluded the FW version (ran
   **1.17.8** on the ESP → still withholds) and the BCF (the ESP runs `bcf_fgh100mhaamd`, the *same*
   BCF chronosalt runs while *delivering* under morse_driver).
3. **But it is NOT a driver→FW command difference.** An exhaustive ID- *and* byte-level diff of
   morse_driver's vs morselib's mesh-vif + peer-add command streams shows them **functionally
   equivalent**. The fix is not "configure the FW like Linux via command X"; the difference is a
   host-side interaction *outside* the FW command interface.

This corrects §#26 on both counts: it's the host stack (not the FW/BCF), and it is genuinely *not a
command* (§#26 guessed a "non-command FW input" — right that it isn't a command, wrong that it's
FW-side).

## 1. The reliable on-ESP re-probe (the decisive test §#26 lacked)
- Instrumented `umac_datapath_process_rx_frame` (the earliest host RX entry, `umac_datapath.c`) with a
  counter split by **A4==TA (direct)** vs **A4≠TA (forward)** and the `MMDRV_RX_FLAG_DECRYPTED` bit,
  exposed via `mmwlan_mesh_get_rx20dbg()` and logged in the app heartbeat. Read via `stty raw` + `cat`
  (no DTR/RTS reset — the fix for §#26's dead probe).
- Forced HW-crypto (`g_mesh_sw_crypto=false`, `umac_mesh.c`).
- Generated a **deterministic** A4≠TA forward: board0 emits, from `umac_mesh_plink_tick` (umac context;
  the app beacon-cb never fires — S1G mesh-peer beacons bypass the app rx_frame_cb), a synthetic 4-addr
  frame to board2 with mesh-SA = board1 — **byte-identical to board0's working *direct* frame except A4**
  (`mmwlan_mesh_debug_synth_forward` → `umac_mesh_forward_data`).

**Result** (board0 = relay/emitter, board2 = measurer; on-air confirmed on chronium's `morse0`):
- board0 → board2 **direct** ping (A4=board0): 0% loss, ~20 ms.
- board0 emitted **739** synthetic A4≠TA forwards; **60+** captured on-air (4-addr, Protected, PN
  incrementing).
- board2 RX-entry: **direct = 291 (288 decrypted), forward = 0.** Same TA, same board0↔board2 pairwise
  MTK, differing only in A4 — the FW delivers *every* direct frame and *zero* forwards.

Positive control is airtight (291 direct delivered), so the 0 is a genuine FW withhold, not a dead
probe. §#26's phenomenon holds; its broken-probe evidence is replaced by a solid one.

## 2. FW version + BCF excluded

| host stack | FW | BCF | A4≠TA forward |
|---|---|---|---|
| morselib (ESP) | 1.17.9 | fgh100mhaamd | withhold |
| morselib (ESP) | **1.17.8** | fgh100mhaamd | **withhold** |
| morse_driver (chronosalt) | 1.17.8 | fgh100mhaamd | **deliver** (§#25) |

- Swapped the ESP's vendored FW to **1.17.8** (`vendor/morse-firmware/firmware/mm6108.bin` ← chronite's
  `~/mm6108-1.17.8.bak`), rebuilt, re-ran the synth-forward test → **still withholds**. Board banner
  confirmed `Morse firmware version: 1.17.8`. FW version excluded.
- The ESP's BCF is `bcf_fgh100mhaamd` (`BOARD=proto1-fgh100m`); its embedded `.board_desc` string is
  literally `mf16858`, so the on-board "BCF board description: mf16858" *is* the Quectel fgh100mhaamd
  BCF — the same BCF chronosalt runs while delivering. BCF excluded.

Same FW + same BCF, opposite behavior ⇒ **the only remaining variable is the host stack.**

## 3. The driver→FW command diff — functionally equivalent
Captured morse_driver's live command stream on a 2-node secured mesh (chronite + chronosalt,
`wpa_supplicant_s1g`, mesh id `rimba-smesh`, SAE, S1G ch27):
- **IDs** — kprobe on `morse_cmd_tx` (`command.c:130`) reading `message_id` at `+2(%x2)`.
- **Bytes** — rebuilt morse_driver (SPI config, matching vermagic) with the existing `MORSE_DBG` CMD
  log (`command.c:186`) enhanced to dump `len` + first 32 payload bytes, gated by the per-feature debug
  level `default=7` (keep `skb=3` to suppress the per-frame flood).

**morse_driver mesh+peer sequence (byte-verified):** `HW_SCAN · MCAST_FILTER · REMOVE_IF · GET_VERSION ·
GET_DISABLED_CHANNELS · ADD_INTERFACE(type=MESH) · GET_CAPABILITIES · SET_QOS_PARAMS×4 · SET_CHANNEL ·
GET_CHANNEL_FULL · INSTALL_KEY(own MGTK) · SET_DUTY_CYCLE · MPSW_CONFIG · BSS_CONFIG ·
MESH_CONFIG(enable_beaconing=1, mbca=1) · SET_STA_STATE×3 (aid, state 2/3/4, flags=0) · INSTALL_KEY×2
(peer MTK type=PTK idx=0 / peer MGTK type=GTK idx=1)`.

**Diff vs morselib:**
- **Peer-add matches byte-for-byte:** `SET_STA_STATE.flags=0`; `INSTALL_KEY` cipher=`AES_CCM`,
  `key_type` PTK(2)/GTK(1), `key_idx` — all identical. Only the internal `aid` index differs (1 vs 2).
- **Shared config matches:** ADD_INTERFACE type=MESH; BSS_CONFIG beacon=100/dtim=1; MESH_CONFIG
  enable_beaconing=1/mbca=1 (only `min_beacon_gap` 10 vs 25 — a beacon-timing value).
- **Every non-shared command ruled out:** `MCAST_FILTER` (multicast list `01:00:5e…`; `configure_filter`
  `mac.c:4269` ignores the FIF flags entirely), `SET_QOS_PARAMS` (EDCA/TX — *also empirically tested*:
  adding it to `mmwlan_mesh_start` did **not** fix delivery), `MPSW_CONFIG`/`SET_DUTY_CYCLE`
  (airtime/regulatory), `HW_SCAN` (morselib host-beacons), the `GET_*` reads.
- morselib-only commands (`BSSID_SET 0x52`, `SET_MESH_CONFIG 0xA018`, `BSS_BEACON_CONFIG 0x3D`) are its
  host-beacon path; none restricts RX.

**No command/flag/value in either direction gates A4-delivery — the command streams are functionally
equivalent.**

## 4. Conclusion + open next steps
The A4≠TA-forward difference is a **host-side interaction not carried by the `morse_cmd_tx` FW-command
interface.** Candidates for a future dig (all deep, low-odds, unnecessary while P5 ships):
- A host↔FW interaction *outside* `morse_cmd_tx` (raw SPI/register access, a data-path setup).
- Frame **content** the FW keys off — a Mesh Configuration IE (113) capability bit, or a
  mesh-header/QoS field morselib emits differently.
- A command sent only on data-flow / a later `BSS_CHANGED_*` event beyond the ~13 s bring-up window.

**P5 host SW-CCMP remains the working answer** (no FW mesh key ⇒ the FW delivers protected frames raw,
host decrypts, bypassing the gate for *all* origins). #20 stays a low-priority backlog item; this is the
re-verified state to resume from.

## Method / bench-safety notes (to resume)
- morselib TEMP (reverted after this session): `g_mesh_sw_crypto=false`; the `g_mesh_rx20dbg[4]` RX-entry
  counter + `mmwlan_mesh_get_rx20dbg`; the armed synthetic forward (`mmwlan_mesh_debug_synth_forward` →
  `umac_mesh_plink_tick`).
- app TEMP (`rimba-halow-mesh-perf`, reverted): `MESH_LINE_RELAY_DEMO` + board0 direct-control ping/arm
  + heartbeat RX20/synthfwd readout.
- Linux capture: `docs/reference/captures/wpa-smesh.conf` (SAE `rimbamesh2026`); the byte-log driver
  (original `.ko` backed up on chronite at `.../wireless/morse/morse.ko.preByteLog.bak`, since restored).
- **Do NOT** set `debug_mask=0x7fffffff` — bit0=DEBUG turns on per-frame logging that floods a
  beaconing node into userspace starvation (locked out chronite + chronium this session; both needed a
  reboot). Use per-feature `logging/default=7` + `logging/skb=3`. A kprobe on `morse_cmd_tx` likewise
  overloads a *live-peering* node. Reboot the Linux nodes for a clean peering state after churn.
- Follow-up cleanup: the now-refuted "FW keys CCMP by the mesh-SA/A4" comments at `umac_mesh.c:134-135`
  and `umac_datapath.c:509/727` should be corrected in a future submodule touch (the gate is real, but
  it is a host-stack difference, not a keys-by-A4 decrypt limit).
