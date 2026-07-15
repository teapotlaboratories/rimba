# Mesh A-MPDU Aggregation on ESP32 / morselib — Feasibility & Staged Plan

**Status:** READ-ONLY design input — no code yet. This is the mandatory "documented TODO"
+ Linux code-map for the port (per `.ai/AGENTS.md` → Plan first / Porting Linux code).

**Trees pinned (verify anchors against these):**
- morselib working tree `dbecb4dd` (`components/halow/.../framework/morselib`); rimba `c16e436`.
- Linux reference (chronite `~/halow`): `rpi-linux` net/mac80211 `372414fd4`, `morse_driver` `3eef5a0`.
- Bench FW/driver standard **1.17.8**.

**Every `file:line` below was re-read against the pinned trees on 2026-07-11 — not the
9-day-old memory `mesh-no-ampdu-aggregation`, which this document supersedes.** Paths under
`umac_*`/`skbq.c`/`umac_ba.c` are relative to the morselib `src/` root; Linux paths are relative
to `net/mac80211/` unless noted.

> **S0 spike RESULT — GO (bench-verified 2026-07-11).** Both halves passed on hardware:
> - **S0a (capability):** temp `printf` after `umac_interface.c:321` on board1 → the MM6108 FW
>   advertises **`AMPDU_cap=1` for a MESH-type vif** (`ampdu_enabled=1` globally). aggr_check gate 2
>   (`:2044`) passes for mesh. Confirms B2.
> - **S0b (on-air emission):** temp-forced `MMDRV_TX_FLAG_AMPDU_ENABLED` on mesh unicast (**secured**
>   mesh, board1↔board0 direct peers, ch27), UDP blast board1→board0, captured on chronium `morse0`.
>   **Genuine A-MPDUs observed:** board1 QoS-Data frames with **consecutive sequence numbers sharing
>   one PPDU TSFT** (e.g. seq 794/795/796 @ tsft 188379322) — 217 aggregated PPDUs (52×2-MPDU,
>   165×3-MPDU); board0 aggregated up to **7 MPDUs/PPDU**. Throughput rose **~0.2 → 1.27 Mbit/s**
>   (~6×). So the MM6108 FW **does** assemble on-air A-MPDUs for 4-addr mesh QoS-data — confirms B4.
>   **The mesh is secured** (the app requests `MMWLAN_OPEN` but morselib forces SAE+PMF+SW-CCMP via the
>   compile-time `MMWLAN_MESH_SEC_PHASE1=1`, `umac_mesh.c:607`; SAE pw `rimbamesh2026`), and the
>   aggregated frames are **CCMP-encrypted** (`wlan.fc.protected=1`), so **SW-CCMP-per-MPDU composes
>   with A-MPDU** — validated on-air, not deferred. Temp code reverted; bench radio-silent. Worklog:
>   `2026-07-11-mesh-ampdu-s0-fw-capability-spike.md`.

---

## 1. Bottom line up front

**Feasible, and materially smaller than the old memory implies.** Rough estimate:
**~6–10 engineering-days** of host-stack wiring + interleaved on-air verification, **gated on a
half-day firmware go/no-go spike (S0)**. This is a BA-session-management + RX-routing +
relay-datapath-retag task — **not** a "multi-day core-RX feature" and **not** "TX aggregation
from scratch."

**The single biggest scope correction vs the old memory:** it named two things that are both wrong.

1. **The named blocker is stale.** "Mesh sends single MPDUs because of `aggr_check`'s
   `MMWLAN_STA_CONNECTED` gate" is refuted **in code**: a mesh vif installs `datapath_ops_mesh`
   (`umac_datapath.c:3677`) whose `.get_sta_state = umac_datapath_get_state_ibss`
   (`umac_datapath.c:3670`) returns `MMWLAN_STA_CONNECTED` **unconditionally**
   (`umac_datapath.c:3386-3390`). So gate 1 at `umac_datapath.c:2038` **passes for mesh**, and
   has since the mesh table bound that fn on 2026-06-26 (`c16e9a8a`) — **before** the memory was
   written. This is also correct follow-Linux behaviour: mesh peers are `wme=true`
   (`mesh_plink.c:542`) and pre-authorized, and `ieee80211_start_tx_ba_session` explicitly
   whitelists `NL80211_IFTYPE_MESH_POINT` (`agg-tx.c:610-614`, def `:605`).
2. **The A-MPDU is assembled by MM6108 firmware/HW, not the host.** `MMDRV_TX_FLAG_AMPDU_ENABLED`
   has exactly one setter (`umac_datapath.c:2274`) and one consumer (`skbq.c:832` + `:845` + the
   reorder-buf field `:847`) that merely copies it into chip descriptor bits
   (`MORSE_TX_CONF_FLAGS_CTL_AMPDU` / `TX_INFO_TID_PARAMS_AMPDU_ENABLED`). There is **no** host
   MPDU-concatenation/delimiter/subframe builder anywhere in-tree. The host owns only: the
   ADDBA/DELBA handshake (`umac_ba.c`), the per-frame eligibility flag, and the RX reorder buffer
   (`umac_datapath.c:1000-1309`) — and **all three already exist and work today for STA/AP**.

**What actually remains** (gaps the memory never named):

- **(RX — gates everything)** On a mesh vif, inbound `BLOCK_ACK` action frames are dropped, so no
  BA session can ever complete on a mesh link. On a **secured** mesh there are **two** drops to
  fix (routing + PMF/robust-mgmt exemption).
- **(Relay)** Forwarded frames bypass `aggr_check` entirely on a separate mgmt-class blocking path.
- **(Multi-hop local-origin)** BA is attempted against `common_stad` (self-addressed RA), which can
  never complete.
- **(Lifecycle)** BA teardown isn't wired into mesh peer loss.
- **(FW)** One unverified go/no-go: does the chip assemble A-MPDU for 4-addr mesh QoS-data with host
  SW-CCMP.

**Payoff is real** (§7): the perf data shows the single-flow mesh is **airtime-bound, not
host-bound**, and A-MPDU directly amortizes the per-frame preamble/IFS/backoff/ACK airtime that
dominates. The just-merged SW-CCMP bulk-AES work removed the crypto ceiling, which is exactly what
makes airtime the top remaining limiter.

> **S3 residual — ✅ SOLVED 2026-07-14 (defrag-before-decrypt, bench-VERIFIED):** the *host* fragmentation
> attempt (fragment→encrypt, halow `9c7daabd`) was bench-proven broken (the MM6108 FW re-headers any
> host-submitted `frag# > 0` MPDU — a hard FW wall, `#20`-family) and **reverted**. The residual is fixed by
> the OPPOSITE order — **defrag-before-decrypt**: the host submits ONE whole frame (no `frag#>0` handed to the
> FW), the FW fragments it (*encrypt→fragment*), and on RX the host **reassembles the raw fragments then
> decrypts the whole once**. Bench PASS: single-hop 131/131, multi-hop board0→board2→board1 143/146 (~98%),
> both directions, `ccmp_fail=0`, no crashes; toggle-off = whole/A-MPDU at high rate + rare natural frag
> handled. The min-MCS floor / MTU cap was NOT needed. Detail: worklog
> `../worklog/2026-07-14-mesh-defrag-before-decrypt-PASS.md` + `rimba-mesh-frag-codemap.md`.

