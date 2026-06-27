# Worklog — 2026-06-25 — 802.11s mesh P1: vif up + periodic beacon (DONE)

**Author:** Aldwin
**Goal:** P1 of the 802.11s mesh port — bring a mesh vif up and self-beacon a mesh
beacon, following morse_driver's mesh BSS config flow exactly.
**Status:** ✅ **DONE — mesh vif up + firmware beaconing periodically, stable.** On HW the
firmware fires beacon IRQs at the configured rate (measured **~9.8 beacons/s** at a 100 TU
/ 102.4 ms interval — 50 beacons every ~5.1 s), the host serves each one, and the node runs
indefinitely with no crash and no command-channel backup. On-air confirmation with a sniffer
/ second node is still pending (chronium is power-unstable) but the firmware-IRQ cadence is
strong evidence of TX.

### Working bring-up sequence (source-derived, follows morse_driver)
`mmwlan_mesh_start` (`umac/mesh/umac_mesh.c`):
`ADD_INTERFACE(MESH)` → `SET_CHANNEL` → `BSS_CONFIG`(0x0006) → `BSSID_SET`(0x0052, own MAC) →
`BSS_BEACON_CONFIG`(0x003D, enable) → arm host beacon engine (`start_beaconing`, sets up
`mesh_ctx` so `umac_mesh_get_beacon` is ready) → **`MESH_CONFIG`(0x0039, START,
enable_beaconing=1, MBCA defaults)**. The mesh beacon (zero-len SSID + Mesh ID IE 114 +
Mesh Config IE 113) is host-built and served on each beacon IRQ — same host-served model as
morselib AP/IBSS.

### The root cause and the fix (the whole story)
The single hard blocker was that `MESH_CONFIG`(0x0039) with `enable_beaconing=1` **wedged the
firmware**: it never responded, drained the command-page pool ("command page exhaustion"),
and then every subsequent command (even health-check 0x19) timed out → reboot. Symptoms
were identical across many sequencings, timeouts (up to 15 s), and resp-buffer variants, so
it looked like a firmware wall. Things that were ruled out along the way:
- **Not a host/driver-task deadlock.** Traced morselib's driver task: beacon-IRQ serving
  (`morse_beacon_work_` → `mmdrv_host_get_beacon`) and command-response processing both run
  on the single `"drv"` task, but a *foreign* task is what blocks in `morse_cmd_tx`, so the
  driver task stays free to serve beacons. No mutex deadlock.
