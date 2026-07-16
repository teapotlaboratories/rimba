# Worklog — 2026-07-11 — Mesh A-MPDU S2: multi-hop next-hop stad Block Ack

**Author:** Aldwin
**Phase:** 802.11s mesh throughput — A-MPDU aggregation (Stage 2, multi-hop origin)
**Goal:** make a **locally-originated, multi-hop** mesh unicast open its Block-Ack
session (and draw its sequence numbers) against the **next-hop peer**, so a multi-hop
origin becomes A-MPDU-eligible on its first-hop link — instead of self-addressing the
ADDBA on the mesh common stad (which can never complete).
**Status:** S2 **CODE DONE + core mechanism proven on-air**. On a fresh secured all-ESP
mesh, a multi-hop origin (board1→board2 via relay board0) now **completes a Block-Ack
handshake with its next hop board0** — captured on `morse0`: ADDBA-req board1→board0
followed 53 ms later by ADDBA-resp board0→board1, both **unprotected** (MFP=no). Before S2
the same flow emitted **zero** on-air ADDBA. **Sustained A-MPDU on the first-hop leg was
NOT achieved**, blocked by an **orthogonal HWMP path-flapping** issue (next hop alternates
board0/board2), not by the S2 change. The change is **uncommitted working-tree** in the
morselib submodule (one file, `umac_datapath.c`), intended for `feat/mesh-ampdu-s2` stacked
on S1; nothing on `main`.

This entry is **standalone**: the change, the Linux code-map, the exact frames/addresses,
and the bench gotchas needed to re-run it are all here. Staged plan + full code-map:
[`../mesh-ap/rimba-mesh-ampdu-aggregation-design.md`](../mesh-ap/rimba-mesh-ampdu-aggregation-design.md).

---

## 1. The gap S2 closes

S1 lit up **single-hop** mesh A-MPDU: a local-origin unicast to a **direct peer** opens a
Block-Ack (BA) session on that peer's stad, the ADDBA is addressed to the real peer, the
handshake completes, and the MM6108 aggregates. (See
[`2026-07-11-mesh-ampdu-s1-blockack-rx-routing.md`](2026-07-11-mesh-ampdu-s1-blockack-rx-routing.md).)

**Multi-hop local-origin was still broken.** When a node originates a unicast to a
destination that is **not** a direct peer (reached via a relay), the frame is dequeued on
the **mesh common stad** (`mesh_ctx.common_stad`), whose `peer_addr`/`bssid` are this
node's **own** mesh MAC. The BA machinery keys entirely off the stad it is handed:

- `umac_ba_session_init(stad)` stores the originator session **on that stad**
  (`umac_ba.c:390`, `data = umac_sta_data_get_ba(stad)`).
- `umac_ba_tx_addba_req(stad)` addresses the ADDBA to
  `umac_sta_data_peek_peer_addr(stad)` / `…_peek_bssid(stad)` (`umac_ba.c:267-268`).

On the common stad both are our own MAC, so the ADDBA **self-addresses** — it can never be
answered, `originator[tid].status` never reaches `UMAC_BA_SUCCESS`,
`umac_ba_is_ampdu_permitted` stays false, and every multi-hop frame goes out as a single
MPDU.

The datapath **already** resolves the correct next-hop peer stad a few lines earlier — for
CCMP keying (the multi-hop relay-decrypt fix): `umac_datapath.c` ~`:2164-2172`,
`key_stad = umac_mesh_get_peer_stad(ra)` where `ra` is the HWMP next hop. S2 makes the BA
decisions (and the sequence number) key off that same `key_stad`.

## 2. The fix — 4 edits, all "use key_stad off the multi-hop mesh path"

All in `umac_datapath_process_tx_frame` (`umac_datapath.c`). `key_stad == stad` for
single-hop / non-mesh / multicast / EAPOL, so every edit is a **no-op** except for a
multi-hop local-origin unicast with an ESTAB next hop.

| # | line | change | why |
|---|---|---|---|
| 1 | aggr_check call | `stad` → `key_stad` | open the originator BA session on the next hop, so the ADDBA is addressed to the real next hop, not self |
| 2 | `umac_ba_get_reorder_buffer_size` | `stad` → `key_stad` | read the FW reorder-window bound from the next hop's session |
| 3 | `umac_ba_is_ampdu_permitted` | `stad` → `key_stad` | gate `MMDRV_TX_FLAG_AMPDU_ENABLED` on the next hop's session state |
| 4 | seqno source (`sta_data`) | bind `sta_data` to `key_stad` (relocated from the top of the fn to just after the key_stad resolution) | draw the 802.11 QoS-data sequence number from the next-hop peer's per-TID counter |

