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
scaffolding in `mmwlan_mesh_peer_open` (replaced by the P3b FSM).
