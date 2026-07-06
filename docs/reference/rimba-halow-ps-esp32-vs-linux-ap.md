# HaLow STA power-save: ESP32 SoftAP vs Linux AP — test setup, method, and measurements

**Date:** 2026-07-05 — **re-run at matched firmware** (supersedes the earlier version-skewed pass).
**Firmware (both sides, verified): stock `rel_1_17_9_2026_Apr_20`.** The ESP32 boards run 1.17.9 chip fw
(from `vendor/morse-firmware`); the Linux nodes run 1.17.9 **driver + fw + dot11ah + hostapd_s1g +
wpa_supplicant_s1g + morse_cli**, all **pure stock, no patches** (driver srcver `65FDC1A3…`). So this is
a clean, matched-version comparison — no firmware-skew confound. (The earlier pass had the Linux side on
1.17.8; those figures are superseded.)

**Question answered:** does a HaLow STA save the same power against the all-ESP32 morselib SoftAP as
against a Linux `hostapd_s1g` AP?
**Answer (matched 1.17.9):** **no-PS, dynamic-PS, and WNM are equivalent**; for **mid-session TWT the
ESP32 AP is ~2× better** — it engages the TWT schedule (STA deep-dozes to ~6.8 mA), while the Linux
`hostapd_s1g` does not (STA stays at dynamic-PS level ~14.7 mA). This holds at matched firmware, so it's
a real AP-implementation difference, not a version artifact.
**Adding ESP32 host light sleep** (§3c) sharpens the divergence: it *backfires* on per-DTIM PS (raising
current to ~32 mA) but pays off with rare wakes — so **TWT + host light sleep is a ~3–4 mA leaf against
the ESP32 AP** (rare 10 s wakes) yet ~32 mA against the Linux AP; WNM+powerdown+host-light-sleep ≈ 3.7 mA
on both; deep-sleep ≈ 2.9 mA (loses the link).

Related worklogs: `docs/worklog/2026-07-05-esp32-ap-ps-retest-matched-1179.md` (this matched retest),
`docs/worklog/2026-07-05-esp32-ap-wnm-responder-and-ps-beacon.md` (WNM responder 5-part fix, PS-disable
root cause, TWT + R3 verification), `docs/worklog/2026-07-04-halow-sta-powersave-ppk2.md` (early ladder).

---

## 1. Test setup

### Topology

```
                    ┌────────────────────────────────────────────┐
                    │           S1G channel 27 (5560 in iw)      │
                    │                                            │
  ┌──────────────┐  │   ┌───────────────┐      ┌──────────────┐  │
  │ board0 (AP)  │◄─┼──►│ board2 (STA)  │      │ chronium     │  │
  │ ESP32-S3 +   │  │   │ ESP32-S3 +    │      │ Pi + MM6108  │  │
  │ FGH100M      │  │   │ FGH100M       │      │ morse0 =     │  │
  │ rimba-halow- │  │   │ rimba-halow-  │      │ S1G MONITOR  │  │
  │ ap           │  │   │ sta           │      │ (ground      │  │
  │ 192.168.12.1 │  │   │ 192.168.12.51 │      │  truth)      │  │
  └──────────────┘  │   └───────┬───────┘      └──────────────┘  │
        ▲           │           │                                │
        │ swap AP   │        ┌──▼───────────┐                    │
  ┌─────┴────────┐  │        │ PPK2         │                    │
  │ chronite     │  │        │ source-meter │                    │
  │ Pi + WM6180  │  │        │ 5.00 V DUT   │                    │
  │ hostapd_s1g  │  │        │ rail, logs   │                    │
  │ 192.168.12.1 │  │        │ mA per 1 s   │                    │
  └──────────────┘  │        └──────────────┘                    │
                    └────────────────────────────────────────────┘
```

- **board2 (DUT, always the measured device):** Seeed XIAO ESP32-S3 + Morse Micro FGH100M (MM6108),
  `BOARD=proto1-fgh100m`, chip fw **1.17.9** (`rel_1_17_9_2026_Apr_20`). Powered **only** by the PPK2 rail.
