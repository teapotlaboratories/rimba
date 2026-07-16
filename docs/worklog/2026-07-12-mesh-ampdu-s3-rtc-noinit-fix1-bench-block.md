# Worklog — 2026-07-12 — Mesh A-MPDU S3: RTC-noinit drop counters + FIX-1 implemented; bench read blocked

**Author:** Aldwin
**Phase:** 802.11s mesh throughput — A-MPDU aggregation (S3 relay forward-leg) — unblocking the RX-drop read
**Goal:** (a) make the forward/RX-drop counters survive the relay crash-reboot so the ~99% early-host-RX drop
can be read after the crash; (b) implement FIX-1 (bus-preserving hw_restart) so board2 stays up under load;
(c) one bench run to name the exact RX drop bucket (allowlist / plaintext / hw-ccmp / sw-ccmp / no-decrypt).
**Status:** **(a) DONE + hardware-validated. (b) IMPLEMENTED + builds green (toggle default OFF, bench-unverified).
(c) BLOCKED** — board2's mesh plink flaps (peers then drops, never stable) so no traffic flows through the relay
to accumulate the counters. The block is a bench/board2-stability issue, proven independent of the instrumentation.
Nothing committed. Bench left radio-silent.

This entry is **standalone**.

---

## 0. TL;DR

- **RTC-noinit counters work.** `g_mesh_fwd_dbg[]` + a magic canary now live in the APP as `RTC_NOINIT_ATTR`
  (morselib refs them `extern`). They survive the INT-WDT panic-reboot (SW reset keeps RTC RAM — confirmed
  `ESP_SYSTEM_PANIC_PRINT_REBOOT=y`, delay 0), zero on a true power-on / magic-mismatch, and dump once at boot
  + every 5 s. On-air proven: `FWD-DBG counters PRESERVED across reset_reason=11` + a clean boot dump.
- **FIX-1 implemented** exactly per the adversarially-verified design (a bus-preserving "soft restart" reusing
  the persistent SPI2_HOST bus across a chip reset + FW reload), 11 edits, **builds green**. Gated behind
  `CONFIG_HALOW_SOFT_HW_RESTART` / runtime `g_mmdrv_soft_hw_restart`, **default OFF** — it is delicate bus code
  that is not yet bench-verified, so the tree behaves exactly as before until the toggle is flipped.
- **The read is blocked** by board2 mesh instability (below), not by any of this session's code.

---

## 1. RTC-noinit forward/RX-drop counters (task a) — DONE + validated

Problem from the prior pass: the `g_mesh_fwd_dbg[FDBG_COUNT]` counters were plain globals, so the INT-WDT
crash-reboot zeroed them before they could be read → the earlier run only ever saw `rx_reached=6` (the ~5 s
since the last boot), never the accumulated total.

Fix (all TEMP instrumentation, uncommitted):
- Moved the counter **storage** out of morselib into the app (`firmware/rimba-halow-mesh-perf/main/app_main.c`)
  as `RTC_NOINIT_ATTR uint32_t g_mesh_fwd_dbg[FDBG_COUNT]` + `RTC_NOINIT_ATTR uint32_t g_mesh_fwd_dbg_magic`.
  morselib (`umac_mesh.c`) keeps only the `extern` + the getter; the ESP-specific RTC attribute stays in the app.
- On boot: `esp_reset_reason() == ESP_RST_POWERON` OR magic-mismatch → `memset` the array + set the magic
  (clean session); else PRESERVE (accumulate across the crash-reboot loop). A PPK2 power-cycle (reflash_hello)
  is a true power-on → clean slate; the INT-WDT panic reboot is a SW reset → RTC RAM preserved.
- Dump `FWD-DBG[boot]` / `RX-DBG[boot]` **once at boot** (before the radio can crash the board) plus the
  periodic 5 s dump, via a shared `dump_fwd_dbg()` helper — so even a fast crash-boot surfaces the surviving totals.

Validated on hardware: a boot showed `FWD-DBG counters PRESERVED across reset_reason=11 (crash-reboot)` and a
clean `FWD-DBG[boot] ... =0 ... RX-DBG[boot] ... =0` line. Builds green (bin fits the SINGLE_APP_LARGE partition).

