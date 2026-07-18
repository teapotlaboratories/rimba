# rimba-halow-scan — scan for Wi-Fi HaLow APs

First MM6108 bring-up example: boots the HaLow radio over SPI (loads firmware +
BCF), prints version/chip info, then does an S1G scan on a loop and prints every
AP it hears — SSID, operating bandwidth, BSSID, RSSI, beacon interval, and the
security (AKM) it advertises. This is the repo's **default `APP`**, so a bare
`make build` / `make flash` builds it.

## Build + flash

Run from the repo root. Pick the `PORT` your board enumerates on
(`/dev/ttyACM*`):

```bash
make build APP=rimba-halow-scan BOARD=proto1-fgh100m
make flash APP=rimba-halow-scan BOARD=proto1-fgh100m PORT=/dev/ttyACM0
make monitor APP=rimba-halow-scan PORT=/dev/ttyACM0        # serial console, Ctrl-] to quit
```

Because this is the default `APP`, `make build` / `make flash BOARD=... PORT=...`
(without `APP=`) build this app too.

## Pairing with the other examples

Point this at any HaLow transmitter to confirm it is on air and read back the
security it advertises:

- **`rimba-halow-ap`** — flash a second board as a SoftAP; this scanner should
  list its SSID and AKM.
- **`rimba-halow-mesh`** / **`rimba-halow-ibss`** — a mesh/IBSS node's beacons
  show up here too, so it doubles as a quick "is my node beaconing?" check.

## What to change for your own network

The scanner needs no SSID/PSK — it just listens — but two things are worth
knowing:

- **Regulatory domain.** The scanned channels come from the board's country
  code, `CONFIG_HALOW_COUNTRY_CODE` under [`boards/<BOARD>/`](../../boards/). It
  defaults to `"??"`, which reports no channels and the scan finds nothing — set
  it to your region (`AU CA EU GB IN JP KR NZ US`).
- **Re-scan interval.** `app_main()` re-scans every 4 s
  (`pdMS_TO_TICKS(4000)`); change that delay for a faster/slower refresh, or
  drop the loop for a single boot-time scan.
