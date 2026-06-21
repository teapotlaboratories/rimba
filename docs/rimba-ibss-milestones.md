# RISK-01 — IBSS / ad-hoc on the MM6108: milestones

Summary of the RISK-01 effort to bring up an **802.11ah (Wi-Fi HaLow) IBSS /
ad-hoc** link on **ESP32-S3 + Morse Micro MM6108** via `mm-iot-sdk`/morselib,
which exposes no public IBSS API. For the blow-by-blow (commands, captures,
diagnoses) see [`worklog/2026-06-18-risk01-ibss-recon.md`](worklog/2026-06-18-risk01-ibss-recon.md).

**Governing requirement:** the implementation is derived from the **Linux side**
— MorseMicro's `morse_driver` (mac80211) for the firmware command sequence, and
`net/mac80211/ibss.c` (MorseMicro kernel fork) for the IBSS beacon/probe-resp IE
set, merge, and ATIM — not improvised from morselib's AP path.

**Hardware:** up to 3× Seeed XIAO ESP32-S3 + HaLow module (reports
`mm6108-mf16858`, chip `0x0306`, firmware **v1.17.6**) **plus a Raspberry Pi +
MM6108 Linux reference node** (`morse_driver`/mac80211, same silicon — the interop
oracle, see [`rimba-linux-node-setup.md`](rimba-linux-node-setup.md)); board config
`boards/proto1-fgh100m` (`bcf_fgh100mhaamd`); US 915.5 MHz, 1 MHz BW, S1G channel
27 / op-class 68 (the Linux `iw … ibss join` frequency for ch27 is **5560** — the
5 GHz-model channel that `dot11ah` maps ch27 onto; on-air is still 915.5 MHz).

---

## Milestones

### M1 — Reconnaissance & feasibility ✅
Reverse-engineered the IBSS firmware-command sequence from `morse_driver`
(`ADD_INTERFACE(ADHOC) → SET_CHANNEL → BSSID_SET → BSS_CONFIG → IBSS_CONFIG`) and
located the injection points in the ESP32 morselib. Pinned a coherent version
matrix (vendored firmware is already the forum-proven v1.17.6; `mm-iot-sdk`
pinned to 2.10.4 for parity). Found the two IBSS commands (`IBSS_CONFIG 0x35`,
`BSSID_SET 0x52`) are undeclared in the ESP32 SDK header but present in the chip
firmware.

### M2 — Firmware accepts the IBSS command sequence ✅
Added the ADHOC interface type + the two missing commands to morselib and a
`mmwlan_ibss_enable()` that drives the clean-state sequence. On hardware the chip
**accepts** `ADD_INTERFACE(ADHOC)`, `BSSID_SET`, `BSS_CONFIG`, and recognises
`IBSS_CONFIG` (returns `EEXIST(-17)` — "already created" — which is the exact
error seen on the Morse forum, now understood and handled, not a wrong command).

### M3 — Stable ad-hoc bring-up + host-generated beacon ✅
The board brings up IBSS and runs a host-generated **long S1G beacon** without
crashing (`ibss_status=0`). Beacon contents corrected against
`net/mac80211/ibss.c`: IBSS capability bit, **IBSS Parameter Set element**
(ATIM=0), S1G Capabilities, S1G Operation, no TIM.

### M4 — RF link + sniffer validated ✅
Built a raw-frame sniffer on the second board
(`mmwlan_register_rx_frame_cb`) and validated it against a known-good SoftAP:
frames captured at 915.5 MHz from the AP's BSSID. Established that the chip
surfaces **probe responses** (not beacons) to the host during an active scan, so
discovery requires the node to **answer probe requests**.

### M5 — Genuine IBSS link proven on air ✅
Added an IBSS datapath (mirroring `ieee80211_ibss_rx_probe_req`) that answers
probe requests with a probe response. A second board now detects the ad-hoc node
as a **genuine IBSS**:

```
rimba-ibss   BSSID 02:12:34:56:78:9a   Capability 0x0002 (IBSS set, ESS clear)
RXFRAME flag=PROBE_RSP len=77 freq=915500kHz  A2=A3=02:12:34:56:78:9a
```

This confirms the MM6108 in ADHOC mode on the ESP32 **transmits and receives on
air** and is advertised as a real IBSS — the central risk of RISK-01 is retired.

