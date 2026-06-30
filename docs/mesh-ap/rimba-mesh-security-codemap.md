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

## Secured peering — code flow (SAE + AMPE)

The handshake that brings a mesh link to ESTAB **before** any data flows — the SW-CCMP data path below
only runs once a peer is secured. morselib has no wpa_supplicant, so the SAE Dragonfly FSM, the AMPE
protect/process, and the MPM plink FSM are *all* host-side in `umac/mesh/umac_mesh.c`; only the Dragonfly
crypto is delegated, reached through the `src/hostap/mesh_sae.c` shim around hostap's compiled `sae.c`.
Three blocks below — **DISCOVERY**, **SAE**, **AMPE** — run left-to-right across the six stages; the two
**P3c** reorder seams are boxed (★ the SAE-before-Open *defer*, ★ the `pmk_valid`/PMKID *gate*). All
file:line are the current working tree, grep-verified 2026-06-29.

```
DISCOVERY  (S1G beacon RX → Mesh-ID/RSN match → candidate peer + SAE kick)
──────────────────────────────────────────────────────────────────────────────────
  FW ─► umac_datapath_process_s1g_beacon                       datapath.c:142
          foreign-BSSID S1G beacon + umac_mesh_is_active()
          strip S1G beacon fields (NEXT_TBTT/CSSID/ANO)        datapath.c:168-179
          → umac_mesh_handle_peer_beacon(source_addr, ies)     datapath.c:179
        │
        ▼
  umac_mesh_handle_peer_beacon                                 umac_mesh.c:1261
     scan IEs for Mesh ID(114) == ours                         umac_mesh.c:1280
     known peer → last_rx_ms heartbeat ; unknown → open        umac_mesh.c:1284-1291
        │
        ▼
  mmwlan_mesh_peer_open                                        umac_mesh.c:1501
     mesh_peer_allowed (forced-topology allowlist)             umac_mesh.c:1511
     mesh_peer_alloc → struct mesh_peer{llid, PLINK_LISTEN}    umac_mesh.c:1519 (alloc :518)
     mesh_sae_start(peer)   ← MPM Open DEFERRED until SAE accept   umac_mesh.c:1528


SAE — Dragonfly simultaneous-open FSM   (Nothing→Committed→Confirmed→Accepted ⇒ PMK+PMKID)
──────────────────────────────────────────────────────────────────────────────────────────
  mesh_sae_start                                               umac_mesh.c:855
     mesh_sae_alloc()                              (shim) mesh_sae.c:26
     mesh_sae_build_commit()  DESTRUCTIVE, cached ONCE         umac_mesh.c:874 → mesh_sae.c:43
     umac_mesh_tx_auth(txn=1 Commit)                           umac_mesh.c:882 (def :786)
     peer->sae_state = COMMITTED                               umac_mesh.c:883
        │
        │   ── peer auth frames arrive ──
        ▼
  FW ─► umac_datapath_process_rx_mgmt_frame_mesh               datapath.c:3461
          subtype AUTH → umac_mesh_handle_auth                 datapath.c:3475
        ▼
  umac_mesh_handle_auth                                        umac_mesh.c:1111
     frame_authentication_parse → seq/status/body             umac_mesh.c:1115
     alg == SAE ; mesh_peer_find(sa) (drop unknown peer)      umac_mesh.c:1119/1128
     → mesh_sae_handle_rx(txn, status, body)                   umac_mesh.c:1141
        ▼
  mesh_sae_handle_rx   (≡ handle_auth_sae + sae_sm_step)       umac_mesh.c:952
     lazy responder alloc on first Commit                      umac_mesh.c:966-977
     ┌ txn=1 Commit ──────────────────────────────────────────────────────────────┐
     │ COMMITTED : mesh_sae_process_commit → tx_confirm → CONFIRMED  :985-993 (shim :76)
     │ CONFIRMED : resync — resend Commit + reprocess + Confirm      :995-1005
     │ ACCEPTED  : genuine restart? → mesh_sae_reauth_free            :1007-1031 (:943)
     └──────────────────────────────────────────────────────────────────────────────┘
     ┌ txn=2 Confirm ─────────────────────────────────────────────────────────────┐
     │ COMMITTED : body NOT validated — resend Commit (resync)        :1045-1056
     │ CONFIRMED : mesh_sae_check_confirm → mesh_sae_get_keys     :1059-1063 (shim :119/:127)
     │            ★ THE ACCEPT: peer->pmk(32)+pmkid(16), pmk_valid=1  :1063-1065
     │            ┌──── SAE-accept hook (≡ mesh_mpm_auth_peer) — fires once ───┐
     │            │ if peer->state == PLINK_LISTEN:                    :1078    │
     │            │   mesh_derive_aek(peer)   AEK = PRF(PMK64)         :1080    │
     │            │   umac_mesh_tx_peering(OPEN)  first PROTECTED Open :1082    │
     │            │   peer->state = PLINK_OPN_SNT                      :1083    │
     │            └──────────────────────────────────────────────────────────┘
     │            sae_state=ACCEPTED ; mesh_sae_clear_temp(keep pmk)  :1088-1090 (shim :152)
     │ ACCEPTED  : replay cached Confirm (lets a stuck peer complete)  :1093-1100
     └──────────────────────────────────────────────────────────────────────────────┘
  mesh_sae_tx_confirm: build_confirm + cache + tx_auth(txn=2)  umac_mesh.c:893 → mesh_sae.c:97


AMPE — protected MPM Open/Confirm → MTK/MGTK derive → key install → ESTAB
──────────────────────────────────────────────────────────────────────────────────
  TX side (our Open / Confirm)
  umac_mesh_tx_peering(action, peer)                           umac_mesh.c:750
     build_mgmt_frame(umac_mesh_build_peering)                 umac_mesh.c:771
  umac_mesh_build_peering                                      umac_mesh.c:627
     cat = SELF_PROTECTED + action                             umac_mesh.c:641
     RSN IE(48, AKM=SAE) + Mesh-Config IE                      umac_mesh.c:662/669
     MPM IE(117): proto=AMPE + llid[+plid] + PMKID(SAE)        umac_mesh.c:678-697
     AMPE element(139): suite=CCMP · my_nonce · peer_nonce ·
        (Open) my_mgtk+RSC+expiry   → MIC IE(140)              umac_mesh.c:704-728
     AAD = {TA=us, RA=peer, body[0:6]} (#16: Open body[4:5]=01 08)  umac_mesh.c:730-743
     mmint_aes_siv_encrypt(aek) → SIV tag + ciphertext         umac_mesh.c:745
        │
        │   ── peer Open/Confirm arrive (subtype ACTION) ──
        ▼
  FW ─► …process_rx_mgmt_frame_mesh → umac_mesh_handle_action  datapath.c:3471
  umac_mesh_handle_action                                      umac_mesh.c:2380
     cat MESH(13)        → umac_mesh_handle_hwmp (data path)   umac_mesh.c:2394-2400
     cat SELF_PROTECTED                                        umac_mesh.c:2403
     mesh_parse_peering_ie → plid, llid-echo, mic_off, PMKID   umac_mesh.c:2418 (def :1545)
     inbound-Open responder: mesh_peer_alloc + mesh_sae_start  umac_mesh.c:2434-2442
     CLOSE: plink→LISTEN (keep SAE) | ESTAB/no-SAE → free      umac_mesh.c:2452-2474
     ★ pmk_valid gate (drop peering pre-SAE)                   umac_mesh.c:2481
     ★ reciprocal PMKID memcmp (drop-only, no teardown)        umac_mesh.c:2490
     mesh_process_ampe (verify SIV MIC + learn)                umac_mesh.c:2534 (def :1614)
        mmint_aes_siv_decrypt(aek) — MIC verify                umac_mesh.c:1645
        learn peer_nonce(off 6) ; (Open) peer_mgtk(off 70)+RSC  umac_mesh.c:1672/1677
        │
        ▼  MPM plink FSM
     LISTEN  +Open  → tx Open + Confirm   → OPN_RCVD           umac_mesh.c:2549-2560
     OPN_SNT +Open  → tx Confirm          → OPN_RCVD           umac_mesh.c:2562-2567
     OPN_SNT +Conf                        → CNF_RCVD           umac_mesh.c:2568-2571
     OPN_RCVD+Conf                        → ESTAB ─┐           umac_mesh.c:2574-2583
     CNF_RCVD+Open  → tx Confirm          → ESTAB ─┤           umac_mesh.c:2590-2601
                                                   ▼
  umac_mesh_link_up_once (netif up, first peer)    umac_mesh.c:2579/2596 (def :1304)
  umac_mesh_peer_secure_estab(peer)                umac_mesh.c:2581/2598 (def :1442)
     SET_STA_STATE AUTH→ASSOC→AUTHORIZED           umac_mesh.c:1452-1456 (mmdrv_update_sta_state :1454)
     mesh_derive_mtk  PRF(PMK32 · min/max nonces · LIDs · MACs)   umac_mesh.c:1474 (def :1375)
     mesh_derive_aek  PRF(PMK64)                                  umac_mesh.c:1475 (def :1412)
     INSTALL_KEY MTK   pairwise, idx0 → umac_keys_install_key     umac_mesh.c:1479-1481
     INSTALL_KEY peer MGTK  group-RX, idx1                        umac_mesh.c:1492-1496
       (own MGTK TX key installed at mesh start; peer IGTK = SW BIP, not sent to FW)
     ⇒ plink ESTAB, secured
```

