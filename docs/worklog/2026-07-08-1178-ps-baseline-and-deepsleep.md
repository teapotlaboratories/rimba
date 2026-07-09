# 2026-07-08 — 1.17.9 regresses STA power-save; matched-1.17.8 baseline + deep-sleep leaf

The headline: **HaLow 1.17.9 roughly doubles the ESP32-STA power-save doze current vs 1.17.8**, on the same
board / rig / AP / TX-cap. This is the "elevated PS numbers" that kicked off the investigation. The whole
bench was reverted to matched **1.17.8** (the good baseline), where the full PS-feature ladder vs the Linux
AP was measured, plus the deep-sleep duty-cycle economics.

(A brief earlier 2026-07-08 pass mis-concluded "TWT is a mislabel / doze tiers cluster at a ~15 mA host floor
/ APs are peers" — RETRACTED; that was the 1.17.9-regressed bench mistaken for a fundamental floor. See the
retraction atop `2026-07-08-userspace-1179-and-ps-retest.md`.)

## The regression (same rig, only the version differs)

board2 (XIAO ESP32-S3 + FGH100M) STA, TX-capped 1 dBm, on the PPK2 (5 V, 1 s avg), C6-triggered ladder,
against the Linux AP (chronite `hostapd_s1g`). Host-awake:

| STA doze mode | **1.17.8** | 1.17.9 |
|---|---|---|
| Dynamic PS | **9.1 mA** | 20.2 |
| WNM+powerdown | **5.1 mA** | 17.5 |
| Dynamic PS + host light sleep | **8.2 mA** | 30.8 (backfire) |
| TWT + host light sleep | **9.6 mA** | 30.8 (backfire) |

At 1.17.8 the board reaches 4–9 mA host-awake (radio off → DFS + tickless idle drop the host deep) and host
light-sleep does NOT backfire. At 1.17.9 the doze current ~doubles and light-sleep blows up to ~31 mA. So the
older doc's 1.17.8 figures (Dyn 14.5, WNM 4.0, ESP-AP TWT 6.8) are physically fine — not mislabels.

## Reverting the bench to matched 1.17.8

All 1.17.8 artifacts were already on the nodes (`/root/rollback_1178/` = morse.ko + dot11ah.ko + mm6108.bin
480664, per-arch; `/root/us_1178_rollback/` = userspace). Per node: restore userspace → `/usr/local/bin`,
swap `/lib/firmware/morse/mm6108.bin` → 480664, copy the 1.17.8 `.ko`s into the module path, `depmod`,
`rmmod morse dot11ah` + `modprobe morse` (wlan1 down so rmmod is clean; the 1.17.8 Pi-5 driver already has
the gpiod reset patch, so the chip re-probes fine). 1.17.9 backed up to `/root/rollback_1179/` first.
Resulting drivers: Pi 5 srcver `BF1E2755`, Pi Zero `405F9B14` (both 1.17.8). ESP side: the ESP
`vendor/morse-firmware/firmware/mm6108.bin` is **byte-identical** to the Linux one (md5 `616ae6b6` @ 1.17.9),
so swapping it to the 1.17.8 bin (`cfe56db2`, 480664) reverts the ESP build too; rimba-hello doesn't load the
chip fw so no reflash was needed (any HaLow app built now bundles 1.17.8).

## Full matched-1.17.8 ladder — ESP32 STA vs Linux AP (2 ladders each)

| Mode | Host-awake | + host light sleep | Battery (2000 mAh) |
|---|---|---|---|
| No-PS | 64.8 mA | 64.0 mA | ~31 h |
| Dynamic PS | 9.1 mA | 8.2 mA | ~9 days |
| TWT (10 s) | 9.9 mA | 9.6 mA | ~9 days |
| WNM + powerdown | 5.1 mA | **4.0 mA** | ~18–21 days |
| Deep sleep (RESET_N-low + ESP32 deep sleep) | — | **0.35 mA** | ~238 days |

TWT ≈ Dyn-PS because the Linux `hostapd_s1g` doesn't engage a mid-session TWT responder (STA falls back to
dynamic PS). WNM+powerdown is the deepest tier that **stays associated**. Deep-sleep floor 0.35 mA (radio held
in RESET_N-low, host in deep sleep) — even cleaner than the earlier ~0.6 mA.

## Deep-sleep as a duty-cycled leaf (`firmware/rimba-deepsleep-cycle`)

