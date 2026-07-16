# 2026-07-15 — FIX-1 (bus-preserving hw_restart): bench-verified, and the verdict is DON'T SHIP

**Status: FIX-1's mechanism WORKS but delivers no demonstrable benefit; a separate, bigger gap makes it moot for
mesh today.** Recommendation: split FIX-1 out of rimba PR #33 (code-review finding #4) and backlog it *paired with*
a mesh hw-restart handler, which is the actual blocker. Nothing about FIX-1 changed; the scaffold was stripped.

## The question
Code-review finding #4: halow `b0ea9f6a` bundles ~194 lines of **dormant** FIX-1 (Kconfig `HALOW_SOFT_HW_RESTART`
`default n`, unset in the mesh-perf sdkconfig) whose own commit message says "NOT YET bench-verified". Ship it,
split it, or verify it? The user chose: verify and close it first.

## Method
The only production trigger for a hw_restart is a health-check failure (`driver_health.c:48` → `morse_reset_chip` →
`mmdrv_host_hw_restart_required`), which needs a chip that has stopped answering over SPI. A **temp scaffold** in
`driver_health.c` forced that verdict synthetically, under real relay-forward load. Legitimate because the
interrupt-WDT trip FIX-1 removes is **host-side** (`spi_bus_free`/`esp_intr_alloc` cross-core IPC) and does not
depend on the chip being wedged.

- **Relay = board2**, per the standing bench rule — board0/board1 have WAKE+BUSY unwired
  (`docs/reference/rimba-bench-devices.md`) and board0-as-relay crash-loops from the wiring, which would confound
  the A/B. Line: board1 (client) → **board2 (RELAY)** → board0 (`iperf -s -u`, 10.9.9.136). Endpoints ran a clean
  build; only the relay carried the scaffold.
- **Single variable:** `g_mmdrv_soft_hw_restart` (0 = baseline full teardown, 1 = FIX-1 soft restart).
- 6 forced restarts per arm at a 12 s cadence after a 40 s settle; UDP `-b 5 -t 70`.

## Results

| | forced restarts | SPI re-inits | crashes (INT-WDT/assert) | mesh after restart #1 |
|---|---|---|---|---|
| **A — FIX-1 OFF** (full teardown) | 6 | **8** (2 boot + 6 restarts) | **0** | dead — server 0.00 Mbit/s, never recovers |
| **B — FIX-1 ON** (bus-preserving) | 6 | **2** (boot only) | **0** | dead — server 0.00 Mbit/s, never recovers |

`Actual SPI CLK 40000kHz` (printed by `spi_bus_initialize`) is the instrument: 8 in arm A proves the full
teardown/re-init really ran on each restart; 2 in arm B proves FIX-1 genuinely preserves the bus.

## Findings

1. **FIX-1's mechanism is VERIFIED.** 8 → 2 SPI re-inits is a clean, single-variable demonstration that
   `mmdrv_soft_restart` routes through `morse_trns_soft_stop/start` and keeps SPI2_HOST + spi_handle + bus_lock +
   the GPIO ISR handlers allocated across a chip reset + FW reload. It also does not assert or misbehave
   (`MMOSAL_ASSERT(mmdrv_soft_restart(...) == 0)` never fired across 6 restarts).
2. **FIX-1 fixes no crash we can reproduce.** The baseline arm — the exact full SPI-host teardown FIX-1 exists to
   avoid — ran 6 times on properly-wired hardware with traffic flowing and **never tripped the interrupt watchdog**.
   With no crash in the control arm, there is no benefit to measure.
