# Worklog — 2026-07-05 — HaLow STA power-save: ESP32 AP vs Linux AP, retest at MATCHED stock 1.17.9

**Author:** Aldwin (with Claude Code)
**Goal:** re-run the ESP32-AP-vs-Linux-AP STA power-save comparison with **matched firmware**, after the
whole bench was brought to stock 1.17.9 — the earlier comparison had a version skew (ESP 1.17.9 / Linux
1.17.8) that confounded it.
**Result:** at matched fw, no-PS / dynamic-PS / WNM are **equivalent**; **mid-session TWT is ~2× better
on the ESP32 AP** (STA deep-dozes to 6.8 mA vs 14.7 mA — the Linux `hostapd_s1g` doesn't engage the
mid-session TWT responder). So the TWT divergence is a **real AP-implementation difference, not a
firmware artifact.**

## Firmware (verified, both sides)

**Stock `rel_1_17_9_2026_Apr_20`, no patches.** ESP boards: 1.17.9 chip fw (from `vendor/morse-firmware`).
Linux nodes: 1.17.9 driver (srcver `65FDC1A3A73287FD44CE6E2` = pure stock) + fw + dot11ah + `hostapd_s1g`
+ `wpa_supplicant_s1g` + `morse_cli`; BCF unchanged. See `docs/reference/rimba-linux-node-setup.md §1`.

## Setup

- **DUT (measured):** board2 = XIAO ESP32-S3 + FGH100M (MM6108), app `rimba-halow-sta` = the PS-ladder,
  powered only by a **PPK2 @ 5.00 V** (source-meter mode; `~/pwr_test/ppk2_mon2.py` → `board2_pwr_sweep.log`,
  1 s avg). Netif-free (zero traffic). Power = 5.00 V × I.
- **Host power condition: ESP32-S3 AWAKE — NO host light sleep** (`CONFIG_PM_ENABLE` not set; app configures
  no `esp_pm`). These numbers isolate the HaLow **radio** PS modes with the host running — the clean
  AP-comparison target (identical host-awake STA on both APs), NOT the absolute floor. Adding host light
  sleep lowers the deep tiers (2026-07-04: WNM+powerdown+host-light-sleep ≈ 3.7 mA, deep-sleep ≈ 2.9 mA)
  but backfires at DTIM1 for dyn-PS/TWT. ~3 mA = board hardware floor, not CPU.
- **AP under test — either:** board0 = ESP32 SoftAP (`rimba-halow-ap`, WNM responder + short_beacon_int,
  pinger off) **or** chronite = Pi 5 + MM6108 `hostapd_s1g` (SSID `rimba-ping`, SAE, `dtim_period=1`,
  `ht_vht_twt_responder=1`). Same SSID/security → identical STA firmware connects to whichever is up.
- **chronium** = morse0 S1G monitor (on-air TWT verification).
- Board hardware floor ≈ 2.9–3 mA (15 mW).

## Method — one ladder app, both APs

`rimba-halow-sta` walks the whole ladder in one aligned run, grep-able uptime markers:
connect → **P1 NO-PS** (`mmwlan_set_power_save_mode(MMWLAN_PS_DISABLED)`) 20 s → **P2 dynamic PS**
(`PS_ENABLED`) 20 s → **P3 TWT** (`mmwlan_twt_setup_request`, REQUESTER, 10 s wake_interval, 65280 µs
min_wake) 40 s → **P4 WNM** (`mmwlan_set_wnm_sleep_enabled_ext`, `chip_powerdown_enabled=1`) 40 s.
Reset board2 at a recorded `T0`, slice `board2_pwr_sweep.log` on `epoch ≥ T0 + phase_uptime`. **Read
spike CADENCE, not just the average** — at DTIM1, dynamic PS and a non-engaged TWT both read ~14 mA; only
the ~SP-interval wake pattern proves the schedule installed. (Pitfall carried from the prior worklog:
`mmhalow_init()` disables PS at `mmhalow.c:200`, so the app must re-enable it.)

## Results (per-phase board2 current, avg over the aligned window)

| Mode | **ESP32 AP** (board0) | **Linux AP** (chronite) |
|---|---|---|
| No PS | 66.0 mA / 330 mW | 65.4 mA / 327 mW |
| Dynamic PS | 16.3 mA / 81 mW (floor 9) | 14.5 mA / 73 mW (floor 9) |
| **TWT, 10 s SP** | **6.8 mA / 34 mW** (floor 5, SP-cadence spikes) | **14.7 mA / 73 mW** (≈ dyn-PS, no SP cadence) |
| WNM + chip powerdown | 5.0 mA / 25 mW (floor 4) | 4.0 mA / 20 mW (floor 4) |

