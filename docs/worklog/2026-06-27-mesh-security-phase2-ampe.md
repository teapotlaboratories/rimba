# 2026-06-27 — Mesh security Phase 2 (AMPE): per-peer-stad TX (P2a)

Phase 2 replaces Phase-1's static/shared keys with the real AMPE key hierarchy: a per-pair MTK
derived from a PMK + exchanged nonces, plus a per-node MGTK exchanged in an AES-SIV-protected AMPE
element — following hostap `wpa_supplicant/mesh_rsn.c` + `mesh_mpm.c` line by line. It is split into
four independently on-air-verifiable increments:

- **P2a — per-peer-stad TX** (this section, DONE): drive unicast mesh TX from the *per-peer* stad
  (queue + dequeue per peer, like the AP datapath) instead of the single common stad. Prerequisite
  for per-pair keys; verified with the existing static keys so the TX-architecture change is
  de-risked from the crypto.
- **P2b — derived per-pair MTK + nonce exchange** (DONE, see below): `MTK = sha256_prf(PMK,
  "Temporal Key Derivation", min/max nonce ‖ LID ‖ AKM ‖ MAC)` on a static PMK; nonces in a
  throwaway IE.
- **P2c — per-node MGTK exchange** (DONE, see below): each node generates its own MGTK and ships
  it to each peer.
- **P2d — real AES-SIV AMPE element** (P2d.2 DONE, see below): the standard AMPE element (139) + MIC
  IE (140) SIV-encrypted under the AEK, replacing the P2b/P2c 'RIM' scaffolding. Remaining sub-steps:
  P2d.3 RSN-IE/PMKID (Linux interop only), P2d.4 own-MGTK-at-start, P2d.5 gold-standard byte-diff vs Linux.

## Pivotal design finding: P2 is integration, not crypto authorship

The ESP build already links all the crypto P2/P3 need — confirmed in the bundled hostap
(`framework/src/hostap`): `sha256_prf` (KDF for MTK/AEK), `aes_siv_encrypt`/`aes_siv_decrypt` (the
AMPE MIC, RFC 5297 — the original Cozybit 802.11s primitive), `aes-omac1`/`aes-ctr` (SIV deps),
`crypto_get_random` (nonce/MGTK), and even `sae.c` (P3). They are exported to morselib through the
`mmint_*` ABI (`hostap_morse_common.h` mangles e.g. `#define sha256_prf mmint_sha256_prf`,
`crypto_get_random → mmint_crypto_get_random`). So P2b/P2c call `mmint_sha256_prf` +
`mmint_crypto_get_random` via a small morselib-local `extern` shim (avoids pulling hostap
`src/crypto/*.h` into morselib and its `utils/common.h` type clashes). The **only** net-new build
change in all of P2 is P2d adding `aes-siv.c` + `aes-ctr.c` to the hostap CMake SRCS (upstream
verbatim) and exporting `mmint_aes_siv_*` via `hostap_morse_common.h`.

## P2a — what changed

Phase-1's decisive gotcha was that mesh TX is **common-stad-driven**: every mesh frame dequeues from
the one common (MBSS) stad, so a single shared MTK on the common stad is what encrypts unicast. That
works only because the P1 keys are static/shared. P2's per-pair MTK means unicast to peer B must be
encrypted under the B-specific key, i.e. queued on and dequeued from **B's per-peer stad**. P2a makes
that switch (keys still static, so the result is observably identical — this isolates the TX change):

`umac/datapath/umac_datapath.c`
- `umac_datapath_lookup_stad_by_tx_dest_addr_mesh` (the TX enqueue lookup) now returns the **per-peer
  stad** for a unicast dest (`umac_mesh_get_peer_stad(addr)`), falling back to the common stad for
  broadcast/multicast/zero or an unknown/not-yet-ESTAB dest. Previously it always returned the common
  stad. Mirrors the existing RX lookup and the AP per-STA model. (Single-hop: dest == next hop; the
  MTK is per *link*, so HWMP multi-hop needs a next-hop resolve here too — a later step.)
