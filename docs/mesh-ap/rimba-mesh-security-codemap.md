# Mesh security (SAE+AMPE) — new-code ↔ Linux code-map

The function-level porting map for the ESP32 **secured 802.11s mesh** (SAE auth + AMPE-encrypted
peering + CCMP data), following Morse's Linux stack line-by-line. Companion to the open-mesh map
([rimba-mesh-80211s-code-map.md](rimba-mesh-80211s-code-map.md)); this doc covers the *security*
layer being added on top. Phase status + the live reference are in
[worklog 2026-06-27-linux-secured-mesh-reference.md](../worklog/2026-06-27-linux-secured-mesh-reference.md).

Reference revisions (pinned): kernel `MorseMicro/rpi-linux mm/rpi-6.12.21/1.17.x` @ `372414fd`;
`morse_driver` 1.17.8; `hostap` 1.17.8 @ `4acb6f6f`. Every cited line was grep-verified 2026-06-27
in both trees (ESP working tree + the chronite `~/halow` reference). ESP side under
`components/halow/.../morselib/src/`.

## Architecture (from recon)

Secured Linux mesh is split: **wpa_supplicant** does SAE + AMPE + the MPM FSM + frame build/parse
with MIC + MTK/MGTK/IGTK derivation; **the kernel/driver** only advertises PRIVACY, creates a
per-peer station handle, moves it to ASSOC/AUTHORIZED, and installs keys. For morselib (no
wpa_supplicant; MPM is host-side in `umac_mesh.c`) the port reproduces *both* halves in the umac
mesh path. Phases: **P0** live Linux ref (done) · **P1** key-install plumbing (static keys) ·
**P2** AMPE · **P3** SAE · **P4** MFP/IGTK + integration.

## The −116 question — RESOLVED (mesh uses the AP path)

A MESH_POINT vif runs the **identical** `.set_key`/`.sta_state` code as AP, and a mesh peer gets a
real firmware station handle (AID). The ADHOC −116 does NOT apply to mesh.

| Evidence (morse_driver) | Where |
|---|---|
| AID bitmap alloc/free gated on `AP \|\| MESH_POINT` | `mac.c:4933` (set), `mac.c:4977` (clear) |
| `morse_mac_vif_init_ap` runs for mesh ("AP/Mesh/IBSS") | `mac.c:3223` (doc), `mac.c:3412` (call for MESH) |
| set_key/sta_state AID = `sta->aid` for the non-STA/non-ADHOC (=mesh) arm | `mac.c:5169`, `mac.c:4851-4862` |
| Builders have zero iftype checks | `command.c:963` (sta_state), `command.c:1009` (install_key) |

## Firmware command ABI (INSTALL_KEY 0x0A, SET_STA_STATE 0x14) — both sides match

| Field | morse_driver (Linux authority) | morselib (ESP) | Match |
|---|---|---|---|
| opcode INSTALL_KEY | `MORSE_CMD_ID_INSTALL_KEY=0x0A` `morse_commands.h` | same, `common/morse_commands.h:24` | ✓ |
| opcode SET_STA_STATE | `=0x14` | `common/morse_commands.h:29` | ✓ |
| install-key req struct | `morse_cmd_req_install_key` `morse_commands.h:1129` | `common/morse_commands.h:806` | ✓ `{pn, aid(u32), key_idx, cipher, key_length, key_type, key[32]}` |
| sta-state req struct | `morse_cmd_req_set_sta_state` `morse_commands.h:696` | `common/morse_commands.h:494` | ✓ `{sta_addr[6], aid(u16), state, flags}` |
| cipher AES_CCM=1 | `enum morse_cmd_key_cipher` | `:779` | ✓ |
| key_type PTK=2/GTK=1/IGTK=3 | `enum morse_cmd_temporal_key_type` | `:800` | ✓ |
| state AUTHORIZED=4 | `enum morse_cmd_ieee80211_sta_state` | `:911` | ✓ |

