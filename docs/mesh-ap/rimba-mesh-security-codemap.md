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
