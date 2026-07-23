# 2026-07-22 — Mesh-gate port S5a (AE-rx-to-app bridge hook): VERIFIED ON-AIR + the S5 design

**Status:** S5a (the foundational morselib primitive for the gate's mesh→AP bridge leg) WRITTEN + COMPILES +
**ON-AIR VERIFIED (PASS, ESP↔ESP)**. The app now receives a received 6-address AE frame's proxied endpoints +
payload — which the plain mesh RX ext-cb cannot see. **Uncommitted** in `components/halow`. **The rest of S5 (the
gate-app L2 bridge itself) is designed below but NOT implemented — it needs a multi-node mesh+AP+STA bench.**
*(HTML render: TODO.)*

## Context — what S5 is, and the gap S5a fills

S1–S4 built the full 802.11s mesh-gate **discovery + AE datapath**: RANN gate discovery (S1/S2), the 6-address
`AE_A5_A6` datapath (S3), MPP learning + `send_to_gates` + multi-hop AE relay (S4a/b/c). S5 makes the gate actually
**BRIDGE** off-mesh traffic — replace the current **L3 `ip_forward`** gateway (`rimba-halow-mesh-ap`: a mesh netif
`10.9.9.x` + an AP netif `192.168.12.1`, two subnets) with an **L2 AE-proxy bridge** (single subnet): the gate
proxies AP-client MACs into the mesh (AE, client as eaddr2) and delivers mesh→AP-client AE frames to the AP.

**The critical gap S5a fills:** the gate's mesh RX comes via `mmwlan_register_rx_pkt_ext_cb(MMWLAN_VIF_STA, …)`,
which delivers the frame **after** morselib strips the Mesh Control — so the gate app **cannot see eaddr1/eaddr2**
of a proxied frame, and can't decide to deliver it to its AP side. S5a adds a callback that recovers them.

## What was implemented — S5a (components/halow, uncommitted, +~46 lines)

- **`mmwlan_mesh_register_ae_rx_cb(cb, arg)`** + `typedef mmwlan_mesh_ae_rx_cb_t(eaddr1, eaddr2, payload,
  payload_len, arg)` — the app registers one callback; the datapath fires it for every received AE frame with the
  proxied endpoints + the post-strip payload (`[LLC/SNAP][L3]`).
- **Datapath wiring** (`umac_datapath.c`): right after the AE Mesh Control is stripped, `umac_mesh_ae_rx_deliver`
  invokes the registered cb. **Notification only** — the normal relay/local-delivery path is unchanged (additive,
  no regression). (Reuses the `rx_ae`/`rx_eaddr1`/`rx_eaddr2` locals S4c already captures.)

Combined S1–S5a diff: **655 insertions** across `umac_mesh.c` / `.h` / `umac_datapath.c`. Compile-verified.

## ✅ VERIFICATION — on-air, ESP↔ESP (PASS)

board0 (`test-mesh-ae`, → board1) injects AE frames (`eaddr1=02:..:bb`, `eaddr2=02:..:aa`, payload = 8-byte
LLC/SNAP + 20 B = 28 B); board1 (`test-mesh-gate-rx`) registers `ae_rx_cb` and logs it:
```
AE-rx CB: eaddr1(DA)=02:00:00:00:00:bb eaddr2(SA)=02:00:00:00:00:aa payload_len=28 payload[0..3]=aaaa0300
```
The app received the full proxied frame — both endpoints AND the payload (28 B, SNAP header `aa aa 03 00`), exactly
what was sent. This is the primitive the gate needs to deliver a proxied frame to its AP side (`payload` → eaddr1).

**Radio-silent cleanup done:** board0/1/2 → `rimba-hello`; chronite + chronium `wlan1` down.

## The rest of S5 — designed, NOT implemented (needs a mesh+AP+STA bench)

The current gateway (`rimba-halow-mesh-ap`, read this session) is **L3**: `mmwlan_ap_enable` (secondary vif) + a 2nd
`esp_netif` (AP, 192.168.12.1) + per-vif RX ext-cbs (`gw_mesh_rx_cb`/`gw_ap_rx_cb` → `gw_rx_deliver` → lwIP
`ip_forward`). The mesh+AP concurrency APIs (`mmwlan_ap_enable`, `mmwlan_register_rx_pkt_ext_cb`, `MMWLAN_VIF_AP`)
ARE present in the current 2.12.3 tree, so S5 is not blocked on branch reconciliation (but the concurrency should be
re-confirmed working on 2.12.3 — it was proven on the older `feat/mesh-ap-concurrency` branch).

- **S5b — gate egress (mesh AE → AP), the direct use of S5a.** In the gate app's `ae_rx_cb`: if `eaddr1` is one of
  our associated AP clients, build an 802.3 frame (`dst=eaddr1`, `src=eaddr2`, `payload`) and inject it onto the AP
  vif (via the AP netif TX / a raw AP-vif TX with `md.vif=MMWLAN_VIF_AP`) instead of delivering locally. Needs the
  AP's associated-STA set (from `ap_sta_status_cb`). Verify: a mesh node sends an AE frame with `eaddr1` = a real AP
  client's MAC → the client receives the payload. **Needs a STA client under the gate's AP.**
- **S5c — gate ingress (AP → mesh) + node de-hardcode.** In `gw_ap_rx_cb`: an AP client `C`'s frame (802.3
  `src=C dst=D`) is proxied into the mesh as an AE frame (`eaddr2=C`, `eaddr1=D`, `mesh_da` = the mesh node owning
  `D` via the MPP table / else `send_to_gates`). Replace the L3 `ip_forward` with this L2 proxy; retire the
  hard-coded `MESH_GATE_IP` (`rimba-halow-mesh/app_main.c`) — nodes reach off-mesh hosts by MAC via MPP, single
  subnet. Needs a new morselib "inject an AE mesh frame with a given payload + eaddrs + dest" API (the S3
  `mmwlan_mesh_send_ae_test` + S4b `send_to_gates` are the seed; generalise to a clean `mmwlan_mesh_tx_proxied`).
