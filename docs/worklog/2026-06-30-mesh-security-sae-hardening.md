# Mesh security — SAE hardening (GAP-C / #14 / #15)

Phase-2 of the secured 802.11s mesh: three hostap-parity hardening fixes on the SAE state machine, derived
line-by-line from `hostap/src/ap/ieee802_11.c` + `src/common/sae.c` + `wpa_supplicant/mesh_mpm.c`. Follows the
#13 SAE re-sync deadlock fix; every edit is **txn-disjoint** from the #13 genuine-restart recovery (the txn==1
Commit teardown + beacon re-discovery), which is left byte-for-byte unchanged. Code-map: `rimba-mesh-security-codemap.md`
§"SAE hardening — GAP-C / #14 / #15".

## The three gaps (recon + adversarial verify, all `gapIsReal=true`, high confidence)

- **GAP-C — crafted/malformed-Commit reauth teardown (DoS).** The #13 ACCEPTED-state reauth gate
  (`umac_mesh.c:1021-1031`) freed a live secure link on a Commit that passed only structural checks
  (length + group-id + non-reflection) **without** running SAE crypto validation. A forged Commit (spoofed peer
  MAC, `status==0`, right length/group, non-reflected, but a scalar outside [1,r-1] or an element off the P-256
  curve) triggered `mesh_sae_reauth_free` → link flap — a remotely-triggerable, repeatable teardown by an
  unauthenticated attacker. hostap rejects exactly that frame and keeps the link (`sae_parse_commit` runs at
  `ieee802_11.c:1502` *before* `sae_sm_step`; a parse failure → `:1538` reply, never reaching the `:1140`
  teardown).
- **#14 — ACCEPTED+Confirm has no anti-replay.** The txn==2 `MESH_SAE_ACCEPTED` branch (`:1093-1100`) resent the
  cached Confirm **unconditionally** for every inbound Confirm; `struct mesh_peer` had no `rc` field. A captured
  Confirm replayed in a flood amplifies into reflected Confirms / a big-sync lockout. hostap silently ignores a
  Confirm whose send-confirm counter ≤ `rc` or `== 0xffff` (`ieee802_11.c:1596-1605`) and rate-limits the resend
  behind big_sync (`:1160-1167`).
- **#15 — unsolicited MPM Open starts a full SAE.** The inbound-peering path (`umac_mesh.c:2434-2442`)
  unconditionally allocated a peer + ran a full Dragonfly SAE from any non-CLOSE peering frame from an unknown
  (allowlisted) MAC. hostap (`mesh_mpm.c:1262-1266`) adds a peer from an Open in a secure AMPE mesh **only** with
  a cached PMKSA.

## The fixes (7 edits, +83/−5 across 2 files)

**GAP-C**
1. `mesh_sae.c` new `mesh_sae_validate_commit(body, len)` — validates on a **throwaway** `sae_data`
   (`os_zalloc` → `sae_set_group(19)` → `sae_parse_commit` with the **exact** proven args `{19,0}` / `H2E` /
   `&ie_offset` → `sae_clear_data` + `os_free`), returns 0 iff crypto-valid. Never touches `peer->sae`.
2. `umac_mesh.c` extern decl.
3. `umac_mesh.c:1027` — append `|| mesh_sae_validate_commit(body,len) != 0` as the **last** term of the txn==1
   ACCEPTED reauth gate, so `mesh_sae_reauth_free` fires only post-validation.

**#14**
4. `umac_mesh.c:452` — add `uint16_t sae_rc` (== hostap `sae->rc`, `sae.h:120`).
5. `umac_mesh.c` txn==2 CONFIRMED — record `sae_rc = LE16(body)` after `mesh_sae_check_confirm` succeeds
   (== `ieee802_11.c:1613`).
6. `umac_mesh.c` txn==2 ACCEPTED — gate the resend: `if (psc <= sae_rc || psc == 0xffff) break;` then
   `mesh_sae_check_big_sync` + `sae_sync++` + the cached-Confirm resend.

**#15**
7. `umac_mesh.c:2434` — `#if MMWLAN_MESH_SEC_PHASE1` drop the unsolicited peering frame + await beacon;
   `#else` keeps open-mesh behaviour. Deliberate divergence: ESP keeps no PMKSA cache, so hostap's
   `(!(security & SEC_AMPE) || wpa_auth_pmksa_get(...))` reduces to "don't add" — drop ALL unknown-peer peering
   frames and rely on the beacon path (both points beacon continuously).

## Source verification (against the live `sae.c` on chronite, not memory)

The validate shim's key assumption — that a fresh scratch `sae_data` + `sae_set_group` is sufficient for
`sae_parse_commit` with no PWE — was confirmed in source:
- `sae_parse_commit` calls `sae_group_allowed` (`sae.c:2178`), which calls `sae_set_group` internally
  (allocs `tmp` + the EC group); it derefs `sae->tmp` (`:2210`) so `tmp` must exist — and it does after
  `sae_group_allowed`.