## 2. FIX-1 (task b) — IMPLEMENTED, builds green, toggle default OFF, bench-unverified

Design produced by a 4-phase agent workflow (3 mappers → synthesis → 3 adversarial red-team lenses → finalize),
grounded in the ESP teardown map + ESP-IDF intr semantics + a Linux morse_driver follow-verify. **FIX-1 = a
bus-preserving soft restart invoked ONLY from `hw_restart_evt_handler`** that reuses the persistent ESP
`SPI2_HOST` bus + `spi_handle` + `bus_lock` + `spi_irq_semb` + BOTH GPIO ISR handlers across a chip reset + FW
reload, dropping exactly the 6 host-teardown calls the INT-WDT root cause blamed
(`spi_bus_free`/`spi_bus_initialize` = `esp_intr_free`/`alloc` → cross-core `esp_ipc` holding the ISR-shared
global intr-alloc spinlock >300 ms). `morse_trns_start/stop` and `mmhal_wlan_init/deinit` are **never touched**,
so genuine first-boot/shutdown are byte-for-byte unchanged.

**11 edits (all in `components/halow`, uncommitted):**
1. `Kconfig` — new `config HALOW_SOFT_HW_RESTART` (**default n**; the design chose y, deviated to n because the
   path is unverified — see §3).
2. `.../transport/morse_transport.h` — declare `morse_trns_soft_start` / `morse_trns_soft_stop`.
3. `.../internal/mmdrv.h` — declare `mmdrv_soft_restart` + `extern bool g_mmdrv_soft_hw_restart`.
4. `.../transport/sdio.c` `morse_trns_reset` — **fix a pre-existing bus_lock leak** on the sdio-startup-fail
   early return (`:699`): it returned with `bus_lock` held. Harmless on the full path (stop deletes the mutex),
   but the soft path PRESERVES `bus_lock`, so without this a transient glitch would wedge the bus. Now releases.
5. `.../transport/sdio.c` `morse_trns_soft_stop` (NEW) — the task-quiesce half of `morse_trns_stop` ONLY
   (mask SPI_IRQ, stop + **join** the spi_irq worker); OMITS the `bus_lock`/`semb` delete + `mmhal_wlan_deinit`.
6. `.../transport/sdio.c` `morse_trns_soft_start` (NEW) — `morse_trns_reset` over the live bus + recreate the
   spi_irq worker + re-arm CCCR IEN/BIC (claim/release-wrapped); **self-unwinds** (joins its own worker) on any
   failure so a soft_start failure can never leave the recreated task derefing a memset'd `driver_data`. Does
   NOT re-register/re-enable SPI_IRQ (persists; re-enabled by `mmdrv_init`) and NOT call `mmhal_wlan_init`.
7. `components/shims/mmhal_wlan.c` — invariant comment on `mmhal_wlan_register_spi_irq_handler`: only
   `gpio_isr_handler_add` sets the per-pin `int_ena`; the soft path must never remove the SPI_IRQ handler or it
   silently kills RX (`int_ena=0`) with no crash.
8. `.../driver/driver.c` — `#include "sdkconfig.h"` + file-scope `bool g_mmdrv_soft_hw_restart` (compile-time
   default from the Kconfig) + `static bool g_mmdrv_bus_persistent`.
9. `.../driver/driver.c` `mmdrv_init`/`mmdrv_deinit` — gate EXACTLY the 3 transport call-sites
   (`morse_trns_start`@init, `morse_trns_stop`@error-unwind, `morse_trns_stop`@deinit) on
   `g_mmdrv_bus_persistent`; everything else byte-unchanged.
10. `.../driver/driver.c` `mmdrv_soft_restart` (NEW) — sets `g_mmdrv_bus_persistent=true`, runs the verbatim
    `mmdrv_deinit()`+`mmdrv_init()` bodies (only the transport primitive swaps), clears it. Carries the
    residual-risk INVARIANT comment (umac-core must serialize this vs first-boot/shutdown).
11. `.../umac/umac_mmdrv_shim.c` `hw_restart_evt_handler` — branch: `g_mmdrv_soft_hw_restart` ? soft : legacy.

