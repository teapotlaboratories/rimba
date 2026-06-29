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
**DONE + on-air PROVEN (2026-06-27).** Two ESP peers derived a byte-identical `pmkid` (`cff6be63…3212d57e`);
on-air SAE 4-frame (`auth_alg=3`, len 98 group-19) + AMPE MPM frames byte-shape-intact (no regression).
Worklog `docs/worklog/2026-06-27-mesh-security-phase3-sae.md` (§P3b); code-map `rimba-mesh-security-codemap.md` (§P3b).

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
**DONE + on-air PROVEN (2026-06-28).** SAE PMK is load-bearing: direct-peer encrypted ping 33/33 on the
SAE-derived MTK; all pairs ESTAB `pmk_valid=1/nonce_ok=1/mgtk_ok=1` with identical SAE PMKIDs; tshark shows
the MPM IE PMKID is the SAE value (not the placeholder). Designed via a recon→design→adversarial-review
workflow (the review caught the SAE-reauth key-desync → free-and-restart, the AEK-in-LISTEN guard, and the
asymmetric-timing retry reset). Worklog §P3c + code-map §P3c. The multi-hop unicast relay ping is a
separate pre-existing follow-up (next-hop pairwise MTK).

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
**Status (2026-06-28): SAE half DONE, AMPE half OPEN.** Brought up **chronite** (`3c:22:7f:37:51:38`) as
the live `wpa_supplicant_s1g` SAE node. Two beacon fixes (Mesh-Config auth byte `0x01`, beacon RSN IE) got
chronite to accept the ESP as a candidate and run SAE; three SAE-lifetime fixes (keep `sta->sae` across
CLOSE/HOLDING, re-auth→retransmit-Confirm) cracked the cross-vendor thrash → **board0 and chronite derive
the byte-identical PMKID `855627ac3141c41d7e75f0e269d10283`** (the H2E/group/password risks below did
**not** bite — group 19 + `sae_pwe=0` matched first try). **AMPE crypto: PROVEN CORRECT vs hostap** —
the AMPE AES-SIV model (AEK = `sha256_prf(PMK64,"AEK Derivation",AKM‖min‖max)`, AAD = `{own,peer,cat6}`,
cat6 = first-6 action-body bytes) was offline-validated by replaying chronite's own logged PMK + AMPE
frame through pycryptodome (`docs/mesh-ap/ampe-siv-validate.py`, VERIFIED PASS); the ESP source matches
line-for-line and `mmint_aes_siv_*` IS hostap's `aes-siv.c`.

