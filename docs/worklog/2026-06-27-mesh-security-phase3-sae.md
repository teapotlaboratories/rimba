# 2026-06-27 — Mesh security Phase 3 (SAE): P3a auth-frame plumbing + the SAE shim

Phase 3 replaces P2's static PMK with a real per-peer SAE-derived PMK/PMKID, by driving morselib's
already-compiled `src/common/sae.c` from a native umac mesh SAE FSM. Plan + recon:
`docs/mesh-ap/rimba-mesh-p3-sae-plan.md`. Phases: **P3a** auth-frame plumbing + the SAE shim (this
doc) → P3b per-peer SAE FSM → P3c PMK seam + reorder → P3d gold-standard ESP↔live-Linux.

## The `mesh_sae` shim — SAE callable from morselib

`sae.c` (+ `dragonfly.c`) are compiled into `mmhostap`, but `sae.h` needs hostap's `u8`/`wpabuf`
types, which clash with morselib's headers. So the SAE crypto is wrapped in a thin shim that lives on
the hostap side and exposes a clean `uint8_t`-based API:

`framework/src/hostap/mesh_sae.c` (added to the hostap CMake SRCS) — includes the hostap prelude +
`common/sae.h` + `utils/wpabuf.h` + `hostap_morse_common.h` (the `mmint_*` mangle, so `wpabuf_*` →
`mmint_wpabuf_*`; `sae_*` are unmangled and resolve directly). It exposes:
`mesh_sae_alloc`/`_free`, `mesh_sae_build_commit` (→ `sae_set_group(19)` + `sae_prepare_commit` +
`sae_write_commit`), `mesh_sae_process_commit` (→ `sae_parse_commit` + `sae_process_commit`, which
fills `sae->pmk/pmkid`), `mesh_sae_build_confirm`, `mesh_sae_check_confirm`, `mesh_sae_get_keys`
(→ the 32-byte PMK + 16-byte PMKID), `mesh_sae_state`. `umac_mesh.c` calls these via local `extern`
declarations — exactly how it reaches `mmint_sha256_prf`. **Compiles + links clean** (the pivotal P3
de-risk: morselib can drive the SAE crypto).

## Auth-frame plumbing (`umac_mesh.c` + the mesh datapath)

- `umac_mesh_tx_auth(peer, transaction, status, sae_body, len)` — builds an 802.11 Authentication
  frame via `frame_authentication_build`. **Gotcha:** that builder appends `auth_data` verbatim for
  non-OPEN algorithms (no seq/status), so the body must be `le16(transaction) ‖ le16(status) ‖
  <SAE body>`. `auth_alg = SAE`, DA(A1)+A3 = peer, SA(A2) = our mesh MAC. TX'd via the common stad.
- `umac_mesh_handle_auth(umacd, rxbufview)` — `frame_authentication_parse` strips seq/status and
  returns `seq`(=transaction), `status_code`, and `auth_data` = the SAE body (exactly what
  `sae_parse_commit`/`sae_check_confirm` consume). P3a: parse + log; P3b drives the FSM.
- `umac_datapath.c`: route `DOT11_FC_SUBTYPE_AUTH` to `umac_mesh_handle_auth` in
  `process_rx_mgmt_frame_mesh`, and add `MGMT/AUTH` to `frames_allowed_pre_association_mesh` (else RX
  SAE is filtered pre-association). SAE auth is 802.11-unprotected, so the protected-bit pre-assoc
  drop doesn't apply.

## On-air proof (2026-06-27, board0 + board1, chronium `morse0` monitor)

A test Commit is emitted from `mmwlan_mesh_peer_open` (TEMP P3a scaffolding — a local `mesh_sae`
instance, no FSM; P3b replaces it). The existing AMPE-over-static-PMK peering is untouched.

```
board0/board1 console:  MESH-SAE tx commit -> <peer> len=98 st=0      (group-19 commit = 2+32+64)
                        MESH-SAE rx auth from <peer> txn=1 status=0 body_len=98
                        netif up=1   (AMPE peering still works alongside)
chronium pcap (tshark): Authentication / algorithm = SAE (3) / Status = Successful
                        SAE Message Type = Commit (1) / Group Id = 19 (256-bit ECP, P-256)
                        Scalar = <32 B> ; Finite Field Element = <64 B>
```

The ESP emits a byte-correct, standard 802.11 SAE Commit (group 19), the peer parses it, and the
mesh stays up. The SAE crypto runs natively on the ESP (the Dragonfly PWE + scalar/element from
`mesh_sae_build_commit`). **P3a done.** Next: P3b — the per-peer SAE FSM (Commit/Confirm →
`sae_process_commit` → per-peer PMK + PMKID), then P3c feeds that PMK into the AMPE.

TEMP to clean up before a main merge: the `<stdio.h>` + `printf` SAE traces and the test-Commit
scaffolding in `mmwlan_mesh_peer_open` (replaced by the P3b FSM). — *Done in P3b below.*

## P3b — the per-peer SAE FSM (Commit/Confirm → PMK + PMKID)

P3b replaces P3a's one-shot test-Commit with a real **per-peer simultaneous-open SAE state machine**
in `umac_mesh.c`. It drives the `mesh_sae` shim (the compiled `src/common/sae.c`) through the full
Dragonfly handshake and derives a per-peer **PMK + PMKID**. The crypto already linked in P3a; P3b is
the *protocol FSM* — which `sae.c` deliberately does not implement (it is pure crypto; the AP's
`src/ap/ieee802_11.c sae_sm_step` owns the state machine, and that is what this ports).

**Scope boundary (important):** SAE runs **alongside** the working MPM/AMPE peering and is **not yet
load-bearing** — AMPE still derives MTK/AEK/PMKID from the static `mesh_p2_pmk`. P3b's sole output is
`peer->pmk`/`peer->pmkid`; **P3c** does the PMK seam (feed `peer->pmk` into `mesh_derive_mtk/_aek`,
real PMKID in the MPM IE) and the SAE-before-Open reorder. So P3b keeps emitting the MPM Open at
discovery (the open-mesh shape) in parallel with SAE — the auth frames are new, the peering timing is
unchanged. **Proof = two ESP peers derive byte-identical `peer->pmkid`** (a deterministic function of
the PMK, so matching PMKIDs ⇒ matching PMKs).

### Where the Linux state machine lives (recon)

hostap splits it: `wpa_supplicant/mesh_rsn.c` is glue (allocates `sta->sae`, builds the first Commit,
kicks `auth_sae_init_committed`, arms a mesh re-auth timer, and after accept derives AEK/MTK from
`sta->sae->pmk`); the **actual FSM** is `src/ap/ieee802_11.c` `sae_sm_step` (990) + `handle_auth_sae`
(1329); the crypto is `src/common/sae.c`. The mesh specialisation: on `SAE_NOTHING + Commit` it sends
Commit **and** Confirm and jumps straight to `SAE_CONFIRMED` (ieee802_11.c:1041-1052); accept is on
the peer's Confirm while `SAE_CONFIRMED` (1138-1140 → `sae_accept_sta` 940). Two timers: the SAE-frame
retransmit (`auth_sae_retransmit_timer` 861, ~1 s, mesh-only) and the higher re-auth timer
(`mesh_auth_timer` 10 s). The anti-thrash cap is `conf->sae_sync` (default **3**, strict `>`); on
exceed the instance resets to `NOTHING` and is disabled 10 s (`sae_check_big_sync` 821-836).

### What landed (`umac_mesh.c` + the shim)

- **`struct mesh_peer`** += `void *sae; uint8_t sae_state; uint8_t sae_sync; uint32_t sae_last_tx_ms;
  uint32_t sae_disabled_until_ms; uint8_t sae_commit[…]/sae_commit_len; uint8_t sae_confirm[…]/len;
  uint8_t pmk[32]; uint8_t pmkid[16]; bool pmk_valid;`. State is mirrored **in the peer**, not the
  sae handle, because `sae.c` never advances `sae->state` and `mesh_sae_build_commit`'s internal
  `sae_set_group` resets the handle. Zero-init is free (the existing `memset` in `mesh_peer_alloc`).
- **`mesh_sae_start(peer)`** — alloc + build + send our Commit → `COMMITTED` (== `mesh_rsn_auth_sae_sta`
  + `auth_sae_init_committed`). Idempotent. **The cached-Commit rule:** `mesh_sae_build_commit` is
  *destructive* (`sae_set_group`→`sae_clear_data` wipes pmk/state/scalars), so it runs **exactly once**
  per session and every retransmit/resync **replays the cached `peer->sae_commit` bytes** — never
  rebuilds. This is the #1 byte-exactness/lifetime trap.
