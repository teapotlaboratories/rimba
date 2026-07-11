# Worklog — 2026-07-04 — HaLow STA power-save: PPK2-measured (TWT / dynamic PS / WNM / deep sleep)

**Author:** Aldwin
**Goal:** actually **measure** the board-level current of every HaLow STA power-save feature on
board2 (the nRF PPK2 node), then push further by sleeping the **ESP32-S3 host** as well
(light-sleep and deep-sleep). Closes the long-open gap from the 2026-06-22 TWT worklog, which
confirmed TWT by *ping cadence* only — "no radio-rail current measured this round."
**Status:** complete. Full power-save ladder measured, board hardware floor identified. All test
firmware reverted; bench radio-silenced.

Self-contained record. Hardware: **board2** = Seeed XIAO ESP32-S3 + FGH100M (MM6108),
`BOARD=proto1-fgh100m`, morselib fw 1.17.6, powered through the **Nordic PPK2** in ampere-meter
mode (5 V passthrough). **AP** = chronite (Raspberry Pi + Wio-WM6180 MM6108), `hostapd_s1g`, morse
1.17.8, `enable_twt=Y`. Config `/home/chronite/hostapd-rimba.conf`: SSID `rimba-ping`, SAE
`rimbahalow`, S1G ch27, `beacon_int=100`, **`dtim_period=1`**, `wnm_sleep_mode=1`.

---

## Method

- **PPK2** powers board2 and logs current at **1 s** with a wall-clock epoch
  (`pwr_test/ppk2_mon_1s.py`, ampere mode, `set_source_voltage(5000)` needed even in passthrough).
- **Sweep firmware** (a temporary rework of `rimba-halow-sta`): associate, then hold each mode for a
  ~40 s **idle** window (NO traffic, so the radio dozes to its floor) with a grep-able marker
  `=== PHASE <name> START uptime=<s>s ===`. The PPK2 trace is sliced per phase by aligning the marker
  epochs to the current log. Phase durations are deterministic, so even when the serial log is lost
  (see below) the schedule anchors the slicing.
- All power-save calls are **morselib host API** — no GPIO/register/radio-IO poked; the API sends
  SPI command frames to the MM6108 fw, which runs the actual radio sleep states. Used:
  `mmwlan_set_power_save_mode()`, `mmwlan_set_dynamic_ps_timeout()`, `mmwlan_twt_setup_request()` /
  `mmwlan_twt_teardown()`, `mmwlan_set_wnm_sleep_enabled_ext()` (field `chip_powerdown_enabled`),
  `mmwlan_shutdown()`.

## 1. Radio power-save, ESP32 host awake (PM off)

| Mode | Board current | Power @5 V | vs baseline |
|---|---:|---:|---:|
| No PS (`MMWLAN_PS_DISABLED`, radio always on) | **64.2 mA** | 321 mW | — |
| Dynamic 802.11 PS (`PS_ENABLED`, DTIM1, 100 ms timeout) | ~13 mA | 65 mW | −80% |
| TWT, 1 s service period | ~13 mA | 65 mW | −80% |
| TWT, 10 s service period | ~13 mA | 66 mW | −80% |
| WNM sleep (multi-DTIM) | **6.7 mA** | 33 mW | −90% |
| WNM sleep + chip power-down | **6.1 mA** | 31 mW | −90% |

- **Any HaLow power-save cuts board2 80–90%** (321 mW → 31–68 mW). This is the number that was never
  measured before.
- **WNM sleep is deepest** because it sleeps across *several* beacons; dynamic PS and TWT wake at
  **every DTIM** (DTIM1 ≈ every 102 ms) so they idle higher (~13 mA), converging to ~6.7 mA only when
  fully silent (the post-sweep tail read 6.7 mA).
- **TWT ≈ dynamic PS at the board level** — both just park the radio between wakes. TWT's extra value
  (scheduled wake + AP downlink buffering) is latency/reliability and *letting the host sleep on a
  known schedule* — which needs host-sleep integration (§2) to show up in board current.
- Raw traces are flat within each mode with periodic DTIM-wake spikes (e.g. no-PS flat 64; WNM flat
  ~6 with rare wakes). Measurement is clean and repeatable.

## 2. + ESP32-S3 light-sleep (`esp_pm`, tickless, PM_ENABLE=y) — counterintuitive

Enabled `CONFIG_PM_ENABLE` + `CONFIG_FREERTOS_USE_TICKLESS_IDLE`, `esp_pm_configure(160/40 MHz,
light_sleep_enable=true)` after association.

| Mode | host awake | + host light-sleep | effect |
|---|---:|---:|---|
| No PS | 64 mA | **31 mA** | ✅ −33 |
| Dynamic PS | 13 mA | **32 mA** | ❌ +19 |
| TWT 1 s / 10 s | 13 mA | **32 mA** | ❌ +19 |
| WNM sleep | 6.7 mA | **33 mA** | ❌ +26 |
| **WNM sleep + chip power-down** | 6.1 mA | **3.7 mA** | ✅ **−2.4 — new associated floor** |