Raw TWT traces (mA/1 s): ESP `6 6 5 6 11 16 5 5 5 6 … 21 6 6 5` (deep floor + periodic SP spikes);
Linux `30 11 13 11 32 10 13 10 30 … 22 11 10` (dynamic-PS-like, no deep floor). All phases returned
`ret=0` on the STA both APs.

**On-air (chronium morse0), matched 1.17.9:** vs the ESP32 AP — `board2→board0 SETUP REQUEST` then
`board0→board2 SETUP ACCEPT` (cat-22 S1G action, unprotected) → the STA installs the TWT agreement and
deep-dozes. vs the Linux AP — the STA sends the request but chronite's `hostapd_s1g` **doesn't engage**
the mid-session responder, so the STA stays in dynamic PS (matches the 14.7 ≈ 14.5 mA reading).

## + ESP32 host light sleep (2026-07-05 second pass)

Re-ran the ladder with `CONFIG_PM_ENABLE` + `CONFIG_FREERTOS_USE_TICKLESS_IDLE` +
`esp_pm_configure(160/40 MHz, light_sleep_enable=true)` (temp variant; reverted after). Host light sleep
is a **multiplier that only pays off with rare radio wakes** (each host wake = full light-sleep exit ≈
700 mA/ms spike). Measurement was fragile — USB-JTAG console flaps during light sleep (sparse markers,
aligned by the surviving `P2end=P3start=uptime66` + fixed durations), the PPK2 monitor stalls on the deep
<5 mA states, and the **WNM enter handshake can sleep through under host light sleep** (WNM phase stuck at
~32 mA one run). Indicative numbers:

| Mode + host-LS | ESP32 AP | Linux AP |
|---|---|---|
| No PS | ~32 mA | ~31 mA |
| Dynamic PS | ~32 mA (backfire) | ~32 mA (backfire) |
| **TWT (10 s)** | **~3–4 mA** ✅ | ~32 mA ✗ |
| WNM + powerdown | ~3.7 mA* | ~3.7 mA* (*2026-07-04 clean run) |
| Deep sleep | ~2.9 mA | ~2.9 mA |

**Standout:** host light sleep **backfires** on per-DTIM PS (no-PS, dyn-PS, TWT-vs-Linux) → ~32 mA; but
**TWT + host light sleep = ~3–4 mA against the ESP32 AP** (rare 10 s wakes let the host deep-sleep), which
does NOT happen against the Linux AP (its TWT keeps DTIM wakes). So the ESP AP's effective mid-session TWT
uniquely unlocks a ~3–4 mA *associated* leaf. WNM+powerdown+host-LS ≈ 3.7 mA both APs; deep-sleep 2.9 mA
(loses link, ~8 s reconnect). ~3 mA is the board hardware floor, not the CPU. Full table + caveats:
`docs/reference/rimba-halow-ps-esp32-vs-linux-ap.md §3c`.

## Conclusions

1. **The all-ESP32 SoftAP is a full peer of the Linux AP for STA power-save** — no-PS, dynamic PS, and
   WNM are within measurement noise (Linux marginally lower on dyn-PS/WNM: 14.5 vs 16.3, 4.0 vs 5.0 mA).
2. **Mid-session TWT is the one real divergence, in the ESP32 AP's favor** (~2×: 6.8 vs 14.7 mA). It held
   at matched firmware → it's a morselib-AP-vs-hostapd_s1g implementation difference, not a version
   artifact. *Scope:* only the mid-session TWT path (`mmwlan_twt_setup_request`) was tested; Linux
   **assoc-embedded** TWT (`mmwlan_twt_add_configuration` before enable) was not — chronite may support
   that path.
3. WNM is the deepest tier on both (~4–5 mA ≈ the board hardware floor).

## State
Full comparison written up in `docs/reference/rimba-halow-ps-esp32-vs-linux-ap.md` (superseding the
skewed pass). Bench radio-silenced after: `rimba-hello` on board0 + board2; chronite hostapd stopped +
`wlan1` down; chronium monitor→managed + `wlan1` down. Nothing committed.