- **`mesh_sae_handle_rx(peer,txn,status,body,len)`** — ports `handle_auth_sae` + `sae_sm_step` (mesh
  branches): lazy responder alloc on a first Commit; `COMMITTED+Commit`→process+Confirm→`CONFIRMED`;
  `CONFIRMED+Commit`→resync (resend cached Commit + reprocess + Confirm); `COMMITTED+Confirm`→resync
  (Confirm body *not* validated, per the mesh gate at ieee802_11.c:1584); `CONFIRMED+Confirm`→
  `check_confirm`+`get_keys`→**`ACCEPTED`** (the PMK lands here); `ACCEPTED+Confirm`→replay cached
  Confirm; `ACCEPTED+Commit`→re-derive in place (P3b divergence — hostap frees the STA at 1144-1151;
  in P3b, SAE isn't gating the link yet so we just restart the handshake).
- **`umac_mesh_handle_auth`** rewritten from the P3a logger: parse → find peer → `mesh_sae_handle_rx`.
  Unknown-peer auth is **dropped** (like Linux's `mesh_pending_auth`, which waits for a peer candidate
  rather than auto-creating a STA) — beacons create the peer within ~100 ms and the sender's SAE
  retransmit redelivers; this keeps `mmwlan_mesh_peer_open` the sole MPM-Open initiator.
- **SAE retransmit** added to `umac_mesh_plink_tick`, keyed off `sae_state` + `sae_last_tx_ms` (~1 s),
  **independent** of the MPM-Open retransmit (`peer->state`). Each fire bumps `sae_sync`; over the cap
  → `mesh_sae_big_sync_reset` (10 s lockout). A self-heal re-kicks SAE once a lockout expires.
- **`mesh_sae_start` call sites:** `mmwlan_mesh_peer_open` (replaces the P3a test-Commit; the MPM Open
  below it stays) and the new-peer branch of `umac_mesh_handle_action` (responder learned from an Open).
- **`mesh_peer_free`** frees `peer->sae` (one site covers all 5 teardown paths) — mirrors
  `sta_info.c:427-428`.
- **Shim:** one addition — `mesh_sae_clear_temp()` → `sae_clear_temp_data` (frees the P-256 bignum
  scratch after accept while keeping pmk/pmkid; == ieee802_11.c:1169). The existing P3a entry points
  are reused unchanged.
- **Cleanup:** `<stdio.h>` + all `printf` SAE traces removed; the ACCEPTED proof line is `MMLOG_INF`
  with the **PMKID only** (public on-wire identifier in the MPM/RSN IE — never the PMK, per the
  no-key-logging rule).

### Byte-exactness / lifetime traps honoured

1. **Cached, non-destructive Commit** (above) — the dominant trap.
2. **Anti-thrash matches hostap exactly:** `check_big_sync` *before* `sync++` (check-then-increment),
   strict `>` `MESH_SAE_SYNC_MAX=3` (so `sync` can reach 4), 10 s lockout — same as ieee802_11.c.
3. **`clear_temp` at accept, not `free`:** the PMK must survive (P3c reads it); freeing the whole
   `sae` at accept would lose it. tmp is dropped to reclaim the bignum scratch.
4. **PMK source field:** `mesh_sae_get_keys` copies the 32-byte PMK + 16-byte PMKID — the same 32/64
   convention `mesh_derive_mtk` (key len 32) / `_aek` (key len 64 = PMK‖32 zeros) already use, so
   P3c is a drop-in source swap.
5. **Group 19 / H2E=0 fixed** in the shim — must match the Linux peer's `sae_groups`/PWE policy for
   P3d cross-vendor interop (hunting-and-pecking).

### Verification

- **Build:** `make build APP=rimba-halow-mesh BOARD=proto1-fgh100m` — **clean** (links clean, zero
  warnings; `rimba_halow_mesh.bin` generated). The morselib FSM resolving `mesh_sae_*` against the
  hostap-side shim re-confirms the P3 linkage premise.
- **On-air — DONE + PROVEN (2026-06-27, board0+board1, chronium `morse0` monitor):**
  - **PMKID match (the decisive proof):** both boards reached SAE ACCEPTED with a **byte-identical
    PMKID** — board0 logged `MESH-SAE e2:72:a1:f8:f9:40 ACCEPTED pmkid=cff6be63…3212d57e`, board1
    logged `MESH-SAE e2:72:a1:f8:ef:a4 ACCEPTED pmkid=cff6be63…3212d57e` (same 16 bytes). Identical
    PMKID ⇒ identical PMK ⇒ the Dragonfly handshake completed and both sides agreed (cross-validated).
    Both then logged `MESH peer … ESTABLISHED+secured … (sae_pmk=1)`.
  - **On-air SAE (tshark on the pcap):** the 4-frame exchange between the two mesh MACs — `auth_alg=3`
    (SAE), Commit (seq 1) + Confirm (seq 2) **both directions**, status success, body len **98**
    (group-19: 2 group + 32 scalar + 64 element). One Confirm retransmit observed (the ~1 s SAE
    retransmit timer — expected).
  - **No regression (AMPE intact on-air):** the MPM self-protected action frames (Open 0x01 / Confirm
    0x02, both ways) still carry the full IE set **217 / 48 / 114 / 113 / 117 / MIC(140)** + the
    encrypted AMPE element — byte-shape-identical to the P2d gold standard. SAE runs **alongside** the
    AMPE/MPM flow exactly as scoped (parallel, not load-bearing).
  - **Verification-build caveat:** the committed proof line is `MMLOG_INF`, which is a
    `printf_blackhole` no-op in the default build (`MMLOG_LEVEL` resolves to `ERR` — neither
    `MMLOG_LEVEL_OVRD`/`_DEFAULT` is defined; INF/DBG/WRN are compiled out). Raising the **global**
    MMLOG level to INF for visibility crash-looped both boards (interrupt-WDT timeout — the INF log
    flood blocks the USB-CDC console inside interrupt-disabled sections during driver init; backtrace
    = `gpio_isr_register`→`esp_intr_alloc` on `ipc_task`, unrelated to the SAE code). So the proof was
    captured with **targeted `printf` traces** (P3a-style TEMP scaffolding: Commit-sent, ACCEPTED
    pmkid, ESTABLISHED — reverted before commit). The default-level P3b firmware is stable; only the
    flood instrumentation tripped the WDT. ESP↔live-Linux-SAE byte-diff is **P3d** (the SAE PMKID is
    not yet on-air in P3b — the MPM IE still carries the P2d.3 placeholder until P3c does the seam).

**Code-map:** the function-level new-code↔Linux map is in
[`docs/mesh-ap/rimba-mesh-security-codemap.md`](../mesh-ap/rimba-mesh-security-codemap.md) (§ P3b SAE FSM).

**Next:** P3c — feed `peer->pmk` into `mesh_derive_mtk/_aek`, put the real `peer->pmkid` in the MPM IE,
reorder SAE-before-Open (gate the Open on `pmk_valid`), then P3d gold-standard ESP↔live-Linux SAE.

## P3c — SAE PMK made load-bearing: PMK seam + real PMKID + SAE-before-Open reorder

P3c flips SAE from observational (P3b) to **load-bearing**: the AMPE AEK + per-pair MTK now derive from
the SAE-negotiated `peer->pmk`, the real SAE PMKID goes on-wire in the MPM IE (reciprocal-checked on RX),
and the MPM Open is **deferred until SAE accepts** — converging to the Linux secure-mesh shape (discover →
SAE only → on accept derive AEK + send the protected Open → MPM/AMPE → ESTAB, MTK from the SAE PMK). The
static `mesh_p2_pmk` and the placeholder PMKID are deleted.

**Designed via a recon→design→adversarial-review workflow.** The review caught three real issues folded
into the implementation (not in the first design): (1) the P3b in-place SAE-reauth desyncs keys under a
per-handshake PMK — replaced with hostap's **free-and-restart** (`mesh_sae_reauth_free` == `ap_free_sta`,
ieee802_11.c:1144-1151); (2) the AEK must be derived **inside** the `state==LISTEN` guard so a reauth
can't swap the AEK out from under an installed MTK; (3) asymmetric SAE-finish timing could time out the
faster peer's MPM budget — fixed by resetting `peer->retries` on every auth-frame RX.

### What landed (`umac_mesh.c`)
- **PMK seam:** `mesh_derive_mtk` reads `peer->pmk` (32 B); `mesh_derive_aek` reads `peer->pmk` (PMK ‖ 32
  zeros = 64 B). The 32/64 convention is unchanged — only the source moves off the deleted static PMK.
- **PMKID seam:** `mesh_peering_params += pmkid`; `tx_peering` / `build_peering` write `peer->pmkid`
  (placeholder deleted); `mesh_parse_peering_ie` emits the advertised PMKID; `handle_action` adds the
  **reciprocal check** (`memcmp(adv, peer->pmkid)` — drop-frame-only on mismatch, == mesh_rsn.c:689-695),
  hardened to also drop an AMPE Open/Confirm with no PMKID.
- **Reorder:** alloc-time AEK removed; `mmwlan_mesh_peer_open` starts SAE with **no Open** (peer stays
  LISTEN); the SAE-accept hook derives the AEK from `peer->pmk` and sends the first protected Open →
  OPN_SNT (== `mesh_mpm_auth_peer` mesh_mpm.c:716); `handle_action` gains the **`pmk_valid` gate** (drop
  peering until SAE accepted, == mesh_mpm.c:1272-1278). MTK installs at ESTAB from `peer->pmk`.

### On-air verification — DONE + PROVEN (2026-06-28, board0/board1/board2, chronium monitor)
Diagnosed with TEMP `printf` traces (P3a-style, no INF flood — reverted before commit):
- **SAE load-bearing for the datapath (decisive):** with board1 pointed at its **direct** peer board0,
  `reply from 10.9.9.136: seq=1..33` — **33/33 encrypted ICMP replies (~18-35 ms, 0% loss after warmup)**.
  The unicast MTK = `sha256_prf(peer->pmk, …)`, so a working encrypted ping proves the SAE PMK feeds AMPE
  + CCMP end-to-end.
- **All pairs ESTAB on the SAE PMK:** board0↔board1 and board0↔board2 both reach ESTAB with
  `pmk_valid=1 nonce_ok=1 mgtk_ok=1`; SAE PMKIDs are byte-identical per pair (board0↔board1
  `54f7af73…4826e6d8`, board0↔board2 `7289a11c…1a723436`); the reciprocal PMKID check passes (`match=1`),
  AMPE verify passes (no failures).
- **PMKID seam on-air (tshark):** the Mesh Peering Management IE (117) now carries **SAE PMKIDs**
  (`58e45457…`, `811b01d2…`), **not** the old placeholder `52494d42…` ("RIMBA-MESH-PMKID").
- **Reorder on-air:** the SAE auth (Commit/Confirm) exchange precedes the MPM Open on each link; the Open
  is emitted only after SAE accept.

