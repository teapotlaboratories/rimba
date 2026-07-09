# HaLow STA power-save: ESP32 SoftAP vs Linux AP — setup, method, and measurements

> **Baseline: matched 1.17.8** (all bench components; verified 2026-07-08). The bench briefly ran 1.17.9
> (2026-07-05 → 08); 1.17.9 was found to **regress STA power-save ~2×** and was reverted. The 1.17.9 pass and
> two claims it produced (both later retracted) are preserved in **[Appendix A](#appendix-a--history--retractions)** —
> everything in §1–§6 is the current 1.17.8 data. Firmware/SHA map: `rimba-bench-devices.md`.

**Question:** does a HaLow STA save the same power against an all-ESP32 morselib SoftAP as against a Linux
`hostapd_s1g` AP — and what does adding an 802.11s **mesh gateway** cost?

**Short answer:** the three AP configs are close peers. **WNM+powerdown (~4 mA) is the deepest connected
tier on all of them.** TWT reaches ~6 mA via the **assoc-embedded** path (which both APs honour); the ESP AP
*additionally* answers a mid-session TWT request that the Linux AP ignores. The mesh gateway adds only ~4 mA
and only on the per-DTIM tiers, and still delivers downlink to a dozing leaf with 0% loss.

---

## 1. Results at a glance (matched 1.17.8)

board2 (XIAO ESP32-S3 + FGH100M) is the measured STA/DUT in every test — powered + metered by the PPK2 @
5.00 V, TX-capped to 1 dBm, triggered by the C6-harness. All three AP configs re-run at matched 1.17.8.

### Firmware / component versions

| Component | Repo (github.com/MorseMicro/…) | Ref | SHA |
|---|---|---|---|
| morse_driver (`morse.ko`+`dot11ah.ko`) | `morse_driver.git` | tag `1.17.8` | `f2f14e6` (Pi 5 + gpiod reset patch; Pi Zero stock) |
| MM6108 fw `mm6108.bin` (480664 B, md5 `cfe56db2`) | `morse-firmware.git` | "1.17.8 firmware (MM6108)" | `fd41e1c` |
| hostap (`hostapd_s1g`+`wpa_supplicant_s1g`) | `hostap.git` | tag `1.17.8` | `10fc5684a` |
| morse_cli | `morse_cli.git` | tag `1.17.8` | `8e9a860` |
| kernel | `rpi-linux.git` | `mm/rpi-6.12.21/1.17.x` | `372414fd` |

Same `mm6108.bin` (`fd41e1c`) on the Linux nodes (`/lib/firmware/morse`) **and** the ESP boards (bundled
from `vendor/morse-firmware`), so the STA + all AP configs run identical chip firmware.

### Idle radio-PS ladder — mA (host-awake / + host light-sleep)

| Mode | Linux AP | ESP32 AP | Mesh+AP gateway |
|---|---|---|---|
| No-PS | 64.8 / 64.0 | 64.6 / 64.6 | 65.8 / 66.5 |
| Dynamic PS | 9.1 / 8.2 | 15.3 / 10.7 | 13.0 / 15.7 |
| TWT (engaged, best path) | 6.0 / 5.8 | 6.6 / 7.0 | see grid ↓ |
| **WNM + powerdown** | 5.1 / **4.0** | 4.2 / 4.2 | 4.3 / 4.5 |
| Deep sleep (radio off) | ~0.35 | ~0.35 | ~0.35 |

### TWT — AP × STA-call grid (mA, host-awake / + light-sleep)

| STA call | Linux AP | ESP32 AP | Mesh-gate |
|---|---|---|---|
| mid-session (`twt_setup_request`) | ✗ 9.9 / 9.6 (ignored → dyn-PS) | ✅ 7.6 / 7.2 (engaged) | ✗ 13.0 / 15.8 (ignored) |
| assoc-embedded (`twt_add_configuration`) | ✅ 6.0 / 5.8 (engaged) | ✅ 6.6 / 7.0 (engaged) | ✅ ~6 (= plain Linux) |

**The ESP32 AP engages TWT on both paths; the Linux AP + mesh-gate (both `hostapd_s1g`) only on
assoc-embedded.** Assoc-embedded is the universal path (~6 mA on all APs). See [§3e](#3e-twt--which-sta-call-engages-which-ap).

### Downlink to a dozing STA

| Test | Delivery | RTT avg | board2 draw |
|---|---|---|---|
| §3b Linux AP, Dyn-PS + 1 Hz ping | 25/25, 0% loss | 80 ms | 23.6 mA |
| §3d Mesh R3 (through the full mesh path) | 25/25, 0% loss | 194 ms | ~20 mA |

### Sleep leaves & special modes

| Mode | Power | Associated? | Notes |
|---|---|---|---|
| WNM + powerdown | ~4 mA | ✅ | deepest while connected (chip powered down, link kept) |
| Deep-sleep cycle | ~0.35 mA asleep | ❌ | ~5.1 s reconnect; wins only for wakes rarer than ~2.5 min |
| STANDBY (deprecated) | 11.3 mA | ✅ | works but the chip stays active → loses to WNM |

### Battery (2000 mAh, idle)

No-PS ~31 h · Dyn-PS ~9 d · TWT ~14 d · **WNM+powerdown ~21 d** · deep-sleep ~238 d.

### Takeaways

- **All three AP configs are close peers.** WNM+powerdown (~4 mA) is the deepest connected tier everywhere;
  deep-sleep (~0.35 mA) only pays for rare wakes.
- **TWT works on both APs** (~6 mA via assoc-embedded); the ESP AP additionally answers the mid-session
  action, so the old "ESP-AP TWT 2× better" was really "ESP handles mid-session, Linux needs assoc-embedded."
- **The mesh costs ~4 mA and only on the per-DTIM tiers** (No-PS / WNM untouched) and delivers to a dozing
  leaf with 0% loss — no real mesh PS tax.
- **1.17.8, not 1.17.9.** 1.17.9 roughly doubles the doze current and makes host light-sleep backfire — the
  original "elevated PS numbers" mystery. See [Appendix A](#appendix-a--history--retractions).

---

## 2. Test setup

### Topology

board2 is the DUT in every test — the AP is the only thing that changes between test cases.

**Constant rig:**

```
   +----------+                     +---------------+                     +-------------+
   |   PPK2   | == 5 V + mA@1s ==>  |    board2     | <== GPIO20->D5 ==   | C6-harness  |
   | (powers  |                     |  STA / DUT    |   (triggers the     |  (ESP32-C6) |
   |  + meas) |                     | TX-cap 1 dBm  |    PS ladder)       +-------------+
   +----------+                     +-------+-------+
                                            | HaLow S1G ch27 (5560 MHz) --> the active test's AP
   +----------+                             v
   | chronium |  Pi 5 -- on-air MONITOR / sniffer (passive; watches every test off-air)
   +----------+
```

**The AP changes per test case (only ONE AP runs at a time):**

```
   A. vs Linux AP          (§3a, §3b, §3c)
      board2 ===HaLow===>  +-------------+
                           |  chronite   |  Pi 5 -- hostapd_s1g AP
                           +-------------+

   B. vs ESP32 SoftAP      (§3a, §3c)
      board2 ===HaLow===>  +-------------+
                           |   board0    |  ESP32 -- morselib SoftAP
                           +-------------+

   C. vs Mesh+AP gateway   (§3d, + R3 downlink)
      board2 ===HaLow===>  +----------------------+  802.11s  +-------------+
                           |  chronite = Mesh+AP  | ==mesh==> | chronosalt  |
                           |  gateway (ap0 + mesh)|           | Pi Zero peer|
                           +----------------------+           +-------------+
```

- **board2 (DUT):** Seeed XIAO ESP32-S3 + Morse Micro FGH100M (MM6108), `BOARD=proto1-fgh100m`. Powered
  **only** by the PPK2 rail. Same STA firmware connects to whichever AP is up.
- **AP under test:** `board0` (ESP32-S3 + FGH100M, `rimba-halow-ap`) or `chronite` (Pi 5 + MM6108,
  `hostapd_s1g`) as a plain AP or as a Mesh+AP gateway. All use the same SSID/security.
- **Link config (identical for all APs):** SSID `rimba-ping`, SAE (`rimbahalow`), PMF required
  (`ieee80211w=2`), **`dtim_period=1`**, beacon interval 100 TU, S1G ch 27.
- **chronium (Pi 5):** monitor mode — every on-air claim (TWT frames, buffered-delivery bursts) is
  cross-checked here.

### Measurement method

Nordic **PPK2** in *source-meter* mode **is** board2's supply, set to **5.00 V**, logging a 1 s current
average, epoch-stamped:

```
~/pwr_test/ppk2_mon2.py           # drain+log monitor -> board2_pwr_sweep.log
# log lines:  epoch=1783288532 avg=6.2mA
```

Power is **P = 5.00 V × I** (whole-board: ESP32 + radio + regulator). Each ladder phase is a fixed 18 s
window; the C6-harness pulses board2's D5 to timestamp phase entry, so the trace self-aligns even if the USB
console drops (see `firmware/rimba-halow-sta`).

Board2 quirks that matter for reproduction:
- Native USB-JTAG serial does **not** auto-reset on open — reset via
  `esptool --chip esp32s3 --port /dev/ttyACM4 --after hard_reset flash_id`.
- Align each window by resetting board2 at a recorded `T0` and slicing `board2_pwr_sweep.log` on `epoch >= T0`.
- The PPK2 monitor can stall silently on the deep <5 mA tiers — check the last log timestamp is fresh.

> **Deep-sleep floor:** the true radio-off floor is reached by holding the MM6108 in hardware reset
> (`RESET_N`/GPIO1 LOW) + ESP32 deep sleep → **~0.35–0.6 mA** (`firmware/rimba-sleep-test` /
> `rimba-deepsleep-cycle`). The older "~2.9 mA floor" used `mmwlan_shutdown()`, which resets but does **not**
> power-gate the chip — treat any "~2.9 mA" in the history below as the `mmwlan_shutdown` number, not the floor.

### On-air verification (chronium)

```
sudo ip link set wlan1 down && sudo iw dev wlan1 set type monitor
sudo ip link set wlan1 up   && sudo iw dev wlan1 set freq 5560
sudo ip link set morse0 up
# then AF_PACKET/SOCK_RAW python decoders (no tcpdump on the Pis), e.g. /tmp/twtcap.py, /tmp/dlcap.py:
#  - S1G action frames: category 22 (S1G Unprotected), action 6 = TWT Setup, 7 = Teardown,
#    TWT element EID 216, setup_command sub-field: REQUEST(0)/ACCEPT(4)/REJECT(7)
#  - per-second AP->STA / STA->AP data-frame counts (burst pattern = buffered delivery)
```

---

## 3. Detailed measurements (matched 1.17.8, PPK2 @ 5 V, board2 TX-cap 1 dBm)

The summary tables live in [§1](#1-results-at-a-glance-matched-1178); this section is the per-test detail,
conditions, and on-air evidence.

### 3a. Idle ladder — board2 vs both plain APs

One STA firmware (`rimba-halow-sta` triggered ladder), one PPK2 rig, both plain APs; only the AP swaps. The
ladder walks **No-PS → Dynamic PS → TWT → WNM+powerdown**, each an 18 s phase, host-awake (§3a) and again
with ESP32 host light-sleep (§3c). Numbers: [§1 ladder table](#idle-radio-ps-ladder--ma-host-awake---host-light-sleep).

- **No-PS ~65 mA, Dynamic PS ~9 (Linux) / ~15 (ESP)** — the ESP AP runs Dyn-PS a few mA higher; otherwise the
  two APs track closely.
- **WNM+powerdown (~4–5 mA) is the deepest associated tier on both APs** — the chip is powered down and the
  link is kept. At 1.17.8 this is reached **host-awake** (radio off → the board's tickless idle drops the
  whole board deep); host light-sleep does *not* backfire here. (The earlier "host-awake can't be below
  ~15 mA" claim was a 1.17.9-regression artifact — see [Appendix A](#appendix-a--history--retractions).)
- **TWT ~6 mA** — see [§3e](#3e-twt--which-sta-call-engages-which-ap) for which STA call engages which AP.

### 3b. Downlink-while-dozing (Linux AP)

board2 at static IP `.51`, Dynamic PS, `ping -i 1` from chronite: **25/25, 0% loss**, RTT 23–166 ms (avg 80 =
the DTIM buffering latency); board2 draws **23.6 mA** under the 1 Hz downlink (vs ~9 mA idle Dyn-PS) — the AP
buffers for the dozing STA and flushes at DTIM. (`iw station dump` shows 0 for a dozing STA — a display
artifact; the successful ping is the proof of delivery. ESP-AP downlink not re-run — its built-in pinger is
compiled out by default, `RIMBA_AP_PING_STAS=0`.)

### 3c. + ESP32 host light sleep (`esp_pm`, tickless)

Second pass with `CONFIG_PM_ENABLE` + `CONFIG_FREERTOS_USE_TICKLESS_IDLE` + `esp_pm_configure(160/40 MHz,
light_sleep_enable=true)` after connect. **Host light sleep is a multiplier, not a free win:** each host wake
is a full light-sleep exit (~700 mA spike, ~ms), so it only nets a saving when radio wakes are **rare**.

At 1.17.8 (the `+ light-sleep` column of the [§1 ladder](#idle-radio-ps-ladder--ma-host-awake---host-light-sleep)):
- **No effect / slight help** on the deep radio-off tiers (WNM+powerdown ~4 mA either way).
- **Slight help** on Linux Dyn-PS (9.1 → 8.2) but **slight backfire** through the mesh-gate (13.0 → 15.7),
  where the co-channel mesh IRQs make the constant light-sleep exits cost more.
- The dramatic ~31 mA "backfire" seen earlier was a **1.17.9** effect; at 1.17.8 light-sleep does not blow up.

### 3d. Mesh+AP gateway + R3 downlink

chronite as a mesh point (`wlan1` 10.9.9.3) **+** SoftAP (`ap0` .12.1) co-channel on one MM6108 + `ip_forward`;
chronosalt as a 2nd mesh node (10.9.9.4 + return route to .12.0/24); board2 the leaf on `ap0` (.51). Mesh+AP
concurrency works on 1.17.8 (mesh on the primary vif `wlan1`, AP on secondary `ap0`); chronite↔chronosalt
mesh ping 4/4, 0% loss. Bring-up: `scratchpad/mesh_gate_up.py` / `mesh_peer_up.py`.

- **Full ladder vs the mesh-gate** (host-awake / +LS), mA: No-PS 65.8/66.5, Dyn-PS 13.0/15.7, TWT(mid)
  13.0/15.8 (≈ Dyn — `ap0` is `hostapd_s1g`, so mid-session TWT is ignored, like the plain Linux AP;
  assoc-embedded would engage ~6 mA), WNM+powerdown 4.3/4.5.
- **Small mesh cost:** the co-channel mesh adds ~+4 mA to Dyn-PS (13 vs plain 9.1) and makes light-sleep
  slightly backfire on Dyn-PS (15.7 vs plain 8.2). **No-PS and WNM+powerdown are unaffected** (radio
  always-on / radio-off). WNM+powerdown (~4.5 mA) stays the deepest tier even through the mesh-gate.
- **R3 — downlink to a dozing leaf through the full mesh path** (chronosalt → HaLow mesh → gateway →
  `ip_forward` → `ap0` → dozing board2): **25/25, 0% loss**, RTT avg 194 ms (max 1.9 s = deep-doze delivery),
  board2 ~20 mA. The gateway buffers + delivers even when the traffic originates on the far side of the mesh.

A same-gateway mesh-on/off A/B (2026-07-05) confirmed the mesh has **no** measurable PS effect on any tier —
so there is no "mesh airtime tax"; the elevated numbers in the old §3a′ table were the 1.17.9 regression +
setup confounds (retracted — see [Appendix A](#appendix-a--history--retractions)).

### 3e. TWT — which STA call engages which AP

The old "Linux `hostapd_s1g` doesn't do TWT" was a **STA-API-path** artifact, not an AP-capability gap.
hostapd's TWT responder (`he_twt_responder`, default-on) advertises TWT in the beacon and negotiates it in the
**association** exchange, but does **not** answer a **mid-session** TWT Setup *action frame*.

- `mmwlan_twt_setup_request()` = a **mid-session action** → hostapd ignores it → STA stays at dyn-PS (~9.9 mA).
- `mmwlan_twt_add_configuration()` **before connect** = TWT in the **assoc IEs** → hostapd's responder
  installs the schedule → **engages**: 6.0 mA host-awake, clean 10 s SP cadence (~4 mA baseline + ~16 mA SP
  spikes every 10 s). `firmware/rimba-twt-assoc`.

The full grid is in [§1](#twt--ap--sta-call-grid-ma-host-awake---light-sleep). **The ESP32 AP engages TWT on
both paths; the Linux AP + mesh-gate only on assoc-embedded** — morselib's SoftAP answers both (hostapd base
+ a mid-session responder morselib added), so **assoc-embedded is the universal path** (~6 mA on all APs).
**TWT is NOT the deepest tier:** it keeps the chip powered to wake at each SP (~6–7 mA), whereas
WNM+powerdown powers the chip *down* (~4 mA). TWT's value is the *scheduled/predictable* wake for periodic
downlink; WNM+powerdown is the lowest stay-associated leaf.

### 3f. Deep-sleep as a duty-cycled leaf (`firmware/rimba-deepsleep-cycle`)

connect → deep-sleep (radio in `RESET_N`-low, **~0.35 mA**) → C6-trigger wake → **re-associate in ~5.1 s**
(cold boot + chip-fw reload + SAE; 5/5 clean cycles). Reconnect tax ≈ **11 s @ ~63 mA ≈ 690 mA·s per wake**.
Average ≈ `0.35 + 690/T` mA for wake interval T:

| Wake interval | Avg current | vs WNM+powerdown (~5 mA, stays associated) |
|---|---|---|
| 30 s | ~23 mA | much worse |
| 1 min | ~12 mA | worse |
| **~2.5 min** | ~5 mA | **break-even** |
| 5 min | ~2.7 mA | better |
| 10 min | ~1.5 mA | better |
| 1 h | ~0.6 mA | far better |

**So the two low-power leaves split by wake cadence:** deep-sleep (loses the link, ~5 s reconnect tax) wins
only for wakes rarer than ~2.5 min; for anything more frequent, WNM+powerdown stays associated at ~4–5 mA
with no reconnect tax and wins.

### 3g. STANDBY (morselib `MMWLAN_STANDBY`, deprecated)

The chip keeps the link + offloads DTIM-PS/ARP/DHCP/keepalive while the host sleeps, GPIO-waking the host.
Tested (`firmware/rimba-standby-test`): enter/exit ret=0, STA stays associated — but **~11.3 mA** (host
light-sleep), *worse* than WNM+powerdown (chip stays active at DTIM1 instead of powering down). It is a
functional offload, not a power win, and is deprecated → **WNM+powerdown remains the best low-power-while-
connected leaf.** No STA-side PMKSA caching (PMK code is mesh-only), so no fast-reconnect shortcut for the
deep-sleep path.

---

## 4. Firmware & commands (reference)

### Build / flash (dev machine)

```
make build APP=rimba-halow-ap  BOARD=proto1-fgh100m       # ESP32 SoftAP
make flash APP=rimba-halow-ap  BOARD=proto1-fgh100m PORT=/dev/ttyACM0
make build APP=rimba-halow-sta BOARD=proto1-fgh100m       # STA ladder (variants below)
make flash APP=rimba-halow-sta BOARD=proto1-fgh100m PORT=/dev/ttyACM4
# post-test radio silence:
make flash APP=rimba-hello     BOARD=proto1-fgh100m PORT=/dev/ttyACMx   # both ESPs
```

**Prerequisite (in-tree fixes, see the 2026-07-05 worklog):** the ESP32-AP results require the **WNM-sleep
responder fix chain** (4 files in the halow submodule) — without it WNM enter blocks forever. TWT needed
**no** AP changes.

### STA app call sequence (the part that decides the power)

```c
/* 1. connect (SAE) ... then: */
mmwlan_set_power_save_mode(MMWLAN_PS_ENABLED);
/* ^ REQUIRED: mmhalow_init() force-disables PS (mmhalow.c:200). Without this line the STA
     sits at ~64-66 mA against ANY AP — this was misdiagnosed as an AP gap for half a day. */

/* dynamic PS: nothing else needed — chip dozes between DTIMs after ~100 ms idle */

/* TWT — assoc-embedded (engages on BOTH APs): call BEFORE mmhalow_connect() */
struct mmwlan_twt_config_args twt = MMWLAN_TWT_CONFIG_ARGS_INIT;
twt.twt_mode                 = MMWLAN_TWT_REQUESTER;
twt.twt_wake_interval_us     = 10000000;     /* 10 s SP */
twt.twt_min_wake_duration_us = 65280;
twt.twt_setup_command        = MMWLAN_TWT_SETUP_REQUEST;
mmwlan_twt_add_configuration(&twt);          /* TWT in the (re)assoc IEs — hostapd honours this */
/* (mmwlan_twt_setup_request() is the MID-SESSION action; only the ESP32 AP answers it.) */

/* WNM sleep (deepest associated tier): */
struct mmwlan_set_wnm_sleep_enabled_args a = { .wnm_sleep_enabled = true,
                                               .chip_powerdown_enabled = true };
mmwlan_set_wnm_sleep_enabled_ext(&a);        /* basic mmwlan_set_wnm_sleep_enabled() does NOT
                                                power the radio down */
```

For the **downlink test** the STA brings its netif up with a static IP so it is pingable:
`192.168.12.<mac[5]>` → board2 = `192.168.12.51`. For the **idle ladder** the STA is netif-less (zero uplink).

### Linux AP bring-up (chronite)

```
sudo hostapd_s1g -B /home/chronite/hostapd-rimba.conf     # ssid rimba-ping, SAE, dtim_period=1, PMF
sudo ip addr add 192.168.12.1/24 dev wlan1
# TWT: he_twt_responder is default-on and negotiates TWT at ASSOCIATION (assoc-embedded path).
#   the old enable_twt / ht_vht_twt_responder knobs are about the MID-SESSION action, which this
#   hostapd_s1g build does not answer (see §3e) — not needed for the assoc-embedded path.
```

---

## 5. Conclusions

1. **The all-ESP32 AP is a full peer of the Linux AP for STA power-save.** No-PS, dynamic PS,
   downlink-buffering-at-DTIM, and the WNM+powerdown floor (~4 mA) are measurably equivalent.
2. **TWT engages on both APs (~6 mA) via the assoc-embedded path.** The only AP-implementation difference is
   that the ESP32 AP *also* answers a mid-session TWT Setup action, which `hostapd_s1g` ignores. Prefer
   `mmwlan_twt_add_configuration()` (assoc-embedded) for portability.
3. **WNM+powerdown is the deepest stay-associated leaf** (~4 mA, chip down, link kept); TWT is higher (~6 mA,
   chip stays up to wake at the SP); deep-sleep (~0.35 mA) wins only for wakes rarer than ~2.5 min.
4. **The mesh gateway costs only a few mA on the per-DTIM tiers** and still delivers downlink to a dozing leaf
   (R3, 0% loss). No mesh power penalty.
5. **The #1 pitfall is on the STA, not either AP:** `mmhalow_init()` disables power-save (`mmhalow.c:200`).
   Any app wanting these numbers must call `mmwlan_set_power_save_mode(MMWLAN_PS_ENABLED)` after init.
6. **Cadence, not average, proves TWT.** At `dtim_period=1`, dynamic PS and a non-installed TWT read the same
   average; the ~SP-interval wake-spike pattern (and on-air burst delivery) is the evidence the schedule is real.
7. **Pin the whole bench to 1.17.8.** 1.17.9 regresses these numbers ~2×.

---

## 6. Post-test state

Per bench policy, everything is radio-silenced after each run: `rimba-hello` flashed to the ESP boards,
`hostapd_s1g`/`wpa_supplicant_s1g`/pingers killed and `wlan1` down on all Pis (monitor back to managed).
Scripts: `~/pwr_test/` (dev machine), decoders on chronium `/tmp/*.py` (tmpfs — re-push after reboot).

Related worklogs: `docs/worklog/2026-07-08-1178-ps-baseline-and-deepsleep.md` (the authoritative 1.17.8
baseline + deep-sleep + TWT-path work), `2026-07-08-userspace-1179-and-ps-retest.md` (the 1.17.9 pass, with
its retraction), `2026-07-05-esp32-ap-wnm-responder-and-ps-beacon.md` (WNM responder 5-part fix),
`2026-07-05-board2-ps-vs-linux-mesh-ap.md` (mesh-gate + R3), `2026-07-04-halow-sta-powersave-ppk2.md` (early ladder).

---

## Appendix A — history & retractions

The bench ran **1.17.9** from 2026-07-05 to 2026-07-08. This appendix preserves that pass and the two claims
it produced, both of which were later disproved. **None of this is current** — §1–§6 above are the 1.17.8
data.

### A.1 The 1.17.9 STA power-save regression (the reason for the revert)

Same board2, same rig, same Linux AP, same TX cap — only the version differs:

| STA doze mode (vs Linux AP, host-awake, matched) | **1.17.8** | 1.17.9 |
|---|---|---|
| Dynamic PS | 9.1 mA | 20.2 |
| WNM+powerdown | 5.1 mA | 17.5 |
| Dynamic PS + host light sleep | 8.2 mA | 30.8 (backfire) |
| TWT + host light sleep | 9.6 mA | 30.8 (backfire) |

1.17.9 roughly **doubles** the doze current and makes host light-sleep **blow up to ~31 mA**. This is the
"elevated PS numbers" that prompted the whole investigation. The bench was reverted to matched 1.17.8 (all
components + the ESP vendor fw bin).

### A.2 RETRACTED — "host-awake WNM can't be below ~15 mA / the 4 mA needs light-sleep / ESP-AP TWT 2× better"

A 2026-07-08 analysis (on the still-1.17.9 bench) argued that a host-awake ESP32-S3 idles at ~15 mA, so a
host-awake 4 mA WNM figure was "impossible" and must have been mislabeled as light-sleep; and that TWT was a
mislabel / all tiers cluster at a ~15 mA floor. **All of that was the 1.17.9 regression, not a physical
floor.** At 1.17.8 the board genuinely reaches ~4 mA host-awake with the radio off (the CPU spends idle time
in WFI even with `esp_pm` off), and TWT engages cleanly at ~6 mA. The original ~4 mA WNM / ~6 mA TWT figures
are physically fine. The earlier "matched 1.17.9 / ESP-AP TWT ~2× better" framing is likewise superseded — at
matched 1.17.8 both APs do TWT at ~6 mA (assoc-embedded); the ESP AP's extra ability is only the mid-session
path ([§3e](#3e-twt--which-sta-call-engages-which-ap)).

### A.3 RETRACTED — "a Mesh+AP raises the STA's doze floors / airtime tax"

The 2026-07-05 mesh-gate table read high doze tiers (Dyn-PS ~20–35, WNM ~16 mA) and I first attributed them to
a mesh airtime tax. A same-gateway **mesh-on/off A/B** disproved it — toggling only the mesh vif changed
nothing (WNM 15.9 vs 16.9 mA). The elevated numbers were a **confounded comparison** (that run: TX-capped +
1.17.8 driver + AP on the secondary vif `ap0`; and, above all, the general 1.17.9 doze inflation). The clean
1.17.8 mesh-gate ladder ([§3d](#3d-meshap-gateway--r3-downlink)) shows the real mesh cost is ~4 mA on the
per-DTIM tiers only. The one solid mesh-gate PS fact that survived from that session is **R3** (downlink to a
dozing leaf works through the full mesh path), re-confirmed at 1.17.8.

### A.4 The 2026-07-05 matched-1.17.9 pass (for the record)

Idle, mA @ 5 V, both plain APs same session, host-awake unless noted (these are ~2× high vs 1.17.8):

| Mode | ESP32 AP | Linux AP |
|---|---|---|
| No-PS | 66.0 | 65.4 |
| Dynamic PS | 16.3 | 14.5 |
| TWT 10 s SP (mid-session) | 6.8 (engaged) | 14.7 (ignored → dyn-PS) |
| WNM + powerdown | 5.0 | 4.0 |

The §3b downlink rows in that pass (ESP TWT burst 1-in-5 delivery; Linux dyn-PS continuous) and the §3c
light-sleep pass (per-DTIM backfire to ~32 mA; TWT-vs-ESP-AP + WNM+LS as the ~3–4 mA winners) were real for
their conditions but are superseded by the matched-1.17.8 numbers in §1/§3. The mid-session-only TWT test in
that pass is what created the "ESP-AP TWT 2× better" headline that A.2 corrects.