**Peer identity:** INSTALL_KEY keys by the 32-bit **AID** (not MAC); SET_STA_STATE carries **MAC + AID**.

## morselib machinery to REUSE (the AP key-install path)

| morselib symbol | file:line | Role |
|---|---|---|
| `mmdrv_update_sta_state(vif_id, aid, addr, state)` | `driver/driver.c:1058` | builds+sends SET_STA_STATE |
| `mmdrv_install_key(vif_id, aid, key_conf)` | `driver/driver.c:1093` | builds+sends INSTALL_KEY (hardcodes AES_CCM; len 16→128/32→256; PTK if pairwise) |
| `umac_keys_install_key(stad, vif_id, key)` | `umac/keys/umac_keys.c:53` | umac key layer; AID via `umac_sta_data_get_aid(stad)` `:58` |
| `morse_cmd_tx(...)` | `driver/morse_driver/command.c:45` | the command-send helper (≡ Linux `morse_cmd_tx`) |
| AP STA-state emit | `umac/ap/umac_ap.c:623` | `mmdrv_update_sta_state(...)` call to mirror |
| AP AID alloc (hostapd-assigned → array index) | `umac/ap/umac_ap.c:509` `umac_ap_alloc_sta` | AP gets AID from hostapd; **mesh has no equivalent → gap** |
| Linux analog (where the kernel installs at link establish) | hostap `mesh_mpm.c:916` `mesh_mpm_plink_estab` | the model for the ESTAB-edge calls |

## The mesh GAP + Phase-1 hooks (where new code goes)

`struct mesh_peer` (`umac/mesh/umac_mesh.c:294`) = `{used, mac[6], llid, plid, state, retries, last_rx_ms}`
— **no AID, no per-peer `umac_sta_data`**. The MPM reaches ESTAB at two edges in
`umac_mesh_handle_action`, which today only flip state + bring the netif up (no key/sta commands):
- OPN_RCVD + CONFIRM → ESTAB: `umac_mesh.c:1469` (+ `umac_mesh_link_up_once` `:1471`)
- CNF_RCVD + OPEN → ESTAB: `umac_mesh.c:1483` (+ `:1485`)

**Phase-1 — exact sequence (derived from the Linux MESH path, NOT the AP association flow):**

The ordered checklist a port must reproduce (from `mesh_rsn.c` + `mesh_mpm.c` + how `morse_driver`
forwards it):

| # | When | Linux source | Firmware command (morselib) |
|---|---|---|---|
| A | mesh **start** (once, before any peer) | `__mesh_rsn_auth_init` `mesh_rsn.c:227` — own MGTK, addr=broadcast, key_idx=1, `KEY_FLAG_GROUP_TX_DEFAULT` | `INSTALL_KEY{key_type=GTK, **aid=0**, key_idx=1}` (own group TX key) |
| — | (own IGTK `mesh_rsn.c:216`) | BIP → morse handles in **software** (`mac.c:5187`) | *not sent to FW* |
| B0 | peer up to AUTHORIZED | `wpa_mesh_set_plink_state` / SET_STATION plink-state → mac80211 steps states; morse **filters NOTEXIST/NONE** (`mac.c:4813`) | `SET_STA_STATE(AUTH)` → `(ASSOC)` → `(AUTHORIZED)`, aid=peer (AUTH creates the FW station) |
| B1 | at ESTAB | `mesh_mpm_plink_estab` `mesh_mpm.c:928` — MTK, `KEY_FLAG_PAIRWISE_RX_TX` | `INSTALL_KEY{key_type=PTK, aid=peer, key_idx=0}` |
| B2 | at ESTAB | `mesh_mpm.c:938` — peer MGTK, `KEY_FLAG_GROUP_RX` | `INSTALL_KEY{key_type=GTK, aid=peer, key_idx=1}` |
| — | (peer IGTK `mesh_mpm.c:945`) | BIP → software (`mac.c:5187`) | *not sent to FW* |

