# rimba-halow-sta

An 802.11ah (Wi-Fi HaLow) **station** example for the ESP32-S3 + Morse Micro
MM6108. It brings up the radio, associates to a HaLow SoftAP over SAE, pins a
static IP on the AP's subnet (the SoftAP runs no DHCP server), and pings the AP
continuously — printing each round-trip time so you can watch the link on the
console.

## Build + flash

Run from the repo root. Flash this board as the station, and a second board with
[`rimba-halow-ap`](../rimba-halow-ap/) as the SoftAP it associates to and pings:

```bash
make build APP=rimba-halow-sta BOARD=proto1-fgh100m
make flash APP=rimba-halow-ap  BOARD=proto1-fgh100m PORT=/dev/ttyACM0   # the AP
make flash APP=rimba-halow-sta BOARD=proto1-fgh100m PORT=/dev/ttyACM1   # this STA

make monitor APP=rimba-halow-sta BOARD=proto1-fgh100m PORT=/dev/ttyACM1  # serial console (Ctrl-] to quit)
```

Bring the AP up first. The AP comes up at `192.168.12.1`; this station pins
`192.168.12.2` on the same subnet, associates, and pings the AP every 2 s. Watch
the console for `associated to "rimba-ping"` followed by the `reply from ... time=... ms`
lines.

## What to change for your own network

Edit the parameters at the top of [`main/app_main.c`](main/app_main.c):

- `LINK_SSID` / `LINK_PSK` — the network name and passphrase (>= 8 chars, required
  for SAE). Keep these in sync with `rimba-halow-ap`.
- `AP_IP` — the SoftAP's IP: the ping target and this station's default gateway.
- `STA_IP` / `NETMASK` — this station's static IP and mask, on the AP's subnet.

`CONNECT_TIMEOUT_MS` (how long to wait for association before idling) and
`PING_INTERVAL_MS` (the ping period) are also defined there if you want to tune them.