**SAE re-sync deadlock — FIXED (task #13, committed 1cef689f/bedb07d).** The actual root cause was NOT
"board0 should retransmit its Confirm" — it was board0's SAE FSM deviating from hostap on ACCEPTED+Commit:
P3d had it resend a stale Confirm, but hostap mesh does `ap_free_sta` (reauth, ieee802_11.c:1144-1151) on a
Commit received in ACCEPTED (= a genuine peer restart; a peer mid-handshake retransmits its *Confirm*, not
a Commit). Restored `mesh_sae_reauth_free`, then an adversarial verification found the free was
unconditional (a DoS divergence — hostap frees only after the Commit clears status/parse/reflection checks)
and hardened it with a status+well-formed+non-reflection gate. Verified vs the live Linux peer: chronite now
completes SAE with board0 (`State Nothing→Committed→Confirmed→Accepted`); the gate doesn't block
genuine-restart recovery. See worklog § "#13" + code-map § #13.

**task #12 — AMPE MIC ROOT CAUSE FOUND (2026-06-28).** Captured board0's runtime AES-SIV inputs (SAE now
converges via #13): board0's `crypt` == chronite's ciphertext exactly, but the 6-byte MIC **AAD diverges** —
board0 `body[0:6]=0f011000dd0e` vs chronite `0f011000 0108` (bytes [4:5]: **S1G** vendor `dd0e` vs **legacy**
Supported-Rates `0108`). The morse driver converts received S1G→11n before mac80211/hostap
(`morse_dot11ah_s1g_to_11n_rx_packet`, mac.c:6628), so hostap MICs over 11n; **morselib MICs over raw S1G**
→ cross-vendor AAD mismatch. ESP↔ESP works (both S1G).

**task #16 DONE (2026-06-28) — CROSS-VENDOR ENCRYPTED MESH PEERING WORKS.** Fix = aad-prefix-only
(per a driver-source-read design): canonicalise the OPEN AMPE AAD `body[4:5]→01 08` (the legacy Supported
Rates EID+len the morse driver synthesises on S1G→11n RX) on both TX + RX, gated to OPEN. **Verified on the
live Linux peer chronite: ESP↔Linux SAE→AMPE→ESTAB** (chronite: `Decrypted AMPE element` both ways, `mesh
plink … established`, `iw station: ESTAB/authorized=yes`; board0 sends a Confirm — never did pre-#16).
This was the load-bearing P3d blocker. See worklog + code-map § #16.

**task #17 — DONE: ENCRYPTED ESP↔Linux ICMP WORKS = the P3d goal.** Keys agreed (MTK/MGTK byte-identical)
and the open mesh worked, so it was neither keys nor the CCMP-AAD. A workflow found it: morselib has no
group-addressed robust-mgmt TX path — `umac_datapath_tx_mgmt_frame` DROPS BC/MC robust-mgmt when PMF is
required (umac_datapath.c:2230-2237, the infra BIP case), so the broadcast HWMP PREQ/PERR was silently
dropped in a secured mesh → board0 never originated a path → Linux mpath stayed RESOLVING. Fix: route a
group/broadcast-DA HWMP through the existing MGTK group-key path (`umac_datapath_tx_mesh_group_frame`);
unicast PREP stays pairwise. **Verified on-air: board0↔chronite encrypted ICMP 0/26 → 5/5; chronite mpath
RESOLVING → ACTIVE/RESOLVED.** Full cross-vendor encrypted mesh: SAE→AMPE→ESTAB(#16)→HWMP→CCMP→ping. See
worklog + code-map § #17.

**task #18 — DONE: no-static-ARP DYNAMIC join works.** The premise ("encrypted broadcast PREQ → does the
MM6108 HW-decrypt group mgmt?") was WRONG: chronium on-air capture shows chronite's broadcast HWMP PREQ is
**unprotected** (Category MESH(13) Path Request, `protected=False`; only group DATA is MGTK-encrypted).
board0 received it but morselib's infra PMF pre-dispatch (`umac_datapath.c:356-385`) dropped the
unprotected robust group-mgmt frame before the HWMP handler → board0 never PREP'd → Linux couldn't resolve
a path to board0 (nor ARP-reply), blocking BOTH directions. This is the RX mirror of #17's TX drop. Fix
(mirrors mac80211): group-addressed MESH/MULTIHOP action frames are `_ieee80211_is_group_privacy_action`
(MGTK/group-privacy class, not BIP) and mesh peers are **MFP=no** (`iw station dump` on chronite →
`MFP: no`), so `ieee80211_drop_unencrypted_mgmt`'s MFP block is skipped — ported as
`frame_is_group_privacy_action` (umac/frames/action.c) + a guard exemption in the drop.
**Verified on-air (clean build): board0↔chronite DYNAMIC (no static ARP) — board0→chronite 0 → 46/0;
chronite→board0 0/8 → 5/5; chronite mpath `0x15` ACTIVE/RESOLVED.** See worklog + code-map § #18.

**Still open:** (a) **task #19** — encrypt the unicast mesh relay path. (b) on-air `morse0` byte-capture
(board0 RF range). (c) hardening #14/#15 + the #13 residual. (d) **task #9** — morselib `pmf_is_required`
vs mac80211 `WLAN_STA_MFP`+key-presence (mesh peers MFP=no but still key-encrypt robust mgmt).

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

## Follow-ups / open investigations (post-P3c)
- **Unicast relay forward — re-encrypt under the next-hop pairwise MTK.** `umac_mesh_forward_data`
  (`umac_mesh.c:2148`) currently relays via `umac_datapath_tx_mgmt_frame(common_stad, …)`, whose
  encryption is for robust *mgmt* frames only — so a forwarded unicast data frame goes out unprotected
  / under the wrong key, and the next hop drops it (the cause of the board1→board2 relay-ping timeout).
  Fix = mirror the merged *group*-forward fix (`umac_datapath_tx_mesh_group_frame`) but for the pairwise
  key: resolve `umac_mesh_get_peer_stad(next_hop)` + a data-path helper selecting `UMAC_KEY_TYPE_PAIRWISE`.
  **This is exactly mac80211's behaviour** — verified in `net/mac80211`: the forward handler re-injects
  the (RX-decrypted) frame with the next-hop as RA, and `ieee80211_tx_h_select_key` (`tx.c:615`) selects
  `tx->sta->ptk` = the next-hop MTK. mac80211 derives the key generically from the frame's addressing in
  one shared stage; morselib has no such stage, so each forward call site must hand-wire the stad + key
  type. `morse_driver` is soft-MAC (the forwarding + key selection are in mac80211; morse only installs
  the per-peer key to the firmware for HW CCMP).
- **mac80211-generic-key-selection vs morselib-hand-wired TX — confirm it doesn't break (A) Linux
  interop of the encrypted DATA/forward path (distinct from P3d's SAE-frame interop — byte-diff ESP
  forwarded/originated frames vs a live Linux relay) and (B) dynamic device join at runtime** (new
  beacon → SAE → ESTAB → keyed → routable, no restart). Known limits to check: per-peer stad pool fixed
  `UMAC_MESH_MAX_PEERS=4`; the relay-demo allowlist is test-only; multi-hop reachability of a new node
  needs BOTH this relay-forward fix AND TX-side PREQ origination ("we don't originate PREQs yet").
  Deliverable: a verified map of every place morselib's hand-wired key/route selection could diverge from
  mac80211 under interop + dynamic-join, with the concrete fixes.
