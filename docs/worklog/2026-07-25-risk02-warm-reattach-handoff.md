# RISK-02 (IBSS cold-boot → joined time) + warm-reattach spike — 2026-07-25

Re-measured RISK-02 on the current bench (board2 + PPK2 + fw 1.17.8 / morselib 2.12.3), then
attempted the "keep the MM6108 warm across sleep, skip the fw reload" optimization (Test C). Test A
landed clean numbers; the warm-reattach is implemented and its control path works end-to-end, but it
hits a transport-layer crash — full detail below so this is resumable without re-deriving anything.

## Rig

- **board2** (efuse `E0:72:A1:F8:F0:08`) = the DUT, on the **PPK2** DUT rail (ampere-meter, 5 V).
  `tools/ppk2_hold.py` keeps it powered + enumerated as `/dev/ttyACM*` (currently ttyACM2).
- **board0** (efuse `E0:72:A1:F8:EF:A4`, chip/IBSS MAC `68:24:99:44:6b:b7`) = the IBSS peer, USB-powered,
  independent of the PPK2. Ran `test-ibss-boottime` CREATE mode → IBSS cell `rimba-ibss` chan 27,
  static IP **192.168.13.183** (the DUT pings this). NOTE: the chip's own MAC (68:24:…), not the efuse.
- Fixture: `firmware/test-ibss-boottime` (dual-mode; CREATE=peer/board0, MEASURE=DUT/board2). Times each
  cold-boot phase with `esp_timer` and prints one `BOOTTIME_MS …` line at the first ICMP reply.
- Both boards were flashed back to `rimba-hello` at the end (radio-silent).

## Test A — cold-boot → IBSS-joined → first-frame (DONE, solid)

Sweep tool: `<scratchpad>/risk02_sweep.py N` (reuses `tools/regtest/tp_power.Ppk2Sampler` — take-over,
power-cycle board2 per cycle, stream current, capture the `BOOTTIME_MS` line, reduce to phase +
energy stats). N=8+4 cycles, all good. **Firmware esp_timer phases (the RISK-02 answer):**

| phase | median | notes |
|---|---|---|
| app (IDF startup → app_main) | 13 ms | fixed |
| **fw** (`mmhalow_init` = MM6108 .mbin load over SPI) | **~650 ms** | the reload |
| **ibss** (`mmwlan_ibss_start` = add vif + join + beacon) | **~800 ms** | biggest chunk |
| frame (join → first ICMP reply) | ~55 ms | 600 ms on the 1st post-takeover boot (ARP settle) |
| **total (CPU-active)** | **~1.52 s** (1.49–2.11) | sd ~19 ms |

Plus PPK2 (host): **~367 mJ (≈20 µAh @ 5 V) per wake-to-joined**, boot mean ~40 mA, **steady joined idle
~64 mA** (no power-save IBSS). Add ~0.2 s rail/ROM (power-on edge → esp_timer start) for the true
**power-on → joined ≈ 1.7 s**. Two levers = **fw-reload (650) + ibss-join (800) = 94 % of boot.**

Gotcha that cost time: a naïve host-clock energy window read ~34 s / 12 J because board2's USB
re-enumerates ~28 s after a *cold* PPK2 power-cycle (first-serial-open backlog). Fix = anchor the
energy window at the physical current-rise with the firmware's own `total` as the length (already in
`risk02_sweep.py`). Enum after a cold cycle is otherwise ~0.8 s once the by-id symlink appears.

## The finding that reshaped Test B/C

`morse_firmware_init` (driver/morse_driver/firmware.c:293) **unconditionally** re-downloads fw+BCF every
boot: `invalidate_host_ptr → load_mbin(fw) → load_mbin(bcf) → trigger → magic_verify → check_compat`;
`morse_firmware_reset` (RESET_N) only on a failed-download retry. There is **no warm/already-running
check**. The chip advertises `MORSE_FW_FLAGS_SUPPORT_HW_REATTACH = BIT(9)` (hw.h:129) but it is only
*printed* (driver.c:166 FWCAPS log) — never acted on. So keeping the MM6108 powered buys nothing by
itself: skipping the 650 ms reload needs a morselib change (Test C), not just a warm chip. Test B (warm
chip, unmodified morselib) was therefore dropped as low-info; went straight to Test C.

Boot chain (cold): `mmdrv_init` (driver.c:339) → `morse_trns_start` (transport/sdio.c:776) →
`mmhal_wlan_init` (RESET_N high) + **`morse_trns_reset`→`mmhal_wlan_hard_reset()`** (sdio.c:689, the
physical RESET_N pulse) + read chip_id → **`morse_firmware_init`** (the 650 ms download). Physical
RESET_N (GPIO1) is driven by the ESP HAL `components/halow/components/shims/mmhal_wlan.c`
(`mmhal_wlan_hard_reset` L162 pulses; `mmhal_wlan_init` L238 raises). `mm610x_digital_reset` (mm6108.c:85)
is a *soft register* reset (writes MORSE_REG_RESET over SPI), not the GPIO.

## Test C — warm reattach: IMPLEMENTED, control path works, CRASHES in transport

### Implementation (submodule `components/halow`, branch `feat/warm-reattach`, UNCOMMITTED)

Gate = a `warm_boot` field on `struct driver_data` (passed to both trns_reset + firmware_init — no new
includes), fed from a one-shot file-static set by a new public `mmwlan_set_warm_boot(bool)`. 6 edits:

1. `.../morse_driver/morse.h` (struct driver_data) — added `bool warm_boot;`.
2. `.../driver/driver.c` — `static bool s_warm_boot_request;` + `void mmdrv_set_warm_boot(bool)`;
   in `mmdrv_init` after the memset: `driver_data.warm_boot = s_warm_boot_request; s_warm_boot_request = false;`.