Red-team majors merged: the `morse_trns_reset` bus_lock leak (#4), the soft_start self-unwind (#6), and FIX-1b
(a guard against `gpio_install_isr_service`) was **dropped as refuted** — `mmhal_os.c:28` already holds RESET_N
low before the `:32` install and that install arms no per-pin interrupt.

**Builds green** (`Project build complete`, bin 0x1753e0 fits 0x177000). **Not bench-verified.**

## 3. Why default OFF (deviation from the design's default y)

The design set the Kconfig default to `y` (confident, adversarially verified). I shipped it **default n** because:
(1) it is delicate SPI-bus-teardown code where subtle bugs hide, (2) the bench could not verify it this session
(§4), and (3) the design itself flags a residual concurrency risk (`g_mmdrv_bus_persistent` needs umac-core
serialization — grep-verify before enabling) and a Linux follow-verify that is "convention, grep-verify on
chronite before merge". Default n keeps the tree behaving exactly as today; enabling FIX-1 is one flag flip
(`CONFIG_HALOW_SOFT_HW_RESTART=y`, or set the runtime `g_mmdrv_soft_hw_restart`) **paired with the on-bench
validation hooks** (RTC-noinit `restart_attempts`/`restart_completions`/`boot_count`, a non-owner-core canary
task watching for a >300 ms inter-edge gap, and a deterministic driver firing N restarts back-to-back).

## 4. Bench read (task c) — BLOCKED; the block is NOT the instrumentation

Rig intended: board0 (client) → board2 (relay/DUT, only fully-wired ESP) → chronite (Linux far endpoint,
`rimba-mesh`, SAE `rimbamesh2026`, ch27, 10.9.9.2). What actually happened:

- **board2's mesh plink FLAPS.** A cleanly PPK2-cold-booted board2 has a **healthy chip** — its SAE + AMPE
  peering with chronite **ESTABLISHES** (chronite log: "mesh plink with e2:72:a1:f8:f0:08 established", 8×) —
  but the plink **closes** repeatedly (8 established / 5 closed, ~1 per 20-30 s; station dump shows 0 peers most
  of the time; ping 100 % loss). No stable plink → no routing → no data traffic → the RX/forward counters stay
  0 → the drop cannot be measured. This is board2's known intermittent instability (the interrupt-WDT / RA
  flap), now seen even without forward load in the current bench state. **FIX-1 is the candidate fix** and is
  the reason to flip it on next: if the flap is the hw_restart crash-loop, FIX-1 should stop it.
- **Instrumentation exonerated.** chronite (independent Linux firmware) also needed a mesh restart to beacon,
  and the ESP↔chronite peering **crypto succeeds** — the failure is at the plink-stability layer, not in the
  RTC-noinit counters or the S3 code.

## 5. Bench gotchas nailed this session (save the next session hours)

- **Serial reader (corrects the bench doc):** the safe non-reset combo on these XIAO ESP32-S3 native-USB boards
  is **`dtr=False` / `rts=False`** (clean run: EN high, IO0 high). The doc's `dtr=True/rts=False` HOLDS IO0 LOW
  → a reset drops into DOWNLOAD mode = **silent** (the boards looked dead for a while). Tools left in the
  session scratchpad: `safe_read.py` (dtr=False/rts=False), `reset_read.py` (connect-then-pulse-reset-into-app,
  captures the full boot incl. the RTC boot dump).