- **AP under test — either:**
  - **board0**: identical ESP32-S3 + FGH100M running the morselib SoftAP (`rimba-halow-ap`), or
  - **chronite**: Raspberry Pi 5 + MM6108, `hostapd_s1g` + `morse_driver`, all stock **1.17.9**.
  Only one AP is powered at a time; both use the **same SSID/security** so the identical STA firmware
  connects to whichever is up.
- **chronium:** Raspberry Pi + MM6108 in monitor mode — every claim is cross-checked on-air here.
- **Link config (identical for both APs):** SSID `rimba-ping`, SAE (password `rimbahalow`), PMF required
  (`ieee80211w=2`), **`dtim_period=1`**, beacon interval 100 TU, S1G ch 27.

### Power measurement

Nordic **PPK2** in *source-meter* mode: it **is** board2's power supply, set to **5.00 V**, sampling
current and logging a 1 s average, epoch-stamped:

```
~/pwr_test/ppk2_mon2.py           # robust drain+log monitor -> board2_pwr_sweep.log
# log lines:  epoch=1783288532 avg=6.2mA
```

Power below is computed as **P = 5.00 V × I** (whole-board: ESP32 + radio + regulator losses).
Board hardware floor (deep sleep, everything off) is ~2.9–3 mA ≈ 15 mW — the practical minimum.

Board2 quirks that matter for reproduction:
- Native USB-JTAG serial does **not** auto-reset on open — reset via
  `esptool --chip esp32s3 --port /dev/ttyACM4 --after hard_reset flash_id`.
- Every measurement window is aligned by resetting board2 at a recorded `T0=$(date +%s)` and slicing
  `board2_pwr_sweep.log` on `epoch >= T0`.
- The PPK2 monitor can stall silently — check the last log timestamp is fresh before trusting a reading.

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

## 2. Firmware and commands

### Build / flash (dev machine)

```
make build APP=rimba-halow-ap  BOARD=proto1-fgh100m       # ESP32 SoftAP
make flash APP=rimba-halow-ap  BOARD=proto1-fgh100m PORT=/dev/ttyACM0
make build APP=rimba-halow-sta BOARD=proto1-fgh100m       # STA test app (variants below)
make flash APP=rimba-halow-sta BOARD=proto1-fgh100m PORT=/dev/ttyACM4
# post-test radio silence:
make flash APP=rimba-hello     BOARD=proto1-fgh100m PORT=/dev/ttyACMx   # both ESPs
```

**Prerequisite (in-tree fixes, see the 2026-07-05 worklog):** the ESP32 AP results require the
**WNM-sleep responder fix chain** (4 files in the halow submodule) — without it WNM enter blocks forever.
TWT needed **no** AP changes.

### STA app call sequence (the part that actually decides the power)

```c
/* 1. connect (SAE) ... then: */
mmwlan_set_power_save_mode(MMWLAN_PS_ENABLED);
/* ^ REQUIRED: mmhalow_init() force-disables PS (mmhalow.c:200). Without this line the STA
     sits at ~64-66 mA against ANY AP — this was misdiagnosed as an AP gap for half a day. */

/* dynamic PS: nothing else needed — chip dozes between DTIMs after ~100 ms idle */

/* TWT (scheduled doze): */
struct mmwlan_twt_config_args twt = MMWLAN_TWT_CONFIG_ARGS_INIT;
twt.twt_mode                 = MMWLAN_TWT_REQUESTER;
twt.twt_wake_interval_us     = 10000000;     /* 10 s SP (5 s in the downlink test) */
twt.twt_min_wake_duration_us = 65280;
twt.twt_setup_command        = MMWLAN_TWT_SETUP_REQUEST;
mmwlan_twt_setup_request(&twt);              /* mid-session S1G action exchange */
/* ... later: */ mmwlan_twt_teardown();

/* WNM sleep (deepest): */
struct mmwlan_set_wnm_sleep_enabled_args a = { .wnm_sleep_enabled = true,
                                               .chip_powerdown_enabled = true };
mmwlan_set_wnm_sleep_enabled_ext(&a);        /* basic mmwlan_set_wnm_sleep_enabled() does NOT
                                                power the radio down */
```

