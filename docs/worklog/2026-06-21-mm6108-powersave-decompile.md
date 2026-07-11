# Worklog — 2026-06-21 — MM6108 firmware decompile + IBSS power-save analysis

**Author:** Aldwin
**Phase:** 1 — IBSS foundation (power-save de-risking, ahead of Phase 4 leaf sleep)
**Goal:** answer "the MM6108 can sleep — is IBSS power-save blocked by firmware or
just the host driver?", and rebaseline the power/battery docs on real datasheet
numbers.
**Status:** analysis complete; one host patch produced; docs rebaselined. No
on-air work this session (pure static analysis + docs). Bench measurements
(idle-RX current, cold-boot time) still owed.

This entry is a static-analysis + documentation record. It does not rewrite any
prior worklog; it captures new findings and the doc edits made from them.

---

## 1. MM6108 firmware decompilation (the enabling work)

Decompiled the vendored `mm6108.mbin` (firmware `rel_1_17_6`, 399,468 B) to
answer the power-save question from the chip side.

- **Container:** Morse **MBIN TLV** stream (`mbin.h`/`firmware_mbin.c`) — `MMFW`
  magic, raw `FW_SEGMENT` TLVs, `EOF`. **Not compressed, not encrypted, not
  signed** (ends `FIELD_TYPE_EOF`, not `EOF_WITH_SIGNATURE`).
- **CPU:** **RISC-V RV32IMC** (`csrrs mhartid`, `mtval`, clean boot vector).
- **Memory map** (walked from the TLV chain): code RAM `0x00100000`,`0x120000`,
  `0x150000`,`0x158000`,`0x1f0000`; data RAM `0x80100000`+; peripherals
  `0x10000000` (not in file). BCF loads at `0x8011fa80`.
- **Tooling:** Ghidra 12.1.2 headless on **chronium** (the Pi-5 Linux node;
  aarch64). The decompiler native isn't shipped for ARM, so it was **built from
  source** (`ghidra_opt`, ARCH_TYPE override). **2114/2114 functions decompiled.**
- **Artifacts (workspace root `mm6108_re/`, outside the repo):**
  `mm6108_decompiled.c` (raw), `mm6108_annotated.c` (whole program, ~110 renamed
  + plate comments on all 2114 + named globals/enums), `mm6108_ibss_powersave.c`
  (157-function IBSS/PS slice + curated cluster), `ibss-powersave.patch`,
  `segments/`, `ghidra_scripts/`, Ghidra project on chronium
  (`~/Developments/mm6108_re/ghidra_proj`).

## 2. Core finding — the AP/STA gate is host policy, not firmware

Full analysis: [`../design-specification/rimba-mm6108-powersave-analysis.md`](../design-specification/rimba-mm6108-powersave-analysis.md).

- The command sender `mmdrv_set_chip_power_save_enabled(vif_id,en)` is
  **interface-agnostic**; `CONFIG_PS`'s wire struct has no iftype field.
- The whole gate is host policy in `umac/ps/umac_ps.c::umac_ps_update()`: it looks
  up a **STA** vif and, finding none on an IBSS-only node, logs *"No STA
  interface. PS disabled"* and returns — so `CONFIG_PS` is never even sent.
- **Firmware decompile confirms the `CONFIG_PS` (0x16) handler ACCEPTS
  interface_type ADHOC(4)** (grouped with AP/MESH; writes the same global PS
  context as STA). The only firmware rule: enabling needs `dynamic_ps_offload!=0`,
  which the host helper already sets.

So **chip power-save in IBSS is not firmware-gated** — it's a host-side policy
that a small patch lifts.

## 3. The wake story (why it's leaf-sender-only)

Dynamic PS = the datasheet **Snooze** tier. A dozing node wakes on its **own TX**
(host asserts the WAKE pin via the bus-PS wakers → `morse_ps_wakeup`) and its own
beacon TBTT, but **cannot wake for unsolicited peer traffic** in IBSS — the
downlink-wake path is 100% AP-dependent (PS-Poll / beacon-TIM / listen-interval),
and the firmware has **no ATIM** and doesn't surface peer beacons. Good for a
send-mostly leaf, broken for a receiver.

## 4. Datasheet sleep modes + IBSS reachability

From the public MM6108-MF08651-US datasheet (Table 6):

