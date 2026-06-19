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

## What was built

**`components/halow` (morselib) — IBSS support:** `morse_commands.h`
(IBSS_CONFIG/BSSID_SET + structs), `mmdrv.h`/`driver.c` (ADHOC type + senders),
`umac_interface.*` (UMAC_INTERFACE_ADHOC), `umac.c` + `umac_ibss.h`
(`mmwlan_ibss_enable`, host IBSS beacon, IBSS datapath), `umac_mmdrv_shim.c`
(beacon dispatch), `umac_datapath.c` (beacon-aware scan), `mmwlan.h`
(`mmwlan_ibss_args` / `mmwlan_ibss_enable`).

**Superproject:** `firmware/rimba-halow-ibss` (IBSS bring-up app),
`boards/proto1-fgh100m` (FGH100M BCF board), `firmware/rimba-halow-scan` (scan
loop + raw sniffer diagnostic), `vendor/mm-iot-sdk` pinned to 2.10.4.

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
