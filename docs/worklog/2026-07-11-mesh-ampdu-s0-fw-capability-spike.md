# Worklog — 2026-07-11 — Mesh A-MPDU S0 firmware go/no-go spike

**Author:** Aldwin
**Phase:** 802.11s mesh throughput — A-MPDU aggregation feasibility (Stage 0)
**Goal:** answer, on hardware, the two questions that gate the whole A-MPDU
feature: (a) does the MM6108 firmware advertise A-MPDU capability for a **mesh**
vif, and (b) will the chip actually emit an **on-air A-MPDU** for 4-address mesh
QoS-data if the host marks the frames eligible? Both must be yes before spending
days on the host-side Block-Ack machinery.
**Status:** S0 both halves = **GO**. Capability probe reads AMPDU_cap = 1 on the
mesh vif; the chip emitted real on-air A-MPDUs (up to 7 MPDUs/PPDU) for mesh
QoS-data. Temporary spike code reverted; both boards reflashed to `rimba-hello`;
sniffer `wlan1` down. Nothing committed. The only missing piece is the host-side
BA handshake, which is Stage 1.

This entry is a **standalone** go/no-go spike record. Every method, address,
counter and capture reading needed to re-run it cold is in this file. Full staged
plan (S0–S3) lives in
[`../mesh-ap/rimba-mesh-ampdu-aggregation-design.md`](../mesh-ap/rimba-mesh-ampdu-aggregation-design.md);
this worklog does not depend on it.

---

## 1. Why now — the crypto ceiling just moved, airtime is the new limiter

Today the 802.11s mesh datapath sends **single MPDUs**: one PPDU per frame, each
paying full preamble + IFS + backoff + ACK overhead. That per-frame airtime tax
is the current throughput ceiling. Until yesterday the *other* ceiling was
host-side SW-CCMP crypto cost; the just-merged bulk-DMA AES-CCM work
(`esp_mesh_ccm_ae/ad`, ~14–28× faster crypto under load) removed it. With crypto
no longer the bottleneck, **per-frame airtime is the throughput limiter**, and
A-MPDU aggregation is the lever that amortises it across many subframes in one
PPDU.

A prior feasibility study rewrote the stale blocker-map for this feature. Its two
load-bearing conclusions, which S0 is built to confirm on real hardware:

- **The CONNECTED gate is NOT the blocker.** The mesh datapath's
  `get_sta_state` returns `CONNECTED` unconditionally, so the aggregation
  eligibility check's first gate is already satisfied for mesh frames. (The old
  blocker-map claimed a `MMWLAN_STA_CONNECTED` gate blocked mesh aggregation;
  that was stale.)
- **A-MPDU is assembled by the MM6108 FIRMWARE, not the host.** The host's job is
  only to (1) set a per-frame "A-MPDU eligible" flag, (2) run the Block-Ack (BA)
  session handshake, and (3) reorder on RX. The chip does the actual subframe
  packing into one PPDU.

If both are true, the feature is a bounded host-side project (BA handshake +
RX reorder), not a firmware change. S0 is the cheap experiment that proves the
firmware side before committing to that project.

## 2. What S0 is — two independent go/no-go questions

- **S0a — capability probe:** does the firmware report A-MPDU as *supported* for
  a mesh vif? This is the gate the aggregation eligibility check consults.
- **S0b — on-air emission:** if the host force-marks mesh unicast data as
  A-MPDU-eligible (bypassing the not-yet-implemented BA handshake), does the chip
  actually put multiple MPDUs into one PPDU on air?

S0a proves the *policy* path is open; S0b proves the *silicon* will do the work.
Both are throwaway instrumentation — nothing here is meant to ship.

## 3. S0a — capability probe: AMPDU_cap = 1 on the mesh vif

**Method.** Added a temporary `printf` in morselib immediately after the per-vif
capability fetch — `umac_interface.c:321`,
`mmdrv_get_capabilities(data->vif_id, &data->capabilities)` — printing the two
values the aggregation eligibility check reads plus the vif type:

- `MORSE_CAP_SUPPORTED(&data->capabilities, AMPDU)`
- `umac_config_is_ampdu_enabled(umacd)`
- the interface type string

Built `rimba-halow-mesh-perf`, flashed **board1** (efuse MAC
`e0:72:a1:f8:f9:40`, `/dev/ttyACM1`), read the boot log over serial.

**Result:**

```
S0PROBE vif=0 type=?? AMPDU_cap=1 ampdu_enabled=1
```

The mesh vif reports **`AMPDU_cap = 1`** and global **`ampdu_enabled = 1`**.

- The `type=??` is not an error — it is the tell that this line **is** the mesh
  vif. `umac_interface_type_to_str` has no case for `UMAC_INTERFACE_MESH` (= 32),
  so it falls through to `default: "??"`. NONE / STA / AP / IBSS all have named
  cases, so the only vif that prints `??` is the mesh one.
- Consequence for the datapath: the aggregation eligibility check's second gate,
  `MORSE_CAP_SUPPORTED(..., AMPDU)` at `umac_datapath.c:2044`, **passes** for a
  mesh vif. Combined with the CONNECTED gate already passing (§1), both eligibility
  gates a mesh frame must clear are open in firmware/config today.

**S0a verdict: GO.** The firmware advertises A-MPDU capability for a mesh vif.

## 4. S0b — on-air A-MPDU emission: the chip aggregates mesh QoS-data

### 4.1 The force (temporary, reverted)

To isolate the single question "*will the chip aggregate mesh data if told to*"
from the missing BA handshake, added a temporary force in `umac_datapath.c`
(~`:2272`, inside `umac_datapath_process_tx_frame`, gated on
`tx_is_mesh_frame`):

- unconditionally set `MMDRV_TX_FLAG_AMPDU_ENABLED` on mesh unicast data, and
- force `tid_max_reorder_buf_size = 8`

i.e. make mesh frames A-MPDU-eligible **without** the BA session that would
normally precede aggregation. This deliberately skips the handshake — it is not
a shippable path, only a probe of the silicon's willingness to pack subframes.

### 4.2 Rig

**Secured mesh (SAE + PMF + host SW-CCMP).** `rimba-halow-mesh-perf` runs
`args = {0}` → `args.security_type = MMWLAN_OPEN`, but the mesh **ignores that**:
morselib gates mesh security on the compile-time `MMWLAN_MESH_SEC_PHASE1`, which
defaults to **1** (`umac_mesh.c:439-440`) and sets every mesh peer stad to
`MMWLAN_SAE` / `MMWLAN_PMF_REQUIRED` (`umac_mesh.c:607`), with the SAE password
hardcoded `"rimbamesh2026"` (`umac_mesh.c:81`). So peering did a full SAE+AMPE
handshake and the data frames are **CCMP-encrypted** — on-air, board1's QoS-Data
is 100% `wlan.fc.protected = 1`.

```
 board0 (relay)                                board1
 rimba-halow-mesh-perf                          rimba-halow-mesh-perf
 /dev/ttyACM0                                   /dev/ttyACM1
 mesh IP 10.9.9.136                             mesh IP 10.9.9.100
 mesh MAC e2:72:a1:f8:ef:a4  <== S1G ch27 ==>   mesh MAC e2:72:a1:f8:f9:40
                          direct peers, freq 5560 (915.5 MHz)

 chronium (Pi 5, dedicated sniffer)
 wlan1 -> monitor, freq 5560, morse0 up, tcpdump -i morse0 -w cap.pcap
```

- **Peering confirmed** before generating load: `ping board1 → board0` = **5/5,
  0% loss** (~21–31 ms steady, 526 ms on the first/warm packet).
- **Monitor bring-up (chronium):** `sudo iw dev wlan1 set type monitor`,
  `sudo iw dev wlan1 set freq 5560`, `ip link set morse0 up`,
  `tcpdump -i morse0 -w cap.pcap`.

