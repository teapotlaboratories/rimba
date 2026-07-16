# 2026-07-14 — Mesh SW-CCMP defrag-before-decrypt: bench-VERIFIED (the proper large-frame fix)

**Status: PASS.** Large secured (SW-CCMP) 802.11s mesh frames now traverse the mesh — single-hop,
multi-hop relay, and both directions — with the MM6108 FW fragmenting them and the host reassembling
before decrypt. Two real bugs were found on-bench and fixed. All-ESP rig, radio-silent afterward.

## Problem

A mesh SW-CCMP unicast frame larger than the 1 MHz / MCS0 single-PPDU limit is **fragmented by the
MM6108 firmware AFTER** the host computed the one CCMP MIC over the whole (unfragmented) frame. The FW
replicates the MAC header per fragment but leaves the CCMP header only on fragment 0 and the MIC only
on the last fragment (**encrypt → fragment**). So each fragment is **not independently decryptable**,
and the existing RX order (decrypt-then-defrag, correct for host-side *fragment → encrypt*) drops them.

Two candidate fixes were considered:
- **Host fragmentation (fragment → encrypt), committed as `9c7daabd`** — REVERTED. The FW re-headers a
  host-built fragmented frame (deemed "not permitted by the 802.11 protocol", per the morse Linux
  driver), breaking the per-fragment MIC. See `2026-07-14-mesh-frag-bench-verify.md`.
- **Defrag-before-decrypt (this worklog)** — let the FW fragment (encrypt → fragment), then on RX
  **reassemble the raw still-encrypted fragments and decrypt the whole once.** This is the fix that works.

## The fix (all in `components/halow` `.../umac/datapath/umac_datapath.c`)

1. **Reorder bypass** (`umac_datapath_process_rx_data_frame`): a fragment (MoreFragments set, or
   fragment number ≠ 0) skips the A-MPDU reorder buffer. All fragments of a frame share ONE sequence
   number, so the BA window would drop the 2nd+ fragment as "outdated". Fragments go straight to the
   after-reorder path. No-op for unfragmented frames.
2. **Reassemble-then-decrypt branch** (`..._after_reorder`), gated to a **protected, FW-raw-delivered,
   mesh SW-CCMP, unicast, fragment**: call `datapath_defrag` to reassemble the raw ciphertext, then run
   `umac_datapath_sw_ccmp_decrypt` on the whole reassembled frame once. Everything else keeps the normal
   decrypt-then-defrag path.

### Bug 1 — the reassembled decrypt omitted the QoS Control from the AAD (found by adversarial review, pre-flash)

`datapath_defrag` re-prepends only the 30-byte MAC header (`dot11_data_hdr_get_len` excludes the separate
2-byte QoS Control, which was peeled from the view earlier). But `mesh_ccmp_aad_nonce` reads the QoS/TID
byte **contiguously at `hdr[30]`**. In the reassembled buffer that byte is the CCMP header's PN0, so the
AAD + nonce were wrong → MIC fails on essentially every frame.

**Fix:** before `datapath_defrag`, copy `[MAC header + QoS]` (still adjacent in the fragment buffer) into
a local `aad_hdr[32]`, clear MoreFragments and set fragment# = 0 on that copy (both resets are REQUIRED —
`mesh_ccmp_aad_nonce` masks neither, and only the sequence number), and pass `aad_hdr` as the AAD header
to the decrypt. The reassembled view still supplies the CCMP header + ciphertext + MIC.

### Bug 2 — `mmdrv_get_rx_metadata` asserted on the reassembled buffer (found on-bench, crash-loop)

Symptom: board2 (relay AND single-hop endpoint) crash-looped with an `MMOSAL Assert` the moment it
processed a fragmented frame — `reasm` counter reached 1 then the board reset, before `decok`. The
mangled `libmorse` symbols pointed only at `umac_datapath_process`; the RX-side counters localized it to
**between reassembly-complete and the decrypt call**. That code was `rx_metadata = mmdrv_get_rx_metadata(rxbuf)`.

Root cause: `mmdrv_get_rx_metadata` (`mmdrv.h`) does `MMOSAL_ASSERT(metadata != NULL)` and is documented
"for RX frames only". The reassembled buffer is a `mmdrv_alloc_mmpkt_for_defrag` buffer with **no RX
metadata** → NULL → assert → reset. Also `datapath_defrag` had already released the original rxbuf, so
the old `rx_metadata` dangled.

**Fix:** `rx_metadata = NULL` after reassembly — it is not read again on the reassembled path (mirrors the
normal decrypt-then-defrag path, which sets `rx_metadata = NULL` before `datapath_defrag`).

## Bench method (all-ESP, temp scaffold — stripped after)