- **Not a missing beacon template.** Pre-pushing a beacon (via `start_beaconing` serving
  beacon #1) before `enable_beaconing=1` did **not** stop the wedge.
- **`enable_beaconing=0` is accepted** and puts the vif in mesh mode, but then the firmware
  fires only **one** beacon IRQ — confirming `enable_beaconing=1` (which the 0x0039 doc says
  makes "firmware start to generate interrupts") is the real periodic trigger.
- **AP-type-vif workaround fails** (Interrupt-WDT crash — AP datapath expects `umac_ap`
  state mesh lacks), and **SDK 2.11.2 has no reference WLAN mesh** (only BLE mesh).

**Actual cause — MBCA mismatch.** Pulled Morse's real `morse_driver` source
(github.com/MorseMicro/morse_driver) and compared `morse_cmd_cfg_mesh` against ours. The
0x0039 wire struct order was correct, but I was sending `mbca_config = 0` and zero
scan/gap/tbtt fields. In the driver, **`mbca_config == 0` means *beaconless* mode**
(`if (mesh_beaconless_mode) mbca.config = 0`), so `mbca_config=0` + `enable_beaconing=1` is
contradictory, and the zero `mbss_start_scan_duration_ms` gave the firmware a 0 ms MBSS
scan → wedge. The driver enables beaconing with MBCA on and real defaults (`mesh.h`):
- `mbca_config = MESH_MBCA_CFG_TBTT_SEL_ENABLE` = `BIT(0)` = 1
- `min_beacon_gap_ms = DEFAULT_MBCA_MIN_BEACON_GAP_MS` = 25
- `mbss_start_scan_duration_ms = DEFAULT_MBSS_START_SCAN_DURATION_MS` = 2048
- `tbtt_adj_timer_interval_ms = DEFAULT_TBTT_ADJ_INTERVAL_MSEC` = 60000

With these set in `mmdrv_cfg_mesh` (only when `enable_beaconing`, mirroring the driver's
`if (mesh_beaconing)` block), 0x0039 now **responds rc=0** after a ~2 s MBSS TBTT-selection
scan and the firmware fires periodic beacon IRQs. `morse_cmd_tx` uses a 5 s timeout to cover
the scan. Constants added to `common/morse_commands.h` as `MORSE_MESH_MBCA_CFG_*` /
`MORSE_MESH_DEFAULT_*`.

Two earlier fixes that were also required:
- **`BSS_BEACON_CONFIG`(0x003D)** returned `-32768` with `resp=NULL`; Linux passes a
  response buffer — added the response struct + pass it.
- **Removed the link-up signal.** An earlier `LoadProhibited` crash was lwIP TX'ing
  (IPv6/mDNS) through the mesh vif before its 4-addr data path exists (P4) — not the beacon.

### On-air verification attempt (2026-06-25) — partial; receiver-side blocked
Tried to independently confirm the beacon radiates. Outcome: **TX cadence reconfirmed,
channel match proven, one real beacon-content bug fixed — but no clean raw capture yet.**

- **Fixed: Mesh Configuration sync method.** The beacon advertised Synchronization Method
  = `0x00` (none); mac80211's default is **neighbour-offset `0x01`**. A Linux mesh node
  *silently ignores* beacons whose path-proto/metric/congestion/sync/auth don't match its
  own, so `0x00` made us invisible to a default Linux mesh. Changed to `0x01`
  (`mesh_build_config_ie`, `umac_mesh.c`) — follows mac80211, not a workaround.
- **Channel/BW match confirmed.** Brought chronium up as a Linux mesh point
  (`iw dev wlan1 set type mp; mesh join rimba-mesh freq 5560`). `morse_cli -i wlan1 channel`
  reports **Primary BW 1 MHz, Primary Channel Index 0** = exactly the ESP's S1G ch27 — so
  tuning is correct (the `iw` "20 MHz" is just the 5 GHz dot11ah model).
- **Per-board unique MAC.** The MM6108 modules share a factory MAC (`bc:2a:33:96:b2:9f` on
  both), so a receiver drops the sender's frames as self. The mesh app now derives a
  locally-administered MAC from the ESP32 efuse MAC (`esp_read_mac` + LA bit) — unique per
  board. Added a peer-beacon RX sniffer to the mesh app (basis for P2).
- **Receiver-side blockers (the wall):** (1) morse **monitor mode** won't tune to 5560
  (DFS-rejected) and delivers ~0 frames — dead end here. (2) The Linux **mesh node** is
  correctly tuned + sync-matched but still creates no station — needs full S1G mesh
  compatibility (rates / S1G operation), which is P2-level peering, and it can't report bare
  reception. (3) **`iw … ibss join` reliably wedges chronium's morse driver** (SSH dies,
  needs power-cycle); `mesh join` does *not* wedge. (4) The ESP raw-RX hook
  (`mmwlan_register_rx_frame_cb`) delivers no beacons in any app. Underlying cause across
  chronium failures: the unresolved **RPi5 undervoltage** (wants a 5V/5A PD supply;
  30/45/60 W chargers negotiate 5V/3A and still under-volt).

**Verdict:** the ~9.8 beacons/s firmware-IRQ cadence is strong evidence the TX loop runs,
but a clean independent on-air capture is still outstanding, blocked by receiver tooling,
not by the mesh code.

### Update (2026-06-25, after the RPi5 power fix) — bench proven, gap narrowed to P2 peering
The undervoltage is fixed (`dmesg | grep -c undervoltage` = 0; was 4) and chronium is stable.
With that:
- **IBSS interop works → bench + ESP beacon TX are confirmed good.** chronium in IBSS mode
  (`iw dev wlan1 ibss join rimba-ibss 5560 fixed-freq 02:12:34:56:78:9a`) sees the ESP IBSS
  node: `Station bc:2a:33:96:b2:9f, signal -27 dBm`. So the ESP **does radiate beacons on
  air** via the umac beacon path, chronium's normal RX path works, and `ibss join` no longer
  wedges (the wedge was undervoltage-driven). This is the key positive control.
