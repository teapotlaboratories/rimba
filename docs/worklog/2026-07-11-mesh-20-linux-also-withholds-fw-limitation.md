# 2026-07-11 — #20 RESOLVED: the A4≠TA HW-crypto withhold is a UNIVERSAL MM6108 firmware limitation (Linux withholds too), not a morselib bug

Bench session (full all-ESP + Linux access) that finally **resolves** backlog item #20 — "in HW-crypto
mode the MM6108 firmware withholds from the host a forwarded 4-address 802.11s mesh data frame whose
A4 (mesh-SA) ≠ TA (immediate transmitter)". The whole prior investigation framed this as a *morselib*
difference from Linux (`morse_driver` supposedly delivers the forward; morselib withholds it). **That
premise is false.** A direct bench test proves **Linux `morse_driver` withholds the identical forward**,
so #20 is a property of the **firmware**, present on both stacks. morselib was following Linux correctly
all along.

## Bottom line
1. **#20 reproduces with a REAL multi-hop forward** (not just the 2026-07-02 synthetic one): a 3-ESP
   forced line delivers `board1→board2` end-to-end **61/61** under SW-CCMP and **0/76** under HW-crypto —
   same rig, only `g_mesh_sw_crypto` flipped. Peering, key-install, and the direct (A4==TA) links all
   work in HW-crypto (relay `estab_peers=2`); ONLY the relayed A4≠TA forward fails.
2. **The command interface is fully exonerated — empirically.** morselib's on-wire command stream was
   captured; it sends `BSS_CONFIG → BSSID_SET → BSS_BEACON_CONFIG → MESH_CONFIG(START)` (BSS before MESH,
   the one ordering difference vs Linux). **Reordering morselib to emit `MESH_CONFIG(START)` first
   (matching Linux) did NOT fix it** — still 0/70. Command *content* was already byte-identical (prior
   sessions), so content **and** order are both ruled out.
3. **DECISIVE: Linux withholds the A4≠TA forward too.** Mixed forced-topology rig
   `board1(ESP) → board0(ESP relay) → chronite(LINUX endpoint, morse_driver, HW-crypto)`. On-air, board0
   transmitted **10** A4≠TA forwards to chronite (`TA=board0 RA=chronite SA=board1`); chronite's kernel
   received **0** of them — while chronite↔board0 **direct** ping ran **40/40, 0% loss**. Linux's firmware
   withholds identically to the ESP's.
4. **Therefore SW-CCMP is the permanent, universal answer.** With no FW mesh key installed the firmware
   delivers protected mesh frames raw and the host decrypts (keyed by TA), bypassing the A4 gate for all
   origins — the shipping morselib default (`g_mesh_sw_crypto = true`). HW-crypto (FW-offloaded CCMP)
   mesh **multi-hop** is impossible on this firmware, on Linux as well as the ESP. This corrects the
   `2026-06-27-mesh-security-phase3-sae.md` §25 claim that "the same FW HW-decrypts + delivers the A4≠TA
   forward on Linux" — that must have been a non-forced topology where the nodes were direct peers (so the
   "forward" was actually A4==TA).

## 1. Premise validation — #20 with a real forward (all-ESP)
App `firmware/rimba-halow-mesh-perf` in `MESH_LINE_RELAY_DEMO`: forced 3-ESP line via the peer allowlist
(`mmwlan_mesh_set_peer_allowlist`) — board0 = relay (allowlist {board1, board2}); board1 = pinger
(allowlist {board0}) → pings board2 (10.9.9.108); board2 = responder (allowlist {board0}). Both ping legs
are A4≠TA forwards through board0. Secured mesh (`MMWLAN_MESH_SEC_PHASE1=1`, SAE+AMPE+CCMP). Boards by
efuse MAC: board0 `…ef:a4` (ttyACM0), board1 `…f9:40` (ttyACM1), board2 `…f0:08` (ttyACM4, PPK2-powered).
Morse fw 1.17.8.

| crypto mode (`umac_mesh.c:146`) | board1→board2 multi-hop |
|---|---|
| SW-CCMP (`g_mesh_sw_crypto=true`, shipping) | **61/61 replies**, ~40 ms |
| HW-crypto (`g_mesh_sw_crypto=false`) | **0 replies / 76 timeouts** |

Control (HW-crypto): relay board0 `estab_peers=2` (both endpoints), board1 `estab_peers=1` — peering +
key-install + direct A4==TA links all succeed; only the relayed A4≠TA forward fails. The synthetic-frame
concern from 2026-07-02 is closed: a genuine relayed forward is withheld.

## 2. Command-order capture + reorder experiment (REFUTED)
Instrumented `morse_cmd_tx` (`driver/morse_driver/command.c`) with an `esp_rom_printf` per command (a raw
`printf` there STACK-OVERFLOWS morselib's task ctx → boot loop; `esp_rom_printf` is stack-safe — declare
`extern int esp_rom_printf(const char*,...)` ABOVE the `#ifdef ENABLE_DRVCMD_TRACE`). Captured board1 from
reset (ESP32-S3 native USB: pyserial open does NOT reset — pulse RTS `setDTR(False);setRTS(True);
sleep;setRTS(False)`):

  `SET_CHANNEL → ADD_INTERFACE → GET_CAPABILITIES → … → BSS_CONFIG(0x0006) → BSSID_SET(0x0052) →
   BSS_BEACON_CONFIG(0x003d) → MESH_CONFIG(0x0039) → CONFIG_PS`

