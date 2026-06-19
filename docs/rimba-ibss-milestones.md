# RISK-01 — IBSS / ad-hoc on the MM6108: milestones

Summary of the RISK-01 effort to bring up an **802.11ah (Wi-Fi HaLow) IBSS /
ad-hoc** link on **ESP32-S3 + Morse Micro MM6108** via `mm-iot-sdk`/morselib,
which exposes no public IBSS API. For the blow-by-blow (commands, captures,
diagnoses) see [`worklog/2026-06-18-risk01-ibss-recon.md`](worklog/2026-06-18-risk01-ibss-recon.md).

**Governing requirement:** the implementation is derived from the **Linux side**
— MorseMicro's `morse_driver` (mac80211) for the firmware command sequence, and
`net/mac80211/ibss.c` (MorseMicro kernel fork) for the IBSS beacon/probe-resp IE
set, merge, and ATIM — not improvised from morselib's AP path.

**Hardware:** 2× Seeed XIAO ESP32-S3 + HaLow module (reports `mm6108-mf16858`,
chip `0x0306`, firmware **v1.17.6**); board config `boards/proto1-fgh100m`
(`bcf_fgh100mhaamd`); US 915.5 MHz, 1 MHz BW, S1G channel 27 / op-class 68.

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

### Remaining — EtherType `0x88B5` data exchange ⬜
The Phase-1 success gate: exchange raw Rimba `0x88B5` data frames between two
IBSS peers. The IBSS data addressing is already in place
(`construct_80211_data_header`: ToDS=FromDS=0, A1=DA, A2=SA, A3=BSSID); the
data-plane TX/RX enqueue path is still stubbed and is the next increment.

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
| IBSS merge (TSF tiebreak) | **not yet implemented** | kernel `net/mac80211/ibss.c` : `ieee80211_rx_bss_info`@1081 (TSF tiebreak@1160) |
| Data-plane TX/RX (`0x88B5`) | **stubbed** — port `src/umac/umac.c` : `umac_ibss_enqueue_tx_frame`/`…dequeue…` | kernel `net/mac80211/tx.c` + `rx.c`; morse_driver data path |

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
   `frames_allowed_pre_association` — matching how Linux treats IBSS peers
   before any `sta_info` exists.

## Build / test

```bash
make build APP=rimba-halow-ibss BOARD=proto1-fgh100m
make flash APP=rimba-halow-ibss BOARD=proto1-fgh100m PORT=/dev/ttyACM0   # IBSS node
make flash APP=rimba-halow-scan BOARD=proto1-fgh100m PORT=/dev/ttyACM1   # detector/sniffer
```

> Bench note: XIAO USB-Serial-JTAG re-enumerates on reset and a beaconing board
> resists esptool reset; for non-interactive capture, flash/reset then wait ~2–3 s
> before opening a continuous `cat` on the port. Return idle boards to the
> radio-free `rimba-hello` between tests.