- `sae_parse_commit_scalar` (`:1892`) does the 1<scalar<r check using only `tmp->order`; its reflection branch
  is gated on `state == SAE_ACCEPTED && peer_commit_scalar_accepted` — a fresh scratch (state NOTHING) **skips**
  it, so **no `own_commit_scalar` deref** (which is why the gate keeps its manual non-reflection memcmp).
- `sae_parse_commit_element_ecc` (`:1945`) does coords<p + valid-point + on-curve using only `tmp->prime/ec` —
  **no `pwe` deref**.

## Build

`make build APP=rimba-halow-mesh BOARD=proto1-fgh100m` — `mmhostap` + `morselib` compile and link clean
(binary 0x160630 B, 6% partition free). No warnings on the changed files.

## On-air verification (chronite = live Linux secured-mesh peer; board0/board1 hardened firmware)

> Note: for these edits the on-air *byte-capture* gold standard is not the primary check — GAP-C/#14 change how
> board0 **processes** received SAE frames (not the bytes it transmits) and #15 makes it **drop** a frame, so
> the verification is behavioural (endpoint state + recovery), confirmed against a live Linux peer.

- **No runtime regression.** Both boards boot and run steady-state with zero crashes/asserts.
- **Secured peering works.** With chronite's secured mesh up (`wpa_supplicant_s1g` + `~/wpa-interop.conf`:
  rimba-mesh / SAE / `rimbamesh2026` / dtim_period=1, S1G chan 27), board0 (`e2:72:a1:f8:ef:a4`) and board1
  (`…f9:40`) both reach **plink ESTAB** — full SAE→AMPE→established with the hardened firmware. → GAP-C/#14/#15
  do **not** regress real secured peering.
- **Genuine-restart recovery.** After a clean restart of chronite's mesh, both boards **re-ESTAB**. → the GAP-C
  validate gate does **not** block recovery (the #1 risk). Consistent with the code-level guarantee that the
  shim uses the exact proven `sae_parse_commit` args, so any Commit the normal SAE path accepts also passes the
  shim.
- **Edits exonerated for the two peer issues below** — the pre-edit **baseline** firmware shows identical
  ESP↔ESP behaviour, and the data path is untouched by GAP-C/#14/#15.

### Pending: injector attack tests (defense-efficacy half)

The active-defense tests — crafted-Commit keep-link (GAP-C), Confirm-replay no-resend (#14), unsolicited-Open
drop (#15) — need a scapy S1G frame injector on chronium `morse0`. **Not completable on this bench as-is:**
chronium `wlan1` is in managed mode (morse0 only delivers in monitor type), scapy is not installed, and morse
`morse0` is a sniffer interface (S1G TX-injection is research-grade). Deferred to a focused session with the
injector set up. The defense **correctness** meanwhile rests on: (1) `sae.c` rejects off-curve/bad-scalar
Commits (read in source); (2) the shim calls `sae_parse_commit` with the proven args (code-verified); (3) it is
wired as a required reauth term; (4) peering+recovery work on-air, so `sae_parse_commit` runs correctly on real
Commits.

## Findings filed as TODOs (separate from these edits — see `rimba-mesh-ap-milestones.md`)

- **ESP↔ESP-direct peering needs a Linux mesh anchor.** Two ESP nodes alone both beacon but never peer (no
  SAE/discovery either way); once chronite anchors the mesh, both peer fine. The baseline firmware shows the
  same → **not** a regression. Hypothesis: independently-started ESP mesh BSSs don't converge (TSF/beacon-sync).
- **Mesh data path — RESOLVED (missing source-node mesh IP, not a forwarding bug).** The symptom *looked*
  bench-wide (secured plinks ESTAB but empty mpath + 100% ping loss, even Linux↔Linux), but the root cause was a
  **test artifact**: repeated `wpa_supplicant_s1g` restarts during debugging cleared chronite's manually-set mesh
  IP (`10.9.9.2`), so pings *from* chronite had no source → no data sent → no HWMP path demand → empty mpath. With
  the IP restored: **chronite→chronogen (Linux↔Linux) 0% loss + 3 mpaths**, **chronite→board0 (ESP) 0% loss**, and
  **chronite→board1 via board0 (multi-hop relay) works** — the full secured SAE+AMPE+CCMP data path (single- and
  multi-hop) is verified on the Phase-2 firmware. (`mesh_config -2` is benign — mac80211 does the forwarding.)
  **Bench gotcha:** per-node mesh IPs are NOT persistent across a wpa restart — re-assign after. Also: a transient
  morse SPI `-19` hang on chronogen cleared with a real reboot (no HW/SW difference from chronosalt — identical
  fw md5 + driver srcversion); an abrupt power-cut left chronosalt unbootable (check SD).

## Deferred backlog

- **Well-formed-forged-Commit reauth DoS residual.** GAP-C reaches hostap parity (closes the *malformed*-Commit
  teardown), but a *valid* forged Commit still tears a live link down — hostap itself reaches `ap_free_sta` for
  any such frame. Fully closing it needs a **non-hostap rate-limit on ACCEPTED-state reauth** — a deliberate
  divergence from the line-by-line port, deferred to a later phase.
