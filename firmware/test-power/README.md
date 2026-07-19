# test-power — power-save ladder DUT (the `tp` tier)

The DUT (reporter) for the **`tp` (power)** regression tier. It runs the canonical 4-tier STA
power-save ladder on **board2** while the **PPK2** meters board2's 5 V rail; the host runner
(`tools/regtest/tp_power.py`) segments the current stream into the tiers and scores a **gross**
current regression (the kind the fw-1.17.9 ~2× PS regression was).

Unlike a T2 reporter, this app emits **no `TEST|RESULT`** — the verdict is a host-side PPK2
measurement the firmware cannot see. It reports the structural facts the host needs on the
`TEST|` console.

## The ladder (18 s per tier, all host-AWAKE)

| Phase | Tier | API | Scored? |
|---|---|---|---|
| 1 | No-PS | `mmwlan_set_power_save_mode(DISABLED)` | validity anchor only (~65 mA) |
| 2 | Dyn-PS | `mmwlan_set_power_save_mode(ENABLED)` | **scored** |
| 3 | TWT (10 s SP) | `mmwlan_twt_setup_request` (mid-session) | recorded, not scored (a Linux AP ignores it) |
| 4 | WNM + chip-powerdown | `mmwlan_set_wnm_sleep_enabled_ext({chip_powerdown})` | **scored** (deepest stay-associated) |

## The rig (keep the C6 in the loop)

- **Trigger:** the ESP32-C6 (`firmware/c6-harness`, GPIO20 → board2 D5/GPIO6) pulses D5 LOW then
  releases it HIGH every 30 s → one ladder run. The DUT waits in `wait_for_trigger()`.
- **Phase marker:** phase N = N × 60 ms pulses on D5 at entry (a PPK2 logic input can timestamp
  these; the primary segmentation is the `TEST|INFO|phase=N ... start` console line).
- **Flash-hold guard:** D5 HIGH at boot → idle host-awake forever (always reflashable; the
  deep-tier USB-wedge escape hatch). Default pull-down → normal run.
- **TX cap:** `mmwlan_override_max_tx_power(1)` — **mandatory** on the close bench (uncapped TX
  saturates the AP RX → retries keep the radio awake → inflated doze current that *mimics* the
  regression). Remove for a real deployment.
- **AP:** associates to SSID `rimba-ping`/SAE, which both the ESP SoftAP (`test-apsta-ap`) and
  the Linux `hostapd_s1g` AP (chronite `hostapd-rimba.conf`) advertise — one DUT, both AP paths.

## Console contract

```
TEST|BEGIN|name=power|rig=...
TEST|STEP|associated|PASS|connected=1 after 4250 ms ssid=rimba-ping tx_cap=1dBm
TEST|INFO|phase=1 no-ps start uptime=...          <- host segmentation timestamps
TEST|INFO|phase=2 dyn-ps start uptime=...
TEST|INFO|phase=3 twt start uptime=... (setup ret=0)
TEST|STEP|twt-installed|PASS|flow=0 installed=1 (AP-dependent)
TEST|INFO|phase=4 wnm-powerdown start uptime=... (enter ret=0)
TEST|STEP|wnm-accepted|PASS|enter ret=0 chip_powerdown=1
TEST|INFO|phase=5 ladder-done uptime=...
```

The host reduces each tier's PPK2 window (median of the plateau, ~1 s settle discarded) to a
median mA, checks No-PS is in the validity window (associated + PS-on + not RX-overloaded), and
scores Dyn-PS + WNM against the calibrated bands. A low mA with a failed `associated` / `wnm-accepted`
STEP → **INCONCLUSIVE**, never PASS.

## Run it

```sh
make test-tp AP=esp            # ESP SoftAP path (board0)  -- AP is REQUIRED
make test-tp AP=linux          # Linux hostapd_s1g path (chronite)
make test-tp AP=esp DRY_RUN=1  # rig + tiers, no hardware
```

Needs board2 on the PPK2 rail (the runner owns the PPK2 and power-cycles it), the C6 powered +
flashed + wired to D5, and an AP (board0 or chronite). Absent PPK2/board2/trigger → SKIP.
