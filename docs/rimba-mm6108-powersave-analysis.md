# MM6108 power-save in IBSS — firmware + host-stack analysis

**Date:** 2026-06-21
**Question:** the MM6108 can sleep, but chip power-save is "STA/AP-only." Is that
gate in the *firmware* or just in the *host driver*? And what wakes a sleeping
node — specifically, can it wake on incoming peer traffic in IBSS?

**Short answer:** the AP/STA gate is **host-side policy** (`umac_ps.c`), not the
command layer and not (provably) the firmware. The `CONFIG_PS` command itself is
interface-agnostic. **But** even if the chip dozes on an IBSS vif, the entire
*downlink-wake* path is AP-dependent — a dozing IBSS node has **no mechanism to
wake for unscheduled peer traffic.** It wakes only for its own TX (and its own
beacon TBTT) and on a hardware GPIO edge. This is the root reason Rimba uses an
**RTC-scheduled** duty cycle rather than chip power-save.

Companion to [`rimba-ibss-milestones.md`](rimba-ibss-milestones.md)
(the "morse driver has no IBSS radio power-save" Finding + the IBSS TODO/decisions).

---

## 1. The firmware blob (so this is reproducible)

`mm6108.mbin`, firmware `rel_1_17_6_2026_Feb_23`, 399,468 bytes.

- **Container:** Morse **MBIN TLV** stream (`mbin.h` / `firmware_mbin.c`): `MMFW`
  magic, then `FW_SEGMENT` TLVs (`base_address` + raw bytes), `EOF`-terminated.
  **All segments raw — not compressed, not encrypted, not opaque-signed.**
- **CPU:** **RISC-V 32-bit, RV32IMC** (compressed insns; `csrrs … mhartid`,
  `mtval`, `hart` strings; boot vector disassembles cleanly). Ghidra language
  `RISCV:LE:32:RV32IC`.
- **Memory map** (walked from the TLV chain):

  | space | regions | meaning |
  |---|---|---|
  | code RAM (exec) | `0x00100000`,`0x00120000`,`0x00150000`,`0x00158000`,`0x001f0000` | reset vector at `0x100000`; bulk code + rodata |
  | data RAM | `0x80100000`,`0x80200000`,`0x80300000`,`0x80400000` | `.data`; BCF loads at `0x8011fa80` |
  | peripherals (not in file) | `0x10000000` | clock/sys-ctrl regs (`0x10054xxx` etc.) |

- RE artifacts checked into `mm6108_re/` (segments, disasm, Ghidra map script,
  README) in the workspace root, outside this repo.

It being plaintext RISC-V with intact strings makes deeper RE *feasible* — but
the decisive findings below came from the **host source**, not the blob.

## 2. The power-save command surface (`morse_commands.h`)

| Cmd | ID | Note |
|---|---|---|
| `CONFIG_PS` | `0x16` | dynamic 802.11 PS toggle. Request = `{enabled, dynamic_ps_offload}` — **no interface-type field** |
| `SET_LONG_SLEEP_CONFIG` | `0x21` | WNM/long sleep, per-vif |
| `TWT_AGREEMENT_INSTALL/REMOVE/VALIDATE` | `0x26/0x27/0x36` | TWT — STA/AP scheduled wake |
| `STANDBY_MODE` | `0x31` | deep standby (host sleeps too) |
| `LI_SLEEP` | `0x49` | listen-interval sleep |

Interface types: `STA=1, AP=2, MON=3, ADHOC=4, MESH=5`. IBSS is `ADHOC(4)`,
mapped to a `UMAC_INTERFACE_ADHOC` (AP-type) vif in the port.

## 3. Where the AP/STA gate actually is

**The command sender is interface-agnostic.** `mmdrv_set_chip_power_save_enabled(vif_id, enabled)`
(`driver/driver.c:1217`) builds a `CONFIG_PS` for *any* vif_id and sends it. No
iftype check. The wire struct has no iftype field. → the driver/command layer
imposes **zero** gate.

**The whole gate is host policy in `umac/ps/umac_ps.c::umac_ps_update()`:**

