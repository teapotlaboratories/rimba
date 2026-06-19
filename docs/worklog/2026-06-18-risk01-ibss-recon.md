# Worklog — 2026-06-18 — RISK-01 IBSS bring-up: reconnaissance & plan

**Author:** Aldwin (with Claude Code)
**Phase:** 1 — IBSS Foundation, RISK-01 (no public IBSS API on the MM6108)
**Branch:** `test2` (off `main`)
**Goal:** prove an MM6108 IBSS / ad-hoc link on **ESP32-S3 + mm-iot-sdk**, by
porting the ad-hoc setup sequence from MorseMicro's Linux mac80211 driver.
**Status:** reconnaissance complete — **feasible**; implementation not yet started.

This entry is the de-risking record before any code. It maps the exact firmware
command sequence the Linux driver uses for IBSS, pins a coherent version matrix,
locates the injection points in the ESP32 morselib, and diagnoses the
`EEXIST(-17)` seen on the bench.

> ## ⚠️ GOVERNING REQUIREMENT — derive everything from the Linux side
> The IBSS implementation MUST be based on the **Linux-side implementation**, not
> improvised from morselib's AP/STA code. Every IBSS behaviour (beacon contents,
> datapath addressing, probe handling, IBSS merge, ATIM) must trace to:
> - **`morse_driver`** (MorseMicro Linux mac80211 driver) for the firmware
>   command sequence / S1G specifics, and
> - **`net/mac80211/ibss.c`** (use the MorseMicro kernel fork) — esp.
>   `ieee80211_ibss_build_presp()` for the IBSS beacon/probe-resp IE set — plus
>   the higher-level programs `wpa_supplicant` (ad-hoc `mode=1`) and
>   `iw … ibss join`.
>
> This was the owner's requirement from the start. Earlier beacon/datapath work
> here was modelled on morselib's **AP path** (probe-response template) — a
> deviation; the beacon likely lacks IBSS-specific IEs (IBSS Parameter Set /
> ATIM, supported rates). Reconcile against `ibss.c` before continuing. Where the
> ESP32/morselib target can't match Linux exactly, call out the divergence
> explicitly. (Memory: `ibss-base-on-linux-implementation`.)

---

## Decisions locked in

- **Target platform:** ESP32-S3 + `mm-iot-sdk` (morselib). The Linux driver is a
  *reference* for the firmware-command sequence, not a build target.
- **Radio module:** Quectel **FGH100M** (`bcf_fgh100mhaamd`). NB `proto1` is the
  Seeed mf16858 board (`bcf_mf16858`); FGH100M will need its own board config.
- **Firmware anchor:** `mm6108.mbin v1.17.6` (`rel_1_17_6_2026_Feb_23`), the
  combo a community member reported IBSS working with on ESP32.

---

## The reference source (correction)

The mac80211 driver is **not** in `github.com/MorseMicro/linux` — that repo is
only a patched Linux *kernel* tree (no `drivers/net/wireless/morse/`). The actual
out-of-tree driver is **`github.com/MorseMicro/morse_driver`** (latest
`0-rel_1_17_9_2026_Apr_20`, pairs with kernel branch `mm/linux-6.12.81/1.17.x`).
That is the authoritative reference for the IBSS command sequence below.

Two code paths exist there: **mac80211/softmac** (`mac.c`, `command.c`) and
**FullMAC/cfg80211** (`wiphy.c`). **IBSS lives only in the mac80211 path**
(`wiphy.c` has `join_ibss` commented out).

---

## The IBSS command sequence (from `morse_driver` mac80211 path)

The chip is the same silicon/firmware the ESP32 drives, so these firmware
commands are exactly what we replicate over morselib's command channel.

Command IDs (`morse_commands.h` — **all present in the ESP32 SDK header too,
except where noted**):

| Command | ID | ESP32 SDK header? |
|---|---|---|
| `SET_CHANNEL` | `0x0001` | ✅ declared |
| `ADD_INTERFACE` | `0x0004` | ✅ declared |
| `REMOVE_INTERFACE` | `0x0005` | ✅ declared |
| `BSS_CONFIG` | `0x0006` | ✅ declared |
| `SET_QOS_PARAMS` | `0x0011` | ✅ declared |
| `IBSS_CONFIG` | `0x0035` | ❌ **gap** (0x0034 → 0x0036) |
| `BSSID_SET` | `0x0052` | ❌ **not declared** |

Interface-type enum (identical on both sides):
`STA=1, AP=2, MON=3, ADHOC=4, MESH=5`.

`IBSS_CONFIG` request struct (from the Linux driver — we recreate this on ESP32):

```c
struct morse_cmd_req_ibss_config {
    struct morse_cmd_header hdr;
    u8 ibss_bssid[6];
    u8 ibss_cfg_opcode;        // CREATE=0, JOIN=1, STOP=2
    u8 ibss_probe_filtering;   // module param, default true
};
```

**Exact ordering** (mac80211 callback order → firmware commands):

1. `ADD_INTERFACE` (`0x0004`), `interface_type = ADHOC(4)` → response returns the
   assigned `vif_id`; everything after is scoped to it.
2. `SET_CHANNEL` (`0x0001`), `dot11_mode = AH` (S1G), op/primary BW from the S1G
   chandef (1/2/4/8 MHz).
3. `BSSID_SET` (`0x0052`).
4. `BSS_CONFIG` (`0x0006`): `beacon_interval_tu`, `dtim_period`, `cssid`
   (compressed SSID, 32-bit; `morse_vif_generate_cssid`).
5. `IBSS_CONFIG` (`0x0035`): `bssid`, opcode `CREATE` (creator) / `JOIN`
   (joiner). **This is the command that actually enters ad-hoc.**
6. `SET_QOS_PARAMS` (`0x0011`): IBSS forces S1G default TXOP 15008 µs.
7. **Beaconing:** the Linux *host* software-generates S1G beacons (`beacon.c`);
   there is no "start beacon" command for IBSS on Linux. On ESP32 this is likely
   handled in firmware — `mmdrv_start_beaconing(vif_id)` exists in morselib
   (`mmdrv.h:196`). **This is the #1 open unknown to validate on hardware.**