**Implemented + VERIFIED on hardware (2026-06-27, feature/mesh-security-phase1):** all five firmware
commands return **0** on a MESH vif (`SET_STA_STATE` AUTH/ASSOC/AUTHORIZED + `INSTALL_KEY` MTK/MGTK)
— the −116 does not block mesh. `struct mesh_peer.aid` = pool-index+1 (Linux assigns it in
wpa_supplicant `hostapd_get_aid` `mesh_mpm.c:798`; morselib self-assigns — platform divergence).
`umac_mesh_peer_secure_estab()` (rows B0–B2) called at both ESTAB edges (`umac_mesh.c:1469`/`:1483`).
The `mmdrv_install_key`/`mmdrv_update_sta_state` wrappers + `morse_cmd_tx` are the shared driver layer
(same commands Linux `morse_driver` emits) — only the *sequence* comes from the mesh path.

**Forced divergence — row A (own MGTK) is DEFERRED, not at mesh start.** Installing the own group key
(aid=0) before peering flips the firmware to expect protected frames, which **breaks morselib's OPEN
MPM peering** (unprotected Open/Confirm dropped → no ESTAB; proven by a clean PHASE1=1-vs-0 A/B). Linux
gets away with the start-time install only because its peering is **AMPE-protected from the start**. So
the own-MGTK install is deferred until P2 (AMPE peering); the per-peer keys at ESTAB are enough to prove
the boundary. Keys are static/shared (Phase 1); P2 derives the real MTK + exchanges MGTK via AMPE.
Details: [worklog 2026-06-27-mesh-security-phase1-keyinstall.md](../worklog/2026-06-27-mesh-security-phase1-keyinstall.md).

P2 then replaces the static keys with real AMPE-derived MTK + AES-SIV MIC (hostap `mesh_rsn.c`);
P3 replaces the static PMK with SAE (`sae.c` + mbedTLS P-256); P4 adds IGTK/MFP. Each verified
on-air against the live reference via chronium's monitor (`morse0`).

## P3b — SAE FSM (per-peer Dragonfly handshake → PMK + PMKID)

The Dragonfly crypto is hostap's compiled `src/common/sae.c`, reached through the `mesh_sae` shim
(`src/hostap/mesh_sae.c`, P3a). `sae.c` is **pure crypto** — it never advances `sae->state`; the
simultaneous-open **state machine** lives in `src/ap/ieee802_11.c` (`sae_sm_step` + `handle_auth_sae`)
with the per-peer kick + key derivation in `wpa_supplicant/mesh_rsn.c`. P3b ports that FSM natively
into `umac_mesh.c` (morselib has no wpa_supplicant). Every Linux line below was grep-verified
2026-06-27 in this tree (`.../framework/src/hostap/`). ESP side = `.../morselib/src/umac/mesh/umac_mesh.c`.

**Scope:** SAE runs alongside the P2 MPM/AMPE flow and is **not yet load-bearing** — AMPE still keys
off the static `mesh_p2_pmk`. P3b's only output is `peer->pmk`/`peer->pmkid`; **P3c** does the PMK
seam + the SAE-before-Open reorder.

