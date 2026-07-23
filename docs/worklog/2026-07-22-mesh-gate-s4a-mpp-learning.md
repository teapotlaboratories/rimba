# 2026-07-22 — Mesh-gate port S4a (MPP proxy-path table + learning): VERIFIED ON-AIR

**Status:** S4a (the MPP learning / RX half of S4) WRITTEN + COMPILES + **ON-AIR VERIFIED (PASS, ESP↔ESP)**. A
node that receives a 6-address AE frame learns the off-mesh source into an MPP (Mesh Proxy Path) table —
`mpp(eaddr2 → mesh_sa)` — the exact port of Linux `mpp_path_add`. **Uncommitted** in `components/halow`.
**S4b (`send_to_gates` + `prepare_for_gate`, the TX fallback) is NOT done — the remaining half of S4.**
*(HTML render: TODO.)*

## Goal

Fourth stage of the approved 802.11s **Mesh-gate** port (`docs/mesh-ap/rimba-mesh-ap-mesh-gate-discovery-design.md`).
S3 gave the AE datapath primitive (build/parse 6-address frames + extract the proxied endpoints); S4 turns those
endpoints into routing state. **S4a (this worklog)** = the **MPP table + learning** on RX: record that off-mesh host
`eaddr2` is reachable via the mesh node that sent the frame (`mpp_path_add(eaddr2, mesh_sa)`). **S4b (next)** = the
TX side: `prepare_for_gate` (rewrite a frame as an AE frame via a gate) + `send_to_gates` (on a next-hop miss, fall
back to a known gate from the S2 `known_gates`). Port of `net/mac80211` `mesh_pathtbl.c` (kernel 6.12.21).

## What was implemented — S4a (components/halow, uncommitted)

`umac_mesh.c` (+~85 for S4a; combined S1–S4a = 441 ins) + `umac_datapath.c` (+3) + `umac_mesh.h` (+~10):
- **MPP table** — a compact `mpp_path_entry` array (`MESH_MPP_MAX`=32): `{ dst[6] (off-mesh host), mpp[6] (proxy
  mesh node), expiry_ms }`. Distinct from the mesh path table (node → next hop); the MPP table answers "which mesh
  node do I send to, to reach this off-mesh host". Reset per-MBSS in `mmwlan_mesh_start` (alongside the path table).
- **`umac_mesh_mpp_learn(dst, mpp)`** (== `mpp_path_add`, `mesh_pathtbl.c:722`) — find-or-add `dst`, set the proxy +
  expiry; guards: never proxy our own MAC or a multicast `dst` (`:729-733`); evicts the oldest slot when full.
- **`mmwlan_mesh_mpp_lookup(dst, mpp_out)`** (== `mpp_path_lookup`) — the S4b `send_to_gates` target select + the S4a
  on-air probe.
- **RX wiring** — the datapath AE-extract block now calls `umac_mesh_mpp_learn(eaddr2, mesh_sa)` (mesh_sa = addr4),
  the exact `net/mac80211` `mpp_path_add` on AE RX (`rx.c:2889`). The S3 `note_ae_rx` probe accessor is kept.
- **Probe poll** in `test-mesh-gate-rx`: after an AE rx, `mmwlan_mesh_mpp_lookup(eaddr2)` + log the proxy node.

Compile-verified: `make build APP=test-mesh-gate-rx BOARD=proto1-fgh100m` → exit 0.

## ✅ VERIFICATION — on-air, ESP↔ESP (PASS)

Reused the S3 rig: **board0** (`test-mesh-ae`, `LINUX_MAC=e2:72:a1:f8:f9:40`, mesh MAC `e2:72:a1:f8:ef:a4`) injects
AE frames with `eaddr2_sa=02:00:00:00:00:aa` to **board1** (`test-mesh-gate-rx` receiver, mesh MAC
`e2:72:a1:f8:f9:40`); both `rimba-smesh`, 1 dBm.