```c
uint16_t sta_vif_id = umac_interface_get_vif_id(umacd,
        UMAC_INTERFACE_NONE | UMAC_INTERFACE_SCAN | UMAC_INTERFACE_STA);
if (sta_vif_id == UMAC_INTERFACE_VIF_ID_INVALID) {
    MMLOG_DBG("No STA interface. PS disabled\n");
    data->pwr_mode = MMWLAN_PS_DISABLED;
    return;                                 // <-- IBSS / ADHOC node stops here
}
uint16_t ap_vif_id = umac_interface_get_vif_id(umacd, UMAC_INTERFACE_AP);
if (ap_vif_id != UMAC_INTERFACE_VIF_ID_INVALID) new_mode = MMWLAN_PS_DISABLED;
...
(void)mmdrv_set_chip_power_save_enabled(sta_vif_id, true);   // only ever the STA vif
```

An ADHOC-only node matches no STA vif → logs *"No STA interface. PS disabled"* →
returns. **`CONFIG_PS` is never even sent in IBSS mode.** The gate is pure policy,
in code we already patch.

| layer | gates IBSS sleep? |
|---|---|
| `umac_ps.c` policy (`umac_ps_update`) | **YES — the entire gate** |
| `mmdrv_set_chip_power_save_enabled` → `CONFIG_PS` | no (any vif_id) |
| `CONFIG_PS` wire struct | no (no iftype field) |
| **MM6108 firmware `CONFIG_PS` handler** | **NO — firmware-confirmed: accepts ADHOC (see §7)** |

## 4. The three sleep layers and what wakes each

1. **Bus PS** (`morse_ps.c`, SPI transport host↔chip). Kept awake by refcounted
   *wakers*: `PS_WAKER_UMAC`, `PS_WAKER_PAGESET` (transfer), `PS_WAKER_COMMAND`
   (cmd in flight), `PS_WAKER_IRQ` (chip has data). Host wakes chip via the WAKE
   pin; chip wakes host via the IRQ pin. Transport-level, mode-independent.
2. **Dynamic chip PS** (`CONFIG_PS=0x16`). Radio dozes after
   `dynamic_ps_timeout_ms` of inactivity.
3. **Deep standby** (`STANDBY_MODE=0x31`). Host MCU sleeps too; chip wakes it on
   the explicit `morse_cmd_standby_mode_exit_reason` enum (wakeup frame,
   associate, **ext GPIO input**, whitelist pkt, TCP-loss, …).

## 5. What wakes a CONFIG_PS-dozing node

| Trigger | Wakes it? | Why |
|---|---|---|
| **Own outbound TX** (host queues a frame) | ✅ | host sets a bus waker → WAKE pin → radio wakes to TX. Mode-independent. |
| **Own beacon TBTT** | ✅ | an IBSS node must wake on its beacon timer to TX its beacon. TX, not peer RX. |
| **Incoming peer traffic (IBSS)** | ❌ | the hole — see below. |
| Deep-standby `EXT_INPUT` (GPIO edge) | ✅ | hardware pin, mode-independent. **The IBSS-viable wake.** |
| Standby `WAKEUP_FRAME` / `WHITELIST_PKT` | ❌ in IBSS | needs the radio already listening on an AP beacon/DTIM schedule to receive the frame to match. |

### Why peer RX cannot wake a dozing IBSS node