New app: connect (SAE) → deep-sleep (MM6108 RESET_N-low, ESP32-S3 deep sleep, **0.37 mA**) → wake on the C6
trigger (D5 pulled LOW; ext1 ALL_LOW + 60 s backup timer) → cold boot → **re-associate to the Linux AP** →
sleep again. `esp_sleep_get_wakeup_cause()` distinguishes a deep-sleep wake (skip the flash-hold guard,
reconnect) from a fresh power-on (guard + 8 s flash window).

Measured, 5/5 clean cycles: wake cause = EXT1 (C6 trigger fires the wake), **reconnect latency ~5.1 s**
(cold boot + chip-fw reload + SAE; 5149/5150/5109/5133/5109 ms). PPK2 per cycle: ~90 mA wake spike → ~65 mA
during the ~11 s re-association → 0.37 mA asleep. **Reconnect tax ≈ 11 s @ ~63 mA ≈ 690 mA·s per wake.**

Average current ≈ `0.37 + 690/T` mA for wake interval T:

| Wake interval | Avg current | vs WNM+powerdown (5.1 mA, stays associated) |
|---|---|---|
| 30 s (measured) | 23.4 mA | much worse |
| 1 min | ~11.9 mA | worse |
| ~2.5 min | ~5.1 mA | break-even |
| 5 min | ~2.7 mA | better |
| 10 min | ~1.5 mA | better |
| 1 h | ~0.56 mA | far better |

**Takeaway — the two low-power leaves split by wake cadence.** Deep-sleep (loses the link, ~5 s reconnect
tax, 0.37 mA floor) wins only for wakes **rarer than ~2.5 min**; for anything more frequent, WNM+powerdown
stays associated at ~4–5 mA with **no reconnect tax** and wins. Pick the leaf by how often the node must
wake, not by the sleep-floor number alone.

## Radio-doze (stay-associated) — "deep-sleep without powering off the radio" (`firmware/rimba-doze-hold`)

The opposite tradeoff to deep-sleep: keep the STA ASSOCIATED and let the *radio* doze, so there is **no
re-association tax**. Constraint: staying associated needs morselib's link state in RAM, so the ESP32 host
can only **light-sleep** (deep sleep would lose it) → floor ~4 mA, not 0.37 mA. Held each doze mode 45 s
against the Linux AP, host-awake:

| Radio-doze mode (STA stays associated) | Current | Notes |
|---|---|---|
| TWT | 13.4 mA | **needs PS enabled first** — `mmwlan_twt_setup_request` alone leaves the radio fully on (No-PS ~64 mA); and the Linux AP doesn't engage a TWT schedule → falls back to dyn-PS, still associated |
| WNM+powerdown | **3.9 mA** | chip powered down, radio deeply dozed, **STILL ASSOCIATED** — the deepest keep-the-link leaf |

**AP-confirmed:** board2 logged **0 disassoc events** across both holds, and chronite's hostapd log shows it
associated once (aid 1) and held (connected time 210 s+, no re-assoc). NB: `iw dev wlan1 station dump` on the
morse driver does **not** list a deeply-dozing STA at that instant (the live poll read 0) — the hostapd log
is authoritative.

**The two low-power strategies side by side:**
- **Radio-doze (WNM+powerdown, ~4 mA):** stays associated, instant resume, no reconnect tax — best when the
  node must stay reachable or wakes often.
- **Deep-sleep (0.37 mA + ~5 s reconnect):** ~10× lower floor but loses the link — best only for wakes rarer
  than ~2.5 min.

**TWT is not the low-power lever on the Linux AP** (no mid-session responder → dyn-PS ~13 mA); WNM+powerdown
is. TWT's deep-doze win would need an AP that installs the schedule (the ESP32 AP, unverified at 1.17.8).

## TWT engages on the Linux AP — assoc-embedded, not mid-session (`firmware/rimba-twt-assoc`)

The "Linux doesn't engage TWT" story was a **STA-API-path** artifact, not an AP gap. hostapd's
`he_twt_responder` (default-on) advertises TWT in the beacon and negotiates it in the **association**
exchange, but doesn't answer a **mid-session** TWT Setup *action frame*:

- `mmwlan_twt_setup_request()` — a mid-session action, used in **every earlier test** — hostapd ignores it →
  STA stays at dyn-PS (~9.9 mA). This is what looked like "TWT doesn't work on Linux."