- **morse monitor mode is broken (not our bug).** With mon0 correctly tuned to S1G ch27
  (`morse_cli -i mon0 channel` → 915500 kHz / 1 MHz), a raw AF_PACKET sniffer captured **0
  frames even while the known-good ESP IBSS was beaconing**. morse delivers RX frames to the
  managed/IBSS/mesh vif processing but not to a monitor vif — so every "0 in monitor" result
  is meaningless, and a raw on-air capture isn't available on this stack.
- **The mesh beacon is a superset of the proven IBSS beacon** (identical header / SSID /
  S1G-capabilities / S1G-operation, plus Mesh ID + Mesh Config IEs), built and TX'd on the
  *same* path, and the firmware requests one beacon per 102.4 ms TBTT (steady 9.8/s) — i.e.
  it is on its beacon schedule and transmitting, same as IBSS. So the mesh beacon is on air.
- **Remaining gap is mesh peering, not P1 beaconing.** chronium-mesh
  (`set type mp; mesh join rimba-mesh freq 5560`, confirmed 915500 kHz / 1 MHz, sync matched)
  forms **no station** for the ESP. A mac80211 mesh node only creates a station for a beacon
  that passes `mesh_matches_local` (mesh-id + path-proto/metric/congestion/sync/auth + basic
  rates + S1G params); a mismatch is dropped silently. Pinning down *which* field still
  mismatches needs RX visibility we don't have here (monitor dead; kernel has no
  dynamic-debug). That diagnosis + fix is **P2 (peering)** — and resolving it is what yields
  the direct ESP↔Linux mesh on-air confirmation.

**Bottom line for P1:** mesh vif up + firmware beaconing periodically on air — **done**, with
the IBSS positive control proving the TX path radiates. Direct mesh-beacon interop capture is
deferred into P2, where the peering compatibility gets worked out.

### Update 2 (2026-06-25) — on-air CONFIRMED + ESP monitor built; firmware RX characterised
The external-receiver dead end was resolved by capturing **on the MM6108 itself**.

- **✅ Mesh beacon CONFIRMED on air.** A second ESP mesh node receives the `rimba-mesh`
  beacons at the per-node rate, Mesh ID intact. No more inference — the beacon radiates.
- **The firmware DOES surface peer beacons** (reversing the earlier "it doesn't"). They
  arrive as **legacy MGMT/Beacon** frames (type 0 / subtype 8), *not* S1G-beacon (EXT/1) —
  the firmware converts S1G→legacy on the way up. Every earlier search missed them because
  it looked for S1G-beacon frames or hooked `rx_frame_cb` (which is *after* the BSSID drop).
- **They arrive with zeroed source addresses (A2/A3 = 00).** The S1G beacon is single-address
  and compressed; the firmware's S1G→legacy RX conversion doesn't repopulate A2/A3. Body
  (TSF, interval, capability, IEs incl. Mesh ID) is intact. So **a node cannot identify a
  peer from a beacon** on this firmware — matching the documented data-driven discovery.
- **Beacon surfacing is gated by the local vif mode.** A *mesh*-mode vif surfaces mesh
  beacons (any Mesh ID — verified), but **not** IBSS or AP beacons (verified with live
  IBSS/AP nodes on-channel). There is no promiscuous firmware mode.

**Built: `mmwlan_register_monitor_cb()` + `firmware/rimba-halow-monitor`.** New morselib hook
(`umac_datapath_rx_frame_filter` tap, before the BSSID filter) + a monitor app that prints
every delivered frame (type, A2/A3, SSID/Mesh ID, RSSI, freq). Validated: captures mesh
beacons across Mesh IDs. README documents the firmware-gated capability matrix. This is the
sniffer that all further mesh work uses (the morse-Linux monitor delivers nothing here).

### Next — P2: peering / MPM
Beacons can't identify peers here (zeroed addresses), so ESP mesh peering must be
**data/action-frame-driven** (the same shape the IBSS port used: discover peer MACs from
frames that carry a real TA, not beacons). MPM = Mesh Peering Open/Confirm/Close **action**
frames, per `net/mac80211/mesh_plink.c` + morse_driver.

