# Worklog — 2026-07-11 — Mesh A-MPDU S1: Block Ack RX routing + mesh MFP=no

**Author:** Aldwin
**Phase:** 802.11s mesh throughput — A-MPDU aggregation (Stage 1, the unlock)
**Goal:** make a **single-hop** local-origin mesh A-MPDU work end-to-end by letting
the Block-Ack handshake complete on a mesh vif. After the S0 spike proved the MM6108
firmware *will* assemble on-air A-MPDUs for 4-address mesh QoS-data when the host marks
frames eligible, the only missing piece was the host-side BA session — and it could
never complete because inbound Block-Ack action frames were dropped on the mesh RX path.
**Status:** S1 **CODE DONE + on-air verified**. Single-hop secured-mesh A-MPDU lights up:
board1→board0 emits real A-MPDUs (consecutive-seq QoS-Data sharing a PPDU), driven by a
Block-Ack handshake that now completes. Two commits on branch
`feat/mesh-ampdu-s1-blockack-rx` (morselib inside `mm-esp32-halow`): `1d44edfa` (BLOCK_ACK
RX routing) + `7fdba0c9` (mesh MFP=no). rimba PR #33 / mm-esp32-halow PR #23, both DRAFT.
Nothing on `main`.

This entry is **standalone**: every mechanism, address, anchor and capture reading needed
to re-run and re-verify S1 is here. The full staged plan (S0–S5) and the Linux code-map
live in [`../mesh-ap/rimba-mesh-ampdu-aggregation-design.md`](../mesh-ap/rimba-mesh-ampdu-aggregation-design.md);
this worklog does not depend on it.

---

## 1. Where S0 left it — the handshake was the only gap

S0 (see [`2026-07-11-mesh-ampdu-s0-fw-capability-spike.md`](2026-07-11-mesh-ampdu-s0-fw-capability-spike.md))
proved two things on hardware:

- **S0a:** the firmware advertises `AMPDU_cap = 1` for a **mesh** vif, so the datapath's
  aggregation-eligibility gates are open.
- **S0b:** with the per-frame A-MPDU-eligible flag *force-set* (bypassing the BA
  handshake), the MM6108 really packs mesh QoS-data into multi-MPDU PPDUs on air, and it
  does so on **CCMP-encrypted** frames (host SW-CCMP composes with A-MPDU).

So the chip does the aggregation; the host only has to (1) run the Block-Ack session
handshake, (2) set the per-frame eligible flag, (3) reorder on RX. (2) and (3) already
exist and work for STA/AP. S1 is (1): make the handshake actually complete on a mesh vif.

## 2. Why the handshake couldn't complete — two RX drops on a secured mesh

A Block-Ack session is negotiated with ADDBA request/response action frames. The
originator side already fired an ADDBA on-air for a single-hop mesh peer today (the
aggregation-eligibility path reaches `umac_ba_session_init` for a peer stad), **but the
peer's ADDBA response never reached the BA state machine**, so the originator session
never flipped to `UMAC_BA_SUCCESS` and `umac_ba_is_ampdu_permitted` stayed false forever.
Two separate drops were in the way, both on the mesh RX path:

1. **Routing drop.** On a mesh vif, ACTION frames go to `umac_mesh_handle_action`
   (`umac_mesh.c`), which handles only `DOT11_CATEGORY_MESH` (HWMP path selection) and
   `DOT11_CATEGORY_SELF_PROTECTED` (MPM/AMPE peering) and `return`s for everything else.
   The generic dispatcher that routes `DOT11_ACTION_CATEGORY_BLOCK_ACK` →
   `umac_ba_process_rx_frame` is on the STA/AP path and was **never reached for a mesh
   vif**. So ADDBA req/resp and DELBA were silently dropped.

2. **Robust-management drop (secured mesh only).** Block-Ack is a *robust* action
   category. The morselib mesh set every peer stad `MMWLAN_PMF_REQUIRED`, so
   `pmf_is_required()` was true and the unprotected-robust-management RX gate
   (`umac_datapath.c`, ~`:373`) dropped the peer's ADDBA before routing could even matter
   — because a MFP=no Linux/ESP peer *sends* ADDBA in the clear.

The second is the more interesting one: it was a **protection asymmetry**. morselib was
transmitting robust mesh management (ADDBA/DELBA, unicast PREP) **CCMP-protected**, while
net/mac80211 runs mesh peers **MFP=no** and sends the same frames **unprotected** — a
cross-implementation mismatch that also breaks interop, not just A-MPDU.

