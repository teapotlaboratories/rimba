# Worklog — 2026-07-12 — ESP mesh-relay Interrupt-WDT crash: code-only root cause

**Author:** Aldwin
**Phase:** 802.11s mesh throughput — A-MPDU aggregation (unblocking a clean multi-hop number)
**Goal:** Determine, from **code only** (bench radio-silent, board2 PPK2-unpowered), whether the ESP
mesh **relay** node's Interrupt-Watchdog (INT_WDT) crash-loop under sustained forward load is a
**software bug** (fixable in host code, following Linux) or a **hard ESP32 capacity wall**.
**Status:** **Root cause found + adversarially verified. VERDICT: software bug, not a hard wall
(high confidence).** The relay explains restart *frequency*, not crash *susceptibility* — the fatal
mechanism is a role-independent, ESP-specific recovery-teardown path. Ranked follow-Linux fixes below.
Nothing implemented, nothing committed; bench untouched (already radio-silent).

This entry is **standalone**.

---

## 0. TL;DR

Under sustained relay load the ESP relay repeatedly **arms** a disproportionate recovery path, and
that recovery path — a full `mmdrv_deinit()`/`mmdrv_init()` that **frees and re-allocates the ESP SPI
host controller and its interrupts across cores via blocking IPC** — is what actually trips INT_WDT.
It is triggered far more often on a relay because the mesh **forward runs in the umac-core evtloop task
and blocks there for up to 1000 ms**, stalling all mesh RX processing → RX/pktmem backlog → the SPI
comm/pager failures that pile up until the 30-failure recovery escalation fires. The panic signature
(INT_WDT, `TASK_WDT_PANIC` disabled) plus the byte-identical originator staying stable rule out a
hardware wall. (An earlier draft placed this block under the driver-task `bus_lock` — see the Stage 1
correction; the block is in the evtloop, and the verdict + fixes are unchanged.)

Two backtrace leads are explained: **(A)** a core in `gpio_isr_loop` and **(B)**
`gpio_isr_register_on_core_static → esp_intr_alloc` on an `ipc_task` are the two sides of a cross-core
stall during the SPI-host teardown/re-init.

---

## 1. The failure and the two panic leads

- Symptom: an ESP node acting as a **relay** (board1 → board0(relay) → board2) crash-loops on
  **INT_WDT — 300 ms, dual-core checked** (`CONFIG_ESP_INT_WDT_TIMEOUT_MS=300`,
  `CONFIG_ESP_INT_WDT_CHECK_CPU1=y`; `CONFIG_ESP_TASK_WDT_PANIC` **not** set, `TASK_WDT_TIMEOUT_S=5`).
- An ESP node that only **originates** on the **same firmware** is rock-stable. A **Pi-5 Linux relay**
  never exhibits this.
- Panic dump leads: **(A)** a CPU in ESP-IDF `gpio_isr_loop`; **(B)**
  `gpio_isr_register_on_core_static → esp_intr_alloc` running on an `ipc_task`.
- Correlated (not proven causal) with opening the relay's USB-CDC console mid-run (XIAO-S3 DTR/RTS).

## 2. Root-cause chain (three stages)

### Stage 1 — relay self-throttle: the mesh forward blocks the **umac-core evtloop task** (CODE-CONFIRMED)

> **Correction (2026-07-12, found while implementing FIX-2/S3):** an earlier draft of this section placed
> the forward's block *inside the driver-task `bus_lock`*, starving the SPI worker. That was **wrong**. The
> RX drain does **not** process the forward inline: `umac_datapath_rx_frame` (`umac_datapath.c:1933`) only
> **queues** the frame + `umac_core_evt_wake` (`:1945-1947`), and `umac_datapath.c` holds **no** `bus_lock`
> at all (grep-clean). The forward runs later in the **umac-core `evtloop` task** (a separate PRI_HIGH task,
> `umac_evtloop.c:109`). The block is real and still the arming mechanism — but it stalls the *evtloop*, not
> `bus_lock`. Verdict (§5) and both fixes (§6) are unchanged by this correction.

The driver task (`driver_task_main`, `driver_task.c:159`) runs `morse_pagesets_work` (`pageset.c:970`)
under `bus_lock` (`pageset.c:985-1077`), but inside that hold it only **reads RX pages and hands each frame
to `umac_datapath_rx_frame`, which queues it and wakes the evtloop** — it does not forward inline:

```
[driver task, under bus_lock]                pageset.c:985  morse_trns_claim(bus_lock)
  morse_pageset_rx_handler                   pageset.c:992
    morse_skbq_process_rx → mmdrv_host_process_rx_frame     pageset.c:688, skbq.c:329
      umac_datapath_rx_frame                 umac_mmdrv_shim.c:33 → umac_datapath.c:1933
        umac_datapath_rx_queue_frame + umac_core_evt_wake   umac_datapath.c:1945-1947   ← QUEUES, returns
  morse_trns_release(bus_lock)               pageset.c:1077

[umac-core "evtloop" task, PRI_HIGH, umac_evtloop.c:109 — holds NO bus_lock]
  umac_datapath_process_rx                   umac_datapath.c:3117
    …after-reorder…                          umac_datapath.c:649
      umac_mesh_forward_data (mesh_da != us) umac_datapath.c:867-874, umac_mesh.c:2573
        umac_datapath_tx_mesh_keyed_frame    umac_datapath.c:2694
          wait_for_tx_ready_(calculate_tx_timeout_ms(umacd, /*blocking=*/true))  = 1000 ms  mmwlan.h:2821
```

For a **relay**, the evtloop reaches the forward, does inline SW-CCMP encrypt, then **blocks up to 1000 ms**
in `wait_for_tx_ready` for TX headroom — **stalling the whole evtloop**, which is the single task that drains
*all* mesh RX + events. While it is blocked, the driver task keeps reading RX pages and queuing them to the
evtloop's RX list, which the stalled evtloop can't drain → RX-queue / pktmem backlog → pager/RX-alloc failures
(`morse_hw_pager_update_consec_failure_cnt`, `pageset.c:905`; second failure path `hw.c:195-198`) and SPI
comm-op timeouts → the failure counters that arm the restart (Stage 2). (`mmdrv_tx_frame` only enqueues to a
skbq — `driver.c:1313`, no inline SPI.) The precise evtloop-stall → failure-count chain remains bench-gated
(§8); the *code-confirmed* fact is only that the forward blocks the evtloop, not `bus_lock`.

### Stage 2 — transient SPI failures escalate to a full chip + host restart (CODE-CONFIRMED)

Every SPI op runs `comms_op_check` (`sdio.c:121`, call sites `:510/:575/:611/:653`). Backpressure and
timeouts increment `consecutive_comm_failures`; at `MAX_COMM_FAILURES = 30` (`sdio.c:106,126`) it masks
the IRQ (`sdio.c:129`) and calls `driver_health_demand_check` (`sdio.c:130`). The PRI-**LOW** health task
(`driver_health.c:30`) runs a 1-retry `morse_cmd_health_check`; on failure →
`morse_reset_chip` (`driver_health.c:41`, pauses TX for HW_RESTART `:43`) →
`mmdrv_host_hw_restart_required` (`umac_mmdrv_shim.c:100`) → queues `hw_restart_evt_handler` to the
umac-core task (`:104`).

### Stage 3 — the restart tears down + re-allocates the SPI host + its interrupts across cores (path CONFIRMED; >300 ms duration INFERRED)

`hw_restart_evt_handler` (`umac_mmdrv_shim.c:68`) runs **`mmdrv_deinit()` then `mmdrv_init()`**
(`:87-88`) → `morse_trns_stop`/`start` (`sdio.c:836/769`) → `mmhal_wlan_deinit`/`init`
(`mmhal_wlan.c:241/233`) → **`spi_bus_remove_device` + `spi_bus_free(SPI2_HOST)`** (`mmhal_wlan.c:107`)
then **`spi_bus_initialize(..., SPI_DMA_CH_AUTO)` + `spi_bus_add_device`** (`mmhal_wlan.c:64,79,93`).

`spi_bus_free`/`initialize` free + alloc the SPI DMA/ISR vector via `esp_intr_free`/`esp_intr_alloc`.
Because **every morselib task is created unpinned** (`xTaskCreate` = `tskNO_AFFINITY`,
`mmosal_shim_freertos_esp32.c:220`), `esp_intr_free` takes the `task_can_be_run_on_any_core` branch
(`vendor/esp-idf/.../intr_alloc.c:744`) and **routes to the vector's owner core via
`esp_ipc_call_blocking`** (`intr_alloc.c:747-750`) → the `ipc_task` running
`esp_intr_alloc`/`gpio_isr_register_on_core_static` = **backtrace B**. This is the **only post-boot
site** that frees/allocs interrupts, so backtrace B is effectively a fingerprint that a hw_restart was
in flight at crash time.