The downlink-wake path is **100% AP-dependent** (confirmed in `mmwlan.h`:
*"send PS-Poll every listen interval"*; *"only updated when traffic is received
from the AP"*; `listen_interval` is *"indicated in the association response… the
AP uses [it] to determine the lifetime of [buffered frames]"*):

```
doze → wake at listen_interval → read the AP's beacon TIM
     → PS-Poll the AP → AP delivers the frames it buffered for me
```

In IBSS none of this exists: no AP to **buffer** a frame for a sleeping peer; no
**TIM** to signal "you have traffic" (`s1g_tim.c` is AP-builds / STA-reads-AP);
**PS-Poll** has no target; the firmware has **no ATIM** (the IBSS-native
substitute) and **doesn't surface peer beacons** (#16). A frame a peer sends
while your radio dozes is simply **missed** — the sender has nowhere to park it
and no way to say "wake up."

## 6. Consequence

Forcing `CONFIG_PS` on an IBSS vif (if the firmware honors it — untested) yields a
node that wakes **only for its own TX and its own beacon**, deaf to peers between
its transmissions:

- ✅ Fine for a pure **leaf / sender**: wake on RTC, push telemetry, sleep. Lower
  idle power, wake-on-own-traffic is enough.
- ❌ Broken for any **receiver** (relay, bidirectional): it silently drops
  inbound peer frames while dozing.

This is exactly why Rimba's **Scheduled mode uses an RTC, not chip power-save**:
it replaces the missing AP/ATIM coordination with an **out-of-band shared clock**.
All nodes agree on listening windows via the RTC; nobody has to be woken by
on-air signalling that IBSS can't provide. And the one mode-independent hardware
wake that *does* work (§5, `EXT_INPUT`/GPIO) is precisely the RTC-alarm→GPIO path.

## 7. ANSWERED — firmware decompilation: CONFIG_PS accepts ADHOC

Decompiled the firmware blob with Ghidra (headless, RV32 on an aarch64 host —
the decompiler native had to be built from source). 2114/2114 functions
recovered. The command dispatcher is `FUN_0013677c` (a switch on `message_id`,
cases `0x16` CONFIG_PS, `0x26/0x27/0x36` TWT, `0x31` STANDBY, `0x35` IBSS,
`0x49` LI_SLEEP, …).

**The `CONFIG_PS` (0x16) handler reads the per-vif `interface_type` (vif struct
+0x24; same enum as the host: STA=1, AP=2, MON=3, ADHOC=4, MESH=5) and branches:**

```c
iftype = vif[vif_id].interface_type;          // (&DAT_80204094)[vif_id*0x1a6]
if (iftype == 1) {                            // STA
    ... set ps flags; FUN_00135740(...)        // + drive PS state machine now
    -> success;
}
if ((iftype == 2) || (iftype - 4u < 2)) {     // AP(2), ADHOC(4), MESH(5)
    dyn = cmd->dynamic_ps_offload;             // byte @ +0x0d
    if (cmd->enabled == 0 || dyn != 0) {       // disable always OK; ENABLE needs dyn!=0
        ps_ctx->enabled    = cmd->enabled;     // FUN_0012f72c -> ctx[4]
        ps_ctx->dynamic_ps = dyn;              // FUN_0012f73c -> ctx[5]
        -> success;
    }
    iVar20 = -0x16;                            // EINVAL: enable w/o dynamic_ps on AP/ADHOC/MESH
}
```

Findings:
- **ADHOC (4) is explicitly handled — not rejected.** It shares a branch with AP
  and MESH. So **chip power-save in IBSS is NOT firmware-gated.**
- The flags it writes (`FUN_0012f72c`→`ctx[4]`, `FUN_0012f73c`→`ctx[5]`) go into
  the **same global PS context** (`&DAT_8020a400`) the STA path uses — real PS
  state, not a stub.
- **One firmware rule for AP/ADHOC/MESH: enabling requires `dynamic_ps_offload != 0`**
  (plain enable returns `-0x16`/EINVAL). The host helper already sends
  `.enabled = .dynamic_ps_offload = enabled`, so it satisfies this as-is.
- Difference vs STA: the STA branch also calls `FUN_00135740` (kicks the PS
  state machine immediately); the ADHOC branch only sets the flags, which the
  lower-MAC PS loop consumes.

**Conclusion: the AP/STA restriction was entirely host-side policy
(`umac_ps.c`).** To make the chip doze in IBSS: bypass `umac_ps_update`'s STA-vif
requirement and call `mmdrv_set_chip_power_save_enabled(ibss_vif_id, true)` (which
sets `dynamic_ps_offload=1`). The firmware accepts it.

**Caveat unchanged (§5):** this arms *dynamic* PS — radio dozes, wakes on own TX,
**deaf to peers** while dozing (no AP TIM, no ATIM in IBSS). Good for a
leaf/sender; not coordinated mesh sleep. A bench measurement (radio-rail current
/ BUSY pin) is still worth doing to confirm the radio physically dozes and to
quantify the leaf-sender win — but the *gating* question is now answered.

Decompiler artifacts: `mm6108_re/mm6108_decompiled.c` (2114 funcs, local) and the
Ghidra project on chronium (`~/Developments/mm6108_re/ghidra_proj`).

## 8. Bottom line for the RTC-scheduled design

This analysis **reinforces** the RTC approach and removes power-save as a risk to
it: RTC-scheduled mode hard power-cycles the radio (RESET_N) and deep-sleeps the
ESP32 (RTC-alarm GPIO wake) — it never relies on the broken chip-PS or any on-air
wake. So feasibility of *coordinated mesh sleep* no longer hinges on the firmware.

The real remaining gate is **not** power-save but **RISK-02: radio
cold-boot-to-IBSS-joined time** (hardening-todo #9) — the per-window cost that
sets the achievable duty cycle and power budget. That number, not the PS
mechanism, decides how *efficient* RTC-scheduled mode is.

## 9. MM6108 hardware sleep modes (datasheet) and IBSS reachability

Source: public **MM6108-MF08651-US Data Sheet v102** §4.4.3 Table 6 (same MM6108
silicon as our mf16858 module). The bare-SoC datasheet is NDA-gated; this module
datasheet carries the figures.

### The three hardware sleep modes (VBAT @3.3 V, 25 °C)

| Mode | Datasheet condition | Typ current | Memory | Wake |
|---|---|---|---|---|
| **Snooze** | RC osc on, memory retained, wake on timer | **42 µA** | retained | timer / self (TX) |
| **Deep sleep** | RC osc on, wake on timer | **1 µA** | not stated retained | timer |
| **Hibernate** | power off, wait for external interrupt | **0.05 µA** | lost | external IRQ (GPIO) |

Context (awake): Listen ≈26–37 mA, active RX ≈26–67 mA; associated-but-PS
(DTIM3/DTIM10 averages) ≈95–395 µA.

### Which are reachable on an ADHOC (IBSS) vif

| Mode | Driven by | IBSS reachable? |
|---|---|---|
| **Snooze** | autonomous when `CONFIG_PS` dynamic-PS on + idle | ✅ **Yes** — `CONFIG_PS` accepts ADHOC (§7 / the patch). The "light sleep" doze; ~42 µA, instant resume, deaf to peers while dozing. |
| **Deep sleep** | `STANDBY_MODE` (0x31) + WNM long-sleep (0x21) | ❌ **Effectively no** — see below. |
| **Hibernate** | `mmhal_wlan_hard_reset()` (full power-down) | ⚠️ Only as full power-off = **cold boot** (= the `RESET_N` cycle Rimba already plans). ~0.05 µA but pays firmware reload on wake. |

### Why Deep sleep is locked to infrastructure mode (traced)

- **WNM long-sleep (0x21)** is firmware hard-gated `interface_type == STA(1)`.
- **STANDBY (0x31)**: the firmware handler `standby_mode_handle` (0x1324f8) is *not*
  per-vif iftype-gated and ENTER **does arm a timer wake** (`FUN_0012216c`,
  duration or absolute vs `get_tsf64`) — i.e. the ~1 µA timer-wake state is a real
  built-in. BUT the standby *subsystem* is pervasively STA-watches-AP: the host
  `umac_offload_standby_enter` uses the STA connection's vif + the AP BSSID to
  monitor; and in firmware the standby state's vif is maintained from a beacon/RX
  path (`FUN_00132cec` ← frame handler `FUN_0012375c`), with a link-state check
  that posts a connection-lost event (`FUN_001452d8` → evt 0x1025) when the link
  isn't up. So it expects an AP to monitor.

### Consequences for Rimba

- The **only chip-managed low-power state usable in IBSS is Snooze (~42 µA)** —
  too high to be a leaf's average on its own.
- The attractive middle tier — **Deep sleep ~1 µA, timer wake, no cold boot** — is
  STA-only via the supported paths. A *forced* `STANDBY_MODE(ENTER)+timer` on the
  IBSS vif is **not flatly forbidden** (command not iftype-gated, timer-wake
  exists), but standby's firmware logic is infra-shaped, so it's a research-grade
  effort with real risk (waiting for AP beacons that never come, spurious
  connection-loss), not a turnkey patch. Flagged as a stretch lead, unverified.
- **Sub-µA in IBSS therefore requires full power-off** (Hibernate/`RESET_N`
  ≈0.05 µA) + RTC wake + **cold boot** — exactly the RTC-scheduled design. The
  floor (0.05 µA) is achievable; the cost is the cold-boot (RISK-02). There is no
  lighter fast-resume state available in IBSS. (Correction to an earlier note:
  Hibernate is "power off" → it needs a firmware reload; it does *not* dodge the
  cold boot.)

Datasheet sources: MM6108-MF08651-US Data Sheet (power tables), MM6108 Data Sheet
v4, MM6108 Product Brief — all at morsemicro.com.