## 3. The fix — 2 edits, follow-Linux

### 3.1 Route BLOCK_ACK action RX to the BA state machine (`1d44edfa`)

In `umac_datapath_process_rx_mgmt_frame_mesh` (`umac_datapath.c`), before handing an
ACTION frame to `umac_mesh_handle_action`, peel off `DOT11_ACTION_CATEGORY_BLOCK_ACK` and
dispatch it to `umac_ba_process_rx_frame`, **keyed on the transmitting peer's stad**
resolved from the frame's SA (A2 = the transmitting neighbour):

```c
if (subtype == DOT11_FC_SUBTYPE_ACTION)
{
    const struct dot11_action *action =
        (const struct dot11_action *)mmpkt_get_data_start(rxbufview);
    if (action->field.category == DOT11_ACTION_CATEGORY_BLOCK_ACK)
    {
        struct umac_sta_data *peer_stad = umac_mesh_get_peer_stad(dot11_get_sa(header));
        if (peer_stad != NULL)
        {
            umac_ba_process_rx_frame(peer_stad, mmpkt_get_data_start(rxbufview),
                                     mmpkt_get_data_length(rxbufview));
        }
        return;
    }
    umac_mesh_handle_action(umacd, rxbufview);
}
```

The BA agreement is **per-link**, so the stad is resolved per-frame from the SA (not the
mesh common stad passed in). ADDBA req **and** resp now take this same path (an earlier
draft split req→mesh/resp→sta and had an asymmetry; both go through `_mesh` now).

**Follow-Linux:** `net/mac80211/rx.c` `ieee80211_rx_h_action` `case WLAN_CATEGORY_BACK`
(reached for a `MESH_POINT` vif) → `ieee80211_process_addba_request` /
`…_resp` / `…_delba`.

### 3.2 Mesh MFP=no — send Block Ack / PREP unprotected (`7fdba0c9`)

Root-fix the protection asymmetry: set the mesh peer stads **and** the common stad
`MMWLAN_PMF_DISABLED` instead of `MMWLAN_PMF_REQUIRED`, in `mesh_peer_alloc` and
`mmwlan_mesh_start` (`umac_mesh.c`). This is guarded by `MMWLAN_MESH_SEC_PHASE1`:

```c
umac_sta_data_set_security(p->stad, MMWLAN_SAE, MMWLAN_PMF_DISABLED);   /* was PMF_REQUIRED */
```

`security_type` stays **SAE**, so the pairwise/group keys are still installed and **DATA
stays CCMP-encrypted**. Only `pmf_is_required()` flips to false, which does two things at
once:

- **TX:** all mesh management (HWMP PREQ/PREP, MPM/AMPE, Block-Ack ADDBA/DELBA) is now
  sent **in the clear**, matching the peer.
- **RX:** `pmf_is_required()` false ⇒ the unprotected-robust-management drop is skipped
  *wholesale* — which makes the interim per-category exemption (`frame_is_block_ack_action`)
  **unnecessary, so it was removed**. Net S1 = routing + MFP=no, two files.

**Follow-Linux:** a mesh peer's `sta` is MFP=no (`iw station dump` → `MFP: no`; the Linux
mesh has no `ieee80211w`), so `net/mac80211/tx.c` protects management only for
`WLAN_STA_MFP` and `ieee80211_drop_unencrypted_mgmt` is likewise gated on `WLAN_STA_MFP`.
morselib now matches: mesh management unprotected, data CCMP.

## 4. On-air verification (secured all-ESP mesh, chronium `morse0`)

**Rig.** Two ESP32-S3 + MM6108 boards as direct secured-mesh peers on S1G ch27
(915.5 MHz); chronium (Pi 5) as the passive monitor (`wlan1`→monitor, freq 5560, capture
on `morse0`). The mesh is **secured** — the perf app requests `MMWLAN_OPEN` but morselib
forces SAE+PMF-policy+SW-CCMP via compile-time `MMWLAN_MESH_SEC_PHASE1=1` (SAE pw
`rimbamesh2026`). A-MPDU has no radiotap field on `morse0`, so aggregation is detected by
**shared TSFT (one PPDU) + consecutive `wlan.seq`** (the standing method from S0).

**Result (verified 2026-07-11, and re-confirmed this session on a fresh bench,
board1→board0 direct, `10.9.9.100`→`10.9.9.136`, UDP 2 Mbit/s × 14 s):**