---

## 2. Corrected blocker-map

| # | Old-memory blocker | Verdict | Current-code reality + anchor |
|---|---|---|---|
| **B1** | "`aggr_check`'s `MMWLAN_STA_CONNECTED` gate blocks mesh" | **Superseded** | Gate 1 (`umac_datapath.c:2038`) calls `data->ops->get_sta_state(stad)`; for mesh that is `umac_datapath_get_state_ibss` (`:3670`) → unconditional `CONNECTED` (`:3386-3390`). Installed by `configure_mesh_mode` (`:3677`). Passes since `c16e9a8a` (06-26), before the memory. Follow-Linux: `agg-tx.c:610-614` whitelists MESH_POINT; `mesh_plink.c:542` sets `wme=true`. |
| **B2** | Implied: a config/FW AMPDU-enable gate blocks mesh | **Superseded (config half)** | `umac_config_is_ampdu_enabled` defaults **true** (`umac_config.c:64`, getter `:167`); single global, no mesh/IBSS path clears it. Remaining OR-term is the FW cap bit `MORSE_CAP_SUPPORTED(...,AMPDU)` (`:2044`) — the **same** bit that gates STA A-MPDU and inbound ADDBA (`umac_ba.c:311`). Caps come from FW per-vif (`umac_interface.c:320`, getter `:955`). → runtime-only unknown, high-prior YES; confirm in S0. |
| **B3** | Implied: local-origin mesh TX never reaches `umac_ba_session_init` | **Partial** | **Single-hop** local-origin DOES reach it: `process_tx_frame` (`:2052`) → `aggr_check(stad=peer)` (`:2206`) → gates pass → `umac_ba_session_init` (`:2049`) fires ADDBA to the real peer (`umac_ba.c:256`/`:268`). **Multi-hop** reaches `aggr_check` but on `common_stad` (self-addressed) — separate gap B7. |
| **B4** | "multi-day core-RX feature" / host TX aggregation from scratch | **Superseded** | FW/HW assembles the A-MPDU. One setter `umac_datapath.c:2274`; consumer `skbq.c:832/:845/:847`; `mmdrv_tx_frame` takes a single mmpkt (wrapper `umac_datapath.c:2771`). RX reorder is host-resident + generic (`:1000-1309`). Mirrors `morse_driver` (chip gets a per-TID AMPDU-enabled + reorder-bufsize param; chip aggregates — `mac.c:1021` `tid_params`, cap `mac.c:78` `DOT11AH_BA_MAX_MPDU_PER_AMPDU=32`). |
| **B5** | *(unnamed)* mesh RX drops `BLOCK_ACK` action frames | **STILL-BLOCKS — the true unlock** | `datapath_ops_mesh.process_rx_mgmt_frame = umac_datapath_process_rx_mgmt_frame_mesh` (`:3661`) → for ACTION calls `umac_mesh_handle_action` (`:3643`), which handles only `DOT11_CATEGORY_MESH` (`umac_mesh.c:2738`) + `DOT11_CATEGORY_SELF_PROTECTED` and `return`s for everything else (`umac_mesh.c:2754-2757`). The generic dispatcher that routes `DOT11_ACTION_CATEGORY_BLOCK_ACK` → `umac_ba_process_rx_frame` (`umac_datapath.c:273-274`) is **never reached for a mesh vif** → the originator session never reaches `UMAC_BA_SUCCESS`, so `umac_ba_is_ampdu_permitted` (`umac_ba.c:514-522`) stays false forever. **On a secured mesh a second drop precedes it:** BlockAck is a robust-mgmt action; the mesh-action PMF/robust path drops the *unprotected* ADDBA at `umac_datapath.c:372-374` (only the `frame_is_mesh_action` exemption survives it). Follow-Linux: `rx.c:3662` `case WLAN_CATEGORY_BACK`, MESH_POINT whitelisted (`:3664`); MFP=no on ADDBA. |
| **B6** | *(unnamed)* forward/relay path can't aggregate | **STILL-BLOCKS relay** | `umac_mesh_forward_data` (`umac_mesh.c:2566`) builds an `MMDRV_PKT_CLASS_MGMT` frame and calls `umac_datapath_tx_mesh_unicast_frame` (`:2606`) → `umac_datapath_tx_mesh_keyed_frame` (`:2678`), which sets `flags = MMDRV_TX_FLAG_IMMEDIATE_REPORT` only (`:2708`), `tid = MMWLAN_MAX_QOS_TID = 7` (`:2732`, and `7 > UMAC_BA_MAX_AGGR_TID = 5` so `aggr_check` would early-return anyway at `:2031`), and transmits **blocking** (`mmdrv_tx_frame(txbuf, true)`, `:2754`). It never enters `process_tx_frame`, never calls `aggr_check`, never sets the AMPDU flag. The next-hop stad it *would* need (`nh_stad`) is already resolved here (`umac_mesh.c:2587`), so B7's self-address problem does **not** apply to relay. |
| **B7** | *(unnamed)* multi-hop local-origin BA is self-addressed | **STILL-BLOCKS multi-hop** | For a non-direct-peer DA, the TX stad falls back to `common_stad`, whose `bssid` and `peer_addr` are both this node's **own** mesh MAC (`umac_mesh.c:3111-3112`, `umac_sta_data_set_peer_addr(common_stad, mesh_ctx.mesh_mac)`). `aggr_check(stad=common_stad)` → `umac_ba_tx_addba_req` sends ADDBA to `peek_peer_addr(common_stad)` = own MAC → no peer responds. The datapath already resolves the true next hop for keying at `umac_datapath.c:2164-2172` (`nh_stad = umac_mesh_get_peer_stad(ra)`) — the fix mirrors that. |
| **B8** | *(unnamed)* BA teardown / lifecycle | **Minor gap** | `mesh_peer_free` (`umac_mesh.c:624`) frees `peer->stad` directly (`:636`) with **no** `umac_ba_session_deinit` / DELBA. `umac_ba_session_deinit` (`umac_ba.c:427`) is called only from ATE test code today. |

**The real remaining gaps, one line:** **B5** (mesh BLOCK_ACK RX routing **+** PMF exemption — unlocks
*everything*), **B6** (relay onto the data path), **B7** (multi-hop next-hop stad), **B8** (teardown),
plus **B2**'s FW-cap confirmation (S0). **B1/B4 are non-issues.**

---

## 3. Current-state trace (QoS-Data unicast, today)