**Built: the mesh mgmt-frame TX path (2026-06-25).** The mesh vif previously had only the
host-served beacon TX path; it had no datapath ops, so any mgmt TX
(`umac_datapath_tx_mgmt_frame_ap`) crashed (LoadProhibited, NULL ops). Mesh is
association-less and self-beaconing like IBSS, and the datapath plumbing has no Linux
equivalent (morselib replaces mac80211), so this mirrors morselib's **IBSS** datapath:
- **`umac_datapath_configure_mesh_mode()` + `datapath_ops_mesh`** (`umac_datapath.c`) — shares
  the generic IBSS/STA helpers (get_state, enqueue, pause, data-header); mesh-specific bits
  are the common-stad lookups, `tx_dequeue_frame_mesh`, and `process_rx_mgmt_frame_mesh`
  (routes ACTION frames to MPM). `frames_allowed_pre_association_mesh` allows S1G beacons +
  ACTION frames.
- **Mesh "common" stad** (the MBSS, for broadcast/mgmt TX), allocated in `mmwlan_mesh_start`
  exactly like `umac_ibss_start`. `umac_mesh_get_common_stad()` backs the datapath ops.
- **`mmwlan_mesh_send_test_action()`** uses `umac_datapath_tx_mgmt_frame(common_stad, …)` (the
  non-`_ap` variant — the `_ap` one assumed AP context the mesh vif lacks).