board1 serial:
```
AE rx count=75 ... eaddr2(SA)=02:00:00:00:00:aa
MPP learned: 02:00:00:00:00:aa via e2:72:a1:f8:ef:a4
```
The MPP table learned **off-mesh host `02:00:00:00:00:aa` → proxy mesh node `e2:72:a1:f8:ef:a4` (board0)** — exactly
`mpp_path_add(eaddr2=proxied SA, mpp=mesh SA)`. `mmwlan_mesh_mpp_lookup(02:..:aa)` returns board0's MAC. The learning
is stable across every AE frame.

**Radio-silent cleanup done** ([[radio-silent-after-every-test]]): board0 + board1 → `rimba-hello`; chronite +
chronium `wlan1` already down from S3. (Footgun hit: two concurrent `make flash APP=rimba-hello` to different ports
raced on the shared `build/rimba-hello/` dir — ninja failed; reflash the SAME app SEQUENTIALLY.)

## Not done — S4b (`send_to_gates` + `prepare_for_gate`, the TX fallback)

The MPP table is currently **learned but not yet consumed** (inert until a TX path uses it). S4b adds:
- **`prepare_for_gate(frame, gate)`** (`mesh_pathtbl.c:134`) — rewrite an ordinary mesh frame into an `AE_A5_A6`
  frame via a gate: eaddr1 = the original mesh DA (final off-mesh dest), eaddr2 = the original mesh SA (source),
  addr3 = the gate, addr1 = the next hop to the gate. The S3 AE build (`umac_mesh_build_forward` with `ae`) is the
  reusable primitive.
- **`send_to_gates`** (`mesh_pathtbl.c:969`) — on a next-hop resolution miss for a non-mesh dest, walk the S2
  `known_gates` (paths with `is_gate`) and emit the frame via a gate instead of dropping it. Hook: the mesh TX
  next-hop resolution (`umac_mesh_lookup_next_hop` miss) + the datapath TX for the node's own off-mesh traffic.
- **Verify:** a node with a known gate (S2) tries to reach an off-mesh dest → the frame goes out as an AE frame to
  the gate (observe on the gate: `mmwlan_mesh_ae_rx_probe` / mpp learning). Needs a gate + off-mesh-dest scenario.
- **⚠** S4b touches the datapath TX (regression-sensitive, the mesh+AP concurrency path) and intertwines with S5's
  routing (the node's default route toward the mesh). Watch the FW A4≠TA withhold
  ([[gateway-e2e-forward-mesh-unicast-gap]]) — though S3 already proved the FW delivers 6-addr AE frames.

## Footguns
- **CCMP is AE-transparent** (established in S3): the AE eaddrs are encrypted body; the AAD is MAC-header-only;
  `ccmp.c` untouched. Do NOT re-derive CCMP for AE.
- **morselib MMLOG is not on the ESP UART** — expose an `mmwlan_*` probe accessor + poll via `ESP_LOGI`
  (`mmwlan_mesh_mpp_lookup` / `mmwlan_mesh_ae_rx_probe`).
- **Don't concurrently `make flash` the same APP to two ports** — the shared `build/<APP>/` dir races; do it
  sequentially (or use per-board build dirs).
- **chronite mesh is flaky** (S3): `type mesh point` with no channel; a driver reload can hang. The MPP-learning
  verify used ESP↔ESP and didn't need a Linux node.
- Re-`scp` the Linux ref from `chronium:/home/chronium/halow/rpi-linux/` each session (kernel 6.12.21).

## Files
- `components/halow/.../umac/mesh/umac_mesh.c` (+441 S1–S4a), `.../umac_mesh.h` (+~63),
  `.../umac/datapath/umac_datapath.c` (+16) — **uncommitted**.
- `firmware/test-mesh-ae/` (AE sender), `firmware/test-mesh-gate-rx/` (receiver: AE + MPP probe polls).
- `docs/mesh-ap/rimba-mesh-ap-mesh-gate-discovery-design.md` — design (S1–S3 marked done; S4a here; §3 table re-pin
  pending).
- Memory: `mesh-gate-8021s-port-planned` (updated: S4a done, S4b remaining).