| signal | before S1 | after S1 |
|---|---|---|
| board1 Block-Ack action frames on air | none complete (resp dropped) | **ADDBA req+resp exchanged** (category 3), `fc.protected = 0` |
| board1 QoS-Data aggregation | single MPDUs | **A-MPDU** — QoS-Data with consecutive `wlan.seq` sharing one PPDU TSFT (e.g. 202/203, 207/208, 210/211) |
| DATA protection | CCMP | **CCMP unchanged** (`fc.protected = 1`) |
| SAE/AMPE peering + ping | ok | ok (5/5) |

Fresh-session capture readings (this session, positive control): of board1's 644 QoS-Data
frames, **15 PPDUs carried 2 aggregated MPDUs** with consecutive seq, RA = board0
(`e2:72:a1:f8:ef:a4`), `fc.protected = 1`; **4 Block-Ack action frames** (category 3),
all `fc.protected = 0` (MFP=no confirmed), addressed between board1 and its real peer
board0. So the handshake completes and the chip aggregates the encrypted MPDUs — exactly
the S0b behaviour, now driven by the real BA session rather than a force.

**The MFP=no half is directly visible on air:** before the fix, tshark showed the ADDBA as
an Action frame with an **empty category** (encrypted body); after, it decodes as
**category 3, `fc.protected = 0`** — the frame is sent in the clear — while DATA stays
`protected = 1`. The mesh HWMP path-selection frames (PREQ/PREP) are likewise
`protected = 0` in capture, the same MFP=no change.

## 5. Bench gotcha — aggregation depth is environmental; A/B in one window

A-MPDU aggregation **depth** is RF/thermal-sensitive and degrades over a long bench
session (hours + many reflashes). A first MFP-build run looked like a regression
(217→13/0 aggregated PPDUs) but an **A/B in the same RF window** showed the *prior*
protected-ADDBA build degraded identically (217→~34) — so it was **environmental, not the
fix**. **Rule:** always A/B a depth drop against the prior build in the same window before
calling it a regression; treat the *presence* of aggregation + the *ADDBA addressing/
protection* as the robust signal, and depth as the noisy one.

## 6. Follow-ups

- **ESP↔Linux BA interop capture — ✅ RESOLVED 2026-07-12** (fresh, rebooted bench). The prior
  "peering won't form / board0 never heard chronite's beacons" was a **mesh-ID mismatch**: the ESP
  beacons `MESH_ID = "rimba-mesh"` but the Linux reference config used `ssid = "rimba-smesh"` (the
  *all-Linux* secured mesh), so board0 dropped chronite's beacons as a foreign mesh — **not** an
  S1G-param or MFP issue. With chronite put on `ssid = "rimba-mesh"` (SAE, ch27, same pw), board0
  (ESP, MFP=no) peered with chronite (Linux) — SAE+AMPE ESTAB, ping 45/45, and chronite's
  `station dump` shows the board0 peer `mesh plink: ESTAB, MFP: no`. **The unprotected-ADDBA handshake
  was captured on `morse0`:** board0 → chronite **NDP ADDBA Request** (dialog token 0x69,
  `fc.protected=0`), chronite → board0 **NDP ADDBA Response** (matching token 0x69, `fc.protected=0`)
  +10 ms — i.e. **a live Linux mesh peer accepts the ESP's now-unprotected ADDBA**, validating the
  MFP=no fix cross-vendor. Both sides use **NDP ADDBA** (settles the design doc's "NDP vs compressed
  BlockAck" question). Under iperf load board0 aggregates CCMP QoS-Data to chronite (~1.8 Mbit/s, deep
  A-MPDU) — SW-CCMP + A-MPDU compose against a Linux peer too. (Rig: interop build = `MESH_LINUX_INTEROP`
  temp toggle in `rimba-halow-mesh-perf`, open peering + auto-ping chronite; chronite Linux mesh via
  `wpa_supplicant_s1g` with `ssid="rimba-mesh"`.)
- **S2 (multi-hop next-hop stad)** and **S3 (relay onto the data path)** — the remaining
  aggregation stages; see the design doc.

## 7. Teardown (radio-silent)

- Per the standing rule, after the bench runs: all ESP boards reflashed to `rimba-hello`
  (radio-silent) and chronium `wlan1` down.
- Nothing committed to `main`; S1 lives on the two draft PR branches.
