# 2026-07-10 — DHCP de-hardcode of the Mesh-gate (task #5): VERIFIED DONE

This is **task #5** of the mesh-gate PR follow-ups (the interim, keeps-L3 de-hardcode). It is
**complete and hardware-verified** — a cold-boot bench run passed every check, with the headline
artifact (`DHCP lease 192.168.12.2 gw 192.168.12.1 …`) captured on the STA console.

## What shipped (branch `feat/mesh-gate-end-to-end`, uncommitted — held for review)
Three apps carry the task-#5 edits (`git diff`):
- **`firmware/rimba-halow-mesh-ap/main/app_main.c`** — `gw_setup_ap_netif()` now runs a **DHCP server**
  on the AP netif: `ESP_NETIF_DHCP_SERVER | ESP_NETIF_FLAG_AUTOUP`, and the AP IP **and gw**
  (`192.168.12.1/24`) go in the **inherent config** (a function-local `static esp_netif_ip_info_t ap_ip`
  set at runtime before `esp_netif_new`). The old manual `esp_netif_set_ip_info` + the
  `action_connected`-then-`set_ip_info` sequence is removed.
- **`firmware/rimba-halow-sta-perf/main/app_main.c`** — `net_task()` converted from a static
  MAC-derived IP to a **DHCP client**: `esp_netif_dhcpc_start` (idempotent — tolerates
  `ALREADY_STARTED` since mmhalow starts a dhcpc on link-up) + a 40×500 ms (20 s) wait-for-lease loop
  that logs `DHCP lease <ip> gw <gw> mask <m>`. (`#define IPERF 1` is orthogonal and unchanged: it puts
  the STA in console mode so the end-to-end `ping 10.9.9.100` can be driven over serial; `net_task`
  runs and logs the lease in either mode.)
- **`firmware/rimba-halow-mesh/main/app_main.c`** — cosmetic: the return-gw literal `10.9.9.136` is now
  `#define MESH_GATE_IP` (documented deployment param; behaviour identical).

## Why the earlier attempt boot-looped (fixed)
First attempt set the DHCP_SERVER flag but left the inherent `ip_info = {0}`. With AUTOUP the AP netif
came up with IP `0.0.0.0`, so esp_netif auto-started dhcps on `0.0.0.0` → `dhcpserver.c` `ip4_addr_isany`
→ **"could not obtain pcb"**; then the manual `ESP_ERROR_CHECK(esp_netif_set_ip_info)` aborted with
**`0x5007 ESP_ERR_ESP_NETIF_DHCP_NOT_STOPPED`** → Guru Meditation boot loop. Fix = mirror stock
WIFI_AP: put the real IP+gw in the inherent config so dhcps auto-starts with a valid server IP, and
never call `set_ip_info` afterward. `CONFIG_LWIP_DHCPS=y` + `MAX_UDP_PCBS=16` were already set.

## Source analysis (done before the bench cycle — predicted PASS, no step-3 fix needed)
The one open question was whether the STA would get a **default gateway** (router option) or just an IP.
Traced through the vendored ESP-IDF (v5.4.2) and confirmed it is offered automatically:
- `components/lwip/apps/dhcpserver/dhcpserver.c`: `dhcps_new()` sets `dhcps_offer = 0xFF` (all options
  on) → router option (opt 3) enabled by default; its value = the netif **gw** field (`get_ip_info`
  reads `netif->gw`, offered only if `gw != 0.0.0.0`) — lines ~168, ~443-455, ~190-196.
- `components/esp_netif/lwip/esp_netif_lwip.c`: `esp_netif_init_configuration` memcpies the whole
  inherent `ip_info` incl. **gw** (~line 623); `netif_add` installs `ip_info->gw` onto the lwIP netif
  (~line 925); the DHCP-server start path passes the real server IP `192.168.12.1` to `dhcps_start`
  (~line 1135) → no isany/pcb failure, and there is no later `set_ip_info` → no `DHCP_NOT_STOPPED`.
- Because the code sets `ap_ip.gw = 192.168.12.1`, the OFFER carries router = `192.168.12.1`.
  **Prediction: STA gets `192.168.12.x` + `gw 192.168.12.1`; the fallback of explicitly enabling the
  router option is unnecessary.** The bench confirmed this exactly.

## Verification — cold-boot bench run (PASS, 2026-07-10)
Rig: gateway `rimba-halow-mesh-ap` on ACM0 (MAC `…ef:a4` → mesh `10.9.9.136` + AP `192.168.12.1`),
board1 `rimba-halow-mesh` on ACM1 (MAC `…f9:40` → mesh `10.9.9.100`), board2 STA `rimba-halow-sta-perf`
on ACM4 (PPK2-powered, MAC `…f0:08`). Boards mapped by efuse MAC, not ttyACM. All three built for
`proto1-fgh100m`, flashed, then **cold-booted simultaneously** via esptool and captured from t≈0 (a
plain pyserial open does NOT reset these XIAO USB-JTAG boards — only an esptool/RTS reset does; the
first, warm-boot capture missed the one-shot boot lines for that reason). Verifier +
`cold_ttyACM{0,1,4}.txt` logs are in the session scratchpad. All 10 checks green:

- **Gateway** — no crash / no boot-loop / no "could not obtain pcb" / no `0x5007`; heartbeats climb
  (`alive uptime=5s…30s mesh_peers=1 ap_stas=1`); `AP netif up: 192.168.12.1/24 + DHCP server`;
  `DHCP server started on interface HALOW_GWAP`; `esp_netif_lwip: DHCP server assigned IP to a client,
  IP is: 192.168.12.2`; `mesh netif static IP 10.9.9.136`; `gateway ready`.
- **board1** — `mesh static IP 10.9.9.100 gw 10.9.9.136 (netif up=1) — responder`.
- **STA** — `STA link up`; **`DHCP lease 192.168.12.2 gw 192.168.12.1 mask 255.255.255.0 (up=1)`**
  (the headline deliverable: zero-config STA got IP + gateway + mask from the gate's DHCP server).
- **End-to-end** — `ping -c 10 10.9.9.100` from the STA console → `10 packets transmitted, 10 received,
  0% packet loss`, every reply `ttl=63` (single gate `ip_forward` hop). RTT ~33-85 ms.

Boot-ordering held on a simultaneous cold start: the gate's DHCP server was up and leased `.2` to the
STA within ~7 s, the mesh peered (`mesh_peers=1`), and the route was 10/10 — a stronger result than a
warm re-test.

**On-air verification: N/A (not skipped, inapplicable).** This task adds no port-authored radio frame.
DHCP DISCOVER/OFFER/REQUEST/ACK are stock ESP-IDF lwIP UDP over the already-on-air-verified AP data
path, not a hand-derived mesh/beacon/action IE — so the "byte-diff vs a live Linux device" gold
standard has nothing rimba-authored to check. Verification is end-to-end functional (lease + 10/10 ping).

## Honest scoping note (carried through)
DHCP cleanly de-hardcodes the **STA** (client) — that is task #5's real deliverable, plus the mesh-node
`#define` tidy. It does **not** de-hardcode the **mesh node's** gw: that would need DHCP-over-mesh,
which is invasive (the mesh netif is created inside shared `mmhalow` as a *client* netif; a server
there means editing shared code) and is **superseded by the planned 802.11s port** (task #1), which
de-hardcodes the mesh node properly via RANN + learned MPP + an L2 bridge. If full DHCP-over-mesh is
ever wanted as an interim, that is a separate `mmhalow` change to raise. Note also the STA already
MAC-auto-derived its IP and used the well-known AP gw, so DHCP is marginal *value* there — but it is
the correct zero-config mechanism and removes the last STA-side hardcode.

## Task order (this done; remaining follow-ups)
**#5 DHCP (DONE)** → **#2 broadcast/multicast forwarding across the gate** (lwIP `ip4_forward` drops
LL bcast/mcast via `ip4_canforward`, so mDNS/discovery won't traverse; may be subsumed by the
L2-bridge) → **#3 wider coverage** (multiple STAs / multi-hop STA→gate→relay→node / soak /
power-save-behind-gate) → **#4 per-stad ops dispatch** (deferred, blocked on #3 finding a failure) →
**#1 full 802.11s L2-bridge + MPP mesh-gate port** (design + Linux code-map ready:
`docs/mesh-ap/rimba-mesh-ap-mesh-gate-discovery-design.md`; RANN+IS_GATE+learned MPP, NOT literal
GANN/PXU; ~12-19 days). Memory `[[mesh-gate-8021s-port-planned]]`.

## Already shipped / don't redo
The end-to-end route itself is DONE + in two PRs: **`teapotlaboratories/mm-esp32-halow#21`** (submodule
feature) + **`teapotlaboratories/rimba#31`** (gateway app + the two fixes: board1 gw `10.9.9.1→10.9.9.136`,
and `gw_rx_deliver` copy-into-PBUF_RAM for forwardable RX). Memory `[[gateway-e2e-forward-mesh-unicast-gap]]`.

## Bench rig + gotchas
board0 gateway (ACM0, efuse `e0:72:a1:f8:ef:a4`) / board1 mesh node (ACM1, `…f9:40`) / board2 STA
(ACM4 when PPK2-powered, `…f0:08`). **Map by efuse MAC not ttyACM** (PPK2 = ACM2+ACM3, don't flash).
IDF venv python `/home/quartz/.espressif/python_env/idf5.4_py3.13_env/bin/python`. **A plain pyserial
open does NOT reset these boards** (correcting an earlier note) — the board keeps running from its last
flash/reset; to capture a fresh boot log, reset via esptool (`--before default_reset --after
hard_reset`) then open the port. `make flash` rebuilds first (build separately to avoid a 2-min shell
timeout / OOM). Matched Morse fw 1.17.8 (BCF `mf16858`, morselib 2.10.4) confirmed on-board this run.
Radio-silenced after the test: `rimba-hello` ×3 + `ppk2_hold.py off` (board2 dark). **Do not `git
commit` during weekday 09:00-17:00 local; never auto-commit** — changes held in the working tree.
