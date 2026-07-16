# 2026-07-14 — Mesh host-fragmentation: on-air bench verification

> **SUPERSEDED (later same day).** This worklog concludes "revert `9c7daabd` and tolerate the ~0.24 %
> loss". The story continued: the **defrag-before-decrypt** approach (let the FW fragment
> *encrypt→fragment*, reassemble the raw fragments then decrypt the whole) was implemented and
> **bench-VERIFIED PASS** — large secured mesh frames now traverse the mesh, single- and multi-hop, both
> directions. See [`2026-07-14-mesh-defrag-before-decrypt-PASS.md`](2026-07-14-mesh-defrag-before-decrypt-PASS.md).
> The `9c7daabd` revert still stands (host fragment→encrypt was broken); defrag-before-decrypt is the
> actual fix, not revert-and-tolerate.

**Goal.** Bench-verify the committed but unverified host 802.11 fragmentation fix for mesh SW-CCMP
(fragment→encrypt; halow `9c7daabd`, superproject `2bb61d2`). Prove on hardware that a full-size
multi-hop mesh frame that takes the **host** fragmentation path is decrypted + reassembled end-to-end
with no MIC failure, and that A-MPDU still sends full-size frames **whole** at high rate (the fragment
path is skipped for aggregated frames).

Design + code-map: [`../mesh-ap/rimba-mesh-frag-codemap.md`](../mesh-ap/rimba-mesh-frag-codemap.md).
Prior context: the RC-on-`key_stad` S3 fix that this residual follows
(`2026-07-13-...`), memories `mesh-frag-host-fragmentation`, `mesh-ampdu-bench-read-blocked`.

## Why it needs a deterministic toggle

The fragment path fires only for a **non-A-MPDU** mesh SW-CCMP frame that is `>= MESH_FRAG_MPDU_MAX`
(728, FCS-less). In normal operation A-MPDU-eligible frames are sent whole (mac80211 DONTFRAG for
aggregated frames), so the residual only appears transiently (cold-start / rate fallback). To exercise
it **on demand**, a temp toggle forces mesh unicast non-A-MPDU so every full-size mesh frame fragments.

## The temp patch (to be stripped after)

**morselib** (`umac_datapath.c` + `umac_mesh.h`), all app-defined globals morselib only references —
mirrors the stripped `g_mesh_fwd_dbg` template:

- `g_mesh_force_no_ampdu` — read at BOTH `do_fragment` gates (origin `process_tx_frame` ~:2448, forward
  `tx_mesh_keyed_frame` ~:2999): `ampdu_eligible = … && !g_mesh_force_no_ampdu`. Forces
  `do_fragment = mesh_swccmp && !ampdu_eligible && frame>=728` true for full-size frames.