**Scope note — the multi-hop relay ping is a *separate, pre-existing* open item, not a P3c regression.**
The relay topology (board1 → board2 via board0) times out because the **unicast relay forward**
(`umac_mesh_forward_data`) still needs the next-hop **pairwise** MTK — a follow-up flagged before P3c. P3c
only changed the per-link keying/ordering, which the direct-peer encrypted ping confirms works. (A
verification-build caveat carries over from P3b: the committed `MMLOG_INF` proof lines are
`printf_blackhole` no-ops at the default `MMLOG_LEVEL=ERR`; raising MMLOG globally crash-loops the boards
via an interrupt-WDT timeout from the log flood, so proofs are captured with targeted printf, reverted.)

**Code-map:** function-level new-code↔Linux map in
[`docs/mesh-ap/rimba-mesh-security-codemap.md`](../mesh-ap/rimba-mesh-security-codemap.md) (§ P3c).

**Next:** P3d — ESP joins a LIVE Linux SAE+AMPE mesh; byte-diff the ESP Commit/Confirm vs a real Linux
peer and confirm cross-vendor dragonfly PMK/PMKID agreement + encrypted ICMP ESP↔Linux. (Plus the
deferred unicast-relay-forward next-hop-MTK follow-up.)

## P3d — ESP ↔ live-Linux SAE interop (cross-vendor dragonfly agreement)

**Goal.** The gold standard from the porting directive: not "ESP↔ESP agree", but "ESP agrees with a
*real Linux* HaLow SAE peer". Bench: **chronite** (Pi, HaLow MAC `3c:22:7f:37:51:38`, `~/halow/rpi-linux`
kernel + `~/halow/hostap`) brought up as a live `wpa_supplicant_s1g` mesh node — `ssid=rimba-mesh`,
`key_mgmt=SAE`, `sae_pwe=0`, `sae_groups=19`, `op_class=68 channel=27`, `sae_password=rimbamesh2026`,
`dtim_period=1` — sharing the air with board0. (chronite's SSH rides `wlan0`; only the `wlan1` s1g
supplicant is ever killed.)

**Two blockers found and fixed; one remains.**

**(1) chronite never treated the ESP as a candidate.** Linux's `mesh_matches_local`
(`net/mac80211/mesh.c:62`) gates candidacy on, among other fields, the beacon's **mesh authentication
protocol identifier** (`ifmsh->mesh_auth_id == ie->mesh_config->meshconf_auth`, `mesh.c:87`) — and a
secured mesh additionally needs an **RSN IE** in the beacon. The ESP beacon was advertising
`meshconf_auth = 0x00` (Open) and carried **no** RSN IE, so chronite silently rejected it. Two beacon
fixes:
- `mesh_build_config_ie`: the Mesh Configuration IE's Authentication Protocol byte `0x00` → **`0x01`**
  (SAE). This is the byte Linux emits at `mesh.c:291` (`*pos++ = ifmsh->mesh_auth_id;`) and compares at
  `mesh.c:87`.
- `umac_mesh_build_beacon`: prepend the **RSN IE** (`mesh_rsn_ie`, group+pairwise CCMP-128, AKM 8 = SAE)
  before the Mesh ID, mirroring Linux `mesh_add_rsn_ie` (`mesh.c:374`, emitted into the beacon at
  `mesh.c:1108`). The `mesh_rsn_ie` blob + `DOT11_IE_RSN(48)` had to move **above** the beacon builder
  (it was defined later in the file, after its first use).

After (1) chronite accepts the ESP and **runs SAE** with it (Commit/Confirm exchanged on-air).

**(2) cross-vendor SAE thrashed — confirm never matched.** Both sides simultaneous-open; both kept
**restarting their commit**, so the send-confirm counters never lined up and the confirm hash never
verified. Root cause, traced with targeted `SAEDBG`/`MPMDBG` printf (reverted): board0 was **destroying
its SAE session on every MPM CLOSE/HOLDING**. While the ESP finished the AMPE side, chronite's plink FSM
floods CLOSE (action 3) — and the ESP's old CLOSE/HOLDING paths freed the peer **and its `sae`**, so the
next beacon started a *fresh* commit, resetting the dragonfly. Linux never does this: `sta->sae` lives in
the station and is freed **only** in `ap_free_sta` (`hostap/src/ap/sta_info.c:427-428`), not on a plink
reset. Fixes, matching that lifetime:
- **CLOSE while mid-SAE / PMK-valid no longer tears down the SAE.** `handle_action`'s CLOSE branch, when
  `peer->state != ESTAB && (peer->sae || peer->pmk_valid)`, now resets the *plink* to LISTEN, keeps the
  SAE/PMK intact, and re-fires the protected Open if the AEK is ready — instead of invalidating + freeing.
- **HOLDING/retries-exhausted keeps a PMK-valid peer alive** (`umac_mesh_plink_tick`): rather than CLOSE→
  HOLDING→free, a `pmk_valid` peer resets its retry budget and re-sends the Open.
- **Re-auth retransmits the cached Confirm instead of freeing.** The ACCEPTED-state Commit case (a peer
  that already finished SAE seeing a duplicate Commit) now retransmits `peer->sae_confirm` if present,
  rather than the old free-and-restart (`mesh_sae_reauth_free`, deleted) — Linux answers a retransmitted
  commit, it doesn't reset.

**Result — cross-vendor SAE CRACKED.** board0 and chronite derive **byte-identical PMKIDs**
(`855627ac3141c41d7e75f0e269d10283` on both consoles) and the confirm mismatch count dropped from
constant to **zero**. This is the P3d headline: an ESP and a real Linux HaLow node complete the SAE
dragonfly handshake and agree on the same PMK/PMKID. (Verified on chronium's `morse0` monitor + both
consoles; MMLOG proof lines are `printf_blackhole` no-ops at the default `MMLOG_LEVEL=ERR`, so the
PMKID/confirm proof was captured with targeted printf, reverted before commit.)

**Remaining blocker (next task, distinct layer): the AMPE MIC verify.** After SAE, chronite sends its MPM
Open (action 1, ~226 B); board0's **AES-SIV AMPE-MIC verification of that Open fails**
(`mesh_process_ampe` → `mmint_aes_siv_decrypt(peer->aek, …, 3, aad, aad_len, …)`), so board0 never
answers → chronite's plink → HOLDING → `BLOCKED` (~300 s). Since the PMK/PMKID agree, the AEK
(`= PRF(pmk‖0…, …)`) should too — so this is a **cross-vendor AES-SIV detail**, most likely the AAD
component order (`{SA, RA/own-MAC, body}`) or a length/field framing that was only ever validated
ESP↔ESP, never against real Linux. No encrypted ESP↔Linux ICMP yet; that waits on this MIC.

**Committed in P3d:** the four real fixes above (beacon auth byte `0x01`, beacon RSN IE, SAE
CLOSE/HOLDING stability, re-auth→retransmit-Confirm). The `MESH_LINUX_INTEROP` A/B flag in `app_main.c`
and all `SAEDBG`/`MPMDBG` scaffolding were reverted. **Code-map:** § P3d in
[`docs/mesh-ap/rimba-mesh-security-codemap.md`](../mesh-ap/rimba-mesh-security-codemap.md).

## P3d (continued) — AMPE AES-SIV MIC: crypto model offline-validated vs hostap; blocker narrowed

The post-SAE blocker (board0's AES-SIV MIC verify of chronite's MPM Open fails) was **de-risked
analytically** before more bench churn. Method: extract chronite's own ground truth from its
`wpa_supplicant_s1g -dd -K` log (it dumps `SAE: PMK`, the MPM frame, the plaintext + encrypted AMPE
element), then **replay it offline** through a known-correct AES-SIV (pycryptodome `MODE_SIV`, first
checked against the RFC 5297 §A.1 KAT). Script: `docs/mesh-ap/ampe-siv-validate.py`.

