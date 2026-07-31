# RAW S0b-4 — the ESP puts an RPS on the air (Q2)

**Date:** 2026-07-31
**Stage:** S0b-4 of the RAW AP-side port (design doc
[`rimba-raw-apside-design.md`](../design-specification/rimba-raw-apside-design.md) §7.2).
**Question (Q2):** when the ESP host splices a hand-built RPS element into its S1G beacon, does that
element reach the air **unmodified**, or does the MM6108 blob rewrite/strip IEs in a host-supplied beacon?

**Predecessors.** S0b-0 (desk checks), S0b-1 (sniffer calibration), S0b-2 (**the gate — PASSED**, all-Linux,
zero ESP code) and the 2026-07-31 throughput A/B are all done. S0b-2 established that the MM6108 RAW engine
arms from a host-authored beacon and that a STA self-restricts, so **everything that remains is ESP-host-side**.

**Decision rules for this stage, pre-registered in the design doc §7.7:**

| Observation | Verdict |
|---|---|
| No EID-208 element on air | **Q3 not evaluable** — implement the mesh host-built-S1G-beacon fallback and re-capture. **Not a BLOCKED verdict.** |
| Element present, byte-matches the Linux reference | **Q2 PASS** — the host splice is the cheap path |

---

## Plan for this session

1. **Bench check + radio-silence audit** (done — below).
2. **Recon** — exact splice point in `umac_ap.c`, how `RIMBA_RAW_S0B_SPIKE` reaches the submodule compile,
   the capture/decode tooling that must be written, and which AP fixture to use.
3. **No-hook baseline capture FIRST.** Without it an EID-208 element on air cannot be attributed to the
   host splice, and a *missing* element gives no evidence the capture/decode path works at all.
4. **Apply the spike edit** behind `#ifdef RIMBA_RAW_S0B_SPIKE`, captured as a named patch file **outside
   `/tmp`** before flashing (the nightly reboot wipes `/tmp`). Baseline to revert to is `main`.
5. **Capture a full DTIM cycle** and classify per frame — Linux's short-beacon allowlist *does* include RPS
   (`ie.c:63`), so a capture that only samples DTIM beacons could report Q2=PASS while non-DTIM beacons carry
   nothing.
6. **Byte-diff** against S0b-2's captured reference `D0 06 20 40 09 04 E0 1F`.
7. **Radio-silent afterwards** — manual; the harness safety net does not cover a hand-run spike.

### Carried-forward footguns that apply to this stage

- **Gate the hook behind `#ifdef RIMBA_RAW_S0B_SPIKE`.** `umac_ap_build_beacon()` is shared morselib and
  **20 apps set `CONFIG_HALOW_AP_MODE=y`** — unguarded, every one of them emits the RPS.
- **`ie_rps_build()`-style two-pass sizing hangs hard.** A pass-1/pass-2 size disagreement is a `while(1)`
  because `MMOSAL_ASSERT` is live in our build.
- **The 4-byte trailing FCS nearly manufactured a false "RPS stripped"** in S0b-2 — brute-forcing the IE
  start offset matched 3/242 frames with *varying* offsets that looked exactly like the conditional S1G
  presence bits. Anchor on EID 213 with the FCS excluded.
- **Frame-count reconciliation, not the `morse0` drop counter.** S0b-1 measured 2.53 % beacon loss while
  `dropped` moved by 1. ~2.5 % is the healthy baseline, not zero.
- **Build only via `make … APP=… BOARD=proto1-fgh100m`** — a bare `idf.py` yields country `"??"` and the
  radio never boots, which masquerades as dead hardware.

---

## Bench state at session start — verified 2026-07-31 05:55 PDT

`make test-bench` (with `bench.env.sh` sourced) against
[`docs/reference/rimba-bench-devices.md`](../reference/rimba-bench-devices.md):

