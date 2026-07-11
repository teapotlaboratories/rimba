# Worklog — 2026-06-18 — HaLow AP↔STA ping (RISK-01 link proven)

**Author:** Aldwin
**Phase:** 1 — IBSS Foundation, tasks 1.2–1.3 (two-node link / RISK-01 fallback)
**Goal:** get two MM6108 boards talking IP to each other.

**Result: SUCCESS.** Two XIAO ESP32-S3 + MM6108 boards run a **bidirectional
ICMP ping** over an 802.11ah AP↔STA link — ~12 ms RTT, US 915.5 MHz, 1 MHz BW.

---

## What was built — `rimba-halow-ap` + `rimba-halow-sta`

Two apps (under `firmware/`), built on the vendored `halow` component:

- **`rimba-halow-ap`** — SoftAP (SAE, S1G chan 27 / op-class 68), static IP
  `192.168.12.1`, also pings the STA.
- **`rimba-halow-sta`** — associates, static IP `192.168.12.2`, pings the AP.

SSID `rimba-ping`, SAE passphrase, PMF required. The link params are duplicated
in both apps with a "MUST MATCH" comment.

Flash one board as AP, the other as STA (different ports), e.g.:
```
make flash APP=rimba-halow-ap  PORT=/dev/ttyACM0
make flash APP=rimba-halow-sta PORT=/dev/ttyACM1
```

---

## The two findings that mattered

Association came up easily (SAE auth, `STA link up`) — but IP traffic did **not**
flow at first. Two distinct issues, found by iterating on hardware:

1. **No DHCP in AP mode.** `mmhalow` always creates the netif with
   `ESP_NETIF_DEFAULT_WIFI_STA()` — a DHCP *client* — even when acting as an AP.
   There is no DHCP server, so the STA's auto-started DHCP client waits forever.
   → Both sides use **static IPs** (stop the DHCP client, `esp_netif_set_ip_info`).

2. **The AP never brings its netif up.** `mmhalow` calls
   `esp_netif_action_connected()` from its link-state callback, but in **AP mode
   that link-up never fires**, so the AP netif stays DOWN and lwIP silently drops
   every ICMP — association up, IP dead. **The fix is one line:** the AP calls
   `esp_netif_action_connected()` itself after setting its static IP. With that,
   pings flow immediately.

Ruling-out steps along the way (kept here so we don't repeat them): the STA TX
failures were unresolved ARP (static ARP entry made TX succeed but still no
replies); switching SAE→OPEN didn't help (ruled out encryption); the AP netif
being DOWN was the actual cause. Static-ARP and OPEN were diagnostic scaffolding
and have been removed — with the netif-up fix, **dynamic ARP and SAE both work**.

---

## Verified on hardware (both directions)

```
STA → AP (192.168.12.1):  reply ... ttl=64 time=12 ms   (0 timeouts)
AP → STA (192.168.12.2):  reply ... ttl=64 time=12 ms   (0 timeouts once STA up)
```

Symmetric ~11–27 ms RTT. **This validates the Phase-1 RISK-01 AP-STA link with a
working bidirectional IP data path on real MM6108 hardware.**

---

## Also this session: ESP-IDF vendored

ESP-IDF v5.4.2 is now a submodule at `vendor/esp-idf` (pinned), and the Makefile's
default `IDF_PATH` points there — the build no longer depends on an out-of-tree
install. The old `~/esp` + `~/.espressif` toolchains were removed and reinstalled
from the vendored tree.

## Build-system note

The Makefile now layers `boards/<BOARD>/sdkconfig.defaults` first, then the app's
own `firmware/<APP>/sdkconfig.defaults` (if present). `rimba-halow-ap` uses this
to add `CONFIG_HALOW_AP_MODE=y`; `rimba-halow-sta` needs no app override.

## Next steps

1. Move from ICMP to the real Rimba datapath: raw L2 frames (EtherType `0x88B5`)
   over this AP-STA link via `mmwlan_tx_pkt` / `mmwlan_register_rx_cb`.
2. Measure MM6108 boot time (RISK-02 / task 1.4).
3. Revisit whether IBSS is reachable at all, or commit to AP-STA for Phase 1.
