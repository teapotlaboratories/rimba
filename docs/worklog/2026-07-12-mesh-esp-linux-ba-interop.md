# Worklog — 2026-07-12 — ESP↔Linux mesh Block Ack interop (the mesh-ID unlock)

**Author:** Aldwin
**Phase:** 802.11s mesh throughput — A-MPDU aggregation (S1 cross-vendor validation)
**Goal:** on a live bench, confirm that a **Linux** 802.11s mesh peer accepts the ESP's
**now-unprotected (MFP=no)** Block-Ack ADDBA and completes a BA session with it — the
one S1 follow-up that had been blocked across two prior attempts.
**Status:** **RESOLVED.** ESP↔Linux mesh peering works; the blocker was a **mesh-ID
mismatch**, not S1G params or MFP. Captured the explicit unprotected **NDP ADDBA** handshake
on `morse0`: ESP→Linux ADDBA **Request** (dialog token `0x69`, `fc.protected=0`) answered by
Linux→ESP ADDBA **Response** (matching token `0x69`, `fc.protected=0`). Under load the ESP
aggregates CCMP QoS-Data to the Linux peer (~1.8 Mbit/s). Bench radio-silent after; nothing
committed.

This entry is **standalone**: the root cause, the exact config, every address/frame, and the
bench gotchas needed to re-run it are here.

---

## 1. Why this was blocked

S1 made the ESP mesh send its Block-Ack management (ADDBA/DELBA) **unprotected** (MFP=no) to
match net/mac80211, which runs mesh peers MFP=no (see
[`2026-07-11-mesh-ampdu-s1-blockack-rx-routing.md`](2026-07-11-mesh-ampdu-s1-blockack-rx-routing.md)).
The open question was interop: **does a real Linux mesh peer accept that unprotected ADDBA?**
Two earlier attempts failed to even form the ESP↔Linux peering — "board0 never heard chronite's
beacons, PEER MESH BEACON never fired" — and were written off as an S1G-param detail plus
late-night bench instability (Pi-Zero brown-outs, a Pi-5 reboot mid-test).

## 2. Root cause — a mesh-ID mismatch, not S1G params or MFP

The ESP mesh app beacons **`MESH_ID = "rimba-mesh"`** (`rimba-halow-mesh-perf`,
`app_main.c:34`). The Linux **secured-mesh reference config** (`wpa-smesh.conf`, the *all-Linux*
4-node mesh) uses **`ssid = "rimba-smesh"`**. `rimba-mesh` ≠ `rimba-smesh` — a **different
802.11s mesh**. So the ESP heard chronite's S1G beacons at the PHY but dropped them as a
**foreign mesh ID**, and never opened a peer link. That is exactly the "never heard the
beacons" symptom — a one-character config mismatch, **not** an S1G BW/`sae_pwe`/beacon issue,
and **not** the MFP policy (MFP=no did not "fix" or "break" peering; it was never reached).

## 3. Rig

```
 board0 (ESP32-S3 + MM6108)                 chronite (Raspberry Pi 5 + MM6108 HAT)
 rimba-halow-mesh-perf (MESH_LINUX_INTEROP)  wpa_supplicant_s1g, ssid "rimba-mesh"
 /dev/ttyACM0                                wlan1 = mesh point
 mesh IP 10.9.9.136                          mesh IP 10.9.9.2
 mesh MAC e2:72:a1:f8:ef:a4  <== S1G ch27 ==> HaLow MAC 3c:22:7f:37:51:38
                    secured (SAE + AMPE), MFP=no, freq 5560 (915.5 MHz)

 chronium (Pi 5, dedicated sniffer): wlan1 -> monitor, freq 5560, morse0 up
```

- **ESP side:** `rimba-halow-mesh-perf` built with `MESH_LINUX_INTEROP` (open peering — no
  allowlist — plus an auto-ping to chronite `10.9.9.2` and the iperf console).
- **Linux side (chronite):** the interop config is the secured-mesh recipe **with the SSID
  changed to match the ESP**:

  ```
  ctrl_interface=/var/run/wpa_supplicant_s1g
  country=US
  user_mpm=1
  network={
      ssid="rimba-mesh"            # <-- MUST match the ESP's MESH_ID
      mode=5                       # WPAS_MODE_MESH
      key_mgmt=SAE
      sae_password="rimbamesh2026"
      country="US"
      op_class=68                  # S1G global op-class, ch27 / 1 MHz
      channel=27
      s1g_prim_chwidth=0
      s1g_prim_1mhz_chan_index=0
      dtim_period=1                # required (mesh join bails otherwise)
      beacon_int=100
  }
  ```
  Brought up with `wpa_supplicant_s1g -B -D nl80211 -i wlan1 -c /tmp/wpa-interop.conf`, then
  `ip addr add 10.9.9.2/24 dev wlan1`.

