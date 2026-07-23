# 2026-07-22 — Mesh-gate port S4b (send_to_gates + prepare_for_gate): VERIFIED ON-AIR (direct-peer gate)

**Status:** S4b WRITTEN + COMPILES + **ON-AIR VERIFIED (PASS, ESP↔ESP, direct-peer gate)**. A node with no path
to an off-mesh destination reaches it through a discovered GATE by wrapping the frame as a 6-address AE_A5_A6
frame — the port of `net/mac80211` `mesh_path_send_to_gates` + `prepare_for_gate`. **Uncommitted** in
`components/halow`. **With S4a (MPP learning), S4 is complete.** ⚠ **Known limitation: multi-hop gates fail** (the
intermediate relay strips the AE endpoints) — a documented follow-up. *(HTML render: TODO.)*

## Goal

The TX-fallback half of S4 (S4a was the MPP learning / RX half). `send_to_gates`: when a node has no mesh path to a
destination that is off-mesh, reach it through a gate discovered in S2 (`known_gates`). `prepare_for_gate`: rewrite
the frame into a 6-address `AE_A5_A6` frame — mesh DA = the gate, eaddr1 (addr5) = the final off-mesh DA, eaddr2
(addr6) = the original source — and send it to the gate, which (S5) bridges eaddr1 off-mesh. Port of Linux
`mesh_pathtbl.c:969` (`mesh_path_send_to_gates`) + `:134` (`prepare_for_gate`), kernel 6.12.21.

## What was implemented — S4b (components/halow, uncommitted)

`umac_mesh.c` (+~65) + `umac_mesh.h` (+~12); combined S1–S4 = **576 ins** across `umac_mesh.c`/`.h`/`umac_datapath.c`:
- **`mmwlan_mesh_send_to_gates(final_dst, src, payload, len)`** — walks `mesh_paths[]` for `is_gate` (the S2 gate
  set), resolves the next hop to each gate (a directly-peered gate → itself; else the HWMP next hop), and sends a
  copy of the frame via each reachable gate as an `AE_A5_A6` frame (reusing the S3 `umac_mesh_build_forward` with
  `ae`): mesh DA = gate, eaddr1 = `final_dst`, eaddr2 = `src`, mesh SA = us. Keyed under the next-hop peer's pairwise
  MTK (== `umac_mesh_forward_data`). Guards: inactive / leaf (`!g_mesh_multihop`) / `g_mesh_num_gates==0` → false.
  This is `prepare_for_gate` (the AE rewrite) + `mesh_path_send_to_gates` (the fan-out) fused.
- **Fixtures:** a `GATE=1` build flag on `test-mesh-gate-rx` (`#ifdef MESH_GATE_MODE` → `set_root_announcements`,
  making it a gate that also AE-rx-probes) + a new `firmware/test-mesh-gate-fwd` (the node: learns a gate, then calls
  `send_to_gates` for a synthetic off-mesh dest). Makefile threads `GATE=` → `MESH_GATE_MODE`.

Compile-verified: `make build APP=test-mesh-gate-fwd` + `APP=test-mesh-gate-rx GATE=1` → exit 0.

## ✅ VERIFICATION — on-air, ESP↔ESP (PASS, direct-peer gate)

Bench: **board0** = GATE (`test-mesh-gate-rx GATE=1`, mesh MAC `e2:72:a1:f8:ef:a4`) emits gate RANNs + AE-rx-probes;
**board1** = NODE (`test-mesh-gate-fwd`, mesh MAC `e2:72:a1:f8:f9:40`) learns the gate then calls
`send_to_gates(final_dst=02:00:00:00:00:cc, src=02:00:00:00:00:dd)`. Both `rimba-smesh`, 1 dBm, directly peered.

- board1 (node): `send_to_gates: sent (known_gates=1)` — it learned the gate and the fallback fired.
- board0 (gate): `AE rx count=13  last eaddr1(DA)=02:00:00:00:00:cc  eaddr2(SA)=02:00:00:00:00:dd` +
  `MPP learned: 02:00:00:00:00:dd via e2:72:a1:f8:f9:40`.

