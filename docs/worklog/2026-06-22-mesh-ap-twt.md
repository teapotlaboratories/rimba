# Worklog — 2026-06-22 — Mesh+AP concurrency + TWT leaf power-save (HaLow AP/STA)

**Author:** Aldwin
**Phase:** exploring an L2 architecture alternative to IBSS for Rimba leaves/relays.
**Goal:** verify the MM6108 can run **802.11s mesh + AP concurrently**, get a real
**TWT** (Target Wake Time) power-save agreement to an ESP32 STA, and rebuild
`wpa_supplicant_s1g` with mesh. All three done on hardware.
**Status:** complete. chronium ran AP + mesh-point + a TWT'ing ESP32 STA all at once.

Self-contained record. Hardware: chronium = Raspberry Pi 5 + Seeed Wio-WM6180 (MM6108,
`wlan1`), morse kernel 6.12.21 / driver+fw+cli 1.17.8. ESP32 = XIAO ESP32-S3 + FGH100M
(`BOARD=proto1-fgh100m`), morselib fw 1.17.6.

---

## Why this matters (the architecture signal)

The IBSS power-save analysis ([`../design-specification/rimba-mm6108-powersave-analysis.md`](../design-specification/rimba-mm6108-powersave-analysis.md))
showed IBSS leaves have **no good chip power-save**: no TWT, no ATIM, no AP to buffer
downlink — so the only sub-µA path was an **RTC cold-boot** every wake (≈1.39 s rejoin tax,
measured — see [`2026-06-21-phase1-validation-complete.md`](2026-06-21-phase1-validation-complete.md) /
dev-plan RISK-02).

This session shows that dead-end **dissolves if leaves are STAs under a relay-AP**: TWT
gives scheduled wake with the **AP buffering downlink**, so the leaf dozes *and* doesn't
miss traffic. Relays mesh together via 802.11s. That's the **Mesh+AP "gateway" topology** —
RISK-01 *Fallback A* in the dev plan, here shown doing what IBSS structurally cannot.

Trade-off: leaves now depend on an always-on relay-AP in range (vs symmetric IBSS peers),
and it's a larger departure from the current IBSS foundation. Not a decision — a signal.

---

## 1. Mesh + AP run concurrently on one MM6108 (chronium)

The loaded morse driver advertises the combination directly:
```
valid interface combinations:
   * #{ managed, AP, mesh point } <= 2, total <= 2, #channels <= 1
```
So one radio can hold **AP + mesh-point** (≤2 vifs), but **only co-channel** (`#channels<=1`).
Verified live: `wlan1` = AP (`rimba-ping`, SAE, ch27/915500 kHz/1 MHz) **and** `mesh0` =
802.11s mesh point (`rimba-mesh`), both on `phy#1`, chip held on 915500/1 MHz.

### Recipe + the gotchas that cost iterations
1. **AP first** via `hostapd_s1g` (S1G channel config sets ch27); assign AP IP.
2. `iw … interface add mesh0 type mp` **silently creates it as `managed`** — you must then
   `ip link set mesh0 down; iw dev mesh0 set type mp` to actually get a mesh point.
3. Give `mesh0` a **distinct locally-administered MAC** (`3e:22:7f:…` vs wlan1's `3c:22:7f:…`)
   or `ip link set up` fails with *"Name not unique on network."*
4. `iw dev mesh0 mesh join rimba-mesh freq 5560` — **bare `freq`, NO `HT20`.** `HT20`
   forces a 2 MHz S1G width → the shared radio jumps to 919000/2 MHz (`#channels<=1`),
   **killing the AP**. Bare `freq 5560` = 1 MHz, co-channel with the AP. (5560 = S1G ch27 in
   the morse 5 GHz model; on-air 915.5 MHz — same mapping the IBSS join used.)

A peerless mesh point shows `NO-CARRIER`/`DORMANT` (normal — carrier comes up on first peer
link); `iw dev mesh0 mpath dump` returns the path table, confirming it's a live mesh iface.
A second mesh node wasn't available (only chronium has a Linux MM6108; ESP32s can't mesh —
morselib has only the bare `MORSE_CMD_INTERFACE_TYPE_MESH=5` enum, no 802.11s code).

## 2. TWT power-save to an ESP32 STA — WORKING