## 4. Result — peering + the unprotected ADDBA handshake

### 4.1 Peering

With the mesh IDs matched, board0 heard chronite and peered immediately:

- board0 console: `reply from 10.9.9.2: seq=1..45 time=7–16 ms` — **ping 45/45**.
- chronite `iw dev wlan1 station dump`: peer `e2:72:a1:f8:ef:a4` → `mesh plink: ESTAB`,
  `authenticated: yes`, **`MFP: no`** — i.e. a full SAE+AMPE peering, MFP=no on the Linux side,
  matching the ESP.

### 4.2 The unprotected NDP ADDBA handshake (chronium `morse0`, cold capture)

Reflashed board0 for a fresh BA state and captured from cold. The three Block-Ack
(category 3) action frames decode as:

| # | t (s) | dir | frame | dialog token | protected |
|---|---|---|---|---|---|
| 97 | 4.200 | board0 → chronite | **NDP ADDBA Request** (0x80), buffers 16, SSN 0 | `0x69` | **No** |
| 103 | 4.213 | chronite → board0 | **NDP ADDBA Response** (0x81), buffers 16 | `0x69` (matches #97) | **No** |
| 101 | 4.210 | chronite → board0 | NDP ADDBA Request (0x80), buffers 64, SSN 1 | `0x01` | **No** |

- **The headline:** board0 sends an **unprotected** ADDBA Request (token `0x69`), and chronite
  answers with an **unprotected** ADDBA **Response** carrying the **same token `0x69`** — a live
  Linux mesh peer **accepts the ESP's now-unprotected ADDBA**. This directly validates the S1
  MFP=no change cross-vendor.
- **Bidirectional:** frame #101 is chronite opening its **own** BA session to board0 (token
  `0x01`) — Linux originates a reverse-direction session too.
- **Both sides use NDP ADDBA** (S1G `0x80`/`0x81`), settling the design doc's open question
  "NDP vs compressed BlockAck" — the ESP and Linux agree on the NDP variant.

### 4.3 Aggregation under load

Driving `iperf -c 10.9.9.2 -u -b 2` from board0, chronium captured board0's QoS-Data to
chronite: **10 aggregated PPDUs** (2 MPDUs each), consecutive sequence numbers (e.g.
843/844, 847/848, 849/850), DA/RA = chronite, all **`fc.protected = 1` (CCMP)**. Throughput
**~1.78 Mbit/s** (peak 1.90). So **SW-CCMP + A-MPDU compose against a Linux peer too**, not just
ESP↔ESP. (chronite's `station dump` counted ~200 KB rx from board0.)

## 5. Bench gotchas (reusable)

- **Mesh IDs must match** exactly. The ESP is fixed at `MESH_ID = "rimba-mesh"`; the Linux
  *secured-mesh* reference is `rimba-smesh`. For ESP interop, put the Linux node on `rimba-mesh`.
- **NetworkManager re-grabs `wlan1`** after a reboot (`Device or resource busy` on `set type
  monitor`/mesh). Release it first: `sudo nmcli dev set wlan1 managed no`.
- **chronite's SSH rides `wlan0`** (brcmfmac), managed by NetworkManager's own
  `wpa_supplicant`. Do **not** `pkill -9 wpa_supplicant` (kills the SSH path); touch only
  `wlan1` and start a separate `wpa_supplicant_s1g` on it.
- **The ESP's ADDBA is an S1G NDP action** (`wlan.fixed.action_code` 0x80/0x81), still
  `wlan.fixed.category_code == 3`; tshark decodes it as "NDP ADDBA Request/Response".
- **The dev host rebooted mid-session** (tmpfs `/tmp` wiped — helper scripts + archived pcaps
  lost, though the findings live here), and **board2's PPK2 DUT-rail power dropped** on the
  reboot. Neither affected the Linux nodes.

## 6. Teardown (radio-silent)

- board0 + board1 reflashed to `rimba-hello`; chronite `wpa_supplicant_s1g` killed + `wlan1`
  down; chronium `morse0`/`wlan1` down. The `MESH_LINUX_INTEROP` app toggle was reverted (the
  tree's `app_main.c` is back to the `MESH_IPERF` default). **Nothing committed.**
- This closes the S1 follow-up: the ESP's unprotected (MFP=no) Block-Ack is accepted by a live
  Linux mesh peer, and A-MPDU works over the ESP↔Linux link.
