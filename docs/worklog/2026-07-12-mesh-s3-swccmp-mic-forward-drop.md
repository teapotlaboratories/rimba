# Worklog — 2026-07-12 — S3 relay forward drop root-cause: host SW-CCMP MIC failure

**Author:** Aldwin
**Phase:** 802.11s mesh throughput — A-MPDU (S3 relay forward-leg) — root-causing the ~99% forward-frame drop
**Goal:** The ESP relay (board2) drops ~99% of forward-intended mesh data frames at the host SW-CCMP decrypt
before they reach the forward call, so the relay forwards ~0 sustained data and S3's forward-leg A-MPDU can't
be measured. Pin the exact cause and fix it, following Linux.
**Status:** **IN PROGRESS.** Bucket named (host SW-CCMP decrypt) and split (MIC failure, not replay). Root cause
of the MIC failure not yet pinned; leading hypothesis = A-MPDU + SW-CCMP composition on the S2 origin leg.

This entry is **standalone** and is updated **as the investigation proceeds** (per `.ai/AGENTS.md` → Worklogs).

---

## 0. TL;DR (so far)

- The read that was blocked for several sessions is done. On a `board0 → board2(relay) → chronite` UDP blast,
  board2's RTC `RX-DBG` shows the ~99% early-host-RX drop is **host SW-CCMP decrypt failure**, and the split
  counter pins it as a **CCMP MIC failure, not replay**:
  `mesh_seen=2084 sw_ccmp_fail=2058 (mic=2052 replay=0) no_decrypt=0 decrypt_ok=26`.
- MIC failure ⇒ **key / AAD / nonce mismatch** between what board0 encrypted and what board2 decrypts.
  `decrypt_ok=26` (a few pass) ⇒ leading hypothesis: the passing few are **non-aggregated singletons**, the
  failing ~2052 are **A-MPDU-aggregated** multi-hop-origin frames — i.e. **SW-CCMP does not compose with FW
  A-MPDU aggregation on the S2 origin leg** (board0→board2).
- Open: pin which header field / key differs (code AAD compare), then an origin-aggregation on/off A/B.

## 1. The measurement + rig

- **Rig:** `board0` (client, UDP iperf auto-blast → 10.9.9.2, temp `g_iperf_blast_target` + static ARP so no
  ESP console is needed) → `board2` (relay/DUT, the only fully-wired ESP) → `chronite` (Linux far endpoint,
  `rimba-mesh`, SAE, ch27). board2 RTC-noinit counters survive the read's warm reset.
- **Split instrumentation (temp, in-tree):** `FDBG_RX_SW_MIC_FAIL` / `FDBG_RX_SW_REPLAY_FAIL` incremented at the
  two `return false` points inside `umac_datapath_sw_ccmp_decrypt` (`umac_datapath.c`): the `mesh_ccmp_decrypt(...)
  != 0` MIC-failure return, and the `!ccmp_is_valid(...)` replay return. App `dump_fwd_dbg` prints `(mic=.. replay=..)`.
- **Result (2026-07-12):** `mic=2052 replay=0` of `sw_ccmp_fail=2058`, `mesh_seen=2084`, `decrypt_ok=26`. So it
  is unambiguously a MIC failure. (An earlier rate-dependence argument had guessed *replay*; the measurement
  overturned it — logged as a refuted hypothesis rather than trusted.)

## 2. Bench-harness gotchas (so the next run is faster)

- **The forced 2-hop is NON-DETERMINISTIC.** All nodes are in RF range; board0 sometimes peers chronite
  *directly* and HWMP routes direct → board2 sees ~4 frames instead of ~2000. Root: the app sets board0's peer
  allowlist AFTER mesh peering can already begin (startup race), so an early chronite peer slips through before
  the allowlist applies. Workaround: re-boot board0 and retry until board2 `mesh_seen ≈ 2000`; proper fix = set
  the allowlist before `mmwlan_mesh_start`. (The peer-*creation* paths are all gated on the allowlist already —
  beacon-open `umac_mesh.c:1673`, inbound-OPEN dropped on secured mesh, inbound-SAE dropped if no peer — so a
  `mesh_peer_alloc` allowlist gate is redundant *once the allowlist is set*; the hole is purely the timing.)