**(a) Local-origin, single-hop (DA is a direct peer).**
`umac_datapath_process_tx_frame` (`:2052`) → TX stad = the **peer stad** →
`aggr_check(umacd, stad=peer, tid, ssc)` (`:2206`, guarded `!is_multicast && !is_eapol` at `:2204`)
→ gate 1 passes (B1), config gate passes (B2), FW-cap gate `:2044` (S0) → `umac_ba_session_init`
(`:2049`) → `umac_ba_tx_addba_req` sends an **NDP ADDBA req** (`umac_ba.c:261`,
`DOT11_BA_ACTION_NDP_ADDBA_REQ`) to the real peer MAC (`:268`). **But** the peer's ADDBA-resp is a
`BLOCK_ACK` action frame → dropped on RX by B5 → `originator[tid].status` never reaches
`UMAC_BA_SUCCESS` → `umac_ba_is_ampdu_permitted` false → `MMDRV_TX_FLAG_AMPDU_ENABLED` never set
(`:2272-2274`). **Result: single MPDUs.** (ADDBA is *attempted* on-air today but cannot complete.)

**(b) Local-origin, multi-hop (DA not a direct peer).**
Same path, but TX stad = `common_stad`, whose `peer_addr` = own mesh MAC (B7) → self-addressed
ADDBA, never completes. **Result: single MPDUs** (and would stay broken even after B5 until B7).

**(c) Forwarded / relay.**
RX handler → `umac_mesh_forward_data` (`umac_mesh.c:2566`) → correct
`nh_stad = umac_mesh_get_peer_stad(next_hop)` (`:2587`) → `build_mgmt_frame` (MGMT class) →
`umac_datapath_tx_mesh_unicast_frame(nh_stad, frame)` (`:2606`) → `umac_datapath_tx_mesh_keyed_frame`
→ `tid=7`, `IMMEDIATE_REPORT`, blocking (`:2708`/`:2732`/`:2754`). **`aggr_check` is never entered.**
**Result: single MPDUs, always — independent of B5.**

**RX reorder is already generic and mesh-ready.** `umac_datapath.c:1244` reads
`umac_ba_get_reorder_buffer_size(stad, tid)`; if the recipient BA session is absent
(`buffer_size == 0` or `get_expected_rx_seq_num < 0`) it delivers straight through (`:1246-1250`),
otherwise it reorders per-stad/TID (`:1253-1303`). So once a **recipient** session exists (created
in `umac_ba_rx_addba_req`, `umac_ba.c:286`, currently unreachable for mesh per B5) reorder engages
**with no new RX-datapath code**. Caveat: one active `rx_reorder_tid` per stad
(`umac_datapath.c:1253-1257`, single `rx_reorder_list`) — fine for mesh BE/TID0 (§7).

---

## 4. The real remaining work

### (a) BA-session setup / handshake for mesh peers
- **Missing:** nothing structural on the originator TX side for **single-hop** —
  `umac_ba_session_init` (`umac_ba.c:390`) + `umac_ba_tx_addba_req` (`:256`) already fire against a
  peer stad; the handshake simply can't complete because responses are dropped (see (b)). For
  **multi-hop**, the session must open on the **next-hop peer stad**, not `common_stad`.
- **Anchor:** `umac_datapath.c:2206` (aggr_check call), `:2164-2172` (existing next-hop resolve to
  mirror), `umac_ba.c:390`/`:256`/`:268`.
- **Linux:** `ieee80211_aggr_check` (`tx.c:1176`) → `ieee80211_start_tx_ba_session` (`agg-tx.c:605`,
  MESH whitelist `:610-614`); ADDBA sent to `sta->sta.addr` = next hop for a relayed frame
  (`rx.c:2788` `mpath->next_hop`, aggr on it `rx.c:2805`).

### (b) RX Block-Ack routing + PMF exemption + reorder — **THE UNLOCK**
- **Missing (two edits on a secured mesh):**
  1. **Route** `DOT11_ACTION_CATEGORY_BLOCK_ACK` to `umac_ba_process_rx_frame` on a mesh vif — add
     a case in `umac_datapath_process_rx_mgmt_frame_mesh` (`:3633`) or before the category gate in
     `umac_mesh_handle_action` (`umac_mesh.c:2754`), resolving the peer stad from the frame TA/SA.
  2. **Exempt** the unprotected ADDBA/BlockAck from the robust-mgmt drop at `umac_datapath.c:372-374`
     — mirror the existing `frame_is_mesh_action` exemption (Linux delivers ADDBA with MFP=no). On an
     **open** mesh this drop is skipped, so edit 2 is secured-mesh-only.
  - Reorder needs **no** new code (§3). Small nuance to verify on the bench: whether the recipient
    session's `next_expected_rx_seq_num` needs seeding from the ADDBA SSC (`ba_ssc`) or the
    timeout-flush self-corrects the first window (`umac_ba.c:336`/`:478-511`).
- **Anchor:** `umac_datapath.c:3633-3649`, `:372-374`, `umac_mesh.c:2738-2757`; generic dispatcher to
  reuse `umac_datapath.c:273-274`; reorder `:1000-1309`.
- **Linux:** `rx.c:3662` `case WLAN_CATEGORY_BACK` (MESH_POINT `:3664`) →
  `ieee80211_process_addba_request` (`agg-rx.c:435`) / resp (`agg-tx.c`) / delba; RX reorder alloc
  `__ieee80211_start_rx_ba_session` (`agg-rx.c:235`).

### (c) Per-frame `AMPDU_ENABLED` eligibility, incl. forward/relay
- **Missing (local multi-hop):** feed `aggr_check` (`:2206`) and the eligibility check (`:2272`) the
  **next-hop peer stad** instead of `common_stad` for a non-direct-peer DA — mirror `key_stad`
  (`:2164-2172`).
- **Missing (relay):** move `umac_mesh_forward_data` off the mgmt-class blocking path onto a data
  path — allocate `MMDRV_PKT_CLASS_DATA_TIDx` (not MGMT), set a real QoS TID **0–5** (not
  `MMWLAN_MAX_QOS_TID=7`), run an `aggr_check`-equivalent on `nh_stad`, set
  `MMDRV_TX_FLAG_AMPDU_ENABLED` via `umac_ba_is_ampdu_permitted(nh_stad, tid)` +
  `tid_max_reorder_buf_size`, and switch `mmdrv_tx_frame(..., true)` → non-blocking `(..., false)`.
  Targeted rewrite of `umac_datapath_tx_mesh_keyed_frame` (`:2678-2760`) / `umac_mesh_forward_data`.
  The on-air QoS wire-TID is already 0, so `tid=7` is an internal-only mismatch.
- **Anchor:** `umac_datapath.c:2271-2274`, `:2708`/`:2732`/`:2754`; alloc-class split in `mmdrv.h`
  (DATA_TID0..7 vs MGMT).
- **Linux:** `ieee80211_rx_h_mesh_fwding` re-injects the forwarded skb into the **normal** data TX
  path with `skb->priority` (TID) preserved — exactly why forwarded frames are aggregation-eligible
  per `(sta, tid)`. morselib's separate mgmt-class enqueue is the divergence to correct.