| Node | State | Note |
|---|---|---|
| board0 `E0:72:A1:F8:EF:A4` | present, `/dev/ttyACM0` | WAKE+BUSY unwired — fine, this stage is beacon-only |
| board1 `E0:72:A1:F8:F9:40` | present, `/dev/ttyACM1` | ditto |
| board2 `E0:72:A1:F8:F0:08` | **not enumerated** | expected — PPK2-rail gated; **not needed** for S0b-4 (§7.5: "beacon-only load ⇒ board0/board1 suffice") |
| chronium | `wlan1` **DOWN** | the sniffer |
| chronite | `wlan1` **DOWN** | Linux reference AP |
| chronogen | `wlan1` **DOWN** | not needed this stage |

**All three Linux radios are down and no `hostapd_s1g` / `wpa_supplicant_s1g` / `tcpdump` is running** — the
bench was left radio-silent by the throughput A/B, as recorded. 2/3 ESP nodes present is the expected
steady state, not drift. Bench pinned at Morse fw **1.17.8**, chip `0x0306`, S1G ch27 (`iw freq 5560`).

---

## Log

### 06:05 — the S0b-1/S0b-2 pcaps were still alive on chronium, and are now in the repo

Recon turned up something better than expected: **the actual capture files from S0b-1 and S0b-2 were still
on `chronium:/tmp`** — which is **tmpfs on a node with 19 days of uptime**. One reboot and the only
ground-truth captures this port has would have been gone. Copied into the repo:

| file | was | contents |
|---|---|---|
| `docs/worklog/artifacts/raw-s0b/s0b-2-reference-rawenabled.pcap` | `chronium:/tmp/raw.pcap`, md5 `7c3bc203ded4a3b288d547ff097325de` | 242 pkts — the S0b-2 **reference**, RAW enabled |
| `docs/worklog/artifacts/raw-s0b/s0b-1-cal-rawdisabled.pcap` | `chronium:/tmp/cal.pcap`, md5 `85588227355d2940f6e150a6dd98a698` | 2294 pkts — the S0b-1 **negative control**, RAW disabled |

(`chronium:/tmp/q4.pcap` is the voided 66 %-loss timing run — deliberately **not** preserved as a
reference; it must not be reused. `smoke.pcap`/`cal.tsv` likewise left behind.)

The S0b-1/S0b-2 *analysis scripts* did **not** survive — they lived in a dev-box session scratchpad under
`/tmp` that the nightly `daily-reboot.timer` wiped, exactly as the S0b-0 patch captures did. Only the
algorithm survived, in prose, in the S0b-2 worklog.

### 06:10 — new tracked decoder, and it is CALIBRATED against both controls

Rather than rewrite a throwaway parser for the third time, the decoder is now a tracked repo tool:
**`tools/raw_s1g_beacon_decode.py`** — the radiotap-aware pcap decoder design doc §7.6 item 1 called for
and which no in-repo script provides (all four existing capture scripts read a live `AF_PACKET` socket and
*skip* the radiotap header: `mesh_beacon_cap.py:31-32`, `mesh_rann_cap.py:47-48`, `mesh_grab_fwd.py:18`).

It handles the two decode traps explicitly, and refuses the failure mode that nearly manufactured a false
"RPS stripped" in S0b-2:

- **The IE offset is COMPUTED from Frame Control, never searched.** Base 15 bytes (FC 2 + Duration 2 +
  SA 6 + Timestamp 4 + Change Seq 1), plus the three conditional fixed fields — Next TBTT `+3` (FC bit
  `0x0100`), Compressed SSID `+4` (`0x0200`), ANO `+1` (`0x0400`). Verified against the driver:
  `morse_dot11_find_s1g_beacon_ies()` at `chronium:~/halow/morse_driver/dot11ah/ie.c:442-457`, sizes at
  `dot11ah/dot11ah.h:16-18,24-26`. The observed FC is reported with every capture as evidence.