*Linux mirror:* the same six stages, but split across three processes — the **kernel** (`net/mac80211`)
only discovers the peer and installs keys, while **wpa_supplicant** (hostap) runs SAE, AMPE, and the
authoritative MPM FSM. DISCOVERY is the *only* stage that stays in-kernel: a secured mesh punts the
candidate up to userspace via `cfg80211_notify_new_peer_candidate` instead of running the in-kernel plink
FSM (that FSM is the open-mesh path, bypassed when security is on). Lines below grep-verified on chronite
`~/halow` (hostap 1.17.8 `4acb6f6f`, `rpi-linux/net/mac80211`).

```
LINUX SECURED-PEERING REFERENCE  —  net/mac80211 (kernel) discovers + installs keys;
wpa_supplicant (hostap) runs SAE + AMPE + the MPM FSM.  Every file:line grep-confirmed on
chronite (~/halow). Kernel = rpi-linux/net/mac80211; the rest are hostap (file shown per block).

══ 1. DISCOVERY  (kernel mac80211) ══════════════════════════════════════════════════════
  driver RX (S1G/legacy beacon or ProbeResp)
        │
        ▼
  ieee80211_mesh_rx_queued_mgmt                              mesh.c:1681
     stype BEACON|PROBE_RESP →
  ieee80211_mesh_rx_bcn_presp                                mesh.c:1456  (dispatch :1701)
     ieee802_11_parse_elems(beacon IEs)
     ├─ secure/unsecure-match gate                           mesh.c:1486
     │    (elems->rsn present XOR security==SEC_NONE) → drop on mismatch
     └─ mesh_matches_local(sdata, elems)                     mesh.c:62
          mesh_id + meshconf_auth(==SAE) + cfg compare       mesh.c:87
          (peer advertised it: beacon mesh_auth_id byte :291; mesh_add_rsn_ie :374 → beacon :1108)
        │ matches
        ▼
  mesh_neighbour_update                                      mesh_plink.c:630
     mesh_sta_info_get                                       mesh_plink.c:592
        mesh_sta_info_alloc                                  mesh_plink.c:553
          user_mpm || SEC_AUTHED ⇒ NO in-kernel sta; punt the candidate to userspace:
             cfg80211_notify_new_peer_candidate              mesh_plink.c:569
          (open-mesh path = __mesh_sta_info_alloc :525 + in-kernel mesh_plink_fsm :871 /
           mesh_plink_establish :845 — BYPASSED for a secured mesh)
        │
        ▼   NL80211 NEW_PEER_CANDIDATE event ───────────────►  wpa_supplicant

══ 2. SAE  (Dragonfly)  —  ieee802_11.c FSM + common/sae.c crypto ════════════════════════
  EVENT_NEW_PEER_CANDIDATE                                   events.c:7052
     wpa_mesh_notify_peer (mesh.c:654) → wpa_mesh_new_mesh_peer  mesh_mpm.c:851  (call mesh.c:667)
        mesh_mpm_add_peer (alloc sta_info, driver add)       mesh_mpm.c:725
        conf->security != NONE  →
  mesh_rsn_auth_sae_sta                                      mesh_rsn.c:371  (call mesh_mpm.c:899)
     sta->sae = zalloc                                       mesh_rsn.c:387
     (PMKSA-cache shortcut: wpa_auth_pmksa_get → sae_accept_sta :411, SKIP fresh SAE)
     mesh_rsn_build_sae_commit                               mesh_rsn.c:340  (call :416)
        sae_set_group       (sae.c:26)                       mesh_rsn.c:328
        sae_prepare_commit  (sae.c:1348) → own scalar+elem   mesh_rsn.c:364
     auth_sae_init_committed                                 ieee802_11.c:1679 (call :423)
        auth_sae_send_commit (:713) → sae_write_commit       sae.c:1674
        sae_set_state(COMMITTED)                              [FSM: NOTHING → COMMITTED]

  ── RX peer Authentication frame (txn 1 = Commit, txn 2 = Confirm) ──
  events.c:6715 → mesh_mpm_mgmt_rx (mesh_mpm.c:904) → ieee802_11_mgmt → handle_auth
     case WLAN_AUTH_SAE                                       ieee802_11.c:3313
        (mesh: lazy-init sta->wpa_sm)                         ieee802_11.c:3315
     handle_auth_sae                                          ieee802_11.c:1326 (call :3329)
        lazy responder: if !sta->sae → zalloc + state NOTHING ieee802_11.c:1372
        sae_sm_step                                           ieee802_11.c:987
        ┌──────────────────────────────────────────────────────────────────────────┐
        │ case SAE_COMMITTED + Commit(txn1)                   ieee802_11.c:1069      │
        │   sae_process_commit (:1072) → sae_derive_keys      sae.c:1662 → :1520     │
        │        → PMK (:1637) + PMKID (:1639) computed                              │
        │   auth_sae_send_confirm (:1075) → sae_write_confirm sae.c:2354 (ap :753)   │
        │   sae_set_state(CONFIRMED)                          ieee802_11.c:1078      │
        │ case SAE_CONFIRMED + Confirm(txn2)                  ieee802_11.c:1116      │
        │   sae_check_confirm (verify peer Confirm)           sae.c:2395             │
        │   sae_accept_sta                                    ieee802_11.c:1137→:937 │
        │     wpa_auth_pmksa_add_sae (PMK → PMKSA cache)      ieee802_11.c:980       │
        │     sae_set_state(ACCEPTED)                         ieee802_11.c:976       │
        └──────────────────────────────────────────────────────────────────────────┘
        (retransmit: auth_sae_retransmit_timer :858 / sae_set_retransmit_timer :901)
        [FSM: COMMITTED → CONFIRMED → ACCEPTED]

══ 3. MPM OPEN  (deferred until SAE ACCEPTS)  —  AEK derive + first protected Open ═══════
  sae_accept_sta (ieee802_11.c:937) ─ wpa_auth start_ampe callback ─►
     wpa_auth_start_ampe (wpa_auth.c:357, trigger :2318) → cb->start_ampe
  auth_start_ampe                                            mesh_rsn.c:127 (registered :171)
     mesh_mpm_auth_peer                                      mesh_rsn.c:141
  mesh_mpm_auth_peer                                         mesh_mpm.c:679
     sta->flags |= WLAN_STA_AUTH                             mesh_mpm.c:695
     mesh_rsn_init_ampe_sta                                  mesh_rsn.c:536 (call :697)
        random_get_bytes(my_nonce)  + zero peer_nonce        mesh_rsn.c:538
        mesh_rsn_derive_aek (call :543)                      mesh_rsn.c:442
           sha256_prf(sae->pmk, 64, "AEK Derivation", …)     mesh_rsn.c:468   ← AEK from PMK
     wpa_drv_sta_add{AUTHENTICATED|AUTHORIZED}               mesh_mpm.c:707   (SET_STATION→kernel)
     mesh_mpm_init_link (my_lid)                             mesh_mpm.c:722
     mesh_mpm_plink_open(PLINK_OPN_SNT)                      mesh_mpm.c:716 (def :546)
        mesh_mpm_send_plink_action(PLINK_OPEN)               mesh_mpm.c:209
           mesh_rsn_protect_frame                            mesh_rsn.c:553 (call :433)
              AMPE IE: CCMP suite, local_nonce=my_nonce,     mesh_rsn.c:594
                       peer_nonce
              OPEN only: append own MGTK ‖ Key RSC ‖ exp     mesh_rsn.c:610
              aes_siv_encrypt(aek, ampe_ie,                  mesh_rsn.c:642
                 aad={own,peer,category})  → MIC IE
        wpa_mesh_set_plink_state(OPN_SNT)
  (own group TX key was installed once at mesh start:
     __mesh_rsn_auth_init :163 → own MGTK aid=broadcast      mesh_rsn.c:227)

══ 4. AMPE  (RX peer Open/Confirm → verify SIV, learn nonce+MGTK, derive MTK) ════════════
  events.c:5645 → mesh_mpm_action_rx                         mesh_mpm.c:1162
     ap_get_sta(sa)                                          mesh_mpm.c:1256
     SAE gate: sta->sae && state != SAE_ACCEPTED → return    mesh_mpm.c:1273
     mesh_rsn_process_ampe                                   mesh_rsn.c:657 (call :1286)
        aes_siv_decrypt(aek, …) — verify MIC                 mesh_rsn.c:724   (res=-2 → OPN_RJCT)
        chosen_pmk == sta->sae->pmkid ?                      mesh_rsn.c:691
        learn peer_nonce = ampe->local_nonce                 mesh_rsn.c:755
        learn peer MGTK from GTKdata (Open only)
     compute event:  PLINK_OPEN→OPN_ACPT (:1374) /           mesh_mpm.c:1374
                     PLINK_CONFIRM→CNF_ACPT (:1395)          mesh_mpm.c:1395
     mesh_mpm_fsm(event)                                     mesh_mpm.c:974 (call :1434)
        OPN_SNT + OPN_ACPT → OPN_RCVD + send Confirm         mesh_mpm.c:1023
        OPN_RCVD + CNF_ACPT → derive_mtk + plink_estab       mesh_mpm.c:1064 / :1065
        CNF_RCVD + OPN_ACPT → derive_mtk + plink_estab +     mesh_mpm.c:1092 / :1093
                              send Confirm
        mesh_rsn_derive_mtk                                  mesh_rsn.c:474
           sha256_prf(sae->pmk, 32, "Temporal Key Deriv",    mesh_rsn.c:529
              min/max(nonce)‖min/max(lid)‖AKM‖min/max(MAC))  ← MTK (pairwise)
        [FSM: OPN_SNT → OPN_RCVD/CNF_RCVD → (key install) → ESTAB]

══ 5. KEY INSTALL + 6. ESTAB ════════════════════════════════════════════════════════════
  mesh_mpm_plink_estab                                       mesh_mpm.c:916
     wpa_drv_set_key  MTK   KEY_FLAG_PAIRWISE_RX_TX          mesh_mpm.c:928   ┐
     wpa_drv_set_key  peer MGTK  KEY_FLAG_GROUP_RX           mesh_mpm.c:938   ├─ nl80211 .set_key
     (wpa_drv_set_key peer IGTK  KEY_FLAG_GROUP_RX :950)     mesh_mpm.c:950   ┘  → morse_driver
                                                                                  → INSTALL_KEY (FW HW crypto)
     wpa_mesh_set_plink_state(PLINK_ESTAB)                   mesh_mpm.c:960
     wpas_notify_mesh_peer_connected                         mesh_mpm.c:971
        → plink ESTABLISHED, secured  (iw station: "mesh plink ESTAB, authorized: yes")
```

