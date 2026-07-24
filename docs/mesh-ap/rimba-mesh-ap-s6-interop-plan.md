# S6 — ESP mesh-gate ↔ live-Linux 802.11s interop: execution plan

**Status:** planned 2026-07-23 (not started). S6 is the **last** mesh-gate stage — the mandatory
live-Linux A/B (design doc `§S6`, `design.md:166-169`, `:237`). This doc is the execution playbook;
the design + code-map live in [`rimba-mesh-ap-mesh-gate-discovery-design.md`](rimba-mesh-ap-mesh-gate-discovery-design.md).

**Scope decisions (2026-07-23):**
1. **Full bidirectional datapath** — go all the way, incl. the Linux-side L2 bridge (the genuine unknown),
   not discovery-only.
2. **The ESP gate-emit change lands standalone first** (Part 0 below), as its own small PR, before S6.

## Where S6 stands (much of the live-Linux A/B is already banked)

| Stage | State | Live-Linux? |
|---|---|---|
| S1 — ESP RANN TX | ✅ done, **byte-identical to a live Linux gate** (chronite in gate mode, chronium monitor) | YES |
| S2 — ESP RANN RX + re-flood + gate-bit beacon | ✅ done (learned a live Linux gate) | YES |
| S3 — AE_A5_A6 datapath | ✅ done **ESP↔ESP** (receiver-decode, byte-exact eaddrs) | **deferred** (Linux leg blocked by chronite's flaky mesh at the time) |
| S4/S5 — MPP · send_to_gates · L2 bridge (both ways) · proxy-ARP | ✅ done ESP↔ESP | no |

So RANN **discovery** interop is largely proven; S6 is the remaining **datapath** interop + two gaps.

## Two load-bearing gaps (verified 2026-07-23)

1. **The production ESP gate app never advertises as a gate on-air.** `firmware/rimba-halow-mesh-ap`
   calls no `mmwlan_mesh_set_root_announcements` (only the `test-mesh-gate*` fixtures do). The ESP↔ESP
   L2 bridge works via **MPP learned from proxied data frames**, not RANN, so it never needed to. But
   **Case B (a Linux node discovering the ESP gate)** needs the gate to emit RANN+`IS_GATE`. → **Part 0.**
2. **The Linux-side L2 bridge datapath is unproven.** On this bench a Linux node was only ever a RANN
   *emitter* for the byte-diff — never bridged mesh↔DS. **Case A (an ESP node reaching an off-mesh host
   through a *Linux* gate)** requires building + validating that bridge; the repo has never done it, and
   whether the morse driver/FW permits bridging the mesh vif is unverified. → **P1, the wildcard.**

Also pending before S6 ships: the **§3 code-map re-pin** (the table still shows stale 2.10.4 line numbers;
`design.md:128-129`) — morselib anchors to `7d7f76ad` (2.12.3), Linux anchors to the bench `rpi-linux`
`372414fd4` (6.12.21). Per `[[porting-ships-verified-codemap]]`.

---

## Part 0 — Standalone gate-emit change (do FIRST, own PR)

Make the ESP gate a real, discoverable 802.11s gate.

- **Code:** in `firmware/rimba-halow-mesh-ap/main/app_main.c`, after `mmwlan_mesh_start(...)` (and before
  or after AP enable), call:
  ```c
  mmwlan_mesh_set_root_announcements(/*root_rann=*/true, /*is_gate=*/true, /*interval_ms=*/5000);
  ```
  Rationale: `5000` matches the Linux bench gate default + the ESP fixtures. **The RANN interval is a RAW
  numeric on the wire (no ms→TU convert), so both sides must use the same value** (S1 footgun,
  `mesh_rann_cap.py`; `2026-07-21-mesh-gate-s1-rann.md:100`). Requires `g_mesh_multihop` true (the setter is
  gated on it) — confirm the gate app enables multihop.
- **Why it's additive/safe:** the ESP↔ESP bridge uses MPP-from-proxied-frames, so RANN emission doesn't
  change that datapath; it only *adds* standards gate discovery. Verify no regression to the retired-L3
  round-trip.
- **Bench verify (board0=gate, board1=mesh node, board2=STA):** (a) the gate emits RANN — capture on
  chronium `morse0` with `tools/mesh_rann_cap.py`, confirm `rann_flags=01` (IS_GATE); (b) the existing
  zero-config round-trip (STA DHCP → ping mesh node) still works; (c) an ESP mesh node reports
  `mmwlan_mesh_gate_count() > 0` (learned the ESP gate via RANN). Radio-silent after.
- **Ship:** branch `feat/gate-emit-rann` → `/code-review` → rebase-merge. Worklog + render + index card.

---

## S6 proper (after Part 0 lands)

### P0 — code-map re-pin (no bench)
Re-grep every §3 code-map anchor on the current trees and stamp "verified <date>":
- morselib: `components/halow/.../umac/mesh/umac_mesh.c` + `.../umac/datapath/umac_datapath.c` at the merged
  `7d7f76ad`/2.12.3 tree (files now under `umac/mesh/` + `umac/datapath/` subdirs).
- Linux: `chronium:~/halow/rpi-linux/net/mac80211/{mesh,mesh_hwmp,mesh_pathtbl,rx,tx}.c` +
  `include/linux/ieee80211.h` at `372414fd4` (6.12.21). **Re-`scp` each session (non-persistent).**
- Anchors to confirm (from the research): `mesh_path_tx_root_frame` `mesh_hwmp.c:1434`,
  `hwmp_rann_frame_process` `:914`, `mesh_path_add_gate`/`mesh_gate_num` `mesh_pathtbl.c:337`/`:397`,
  `prepare_for_gate` `:134`, `mpp_path_add`/`mpp_path_lookup` `:722`/`:274`, `mesh_path_send_to_gates`
  `:969`, `struct ieee80211_rann_ie`/`RANN_FLAG_IS_GATE`/`WLAN_EID_RANN=126` `ieee80211.h:1092`/`:1103`/`:3665`,
  AE build `mesh.c:884`/`:851`, strip-8023 `net/wireless/util.c:555`/`:583`.

### P1 — Case A: Linux gate ↔ ESP node (the wildcard: full datapath)
1. **Discovery (proven, re-confirm):** chronite up as `rimba-smesh` (secured) → gate mode
   (`iw dev wlan1 set mesh_param mesh_hwmp_rootmode 4 / mesh_gate_announcements 1 / mesh_hwmp_rann_interval
   5000`). ESP node learns it (`mmwlan_mesh_gate_count() > 0`, re-floods the RANN correctly).
2. **Build the Linux bridge (NEW — never done here):**
   ```sh
   sudo iw dev wlan1 set mesh_param mesh_fwding 1
   sudo ip link add name mesh-br type bridge
   sudo ip link set dev wlan1 master mesh-br      # mesh side
   sudo ip link set dev eth0  master mesh-br      # DS side
   sudo ip link set dev mesh-br up
   sudo ip addr flush dev wlan1; sudo ip addr add 10.9.9.2/24 dev mesh-br
   ```
3. **Prove the datapath:** an ESP mesh node sends to an off-mesh host on the Linux DS (`eth0` segment) →
   the ESP node's `send_to_gates`/`tx_proxied` wraps it AE toward the Linux gate → Linux `mpp_path_add`
   learns it + forwards to the DS; confirm with `iw dev wlan1 mpp dump` (shows the ESP-proxied source) and
   an actual ping/reply DS↔ESP-node. **Open unknowns:** does the morse driver allow bridging the mesh vif;
   what the "DS" is (wired `eth0`, or a Linux AP vif — single-radio mesh+AP on *Linux* is unproven); keep
   `10.9.9.0/24` flat to match the ESP retired-L3 design. **Expect to iterate.**

### P2 — Case B: ESP gate ↔ Linux node (needs Part 0)
1. **Discovery:** with Part 0 landed, a Linux node discovers the ESP gate's RANN — `iw dev wlan1 mpath
   dump` shows the ESP gate; the Linux gate list/`mesh_gate_num` populates; Linux beacon/logs confirm.
2. **AE/MPP leg (the S3-deferred item):** `make flash APP=test-mesh-ae LINUX_MAC=<linux wlan1 MAC>` →
   the ESP originates AE frames to the Linux node → `iw dev wlan1 mpp dump` shows the ESP-proxied `eaddr2`
   reachable via the ESP (Linux `mpp_path_add` validates the on-wire AE encoding). `LINUX_MAC=` needs the
   CMakeLists `target_compile_definitions ... TEST_LINUX_MAC` propagation.
3. **Full bridge datapath:** a Linux mesh node reaches an AP client behind the ESP gate (through the ESP
   gate's S5b/S5c bridge + proxy-ARP), both directions.

### P3 — byte-diff sign-off + ship
- **RANN both directions:** `sudo python3 tools/mesh_rann_cap.py 40 /tmp/rann_cap` on chronium with the
  ESP gate + the Linux gate emitting at once; byte-diff the two `rann_<SA>.txt` `action_hex`/`full_hex`
  with the S1 **normalized mask** (SA/BSSID/rann_addr + SeqCtrl/rann_seq/FCS → `00`). Assert byte-identical.
- **Gate bit:** `tools/mesh_beacon_cap.py` — Formation-Info connected-to-gate bit both sides.
- **AE:** cannot monitor-diff (CCMP-encrypted body) → the `iw mpp dump` checks above are the proof.
- **Record the verification tier per frame** (log-only / source-layout / **live-device gold standard**),
  `milestones.md:991-1011`. Ship the re-pinned code-map. **Radio-silent after every test.**

---

## Bench setup (fixed by the inventory)

| Node | Role in S6 | Notes |
|---|---|---|
| **chronium** (Pi5, `3c:22:7f:37:50:42`) | **Sniffer** — `morse0` monitor, `freq 5560` | Can't be sniffer AND mesh member at once. Monitor recipe: `rimba-bench-devices.md:490-500`. Pi5 reset-gpiod patch required. |
| **chronite** (Pi5, `3c:22:7f:37:51:38`, `10.9.9.2`) | **The Linux GATE** (+ mesh node) | Best candidate (stable, full `~/halow` source, wired-driver-capable). ⚠ mesh was flaky at S3 (`type mesh point` no channel, driver-reload hang) — fall back to chronogen. **`wlan0`-only access.** |
| **chronogen** (Pi Zero 2 W, `10.9.9.4`) | Fallback Linux node | Intermittent morse-SPI `-19 ENODEV`, clears on power-cycle. |
| **chronosalt** (Pi Zero 2 W, `10.9.9.3`) | avoid for bring-up | ⚠ power-marginal — reboots when `wlan1` comes up. |
| **board2** (`E0:72:A1:F8:F0:08`) | **ESP DUT** | Only fully-wired ESP; PPK2-power-gated (`tools/ppk2_hold.py`). board0/1 = light-load spares. |

- **Whole bench must be fw 1.17.8** — check at session start (dmesg fw SIZE `480664`=1.17.8; `morse_cli
  version` mis-reports). Cross-device byte-diffs are invalid under skew. `[[morse-fw-same-version]]`.
- **SAE-secured mesh** `rimba-smesh`, pw `rimbamesh2026`, ch27/op_class68 — the Linux node joins the
  **secured** mesh (config `docs/reference/captures/wpa-smesh.conf`); an open Linux mesh can't peer.
- **Linux mesh starts ONLY via `wpa_supplicant_s1g`**, never bare `iw mesh join`. Gate mode is layered at
  runtime via `iw set mesh_param` after `MESH-GROUP-STARTED`. `[[morse-linux-mesh-via-supplicant]]`.

## Footguns (each cost real time before)

- **Don't reboot the Linux nodes** — config lives in `/tmp` (tmpfs); un-wedge via driver re-probe. `[[dont-reboot-linux-mesh-nodes]]`
- Kill the mesh daemon with `pkill -9 wpa_supplicant` (substring), **NOT** `pkill -f wpa_supplicant_s1g`
  (self-kills the ssh session). Wrong channel form (`frequency=5560`) or missing `country=US` hard-wedges
  the chip. `pkill -9 -x wpa_supplicant_s1g` (exact) is fine for the mesh daemon specifically.
- RANN interval is raw-numeric on the wire — both sides use the same value (no ms→TU).
- `LINUX_MAC=`/`STA_IP=`/etc. need CMakeLists `target_compile_definitions` propagation or the `-D` is
  silently dropped (retire-L3 footgun). `make flash` doesn't clear stale `TEST_*` CMake cache vars — `rm
  build/<app>/<board>/CMakeCache.txt` before reflashing a changed test config.
- Identify ESP boards by efuse MAC, not `/dev/ttyACM*` (they re-enumerate). board2 unpowered ⇒ re-run `ppk2_hold.py`.

## Tools + references
- Capture/diff: `tools/mesh_rann_cap.py` (RANN), `tools/mesh_beacon_cap.py` (gate bit),
  `tools/mesh_grab_fwd.py` + `tools/mesh_ccmp_verify.py` (encrypted AE).
- Fixtures: `firmware/test-mesh-gate` (RANN emitter), `firmware/test-mesh-ae` (AE injector, supports
  `LINUX_MAC=`), `firmware/test-mesh-gate-rx` (AE/MPP receiver).
- Method: `docs/worklog/2026-07-21-mesh-gate-s1-rann.md` (RANN A/B), `2026-07-22-mesh-gate-s3-ae-datapath.md`
  (AE + the deferred Linux leg), `[[verify-onair-chronium-monitor]]`, `docs/reference/rimba-bench-devices.md:485-509`.
- Linux config: `docs/reference/captures/wpa-smesh.conf`, `docs/reference/rimba-linux-node-setup.md`,
  `docs/reference/rimba-linux-halow-monitor.md`.
- Design + code-map: `docs/mesh-ap/rimba-mesh-ap-mesh-gate-discovery-design.md`,
  `docs/mesh-ap/rimba-mesh-80211s-code-map.md`.
