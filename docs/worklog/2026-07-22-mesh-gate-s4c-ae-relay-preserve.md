# 2026-07-22 — Mesh-gate port S4c (preserve AE through the relay): VERIFIED ON-AIR (multi-hop gate)

**Status:** S4c WRITTEN + COMPILES + **ON-AIR VERIFIED (PASS, 3-node line topology)**. An intermediate relay now
re-emits a 6-address `AE_A5_A6` frame with its proxied endpoints INTACT, so a **multi-hop gate** receives the
`eaddr1`/`eaddr2` — fixing the limitation the S4b adversarial workflow surfaced. **Uncommitted** in
`components/halow`. With S4a+S4b+S4c, **S4 works for multi-hop gates.** *(HTML render: TODO.)*

## Goal

Fix the multi-hop-gate gap found by the S4b verification: `send_to_gates` emits a correct AE frame toward a gate,
but for a gate 2+ hops away the **intermediate relay** stripped the AE Mesh Control (`umac_datapath.c`
`mmpkt_remove_from_start(mctrl_len)` removes the 18-byte header incl. the 12 eaddr bytes) and re-forwarded via
`umac_mesh_forward_data`, which built a **non-AE** frame (`.ae` never set) — so the far gate lost the proxied
endpoints. Linux forwards the mesh header verbatim. S4c makes the relay preserve/re-emit AE.

## What was implemented — S4c (components/halow, uncommitted, +~33 lines)

- **`umac_mesh_forward_data` gains AE params:** signature is now
  `umac_mesh_forward_data(mesh_da, mesh_sa, bool ae, eaddr1, eaddr2, payload, len)` — when `ae`, the rebuilt frame
  carries `MESH_FLAGS_AE_A5_A6` + `eaddr1`/`eaddr2` (via the S3 `umac_mesh_build_forward` `ae` path). The mesh SA
  (addr4) was already preserved end-to-end; only addr2/TA changes per hop, matching 802.11s.
- **RX relay branch (`umac_datapath.c`):** the AE flag + `eaddr1`/`eaddr2` are captured into `rx_ae`/`rx_eaddr1`/
  `rx_eaddr2` at the Mesh-Control extraction (same place S3/S4a already parse them) and passed to
  `umac_mesh_forward_data` on relay. A non-AE frame relays exactly as before (`ae=false`, eaddrs ignored).

Combined S1–S4c diff: **609 insertions** across `umac_mesh.c` / `.h` / `umac_datapath.c`. Compile-verified.

## ✅ VERIFICATION — on-air, 3-node forced line topology (PASS)

New fixture `firmware/test-mesh-gate-relay` (symmetric, role-by-MAC, forced line via peer allowlist + filtered
peer-open — modeled on `test-mesh-relay`): **NODE (board0) ↔ RELAY (board1) ↔ GATE (board2)**, all `rimba-smesh`,
1 dBm. The GATE floods a gate RANN → the RELAY re-floods it (S2) → the NODE learns the 2-hop gate + a path via the
RELAY → the NODE `send_to_gates(final_dst=02:..:cc, src=02:..:dd)` → the frame goes NODE→RELAY→GATE.

Serial:
- **NODE (board0):** `send_to_gates: sent (known_gates=1)` — learned the 2-hop gate + sending.
- **RELAY (board1):** `role=RELAY peers=2 gates=1 ae_rx=49` — the middle of the line (both neighbours), relaying.
- **GATE (board2):** `role=GATE peers=1 ... ae_rx=13  last AE: eaddr1(DA)=02:00:00:00:00:cc eaddr2(SA)=02:00:00:00:00:dd`.

The gate, **two hops away**, received the AE frame with the proxied endpoints **intact** — `eaddr1=02:..:cc` +
`eaddr2=02:..:dd`, exactly what the NODE's `send_to_gates` set. Before S4c the gate's `ae_rx` would stay 0 (the
relay strips AE → the gate gets a plain frame). Receiving the exact eaddrs after the relay proves S4c preserves AE
end-to-end. Full chain verified: **S2 (RANN flood + PREQ path via the relay) → S4b (send_to_gates) → S4c (AE
survives the relay) → S3 (gate parses AE) → S4a (gate learns mpp).**

**Radio-silent cleanup done:** board0/1/2 → `rimba-hello`; chronite + chronium `wlan1` down.

## Notes
- The relay reconstructs a fresh Mesh Control (new mesh seqnum, `ttl=HWMP_ELEMENT_TTL`) with the eaddrs preserved —
  the mesh-ttl-not-decremented behaviour is pre-existing (applies to all relayed frames, not S4c); a per-hop mesh
  TTL decrement is a separate faithfulness item (loop safety is via HWMP path-selection + the RMC for group frames).
- `board2` was found on **`ttyACM2`** this session (not `ttyACM4`) — identify ESPs by efuse MAC, ports re-enumerate.

## Next step — S5 (gate L2 bridge)
S1–S4 give the full discovery + AE datapath: RANN gate discovery, MPP learning, `send_to_gates` (direct + multi-hop).
S5 makes the gate actually BRIDGE: on the gate, deliver a received AE frame's payload to `eaddr1` off-mesh (the AP
side) and proxy AP-client MACs into the mesh (AE frames with the client as `eaddr2`); consume the MPP table (S4a)
for return traffic; retire the hard-coded `MESH_GATE_IP`; replace the two-netif `ip_forward`
(`rimba-halow-mesh-ap/app_main.c`) with the L2 bridge. Then **S6** — full live-Linux interop (needs a healthy
chronite mesh; it was flaky this session).

## Files
- `components/halow/.../umac/mesh/umac_mesh.c` + `.h` (`umac_mesh_forward_data` AE params),
  `.../umac/datapath/umac_datapath.c` (relay preserves AE) — **uncommitted** (S1–S4c = 609 ins).
- `firmware/test-mesh-gate-relay/` — the 3-role line-topology fixture (untracked).
- Memory: `mesh-gate-8021s-port-planned` (updated: S4 complete incl. multi-hop).