So morselib sends BSS_CONFIG/BSSID **before** MESH_CONFIG(START); Linux sends MESH_CONFIG(START) first
(via `ieee80211_start_mesh`), then BSS/BSSID via `bss_info_changed`. Patched `mmwlan_mesh_start` to move
BSS_CONFIG + BSSID_SET to AFTER MESH_CONFIG(START); new order confirmed on-wire
(`BSS_BEACON_CFG → MESH_CONFIG → BSS_CONFIG → BSSID_SET`). **Result: still 0/70, peering still fine.**
The one genuine host difference (order) is not the cause.

## 3. The decisive Linux-endpoint test
To ask "does Linux deliver an A4≠TA forward in HW-crypto?" without needing Linux forced-topology support,
use ESP allowlists to force a mixed line:
- board0 (ESP relay): allowlist {board1, **chronite** `3c:22:7f:37:51:38`}.
- board1 (ESP pinger): allowlist {board0} ONLY → board1 **rejects** a direct peering with chronite, so
  `board1→chronite` MUST relay via board0. Static ARP (10.9.9.50 → chronite MAC) makes the ping unicast
  immediately (no broadcast-ARP confound). board1 pings chronite 10.9.9.50.
- chronite: joined the ESP secured mesh `rimba-mesh` (SAE `rimbamesh2026`, ch27) via
  `wpa_supplicant_s1g` (`ssid="rimba-mesh"`, else = `docs/reference/captures/wpa-smesh.conf`), IP
  10.9.9.50. Default `morse_driver` = **HW-crypto** (offloads CCMP keys).

Forced topology verified on chronite: `iw station dump` → board0 **ESTAB**, board1 **BLOCKED**; `mpath
dump` → board1 reachable via next-hop board0, **HOP_COUNT 2**.

**Observable = chronite's own reception** (a raw `AF_PACKET` socket on wlan1 — tcpdump isn't installed on
chronite; needs `sudo`). A mesh vif presents decapsulated 802.3 frames, so the sniffer sees exactly what
the firmware hands to the kernel:

- **Positive control:** chronite → board0 (direct peer, A4==TA) ping = **40/40, 0% loss**; the sniffer saw
  all 39 of board0's echo replies (10.9.9.136) → sniffer works, board0↔chronite MTK + HW-crypto path work.
- **board1 (10.9.9.100) forwards: 0 IP frames** reached chronite's kernel in 20 s.
- **On-air (chronium monitor, `morse0`):** board1→board0 first-hop x12 (`TA=board1 RA=board0 SA=board1`,
  A4==TA) **and board0→chronite x10 (`TA=board0 RA=chronite SA=board1`, A4≠TA)** — board0 IS transmitting
  the forward. So the frames are on air, chronite hears board0 (direct works), yet its kernel gets none.

⇒ **chronite's firmware withholds the A4≠TA forward after HW-decrypt — the same behavior as the ESP.**
Because a SW-crypto Linux node would deliver it (raw, host-decrypt), chronite withholding also confirms it
was genuinely in HW-crypto. The gate is the firmware's post-decrypt "SA(A4) must equal the key-owner (TA)"
behavior, and it is identical on both host stacks.

## 4. Method / reproduction + bench-safety notes
- morselib TEMP (all reverted): `g_mesh_sw_crypto=false`; `esp_rom_printf` cmd trace in `morse_cmd_tx`;
  the `mmwlan_mesh_start` reorder. App TEMP (reverted): `MESH_LINE_RELAY_DEMO` retargeted to a chronite
  endpoint + static ARP + peer-count heartbeat.
- Boards identified by efuse MAC via IDF esptool (ports re-enumerate); board2 powered via
  `tools/ppk2_hold.py`. Build/flash: `make {build,flash} APP=rimba-halow-mesh-perf BOARD=proto1-fgh100m
  PORT=/dev/ttyACMx`.
- chronium (Pi5 sniffer) is power-marginal on the monitor-mode switch under load — browned out+rebooted
  once early, then stable; keep captures short. morse0 S1G frames: parse radiotap len at bytes[2:4], then
  the standard 802.11 MAC header (4-addr when toDS&&fromDS: A1@4 A2@10 A3@16 A4@24).
- Radio-silence restored after: all 3 ESPs reflashed to `rimba-hello`; chronite mesh-leave + `wlan1 down`;
  chronium `morse0`/`wlan1 down`. All temp code git-reverted; `g_mesh_sw_crypto` back to `true`.

## 5. Disposition
#20 is **CLOSED as a firmware limitation**. No morselib change can make HW-crypto mesh multi-hop work;
SW-CCMP is the shipping and permanent answer for the ESP, and Linux mesh multi-hop is subject to the same
limitation under HW-crypto. Stop investigating #20 as a morselib/command/BCF/ordering difference (all
byte-identical + reorder empirically refuted). The move from HW→SW CCMP is not a regression to fix but the
required design for multi-hop on this firmware.
