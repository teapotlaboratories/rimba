# P3 (SAE) — implementation plan: per-peer SAE PMK for the ESP32 secured mesh

**Goal:** replace the static `mesh_p2_pmk` + the placeholder PMKID with a real per-peer SAE-derived
PMK/PMKID, by driving morselib's already-compiled `src/common/sae.c` from a native umac mesh SAE FSM.
P2 (AMPE over the static PMK) is byte-exact vs live Linux (P2d.5) and stays unchanged downstream of
the PMK seam — only the PMK *provenance* changes.

**Reference (guide, not drop-in):** `wpa_supplicant/mesh_rsn.c` + `src/ap/ieee802_11.c` `sae_sm_step`.
Those need AP `hostapd_data`/`wpa_auth`/`sta_info` infra that isn't built, so the umac FSM is ported,
not linked. Everything below the PMK seam is reused intact.

**Reused (compiled, no build change):** all of `sae.c`/`sae.h` (group 19); the auth-frame codec
`frame_authentication_build`/`_parse` (authentication.c:14/48); the `mmint_*` mbedtls backend;
`mesh_derive_mtk`/`_aek` (only their PMK source changes). **New:** per-peer `struct sae_data`
lifecycle; the native simultaneous-open SAE FSM; AUTH routing + pre-assoc allow in the mesh datapath;
a wpabuf↔raw bridge; reorder so SAE precedes the AMPE Open.

**SAE API (`src/common/sae.h`, verified):** `sae_set_group(sae,19)` ·
`sae_prepare_commit(a1,a2,pw,pwlen,sae)` · `sae_write_commit(sae,wpabuf,NULL,NULL)` →SAE body ·
`sae_parse_commit(sae,data,len,&tok,&tlen,grp,h2e,&off)` · `sae_process_commit(sae)` (→ fills
`sae->pmk[64]` group-19 pmk[32..63]=0, `pmk_len=32`, `pmkid[16]` — exactly the 32/64 convention
`mesh_derive_mtk`/`_aek` already hard-code) · `sae_write_confirm(sae,wpabuf)` ·
`sae_check_confirm(sae,data,len,&off)` · `sae_clear_data`/`sae_clear_temp_data`. PMK + PMKID are free
outputs of `sae_process_commit`; we write no derive call.

## P3a — Mesh AUTH-frame TX/RX plumbing (no FSM)
Smallest slice: emit one SAE Commit on-air + route received AUTH into a stub handler.
1. `umac_datapath.c` `frames_allowed_pre_association_mesh[]` (≈:3208) — add `MGMT/AUTH` (else RX SAE
   is filtered pre-association). The protected-bit pre-assoc drop doesn't affect SAE (auth is unprotected).
2. `umac_datapath_process_rx_mgmt_frame_mesh` (≈:3193) — `else if (subtype==AUTH) umac_mesh_handle_auth(...)`.
3. New `umac_mesh_tx_auth(peer,transaction,status,sae_body,len)` modeled on `umac_mesh_tx_peering`.
   **Frame-assembly gotcha:** `frame_authentication_build` appends `auth_data` *verbatim, no seq/status*
   for non-OPEN — so the caller blob MUST be `htole16(transaction)‖htole16(status)‖<SAE body>`.
   `.auth_alg=DOT11_AUTH_ALG_SAE`, DA=peer, SA=us.
4. New stub `umac_mesh_handle_auth` — `frame_authentication_parse` strips seq/status and returns
   `seq`(=transaction), `status_code`, and `auth_data`=SAE body (exactly what `sae_parse_commit`/
   `sae_check_confirm` consume). P3a: just log + `mesh_peer_find`.
5. **Linkage (pivotal de-risk):** umac_mesh.c includes `src/common/sae.h` + `src/utils/wpabuf.h`;
   confirm the linker resolves `sae_*`/`wpabuf_*` (driver.c references sae *types* but does not call
   the handshake — this caller path is new). Verify a trivial `sae_set_group` links before more.
**Verify:** chronium monitor — a Management/Auth frame, `auth_alg==3`, `transaction==1`, DA=peer;
byte-diff the group/scalar/element vs a live Linux Commit.