Modified `firmware/rimba-halow-sta` to request TWT before associating (morselib requires it
before `mmwlan_sta_enable`):
```c
struct mmwlan_twt_config_args twt = MMWLAN_TWT_CONFIG_ARGS_INIT;
twt.twt_mode = MMWLAN_TWT_REQUESTER;
twt.twt_wake_interval_us = 5000000;     /* 5 s service period */
twt.twt_min_wake_duration_us = 65536;   /* ~65 ms wake */
twt.twt_setup_command = MMWLAN_TWT_SETUP_REQUEST;
mmwlan_twt_add_configuration(&twt);     /* before mmhalow_connect */
```

**AP side needed no change** — `enable_twt` is a morse driver module param defaulting to
**true** (and `morse.conf` doesn't disable it), so the AP already advertises the S1G TWT
responder (`S1G_CAP8_TWT_RESPOND`, set in `morse_driver/mac.c` when `enable_twt`). hostapd's
`he_twt_responder` is the 11ax field — *not* the S1G path; left unset.

**Evidence the agreement is live (the ping cadence is the proof):**
- ESP32: `TWT add_configuration (requester, 5s/65ms) -> 0 (0=OK)`, then STA link up + IP.
- Replies **cluster in ~5 s bursts** with timeouts between — the STA dozes, the AP buffers
  downlink, and each wake window delivers the held ICMP replies together:
  - first reply seq=9 @ **126 ms** (buffered-delivery latency on wake),
  - burst seq 12–16 within ~50 ms @ t≈28.5 s,
  - burst seq 19–23 @ t≈33.5 s — **exactly 5 s later** = the TWT service period.
  In-window RTT ~9–13 ms; pings issued mid-doze (1 s ping vs 5 s wake, 2 s timeout) expire.
- AP side: STA `68:24:99:44:6b:b7` `authorized`, −39 dBm, rx/tx counters advancing;
  `morse_cli -i wlan1 twt {conf|remove}` exists.

This is the chip-managed leaf power-save IBSS can't provide. (No radio-rail current measured
this round — datasheet Snooze ≈42 µA is the expected dozing draw; timing/behaviour confirmed.)

## 3. `wpa_supplicant_s1g` rebuilt with mesh

chronium's `wpa_supplicant_s1g` had never been built (§6 of
[`../reference/rimba-linux-node-setup.md`](../reference/rimba-linux-node-setup.md): deferred — DPP hit an
OpenSSL-3 `EC_KEY` deprecation-as-error). Rebuilt it: enabled `CONFIG_MESH=y`, **disabled
`CONFIG_DPP/DPP2/DPP3`** to dodge that error. Clean build, **55 mesh symbols**, installed to
`/usr/local/bin/wpa_supplicant_s1g`. Secured (SAE) 802.11s mesh is now possible (the §1 mesh
above used open `iw` mesh; SAE mesh needs this binary).

## 4. TWT between two ESP32s (one AP, one STA) — does NOT work; here's why + how to fix