- `mmwlan_twt_add_configuration()` **before connect** (TWT in the assoc IEs) → hostapd installs the schedule →
  **ENGAGES**: **6.0 mA host-awake**, clean **10 s SP cadence** (~4 mA baseline + ~16 mA SP spikes every
  10 s), **5.8 mA** + host light sleep, 0 disassoc. This is the path the morselib `twt_setup` example uses.

So **both APs do TWT at ~6 mA**; the old "ESP-AP TWT 2× better" was "ESP answers the mid-session action,
Linux needs the assoc-embedded path." **TWT is NOT the deepest tier** — it keeps the chip powered to wake at
each SP (6.0/5.8 mA); **WNM+powerdown powers the chip *down* (4.0 mA)** and wins on pure power. TWT's value
is the *scheduled, predictable* wake (the AP buffers downlink and flushes it at the SP).

## ESP32 AP ladder + §3b downlink (1.17.8)

**ESP32 AP (board0) idle ladder** (host-awake / + light-sleep), mA: No-PS 64.6/64.6, Dyn-PS 15.3/10.7,
**TWT 7.6/7.2** (mid-session — the ESP AP answers the mid-session action; clean 10 s SP cadence, matching the
doc's original ~6.8 mA), WNM+pd 4.2/4.2. Both APs track closely except the ESP AP's Dyn-PS runs higher
(15.3 vs Linux 9.1). **WNM+powerdown (~4 mA) is the deepest associated tier on both**; TWT (~6–8 mA) stays
higher (chip stays powered to wake at the SP).

**§3b downlink-while-dozing (Linux AP):** board2 at static IP .51, Dyn-PS, `ping -i 1` from chronite →
**25/25, 0% loss**, RTT 23–166 ms (avg 80 = DTIM buffering), board2 **23.6 mA** under the 1 Hz load (vs ~9 mA
idle) — matches the doc's ~25 mA §3b row. (`iw station dump` reads 0 for a dozing STA = display artifact; the
successful ping is the proof. ESP-AP downlink not re-run — its pinger is `#if RIMBA_AP_PING_STAS`, default 0.)

New fw this pass: `rimba-twt-assoc`, `rimba-standby-test`, `rimba-downlink-test` (+ earlier
`rimba-deepsleep-cycle`, `rimba-doze-hold`).

## Q2 "deep-sleep but keep the link" — what morselib actually offers

- **`MMWLAN_STANDBY`** (`morselib/src/umac/offload/`) — the Morse chip keeps the association alive (normal
  DTIM PS + ARP/DHCP/keep-alive offload) while the ESP32 host sleeps, waking it via GPIO on a
  wake-packet/deauth/trigger. **TESTED (`firmware/rimba-standby-test`):** the symbols ARE compiled into the
  ESP build; `mmwlan_standby_enter`/`_exit` return 0; board2 **stayed associated** through a 45 s standby hold
  (hostapd log: no disassoc; 0 disassoc/no auto-exit STA-side). **But the power is ~11.3 mA** (host
  light-sleep) — *worse* than dyn-PS+LS (8.2) and far worse than **WNM+powerdown+LS (4.0)**. Reason: at
  `dtim_period=1` the chip in standby still wakes every DTIM (~100 ms) and TXes periodic keep-alives (spikes
  to ~26 mA), i.e. it keeps the chip **active**, whereas WNM+powerdown powers the chip **down**. So even with
  the host fully deep-asleep, standby would stay chip-bound (~8–11 mA) and lose to WNM+powerdown. STANDBY's
  value is functional (offload housekeeping so the host wakes only on real traffic), **not** a raw-power win —
  and it's **deprecated**. Verdict: not the low-power lever; WNM+powerdown (4 mA, stays associated) remains
  best for "low power while connected." (A higher AP DTIM would lower standby's chip draw, but WNM+powerdown
  still wins.)
- **No STA-side PMKSA caching** in morselib (the PMK/PMKID code is all mesh-SAE), so there's no easy
  fast-reconnect shortcut to shave the ~5 s deep-sleep re-assoc tax.

## Open

- The **ESP32 AP** was not re-measured at 1.17.8 (its TWT path — the mid-session action it answers — is
  understood; the numbers just weren't retaken at the good baseline).
- The 1.17.9 regression mechanism (why doze current doubles + light-sleep backfires) is characterised but not
  root-caused in the driver/fw.