## P3b — Per-peer SAE FSM: Commit/Confirm → PMK + PMKID (AMPE still on static PMK)
`struct mesh_peer` += `struct sae_data *sae; uint8_t sae_state; unsigned sae_sync; uint8_t pmk[32];
bool pmk_valid; uint8_t pmkid[16];`. `sae` is heap (`os_zalloc`), freed in `mesh_peer_free` +
teardown (`sae_clear_data`+free). Native FSM porting `sae_sm_step` (the `mesh & MESH_ENABLED`
branches as the default):
- **Initiator** (heard candidate beacon): `sae_set_group(19)`→`sae_prepare_commit`→`sae_write_commit`
  →TX Commit → `COMMITTED`.
- **RX Commit in NOTHING** (responder): `sae_parse_commit`→reply Commit→`sae_process_commit` (PMK)
  →*mesh*: immediately `sae_write_confirm`+TX(2)→`CONFIRMED`.
- **RX Commit in COMMITTED:** `sae_process_commit`→Confirm→`CONFIRMED`.
- **RX Confirm in CONFIRMED:** `sae_check_confirm` OK → copy `sae->pmk/pmkid`→`peer`, `pmk_valid=1`,
  `ACCEPTED`, free `sae`. (= `sae_accept_sta` but STOP before `wpa_auth_pmksa_add_sae` — mesh consumes
  the PMK natively.)
- Anti-thrash (`sae_check_big_sync` via `sae_sync`) + a per-peer retry timer (re-Commit ≤3×).
**Credential:** `password="rimbamesh2026"` (Linux `sae_password`), wired as a config field.
**Verify:** monitor captures the 4-frame exchange (Commit×2, Confirm×2); the two ESP peers' derived
`peer->pmk`/`pmkid` are byte-identical (the proof the handshake is real). Group 19 + H2E must match Linux.

## P3c — Feed the SAE PMK into AMPE; real PMKID; reorder SAE-before-Open (ESP↔ESP)
- **PMK seam:** `mesh_derive_mtk` + `mesh_derive_aek` read `peer->pmk` instead of `mesh_p2_pmk`
  (the 32/64 convention is unchanged); delete `mesh_p2_pmk`.
- **PMKID seam:** `mesh_peering_params += pmkid`; `tx_peering` sets `.pmkid=peer->pmkid`;
  `build_peering` appends `peer->pmkid` (drop `mesh_pmkid_placeholder`). Add the reciprocal PMKID
  check on RX (`handle_action` after parse — mismatch drops the Open, symmetric with Linux).
- **Reorder:** drop the alloc-time `mesh_derive_aek` (no PMK yet); `peer_open`/responder start **SAE**
  first; gate both AMPE entry points on `pmk_valid`; on SAE ACCEPTED derive the AEK (now from
  `peer->pmk`) and send the AMPE Open carrying the real PMKID.
**Verify:** monitor ordering SAE×4 → AMPE Open/Confirm → ESTAB; the MPM IE PMKID == the SAE PMKID;
encrypted ping ESP↔ESP; the AMPE element bytes still match the P2 gold-standard capture (proves the
only change is PMK provenance).

## P3d — Gold standard: ESP joins a LIVE Linux SAE+AMPE mesh (the P2-unreachable milestone)
chronosalt/chronogen run `wpa_supplicant_s1g` (`sae_password='rimbamesh2026'`, group 19,
`dtim_period=1`; NOT `iw mesh join`). Match group/H2E/AKM(`00-0f-ac-08`)/mesh_id/channel/password.
**Verify (match a LIVE device):** ESP Commit/Confirm byte-diff vs chronosalt's live frames; Linux
`-dd` shows it accepts the ESP Commit, derives a PMK, and its PMKID == ESP's `peer->pmkid`
(cross-vendor dragonfly agreement — the real proof); AMPE peers; encrypted ICMP ESP↔Linux.
**Top interop risks:** group/H2E mismatch (most likely — may need `sae_prepare_commit_pt` +
status-126 H2E path); password byte-exactness; simultaneous-open timing vs Linux `mesh_auth_timer`.

## Cross-cutting
Ship a function-level new-code↔Linux code-map (each umac SAE handler → its `ieee802_11.c sae_sm_step`/
`mesh_rsn.c` line). No build gate (sae.c is unconditionally compiled) — only morselib include/link
exposure of `sae_*`/`wpabuf_*` (P3a.5). The full recon output is in the workflow task log.