### 4.3 Traffic — saturate the TX queue so frames can back up

`board1: iperf -c 10.9.9.136 -u -b 2 -t 14` — UDP, **2 Mbit/s offered** (roughly
10× the real link capacity) for 14 s. The point of over-offering is to keep the
TX queue non-empty: A-MPDU can only aggregate frames that are *already queued*
when a PPDU is built, so the queue has to back up for aggregation to happen at
all.

**iperf throughput** (3-second intervals): **0.59, 1.22, 1.27, 0.24,
0.22 Mbit/s**; **avg 0.71 Mbit/s, PEAK 1.27 Mbit/s** — against the
**~0.2 Mbit/s single-MPDU baseline** for this link, i.e. roughly **6×** at peak.
The throughput jump is corroborating, but not proof by itself; the proof is in
the capture.

### 4.4 Detecting aggregation without a radiotap A-MPDU field

The morse0 radiotap header does **not** expose an A-MPDU status field to tshark,
so aggregation cannot be read off a flag. It was detected instead by **TSFT
clustering + consecutive sequence numbers**:

- MPDUs transmitted inside one A-MPDU are carried in **one PPDU**, so they share
  the **exact same `radiotap.mactime`** (the µs TSFT timestamp of the PPDU).
- A genuine aggregate additionally carries **consecutive `wlan.seq`** values (the
  MAC hands the queued frames to the aggregator in sequence order).

So: group QoS-Data frames by identical TSFT; any group of size ≥ 2 is one
multi-MPDU PPDU; consecutive `wlan.seq` inside the group is the airtight
signature. This is a reusable method for this sniffer and is recorded as such.

### 4.5 Capture results

**3498 frames** total on morse0. Of board1's transmissions, **1310 were
QoS-Data** (subtype `0x28`). Grouping those by shared TSFT:

| PPDU size (MPDUs) | count | frames covered |
|---|---:|---:|
| 1 (single) | 711 | 711 |
| 2 | 52 | 104 |
| 3 | 165 | 495 |
| **aggregated (≥2)** | **217** | **599** |

So **217 aggregated PPDUs** carried 599 of the 1310 QoS-Data frames — **~46 % of
board1's mesh QoS-data went out aggregated**.

**Airtight confirmation** — three separate shared-TSFT groups, each exactly 3
QoS-Data frames with **consecutive** sequence numbers, identical
SA = board1 / DA = board0 / TA = board1, identical length 1624:

| TSFT (µs) | seq numbers | MPDUs |
|---|---|---:|
| 188379322 | 794, 795, 796 | 3 |
| 188364744 | 791, 792, 793 | 3 |
| 188346837 | 788, 789, 790 | 3 |

Consecutive `wlan.seq` + identical PPDU timestamp + identical addresses on three
independent groups = a real A-MPDU. Coincidence is excluded: three unrelated
frames could not share a µs-exact TSFT *and* be sequential *and* share all three
addresses by chance.

**board0's return direction aggregated even deeper.** Its QoS-Data PPDU
size distribution: `131×1, 50×2, 27×3, 11×4, 13×5, 14×6, 4×7` — i.e. **up to 7
MPDUs per PPDU**. So the chip aggregates in both directions, and depth is not
pinned at 3.

**S0b verdict: GO.** The MM6108 firmware (fw **1.17.8**) *does* assemble on-air
A-MPDUs for 4-address mesh QoS-data when the host marks the frames eligible.

## 5. Conclusion — feature is a GO; Stage 1 is the host BA handshake

Both halves of S0 pass on hardware:

- **S0a:** the firmware advertises A-MPDU for a mesh vif (`AMPDU_cap = 1`), so the
  datapath's eligibility gates are open.
- **S0b:** given eligible frames, the chip really packs mesh QoS-data into
  multi-MPDU PPDUs on air (3 MPDUs board1→board0, up to 7 board0→board1).

