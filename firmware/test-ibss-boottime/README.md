# test-ibss-boottime — RISK-02 cold-boot → first-frame rig

The measurement fixture behind the **RISK-02** boot-time numbers: how long a cold ESP32-S3 + MM6108
takes to go from power-on to **joined an IBSS cell and exchanging frames**, and what that costs in
energy per wake.

Like [`test-power`](../test-power/README.md), this app emits **no `TEST|RESULT`** — it is not a T2
test. The verdict is a host-side PPK2 measurement the firmware cannot see. It is in
`tools/regtest/manifest.py` so the **T0 build matrix** keeps it compiling, and nothing more.

## The two roles

One flat binary; the role is a compile flag, so the harness can flash either side from one source.

| Build | Board | Role |
|---|---|---|
| `IBSS_CREATE=1` | board0 | **creator / peer.** Creates the provisioned cell, pins a static IP, and lets lwIP auto-reply to the DUT's pings. Always on. Prints its own IP at boot — that is the value to pass as `PING_IP=`. |
| *(default)* | board2 | **MEASURE DUT.** Joins the cell, stamps each boot phase with `esp_timer`, pings `PING_IP=` and prints one summary line at the first ICMP reply. |

Cell parameters (BSSID / SSID / channel) are identical to
[`rimba-halow-ibss`](../rimba-halow-ibss/README.md), so that example also works as the peer.

## Running it

```sh
# 1. bring the peer up first and note the IP it prints
make flash APP=test-ibss-boottime BOARD=proto1-fgh100m PORT=<board0> IBSS_CREATE=1

# 2. SWITCHING ROLES — clear the build first (see the warning below)
make fullclean APP=test-ibss-boottime BOARD=proto1-fgh100m

# 3. flash the DUT pointed at that IP, then read its console
make flash-monitor APP=test-ibss-boottime BOARD=proto1-fgh100m PORT=<board2> PING_IP=192.168.13.164
```

> ⚠️ **`IBSS_CREATE` is sticky — `make fullclean` between roles.** idf.py keeps a persistent
> `CMakeCache.txt` under `build/<app>/<board>/`, so a `-D TEST_IBSS_CREATE` from an earlier
> `IBSS_CREATE=1` build **survives** a later build that omits the flag. Both roles share one build
> directory, so building the creator and then "the DUT" without clearing silently reflashes the
> **creator** — you get two peers, no `BOOTTIME_MS` line, and something that looks exactly like a
> dead link. `tools/regtest/t2_onair.py:50` clears this cache for harness-driven apps, but this
> fixture is run by hand, so nothing clears it for you.
>
> Each role logs `ROLE=MEASURE (DUT)` or `ROLE=CREATE (peer)` as its first line — **check it before
> trusting a run.**

`PING_IP=` is the same make flag every other pinging fixture uses. The source default exists only so
a bare `make build` (the T0 matrix passes no flags) still compiles — it is a placeholder, not a bench
fact, so **always pass `PING_IP=` for a real run**.

## The output

The DUT prints exactly one machine-parseable line at the first ICMP reply:

```
BOOTTIME_MS app=.. fw=.. ibss=.. frame=.. total=..
```

| Phase | Spans |
|---|---|
| `app` | esp_timer start → `app_main` entry (ESP-IDF startup) |
| `fw` | `mmhalow_init` — MM6108 `.mbin` load over SPI + chip init + netif |
| `ibss` | `mmwlan_ibss_start` — ADD ADHOC vif + `cfg_ibss` JOIN + beaconing |
| `frame` | joined → first ICMP reply (IP up + ARP + first exchange) |
| `total` | `app + fw + ibss + frame` |

## Reading the number correctly

**`total` is not the cold-boot time.** `esp_timer` only starts once the ESP-IDF timer is up, so the
printed figure **excludes** the power rail ramp and the ROM / 2nd-stage bootloader. Measure that gap
host-side — PPK2 power-on edge → first current activity — and add it.

Two bench footguns, both recorded in `docs/worklog/2026-07-25-risk02-warm-reattach-handoff.md`:

- **board2 cannot do a deep-sleep timer wake while USB is attached** (`ESP_RST_USB`). Power-cycle the
  PPK2 DUT rail for each sample instead.
- **Do not time the energy window off the host clock.** USB re-enumeration makes it mis-read by tens
  of seconds. Anchor on the physical current rise in the PPK2 trace.

The DUT keeps pinging every 500 ms after it reports, so a current reading taken *after* the first
reply includes that traffic — segment the trace at the `BOOTTIME_MS` line if you want a clean idle
figure.