- `umac_datapath_tx_dequeue_frame_mesh` rewritten from a common-stad-only pop into an AP-style
  scheduler: a new pure helper `mesh_get_next_tx_stad()` returns the common stad first (broadcast/
  mgmt + group MGTK), then the first established peer stad with queued traffic — mirroring
  `umac_ap_get_next_sta_for_tx` walking `data->stas[]`. The dequeue scans + pops + recomputes
  `has_more` under one critical section; `has_more` is an aggregate re-scan (mesh has no single
  queued-frame counter like the AP's `num_pkts_queued`), so the other peers' queues never stall.

`umac/mesh/umac_mesh.{c,h}`
- `umac_mesh_peer_stad_at(size_t index)` — enumerates peer slot `index`'s stad, but only when that
  slot is `used && state == MESH_PLINK_ESTAB` (else NULL), letting the scheduler iterate established
  peers without exposing `mesh_peers[]`.
- `UMAC_MESH_MAX_PEERS` lifted into the header as the single source of truth; the .c's
  `MESH_MAX_PEERS` now derives from it (the datapath loop and the peer table agree by construction).

## On-air proof (2026-06-27, board0 + board1, ch27 clear, chronium `morse0` monitor)

2-board direct test (board1 → board0, both on the P2a build). Linux mesh stopped (ch27 clear); the
third ESP (board2) was on a PPK2 power-profiling rig with older fw, so a 2-node direct ping was used
— board1's ping target was temporarily pointed at board0 (`10.9.9.136`) for this.

```
board1 console:  54 replies, 0 timeouts  (seq 54..93 unbroken)  -> 0% loss
chronium morse0 pcap (694 frames, tshark):
  QoS-Data Protected tally:
    board0 (e2:72:a1:f8:ef:a4):  28 Protected=1,  0 plaintext
    board1 (e2:72:a1:f8:f9:40):  29 Protected=1   (all 28 unicast->board0 + 1 broadcast)
                                  1 plaintext = a startup broadcast gratuitous ARP (10.9.9.100),
                                  common-stad path, in the window before the group key went active
                                  — benign, identical to P1, NOT on the unicast path P2a changed.
  unicast board1->board0 frame:  Protected=1, RA=board0 / TA=board1,
                                  CCMP Ext-IV present, Key Index = 0  (pairwise MTK, per-peer stad)
```

**0% loss is the decisive functional proof**: if the per-peer enqueue and the new scheduler dequeue
were inconsistent, unicast frames would stick on the per-peer queue and never TX (exactly the
regression hit mid-P1 when only the enqueue was moved to the per-peer stad). They drain cleanly, and
the CCMP **Key Index 0** confirms the encrypting key is the **pairwise MTK selected from the per-peer
stad**, not the group key — the per-peer-stad TX path is correct end to end. This is the foundation
P2b's per-pair derived MTK installs onto.

Multi-peer drain (one relay node with two established peers) is verified by construction — the
scheduler loops over all peer slots — and is deferred to a later 3-node on-air run once board2 is
free of the profiling rig.

## P2b — derived per-pair MTK + nonce exchange (DONE)

Replaces Phase-1's shared `mesh_p1_mtk` with a per-pair MTK derived from a static PMK + the peering
nonces, byte-exact with hostap `mesh_rsn_derive_mtk`/`mesh_rsn_derive_aek` (verified against the
bundled `wpa_supplicant/mesh_rsn.c`, not from memory). All crypto is reused via the `mmint_*` ABI —
no crypto ported.

`umac/mesh/umac_mesh.c`:
- `extern int mmint_sha256_prf(...)` + `extern int mmint_crypto_get_random(...)` — a morselib-local
  shim onto the mangled hostap crypto (avoids pulling `src/crypto/*.h` and its `utils/common.h`
  type clashes). Confirmed at link time: no undefined references, so morselib resolves the mangled
  hostap symbols — the linkage assumption behind all of P2 (incl. P2d's `aes_siv`).
- `struct mesh_peer`: `my_nonce[32]`, `peer_nonce[32]`, `peer_nonce_valid`, `mtk[16]`, `aek[32]`.
- `mesh_peer_alloc`: `mmint_crypto_get_random(my_nonce, 32)` — a fresh local nonce per (re)alloc
  (the `memset(p,0,...)` clears `peer_nonce_valid`, so a re-peer can't reuse a stale MTK).
- Constants: `mesh_p2_pmk[32]` (the static PMK, identical on every node) + `mesh_ampe_akm_sae[4]`
  = `00 0f ac 08` (SAE, big-endian like `RSN_SELECTOR_PUT`).
- `mesh_derive_mtk(peer)`: ctx[84] = `min/max(myNonce,peerNonce)` ‖ `min/max LID` (LE16, **numeric**
  order) ‖ `AKM` ‖ `min/max(myMAC,peerMAC)`; `sha256_prf(PMK,32,"Temporal Key Derivation",ctx,84,
  mtk,16)`. Nonces/MACs ordered by `memcmp`.
- `mesh_derive_aek(peer)`: ctx[16] = `AKM` ‖ `min/max MAC`; **key = PMK(32)‖32 zeros, length 64**
  (mirrors hostap's `sizeof(sae->pmk)=SAE_MAX_PMK_LEN`; the MTK uses len 32 — mixing the two is the
  #1 byte-exactness break). Derived in P2b to validate the 64-byte path; consumed by P2d's AES-SIV.
- `umac_mesh_peer_secure_estab`: derive MTK+AEK, then install `peer->mtk` (was `mesh_p1_mtk`) on the
  per-peer stad. The peer/own MGTK stays static (`mesh_p1_mgtk`) until P2c.
- Nonce exchange (throwaway scaffolding, replaced by the real AMPE element in P2d): a vendor IE (221,
  marker `'RIM'`/type 1) carrying the 32-byte local nonce on Open + Confirm; `mesh_parse_peering_ie`
  extended to also return the peer nonce, stored into `peer->peer_nonce` before ESTAB.

### On-air proof (2026-06-27, board0 + board1, chronium `morse0` monitor)

```
board1 console:  MESH-SEC derive aid=1 nonce_ok=1 mtk=477aa0b3 aek=3702c4a2   ; 55 replies, 0 timeouts
board0 console:  MESH-SEC derive aid=2 nonce_ok=1 mtk=477aa0b3 aek=3702c4a2   ; (ping responder)
chronium pcap:   board1<->board0 unicast QoS-Data ALL Protected=1 / CCMP (28 + 30 frames)
```

Decisive points: the MTK is **derived** (`477aa0b3`), not the static `00 11 22 33…`; board0 and board1
independently derived the **identical** MTK **and** AEK (`3702c4a2`) — proof the min/max byte order
(memcmp nonces/MACs, numeric LIDs) is correct on both ends; `nonce_ok=1` confirms the nonce carrier
worked; and 0% loss proves the derived key actually encrypts and decrypts. board0 also had board2
(older fw, no nonce IE) allocated as a peer slot but it never reached ESTAB, so it stayed isolated
from the secured board0↔board1 link. Each pair derives a distinct MTK by construction (different
nonces/MACs per pair); the explicit 3-node A↔B ≠ A↔C demonstration and the Linux byte-diff are
deferred to a 3-new-fw-node run / the P2d gold standard.

## P2c — per-node MGTK exchange (DONE)

Replaces P2b's still-static MGTK with a per-node group key: each node generates its own MGTK once at
mesh start, advertises it to each peer in its Open, and installs the peer's MGTK as a group-RX key so
the peer's broadcast/multicast decrypts under the peer's own key (mac80211 `__mesh_rsn_auth_init` +
`mesh_mpm_plink_estab`). The static `mesh_p1_mgtk` is removed.

`umac/mesh/umac_mesh.c`:
- `struct umac_mesh_context.own_mgtk[16]`: generated once at mesh start (`mmint_crypto_get_random`),
  before any Open carries it. Install onto the common stad (group-TX, key_idx 1) stays deferred to
  first ESTAB (a group key at start breaks OPEN peering — the P1 gotcha; moves to start only in P2d).
- `struct mesh_peer.peer_mgtk[16]/peer_mgtk_rsc[8]/peer_mgtk_valid`: the peer's own MGTK, learned
  from its Open, installed as our group-RX key for that peer at ESTAB.
- Carrier IE extended: the 'RIM' vendor IE now has type 1 (nonce only, Confirm) and type 2 (nonce +
  16-byte MGTK + 8-byte RSC, Open). `mesh_parse_peering_ie` returns the peer MGTK; stored pre-ESTAB.
- RSC advertised as 0 (own MGTK PN starts at 0 when sent in the Open, before group TX); applying a
  non-zero RSC is deferred to P2d. (`umac_key.rx_seq` is a uint64 replay-counter array, not the RSC
  bytes — left zeroed.)
- No RX change needed: a received group frame already resolves to the **transmitter's** per-peer stad
  (`lookup_stad_by_peer_addr_mesh` is called with the frame's TA, `umac_datapath.c:1460/1643`), which
  is exactly where that originator's MGTK lives. The P2a per-peer-stad model already routes it.

### On-air proof (2026-06-27, board0 + board1, chronium `morse0` monitor)

```
board0 console:  own MGTK (group TX) = 12a33130 ; installs peer MGTK (group RX) = 65529a3e
board1 console:  own MGTK (group TX) = 65529a3e ; installs peer MGTK (group RX) = 12a33130
  -> board0-own == board1-installed, board1-own == board0-installed, and 12a33130 != 65529a3e
     (each node's MGTK is distinct and exchanged correctly).
board1 broadcasts on-air: QoS-Data, Protected=1, CCMP Key Index 1 (group MGTK), PN increments.
ping board1->board0: 0% loss.
```

**Decryption proved on-air (stronger than planned):** the monitor caught board0 re-broadcasting
board1's DHCP-Discover / ARP content with **TA=board0, SA=board1** — board0 could only emit that
plaintext content by first **decrypting board1's keyid-1 broadcast with board1's per-node MGTK**
(installed as group-RX on board0's per-peer stad for board1). So the full path works: board1
originates a group frame encrypted under its own MGTK, board0 decrypts it under the same key. The MTK
also changed vs P2b (`9f1d7647` vs `477aa0b3`) because nonces are fresh per peering, while the AEK
stayed `3702c4a2` (no nonce input) — a nice consistency check.

### Finding (pre-existing, NOT a P2c regression) — FIXED: forwarded group frames were plaintext

The P2c capture showed board0's **re-broadcast** (group-forward) of board1's frame going out in the
clear (DHCP/ARP content visible, TA=board0). The mesh group-forward path (`umac_mesh_build_rebcast` →
`umac_datapath_tx_mgmt_frame`, from `c16e9a8a`, the original mesh data plane — predates all security
work) re-emits via the **management-frame TX path, which only encrypts robust *management* frames**
(PMF/pairwise) and overwrites the tx metadata, so a forwarded *data* frame goes out unencrypted.
Linux re-encrypts a forwarded mesh group frame with the local MGTK on re-TX; the ESP forward did not.
Orthogonal to P2c's key exchange (it concerns *re-transmitting others'* frames), but a real
secured-mesh gap — a forwarded multicast leaked in plaintext.

**Fix:** a dedicated `umac_datapath_tx_mesh_group_frame(stad, txbuf)` (umac_datapath.c) applies the
*data-path* rule to the pre-built re-broadcast: when the stad's security != OPEN, encrypt under the
**GROUP** key (`umac_keys_get_active_key_id(stad, GROUP)` = the forwarder's own MGTK on the common
stad), set the Protected bit + `MMDRV_TX_FLAG_HW_ENC` + key_idx + increment the PN. The mesh
group-forward (`umac_mesh_handle_group_data`) now calls it instead of `umac_datapath_tx_mgmt_frame`,
so a forwarded multicast is CCMP-protected on every hop. **Verified on-air:** board0's forwarded
frames (TA=board0, SA=board1, group DA) are now all Protected=1 / CCMP **Key Index 1** (board0's own
MGTK), PN incrementing — the DHCP/ARP plaintext is gone. Unicast steady-state ping 0% loss (one
boot-time transient timeout during peering churn). The same gap on the *unicast* relay forward
(`umac_mesh_forward_data`, which needs the next-hop **pairwise** MTK, not the group key) remains a
separate follow-up.

## P2d.2 — real AES-SIV-protected AMPE element (DONE)

Replaces the throwaway 'RIM' carrier IE with the standard AMPE element (`WLAN_EID_AMPE`=139) wrapped
in a `WLAN_EID_MIC`=140 element, AES-SIV-encrypted under the **AEK** — the real 802.11s AMPE,
byte-exact with hostap `mesh_rsn_protect_frame` / `mesh_rsn_process_ampe` (this Morse
`CONFIG_IEEE80211AH=1` build → the **fixed-6 AAD** variant). All crypto reused.

Build integration (P2d.1): `components/hostap/CMakeLists.txt` adds `aes-ctr.c` + `aes-siv.c` to SRCS
(`omac1`/`aes_encrypt` already provided by `crypto_mbedtls_mm.c`); `hostap_morse_common.h` mangles
`aes_siv_encrypt/decrypt → mmint_aes_siv_*`. morselib calls them through `extern int mmint_aes_siv_*`
shims — links clean (the same `mmint_*` mechanism proven for `sha256_prf` in P2b).

`umac/mesh/umac_mesh.c`:
- AEK is derived at **`mesh_peer_alloc`** (not ESTAB) + `aek_valid` flag — it depends only on the MACs
  + PMK, and must be ready to verify the very first incoming Open (whose AMPE element carries the
  peer's nonce). MTK still derives at ESTAB (needs the exchanged nonces).
- TX (`umac_mesh_build_peering`): build the plaintext AMPE element — `[139][len]`,
  `selected_pairwise_suite = CCMP 00 0f ac 04` (**not** the SAE AKM), `local_nonce[32]`,
  `peer_nonce[32]`, and on **Open** the GTKdata (`MGTK[16] ‖ RSC[8] ‖ GTKExpiry=0xffffffff`); len 68
  (Confirm) / 96 (Open). Append `[140][16]` then `aes_siv_encrypt(AEK, key_len=32, ampe_element,
  num_elem=3, AAD={TA=us, RA=peer, first-6-body}, lens={6,6,6})`: the 16-byte SIV IV is the MIC IE
  body, the ciphertext spills past it. Two-pass consbuf — encrypt only on the fill pass; the 6-byte
  AAD is read from `(uint8_t*)hdr + sizeof(*hdr)` (the category byte) once the body is final.
- RX (`mesh_parse_peering_ie` + `mesh_process_ampe`): the parser locates the MPM IE (117) link ids and
  the MIC IE (140), stopping at 140 (the ciphertext is not valid TLV). `mesh_process_ampe`
  `aes_siv_decrypt(AEK, 32, IV‖ciphertext, AAD SWAPPED {TA=peer, RA=us, first-6}, {6,6,6})`; on
  success checks `eid==139`, the peer_nonce echo (zero or == our nonce), learns the peer's local_nonce
  → `peer->peer_nonce`, and (Open) the MGTK → `peer->peer_mgtk`. A verify failure drops the frame
  without advancing the FSM.

### On-air proof (2026-06-27, board0 + board1, chronium `morse0` monitor)

```
both consoles:  MESH-SEC ampe rx action=1 (Open)    verify=0 eid=139   (crypt_len 114 = 16 + 98)
                MESH-SEC ampe rx action=2 (Confirm)  verify=0 eid=139   (crypt_len  86 = 16 + 70)
                estab nonce_ok=1 mgtk_ok=1 ; board0 mtk == board1 mtk (identical, e.g. 77b9090d)
ping board1->board0:  43 replies, 0 timeouts ; 78 unicast frames all Protected=1.
Open frame IEs on-air: 217, 114, 113, 117, 140  — NO vendor IE (221); the 'RIM' carrier is gone, and
                no plaintext AMPE(139) tag appears (it is the encrypted ciphertext after the MIC IE).
```

Decisive: **the AES-SIV verify succeeds on both Open and Confirm, and both nodes independently derive
the IDENTICAL per-pair MTK from the AMPE-exchanged nonces** — any wrong byte (AAD order/length, the
32-byte AEK key length, the nonce offsets, the CCMP-vs-SAE suite) would fail the verify or mismatch the
MTK. Then encrypted unicast pings at 0% loss. The on-air wire format matches `mesh_rsn_protect_frame`
(MPM IE then `[140][16][SIV-IV][ciphertext]`). The AMPE port is functionally complete for ESP↔ESP.

### Remaining P2d sub-steps (deferred)
- **P2d.3** — Linux-shaped peering wrapper: **DONE.** `umac_mesh_build_peering` now emits the RSN IE
  (48, the exact live-Linux bytes `30 14 01 00 00 0f ac 04 01 00 00 0f ac 04 01 00 00 0f ac 08 00 00`),
  the **PRIVACY capability** (`10 00`), and the MPM IE with **`protocol=1` + a 16-byte PMKID** (a
  placeholder — the real cross-vendor PMKID needs **P3 (SAE)**). The parser excludes the trailing PMKID
  from its plid-present check (else an Open's PMKID is misread as a plid) and skips the RSN IE. The
  PRIVACY cap is inside the 6-byte AMPE AAD window, but both ESP ends change together so the SIV still
  verifies. **Verified on-air + byte-diffed vs the live Linux frame:** both nodes AMPE verify=0 on
  Open+Confirm + netif up; the ESP Open's IE tags are now `217, 48, 114, 113, 117, 140` (security tail
  `48,114,113,117,140` matches Linux), and the raw bytes are byte-identical — RSN IE
  `30 14 01 00 00 0f ac 04 01 00 00 0f ac 04 01 00 00 0f ac 08 00 00`, MPM IE `75 14 01 00 …`
  (protocol=1), MIC IE `8c 10 …` — the ONLY difference is the PMKID value (placeholder vs SAE-derived,
  which P3 supplies). (Beacon RSN IE deferred — only a Linux node *initiating* to the ESP needs it,
  which also needs P3.)
- **P2d.4** — own-MGTK install moved from first-ESTAB back to mesh start: **DONE.**
  `umac_mesh_install_common_keys()` now runs in `mmwlan_mesh_start` (after `own_mgtk` is generated +
  the common stad is set up), matching hostap `__mesh_rsn_auth_init`; the first-ESTAB install +
  deferral comment are removed. The P1 gotcha (firmware dropping the unprotected peering frames once a
  group key is installed) **does NOT recur** now peering is AMPE-protected — **verified on bench:**
  with the MGTK installed at start, both nodes still AMPE verify=0 on Open+Confirm and reach netif-up.
  Bonus: this closes the P2c startup plaintext-broadcast window (the group key is live before netif-up).
- **P2d.5** — gold-standard byte-diff vs a live Linux secured-mesh node: **DONE, see below.**

## P2d.5 — gold-standard byte-diff vs a live Linux secured mesh (DONE)

Stood up the real Linux SAE+AMPE mesh (`wpa_supplicant_s1g -dd -K` on chronosalt + chronogen, the
AH-patched 1.17.8 morse build → the same fixed-6 AAD; `rimba-smesh`, SAE), peered them (plink ESTAB),
and pulled a live node's **un-redacted plaintext AMPE element** plus the on-air frame, to byte-diff
against the ESP's P2d.2 output. (The PMKs differ — Linux SAE vs ESP static — so the *encrypted* bytes
and the random nonces/MGTK necessarily differ; the comparison is the element layout + the fixed
fields + the sizes + the on-air framing.)

Live Linux plaintext AMPE element, Open (`wpa-K.log`, len=98):
```
8b 60 | 00 0f ac 04 | <32B local_nonce> | <32B peer_nonce = 00…00> | <16B MGTK> | <8B RSC = 00…00> | ff ff ff ff
```
Field-for-field vs what the ESP `umac_mesh_build_peering` emits — **every fixed field is identical**:

| field | live Linux | ESP P2d.2 |
|---|---|---|
| EID / len | `8b 60` (139 / 96) | `8b 60` (139 / 96) |
| selected_pairwise_suite | `00 0f ac 04` (CCMP) | `00 0f ac 04` (CCMP) |
| local_nonce(32) ‖ peer_nonce(32) | nonce ‖ **all-zero on the first Open** | nonce ‖ **all-zero until peer heard** |
| GTKdata (Open) | MGTK(16) ‖ **RSC=0**(8) ‖ **`ff ff ff ff`** | MGTK(16) ‖ **RSC=0**(8) ‖ **`ff ff ff ff`** |
| encrypted size | **114** (Open) / **86** (Confirm) | **114** (Open) / **86** (Confirm) |

On-air (chronium `morse0`), the Linux Open's IE tags are `221, 217, 232, 48, 114, 113, 117, 140`; the
ESP's are `217, 114, 113, 117, 140`. The **security-relevant tail `…114, 113, 117, 140`** (Mesh ID,
Mesh Config, MPM IE, then the **MIC IE 140** with the encrypted AMPE spilling past it) is **identical**.
The Linux-only extras are precisely the deferred **P2d.3** items: the **RSN IE (48)**, the MPM IE with
**`protocol=1` + a 16-byte PMKID** (`75 14 01 00 …`), and the **PRIVACY capability** (`10 00`). The
remaining tag diffs (`221`/`232` vendor + S1G-operation IEs) are PHY-layer, from the morse 5 GHz-model
representation, not AMPE.

**Conclusion:** the ESP's AMPE element is byte-exact with a live Linux Morse device — same EID/len,
CCMP suite, nonce + GTKdata layout, RSC, expiry, and encrypted sizes — and it sits in the same on-air
IE position. The only gap to a strict Linux peer is the RSN-IE/PMKID/PRIVACY wrapper (P2d.3), and a
real cross-vendor PMKID needs **P3 (SAE)**. The Linux mesh was stopped afterward (bench restored).

## Bench note

While freeing a serial port for flashing, a `fuser -k` on `/dev/ttyACM2` killed `/tmp/ppk2_hold.py`
(the PPK2 power-hold for board2 — the USB ports had re-enumerated, so ACM2 was the PPK2, not board2's
console). That cut board2's 5 V DUT power. Restored by re-running `/tmp/ppk2_hold.py` with the IDF
python (which has `ppk2_api`): board2 re-enumerated and power is held again (ampere-meter, DUT ON).
Lesson: identify each `/dev/ttyACM*` by USB VID:PID (Espressif `303a:1001` vs Nordic PPK2 `1915:c00a`)
before assuming a port is a board console — USB re-enumeration shuffles the ACM numbers.