For the **downlink (R3) test** the STA also brings its netif up with a static IP so it is pingable:
`192.168.12.<mac[5]>` → board2 = `192.168.12.51` (`esp_netif_set_ip_info` + `esp_netif_action_connected`).
For the **idle ladder** the STA is netif-less (zero uplink traffic).

### AP-side downlink generator (1 Hz ping to the STA)

- **ESP32 AP:** built into `rimba-halow-ap` — `esp_ping` session per authorized STA,
  `interval_ms=1000, count=INFINITE`, target `192.168.12.<mac[5]>`; successes/timeouts on the AP UART.
- **Linux AP:** `ping -i 1 -D -O 192.168.12.51 > /tmp/apping.log` on chronite.

### Linux AP bring-up (chronite)

```
sudo hostapd_s1g -B /home/chronite/hostapd-rimba.conf     # ssid rimba-ping, SAE, dtim_period=1, PMF
sudo ip addr add 192.168.12.1/24 dev wlan1
# TWT responder attempts for the mid-session comparison:
#   enable_twt=1              -> REJECTED: "unknown configuration item" (this hostapd_s1g build)
#   ht_vht_twt_responder=1    -> accepted, but did NOT engage a mid-session TWT responder (no
#                                Setup Response on-air; see §4)
```

## 3. Measurements — the raw numbers

All at 5.00 V, board2 whole-board, 1 s samples. Representative slices from the aligned traces.

### 3a. Idle ladder (no traffic, netif-less STA) — **MATCHED stock 1.17.9, both APs same session (2026-07-05)**

One STA firmware (`rimba-halow-sta` ladder app, fw 1.17.9), one PPK2 rig, both APs the same session —
only the AP swapped. Each row is the avg over its ~18–36 s aligned window.

> **Measurement condition: ESP32-S3 host AWAKE — no host light sleep** (`CONFIG_PM_ENABLE` not set; the
> app configures no `esp_pm`/light-sleep). These rows **isolate the HaLow radio power-save modes** with
> the host running, which is the clean AP-comparison target (identical host-awake STA on both APs). They
> are **not** the absolute minimum: stacking ESP32 host light sleep pushes the deep tiers lower — the
> 2026-07-04 ladder measured **WNM + chip-powerdown + host-light-sleep ≈ 3.7 mA**, deep-sleep ≈ 2.9 mA —
> but host light sleep **"backfires" at `dtim_period=1`** for dynamic PS / TWT (host wakes every ~102 ms).
> The ~3 mA is the **board hardware floor**, not the CPU.

**vs ESP32 AP (board0, morselib SoftAP):**

| Mode | Current | Power | Notes |
|---|---|---|---|
| No PS | **66.0 mA** | **330 mW** | radio always on |
| Dynamic PS | **16.3 mA** | **81 mW** | wakes every DTIM (floor 9) |
| TWT, 10 s SP | **6.8 mA** | **34 mW** | deep SP-cadence doze (floor 5); on-air `SETUP ACCEPT` confirmed |
| WNM + chip powerdown | **5.0 mA** | **25 mW** | deepest (floor 4) |

**vs Linux AP (chronite, `hostapd_s1g` + `ht_vht_twt_responder=1`):**

| Mode | Current | Power | Notes |
|---|---|---|---|
| No PS | **65.4 mA** | **327 mW** | radio always on |
| Dynamic PS | **14.5 mA** | **73 mW** | wakes every DTIM (floor 9) |
| TWT, 10 s SP | **14.7 mA** | **73 mW** | **≈ dynamic PS** — mid-session TWT not engaged (STA stays dyn-PS; no SP-cadence doze) |
| WNM sleep | **4.0 mA** | **20 mW** | deepest (floor 4) |

### 3b. Downlink-while-dozing (AP pings the STA at 1 Hz) — *earlier skewed pass (ESP 1.17.9 / Linux 1.17.8); not re-run at matched fw*

**vs ESP32 AP:**

| Phase | STA power trace | AP ping result | On-air pattern |
|---|---|---|---|
| Dynamic PS | ~25 mA steady (`27 27 24 24 26 25 …`) | **every ping answered**, RTT 40–130 ms | continuous 1/s delivery at DTIM |
| TWT 5 s SP | `4 4 4 4 24 4 4 4 4 22 …` → **~8 mA avg** | success cadence flips to **1-in-5** (seq 118, 123, 128, 133, 138 …), rest time out at 1 s but are **delivered late** | AP→STA + STA→AP **bursts at sec 37/42/47/52/57** — pings buffered, flushed at each SP |

