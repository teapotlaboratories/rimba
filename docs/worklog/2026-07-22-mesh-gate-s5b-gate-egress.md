# 2026-07-22 — Mesh-gate port S5b (gate egress: mesh AE → AP client): VERIFIED ON-AIR

**Status:** S5b (the mesh→AP leg of the gate L2 bridge) WRITTEN + COMPILES + **ON-AIR VERIFIED (PASS, 3-node
mesh+AP+STA)**. The gate delivers a received proxied AE frame onto its AP vif toward the target client — using the
S5a hook. **Also confirmed: mesh+AP concurrency works on 2.12.3.** App-level (no morselib change; the `components/
halow` diff stays 655 ins from S5a). **Gate app `rimba-halow-mesh-ap` modified (tracked) + `test-mesh-ae`/`Makefile`
extended.** *(HTML render: TODO.)*

## Goal

The mesh→AP direction of the gate L2 bridge (S5b): when the gate receives a proxied `AE_A5_A6` frame whose `eaddr1`
(final DA) is one of its associated AP clients, deliver the payload onto the AP vif addressed to that client — the
direct use of the S5a `mmwlan_mesh_register_ae_rx_cb` hook. (The AP→mesh ingress + retiring the L3 `ip_forward` is
S5c.)

## What was implemented — S5b (firmware/rimba-halow-mesh-ap, tracked — modified)

- **AP-client set:** `ap_sta_status_cb` now maintains `g_ap_clients[]` (add on `AUTHORIZED`, swap-remove on
  `UNKNOWN`) + `gate_is_ap_client(mac)`.
- **`gate_ae_rx_cb`** (registered via `mmwlan_mesh_register_ae_rx_cb`): if `eaddr1` is one of our AP clients,
  recover the ethertype from the AE payload's LLC/SNAP (`aa aa 03 00 00 00 <ET>`), build an Ethernet II frame
  `[dst=eaddr1][src=eaddr2][ethertype][L3]`, and inject it onto the AP vif (`mmwlan_alloc_mmpkt_for_tx` +
  `mmwlan_tx_pkt` with `md.vif=MMWLAN_VIF_AP`; non-blocking, runs in the mesh RX task). Coexists with the existing
  L3 `ip_forward` (a proxied frame to an AP client takes the AP-inject path; other mesh RX still reaches the mesh
  netif). morselib re-adds LLC/SNAP + CCMP-encrypts to the client on the AP downlink.

**Test fixture:** `test-mesh-ae` extended with build-time `MESH_ID=` (default `rimba-smesh`) + `EADDR1=` (the
proxied final-DA MAC) overrides (Makefile `MESH_ID=`/`EADDR1=` → `TEST_MESH_ID`/`TEST_EADDR1` + CMake propagation),
so a mesh node can point at the gate app (`rimba-mesh`) with `eaddr1` = the gate's AP client MAC.

## ✅ VERIFICATION — on-air, 3-node mesh+AP+STA (PASS)

- **board0 = GATE:** `rimba-halow-mesh-ap` + S5b — MESH (`rimba-mesh`, MAC `e2:72:a1:f8:ef:a4`) **+** SoftAP
  (`rimba-ping`, BSSID `6a:24:99:44:6b:b7`, 192.168.12.1 + DHCP) **CONCURRENT**.
- **board2 = STA:** `rimba-halow-sta` — associated to `rimba-ping` (MAC `bc:2a:33:96:b2:33` — the MM6108 MAC, not
  the efuse) and actively pinging the gate (`reply from 192.168.12.1`), so the AP data path to it is proven.
- **board1 = MESH NODE:** `test-mesh-ae MESH_ID=rimba-mesh LINUX_MAC=e2:72:a1:f8:ef:a4 EADDR1=bc:2a:33:96:b2:33` —
  peers with the gate's mesh vif and sends proxied AE frames to it (`eaddr1`=the STA, `eaddr2=02:..:aa`, SNAP+20 B).

Gate serial:
```
==> MESH vif up (primary).
==> AP vif up (secondary) — CONCURRENT with mesh.
AP client joined: bc:2a:33:96:b2:33 (1 total)
S5b mesh->AP: 34 B to AP client bc:2a:33:96:b2:33 (src 02:00:00:00:00:aa)   (repeating every ~3s)
```
The gate received each proxied AE frame → its `gate_ae_rx_cb` (S5a hook) fired → recognized `eaddr1` as an AP client
→ rebuilt a 34 B Ethernet frame (14 hdr + 20 L3, `src`=`02:..:aa`) → injected it onto the AP vif toward the STA
(`mmwlan_tx_pkt` SUCCESS). Since the AP↔STA data path is independently proven (the ping), the frame reaches the STA.
**The gate bridged a proxied mesh frame onto its AP side.**

**Key prerequisite confirmed:** the whole thing ran with the gate doing mesh + AP + a mesh peer + an AP client
simultaneously — **mesh+AP concurrency works on 2.12.3** with the S1–S5a datapath (the concurrency was previously
proven only on the older `feat/mesh-ap-concurrency` branch).

**Radio-silent cleanup done:** board0/1/2 → `rimba-hello`; chronite + chronium `wlan1` down.

## Not done — S5c (AP → mesh ingress + retire the L3 gw) + full bidirectional bridge

- **S5c:** in `gw_ap_rx_cb`, proxy an AP client `C`'s frame (802.3 `src=C dst=D`) INTO the mesh as an AE frame
  (`eaddr2=C`, `eaddr1=D`, `mesh_da`=the MPP-owner-of-`D` (S4a) / else `send_to_gates`). Needs a clean morselib
  "inject an AE mesh frame with payload + eaddrs + dest" API (seed: `mmwlan_mesh_send_ae_test` + `send_to_gates`
  → generalise to `mmwlan_mesh_tx_proxied`). Replace the L3 `ip_forward` with the L2 proxy; retire the hard-coded
  `MESH_GATE_IP` (`rimba-halow-mesh/app_main.c`) — single subnet, nodes reach off-mesh hosts by MAC via MPP.
- **Fuller S5b proof (optional):** a STA-side raw-RX observer (register `mmwlan_register_rx_pkt_ext_cb(VIF_STA)` on
  `rimba-halow-sta`) to log the received frame — the current proof is gate-side inject SUCCESS + the proven AP data
  path. A true bidirectional data test (AP client ↔ mesh node ping) needs S5c (the return path).
- **S6:** live-Linux interop.

## Notes / footguns
- The STA's AP-side MAC is the **MM6108 MAC** (`bc:2a:33:96:b2:33`), not board2's efuse MAC — read it from the
  gate's `AP client joined` log, not `read_mac`.
- `gate_ae_rx_cb` runs in the mesh RX task; the AP inject is non-blocking (no `mmwlan_tx_wait_until_ready`) to not
  stall RX. The AE payload MUST be LLC/SNAP-encapsulated to recover the ethertype (real mesh data is).
- mesh+AP concurrency works on 2.12.3 (feasibility gate for S5, now cleared).

## Files
- `firmware/rimba-halow-mesh-ap/main/app_main.c` — S5b gate egress (tracked, **modified**).
- `firmware/test-mesh-ae/` (+ `MESH_ID`/`EADDR1` overrides) + `Makefile` (threads them) — untracked/modified.
- `components/halow` — unchanged from S5a (655 ins; S5b is app-level).
- Memory: `mesh-gate-8021s-port-planned` (updated: S5b done, S5c next).