**Edit #4 was NOT in the original design doc's S2 sketch** (which named only the three BA
lines). It was surfaced by an adversarial follow-Linux review and is **required for
correctness**, not cosmetic. The single-next-hop, unidirectional bench rig *masks* the bug
— but for any 2+-next-hop mesh, or single-hop-to-P mixed with multi-hop-via-P, it corrupts
A-MPDU reorder. `sta_data` is used exactly once in the function (the seqno stamp), so the
relocation is clean, and it mirrors the existing mid-function `key_stad` declaration.

### Follow-Linux (anchors re-grepped against chronite `~/halow/rpi-linux` net/mac80211)

net/mac80211 keys the QoS-data sequence number **and** BA aggregation on the **next-hop
sta**, not the final destination and not a per-vif counter:

- **Seqno** — `ieee80211_tx_h_sequence` stamps `hdr->seq_ctrl =
  ieee80211_tx_next_seq(tx->sta, tid)` (`tx.c:886`), and `ieee80211_tx_next_seq` returns
  from `sta->tid_seq[tid]` (`tx.c:807-815`). `tx->sta` is bound to the **RA/next hop**:
  `ieee80211_tx_prepare` does `tx->sta = sta_info_get(sdata, hdr->addr1)` when the sta arg
  is NULL for mesh (`tx.c:1239-1241`), and `hdr->addr1` was set to the next hop by
  `mesh_nexthop_lookup` (`mesh_hwmp.c:1385-1388`, `memcpy(hdr->addr1,
  next_hop->sta.addr, …)`). (`ieee80211_lookup_ra_sta` deliberately returns NULL for
  MESH_POINT — "determined much later", `tx.c:2497-2501`.)
- **Aggregation on next hop** — `ieee80211_rx_mesh_fast_forward` takes
  `sta = mpath->next_hop` (`rx.c:2788`) and runs `ieee80211_aggr_check(sdata, sta, skb)`
  (`rx.c:2805`) → `ieee80211_start_tx_ba_session(&sta->sta, tid, 0)`.
- **Mesh whitelisted for TX BA** — `ieee80211_start_tx_ba_session` accepts
  `NL80211_IFTYPE_MESH_POINT` (`agg-tx.c:641`).

So morselib now stamps the seq and opens the BA on `key_stad` = next hop — the exact
analog of Linux binding `tx->sta` to `mpath->next_hop`.

## 3. Bench A/B — fresh secured all-ESP mesh, chronium `morse0`

**Rig.** Forced line via per-node peer allowlists (`rimba-halow-mesh-perf`, `MESH_IPERF`):
board1 (`10.9.9.100`) — board0 (relay, `10.9.9.136`) — board2 (`10.9.9.108`); board1 and
board2 allow only board0, so a board1→board2 iperf is forced 2-hop via board0. Secured
mesh (SAE + PMF-policy + host SW-CCMP; DATA is CCMP, `fc.protected=1`). Monitor: chronium
`wlan1`→monitor, freq 5560 (S1G ch27), capture on `morse0`. A-MPDU detected by shared TSFT
(one PPDU) + consecutive `wlan.seq`. UDP 2 Mbit/s offered.

### 3.1 BEFORE (S1 build) — the two controls

| flow | throughput | board1 QoS-Data | aggregated | board1 ADDBA (cat 3) |
|---|---|---|---|---|
| **single-hop** board1→board0 (control) | 0.33 Mbps | 629 PPDUs (644 frames) | **15 PPDUs × 2 MPDU** (seq 202/203, 207/208, 210/211; RA=board0; `protected=1`) | **YES → board0** (real peer); req+resp, `protected=0` |
| **multi-hop** board1→board2 | 0.23 Mbps | 545 PPDUs (545 frames) | **0** | **NONE** (self-addressed on common_stad; never on air) |

The single-hop control proves the RF environment **can** aggregate in this window; the
multi-hop 0% is therefore the code (self-addressed BA on the common stad), not the RF.

### 3.2 AFTER (S2 build) — cold-start multi-hop, chronium capture `e.pcap`

**The core S2 behaviour is proven on air: the multi-hop origin completes a Block-Ack
handshake with its next hop.**

```
#428  t=13.971s  board1 → board0   ADDBA request   fc.protected=0
#436  t=14.023s  board0 → board1   ADDBA response   fc.protected=0    (+53 ms)
```