### M6 — IBSS data path: bidirectional IP, dynamic ARP ✅
Un-stubbed the data plane (single shared peer `stad`, STA-style enqueue/dequeue,
OPEN-plaintext TX, IBSS addressing) and fixed the real blocker: the RX **data**
path resolved the VIF as STA/AP only and dropped `UMAC_INTERFACE_ADHOC` frames
(`umac_datapath.c` "Invalid RX VIF"); added an ADHOC→`MMWLAN_VIF_AP` case. Two
boards then exchange **bidirectional IP over IBSS with dynamic ARP** (no static
entry):

```
RXDATA dst=<self> src=<peer> eth=0x0800   reply from 192.168.13.2: time=17 ms
counts: ok=18  rxdata=36  timeout=0  senderr=0   (~16-17 ms RTT)
```

**The Phase-1 data gate is met** — two battery-class nodes exchange L2/IP frames
with no infrastructure. The literal Rimba EtherType `0x88B5` rides this same path
(it's just a different EtherType than the `0x0800` used here; sending raw `0x88B5`
is an app-level `mmwlan_tx`, not a datapath change).

### RISK-01 — RESOLVED
All five layers (interface/commands, bring-up, beacon, probe-answering, data
path) work end-to-end on two boards, derived from and verified against the Linux
implementation. IBSS is a viable L2 for Rimba on the MM6108; the RISK-01 fallback
(AP-STA) is not required.

---

## Post-RISK-01 — Phase-1 hardening & validation

These extend the proven link toward a robust mesh. Detail:
[`worklog/2026-06-20-ibss-adoption-interop-phantom.md`](worklog/2026-06-20-ibss-adoption-interop-phantom.md),
plus [`rimba-ibss-hardening-todo.md`](rimba-ibss-hardening-todo.md) (backlog) and
[`rimba-ibss-test-plan.md`](rimba-ibss-test-plan.md) (P0/I results).

### H1 — N-node addressing + 3-board bench (P0) ✅
One binary on every board; each derives its IP from its MAC
(`192.168.13.<mac[5]>`) and pings every discovered peer. 3 boards form a full
mesh (P0.1–0.7); per-peer `sta_data`+AID so RX dedup is isolated per sender.

