# rimba-halow-ap

An 802.11ah (Wi-Fi HaLow) **SoftAP** example for the ESP32-S3 + Morse Micro
MM6108. It brings up a SAE + PMF-secured SoftAP, pins a static IP so the AP
answers ICMP (mmhalow runs no DHCP server in AP mode), and logs each station as
it joins or leaves. It carries a **high-density AP config** — up to 255
associated stations, with the per-STA state routed to PSRAM.

## Build + flash

Run from the repo root. Flash this board as the AP, and a second board with
[`rimba-halow-sta`](../rimba-halow-sta/) as the station that associates and pings it:

```bash
make build APP=rimba-halow-ap  BOARD=proto1-fgh100m
make flash APP=rimba-halow-ap  BOARD=proto1-fgh100m PORT=/dev/ttyACM0   # this AP
make flash APP=rimba-halow-sta BOARD=proto1-fgh100m PORT=/dev/ttyACM1   # the STA

make monitor APP=rimba-halow-ap BOARD=proto1-fgh100m PORT=/dev/ttyACM0  # serial console (Ctrl-] to quit)
```

The AP comes up at `192.168.12.1`; the STA pins `192.168.12.2` on the same subnet
and pings the AP. Watch both consoles to see the station authorize and the ping
round-trips.

## What to change for your own network

Edit the parameters at the top of [`main/app_main.c`](main/app_main.c):

- `LINK_SSID` / `LINK_PSK` — the network name and passphrase (>= 8 chars, required
  for SAE). Keep these in sync with `rimba-halow-sta`.
- `LINK_S1G_CHAN` / `LINK_OP_CLASS` — the S1G channel and global operating class
  (defaults to US 915.5 MHz, 1 MHz BW). Match your regulatory domain.
- `AP_IP` / `NETMASK` — the SoftAP's static IP / gateway and mask.

## High-density config knobs

The 255-STA capacity is set in [`sdkconfig.defaults`](sdkconfig.defaults):

- `CONFIG_HALOW_AP_MAX_STAS=255` — the AP admission ceiling. `LINK_MAX_STAS` in
  `app_main.c` (the runtime `max_stas`) is set to match. 255 is the max of the
  `uint8_t max_stas` field.
- `CONFIG_HALOW_STA_DATA_IN_PSRAM=y` — routes the per-STA `umac_sta_data`
  (~912 B each) and the TWT agreement table to PSRAM instead of the small
  internal-SRAM pool. At 255 STAs that is ~230 KB, so PSRAM is required.

For a small deployment, lower both `CONFIG_HALOW_AP_MAX_STAS` and `LINK_MAX_STAS`.