- **The 4-byte trailing FCS is stripped**, and an IE walk that does not land *exactly* on the end is
  reported as an undecodable frame — never retried at another offset.

**Calibration — both controls reproduce the published S0b results to the frame:**

| | reference pcap (RAW on) | control pcap (RAW off) |
|---|---|---|
| S1G beacons decoded | **236** (worklog: 236) | **2222** (worklog: 2222) |
| undecodable | **0** | **0** |
| distinct IE chains | **1** — `[213, 5, 208, 217, 232, 214, 0, 221]` | **1** — `[213, 5, 217, 232, 214, 0, 221]` |
| IE offset | **15 on every frame** | **15 on every frame** |
| FC observed | `0x081c` — all three presence bits clear | `0x081c` — same |
| EID-208 | **1 distinct, `D0 06 20 40 09 04 E0 1F`, byte-identical to the golden constant** | **ZERO across 2222 beacons** |
| RSSI median | −36 dBm | −36 dBm |

The reconciliation arithmetic also lands: the control capture spans **2290** TBTT slots and holds 2222
beacons — **68 missing**. S0b-1 reported 58 genuinely absent *plus* 72 undecoded 33-byte frames of which
**10 coincidentally landed in empty slots**. 68 − 10 = 58, exactly. Two independent methods agree.

**Why this matters more than it looks.** S0b-1's and the throughput A/B's shared lesson was *calibrate the
instrument before trusting it*. Here the instrument is the decoder, and an uncalibrated one is the single
most dangerous component in this stage: if it were wrong, an ESP beacon that genuinely carried the RPS
would decode as *no element on air*, and §7.7 maps that observation to "implement the mesh fallback,
re-capture" — hours of work chasing a bug in the parser. It is now pinned to two known-good captures with
opposite expected answers.

### 06:10 — S0b-3's answer falls out of the archived captures for free

The decoder also reads S1G Capabilities octet [6] (`DOT11_MASK_S1G_CAP6_RAW_OPERATION_SUPPORT` = bit 3):

- reference (RAW enabled): **`0x08` on 236 of 236 beacons** — the bit is **SET**
- control (RAW disabled): **`0x00` on 2222 of 2222 beacons** — the bit is **CLEAR**

That is the live-Linux on-air A/B for the caps bit, i.e. **exactly the byte-diff target S0b-3 was going to
book bench time for**, already captured. It independently confirms the source reading recorded on
2026-07-30 that the bit is *live state* (set only while RAW is actually running), not a static capability
advertisement — and it is the reference the ESP arm must reproduce.

### 06:15 — how the spike is armed: one build-system line, ZERO submodule CMake edits

The design doc (§7.4) said *"`IDF_EXTRA_D` can only pass `-D` defines into the app build, never patch the
submodule"*. **Half of that is wrong, and the half that is wrong is the useful half.** `IDF_EXTRA_D`
passes CMake **cache variables** (`Makefile:49-62` → `idf.py -D` → `cmake -D`), which today's fixtures
individually convert into *app-private* compile defines with
`target_compile_definitions(${COMPONENT_LIB} PRIVATE …)`. That privacy is a property of how those apps
were written, not of the mechanism.

A **build-global** define does reach morselib. IDF applies the build-wide `COMPILE_DEFINITIONS` property
as a directory-scoped `add_compile_definitions()` inside every `idf_component_register`
(`vendor/esp-idf/tools/cmake/component.cmake:468,476`), and `components/halow/CMakeLists.txt` registers at
`:15-22` and only *then* does `add_subdirectory(components)` at `:29` — so `libmorse`
(`morselib/CMakeLists.txt:116`) inherits it. It is the same route by which `ESP_PLATFORM` and `IDF_VER`
already land on `umac_ap.c`'s compile line.

So the arming is **one line in the fixture's own top-level CMakeLists**
(`firmware/test-raw-rps/CMakeLists.txt:21`):