### (d) Capability / FW-contract items
- **Confirm (S0):** FW returns AMPDU-capable for a MESH vif (`:2044`; log `data->capabilities` after
  `umac_interface.c:320`). High-prior YES (chip-wide HW cap, same bit as STA + `umac_ba.c:311`).
- **Confirm (S0):** MM6108 actually assembles A-MPDU for **4-addr** mesh QoS-data (A4≠TA) and
  composes with **host SW-CCMP** per-MPDU. The true go/no-go (memory #20 shows the FW withholds the
  A4≠TA *HW-crypto forward* — a delivery gate, distinct from TX assembly, but adjacent enough to
  demand an on-air check).
- **Polish (follow-Linux, non-blocking):** (1) `aggr_check` doesn't gate on the **peer's advertised**
  AMPDU cap — Linux uses `mors_sta->ampdu_supported` from the peer S1G cap IE (`morse_driver/mac.c:4701`,
  gate `:2121`). (2) mesh never populates per-peer `ampdu_mss`; it's set only in the STA-assoc path,
  so mesh A-MPDU would carry `mmss=0` (`skbq.c:840`; Linux sets it via `S1G_CAP3` at `mac.c:1156`). FW
  likely applies its own floor — S0 says whether this is cosmetic or functional.

### (e) Mesh-peer BA teardown / lifecycle
- **Missing:** `umac_ba_session_deinit` + DELBA on peer loss. Wire into `mesh_peer_free`
  (`umac_mesh.c:624`, currently `mmosal_free(peer->stad)` at `:636` with no BA cleanup) and PERR /
  peering-close teardown.
- **Anchor:** `umac_ba.c:427`, `umac_mesh.c:624-640`.
- **Linux:** `ieee80211_sta_tear_down_BA_sessions` on `sta_info` removal; DELBA on plink close.

---

## 5. Staged, reversible implementation plan

Ordered so each stage is independently landable, low-risk first, inert if partial. Every code stage
ends with the project on-air rule: capture ADDBA-req/resp + BlockAck on chronium `morse0` monitor
(S1G ch, `CONFIG_MORSE_MONITOR`) **and** iperf/ping before-vs-after, then **radio-silent teardown**
after each bench run (flash `rimba-hello` to the ESPs + `ip link set wlan1 down` on Linux nodes).

### S0 — FW-capability spike (READ + bench, **go/no-go, do first, ~0.5 day**) — ✅ DONE 2026-07-11: GO (see header result box)
- **Goal:** prove the chip (1.17.8) (i) advertises AMPDU for a MESH vif and (ii) will emit an on-air
  A-MPDU for a 4-addr mesh QoS-data flow with host SW-CCMP.
- **Method:** temporary log of `data->capabilities` after `umac_interface.c:320`; then on an existing
  single-hop secured mesh, temporarily short-circuit the B5 RX drops + force `AMPDU_ENABLED` on a
  peer stad and watch `morse0` for an actual aggregated MPDU (A-MPDU delimiters /
  `WAS_AGGREGATED` tx-status) with a correct CCMP MIC at the peer. **No permanent code.**
- **Inert:** pure spike. **If NO-GO, stop** — host work can't help (document as a second universal FW
  limitation alongside #20).

### S1 — Mesh `BLOCK_ACK` action RX + mesh MFP=no — ✅ CODE DONE + SECURED-MESH ON-AIR VERIFIED 2026-07-11