### H2 — Adopt the momentary-systems IBSS implementation (proper EEXIST fix) ✅
Replaced our inline IBSS with the public `momentary-systems/esp-halow-ibss`
implementation (`umac_ibss.c` module + `datapath_ops_ibss`), adopted onto our
`esp32-2` base. This is the **Linux-faithful teardown-first bring-up**: it removes
the active boot interface before `ADD_INTERFACE(ADHOC)` (mirrors
`ieee80211_do_stop` → morse `REMOVE_INTERFACE`), so `IBSS_CONFIG(CREATE)` returns
**0** — the `EEXIST(-17)` is fixed at the root, not tolerated. Brings peer age-out
(#14), teardown (#6), and membership callbacks (#12). Validated on HW: no EEXIST,
pure 2-/3-ESP32 full mesh, Linux interop data 2/2.

### H3 — Drop/rejoin resilience, P0.6 (age-out unblocks survivor rediscovery) ✅
With age-out merged, a survivor now frees a departed peer's record (~30 s) and
re-acquires it as a **fresh record** on return — the survivor-side test that was
structurally impossible before. Mirrors `ieee80211_ibss_sta_expire`.

### H4 — Linux interop in a mixed cell, I.5 (the #17 phantom-flood bug) ✅
3 ESP32 + 1 Linux `morse_driver` node on one cell. First run **failed**: the
adopted datapath bound IBSS to the **AP-mode** `frames_allowed_pre_association`
list, which omits `S1G_BEACON`. So every received S1G beacon fell through to the
RX filter's `dot11_get_ta()` peer-mint — which on an S1G beacon reads the
**`time_stamp`** field (addr2 offset), minting a fresh phantom peer per beacon,
flooding the 8-slot table and evicting real peers (a survivor was starved to 0
replies). **Fix:** give IBSS its own allowed-list including `S1G_BEACON`, so
beacons skip the mint and route to `process_s1g_beacon`. After the fix: **full
all-pairs reachability, 0 phantoms.** This bug was present in the upstream
momentary-systems fork too (same `ap_mode` binding) — surfaced only by our Linux
interop testing.

### H5 — Beacon discovery is data-driven (the #16 correction) ✅
On-air `DBG-SA` probing settled how peers are actually discovered, and **corrected
an earlier wrong belief**: beacon-based discovery does *no* useful work on this
hardware/firmware.
- The **mm6108 firmware (1.17.6) does not surface same-cell peer beacons to the
  host** — a pure 2-ESP32 cell communicates fine but **0** beacons reach
  `process_s1g_beacon`. So ESP32↔ESP32 discovery is entirely data-driven.
- A `morse_driver` node's S1G beacon carries **`source_addr = BSSID`** (not the
  sender's MAC), confirmed both on-air and in the Linux driver
  (`morse_dot11ah_s1g_to_beacon` copies the single S1G address into *both*
  `mgmt->sa` and `mgmt->bssid`). So even surfaced beacons can't identify a peer.

`process_s1g_beacon` now **drops** beacons (no peer minting; it was creating a junk
`BSSID` peer). All real discovery is the **data-frame** path
(`lookup_stad_by_peer_addr_ibss` on a frame's real TA) — which is what Linux+morse
effectively does too (`ieee80211_ibss_rx_bss_info` keys on `mgmt->sa`, but morse
sets `mgmt->sa = bssid`). Marked **morse hardware/firmware dependent** in the
driver; revisit only if a future firmware surfaces real per-node beacons.

---

## Code development overview & Linux comparison

The port was built bottom-up, each layer mapped to (and derived from) its Linux
source and verified on hardware: firmware commands → bring-up sequence → host
beacon → datapath/probe-answering. The MM6108 firmware is the same silicon Linux
drives, so the firmware-command layer matches `morse_driver` directly; the
upper-layer behaviour (beacon IEs, probe answering, addressing, merge) matches
`net/mac80211/ibss.c`.

Path shorthands:
- **port** = `components/halow/components/mm-iot-sdk/framework/morselib/` (+ a few
  superproject files under `firmware/`, `boards/`).
- **kernel** = `MorseMicro/linux` (fork branch `mm/linux-6.12.81/1.17.x`).
- **morse_driver** = `MorseMicro/morse_driver` (out-of-tree mac80211 driver).

| Concern | This port (file : symbol) | Linux equivalent (file : symbol) |
|---|---|---|
| ADHOC interface type + add | port `src/internal/mmdrv.h` : `MMDRV_INTERFACE_TYPE_ADHOC`; `src/driver/driver.c` : `mmdrv_add_if` ADHOC case | morse_driver `mac.c` : `interface_modes` `BIT(ADHOC)`@7211; `command.c` : `morse_cmd_add_if` (`NL80211_IFTYPE_ADHOC`→`MORSE_CMD_INTERFACE_TYPE_ADHOC`) |
| IBSS commands + structs (`IBSS_CONFIG` 0x35, `BSSID_SET` 0x52) | port `src/common/morse_commands.h` : `morse_cmd_req_ibss_config`, `…_bssid_set`; `src/driver/driver.c` : `mmdrv_cfg_ibss`, `mmdrv_set_bssid` | morse_driver `morse_commands.h` : `morse_cmd_req_ibss_config`; `command.c` : `morse_cmd_cfg_ibss`@2228, `morse_cmd_set_bssid` |
| Bring-up sequence + create/join, EEXIST handling | port `src/umac/umac.c` : `mmwlan_ibss_enable`, `umac_ibss_do_start` | kernel `net/mac80211/ibss.c` : `__ieee80211_sta_join_ibss`; morse_driver `mac.c` : `morse_mac_join_ibss`@5308, `bss_info_changed` IBSS branch@4127 |
| Host IBSS beacon (IBSS cap bit, IBSS Param Set, S1G Cap/Op, no TIM, long) | port `src/umac/umac.c` : `umac_ibss_build_beacon`, `umac_ibss_get_beacon`, `umac_ibss_append_s1g_ies`; dispatch `src/umac/umac_mmdrv_shim.c` : `mmdrv_host_get_beacon` | kernel `net/mac80211/ibss.c` : `ieee80211_ibss_build_presp`@38 (IBSS Param Set@133, cap `WLAN_CAPABILITY_IBSS`@475); 11n→S1G conv morse_driver `dot11ah/tx_11n_to_s1g.c` : `morse_dot11ah_beacon_to_s1g`@819 (`EXT\|S1G_BEACON`@831) + `beacon.c` : long-only for ADHOC@382 |
| Probe-request answering | port `src/umac/umac.c` : `umac_ibss_process_rx_mgmt_frame`, `umac_ibss_build_probe_resp`, `datapath_ops_ibss` | kernel `net/mac80211/ibss.c` : `ieee80211_ibss_rx_queued_mgmt`@1584 → `ieee80211_rx_mgmt_probe_req`@1490 |
| IBSS data 802.11 addressing (ToDS=FromDS=0; A1=DA, A2=SA, A3=BSSID) | port `src/umac/umac.c` : `umac_ibss_construct_80211_data_header` | kernel `net/mac80211/tx.c` : `ieee80211_build_hdr` (`NL80211_IFTYPE_ADHOC` case) |
| Beacon-aware discovery (workaround) | port `src/umac/datapath/umac_datapath.c` : `…_process_rx_mgmt_frame_sta` BEACON case | n/a — ESP32 `hw_scan` only surfaces probe responses to the host, so we answer probes (as Linux does) and also feed beacons to the scanner |
| Public API / app entry | port `include/mmwlan.h` : `mmwlan_ibss_enable`, `mmwlan_ibss_args`; app `firmware/rimba-halow-ibss/main/app_main.c` | userspace: `iw dev … ibss join`; `wpa_supplicant` (`mode=1`) |
| IBSS merge (TSF tiebreak) | **not yet implemented** (#4) | kernel `net/mac80211/ibss.c` : `ieee80211_rx_bss_info`@1081 (TSF tiebreak@1160) |
| Data-plane TX/RX (`0x88B5` + IP) | ✅ working — port `src/umac/datapath/umac_datapath.c` : `umac_datapath_process_rx_data_frame`, `…tx_dequeue_frame_ibss` | kernel `net/mac80211/tx.c` + `rx.c`; morse_driver data path |

### Post-RISK-01 hardening — adoption + #16/#17 (2026-06-20)

After H2, IBSS moved out of `src/umac/umac.c` into the adopted module
`src/umac/ibss/umac_ibss.c` (+ `datapath_ops_ibss` in `umac_datapath.c`). New/changed
mappings from this phase:

| Concern | This port (file : symbol) | Linux equivalent (file : symbol) |
|---|---|---|
| IBSS module + public API (factor-out #12) | `src/umac/ibss/umac_ibss.c` : `mmwlan_ibss_start`/`_stop`, `umac_ibss_get_or_create_peer_stad` | kernel `net/mac80211/ibss.c` : `__ieee80211_sta_join_ibss`, `ieee80211_ibss_add_sta`@581 |
| Teardown-first bring-up (EEXIST fix, H2) | `umac_ibss.c` : `mmwlan_ibss_stop` → `REMOVE_INTERFACE` before `ADD_INTERFACE(ADHOC)` | kernel `net/mac80211/iface.c` : `ieee80211_do_stop`; morse_driver `mac.c` : `morse_mac_ops_remove_interface` |
| Peer age-out / free (#14) | `umac_ibss.c` : `mmwlan_ibss_age_peers(threshold_ms)`, per-peer `last_rx_ms`, LRU at 8-cap | kernel `net/mac80211/ibss.c` : `ieee80211_ibss_sta_expire` (`IEEE80211_IBSS_INACTIVITY_LIMIT` 60 s) |
| Peer discovery (the real path) — **data-driven** | `umac_datapath.c` : `umac_datapath_lookup_stad_by_peer_addr_ibss` (get-or-create on a data frame's real TA) | kernel `net/mac80211/ibss.c` : `sta_info_get(mgmt->sa)` → `ieee80211_ibss_add_sta`; `rx.c` sta lookup |
| Beacon RX handler — **drops, no peer mint (#16)** | `umac_datapath.c` : `umac_datapath_process_s1g_beacon` | kernel `net/mac80211/ibss.c` : `ieee80211_ibss_rx_bss_info`@968 (keys `mgmt->sa`) — see "morse beacon SA=BSSID" below |
| IBSS allowed-pre-assoc list (**#17 phantom-flood fix**) | `umac_datapath.c` : `frames_allowed_pre_association_ibss[]` (incl. `S1G_BEACON`), bound on `datapath_ops_ibss` | kernel design: beacons route to `ieee80211_ibss_rx_bss_info`, never minted as "unknown sender" via the data path; cf. `frames_allowed_pre_association_sta_mode` which also lists `S1G_BEACON` |
| morse S1G beacon **SA=BSSID** (why beacon discovery is moot) | firmware (mm6108.mbin 1.17.6) lower-MAC TX rewrite + non-surfacing of peer beacons | morse_driver `dot11ah/rx_s1g_to_11n.c` : `morse_dot11ah_s1g_to_beacon` (sets `mgmt->sa = mgmt->bssid = s1g.sa`); TX `dot11ah/tx_11n_to_s1g.c` : `morse_dot11ah_beacon_to_s1g` (SW-4741) |

### Key divergences from Linux (and why)
1. **S1G beacon conversion location.** On Linux the 11n→S1G beacon conversion is
   *host-side* (`morse_driver/dot11ah/tx_11n_to_s1g.c`). On ESP32 the **chip
   firmware** performs it (proven by the working SoftAP). So we feed an
   11n-form PV0 beacon body (incl. the S1G Cap/Op IEs, as hostapd does for AP)
   and rely on the chip to emit the S1G `EXT/S1G_BEACON` per interface type.
2. **State-machine scope.** Linux mac80211 runs the full IBSS state machine
   (merge, TSF adoption, ATIM, per-peer `sta_info`). This port implements the
   subset proven so far — bring-up, beacon, probe-answering — with **merge and
   the data plane still pending**. ATIM is intentionally omitted (window 0 =
   always awake; per `ibss.c` it is power-save-only).
3. **No association objects.** IBSS has no association, so the datapath `stad`
   lookups return `NULL` and frames are admitted via
   `frames_allowed_pre_association`. **The adopted code reused the AP-mode list
   here, which omits `S1G_BEACON` — that was the #17 phantom-flood bug** (a beacon
   isn't "allowed," so it fell to the data-path `dot11_get_ta` mint and read the
   beacon timestamp as a transmitter). Fixed by an IBSS-specific list that allows
   `S1G_BEACON` (like STA mode). Linux never has this problem: a beacon goes to
   `ieee80211_ibss_rx_bss_info`, not the unknown-sender data path.
4. **Discovery is data-driven, not beacon-based** (H5/#16). On this firmware,
   peer beacons aren't surfaced to the host and morse beacons carry `SA=BSSID`,
   so peers are learned from data-frame source addresses. This matches Linux+morse
   in practice (`ieee80211_ibss_rx_bss_info` keys `mgmt->sa`, but the morse RX
   path sets `mgmt->sa = bssid`). Morse hardware/firmware dependent.

## Build / test

```bash
make build APP=rimba-halow-ibss BOARD=proto1-fgh100m
# one binary on every node — flash 1..3 boards:
for p in 0 1 2; do make flash APP=rimba-halow-ibss BOARD=proto1-fgh100m PORT=/dev/ttyACM$p; done
make monitor APP=rimba-halow-ibss PORT=/dev/ttyACM0      # "reply from 192.168.13.N … IBSS DATA OK"
```

Linux interop (the 4th node) — Raspberry Pi + MM6108, `morse_driver`/mac80211; bring-up
in [`rimba-ibss-linux-interop-runbook.md`](rimba-ibss-linux-interop-runbook.md). Join the
same pinned cell with **frequency 5560** (S1G ch27 in the 5 GHz model; on-air 915.5 MHz):

```bash
sudo iw dev wlan1 ibss join rimba-ibss 5560 fixed-freq 02:12:34:56:78:9a
sudo ip addr add 192.168.13.66/24 dev wlan1     # octet from the node MAC, like the ESP32s
```

> Bench note: XIAO USB-Serial-JTAG re-enumerates on reset and a beaconing board
> resists esptool reset; for non-interactive capture, flash/reset then wait ~2–3 s
> before opening a continuous `cat` on the port. Return idle boards to the
> radio-free `rimba-hello` between tests.