- **chronite (Pi5) reboots mid-blast**, wiping `/tmp` (tmpfs) including `wpa-interop.conf` → the mesh won't come
  back until the config is re-pushed. Re-push it and bring wlan1 up again.

## 3. Investigation log

*(appended as it happens)*

- **2026-07-12 — split done, MIC confirmed (§1).**
- **2026-07-12 — code AAD/flag review: the encrypt & decrypt paths are SYMMETRIC; code largely exonerated.**
  - `mesh_ccmp_encrypt` and `mesh_ccmp_decrypt` (`ccmp.c:290`/`:304`) both call the SAME
    `mesh_ccmp_aad_nonce(hdr, ccmp_hdr, …)` (`ccmp.c:212`) — a faithful port of hostap `wlantest/ccmp.c`
    `ccmp_aad_nonce`. So AAD+nonce are built identically on both sides given the same `hdr`/`ccmp_hdr`.
  - The AAD **masks out every volatile field**: FC subtype/retry/pwr-mgmt/more-data bits (`ccmp.c:226`/`:239`),
    the 802.11 sequence number (`:248`, keeps only Frag#), and QoS ack-policy/EOSP/A-MSDU bits (`:257-260`,
    keeps only the TID). So a normal FW header rewrite during A-MPDU framing (seqno, ack policy) does NOT
    change the AAD. Unmasked/kept: FC-normalized, A1/A2/A3, A4 (4-addr), TID, and the PN (from ccmp_hdr).
  - TX crypto flags are correct: a mesh SW-CCMP frame gets `MMDRV_TX_FLAG_AMPDU_ENABLED` (`umac_datapath.c:2306`)
    but NOT `MMDRV_TX_FLAG_HW_ENC` (only the else/HW branch sets it, `:2291`) — so the FW aggregates the
    already-host-encrypted frame without re-crypting it. Correct SW-CCMP + A-MPDU combination.
  - The key board0 uses (key_stad=board2 for a forward) is the SAME board0↔board2 pairwise key that decrypts
    the **working single-hop** case. So neither the key, the AAD recipe, nor the flags obviously differ from the
    single-hop path that works. ⇒ the MIC failure is empirical (on-air header/body altered, or a key epoch/churn
    during the estab=5/close=1 peering churn seen in this run), not a static code asymmetry I can see.
- **2026-07-12 — NEXT (decisive, cheap, non-flaky): single-hop A/B.** Point board0's auto-blast at **board2's
  own IP (10.9.9.108)** instead of chronite → board0→board2 DIRECT (single-hop, board2 = dest, still aggregates
  per S1), NO chronite, NO 2-hop non-determinism. Read board2 RX-DBG:
  - `decrypt_ok` high ⇒ single-hop SW-CCMP+A-MPDU decrypts fine ⇒ the MIC failure is **specific to the A4≠TA
    multi-hop forward** (the only AAD delta is A3=DA=chronite≠A1) — chase board0's multi-hop-origin header build.
  - `mic_fail` high even single-hop ⇒ SW-CCMP+A-MPDU don't compose at all under this load (contradicts the S1
    ~1.8 Mbps result) ⇒ a load/aggregation-depth interaction.
