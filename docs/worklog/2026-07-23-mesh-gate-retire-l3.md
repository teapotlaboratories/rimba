# 2026-07-23 — Mesh-gate: retire the L3 path → flat single-subnet L2 bridge (zero-config DHCP)

**Status:** ✅ **DONE + ON-AIR VERIFIED.** The all-ESP 802.11s mesh-gate is now a pure L2 bridge on ONE
flat `10.9.9.0/24` subnet. The old L3 router — a second `192.168.12.0/24` AP subnet + `CONFIG_LWIP_IP_FORWARD`
+ a `MESH_GATE_IP` return route — is deleted. An AP client is zero-config end-to-end: it DHCPs a `10.9.9.x`
from the gate and reaches any mesh node directly (the gate L2-bridges + proxy-ARP resolves), with no static
IP and no route. App-level only (no morselib change). Landed on branch `feat/mesh-gate-retire-l3`.

## Why

Proxy-ARP (2026-07-22) made the L2 bridge (S5b/S5c + B1/B2) zero-config for *resolution*, so the L3
`ip_forward` path + the separate AP subnet + `MESH_GATE_IP` became dead weight — they were the reason an AP
client had to sit on a different subnet and needed a static `10.9.9.x` or a gateway route to reach the mesh.
Collapsing to one flat subnet is the documented next step and simplifies the design before S6 (Linux interop).

## What changed (all app-level)

- **Gate `firmware/rimba-halow-mesh-ap`** — AP netif moved `192.168.12.1 → 10.9.9.1` (same flat subnet as
  the mesh); its DHCP server now hands out `10.9.9.x`. The default `dhcps` pool is small (≤ `LINK_MAX_STAS`
  leases from `10.9.9.2`), so it stays clear of the mesh nodes' `10.9.9.100–163` static range. Dropped the
  mesh-netif L3 gateway vestige (`gw = 0` now). All the L2 bridge machinery (`gate_ae_rx_cb`, `gw_ap_rx_cb`,
  `gw_mesh_rx_cb`, `gw_proxy_arp`, `gw_arp_announce_task`) and `gw_rx_deliver` (still needed for gate-local
  delivery — the DHCP server + ARP/ping to the gate) are unchanged. Header + logs re-worded L2.
- **`sdkconfig.defaults`** — removed `CONFIG_LWIP_IP_FORWARD=y` (+ the NAPT line). Verified off in the
  regenerated sdkconfig; the binary even shrank slightly.
- **STA `firmware/rimba-halow-sta`** — default is now a **DHCP client** (mmhalow's STA netif is
  `ESP_NETIF_DEFAULT_WIFI_STA`, AUTOUP + dhcpc, and mmhalow calls `action_connected` on link-up, so dhcpc
  auto-runs — the app just waits for the lease). Static IP is now opt-in via `-D TEST_STATIC_IP`
  (`make … STA_IP=a.b.c.d`) for an AP with no DHCP server (the plain rimba-halow-ap demo). `PING_IP` +
  `NO_PING` still work; CMakeLists forwards `TEST_STATIC_IP`/`TEST_PING_IP`/`TEST_NO_PING`.
- **Mesh node `firmware/rimba-halow-mesh`** — removed the opt-in `MESH_GATE_IP` return-route `#ifdef`
  (it only ever pinned `192.168.12.0/24 via gate`, now unneeded); nodes stay static `10.9.9.<100+lowbits>`
  with no gateway.
- **Makefile** — the STA's `L2_DST_MAC` flag replaced by `STA_IP` (→ `TEST_STATIC_IP`).

## ✅ VERIFICATION — on-air, 3-node (board0=gate, board1=mesh node .100, board2=STA)

board2 = STA `rimba-halow-sta` **default (DHCP client)**. board1 = plain mesh node (responder at
`10.9.9.100`). board0 = gate (retire-L3).

- **Gate boot:** `mesh + SoftAP L2-bridged on one MM6108 (flat 10.9.9.0/24)` · `AP netif up: 10.9.9.1/24 +
  DHCP server` · `mesh netif static IP 10.9.9.136 (L2-bridge gate host)` · **`L2 bridge ready: AP clients +
  mesh nodes share 10.9.9.0/24 (DHCP + proxy-ARP, no ip_forward)`** · `AP client joined: bc:2a:33:96:b2:33`.
- **STA boot (the zero-config proof):** `associated to "rimba-ping"` → **`DHCP lease 10.9.9.2 — zero-config
  on the flat mesh subnet`** → `pinging 10.9.9.100 …` → **`reply from 10.9.9.100 seq=1 time=46 ms`** and
  continuously (seq 1→35, ~1 drop). No static IP, no route: the STA leased `.2` (the first pool address,
  clear of the mesh `.100+` range) and reached the mesh node on the flat subnet from the FIRST ping.
- **Gate steady-state:** `S5b mesh->AP` + `S5c AP->mesh` both ways + `proxy-ARP push: refreshed 2 …` every
  3 s. No `ip_forward`, no abort/crash. `CONFIG_LWIP_IP_FORWARD` confirmed off at build time.
- **Two-netif coexistence (the one new risk):** RESOLVED — the AP netif (`10.9.9.1`) + mesh netif
  (`10.9.9.136`) share `10.9.9.0/24` with no misrouting; DHCP serves, the bridge carries, the STA
  re-associated + re-leased cleanly after a gate reset. The DHCP-single-subnet target worked; the
  static-client fallback was not needed.

Radio-silent cleanup done (`rimba-hello` to all three; board2 PPK2-powered off).

## Footguns hit

- **idf.py CMake-cache accumulation.** `make flash APP=x` (no flags) does NOT clear previously-set `TEST_*`
  cache vars — the first run flashed *stale* configs (STA still `TEST_STATIC_IP=10.9.9.50 TEST_NO_PING=1`,
  mesh still `TEST_PING_IP=10.9.9.50`). Fix: `rm build/<app>/<board>/CMakeCache.txt` before reflashing a
  changed test config. (The sdkconfig has the same trap — `rm …/sdkconfig` to re-apply changed defaults.)
- A CMakeLists that doesn't `target_compile_definitions` a `-D` var silently drops it — the STA's
  `TEST_STATIC_IP` did nothing until the CMakeLists forwarded it (`py_compile`-style "it built" ≠ "the
  branch was exercised"; confirm the macro reached `build.ninja`).

## Files
- `firmware/rimba-halow-mesh-ap/main/app_main.c` + `sdkconfig.defaults` — AP on `10.9.9.1` + DHCP,
  no ip_forward, L2-bridge docs.
- `firmware/rimba-halow-sta/main/app_main.c` + `CMakeLists.txt` — DHCP-client default, `TEST_STATIC_IP` opt-in.
- `firmware/rimba-halow-mesh/main/app_main.c` — dropped `MESH_GATE_IP`.
- `Makefile` — `STA_IP` flag.
- Memory: `mesh-gate-8021s-port-planned` (retire-L3 done — flat single subnet + zero-config DHCP).