This confirms the feasibility study's core claim: **A-MPDU assembly is a firmware
capability the host only has to *enable*.** The only missing piece is the
host-side **Block-Ack handshake** — the S1 implementation work: routing inbound
`BLOCK_ACK` action frames to the BA state machine, and (for a secured mesh) a PMF
exemption so the BA action frames are accepted. Nothing in S0 required a firmware
change, which is the whole point of running the spike first.

## 6. Caveats — stated honestly

- **SW-CCMP + A-MPDU composition is demonstrated on-air (positive result).** The
  mesh was secured (§4.2), so the forced A-MPDUs were themselves CCMP-encrypted:
  the firmware aggregated **CCMP-protected** 4-address mesh QoS-Data (board1's
  QoS-Data was 100% `wlan.fc.protected = 1`). So the composition a *real* secured
  mesh needs — SW-CCMP encrypt each MPDU, *then* let the chip aggregate the
  ciphertext MPDUs into one PPDU — is already shown here, not deferred. Honest
  residual: aggregation depth was still modest (3–7 MPDUs) and this was an
  all-ESP mesh.
- **Modest aggregation depth (3–7 MPDUs).** Two likely limiters, neither a
  blocker: the **1 MHz S1G A-MPDU duration cap** (a 1 MHz PPDU can only be so
  long) and a **shallow TX queue** at these rates (few frames actually resident
  when a PPDU is built). Deeper aggregation would need higher offered load and/or
  wider bandwidth — a Stage-2/3 tuning question, not a go/no-go one.
- **Force ≠ handshake.** S0b bypassed the BA session. The measured throughput is
  therefore an *optimistic* proxy: a real deployment pays BA setup/teardown and
  the receiver actually reorders. It shows the ceiling exists and is reachable,
  not the steady-state number.

## 7. Bench method gotchas (reusable)

- **iperf `-b` wants a bare integer in Mbit/s.** `-b 2` is accepted; `-b 2M` is
  **rejected** by this iperf build.
- **`iw` is not on chronium's non-login-shell PATH.** Invoke it as `sudo iw …`
  from a non-interactive SSH command, or it is "command not found".
- **pyserial reset-on-open is flaky on these XIAO ESP32-S3 boards.** Drive the
  REPL with `dtr=True, rts=False` plus a long pre-send read delay to absorb any
  boot glitch; don't assume open() cleanly resets or cleanly *doesn't*.
- **No A-MPDU radiotap field on morse0.** Detect aggregation by **TSFT clustering
  + consecutive `wlan.seq`** (§4.4). This is the standing method for this sniffer.

## 8. Teardown (radio-silent)

- Temporary spike code reverted — morselib tree clean (the `umac_interface.c`
  probe `printf` and the `umac_datapath.c` force both removed).
- board0 + board1 reflashed to **`rimba-hello`** (radio-silent).
- chronium **`wlan1` down** (`ip link set wlan1 down`).
- **Nothing committed.**

## 9. Next steps (Stage 1, not started)

1. **Implement the host BA handshake** — route inbound `BLOCK_ACK` action frames
   to a BA state machine; originate ADDBA for a mesh peer/TID; tear down on
   timeout.
2. **Secured-mesh PMF exemption** so BA action frames are accepted on an
   AMPE-encrypted mesh.
3. **Secured bench under the real handshake** — re-run S0b's capture method with
   the actual BA handshake (not the force) to re-confirm encrypt-then-aggregate
   at depth; §6 already shows the composition on-air under the forced path.
4. **Depth/throughput tuning** — characterise aggregation depth vs offered load
   and bandwidth once the handshake is real (BA-gated, not forced).

Full staged plan and code-map:
[`../mesh-ap/rimba-mesh-ampdu-aggregation-design.md`](../mesh-ap/rimba-mesh-ampdu-aggregation-design.md).
</content>
</invoke>