3. **NEW (blocking) — a mesh node cannot survive ANY hw_restart.** In **both** arms the server's received rate goes
   to 0.00 Mbit/s ~4 s after the first restart and never recovers. Root cause is code-established:
   `hw_restart_evt_handler` (`umac_mmdrv_shim.c:68`) restores scan + STA connection only, and
   `umac_interface_reinstall_vif` is called **exclusively** with `UMAC_INTERFACE_STA`
   (`umac_connection.c:1660`). There is no `umac_mesh_handle_hw_restarted`: after the chip reset wipes its vifs,
   nothing re-adds the mesh vif, re-runs SET_MESH_CONFIG, reinstalls the mesh keys, or restarts beaconing. The relay
   goes **silently deaf**. This is FIX-1-independent (it happens in the baseline arm too) and is strictly the bigger
   problem — note the ugly irony that the *baseline's* crash-reboot at least self-heals via the app's re-init,
   whereas a successful FIX-1 would leave a silent permanent zombie.
4. **NEW — the periodic health check never runs under load.** `skbq.c:333` sets
   `health_check.last_checked = mmosal_get_time_ms()` on every successful chip transaction, so `should_skip()`
   defers the periodic check forever while traffic flows (measured: 38 skips / 3 checks, all 3 before traffic
   started). ⇒ **under load the ONLY path to a restart is the 30-comm-failure escalation** (`comms_op_check` →
   `driver_health_demand_check` → `check_demanded`), exactly the Stage-2 chain in
   `[[mesh-relay-intwdt-rootcause]]`. The scaffold had to mirror that path to fire at all.

## Honest limitations
- **n=1 restart-under-load per arm.** Because of finding #3, traffic dies after the first restart, so restarts 2–6
  ran on an idle relay. Only restart #1 in each arm was genuinely under forwarding load.
- The board2 crash is documented as **INTERMITTENT** (crashed during one blast; survived 540 s and 110 s in others).
  So "0 crashes" here does **not** prove the crash cannot happen on board2 — it proves the teardown does not
  *deterministically* trip the WDT. Refuting an intermittent fault needs far more than 1 loaded restart per arm.
- The forced failure is synthetic: the chip stays healthy, so this does not reproduce whatever SPI state a genuine
  fault leaves behind.

## Disposition
- **Split FIX-1 out of PR #33.** It is dormant, its target crash does not reproduce on properly-wired hardware under
  forced restarts, and it cannot deliver its benefit for mesh until #3 is fixed.
- **Backlog: FIX-1 + a mesh hw-restart handler as ONE item.** A `umac_mesh_handle_hw_restarted` (mirroring
  `umac_connection_handle_hw_restarted`: reinstall the mesh vif, reconfigure the channel, reinstall keys, restart
  beaconing, re-peer) is the prerequisite. Revisit FIX-1 only with a rig that actually reproduces the crash.
- Also worth noting for the future: finding #4 means a relay under sustained load is *insulated* from the periodic
  health check, which weakens the original "relay = restart-frequency amplifier" framing — the relay reaches a
  restart only via comm failures, not via the timer.

## Rig gotchas (cost real time this session)
1. **`printf` blows the health task's 400-word (1600 B) stack** → truncated print, corrupted `driver_data`, then
   `assert failed: xQueueSemaphoreTake queue.c:1713 (pxQueue->uxItemSize == 0)` on the next `mmosal_semb_wait`.
   The scaffold bumped it to 1536 words. This crash **armed the documented persistent boot-storm** on board0
   (INT-WDT every ~1 s), which looks exactly like the real bug but was self-inflicted.
2. **The client's UDP rate is meaningless — read the SERVER's.** With D1 in the tree, an undeliverable origin frame
   is dropped and returns `MMWLAN_SUCCESS`, so lwIP/iperf see a successful send: a fully broken path reports the
   full offered rate (4.72 of 5 Mbit/s). The server-side receive rate is the only ground truth.
3. **Serial nodes are volatile.** A crash-looping board re-enumerates and `/dev/ttyACM0` can vanish
   (board0 → ttyACM5). Resolve `/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_<EFUSE_MAC>-if00` on
   every attempt; "esptool can't sync" was a stale path, not a dead board.

Bench radio-silent afterward (all 3 ESPs → rimba-hello, board2 left powered via the PPK2 hold). Scaffold stripped;
app `git restore`d. Nothing committed from this investigation.
