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