- board1's **first** BA action frame is addressed to **board0 (the next hop)**, not self —
  the S2 change. board0 answers 53 ms later → the originator session on board0 reaches
  `UMAC_BA_SUCCESS`. Both frames are **unprotected** (S1's mesh MFP=no).
- **Before S2 the same flow emitted zero on-air ADDBA** (§3.1). So S2 flips "no BA session
  possible for a multi-hop origin" → "BA session completes on the next hop." That is the
  stage's unlock.
- DATA stays **CCMP** (`fc.protected=1`) — SW-CCMP unchanged.

**But sustained A-MPDU on the first-hop leg = 0** (the RA=board0 subset: 319 QoS-Data, 0
aggregated). The cause is **not** S2 — see §4.

### 3.3 Throughput

Multi-hop board1→board2: **0.23 Mbps (before) → 0.30–0.35 Mbps (after)**, a modest lift
consistent with an established BA session but little actual aggregation (the queue rarely
holds consecutive next-hop frames — §4). This is *not* the multi-x A-MPDU win; that needs a
stable path (§4/§6).

## 4. Why sustained aggregation didn't happen — HWMP path-flapping (orthogonal to S2)

board1's on-air QoS-Data RA distribution (both the confounded and the clean runs):

```
   ~319   RA = board0  (e2:72:a1:f8:ef:a4)   ← correct next hop, aggregatable
   ~221   RA = board2  (e2:72:a1:f8:f0:08)   ← RA = the FINAL destination
```

board1 alternates between routing via board0 (RA=next-hop=board0) and emitting frames with
**RA = the final destination board2** (which board2 drops — its allowlist only accepts
board0). This split persisted even with board0 perfectly stable, so it is **not** a relay
outage — it is **HWMP path instability**: board1's mpath to board2 keeps flapping/expiring,
and when there is no resolved next hop the header builder falls back to RA=destination. This
matches the **known HWMP multi-path flapping bug** (no PREQ reply-dedup + per-reply SN),
recorded in `mesh-p6c-airtime-and-hwmp-flapping`.

Two consequences, both of which suppress aggregation independent of S2:

1. **Queue dilution.** The FW aggregates *consecutive same-RA* frames resident in the TX
   queue. With the queue interleaving board0-destined and board2-destined frames, board0
   frames are rarely adjacent → almost never packed into one A-MPDU.
2. **Interleaved seq spaces (by design, per edit #4).** RA=board0 frames draw board0's
   per-TID counter; RA=board2 frames draw the common-stad counter. Each subset is internally
   monotonic (correct), but they interleave on air — so even the board0 subset seldom
   presents a run of consecutive board0-seq frames back-to-back.

Add the standing **shallow-queue** limiter at ~0.3 Mbps (few frames resident when a PPDU is
built — even the clean single-hop control only reached 2-MPDU depth), and the first-hop leg
stays at depth 1.

**Net:** S2's job — open + complete the BA session on the next hop — is done and proven.
Turning that into a *sustained* multi-hop A-MPDU needs a **stable single next-hop path**,
which this all-in-RF-range forced-line rig cannot provide (board2 is in board1's RF range,
so HWMP keeps finding/losing a direct path). See §6.

## 5. Bench gotcha — do NOT open the relay's serial console mid-run

board0 (the relay) hit a **`Interrupt wdt timeout on CPU0`** panic — decoded to
`gpio_isr_loop` → `gpio_isr_register_on_core_static` / `esp_intr_alloc`
(ESP-IDF `esp_driver_gpio`), i.e. the **GPIO interrupt layer**, not morselib. It occurred
**after** a capture, coincident with opening board0's USB-serial console to (re)start its
iperf server; board1 — identical firmware, also under load — stayed rock-stable the whole
session. The XIAO ESP32-S3's native-USB DTR/RTS toggling on port-open very plausibly
glitched a GPIO the MM6108 driver services, storming the ISR.

**Mitigation that worked:** for a multi-hop test the relay needs **no console** (board2 is
the iperf server, board1 the client) — leave the relay's console closed after boot and it
stays up. This also **exonerates the S2 datapath change**: the crash is in the GPIO/ISR
layer, board1 runs the same code fine, and S2 touches only the host TX datapath.

## 6. Follow-ups

- **Clean sustained multi-hop A-MPDU** needs a **stable single next-hop path**. Two ways:
  (a) fix the HWMP path-flapping (PREQ reply-dedup) so board1 holds next_hop=board0; or
  (b) a genuinely out-of-RF-range far node (physical separation / TX-power) so no direct
  path competes; or (c) tighten the forced-topology harness so board1 cannot even form an
  mpath with next_hop=the destination. Any of these should let the (now-completing) BA
  session actually aggregate the first-hop leg.
- **S3 (relay onto the data path)** — the dominant relay-goodput win; independent of S2.
- The interrupt-WDT on the relay under load is worth a standalone look (GPIO/ISR servicing
  vs MM6108 IRQ under sustained forward load), but is not on the A-MPDU critical path.

## 7. Teardown (radio-silent)

- All three ESP boards reflashed to `rimba-hello` (radio-silent); chronium `wlan1` down.
- Evidence pcaps archived: `s2_a.pcap` (baseline multi-hop), `s2_b.pcap` (single-hop
  control), `s2_e.pcap` (S2 cold multi-hop — the 428/436 handshake).
- **Nothing committed.** S2 is an uncommitted one-file working-tree change
  (`umac_datapath.c`) on top of the S1 branch, intended for `feat/mesh-ampdu-s2`.
