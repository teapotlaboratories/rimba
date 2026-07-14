# Mesh A-MPDU S3 — the relay-forward drop was a TX-rate-attribution bug (moreFrag / FW fragmentation). FIXED + on-air confirmed (2026-07-13)

## TL;DR

The long-standing "S3 relay forwards ~0 / ~99.6% of forwarded frames MIC-fail" wall was **not**
#20 (HW-crypto A4≠TA), **not** a pairwise-MTK desync, and **not** an A-MPDU-of-4addr interaction.
It was a **wrong TX-rate attribution** on the multi-hop mesh **origin** leg:

- A locally-originated mesh unicast to a multi-hop destination is dequeued on the **`common_stad`**
  (the final destination is not a direct peer). `umac_datapath_process_tx_frame` initialized the
  **rate-control table and the AID on `stad` (= `common_stad`)** — the untrained MBSS default, ≈ MCS0 —
  while the sequence-number space, BA session and reorder window had already been (correctly, S2) moved
  to `key_stad` (the next hop).
- At **MCS0 / 1 MHz** a full-size (~1500 B) 4-address frame **exceeds the maximum PPDU**, so the **MM6108
  firmware fragments it**. Fragmentation sets **moreFrag (FC bit 10, 0x0400)** in the on-air Frame Control
  **after** the host has already computed the SW-CCMP MIC over the *unfragmented* FC. CCMP AAD covers FC
  and (correctly, per 802.11 / mac80211) does **not** mask moreFrag, so the next hop's MIC check fails on
  ~99.6% of the (full-size, multi-hop) frames.

**Fix** (`umac_datapath.c`, `umac_datapath_process_tx_frame`): initialize `tx_metadata->aid` and
`umac_rc_init_rate_table_data(...)` on **`key_stad`** (the next hop) instead of `stad`, mirroring
net/mac80211 where `tx->sta = sta_info_get(addr1 = mpath->next_hop)` drives rate control and the per-STA
TX context. `key_stad == stad` off the multi-hop mesh path, so single-hop / non-mesh / multicast are
byte-identical.