| Mode | typ | IBSS-reachable | role |
|---|---|---|---|
| Snooze | 42 µA | ✓ via `CONFIG_PS` patch | leaf-unsuitable (~2.9 mA in-cell) |
| Deep sleep | 1 µA | ✗ STA-only (STANDBY/WNM firmware-gated) | — |
| Hibernate | 0.05 µA | ✓ via `RESET_N` (= cold boot) | RTC-scheduled leaf / relay sleep slots |

Traced `standby_mode_handle` (0x1324f8): it *does* arm a timer wake and isn't
hard iftype-gated, but the standby subsystem is pervasively STA-watches-AP
(vif bound from a beacon-RX path; connection-loss event on link-down). So a
forced timer-wake Deep-sleep on ADHOC is a research-grade stretch, not turnkey.

**ATIM: closed — not available.** No ATIM anywhere in fw 1.17.6 (only mesh TBTT).
Implementing it = lower-MAC firmware work; not feasible by patching.
Open Issue #6 marked CLOSED in the development plan.

## 5. The patch (host-side, leaf-sender only)

`mm6108_re/ibss-powersave.patch` — `umac_ps.c` falls back to the **ADHOC** vif
when no STA vif exists, so `CONFIG_PS` (Snooze) can be armed in IBSS. Validated:
applies clean (`patch -p1`), symbols present. **Warning baked in:** uncoordinated
dynamic PS, deaf to peers while dozing — leaf-sender only; relays must not enable.
Not on the critical path (RTC design doesn't need it).

## 6. Power/battery rebaseline (datasheet currents)

Discovered the docs used a **12 mA** idle-RX assumption; datasheet "Listen" at
1 MHz is **26 mA**. Rebaselined [`../design-specification/rimba-battery-analysis.md`](../design-specification/rimba-battery-analysis.md)
to 26 mA throughout (v1.1):

- Relay **Continuous ≈ 28 mA** (was 14), **Scheduled K=6 ≈ 14.3 mA** (was 7.2).
- Battery-only life: Continuous **~1 wk**, Scheduled **~2 wk** (were ~2 wk / ~4 wk).
- Solar: recommended panel **3 W Continuous / 2 W Scheduled** (were 2 W / 1 W);
  self-sustain thresholds 1.46 / 0.75 h-sun/day.
- Hibernate corrected 0.1 mA → **0.05 µA** (favorable): leaf sleep floor is now
  **ESP32-bound** (~10–20 µA), radio negligible. Leaf life **unchanged**
  (sleep-dominated) — ~17 yr radio+MCU at 15 min.
- New §3A sleep-mode viability matrix; Snooze-as-leaf shown non-viable (~2.9 mA).

## 7. Docs updated this session

- **`design-specification/rimba-mm6108-powersave-analysis.md`** — NEW; the source-of-truth analysis
  (§1–§9: gate, wake, decompile evidence, datasheet modes, standby trace).
- **`design-specification/rimba-battery-analysis.md`** — rebaselined to 26 mA (v1.1); sleep-mode
  matrix; SPEC-UPDATE-PENDING note.
- **`rimba-development-plan.md`** — relay figures (~28/~14.3 mA); task 1.12a +
  RISK-01 fallback notes (firmware-confirmed no IBSS PS); Phase-5 relay-power
  criterion revised; Open Issues #5 (approach resolved) / #6 (CLOSED); §7
  SPEC-UPDATE-PENDING note.
- **`design-specification/rimba-mesh-comparison.md`**, **`design-specification/rimba-mesh-topology.md`** — relay current
  reconciled to the 26 mA baseline (density conclusions unchanged).
- **Intentionally untouched:** `halow-mesh-dtn-spec.md` (superseded), prior
  worklogs (history), `design-specification/rimba-protocol-spec.md` (flagged SPEC UPDATE PENDING —
  deferred until bench numbers).

## 8. Open items / next steps

1. **Measure idle-RX current at 1 MHz** on hardware — the single number that
   decides which relay column (12 vs 26 mA) is real; drives panel BOM.
2. **Measure RISK-02 cold-boot-to-IBSS-joined time** — gates RTC-scheduled mode
   efficiency and the leaf active-phase energy.
3. (Optional) bench the `CONFIG_PS`-on-IBSS patch — confirm the radio physically
   dozes (radio-rail current / BUSY pin) for the leaf-sender case.
4. **Update `design-specification/rimba-protocol-spec.md`** (Section 12/13 power, §15 open issues)
   once 1–2 are measured.

Caveat on all absolute power numbers: they rest on the datasheet 26 mA idle-RX
until (1) confirms it.