**Stage correspondence (Linux → ESP):**

| Stage | net/mac80211 + hostap (reference) | morselib (this port) |
| --- | --- | --- |
| **Discovery** — beacon auth-id + RSN match → candidate peer | `ieee80211_mesh_rx_bcn_presp` mesh.c:1456 (RSN gate :1486) → `mesh_matches_local` mesh.c:62/:87 → `cfg80211_notify_new_peer_candidate` mesh_plink.c:569 | `umac_mesh_handle_peer_beacon` (Mesh-ID match) umac_mesh.c:1280 → `mmwlan_mesh_peer_open` :1501 → `mesh_peer_alloc` :518 |
| **SAE Commit** — send Commit → COMMITTED | `mesh_rsn_auth_sae_sta` mesh_rsn.c:371 + `auth_sae_init_committed` ieee802_11.c:1679 (`sae_write_commit` sae.c:1674) | `mesh_sae_start` umac_mesh.c:855 (`mesh_sae_build_commit` shim mesh_sae.c:43; `umac_mesh_tx_auth` txn=1 :882) |
| **SAE Confirm** — RX FSM → accept → PMK+PMKID, send Confirm | `handle_auth_sae` ieee802_11.c:1326 / `sae_sm_step` :987; `sae_check_confirm` sae.c:2395 → `sae_accept_sta` ieee802_11.c:937 (PMK :980); `sae_write_confirm` sae.c:2354 | `umac_mesh_handle_auth` :1111 → `mesh_sae_handle_rx` :952; `mesh_sae_check_confirm`/`get_keys` → `pmk_valid` umac_mesh.c:1063-1065; `mesh_sae_tx_confirm` :893 |
| **AEK derive + deferred MPM Open** — fires once at SAE accept | `mesh_mpm_auth_peer` mesh_mpm.c:679 → `mesh_rsn_derive_aek` mesh_rsn.c:468 (PMK64) → `mesh_mpm_plink_open` :716 | SAE-accept hook umac_mesh.c:1078-1083 (`mesh_derive_aek` :1080, `umac_mesh_tx_peering(OPEN)` :1082) |
| **AMPE Open** — build protected Open/Confirm + SIV | `mesh_rsn_protect_frame` mesh_rsn.c:553 (AMPE IE :594, own MGTK :610, `aes_siv_encrypt` :642) | `umac_mesh_build_peering` umac_mesh.c:627 (AMPE elem :704, own MGTK :728, `mmint_aes_siv_encrypt` :745) |
| **AMPE process + MTK** — verify MIC, learn nonce/MGTK, derive MTK | `mesh_rsn_process_ampe` mesh_rsn.c:657 (`aes_siv_decrypt` :724, peer_nonce :755) → `mesh_rsn_derive_mtk` mesh_rsn.c:474/:529 (PMK32) | `mesh_process_ampe` umac_mesh.c:1614 (`mmint_aes_siv_decrypt` :1645, peer_nonce :1672, peer_mgtk :1677) → `mesh_derive_mtk` :1375 |
| **Key install** — MTK pairwise + peer MGTK group | `mesh_mpm_plink_estab` `wpa_drv_set_key` MTK mesh_mpm.c:928 / peer MGTK :938 (own MGTK TX at mesh start :227) → nl80211 .set_key → INSTALL_KEY | `umac_mesh_peer_secure_estab` umac_mesh.c:1442 (SET_STA_STATE :1454; `umac_keys_install_key` MTK :1481, peer MGTK :1496; own MGTK TX at mesh start) |
| **ESTAB** — plink established, secured | `wpa_mesh_set_plink_state(PLINK_ESTAB)` mesh_mpm.c:960 + `wpas_notify_mesh_peer_connected` :971 | `umac_mesh_handle_action` FSM edges OPN_RCVD+Conf :2574-2581 / CNF_RCVD+Open :2590-2598 ⇒ ESTAB |