> **The unlock (routing).** `umac_datapath_process_rx_mgmt_frame_mesh` (`umac_datapath.c:3652`) dispatches
> `DOT11_ACTION_CATEGORY_BLOCK_ACK` → peer-keyed `umac_ba_process_rx_frame(peer_stad,…)` (`:3657`, peer stad
> from frame SA), mirroring net/mac80211 `rx.c:3662` `case WLAN_CATEGORY_BACK`. Both ADDBA req+resp take this
> path. **On-air (secured all-ESP mesh, no force):** board1 emits genuine **A-MPDUs** (consecutive-seq MPDUs
> sharing a PPDU TSFT — e.g. 852/853/854) — which requires a **completed BA session**, proving the handshake
> now completes through the routing.
>
> **SW-CCMP composes with A-MPDU.** The mesh is secured (SAE+PMF+SW-CCMP, forced by `MMWLAN_MESH_SEC_PHASE1=1`
> regardless of the app's OPEN request). Captured QoS-Data frames are **100% `protected=1`** (CCMP), and they
> aggregate — so host CCMP-per-MPDU + A-MPDU compose, validated on-air.
>
> **MFP-asymmetry root fix (2026-07-11).** The mesh set peer stads `PMF_REQUIRED`, so the host pairwise-encrypted
> unicast robust mgmt (ADDBA/DELBA, unicast PREP) at `umac_datapath.c:2584` — while net/mac80211 runs mesh peers
> **MFP=no** (`tx.c:458` protects mgmt only for `WLAN_STA_MFP`; the Linux mesh has no `ieee80211w`, verified
> `iw station dump` → `MFP: no`) and sends them in the clear. So the ESP transmitted a CCMP-protected ADDBA a
> Linux peer doesn't expect — an interop-breaking asymmetry (morselib's own comment called the stad
> "*incorrectly* PMF_REQUIRED"). **Fixed at the root:** set the mesh peer stads + common stad **MFP=no**
> (`MMWLAN_PMF_DISABLED`, `umac_mesh.c:607`/`:3119`). `pmf_is_required()` is now false for mesh, so all mesh
> management is sent unprotected **and** the unprotected-robust-mgmt RX drop (`:373`) is skipped wholesale —
> which makes the earlier per-category exemption (`frame_is_block_ack_action`, the interim "edit 1")
> unnecessary, so it was **removed**. `security_type` stays SAE, so pairwise/group keys + **DATA CCMP are
> unchanged**. Net S1 change = BLOCK_ACK routing + MFP=no (2 files).
>
> **On-air verified (secured all-ESP mesh, chronium `morse0`):** after the fix the **ADDBA is transmitted
> unprotected** (tshark now decodes category 3, `fc.protected=0`; before: encrypted → empty category) while
> **DATA stays CCMP** (`fc.protected=1`); SAE/AMPE peering + ping (5/5) + A-MPDU still work. An **A/B in the
> same RF conditions** confirmed the evening's reduced aggregation depth was **environmental** — the prior
> protected-ADDBA build degraded identically (217 PPDUs @20:04 → ~34 @22:12) — **not** a regression from this
> change.
>
> **Follow-ups:** on-air ESP↔Linux BA interop capture once a stable Linux mesh node exists (this session it
> was blocked by bench HW — cross-vendor SAE peering wouldn't form + both Pi-Zero nodes reboot on radio-up);
> multi-hop (S2) + relay (S3). Worklog: `2026-07-11-mesh-ampdu-s1-blockack-rx-routing.md` (TODO).
- **Goal:** let ADDBA/DELBA/BlockAck action frames reach the BA state machine on a mesh vif so a
  single-hop originator session reaches `UMAC_BA_SUCCESS`.
- **Touch (two edits):** (1) add a `DOT11_ACTION_CATEGORY_BLOCK_ACK` case routing to the existing
  `umac_datapath.c:273-274` path from `process_rx_mgmt_frame_mesh` (`:3633`) / `umac_mesh_handle_action`
  (`:2754`); (2) exempt the unprotected BlockAck action from the robust-mgmt drop
  (`umac_datapath.c:372-374`), mirroring `frame_is_mesh_action` (secured-mesh only).
- **Linux:** `rx.c:3662-3696` (BACK category, MESH whitelist, MFP=no).
- **Inert/testable:** additive; both edits turn a silent drop into a dispatch. No originator session is
  opened by this change, so it's a no-op on TX until one exists — but because single-hop TX **already**
  fires ADDBA today (§3a), **single-hop local-origin mesh A-MPDU should light up after S1 alone**, and
  RX reorder engages with no further code. Bench: on-air ADDBA-resp consumed → session flips to
  `UMAC_BA_SUCCESS`; iperf single-hop before/after.

### S2 — Multi-hop local-origin: BA on the next-hop peer stad — ✅ CODE DONE + core mechanism on-air-verified 2026-07-11

> **On-air (secured all-ESP forced line board1→board0→board2, cold capture):** the multi-hop origin now
> **completes a Block-Ack handshake with its next hop** — ADDBA-req board1→board0 (#428) + ADDBA-resp
> board0→board1 (#436, +53 ms), both **unprotected** (MFP=no). Before S2 the same flow emitted **zero**
> on-air ADDBA (self-addressed on `common_stad`). **Sustained A-MPDU on the first-hop leg was NOT achieved**
> — blocked by the **orthogonal** HWMP path-flapping bug (next hop alternates board0/board2, diluting the
> queue), not by S2 (single-hop, a no-op under S2, still aggregates). Worklog:
> `2026-07-11-mesh-ampdu-s2-multihop-nexthop-stad.md`. Uncommitted one-file working-tree change.
>
> **The design's 3-line touch below was INCOMPLETE — the correct S2 is 4 edits**, all "use `key_stad`
> (= the next-hop peer stad already resolved for CCMP keying at `umac_datapath.c:2164-2172`) off the
> multi-hop mesh path; `key_stad == stad` for single-hop/non-mesh/multicast so all four are no-ops there":
> 1. `aggr_check` call (~`:2206`) `stad`→`key_stad` — ADDBA on the next hop, not self-addressed.
> 2. `umac_ba_get_reorder_buffer_size` (~`:2271`) `stad`→`key_stad`.
> 3. `umac_ba_is_ampdu_permitted` (~`:2272`) `stad`→`key_stad`.
> 4. **(MISSED by the original sketch)** relocate the `sta_data` binding (was at the top of the fn) to
>    **after** the key_stad resolution and bind it to `key_stad`, so the **802.11 QoS-data sequence number**
>    (`:2199-2201`) is drawn from the next-hop peer's per-TID counter. Required for correctness on any
>    2+-next-hop or mixed single/multi-hop-via-P mesh; the single-next-hop bench rig *masks* its absence.
>    Follow-Linux: `ieee80211_tx_h_sequence` stamps `seq_ctrl = ieee80211_tx_next_seq(tx->sta, tid)`
>    (`tx.c:886`, `tx.c:807-815`) with `tx->sta = sta_info_get(addr1 = mpath->next_hop)` (`tx.c:1239-1241`,
>    `mesh_hwmp.c:1385-1388`) — a per-next-hop-peer counter, NOT per-vif, NOT per-final-dest.
> - Confirmed **stays on `stad`** (verified, do NOT move): `tx_metadata->aid` (~`:2280`, tx-status→RC
>   routing, not read by the FW A-MPDU path) and `umac_rc_init_rate_table_data` (~`:2296`, the separate
>   mesh-RC workstream).
- **Goal:** make BA setup + eligibility + the QoS-data seqno use the resolved next-hop peer stad for a
  non-direct-peer DA instead of `common_stad`.
- **Linux:** `rx.c:2788`/`:2805` (aggr keyed on `mpath->next_hop`); `agg-tx.c:641` (MESH whitelist);
  `tx.c:886`/`:807-815`/`:1239-1241` + `mesh_hwmp.c:1385-1388` (seqno off the next-hop sta).
- **Inert/testable:** guarded to the multi-hop mesh case; single-hop unaffected (byte-identical path,
  `key_stad == stad`). Bench: forced line `board1→board0→board2`; confirm the ADDBA-req/resp to/from the
  real next hop on `morse0` (done). A *sustained-aggregation* number needs a stable single next-hop path
  (fix HWMP flapping, or an out-of-RF-range far node) — see the worklog §6.

### S3 — Relay/forward onto the aggregation-eligible data path (~2–3 days + bench)
> **✅✅ S3 BLOCKER ROOT-CAUSED + FIXED + ON-AIR CONFIRMED 2026-07-13. Worklog
> `docs/worklog/2026-07-13-mesh-ampdu-s3-rate-attribution-fix.md`.** The "relay forwards ~0 / ~99.6% of
> forwarded frames MIC-fail" wall that blocked S3 for weeks was NOT the forward path failing to aggregate
> (the B6 work below is valid but was never the cause), NOT #20, and NOT a key desync. It was a **TX-rate
> attribution bug on the multi-hop ORIGIN leg**: a mesh origin unicast to a multi-hop dest is dequeued on
> the `common_stad` (final dest isn't a direct peer), and `umac_datapath_process_tx_frame` initialized the
> **rate table + aid on `stad` (=common_stad ≈ MCS0)** while seqno/BA/reorder already keyed on `key_stad`.
> At MCS0/1 MHz a full-size 4-addr frame exceeds the max PPDU → the MM6108 FW **fragments** it, setting
> moreFrag in the on-air FC **after** the host SW-CCMP MIC → the next hop MIC-fails ~99.6%, so the relay
> could never even decrypt the origin frames to forward them. **FIX (`umac_datapath.c` ~:2334/:2350, follows
> net/mac80211 `tx->sta = sta_info_get(addr1=next_hop)`): init `aid` + `umac_rc_init_rate_table_data` on
> `key_stad`, not `stad`** (no-op off the multi-hop path). On-air (all-ESP line board0→board2→board1):
> board2 decrypt **0.4% → 96.5%**, relay now decrypts + forwards, board1 end-to-end. Residual ~3.5% = the
> mmrc MCS0 rate-chain fallback / cold-start transient (deterministic follow-up: min-MCS floor or MTU cap;
> the frag-threshold param is vendor-confirmed useless here). This completes the "frame follows the next hop"
> pattern the S2 work started. The B6 forward-path aggregation below is now UNBLOCKED + measurable.
>
> **✅ CODE DONE 2026-07-12 (builds green, adversarially reviewed, NOT bench-verified; uncommitted).** Worklog
> `docs/worklog/2026-07-12-mesh-ampdu-s3-relay-fwd-aggregation.md`. Landed with **FIX-2** (non-blocking forward,
> the interrupt-WDT trigger) at the same seam. New `build_mesh_data_frame` allocs the forward on
> `MMDRV_PKT_CLASS_DATA_TID0+tid` (MGMT class → `mgmt_q`, never A-MPDUs); `umac_datapath_tx_mesh_keyed_frame`
> gains per-TID seqno + `aggr_check` + `AMPDU_ENABLED` for the unicast forward (all consistent at TID 0),
> `mmdrv_tx_frame(txbuf,!fwd_aggregate)` channel select, and a non-blocking drop-gate hoisted to the top.
> Behind `MESH_FWD_DATA_AGGREGATE` (default 1) for bench A/B. In-place-forward deferred.
- **Goal:** forwarded unicast aggregates per next-hop link (the dominant relay-goodput win). Naturally
  combined with the independent **in-place-forward** optimization at the same seam.
- **Touch:** `umac_mesh_forward_data` (`umac_mesh.c:2566`) + `umac_datapath_tx_mesh_keyed_frame`
  (`:2678`): DATA_TIDx alloc not MGMT; QoS TID 0–5 not `MMWLAN_MAX_QOS_TID`; `aggr_check`-equivalent on
  `nh_stad`; set `AMPDU_ENABLED` + `tid_max_reorder_buf_size` via `umac_ba_is_ampdu_permitted(nh_stad,
  tid)`; non-blocking `mmdrv_tx_frame`. Preserve host SW-CCMP per-MPDU (`:2714-2724`) and the A4/mesh-SA
  header. tx-status accounting then works for free (`umac_datapath.c:2776`).
- **Linux:** `ieee80211_rx_h_mesh_fwding` (re-inject into data TX path, TID preserved).
- **Inert/testable:** largest change — land behind a mesh config flag (reuse the `umac_config` pattern)
  so it can toggle back to today's mgmt-class forward. Bench: 2-hop iperf before/after (baseline
  ~0.03–0.06 Mbps) + on-air A-MPDU on the relay egress.

#### S3 residual (full-size non-aggregated frames) — DECISION: host fragmentation is a FW wall; use MTU-cap / min-MCS floor

> **⛔ DECISION 2026-07-14 — host 802.11 fragmentation (fragment→encrypt) is ABANDONED for mesh SW-CCMP;
> it is bench-proven incompatible with this firmware. The deterministic residual fix is a min-MCS floor or
> mesh-vif MTU cap (so a SW-CCMP frame never needs fragmenting), NOT host fragmentation.**
>
> The S3 residual (any full-size mesh SW-CCMP frame whose rate/size won't fit one PPDU — e.g. cold-start /
> pre-BA / rate-fallback — gets FW-fragmented after the host CCMP MIC, so the peer MIC-fails) was first
> pursued via **host fragmentation** (mirror net/mac80211 fragment→encrypt: split into <728 B FCS-less
> MPDUs, per-fragment PN/MIC/moreFrag, non-aggregated). Implemented + adversarially reviewed + committed as
> halow `9c7daabd` (rimba `2bb61d2`); code-map `rimba-mesh-frag-codemap.md`.
>
> **Bench-verified 2026-07-14 = BROKEN.** Worklog `docs/worklog/2026-07-14-mesh-frag-bench-verify.md`
> (forced all-ESP line board0→board2(relay)→board1). board0 host-fragments correctly (`TXsent=252` = 3
> fragments/ping; on-air **frag0 is perfect**), but the **MM6108 FW re-processes every MPDU with
> `fragment_number > 0`** — off-air it prepends a byte-identical **duplicate 32-byte MAC header** (CCMP
> header shoved to offset 64), sets Retry, clears MoreFrags → corrupts the CCMP AAD → the relay MIC-fails
> **~64%** (158/248), reassembles **0** MSDUs, forwards nothing; pings dead. Control (fix off, A-MPDU sends
> full-size frames **whole**) delivers **100+ pings end-to-end** once the BA session is up → the rig is
> sound and the failure is the fragment path; the fix is also **ineffective** (its residual = cold-start
> pre-BA frames = exactly what gets mangled + lost).
>
> **Follow-Linux cross-check (both trees, VERY HIGH confidence) = a hard FW wall, `#20`-family.** Linux
> never host-fragments on this FW: `ieee80211_tx_h_fragment` skips host frag under `SUPPORTS_TX_FRAG`
> (`net/mac80211/tx.c:967-968`) + defaults to `DONTFRAG`; morse_driver sets `SUPPORTS_TX_FRAG` from the FW
> `HW_FRAGMENT` cap (`morse_driver/mac.c:6835-6836`); fragmentation is FW-owned via a **global threshold**
> command. There is **no per-frame "already-fragmented / raw / leave-it-alone" field** in the host→FW TX
> descriptor in EITHER tree (`skb_header.h:57-71` / morselib `mmdrv.h:609-619`), so the hypothesized
> morselib gap-flag does not exist. **Crypto mode (verified 2026-07-14):** the bench Linux nodes run **HW
> crypto** (`no_hwcrypt=N` live on chronite/chronium/chronosalt; `morse_mac_ops_set_key` HW-offloads CCMP,
> `mac.c:5141-5230`), which is **single-hop-only for encrypted traffic** — `#20` blocks the HW-crypto
> multi-hop forward — so there is **no working multi-hop-encrypted Linux mesh to follow**. mac80211 also
> **never host-fragments by default** (`frag_threshold=-1` → `DONTFRAG` on every unicast, `tx.c:1275-1279`);
> it relies on the FW to fragment, which is correct only under HW crypto. Forcing Linux to SW crypto
> (`no_hwcrypt=1`) for multi-hop would hit the **same wall** the ESP does. (Earlier phrasing "Linux uses HW
> crypto to sidestep this" was imprecise + self-contradictory vs `#20` — the accurate statement is that the
> ESP's SW-CCMP multi-hop mesh is territory Linux does not operate in, so there is no reference and no
> host-side fragmentation fix on this FW.)
>
> **On-air confirmed 2026-07-14.** Secured single-hop Linux mesh (HW crypto), forced MCS0 + a 512 B frag
> threshold → the **FW fragmented a 1400 B ping into 4 CLEAN fragments** (MoreFrags 1,1,1,0, per-fragment
> CCMP + consecutive PN, **CCMP at offset 32 = single-header**, len 528 — vs the ESP's frag>0 double-header,
> CCMP at offset 64, len 763) and it **delivered 61/70 (87%)** vs the ESP relay's 0%. Under HW crypto the FW
> owns fragment→encrypt and gets it right; the ESP fails only because SW-CCMP forces the host to encrypt,
> which the FW then re-headers. Worklog `2026-07-14-mesh-frag-bench-verify.md` → "On-air confirmation".
>
> **Actions:** (1) `9c7daabd` **REVERTED** 2026-07-14 (`git revert --no-commit`, staged, builds green,
> commit held for after-hours). (2) An MTU-cap / min-MCS floor was scoped but is **NOT** being shipped —
> see the update below.
>
> **UPDATE 2026-07-14 (ESP verification of the reverted mesh) — the revert IS the practical fix; MTU-cap is
> unnecessary.** Measured (Linux, same FW): a full-MTU frame at **MCS0/1 MHz goes WHOLE (1566 B)** — MCS0 is
> NOT where a normal frame fragments; the mesh only fragments when it drops BELOW MCS0 (the 1 MHz repetition
> mode, single-PPDU budget ~728–740 B — so `728` was right for the *worst* rate, not MCS0). On the **reverted
> ESP mesh** (no host frag, real conditions, full-size multi-hop): **fragmentation is RARE — 1 fragmented
> frame in 20 s (~0.24%) vs 410 whole**; ~86% ping success on a *marginal* MCS0/MCS2 bench link where the
> ~14% loss is low-rate RF loss, not fragmentation; on a good link (MCS7) fragmentation is **zero**. So the
> revert removes the *systematic* breakage (Test A/B *forced* every frame to fragment; unforced, the FW
> fragments almost nothing). **An MTU cap is overkill** for a &lt;0.3% weak-link edge (it caps every frame).
> **A min-MCS floor** (keep the RC off the ~728-budget bottom rate) is the right *optional* weak-link
> hardening, but needs the fragmenting rate pinned and the payoff is small — a future item, not a bolt-on.
> Worklog `docs/worklog/2026-07-14-mesh-frag-bench-verify.md` → "Revert + ESP verification".

### S4 — BA teardown / lifecycle (~1 day)
- **Goal:** no leaked sessions / stale DELBA on peer loss or path change.
- **Touch:** `mesh_peer_free` (`umac_mesh.c:624`) → `umac_ba_session_deinit` (`umac_ba.c:427`) + DELBA
  for originator & recipient on `nh_stad`; consider teardown on HWMP next-hop change (path flapping,
  §7).
- **Inert/testable:** additive cleanup; soak with peer churn to confirm `originator[6]`/`recipient[6]`
  don't exhaust.

### S5 — Follow-Linux polish + tuning (~1 day, optional)
- **Goal:** per-peer advertised-AMPDU-cap gate in `aggr_check`; populate mesh `ampdu_mss`; evaluate
  raising the reorder window against per-peer reorder RAM across `MESH_MAX_PEERS`.
- **Touch:** `umac_datapath.c:2043` (add peer-cap gate), the mesh connection path (`ampdu_mss`),
  `umac_datapath.h` reorder-maxlen.
- **Linux:** `mac.c:4701-4703` (peer AMPDU cap), `mac.c:1156` (`ampdu_mss`), `mac.c:78`
  (`DOT11AH_BA_MAX_MPDU_PER_AMPDU`).

**Landing-order rationale:** S1 is the smallest change and the single unlock (single-hop A-MPDU works
after it alone); S2 and S3 are independent add-ons; S4/S5 are hardening. **If S0 is NO-GO, none of
S1–S5 ship.**

---

## 6. Linux code-map skeleton

Mandatory follow-Linux map (pinned trees above). "Exists" = already in morselib (STA/AP path), reused
for mesh; "ADD" = new mesh wiring. Line-exact anchors to be re-confirmed in each stage's shipped
code-map as the code lands.

| morselib element | State | Linux symbol @ file:line |
|---|---|---|
| TX BA start trigger — `umac_datapath_aggr_check` (`umac_datapath.c:2026`, call `:2206`) | Exists | `ieee80211_aggr_check` `tx.c:1176`; `ieee80211_start_tx_ba_session` `agg-tx.c:605` (MESH whitelist `:610-614`) |
| ADDBA-req TX — `umac_ba_tx_addba_req` (`umac_ba.c:256`) | Exists | `ieee80211_send_addba_request` `agg-tx.c:61`; sender `:479` |
| **ADDBA/DELBA/BA RX dispatch on mesh vif** — **✅ S1 code done 2026-07-11**: BLOCK_ACK case in `umac_datapath_process_rx_mgmt_frame_mesh` `umac_datapath.c:3652`, peer-keyed `umac_ba_process_rx_frame(peer_stad,…)` `:3657` (peer stad from frame SA) | Wired | `rx.c:3662` `case WLAN_CATEGORY_BACK` (MESH `:3664`) → `ieee80211_process_addba_request` `agg-rx.c:435` |
| **Mesh MFP=no** — **✅ S1 code done 2026-07-11**: peer + common stad `MMWLAN_PMF_DISABLED` `umac_mesh.c:607`/`:3119` (so mesh mgmt incl. ADDBA/PREP is sent unprotected; the interim `frame_is_block_ack_action` exemption was removed as redundant) | Root fix | mesh sta MFP=no → mgmt protection off (`tx.c:458`) + `ieee80211_drop_unencrypted_mgmt` drop skipped (gated on `WLAN_STA_MFP`) |
| BA RX consumer — `umac_ba_process_rx_frame` (`umac_ba.c:348`) | Exists (unreachable for mesh until S1) | `ieee80211_process_addba_resp` (`agg-tx.c`) → `ieee80211_agg_tx_operational` |
| RX reorder buffer (`umac_datapath.c:1000-1309`; fields `umac_datapath_data.h:95`/`:97`) | Exists — generic, auto-engages | `__ieee80211_start_rx_ba_session` `agg-rx.c:235`; reorder release in `rx.c` |
| Recipient session — `umac_ba_rx_addba_req` (`umac_ba.c:286`, cap check `:311`) | Exists (unreachable until S1) | `ieee80211_process_addba_request` `agg-rx.c:435` → `__ieee80211_start_rx_ba_session` |
| Mesh state gating — keep unconditional CONNECTED (`get_state_ibss` `:3670`) | Exists — correct as-is | `mesh_plink.c:542` (`wme=true`); pre-authorized peer |
| **Multi-hop next-hop BA target** — **✅ S2 code done 2026-07-11**: `aggr_check`/`reorder_buf_size`/`is_ampdu_permitted` + the QoS-data seqno all keyed on `key_stad` (= `nh_stad` from `umac_datapath.c:2164-2172`); on-air ADDBA req+resp to/from the real next hop | Wired | aggr keyed on `mpath->next_hop` `rx.c:2788`/`:2805`; seqno off `tx->sta`=next hop `tx.c:886`/`:1239-1241`, `mesh_hwmp.c:1385-1388` |
| **Relay re-inject onto data path** — `umac_mesh_forward_data` (`umac_mesh.c:2573`) → DATA-class `build_mesh_data_frame` + `umac_datapath_tx_mesh_keyed_frame` per-TID seqno/`aggr_check`/`AMPDU_ENABLED` — **✅ S3 code done 2026-07-12** | Wired | `ieee80211_rx_h_mesh_forward` (`set_qos_hdr` + re-inject into data TX path, TID preserved) |
| Per-TID FW AMPDU param (`skbq.c:832`/`:845`/`:847`) | Exists | `morse_driver/mac.c:1021` (`tid_params`), cap `mac.c:78` (`DOT11AH_BA_MAX_MPDU_PER_AMPDU=32`) |
| **BA teardown** — `umac_ba_session_deinit` (`umac_ba.c:427`) | **ADD wire into `mesh_peer_free` (S4)** | `ieee80211_sta_tear_down_BA_sessions` on `sta_info` removal |
| Peer AMPDU-cap gate / `ampdu_mss` (S5) | **ADD** | `mac.c:4701-4703` (peer cap), `mac.c:1156` (`ampdu_mss`) |

**Deliberate divergences from Linux (justified):**
- **FW/HW assembles the A-MPDU.** No host MPDU-concatenation/delimiter builder — the chip aggregates on
  `MORSE_TX_CONF_FLAGS_CTL_AMPDU`. This *matches* `morse_driver` (also delegates to the MM6108) and is
  unlike a generic softmac. Host owns BA state + flag + reorder only.
- **Fixed arrays, not dynamic per-TID.** `originator[6]`/`recipient[6]` per stad
  (`umac_ba_data.h:48`/`:50`, `UMAC_BA_MAX_SESSIONS=6`) and a **single** active reorder TID per stad
  (`umac_datapath.c:1253-1257`) vs Linux dynamically-allocated `tid_ampdu_rx[]`. Acceptable: mesh data
  is BE/TID0.
- **Host-timer scaffolding.** ADDBA backoff via `umac_core_register_timeout` (`umac_ba.c:416`) vs Linux
  `wiphy_work`; ADDBA sent **inline** in `session_init` rather than deferred to a work item after a
  driver `TX_START` confirm.
- **S1G NDP ADDBA.** morselib emits `DOT11_BA_ACTION_NDP_ADDBA_REQ` (`umac_ba.c:261`); confirm the ESP
  FW + Linux peer expect NDP-BlockAck vs legacy compressed BlockAck.

---

## 7. Risks / open questions / bench-only unknowns

**Is the payoff real? Yes — the old memory's "goodput looks host-bound" doubt is contradicted by
current data.** The perf matrix (milestones §Performance) shows: within each row node type barely
matters (channel airtime dominates, not Linux-vs-ESP); open ≈ secured relay (~0.14 vs ~0.23 Mbps, same
order); and the just-merged bulk-DMA AES-CCM cut per-frame crypto ~14–36× but **did not** raise
single-flow throughput. All three point the same way: the limiter is **airtime**, and single-flow mesh
(~0.13–0.20 Mbps 1-hop) sits ~5× below AP↔STA (~0.8–1.1 Mbps) largely on 4-addr + Mesh-Control +
per-frame preamble/IFS/backoff/ACK overhead. A-MPDU amortizes that overhead across N MPDUs — precisely
the airtime tax — so it should lift single-flow and especially 2-hop relay (~0.03–0.06 Mbps). **Bench
measures it directly:** iperf UDP/TCP before/after on 1-hop and the forced 2-hop line + `morse0`
capture of a real A-MPDU.

**FW go/no-go (S0, dominant risk):** whether MM6108 assembles A-MPDU for 4-addr mesh QoS-data (A4≠TA)
and composes it with host SW-CCMP per-MPDU. Memory #20 proves the FW *withholds the A4≠TA HW-crypto
forward* (a universal MM6108 limitation, Linux too) — that's an RX/decrypt-delivery gate, **not** TX
assembly, so it doesn't directly predict A-MPDU behaviour, but it's close enough that S0 must settle it
on-air before any host commitment.

**A-MPDU subframe/duration limits at 1 MHz S1G:** driver clamp 32 MPDU/A-MPDU (`mac.c:78`); morselib
reorder window default 16 (`umac_ba.c:410`, `umac_datapath.h`). At 1 MHz the TXOP/PPDU-duration limit
may cap the *effective* subframe count well below 16, and small mesh frames may rarely queue deep enough
to aggregate — so measure realized aggregation depth, not just "on." Reorder RAM scales with
window × `MESH_MAX_PEERS` (S5 budget question).

**Interop with Linux mesh peers:** ADDBA/BlockAck encoding must match (NDP vs compressed BlockAck); on a
secured mesh the ADDBA is a robust action — the TX side uses `umac_datapath_build_and_tx_mgmt_frame`
(`umac_ba.c:274`), **not** the mesh keyed path, so confirm on-air a peered node accepts it (PMF/pairwise)
once S1 lands.

**Multi-hop `common_stad` BA identity:** the fix is per-next-hop-link BA (follow Linux
`mpath->next_hop`). This interacts with the known HWMP path-flapping bug (no PREQ reply-dedup): a
next-hop change could strand a session → S4 should tear down on path change; a churn soak must confirm
no session-struct exhaustion.

**Reorder SSC seeding:** whether the recipient session's `next_expected_rx_seq_num` needs seeding from
the ADDBA `ba_ssc` (`umac_ba.c:336`/`:478-511`) or the timeout-flush self-corrects the first window —
bench-verify first-window ordering/latency (S1).

**TID distribution:** local-origin mesh data `tx_metadata->tid` (`:2058`) is effectively BE/TID0, so
only one originator session per peer is exercised — fine, but confirm nothing silently sets
`tid > UMAC_BA_MAX_AGGR_TID`.

---

## 8. Docs / worklogs + milestones integration

**This document** = `docs/mesh-ap/rimba-mesh-ampdu-aggregation-design.md` (feasibility + staged plan +
§6 code-map, satisfying the mandatory code-map rule; split §6 into a separate
`rimba-mesh-ampdu-codemap.md` if it grows).

**Per-stage worklogs** (dated, standalone, each with its companion HTML render per `.ai/AGENTS.md`):
- `docs/worklog/2026-07-xx-mesh-ampdu-s0-fw-capability-spike.md` (go/no-go, on-air)
- `docs/worklog/2026-07-xx-mesh-ampdu-s1-blockack-rx-routing.md` (single-hop A-MPDU lights up)
- `docs/worklog/2026-07-xx-mesh-ampdu-s2-multihop-nexthop-stad.md`
- `docs/worklog/2026-07-xx-mesh-ampdu-s3-relay-datapath-aggr.md` (2-hop iperf before/after)
- `docs/worklog/2026-07-xx-mesh-ampdu-s4-teardown-lifecycle.md`

**Plug into `docs/mesh-ap/rimba-mesh-ap-milestones.md`:**
- **Correct the stale backlog bullet (~`:805-810`)** — it still says "gate is `aggr_check`'s
  `MMWLAN_STA_CONNECTED` check" and "multi-day core-RX feature." Replace with the corrected blocker-map
  (B5 the true unlock; FW/HW assembles; the CONNECTED gate already passes) and link this design doc.
- Cross-reference the ✅ SW-CCMP bulk-AES item as the prerequisite that made this the top ceiling.
- On completion, update the §Performance table with measured before/after goodput.