**Result (on-air, all-ESP forced line board0 → board2(relay) → board1):** board2's decrypt success on the
forwarded-origin leg went from **0.4% → 96.5%** (`mesh_seen=1917 sw_ccmp_fail=68 decrypt_ok=1849`); the relay
now decrypts **and forwards** (`rx_reached=1834 mmdrv_ok=1038`); board1 (far endpoint) receives it
(`decrypt_ok=1065`). The residual ~3.5% is the mmrc rate-training transient (first frames still go at MCS0
until the peer's rate table trains up).

## How the confound was cleared (the essential first step)

Every prior session diagnosed this on a bench where all nodes are −3…−25 dBm apart, so the client (board0)
would **peer the final destination directly** and appear to "grab the wrong key". That confounded the dual-side
key comparison (it showed board0 enc `6f1d…` vs board2 dec `7e58…` = an apparent MTK desync, which drove the
`plid-IGNR` / re-peer-desync theory — since **refuted**).

The confound was a **startup race**: the perf app called `mmwlan_mesh_set_peer_allowlist()` *after*
`mmwlan_mesh_start()`, leaving a window where the empty allowlist (count==0 ⇒ allow-all) let a neighbour peer
before the `{next-hop-only}` allowlist took effect. **Fix: hoist the allowlist config ahead of
`mmwlan_mesh_start`** (app_main.c). Verified on-air: after the fix board0's plink to the far node decays
(inactive → 21 s and climbing) and never re-forms — board0 peers **only** board2.

On the clean forced topology the dual-side CCMP capture became decisive:

| field | board0 (encrypt) | board2 (decrypt, failing frame) |
|---|---|---|
| key   | `36 bf d2 71 bf 7b 23 96` | `36 bf d2 71 bf 7b 23 96` — **identical** |
| nonce | `00 e272a1f8efa4 000000000001` | identical |
| AAD   | `88 **43** …` | `88 **47** …` — **only FC byte1 differs (moreFrag)** |

Key + nonce identical ⇒ **not a key desync**. The only difference is the moreFrag bit in the on-air FC.

## Elimination path (what was ruled out, and how)

1. **MTK desync / `plid-IGNR`** — REFUTED. Forced clean topology ⇒ board0 encrypt key == board2 decrypt key.
   The prior `6f1d`-vs-`7e58` was a confound (board0 had a chronite key to grab). The `plid-IGNR` guard stays
   as legit follow-Linux hardening but is unrelated to S3.
2. **Host sets moreFrag** — REFUTED. `construct_80211_data_header_mesh` builds FC clear; `frag_threshold`
   defaults to 0 (⇒ UINT32_MAX, no host frag); a grep for `0x0400`/moreFrag in the TX path finds nothing;
   `skbq.c` (`convert_tx_metadata_to_tx_info`) sets only metadata flags, never the 802.11 FC. So the FW sets it.
3. **A-MPDU-of-4addr interaction** — REFUTED by A/B. Suppressing A-MPDU eligibility for the multi-hop origin
   leg (`key_stad != stad`) left moreFrag set and the MIC still failing (`decrypt_ok=8/3344`). Fragmentation is
   rate/size-driven, independent of aggregation.
4. **Frame size alone** — REFUTED. Large *single-hop* (A1==A3) blast frames decrypt fine (99.4%, prior data);
   small multi-hop pings work. The discriminator between "works" and "fails" for the same-size frame is the
   **rate**, which tracks the dequeue stad: single-hop → trained peer rate; multi-hop → common_stad ≈ MCS0.

## The fix (code)

`components/halow/.../umac/datapath/umac_datapath.c`, `umac_datapath_process_tx_frame`:

```c
    /* aid + rate control follow the NEXT-HOP peer (key_stad), not the dequeue stad. ... Keeping them on
     * common_stad sent multi-hop frames at MCS0, where a full-size 4-addr frame exceeds the 1 MHz max PPDU
     * -> the FW FRAGMENTS it (sets moreFrag) after the host SW-CCMP MIC -> the next hop MIC-fails ~99.6%. */
    tx_metadata->aid = umac_sta_data_get_aid(key_stad);   /* was: stad */
    ...
        umac_rc_init_rate_table_data(key_stad, &tx_metadata->rc_data, rts_required, ...);  /* was: stad */
```

This completes the S2 "the frame follows the next hop" pattern (seqno/BA/reorder already keyed on `key_stad`
at :2209 / :2246 / :2314). Off the multi-hop mesh path `key_stad == stad`, so the change is a no-op for
single-hop, non-mesh (STA/AP) and multicast (multicast uses `rate_table_mgmt` regardless).

The relay forward path (`umac_datapath_tx_mesh_keyed_frame`, reached via
`umac_datapath_tx_mesh_unicast_frame(nh_stad, …)`) was already keyed on the next-hop peer, so its rate is
correct — no change needed there.

## Bench method / evidence

- Pure all-ESP forced line **board0 (client) → board2 (relay/DUT) → board1 (far)**, all three on a proper
  `make … BOARD=proto1-fgh100m` build (country **US**; chip 0x0306 / fw 1.17.8). ESP↔ESP peering was used
  because the cross-vendor ESP↔Linux (chronite) SAE would not re-peer this session.
- Instrumentation (temp, RTC-noinit so it survives the console-open warm reset): forward/RX-drop counters
  (`FWD-DBG` / `RX-DBG` / `TX-DBG`) + a dual-side CCMP key/AAD/nonce capture (`CCMP-ENC` on the encrypter,
  `CCMP-DEC` on a decrypt failure) dumped by the app at boot. RTC magic bumped each build because the dev-host
  USB hub's per-port `disable` cuts DATA only, **not VBUS** — board0/board1 stay powered across a "cycle" and
  can't be POWERON-reset to zero RTC, so a magic mismatch is the only reliable force-zero.
- **Confirmed healthy warm-reset**: opening the USB-JTAG console warm-resets these XIAO ESP32-S3 boards, and on
  a correct US build the MM6108 comes back up clean (chip 0x0306) — the prior "warm-reset-wedge / dead MM6108"
  was itself the `??`-country build bug, not a hardware wedge.

## Before / after (board2 RX-DBG, the relay's decrypt of board0's origin frames)

| | mesh_seen | sw_ccmp_fail (mic) | decrypt_ok | rate |
|---|---|---|---|---|
| before (RC on common_stad) | 2073 | 2064 (2053) | 9 | 0.4% |
| after (RC on key_stad)     | 1917 | 68 (63)     | 1849 | **96.5%** |

## Adversarial review (3-lens workflow, post-fix)

Confirmed the core fix closes the systematic drop with no blocking regression (rate/bandwidth, aid, CCMP key,
seqno, and BA/reorder all follow `key_stad`; the aid change also correctly routes tx-status RC feedback back to
`key_stad`, so init + feedback are consistent — and it means multi-hop-origin frames now actually *train* the
next hop's rate, which under the old aid=0 they did not). Refinements to the residual, worth a follow-up:

- **The residual ~3.5% is NOT only cold-start** — the mmrc rate chain `[best_tp, second_tp, best_prob, baseline]`
  hard-sets `baseline` (rates[3]) to **MCS0/1MHz** (`umac/rc/…mmrc.c` ~:638/:994), and `mmrc_get_rates` never
  excludes a rate that can't fit the frame in one PPDU (the `frame_size` arg only budgets attempt *time*). So on
  any retry-descent to the fallback, a full-size 4-addr frame still fragments. Fixing rates[0] (this fix) is
  necessary but not sufficient to reach 100%.
- **Latent 100%-loss edge**: if `umac_rc_start` early-returns (bandwidth/rate mask → 0), the peer's
  `reference_table` stays NULL and `umac_rc_init_rate_table_data` falls back to the mgmt table = MCS0 for the
  WHOLE chain (`umac/rc/umac_rc.c` ~:437). Not active here (96.5% ⇒ RC trained), but it would masquerade as this
  same bug.
- **Config guns**: `apply_rate_table_overrides` (fixed tx_rate) and `apply_mcs10_mode` (rewrites MCS0/1MHz → MCS10,
  which has *smaller* single-PPDU capacity than MCS0) can both re-introduce a fragmenting rate over the fix. Avoid
  a low fixed-rate override with full-size secured mesh.
- **Heterogeneous-mesh latent**: `umac_connection_populate_tx_metadata` (:2321) stamps `ampdu_mss`/MMSS/TP/1MHz-CR
  from a GLOBAL connection record, not `key_stad`. Harmless on the all-MM6108 bench (identical caps), but to fully
  mirror `tx->sta = next_hop` these four A-MPDU caps should also be per-next-hop-peer.

## Follow-ups (not blocking)

- **Deterministic residual fix (recommended)**: give full-size SW-CCMP mesh frames a **min-MCS floor** (exclude any
  chain rate whose single-PPDU capacity can't carry the assembled 4-addr + 6 B mesh-ctrl + CCMP MPDU at 1 MHz), or
  an MTU cap. The 802.11 fragmentation-threshold param does NOT help — this fragmentation is PHY/rate-driven (PPDU
  fit), not policy-driven; raising the threshold only adds host frag, never prevents the FW's PPDU-fit frag. The
  min-MCS floor is the throughput-preserving middle ground vs a full MCS0-safe MTU cap.
- **Rate-training transient (~3.5%)**: the first frames after a peer/rate reset still ride MCS0 (cold-start seed +
  the MCS0 fallback above) and fragment until the rate trains. Subsumed by the min-MCS floor.
- **S3 forward capacity**: board2 `FWD-DBG build_null=662` — the forwarded-frame allocation fails under the
  5 Mbit blast. Separate S3 item (the 1038 that allocated forwarded fine).
- **A-MPDU aggregation depth** on the now-flowing multi-hop path still needs a working on-air sniffer
  (chronite/chronium morse0 monitor delivered 0 frames again this session).
- All instrumentation here is temp/uncommitted; only the `umac_datapath.c` rate/aid change (+ the app
  allowlist-before-start hoist) are keepers.