```cmake
idf_build_set_property(COMPILE_DEFINITIONS "RIMBA_RAW_S0B_SPIKE=1" APPEND)
```

**Verified empirically, not by inference** — from `build/test-raw-rps/proto1-fgh100m/compile_commands.json`,
both guarded files carry `-DRIMBA_RAW_S0B_SPIKE=1`:

| file | spike define on its compile line |
|---|---|
| `morselib/src/umac/ap/umac_ap.c` | **yes** |
| `morselib/src/umac/ies/s1g_capabilities.c` | **yes** |

and, checking the other AP apps' existing build trees, **isolation holds**:

| app | `umac_ap.c` compiled | spike define |
|---|---|---|
| `test-raw-cap` | yes | **no** |
| `rimba-halow-ap` | yes | **no** |
| `test-mesh-gate-ap` | yes | **no** |

That check is not ceremony. If the define had failed to reach `umac_ap.c`, the `#ifdef` body would have
compiled to nothing, the capture would have shown no EID-208, and §7.7 maps *"no EID-208 element on air"* to
*"implement the mesh host-built-S1G-beacon fallback and re-capture"* — hours of work chasing a build-system
mistake dressed up as a firmware finding.

**Rejected alternatives, with reasons:**

- **A CMake cache var via `IDF_EXTRA_D`** — a live footgun. Cache entries are **sticky per build dir**, and
  `tools/regtest/t0_build.py:242` rebuilds each app into that same `build/<APP>/<BOARD>` with no vars. One
  spike build would silently keep the RPS compiled into T0 until a `fullclean`. (This is the known
  "`idf.py` CMakeCache makes build flags sticky" trap.)
- **A Kconfig option** — would have worked. (`CONFIG_*` symbols *are* visible inside `umac_ap.c`:
  `common/common.h` → `mmwlan.h:44-46` `#ifdef ESP_PLATFORM #include "sdkconfig.h"`, and `ninja -t deps`
  lists the generated `sdkconfig.h` as a real dependency.) Rejected only because it costs a submodule
  Kconfig edit plus an `sdkconfig.defaults` line for no gain over one superproject line. It **is** the right
  mechanism for S1+, when this stops being a spike.
- **`target_compile_definitions(libmorse PUBLIC …)`** — rejected: that arms every app that links morselib,
  which is the contamination the guard exists to prevent.
- **A bare `#define` at the top of `umac_ap.c`** — rejected: arms all 27 AP-path apps with no off switch.

