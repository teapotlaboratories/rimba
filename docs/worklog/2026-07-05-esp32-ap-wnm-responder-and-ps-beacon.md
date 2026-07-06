# Worklog — 2026-07-05 — ESP32 AP WNM-sleep responder + STA power-save beacon fix (HaLow AP/STA)

**Author:** Aldwin (with Claude Code)
**Goal:** (1) make the morselib ESP32 SoftAP a working **WNM-sleep responder** so an all-ESP32 STA
can enter/exit WNM sleep (previously the STA was trapped, blocked forever); (2) make the STA actually
**doze / power-save** against the ESP32 AP.
**Status:** (1) **DONE** — responder works end-to-end, on-air verified (`WNM ENTER/EXIT RETURNED 0`,
CCMP-encrypted response, board2 ACKing). (2) **DONE** — the doze was a STA-app config issue (PS disabled
by `mmhalow.c:200`), NOT an AP gap. With PS re-enabled, board2 dozes to **~13 mA (dynamic PS) / ~4–5 mA
(WNM+powerdown)** against the ESP32 AP — the all-ESP32 low-power leaf works. Bonus: fixed a real beacon
bug along the way (`short_beacon_int`, EID 213 `beacon_interval` 0→100).
(3) **TWT verified** (Part 3) — board2 negotiates a TWT agreement with the ESP32 AP and dozes on its
10 s schedule (~6 mA, waking every ~10 s), on-air ACCEPT + PPK2 cadence confirmed. No AP code change.

Self-contained. Hardware: **board0** = ESP32-S3 + FGH100M SoftAP (app `rimba-halow-ap`), **board2** =
ESP32-S3 + FGH100M STA (app `rimba-halow-sta`), morselib fw 1.17.6, `BOARD=proto1-fgh100m`. board2 is
PPK2-powered (current measured). **chronite** = Linux hostapd_s1g AP (reference). **chronium** = HaLow
monitor (morse0, S1G ch27, freq 5560) — the on-air ground truth. Method throughout: byte-diff on-air
vs a live Linux AP (see `docs/reference/rimba-linux-halow-monitor.md`).

---

## Part 1 — WNM-sleep responder: a 5-part fix chain

**Symptom:** board2 sends a WNM-Sleep ENTER request; `mmwlan_set_wnm_sleep_enabled(true)` blocks
forever, retrying every 1 s (60×) then `MMWLAN_TIMED_OUT`; `Transmit blocked: 6` recurs. Against a
Linux AP the SAME board2 entered/exited WNM sleep cleanly (6.7 mA). So the STA is capable; the ESP32
AP's responder was the gap. Root-caused by two adversarial-verification workflows; each fix follows the
hostapd/Linux code faithfully.