- **2026-07-12 — SINGLE-HOP A/B RESULT: single-hop decrypts fine; the bug is A4≠TA-forward-specific.**
  board0→board2 DIRECT (board2=dest), same 5 Mbit blast: board2 RX-DBG
  `mesh_seen=3315 sw_ccmp_fail=23 (mic=21 replay=0) decrypt_ok=3292` = **99.4% decrypt success** (vs 1.2% for
  the multi-hop forward). `PLINK-DBG estab=1 close=0` (stable peering) ⇒ the earlier peering-churn idea is
  REFUTED (stable + works single-hop). So SW-CCMP + A-MPDU compose fine; the ~99% MIC failure is **specific to
  the multi-hop FORWARD frame (A4≠TA, A3=DA≠A1=RA)**. The AAD includes A1/A2/A3/A4, all symmetric, so board0
  must be putting a different A3 (or address) on-air than it encrypted the AAD over — a header-build-vs-encrypt
  inconsistency on the multi-hop-origin path (or the mesh layer rewrites an address after encrypt). **NEXT:**
  trace where the 4-addr mesh local-origin header sets A3(DA)/A4(SA)/A1(RA) relative to sw_ccmp_encrypt; and/or
  capture board0's on-air TX header + MIC and diff vs what it encrypted (chronium morse0 monitor, if working).
- **2026-07-12 — header build traced; host TX chain EXONERATED.** `umac_datapath_construct_80211_data_header_mesh`
  (`umac_datapath.c:3673`) builds the 4-addr mesh header BEFORE `process_tx_frame` encrypts, with
  **A1=next_hop (board2), A2=us (board0), A3=DA (chronite), A4=SA (board0)** (`:3701`/`:3702`/`:3703`/`:3706`) —
  correct, and no post-encrypt rewrite (the `addr1=bssid` clobber at `:2030` is in the *STA* builder
  `..._header_sta`, not the mesh path). CCMP-header offset is identical single vs multi (both are 4-addr QoS,
  TO_DS+FROM_DS both set, `:3691`), and the Mesh Control header is inside the encrypted body (not the AAD, and
  after the CCMP header) so its size doesn't shift the ccmp_hdr offset. So the whole host TX chain — header
  addresses, AAD/nonce recipe, `AMPDU_ENABLED` without `HW_ENC`, and the key — is correct and symmetric, and is
  the SAME chain that decrypts 99.4% single-hop. **Conclusion:** the MIC failure is NOT a visible host-code
  asymmetry; board0's ON-AIR multi-hop frame must differ from the bytes it encrypted the AAD over — an FW-level
  interaction between the multi-hop-origin 4-address forward and A-MPDU aggregation. **DECISIVE NEXT STEP
  (bench, gated on a working sniffer):** capture board0's on-air multi-hop TX on chronium `morse0`, decode the
  802.11 header + CCMP header/PN, and byte-diff vs a single-hop frame from the same board0 — the differing field
  is the cause. (The monitor delivered 0 frames in recent sessions; needs re-setup first.)

## 4. Status / summary

- **Answered:** the S3 relay's ~99% forward-frame drop is a host SW-CCMP **MIC failure** (mic=2052, replay=0),
  and it is **specific to the multi-hop A4≠TA forward** — single-hop SW-CCMP+A-MPDU decrypts 99.4% fine.
- **Exonerated (static + single-hop A/B):** the host TX header build, AAD/nonce recipe, crypto flags, and key.
- **Open:** the exact differing on-air field — needs a working `morse0` sniffer to capture + byte-diff board0's
  multi-hop vs single-hop TX. Likely an FW multi-hop-4-addr + A-MPDU interaction, not host code.
- **Bench:** radio-silent (all ESPs → rimba-hello, chronite wlan1 down). Temp instrumentation in-tree
  (MIC/replay split, auto-blast rig with single-hop target). Nothing committed.

## 5. On-air capture (the decisive step) — in progress

- **Monitor revived.** chronium is the dedicated sniffer (`CONFIG_MORSE_MONITOR=y`, Pi5 reset-patched).
  Recipe (`docs/reference/rimba-linux-halow-monitor.md`): `wlan1` → `type monitor`, `freq 5560` (S1G ch27),
  `morse0` up, read with `~/halow-mon.py <secs> <ta_prefix>` (AF_PACKET raw radiotap). Verified tuned:
  `iw dev wlan1 info` → `type monitor`, `channel 112 (5560 MHz)`; `morse_cli -i wlan1 channel` → 915500 kHz.
  (Config gotcha: bring wlan1 up/tune in separate steps — a combined pkill+down+type+up+freq script hung the
  driver; step-by-step worked.)