**Count correction:** the design doc §7.4 says "20 apps under `firmware/` set `CONFIG_HALOW_AP_MODE=y`".
The real number is **27** (26 before this stage's fixture). Only **7** of them actually instantiate a
SoftAP vif; the other 20 need the flag because morselib gates `umac_ap.c` and friends on it and mesh/IBSS
link against them. The blast radius of an unguarded define is a third larger than the doc claims.

---

## The two guarded edits

Both are working-tree changes to the `components/halow` submodule, captured as
`docs/worklog/artifacts/raw-s0b/s0b-4-morselib-spike.patch` (32 insertions, 2 files) **before flashing**,
and verified to `git apply --reverse --check` cleanly. The baseline to return to is `main`.

**1. `morselib/src/umac/ap/umac_ap.c`, inside `umac_ap_build_beacon()`** — between the
`ie_s1g_tim_build(...)` call and the `consbuf_append(buf, data->config.tail, …)` that follows it, i.e. the
`:418/419` the design doc predicted, unchanged in the current tree. The whole builder is three
concatenations — hostapd's opaque `head`, the TIM, hostapd's opaque `tail` — so splicing there puts the RPS
**directly after the TIM**, which is the Linux adjacency.

```c
static const uint8_t k_rps_golden[8] = { 0xD0, 0x06, 0x20, 0x40, 0x09, 0x04, 0xE0, 0x1F };
consbuf_append(buf, k_rps_golden, sizeof(k_rps_golden));
```

**Why this cannot hang.** `build_frame_with_class()` runs the builder **twice** — pass 1 with
`buf->buf == NULL` to size, pass 2 into the real buffer — and a pass-1/pass-2 size disagreement is a hard
`while(1)` on the `MMOSAL_ASSERT` in `consbuf_append` (`consbuf.c:30`), which is live in our build
(unlike `MMOSAL_DEV_ASSERT`, which compiles to `((void)(x))`). An unconditional fixed-size append agrees
across both passes by construction. Headroom is not close either: the TIM builder over-reserves 256 bytes
in pass 1 (`sizeof(struct dot11_ie_tim)` 5 + `MAX_PVB_LEN` 251) and writes at most 45 in pass 2, leaving
**~219 bytes of slack** after the 8-byte element.

**2. `morselib/src/umac/ies/s1g_capabilities.c`, in `ie_s1g_capabilities_build_ap()`** — the `if (false)`
gate at `:276-280` on `DOT11_MASK_S1G_CAP6_RAW_OPERATION_SUPPORT`. The `#else` branch keeps the original
`if (false)` verbatim, so the diff against upstream is additive only.

Two things make the guard here load-bearing rather than hygiene:

- **This file is in the UNCONDITIONAL `SRCS` list** (`morselib/CMakeLists.txt:52`) — unlike `umac_ap.c`,
  which is only compiled under `CONFIG_HALOW_AP_MODE`. It builds into every STA and mesh image.
- **The bit is live state, not a capability advert.** Flipping it on unconditionally would advertise RAW on
  an AP with no schedule — the "don't ship an advert you can't enforce" case. It is honest *only* under this
  same `#ifdef`, which is exactly when edit 1 puts a schedule into every beacon. That is why S0b-3 was
  folded into S0b-4 rather than run as the one-line flip originally planned.

---

## ✅ RESULT — Q2 PASS and S0b-3 PASS. Gold standard reached.

**Rig:** board0 (`68:24:99:44:6b:b7`, AP vif SA `6a:24:99:44:6b:b7`) as the ESP AP on `rimba-rawrps`;
chronite as the Linux RAW reference AP on `rimba-ping` (tracked `hostapd-rimba.conf` + `morse_cli -i wlan1
raw -s 20200,2 -a 1,255 -t 0 enable 1`, both commands `rc=0`); chronium in monitor on `morse0`, S1G ch27
(`type monitor`, `channel 112 (5560 MHz)`, `Operating Frequency: 915500 kHz`). Both APs beaconing
**simultaneously**, so a single capture holds both elements under identical radiotap conditions — which is
what §7.2 asks for, and what makes this a live-device diff rather than a spec diff.

### The A/B, on the same fixture

| | baseline (no hook) | spike | Linux reference (same capture as spike) |
|---|---|---|---|
| beacons decoded | **467** | **519** | **515** |
| undecodable | 0 | 0 | 0 |
| EID-208 present | **0** | **519 / 519** | **515 / 515** |
| RPS bytes | — | **`D0 06 20 40 09 04 E0 1F`** | **`D0 06 20 40 09 04 E0 1F`** |
| byte-identical to §7.1 golden | — | **True** | **True** |
| S1G caps octet[6] | **`0x00`** ×467 | **`0x08`** ×519 | **`0x08`** ×515 |
| DTIM / non-DTIM | 158 / 309 | **171 / 348** | 515 / 0 |
| IE offset | 15 (all) | 15 (all) | 15 (all) |
| FC | `0x401c` | `0x401c` | `0x081c` |
| RSSI median | −52 dBm | −52 dBm | −35 dBm |
| capture completeness | 97.29 % | 98.11 % | 97.35 % |

**Q2 = PASS.** The blob does **not** rewrite or strip IEs in a host-supplied beacon. The eight octets the
host wrote arrive on the air unchanged, on every single beacon, and are byte-identical to what a live Linux
device transmitted in the *same* capture — the gold standard the on-air rule asks for, not merely the
source-layout floor.

**S0b-3 = PASS.** The S1G Capabilities RAW Operation Support bit reaches the air from the host too. The
full element goes `1e 00 40 00 00 08 00 02 60 00 fd 00 fa 01 00` → `1e 00 40 00 00 08 **08** 02 60 00 fd 00
fa 01 00`: **one bit, everything else byte-identical** — the same signature the Linux A/B showed. So §7.7's
"S3 is dead as written" branch does not fire, and the host-built-S1G-beacon fallback is **not** needed.

### The RPS lands in the right place — and the chain difference is pre-existing, not a defect

```
ESP    [0, 5, 208, 48, 127, 244, 213, 217, 232, 214, 221]
                 ^ RPS directly after TIM
Linux  [213, 5, 208, 217, 232, 214, 0, 221]
                 ^ RPS directly after TIM
```

The **overall** chains differ, and that is expected and unrelated to RAW: on the ESP, 213/217/232/214 live
inside hostapd's `tail` blob (`beacon.c:2562-2569`) and therefore land *after* the TIM, whereas the Linux
driver's S1G converter emits 213 first. **The Q2 verdict is the eight RPS octets plus the TIM→RPS
adjacency, not the chain vector** — diffing the whole vector would report a false mismatch on a
long-standing ordering difference that predates this work. Both are correct on the thing that matters:
RPS immediately follows TIM on both stacks.

### The short-beacon worry is measured, not argued

§7.2 flagged that Linux's short-beacon IE allowlist *does* include RPS, so a capture sampling only DTIM
beacons could report Q2=PASS while non-DTIM beacons carried nothing. The fixture runs `dtim_period=3` (a
deliberate divergence from the reference AP's 1, recorded in `app_main.c`) precisely so both classes appear:
**171 DTIM and 348 non-DTIM beacons, and all 519 carry the RPS.** Source agrees — morselib has no
short-beacon TX path at all and `umac_ap_build_beacon()` is the single unconditional builder for every
beacon — but this is the measurement rather than the argument.

### Integrity of the runs

Every capture reconciles against TBTT slots spanned from `radiotap.mactime`: 97.29 % / 98.11 % / 97.35 %
complete, against the ~2.5 % healthy bench baseline S0b-1 established. No run is anywhere near the
material-loss threshold that would void it. The `morse0` drop counter moved by **1** across the baseline
capture and **4** across the spike capture — consistent with S0b-1's finding that it is a weak secondary
signal only (it accounted for 1 of 58 genuinely missing beacons there), which is why the frame-count
reconciliation is the gate.

RSSI −52 dBm on the ESP arm, nowhere near the ≈−3 dBm RX-overload signature. `mmwlan_override_max_tx_power(1)`
was accepted (no refusal logged). The design doc §7.5 claims "the ESP fixtures cap to 1 dBm" — **that was
false** for the AP fixture family (`test-raw-cap`, `rimba-halow-ap` and `rimba-halow-ap-perf` never call it);
this fixture does, following the `test-apsta-ap` precedent, and the call must sit between `set_config` and
`wifi_start` because the override returns `MMWLAN_UNAVAILABLE` once the WLAN subsystem is active.

---

## Bench footguns hit this session

- **`sudo pkill -9 -f wpa_supplicant_s1g` over ssh killed its own session.** The `-f` pattern matches the
  *full command line*, and the remote shell's command line contains the pattern because the script is passed
  as an argument to `bash` — so `pkill` matched and killed its own parent. The whole block died silently:
  no output, and the monitor was never brought up (`wlan1` still read `type managed`, which is what exposed
  it). This is the known `pgrep -f` self-match trap in `pkill` form, and over ssh it is worse because the
  failure looks like the commands simply not running. Nothing needed killing anyway — the bench had been
  left radio-silent — so the fix was to drop the line.
- **No `/sys/kernel/debug/morse/` on chronite**, so the passive `dump_raw_configs` readback used in S0b-2
  was unavailable here. Not a problem: `morse_cli … raw … enable 1` returned `rc=0` and the *on-air element*
  is a strictly better readback — the captured `D0 06 20 40 09 04 E0 1F` proves the config landed exactly.
- **`morse_cli -i wlan1 raw` with no sub-command** prints a usage error rather than dumping current state;
  it is a setter, not a getter.
- **The AP vif's SA is not the chip MAC.** The board reports `68:24:99:44:6b:b7` but beacons as
  `6a:24:99:44:6b:b7` (locally-administered bit set). Filtering a capture on the console-reported MAC finds
  nothing — which would look exactly like "the ESP never beaconed".

## Bench state left behind — radio-silent, verified

- **board0** reflashed to `rimba-hello`; console shows the banner and heartbeats with **zero** `mmwlan`/HaLow
  lines.
- **chronite**: RAW id 1 disabled + deleted, `hostapd_s1g` killed (0 processes), addresses flushed, `wlan1`
  DOWN.
- **chronium**: `morse0` DOWN, `wlan1` DOWN and restored to `type managed`.
- board1 untouched; board2 never powered (PPK2-gated, not needed).

## Artifacts

| file | what |
|---|---|
| `docs/worklog/artifacts/raw-s0b/s0b-4-esp-baseline-nohook.pcap` | 480 pkts — ESP AP, no hook. Zero EID-208, caps `0x00` |
| `docs/worklog/artifacts/raw-s0b/s0b-4-esp-spike-dualap.pcap` | 1043 pkts — **both APs**, the byte-diff capture |
| `docs/worklog/artifacts/raw-s0b/s0b-2-reference-rawenabled.pcap` | rescued S0b-2 reference |
| `docs/worklog/artifacts/raw-s0b/s0b-1-cal-rawdisabled.pcap` | rescued S0b-1 negative control |
| `docs/worklog/artifacts/raw-s0b/s0b-4-morselib-spike.patch` | the two guarded submodule edits |
| `tools/raw_s1g_beacon_decode.py` | the tracked decoder |
| `firmware/test-raw-rps/` | the spike fixture (+ `tools/regtest/manifest.py` entry) |

## What this does NOT prove, and what is next

**It does not prove the chip acts on the ESP's schedule.** Q2 is an *advertise*-plane result: the bytes
reach the air. Whether the MM6108's RAW engine arms from an **ESP-authored** beacon — Q3b — is read from the
chip's own tag-4210 `raw_stats_t` counters, not from a capture, and that is **S0b-5**:

- Read the ESP's counters via `mmwlan_get_morse_stats(1, …)` — already public and already exported by the
  `mmwlan*` glob, so **no submodule change**; the work is app-side (TLV walk, tag 4210, 15×LE-u32, `TEST|`
  reporting). Expect `assignments[]` to climb ≈ once per beacon and `invalid_assignments` to stay 0, as the
  Linux arm did (2179 / 0).
- Read tags 4171-4180 too (*beacons late from host*, *beacons TX late*, *beacons missing from host*). The RAW
  window is referenced to the **end of the beacon carrying the RPS**, so a late host-served beacon corrupts
  the time reference even with perfect enforcement. Without that control, *"the ESP cannot serve beacons
  punctually enough for RAW"* is indistinguishable from *"the FW has no AP-direction engine"* — and only the
  first is fixable.
- The fixture is already the right shape for it: `test-raw-rps` parks forever and already carries the `TEST|`
  contract.

Still open from earlier stages and **not** touched here: the ≤6-slot cap, `frame_crosses_slot` showing real
frames are too long for a 10100 µs slot at 1 MHz, and the big one — **RAW costs the AP ~22 % of its downlink
as a standing cost, and the client count at which the contention saving exceeds it is not answerable on this
bench.**