The gate received exactly the AE frame `send_to_gates` built — final off-mesh DA `02:..:cc` as eaddr1, source
`02:..:dd` as eaddr2 — and learned `mpp(02:..:dd → board1)`. The full path works: **discover a gate (S2) → fall back
to it for an unreachable dest (S4b) → the gate receives a byte-correct proxied AE frame (S3) → learn the proxied
source (S4a).** A 3-lens adversarial workflow independently confirmed the frame construction is byte-correct vs
Linux (eaddr1←final DA, eaddr2←src, mesh_da=gate, correct next-hop keying) with no regression (send_to_gates is
purely additive, has no live datapath caller yet, gate walk bounded, gates-with-no-peer skipped).

**Radio-silent cleanup done:** board0 + board1 → `rimba-hello`; chronite + chronium `wlan1` already down.

## ⚠ Known limitation (surfaced by the adversarial workflow) — multi-hop gates

> **Update (same session): FIXED in S4c** — see `2026-07-22-mesh-gate-s4c-ae-relay-preserve.md` (the relay now
> preserves AE; multi-hop gates verified 3-node). The limitation below is the point-in-time state at S4b.

**`send_to_gates` only works end-to-end when the gate is a DIRECT PEER (1 hop).** For a gate 2+ hops away,
`send_to_gates` emits a correct AE frame toward the gate, but at the **intermediate relay** the RX datapath strips
the entire 18-byte AE Mesh Control (`umac_datapath.c` `mmpkt_remove_from_start(mctrl_len)`, `mctrl_len` includes the
12 eaddr bytes) and re-forwards via `umac_mesh_forward_data`, which builds a **non-AE** 4-address frame (`.ae`
never set). So the gate at the far end receives a plain frame with the proxied endpoints GONE and cannot bridge it.
This diverges from Linux, where an intermediate node forwards the skb verbatim (mesh header + eaddrs preserved).
- **Root:** an S3-era relay-path gap (the relay never preserved AE — S3 only did origin TX + RX extract), newly
  *exercised* by S4b's multi-hop-gate branch. NOT a flaw in `send_to_gates`'s frame construction.
- **Fix (follow-up, "S4c"):** make the RX relay branch preserve/re-emit AE — thread the extracted eaddr1/eaddr2
  (already parsed at `umac_datapath.c`) through `umac_mesh_forward_data` (add optional `ae`/eaddrs) so a relayed AE
  frame stays AE. Needs a **3-node line topology** (gate ← relay ← node) to verify on-air — deferred rather than
  landed unverified. The bench used a direct-peer gate, which is correct + verified.

## Minor notes (non-blocking, from the workflow)
- **consbuf max-MTU:** an AE frame is 12 B larger; `consbuf_append` `MMOSAL_ASSERT`s on overflow. Pre-existing
  (identical to the S3 `send_ae_test` path, bounded by the mmpkt reserve) — worth a max-MTU case in the fixture.
- **MESH_PATH_ACTIVE divergence:** the direct-peer branch sends to an ESTAB-peered gate even if `mpath->active`
  is false, where Linux requires `MESH_PATH_ACTIVE`. Benign + more permissive for a 1-hop peer (an S5 byte-diff nit).

## Next step
- **S4c (small):** preserve AE through the relay (fixes multi-hop gates). Then **S5** — the gate L2 bridge: on the
  gate, deliver a received AE frame's payload to eaddr1 off-mesh (the AP side) + proxy AP-client MACs into the mesh;
  retire the hard-coded `MESH_GATE_IP`. Then **S6** — full live-Linux interop (needs a healthy chronite mesh).

## Files
- `components/halow/.../umac/mesh/umac_mesh.c` (S1–S4, ~576 ins total with `.h`/`umac_datapath.c`), `.../umac_mesh.h`,
  `.../umac/datapath/umac_datapath.c` — **uncommitted**.
- `firmware/test-mesh-gate-fwd/` (the node), `firmware/test-mesh-gate-rx/` (+ `GATE=1` gate mode), `Makefile`
  (`GATE=` → `MESH_GATE_MODE`).
- `docs/mesh-ap/rimba-mesh-ap-mesh-gate-discovery-design.md` — design (S1–S3 marked done; S4a/S4b here).
- Memory: `mesh-gate-8021s-port-planned` (updated: S4 done, multi-hop-gate limitation + S5 next).