**Result — the entire ESP AMPE crypto model is byte-correct vs hostap.** Using chronite's logged PMK
`8c8110bc…` for the peering at log line 4009 and its transmitted Confirm-bearing frame, the offline
oracle reproduces chronite's encrypt exactly:
- **AEK** = `sha256_prf(PMK‖32 zeros [64 B], "AEK Derivation", AKM_SAE(00 0f ac 08) ‖ min(MAC) ‖ max(MAC))`.
  (HMAC zero-pads a 32-B key to 64, so `PMK‖zeros` ≡ `PMK` — the 64-vs-32 question is moot; both yield
  the same AEK, and the ESP's `pmk64` is correct.)
- **AAD** = `{ own_MAC, peer_MAC, cat6 }` in that order, where `cat6` = the **first 6 action-body bytes**
  (`0f 03 72 0a 72 69` = category, action, then the Mesh ID IE's `EID/len/'r'/'i'`). Every other AAD
  ordering or `cat` length **fails** the verify — only this one passes, recovering the logged plaintext
  `8b 44 00 0f ac 04 …`.
- **AES-SIV** key length 32 (AES-128-SIV); `crypt` = `SIV(16) ‖ ciphertext`.

The ESP source (`mesh_derive_aek`, `mesh_process_ampe`'s `aad = {sa, mesh_ctx.mesh_mac, body}` +
`crypt = &body[mic_off+2]`) **matches this validated model line-for-line**. And `mmint_aes_siv_*` is
hostap's own `src/crypto/aes-siv.c` compiled into the morse lib (per the extern's comment) — i.e. the
same reference the oracle confirmed. So the AES-SIV algorithm, the AEK derivation, and the AAD are **not**
the bug.

**Blocker therefore narrowed to board0's *runtime* AES-SIV inputs** — i.e. whether, on the live RX frame,
board0's `peer->aek`, `body[0:6]`, and reconstructed `crypt` are the bytes the model expects. Capturing
that needs a one-shot `AMPEDBG` dump (added in `mesh_process_ampe`, reverted after) during a *completed*
ESP↔Linux SAE→AMPE exchange — and that exchange is the bottleneck (next paragraph). The dump fired
**once** (board0 did reach `mesh_process_ampe` on chronite's Open), but a clean capture needs the SAE to
converge reliably.

**NEW finding — cross-vendor SAE re-sync deadlock (blocks reliable AMPE testing; dynamic-join concern).**
After a one-sided restart, the bench wedges in a repeatable state: board0 **accepts SAE and races to the
MPM Open**, while chronite stays `MPM: SAE not yet accepted for peer` and just drops the Opens. Neither
re-prompts: chronite won't re-send its Commit, and board0 (ACCEPTED) only retransmits its Confirm *in
response to* a Commit — so board0's Confirm, if lost once, is never re-offered and chronite waits forever.
The earlier breakthrough run converged by frame-timing luck; the steady state can deadlock. This is the
same class as task #9 (dynamic device join) — a robust ESP should retransmit its SAE Confirm while
ACCEPTED-but-not-yet-ESTAB, not only on a received Commit. (USB-CDC also drops on RTS-reset and on heavy
per-byte `printf` in the beacon-TX/irq context — dump via one prebuilt-buffer `printf`, and reset by
reflashing, not RTS.)

**Status:** P3d's headline (cross-vendor SAE) is committed and the AMPE crypto is proven correct on paper
against the live Linux peer; the remaining work is (1) make cross-vendor SAE converge deterministically
(fix the re-sync deadlock), then (2) capture board0's runtime `AMPEDBG` and diff vs
`ampe-siv-validate.py`. No encrypted ESP↔Linux ICMP yet.

### ⚠ On-air verification GAP (P3d AMPE crypto) — NOT yet satisfied

The P3d AMPE-crypto result above is **offline-validated only** — against chronite's *own*
`wpa_supplicant_s1g -dd -K` log (the transmitter's claim of what it sent) + the RFC 5297 KAT, replayed
through pycryptodome. It is **NOT** independently confirmed on chronium's `morse0` monitor, so per
[[verify-onair-chronium-monitor]] (logs/replay are not sufficient; the gold standard is the actual
on-air bytes byte-diffed against a live Linux device) this finding is **pending on-air confirmation**.

Attempted the chronium capture (2026-06-28) and confirmed three concrete blockers (evidence, not
assumption — 30 s `morse0` capture, raw-hex MAC presence):
- **board0 (`e272…efa4`) = 0 frames** on chronium, while **chronite (`3c22…5138`) = 297** and
  **board1 (`…f9:40`) = 310**. chronium is on the right channel (it hears the Linux node) but board0 is
  **out of chronium's RF range** — chronite hears board0 (short hop) but chronium does not. The earlier
  ESP↔ESP on-air diffs worked only because those boards sat in chronium's range.
- **chronite is SAE-deadlocked** (re-sync deadlock, this worklog) → it emits **no fresh AMPE frame** to
  capture until SAE converges.
- **stock tshark mis-parses S1G self-protected action frames** (8 of 896 decoded as mgmt) → a byte-diff
  needs raw-hex extraction (`tcpdump -xx` + manual parse), not `tshark -Y`.

**To close (fold into #12/#13):** fix the SAE re-sync deadlock so chronite transmits a real AMPE Open →
place board0 within chronium's RF range → capture that frame on `morse0` → byte-diff vs (a) chronite's
`-K` log and (b) board0's runtime RX inputs. That single capture yields BOTH the on-air confirmation and
the runtime board0 input check (#12) at once. Until then: AMPE crypto = "offline-validated vs hostap",
**not** "on-air verified".

## #13 — cross-vendor SAE re-sync deadlock RESOLVED (restore hostap reauth on ACCEPTED+Commit)

**Root cause.** board0's SAE FSM deviated from hostap on the `MESH_SAE_ACCEPTED` + Commit (txn 1) case.
hostap mesh (ieee802_11.c:1140-1148) does `ap_free_sta` — frees the STA + PMKSA and re-runs SAE from
scratch — because a Commit arriving on an already-ACCEPTED link can only mean the peer genuinely
**restarted** (a peer still in simultaneous-open retransmits its *Confirm* (txn 2), never a Commit). P3d
had replaced that with "retransmit our cached Confirm", so after a one-sided restart (peer reboot /
supplicant restart) board0 stayed ACCEPTED and **replayed a stale Confirm the restarted peer could never
verify against its new commit pair** → permanent deadlock (board0 races to MPM Open; Linux stays "SAE not
yet accepted"). P3d removed the reauth to stop a thrash, but that thrash was the *CLOSE handler* freeing
our SAE (a separate cascade, fixed in P3d and kept); reauth itself was not the cause.

**Fix.** Restored `mesh_sae_reauth_free` (invalidate paths + free peer == `ap_free_sta`) for the
ACCEPTED+Commit case; ACCEPTED+Confirm (txn 2) still resends our Confirm (== hostap's else branch). The
big-sync lockout is the anti-thrash backstop (== hostap `sae_check_big_sync`). One-line behavioural diff,
+24/−12, no scaffolding.

**Verified against the LIVE Linux peer (chronite, 2026-06-28).** Parked board1/board2 (clean air), flashed
board0 (fix + interop), restarted chronite. chronite's own state machine now **completes SAE with board0**
— repeated `SAE: State Nothing -> Committed -> Confirmed -> Accepted for peer e2:72:a1:f8:ef:a4` — whereas
before the fix it was stuck emitting only `MPM: SAE not yet accepted for peer` (deadlock). The deadlock is
broken and SAE now converges reliably cross-vendor.

**The remaining re-cycle is the #12 AMPE blocker, NOT a #13 thrash.** chronite receives **only PLINK
action 1 (Open) from board0 (×4552), never action 2 (Confirm)**, and board0 **never reaches plink ESTAB**:
board0 cannot verify chronite's Open AMPE element (the AES-SIV MIC, #12), so it never answers with a
Confirm → chronite times out → CLOSE flood (×2237) → peer torn down → fresh SAE. So #13 (SAE) is fixed;
ESTAB is gated on #12 (AMPE). Bonus: reliable SAE convergence now unblocks capturing board0's runtime
`AMPEDBG` for #12.

**On-air gap (same as #12):** this is verified via the live Linux peer's state machine (the Linux node
received + verified board0's SAE frames on-air and advanced to Accepted), **not** yet a chronium `morse0`
byte-capture of board0's frames — board0 is still out of chronium's RF range. To fully close: relocate
board0 into chronium range and capture the SAE 4-frame exchange + reauth on `morse0`.

### #13 verification (adversarial workflow) + hardening — "does it follow Linux?"

Ran an adversarial verify→refute→synthesize workflow (5 dimensions, each grepped against chronite's
actual hostap tree + the ESP source, then a skeptic tried to refute each). Verdict: the reauth **direction
is faithful** (free on genuine restart == `ap_free_sta`; beacon-driven re-init; retransmit state→frame
mapping all match), but the first cut was **NOT a line-by-line port** — it freed on **any** Commit (txn 1)
in ACCEPTED, skipping hostap's `handle_auth_sae` validation envelope. The refute pass caught a subtle
point the verify pass got wrong: hostap's reject paths (`reply:`/`remove_sta:`) do **not** free an MPM
mesh peer (`added_unassoc` is false, ieee802_11.c:1657), so hostap **keeps** an ACCEPTED link on a
malformed/reflected/status-bearing Commit — while the ESP tore it down. That made the unconditional free a
real **single-frame link-flap / DoS** on an established secure link (a forged Commit spoofing the peer MAC
flaps the link + invalidates its paths).

**Hardening (follow hostap exactly).** Gate the reauth before `mesh_sae_reauth_free`: require
`status==0` (== :1466) + a well-formed Commit for our group (length + group-id match, == :1457-1462) +
non-reflection (received scalar‖element ≠ our cached commit's, == :1511). Only the well-formed, success,
non-reflected Commit a genuine restart emits tears the link down; anything else is dropped with the link
intact (matching hostap). **On-air re-verified (2026-06-28):** with the gate, chronite still reaches
`SAE: State …→Accepted for peer e2:72:a1:f8:ef:a4` (×33) — the gate does NOT block genuine-restart
recovery, so the deadlock fix is preserved. **Residual:** the gate does length/group/reflection but not the
full `sae_parse_commit` crypto validation (scalar-in-range / element-on-curve) — a narrower crafted-garbage
vector; tracked as a follow-up. Two adjacent divergences the workflow surfaced (pre-existing, tasks #14/#15):
ACCEPTED+Confirm has no rc/0xffff anti-replay + big_sync (ESP↔ESP Confirm-storm risk); the MPM-Open
SAE-start is ungated vs hostap's PLINK_OPEN + cached-PMKSA check (mesh_mpm.c:1257-1265). Code-map: § #13 in
the security code-map.

## #12 — AMPE AES-SIV MIC: ROOT CAUSE FOUND (S1G-vs-11n frame representation in the AAD)

With #13 making SAE converge reliably, board0 reliably reaches `mesh_process_ampe` on chronite's Open, so
I captured board0's **runtime** AES-SIV inputs (one-shot `printf` of `aek` + the 3 AAD components + `crypt`)
and replayed them through the offline oracle (`ampe-siv-validate.py`). Decisive result:

- **board0's `crypt` is byte-for-byte chronite's transmitted ciphertext** (matched chronite's logged
  `encrypted AMPE element`) — so crypt reconstruction (MIC element SIV ‖ trailing ciphertext) is correct.
- **The AAD diverges.** Oracle `decrypt_and_verify` FAILS on board0's inputs, and CTR-only (AEK+crypt,
  AAD-independent) yields garbage → so it's not just the AAD order; the AEK or the AAD bytes differ. Pinned
  to the **AAD body6**: board0 sees `body[0:6] = 0f 01 10 00 dd 0e`, but chronite's frame body is
  `0f 01 10 00 01 08 …`. First 4 bytes (category, action, capability) match; **bytes [4:5] differ —
  `dd 0e` vs `01 08`**, the EID+len of the first element. (Dumping `body[0:72]`: board0's frame carries
  **S1G** IEs — vendor `dd 0e`, EID 217 `d9 0f` — where chronite's *logged* frame carries **legacy** IEs —
  Supported Rates `01 08`, Ext Supported Rates `32 0e`.)

**Why:** the morse Linux driver **converts every received S1G frame to 11n (legacy) before handing it to
mac80211/hostap** — `morse_dot11ah_s1g_to_11n_rx_packet` (`morse_driver/mac.c:6628-6629`, *"Perform S1G to
11n conversion prior to passing to mac80211"*; entry `morse_mac_process_s1g_mgmt_or_beacon` :6593), and the
reverse 11n→S1G on TX. So **hostap always computes the AMPE MIC over the 11n representation** (first IE =
legacy Supported Rates `01 08`). **morselib has NO such conversion** — it computes the AES-SIV AAD over the
*raw S1G* frame (`dd 0e`). The 6-byte MIC AAD (`MESH_RSN_FRAME_MIC_OFFSET`) includes those first-IE bytes,
so board0's AAD ≠ chronite's → AES-SIV MIC verify fails. **ESP↔ESP AMPE works** because both ends are
S1G-self-consistent; **only cross-vendor breaks**, on the representation gap — which is exactly why every
prior P2/P3 AMPE proof (ESP↔ESP) passed while ESP↔Linux does not.

**This is also the on-air explanation:** chronite's *logged* CMD_FRAME (what hostap built, pre-driver) is
11n; the *on-air* frame (post-driver) is S1G = what board0 receives. The two representations are the gap.

**Fix (new task #16, substantial):** port the morse `dot11ah` S1G↔11n conversion — at minimum the subset
needed so morselib computes the mesh self-protected-action AMPE MIC over the **11n** representation
(RX: S1G→11n before the AAD/verify; TX: build/MIC in 11n, emit S1G), matching `morse_dot11ah_s1g_to_11n_*`.
Derive from `morse_driver` (`dot11ah` lib) per the follow-Linux directive. Until then: no encrypted
ESP↔Linux ICMP. (board0 holds a diagnostic interop binary in flash; repo reverted clean.)

## #16 — cross-vendor AMPE MIC FIXED: ESP↔Linux encrypted mesh peering ESTAB

The #12 root cause (the AMPE AES-SIV MIC AAD differs S1G-vs-11n) was fixed with a minimal,
spec-driven change (an investigate→design workflow read the morse `dot11ah` driver source and proved the
scope is **aad-prefix-only**). The morse driver, on RX, converts the S1G frame to 11n before mac80211/
hostap and force-inserts a legacy **Supported Rates** IE (EID 1, len 8) as the first IE
(`morse_dot11ah_s1g_to_11n_rx_packet`); so hostap's 6-byte MIC AAD has `body[4:5] = 01 08`, while the ESP's
on-air S1G OPEN has the S1G-caps vendor IE there (`body[4:5] = dd 0e`). Fix: **canonicalise the OPEN AAD's
`body[4:5]` to `01 08`** — a 2-byte substitution applied symmetrically on TX (`umac_mesh_build_peering`
AMPE protect, gated by `is_open`) and RX (`mesh_process_ampe`, gated by `action==OPEN`), plus a local
`#define WLAN_EID_SUPP_RATES (1)`. CONFIRM's 6-byte window is all fixed fields (cat/action/cap/AID) so its
`body[4:5]` is the AID — **not** substituted (the gate is essential); CLOSE carries no MIC. The AMPE
ciphertext/SIV are keyed only by the AEK and were already byte-identical to a Linux peer, so only the AAD
the tag binds to changes. No full frame conversion — the ciphertext is representation-independent.

**Verified on the LIVE Linux node (chronite, 2026-06-28).** After flashing board0 with the fix and reviving
chronite (the repeated rapid restarts had wedged its `wlan1` morse driver — `Failed to initialize driver
interface`; fixed by `modprobe -r morse; modprobe morse` + interface reset → `MESH-GROUP-STARTED`),
board0 ↔ chronite now complete the **full encrypted peering**:
- board0 sends a **Confirm (PLINK action 2)** — it never did before #16 (it could not verify chronite's
  Open AMPE, so it never advanced past Open).
- chronite logs **`mesh: Decrypted AMPE element`** (×2) — the AES-SIV MIC **verifies both directions**.
- **`wlan1: mesh plink with e2:72:a1:f8:ef:a4 established`** → **`MPM … into ESTAB`**; `iw station dump`:
  **`mesh plink: ESTAB, authorized: yes`** — a stable single peering (counts 1–2, no re-SAE loop).

So the **cross-vendor encrypted 802.11s mesh peering (SAE → AMPE → ESTAB) between an ESP and a live Linux
HaLow node works.** This was the load-bearing P3d blocker.

**Still open — the encrypted DATA path (ICMP ping), a separate layer (new task #17).** board0's ping to
chronite (`10.9.9.2`) still times out though the plink is ESTAB/authorized: board0 has no static ARP, so it
relies on broadcast ARP (group MGTK) to resolve chronite, then unicast (pairwise MTK). The cross-vendor
CCMP **data** path (group + pairwise) is untested and is the next blocker for an end-to-end encrypted
ESP↔Linux ping — distinct from the AMPE peering MIC fixed here. (ESP↔ESP data already works — P3c 33/33.)

**On-air note:** verified via the live Linux node's own state machine + crypto (it decrypted the ESP's
AMPE element and authorized the plink) — strong cross-vendor evidence; the chronium `morse0` byte-capture
remains pending (board0 RF range), same standing gap.

## #17 — encrypted ESP↔Linux DATA path (ICMP ping): characterized, NOT yet fixed

With the peering layer solid (#16: ESTAB/authorized), the encrypted **data** path was probed. Findings:
- **Both directions fail at L2→L3.** board0's ping to chronite times out; chronite→board0 ping = 100% loss.
  chronite's `iw station` for board0 shows a healthy plink (`ESTAB, authorized: yes`, rx_packets growing
  into the thousands, `rx drop misc` ~11) — frames arrive at chronite's mac80211 but **tcpdump on `wlan1`
  shows zero ARP/ICMP**, i.e. no decrypted data reaches the IP stack either way.
- **Not a broadcast-ARP-only problem.** Added a static ARP on board0 for chronite (`10.9.9.2 →
  3c:22:7f:37:51:38`) so the ICMP goes out **unicast** (pairwise MTK), bypassing broadcast-ARP (group
  MGTK) resolution, and pinned board0 in chronite's neigh. Still 0 replies → the **unicast MTK data path
  itself fails**, not just broadcast.
- **The MTK derivation formula MATCHES hostap byte-for-byte.** board0 `mesh_derive_mtk` vs chronite
  `mesh_rsn_derive_mtk`: identical context `min/max(nonce)‖min/max(LID, LE16 numeric)‖AKM(00 0f ac 08)‖
  min/max(MAC)`, key=PMK(32), label "Temporal Key Derivation", out 16. Nonces+LIDs are symmetric, so the
  MTK *should* agree — derivation is **not** the smoking gun.

So the encrypted CCMP **data** path is broken cross-vendor while the encrypted **peering** (AMPE) works —
even though ESP↔ESP direct encrypted ping works (P3c 33/33). Remaining suspects, for a focused next effort:
(a) runtime key **disagreement** despite the matching formula (nonce/LID actually exchanged wrong → MTK
differs at runtime) — verify by dumping board0's runtime MTK/MGTK and comparing to chronite's `-K` log;
(b) board0's MGTK **install** on chronite (broadcast key) — confirm chronite installed board0's MGTK;
(c) the mesh-data **CCMP/S1G↔11n** format (the 4-address mesh data header / CCMP AAD differing across the
driver's S1G↔11n data-frame conversion) — capture board0's actual data frame and chronite's RX-drop reason;
(d) board0's data-path firmware key install (idx/format). Needs board0 runtime instrumentation (dump MTK +
the TX data frame) + a chronite RX-drop trace. **No encrypted ESP↔Linux ICMP yet** — but the encrypted
PEERING (the load-bearing P3d milestone) is done.

**Bench note:** rapid `wpa_supplicant_s1g` restarts wedge chronite's `wlan1` (`Failed to initialize driver
interface`) → recover with `modprobe -r morse; modprobe morse` + `ip link set wlan1 down/up`, restart the
supplicant (`MESH-GROUP-STARTED`); only ever touch wlan1, never wlan0/SSH.

### #17 progress — board0 runtime keys captured; matched comparison bench-blocked

Instrumented board0 to dump its runtime data keys at ESTAB (periodic `KEYDBG` printf from the plink tick,
reverted). Captured (board0's peering with chronite):
- **board0 MTK** (pairwise, derived) = `d36094d5af8ca39561d889b338caeed9`
- **board0 own MGTK** (group, board0's own — chronite installs it as RX) = `998642cf946f318f1222c19064eeb5c7`
- nonces (prefix) myn=`dcff4d81…` pen=`067c1396…`, llid=`31d7` plid=`0ff4`.

chronite logs its installed keys with `-K` (`mesh: MTK - hexdump`, `mesh: RX MGTK - hexdump`), so the
**decisive next test is a direct compare**: board0 MTK == chronite MTK? (derivation/inputs agree) and board0
own-MGTK == chronite's RX-MGTK-for-board0? (the exchange/install works). If equal → the data failure is the
mesh-data CCMP/frame format (deep, HW/firmware); if not → a runtime key/derivation-input mismatch (fixable).

**Blocked on the bench:** getting board0's and chronite's keys for the SAME peering needs a clean
co-peering, but (a) board0's RTS reset doesn't actually reset the chip (USB drops, app keeps running → same
keys), so a reliable reset = reflash; and (b) chronite's HaLow wedged again — after repeated supplicant
restarts the mesh won't come up (`Failed to set interface to mode 2: -16 Device or resource busy`,
`Failed to initialize driver interface`), and a `modprobe -r morse; modprobe morse` + iface reset did not
clear it this time — **chronite likely needs a reboot** to recover the morse chip. Resume #17 with: reboot
chronite → clean mesh → reflash board0 (periodic KEYDBG) → capture board0 MTK/MGTK + chronite's logged
MTK/MGTK for the SAME peering (match by the nonces) → compare. Repo reverted clean (diagnostic only).

### #17 update — keys AGREE cross-vendor → the blocker is the mesh-DATA CCMP frame format (not keys)

Recovered chronite (see bench note) and instrumented board0 (periodic `KEYDBG`, reverted) to dump its
runtime data keys, then compared against chronite's `-K`-logged keys for the **same** peering:

| key | board0 (ESP) | chronite (Linux) | |
| --- | --- | --- | --- |
| MTK (pairwise, derived) | `3da2341263436e3b740636831818599f` | `3d a2 34 12 63 43 6e 3b 74 06 36 83 18 18 59 9f` | **identical** |
| MGTK (group, board0's own / chronite RX) | `0d2fe61fbf6efe9cea5a98597377154c` | `0d 2f e6 1f bf 6e fe 9c ea 5a 98 59 73 77 15 4c` | **identical** |

So the derived **pairwise MTK** AND the exchanged **group MGTK** agree byte-for-byte cross-vendor — the SAE
PMK, the MTK derivation, and the MGTK exchange/install are all correct. Yet with the keys matching, the
encrypted data STILL fails both ways (board0 ping 0/26; chronite tcpdump sees no decrypted ARP/ICMP). **The
blocker is therefore the mesh-DATA CCMP frame format, not the keys** — almost certainly the same class of
S1G↔11n representation gap that broke the AMPE MIC (#16), but now for a DATA frame's CCMP AAD (the 4-address
mesh MAC header / mesh-control / QoS that CCMP authenticates). Next: determine whether the morse Linux side
does mesh-data CCMP in HW (over the S1G frame) or in mac80211 SW (over the converted 11n frame), and what
header bytes its CCMP AAD covers, vs the ESP/MM6108 — then align. (ESP↔ESP data works because both are
S1G-consistent, same as the #16 story.)

**Bench — chronite RECOVERED (root cause was a bad config I reconstructed from memory):** the mesh wouldn't
come up because my recreated `/tmp/wpa-interop.conf` was wrong — it used `frequency=5560` and omitted
`country=US`, so the nl80211 mesh-join wedged the morse chip trying to tune an un-enabled S1G channel (each
failed join hard-wedged the chip → `rmmod` blocked → only a reboot cleared it, looking like a hardware
fault). The CORRECT config (per `docs/reference/captures/wpa-smesh.conf`) uses GLOBAL `country=US` +
`op_class=68`/`channel=27`/`s1g_prim_chwidth=0`/`s1g_prim_1mhz_chan_index=0` + `dtim_period=1` (mesh join
bails without it), NOT `frequency=`. With it: `MESH-GROUP-STARTED`, stable, board0 re-peers to ESTAB. The
working config is now persisted at `chronite:~/wpa-interop.conf` (survives reboots; `/tmp` does not).

## #17 — RESOLVED: encrypted ESP↔Linux ICMP works (broadcast HWMP must be MGTK-encrypted)

The data-path blocker was NOT the CCMP-AAD format and NOT a key mismatch (keys agree, the open mesh
worked). An investigate-workflow (read morse_driver + mac80211 + the ESP datapath) found the real cause:
**morselib has no group-addressed robust-management TX path.** `umac_datapath_tx_mgmt_frame` DROPS any
BC/MC robust-mgmt frame when PMF is required — `"Unsupported attempt to TX a BC/MC RMF, frame dropped"`
(`umac_datapath.c:2230-2237`; that branch is the infrastructure BIP case, unsupported here) — and the
broadcast HWMP PREQ/PERR from `umac_mesh_tx_hwmp` (`umac_mesh.c:1873`) routed straight through it. So in a
**secured** mesh board0 silently emitted **no PREQ**: it could not originate a mesh path (ping 0/26) and
chronite's HWMP mpath to board0 stayed `RESOLVING` (next_hop=0, frames queued). The **open** mesh worked
only because PMF is off and the drop was skipped — exactly the open-works/secured-fails asymmetry.

A secured 802.11s mesh protects a **group-addressed** HWMP frame with the **MGTK (CCMP)**, not BIP. **Fix:**
in `umac_mesh_tx_hwmp`, route a group/broadcast-DA HWMP frame (PREQ/PERR) through the existing mesh
group-key path `umac_datapath_tx_mesh_group_frame` (encrypt under the common-stad MGTK + set
Protected/HW_ENC, leaving the ACTION subtype intact — same as a forwarded group DATA frame); keep the
unicast PREP on the pairwise mgmt path. 16-line change, no FW changes (the chip already does the cipher +
S1G conversion).

**VERIFIED on-air vs the live Linux peer (chronite, 2026-06-28):**
- board0 ping to chronite: **0/26 → 5/5 replies (67–298 ms)**.
- chronite's mpath to board0: `RESOLVING, next_hop=00:00:00:00:00:00` → **`ACTIVE/RESOLVED, flags 0x15,
  next_hop=e2:72:a1:f8:ef:a4`**; chronite TX-to-board0 jumped ~11 → 97 packets.

**This is the full cross-vendor encrypted 802.11s mesh — the P3d goal:** SAE → AMPE → ESTAB (#16) → HWMP
path resolution (encrypted PREQ, this fix) → CCMP data → **encrypted ICMP ESP↔Linux**.

**Verified with the interop test config's static ARP** (so board0 ORIGINATES the PREQ, which sets chronite's
reverse path too). **Follow-ups (no-static-ARP dynamic operation):** (1) RX — board0 must answer Linux's
encrypted broadcast PREQ with a PREP (group robust-mgmt RX replay space + CCMP-under-MGTK decrypt,
`umac_datapath.c:1129-1167`/`:371-385`); (2) encrypt the unicast mesh relay path (`umac_mesh_forward_data`).
Bench note: chronite rebooted once during testing (bench instability / morse-chip wedge, not the fix — the
fix's encrypted PREQ verified working after); avoid hammering chronite's sshd with rapid SSH (MaxStartups).

### #18 progress — no-static-ARP dynamic operation is multi-faceted (diagnosed, not fixed)

The #17 encrypted ping works WITH a static ARP (board0 originates the PREQ). For no-static-ARP dynamic
operation board0 must both (a) get its broadcast ARP to the Linux peer and (b) answer the Linux peer's
encrypted PREQ with a PREP. Attempted the RX-replay-space fix and tested without the static ARP — still
0 replies (36 `send error` = ARP never resolves). Diagnosis (chronite side):
- **board0's broadcast ARP never reaches chronite's IP** (tcpdump on wlan1: no ARP) — board0's *originated
  group-data broadcast* (under board0's MGTK) isn't decrypted/delivered by chronite. A separate group-DATA
  cross-vendor issue (or board0 isn't emitting the broadcast at all), distinct from the HWMP-mgmt path.
- **chronite's mpath to board0 stays `RESOLVING`** (next_hop=0) — board0 still doesn't PREP chronite's
  encrypted broadcast PREQ.

**RX-replay-space fix attempted (reverted, unverified, insufficient alone):** `umac_datapath.c:1156`
`process_mgmt_frame_ccmp_header` validates a protected mgmt frame's PN against
`UMAC_KEY_RX_COUNTER_SPACE_IND_ROBUST_MGMT` (the pairwise key's mgmt counter) — wrong for a group-addressed
PREQ, whose key_id selects the MGTK and whose PN is the group key's sequence. Tried switching to
`UMAC_KEY_RX_COUNTER_SPACE_DEFAULT` when `mm_mac_addr_is_multicast(dot11_get_da(header))`. It's a plausible
correctness fix but did NOT make the no-static-ARP ping work, so the blocker is elsewhere too. **Open
question that gates it:** does the MM6108 firmware HW-DECRYPT a group-addressed MANAGEMENT frame with the
MGTK at all (set `MMDRV_RX_FLAG_DECRYPTED`)? If not, the frame is dropped at `umac_datapath.c:1143-1148`
before any replay check, and no morselib change helps. **Next session:** add a one-shot diagnostic in
`process_mgmt_frame_ccmp_header` (for a multicast-DA protected mgmt frame: log the DECRYPTED flag + the
replay result) AND check board0's group-DATA broadcast TX path — determine, empirically, which of {firmware
no-decrypt, replay space, PREP generation, group-data-broadcast} is the blocker for each of the two halves.
The static-ARP interop config remains the working path for the #17 milestone.

## #18 — RESOLVED: no-static-ARP dynamic ESP↔Linux join (group-privacy-action RX exemption)

**Result (2026-06-28, board0 ↔ chronite, on-air + functional):** board0 joins the live secured Linux
mesh and pings chronite **with no static ARP** — fully dynamic ARP + HWMP path resolution, bidirectional,
CCMP-encrypted. board0→chronite **46/0 then 39/0 then 11/0** replies (was **0 replies / all timeouts**
before the fix); chronite→board0 **5/5, 0% loss** (was 0/8); chronite's mpath to board0 =
`FLAGS 0x15` (ACTIVE+RESOLVED), `NEXT_HOP=e2:72:a1:f8:ef:a4` direct, `HOP_COUNT=1`. Verified on the
**clean fix build** (no debug probes, `MESH_LINUX_INTEROP` only as test scaffolding).

**The prior "#18 progress" hypothesis above was WRONG and is superseded.** It assumed board0 had to
*decrypt an MGTK-encrypted broadcast PREQ* and that the gate was "does the MM6108 HW-decrypt group
MANAGEMENT frames." On-air capture overturned both premises.

### What it actually is — on-air gold standard (chronium morse0 monitor, tshark)
Triggered chronite to originate a PREQ for board0 (`ip neigh replace 10.9.9.136 lladdr <board0> … ;
ping 10.9.9.136`) while capturing on chronium. Frame-type histogram of chronite's TX (`wlan.fc.type_subtype`):
- `0x0031` ×825 — S1G Beacons.
- `0x0028` ×78 — **QoS Data, `protected=True`, broadcast** → chronite's group DATA *is* MGTK-encrypted on-air.
- `0x000d` ×25 — **Action, `protected=False`, broadcast** → chronite's broadcast HWMP Action frames are
  **NOT encrypted**.

`tshark -V` on a `0x000d` frame = **Category MESH(13) / HWMP Mesh Path Selection / Path Request**,
Originator `3c:22:7f:37:51:38` (chronite), **Target `e2:72:a1:f8:ef:a4` (board0)**, TO=1, `protected=False`.
So board0's PREQ arrives **in the clear**, not encrypted. A throwaway board0 RX probe at the umac RX entry
(`umac_datapath_process_rx_frame`) independently logged the same: `t=0 st=13 prot=0 DEC=0 stad=1 da=ff:…ff`
(mgmt / Action / unprotected / broadcast, from chronite's peer record), ×26.

### Root cause — morselib drops the unprotected group PREQ before HWMP sees it
board0 receives the PREQ at the umac RX entry but a throwaway probe in `umac_mesh_handle_hwmp` showed it
was **never reached** (no PREQ target-match, no PREP). The drop is in
`umac_datapath_process_rx_mgmt_frame` (`umac/datapath/umac_datapath.c:356-385`), the generic pre-dispatch
PMF check:
- `frame_is_robust_mgmt()` → `frame_is_robust_action()` (`umac/frames/action.c`) classifies category
  **MESH(13)** as **robust** (it excludes SELF_PROTECTED(15) — which is why the unprotected AMPE peering
  frames pass — but not MESH/MULTIHOP). ✓ matches mac80211 (`_ieee80211_is_robust_mgmt_frame` also does
  NOT exclude MESH).
- `umac_sta_data_pmf_is_required(stad)` is **true** for the secured-mesh peer, and the frame is unprotected
  and multicast with **no MMIE** (a PREQ carries no BIP Management MIC IE) → falls to the `else` branch →
  `umac_datapath_process_unprotected_robust_mgmt_frame()` + `return`. The frame never reaches
  `data->ops->process_rx_mgmt_frame` → `…_mesh` → `umac_mesh_handle_action` → `umac_mesh_handle_hwmp`, so
  board0 never PREPs, so chronite can never resolve a path to board0 — and therefore can't even ARP-reply
  to board0 (chronite needs board0's path to unicast the reply), which is why board0's *own* ping to
  chronite also failed (ARP never completes). One drop blocks both directions.

This is the **RX mirror of the #17 TX bug**: morse's infra PMF logic mishandles legitimately-unprotected
group-addressed mesh HWMP frames (#17 was the TX side dropping broadcast robust mgmt; #18 is the RX side).

### Why Linux accepts it — net/mac80211 (chronite ~/halow/rpi-linux), line by line
`ieee80211_rx_h_mgmt_check` (`net/mac80211/rx.c:3400`) → `ieee80211_drop_unencrypted_mgmt` (`rx.c:2436`).
For our frame the only branch that could drop it is the multicast-robust-mgmt/BIP check at `rx.c:2473`
(`ieee80211_is_multicast_robust_mgmt_frame && get_mmie_keyidx < 0`) — but that whole block is inside
`if (rx->sta && test_sta_flag(rx->sta, WLAN_STA_MFP))` (`rx.c:2454`). **mac80211 mesh peers are MFP=no**
(confirmed at runtime: `iw dev wlan1 station dump` on chronite shows the board0 peer with **`MFP: no`**;
mesh STAs are added via `cfg.c` without `NL80211_STA_FLAG_MFP`), so the block is **skipped** and the frame
falls through to `RX_CONTINUE` → the mesh HWMP handler. mac80211 also formalises this class as
`_ieee80211_is_group_privacy_action` (`include/linux/ieee80211.h:4611`): a group-addressed **MESH(13) /
MULTIHOP(14)** action frame is MGTK/group-privacy class, **not** BIP — so the no-MMIE drop must not apply.

### The fix (morselib, 4 files — mirrors mac80211)
1. `dot11/dot11.h` — add `DOT11_ACTION_CATEGORY_MESH = 13`, `DOT11_ACTION_CATEGORY_MULTIHOP = 14` to
   `enum dot11_action_category` (= mac80211 `WLAN_CATEGORY_MESH_ACTION` / `…_MULTIHOP_ACTION`).
2. `umac/frames/action.c` — new `frame_is_group_privacy_action(view)`, a line-for-line port of
   `_ieee80211_is_group_privacy_action`: a mgmt Action frame with the group bit set on the DA (addr1) and
   category MESH or MULTIHOP.
3. `umac/frames/frames_common.h` — declare it (next to `frame_is_robust_mgmt`).
4. `umac/datapath/umac_datapath.c:356-385` — add `&& !frame_is_group_privacy_action(rxbufview)` to the
   unprotected-robust-mgmt drop guard, so a group-addressed mesh/multihop action frame skips the BIP/MMIE
   drop and proceeds to the mesh handler — exactly mac80211's MFP=no behaviour for these frames.

`pmf_is_required` is left **true** (so board0's *unicast* PREP to chronite stays CCMP-encrypted under the
pairwise MTK, matching what chronite does for its PREP, proven in #17). The broader morselib/mac80211
mismatch — morselib gates robust-mgmt TX/RX on `pmf_is_required`, mac80211 on the per-STA MFP flag + key
presence (mesh peers MFP=no but still key-encrypt) — is recorded against task #9.

### Verification chain (all four links)
1. **on-air (chronium):** chronite TX's an *unprotected* group PREQ targeting board0 (frame 45, tshark `-V`).
2. **board0 RX:** receives it unprotected at the umac entry (`#18ENTRY t=0 st=13 prot=0`).
3. **board0 HWMP:** with the fix, reaches the PREQ target-match — captured
   `#18PREQ sa=3c:22:7f:37:51:38 tgt=e2:72:a1:f8:ef:a4 me=e2:72:a1:f8:ef:a4` (target == self).
4. **functional:** bidirectional dynamic ping (board0→chronite 46+39+11/0; chronite→board0 5/5; chronite
   mpath `0x15` direct). Before the fix every one of these was 0 / `RESOLVING`.

## #19 — relay-forward keyed under the next-hop pairwise MTK + broadcast HWMP sent unprotected

Two related fixes for the ESP-as-intermediate-hop (multi-hop relay) path. Both verified on-bench (3-ESP
relay-demo + board0↔chronite regression). **The end-to-end ESP↔ESP relay ping does NOT yet complete — a
separate downstream blocker remains (board2 doesn't decrypt the forwarded unicast); see "Remaining" below.**

### Fix 1 — #19: encrypt the unicast relay-forward (umac_mesh_forward_data)
`umac_mesh_forward_data` (ESP relays a unicast frame whose mesh-DA isn't us) sent the pre-built 4-address
data frame via `umac_datapath_tx_mgmt_frame(common_stad, …)`. For a *data* frame `frame_is_robust_mgmt` is
false, so it went out **plaintext**, and keyed off the common stad (no per-next-hop pairwise key). Unicast
analog of the P2c group-forward-plaintext bug. net/mac80211 re-keys a forwarded unicast with the next-hop
PTK (`ieee80211_tx_h_select_key` tx.c:614: `tx->sta->ptk` for a unicast RA). Fix: a pairwise sibling of the
#17 group helper.
- `umac_datapath.c`: refactor `umac_datapath_tx_mesh_group_frame` into a shared
  `umac_datapath_tx_mesh_keyed_frame(stad, txbuf, key_type)` core + two thin wrappers — `_group_frame`
  (UMAC_KEY_TYPE_GROUP, unchanged behaviour) and new `_unicast_frame` (UMAC_KEY_TYPE_PAIRWISE).
- `umac_mesh_forward_data`: look up the next-hop's per-peer stad (`umac_mesh_get_peer_stad(next_hop)`) and
  send via `umac_datapath_tx_mesh_unicast_frame(nh_stad, …)` — encrypts under the next hop's MTK (key idx 0).
- **Verified exercised + correctly keyed** (static-ARP relay, time-throttled probes): board0 forwards
  board1→board2 data with `nh_stad` found and `key_type=PAIRWISE key_id=0 sec=2` — encrypted under board2's
  pairwise MTK, ~20×/window. The #19 keying does exactly its job.

### Fix 2 — broadcast HWMP sent UNPROTECTED (supersedes the #17 MGTK-encryption of broadcast HWMP)
#17 routed broadcast HWMP (PREQ/PERR/RANN) through the MGTK group path because
`umac_datapath_tx_mgmt_frame` DROPS a BC/MC robust-mgmt frame under PMF (umac_datapath.c:2238). That worked
ESP↔Linux only because chronite *decrypts* the ESP's MGTK-encrypted PREQ — but in an ESP↔ESP relay the
relay node couldn't process a peer's encrypted group-management frame, so multi-hop never resolved. #18
established the real rule: **Linux sends group HWMP UNPROTECTED** (mesh peers are MFP=no). So the faithful
fix is the TX mirror of the #18 RX exemption:
- `umac_datapath_tx_mgmt_frame`: add `&& !frame_is_group_privacy_action(txbufview)` to the robust-mgmt
  guard, so a group-addressed Mesh/Multihop action frame skips the BC/MC drop and the encryption and goes
  out in the clear (a unicast PREP is NOT a group-privacy action → stays pairwise-CCMP-encrypted).
- `umac_mesh_tx_hwmp`: always use `umac_datapath_tx_mgmt_frame` (the group-path special-case is removed);
  broadcast HWMP now emits unprotected, unicast PREP stays pairwise.
- **Verified:** ESP↔ESP, the full PREQ chain now resolves — board1 originates an unprotected PREQ →
  **board0 receives it** (`#HWRXPREQ`) → board0 forwards it → **board2 receives it** → board2 **PREPs**
  (`#HWTX act=1`). And **no #17/#18 regression**: board0↔chronite dynamic no-static-ARP encrypted ping
  still works (board0→chronite 39/3, chronite→board0 5/5, chronite mpath `0x15`) — chronite accepts the
  now-unprotected PREQ (MFP=no), exactly as predicted.

### Remaining (the next task — NOT #19's keying): board2 doesn't decrypt the forwarded unicast
With static ARPs forcing the path, the full chain is exercised: HWMP resolves, and board0 forwards board1's
ICMP to board2 under board2's pairwise MTK (correctly keyed, confirmed above). But the relay ping is still
0 replies because **board2 never receives the forwarded encrypted unicast at the umac layer** — a board2-RX
probe (`#MRXD`) fired 0× despite board0 sending ~20 correctly-keyed encrypted forwards (board0's own
`#MRXD` confirms it RX'd board1's ICMP and forwarded it). board2 *does* receive board0's unprotected
broadcast PREQ, so it's specifically the **encrypted 4-address forwarded frame** the firmware drops before
delivery — likely the last-hop addressing (A1==A3==board2, mesh-SA=board1≠TA=board0) confusing the firmware
key-lookup/decrypt. This is firmware-RX territory (#9-adjacent), downstream of #19's correct forward-keying.

### Bench note
chronite's secured mesh was stopped (wlan1 down) to clear the channel for the ESP relay test, then brought
back up for the regression check. Scoped stop/restart only ever touches the **wlan1 s1g** supplicant
(`pkill -x wpa_supplicant_` — the truncated comm; never the wlan0 `wpa_supplicant` carrying SSH).

### #19 + rework — ON-AIR VERIFIED (chronium morse0 monitor, 2026-06-28)
Gold-standard off-air capture (chronium in monitor on ch27, `tcpdump -i morse0`, 1123 frames, the 3-ESP
relay running with static ARPs). **All three ESPs are in chronium's range now** (board0 325 / board1 316 /
board2 308 frames — the earlier "board0 out of range" no longer holds). tshark decode:
- **board1 PREQ**: `0x000d` (Action) **protected=False** broadcast — the broadcast-HWMP-unprotected rework,
  confirmed on-air. board0 re-broadcasts it the same way (`0x000d protected=False` ×7).
- **board0 PREP → board1**: `0x000d` **protected=True** unicast ×15 — unicast PREP stays pairwise-encrypted.
- **#19 forward — board0 → board2 QoS Data** (`0x0028`) **protected=True** ×23. Verbose decode of one frame:
  `RA=board2  TA=board0  A3(DA)=board2  A4(SA)=board1` (origin preserved), **CCMP** with sequential Ext IVs
  (…B1, B2, … C8). So the relayed unicast is on-air a 4-address CCMP QoS-Data frame keyed for the next hop —
  exactly what #19 builds + `umac_datapath_tx_mesh_unicast_frame` encrypts. The mac80211 mesh-forward shape
  (4-addr, A4=origin, GTK-not, next-hop-PTK) matches.

This also **pins #20**: board0's forwards are on-air-correct (Protected, valid incrementing PNs), so the
relay-ping failure is definitively **board2's RX-decrypt** of the forwarded 4-addr unicast, not board0's TX.

## #20 — multi-hop relay last hop: MM6108 HW-crypto can't decrypt a forwarded frame (firmware limitation)

After #19 (forward correctly keyed + on-air CCMP-verified) the relay ping still fails at the last hop:
board2 doesn't decrypt board0's forwarded unicast. Investigated per the "confirm firmware behavior" plan.

**1. Direct vs forward (isolates the difference to A4).** board0→board2 **DIRECT** ping = **38/0** — board2
decrypts board0's direct unicast fine (key/path/HW-decrypt all OK). The **forwarded** unicast is dropped.
The only frame difference is **A4 (mesh-SA)**: direct `A4=board0=TA`; forward preserves `A4=board1≠TA`.

**2. Firmware DROPS the forwarded frame (board2 umac never sees it).** An RX-entry probe on board2
(`umac_datapath_process_rx_frame`, filtered to TA=board0 + 4-addr) fired **0×** while chronium's `morse0`
monitor simultaneously showed board0 transmitting **16 Protected QoS-Data forwards** to board2. So the
firmware discards the forwarded frame before delivering it to the host → a morselib software-decrypt isn't
viable as-is (the host never gets the frame).

**3. Same firmware + HW crypto on Linux → same limitation.** chronite (the live Linux mesh) runs
`no_hwcrypt=N` + `thin_lmac=N` → HW crypto (firmware CCMP), identical to the ESP. The morse driver
(`mac.c:5845`) only reads the firmware's `MORSE_RX_STATUS_FLAGS_DECRYPTED` per-frame — the *firmware* decides
deliver-vs-drop. So Linux multi-hop forwarding would hit the same wall; it has just never surfaced because
the bench mesh is all single-hop (every node in RF range → direct peering). morselib has **no** sw-crypto mode.

**Conclusion:** the MM6108 firmware in HW-crypto mode keys CCMP decryption by the **mesh-SA (A4)** and drops a
forwarded 4-address frame whose origin (A4) isn't a peer of the receiver. Multi-hop mesh **forwarding**
fundamentally requires **SW crypto** (Linux `no_hwcrypt`/`thin_lmac` → `set_key=NULL` → host CCMP keyed by the
link/TA), which morselib doesn't implement. This is a firmware/architecture boundary, not a #19 keying bug
(#19's forward is on-air-correct). It also bounds task #9 (multi-hop reachability).

**Fix options (a design decision):** (1) **SW crypto on the ESP** — architecturally correct, large (host-side
CCMP + a firmware mode that delivers un-decrypted frames, which the morselib build may not expose). (2)
**A4-rewrite at the relay** (set A4=relay so the firmware decrypts by A4=TA) — works around the SA-keying but
diverges from 802.11s and loses the true origin for 3+ hops. (3) **Accept single-hop-only** for HW-crypto
deployments. **Pending definitive proof:** a *forced* Linux multi-hop relay (direct peering blocked) to show
Linux also fails — impractical on the all-in-range bench without a peer-block mechanism.

### #20 — SW-crypto feasibility verdict: FEASIBLE, substantial effort, low firmware risk

Per the chosen path (SW crypto = the architecturally-correct way around the HW-crypto forwarding wall).

**1. Software CCMP IS available in the tree.** `src/hostap/wlantest/ccmp.c` has `ccmp_decrypt`/`ccmp_encrypt`
(via `aes_ccm_ad`/`aes_ccm_ae`), keyed by a caller-supplied TK — so I can decrypt a forwarded frame keyed by
the link/TA. Not currently in the CMake, but addable exactly like `aes-siv.c`/`aes-ctr.c` were for AMPE. The
ESP32-S3 + mbedTLS have AES HW acceleration, so SW CCMP is cheap.

**2. Mechanism = `no_hwcrypt`, NOT thin-LMAC.** thin-LMAC needs a *different firmware binary*
(`mm6108-tlm.bin`, the `-tlm` variant — `mm6108.c:mm610x_get_fw_path`), which the ESP build doesn't ship.
`no_hwcrypt` runs on the *same* `mm6108.bin`: don't offload keys → `set_key=NULL` on Linux → the firmware
delivers all frames raw → the host does CCMP keyed by the link/TA. The morselib equivalent: skip
`umac_keys_install_key`'s firmware offload (the single seam is `umac_mesh_peer_secure_estab`, umac_mesh.c:1430)
and do host CCMP.

**3. Hybrid (HW direct + SW forward) is NOT possible.** The firmware drops an un-decryptable *protected*
frame when it HAS keys (the #20 observation: board2 had the board0↔board2 key, still dropped the A4=board1
forward). So you can't selectively SW-decrypt only forwards — it's all-or-nothing per node, and every node
that must receive a forwarded frame moves to SW crypto → the whole secured mesh goes SW-crypto.

**4. Effort — substantial (port mac80211's SW CCMP into morselib's mesh data path):**
- TX: build CCMP header + PN, `aes_ccm_ae` the payload, send plaintext-to-firmware (drop HW_ENC).
- RX: detect Protected, look up the link key by TA, `aes_ccm_ad`, PN/replay check, strip CCMP header.
- Key mgmt: keep MTK/MGTK in host memory (un-offloaded), per-key TX PN + RX replay window.
- Seams: the mesh TX helpers (`umac_datapath_tx_mesh_*` / `_tx_mgmt_frame`) + the data RX path
  (`umac_datapath_process_rx_data_frame`). The group MGTK + pairwise MTK already live in morselib.

**5. Low-risk make-or-break to confirm FIRST:** does the ESP `mm6108.bin` deliver a *protected* frame raw
when NO key is installed (like Linux `no_hwcrypt=Y`)? Same firmware + Linux works ⇒ very likely yes. Confirm
with a quick no-key node test (board2 skips `umac_keys_install_key`; board0 HW-encrypts a direct frame to it;
probe board2's RX entry for the frame arriving with `prot=1 DEC=0`) before committing to the full port.

**Verdict:** feasible; the only hard blocker would be (5) failing, which is unlikely. It is a real
multi-session implementation (host-side CCMP), not a quick fix — the call is whether multi-hop forwarding is
worth that vs the single-hop-on-HW-crypto status quo (#19 + broadcast-HWMP already give a working 1-hop
secured mesh ESP↔Linux).