| ESP new code (`umac_mesh.c`) | Linux/hostap reference (file:line) | Notes |
|---|---|---|
| `mesh_sae_start` (:815) — alloc + build/send Commit → `COMMITTED` | `mesh_rsn.c:371` `mesh_rsn_auth_sae_sta` + `ieee802_11.c:1696` `auth_sae_init_committed` | Commit built **once**, bytes cached (build is destructive: `sae_set_group`→`sae_clear_data`) |
| `mesh_sae_handle_rx` (:915) — parse + dispatch | `ieee802_11.c:1329` `handle_auth_sae` + `ieee802_11.c:990` `sae_sm_step` | mesh branches only |
| ↳ lazy responder alloc on first Commit | `ieee802_11.c:1375-1390` | txn==1 + success only |
| ↳ `COMMITTED`+Commit → process + Confirm → `CONFIRMED` | `ieee802_11.c:1074-1083` | the mesh "send Confirm" |
| ↳ `CONFIRMED`+Commit → resend Commit + reprocess + Confirm | `ieee802_11.c:1121-1137` | resync (peer restart) |
| ↳ `COMMITTED`+Confirm → resend Commit (body not validated) | `ieee802_11.c:1084-1097` (+ gate `:1584`) | mesh skips `check_confirm` < CONFIRMED |
| ↳ `CONFIRMED`+Confirm → `check_confirm` + `get_keys` → `ACCEPTED` | `ieee802_11.c:1138-1140` + `sae_accept_sta` `:940`; PMK copy `:983` | **PMK/PMKID land here** |
| ↳ `ACCEPTED`+Confirm → replay cached Confirm | `ieee802_11.c:1163-1172` | lets a stuck peer complete |
| ↳ `ACCEPTED`+Commit → `mesh_sae_restart_in_place` (:897) | `ieee802_11.c:1144-1151` | **P3b divergence:** Linux `ap_free_sta`s; P3b re-derives in place (SAE not gating yet) |
| `mesh_sae_tx_confirm` (:853) | `ieee802_11.c:756` `auth_sae_send_confirm` / `sae.c:2354` `sae_write_confirm` | caches bytes for ACCEPTED replay |
| SAE retransmit in `umac_mesh_plink_tick` (~:1090) | `ieee802_11.c:861-895` `auth_sae_retransmit_timer` + `:904-913` `sae_set_retransmit_timer` | ~1 s, mesh-only, independent of MPM-Open retransmit |
| `mesh_sae_check_big_sync` (:884) / `mesh_sae_big_sync_reset` (:868) | `ieee802_11.c:821-836` `sae_check_big_sync` | cap `MESH_SAE_SYNC_MAX=3` (== `conf->sae_sync`, strict `>`), 10 s lockout |
| `mesh_sae_clear_temp` shim (`mesh_sae.c:152`) → `sae_clear_temp_data` | `ieee802_11.c:1169` | drop bignums at accept, keep pmk/pmkid |
| `umac_mesh_handle_auth` (:1037) — find peer → drive FSM | `ieee802_11.c:3315-3334` (SAE case) | unknown-peer auth dropped (== `mesh_pending_auth` `:3160-3177`) |
| `mesh_sae_start` at discovery: `mmwlan_mesh_peer_open` + `umac_mesh_handle_action` new-peer | `mesh_mpm.c:899` `mesh_rsn_auth_sae_sta` | **P3b keeps the Open at discovery** (`mesh_mpm.c:897` SEC_NONE shape); P3c reorders |
| `peer->sae` free in `mesh_peer_free` | `sta_info.c:427-428` `sae_clear_data` + `os_free` | one site, all 5 teardown paths |
| `mesh_sae_get_keys` → `peer->pmk`(32)/`pmkid`(16) | `sae.c:1637-1639`; `mesh_rsn_get_pmkid` `mesh_rsn.c:435` | 32/64 convention matches `mesh_derive_mtk`(32)/`_aek`(64) → P3c is a source swap |

**Deferred to P3c/P3d (intentional divergences, flagged so the byte-diff isn't misread):** SAE-before-Open
reorder + `pmk_valid` gating (`mesh_mpm.c:894-900`, `mesh_mpm_auth_peer` `:679-717`); the RX SAE-accept
drop gate (`mesh_mpm.c:1272-1278`); real PMKID in the MPM IE + `chosen_pmk` check (`mesh_rsn.c:689-695`);
the `sae->rc` send-confirm anti-replay (`ieee802_11.c:1599-1616`); H2E status path (126/127). PMKSA
caching (`mesh_rsn.c:392-413`) is not ported (no `wpa_auth` PMKSA cache on morselib).