- **Opening the USB-JTAG console WARM-RESETS the board** (USB_UART_CHIP_RESET, `reset_reason=11`), and a warm
  reset leaves the MM6108 GARBAGE ("Error retrieving version info - 3", chip ID 0x0000, MAC 00:..:00, "Channel
  list not set") → `mmwlan_mesh_start FAILED status=4` (=`MMWLAN_CHANNEL_LIST_NOT_SET`). Only a real POWER-CYCLE
  heals it. So: cold-boot board2 (PPK2) and **do not console it during a run** — read its RTC counters from the
  boot dump on the *next* boot (RTC survives). board0/board1 can't be PPK2-cold-booted, so once warm-reset-wedged
  they can't be un-wedged remotely.
- **`pkill`/`pgrep`/`awk` self-match bit me 3×** (exit 144): any pattern containing the literal script name
  matches your OWN shell's cmdline → self-kill. Kill the PPK2 hold by exact PID (or an `awk` pattern the command
  doesn't contain literally). Killing `tools/ppk2_hold.py` powers board2 OFF (its cleanup toggles DUT off);
  restarting the hold cold-boots board2.
- **chronium morse0 monitor delivered 0 frames** this session despite live on-air traffic (proven by chronite's
  wpa log seeing the ESPs) — the monitor is broken / needs re-setup. Do NOT trust "0 frames = dead air".

## 6. Next steps

1. **Recover the bench** (physical is fine): a full clean reset of all nodes; re-verify the chronium morse0
   monitor works (it delivered 0 frames — needs re-setup) so on-air state is observable non-invasively.
2. **Test whether FIX-1 stops board2's flap** — build `CONFIG_HALOW_SOFT_HW_RESTART=y`, flash board2, PPK2
   cold-boot, watch the plink via chronite (0 flaps = FIX-1 is the fix, and the read is unblocked). Add the §3
   RTC-noinit restart telemetry + non-owner-core canary before trusting it.
3. **Run the read**: with a stable board2 relay, blast board0→board2→chronite (or drive from chronite so no ESP
   console is needed), then read board2's RTC `RX-DBG[boot]` to name the ~99 % drop bucket
   (allowlist / plaintext / hw-ccmp / sw-ccmp / no-decrypt), per the prior diagnosis
   (`docs/worklog/2026-07-12-...` A-MPDU chain + the design doc §S3/B6).
4. Grep-verify on chronite the FIX-1 Linux-provenance + the umac-core serialization residual risk before
   enabling FIX-1 for real.

## 7. FIX-1 ON-BENCH TEST (toggle=y) — hw_restart never fires at idle; FIX-1 not the flap fix

Enabled FIX-1 (`CONFIG_HALOW_SOFT_HW_RESTART=y` → runtime `g_mmdrv_soft_hw_restart=true`) + added minimal RTC
telemetry (the design's edit 12, minimal): `g_hwr_boot_count` (app_main entries — a crash-reboot loop climbs
it), `g_hwr_attempts` (hw_restart_evt_handler entries), `g_hwr_completions` (restarts that returned). Flashed
board2, PPK2-cold-booted, ran it against a live chronite mesh, read the RTC counters via the boot dump.

**Three cold-boot runs, and `restart_attempts=0` EVERY time** (hw_restart_evt_handler never entered), with
`boot_count` staying at cold-boot + my console-read resets only (no crash-reboots):
- **Run A** (FIX-1, no telemetry): board2 peered + flapped 8 established / 5 closed, then went quiet.
- **Run B** (FIX-1 + telemetry): `boot_count=3 restart_attempts=0`; chronite log showed board2 established 0
  new plinks — board2 did NOT peer this run.
- **Run C** (fresh chronite + cold-boot board2): `boot_count=2 restart_attempts=0`; delta 0 establishes —
  board2 again did NOT peer.

**Conclusions (definitive from `restart_attempts=0` + `boot_count` low):**
1. **board2 does NOT crash-loop / hw_restart at idle** — so the mesh-plink flap (Run A) and the general
   non-peering are a **mesh peering / SAE-MPM-layer** problem, NOT the INT-WDT hw_restart crash. The prior
   sessions' assumption that "board2 flapping == the interrupt-WDT crash-loop" does NOT hold at idle (no
   forward load). The INT-WDT crash is a **forward-load** phenomenon; the idle flap is separate.
2. **FIX-1 is not the fix for what blocks the read**, and it could not be **exercised** on-bench: it only runs
   on a hw_restart, which never fired — the bench never reached the sustained-forward-load state that triggers
   the crash (the peering never stabilized enough to carry load). FIX-1 **builds, boots, and runs cleanly**
   (board2 ran the FIX-1=y firmware for 60 s with zero crashes / regressions), but its crash-fixing behaviour
   remains **unverified**.
