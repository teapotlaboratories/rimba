# rimba-halow-mesh-monitor

An **802.11s mesh-beacon monitor** for the Morse Micro MM6108 (Wi-Fi HaLow),
running on an ESP32-S3. It prints a one-line summary of every frame the firmware
delivers to the host on a chosen S1G channel.

> **Scope:** this reliably monitors **mesh** beacons (the firmware only surfaces
> foreign beacons to a mesh-mode vif). It does **not** see IBSS or AP beacons — the
> MM6108 firmware has no promiscuous mode and gates beacon delivery by the local
> vif's mode. Hence the `-mesh-` in the name. See "What it can and cannot capture".

## Why this exists

There is no usable external HaLow sniffer on this bench:

- The Linux **morse_driver monitor mode delivers no frames** — even with a monitor
  vif correctly tuned to the S1G channel, an `AF_PACKET` capture sees nothing.
- Ordinary Wi-Fi NICs cannot demodulate S1G (sub-GHz, 1/2/4 MHz) at all.

So to observe what is actually on the air (essential for the 802.11s mesh work),
we capture **on the MM6108 itself**, from inside morselib, at the point where the
firmware hands received frames to the host datapath — *before* the normal
address/BSSID filter discards foreign frames.

## How it works

1. The app brings up a **mesh vif** on the configured channel. Mesh is the mode in
   which the MM6108 firmware surfaces *foreign* (other-BSSID) beacons to the host
   (see "Firmware constraints" below).
2. It registers morselib's promiscuous monitor hook,
   `mmwlan_register_monitor_cb()` (added in this fork — see
   `src/internal/mmwlan_internal.h` and the tap in
   `umac/datapath/umac_datapath.c:umac_datapath_rx_frame_filter`). The hook fires
   for **every** frame the firmware delivers, before any filtering.
3. The hook (which runs on the driver/RX task) parses a few fields and enqueues a
   compact summary; a separate task prints it, so the RX path stays fast.

Each line looks like:

```
#42 Beacon     A2=00:00:00:00:00:00 A3=00:00:00:00:00:00 rssi=-2 915500kHz len=84 mesh="rimba-mesh"
```

`type`, source/BSSID (A2/A3), RSSI (dBm), receive frequency, length, and the SSID
or Mesh ID for beacons/probe-responses.

## Build & run

```sh
make build   APP=rimba-halow-mesh-monitor BOARD=proto1-fgh100m
make flash   APP=rimba-halow-mesh-monitor BOARD=proto1-fgh100m PORT=/dev/ttyACM2
make monitor APP=rimba-halow-mesh-monitor BOARD=proto1-fgh100m PORT=/dev/ttyACM2   # serial console
```

Configuration (top of `main/app_main.c`):

- `MON_S1G_CHAN` — S1G channel to listen on (default 27 = US 915.5 MHz, 1 MHz).
- `MON_MESH_ID`  — mesh ID for the listening vif. Use one **distinct** from the
  network under test; a mesh-mode monitor still captures beacons from other mesh
  IDs (verified), so a distinct ID keeps the monitor from joining the target mesh.

## What it can and cannot capture (firmware-gated)

The MM6108 firmware does **not** offer a true promiscuous mode. It surfaces beacons
to the host according to the **local vif's mode/purpose**, so what this monitor sees
depends on the mode it runs in. Measured on fw 1.17.9, monitor in **mesh** mode:

| Target on the channel        | Captured? | Notes                                            |
|------------------------------|-----------|--------------------------------------------------|
| **Mesh** beacons (any Mesh ID) | ✅ yes   | Primary use case. ~per-node beacon rate.         |
| **IBSS** beacons             | ❌ no      | Firmware does not surface foreign IBSS beacons.  |
| **AP** beacons               | ❌ no      | Firmware does not surface AP beacons to a mesh vif. |
| Probe requests / action / data | ✅ (when delivered) | Mgmt/data frames the firmware passes to the datapath are surfaced. |

To monitor other modes you would build the monitor in that mode (swap the
`mmwlan_mesh_start()` bring-up for the STA/IBSS/AP equivalent), but note:

- **IBSS**: the firmware does not surface peer IBSS beacons in IBSS mode either
  (IBSS peer discovery on this firmware is *data-driven*, from data-frame
  addresses, not beacons). So an IBSS-mode monitor cannot see other IBSS nodes'
  beacons.
- **AP / infrastructure**: AP beacons reach a STA via the scan path, which is a
  firmware hardware-scan and does **not** pass through this datapath tap. A
  STA-mode monitor associated to an AP would see that AP's beacons, but this is not
  a general infrastructure sniffer.

In short: **this is a reliable mesh-beacon monitor**, and a best-effort monitor for
other frame types the firmware happens to deliver — not a universal promiscuous
sniffer, because the firmware does not support one.

## Known artifacts (firmware S1G⇄legacy conversion)

- **Source/BSSID addresses are usually zeroed (A2/A3 = `00:..:00`).** HaLow beacons
  are S1G Beacons on air (a single-address, compressed format). The firmware
  converts them to legacy beacons before handing them to the host and does not
  populate A2/A3 on this path. The frame body (timestamp, beacon interval,
  capability, IEs incl. SSID/Mesh ID) is intact; only the addresses are lost.
  Consequently a node cannot identify a *peer* from a beacon here — matching the
  documented "data-driven discovery" behaviour.
- **RSSI** is delivered and valid (will read near 0 dBm for boards a few cm apart).
- **Frequency** is reported in units of 100 kHz (e.g. `915500kHz` = 915.5 MHz).

## Relation to other apps

`rimba-halow-scan` uses `mmwlan_register_rx_frame_cb()`, which only fires for
management sub-types that pass the datapath's BSSID filter — so it never sees
foreign beacons. This app uses the new `mmwlan_register_monitor_cb()` tap, which is
upstream of that filter, which is why it can see them.
