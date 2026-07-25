# 2026-07-24 — S6 P2 + P3 (Case B + byte-diff sign-off): **S6 COMPLETE**

**Status:** ✅ **DONE + ON-AIR PROVEN — S6, the last mesh-gate stage, is COMPLETE.** A live Linux 802.11s
node discovers the **production** ESP gate (Part 0's RANN), learns an ESP-proxied source over the AE datapath,
and the production gate's RANN is **byte-identical to a live Linux gate's RANN**. With P1 (Case A) already
proven, the all-ESP 802.11s mesh-gate now interoperates with mainline Linux `net/mac80211` **both
directions**.

## Setup

`rimba-mesh` (the production gate's mesh), 3-node + sniffer:
- **board0** = ESP gate `rimba-halow-mesh-ap` (mesh MAC `e2:72:a1:f8:ef:a4`, emits RANN via Part 0).
- **chronite** = Linux node/gate (`3c:22:7f:37:51:38`), `wpa_supplicant_s1g` on `wpa-rmesh.conf`
  (ssid `rimba-mesh`, SAE `rimbamesh2026`), gate mode layered at runtime.
- **board1** = `test-mesh-ae MESH_ID=rimba-mesh LINUX_MAC=3c:22:7f:37:51:38` (the AE injector).
- **chronium** = `morse0` monitor (ch27 / freq 5560) — the byte-diff sniffer.

`wpa-rmesh.conf` was derived from `wpa-smesh.conf` (`sed ssid → rimba-mesh`). Whole bench fw **1.17.8**.
chronite was power-cycled by the owner after P1 wedged it — healthy for this run.

## P2a — Case B discovery: a Linux node discovers the ESP gate's RANN (gold-standard on-air)

chronite peered with board0 (`mesh plink: ESTAB`, −27 dBm) and recorded board0's RANN SN in its mpath.
The definitive proof, captured on chronium — **chronite re-floods board0's gate RANN**:

| field | board0 (ESP gate, origin) | chronite (Linux, re-flood) | note |
|---|---|---|---|
| SA | `e2:72:a1:f8:ef:a4` | `3c:22:7f:37:51:38` | each node's own MAC |
| rann_addr | `e2:72:a1:f8:ef:a4` | `e2:72:a1:f8:ef:a4` | **preserved** — chronite propagates board0's root |
| flags | `0x01` (IS_GATE) | `0x01` (IS_GATE) | **preserved** |
| hop / ttl | `0 / 31` | `1 / 30` | standard propagation transform |
| rann_seq | 60/62/64/66 | 60/62/64/66 | **preserved** — one re-flood per received RANN |
| metric | `0` | `2410` | link metric accumulated |

`action_hex`: origin `0d017e1501001fe272a1f8efa4…8813000000000000`, re-flood
`0d017e1501011ee272a1f8efa4…881300006a090000` — the only deltas are hop/ttl/metric, i.e. a **byte-perfect**
`hwmp_rann_frame_process`. So a mainline Linux stack **accepts, records-as-gate, and re-propagates** Part 0's
production-gate RANN. (The exact mirror of S2, where the ESP re-flooded a Linux gate's RANN.)

## P2b — the AE/MPP leg (the S3-deferred Linux leg, now closed)

board1 (`test-mesh-ae`) peered with chronite (`linux_peered=1`, `estab_peers=2`) and injected `AE_A5_A6`
frames every 3 s (mesh DA=chronite; proxied `eaddr1=02:..:bb`, `eaddr2=02:..:aa`). On chronite:
```
$ iw dev wlan1 mpp dump
DEST ADDR         PROXY NODE        IFACE
02:00:00:00:00:aa e2:72:a1:f8:f9:40 wlan1
```
Linux `mpp_path_add` parsed the ESP's on-wire AE frame and learned the **ESP-proxied source** (`02:..:aa`)
via the ESP (board1). So the ESP's `AE_A5_A6` encoding (`umac_mesh_build_forward`) is Linux-parseable — the
S3-deferred bonus (blocked back then by chronite's flaky mesh) is done.

## P3 — byte-diff sign-off

**RANN both directions** — the production ESP gate vs a live Linux gate, both as originators (hop=0),
machine-checked with the S1 normalized mask (rann_addr + rann_seq → `00`):
```
ESP    (norm): 0d017e1501001f 000000000000 00000000 8813000000000000
Linux  (norm): 0d017e1501001f 000000000000 00000000 8813000000000000   → BYTE-IDENTICAL: True
```
Every invariant field matches (cat `0d`, act `01`, eid `7e`, len `15`, **flags `01`**, hop `00`, ttl `1f`,
interval `8813`=5000, metric `0`); the only raw differences are rann_addr (each gate's own MAC) + rann_seq.
This re-confirms the S1 result now against the **production** gate app.

**Gate bit** — mesh beacon Formation Info, both sides:
```
board0 (ESP):    formation_info=0x05 → connected_to_gate(bit0)=1  peer_count=2
chronite (Linux): formation_info=0x05 → connected_to_gate(bit0)=1  peer_count=2   → byte-identical
```
Both set "Connected to Mesh Gate" once they know a gate; the ESP's byte matches Linux.

**AE** cannot be monitor-diffed (CCMP-encrypted body) → the `iw mpp dump` checks (P1's datapath + P2b) are
the proof. **Code-map** was already re-verified (P0, 2026-07-24). Verification tier: **live-device gold
standard** across the board.

## S6 status — COMPLETE

| Stage | Proof |
|---|---|
| Part 0 — production gate emits RANN | RANN `flags=0x01` on-air ≡ S1; round-trip intact; ESP node `known_gates=1` |
| P0 — code-map re-pin | whole §3 table re-verified (morselib `8a70405c` + Linux `372414fd4`) |
| P1 — Case A (Linux gate → ESP node) | off-mesh DS ↔ ESP 14/14 through a Linux gate's L2 bridge; Linux `mpp` learned the ESP-proxied source |
| P2 — Case B (ESP gate → Linux node) | Linux re-floods the ESP gate's RANN; Linux `mpp` learns an ESP AE source |
| P3 — byte-diff sign-off | RANN ≡ live Linux gate; gate bit ≡ Linux |

## Regression coverage — S6 is now codified in the harness

S6 interop was manual; it now has an automated T2 test so a re-run gives the same verdict. Added
**`mesh-gate-linux`** (S6 Case B / discovery): the harness brings a Linux node up as a `rimba-smesh`
**gate** over ssh, flashes an ESP reporter, and asserts the ESP **learned that node as a gate via RANN**
(`mmwlan_mesh_gate_count() > 0`). PASS = peered + `known_gates>0`; FAIL = peered but never learned the gate
(a real RANN-RX interop regression); INCONCLUSIVE = no plink (gate down / RF). Pieces:
- `firmware/test-mesh-gate-linux/` — new reporter fixture (derived from `test-mesh-linux`; `LINUX_MAC` is a
  build arg; emits `TEST|RESULT`).
- `linux_peer.bring_up_gate(host)` = `bring_up_mesh` + the gate recipe (`iw set mesh_param
  mesh_hwmp_rootmode 4 / mesh_gate_announcements 1 / mesh_hwmp_rann_interval 5000`).
- `t2_onair`: a `mesh-gate` `linux_setup` dispatch + `LINUX_MAC` build-var wiring (no `LINUX_IP` — no ping).
- `manifest.APPS` + `t2_tests.MESH_GATE_LINUX` (registered in `T2_TESTS`).

**Bench-verified GREEN:** `make test-lint` clean, `make test-unit` 46/46, the fixture builds, and
`make test-t2 TEST=mesh-gate-linux LINUX_HOST=chronite LINUX_MAC=3c:22:7f:37:51:38 LINUX_IP=10.9.9.2` →
**PASS 124.9 s** (board2 peered the chronite gate + `known_gates>0`), then the harness auto-silenced board2
and tore down the Linux gate. (This is the discovery leg; the AE/MPP leg + the RANN byte-diff stay manual —
they need `iw mpp dump` / a chronium-monitor capture the harness doesn't automate yet.)

## Cleanup (radio-silent, complete)
board0/board1 → `rimba-hello`; board2 off; chronite mesh killed + `wlan1` down; chronium monitor down +
`type managed`; chronogen down (from P1). All Pi management links intact.

## Files
- `docs/reference/captures/wpa-rmesh.conf` — **new**: the `rimba-mesh` (non-`smesh`) secured wpa config for a
  Linux node on the production gate's mesh (worth committing next to `wpa-smesh.conf`; currently only in the
  session scratchpad + on chronite `/tmp`).
- Memory: `mesh-gate-8021s-port-planned` (S6 COMPLETE).

## Next
- **Ship:** Part 0 (`feat/gate-emit-rann`) + the S6 doc/fixture changes — held for the work-hours commit rule
  (do after 17:00; the owner commits). The whole 802.11s mesh-gate port is now feature-complete + live-Linux
  interop-verified.