- Rig: board0 (origin, 10.9.9.136) → board2 (relay, .108) → board1 (far, .100); forced-topology allowlist.
- Force the FW to fragment: `mmwlan_set_fragment_threshold(700)` + full-size 1400 B pings @ 200 ms.
- RX instrumentation (temp `mmwlan_dbg_frag_*` globals, `mmwlan*`-prefixed to survive the library mangler):
  `seen` (fragments entering the branch), `f0` (fragment# 0), `last` (MoreFragments 0), `reasm`
  (`datapath_defrag` completed), `decok` (whole-frame decrypt OK), + `ccmp_fail`. Dumped via
  `mmwlan_get_umac_stats` on the app heartbeat.
- Observable: board0 ICMP reply rate (a reply means every reassemble-then-decrypt on the path succeeded).
- Clean peering requires flashing **all** boards fresh together (a lone reflash flaps the peer link).

## Results

| Test | board0 replies | relay (board2) | far (board1) | asserts |
|------|----------------|----------------|--------------|---------|
| Single-hop 1400 B forced-frag | **131 / 131 (100%)** | n/a (endpoint) `reasm=390/3=130, decok=130, ccmp_fail=0` | idle | 0 |
| Multi-hop 1400 B forced-frag | **143 / 146 (~98%)** | `reasm=255 decok=255 ccmp_fail=0` (both legs, forwards) | `reasm=128 decok=128` | 0 |
| Toggle OFF (no forced frag) | **117 / 121 (~97%)** | `FRAG seen 767→10` (mostly whole/A-MPDU) `reasm=3 decok=3` | `reasm=1 decok=1` | 0 |

- **Every fragment reassembles AND decrypts** (`reasm == decok`, `ccmp_fail = 0`) at all four RX points
  (relay ×2, far, origin), in both directions. A full-size frame fragments into **3** MPDUs.
- **On-air (chronium morse0 monitor):** the FW *does* fragment host-built mesh SW-CCMP frames — 3 MPDUs
  sharing one sequence number, incrementing fragment#, MoreFragments on all but the last. (An earlier
  worry that the FW refuses to fragment host frames was a peer-flap confound, not a size limit.)
- **Toggle OFF confirms the whole-frame / A-MPDU path is untouched** by the `is_fragment` bypass: without
  the forced threshold, ~97 % of large frames go whole (A-MPDU) at high rate; the rare natural
  FW-fragmentation (~0.2 %, the real-world case) is handled by the same reassemble path.

## 10-minute soak (stability + leak + no-silent-failure)

Re-flashed all three fresh (multi-hop forced-frag) with cumulative counters + a heap watch, ran **600 s
continuous**:

| node | fragments | reassemblies | `decok` | `ccmp_fail` | heap free (start→end) |
|------|-----------|--------------|---------|-------------|------------------------|
| board0 origin | 8 856 | 2 952 | 2 952 | 0 | 8 619 000 → 8 620 860 |
| board2 relay | 17 716 | 5 905 | 5 905 | 0 | 8 623 560 → 8 622 164 |
| board1 far | 8 859 | 2 953 | 2 953 | 0 | 8 625 248 (flat) |

- **`reasm == decok` on every 5 s heartbeat, every board** — ~11 800 reassemble-decrypt cycles network-wide,
  **zero silent failures**; `ccmp_fail = 0` the whole run.
- **ping 2952 / 2956 = 99.86 %** — all 4 timeouts in the first 15 s (cold-start peering), **zero after
  t=15 s** → steady-state effectively 100 % for ~9.75 min.
- **Heap flat** (board0 even rose; board2 ±1.4 KB jitter) across ~26 K fragments processed → **no leak** from
  the `datapath_defrag` buffers. **`rxq_drop = txq_drop = 0`** → no backpressure.
- **Zero crashes / reboots:** each board logged an unbroken 119-heartbeat chain (uptime 5 s → 595 s); the only
  resets in the capture were the initial `USB_UART_CHIP_RESET` flash boots.

## Conclusion

Defrag-before-decrypt is the proper, bench-verified fix for large secured mesh frames. It works for both
FW-forced and naturally-occurring fragmentation, single- and multi-hop, both directions, no crashes. The
committed `9c7daabd` host-fragmentation path is reverted (it added a broken path with no benefit).

**Fix location:** `components/halow` `umac_datapath.c` (reorder bypass + reassemble-then-decrypt branch +
the `aad_hdr` QoS fix + `rx_metadata = NULL`). Builds green via `make build APP=rimba-halow-mesh-perf
BOARD=proto1-fgh100m`. Temp bench scaffold + `mmwlan_dbg_*` counters stripped; app restored.

## Gotchas (reusable)

- `mmdrv_get_rx_metadata` **asserts** on any buffer without RX metadata — never call it on a
  `datapath_defrag` (reassembled) buffer.
- The reassembled decrypt needs a **contiguous `[MAC header + QoS]`** AAD header; `datapath_defrag`
  re-prepends only the MAC header.
- morselib globals must be **`mmwlan*`-prefixed** to survive the `librarymangler` (protected-symbol list)
  — needed to read temp instrumentation from the app.
- `reasm == decok, ccmp_fail == 0` at every hop is the pass signature; `reasm > decok` = decrypt bug,
  `reasm = 0` with `seen > 0` = reassembly not completing (missing last fragment / seq mismatch).

## Radio state

All 3 ESPs flashed with `rimba-hello` (radio-silent); chronium `wlan1` + `morse0` set down.