Result on HW: action frames TX cleanly (status 0, **no crash**), and the mesh-monitor
receives them (len 28). **Key finding:** received ACTION frames have **zeroed source
addresses (A2/A3 = 0), exactly like beacons** — so the firmware strips the source MAC on RX
for *management* frames generally, not just beacons. (Data frames keep their TA — that's how
the IBSS port did discovery.) **Consequence for MPM:** an ESP can't learn a peer's address
from a received beacon *or* action frame, so ESP-side peering must use **provisioned peer
MACs** (consistent with this product's provisioned-network model) rather than discovery.

### ROOT CAUSE of the "zeroed source address" — it was OUR bug, not the firmware (2026-06-25)
The "firmware strips the source MAC / firmware wall / must use provisioned peer MACs"
conclusion was **wrong**. The mesh bring-up set `mesh_ctx.mesh_mac` via
`umac_interface_get_vif_mac_addr()`, which only resolves the **STA/AP roles** and returns
**zero** for a mesh vif. So `mesh_mac = 00:00:…:00`, and every mesh beacon + action frame was
**built with a `00:00` source address** (A2 = A3 = 0). Everyone correctly saw the zeros we
transmitted — the ESP monitor, and (verified by rebuilding `morse_driver` with
`CONFIG_MORSE_MONITOR` and capturing on `morse0`) **Linux too**.

**Fix** (`umac_mesh.c`): set `mesh_ctx.mesh_mac` from the address the vif was actually created
with — the app's `if_addr`, or `umac_interface_get_device_mac_addr()` for the factory MAC —
not the role-based getter. **Confirmed on Linux `morse0`:** beacons now carry the real
per-node MAC (`SA = BSSID = e2:72:…`).

**Consequences:** normal **beacon-based mesh discovery is viable** (the peer's identity is in
the beacon, since a mesh node's BSSID = its own MAC) — **no provisioned-peer workaround
needed.** The action-frame TX path work still stands (it's needed for MPM regardless).

**Tooling win:** chronium's `morse_driver` rebuilt with `CONFIG_MORSE_MONITOR=y` (it was off —
the reason every prior Linux monitor attempt delivered nothing). The `morse0` raw-monitor
netdev now works and is the reference sniffer (build cmd in `reference/rimba-linux-node-setup.md`
+ `CONFIG_MORSE_MONITOR=y`; backup of the old module at `~/morse.ko.bak`).

### Next
1. **mac80211 still forms no mesh peer** even with the real address → the remaining gap is
   **mesh compatibility** (`mesh_matches_local`: S1G basic rates / mesh-config fields). This is
   now the genuine P2 blocker — diagnosable with the working `morse0` monitor + the ESP
   mesh-monitor. Follow `net/mac80211/mesh.c` / `mesh_plink.c`.
2. Then MPM Open/Confirm/Close, and the 4-addr data path (P4).
3. Reflash board2 (`rimba-halow-mesh-monitor`) with the MAC fix; remove the debug action probe.
Bench: board0 = mesh + 1 Hz action probe, board1 = mesh, board2 = `rimba-halow-mesh-monitor`
(old build), chronium = Linux mesh node + working `morse0` monitor.

---

(Earlier-session notes below are superseded by the summary above.)

Hardware: XIAO ESP32-S3 + FGH100M (`/dev/ttyACM0`), MM6108 fw **1.17.9** (generated
from `vendor/morse-firmware`). morselib 2.10.4 + local patches.

## What was implemented (mirrors umac/ibss, follows morse_driver)

morselib (halow fork):
- **Interface plumbing:** `MMDRV_INTERFACE_TYPE_MESH=5` (`mmdrv.h`),
  `UMAC_INTERFACE_MESH=32` (`umac_interface.h`) + the umac→driver type map and the
  MESH-is-exclusive compatibility rule (`umac_interface.c`); `MORSE_CMD_INTERFACE_TYPE_MESH`
  already existed.
- **Firmware-command defs** (byte-compatible with morse_driver): `MESH_CONFIG=0x0039`,
  `SET_MESH_CONFIG=0xA018`, opcodes START=0/STOP=1, `MORSE_CMD_MESH_ID_LEN_MAX=32`, and
  the two request structs (`common/morse_commands.h`).
- **Driver wrappers** (`driver.c`): `mmdrv_cfg_mesh` (MESH_CONFIG) and
  `mmdrv_set_mesh_config` (SET_MESH_CONFIG); MESH case in `mmdrv_add_if`.
- **`umac/mesh/umac_mesh.{c,h}`** — new module mirroring `umac/ibss`: mesh context,
  `umac_mesh_is_active()`, `umac_mesh_get_beacon()` + a mesh beacon builder (zero-length
  SSID + Mesh ID IE 114 + Mesh Configuration IE 113, open HWMP/airtime), and the public
  `mmwlan_mesh_start()` / `mmwlan_mesh_stop()` + `struct mmwlan_mesh_args`.
- **Beacon routing:** `mmdrv_host_get_beacon()` routes to `umac_mesh_get_beacon()` when
  a mesh vif is active (`umac_mmdrv_shim.c`).
- Registered `umac_mesh.c` in `components/morselib/CMakeLists.txt`.

App: `firmware/rimba-halow-mesh` rewritten from the P0 probe into a mesh bring-up
(`mmwlan_mesh_start`, self-beacon, heartbeat). Builds + links clean.

## Hardware findings (the useful part)

Bring-up sequence tried: `ADD_INTERFACE(MESH)` → `SET_CHANNEL` → `BSS_CONFIG` →
`MESH_CONFIG(START)` (+ host beacon engine).

1. **`ADD_INTERFACE(MESH=5)`, `SET_CHANNEL`, `BSS_CONFIG` — all accepted.** (P0 already
   proved the interface type; the channel/BSS configure cleanly.)
2. **`SET_MESH_CONFIG` (0xA018) is a HOST/driver command, not a firmware command.**
   First attempt sent it to the firmware → rejected `-22`. Reading morse_driver
   (`command.c` switch → `morse_cmd_set_mesh_config` in `mesh.c`) shows the 0xA0xx range
   is handled *driver-side*: it just stores mesh ID / max_plinks in the driver's own
   state (to build the beacon) and then sends the firmware `MESH_CONFIG`. morselib *is*
   the host and builds its own beacon, so it must **not** send 0xA018 — removed it.
3. **`MESH_CONFIG` (0x0039) hangs the chip.** With the corrected sequence it never
   responds: `Command 39 timed out` (fw_status -116), `command page exhaustion`, then a
   watchdog reboot / `xQueueSemaphoreTake` assert. Tried both orders (host beacon engine
   before/after) and `enable_beaconing` on/off — all hang or reboot.

## Investigation — NOT a firmware wall; my sequence is wrong

The firmware is fine: same 1.17.9 `.mbin` (converted from `vendor/morse-firmware`), and
others have reported working ESP32 802.11s. The errors I saw (`MESH_CONFIG` hang,
`BSS_BEACON_CONFIG` -32768) are from sending the **wrong commands / wrong order / wrong
preconditions** — because I reverse-engineered the sequence from `morse_driver` in
isolation and guessed, instead of following Morse's **actual** mesh flow, which is driven
by their **patched mac80211** (kernel patch — confirmed: `rpi-linux/net/mac80211/mesh.c`
carries the Morse commit *"mac80211: return RX_QUEUED in 6.6+ to resolve multi-hop
failures"*).

### Source-derived firmware command sequence (morse_driver `mac.c` `bss_info_changed`)
On a mesh vif coming up, mac80211 fires `bss_info_changed` with multiple flags; the driver
sends, in order (by flag):

1. `morse_cmd_config_beacon_timer` → **BSS_BEACON_CONFIG (0x003D)** — on `BEACON_ENABLED`
2. `morse_cmd_set_bssid` → **SET_BSSID** — on `BSSID` ← **morselib mesh does NOT send this**
3. `morse_cmd_cfg_bss` → **BSS_CONFIG (0x0006)** — on beacon info
4. mesh start itself: `morse_cmd_set_mesh_config` (the 0xA018 HOST handler) internally
   calls `morse_cmd_cfg_mesh_bss` → **MESH_CONFIG (0x0039)**. So the host SET_MESH_CONFIG
   *logic* is what triggers the firmware MESH_CONFIG (I correctly drop the 0xA018 *wire*
   command, but the trigger logic — call MESH_CONFIG — must remain).

### Concrete gaps in my morselib bring-up
- **SET_BSSID — added, accepted.** Implemented `BSSID_SET` (0x0052) + `mmdrv_set_bssid`
  and send it (mesh node's own MAC) after BSS_CONFIG. The firmware accepts it. But
  `MESH_CONFIG` (0x0039) **still hangs** the same way, so SET_BSSID was a correct addition
  but not sufficient.
- **`config_beacon_timer` (0x003D)** returned -32768 from my call — likely wrong vif
  state/order, not "unsupported".
- **Order / remaining commands.** With ADD_INTERFACE(MESH) + SET_CHANNEL + BSS_CONFIG +
  SET_BSSID all accepted, MESH_CONFIG still times out (command-page exhaustion → reboot),
  and the exhaustion correlates with `start_beaconing`. Still missing something the Linux
  mesh-start path does (another command and/or the exact order, and possibly the beacon
  must not start until the firmware is in mesh mode). **Incremental guessing has hit its
  limit — the next move must be the ground-truth command sequence, not more guesses.**

## Live-capture attempt — blocked by chronium hardware

Tried to capture the ground-truth command sequence on chronium (`debug_mask=0xffffffff` +
`iw … mesh join`). Two problems made it unusable:
- The morse driver **leaks netdev resources** on repeated `iw interface add/del` — after a
  few cycles, `interface add` fails `-23 ENFILE` (not real fd exhaustion: `file-nr` ~800).
  A `modprobe -r` left the SPI device unbound → `bind` returns I/O error → needs a reboot.
- chronium logs **undervoltage** events — the MM6108 appears unresponsive at times
  (commands return rc=0 but the iface never joins and dmesg stays empty).

Recovered chronium twice via reboot (authorized); `wlan1` comes up clean on boot. The
capture needs a **single, careful** mesh-up (one `iw mesh join`, no add/del churn) and
ideally chronium's power issue addressed.

## Next steps

1. **Add SET_BSSID** to the mesh bring-up (find the 802.11s mesh BSSID value morse uses;
   `morse_cmd_set_bssid` / the existing `mmdrv` BSSID path) and re-test — strongest single
   candidate for the `MESH_CONFIG` hang.
2. **Get the exact order** from one clean chronium capture (no churn) or by reading the
   mac80211 mesh-start path (`ieee80211_start_mesh` → the `BSS_CHANGED_*` flag set) + the
   morse_driver handlers; replicate command-for-command.
3. Re-test the beacon (detect with `rimba-halow-scan` on a 2nd board or chronium).

## State

On branch `feature/mesh-p0-fw-probe` (rebased onto current `main`). All code compiles +
links; bring-up reaches `MESH_CONFIG` and stops. Nothing committed for P1. The
`BSS_BEACON_CONFIG`/`mmdrv_config_beacon_timer` defs are kept but the call is removed
(see umac_mesh.c comment). `SET_MESH_CONFIG` (0xA018) wrapper kept, not sent (host-side).
