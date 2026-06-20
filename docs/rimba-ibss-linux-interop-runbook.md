# P0.5 — Linux HaLow IBSS interop runbook

Bring up a Linux MM6108 node (`morse_driver` + mac80211) as the **reference
implementation** and join it to our open IBSS cell, to check our ESP32 port talks
to real Linux IBSS on the **same silicon**. Expands test plan
[`rimba-ibss-test-plan.md`](rimba-ibss-test-plan.md) §5 (I.1–I.5) with concrete
commands and the one genuine unknown.

> **Status: DRAFT — not yet run.** The exact S1G IBSS *join* syntax is unverified
> (see §3); resolve it empirically on the Linux node, then fold the working command
> back into this file and test-plan §5.

---

## 0. The headline caveat (read first)

**There is no vendor-documented IBSS bring-up for morse_driver.** The Morse Linux
Porting Guide (`docs/MM_APPNOTE-24_Linux_Porting_Guide.pdf`) documents only
**AP** (`hostapd_s1g`) and **STA** (`wpa_supplicant_s1g`) — both infrastructure.
The Teapot/Luckfox Linux reference *compiles* `NL80211_IFTYPE_ADHOC` support but
"never configures an IBSS — no proven IBSS recipe there." So the Linux side of
P0.5 is itself unproven; this runbook is as much about *discovering* the morse
IBSS recipe as running the test. That uncertainty is exactly why P0.5 is the
reference-impl risk gate before Phase 2.

---

## 1. Cell parameters (must match every ESP32 node, already on-air)

| Param | Value | Source |
|---|---|---|
| SSID | `rimba-ibss` | app |
| BSSID (pinned) | `02:12:34:56:78:9a` | app (no TSF merge either side) |
| S1G channel | 27, op_class 68, 1 MHz BW | verified on-air (S1G Operation IE) |
| Frequency | **915500 kHz** (915.5 MHz) | sniffer capture |
| Country / regdomain | US | app + `morse.ko country=US` |
| Security | OPEN (plaintext) | Phase 1 |
| IP | `192.168.13.<octet>`, octet = `mac[5]` (0→1, 255→254) | identical derivation on every node |

Our beacon currently carries **no Country IE and no S1G Short Beacon** — flagged as
a likely parser-divergence point vs morse_driver (see §6).

---

## 2. Linux prerequisites (from the porting guide + Luckfox reference)

```sh
# Kernel modules (insert in order). country=US to match our regdomain.
modprobe mac80211
modprobe crc7
insmod dot11ah/dot11ah.ko
insmod morse.ko bcf=bcf_fgh100mhaamd.bin spi_clock_speed=10000000 country=US
# Confirm a new phy + wlanX appears (dmesg). morse_cli must be in PATH.
iw reg set US
```

Needed in PATH: `morse_cli` (required for correct operation), `iw`, `ip`, and —
if the wpa_supplicant path (§3 method B) is used — `wpa_supplicant_s1g` from the
Morse `hostap` fork.

> **⚠ Version skew.** The Luckfox reference stack is MM **1.15.3** (driver/hostap,
> kernel `5.10.11/1.15.x`); our ESP32 firmware is **1.17.6**. Test-plan §2 wants
> generations matched. Prefer bringing the Linux driver/firmware up to **1.17.x**
> before trusting an interop result; if that's not possible, record the skew as a
> caveat on any failure (the beacon IE set can differ across generations).

---

## 3. Join the IBSS — the open question (try in order)

mac80211 advertises ADHOC, so `set type ibss` should be accepted. The unknown is
how morse expresses the **S1G ch27 / 915.5 MHz** in the join. Two candidate
methods; determine the real frequency table first:

```sh
# Inspect what the driver advertises for the S1G band — gives the exact freq arg.
iw phy
iw list | sed -n '/Frequencies/,/Bitrates/p'   # look for the ~900 MHz S1G channels
```

### Method A — `iw` ad-hoc (closest to our reverse-engineering basis)

```sh
iw dev wlan0 set type ibss
ip link set wlan0 up
# <FREQ> from `iw list`: try 915500 (kHz) or 915 (MHz) per what the morse iw fork
# accepts. fixed-freq + BSSID because neither side does TSF merge.
iw dev wlan0 ibss join rimba-ibss <FREQ> fixed-freq 02:12:34:56:78:9a
```