3. `.../internal/mmdrv.h` — declared `void mmdrv_set_warm_boot(bool)`.
4. `.../driver/transport/sdio.c` (`morse_trns_reset`) — `if (!driverd->warm_boot) mmhal_wlan_hard_reset();`.
5. `.../morse_driver/firmware.c` (`morse_firmware_init`) — if `warm_boot`: `get_host_table_ptr` +
   `magic_verify` + `check_compatibility` (all read the live running fw's host table, no download);
   success → return 0; failure → return err (caller retries cold). Skips invalidate/load/trigger.
6. `.../include/mmwlan.h` + `.../umac/umac.c` — public `mmwlan_set_warm_boot(bool)` → `mmdrv_set_warm_boot`.
   (mmwlan* is globbed in protected_syms.txt, so it auto-exports.)

Builds clean (morselib recompiled + linked; board2 flashed OK).

### Fixture (superproject, UNCOMMITTED): `firmware/test-ibss-boottime` `TEST_WARM_CYCLE` (Makefile `WARM=1`)

MEASURE mode, when `TEST_WARM_CYCLE`: cold boot → join → BOOTTIME(cold) → **`esp_restart()` with RESET_N
(GPIO1) held HIGH via an RTC-GPIO hold** → next boot detects `esp_reset_reason()==ESP_RST_SW` → sets
`mmwlan_set_warm_boot(true)` → warm re-attach. `main/CMakeLists.txt` adds `driver esp_hw_support` +
forwards `TEST_WARM_CYCLE`; root `Makefile` adds `$(if $(WARM),-D TEST_WARM_CYCLE=1)`.

Why esp_restart, not deep sleep: **deep-sleep timer-wake is unachievable on board2 while USB is
attached** — the ESP32-S3 USB-Serial-JTAG console resets the chip (`reset_reason=11 ESP_RST_USB`,
`wakeup_cause=0`) within the sleep window before the timer fires. Verified undisturbed (no host
polling): every post-"DEEP SLEEP" boot was ESP_RST_USB, never ESP_RST_DEEPSLEEP. The **RTC-GPIO hold
survives a SW reset** (RTC domain persists), so esp_restart keeps the MM6108 warm and isolates the
reattach mechanism + its latency saving. (Deep-sleep energy — radio kept warm draws ~64 mA
continuously — is a separate tradeoff: warm-reattach only wins for short/frequent wakes.)

### RESULT — the wall

The control path works: `boot: reset_reason=3 -> WARM re-attach` fires every restart, warm flag set.
But **every warm boot then panics: `Guru Meditation Error: Core 0 panic'ed (Interrupt wdt timeout on
CPU0)`** during `mmhalow_init` (early, before the version print → in `mmwlan_boot` → transport/firmware
init), then reboots cold. So re-establishing the SPI transport against a chip running *application*
firmware (not the bootloader) **deadlocks / storms interrupts**. Cold boots are unaffected (fw ~650 ms,
mode=cold — the change is inert when warm_boot is false).

**Root-cause hypothesis (unverified):** the hard reset normally clears the chip's host-interface state;
on the warm path it's stale — the running MM6108 likely holds its SPI IRQ (GPIO3, level-triggered LOW)
asserted with pending events, so enabling the ISR (`morse_trns_start` starts the spi_irq_task +
`mmhal_wlan_set_spi_irq_enabled`) storms → Interrupt WDT. This is exactly the BIT9 gap: the fw supports
HW reattach but morselib has **no reattach handshake** — skipping reset+download is necessary but not
sufficient; the host-interface protocol state must be re-synced with the running fw.

## State / cleanup

**DECISION (user, 2026-07-25): REVERT the warm-reattach.** Narrow ROI + it needs the BIT9 handshake
(transport surgery), and Test A already answers RISK-02. So:

- **Submodule `components/halow`: reverted to clean `main` (8a70405c = origin/main); branch
  `feat/warm-reattach` DELETED.** The 6 morselib edits are gone from the tree but fully specified in
  the "Implementation" section above — reproducible verbatim if ever resumed.
- **Fixture `test-ibss-boottime`: the `TEST_WARM_CYCLE` additions were stripped**; it's back to the
  cold-boot MEASURE/CREATE Test A tool (compiles clean). The `WARM=1` Makefile flag was removed
  (CREATE/PEER_IP kept). The cold fixture is retained as the RISK-02 measurement tool (untracked).
- Bench radio-silent (board0 + board2 → rimba-hello). `ppk2_hold` running, board2 = ttyACM2.
- `<scratchpad>/risk02_sweep.py` = the Test A PPK2 sweep (keep for re-runs; not in-repo).

## Next steps (resumable)

1. **Decision:** is warm-reattach worth the transport surgery? ROI is narrow — it wins only for
   short/frequent wake cycles (keeping the radio warm costs ~64 mA; a leaf sleeping minutes is better
   off cold-reloading). Test A already answers the RISK-02 number (~1.7 s, ~367 mJ/wake).
2. If pursuing: implement the **HW_REATTACH (BIT9) handshake** — before enabling the SPI IRQ on a warm
   boot, drain/ack the chip's pending host-interface state (read + clear its IRQ/status), or find the
   MorseMicro reattach sequence. Verify with `esp_restart` first (fast, no USB issue), then the real
   deep-sleep case needs USB detached (field scenario) or a GPIO/EXT1 wake with USB disabled pre-sleep.
3. Either way, land Test A's number in the RISK-02 record and decide warm-reattach = backlog vs active.
