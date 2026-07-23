# 2026-07-22 — Mesh-gate port S5c (AP → mesh ingress): VERIFIED END-TO-END — the gate bridges BOTH ways

**Status:** S5c (the AP→mesh leg of the gate L2 bridge) WRITTEN + COMPILES + **ON-AIR VERIFIED (PASS, 3-node
mesh+AP+STA)**. The gate proxies an AP client's frame INTO the mesh as an AE frame, and the mesh node receives it +
learns the client's MPP. **With S5b, the gate now bridges BOTH directions.** New morselib primitive
`mmwlan_mesh_tx_proxied` (`components/halow` diff → 740 ins). Gate app `rimba-halow-mesh-ap` + `test-mesh-gate-rx` /
`rimba-halow-sta` / `Makefile` extended. *(HTML render: TODO.)*

## Goal

The AP→mesh direction of the gate L2 bridge (symmetric to S5b's mesh→AP): when an AP client `C` sends a unicast
frame to a MESH node `D`, the gate proxies it INTO the mesh as an `AE_A5_A6` frame (`eaddr2=C`, `eaddr1=D`), so `D`
receives the client's traffic and learns `mpp(C → gate)` for the return path. (Linux: the gate xmits the AP client's
frame into the mesh with AE.)

## What was implemented — S5c

**morselib (`components/halow`, uncommitted, +~85 lines):** `mmwlan_mesh_tx_proxied(final_dst, src, payload, len)`
— inject a proxied `AE_A5_A6` frame (`eaddr1=final_dst`, `eaddr2=src`, mesh SA=us) resolving where to send:
`final_dst` as a reachable mesh node (direct peer / HWMP path) → mesh DA=`final_dst`; else its MPP proxy (S4a) →
mesh DA=the proxy; else `send_to_gates` (S4b). Reuses the S3 `umac_mesh_build_forward` AE path + the next-hop keying
of `umac_mesh_forward_data`.

**Gate app (`rimba-halow-mesh-ap`, tracked — modified):** `gw_ap_rx_cb` now inspects the AP RX Ethernet frame — a
UNICAST frame whose dst is NOT the gate (`g_ap_bssid`/`g_mesh_mac`) is re-encapsulated `[dst][src][ethertype][L3]`
→ `[LLC/SNAP][L3]` and proxied via `mmwlan_mesh_tx_proxied(dst, src, …)`; frames to the gate (DHCP/ping) + broadcast
fall through to the existing L3 path. (`g_ap_bssid` stashed from `mmwlan_ap_get_bssid`.)

Combined S1–S5c morselib diff: **740 insertions** across `umac_mesh.c`/`.h`/`umac_datapath.c`. Compile-verified.

## ✅ VERIFICATION — on-air, 3-node mesh+AP+STA (PASS)

- **board0 = GATE:** `rimba-halow-mesh-ap` + S5b + S5c (MESH `rimba-mesh` MAC `e2:72:a1:f8:ef:a4` + AP `rimba-ping`).
- **board2 = STA:** `rimba-halow-sta L2_DST_MAC=e2:72:a1:f8:f9:40` — associated to the AP (MAC `bc:2a:33:96:b2:33`),
  plus a **static ARP** (fake IP `192.168.12.99` → board1's mesh MAC) it pings, so its frames leave with L2 dst =
  board1 (not the gate) — the L2-bridge trigger.
- **board1 = MESH DEST:** `test-mesh-gate-rx MESH_ID=rimba-mesh` — peers with the gate + has the S5a `ae_rx_cb`.

Gate serial: `S5c AP->mesh: 100 B from bc:2a:33:96:b2:33 to e2:72:a1:f8:f9:40` (every 2 s).
board1 serial:
```
AE-rx CB: eaddr1(DA)=e2:72:a1:f8:f9:40 eaddr2(SA)=bc:2a:33:96:b2:33 payload_len=100 payload[0..3]=aaaa0300
MPP learned: bc:2a:33:96:b2:33 via e2:72:a1:f8:ef:a4
```
The STA's frame (L2 dst = board1) was **delivered by the AP to the gate's `gw_ap_rx_cb`** (resolving a key unknown —
the AP forwards a STA's non-BSSID unicast to the host), **proxied into the mesh** (`eaddr2`=the STA, `eaddr1`=board1,
100 B SNAP+ICMP intact), and board1 **learned `mpp(STA → gate)`**. So the gate bridged an AP client's frame INTO the
mesh, and the mesh node can now reach the client for return traffic.

**Both directions verified:** S5b (mesh AE → AP client) + S5c (AP client → mesh) + MPP learning tie the return path
together. The **core L2-bridge datapath is complete.**

**Radio-silent cleanup done:** board0/1/2 → `rimba-hello`; chronite + chronium `wlan1` down.

## Not done — the L2-bridge finishing touches
- **A true bidirectional round-trip** (STA ↔ mesh node ping through the bridge, both ways). The datapath is all
  present (S5b+S5c+MPP); it needs the mesh node to hold the STA's target IP + reply (its reply uses `mpp(STA→gate)`
  → `mmwlan_mesh_tx_proxied`/`send_to_gates` → the gate → S5b to the STA). The current ICMP is to `192.168.12.99`
  which board1 doesn't own, so it drops it — a real round-trip needs the single-subnet IP plan below.
- **Single subnet + retire `MESH_GATE_IP`** (`rimba-halow-mesh/app_main.c`): put AP clients + mesh nodes on one
  subnet so reachability is by MAC (via MPP), no L3 gw. Replace the L3 `ip_forward` with the L2 proxy as the primary
  path (it currently coexists).
- **Proxy-ARP / broadcast bridging:** a real ARP-driven flow (vs the static ARP used here) needs the gate to bridge
  broadcast/multicast AP↔mesh — the **multicast AE (AE_A4)** path, which was out of S3 scope. That's the last datapath
  piece for a zero-config L2 bridge.
- **S6:** live-Linux interop.

## Footguns
- The AP DOES deliver a STA's unicast-to-non-BSSID frame to `gw_ap_rx_cb` (S5c relies on it — confirmed on-air).
- `gw_ap_rx_cb` copies the frame + `mmpkt_release`s it on the proxy path (owns it); the local path hands it to
  `gw_rx_deliver` (which releases). Don't double-free.
- STA AP-MAC = the MM6108 MAC (`bc:2a:33:96:b2:33`), read from the gate `AP client joined` log.
- `s_ap2mesh`/`s_ae2ap` static buffers are safe (single mesh-RX task); `mmwlan_mesh_tx_proxied` copies the payload.

## Files
- `components/halow/.../umac/mesh/umac_mesh.c` + `.h` (`mmwlan_mesh_tx_proxied`) — **uncommitted** (S1–S5c = 740 ins).
- `firmware/rimba-halow-mesh-ap/main/app_main.c` — S5b egress + S5c ingress (tracked, **modified**).
- `firmware/test-mesh-gate-rx` (+ `MESH_ID=`), `firmware/rimba-halow-sta` (+ `L2_DST_MAC=` static-ARP ping),
  `firmware/test-mesh-ae` (+ `MESH_ID=`/`EADDR1=`), `Makefile` (threads the flags) — untracked/modified.
- Memory: `mesh-gate-8021s-port-planned` (updated: S5c done, both bridge legs verified).
