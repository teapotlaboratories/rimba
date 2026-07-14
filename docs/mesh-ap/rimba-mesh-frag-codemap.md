# Mesh host-side 802.11 fragmentation (fragment-before-encrypt) — design + code-map

**Why.** The ESP mesh uses host software CCMP for 802.11s frames (the MM6108 HW crypto can't do the mesh
4-address per-peer keying). When a host-encrypted frame is larger than the single-PPDU limit at its TX rate,
the **MM6108 FW fragments it *after* the host computed the CCMP MIC over the whole (unfragmented) frame**,
setting moreFrag in the on-air FC. CCMP AAD covers FC (moreFrag is not masked), so the peer's MIC check
fails. The morse **Linux** driver calls this out explicitly — FW-fragmenting a host-built frame is "not
permitted by the 802.11 protocol" (`morse_driver/beacon.c:26-30`, `vendor_ie.c:18-22`) — and avoids it by
doing fragmentation in the **host** (mac80211) and gating FW fragmentation behind a `HW_FRAGMENT` capability
(`morse_driver/mac.c:6834-6837` sets `SUPPORTS_TX_FRAG` only if FW advertises it; `:7473-7478`).

The correct order (802.11 / mac80211) is **fragment → encrypt** on TX and **decrypt → defragment** on RX.
The RC-on-key_stad fix (committed) already eliminates the *systematic* MCS0 case; this is the proper fix for
the residual (any large mesh frame whose rate is low enough that it won't fit one PPDU).

## The mac80211 pattern (Linux, `/home/chronite/halow/rpi-linux`, verified by grep)

- **TX order** — `net/mac80211/tx.c:1856-1861`: handler list runs `ieee80211_tx_h_sequence` →
  `ieee80211_tx_h_fragment` (`tx.c:951`) → `ieee80211_tx_h_encrypt` (`tx.c:1045`). So the MSDU is split into
  MPDUs *before* encryption; the sequence number is stamped once, before fragmentation.
- **Fragment size** — `tx.c:897` `per_fragm = frag_threshold - hdrlen - FCS_LEN`. A **fixed byte threshold**
  (`wiphy->frag_threshold`), not rate-aware. Skipped (`DONTFRAG`, `tx.c:1275-1279`) for multicast, for
  `skb_len + FCS <= threshold`, and **for A-MPDU frames**.
- **Per-fragment header** — `tx.c:996-1022`: every fragment but the last gets `IEEE80211_FCTL_MOREFRAGS`;
  the last clears it; `fragnum` (0,1,2…) is OR'd into `seq_ctrl`'s frag subfield; the **seq number is
  identical across all fragments** (copied from the original header, `ieee80211_fragment` `tx.c:940`).
  Multi-rate retry is disabled for fragmented frames (`tx.c:1006-1010`).
- **Per-fragment CCMP** — `net/mac80211/wpa.c:498-513` iterates the fragment queue calling `ccmp_encrypt_skb`
  per fragment: each gets its **own** CCMP header (`wpa.c:464/483`), its **own** PN from a single monotonic
  per-key counter that ticks once per fragment (`wpa.c:474` `atomic64_inc_return(&key->conf.tx_pn)`), and its
  **own** MIC (`wpa.c:492`). N fragments burn N consecutive PNs.
- **RX order** — `net/mac80211/rx.c:4189-4191`: `ieee80211_rx_h_decrypt` → `ieee80211_rx_h_defragment` →
  mic_verify. Each fragment is decrypted individually first; `ieee80211_rx_h_defragment` (`rx.c:2247`)
  reassembles, requiring **step-of-1 PN continuity** across a frame's fragments (`rx.c:2352-2357`).
- **Mesh** — no iftype special-casing in the fragment path; a relay reassembles+decrypts then re-injects the
  **whole** frame into the TX chain (`rx.c:2970-2992`, `NEED_TXPROCESSING`), which re-fragments per next hop.
  Never forwards raw fragments.

## morselib mapping

**RX side — decrypt→defrag already correct + ONE small addition (reorder bypass for fragments).**
`umac_datapath_process_rx_data_frame_after_reorder` runs `umac_datapath_sw_ccmp_decrypt` (:736) **then**
`datapath_defrag()` (:794); `datapath_defrag.c` reassembles plaintext fragments per-TID (seq#, frag#,
is_protected). So each host-SW-CCMP fragment decrypts on its own, then the plaintext pieces reassemble —
exactly the mac80211 order. **Added:** the A-MPDU reorder (`umac_datapath_process_rx_data_frame` ~:1388) now
bypasses the reorder for a fragmented MPDU (`More Fragments || frag# != 0`), delivering it straight to
decrypt+defrag. Needed because our host fragmentation can emit fragments during the ADDBA-handshake window
(originator BA not yet SUCCESS, but the recipient already built a reorder buffer): the BA window advances per
MSDU sequence number, so a same-seq# `frag1+` would be dropped as "outdated". 802.11 makes A-MPDU +
fragmentation mutually exclusive, so fragments are never legitimately reordered. No-op for unfragmented frames.
(Bench follow-up: confirm `ccmp_is_valid`'s replay window doesn't false-drop the per-fragment consecutive PNs,
i.e. mac80211's step-of-1 PN chaining, `rx.c:2352`.)

**TX side — the work.** New static helper in umac_datapath.c:

```
static enum mmwlan_status umac_datapath_tx_fragment_and_encrypt(
    struct umac_data *umacd, struct umac_sta_data *key_stad,
    struct mmpktview *txbufview,     /* built, UNencrypted: [MAC hdr|QoS|mesh-ctrl|SNAP|payload] */
    uint32_t hdrlen,                 /* MAC hdr + QoS control (replicated per fragment) */
    enum umac_key_type key_type, int key_id,
    const struct mmdrv_tx_metadata *meta_tmpl); /* vif_id/aid/rc_data; NON-aggregated, no HW_ENC */
```
mirrors `ieee80211_fragment` + `ieee80211_tx_h_fragment` + `ieee80211_crypto_ccmp_encrypt`:
- `per_fragm = MESH_FRAG_MPDU_MAX(728) - 1 - hdrlen - DOT11_CCMP_HEADER_LEN(8) - MIC(8)` (FCS-less; the FW appends the FCS).
- `nfrags = ceil(body_len / per_fragm)`; body = txbufview past `hdrlen`.
- For i in 0..nfrags-1: `mmdrv_alloc_mmpkt_for_tx(DATA/MGMT class)` with CCMP head/tailroom; copy the MAC
  header, `DOT11_SEQUENCE_CONTROL_SET_FRAGMENT_NUMBER(sc, i)`, set/clear `DOT11_MASK_FC_MORE_FRAGMENTS`
  (i<last ⇒ set); append body chunk i; `umac_keys_increment_tx_seq(key_stad,key_id)` + `sw_ccmp_encrypt`
  (own PN/MIC); copy `meta_tmpl` (tid, key_idx, aid, rc_data) with **no** `MMDRV_TX_FLAG_AMPDU_ENABLED`;
  `mmdrv_tx_frame(frag, is_mgmt)`; on any failure release + `goto` cleanup. Release the original.

**Call sites** (both mesh SW-CCMP paths):
- Origin: `umac_datapath_process_tx_frame`, replacing the in-place `sw_ccmp_encrypt` at ~:2273 with:
  `if (!is_multicast && !AMPDU_ENABLED && mpdu_len > MESH_FRAG_MPDU_MAX) fragment; else encrypt-whole`.
  (Compute AMPDU-eligibility before the branch — hoist the `umac_ba_is_ampdu_permitted` read up.)
- Forward: `umac_datapath_tx_mesh_keyed_frame`, same gate before its `sw_ccmp_encrypt`.

**Trigger + threshold.** Fragment iff: mesh SW-CCMP frame **AND** not A-MPDU-eligible (mac80211 DONTFRAG for
A-MPDU — preserves aggregation) **AND** the FCS-less on-air frame length (hdr+body+CCMP+MIC — the FW appends
the FCS) **>=** `MESH_FRAG_MPDU_MAX` = **728** = `morse_driver/beacon.c` `DOT11AH_1MHZ_MCS0_MAX_BEACON_LENGTH`
(= 764−36; the driver rejects a 1 MHz beacon whose FCS-less `skb->len` >= this, so the FW re-fragments
frames >= it). Each fragment's FCS-less frame is kept strictly **< 728**. Fixed byte threshold, matching
mac80211. **NB** the FW limit is on the FCS-less host frame (`skb->len`), NOT the PSDU-with-FCS — an earlier
764 draft over-sized fragments so they'd be FW-re-fragmented (fix inert); an adversarial review caught it,
corrected to 728 FCS-less. A `nfrags <= 16` guard (the fragment number is a 4-bit field) drops an over-large
frame defensively (unreachable at the ~1514 B TX cap, per_fragm ~680 → ≤3 fragments).

## Code-map (new morselib ↔ Linux)

| morselib (new) | Linux mac80211 |
|---|---|
| `umac_datapath_tx_fragment_and_encrypt` split loop | `ieee80211_fragment` `tx.c:892-948` |
| per-fragment moreFrag/frag#/seq stamping | `ieee80211_tx_h_fragment` `tx.c:996-1022` |
| per-fragment `sw_ccmp_encrypt` (own PN/MIC) | `ieee80211_crypto_ccmp_encrypt`/`ccmp_encrypt_skb` `wpa.c:426-513` |
| DONTFRAG-for-A-MPDU + threshold gate | `tx.c:1275-1279` |
| RX `sw_ccmp_decrypt` (:736) → `datapath_defrag` (:794) | `rx.c:4189-4191` decrypt→defragment |
| `MESH_FRAG_MPDU_MAX = 728` (FCS-less) | `beacon.c` `DOT11AH_1MHZ_MCS0_MAX_BEACON_LENGTH (764-36)` |

## Open / follow-ups

- **Rate-aware threshold (optional refinement).** Fixed 764 fragments a large *non-A-MPDU* frame even at high
  rate (transient: only before a BA session is up, since A-MPDU frames are skipped). A rate-aware `per_fragm`
  (= bytes that fit the PPDU at `rc_data[0]`, anchored at 764@MCS0 via the mmrc data-rate table) would drop
  even that; deferred — mac80211 itself uses a fixed threshold.
- **A-MPDU frame that falls back to MCS0 on retry** is not fragmented (DONTFRAG) — same tiny residual mac80211
  has; nearly-negligible (last-retry frame). Rate-aware would also close this.
- Confirm on bench the RX defrag path decrypts+reassembles host-SW-CCMP fragments without a false replay-drop.
