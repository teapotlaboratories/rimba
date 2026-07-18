# rimba-halow-ap-perf — HaLow AP-side iperf throughput

An 802.11ah (Wi-Fi HaLow) **SoftAP** example for the ESP32-S3 + Morse Micro
MM6108 that measures link throughput from the AP side. It brings up a SAE +
PMF-secured SoftAP, pins a static IP (`192.168.12.1`), and starts an
`esp_console` REPL so you can run `iperf -s` over the serial console. Pair it
with [`rimba-halow-sta-perf`](../rimba-halow-sta-perf/) — the station that
associates and runs `iperf -c 192.168.12.1` as the client.

## Build + flash

Run from the repo root. Flash this board as the AP server and a second board
with `rimba-halow-sta-perf` as the client:

```bash
make build APP=rimba-halow-ap-perf  BOARD=proto1-fgh100m
make flash APP=rimba-halow-ap-perf  BOARD=proto1-fgh100m PORT=/dev/ttyACM0   # this AP
make flash APP=rimba-halow-sta-perf BOARD=proto1-fgh100m PORT=/dev/ttyACM1   # the STA client

make monitor APP=rimba-halow-ap-perf BOARD=proto1-fgh100m PORT=/dev/ttyACM0  # serial console (Ctrl-] to quit)
```

## Running the test

1. On this AP's console, start the server: `iperf -s`
2. On the STA's console (`rimba-halow-sta-perf`), start the client once it has
   associated: `iperf -c 192.168.12.1`

The AP prints the measured throughput as the transfer runs. The `ping` command
is also registered on both consoles for a quick reachability check before you
start iperf.

## What to change for your own network

Edit the parameters at the top of [`main/app_main.c`](main/app_main.c) — and
keep them **identical** to `rimba-halow-sta-perf`, or the two boards will not
form a link:

- `LINK_SSID` / `LINK_PSK` — the network name and passphrase (>= 8 chars,
  required for SAE).
- `LINK_S1G_CHAN` / `LINK_OP_CLASS` — the S1G channel and global operating class
  (defaults to US 915.5 MHz, 1 MHz BW). Match your regulatory domain.
- `LINK_MAX_STAS` — how many stations the AP admits (defaults to 4).
- `AP_IP` / `NETMASK` — the SoftAP's static IP / gateway (the STA's iperf
  target) and mask.
