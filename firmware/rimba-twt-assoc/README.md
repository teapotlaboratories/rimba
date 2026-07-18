# rimba-twt-assoc — TWT power-save at association

A low-power HaLow **station** example. It brings up the MM6108 radio, requests a
**TWT (Target Wake Time)** agreement *before* associating so the request rides in
the (re)association IEs — the path a HaLow AP's assoc-time TWT responder (e.g.
hostapd's `he_twt_responder`, default-on) actually answers — then associates over
SAE and enables power-save. With the agreement up, the radio wakes only for its
scheduled service period (~every wake interval) instead of every DTIM beacon, so a
mostly-idle node spends far more time dozing.

A mid-session `mmwlan_twt_setup_request()` action frame is *not* answered by every
AP, so this assoc-embedded path is the portable way to bring TWT up.

## Build + flash

Run from the repo root. Pick the `PORT` your board enumerates on (`/dev/ttyACM*`):

```bash
make build APP=rimba-twt-assoc BOARD=proto1-fgh100m
make flash APP=rimba-twt-assoc BOARD=proto1-fgh100m PORT=/dev/ttyACM0
make monitor APP=rimba-twt-assoc PORT=/dev/ttyACM0        # serial console, Ctrl-] to quit
```

## Pairing with the other examples

This is a station — it needs a HaLow AP to associate to. Flash a second board with
the AP example and point this one at it:

```bash
make flash APP=rimba-halow-ap    BOARD=proto1-fgh100m PORT=/dev/ttyACM0   # the AP
make flash APP=rimba-twt-assoc   BOARD=proto1-fgh100m PORT=/dev/ttyACM1   # this TWT STA
```

Any HaLow AP with an assoc-time TWT responder works — a Linux `hostapd_s1g` AP with
`he_twt_responder` (its default) will negotiate the TWT agreement too. On the
console you should see the station associate, turn power-save on, and then only the
periodic heartbeat.

## What to change for your own network

- **`LINK_SSID` / `LINK_PSK`** (top of `main/app_main.c`) — the SAE SSID and
  passphrase of your AP. The defaults match `rimba-halow-ap`.
- **`TWT_WAKE_INTERVAL_US`** — how long the radio sleeps between service periods
  (default 10 s). Widen it for deeper idle sleep, shorten it for lower downlink
  latency.
- **`TWT_MIN_WAKE_DURATION_US`** — the awake window granted per service period.
- **Regulatory domain.** The operating channel comes from the board's country
  code, `CONFIG_HALOW_COUNTRY_CODE` under [`boards/<BOARD>/`](../../boards/). It
  defaults to `"??"` (no channels) — set it to your region
  (`AU CA EU GB IN JP KR NZ US`).