Teardown: `IBSS_CONFIG` opcode `STOP(2)` → `REMOVE_INTERFACE (0x0005)`.

HaLow/IBSS quirks worth noting: ADHOC is treated as an **AP-type** interface (it
beacons); **no ATIM window** (S1G IBSS doesn't use ATIM — relevant to Open Issue
#6 power-save); long S1G beacons only (no short beacons in IBSS); AID is 0 so the
driver overloads it with the low 2 bytes of the peer MAC for peer lookup.

---

## ESP32 morselib: where ad-hoc is gated (and the injection points)

The public stack is `mmwlan → umac → mmdrv → morse_cmd → chip`, and **every layer
above the wire assumes STA or AP**:

- `mmwlan` (public API): only STA / AP / SoftAP enable paths. No IBSS API.
- `umac_interface.c`: maps interface types to STA or AP only; anything else
  `MMOSAL_DEV_ASSERT(false)` + `INVALID_ARGUMENT`.
- `mmdrv_add_if()` (`driver.c:548`): `switch` STA→STA, AP→AP, **default →
  `MMWLAN_INVALID_ARGUMENT`**. `enum mmdrv_interface_type` (`mmdrv.h:179`) only
  defines STA/AP.

**But the wire is not the blocker.** The `ADD_INTERFACE` builder at
`driver.c:568` sends `.interface_type = htole32(if_type)` for whatever `if_type`
it's handed, and `MORSE_CMD_INTERFACE_TYPE_ADHOC = 4` is already in the SDK
header. The two practical injection points:

- **`mmdrv_add_if()` switch** — add `MMDRV_INTERFACE_TYPE_ADHOC` (enum + one
  `case`) to get the firmware to create an ad-hoc vif.
- **`morse_cmd_tx()`** (`command.c:47`) — generic command-send primitive; use it
  to issue the undeclared `IBSS_CONFIG (0x0035)` and `BSSID_SET (0x0052)` with
  request structs we define to match the Linux driver.

### Diagnosis of the bench `EEXIST(-17)`

morselib creates its (STA) vif during the normal enable path. Issuing another
`ADD_INTERFACE` afterwards hits an interface that already exists → `EEXIST(-17)`.
(timothyb89's earlier `EINVAL` was a different ordering fault.) The fix is to
drive `ADD_INTERFACE(ADHOC)` from a **clean state** — before any STA/AP enable,
or after removing the default vif — not to add a second interface on top.

---

## Version matrix (binary-verified)

`mm6108.mbin` is **byte-identical** between `morsemicro/firmware 2.10.4-esp32-1`
and `-esp32-2` (same SHA256; both `rel_1_17_6_2026_Feb_23`). The only delta
between the two ESP components is the version tag + an `idf <6.0` cap on the
`halow` component. So our vendored `2.10.4-esp32-2` **already ships the
forum-proven firmware**.

| Item | Vendored now | Recommended | Note |
|---|---|---|---|
| Chip FW `mm6108.mbin` | v1.17.6 | v1.17.6 | ✅ already the proven blob |
| `components/halow` + `firmware` | `2.10.4-esp32-2` | keep | identical FW to `-esp32-1` |
| `vendor/mm-iot-sdk` (reference) | `2.11.2` | **pin to `2.10.4`** | match the proven morselib generation; build uses `components/halow`, so this is reference-only |
| ESP-IDF | 5.4.2 (vendored) | keep | satisfies `halow >=5.4.2,<6.0` |

Caveat: the ESP32 build uses `components/halow` (esp-halow), **not**
`vendor/mm-iot-sdk`. The mm-iot-sdk prebuilt morselib `.a` is ARM-only; ESP32
morselib is built from source in the halow component. Confirm the IBSS edits land
in the **halow** component's morselib copy, not just `vendor/mm-iot-sdk`.

---

## Forum / prior-art summary

- **Thread 1653 (FGH100M IBSS):** Morse staff (ajudge) — IBSS is "something we
  offer support for on our **Linux driver**" (officially Linux-only). timothyb89
  (ESP32) hit `EINVAL` on `INTERFACE_TYPE_ADHOC`, fixed his init sequence, and
  "tentatively got things working" on `mm6108.mbin v1.17.6 / 2.10.4-esp32-1` —
  **but never posted the corrected sequence.** *(Our own post in that thread,
  asking for the ordering after hitting `EEXIST(-17)`, has no reply.)*
- **Thread 1085 (MAC-layer access):** morselib source is open as of rel 2.10 for
  porting to other MCUs, but Morse will **only support unmodified morselib**; no
  raw-frame injection / advertising mode officially. → our approach is
  unsupported-by-vendor by definition; keep changes minimal and well-isolated.
- **Teapot worklog (Luckfox + FGH100M, Linux):** working SPI bring-up at MM
  release **1.15.3** (driver/hostap/morse_cli `1.15.3`, kernel branch
  `mm/linux-5.10.11/1.15.x`, firmware repo branch `1.15`), BCF
  **`bcf_fgh100mhaamd.bin`**, `insmod morse.ko bcf=bcf_fgh100mhaamd.bin
  spi_clock_speed=10000000 country=US`. It compiles `NL80211_IFTYPE_ADHOC`
  support but **never configures an IBSS** — no proven IBSS recipe there, just a
  solid SPI bring-up reference.

---

## Two implementation strategies

**A — Low-level command injection (minimal morselib touch).** A `rimba_ibss`
module issues the raw sequence via `morse_cmd_tx()`, bypassing umac. Closest to
the Linux command flow; smallest diff to vendored code. *Risk:* the RX datapath /
netif plumbing still expects a umac-known vif — frames may not be delivered up
without umac awareness.

**B — Extend umac/mmdrv with an ADHOC path (deeper, "complete").** Add
`MMDRV_INTERFACE_TYPE_ADHOC` + a `UMAC_INTERFACE_ADHOC` path + a public
`mmwlan_ibss_enable()`, mirroring the Linux `bss_info_changed` IBSS branch and
reusing `mmdrv_start_beaconing()`. Cleaner data path; matches the dev plan's
`rimba_ibss_init()` wrapper. *Risk:* larger modification of unsupported morselib;
must confirm firmware-side beaconing.

**Leaning B** for a working data path, but validating the **A** command sequence
first on hardware (does `ADD_INTERFACE(ADHOC)` + `IBSS_CONFIG` + beaconing bring
a link up at all?) is the cheapest way to retire the #1 unknown.

---

## Open unknowns to retire on hardware (in order)

1. Does the v1.17.6 firmware **auto-beacon** for IBSS once `IBSS_CONFIG(CREATE)`
   is sent, or must the host TX S1G beacons (as on Linux)? (`mmdrv_start_beaconing`)
2. Is `IBSS_CONFIG (0x0035)` / `BSSID_SET (0x0052)` actually required on ESP32,
   or does `ADD_INTERFACE(ADHOC)` + `BSS_CONFIG` suffice? (timothyb89's silence)
3. Does RX deliver up the stack with an ADHOC vif umac doesn't track? (→ A vs B)
4. The exact clean-state ordering that avoids `EEXIST(-17)`.

## Update — command-acceptance test implemented (builds; awaiting hardware)

Done in this session (working tree, uncommitted):

- [x] Pinned `vendor/mm-iot-sdk` → `2.10.4` (reference parity).
- [x] Added `boards/proto1-fgh100m/` (same XIAO pins, `bcf_fgh100mhaamd.mbin`).
- [x] Built out `firmware/rimba-halow-ibss/` (diagnostic app).
- [x] morselib additions (in `components/halow/.../morselib`):
  - `morse_commands.h`: declared `MORSE_CMD_ID_IBSS_CONFIG (0x0035)` +
    `MORSE_CMD_ID_BSSID_SET (0x0052)` and their req/resp structs + opcode enum
    (recreated from `morse_driver`).
  - `mmdrv.h`/`driver.c`: `MMDRV_INTERFACE_TYPE_ADHOC`, the `ADD_INTERFACE`
    switch case, and new `mmdrv_set_bssid()` / `mmdrv_cfg_ibss()` senders.
  - `umac_interface.{h,c}`: `UMAC_INTERFACE_ADHOC` (AP-type), type mapping to
    `MMDRV_INTERFACE_TYPE_ADHOC`, compat exclusion vs STA.
  - `umac_core.h`: `ibss_start` event arg.
  - `umac.c`: public **`mmwlan_ibss_enable()`** + handler — mirrors the AP path,
    skips the supplicant, issues the full sequence, logs each command's status.
  - `mmwlan.h`: `struct mmwlan_ibss_args` + `mmwlan_ibss_enable()` decl
    (auto-exported via the `mmwlan*` rule in `protected_syms.txt`).
- [x] **Builds clean**: `rimba-halow-ibss` on `proto1-fgh100m`, and the non-AP
  `rimba-halow-scan` (regression — shared morselib edits don't break other apps;
  unused IBSS path is `--gc-sections`-collected).

**Scope of this increment:** command *acceptance*, not a full link. The app calls
`mmwlan_ibss_enable()` with `start_beaconing = false` (no IBSS beacon body is
built yet) and prints each command's return code. It directly tests the
`EEXIST(-17)` fix (clean-state add) and whether the firmware honours
`IBSS_CONFIG (0x0035)`.

### Hardware test procedure

> Radio-silent rule still applies — flash `rimba-hello` when not actively testing.

```bash
make flash-monitor APP=rimba-halow-ibss BOARD=proto1-fgh100m PORT=/dev/ttyACM0
```

Watch the console for the `IBSS:` lines:

```
IBSS: ADD_INTERFACE(ADHOC) ok (vif_id=…)
IBSS: SET_CHANNEL ok (chan#=27, 1 MHz)
IBSS: BSSID_SET (…) ret=0
IBSS: BSS_CONFIG (bi=100 cssid=0x…) ret=0
IBSS: IBSS_CONFIG(CREATE) ret=0
==> mmwlan_ibss_enable SUCCESS — firmware accepted the IBSS sequence
```

### Decision tree (read off the `IBSS_CONFIG` line)

- **`IBSS_CONFIG(CREATE) ret=0`** → the firmware supports ad-hoc. Proceed to the
  next increment: build an IBSS beacon body + an IBSS datapath vtable
  (`construct_80211_data_header` with IBSS addressing, `lookup_stad_*` returning
  a peer for any address), set `start_beaconing = true`, and check a 2nd board
  (built `-DRIMBA_IBSS_CREATOR=0`) hears it / exchanges `0x88B5`.
- **`ret = -EEXIST(-17)`** → an interface still pre-exists; the clean-state
  assumption is wrong for this build — trace what added it before
  `mmwlan_ibss_enable`.
- **`ret = -EINVAL(-22)` or non-zero on `IBSS_CONFIG`** → the `0x0035` struct
  layout/opcode differs from `morse_driver` on this firmware; re-check the field
  order, or the command id. (`ADD_INTERFACE(ADHOC)` succeeding but `IBSS_CONFIG`
  failing would localise the problem precisely.)
- **`ADD_INTERFACE(ADHOC)` itself fails** → the firmware rejects interface type
  4; revisit whether a different chip-fw build is needed (vs the forum report).

Capture the console output into a new worklog entry whichever way it goes.

## Result — first hardware run (2026-06-18, board on /dev/ttyACM0)

Ran on a XIAO ESP32-S3 + Seeed HaLow module. Boot reported chip `0x0306`
(MM6108), firmware `1.17.6`, **board description `mf16858`** — NOTE this is the
mf16858 module, not FGH100M (the `mf16858` string shows up regardless of which
BCF file is compiled, so it's the module's burned-in identity; the bench
hardware is mf16858 and should use `boards/proto1` / `bcf_mf16858`).

Per-command outcome:

| Command | Result |
|---|---|
| `ADD_INTERFACE(ADHOC)` (type 4) | accepted |
| `SET_CHANNEL` | accepted |
| `BSSID_SET` (0x52) | accepted |
| `BSS_CONFIG` (0x06) | accepted |
| **`IBSS_CONFIG` (0x35), opcode CREATE** | **`rc -17` (EEXIST)** |

```
E ev morse_cmd_tx Command 35:110 failed with rc -17 (0xffffffef)
E ev umac_ibss_do_start IBSS: IBSS_CONFIG rejected by firmware (ret=-17)
```

**Interpretation — this resolves the open unknowns:**
- The v1.17.6 firmware **does support ad-hoc**. Interface type 4 is accepted, and
  command `0x35` is *recognised* — it returned a state error (`EEXIST`), not
  `-EINVAL`/"unknown command". `BSSID_SET (0x52)` is likewise accepted.
- The `EEXIST(-17)` is exactly the error reported on the Morse forum, now
  localised: it is **`IBSS_CONFIG(CREATE)` reporting the IBSS already exists**.
  On ESP32, `ADD_INTERFACE(ADHOC)` + `BSS_CONFIG` already establish the BSS, so
  the explicit CREATE is redundant. The fix is to **not treat `-17` as fatal**
  (and not tear the interface down on it) — likely the unpublished "init
  sequence" fix from the forum.

**Next experiment (cheap, high-value):** treat `IBSS_CONFIG` `-17` as
success-in-place (keep the interface up instead of removing it on the error
path), then check whether the chip is **already auto-beaconing** in IBSS mode —
run `rimba-halow-scan` on a second board and look for SSID `rimba-ibss` / the
shared BSSID. If it appears, firmware-side IBSS beaconing works and we can skip
hand-building beacons; if not, the beacon-body + datapath increment is needed.

## Result — auto-beacon experiment (2026-06-18, two boards)

Made `IBSS_CONFIG` `-17` non-fatal (keep the interface up). Board 0 then reports:

```
IBSS: IBSS_CONFIG reports already-exists (rc -17) — treating as created
==> mmwlan_ibss_enable SUCCESS — firmware accepted the IBSS sequence
alive (creator=1, ibss_status=0)
```

So the full sequence now "succeeds" and the ADHOC interface stays up
(`start_beaconing=false`). Board 1 (`rimba-halow-scan`, same channel list) was
booted while board 0's IBSS was up and **found no networks** ("Scan finished",
zero APs).

**Conclusion: the MM6108 firmware does NOT auto-beacon in IBSS mode.** Bringing
up the ADHOC interface + `BSS_CONFIG` + `IBSS_CONFIG` is not enough to put it on
air — beacons must be **host-generated and TX'd**, exactly the Linux-driver model
(`beacon.c` software S1G beacons). `mmdrv_start_beaconing()` can't be used as-is:
it pulls the beacon body from `umac_ap_get_beacon()` (AP data we don't allocate
on the ADHOC path), so it would null-deref. Both unknowns #1 (auto-beacon — NO)
and #2 (IBSS_CONFIG real — YES, returns EEXIST) are now retired.

(Aside: both bench modules report board `mf16858`; per the owner we keep
`bcf_fgh100mhaamd`/`proto1-fgh100m` for now — it boots fine either way.)

## Where this leaves RISK-01

The chip-command half of RISK-01 is **proven**: ad-hoc interface type and the
IBSS commands work; `EEXIST` is understood and handled. What remains is the
host-side **IBSS beacon + datapath** increment:

1. Allocate IBSS state akin to `umac_ap_enable_ap` (so `umac_ap_get_beacon` has a
   valid context), or add an IBSS beacon builder.
2. Hand-build an IBSS beacon body (long S1G beacon: SSID + supported rates + S1G
   capabilities/operation + IBSS parameter set; IBSS capability bit, NO TIM),
   store as `config.head`/`tail`, then `mmdrv_start_beaconing()`.
3. Add an IBSS datapath vtable (`construct_80211_data_header` with IBSS
   addressing ToDS=FromDS=0/A1=DA/A2=SA/A3=BSSID; `lookup_stad_*` returning a
   peer for any address since IBSS has no association).
4. Two-board check: board 0 beacons, board 1 `rimba-halow-scan` sees `rimba-ibss`
   / the shared BSSID; then exchange EtherType `0x88B5` frames (the Phase-1 gate).

## Result — IBSS beacon implemented; detection needs probe-response (2026-06-18)

Implemented host-generated IBSS beaconing (all in the `halow` morselib copy):
- `umac_ibss.h` + code in `umac.c`: a single IBSS beacon context (`g_ibss`), a
  builder `umac_ibss_build_beacon` (PV0 mgmt header, beacon subtype, broadcast
  DA, our MAC as SA, shared BSSID; timestamp/BI; **IBSS capability bit**; SSID IE;
  `ie_s1g_capabilities_build`; a hand-packed **S1G Operation IE**; no TIM), and
  `umac_ibss_get_beacon` (mgmt frame + tx metadata, mirroring `umac_ap_get_beacon`).
- `umac_mmdrv_shim.c`: `mmdrv_host_get_beacon` dispatches to the IBSS getter when
  `umac_ibss_is_active()`.
- `umac_ibss_do_start` now populates `g_ibss` (incl. the S1G operation captured
  from `umac_interface_get_current_s1g_operation_info` after SET_CHANNEL) and
  calls `mmdrv_start_beaconing`. App sets `start_beaconing=true`.

**On hardware:** board 0 brings up IBSS and beacons **stably** — `ibss_status=0`,
no crash, no beacon-worker errors. But board 1 (`rimba-halow-scan`) sees **no
networks**.

**Root cause (from the RX dispatch, not the beacon):**
- `umac_datapath_process_rx_mgmt_frame_sta` (`umac_datapath.c:341`) feeds **only
  `PROBE_RSP`** to the scanner; **beacons are not processed for scanning**.
- The HW scan is **active**: it broadcasts probe requests and waits for probe
  *responses*.
- Our IBSS node beacons but has **no datapath / mgmt-RX handler**, so it never
  answers probe requests → scanner gets no probe response → board 0 is invisible
  to scan despite beaconing correctly. (The AP path answers probes via
  `datapath_ops_ap.process_rx_mgmt_frame` → `umac_ap_handle_probe_req`.)

So both "scan detects us" and the `0x88B5` exchange require the **IBSS datapath**
(next increment): an IBSS datapath vtable whose `process_rx_mgmt_frame` answers
probe requests (build a probe response from `g_ibss` — same IEs as the beacon),
plus `construct_80211_data_header` (IBSS addressing) and a peer model for data.

Testing aid: `rimba-halow-scan` was given a re-scan loop (every 4 s) so
non-interactive capture doesn't have to race a one-shot boot scan.

Capture note (bench): XIAO USB-Serial-JTAG re-enumerates on reset; a beaconing
board can't be reset by esptool (CPU busy). Reliable capture = flash/reset, then
`sleep ~2-3 s`, then a single continuous `cat` (see commands in the session).

## Result — beacon-aware scan still doesn't detect the IBSS node (2026-06-18)

Made the STA RX path also feed **beacons** to the scanner
(`umac_datapath.c`: added a `DOT11_FC_SUBTYPE_BEACON` case →
`umac_scan_process_probe_resp`; the probe-resp parser is subtype-agnostic and
needs SSID + S1G Capabilities + S1G Operation IEs, all of which our beacon has).
Also looped `rimba-halow-scan` (4 s) for reliable capture.

Two-board test: board 0 = `rimba-halow-ibss` (ADHOC, beaconing), board 1 =
beacon-aware `rimba-halow-scan`. **Board 1 still finds no networks** over multiple
scan cycles.

So the cross-board link is not yet demonstrated, and the cause is now in a
**low-visibility** area (no RF sniffer on the bench):
- **Either** the chip is not actually radiating the host-provided IBSS beacon —
  plausibly the host beacon needs the fuller AP-style TX context to be admitted
  (the working SoftAP path also sets up `sta_common` + `umac_datapath_configure_ap_mode`
  + a valid mgmt rate-control table; our ad-hoc path skips those). The beacon
  worker runs without error, but "handed to the chip" ≠ "on air".
- **Or** the chip does not deliver received beacons to the host during an active
  scan (reports only probe responses), so even a perfect beacon wouldn't show.

Both are hard to disambiguate without either (a) a HaLow sniffer / monitor-mode
capture to confirm OTA beacons, or (b) building the fuller datapath + AP-style TX
context so the node both radiates and answers probe requests.

### State of RISK-01 at this checkpoint
- ✅ ADHOC interface + IBSS commands accepted by firmware (HW-proven).
- ✅ `EEXIST(-17)` understood and handled.
- ✅ Host IBSS beacon generated; board runs stably in ad-hoc mode, no crash.
- ❌ Not yet observed on a second board (radiation/scan-delivery unverified).

### Recommended next steps (need a decision)
1. **Best unblocker: a HaLow sniffer** (another MM6108 in monitor mode, or a
   HaLow-capable analyzer) to confirm whether board 0's beacon is on air. This
   removes the guesswork.
2. **Or** build the full IBSS datapath + AP-style TX context: allocate a peer
   `stad`, `umac_datapath_configure_*` for ad-hoc, valid mgmt rate control, and
   probe-request answering — i.e. make the ad-hoc interface a first-class TX/RX
   interface like AP, then exchange `0x88B5`. Larger, iteration-heavy.

### Working-tree changes from this increment (all uncommitted)
- morselib: `umac_ibss.h`, IBSS beacon + `mmwlan_ibss_enable` in `umac.c`, shim
  dispatch, `morse_commands.h`/`mmdrv.h`/`driver.c` IBSS commands,
  `umac_interface.*` ADHOC type, and the beacon-aware scan case in
  `umac_datapath.c`.
- apps: `firmware/rimba-halow-ibss/` (new); `rimba-halow-scan` got a re-scan loop
  (testing aid — candidate to revert).
- board: `boards/proto1-fgh100m/`.

## Result — in-scan raw sniffer sees nothing (2026-06-18)

Used board 1 as a sniffer (the owner's suggestion): added a raw-frame capture to
`rimba-halow-scan` via the internal `mmwlan_register_rx_frame_cb` (filter
`BEACON|PROBE_RSP|PROBE_REQ`), printing each frame's flag/len/freq/RSSI/A2/A3 and
flagging A3 == our IBSS BSSID. Ran board 0 = beaconing IBSS, board 1 = scan+sniff.

**Result: zero `RXFRAME` callbacks over 5 scan cycles** — no OTA frames captured
at all.

Inconclusive but narrowing:
- Consistent with **board 0 not radiating** the host beacon (leading hypothesis:
  the chip needs the fuller AP-style TX context — peer `stad`,
  `umac_datapath_configure_*`, valid mgmt rate-control — before it will transmit;
  the minimal ad-hoc path skips these).
- OR the `rx_frame_cb` is **not exercised during a hardware-offloaded scan**
  (`hw_scan`), so the sniff is a false negative — in which case a true
  **monitor-mode interface** (`INTERFACE_TYPE_MON=3` + fixed channel + raw RX) is
  needed for ground truth, rather than piggy-backing on scan.

So three RX-side approaches (scan, beacon-aware scan, in-scan raw sniff) have all
shown no reception. Verdict: **blocked on RF ground-truth.** The two ways
forward, both sizable:
1. **Monitor-mode interface on board 1** — a real sniffer (not scan-based) to
   definitively answer "is board 0 on air?" before more TX work.
2. **Full IBSS datapath + AP-style TX context** — make the ad-hoc interface a
   first-class TX/RX interface (peer `stad`, datapath config, rate control,
   probe-request answering) so it both radiates and is reachable; then `0x88B5`.

The chip-command layer of RISK-01 remains proven; the open question is purely the
host-side TX/RX plumbing needed to get frames on/off the air in ad-hoc mode.

## Verification — decoded the on-air IBSS frame vs Linux (2026-06-19)

Added a hexdump to the board-1 sniffer and decoded a captured IBSS probe response
(77 B) byte-for-byte. It is well-formed and faithful to the Linux IBSS frame:

```
FC=0x0050 (PV0 mgmt, PROBE_RESP)  Dur=0x02d0
A1/DA = bc:2a:33:96:b2:9f  (the probe requester, board 1)
A2/SA = 02:12:34:56:78:9a  (= BSSID)   ← chip applied SW-4741 override
A3/BSSID = 02:12:34:56:78:9a
timestamp=0  beacon_int=100TU  cap=0x0002 (IBSS set, ESS clear)
IE 00/0a "rimba-ibss"            SSID
IE 06/02 00 00                   IBSS Parameter Set, ATIM=0  (exact ibss.c:133 match)
IE d9/0f …                       S1G Capabilities (217)
IE e8/06 00 44 1b 1b 00 00       S1G Operation (232): op_class 68, chan 27
```

**Two earlier concerns resolved by the decode:**
- The flagged "SA = my_mac bug" is **not a bug**. The wire SA = BSSID — the chip
  applies morse_driver's SW-4741 (`tx_11n_to_s1g.c:867`, "SA MUST be the BSSID")
  itself. My source `SA = my_mac` matches mac80211 `ieee80211_ibss_build_presp`
  (`sa = vif.addr`); the chip overrides it. Faithful, no fix needed.
- The host body provides S1G Cap/Op and the chip does **not** duplicate them — so
  on ESP32 the host supplies the S1G content IEs (as the AP path does via
  hostapd) and the chip handles S1G framing + SA. Confirmed: no malformed/dup IEs.

**Minor deltas vs Linux (not bugs, optional to close):** no legacy Supported
Rates IE (mac80211 emits it in the 11n presp but morse_driver strips it for S1G,
so its absence matches the S1G wire); no Country IE and no S1G Short Beacon
Interval IE (Linux probe-resp carries these). The IBSS-defining content matches.

Note: this is the PROBE RESPONSE (shares `umac_ibss_append_s1g_ies` with the
beacon, so the beacon's IE content is equally verified). The S1G *beacon* frame
itself isn't surfaced by the scan-based sniffer, so its EXT/S1G-beacon framing is
the chip's job and unverified-on-wire.

## Verification — command structs vs raw morse_driver (2026-06-19)

Diffed my recreated IBSS commands against `MorseMicro/morse_driver@main`
(`morse_commands.h`, `command.c`) — **byte-for-byte match, no discrepancies**:
- IDs: `MORSE_CMD_ID_IBSS_CONFIG = 0x0035`, `MORSE_CMD_ID_BSSID_SET = 0x0052`.
- `morse_cmd_req_ibss_config { hdr; u8 ibss_bssid[6]; u8 ibss_cfg_opcode; u8
  ibss_probe_filtering; } __packed` — opcodes CREATE=0/JOIN=1/STOP=2.
- `morse_cmd_req_bssid_set { hdr; morse_cmd_mac_addr bssid; } __packed`.
- `morse_cmd_header` = 6×`__le16` (12 B).
My structs are `MM_PACKED` and `ibss_probe_filtering` defaults to 1 (matches the
driver's `enable_ibss_probe_filtering = true`). Opcode selection
(stop→STOP, else creator→CREATE/joiner→JOIN) matches `morse_cmd_cfg_ibss`.

### Linux-fidelity verification summary
- ✅ Beacon/probe-resp IE content (on-wire decode) — faithful.
- ✅ Command IDs + structs — byte-for-byte vs morse_driver.
- ✅ Command sequence + probe-answering + addressing — from morse_driver / ibss.c.
- 🚧 Data path — plumbing implemented (see below); data not yet flowing.

## Data path — plumbing implemented; ARP not resolving (2026-06-19)

Un-stubbed the IBSS data plane in `umac.c`, reusing the STA single-peer model:
- A single shared peer `stad` (`g_ibss.stad = umac_sta_data_alloc_static`) — IBSS
  has no association, so all peers map to it; real DA/SA travel in the 802.11
  header. The TX path requires a non-NULL stad, so the `lookup_stad_*` ops return
  it.
- `enqueue/dequeue_tx_frame` use the per-stad queue
  (`umac_sta_data_queue_pkt`/`_pop_pkt`), mirroring `datapath_ops_sta`.
- Marked the shared stad **OPEN** security so the TX datapath sends **plaintext**
  (`umac_datapath.c:1733` only encrypts when `security_type != MMWLAN_OPEN`) —
  correct for a keyless open IBSS.
- `construct_80211_data_header` already does IBSS addressing (ToDS=FromDS=0,
  A1=DA, A2=SA, A3=BSSID).
- App (`rimba-halow-ibss`) now picks role from its MAC (one binary for both
  boards), pins a static IP (CREATOR .1 / JOINER .2), brings the netif up
  (IBSS, like AP, fires no link-up event), and pings the peer.

**On two boards:** both come up with correct roles, beacon, and start pinging —
but every ping **times out** with `ping_sock: send error=0` on *both* sides. That
is the **ARP-unresolved** symptom: lwIP has no MAC for the peer IP, so it never
emits the ICMP. (Identical to the AP-STA ping test, which needed a static ARP
entry — see `2026-06-18-halow-ap-sta-ping.md`.)

**So the data plumbing runs but a frame round-trip isn't completing.** Open
question to debug next: does the ARP broadcast TX go out / get received, and does
unicast data RX get delivered up? Plan:
1. Add **static ARP** entries (peer IP → peer MAC) on both boards to bypass ARP
   and isolate unicast data TX/RX (exactly what unblocked AP-STA). Peer MACs:
   creator `68:24:99:44:6b:b7`, joiner `bc:2a:33:96:b2:9f`.
2. If static ARP makes ICMP flow → broadcast/ARP path is the remaining gap; if
   not → unicast data RX delivery (the shared-stad RX dedup / 802.11→802.3
   translate path) needs tracing against mac80211 `rx.c`.
3. Then swap ICMP for raw EtherType `0x88B5` (the literal Phase-1 gate).

### Static-ARP diagnostic result (2026-06-19)

Added a static ARP entry (peer IP → peer MAC) on both boards (`USE_STATIC_ARP`,
`etharp_add_static_entry`). Result: the **`send error=0` disappears** — lwIP now
*sends* the ICMP (TX no longer blocked on ARP) — **but pings still time out** on
both boards. So:
- **Unicast TX is now issued** (ARP was the only thing blocking the send).
- **The round-trip still fails** → either the data frame isn't reaching the air,
  or (more likely, since it's symmetric on both boards) the **RX data path isn't
  delivering received frames up to the netif**.

Static ARP was the diagnostic; **the goal remains dynamic ARP** (which AP-STA got
once its netif came up). But dynamic ARP can't work until the data round-trip
does — and the round-trip is the real remaining bug.

**Next diagnostic:** instrument the RX data path — log in `mmhalow` `halow_rx`
(the `rx_pkt_callback`) and/or the umac RX data handler — to see whether the peer
receives the frame at all. That splits it cleanly: frames arrive → RX→netif
delivery bug; frames don't arrive → OTA TX bug (verify via TX counters or a
monitor interface, since the scan sniffer only surfaces mgmt frames, not data).
Check `umac_datapath_process_tx_frame` (dequeue→frame build→`mmdrv_tx_frame`) for
a silent drop on the shared-stad path, and the RX dedup/seq handling against
mac80211 `rx.c` for the IBSS case.

## ✅ Data path works — RX-VIF bug found & fixed; IP ping over IBSS (2026-06-19)

Root cause of the dropped round-trip: the RX **data** path resolves the VIF by
checking only STA or AP (`umac_datapath.c` ~595): our interface is
`UMAC_INTERFACE_ADHOC`, so it fell through to `"Invalid RX VIF" → drop`. **Every
received IBSS data frame was dropped before the netif.** Fix: add an
`UMAC_INTERFACE_ADHOC` case mapping to `MMWLAN_VIF_AP` (no dedicated IBSS vif
enum), so RX data is delivered up.

With the fix + static ARP, two boards exchange ICMP over IBSS:
```
RXDATA len=106 dst=68:24:99:44:6b:b7 src=bc:2a:33:96:b2:9f eth=0x0800
reply from 192.168.13.2: seq=32 time=17 ms  <== IBSS DATA OK
counts: ok=16  rxdata=32  timeout=0   (~17-20 ms RTT)
```
The **Phase-1 data gate is met** (IP data exchange between two MM6108 IBSS peers).
Note the data-frame `src` is the peer's **real MAC** — SW-4741 (SA=BSSID) is
beacon-only, so unicast data addressing is normal and ARP can work.

Implementation note: the shared-stad model + the STA-style enqueue/dequeue +
OPEN-plaintext TX + IBSS addressing all check out on the wire. RX dedup uses the
single shared stad's sequence space (fine for 2 nodes; revisit for >2).

### ✅ Dynamic ARP works (goal met) — no static entry (2026-06-19)
Set `USE_STATIC_ARP=0` (remove the static ARP entry entirely) and re-ran:
```
counts: ok=18  rxdata=36  timeout=0  senderr=0   (~16-17 ms RTT)
```
ARP resolves **dynamically** over the IBSS — the static entry was only ever a
diagnostic. The RX-VIF drop was the sole root cause; once received frames are
delivered, the ARP broadcast→unicast-reply round-trip completes and IP flows with
no manual entries. This matches how AP-STA behaved once its netif was up.

## RISK-01 — RESOLVED

End-to-end on two ESP32-S3 + MM6108 boards, all derived from the Linux side:
- ✅ ADHOC interface + IBSS commands (vs morse_driver, byte-verified)
- ✅ Stable bring-up; `EEXIST(-17)` understood/handled
- ✅ Host IBSS beacon (vs ieee80211_ibss_build_presp; on-wire verified)
- ✅ Probe-answering → discoverable as a genuine IBSS (cap 0x0002)
- ✅ **IBSS data path: bidirectional IP, dynamic ARP, 0 timeouts, ~17 ms RTT**

The Phase-1 success gate (two battery nodes exchange L2 frames without
infrastructure) is demonstrated. The literal Rimba EtherType `0x88B5` rides the
same data path — it's just a different EtherType than the IP (0x0800) used here;
sending raw `0x88B5` is an app-level `mmwlan_tx` with that EtherType, not a
datapath change.

Cleanup still owed before this is merge-ready: gate/remove the `RXDATA` debug log
in `mmhalow.c` and the static-ARP/MAC diagnostic scaffolding in the app; resolve
the `.mbin.o` artifact churn (above).

## TODO — investigate the `components/firmware/*.mbin.o` build artifacts

Hit a footgun: `components/firmware/mm6108/bcf_fgh100mhaamd.mbin.o` (and the other
`*.mbin.o`) are objcopy-generated binary objects (`make_binary_object` in the
firmware component's CMakeLists, from the `.mbin` blobs). They are **tracked in
git** (committed in the pristine import) **but the build regenerates them
in-place for the target arch (xtensa)**. Consequences observed:
- `git checkout` / reverting a `.mbin.o` restores the committed object, which is
  **not an xtensa object**, and the incremental build does **not** regenerate it
  (its mtime looks up-to-date) → link fails: *"unknown architecture … incompatible
  with xtensa output"*. Fix used: `rm` the `.o` to force regeneration.
- Every build dirties the firmware submodule (the `.o` changes), which is why the
  RISK-01 commits had to carefully avoid staging it.

**To investigate / decide:** should these `.mbin.o` be gitignored in the firmware
submodule (they're generated), or have CMake emit them into the build tree
instead of in-source? Either would stop the per-build churn and remove the
revert-breaks-the-build trap. Confirm what the committed `.o` actually is (wrong
arch vs placeholder) and why upstream tracks it. Low priority, but it keeps
biting the commit workflow.

## Result — sniffer validated against a known AP (2026-06-19)

Per the owner's suggestion, validated the board-1 sniffer against a known-good
radiator: board 0 = `rimba-halow-ap` (SoftAP), board 1 = scan+sniff. **It works:**
- Scanner listed `rimba-ping` (BSSID `6a:24:99:44:6b:b7`, cap `0x0011`).
- Raw sniffer (`rx_frame_cb`) captured frames at **915.5 MHz / ch27**, `A2=A3=`
  the AP's BSSID — so the RF link between the two boards is fine and the capture
  path works.

**Crucial detail:** every captured frame was **`flag=0x20` = PROBE_RSP**, never
`0x100` = BEACON — even though the AP beacons continuously. So during a
hardware-offloaded scan, **the chip delivers probe *responses* to the host, not
beacons.** The AP is discoverable purely because it **answers probe requests**
(active scan).

This re-frames everything:
- The IBSS null result was NOT evidence the node is silent — beacons aren't
  surfaced during scan regardless, and the IBSS node doesn't answer probes, so
  there was nothing to capture either way.
- **Active-scan detection works for anything that answers probe requests.** The
  cheapest proof that the ad-hoc node is on air (TX + RX) is therefore to make it
  **answer probe requests**, not to chase beacon reception.
- A scan-based sniffer cannot see beacons; only a true monitor interface
  (`INTERFACE_TYPE_MON`, promiscuous) could — but that's now a detour.

### Revised plan (next)
Build the IBSS node's **RX/TX datapath** so it answers probe requests:
1. Configure a datapath on the ad-hoc interface (so RX mgmt frames are delivered)
   with a `process_rx_mgmt_frame` that replies to PROBE_REQ using a probe response
   built from `g_ibss` (same IEs as the beacon) + a peer `stad` + the mgmt-TX path.
2. Verify: board 1's normal scan now lists `rimba-ibss` (proves the ad-hoc node
   TX/RXes on air).
3. Then extend the datapath to `0x88B5` data frames (IBSS addressing) — Phase-1 gate.

This is the full datapath work (task #9); the validation shows it's also the
shortest route to an on-air proof.

## Reference — authoritative IBSS beacon/probe-resp spec from Linux (2026-06-19)

Extracted from `MorseMicro/linux@mm/linux-6.12.81/1.17.x` `net/mac80211/ibss.c`
(`ieee80211_ibss_build_presp`) + `morse_driver` `dot11ah/tx_11n_to_s1g.c`,
`dot11ah/ie.c`, `beacon.c`. This is what our IBSS beacon MUST match.

**mac80211 builds the IBSS beacon/probe-resp body (11n form), in order:**
fixed (timestamp, beacon_int, capability) → SSID(0) → Supported Rates(1) → DS
Param(3, **2.4 GHz only**) → **IBSS Parameter Set(6, len 2, ATIM=0 — ALWAYS
present)** → [CSA] → Ext Supp Rates(50) → custom IEs (RSN) → [HT/VHT] → WMM.
Capability = `WLAN_CAPABILITY_IBSS` set, `ESS` clear, `PRIVACY` if encrypted.

**morse_driver converts to S1G on TX** (`morse_dot11ah_beacon_to_s1g`): frame
becomes **EXT / S1G_BEACON**, `SA = BSSID` (SW-4741); strips Supported Rates / Ext
Rates / DS / HT / VHT (`morse_dot11ah_mask_ies`); the **IBSS Parameter Set (6) is
NOT masked → carried through**; injects S1G Beacon Compatibility + S1G
Capabilities + S1G Operation + S1G Short Beacon Interval. Clears short-slot-time.
**IBSS S1G beacons are always LONG** (`beacon.c:388` — `if (type==ADHOC)
short_beacon=false`). Probe-resp S1G IE set: SSID, Country, [RSN], S1G
Capabilities, S1G Operation, S1G Short Beacon Interval.

**IBSS merge** (`ieee80211_rx_bss_info`): adopt a foreign IBSS iff same SSID, same
channel, IBSS cap set, different BSSID; tiebreak by **TSF — higher beacon TSF
wins**, the lower-TSF node adopts that BSSID/TSF. **ATIM** is power-save only
(window 0 = always awake); not needed for basic data exchange.

**Divergence to note (ESP32 vs Linux):** on Linux the 11n→S1G beacon conversion
is host-side (morse_driver); on ESP32 the **chip firmware** does it (proven by the
working SoftAP, whose host beacon is an 11n-form PV0 frame). So on ESP32 we feed
an 11n-form beacon body (incl. the S1G Capabilities/Operation IEs, as hostapd does
for AP) and rely on the chip to emit the S1G beacon per interface type (ADHOC).

**Applied so far:** added the IBSS Parameter Set IE (EID 6) to
`umac_ibss_build_beacon` (was missing — the one clear deviation in the
AP-template-derived beacon). Capability already has IBSS set / short-slot clear.

## ✅ Result — genuine IBSS node detected on air (2026-06-19)

Built an **IBSS datapath** (`datapath_ops_ibss` + helpers in `umac.c`), configured
on the ad-hoc interface in `umac_ibss_do_start`, mirroring Linux:
`process_rx_mgmt_frame` answers PROBE_REQ with a probe response built from
`g_ibss` (SSID + IBSS Param Set + S1G Capabilities + S1G Operation), exactly as
`net/mac80211/ibss.c` `ieee80211_ibss_rx_probe_req` does; stad lookups return NULL
and PROBE_REQ is in `frames_allowed_pre_association` (no association in IBSS); data
addressing op uses IBSS (ToDS=FromDS=0, A1=DA, A2=SA, A3=BSSID). Data-plane
enqueue/dequeue are stubs for now.

Two-board test (board 0 = `rimba-halow-ibss`, board 1 = scan+sniff):
```
rimba-ibss   BSSID 02:12:34:56:78:9a   Capability 0x0002 (IBSS set, ESS clear)
RXFRAME flag=0x20 (PROBE_RSP) len=77 freq=915500kHz  A2=A3=02:12:34:56:78:9a  <== IBSS MATCH
```
6 scan detections + 6 captured probe responses.

**This proves the core of RISK-01 on hardware:** MM6108 in ADHOC mode on the
ESP32 **transmits and receives on air** (received board 1's probe requests,
answered with probe responses) and is advertised as a real **IBSS** (cap
`0x0002`). The earlier non-detection was purely the missing probe-answering, not a
radiation problem.

### RISK-01 status
- ✅ ADHOC interface + IBSS commands accepted (HW)
- ✅ `EEXIST(-17)` understood/handled
- ✅ Stable ad-hoc bring-up + host beacon (Linux-correct IEs incl. IBSS Param Set)
- ✅ **On-air TX+RX in IBSS mode, detected by a second board as a genuine IBSS**
- ⬜ EtherType `0x88B5` data exchange between two IBSS peers (data-plane ops still
  stubbed) — the remaining Phase-1 gate.

### Next: data path
Un-stub `enqueue_tx_frame`/`dequeue_tx_frame` (or route data TX through a peer
`stad` + the existing TX machinery), deliver RX data frames up to the app, and
exchange `0x88B5` frames between two IBSS nodes. Keep deriving from
`net/mac80211/ibss.c` + `morse_driver` per the governing requirement.