**vs Linux AP (chronite):**

| Phase | STA power trace | AP ping result | On-air pattern |
|---|---|---|---|
| Dynamic PS | ~25 mA (min 19, max 38, avg 25; 0/123 s < 12 mA) | 127 replies / 12 lost, RTT 46–126 ms | continuous 1/s delivery at DTIM |
| TWT 5 s SP (mid-session) | **unchanged ~25 mA** | continuous replies (still dynamic PS) | STA sends `TWT SETUP REQUEST` — **AP never responds** (no ACCEPT/REJECT on-air), no bursts |

### 3c. + ESP32 host light sleep (`esp_pm`, tickless) — the multiplier that only pays off with RARE wakes

Second pass with `CONFIG_PM_ENABLE` + `CONFIG_FREERTOS_USE_TICKLESS_IDLE` + `esp_pm_configure(160/40 MHz,
light_sleep_enable=true)` after connect — the ESP32-S3 host light-sleeps between radio wakes. **Host
light sleep is a multiplier, not a free win:** each host wake is a full light-sleep exit (PLL relock +
cache restore ≈ 700 mA spike, ~ms), so it only nets a saving when radio wakes are **rare**.

> ⚠️ **Measurement caveats (light-sleep pass, 2026-07-05):** the USB-serial-JTAG console flaps during
> light sleep (sparse markers → aligned by the one surviving marker + fixed phase durations), the PPK2
> monitor stalls intermittently on the deep <5 mA states, and — importantly — the **WNM *enter*
> handshake can sleep through under host light sleep** (observed the WNM phase stuck at ~32 mA one run,
> i.e. WNM didn't engage). So these are **indicative** (documented "for now"); the clean
> WNM+powerdown+light-sleep floor comes from the 2026-07-04 dedicated run. **Future work:** a clean
> host-light-sleep re-measure needs a sleep-robust harness — phase markers that survive the USB flap (a
> GPIO phase pin, or fixed-timing alignment), a monitor that doesn't stall on the <5 mA states, and
> disabling host light sleep during the WNM enter/exit handshake so it doesn't sleep through it. The
> **relative pattern** (backfire on per-DTIM PS; TWT-vs-ESP-AP and WNM+powerdown as the ~3–4 mA winners)
> is solid regardless.

| Mode (+ host light sleep) | **ESP32 AP** | **Linux AP** | why |
|---|---|---|---|
| No PS | ~32 mA / 160 mW | ~31 mA | radio always on; host light-sleeps some but wakes constantly |
| Dynamic PS | ~32 mA / 160 mW | ~32 mA | **backfires** — per-DTIM (~102 ms) wakes = constant light-sleep exits |
| **TWT (10 s SP)** | **~3–4 mA / ~18 mW** ✅ | **~32 mA** ✗ | **the key divergence** — the ESP AP's TWT gives *rare* 10 s wakes so the host deep-light-sleeps between them; the Linux AP's TWT keeps DTIM wakes, so it backfires like dyn-PS |
| WNM + chip-powerdown | ~3.7 mA / 19 mW* | ~3.7 mA / 19 mW* | radio fully off → no IRQs → host light-sleeps for long stretches (*when the WNM enter succeeds under light sleep) |
| **Deep sleep** (CPU + radio off) | **~2.9 mA / 14 mW** | ~2.9 mA | loses the link, ~8 s reconnect per wake; ~3 mA is the **board hardware floor**, not the CPU |

**Takeaway:** host light sleep **backfires** on per-DTIM PS (no-PS, dynamic PS, and TWT-against-the-Linux-AP)
— it *raises* current to ~32 mA. It **pays off only with rare wakes**: WNM+powerdown (both APs, ~3.7 mA)
and — the standout — **TWT against the ESP32 AP (~3–4 mA)**, because only the ESP AP's TWT actually
suppresses the DTIM wakes. So the deepest *associated* low-power leaf (WNM/TWT + host light sleep, ~3.7 /
~3.5 mA) barely beats deep-sleep (2.9 mA) while keeping the link — deep-sleep only wins when wakes are so
rare the ~8 s reconnect amortises.

## 4. Comparison

### Summary table (P = 5.00 V × I)

Idle rows = **matched stock 1.17.9**, both APs measured the same session (2026-07-05), 5.00 V:

| Scenario | ESP32 AP | Linux AP | Verdict |
|---|---|---|---|
| No PS | 66.0 mA / 330 mW | 65.4 mA / 327 mW | **equal** (radio always on) |
| Dynamic PS, idle | 16.3 mA / 81 mW | 14.5 mA / 73 mW | ~equal (Linux marginally lower) |
| **TWT (10 s SP), idle** | **6.8 mA / 34 mW** | 14.7 mA / 73 mW | **ESP32 AP ~2× better** — engages the schedule |
| WNM + powerdown, idle | 5.0 mA / 25 mW | 4.0 mA / 20 mW | ~equal (Linux marginally lower) |
| Dynamic PS + 1 Hz downlink ³ | 25 mA / 125 mW | 25 mA / 125 mW | equal — both buffer + deliver at DTIM |
| TWT + 1 Hz downlink ³ | ~8 mA / 40 mW | 25 mA / 125 mW | ESP32 AP better (Linux TWT not engaged) |

³ downlink rows are from the **earlier version-skewed pass** (ESP 1.17.9 / Linux 1.17.8), not re-run at
matched fw — treat as indicative. The TWT idle divergence held at matched fw, so it is a real
AP-implementation difference: the ESP AP's mid-session TWT responder installs the schedule and the STA
deep-dozes (on-air `SETUP ACCEPT` confirmed both passes); chronite's `hostapd_s1g` does not engage the
mid-session responder (`enable_twt` = unknown config item; `ht_vht_twt_responder=1` accepted-but-inert),
so the STA falls back to dynamic PS.

### Battery perspective (ideal 2000 mAh)

| Mode (vs ESP32 AP) | Runtime |
|---|---|
| No PS | ~30 h |
| Dynamic PS | ~6 days |
| TWT 10 s | ~12 days |
| WNM sleep | ~18–20 days |

### Conclusions

1. **The all-ESP32 AP is a full peer of the Linux AP for STA power-save.** Baseline, dynamic PS, and
   downlink-buffering-at-DTIM are measurably identical.
2. **Mid-session TWT is where they diverge — in the ESP32 AP's favor.** morselib's AP answers a
   mid-session TWT Setup (S1G Unprotected action, cat 22) with ACCEPT and the FW schedules the doze;
   the STA then wakes only at the service period and downlink arrives as a buffered burst per SP.
   chronite's `hostapd_s1g` build exposes no working knob for the mid-session responder
   (`enable_twt` unknown; `ht_vht_twt_responder=1` accepted-but-inert). *Scope caveat:* Linux
   **assoc-embedded** TWT (`mmwlan_twt_add_configuration` before `sta_enable`) was not re-tested here;
   the 07-04 Linux TWT (~13 mA, DTIM-cadence) likely used that path.
3. **The #1 pitfall is on the STA, not either AP:** `mmhalow_init()` disables power-save
   (`mmhalow.c:200`). Any app wanting these numbers must call
   `mmwlan_set_power_save_mode(MMWLAN_PS_ENABLED)` after init — and use
   `mmwlan_set_wnm_sleep_enabled_ext(..., chip_powerdown_enabled=1)` for the deep WNM tier.
4. **Cadence, not average, proves TWT.** At `dtim_period=1`, dynamic PS and a non-installed TWT read
   the same ~13 mA average; the ~SP-interval wake-spike pattern (and the on-air burst delivery) is the
   evidence the schedule is real.

## 5. Post-test state

Per bench policy everything was radio-silenced after the run: `rimba-hello` flashed to board0 + board2,
`hostapd_s1g`/pingers killed and `wlan1` down on chronite + chronium (monitor back to managed).
Measurement scripts: `~/pwr_test/` (dev machine), decoders on chronium `/tmp/*.py` (tmpfs — re-push
after reboot).