3. **board2's mesh is degrading over the session** — it peered+flapped in Run A then would not peer at all in
   Runs B/C (consistent with the known RF/thermal marginality on a long bench session, and possibly the MM6108
   wearing through repeated reflash + cold-boot cycles).

**Revised next steps:** (i) the read is blocked by the **idle mesh-plink instability** (peer→close / won't peer)
— diagnose that at the MPM/SAE layer (needs a working chronium morse0 monitor to capture the peering frames,
and/or an all-ESP line where peering is more reliable), NOT via FIX-1. (ii) FIX-1 still needs a **loaded**
relay to exercise its target crash — only reachable once the idle flap is fixed and a client can push sustained
traffic. (iii) The `boot_count`/`restart_attempts` RTC telemetry is the right instrument for both — keep it.

**After every test:** radio-silent — `rimba-hello` on all ESPs + `ip link set wlan1 down` on the Linux nodes.
This session ended radio-silent (board0/1/2 = rimba-hello; chronite + chronium wlan1 down; board2 stays
PPK2-powered running hello = silent). The FIX-1 toggle was reverted to default OFF (the `=y` removed from the
gitignored app sdkconfig). Nothing committed.

## 8. LOCALIZING THE PLINK FLAP — it's ESP-side; ESP↔ESP test inconclusive (bench chips degraded)

Since the flap (not FIX-1's crash) is what blocks the read, localized WHERE the mesh peering breaks.

**All-Linux control (chronite ↔ chronogen, same secured config `rimba-mesh` / SAE / ch27, same air):
ROCK STABLE** — 0 establish / 0 close over 90 s, peer -45 dBm active (inactive 92 ms), ping **8/8 0% loss**.
→ **The flap is NOT environmental (RF/thermal/power); it is ESP-side.** Earlier chronite logs for the ESP peer
showed establish → "MPM closing plink" → "deauthenticated due to inactivity": the ESP ESTABLISHES then goes
SILENT (stops maintaining the plink) and chronite times it out. NB the ESP↔Linux mesh WAS stable earlier
(ping 45/45, same day) → regression/condition; suspect the recent S1 **MFP=no** (touched mesh peering
security) + S2/S3.

**ESP↔ESP test (to split native-vs-interop) — INCONCLUSIVE.** Added mesh-plink RTC telemetry
(`g_plink_estab`/`g_plink_close`: umac_mesh.c ESTAB sites :2969/:2987 + `mesh_peer_free` teardown; dumped
"PLINK-DBG" — stable peer ⇒ estab=1, flap ⇒ climbs). Ran board2 (cold-boot) ↔ board0 (flash), chronite DOWN,
90 s: **both estab=0** — neither peered. board0's MM6108 came up **garbage** (version-info error, chip ID
0x0000, `CHANNEL_LIST_NOT_SET`, mesh_start FAILED=4) even on a plain flash-boot, and board2's cold-boot mesh
had gone unreliable (peered in this session's first run, not since). **Root: the ESP MM6108s have DEGRADED over
a very long, abuse-heavy session** (dozens of reflash + cold-boot + thermal cycles), and board0/board1 have NO
power-cycle path (only board2 is PPK2-cold-bootable). So native-vs-interop could not be measured.

**Adjacent ESP finding:** a **warm ESP reset does NOT cleanly reset the MM6108** — it comes back garbage
(version-info error, `CHANNEL_LIST_NOT_SET`); only a real POWER cycle heals it. `mmhal_wlan_hard_reset`'s
RESET_N pulse (5 ms low / 20 ms high) may be insufficient after the chip has been running. ESP-side, adjacent
to FIX-1's chip-reset-over-bus work, and possibly related to the peering marginality.

**Next (needs a FRESH bench — physical power-cycle ALL nodes + cool-down):** (1) redo the ESP↔ESP plink test
with the telemetry already in place → native ESP MPM bug vs cross-vendor interop; (2) bisect the ESP↔Linux
flap against the last-good tree, MFP=no first; (3) fix the chronium morse0 monitor (0 frames this session) to
capture the ESP's MPM/SAE/keepalive frames vs the stable Linux exchange. Then the RX-drop read + FIX-1-under-load
become reachable.