- **Rig:** board0 (multi-hop blaster → chronite 10.9.9.2 via board2), board2 (relay), chronite (mesh dest,
  10.9.9.2), chronium (passive sniffer). Capturing board0's TA (`e2:72:a1:f8:ef:a4`) frames for ~90 s over the
  blast. Looking for board0's DATA-4addr forward frames (A1=RA=board2, A2=board0, A3=DA=chronite, A4=board0)
  and decoding the QoS control + CCMP header (PN) to spot the on-air anomaly vs the working single-hop frame.
- **CAPTURE RESULT: board0's multi-hop frames are ON-AIR VALID.** 920 DATA-4addr frames, all with
  **A1=board2 (RA), A2=board0 (TA), A3=chronite (DA), A4=board0 (SA)** — confirmed multi-hop, no direct.
  QoS ctrl = `00 01` on all (TID 0 + Mesh Control Present bit ✓). CCMP header e.g. `c5 04 00 20 00 00 00 00`
  = PN 0x04c5, keyid 0, ExtIV set — **PN present + monotonically incrementing** (0x04c5→0x04da…). So the
  "blank/duplicate-PN" idea is REFUTED for the first instance of each PN; there ARE retransmit duplicates
  (e.g. 0x04da×3) but those would be *replay* fails and replay=0, so the MIC failures are not the duplicates.
  ⇒ **the on-air header + CCMP look fully decryptable** — the mismatch is invisible on the wire (in what
  board0 encrypted the AAD over, or the key), not a malformed frame.