The freeing/allocating core enters `portENTER_CRITICAL(&spinlock)` on the **global** intr-alloc spinlock
(`intr_alloc.c:121`), the *same* spinlock taken from **ISR context** by shared-vector dispatch via
`portENTER_CRITICAL_ISR` (`intr_alloc.c:459/481`). While one core holds it (interrupts disabled there)
across the IPC round-trip, the **other** core taking any shared interrupt (GPIO/SPI) spins on it with
interrupts disabled → **backtrace A**. With `INT_WDT_CHECK_CPU1=y`, either stuck core → **INT_WDT**.

**Confirmed vs inferred:** Stages 1–2 fully code-confirmed; that a restart was active at crash time
(backtrace B is reachable *only* via `spi_bus_initialize`/`esp_intr_alloc`); the shared global spinlock
hazard. **Inferred-pending-bench:** that the window actually held a core's interrupts off **>300 ms**
(source can't bound `esp_ipc_call_blocking` + spinlock duration), and that relay load actually reaches
`consecutive_comm_failures == 30` (no clean repro). Both are bench-measurable (§6).

## 3. Relay vs originator — frequency, not susceptibility

The forward branch (`umac_datapath.c:871`, gated `mesh_ctrl_present && mesh_da != us`) is reachable
**only** from the umac-core evtloop's RX processing; an originator never reaches it. The originator runs
the *identical* `umac_datapath_tx_mesh_keyed_frame` from the app/lwIP task (`mmwlan_tx`) — its identical
up-to-1000 ms wait blocks only its own caller, never the evtloop that drains mesh RX. So the relay's
forward stalls its own RX pipeline and drives `consecutive_comm_failures` to 30 far more often, **arming**
the fragile restart far more often.

But the fatal `hw_restart` teardown is **role-independent** — it runs on *any* restart trigger. There
are four `driver_health_demand_check` sites; `hw.c:75` (`MORSE_INT_HW_STOP_NOTIFICATION`, a firmware
watchdog) has nothing to do with role, so an originator that ever hits HW_STOP crashes identically. The
USB-CDC console-open is a code-plausible **role-independent** trigger too: the GPIO ISR service is
non-IRAM (`mmhal_os.c:32`) and `gpio_set_intr_type` is flash-resident (`GPIO_CTRL_FUNC_IN_IRAM` unset),
so a flash-cache-disable window can delay IRQ masking → SPI failures → the same restart. **The relay is
a restart-frequency amplifier, not a distinct crash class.** This is why the fix priority is FIX-1
(make the restart safe for all triggers) *and* FIX-2 (remove the relay's amplification).

## 4. Refuted hypotheses

- **"Level-triggered SPI IRQ spins `gpio_isr_loop` during teardown"** — REFUTED as the primary
  mechanism. The pin is masked `GPIO_INTR_DISABLE` **three times** before `spi_bus_free`
  (`sdio.c:129`, `driver.c:515`, `sdio.c:839`), and `mmhal_wlan_deinit` lowers **RESET_N=0 first**
  (`mmhal_wlan.c:245`), de-asserting the chip IRQ source. (One lens still flagged the *latent* ordering
  fragility in `mmhal_wlan_deinit`, which never masks in-function — safe today only because callers
  pre-mask; closed cheaply by FIX-4.)
- **"Pure level-IRQ `gpio_isr_loop` livelock, no restart"** — REFUTED. The ISR masks before
  `gpio_isr_loop` clears status; the loop is IRAM, bounded, single-snapshot; the sole steady-state
  re-enable is in the worker **task** (`hw.c:87`) — a strict 1:1 mask/re-enable handshake that a task
  cannot storm. Also role-agnostic, so it can't explain a relay-only crash.
- **Priority inversion on `bus_lock`** — REFUTED. `bus_lock` is `xSemaphoreCreateMutex` **with**
  priority inheritance (`mmosal_shim_freertos_esp32.c:325`). The Stage-1 self-throttle is *not*
  inversion (the boosted holder is itself the stuck task).
- **Hard ESP32 capacity wall** — REFUTED as the panic cause (§5).

## 5. Verdict: software bug, not a hard wall (confidence: HIGH)

Decisive is the **failure signature**: INT_WDT fires only when a core keeps interrupts disabled / stuck
in ISR-context >300 ms. A genuine throughput/worker/memory ceiling manifests as backpressure, loss, and
comm-op timeouts — and, on this build (`TASK_WDT_PANIC` disabled, 5 s), at most a *non-panic* task-WDT
warning. It never disables interrupts for 300 ms. So the panic is categorically a
code-in-critical-section fragility, not bandwidth exhaustion. Reinforcing: the originator uses the
byte-identical SPI stack (single SPI2_HOST @ 40 MHz, one 768-word worker, same sub-75B CPU-poll xfers)
and is stable; and the code has abundant software slack (a one-arg change makes the forward best-effort;
the escalation threshold, teardown ordering, task affinity, IRAM flags are all host-code knobs). A real
throughput ceiling *does* exist, but its only effect is to raise the **rate of arming** the fragile
recovery path — trigger frequency, not panic cause.

*Caveat:* the exact >300 ms sub-mechanism (global intr-alloc spinlock + IPC round-trip vs. the latent
level-line window) is medium-confidence and needs a panic dump to pin. Both candidates are
software-removable, so the verdict is robust to which one it is.

## 6. Ranked follow-Linux fixes

> **Follow-Linux caveat:** `morse_driver`/`net/mac80211` are **not checked out on this disk** (only a
> `.git/modules/vendor/morse_driver` stub; the live tree is on **chronite `~/halow`** @ `morse_driver
> 3eef5a0` / `rpi-linux 372414fd`). The Linux "how it's done" claims below are convention and **must be
> grep-verified on chronite before implementing FIX-1/2/3** (per the ship-a-verified-code-map rule).

### True root-cause fixes

- **FIX-1 (primary — removes the panic mechanism): split "chip reset" from "SPI-host teardown."**
  Target: `hw_restart_evt_handler` (`umac_mmdrv_shim.c:68,87-88`), `morse_trns_stop/start`
  (`sdio.c:836/769`), `mmhal_wlan_deinit/init` (`mmhal_wlan.c:241/233`). Make hw-restart toggle RESET_N
  + reload FW + re-init chip state **without** `spi_bus_free`/`spi_bus_initialize` and **without**
  `gpio_isr_handler_remove/add` — keep the ESP SPI peripheral + `esp_intr` allocation persistent across
  a firmware/chip restart. *Linux:* `morse_driver` resets the chip / reloads FW but never `free_irq`s +
  re-probes the host MMC/SPI controller (kernel owns it persistently). Removes backtrace B and the whole
  cross-core interrupts-disabled window. Risk: medium (must fully re-init the chip over a live bus).
  **Full fix for the panic, for every trigger.**

- **FIX-2 (removes the relay trigger): non-blocking forward off the evtloop's blocking wait.**
  Minimal (very low risk): `calculate_tx_timeout_ms(umacd, true)` → `(umacd, false)` (= 0) at the
  forward's wait site so it drops-on-full instead of stalling the evtloop up to 1000 ms.
  Proper: at `umac_datapath.c:871`, don't call `umac_mesh_forward_data` synchronously in the evtloop RX
  processing — clone+queue to a forward work item and return; move SW-CCMP off the evtloop
  window. *Linux:* mirrors `ieee80211_rx_h_mesh_forward` (requeue to the tx path, drained by a separate
  workqueue) + qdisc drop-on-full. Risk: low (arg flip) → medium (requeue). **Removes the load→arm
  bridge; also independently raises multi-hop goodput (this is the S3/B6 relay-aggregation seam).**

- **FIX-3 (proportionate recovery): retry/backoff before escalation.** Target `comms_op_check`
  (`sdio.c:121`). Retry/backoff transient CMD52/53 errors; gate the full restart on a *confirmed* FW
  watchdog (`MORSE_INT_HW_STOP_NOTIFICATION`, `hw.c:73`) or a failed health-ping, not a raw count of 30
  transient failures. *Linux:* retry-not-restart is the `morse_driver` transient-error convention. Risk:
  low-medium. Cuts restart frequency; pairs with FIX-1/2.

### Defense-in-depth hardening

- **FIX-4: mask before free in deinit.** In `mmhal_wlan_deinit` (`mmhal_wlan.c:241`) call
  `gpio_set_intr_type(SPI_IRQ/BUSY, GPIO_INTR_DISABLE)` **before** `spi_bus_free` and
  `gpio_isr_handler_remove`. Closes the latent level-line-spin window regardless of caller ordering.
  Risk ≈ zero. (Not the current cause; callers pre-mask today.)
- **FIX-5: pin the driver tasks to one core** (`xTaskCreatePinnedToCore` in `mmosal_task_create`,
  `mmosal_shim_freertos_esp32.c:220`) for the SPI worker, driver_task, umac-core, health tasks. Then
  `esp_intr_free` frees locally instead of via IPC, and teardown can't contend cross-core with an ISR on
  the opposite core. *Linux:* single-CPU-affinity SDIO IRQ model. Risk: medium (scheduling/throughput).
- **FIX-6: IRAM-safe GPIO ISR.** `CONFIG_GPIO_CTRL_FUNC_IN_IRAM=y` + install the GPIO ISR with
  `ESP_INTR_FLAG_IRAM` (`mmhal_os.c:32`) + `IRAM_ATTR` on `morse_spi_irq_handler` and the mask path, so
  IRQ masking can't stall on a flash-cache-disable window. Addresses the console-open correlation. Risk:
  low (IRAM budget).

## 7. Bench repro + validation plan (relay has no usable console)

Bench radio-silent; board2 PPK2-unpowered. Restore: repower board2 (`tools/ppk2_hold.py`), confirm fw
**1.17.8** on all nodes (dmesg fw SIZE 480664), bring up board1 → board0(relay) → board2.

Non-console instrumentation (the relay has no usable UART mid-run):
1. **RTC-noinit counters** (survive the panic, read after reboot): `hw_restart_count`, peak
   `consecutive_comm_failures`, `forward_wait_timeout_count` (MMWLAN_TIMED_OUT from the forward wait),
   `bus_lock` max-hold-µs in `morse_pagesets_work` (claim@985 → release@1077). **Proves Stage 1-2:** the
   relay reaches ≥30 / `hw_restart_count` increments / long holds + timeouts; the originator stays ~0.
2. **GPIO scope strobes** (spare pins): high on entry / low on exit of `hw_restart_evt_handler` and
   around `esp_intr_free/alloc`. **Proves the Stage-3 duration** (the one inferred link).
3. **`esp_core_dump` to flash:** read both cores' PC + interrupt state offline — confirm backtrace B =
   `esp_intr_alloc`/ipc_task, and resolve whether backtrace A is a spinlock spin vs. an innocent
   CHECK_CPU1 snapshot. **Resolves the residual mechanism uncertainty.**
4. **Logic analyzer on SPI_IRQ** across a crash window (one-shot vs. re-latch after RESET_N=0).

A/B experiments:
- **Isolate role:** load-sweep the relay to the crash; confirm the originator never restarts under
  identical offered load.
- **Prove the bridge (FIX-2 minimal alone):** flip the forward timeout to non-blocking; expect
  `hw_restart_count → 0` and no INT_WDT under the same load.
- **Prove the mechanism (FIX-1 alone):** keep the blocking forward but make hw-restart reuse the bus;
  force a restart via a test hook that injects 30 comm failures; confirm **no INT_WDT**.
- **Close the latent window (FIX-4):** deliberately call `mmhal_wlan_deinit` without pre-masking to open
  the level-line spin, confirm it hangs, then confirm FIX-4 closes it.
- **Console correlation (FIX-6):** with FIX-6 off vs on, open USB-CDC mid-run; expect the correlation to
  vanish with FIX-6.

**After every test:** radio-silent — flash `rimba-hello` to all ESPs + `ip link set wlan1 down` on any
Linux node.

## 8. Residual uncertainty (honest)

- The exact >300 ms interrupts-disabled duration and which of (spinlock-spin vs. IPC round-trip vs.
  latent level-line) dominates — needs experiments 2/3.
- No clean repro that sustained relay load actually hits `consecutive_comm_failures == 30` — needs
  counter #1.
- The USB-CDC/DTR correlation is a code-plausible aggravator, not code-provable as causal — needs the
  FIX-6 experiment.
- The follow-Linux "how it's done" claims are convention, not on-disk here — **grep-verify on chronite
  before merging FIX-1/2/3.**

**Verdict unchanged by any of these:** the failure signature + identical-HW originator stability +
in-code slack make this a **software bug**. FIX-1 removes the panic mechanism; FIX-2 removes the trigger.

## 9. Method note

Root-caused code-only via a 3-phase agent workflow (6 independent causal lenses → 3 adversarial
verifications incl. the hard-wall counter-thesis → synthesis), then re-verified by hand. The
software-vs-wall verdict and both fixes have held; the **task context of the blocking forward was
corrected twice**: the workflow moved it off the SPI worker onto the RX drain, and the FIX-2/S3
implementation pass (this same day) then established it runs in the **umac-core evtloop task**, not the
driver-task `bus_lock` (Stage 1 correction box). The relay claim is **frequency, not susceptibility**.
See [`../mesh-ap/rimba-mesh-ampdu-aggregation-design.md`](../mesh-ap/rimba-mesh-ampdu-aggregation-design.md);
the S2 multi-hop worklog explains why a stable relay is a prerequisite for a clean multi-hop A-MPDU number.