**Sleeping the host helped in two places and hurt everywhere else.** The 1 s averages hide fast
structure: within each second the current dips to **0 mA** (both asleep) and spikes to **~700 mA**
(wake). The problem is **wake frequency vs. wake cost**: at DTIM1 the radio wakes the host ~10×/s,
and each host wake is a full light-sleep exit (PLL relock + cache restore ≈ 700 mA spike). At 10
wakes/s the relock overhead **exceeds** the sleep savings → ~32 mA, *worse* than the no-PM `waiti`
idle (6–13 mA). No-PS improved (host was otherwise pinned awake). The winner is **WNM+powerdown**:
with the radio chip fully powered off there are no IRQs to wake the host, so it light-sleeps for long
stretches → **3.7 mA / 19 mW**, the lowest while **staying associated**.

**Takeaway:** host light-sleep only pays off when wakes are *rare*. Naively enabling `esp_pm` on top
of radio PS backfires. To win: cut the wake rate (raise AP `dtim_period` / STA listen interval) or
power the radio down.

## 3. ESP32-S3 deep sleep — the RTC leaf cycle

Leaf firmware: boot → re-associate → brief work → `mmwlan_shutdown()` → `esp_deep_sleep(30 s)` →
wake = full reboot → repeat. Deep sleep powers off CPU + RAM, so **the association is lost** and each
wake pays a cold-boot + re-associate tax. PPK2 trace shows a clean cycle:

| Phase | Board current | Duration |
|---|---:|---:|
| **Deep sleep** (CPU off + radio shut down) | **~2.9 mA / 14 mW** (min 2.72) | the sleep window |
| Wake → cold-boot → re-associate | ~70–85 mA | **~8 s per wake** |

**The ~3 mA floor is hardware, not the CPU.** The ESP32-S3 in deep sleep is ~7 µA, yet the board
reads 2.9 mA. That residual is the always-on board hardware: the FGH100M/MM6108 module regulator +
whatever remains after `mmwlan_shutdown()` (it *resets* the chip, does not hard power-gate it), the
XIAO 5 V→3.3 V regulator quiescent draw, and the power LED. So **deep sleep (2.9 mA) barely beats
WNM+powerdown+light-sleep (3.7 mA)** — and deep sleep loses the link + pays ~8 s reconnect per wake.

## The board2 power-save ladder

| State | Current | Power | Associated? |
|---|---:|---:|:--:|
| No power save | 64 mA | 321 mW | ✓ |
| Radio PS (dynamic / TWT / WNM), host awake | 6–13 mA | 31–65 mW | ✓ |
| WNM sleep + chip-powerdown + host light-sleep | 3.7 mA | 19 mW | ✓ |
| Deep sleep (radio shut down + CPU off) | 2.9 mA | 14 mW | ✗ (~8 s reconnect) |
| below ~3 mA | — | — | **needs hardware** |

**Deploy implication:** the floor is duty-cycle-driven. Below ~3 mA requires **hardware** (power-gate
the FGH100M, drop the power LED, more efficient regulator) — the firmware/silicon core is already at
µA. Because deep-sleep barely beats staying associated with the radio off, **rare wakes → deep-sleep
RTC leaf; frequent / latency-sensitive → WNM+powerdown stays connected at nearly identical power**
with no reconnect tax.

## RAW (Restricted Access Window) — not power-measured (single-STA limit)

`hostapd_s1g` supports RAW (`raw_enabled`, `raw_conf`, `praw_period`, `praw_start_offset` in the
binary; the AP logs `RAW Settings:` on boot). STA side is just `mmwlan_sta_args.raw_sta_priority`
(0–7, −1 = off); the AP owns the schedule. RAW is an **AP-scheduled multi-STA contention** feature —
with a single board2 STA its power benefit ≈ dynamic PS (nothing to contend with). Measuring its real
win (per-group slots cutting collisions + letting STAs sleep outside their slot) needs **multiple
STAs** (board0/board1 as extra STAs). Deferred.

## Gotchas (for next time)

- **Sleep firmware flaps USB.** Light-sleep (CPU+USB power-down each DTIM) and deep-sleep (USB off for
  the whole sleep window) make board2's `/dev/ttyACM4` appear/disappear, so esptool can't grab it and
  the serial log garbles. **Fix:** power-cycle board2 via the PPK2 DUT rail and flash with **direct
  esptool** (skip `make`'s cmake latency) in the ~8 s fresh-boot window before the fw sleeps. Helpers
  in `pwr_test/` (`reflash_and_flash.py`, `reflash_hello.py`, `killall_pwr.py`). Rely on the PPK2 (not
  the serial) for the numbers; the deterministic phase schedule anchors the slicing.
- **`esp_pm` overhead is real.** Enabling PM without an achievable sleep raises idle current — light
  sleep only wins when the sleep interval amortizes the ~ms relock.
- **`iw info` misreports txpower**, and other bench notes: see the mesh worklogs.

## State at end

All test firmware reverted (`rimba-halow-sta` back to its TWT action-frame test; app `sdkconfig.defaults`
removed). board2 = rimba-hello + powered OFF; board0/board1 = rimba-hello; all 4 Linux nodes `wlan1`
DOWN, chronite hostapd stopped. Repo clean. Measurement scripts live outside the repo in
`~/pwr_test/` (`ppk2_mon_1s.py`, `analyze.py`, `analyze_pm.py`, the reflash helpers).