- `g_frag_dbg[FRAG_DBG_COUNT]` counters:
  - TX: `FRAG_TX_ORIGIN`, `FRAG_TX_FWD` (which gate fired), `FRAG_TX_SENT` (total fragment MPDUs emitted).
  - RX: `FRAG_RX_MESH_SEEN` (denominator), `FRAG_RX_FRAG_SEEN` (of those, MoreFrags||frag#!=0),
    `FRAG_RX_MIC_FAIL` / `FRAG_RX_REPLAY_FAIL` (inside `sw_ccmp_decrypt`), `FRAG_RX_DECRYPT_OK`
    (per-fragment decrypt survived), `FRAG_RX_REASSEMBLED` (a fragmented MSDU fully reassembled —
    `datapath_defrag` returned a buffer), `FRAG_RX_FWD_REACHED` (relay reached `umac_mesh_forward_data`).
- `datapath_defrag.c` UNCHANGED (reassembly counted in the caller).

**app** (`rimba-halow-mesh-perf`): new `MESH_FRAG_TEST` mode = forced all-ESP line
**board0 (origin) → board2 (relay) → board1 (far)** — board2 is the ONLY fully-wired ESP so it takes
the relay role. board0 large-auto-pings board1 (1400 B, 200 ms) THROUGH board2; static ARP both
endpoints (echo + reply both cross the relay). `MESH_FORCE_FRAG` sets the toggle. Counters dumped as a
one-line `FRAG-DBG[…]` via ESP_LOGI every 3 s.

## Expected signatures

- **Test A (force-frag):** board0 `TXorigin>0`, `TXsent≈3×`; board2 (relay) `RXmesh` high, `RXfrag≈RXmesh`,
  `mic_fail≈0`, `decrypt_ok≈RXmesh`, `reasm>0`, `fwd_reached>0`, `TXfwd>0` (relay re-fragments on forward);
  board1 (far) `decrypt_ok>0`, `mic_fail≈0`, `reasm>0`; board0 console logs `reply from 10.9.9.100`
  (end-to-end round trip). A MIC-fail count near `RXfrag` would mean the moreFrag break was NOT fixed.
- **Test B (toggle off):** `TXorigin/TXfwd/TXsent≈0` (nothing fragments), traffic flows, throughput high,
  frames whole/aggregated on-air.

## Log

- **Implemented** the temp patch (morselib 12 points + app mode/dump/globals). Build clean (US country,
  fits 4% free). Adversarial review (4-lens workflow): 1 real finding — `mic_fail`/`replay` counters were
  also hit by the protected-mesh-**action** decrypt path (line 1519); fixed by gating on
  `space == UMAC_KEY_RX_COUNTER_SPACE_DEFAULT` (data path only). Link-breakage for sibling apps noted +
  accepted (temp app-defined globals; only mesh-perf is built; rimba-hello links no morselib).

## Test A (force fragmentation) — 2026-07-14, all-ESP line board0→board2→board1 — **FAIL**

Bench: board0=ACM0 (origin), board1=ACM1 (far), board2=ACM4 (relay, PPK2). Force-frag build flashed to
all 3. 90 s large auto-ping (1400 B, 200 ms). chronium morse0 monitor ch27 for the on-air decode.

**Console counters (t=87 s):**
- board0 (origin): `TXorigin=84 TXsent=252` — **fragments correctly** (exactly 3 host fragments/ping).
- board2 (relay): `RXmesh=248 RXfrag=238 mic_fail=158 (~64%) decrypt_ok=89 reasm=0 fwd_reached=0`.
- board1 (far): counters froze at `RXmesh=27` ~t=11 s (no traffic reaches it). **Zero ping replies.**

So the relay MIC-fails ~2 of every 3 fragments, **never reassembles a single MSDU** (`reasm=0`), never
forwards (`fwd_reached=0`) → the line is dead end-to-end.

**On-air root cause (chronium morse0, 4-addr QoS-Data decode + raw hex).** Each ping → 3 host fragments,
1:1 with the on-air frames (no FW re-fragmentation). But:
- **frag0** (frag#=0): `[MAC hdr 32][CCMP 8][cipher][MIC]` — **correct** (727 B FCS-less, MoreFrags=1).
- **frag1 / frag2** (frag# > 0): `[MAC hdr 32][DUPLICATE MAC hdr 32][CCMP 8][cipher][MIC]` — a **byte-identical
  copy of the fragment's own 32-byte MAC header is prepended** (frag1=759 B = 727+32; frag2=164 = 132+32).
  The CCMP header (`..09 00 20 ..`, keyid 0x20) sits at offset **64**, after two headers. The FW also sets
  Retry and **clears MoreFrags** on these.

`umac_datapath_sw_ccmp_encrypt` emits a correct single-headered frame (CCMP present + well-formed), so the
extra 32-byte header is added **after host encryption, on the FW/TX path**, and **only to fragments with
frag# > 0** (frag0 is untouched). The host builder code is identical for every fragment index — so this is
the **MM6108 FW mishandling host-submitted non-first fragments of a same-sequence-number burst** (it
prepends a replicated header, flips Retry/MoreFrags). The prepended header shifts the CCMP header + corrupts
the AAD/length the receiver reconstructs → MIC fails at the relay → no reassembly → no multi-hop delivery.

**Verdict: the committed host-fragmentation fix (halow 9c7daabd) is NOT verified — it is broken on this
hardware.** frag0 proves host fragment→encrypt is structurally right; the FW mangles frags i>=1. Same class
as `#20` (FW-level mesh limitations).

## Test B (toggle OFF) — control: A-MPDU sends full-size frames whole — **PASS**

Same rig + instrumentation, `MESH_FORCE_FRAG` off (`g_mesh_force_no_ampdu=false`), 70 s.

- **Cold start (t≈6–17 s):** the fix fires **naturally** here (the BA session isn't up yet → `ampdu_eligible
  = umac_ba_is_ampdu_permitted(...) = false` → `do_fragment` true for the full-size ping). board0 fragments
  **2** frames (`TXorigin=2 TXsent=6`) and those cold-start fragments MIC-fail on the relay exactly as in
  Test A (`RXfrag`/`mic_fail` climb, `reasm=0`). Pings 1–11 timeout.
- **BA/A-MPDU establishes (~t=17 s):** `TXorigin` **freezes at 2** — fragmentation stops, frames go **whole
  (aggregated)**. board2 forwards the whole frames (`fwd_reached` 0→**223**), board0 gets **100+ ping
  replies** (seq 12→135, RTT ~65–340 ms, steady ~70–130 ms). `reasm` stays 0 throughout (whole frames need
  no reassembly). The only fragments/MIC-fails in the whole run are the cold-start ones (frozen values).

**PASS** — A-MPDU-whole delivery works end-to-end over board0→board2→board1; the rig + topology are sound.
This isolates the failure to the fragment path and shows the fix is **ineffective for its purpose**: the
residual case it targets (cold-start / pre-BA full-size frames) is exactly where its fragments get mangled
and lost — the same outcome as letting the FW fragment them. It neither helps nor hurts; it adds a broken
path. (Impact is bounded: once the BA session is up, mesh rides A-MPDU-whole and is fine.)

## Conclusion + recommendation

**Root cause (on-air proven):** the MM6108 ESP firmware re-headers any host-submitted MPDU whose
`fragment_number > 0` (prepends a 32-byte copy of the frame's MAC header, sets Retry, clears MoreFrags),
corrupting the CCMP AAD so the receiver MIC-fails it. Host 802.11 fragmentation (fragment→encrypt) is
therefore **not viable on this FW** for anything past frag0. This is a hardware/FW limitation in the same
family as `#20` (host-stack vs FW-command mesh limits), not a morselib logic bug — `sw_ccmp_encrypt` + the
builder emit a correct single-fragment frame; the FW mangles the multi-fragment burst.

**Follow-Linux cross-check (source, both trees — confirms it's a FW wall, not a morselib gap).** The
morse **Linux** driver does NOT host-fragment on this firmware, and there is no per-frame "already
fragmented / leave-it-alone" flag anywhere in the host↔FW protocol:
- mac80211's host fragmenter `ieee80211_tx_h_fragment` returns `TX_CONTINUE` (skips host fragmentation) when
  the driver sets `SUPPORTS_TX_FRAG` (`net/mac80211/tx.c:967-968`), and defaults to `DONTFRAG`
  (`tx.c:964`, `frag_threshold=-1`, `tx.c:1275-1280`). morse_driver sets `SUPPORTS_TX_FRAG` straight from the
  firmware's `HW_FRAGMENT` capability bit (`morse_driver/mac.c:6835-6836`; cap `capabilities.h:86`, read via
  `morse_cmd_get_capabilities`). So Linux hands the FW **whole** (frag#=0) MPDUs + a global threshold
  (`.set_frag_threshold`, `mac.c:5632` → `command.c:2387`); the **FW** fragments internally. It never emits a
  frag# > 0 MPDU to the chip.
- The host→FW TX descriptor carries **no** fragment/seq/"raw"/"don't-reprocess" field in EITHER tree: Linux
  `struct morse_skb_tx_info.flags` (`morse_driver/skb_header.h:243-251,57-71`) and morselib `MMDRV_TX_FLAG_*`
  (`morselib/src/internal/mmdrv.h:609-619`) both expose only AMPDU/HW_ENC/VIF/KEY_IDX/IMMEDIATE_REPORT/… —
  no fragment bit. So neither stack *can* tell the FW "this is a pre-built host fragment."
- morselib already mirrors the FW-owned threshold path (`mmwlan_set_fragmentation_threshold` →
  `MORSE_PARAM_ID_FRAGMENT_THRESHOLD`, cap `MORSE_CAPS_HW_FRAGMENT`).
- **The crypto mode — verified 2026-07-14 (correcting an earlier loose "Linux uses HW crypto" claim that was
  also self-contradictory vs `#20`).** The bench Linux nodes run **HW crypto**: `no_hwcrypt=N` live on
  chronite/chronium/chronosalt (`is_sw_crypto_mode()` = `return no_hwcrypt`, `mac.c:529`), and
  `morse_mac_ops_set_key` installs CCMP keys **in the FW** by default (`mac.c:5141-5230`; SW fallback only if
  `no_hwcrypt` or the install fails). Under HW crypto the FW does fragment→encrypt in the correct order — but
  `#20` (bench-proven) blocks the HW-crypto multi-hop **forward** (a Linux node received 0 of an ESP relay's
  A4≠TA forwards), so **default Linux mesh is single-hop-only for encrypted traffic, and there is NO working
  multi-hop-encrypted Linux mesh** (nobody runs `no_hwcrypt`). So there is **no Linux precedent** for the
  ESP's case (SW-CCMP multi-hop mesh). Forcing Linux to SW crypto (`no_hwcrypt=1`) for multi-hop would hit
  the **same wall**: mac80211 still wouldn't host-fragment (`DONTFRAG` default) → the FW fragments the
  SW-encrypted whole frame → moreFrag-after-MIC; or, with `iw phy set frag` forcing host fragmentation, the
  FW re-headers frag#>0 (the wall this session found). Linux has no magic here.
  - **Confidence:** `no_hwcrypt=N` (live) + `set_key` HW-offload + mac80211 `DONTFRAG`-default = **VERIFIED**
    (module param + source). "HW-crypto ⇒ single-hop-only / SW-crypto multi-hop would hit the same wall" =
    **solid inference** (from `#20` + the source), and the HW-crypto-fragments-cleanly half is now **captured
    on-air** (below).

### On-air confirmation — Linux HW-crypto FW fragmentation is CLEAN + delivers (2026-07-14)

Closed the one remaining gap empirically. Brought up a **secured single-hop Linux mesh** chronite ↔
chronogen (SAE+AMPE, `rimba-smesh`, HW crypto — `no_hwcrypt=N`), forced **MCS0** (`fixed_mcs=0`,
`enable_fixed_rate=Y` on both) and a **512 B frag threshold** on chronite (`iw phy phy5 set frag 512` — so the
FW fragments; note the driver *does* honor the threshold command), and captured chronite→chronogen on
chronium's `morse0`.

- **The FW fragments cleanly.** A 1400 B ping → 4 on-air fragments, e.g. `seq=51 frag=0..3`: MoreFrags
  **1,1,1,0** (correct), incrementing frag#, **constant seq#**, each fragment **528 B** with its **own CCMP
  header + a strictly consecutive PN** (82,83,84,85). Raw hex of a `frag=1`:
  `[0-31]=MAC hdr … b1 07(seq,frag1) …` then `[32]=58 01 00 20 00 00 00 00` = **CCMP header at offset 32**.
- **Airtight A/B vs the ESP.** Linux frag>0 = **single-header** (CCMP at offset **32**, len 528). ESP frag>0
  (Test A) = **double-header** (a duplicate 32 B MAC header, CCMP shoved to offset **64**, len 763). Same
  silicon + FW; the only difference is **who encrypts**: Linux = FW (HW crypto → the FW owns fragment→encrypt
  and produces correct fragments); ESP = host (SW-CCMP → the FW re-headers the host's frag>0 MPDUs).
- **It delivers end-to-end.** ping chronite→chronogen through the FW-fragmented path = **61/70 (87%)** —
  chronogen reassembles + replies (the ~13% loss is expected: MCS0/1 MHz, 4-fragment frames, lose the whole
  MSDU if any fragment drops). Contrast the ESP relay: **0/…** delivered, reassembled **zero**.

So: **under HW crypto the FW fragments a mesh frame correctly and it works; the ESP fails only because it must
use SW crypto (`#20`), where the FW can't do the encrypt step and mangles host-submitted fragments.** The
"MTU-cap / min-MCS = never fragment" conclusion is confirmed as the only path for the ESP's SW-CCMP mesh.
Bench torn down + forced params (fixed-rate, frag threshold) reset; radio-silent.

**Recommendation (deterministic).** Host fragmentation (fragment→encrypt→submit) is **fundamentally
incompatible** with this MM6108 firmware for mesh SW-CCMP — a hard FW wall in the `#20` family, not a
fixable morselib flag. Abandon it and instead keep every full-size mesh SW-CCMP frame **under the
single-PPDU limit so it is never fragmented at all** — a **mesh-vif MTU cap** (host never hands down a
frame that would need fragmenting) or a **min-MCS floor** on the mesh RC (a full frame always fits one PPDU
at the worst rate). Either keeps frag#=0 always. Meanwhile A-MPDU-whole already carries the steady-state
mesh fine (Test B). The committed 9c7daabd host-fragmentation fix should be **reverted or gated off** (it
adds a broken path with no benefit); this is the owner's call — left in the tree for review, not reverted
here.

## Revert + ESP verification of the reverted mesh (2026-07-14) — the revert IS the practical fix; MTU-cap unnecessary

`9c7daabd` **reverted** (`git revert --no-commit`, staged, builds green, commit held for after-hours). Then
verified what the reverted mesh actually does — because the "MTU-cap is needed" conclusion above rested on
the premise that the mesh routinely fragments, which turned out to be **much narrower** than the forced
Test A/B implied.

**First, the boundary is rate-dependent (measured on Linux, same FW).** At **MCS0/1 MHz** a full-MTU mesh
frame — **1566 B on-air — goes WHOLE** (45/45 ping, all frames `len=1566 more=0`). So MCS0 is *not* where a
normal frame fragments. But the mesh drops **below** MCS0 on a weak link (the 1 MHz repetition mode, budget
~half MCS0), and there the single-PPDU budget is ~**728–740 B** — so the old `728` number was right *for the
worst rate*, not for MCS0. A frame only fragments when the link is bad enough to ride that bottom rate.

**Then the ESP verification (reverted build, no host fragmentation, all-ESP line board0→board2→board1,
full-size 1400 B auto-ping, on-air capture):**
- **Fragmentation is RARE: 1 fragmented frame in 20 s (~0.24%) vs 410 whole frames.** The revert took the
  mesh from "*every* full frame fragments" (the forced host-fix behavior) to "almost none."
- **~86% ping success** (161 replies / 26 timeouts), but the bench link was running **MCS0/MCS2** — a
  marginal / RX-overloaded link (boards too close, see [[bench-halow-rx-overload]]). So **the ~14% loss is
  overwhelmingly low-rate RF loss, not fragmentation.** On a good link (MCS7) a full frame fits one PPDU →
  **zero** fragmentation.
- The rare FW-fragmented frame still breaks (the FW fragments a host-SW-CCMP whole frame → MoreFrags-after-MIC
  + no per-fragment CCMP on frag>0, the same break class), but at 0.24% it is a negligible loss that
  retransmits.

**Revised recommendation (supersedes the "MTU-cap is the only path" line above):**
- **The revert is the fix.** It removes the systematic breakage; the residual is ~0.24% on a marginal link
  and ~0 on a good link.
- **An MTU cap is overkill** — it would cap *every* mesh frame (to ~660 B if sized for the worst rate, or
  ~1400 B for a looser margin) to eliminate a &lt;0.3% edge that only appears on a weak link. Not worth the
  throughput.
- **A min-MCS floor is the right *optional* hardening** for weak-link robustness (keep the mesh RC off the
  ~728-budget bottom rate so a full frame always fits) — but it needs the exact fragmenting rate pinned
  first (looks like the 1 MHz repetition mode below the normal MCS0–7 mmrc set), and the payoff is small.
  Treat it as a future item, not a bolt-on.

Bench torn down after each run; params reset; **radio-silent** (all ESPs `rimba-hello`, chronium + Linux
`wlan1` down). The `9c7daabd` revert is staged; commit held for after 17:00.
