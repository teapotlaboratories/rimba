# rimba-deepsleep-cycle

A timer-woken **deep-sleep battery leaf** for the ESP32-S3 + Morse Micro MM6108
on 802.11ah (Wi-Fi HaLow). It associates to a HaLow SoftAP over SAE, holds the
link up briefly, then powers the radio fully off (MM6108 `RESET_N` held low
through sleep) and puts the ESP32-S3 into deep sleep. An RTC timer wakes it after
`SLEEP_S`; it cold-boots, re-associates, logs the reconnect latency, and repeats.

The radio is **fully powered off during sleep** — the MM6108 draws no current
between wakes, and the whole board sits at roughly 0.35 mA. This models a
low-duty-cycle sensor that only needs the link for a moment each interval.

## Build + flash

Run from the repo root. Pair it with a HaLow SoftAP on another board — the
[`rimba-halow-ap`](../rimba-halow-ap/) example — using a matching SSID/PSK:

```bash
make build APP=rimba-deepsleep-cycle BOARD=proto1-fgh100m
make flash APP=rimba-deepsleep-cycle BOARD=proto1-fgh100m PORT=/dev/ttyACM1   # this leaf

make monitor APP=rimba-deepsleep-cycle BOARD=proto1-fgh100m PORT=/dev/ttyACM1 # serial console (Ctrl-] to quit)
```

Each wake prints `RECONNECTED in <ms>` — the cold-boot-to-associated time, which
is the deep-sleep "tax" you pay per cycle.

## What to change for your own network

Edit the `#define`s at the top of [`main/app_main.c`](main/app_main.c):

- `LINK_SSID` / `LINK_PSK` — the network name and passphrase (>= 8 chars,
  required for SAE). Match the AP you associate to.
- `SLEEP_S` — the deep-sleep interval between wakes, in seconds (default 30).
  Raise it for a lower duty cycle and lower average current.
- `MM_RESET` — the GPIO wired to the MM6108 `RESET_N` line; held low through
  sleep to power the radio off. Match your board (default GPIO1 on proto1-fgh100m).