- **LEADING ROOT-CAUSE HYPOTHESIS — per-link MTK key mis-selection.** In AMPE mesh each peer LINK has its own
  MTK (board0↔board2 key ≠ board0↔chronite key; see the comment at `umac_datapath.c:3606` "the MTK is per
  *link* (next hop), not per final DA"). The TX `stad` is resolved by the DEST (chronite); S2 overrides the
  crypto key to the NEXT-HOP via `key_stad = umac_mesh_get_peer_stad(ra)` (`:2182`) and encrypts with it
  (`sw_ccmp_encrypt(key_stad,…)` `:2281`, key_id from key_stad `:2206`). **If key_stad ever fails to override
  to the next-hop (board2) and stays = stad (chronite), board0 encrypts with the board0↔chronite MTK while
  board2 decrypts with the board0↔board2 MTK → MIC fail — 99% multi-hop, 0% single-hop (where dest==next-hop
  so the key is right regardless).** This exactly matches the single-hop-works / multi-hop-fails result.
  The static code *looks* correct (ra=A1=board2 from the header builder, get_peer_stad(board2) should resolve),
  and the on-air A1=board2, so the discrepancy is hidden — needs instrumentation to confirm which key was used.
- **DECISIVE NEXT STEP:** instrument board0's `process_tx_frame` for a mesh multi-hop unicast to record whether
  `key_stad` overrode to the next-hop or fell back to the dest stad (e.g. RTC counters: `nh_override` vs
  `nh_fallback`, plus the key_stad-vs-stad peer MAC), OR dump the 16-byte MTK board0 used and manually AES-CCM-
  decrypt a captured frame under both candidate link keys. Whichever key gives a valid MIC pins it. If it's the
  fallback, the fix is on the S2 key_stad path (ensure the next-hop MTK is always used for a multi-hop origin).
- **2026-07-12 — key_stad CONFIRMATION RUN: the per-link-MTK hypothesis is REFUTED.** Temp RTC counters in
  board0's `process_tx_frame` (`FDBG_TX_MULTIHOP`/`NH_NULL`/`NH_EQ_STAD`/`NH_OK`) recorded, per mesh multi-hop
  unicast, whether `key_stad` overrode to a distinct next-hop peer stad. board0 RTC after a 2297-frame
  multi-hop blast: `multihop=2297 nh_null=30 nh_eq_stad=0 nh_ok=2267`. **`nh_ok = 2267 (98.7%)`** — board0
  encrypts ~99% of multi-hop frames under the next-hop (board2) link key, the SAME key that decrypts single-hop
  at 99.4%; only 30 (1.3%) fell back — far too few to explain the ~2052 MIC failures. **NOT the key selection.**
  (Instrumentation gotcha: board0's VBUS cold-cycle doesn't reliably register as `ESP_RST_POWERON`, so RTC-noinit
  counters read stale garbage after `FDBG_COUNT` grew — fixed by bumping `MESH_FWD_DBG_MAGIC` to force a zero on
  a magic mismatch; board2's PPK2 power-off drains RTC cleanly, so its counters were fine throughout.)
- **Where it stands:** same correct key both sides, symmetric AAD recipe, on-air header valid — yet the A4≠TA
  multi-hop forward MIC-fails while single-hop passes. The only AAD delta is A3=DA, and board0's encrypt-A3 ==
  on-air A3 == board2's decrypt-A3 (all chronite) *should* match. A hidden discrepancy remains. **DECISIVE NEXT
  STEP:** dual-side byte dump — log the exact AAD+key+nonce+MIC board0 uses to ENCRYPT a multi-hop frame (matched
  by PN) and that board2 uses to DECRYPT the same PN; diff them. The first differing byte pins it (prime
  candidate: how each side handles A3 for an A3≠A1 forward).
- **2026-07-12 — ROOT CAUSE FOUND (dual-side CCMP dump): KEY MISMATCH.** First verified the crypto follows
  Linux (chronite `~/halow/rpi-linux`): the AAD recipe (`mesh_ccmp_aad_nonce` == mac80211 `ccmp_gcmp_aad`,
  wpa.c:318) and the mesh header (`construct_80211_data_header_mesh` == `ieee80211_fill_mesh_addresses`,
  mesh.c:851: A1=next-hop/A2=SA/A3=DA/A4=SA) both match. Then captured board0's ENCRYPT vs board2's DECRYPT of
  the unicast multi-hop frame (temp `mmwlan_ccmp_dbg_capture`, filter fixed to unicast A1 — a first pass caught
  group frames, incidentally showing chronite's GROUP frames also MIC-fail at board2):
  `CCMP-ENC key=6f 1d 9d 9c 48 af d6 49`, `CCMP-DEC key=91 bd e3 64 a0 a4 40 5a`. **The AAD (masked-FC + A1
  board2 + A2 board0 + A3 chronite + A4 board0 + QoS) MATCHES on both sides; the KEY does NOT.** So board0
  encrypts under one key and board2 decrypts under another — the **board0↔board2 pairwise MTK is not
  synchronized** → MIC never verifies → the ~99% forward drop. This OVERTURNS the earlier "same correct key
  both sides" assumption (that was inferred from the correct key_stad *selection*; the actual key BYTES differ).
  The AES-CCM is fine (RFC-3610 KAT passes; single-hop = 99.4% where the same peering had synced keys).
- **Leading mechanism (to confirm):** single-hop A/B had STABLE peering (`close=0`) → keys in sync (99.4%);
  the multi-hop runs had CHURN (`estab` climbing / `close>0`). AMPE re-derives the MTK on each re-peer, so a
  re-SAE/re-AMPE that installs the new MTK **asymmetrically** desyncs the pairwise key. This ties the forward
  drop back to the peering instability that opened this thread — the plink-liveness fix (fewer re-peers) should
  reduce it, but the real fix is a **symmetric AMPE MTK install/rekey** (follow hostap `mesh_rsn`/`wpa_auth`
  key-install ordering). NEXT: confirm which key `6f 1d 9d…` is (board0's own board2-MTK vs a stale/other-peer
  MTK) and whether it changes across a re-key, then fix the rekey path.