- **S6 — full live-Linux interop** (bidirectional gate↔node, `iw mpp dump`; needs a healthy chronite mesh).

**Bench for S5b/S5c:** the gate (mesh+AP, `rimba-halow-mesh-ap` + S5 bridge) + a STA client under its AP
(`rimba-halow-sta`) + a mesh node (`rimba-halow-mesh`), all one subnet. This is the mesh+AP-concurrency rig (task
#17, `[[gateway-e2e-forward-mesh-unicast-gap]]`) — board mapping by efuse MAC, not ttyACM.

## Footguns
- **The gate can't see AE eaddrs via the plain RX ext-cb** — that's why S5a's callback exists; use it for the
  mesh→AP leg (not `mmwlan_register_rx_pkt_ext_cb`).
- CCMP is AE-transparent (S3); morselib MMLOG isn't on the ESP UART (use `mmwlan_` accessors/callbacks); identify
  ESPs by efuse MAC (board2 was `ttyACM2` this session); don't concurrently `make flash` the same APP to 2 ports.
- Re-`scp` the Linux ref from `chronium:/home/chronium/halow/rpi-linux/` each session (kernel 6.12.21).

## Files
- `components/halow/.../umac/mesh/umac_mesh.c` + `.h` (`mmwlan_mesh_register_ae_rx_cb` + deliver),
  `.../umac/datapath/umac_datapath.c` (fires the cb) — **uncommitted** (S1–S5a = 655 ins).
- `firmware/test-mesh-gate-rx/` — receiver, now also registers the S5a `ae_rx_cb` (logs the proxied frame).
- Memory: `mesh-gate-8021s-port-planned` (updated: S5a done, S5b/S5c designed).