| # | Fix | File | Root cause / evidence |
|---|---|---|---|
| 1 | Compile the dormant responder: add `src/ap/wnm_ap.c` to SRCS + `CONFIG_WNM_AP=1` | `components/halow/components/hostap/CMakeLists.txt` (AP SRCS + BUILD_DEFINES blocks) | `wnm_ap.c` (`ieee802_11_rx_wnmsleep_req`) was never compiled; the `case WLAN_ACTION_WNM` dispatch (`ieee802_11.c:6194`) is `#ifdef CONFIG_WNM_AP`. Linux `hostapd_s1g` builds both. |
| 2 | `bss->wnm_sleep_mode = 1` | `src/hostap/wpa_supplicant/ap.c` `wpa_supplicant_conf_ap` (after `dtim_period`) | `wnm_ap.c:292` early-returns if `!conf->wnm_sleep_mode`. Linux sets it via `wnm_sleep_mode=1` in `.conf`; wpa_supplicant AP mode has no knob. On-air: `rx_wnmsleep_req mode=1`. |
| 3 | Add an AP `send_action` op `mmwpas_send_action_ap` (wired into `mmwlan_wpas_ops_ap`) | `src/umac/supplicant_shim/driver_ap.c` | `hapd_drv_send_action()` (`ap_drv_ops.c`) does `if (!driver->send_action) return 0;` — **returns success without sending** when the driver has no `.send_action`. The AP ops had none, so the response was silently dropped. Builds the PV0 mgmt header (`dot11_build_pv0_mgmt_header`, `DOT11_FC_SUBTYPE_ACTION`) around hostapd's action body and TXs via `umac_datapath_tx_mgmt_frame_ap`. |
| 4 | CCMP-protect the robust unicast response | `src/umac/datapath/umac_datapath_ap.c` `umac_datapath_tx_mgmt_frame_ap` | Response went out **unprotected**; board2 (SAE+PMF) deterministically drops unprotected robust mgmt (`umac_datapath.c:390`). Deterministic 100% failure across 60 retries ruled out PS-timing. Mirror the proven STA protect block (`umac_datapath.c:2450`): look up the DEST stad by `header->addr1`, and if `pmf_is_required && frame_is_robust_mgmt && !multicast`: set `DOT11_MASK_FC_PROTECTED`, `key_id = umac_keys_get_active_key_id(dst_stad, PAIRWISE)`, `umac_keys_increment_tx_seq`, then `HW_ENC`. Safe (AP↔STA unicast A2=TA=BSSID; the mesh #20 A4≠TA gate does not apply). |
| 5 | Reserve CCMP header headroom + MIC tailroom in the response buffer | `src/umac/supplicant_shim/driver_ap.c` `mmwpas_send_action_ap` alloc | Alloc was `MMDRV_PKT_CLASS_DATA_TID7, 0, hdr+body` (zero headroom) but flagged `HW_ENC` → the FW could not prepend the 8-byte CCMP header / append the MIC → **malformed on-air** → board2 dropped it **pre-ACK**. Fix mirrors the STA mgmt path (`frame_constructor.c:21`): `MMDRV_PKT_CLASS_MGMT, DOT11_CCMP_HEADER_LEN, hdr+body+DOT11_CCMP_256_MIC_LEN`. |

**The decisive on-air discriminators (chronium monitor):**
- Before #4: response `PROT=0` → board2 drops it (PMF).
- After #4, before #5: response `PROT=1` but **constant PN=0x91, retry=1** ×11 = board2 dropping it
  **pre-ACK** (a decrypt-asymmetry would ACK first, then advance the PN) → the malformed-buffer clue.
- After #5: **PN 0x49→0x52 (incrementing), retry=0** = board2 ACKing fresh responses → and on the STA:
  **`WNM ENTER #0/EXIT #0/ENTER #1 RETURNED 0`**, looping. Responder works.

## Part 2 — The doze (STA power-save) — SOLVED (it was the STA, not the AP)

**Symptom:** with the WNM handshake working, board2's radio still sat at steady **~66 mA** through both
the dynamic-PS and WNM windows (85/85 samples radio-on, PPK2). It looked like an AP-side gap because the
same board2 dozed to 6.7/13 mA against a Linux AP.

**RESOLUTION — the AP was fine; the STA test app had power-save disabled.** `mmhalow_init()` explicitly
calls **`mmwlan_set_power_save_mode(MMWLAN_PS_DISABLED)` (`components/halow/mmhalow.c:200`)**, so the
radio stays on at ~64 mA regardless of the AP (this is exactly the "No PS = 64.2 mA" row in the
2026-07-04 PPK2 worklog). The WNM test never re-enabled PS, and it used the **basic**
`mmwlan_set_wnm_sleep_enabled(true)` which negotiates the protocol but leaves `chip_powerdown_enabled=0`,
so the radio never powers down. **The fix is entirely STA-app-side** (no morselib/AP change):
```c
mmwlan_set_power_save_mode(MMWLAN_PS_ENABLED);                       /* undo mmhalow.c:200 */
struct mmwlan_set_wnm_sleep_enabled_args a = {.wnm_sleep_enabled=1, .chip_powerdown_enabled=1};
mmwlan_set_wnm_sleep_enabled_ext(&a);                               /* WNM + radio powerdown */
```
**Measured after (PPK2, board2 against the board0 ESP32 AP):** dynamic-PS baseline **~9–13 mA**, WNM
sleep **~4–5 mA** (min 4 mA) — matching/beating the Linux-AP ladder (13 / 6.7 mA). **The all-ESP32
low-power leaf works.**

**Red-herring trail (documented so it isn't re-run):** while assuming an AP gap, three AP-beacon
candidates were fixed/tested on-air and **all RULED OUT by re-measuring** — none moved the 66 mA because
PS was off the whole time:
- **S1G Beacon Compatibility `beacon_interval` (EID 213) = 0** (Linux = 100). This IS a real beacon bug:
  `hostapd_eid_s1g_beacon_compat` (`ieee802_11_s1g.c:118`) computes `short_beacon_int * beacon_int` and
  `short_beacon_int` has no default (0). Fixed via `bss->short_beacon_int = 1` in `wpa_supplicant_conf_ap`
  — on-air confirmed 0→100. **Kept** as a legitimate correctness fix (matches Linux), though NOT the doze
  cause.
- **S1G Capabilities (EID 217):** overrode `hw_mode->s1g_capab` (driver_ap.c) to the exact Linux bytes
  `9e0040f8800c00024000`, confirmed on-air — no change. Reverted.
- **AP app pings the STA at 1 Hz forever** (`start_ping_octet`, `ESP_PING_COUNT_INFINITE`); disabling it
  did not move the 66 mA. Left enabled (it's a downlink/TWT test aid) but noted it keeps a PS STA busier.
- The "Linux AP sends short beacons, ESP sends full" delta was also a red herring: the FC `0x08`/`0x40`
  is BSS-BW vs the Security bit, **not** the short-beacon (Next-TBTT) indicator, and the dozing Linux AP
  sends non-short beacons too. **Short beacons are NOT required to doze.** (A workflow risk-analysis
  caught this before any short-beacon code was written.)

## Part 3 — TWT (Target Wake Time) verified end-to-end on the ESP32 AP

The natural completion of the power-save story: WNM is a whole-session sleep; **TWT** is the modern S1G
*scheduled* wake mechanism. An understand-workflow (4 agents) first confirmed the AP TWT **responder** is
fully compiled + dispatched with **no WNM-class gaps** — TWT setup/teardown are **unprotected S1G Action
frames (category 22)** sent via the umac datapath directly, so they never needed the WNM `send_action` or
CCMP-protect fixes (`frame_is_robust_action` returns false for cat 22, so the CCMP blocks correctly skip).
**No AP code change was required** — the AP vif auto-arms as TWT responder (`umac_interface.c:147`).

**STA test app** (`rimba-halow-sta`, netif-free): connect → `mmwlan_set_power_save_mode(MMWLAN_PS_ENABLED)`
→ `mmwlan_twt_config_args{REQUESTER, wake_interval=10 s, min_wake=65280 us, SETUP_REQUEST}` →
`mmwlan_twt_setup_request()` → hold idle → `mmwlan_twt_teardown()`.

**Verified (on-air chronium morse0 + PPK2 board2):**
- On-air: **AP → STA** `category 22, action 6, setup_command = ACCEPT(4), PROT=0` (unprotected, no PN —
  spec-correct for an S1G Unprotected action, and distinct from the WNM response which carries a PN);
  STA → AP `action 7` teardown. The responder answers a mid-session TWT Setup with ACCEPT.
- PPK2: in the TWT window board2 sits at **~6 mA floor with a ~20 mA wake spike every ~10 s** — the
  **wake cadence matches the negotiated 10 s service period**. That cadence (not the average) is the proof:
  it resolves the two pre-identified risks — **R1** (FW cmd 0x26 `TWT_AGREEMENT_INSTALL` *did* arm the
  schedule on the STA vif; a failed install would wake every ~102 ms DTIM) and **R4** (TWT is genuinely
  scheduling wakes, not just reading like dynamic PS). STA UART: `TWT SETUP RETURNED 0`.
- **R3 CONFIRMED — downlink to a dozing STA works (AP host-buffer + flush-on-wake).** Gave board2 a
  static IP (192.168.12.51) so the AP's 1 Hz pinger reaches it, and measured both PS modes at once
  (all three signals agree): **Dynamic PS** — every ping succeeds (RTT ~40–130 ms); the AP buffers +
  delivers at DTIM while the STA dozes. **TWT (5 s SP)** — the pings are buffered and flushed as a
  ~5-frame BURST at each 5 s service period (on-air: AP→STA + STA→AP bursts at sec 37/42/47/52/57…;
  PPK2: ~4 mA floor with a ~24 mA spike every 5 s; AP ping-success cadence flips 1→5, i.e. 1 ping/SP
  lands inside the 1 s ping timeout, the rest are delivered late-but-correctly at the SP). So downlink
  to a *sleeping* leaf is buffered while it dozes and delivered at its next wake — not dropped.
- **ESP AP vs LINUX AP (chronite hostapd_s1g), same R3 test:** (i) **core R3 EQUIVALENT** — the Linux AP
  also delivers downlink at DTIM to a dynamic-PS STA (continuous 1/s, RTT ~46–126 ms, board2 ~25 mA);
  (ii) but for **mid-session TWT** (what `mmwlan_twt_setup_request` uses) the **ESP AP is the better
  responder** — it answers the TWT Setup Request with ACCEPT and gives a ~6 mA SP-only doze with buffered
  burst delivery, whereas chronite sent **no TWT response** on-air (with any config this hostapd_s1g
  accepts — `enable_twt` is rejected, `ht_vht_twt_responder=1` didn't engage the mid-session responder),
  so board2 stayed in dynamic PS at ~25 mA (0/123 s below 12 mA). Caveat: only the mid-session TWT path
  was tested; chronite may support **assoc-embedded** TWT (a different path, likely the 2026-07-04 ~13 mA
  run). So the ESP AP leads specifically on the mid-session path + doze depth (SP-only ~6 mA vs the Linux
  AP's DTIM-cadence ~13 mA).

## Code-map (new/changed code ↔ anchor)

All in `components/halow/components/mm-iot-sdk/framework/` unless noted; verify by grepping both trees.

| File:sym | Change | Linux/hostapd anchor |
|---|---|---|
| `components/halow/components/hostap/CMakeLists.txt` (SRCS + BUILD_DEFINES, `CONFIG_HALOW_AP_MODE` blocks) | +`src/ap/wnm_ap.c`, +`CONFIG_WNM_AP=1` | same bundled `wnm_ap.c`; Linux `hostapd_s1g` compiles it |
| `src/hostap/wpa_supplicant/ap.c` `wpa_supplicant_conf_ap` | `bss->wnm_sleep_mode = 1`; `if (!bss->short_beacon_int) bss->short_beacon_int = 1` | Linux `.conf`: `wnm_sleep_mode=1`, `short_beacon_int` |
| `src/umac/supplicant_shim/driver_ap.c` `mmwpas_send_action_ap` + `.send_action` in `mmwlan_wpas_ops_ap` | new AP action-frame TX (PV0 hdr + CCMP headroom/MIC tailroom) via `umac_datapath_tx_mgmt_frame_ap` | STA `mmwpas_send_action` (`driver.c`) + `frame_constructor.c:21` alloc |
| `src/umac/datapath/umac_datapath_ap.c` `umac_datapath_tx_mgmt_frame_ap` | robust-unicast-PMF CCMP-protect block (dest-stad key) | STA-side protect block `umac_datapath.c:2450` |
| `src/hostap/src/ap/wnm_ap.c` `hostapd_eid_s1g_beacon_compat` (`ieee802_11_s1g.c:118`) | (unchanged — the `short_beacon_int*beacon_int` formula that made the fix necessary) | — |

## State at end
WNM responder fix committed to the working tree (5 files in the halow submodule); debug `printf` probes
stripped. board2 test firmware = a WNM sleep test (`rimba-halow-sta`). Bench left UP for the follow-up:
board0 = fixed AP, board2 = STA, chronite = Linux reference AP (SSID `rimba-linux`), chronium = monitor.
Measurement scripts in `~/pwr_test/` (`ppk2_mon2.py` robust monitor, `reflash_and_flash.py`, the on-air
beacon decoders on chronium `/tmp/*.py`). Both goals met. Any app that wants STA power-save against this
(or any) AP must re-enable it after `mmhalow_init()` — `mmwlan_set_power_save_mode(MMWLAN_PS_ENABLED)` +
`mmwlan_set_wnm_sleep_enabled_ext(chip_powerdown_enabled=1)` — because `mmhalow.c:200` disables it.