The reorder vs Linux is intentional and load-bearing (**P3c**): Linux peers AMPE-protected from the start,
so it installs its own MGTK at mesh start and sends the Open immediately; morselib's Open is
droppable-as-unprotected until the AEK exists, so the Open is **deferred** behind the SAE-accept hook
(umac_mesh.c:1078-1083, gated `state == LISTEN` so it fires exactly once) and the own-MGTK TX-key install
is moved out of the ESTAB seam to mesh start. Two traps fall out of the crypto: `mesh_sae_build_commit` is
*destructive* (it runs `sae_set_group` then clears its scratch), so it is built once and the bytes are
replayed for every retransmit/resync (umac_mesh.c:871-874); and the AEK uses **PMK64** (PMK‖32 zeros) while
the MTK uses **PMK32** — mixing the two silently breaks key agreement (`mesh_derive_aek` :1412 /
`mesh_derive_mtk` :1375). On HW-crypto the MTK + peer MGTK land in the FW via `INSTALL_KEY`; under **P5**
SW-CCMP the same keys are kept host-side and the SW-CCMP data path below consumes them.

## Host SW-CCMP data path — code flow

For multi-hop the MM6108 firmware, in HW-crypto mode, does NOT deliver a forwarded 4-addr (A4≠TA) frame to
the host — an A4-sensitive FW *delivery* gate (#20; NOT a keys-by-A4 decrypt limit — the same FW HW-decrypts
forwards keyed by TA on Linux, see the peering flow above + §#26 / the #20 backlog). The fix moves ALL mesh
data + robust-mgmt CCMP into the host: no FW key offload → the FW delivers protected frames raw → the host
encrypts on TX / decrypts on RX, mirroring net/mac80211. Three flows below; the two fixes from this effort are boxed inline (★ **P5e** = TX key
selection, ★ **#22** = RX unprotected-mesh-action gate). All file:line are the current working tree.

### TX — encrypt (a mesh frame leaves the host already-protected)

```
  lwIP netif-out  /  re-injected forwarded frame
        │
        ▼
  umac_datapath_tx_dequeue_frame_mesh                     datapath.c:3385
     mesh_get_next_tx_stad() ─► stad   (common_stad, or a per-peer stad)
        │
        ▼
  umac_datapath_process_tx_frame(stad)                    datapath.c:2027
        │
        ├─► construct_80211_data_header_mesh              datapath.c:3421
        │      A1/RA = umac_mesh_lookup_next_hop(dest)     ← the HWMP next hop
        │      A2 = us   A3 = mesh-DA(final)   A4 = mesh-SA(origin)
        │
        │   is_multicast = is_mcast(A1)
        │
        │   ★ P5e fix                                      datapath.c:2105
        │   ┌────────────────────────────────────────────────────────────┐
        │   │ key_stad = stad;                                            │
        │   │ if (mesh_active && !is_multicast)         // UNICAST        │
        │   │     key_stad = umac_mesh_get_peer_stad(A1) ?: stad;         │
        │   │  → key a unicast under the NEXT-HOP peer's MTK, not the     │
        │   │    common_stad's vestigial fallback key (mesh_p1_mtk).      │
        │   │    Group TX + single-hop resolve back to `stad`.            │
        │   └────────────────────────────────────────────────────────────┘
        │   key_id = umac_keys_get_active_key_id(key_stad, GROUP|PAIRWISE)
        │   umac_keys_increment_tx_seq(key_stad, key_id)   // PN++ before use (mac80211 order)
        ▼
  umac_datapath_sw_ccmp_encrypt(key_stad, …)              datapath.c:518
     tk = umac_keys_get_key_data(key_stad, key_id)
     mesh_ccmp_encrypt(tk, hdr, …)                        ccmp.c:155 ─► mmint_aes_ccm_ae
     → splice the 8-B CCMP header between hdr and body, append the MIC
        │
        ▼
  FW TX   (frame already encrypted host-side; the FW holds no mesh key)
```
*Linux mirror:* `ieee80211_tx_h_select_key` (tx.c) keys a unicast from `tx->sta->ptk` with `tx->sta`
resolved from the RA; `ieee80211_crypto_ccmp_encrypt`. A **relayed** unicast takes the same shape via
`umac_mesh_forward_data` (mesh.c:2226) → `nh_stad = umac_mesh_get_peer_stad(next_hop)` (mesh.c:2243) →
`umac_datapath_tx_mesh_unicast_frame` → the same `sw_ccmp_encrypt`, keyed under the next-hop MTK (#19).

### RX — decrypt + dispatch (FW delivers protected frames raw)

```
  FW ─► umac_datapath_process_rx_frame                    datapath.c:1796
          stad = lookup_stad_by_peer_addr(TA)
          │
    ┌─────┴───── frame_type ───────────────────────────────────┐
    │ DATA                                                      │ MGMT (action)
    ▼                                                           ▼
  process_rx_data_frame_after_reorder              umac_datapath_process_rx_other_frame   datapath.c:1360
    protected & !FW_DECRYPTED & sw_crypto:            │
      sw_ccmp_decrypt(stad, …, DEFAULT)  :590         ├─► process_mgmt_frame_ccmp_header  datapath.c:1287
        mesh_ccmp_decrypt  ccmp.c:169                 │      protected → sw_ccmp_decrypt(stad, IND_ROBUST_MGMT)
        → mmint_aes_ccm_ad, then PN replay-check      │      unprotected → pass straight through (no decrypt)
    │                                                 ▼
    │  mesh-DA == us ?                          umac_datapath_process_rx_mgmt_frame        datapath.c:345
    ├─ YES → strip → deliver up to lwIP/IP        ★ #22 fix                                 datapath.c:373
    └─ NO  → umac_mesh_forward_data(DA,SA,…)      ┌────────────────────────────────────────────────────┐
             datapath.c:862   (RELAY)            │ if (!protected && pmf_is_required(stad)             │
             → re-encrypt under next-hop MTK     │      && !frame_is_mesh_action(view))    // was      │
               (see TX/forward above)            │           frame_is_group_privacy_action            │
                                                 │     → drop as unprotected robust mgmt              │
                                                 │  mesh peers are MFP=no ⇒ a Linux peer's UNPROTECTED │
                                                 │  unicast PREP now PASSES (was dropped here)         │
                                                 └────────────────────────────────────────────────────┘
                                                         │  ops->process_rx_mgmt_frame_mesh   datapath.c:3459
                                                         ▼
                                                  umac_mesh_handle_action                  mesh.c:2378
                                                    category MESH(13) →
                                                  umac_mesh_handle_hwmp(body, SA)          mesh.c:2022 (call :2396)
                                                    gate: mesh_peer_allowed(SA)
                                                    PREQ → mesh_path_update(reverse) + (tgt==us? PREP : flood)
                                                    PREP → mesh_path_update(forward) + (orig==us? install : forward)
                                                    PERR → path teardown
```
*Linux mirror:* `ieee80211_rx_h_decrypt` (rx.c) keys a protected unicast from `rx->sta->ptk`
(`ieee80211_crypto_ccmp_decrypt`); the unprotected-robust-mgmt drop is `ieee80211_drop_unencrypted_mgmt`,
whose entire drop block is gated on `test_sta_flag(rx->sta, WLAN_STA_MFP)` — **false for a mesh peer**, so
the unprotected unicast mesh action passes (which `frame_is_mesh_action` reproduces).

### End-to-end — secured multi-hop relay (the two flows above, composed per hop)

```
  board1 (ESP, endpoint)         board0 (ESP, RELAY)              board2 / chronite (endpoint)
  ──────────────────────         ───────────────────              ────────────────────────────
  DATA  (each leg CCMP under that leg's pairwise MTK):
    originate ICMP ──[MTK b1·b0]──► sw_ccmp_decrypt(DEFAULT) ──► forward ──[MTK b0·X]──► sw_ccmp_decrypt
    key_stad = b0  (★P5e)          mesh-DA ≠ us → relay           umac_mesh_forward_data        → up to IP
                                                                  nh_stad MTK (#19)

  HWMP  (board1 resolves a path to X via board0):
    PREQ(target=X) ──► flood ──► X: PREP(orig=b1) ──► forward PREP ──► install path
    start_discovery    handle_hwmp   (target==me)      lookup_next_hop(b1)   mesh_path_update

  Cross-vendor twist (X = chronite, Linux, MFP=no):
    chronite's PREP is UNPROTECTED → board0 RX would drop it pre-#22;
    the ★#22 frame_is_mesh_action exemption lets it reach handle_hwmp → board0 forwards it → board1 installs
    its path and keeps it refreshed (so the relay SUSTAINS instead of dying after a burst).
```

### The same flow in net/mac80211 (the reference the ESP side is ported from)

morselib collapses mac80211's per-handler `CALL_RXH`/`CALL_TXH` pipelines into straight-line functions, but
the *stages* line up 1:1. Lines below are grep-verified in chronite's `~/halow/rpi-linux/net/mac80211`.

```
LINUX TX                                                    LINUX RX
────────                                                    ────────
ieee80211_subif_start_xmit                  tx.c            driver RX → ieee80211_rx_handlers      rx.c:4186
   mesh_nexthop_resolve(skb)        tx.c:2054 →                (the CALL_RXH pipeline)
      mesh_nexthop_lookup        pathtbl.c:1239               │
      → set RA = next hop; kick PREQ if no path              ├─ ieee80211_rx_h_decrypt           rx.c:4189
   ieee80211_build_hdr             tx.c:2596                  │     unicast protected:
      4-addr: A1=next-hop A2=us A3=DA A4=SA                   │       rx->key = rx->sta->ptk  (sta from TA)
   │                                                          │       → ieee80211_crypto_ccmp_decrypt  wpa.c:515
   ▼ __ieee80211_tx → CALL_TXH pipeline      tx.c:1816        │     unprotected / HW-decrypted: pass
   ├─ ieee80211_tx_h_select_key             tx.c:1820   ┌─────┴──────── class ───────────────────────────┐
   │     unicast: tx->key = tx->sta->ptk                │ DATA                         │ MGMT (action)
   │              (sta resolved from the RA)            ▼                              ▼
   │     multicast: tx->key = GTK              ieee80211_rx_h_data        rx.c:4194   ieee80211_rx_h_action   rx.c:4202
   ├─ … h_sequence / h_fragment …                → ieee80211_rx_mesh_data rx.c:2827     ieee80211_drop_unencrypted_mgmt
   ▼                                                 DA==us? deliver to IP                              rx.c:3443
   ieee80211_tx_h_encrypt          tx.c:1861         else: build fwd_skb (:2940)         └─ gated on test_sta_flag(
      → ieee80211_crypto_ccmp_encrypt wpa.c:498            → re-xmit  (RELAY)               sta, WLAN_STA_MFP) — FALSE
   │                                                                                       for a mesh peer ⇒ unprot
   ▼ driver xmit (HW or SW crypto per key flags)                                           action PASSES
                                                                                       case WLAN_CATEGORY_MESH_ACTION
                                                                                                          rx.c:3766
                                                                                         → mesh_rx_path_sel_frame
                                                                                                   mesh_hwmp.c:1013
                                                                                            ├─ hwmp_preq_frame_process :660
                                                                                            ├─ hwmp_prep_frame_process :796
                                                                                            └─ hwmp_perr_frame_process :858
```

**Stage correspondence (Linux → ESP):**

| net/mac80211 (reference) | morselib (this port) |
| --- | --- |
| `mesh_nexthop_resolve` / `mesh_nexthop_lookup` + `ieee80211_build_hdr` (set RA=next-hop, 4-addr) | `umac_datapath_construct_80211_data_header_mesh` (`umac_mesh_lookup_next_hop`) datapath.c:3421 |
| `ieee80211_tx_h_select_key`: unicast `tx->key = tx->sta->ptk` (sta from RA) | ★ **P5e** `key_stad = umac_mesh_get_peer_stad(A1)` datapath.c:2105 |
| `ieee80211_tx_h_encrypt` → `ieee80211_crypto_ccmp_encrypt` (wpa.c:498) | `umac_datapath_sw_ccmp_encrypt` → `mesh_ccmp_encrypt` (ccmp.c:155) |
| `ieee80211_rx_h_decrypt`: `rx->key = rx->sta->ptk` → `ieee80211_crypto_ccmp_decrypt` (wpa.c:515) | `…_sw_ccmp_decrypt` → `mesh_ccmp_decrypt` (ccmp.c:169) — DEFAULT / IND_ROBUST_MGMT space |
| `ieee80211_rx_mesh_data` → `fwd_skb` re-xmit (rx.c:2827) | `umac_mesh_forward_data` (mesh.c:2226) → next-hop MTK |
| `ieee80211_drop_unencrypted_mgmt`, gated on `test_sta_flag(sta, WLAN_STA_MFP)` (rx.c:3443) | ★ **#22** `!frame_is_mesh_action(view)` in `umac_datapath_process_rx_mgmt_frame` datapath.c:373 |
| `mesh_rx_path_sel_frame` + `hwmp_{preq,prep,perr}_frame_process` (mesh_hwmp.c:1013/660/796/858) | `umac_mesh_handle_hwmp` (PREQ/PREP/PERR branches) mesh.c:2022 |

The two divergences that needed fixing this effort are exactly the two ★ rows: morselib hand-wired the key
stad per call site (so the originate path picked the wrong stad — P5e), and it gated the unprotected-mgmt
drop on a policy flag + frame-type list instead of the peer's MFP state (so a Linux MFP=no peer's unicast
PREP was dropped — #22).

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

## P3c — SAE PMK load-bearing (PMK seam + real PMKID + SAE-before-Open reorder)

P3c moves the previously-deferred items above into the live path: the AMPE AEK + per-pair MTK derive from
the SAE `peer->pmk`, the real SAE PMKID goes on-wire (reciprocal-checked), and the MPM Open is deferred
until SAE accepts. All Linux lines below were grep-verified 2026-06-28 in this tree (`.../src/hostap/`).

| ESP change (`umac_mesh.c`) | Linux/hostap reference (file:line) |
|---|---|
| `mesh_derive_mtk` reads `peer->pmk` (32 B) | `mesh_rsn.c:529` `sha256_prf(sta->sae->pmk, SAE_PMK_LEN, …)` |
| `mesh_derive_aek` reads `peer->pmk` (PMK‖32 zeros = 64 B) | `mesh_rsn.c:468` `sha256_prf(sta->sae->pmk, sizeof=SAE_MAX_PMK_LEN=64, …)` |
| delete static `mesh_p2_pmk` | — (Linux PMK is always `sta->sae->pmk`) |
| `mesh_peer_alloc`: remove alloc-time AEK | AEK is in `mesh_rsn_init_ampe_sta` `mesh_rsn.c:536-544` (line 543), called from `mesh_mpm_auth_peer`, **not** at add_peer |
| `mmwlan_mesh_peer_open`: SAE only, defer the Open (peer stays LISTEN) | `mesh_mpm.c:894-900` SEC_AMPE else-branch: `mesh_rsn_auth_sae_sta` with no `mesh_mpm_plink_open` |
| SAE-accept hook: derive AEK + send first protected Open → OPN_SNT | `sae_accept_sta` `ieee802_11.c:940` → `WPA_AUTH` `wpa_auth.c:2318` → `auth_start_ampe` `mesh_rsn.c:127/141` → `mesh_mpm_auth_peer` `mesh_mpm.c:679-717` (Open at 716) |
| `handle_action` `pmk_valid` gate (drop peering pre-SAE) | `mesh_mpm.c:1272-1278` (`sta->sae && state != SAE_ACCEPTED` → return) |
| `build_peering`/`tx_peering`/`params` write `peer->pmkid`; placeholder deleted | `mesh_mpm.c:359-366` (`mesh_rsn_get_pmkid` into the Open) + `mesh_rsn.c:435-438` (copies `sta->sae->pmkid`) |
| `mesh_parse_peering_ie` emits advertised PMKID; `handle_action` reciprocal `memcmp` (drop-only) | `mesh_rsn.c:689-695` (`chosen_pmk` vs `sta->sae->pmkid`, PMKID_LEN → return −1) |
| `mesh_sae_reauth_free` (ACCEPTED+Commit → free + invalidate paths) | `ieee802_11.c:1144-1151` `ap_free_sta`, `*sta_removed=1` |
| MTK derive at ESTAB edge (`umac_mesh_peer_secure_estab`) | `mesh_mpm.c:1062-1066` / `:1090-1096` (`if SEC_AMPE mesh_rsn_derive_mtk` then `plink_estab`) |
| `peer->retries=0` on auth-frame RX (asymmetric-timing fix) | (ESP-specific: keeps the faster peer's MPM budget alive while the slower side finishes SAE) |

**On-air verified (2026-06-28):** direct-peer encrypted ping 33/33 replies on the SAE-derived MTK; all
pairs ESTAB `pmk_valid=1/nonce_ok=1/mgtk_ok=1` with byte-identical SAE PMKIDs; tshark shows the MPM IE
PMKID is the SAE value (not the placeholder). The multi-hop **unicast relay** ping remains a separate
pre-existing follow-up (`umac_mesh_forward_data` needs the next-hop pairwise MTK). **Next: P3d**
(ESP↔live-Linux SAE byte-diff + cross-vendor PMK/PMKID agreement).

## P3d — ESP ↔ live-Linux SAE interop (cross-vendor beacon + SAE lifetime)

Verified against chronite's reference trees (`~/halow/rpi-linux` kernel `net/mac80211/mesh.c`,
`~/halow/hostap` `src/ap/*`). Every line below was `grep`-confirmed in those trees, not recalled.

| New code (`umac_mesh.c`, unless noted) | Linux / hostap counterpart |
| --- | --- |
| `mesh_build_config_ie`: Mesh-Config Authentication-Protocol byte `0x00`→`0x01` (SAE) | Beacon emits `*pos++ = ifmsh->mesh_auth_id;` `net/mac80211/mesh.c:291`; candidacy gate compares it `mesh_matches_local` `mesh.c:87` (`ifmsh->mesh_auth_id == ie->mesh_config->meshconf_auth`) |
| `umac_mesh_build_beacon`: prepend `mesh_rsn_ie` (CCMP-128 group+pairwise, AKM 8 = SAE) before Mesh ID; `mesh_rsn_ie`+`DOT11_IE_RSN(48)` moved above the builder | `mesh_add_rsn_ie` `net/mac80211/mesh.c:374`, emitted into the beacon at `mesh.c:1108`; candidacy at `mesh.c:1501` |
| `handle_action` CLOSE while `state!=ESTAB && (sae||pmk_valid)`: reset plink→LISTEN, **keep** SAE/PMK, re-Open if AEK ready (no free) | Linux keeps `sta->sae` across plink resets; SAE freed **only** in `ap_free_sta` `hostap/src/ap/sta_info.c:427-428` (`sae_clear_data(sta->sae); os_free(sta->sae);`) — a CLOSE/HOLDING never destroys it |
| `umac_mesh_plink_tick` OPN_SNT retries-exhausted: `pmk_valid` peer resets retries + re-Opens instead of CLOSE→HOLDING→free | same SAE-lifetime invariant — the dragonfly survives MPM retransmission; only `ap_free_sta` (`sta_info.c:427-428`) frees it |
| ACCEPTED+Commit retransmits cached `peer->sae_confirm` (`mesh_sae_reauth_free` deleted) | hostap answers a retransmitted commit; temp-only reset is `sae_clear_temp_data(sta->sae)` `ieee802_11.c:1166`, **not** a full free |

**On-air verified (2026-06-28):** with the beacon fixes, chronite (`3c:22:7f:37:51:38`) accepts the ESP as
a mesh candidate and runs SAE; with the SAE-lifetime fixes, board0 and chronite derive the **byte-identical
PMKID `855627ac3141c41d7e75f0e269d10283`** (confirm mismatch = 0) — cross-vendor dragonfly agreement.
**Open (next):** post-SAE AMPE — board0's AES-SIV MIC verify of chronite's MPM Open fails
(cross-vendor AAD/SIV detail); no encrypted ESP↔Linux ICMP yet.

## #13 — SAE re-sync deadlock fix (ACCEPTED+Commit reauth) + hardening

Verified by an adversarial verify→refute workflow against chronite's hostap tree (`~/halow/hostap`); every
row grep-confirmed on both sides, not recalled.

| New code (`umac_mesh.c`) | hostap counterpart |
| --- | --- |
| `mesh_sae_handle_rx` txn1 `case MESH_SAE_ACCEPTED` → `mesh_sae_reauth_free(peer); return` | `sae_sm_step` `case SAE_ACCEPTED` + `auth_transaction==1` && mesh → `wpa_auth_pmksa_remove` + `ap_free_sta` + `*sta_removed=1` (`src/ap/ieee802_11.c:1140-1148`) |
| `mesh_sae_reauth_free` = `umac_mesh_invalidate_paths_via` + `mesh_peer_free` | `ap_free_sta` STA teardown + `wpa_auth_pmksa_remove` PMKSA drop; `sae_clear_data(sta->sae)`+`os_free` (`src/ap/sta_info.c:426-428`) |
| **Reauth gate (hardening):** free only on `status==0` + well-formed-for-our-group (len + group-id) + non-reflection (scalar‖element ≠ our cached commit) | `handle_auth_sae` txn1 validation BEFORE `sae_sm_step`: status-success (`:1466`), group-supported (`:1457-1462`), `sae_parse_commit` (`:1502`), reflection→`SAE_SILENTLY_DISCARD` (`:1511`); reject paths do NOT free an MPM peer (`added_unassoc`=false, `:1657`) |
| txn2 `case MESH_SAE_ACCEPTED` resends cached Confirm (no anti-replay yet — task #14) | ACCEPTED else-branch: `sae_check_big_sync`+`sync++`+`auth_sae_send_confirm`+`sae_clear_temp_data` (`:1162-1170`) + Sc/rc/0xffff anti-replay (`:1581-1613`) |
| `umac_mesh_plink_tick` SAE retransmit: COMMITTED→Commit, CONFIRMED→Confirm, ~1s, big-sync each fire | `auth_sae_retransmit_timer`: big_sync+sync++ + COMMITTED→`auth_sae_send_commit`, CONFIRMED→`auth_sae_send_confirm`, default (incl ACCEPTED)→no frame (`:858-892`) |
| beacon-driven re-init after free: beacon → `mmwlan_mesh_peer_open` → `mesh_peer_alloc`+`mesh_sae_start` | `wpa_mesh_notify_peer`→`wpa_mesh_new_mesh_peer`→`mesh_mpm_add_peer`+`mesh_rsn_auth_sae_sta` (`src/ap/mesh.c`, `mesh_mpm.c:851/899`, `mesh_rsn.c:386-389`) |

**Verified (2026-06-28):** chronite (live Linux) completes SAE with board0 (`State Nothing→Committed→
Confirmed→Accepted`, was deadlocked at "SAE not yet accepted"); the hardening gate does not block
genuine-restart recovery (chronite still reaches Accepted ×33). The remaining re-cycle is the #12 AMPE MIC
blocker (board0 sends only Open, never Confirm, never ESTAB). On-air `morse0` byte-capture of board0 frames
pending (board0 RF range).

## #16 — AMPE MIC AAD canonicalisation (S1G→11n, aad-prefix-only)

| New code (`umac_mesh.c`) | morse driver / hostap counterpart |
| --- | --- |
| `#define WLAN_EID_SUPP_RATES (1)` + the AAD-canonicalisation note | the IE hostap's 11n view has first (`WLAN_EID_SUPP_RATES`) |
| RX: `mesh_process_ampe` copies `body[0:6]`→`aad2`, and for `action==WLAN_SP_MESH_PEERING_OPEN` sets `aad2[4]=1, aad2[5]=8` | driver `morse_dot11ah_s1g_to_11n_rx_packet` strips S1G caps + force-inserts legacy Supported Rates (EID 1, len 8) as the first IE before hostap (`morse_driver/mac.c:6628`); hostap MICs over that 11n body |
| TX: `umac_mesh_build_peering` AMPE protect, after the `aad2` copy, `if (is_open) aad2[4]=1, aad2[5]=8` | symmetric: the peer's driver reconstructs the 11n body on RX, so our SIV must bind to `01 08`; reverse 11n→S1G TX conversion in the driver |
| CONFIRM: no substitution (gate `action==OPEN`) — its `body[4:5]` is the AID, a fixed field | hostap CONFIRM AAD window = cat/action/cap/AID; first IE at body[6], outside the 6-byte window |

**Verified (2026-06-28, live Linux peer chronite):** ESP↔Linux SAE→AMPE→ESTAB — chronite logs
`mesh: Decrypted AMPE element` both ways + `mesh plink … established` + `iw station: mesh plink ESTAB,
authorized: yes`; board0 sends a Confirm (never did pre-#16). Cross-vendor encrypted mesh peering works.
**Open:** the encrypted DATA path (ICMP ping) — task #17 (group MGTK + pairwise MTK cross-vendor CCMP).

## #17 — broadcast HWMP encrypted under the MGTK (cross-vendor encrypted mesh data)

| New code (`umac_mesh.c`) | Linux/morse counterpart |
| --- | --- |
| `umac_mesh_tx_hwmp`: route group/broadcast-DA HWMP (`p->da[0]&0x01` → PREQ/PERR) through `umac_datapath_tx_mesh_group_frame` (MGTK/CCMP); unicast PREP stays on `umac_datapath_tx_mgmt_frame` | net/mac80211 mesh_hwmp.c `mesh_path_sel_frame_tx` + `ieee80211_select_key`: a broadcast PREQ is group-addressed robust mgmt, CCMP-protected with the MGTK (not BIP) |
| (was) `umac_datapath_tx_mgmt_frame` DROPS BC/MC robust-mgmt (`umac_datapath.c:2230-2237`) — the infra BIP case, unsupported | mac80211 protects group robust mgmt with BIP/IGTK in infra, but with the MGTK (CCMP) in a mesh |

**Verified (2026-06-28, live Linux peer):** board0↔chronite encrypted ICMP 5/5; chronite mpath ACTIVE/RESOLVED
(flags 0x15). Full encrypted ESP↔Linux mesh (SAE→AMPE→ESTAB→HWMP→CCMP→ping). Follow-ups: RX PREP for
no-static-ARP dynamic join; encrypt the unicast relay.

## #18 — group-privacy-action RX exemption (no-static-ARP dynamic join)

A group-addressed mesh HWMP frame (broadcast PREQ/PERR) is sent **unprotected** on-air (confirmed via
chronium tshark: Category MESH(13) Path Request, `protected=False`). morselib's infra PMF pre-dispatch
dropped it as an unprotected robust mgmt frame, so the HWMP handler never saw it and board0 never PREP'd →
no Linux peer could resolve a path to board0 (and board0's own ARP→ping never completed).

| New code | Linux/morse counterpart |
| --- | --- |
| `dot11/dot11.h`: `DOT11_ACTION_CATEGORY_MESH = 13`, `DOT11_ACTION_CATEGORY_MULTIHOP = 14` | `include/linux/ieee80211.h` `WLAN_CATEGORY_MESH_ACTION` / `WLAN_CATEGORY_MULTIHOP_ACTION` |
| `umac/frames/action.c`: `frame_is_group_privacy_action(view)` — mgmt Action + multicast DA(addr1) + category MESH/MULTIHOP | `include/linux/ieee80211.h:4611` `_ieee80211_is_group_privacy_action` (line-for-line) |
| `umac/frames/frames_common.h`: declare it | (header decl, next to `frame_is_robust_mgmt`) |
| `umac/datapath/umac_datapath.c:356-385`: add `&& !frame_is_group_privacy_action(rxbufview)` to the unprotected-robust-mgmt drop guard | `net/mac80211/rx.c:2436` `ieee80211_drop_unencrypted_mgmt`: the multicast-robust/BIP drop (`rx.c:2473`) is inside `if (rx->sta && test_sta_flag(.., WLAN_STA_MFP))` (`rx.c:2454`); **mesh peers are MFP=no** (chronite `iw station dump` → `MFP: no`) so the block is skipped → `RX_CONTINUE` |

`pmf_is_required` kept **true** so the *unicast* PREP stays CCMP-encrypted under the pairwise MTK (matches
chronite, proven #17). The morselib-`pmf_is_required` vs mac80211-`WLAN_STA_MFP`+key-presence mismatch is
tracked in task #9.

**Verified (2026-06-28, clean fix build, live Linux peer):** board0↔chronite **dynamic** join, no static
ARP — board0→chronite 46+39+11/0 replies (was 0); chronite→board0 5/5; chronite mpath to board0 `0x15`
(ACTIVE+RESOLVED) direct 1-hop. Captured `#18PREQ tgt==me==board0` (target-match reached).

## #19 — relay-forward pairwise keying + broadcast HWMP unprotected (multi-hop relay)

Two fixes for the ESP-as-intermediate-hop path. The forward-keying is verified correct; the end-to-end
ESP↔ESP relay ping has a separate downstream blocker (board2 doesn't decrypt the forwarded unicast).

| New code | Linux/morse counterpart |
| --- | --- |
| `umac_datapath.c`: `umac_datapath_tx_mesh_group_frame` refactored into `umac_datapath_tx_mesh_keyed_frame(stad, txbuf, key_type)` core + `_group_frame` (GROUP) / new `_unicast_frame` (PAIRWISE) wrappers | net/mac80211 `ieee80211_tx_h_select_key` (tx.c:614): unicast RA → `tx->sta->ptk`; multicast → GTK |
| `umac_datapath.h`: declare `umac_datapath_tx_mesh_unicast_frame` | — |
| `umac_mesh.c` `umac_mesh_forward_data`: `umac_mesh_get_peer_stad(next_hop)` → `umac_datapath_tx_mesh_unicast_frame(nh_stad, …)` (encrypt under next-hop MTK, key idx 0) | a forwarded unicast is keyed with the next-hop PTK |
| `umac_datapath.c` `umac_datapath_tx_mgmt_frame`: `&& !frame_is_group_privacy_action(txbufview)` on the robust-mgmt guard → group HWMP emitted unprotected (TX mirror of #18 RX) | mesh peers MFP=no → net/mac80211 sends group HWMP in the clear; supersedes #17's MGTK-encryption of broadcast HWMP |
| `umac_mesh.c` `umac_mesh_tx_hwmp`: always `umac_datapath_tx_mgmt_frame` (group-path special case removed); broadcast unprotected, unicast PREP pairwise | — |

**Verified (2026-06-28):** #19 forward correctly keyed (`key_type=PAIRWISE key_id=0`, ~20×); ESP↔ESP PREQ
chain resolves (board1→board0→board2, board2 PREPs); **no #17/#18 regression** (board0↔chronite dynamic
encrypted ping 39/3 + 5/5, mpath `0x15`). **On-air gold standard (chronium morse0 monitor):** board1/board0
PREQ = Action `protected=False`; board0→board2 forward = 4-addr QoS Data `protected=True`, CCMP, `RA=board2
TA=board0 A3=board2 A4=board1(origin)`, incrementing PNs — the relayed unicast is on-air CCMP-encrypted for
the next hop. **Open:** board2 doesn't decrypt board0's forwarded 4-addr unicast (firmware RX, #9-adjacent;
board0's TX is on-air-correct) — the relay ping doesn't complete yet → task #20.

## #21 P5e — originated multi-hop unicast keyed off the next-hop peer stad (completes the SW-CCMP relay)

The #20 firmware-HW-crypto limit was resolved by the host-CCMP port (#21 P5a–d, single-hop verified). P5e
closed the multi-hop gap. Root cause was NOT HWMP routing (discovery completes: board2 `#DISC`→board0 floods
→board1 `PREP(us)`→board0 forwards PREP→board2 `FORME`); it was that a **locally-originated** mesh unicast to
a multi-hop dest is dequeued on the **`common_stad`** (which carries only the vestigial static fallback
`mesh_p1_mtk` at key_idx 0), so `process_tx_frame` CCMP-keyed it off that fallback while the header's RA is the
HWMP next hop, which decrypts under the real per-pair MTK → MIC fail → forward dropped. This is the originate-
side analogue of the #19 forward fix.

| New code | Linux/morse counterpart |
| --- | --- |
| `umac_datapath.c` `umac_datapath_process_tx_frame`: new local `key_stad` = `stad`, but for an active-mesh **unicast** switch to `umac_mesh_get_peer_stad(ra)` (RA = the HWMP next hop set by `umac_datapath_construct_80211_data_header_mesh`); use `key_stad` for the active-key-id lookup, key length, TX-PN increment, and `umac_datapath_sw_ccmp_encrypt` | net/mac80211 `ieee80211_tx_h_select_key` (tx.c:614): a unicast frame is keyed from `tx->sta->ptk`, and `tx->sta` is resolved from the RA (next hop) — same as the #19 forward path, applied to the originate path |

Single-hop unicast (dest is the direct peer) and group TX (own MGTK on the common_stad) resolve `key_stad`
back to `stad`, so they are unchanged; the fix also corrects the reverse-direction ICMP-echo origination.

**Verified on-air (2026-06-29, chronium morse0 monitor, forced line board1—board0—board2):** before fix =
0 ping replies, board0 SW-decrypt **18/18 MICFAIL** (board2 encrypt `key=00112233` fallback ≠ board0 decrypt
`key=877601e8` per-pair MTK, same kid/PN), `#MESHFWD` 0×. After fix (clean trace-free firmware) = board1↔board2
ping **52 replies / 3 pre-peering timeouts** (RTT 49–67ms), board0 SW-decrypt all-OK with matching key
fingerprints, `#MESHFWD` fires; chronium (2941 frames) shows board0 relaying **both** directions — 55
request-forwards (`TA=board0 SA=board1`) + 58 reply-forwards (`TA=board0 SA=board2`), 4-addr `Protected=True`.
This completes the secured 802.11s multi-hop relay on host SW-CCMP.

## #22 — accept an MFP=no peer's unprotected unicast mesh action (cross-vendor multi-hop with a Linux endpoint)

Cross-vendor multi-hop (board1 ESP → board0 ESP relay → chronite Linux) worked only in a burst then died:
chronite sends its unicast PREPs **UNPROTECTED** (Linux mesh peers are MFP=no), and board0 dropped them at the
PMF pre-dispatch, so board1's PREQ→PREP path discovery to chronite never completed (the burst came from
transient chronite-PREQ reverse-routes). The ESP marks mesh peers `MMWLAN_PMF_REQUIRED` but morse mesh is
MFP=no; the #18 fix exempted only *group* mesh actions from the unprotected-robust-mgmt drop, not the unicast
PREP.

| New code | Linux/morse counterpart |
| --- | --- |
| `frames/action.c` + `frames_common.h`: new `frame_is_mesh_action(view)` — a Mesh(13)/Multihop(14) Action frame of ANY addressing (group OR unicast); the unicast-inclusive sibling of `frame_is_group_privacy_action` (no group-DA requirement) | net/mac80211: a mesh path-selection frame is a robust mgmt action; whether to drop it when unprotected is decided solely by the peer's MFP state, not its addressing |
| `umac_datapath.c` `umac_datapath_process_rx_mgmt_frame`: the unprotected-robust-mgmt drop gate exempts `!frame_is_mesh_action(rxbufview)` (was `!frame_is_group_privacy_action`) — so an unprotected unicast PREP from a mesh peer is no longer dropped | net/mac80211 `ieee80211_drop_unencrypted_mgmt` (rx.c): the entire unprotected-robust-mgmt drop block is inside `if (rx->sta && test_sta_flag(rx->sta, WLAN_STA_MFP))` — **false for a mesh peer (MFP=no)** → the block is skipped and the unprotected unicast mesh action passes (ASSOC'd) |

The MFP setting / RSN IE / AMPE / TX are untouched (so the working AMPE peering + ESP↔ESP relay are
unaffected); this mirrors mac80211's *behaviour* (accept the MFP=no peer's unprotected mesh actions) via the
established morse frame-type-exemption pattern.

**Verified on-air (2026-06-29, chronium):** board1↔chronite via board0 ping **50 replies / 2 pre-peering
timeouts, sustained** (seq 14→65; was a ~16-reply burst then dead). board0 now forwards chronite's PREP
(`#PREPFWD` nh_ok=1 → board1; was 0), board1 installs+refreshes its path (`#PREPME` ×5; was a one-shot 1),
chronite's PREP storm collapses 244+→~5. chronium: board0 relaying both ways sustained (57 req-fwd + 56
reply-fwd). Cross-vendor multi-hop (ESP relay ↔ Linux endpoint) now works end-to-end.

**#20 correction (DEFINITIVE — on-air measured 2026-06-29; supersedes ALL earlier #20 verdicts):** the MM6108
firmware **HW-decrypts + delivers** a forwarded 4-addr A4≠TA mesh frame, keyed by the **TA-link** (NOT by A4).
Proven on chronite three independent ways: (1) **ftrace** — `ieee80211_crypto_ccmp_decrypt` for a confirmed
foreign-A4 forward calls ONLY `ieee80211_hdrlen`+`skb_pull` (~2 µs), never `crypto_aead_decrypt`/AES (all
ftrace-able), i.e. the FW delivered it already-decrypted (`RX_FLAG_DECRYPTED` set) and mac80211 did NO SW
decrypt; (2) **removing the originator from chronite's FW STA table** (`sta_tx_count_table`) changes nothing —
still HW-decrypted + delivered (A4-in-table is irrelevant); (3) **loading the ESP's exact 1.17.9 firmware onto
chronite** still delivers (not a FW-version difference; ESP=1.17.9 from the build, Linux=1.17.8; same
`fgh100mhaamd` BCF on chronosalt also delivers). **So #20 is a morselib HOST-STACK bug — NOT a firmware limit,
NOT the FW version, NOT an A4-registration gap.** The FW-command sequence is byte-identical to the morse driver
(workflow + live kprobe: ADD_INTERFACE/BSS_CONFIG/MESH_CONFIG + SET_STA_STATE/INSTALL_KEY all match), so the
morselib gap is most likely a host-side RX drop in the HW-crypto path (the `MMDRV_RX_FLAG_DECRYPTED` gate /
stad-by-TA lookup around `umac_datapath.c:569`), not a missed FW config. host SW-CCMP (P5) sidesteps the FW and
is the shipped general solution; a morselib HW-crypto fix is viable (FW proven capable). The exact seam is the
open #20 fix task. *(RETRACTS the earlier "Linux uses a mac80211 SW fallback" mechanism — ftrace shows the FW
HW-decrypts, no SW fallback runs — and the "A4-must-be-registered" model.)* Worklog §#25. **REFINED §#26 (fix dig findings — #20 filed as BACKLOG, not closed):** the drop is NOT
the morselib host RX code — that path is **A4-AGNOSTIC** (every gate keyed by TA: `ccmp_is_valid` :711, the BA
reorder window, the stad lookup :1818). Since the #20 drop is **A4-SENSITIVE** (it drops the forward but passes
a direct frame from the **same** relay/TA/key), it must be at the one A4-sensitive layer = the **MM6108
FIRMWARE** — an ESP-specific FW decrypt-gate behaviour (a non-command/BCF/build difference; the same binary
delivers on Linux, which imposes no host gate — `mac.c:5844`/`:6632` + `ieee80211_rx_h_mesh_fwding`). host
SW-CCMP (P5) bypasses it: no FW key → the FW delivers the frame raw → host decrypts. So there is **no clean
morselib HW-crypto fix** (supersedes the "HW-crypto fix is viable" line above); P5 stays the answer.