**Test (2026-06-22):** chronium radio stopped; ACM1 = ESP32 AP (`rimba-halow-ap`, `rimba-ping`),
ACM0 = ESP32 STA (TWT requester). The STA associated and requested TWT (`add_configuration ->
0`), but the **ping pattern was flat — every 1 s, ~10–20 ms, no 5 s bursts, no dozing**
(contrast the chronium-AP run's 5 s reply-clusters). So **no TWT agreement formed: the ESP32
AP is not a TWT responder.** It degrades gracefully — the request is silently unanswered, the
link works, just with no power-save.

**Root cause (morselib host stack, requester-only):**
- `umac/twt/umac_twt.c` `umac_twt_init_vif()` hardcodes `data->requester = true; data->responder
  = false;`
- `umac/ies/twt_ie.c:16` builds the TWT IE **only** for `MMWLAN_TWT_REQUESTER` (early-returns
  otherwise) — no responder IE.
- The agreement-install path (`umac_twt_install_pending_agreements`) is driven from the **STA
  connection** path (`umac_connection.c`); there is no AP-side handler for an incoming TWT
  setup-request action frame, and no responder response/install path.

**This is NOT a silicon limit — the firmware supports responder TWT.** The ESP32 firmware
command surface has `MORSE_CMD_ID_TWT_AGREEMENT_INSTALL = 0x0026` (+ `_REMOVE 0x0027`,
`_VALIDATE 0x0036`) with `struct morse_cmd_req_twt_agreement_install` — **the same command the
Linux `morse_driver` uses** (`command.c: morse_cmd_twt_agreement_install_req` →
`MORSE_CMD_ID_TWT_AGREEMENT_INSTALL`). The firmware does the heavy lifting (downlink buffering +
service-period scheduling) once an agreement is installed.

**How to make the ESP32 AP a TWT responder (a morselib patch, "follow Linux"):** mirror the
Linux driver's responder path (same chip) —
1. **Advertise the capability:** set `S1G_CAP8_TWT_RESPOND` in the AP's S1G Capabilities IE
   (beacon/probe-resp/assoc-resp), as the Linux driver does at `morse_driver/mac.c:1275-1276`
   gated on the `TWT_RESPONDER` capability. Without this the STA won't even ask the AP.
2. **Set `responder = true`** for the AP vif in `umac_twt_init_vif` (today hardcoded false).
3. **Handle the incoming TWT setup REQUEST** action frame on the AP vif (parse via the existing
   `umac_twt_process_ie`), decide the agreement params, and **send a TWT setup RESPONSE** action
   frame — add a responder branch to `twt_ie.c` (mirror Linux `morse_mac_send_twt_action_frame`,
   `twt.c:367`).
4. **Install the agreement to the firmware** via `MORSE_CMD_ID_TWT_AGREEMENT_INSTALL (0x0026)`
   with the negotiated params + the STA's AID/MAC. Derive the 20-byte agreement-blob format from
   the Linux driver (`morse_driver/command.c:2179` `morse_cmd_twt_agreement_req`). The firmware
   then buffers downlink for the dozing STA and schedules the service period.

Effort: a bounded morselib patch (the requester plumbing + IE parsing exist; the responder is the
mirror image, and the firmware install command is already present). Risk is mostly getting the
agreement-blob format + action-frame exchange right — both readable in the Linux driver source on
chronium (`~/halow/morse_driver/twt.c`, `command.c`). Not attempted this session.

**Implication:** for the Mesh+AP architecture, **TWT leaf power-save works only when the AP is a
Linux node** (or a TWT-responder-patched ESP32). An all-ESP32 AP+STA pair gets association + data
but **no TWT** until the responder role is ported. If relays are Linux (Pi-class) gateways
anyway, this is moot; if relays must be ESP32, the responder patch is the unlock.

### 4a. ESP32 AP TWT-responder implementation (in progress, strictly follows morse_driver)

morselib has no AP-side mgmt MLME (the bundled hostapd builds assoc-resp); the Linux driver does
the whole TWT responder in the *driver* — parses the STA's TWT IE from the assoc-req RX hook
(`mac.c:6405` → `morse_mac_process_rx_twt_mgmt`), splices the accept IE into the assoc-resp on TX
(`mac.c:1861`), and installs the agreement to firmware on STA-authorized (`mac.c:5024`). The
faithful port keeps it all in morselib's driver layer. Five pieces:

**DONE (compiles, low-risk foundation):**
1. **Advertise responder cap** — `umac/ies/s1g_capabilities.c` `ie_s1g_capabilities_build_ap`: set
   `DOT11_S1G_CAP_INFO_8_SET_TWT_RESPONDER_SUPPORT` when `MORSE_CAP_SUPPORTED(…, TWT_RESPONDER)`
   (mirrors `morse_driver/mac.c:1275`). So a requesting STA now includes its TWT IE in the assoc-req.
2. **Enable responder for AP vif** — `umac_twt_init_vif()` gained an `is_responder` arg
   (`requester = !is_responder`); `umac_interface.c` now calls it for `UMAC_INTERFACE_AP` with
   `true` (gated on the fw responder cap), and `false` for STA. Mirrors `morse_twt_init_vif`.

**REMAINING (the negotiation core — larger, AP-path, needs test iteration):**
3. **Parse + accept policy** — in the AP assoc-req handler (`umac_connection_process_assoc_req`,
   `supplicant_shim/driver.c:697`), find the STA's `dot11_ie_twt`; run the Linux policy
   (REQUEST/SUGGEST → accept, DEMAND/GROUPING → reject, dup flow_id → reject); build an accept
   agreement (params as-is, `setup_cmd = ACCEPT(4)`, clear request bit, recompute mantissa,
   optional non-overlap target wake time). New responder routine mirroring `morse_twt_send_accept`.
4. **Splice accept IE into the assoc-resp** — in `umac_datapath_tx_mgmt_frame_ap`
   (`supplicant_shim/driver_ap.c:449`), the `mac.c:1861` equivalent: detect assoc-resp to the STA
   and insert the responder TWT IE. (morselib lacks a clean extra-IE hook, so this is buffer
   surgery on the outgoing mgmt frame — the trickiest part.)
5. **Install to firmware on authorized** — reuse `umac_twt_install_agreement` →
   `mmdrv_twt_agreement_install_req` (`MORSE_CMD_ID_TWT_AGREEMENT_INSTALL = 0x0026`, present in the
   ESP32 fw). The fw then buffers downlink + schedules the service period.

### 4b. IMPLEMENTED — and BLOCKED by a firmware gate in mm6108.mbin 1.17.6

All five pieces were implemented in morselib (capability advertise in `s1g_capabilities.c`;
responder enable in `umac_twt_init_vif`/`umac_interface.c`; RX parse+accept, TX response-IE
build, and firmware install in `umac_twt.c`; RX hook in `supplicant_core.c`; TX-splice +
install hooks in `driver_ap.c`). Built clean and **the negotiation works end-to-end on a
2-ESP32 bench**:
- AP log: `accepted agreement for 68:24:99:44:6b:b7` → `inserting accept IE into assoc-resp`
  → install hook fires (`state=2 peermatch=1`).
- STA: gets the ACCEPT and **dozes** (pings go from steady-1 s to timeouts — it sleeps).

**But the firmware rejects the agreement install on the AP vif.** Raw return = `-32768`
(`0xffff8000`). Decompiling mm6108.mbin 1.17.6 (the dispatcher case `0x26` =
`MORSE_CMD_ID_TWT_AGREEMENT_INSTALL`) shows the cause:
```c
case 0x26:                                   // TWT_AGREEMENT_INSTALL
  if (vif_id < 2) {
    if (vif[vif_id].interface_type == 1) {   // *** STA only ***
      ... link state, then twt_agreement_install(...) ...
    } else { iVar20 = -0x8000; }             // AP/ADHOC/etc -> 0xffff8000
  }
```
The firmware **hardcodes TWT-agreement-install to `interface_type == STA(1)`**; an AP vif
(type 2) returns `0xffff8000`. The chip *advertises* `TWT_RESPONDER` capability (`TWTCAP
resp=1`) but the install handler won't honor it for an AP. So the responder negotiates fine
(the STA dozes) but the firmware never schedules the service period → the dozing STA's
downlink is never delivered → all-timeouts.

**Conclusion:** TWT *responder* is **not possible on the ESP32 with firmware 1.17.6** — it is
a firmware-level gate, independent of host code. This is exactly why ESP32-STA ↔ **chronium**-AP
TWT works (chronium runs fw **1.17.8**, whose driver does AP TWT responder) but ESP32-AP ↔
ESP32-STA cannot. **To make it work:** an ESP32 mm6108 firmware that allows AP-vif TWT install
(≥1.17.8 if it lifts the gate — needs validation against morselib `2.10.4`), or keep the AP on
a Linux node. The host-side responder is complete and Linux-faithful, and will work as-is on a
firmware that permits the AP install.

**Caveat (host):** morselib's `umac_twt_data` holds a *single* per-vif agreement
(`UMAC_TWT_NUM_AGREEMENTS = 1`), not Linux's per-STA list — fine for the 1-STA bench, but a
multi-STA responder needs per-STA agreement storage.

### 4c. Firmware investigation — newer fw exists but does NOT lift the gate (DEFINITIVE)

Corrected an earlier wrong claim: a newer **embedded MBIN** firmware *does* exist.
- **mm-iot-sdk 2.11.2** (GitHub `main`, 2026-05-01) bundles `framework/morsefirmware/mm6108.mbin`
  = **`rel_1_17_8`** (MBIN format, `MMFW` magic, 399796 B). The ESP component registry
  (`morsemicro/halow`) just lags at 2.10.4-esp32-2 / 1.17.6. (1.17.9 exists only as a Linux ELF
  `.bin` — no MBIN.)

**Swapped 1.17.8 `.mbin` onto the ESP32 AP and re-tested — the install STILL returns `0xffff8000`.**
So the embedded firmware gates `TWT_AGREEMENT_INSTALL` (cmd 0x26) to `interface_type==STA` in
**both 1.17.6 and 1.17.8**. Newer embedded firmware does not help. (Also: fw 1.17.8 + our forked
morselib **2.10.4** is ABI-unstable — association degraded badly — because 1.17.8 pairs with
morselib **2.11.2**; restored to 1.17.6.)

**The firmware is NOT the differentiator — proven byte-identical.** Compared chronium's Linux
`mm6108.bin` (ELF) against the embedded `mm6108.mbin` (MBIN), both `rel_1_17_8_2026_Mar_24`:
`host_imem` and `mac_imem` (the MAC code that *contains* the 0x26 TWT handler + STA gate) and
`uphy_imem` are **byte-identical** (sha256 match; only `mac_rodmem`/`lphy_imem` data/PHY-cal differ).
So the Linux `.bin` is the **same firmware image** as the embedded `.mbin`, just a different
container (ELF vs MBIN). chronium's firmware therefore *also* gates the AP-vif TWT install
(`0xffff8000`) — yet chronium serves TWT. **Conclusion: chronium does AP TWT responder entirely in
the HOST** (Linux `mac80211`/`morse_driver` AP power-save — buffer the dozing STA's downlink,
deliver at its service period via host timers), NOT via a firmware install. That host machinery is
what morselib lacks, matching Morse staff: AP power-save is **Linux-driver-only, "not in the
MM-IoT-SDK," "not on the roadmap"** (community.morsemicro.com t/1638, t/1667).

**Can the Linux `.bin` be loaded on the ESP32?** No useful path: (1) format — ESP32 needs MBIN
(`MMFW`), `.bin` is ELF; (2) even converted ELF→MBIN it's the **same gated firmware bytes**, so it
changes nothing. The firmware-install step in this responder is a **red herring** — Linux's install
is gated too; Linux serves in the host.

**CONCLUSION (superseded by §4d — IT WORKS):** the gap is host-side AP-TWT serving in morselib.
That turned out to be a small, specific fix, not a big SP-scheduler port — see §4d.

### 4d. ✅ BREAKTHROUGH — ESP32 AP TWT responder WORKS (host-side, stock 1.17.6 fw)

The "host-side AP-TWT serving" was a **~7-line gap**, not a major feature. Diagnosis (a temp PM-bit
trace at `umac_datapath.c:1528`): a dozing TWT STA behaves correctly — it bursts frames at its SP
(PM=0 awake → … → PM=1 doze), and the AP **does** buffer its downlink (the umac_ap PS path) and
tracks sleep state from the PM bit. The bug: `umac_ap_set_stad_sleep_state()` cleared the STA's TIM
bit on wake but **never kicked the TX loop**, so buffered downlink waited for the next beacon/DTIM
instead of being delivered during the STA's (short) open SP → the STA timed out.

**Fix (`umac_ap.c`, in `umac_ap_set_stad_sleep_state`):** when a STA transitions asleep→awake and
has queued frames, call `umac_core_evt_wake(umacd)` to flush its buffered downlink NOW — mirroring
the group-addressed release in `umac_ap_get_beacon()`. (The normal TX path already wakes the loop at
`umac_datapath.c:1978`; the PS-wake path was the one place it was missing.)

**Result:** TWT leaf power-save works end-to-end on the **ESP32 AP**, on **stock fw 1.17.6, no
firmware change**. STA dozes (TWT), AP buffers downlink, delivers at the SP. Test (1 s TWT interval,
2 s ping timeout): **18–23 replies, 0 timeouts, ~15 ms RTT.** At longer intervals the downlink
latency = the interval (the inherent TWT trade-off; fine for a leaf that wakes every N minutes).

**The firmware-install gate (cmd 0x26 → STA-only) is a red herring.** The install fails on the AP
vif but is **unnecessary** — host-side buffering+flush delivers the SP traffic. (This mirrors Linux,
whose firmware install is gated the same way and which also serves host-side.) `umac_twt_responder_install`
now treats the gated install as expected/harmless.

**So: ESP32-as-TWT-responder-relay IS achievable.** A relay-AP can give its leaf STAs TWT
power-save with the morselib responder (§4a) + this flush-on-wake fix — no firmware change, no Linux
node required. (Caveats still open: single-STA agreement slot — multi-STA needs the per-STA list;
current is verified only on the bench with a ping workload; the STA's TWT interval is app config.)

**⚠ Side effect of the responder code on the gated firmware:** because the ESP32 AP now *advertises*
TWT responder and *accepts* requests, a requesting STA **dozes** but the AP can't serve it (install
gated) → the STA's traffic is degraded vs a plain AP. So the responder code should be **reverted or
its capability-advertise disabled** until an embedded firmware supports AP install.

## Commands (reference)

chronium Mesh+AP+TWT bring-up scripts live at `/tmp/mesh_ap_test.sh`, `/tmp/mesh_join2.sh`
on chronium (and locally). ESP32: `make flash APP=rimba-halow-sta BOARD=proto1-fgh100m`.

## State at end of session
chronium: AP (`wlan1`) + mesh point (`mesh0`) + TWT'ing ESP32 STA, all concurrent. ESP32
ACM0 = TWT STA app; the only repo change is the TWT block added to `rimba-halow-sta`. No
commits.