If `iw` rejects the S1G frequency (likely — mainline `iw` predates S1G), fall back
to Method B.

### Method B — `wpa_supplicant_s1g` ad-hoc (`mode=1`), OPEN

Mirrors how Linux runs IBSS, and uses the same S1G channel fields the guide's
hostapd example uses (`channel` / `op_class` / `s1g_prim_*`). Config:

```
ctrl_interface=/var/run/wpa_supplicant_s1g
network={
    ssid="rimba-ibss"
    mode=1
    bssid=02:12:34:56:78:9a
    frequency=915500            # or the S1G channel fields below if required
    # channel=27  op_class=68  s1g_prim_chwidth=1  s1g_prim_1mhz_chan_index=0
    key_mgmt=NONE               # OPEN — Phase 1
}
```

```sh
wpa_supplicant_s1g -D nl80211 -i wlan0 -c wpa_ibss.conf
```

> Whichever method joins, **record the exact working command here** and in test-plan
> §5, replacing the `<S1G_FREQ_FOR_CH27>` placeholder.

### Static IP (matches the ESP32 MAC-octet derivation)

```sh
MAC5=$(cut -d: -f6 /sys/class/net/wlan0/address)
ip addr add 192.168.13.$((16#$MAC5))/24 dev wlan0
ip link set wlan0 up
```

Because the ESP32 app auto-pings **every** discovered peer at `192.168.13.<octet>`,
once the Linux node holds its matching octet IP the ESP32 side begins pinging it
**without any change** — the N-node addressing makes the Linux node "just another
peer."

---

## 4. Verification — mapped to test-plan I.1–I.5

| # | What | Linux-side command | Pass |
|---|---|---|---|
| I.1 | Mutual discovery | `iw dev wlan0 scan \| grep -A4 rimba-ibss`; `iw dev wlan0 station dump` | Linux sees our SSID/BSSID **and** lists our ESP32 MACs; our `IBSS peers` dump shows the Linux MAC |
| I.2 | Beacon interop (both ways) | watch `dmesg -w` while joined | Linux ingests our beacons w/o error; ESP32 ingests Linux beacons w/o garbage/crash — **the riskiest divergence** |
| I.3 | Data path | `ping 192.168.13.<esp octet>`; ESP32 auto-pings Linux octet | bidirectional replies, low loss |
| I.4 | On-air frame diff | monitor iface + capture (below); decode our vs Linux beacon/probe/data | frames decode; diff explains any I.2 issue (closes backlog #11) |
| I.5 | Mixed 4-node cell | 3 ESP32 + Linux up together | all-pairs reachable + broadcast reaches everyone |

### I.4 monitor capture (also closes #11 — verify our S1G beacon on-wire)

```sh
iw phy phy0 interface add mon0 type monitor
ip link set mon0 up
tcpdump -i mon0 -w /tmp/rimba-ibss.pcap   # or wireshark; decode S1G beacon IEs
```

Diff our hand-built beacon (PV0 11n-form: IBSS cap bit, IBSS Param Set ATIM=0,
S1G Cap/Op, no TIM, **no Country IE**) against a morse_driver-emitted beacon.

---

## 5. Suggested order

1. Bring up Linux (§2), confirm phy + `country=US`; resolve the S1G freq table (§3 top).
2. Join the cell — Method A, else B (§3); pin BSSID; set the octet IP.
3. I.1 discovery → I.2 beacon (watch dmesg both sides) → I.3 ping → I.4 capture/diff.
4. Bring all 3 ESP32 up → I.5 mixed cell.
5. Record the working join command back into this file + test-plan §5; flip I.* marks.

---

## 6. Risk points to watch (why this might fail)

- **No vendor IBSS recipe** — the join syntax itself may need a morse-specific path
  (morse_cli, a fork-only iw flag, or a wpa_supplicant field) not yet known.
- **S1G beacon IE divergence** — our PV0 11n-form beacon vs morse_driver's dot11ah
  conversion; **missing Country IE / Short Beacon** on our side may trip the morse
  parser (I.2). This is the crux.
- **Version skew** 1.15.3 (Linux) vs 1.17.6 (ESP32) — IE sets can differ; match if
  possible.
- **BSSID pinning** — required both sides (no TSF merge); a mismatch silently
  splits the cell.
- **Regdomain** — both must be US, or the 915.5 MHz channel is unavailable/illegal.
