# Rimba Protocol — Development Plan
## Draft 0.5

*Companion to: rimba-protocol-spec.md Draft 0.26*

*Last updated 2026-06-21: RISK-01 resolved + Phase-1 IBSS foundation validated; RISK-02 boot time measured (≈1.39 s); Wio-WM6180/FGH100M hardware baseline + schematic findings (no power-enable line; BUSY/WAKE DNP) folded in.*

---

## Table of Contents

1. [Overview](#1-overview)
2. [Prototype Hardware BOM](#2-prototype-hardware-bom)
3. [Risk Register](#3-risk-register)
4. [Development Phases](#4-development-phases)
5. [Validation Criteria](#5-validation-criteria)
6. [SDK and Library Dependencies](#6-sdk-and-library-dependencies)
7. [Known Open Issues from Spec](#7-known-open-issues-from-spec)

---

## 1. Overview

This document covers the practical implementation plan for Rimba on ESP32-S3 + MM6108. It is a companion to the Rimba Protocol Specification (rimba-protocol-spec.md) and addresses implementation risks, development sequence, and validation criteria.

**Target platform**: Espressif ESP32-S3 + Morse Micro MM6108 (802.11ah)  
**SDK**: morselib (Apache 2.0, open source, mm-iot-sdk)  
**Reference hardware**: Seeed Xiao ESP32-S3 + Seeed HaLow (MM6108 module)  
**Estimated prototype timeline**: 3–4 months (1–2 engineers)

**Critical path**: IBSS API → IBSS boot time measurement → BPv7 implementation. All three must succeed before the protocol can function end-to-end. **(2026-06-21: IBSS API resolved + foundation validated; IBSS cold-boot time measured ≈1.39 s — see RISK-01/RISK-02. Critical path now clears to BPv7.)**

---

## 2. Prototype Hardware BOM

Minimum hardware needed to begin development. All items available off-the-shelf.

| Item | Purpose | Quantity | Est. cost |
|---|---|---|---|
| Seeed Xiao ESP32-S3 | Host MCU — ESP32-S3, PSRAM, USB | 6 | ~$6 each |
| Seeed HaLow (MM6108) module | 802.11ah IBSS radio | 6 | ~$15 each |
| RV-3028-C7 RTC breakout | Mandatory 1 ppm RTC | 6 | ~$5 each |
| Logic analyser (e.g. Saleae Logic 8) | SPI/I2C/UART debugging | 1 | ~$120 |
| USB power meters × 2 | Leaf and relay current measurement | 2 | ~$20 each |
| Bench power supply | Stable 3.3V for power measurement | 1 | ~$50 |
| Jumper wires, breadboards | Prototyping | — | ~$20 |

**Total prototype cost**: ~$440

**Note on BCF files**: The Seeed HaLow module ships with a vendor-supplied BCF (Board Configuration File) for regulatory compliance. Do not substitute BCF files from other modules. For custom PCB designs, obtain BCF from Morse Micro or a certified module partner before production.

**Hardware baseline pinned (2026-06-21)**: the validated module is the **Seeed Wio-WM6180 Wi-Fi HaLow for XIAO** (Quectel **FGH100M** / MM6108) → build `BOARD=proto1-fgh100m`, BCF **`bcf_fgh100mhaamd.mbin`**. (The repo also has `proto1` → `bcf_mf16858.mbin` for the Seeed mf16858 module — *don't* flash that BCF onto FGH100M hardware; same XIAO wiring, different calibration.) **Schematic finding** (`WI-FI_HALOW_FGH100M_EXT01_V30`): the board has **no host power-enable / load switch** — the MM6108 is rail-fed via ferrite beads, so radio power-cycling is via **`RESET_N` (GPIO1)**, not a VBAT cut. Reaching the datasheet sub-µA Hibernate floor may therefore require adding a **load switch** (hardware mod). **BUSY (GPIO5)** and **WAKEUP_IN (GPIO2)** are on **DNP footprints** (populate 0 Ω to enable). This matters for the USB power-meter measurements below.

---

## 3. Risk Register

Risks are ordered by priority. Items marked **BLOCKING** must be resolved before dependent phases can begin.

**Risk-to-phase traceability:**

| Risk | Priority | Resolved in | Key task |
|---|---|---|---|
| RISK-01 — IBSS API | ✅ RESOLVED (2026-06-20) | Phase 1 | 1.2–1.3 (`mmwlan_ibss_*`); foundation validated (multi-node, Linux interop, ~6.5 h soak) |
| RISK-02 — IBSS boot time | ✅ MEASURED (2026-06-21) | Phase 1 | 1.4 — cold-boot-to-joined **≈1.39 s** (not 30 ms); absorbed by RTC-scheduled design (1.12a), gates duty cycle not feasibility |
| RISK-03 — BPv7 implementation | BLOCKING | Phase 4 | 4.1–4.4 (RFC 9171 subset) |
| RISK-04 — Memory without PSRAM | MEDIUM | Phase 1 | 1.8 (validate PSRAM) |
| RISK-05 — Adaptive TX power API | LOW | Phase 5 | 5.3 (adaptive TX power) |
| RISK-06 — morselib SDK stability | LOW | Phase 2 | Pin SDK version at first sustained use |

---

### RISK-01 — IBSS API: No public `mmwlan_ibss_enable()` **[BLOCKING]**

**Probability**: Certain (the API does not exist)  
**Impact**: Critical — entire protocol depends on IBSS mode  
**Owner**: First engineering task

**Description**: morselib exposes `MORSE_CMD_INTERFACE_TYPE_ADHOC` in `morse_commands.h` and a community member confirmed IBSS working on ESP32 (May 2026), but no public `mmwlan_ibss_enable()` API exists. The init sequence must be reverse-engineered from morselib source.

**Mitigation**:
1. Clone mm-iot-sdk: `git clone https://github.com/MorseMicro/mm-iot-sdk`
2. Read `morselib/src/mmwlan_sta.c` and `morselib/src/mmwlan_softap.c` to understand init patterns
3. Trace how `MORSE_CMD_INTERFACE_TYPE_AP` and `MORSE_CMD_INTERFACE_TYPE_STA` are used
4. Replicate the pattern substituting `MORSE_CMD_INTERFACE_TYPE_ADHOC`
5. Write `rimba_ibss_init()` wrapper, test on two boards

**Success signal**: Two boards exchange raw Ethernet frames (EtherType `0x88B5`) in IBSS mode.  
**Fallback**: If morselib IBSS is truly broken, file an issue with Morse Micro and request documentation. The community success in May 2026 suggests it works.

#### RISK-01 Fallback Strategy — What If IBSS Doesn't Work

"Doesn't work" has three degrees, with very different responses. The key
reassurance: **only the L2 link/discovery layer depends on IBSS. Everything
above L2 — DTN/BPv7 bundles, custody, mules, OGM/RREQ routing, OTA, config
convergence, encryption — is link-agnostic and survives any of these fallbacks.**
IBSS failure is a link-layer setback, not a whole-project failure.

```
Degree 1 — IBSS init quirks / slow boot (ANNOYING, not fatal):
  IBSS forms but boot is too slow for Scheduled mode, or association flaky.
  → Fallback: drop Scheduled mode, run relays in Continuous mode only
    (wake on MM6108 interrupt). Power penalty (~28 mA vs ~14.3 mA¹) on
    always-on relays; leaves still wake on RTC. Already specced — just
    don't use Scheduled. This is the path tasks 1.4/1.12 gate.
    ¹ Datasheet idle-RX baseline (26 mA Listen, 1 MHz); see
    rimba-battery-analysis.md. Was ~14/~7.2 mA at the old 12 mA estimate —
    confirm idle-RX on hardware.

Degree 2 — IBSS works but not as Rimba assumes (design adjustments):
  No ATIM doze window (Open Issue #6 — now CONFIRMED absent in mm6108 fw
  1.17.6 by decompilation; see rimba-mm6108-powersave-analysis.md §9), or too
  few peers per cell.
  → No ATIM: relays use Continuous mode (as Degree 1).
  → Few peers: cap cluster size, add relays, lean on mules to bridge
    smaller clusters. Topology tuning, not protocol change. DTN/custody/
    mule core untouched.

Degree 3 — no usable ad-hoc mode at all (the real threat):
  morselib only supports AP/STA (infrastructure), no peer-to-peer ADHOC.
  → Rimba's REAL requirement is "two battery nodes exchange L2 frames
    without fixed infrastructure" — IBSS is just the chosen mechanism.
    Swap the mechanism, keep the architecture. Options in order:

    Fallback A — AP-STA mesh (MOST LIKELY real fallback):
      Relays run as APs; leaves associate as STAs. Relay-to-relay: one
      runs a STA interface alongside its AP role, or use 802.11s for the
      backbone.
      + Uses the supported mode; HaLow AP supports many STAs + TWT, which
        gives BETTER leaf power management than IBSS (TWT unavailable in
        IBSS).
      − More complex role assignment; AP is a per-cluster single point;
        relay-to-relay peering needs a design decision.
      Most of Rimba sits above L2 and survives — rewrite the link layer,
      keep the rest.
      Porting status (if this fallback is taken): AP mode + an AP **TWT
      responder** (the leaf power-save win) are now ported to ESP32/morselib
      and verified on stock fw 1.17.6; 802.11s mesh for the backbone is NOT
      ported (Linux-only). Details + the new-code↔Linux comparison tables:
      docs/mesh-ap/rimba-mesh-ap-milestones.md.

    Fallback B — different sub-GHz PHY (big change, core survives):
      Port the DTN+mule+routing stack onto LoRa (cf. TU Darmstadt
      BPv7-over-LoRa). Lose HaLow bandwidth, keep Rimba's actual novelty.

    Fallback C — 802.11s mesh mode:
      If morselib supports 802.11s, use it as the L2 mesh substrate.
      Less control, but handles peering/forwarding.
```

**De-risking sequence (why Phase 1 is BLOCKING):** Phase 1 exists to find out
which degree you're in, on two boards, before building anything on top. Degree 1
→ proceed (drop Scheduled mode). Degree 2 → proceed (Continuous relays +
topology tuning). Degree 3 → STOP and re-plan the L2 layer onto AP-STA
(Fallback A) before continuing — roughly weeks of link-layer rework, but the
upper-layer design is intact.

#### RISK-01 — HELLO and Discovery Under AP-STA (if Fallback A is taken)

If the AP-STA fallback is taken, the HELLO frame changes role but **stays at
L2** (still a Rimba `0x88B5` data frame carrying Rimba-layer state, NOT a BPv7
bundle). Two things shift:

```
1. HELLO's discovery role shrinks — 802.11 already does it.
   AP-STA infrastructure mode provides, as L2 management frames:
     Beacon (AP→broadcast), Probe Req/Resp, Association Req/Resp.
   So "who exists / who's alive" for the AP-STA link is handled by the
   802.11 MAC. A relay-AP knows its leaves from the association table; a
   leaf-STA knows its relay from the association.
   → HELLO sheds the pure-discovery parts and becomes the carrier of
     RIMBA state that 802.11 management frames don't know about:
     role, sw_version, config_version+hash, store_pct, MULE flag +
     storage_kb, node_pub_key + cert, ota_pending, position.

2. HELLO's broadcast model fragments — "everyone hears one broadcast" is gone.
   IBSS: one broadcast (dst FF:FF:FF:FF:FF:FF), all peers hear it. Uniform.
   AP-STA:
     - AP→its STAs: broadcast within the BSS (STAs hear it)
     - STA→AP: unicast to the AP
     - AP↔AP: needs a peering link FIRST (one acts as STA, or 802.11s
       backbone) before HELLO can flow — this is the new relay-to-relay
       complexity AP-STA introduces, exactly what IBSS gave for free.
```

The relay-to-relay case is where AP-STA costs the most: IBSS's symmetric-peer
model is what made relay-to-relay HELLO, OGM flooding, and the whole backbone
simple. Under AP-STA the open design decision is how two relays peer —
STA-alongside-AP, or 802.11s mesh for the backbone while keeping AP-STA for leaf
attachment. This decision should be made up front if Phase 1 forces the AP-STA
pivot.

---

### RISK-02 — MM6108 IBSS Boot Time **[MEASURED ✅ 2026-06-21 — ≈1.39 s]**

**Probability**: Target missed (measured ~46× the 30 ms assumption)  
**Impact**: High — sets the leaf duty cycle and power budget (but does **not** block the design)  
**Owner**: Measure on first hardware bring-up — **done**

**Description**: The spec assumes `LEAF_BOOT_MS = 30ms` (MM6108 IBSS join time from power-on). This had never been measured. Typical 802.11 chip boot times range from 50ms to 400ms depending on mode and firmware. If the actual time exceeds 30ms, the leaf scan window must widen, increasing average power.

> **RESULT (2026-06-21) — cold-boot-to-IBSS-joined ≈ 1.39 s.** Seeed Wio-WM6180
> (FGH100M / MM6108) for XIAO, `BOARD=proto1-fgh100m`, fw 1.17.6. 20-cycle
> in-firmware loop (`mmwlan_boot` → `mmwlan_ibss_start` → `mmwlan_ibss_stop` →
> `mmwlan_shutdown`, timed with `esp_timer`):
>
> | Phase | mean (min–max) |
> |---|---|
> | `mmwlan_boot` — firmware `.mbin` load over SPI @40 MHz + chip init | **616 ms** (599–631) |
> | `mmwlan_ibss_start` — ADD ADHOC vif + cfg_ibss JOIN + start beaconing | **772 ms** (740–805) |
> | **TOTAL cold-boot-to-joined** | **≈1389 ms** (1349–1425) |
>
> **20/20 cycles, 0 failures, ±40 ms** — reliable and tightly bounded. (An earlier
> loop that skipped `mmwlan_ibss_stop` before `shutdown` hit `MMWLAN_UNAVAILABLE`
> re-boots — a teardown-ordering bug, not a hardware limit. Fixed.)
>
> **Interpretation:** lands in the ">200 ms" row below — the 30 ms `LEAF_BOOT_MS`
> assumption is obsolete for an MM6108 IBSS cold boot. **This does not block Rimba.**
> The RTC-scheduled design (task 1.12a, Issue #5) already plans a full radio
> power-off + cold boot per wake, so ≈1.39 s is the **per-wake "rejoin tax"** that
> sets the achievable duty cycle — *not* a per-frame latency. At infrequent leaf
> wakes (e.g. every 10 min) it is a sub-percent active-time tax. The number to keep
> reducing it is the `mmwlan_ibss_start` portion (772 ms of fixed bring-up).
>
> **Hardware mechanism (Wio-WM6180 schematic `WI-FI_HALOW_FGH100M_EXT01_V30`,
> checked 2026-06-21):** the board has **no host power-enable / load switch** — the
> MM6108 is fed from the XIAO 3V3/5V rails through ferrite beads. So "power-cycle the
> radio" = drive **`RESET_N` (GPIO1)**, *not* cut VBAT. Whether `RESET_N`-held
> reaches the datasheet Hibernate floor (~0.05 µA) or a higher reset current is
> **unmeasured** (no current rig this round) — if too high, reaching sub-µA needs a
> **load-switch hardware mod**. The board's **BUSY (GPIO5)** and **WAKEUP_IN (GPIO2)**
> sit on **DNP footprints**; a planned 0 Ω mod (all 3 boards) wires them to (a)
> tighten the boot handshake and (b) enable evaluating chip-PS/Snooze. The firmware
> pin map (`boards/proto1-fgh100m`, SPI2_HOST @40 MHz mode 0, sw-CS) **matches the
> schematic exactly** — verified pin-by-pin.

**Measurement procedure** (as run; the original GPIO/sniffer method is unnecessary — the host SDK timing is authoritative and the firmware doesn't surface peer beacons anyway, see Issue #6 / I.4):
```
In-firmware loop, timed with esp_timer_get_time():
  mmwlan_boot()       -> t_boot   (firmware reload + chip init)
  mmwlan_ibss_start() -> t_ibss   (vif up + beaconing = joined)
  [hold joined], mmwlan_ibss_stop(), mmwlan_shutdown()  (clean teardown)
Repeat 20×, report min/mean/max per phase.
```

**Impact of results**:

| Measured boot time | Action |
|---|---|
| < 30ms | Spec confirmed. No changes needed. |
| 30–80ms | Update `LEAF_BOOT_MS` in spec. Increase `LEAF_SCAN_WINDOW_MS` to 250ms. Minor power impact. |
| 80–200ms | Significant scan window increase. Leaf power may double. Recalculate power budget. Consider wake-on-interrupt from relay beacon as alternative. |
| > 200ms | Fundamental issue. Investigate MM6108 IBSS fast-start options in morselib source. May need to keep MM6108 in low-power standby (not full off) between leaf wakes. **← ACTUAL (≈1389 ms). Resolution: not a per-frame cost — it's the per-wake tax of RTC-scheduled mode (1.12a). `LEAF_BOOT_MS` → ~1400 ms; fold into the power budget; no fast-resume IBSS state exists (standby/deep-sleep are STA-only — see rimba-mm6108-powersave-analysis.md §9), so the cold boot is unavoidable for sub-µA sleep.** |

---

### RISK-03 — BPv7 Implementation: No ESP32 Library **[BLOCKING]**

**Probability**: Certain (no library exists)  
**Impact**: High — all data delivery depends on BPv7  
**Owner**: Phase 4 engineering task

**Description**: No BPv7 (RFC 9171) library exists for ESP32. Implementation required. Two viable options:

**Option A — Implement RFC 9171 required subset from scratch (recommended)**

Implement only what Rimba actually uses:
- Primary block (CBOR-encoded header)
- Payload block (CBOR-wrapped CBOR sensor data)
- BIB (Block Integrity Block) for BPSec authentication
- BCB (Block Confidentiality Block) for BPSec encryption
- `ipn:` endpoint scheme only

Skip (not needed for v1):
- Custody signals
- Status reports
- BPv7 fragmentation (handle at mesh layer instead)
- `dtn:` endpoint scheme

Estimated effort: 3–5 weeks. Produces a lean, embedded-optimised implementation with no POSIX dependency.

**Option B — Port ION-DTN (NASA/JPL)**

ION-DTN is designed for embedded environments including spacecraft flight computers. Now BPv7-only (RFC 9171). Available at `https://github.com/nasa-jpl/ION-DTN`.

Porting work: replace POSIX file I/O with LittleFS, replace POSIX sockets with Rimba mesh frame calls, trim unused convergence layers.  
Estimated effort: 5–8 weeks. Produces a more complete implementation but with more porting risk.

**Recommendation**: Option A. A Rimba-specific minimal implementation is faster to ship, easier to audit for security, and avoids POSIX porting complexity.

---

### RISK-04 — Memory Constraints Without PSRAM **[MEDIUM]**

**Probability**: Medium  
**Impact**: Medium — may require module change  
**Owner**: Validate in Phase 1

**Description**: ESP32-S3 has ~384 KB usable SRAM after FreeRTOS overhead. Rimba's components total an estimated 400–470 KB, which may exceed this without PSRAM.

**Memory estimate**:

| Component | Est. size |
|---|---|
| morselib binary + heap | ~200–300 KB |
| FreeRTOS + ESP-IDF base | ~80 KB |
| BPv7 implementation | ~30–50 KB code + 50 KB heap |
| Routing table (500 nodes) | ~8 KB |
| mbedTLS crypto context | ~40 KB |
| Bundle aggregation buffer | ~2 KB |
| Application + CBOR + misc | ~30 KB |
| **Total** | **~440–560 KB** |

**Mitigation**: Use ESP32-S3 module with external PSRAM (most Seeed Xiao HaLow and similar modules include 8 MB PSRAM). Assign large heap allocations (bundle store buffer, routing table) to PSRAM via `heap_caps_malloc(size, MALLOC_CAP_SPIRAM)`. Internal SRAM holds time-critical morselib and FreeRTOS structures.

---

### RISK-05 — Adaptive TX Power API in morselib **[LOW]**

**Probability**: Low (likely available but unconfirmed)  
**Impact**: Low — TX power adaptation affects dense deployment only  
**Owner**: Check morselib source early

**Description**: Rimba Section 13.3.1 requires per-node adaptive TX power control. The morselib API likely exposes a TX power setting function, but it needs to be confirmed.

**Mitigation**: Search morselib source for `tx_power` or `mmwlan_set_tx_power`. If available, implement Section 13.3.1 as specified. If absent, use fixed TX power initially and file a morselib feature request.

---

### RISK-06 — morselib SDK Updates Breaking IBSS **[LOW]**

**Probability**: Low  
**Impact**: Medium — SDK update could break the undocumented IBSS path  
**Owner**: Ongoing

**Description**: Because IBSS uses undocumented morselib internals, an SDK update could change the init sequence and break `rimba_ibss_init()`.

**Mitigation**: Pin development to a specific mm-iot-sdk git commit hash. Fork morselib if needed. Track Morse Micro's GitHub for changes to `morse_commands.h` and the ADHOC code path. Submit a PR upstream to formalise `mmwlan_ibss_enable()` as a public API.

---

## 4. Development Phases

### Phase 1 — IBSS Foundation (Weeks 1–3) **[BLOCKING]**

**Goal**: Prove two boards can exchange raw Rimba frames over 802.11ah IBSS.

**Risks addressed**: RISK-01 (IBSS API — resolved by tasks 1.2–1.3), RISK-02 (boot time — measured in task 1.4, gates Scheduled-mode feasibility in 1.12), RISK-04 (memory/PSRAM — validated in task 1.8).

**Tasks**:

| # | Task | Effort | Depends on |
|---|---|---|---|
| 1.1 | Set up ESP-IDF + morselib build for Seeed Xiao HaLow | 1 day | Hardware |
| 1.2 | Read morselib AP and STA init sequences | 2 days | morselib source |
| 1.3 | Write `rimba_ibss_init()` wrapper using ADHOC interface type | 3 days | 1.2 |
| 1.4 | ✅ **Measure MM6108 IBSS boot time** (RISK-02) — **DONE 2026-06-21: cold-boot-to-joined ≈1.39 s** (boot 616 ms + ibss_start 772 ms), 20/20 reliable. See RISK-02. | 1 day | 1.3 |
| 1.5 | Update `LEAF_BOOT_MS` and `LEAF_SCAN_WINDOW_MS` in spec | 1 day | 1.4 |
| 1.6 | Implement raw Ethernet frame TX/RX (EtherType 0x88B5) | 2 days | 1.3 |
| 1.7 | Prove two-node frame exchange (ping test) | 1 day | 1.6 |
| 1.8 | Validate PSRAM availability and memory allocation | 1 day | 1.1 |
| 1.9 | **Connect MM6108 interrupt line to ESP32-S3 wake pin** | 0.5 day | 1.3 |
| 1.10 | **Implement Continuous mode relay sleep (FreeRTOS light sleep + MM6108 interrupt wake)** | 1 day | 1.9 |
| 1.11 | **Verify Continuous mode: relay wakes on incoming frame, MCU sleeps between frames** | 1 day | 1.10 |
| 1.12 | **Assess Scheduled mode feasibility** — ⚠️ **revised (2026-06-21):** boot is ≈1.39 s, far over the old 100 ms TDMA-sleep threshold, so per-frame TDMA sleep is off the table. Scheduled mode is instead the **RTC-driven cold-power-cycle** in 1.12a (wake → cold-boot ≈1.39 s → exchange → off); 1.39 s is the per-wake tax, viable for infrequent (minutes-scale) wakes. | 1 day | 1.4 |
| 1.12a | **RTC-scheduled radio power-cycling = Scheduled mode** (early focus). The morse driver/firmware has **no IBSS radio power-save** (TWT is STA/AP-only — now **firmware-confirmed by decompilation**: TWT/WNM-sleep/Deep-sleep are STA-gated, ATIM is absent; only *Snooze* via `CONFIG_PS` is reachable in IBSS but is leaf-unsuitable — see [`rimba-mm6108-powersave-analysis.md`](rimba-mm6108-powersave-analysis.md) §7/§9), so drive the duty cycle from the **ESP32 + RTC**: wake on the RTC alarm → power the MM6108 on → join the pinned cell → exchange → power off → deep-sleep. Bypasses chip power-save *and* TSF sync (RTC is the clock). **Gating measurement = radio cold-boot-to-IBSS-joined time (RISK-02, task 1.4)** — that sets the viable wake period + power budget. Runs after the small Phase-1 validation. See [`rimba-ibss-milestones.md`](ibss/rimba-ibss-milestones.md) TODO #8/#9. | 2–3 days | 1.4 |
| 1.13 | **Implement NTP dev-mode time sync** (Phase 1 only — replaced by RTC in Phase 4) | 1 day | 1.1 |

**Phase 1 success gate**: Two boards exchange `EtherType 0x88B5` frames over IBSS. Boot time measured and spec updated. Continuous mode relay sleep confirmed working. NTP dev-mode providing absolute time to both boards.

> **Phase 1 status (2026-06-21) — IBSS foundation VALIDATED, exceeded the 2-board gate.**
> - **RISK-01 resolved & hardened:** `mmwlan_ibss_*` API (adopted from the
>   momentary-systems fork + our Linux-derived fixes). Validated well beyond two
>   boards: **3-board full mesh, per-peer state, drop/rejoin, Linux `morse_driver`
>   interop, and a ~6.5 h 4-node soak** (0 reboots, no heap leak). See
>   [`rimba-ibss-milestones.md`](ibss/rimba-ibss-milestones.md),
>   [`rimba-ibss-test-plan.md`](ibss/rimba-ibss-test-plan.md).
> - **RISK-02 measured:** cold-boot-to-IBSS-joined **≈1.39 s** (task 1.4). The 30 ms
>   `LEAF_BOOT_MS` assumption is retired; ≈1.4 s is the RTC-wake tax (1.12a).
> - **Beacon/discovery (I.4 / Issue #6):** the MM6108 firmware does **not surface
>   peer beacons** to the host, and an S1G beacon carries `source_addr = BSSID` by
>   frame format — so peer discovery is **data-driven** on both ESP32 and Linux
>   (matches mac80211). On-air beacon capture needs an external sniffer; not a
>   functional blocker. (The ESP32 also builds a *legacy* MGMT beacon host-side vs
>   Linux's software S1G translation — a noted divergence, not a defect.)
> - **Hardware baseline pinned:** Seeed Wio-WM6180 (FGH100M / MM6108) for XIAO =
>   `BOARD=proto1-fgh100m` (BCF `bcf_fgh100mhaamd.mbin`); schematic-verified pin map.
> - **Remaining Phase-1 items:** the RTC-scheduled power-cycle prototype (1.12a) and
>   Continuous-mode sleep (1.9–1.11); next concrete step is the BUSY/WAKE 0 Ω mod +
>   re-measuring boot before/after, then radio-rail current (needs a power rig).

### Development Mode: External RTC is not required for Phase 1–3

During Phase 1–3, the external RTC (RV-3028-C7) can be omitted. Instead:

```
Time source:   ESP32-S3 built-in 2.4 GHz WiFi → NTP (pool.ntp.org)
               Sync on boot, disconnect WiFi before starting MM6108.
               ESP32-S3 internal RTC (150 ppm) maintains time during session.

Dev config:    RELAY_DRIFT_RATE_PPM = 150
               TIMESYNC_STALE_S = 3600 (1 hour, resync often)
               Max leaves per relay drops to ~85 (K=4) — fine for dev mesh

Implication:   ESP32-S3 internal 2.4 GHz WiFi and MM6108 HaLow are
               completely independent RF paths — no conflict.
               NTP runs on internal WiFi; Rimba runs on MM6108 HaLow.

Add RTC before: Phase 4 (leaf wake scheduling) — accurate timing needed
                for 15-minute sleep cycles and power measurement.
```

See rimba-rtc-comparison.md for full NTP dev-mode implementation details.

---

### Phase 2 — Peer Links and Link Security (Weeks 3–5)

**Goal**: Two nodes discover each other via HELLO and establish an encrypted peer link.

**Risks addressed**: RISK-06 (morselib SDK stability — first sustained exposure to the IBSS API surface; pin SDK version here). No new blocking risks; this phase builds on Phase 1's resolved IBSS foundation.

**Tasks**:

| # | Task | Effort | Depends on |
|---|---|---|---|
| 2.1 | Implement HELLO frame encoding/decoding (CBOR via TinyCBOR) | 3 days | 1.7 |
| 2.2 | Implement HELLO broadcast and neighbour table | 2 days | 2.1 |
| 2.3 | Implement X25519 ECDH key exchange (mbedTLS) | 2 days | — |
| 2.4 | Implement HKDF session key derivation (mbedTLS) | 1 day | 2.3 |
| 2.5 | Implement PEER_OPEN / PEER_CONFIRM exchange | 3 days | 2.2, 2.4 |
| 2.6 | Implement AES-128-CCM frame encryption/decryption | 2 days | 2.4 |
| 2.7 | Integrate hop-by-hop encryption into DATA frame TX/RX | 2 days | 2.5, 2.6 |
| 2.8 | Implement peer link key storage in RTC memory | 1 day | 2.5 |
| 2.9 | Verify key resumption across sleep cycles | 1 day | 2.8 |

**Phase 2 success gate**: Two nodes auto-discover, perform ECDH, and exchange AES-encrypted DATA frames. Key persists across deep sleep.

---

### Phase 3 — Routing and Mesh Forwarding (Weeks 5–9)

**Goal**: A 5-node mesh routes bundles end-to-end via OGM and RREQ/RREP.

**Risks addressed**: No register risks directly, but this phase validates the RREQ storm behaviour underlying Open Issue #4 (RREQ Storm Validation) at small scale (gossip in task 3.9, adaptive jitter in 3.10). Full adversarial/scale validation deferred to Phase 5 and the hardening plan.

**Tasks**:

| # | Task | Effort | Depends on |
|---|---|---|---|
| 3.1 | Implement OGM frame encoding/decoding | 2 days | Phase 2 |
| 3.2 | Implement OGM flood with Trickle suppression | 3 days | 3.1 |
| 3.3 | Implement forwarding table from OGM | 2 days | 3.2 |
| 3.4 | Implement DATA frame forwarding (mesh header, TTL) | 2 days | 3.3 |
| 3.5 | Verify 3-hop OGM routing on 3-node bench | 2 days | 3.4 |
| 3.6 | Implement RREQ frame + expanding ring search | 3 days | 3.4 |
| 3.7 | Implement RREP unicast response | 2 days | 3.6 |
| 3.8 | Implement RERR + DTN retry trigger | 2 days | 3.7 |
| 3.9 | Implement gossip probability forwarding | 1 day | 3.6 |
| 3.10 | Implement adaptive RREQ jitter | 1 day | 3.9 |
| 3.11 | Verify 5-node mesh: RREQ discovers route, DATA follows | 2 days | 3.8 |
| 3.12 | Implement connectivity state machine (ISOLATED/MESH_ONLY/CONNECTED) | 2 days | 3.11 |
| 3.13 | Implement dynamic routing mode switching (OGM ↔ RREQ/RREP) | 2 days | 3.12 |

**Phase 3 success gate**: 5-node mesh routes a DATA frame end-to-end via both OGM and RREQ/RREP. RERR triggers re-routing when a node is removed.

---

### Phase 4 — DTN Bundle Layer (Weeks 9–14)

**Goal**: Nodes store, carry, and forward BPv7 bundles. Leaf sleep cycle works end-to-end.

**Risks addressed**: RISK-03 (BPv7 implementation — resolved by tasks 4.1–4.4, the from-scratch RFC 9171 subset). Also resolves Open Issue #5 (leaf power without TWT — measured in task 4.12) and validates Scheduled-mode timing dependency on RISK-02 (RTC integration before task 4.9).

**Time source note**: Tasks 4.1–4.8, 4.13–4.15 only need wall-clock time — use NTP once on boot via ESP32-S3 internal WiFi. **The RV-3028-C7 must be wired and its driver implemented before starting task 4.9** (leaf wake scheduling). From task 4.9 onward, the external RTC provides both accurate wall-clock time and the alarm interrupt used to wake the leaf from deep sleep. See rimba-rtc-comparison.md for wiring and driver details.

**Tasks**:

| # | Task | Effort | Depends on |
|---|---|---|---|
| 4.1 | Implement BPv7 primary block encoder/decoder (CBOR) | 5 days | — |
| 4.2 | Implement BPv7 payload block | 2 days | 4.1 |
| 4.3 | Implement BPSec BCB (confidentiality) with COSE AEAD | 4 days | 4.1 |
| 4.4 | Implement BPSec BIB (integrity) | 2 days | 4.3 |
| 4.5 | Implement bundle store on LittleFS (256 KB partition) | 3 days | 4.2 |
| 4.6 | Implement bundle lifetime enforcement (NTP time for 4.1–4.8; RTC from 4.9) | 1 day | 4.5 |
| 4.7 | Implement bundle store eviction policy (STORE_EVICT_PCT) | 2 days | 4.6 |
| 4.8 | Integrate BPv7 with mesh routing (receive → store → forward) | 3 days | Phase 3, 4.5 |
| 4.13 | Implement mule custody transfer (CUSTODY_REQ/ACK protocol) | 4 days | 4.8 |
| 4.14 | Implement gateway anycast routing (GATEWAY_ANYCAST_ID) | 2 days | Phase 3 |
| 4.15 | Implement TIME_SYNC event-driven sending and RTC update | 2 days | 4.9 |
| — | **Add RV-3028-C7 hardware** (wire I2C + INT pin, implement driver) | 2 days | before 4.9 |
| 4.9 | Implement leaf wake scheduling using RTC alarm interrupt | 3 days | RTC added |
| 4.10 | Implement relay leaf schedule tracking + drift window | 3 days | 4.9 |
| 4.11 | Implement leaf deep sleep + RTC alarm wakeup | 2 days | 4.9 |
| 4.12 | **Measure leaf average current in full sleep cycle** | 2 days | 4.11 |

**Phase 4 success gate**: Leaf wakes on RTC alarm schedule, sends BPv7 bundle encrypted with BPSec, relay stores it when no route, forwards it when route available. Measured leaf power within 2× of spec prediction (~20 µA target).

---

### Phase 5 — Full Integration and Validation (Weeks 14–18)

**Goal**: Full 10-node protocol integration, power validation, and scale testing.

**Risks addressed**: RISK-05 (adaptive TX power API — resolved by task 5.3). Validates Open Issue #4 (RREQ storm) at scale via task 5.10 (dense scenario), Open Issue #6 (IBSS power save) via relay current measurement in 5.7, and Open Issue #9 (mule authentication — exercised but not hardened, in task 5.9).

**Tasks**:

| # | Task | Effort | Depends on |
|---|---|---|---|
| 5.1 | Implement bundle aggregation (AGG flag, hold timer) | 3 days | Phase 4 |
| 5.2 | Implement relay TDMA superframe with leaf windows | 3 days | Phase 4 |
| 5.3 | Implement adaptive TX power (Section 13.3.1) | 2 days | 5.2 |
| 5.4 | Implement multi-gateway TIME_SYNC acceptance rule | 1 day | Phase 4 |
| 5.5 | Implement gateway anycast RREP response | 2 days | Phase 3 |
| 5.6 | 10-node integration test: 2 gateways, 4 relays, 4 leaves | 3 days | All above |
| 5.7 | Measure relay average current in 10-node test | 2 days | 5.6 |
| 5.8 | Validate routing failover: remove relay mid-test | 1 day | 5.6 |
| 5.9 | Validate mule: physically move mule node through mesh | 2 days | 5.6 |
| 5.10 | Validate dense scenario: 10 nodes in 1m radius, check CSMA | 1 day | 5.6 |
| 5.11 | Document `rimba_ibss_init()` and submit upstream to Morse Micro | 2 days | 5.6 |

**Phase 5 success gate**: 10-node mesh delivers sensor bundles end-to-end in < 10 seconds (connected path). Leaf power within 2× spec. Relay failover in < 60 seconds.

---

### Phase 6 — Optional: Geographic Routing (Weeks 18–20)

**Risks addressed**: None from the register — this is optional functionality. Validates the geographic routing tiers (Section 8.12) if GPS hardware is present.

Only if GPS modules are included in target hardware.

| # | Task | Effort | Depends on |
|---|---|---|---|
| 6.1 | GPS NMEA parser for lat/lon (UART) | 2 days | — |
| 6.2 | Store position in flash on first fix | 1 day | 6.1 |
| 6.3 | Add lat_e6/lon_e6/pos_age_s to HELLO encoder | 1 day | Phase 2 |
| 6.4 | Implement position table + expiry | 1 day | 6.3 |
| 6.5 | Implement quality-weighted distance score | 2 days | 6.4 |
| 6.6 | Implement Tier 1/2/3 forwarding decision | 3 days | 6.5 |
| 6.7 | Test geographic routing: 5-node outdoor test | 2 days | 6.6 |

---

## 5. Validation Criteria

### Phase 1 — IBSS Foundation

| Test | Pass criteria |
|---|---|
| IBSS init | Both boards join same BSSID, appear in each other's IBSS neighbour scan |
| Raw frame exchange | Board A sends EtherType 0x88B5 frame; Board B receives it with correct payload |
| Boot time measurement | P95 boot time measured and recorded. Spec updated if > 30ms. |
| Memory headroom | At least 50 KB SRAM free after morselib initialisation |

### Phase 2 — Peer Links

| Test | Pass criteria |
|---|---|
| HELLO discovery | Board A hears Board B's HELLO within 2× base interval |
| Peer link establishment | PEER_OPEN / PEER_CONFIRM completes in < 300ms |
| Encrypted DATA | Board A sends AES-CCM encrypted DATA; Board B decrypts correctly |
| Key persistence | Board A sleeps 30 seconds, wakes, sends DATA without re-running ECDH |

### Phase 3 — Routing

| Test | Pass criteria |
|---|---|
| 3-hop OGM route | Node A reaches Node C via Node B using OGM forwarding table |
| RREQ discovery | Route to unknown destination discovered in < 2 seconds |
| Gossip control | Dense 5-node test shows < 3 re-broadcasts per RREQ on average |
| RERR recovery | Remove intermediate node; route re-established in < 60 seconds |

### Phase 4 — DTN

| Test | Pass criteria |
|---|---|
| Bundle creation | BPv7 bundle created with correct CBOR structure and timestamp |
| BPSec encryption | Bundle decrypts correctly at destination using derived bundle key |
| Bundle store | 50 bundles stored and retrieved correctly after power cycle |
| Bundle expiry | Bundle with 5-second lifetime discarded after 5 seconds |
| Store-and-forward | Bundle stored when route absent; forwarded when route appears |
| Mule transfer | Mule receives 10 bundles via custody; originating relay clears them |
| RTC persistence | RTC time advances correctly through deep sleep (10 × 60s cycles) |
| Leaf wake cycle | Leaf wakes within ±5ms of scheduled time (10 consecutive cycles) |
| Relay catch rate | Relay catches leaf in receive window: 10/10 consecutive cycles |
| Leaf power | Average current < 50 µA (measured, 15-min cycle, no sensor) |

### Phase 5 — Integration

| Test | Pass criteria |
|---|---|
| 10-node delivery | Bundle from leaf reaches gateway in < 10 seconds (connected path) |
| Relay power | Within range of datasheet baseline (measured, 6 backbone peers): Continuous ~28 mA, Scheduled (K=6) ~14.3 mA. ⚠️ targets revised from the old "< 12 mA" — that used a 12 mA idle-RX estimate; datasheet Listen is 26 mA. Confirm idle-RX here. See rimba-battery-analysis.md. |
| Gateway failover | Remove one gateway; bundles route to second gateway in < 120 seconds |
| Isolation recovery | Isolated relay with no route; mule collects bundles within 1 sweep |

---

## 6. SDK and Library Dependencies

| Library | Purpose | Source | Licence | Risk |
|---|---|---|---|---|
| morselib (mm-iot-sdk) | MM6108 IBSS MAC driver | github.com/MorseMicro/mm-iot-sdk | Apache 2.0 | MEDIUM (IBSS undocumented) |
| ESP-IDF | ESP32-S3 base SDK | github.com/espressif/esp-idf | Apache 2.0 | LOW (very mature) |
| mbedTLS | X25519, ECDH, AES-CCM, HKDF | Bundled with ESP-IDF | Apache 2.0 | LOW |
| TinyCBOR | CBOR encode/decode | github.com/intel/tinycbor | MIT | LOW |
| LittleFS | Bundle store filesystem | github.com/joltwallet/esp_littlefs | MIT | LOW |
| RV-3028 driver | External RTC I2C | Community ESP-IDF drivers | Varies | LOW |
| BPv7 | Bundle Protocol v7 | **Implement from scratch** | Own | MEDIUM |

**No Linux, no POSIX, no C++ STL. All implementations must be C99-compatible for FreeRTOS.**

---

## 7. Known Open Issues from Spec

> 📌 **SPEC UPDATE PENDING (2026-06-21):** the power-save investigation
> (`rimba-mm6108-powersave-analysis.md`) and the rebaselined
> `rimba-battery-analysis.md` (datasheet 26 mA idle-RX) supersede several spec
> figures: relay current (Continuous ~28 mA / Scheduled ~14.3 mA, was ~14/~7.2),
> solar panel sizing (3 W / 2 W), and the chip-sleep story (ATIM absent,
> Deep-sleep STA-only, Snooze leaf-unsuitable → Issue #6 closed). After the
> Phase-1 idle-RX + boot-time measurements, fold the final numbers into
> `rimba-protocol-spec.md` (Section 12/13 power, §15 open issues).

The following issues from the Rimba protocol specification (rimba-protocol-spec.md, Section 14) have direct development plan implications:

| Issue # | Description | Dev plan impact |
|---|---|---|
| Issue #1 | IBSS init sequence undocumented | **Phase 1 — tasks 1.2–1.3** (RISK-01) |
| Issue #2 | Leaf peer link key lifetime | **RESOLVED** — addressed by Phase 2 (task 2.8, RTC memory storage) |
| Issue #3 | BCF file dependency | Use Seeed HaLow module for prototype; flag for production (RISK linked) |
| Issue #4 | RREQ storm validation at scale | Phase 3 bench testing (5-node, tasks 3.9–3.11); dense scale test in Phase 5 (task 5.10) |
| Issue #5 | Leaf power without TWT | **Approach resolved**: RTC-scheduled full power-off (Hibernate ≈0.05 µA), not chip PS. Leaf life is sleep-dominated, ESP32-bound (~10–20 µA). Measure in Phase 4 (task 4.12). See rimba-mm6108-powersave-analysis.md / rimba-battery-analysis.md. |
| Issue #6 | IBSS ATIM power save | **CLOSED — not available.** mm6108 fw 1.17.6 has no ATIM, and Deep sleep (1 µA timer-wake) is STA-only (firmware-confirmed by decompilation). The only IBSS-reachable chip sleep is Snooze (42 µA) via `CONFIG_PS`, which is leaf-unsuitable. v1 uses RTC power-off, not chip PS. See rimba-mm6108-powersave-analysis.md §9. |
| Issue #7 | Key rotation | Out of scope for v1 — see hardening plan Tier 4 |
| Issue #8 | Bundle fragmentation | Out of scope for v1 — MSDU limit handles most cases |
| Issue #9 | Mule authentication | Phase 4 basic custody only (task 4.13); auth deferred — see hardening plan Tier 1 |
| Issue #10 | OGM metric tuning | Phase 3 measurement and calibration |

---

*Document status: Draft 0.5*  
*Companion to: rimba-protocol-spec.md Draft 0.26*
