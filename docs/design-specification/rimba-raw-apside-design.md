# RAW (Restricted Access Window) — AP-side port: feasibility + design recon

**Status:** recon **DONE 2026-07-24**; **S0a DONE** (`MORSE_CAPS_RAW=1`); **S0b-0 / S0b-1 / S0b-2 DONE 2026-07-30.**

> ## ✅ THE GATE IS PASSED — Q3a and Q4 both answered positively on hardware, with zero ESP code
>
> **The MM6108 firmware's RAW engine arms from a host-authored beacon and actively gates traffic, and a
> STA parses a received RPS and self-restricts.** Measured on the all-Linux rig 2026-07-30 (S0b-2, §7.2):
> the AP's `assignments[0]` went 0 → **2179** with `invalid_assignments = 0`, and **both ends** deferred
> their own frames — `aci_frames_delayed` **92** (AP) and **239** (STA), `frame_crosses_slot` 27 on each.
> The golden RPS constant was validated **on air, byte-identical, in 236/236 beacons**.
>
> **What this retires:** the [BLOCKING] risk in §5, and the "STA-side RAW is believed working but is
> unmeasured anywhere in this repo" caveat. Both are now measured.
>
> **What remains is entirely ESP-host-side** — Q2 (does a host-appended RPS survive the ESP FW's
> legacy→S1G beacon re-conversion?) and Q3b (does the engine arm when the *ESP* is the AP?). Those are
> S0b-3/4/5 and they need ESP code. **The feature is no longer gated on a source-undecidable firmware
> question.**

*Port not started.* This is a
**design-recon deliverable, not the implementation.** Verdict: **GO-WITH-CAVEATS** — the advertise-half
(author + insert the RPS IE into the beacon) is a tractable host-side IE port modeled directly on the
802.11s mesh RANN (just shipped) and the existing S1G-TIM builder; the enforce-half (per-slot medium
gating) lives in the MM6108 firmware and **cannot be reproduced on the host** — so the feature's viability
rests entirely on whether that firmware enforces *AP-direction* RAW slots on-air, which source cannot
decide. The S0 spike (§3) settles it.

**Method:** synthesized from a 3-angle source recon — the Linux `morse_driver` RAW machinery
(`raw.c` 1742 lines + `page_slicing.c` + `raw.h`, scp'd from the bench), the current morselib/MM6108-FW
RAW surface (`components/halow/.../morselib/src`), and the ESP32 AP beacon path (`umac/ap/umac_ap.c`).
Every claim carries a `file:line` anchor grepped in the source. Follow-Linux, byte-diffable,
on-air-verifiable, per the code-map porting rule (`.ai/AGENTS.md` → Porting Linux code). Backlog item:
`docs/mesh-ap/rimba-mesh-ap-milestones.md` (RAW, AP-side).

---

# RAW (Restricted Access Window) — AP-side Feasibility + Design Recon

Synthesis of three source maps: (1) Linux `morse_driver` RAW machinery, (2) morselib/MM6108-FW hooks, (3) ESP32 AP beacon path. Follow-Linux, byte-diffable, on-air-verifiable. Every claim carries the originating map's file:line anchor.

---

## 1. FEASIBILITY VERDICT: **GO-WITH-CAVEATS** (advertise-half GO; enforce-half spike-gated)

**Load-bearing reason.** The RAW architecture splits cleanly into a *control plane* (author the RPS IE, insert it into the beacon) and an *enforcement plane* (microsecond-granular per-slot medium-access gating). The control plane is a pure host-side byte-assembly + IE-insertion job that is mechanically tractable and low-risk on ESP32 — it reuses the exact host-built-IE mechanism the mesh already ships. The enforcement plane lives entirely in the MM6108 firmware/PHY, is not exposed by any morselib chip command, and **cannot be reproduced on the host at all**. So the whole port's value hinges on one binary question that source cannot answer.

**THE key question — is enforcement in the FW?**

The evidence says the *design intends* FW enforcement, and there is a strong positive signal, but it is not yet proven for the AP direction on the ESP image:

- **The Linux driver does not enforce — it only authors bytes.** Grep of `raw.c` for every scheduling primitive (`hrtimer`, `mod_timer`, `stop_queue`/`wake_queue`, `netif_*`, `ieee80211_tx`, `nav`, `edca`, `ktime`, `udelay`) returns nothing; the only thing it ever schedules is `schedule_work(&raw->update_work)` to *regenerate the IE* (Map 1 §0, `raw.c:1140,1595,1611`). Enforcement is therefore below the driver, in the chip (Map 1 §3).
- **STA-side RAW is already fully working in the ESP firmware with zero host slot-scheduler code.** morselib ships `raw_sta_priority` (Map 1 §7, `mmwlan.h:1101`; Map 2 Q1b), and its only host effect is setting one S1G-caps bit (`s1g_capabilities.c:134-139`). There is no RPS parser and no slot self-restriction anywhere in host morselib (Map 2 Q1b) — yet the feature functions, which *only* makes sense if the MM6108 blob parses RPS and enforces the STA's slot in-chip. This is the strongest evidence the enforcement engine exists in the shipped image (Map 1 §7, Map 2 Q2).
- **The ESP and Linux beacon paths are closer than this doc originally claimed — CORRECTED 2026-07-30 (S0b scoping).** The original text here said Linux FW-offloads the beacon + RPS while the ESP host-builds it, and put that asymmetry "exactly on the enforcement boundary." That is wrong: **enabling RAW switches Linux's beacon offload OFF.** `morse_hw_beacon_can_offload()` returns false when `morse_raw_is_enabled(mors_vif)` (`hw_beacon.c:398`, `:415-416`), and `WLAN_EID_S1G_RPS` is absent from the offload IE allowlist (`hw_beacon.c:17-24`). So with RAW on, **both** hosts build and serve the beacon every TBTT, and **neither sends the chip any RAW command** — the RPS element in the beacon is the firmware's only RAW input, on both platforms. **CORRECTED AGAIN 2026-07-30 (source-verified):** the residual difference is narrower still — in fact there may be none. This doc claimed the ESP hands the chip a **legacy PV0** beacon that the FW converts for AP-type vifs, citing `umac_mesh.c:214-218`. **That anchor is a comment, and the code disagrees.** The ESP's hostapd builds a **finished S1G beacon** — `ext_head->frame_control = IEEE80211_FC(WLAN_FC_TYPE_EXT, WLAN_FC_STYPE_S1G_BEACON)` (`src/hostap/src/ap/beacon.c:2357`) and `params->head = (u8 *) ext_head` (`:2673`) — and **there is no legacy→S1G conversion layer ESP-side at all**: `grep -rIl -e ies_mask -e mask_ies -e 11n_to_s1g components/halow` returns **zero files**. Both platforms therefore hand the chip a finished S1G beacon; the ESP simply builds it in hostapd rather than in a `dot11ah.ko` equivalent. **So Q2's original framing describes a mechanism that does not exist on the AP path.** What remains genuinely open is narrower and must not be overstated: *does the MM6108 blob rewrite IEs inside a host-supplied beacon?* Source cannot answer that on either platform — only the capture can.
- **No CONFIG_RAW chip command exists in morselib.** The morselib chip-protocol enum `enum morse_cmd_id` has no RAW opcode (Map 2 Q2, `morse_commands.h:15-99`); `MORSE_CMD_ID_BSS_BEACON_CONFIG=0x003D` carries only an `enable` flag, no beacon body/IE payload (`:379-383`). `MORSE_CMD_ID_CONFIG_RAW=0xA021` exists *only* in hostapd's vendored copy of the Linux protocol and is sent *only* by `nl80211_raw_priority_enable()` in `driver_nl80211.c` — **which is not compiled for ESP** (Map 2 Q2, `CMakeLists.txt` builds only `drivers_morse.c` + `driver_common.c`). The compiled-in hostapd RAW code (`hostapd_get_raw_aid`, `hostapd_send_raw_config`) is a dead no-op: it guards on `driver->raw_global_enable`/`raw_priority_enable`, which `mmwlan_wpas_ops_ap` does not define, so it logs "Driver interface not defined" and returns (Map 2 Q1c, `hostapd.c:2148-2151,2167-2171`; `driver_ap.c:767`).

**What this resolves to:** if the RPS IE is a *beacon-borne advertisement the FW enforces* (like it evidently is for STAs), the port is a tractable host-IE feature directly analogous to the mesh RANN — **GO**. If AP-direction enforcement turns out to be absent from the ESP `.mbin` (only STA-direction present), then RAW is **BLOCKED**: it would need host-side airtime scheduling the chip does not expose (no host TSF/TBTT clock, no medium-access gate — Map 3 §3, `umac_ap.c:376-389`, `umac_mesh.c:227`) and an FW change we cannot make.

**What remains an on-air-spike unknown (source-undecidable):**
1. **Q1 — ✅ ANSWERED by S0a (2026-07-30).** Does the shipped ESP `.mbin` advertise `MORSE_CAPS_RAW` / `MORSE_CAPS_PAGE_SLICING`? **Both set on fw 1.17.8.** Necessary, not sufficient — the capability word carries no direction.
2. **Q2 — RE-FRAMED 2026-07-30.** Originally *"does the RPS survive the FW's legacy→S1G beacon re-conversion?"* — but **there is no such conversion on the ESP AP path** (see §1). The real question is simply: does a host-inserted RPS reach air intact from the ESP, byte-matching a live Linux RAW AP? The only way it could fail is the blob rewriting IEs in a host-supplied beacon, which is unknown on both platforms.
3. **Q3 — RE-SPECIFIED 2026-07-30 (S0b scoping).** As originally written ("does the AP-direction FW-MAC *confine* a RAW-assigned STA to its slot?") this names a mechanism that may not exist: 802.11ah RAW is honoured by the **STA restricting itself**; there is no AP policing primitive. The firmware's own RAW instrumentation agrees — every field of `raw_stats_t` counts the **local** transmitter deferring its **own** frames (`morse_cli/stats_format.h:81-99`: `aci_frames_delayed`, `bc_mc_frames_delayed`, `abs_frames_delayed`, `frame_crosses_slot_delayed`), and there is no "peer violated RAW" counter anywhere. A GO/BLOCKED gate written against AP-policing would return **BLOCKED for a feature that works as specified**. Split it:
   - **Q3a — does the chip's RAW engine ARM at all when the RPS comes from a host beacon?** Observable: `assignments[]` in the tag-4210 `raw_stats_t` moving on the AP, and `invalid_assignments` staying flat. This is a property of the **shared blob** (see below), so it is answerable on Linux hardware with zero ESP code.
   - **Q3b — does it arm when the *ESP* is the AP?** Needs Q2 to pass first.
4. **Q4** — does the MM6108 STA firmware honour a received RPS for UL self-restriction? Observable: the **STA's own** `aci_frames_delayed` incrementing, corroborated by uplink timing in the capture. Answered as a by-product of the all-Linux phase.

**The blob is the same file on both platforms, and that is what makes the cheap ordering possible.** `md5sum vendor/morse-firmware/firmware/mm6108.bin` = `cfe56db2706458050dcc60baddabc632` (480664 B) = `chronium:/lib/firmware/morse/mm6108.bin`; the ESP build converts *that* ELF to a `.mbin` at build time (`cmake/mm-fw-gen/firmware/CMakeLists.txt:37,51-57,77-82` — nothing firmware-related is committed under `components/`, so the SDK-bundled `mm6108.mbin` there is inert). Combined with "RAW reaches the chip only as beacon bytes," **Q3a and Q4 are properties of a binary the Linux bench already runs.** The residual is whether the TLV+deflate repack is lossless for the executable segments — strong-but-not-conclusive, which is why the ESP arm still confirms.

The spike (S0 below) settles all four before any port commitment.

---

## 2. MINIMUM-VIABLE SCOPE

**MVP — the smallest useful first cut:**
- **One static GENERIC RAW assignment covering all connected AIDs** — a single Group `[start_aid=1 … end_aid=max]`, Page Index 0 (Map 1 §1e-2). The Linux driver itself only ever emits GENERIC type (`raw.c:1070`, WARN_ON others `raw.c:394-398`), so this is byte-faithful, not a shortcut.
- **Slot Definition format 0 (8-bit count / 6-bit slots)**, small fixed slot count (e.g. 2), duration via `duration_us = 500 + cslot·120` (Map 1 §1d, `CSLOT_TO_US raw.c:103`). Replicate the driver's literal num_slots cap of 6 (Map 1 §1d, footgun §7).
- **RAW Start Time = 0** (omit the optional byte), **no Channel Indication** (never emitted — Map 1 §1e-3), **no Periodic/PRAW** (non-periodic only).
- **Static config only** — no smart-manager dynamic path (`morse_raw_process_dynamic_cmd raw.c:1435`).
- **No page slicing** — orthogonal, separately portable (Map 1 §6, gated on `dtim_period>1` + cap, `page_slicing.c:409-434`).
- Advertise the AP S1G-caps RAW bit (flip the `if (false)` at `s1g_capabilities.c:276-280`, Map 2 Q1b / Map 3 §2).

This MVP is exactly enough to run S0 and to gate a RAW-capable STA into a single window — the mesh-gate scheduling use case's atom.

**Full feature (deferred until MVP is on-air-proven):**
- Per-priority AID grouping — QoS-UP→AID bits[10:8] (Map 1 §2, `raw.h:20-27`; learned from QoS Traffic Cap IE `raw.c:906-907`), up to 8 user-priority RAWs + 1 internal (`MAX_NUM_RAWS=9`). The AID-decomposition machinery already exists ESP-side (Map 3 §2, `s1g_tim.c:10-31`).
- Multiple RAW assignments concatenated in one RPS IE (`configs_list[MAX_NUM_RAWS]` Map 1 §4).
- Beacon spreading / dynamic cycles (`raw.h:199-213`, `raw.c:499-604`) — the RPS-side analog of TIM page slicing; requires the beacon-TX-complete hook to age state (Map 1 §3, `morse_raw_beacon_sent raw.c:1585-1597`).
- Periodic RAW (PRAW) — 3-byte optional + `dtim_period×10` retransmit cadence (Map 1 §1e-4, §4, `raw.c:135,715-734`).
- Page slicing (Map 1 §6) as its own follow-on port.

---

## 3. STAGED PLAN S0..S6 (mesh-gate style, compile-verify each stage)

**S0 — FEASIBILITY SPIKE (the true gate; blocking).** → **Split into S0a/S0b; the executable plan is §7.**
- **S0a — capability probe. ✅ DONE 2026-07-30.** One board, no sniffer, no peer, no beacon hook. Result
  `MORSE_CAPS_RAW=1`, `MORSE_CAPS_PAGE_SLICING=1` on fw 1.17.8 ⇒ **GO-CANDIDATE**. Necessary, not
  sufficient: the capability word carries no direction. Worklog `docs/worklog/2026-07-30-raw-s0a-capability-probe.md`.
- **S0b — the on-air spike. Scoped 2026-07-30, not yet run — §7 below.** *This paragraph's original
  sketch (build the ESP hook → then stand up the Linux reference → then measure) has been superseded: the
  gate is now an all-Linux run needing zero ESP code, and Q3 has been re-specified. See §7.*
- **Decision (as re-specified):** the chip's RAW engine arms from a host-authored beacon **and** a STA
  self-restricts ⇒ **GO**, proceed to S1. The engine never arms on **Linux** hardware ⇒ **BLOCKED**;
  vendor/FW ask. A stripped-or-mangled RPS on the *ESP* is **not** BLOCKED — it is the host-built-S1G-beacon
  fallback (the mesh pattern), i.e. a cost question. See §7.7 for the full pre-registered decision table.

**S1 — RPS payload builder (follow-Linux, byte-identical).**
Port `raw.c`'s RPS-payload assembly into a net-new `umac/ies/ie_rps.c` (sibling of `s1g_tim.c`): RAW Control octet (Map 1 §1c), Slot Definition (§1d), RAW Group hierarchical-AID packing (§1e-2). Add `DOT11_IE_S1G_RPS = 208` to `dot11/dot11.h` and a `struct dot11_ie_rps` (all net-new — Map 3 §1). GENERIC-only, format-0, single group. Compile-verify. Unit-diff the produced bytes against the S0 hand-built reference.

**S2 — EID framing + beacon splice.**
Add the `Element ID(208)+Length` wrapper (payload-only from the Linux builder — Map 1 §1a/§5 footgun; note the ESP sibling `ie_s1g_tim_build` is EID-framed, so `ie_rps_build()` should emit the header itself via `ie_build_hdr()` and this stage largely collapses into S1) and the insertion call `ie_rps_build(buf, …)` at the exact hook: **`umac_ap_build_beacon()` between line 418 and 419**, after `ie_s1g_tim_build` and before the `config.tail` append (Map 3 §1, `umac_ap.c:406-420`), using the `consbuf_reserve`/`consbuf_append` primitives modeled on the RANN branch (`umac_mesh.c:2341-2346`). Compile-verify; re-run the S0 capture to confirm the dynamically-built IE matches.

**S3 — AP config surface + enable knob.**
Add a RAW field to `mmwlan_ap_args` (absent today — Map 1 §7, `mmwlan.h:2012-2135`) and an enable/disable + config entry point mirroring `morse_raw_process_cmd` / `morse_raw_enable` (Map 1 §4, `raw.c:1559-1583,1137-1141`), AP-iftype-gated (`raw.c:1567`). Wire the AP S1G-caps RAW advertise bit on when enabled (Map 3 §2, `s1g_capabilities.c:276-280`). Compile-verify.

**S4 — AID-list + single-group mapping.**
Build the ordered connected-AID list from the ESP dense-AID allocator/bitmap (`umac_ap_alloc_sta umac_ap.c:663-688`, `data->bitmap` `umac_ap_data.h:30`, `aid_is_valid traffic_bitmap.h:27`) — the ESP analog of `morse_generate_aid_list` (Map 1 §2, `raw.c:247-272`). Map config `start_aid/end_aid` to the live range; refresh on STA join/leave (analog of `morse_raw_refresh_aids raw.c:841-871`). Compile-verify. **This is the MVP finish line** — on-air re-verify the full path.

> ⚠ **S4 AS WRITTEN ABOVE IS MISCONCEIVED — established from the reference 2026-08-02, before any code.**
> This stage assumed the connected-AID list narrows the advertised RAW group. **It does not.** In
> `morse_driver` the AID list has exactly one consumer: **beacon spreading**, which is S6. Every use of
> `raw->aid_list` and of `start_aid_idx`/`end_aid_idx` sits inside
> `if (config->beacon_spreading.nominal_sta_per_beacon …)` — `raw.c:519` in
> `morse_raw_generate_assignment()`, and `raw.c:862` in `morse_raw_refresh_aids()`, which refreshes
> indices *only* for spreading configs. The non-spreading path is explicit, with the reference's own
> comment at `raw.c:595-599`:
>
> ```c
> /* If not using beacon spreading or no connected STAs use the full AID range. */
> } else {
>     current_beacon_start_aid = config->start_aid;
>     current_beacon_end_aid   = config->end_aid;
> ```
>
> So for a plain GENERIC RAW the advertised group **is** the configured range — which is exactly what S1
> already emits. Building `morse_generate_aid_list` and the join/leave refresh now would be **dead
> scaffolding**: no on-air change, nothing to verify against, and its only consumer arriving two stages
> later.
>
> **This is also the right reading of the feature.** The RAW group expresses *which AIDs are subject to
> RAW* — a policy the integrator sets — not *which AIDs happen to be associated*. Narrowing it to the
> connected set would conflate the two and would churn the beacon on every join and leave.
>
> **Consequence: S3 is the MVP finish line, not S4.** The AID-list work moves into S6, where beacon
> spreading actually consumes it. See `docs/worklog/2026-08-02-raw-s4-is-scaffolding.md`.

**S5 — Per-priority AID grouping (full-feature).**
QoS-UP→AID bits[10:8] packing (Map 1 §2, `raw.h:20-27`), learn priority from the QoS Traffic Cap IE in (Re)Assoc Req (`raw.c:906-907`); reuse the compiled-in-but-inert `hostapd_get_raw_aid` grouping (Map 2 Q1c/Q3, `ieee802_11.c:3833`). Multiple assignments in one RPS IE. Compile-verify + on-air multi-STA.

**S6 — Beacon-cadence dynamics + PRAW (full-feature).**
Hook the beacon-TX-complete event (ESP: `morse_beacon_tx_one`/beacon IRQ, `beacon.c:55` — Map 3 spine) to the analog of `morse_raw_beacon_sent` (Map 1 §3, `raw.c:1585-1597`) to age PRAWs and advance beacon spreading (`raw.c:957-978,741-774`). Periodic RAW + `dtim_period×10` retransmit (`raw.c:135,715-734`). Compile-verify + on-air soak.

*(Page slicing — Map 1 §6 — is a parallel, independently-portable track, not on this critical path.)*

---

## 4. LINUX CODE-MAP SKELETON (fill file:line ↔ file:line during the port)

Per the repo's porting rule (the code-map porting rule (`.ai/AGENTS.md` → Porting Linux code)): every Linux-derived function ships a verified new-code↔Linux anchor, grepped in both trees, never from memory.

| Concern | Linux source (morse_driver) | ESP new-code target (to fill) |
|---|---|---|
| RPS IE size calc | `morse_raw_calc_rps_ie_size` `raw.c:378-418` | `ie_rps.c` (new) — `ie_rps_calc_size()` `:___` |
| RPS payload generate | `morse_raw_generate_rps_ie` `raw.c:616-672` | `ie_rps.c` `ie_rps_build()` `:___` |
| RAW assignment (AID range) | `morse_raw_generate_assignment_with_aid_range` `raw.c:425-488` | `ie_rps.c` `:___` |
| RAW Control octet | masks `raw.c:15-37`, write `raw.c:440-477` | `ie_rps.c` `:___` |
| Slot Definition encode | `morse_raw_generate_slot_definition` `raw.c:287-357`; `CSLOT_TO_US` `raw.c:103` | `ie_rps.c` `:___` |
| RAW Group hierarchical-AID pack | `raw.c:66-76,460-469` | `ie_rps.c` `:___` |
| EID(208)+Length framing + splice | insert `beacon.c:248-269`; store `dot11ah/ie.c:626-637`; serialise EID+LEN+payload `dot11ah/ie.c:465-476`; order tables `ie.c:29-58` (long) / `:60-69` (short) | `umac_ap.c` between `:418` and `:419` (Map 3 §1) `:___` |
| Get RPS IE / size accessors | `morse_raw_get_rps_ie` `raw.c:420-423`, `_size` `raw.c:359-367` | morselib accessor `:___` |
| AID list build | `morse_generate_aid_list` `raw.c:247-272` | reuse `umac_ap` dense-AID + `data->bitmap` (`umac_ap.c:663-688`, `umac_ap_data.h:30`) `:___` |
| AID refresh on join/leave | `morse_raw_refresh_aids` `raw.c:841-871` | hook `umac_ap_add_sta`/leave `:___` |
| AID→index binary search | `raw_bsearch_aid_indexes`/`raw_update_aid_indexes` `raw.c:784-833` | `:___` (full-feature) |
| QoS-UP→priority learn | `morse_raw_process_rx_mgmt` `raw.c:873-915` | reuse hostapd `sta->raw_priority` `ieee802_11.c:4522-4529` `:___` |
| Priority→AID group | AID bits[10:8] `raw.h:20-27` | `ie_rps.c` + `hostapd_get_raw_aid ieee802_11.c:3833` `:___` |
| Enable/disable knob | `morse_raw_enable`/`_disable` `raw.c:1137-1141`; `_is_enabled` `raw.c:1630` | new `mmwlan_ap` RAW cfg entry `:___` |
| Config command parse | `morse_raw_process_cmd` `raw.c:1559-1583`, `morse_raw_cmd_to_config` `raw.c:1062-1131` | `mmwlan_ap_args` RAW field + parser `:___` |
| Config valid gate | `morse_raw_is_config_valid` `raw.c:1046-1052` | `:___` |
| AP S1G-caps RAW advertise bit | (Linux sets) | flip `if(false)` `s1g_capabilities.c:276-280` `:___` |
| Beacon-sent cadence hook | `morse_raw_beacon_sent` `raw.c:1585-1597`, `morse_raw_do_update` `raw.c:957-978` | beacon-TX-complete `beacon.c:55` → `:___` (S6) |
| PRAW cadence | `morse_raw_start_praw_transmission` `raw.c:715-734`, `DTIMS_FOR_PRAW_TX` `raw.c:135` | `:___` (S6) |
| Page Slice element (separate track) | `morse_insert_page_slice_element` `page_slicing.c:77-195`; gate `:409-434` | new `ie_page_slice.c` `:___` |

---

## 5. OPEN RISKS + on-air unknowns

- **[BLOCKING] ~~The chip's RAW engine may not arm from a host-authored beacon (Q3a)~~ — ✅ RETIRED by S0b-2, 2026-07-30.** It arms and it gates: `assignments[0]` 0 → **2179** with `invalid_assignments = 0`, and `aci_frames_delayed` **92** (AP) / **239** (STA). Measured all-Linux on the byte-identical blob, zero ESP code. The TWT caution flag (`umac_twt.c:578-590` refuses AP-side schedule installs) did **not** generalise to RAW — noted so it is not cited as evidence again.
- **[MED, downgraded 2026-07-30] ~~FW legacy→S1G re-conversion may strip the host RPS IE~~ — the mechanism does not exist.** Both risks below were written around a *legacy→S1G conversion on the ESP AP path*. Source-verified: there isn't one. The ESP's hostapd emits a **finished S1G beacon** (`beacon.c:2357`, `:2673`) and no conversion layer exists in `components/halow` at all. The `umac_mesh.c:214-218` anchor these rested on is a **comment**, not code. **The mesh host-built-S1G fallback is therefore not needed for this reason** — the AP path already does what the fallback would have provided.
  **What survives, narrowed:** does the MM6108 blob rewrite IEs inside a host-supplied beacon? Unknown on *both* platforms; source cannot settle it; the capture can. If the RPS reaches air but the ESP's `assignments[]` stays flat, that is the signal — so **never report a Q3b verdict from the capture alone; read the ESP's tag-4210 counters in the same run.** (The Linux arm already showed `assignments[]` moving, so a flat ESP counter with a valid on-air element would be genuinely ESP-specific.)
- **[MED] ⚠ THE ESP MUST NOT FLIP THE S1G-CAPS BIT TO `if (true)` — Linux gates it on RAW being LIVE.** Source-verified on chronite 1.17.8, and confirmed on air. Linux sets the bit **once per vif** from the firmware manifest capability — `if (MORSE_CAPAB_SUPPORTED(&mors_vif->capabilities, RAW)) s1g_capab->capab_info[6] |= S1G_CAP6_RAW_OPERATION;` (`mac.c:1228-1229`, in `morse_mac_set_s1g_capab()` `mac.c:1088`, called for **every** vif type at `mac.c:3202`) — then **re-clears it on every beacon and every management frame** unless RAW is actually running: `if (!morse_raw_is_enabled(mors_vif)) capab_info[6] &= ~S1G_CAP6_RAW_OPERATION;` (`mac.c:1392-1393`, reached from `beacon.c:197` and `mac.c:1905`). The predicate needs **both** `mors_vif->ap` (kzalloc'd only for AP-type vifs, `mac.c:3236,3239`) **and** `RAW_STATE_ENABLED` (`raw.c:1630-1632`), and the config entry point rejects non-AP vifs at `raw.c:1567`.
  **On-air A/B, same AP, same session, measured 2026-07-30:** `…80 0C 00 02…` (RAW off, bit3=0, 2220 beacons) vs `…80 0C 08 02…` (RAW on, bit3=1, 236 beacons) — **one bit, everything else byte-identical.**
  **So the ESP's `if (false)` must become "is RAW enabled for this AP", not `if (true)`.** An unconditional flip would advertise RAW on an AP with no schedule configured — exactly the *"do not ship a RAW advertisement the AP cannot enforce"* case §7.7 rules out. **S0b-3 cannot be a one-line flip in isolation**; it needs whatever carries "RAW is enabled" to exist first, even as a stub.
  **Corollary:** a Linux STA can **never** advertise this bit, so it is not a valid control for an ESP STA (which does set it via `raw_sta_priority`). And the AP's real per-STA RAW input is not this bit at all — it is the **QoS Traffic Capability element, EID 89**, read at assoc (`raw.c:897,907`) and only once RAW is already enabled (`raw.c:890`). Short beacons carry no EID 217 at all.
  **Not a licence to call it free:** `S1G_CAP6_RAW_OPERATION` has exactly three hits tree-wide (define `s1g_ies.h:64`, set, clear), zero in hostap, and morselib's getter has zero callers — so no *source* reads it. The MM6108 blob was not searched, so whether the STA firmware keys on it is **unknown**.
- **[MED] ~~The AP's S1G-caps RAW bit may never reach air~~ — the ESP-side mechanism, restated.** Linux **deletes and regenerates** the S1G Capabilities element during conversion (`ie.c:518` + `tx_11n_to_s1g.c:922`), so the ESP FW plausibly does too — in which case flipping `s1g_capabilities.c:276-280` is invisible and **S3's "wire the advertise bit on when enabled" is dead as written**. Not listed in the original recon. Compounding it: on Linux the AP's own RAW bit is **coupled to RAW being enabled** (`mac.c:1392`), which is evidence the bit is load-bearing rather than decorative — so a Q3b negative caused by a stripped caps bit would be indistinguishable from "no engine." **S0b-3 settles it for one flash, no STA.**
- **[MED] ~~`MORSE_CAPS_RAW` may be unset~~ — ✅ CLOSED by S0a (2026-07-30):** both `MORSE_CAPS_RAW` and `MORSE_CAPS_PAGE_SLICING` are set on fw 1.17.8. The residual warning stands: advertising RAW with a non-arming FW would be actively harmful (STAs restrict, AP doesn't gate → wasted airtime).
- **[MED] ~~STA-side UL self-restriction is UNMEASURED~~ — ✅ RETIRED by S0b-2, 2026-07-30.** It is now measured: a STA at AID 1 parsed a **received** RPS (its own `assignments[0]` = **2083**) and self-restricted (`aci_frames_delayed` = **239**). Since neither host stack has an RPS parser (Linux's `raw.c` is AP-only — `raw.c:1567`, `:1705`), that behaviour is necessarily in the blob, which is the same blob the ESP runs.
- **[MED] A Linux STA cannot advertise RAW Operation Support, so it is not a clean control.** `mac.c:1392-1393` unconditionally clears the bit on a STA vif (`morse_raw_is_enabled` needs `mors_vif->ap`, NULL on a STA) and `morse_raw_process_cmd` hard-rejects non-AP vifs (`raw.c:1564-1573`); its only opt-in, `raw_sta_priority`, sets a *different* field (QoS Traffic Capability UP). The **ESP** STA does the opposite (`s1g_capabilities.c:134-139`). If the blob's engine keys on that bit, an all-Linux STA arm is already negative for a reason unrelated to AP direction. **Run both a Linux and an ESP STA in the same capture** (§7.3).
- **[LOW] Byte-fidelity footguns (mechanical, replicate exactly):** the *Linux* builder returns payload only — add EID **208** + Length yourself (Map 1 §1a/§5); replicate the literal num_slots cap 6/3 not the field's 63/7 (Map 1 §1d/§7, `raw.c:312,321,324`); never emit the Channel Indication 2 bytes (Map 1 §1e-3, `raw.c:472-474`); duration is `500 + cslot·120`, start-time in 2-TU units and referenced to the **end of the beacon that carried the RPS**, not the TBTT (`raw.c:103,122`); `GROUP_IND` (bit 5 of RAW Control) is set **unconditionally** (`raw.c:456`), not only when a group is present; the slot-count cap **writes back** into the caller's config (`raw.c:332`), silently changing subsequent beacons.
- **[MED] ✅ HANDLED IN S1 — `ie_rps_build()` copies `s1g_tim.c`'s two-pass size discipline, not RANN's.** `build_frame_with_class` (`frame_constructor.c:13-36`) runs every beacon builder **twice** — a NULL-buffer sizing pass, then a fill pass — and `consbuf_append`/`consbuf_reserve` each `MMOSAL_ASSERT(len <= buf_size - offset)` (`consbuf.c:30,54`). `MMOSAL_ASSERT` is **live** in the ESP-IDF build (`MMOSAL_NOASSERT` undefined) and expands to `while(1){}` (`mmosal.h:1006-1017`).
  ⚠ **WORDING CORRECTED 2026-07-31 — this bullet used to say "a pass-1/pass-2 size disagreement is a hard hang", and that is imprecise enough to cause a bug.** The contract is an **UPPER BOUND, not an equality**: the frame's final length is taken from **pass TWO** (`mmpkt_append(view, cbuf.offset)`, `frame_constructor.c:32`); pass one only sizes the *allocation*. `ie_s1g_tim_build` proves it — reserves **256** bytes, writes at most **45**. Only **under**-reserving hangs. **Reading the old wording literally and padding the element out to the reserved length is a BUG**: the bytes after the element are hostapd's `tail`, so padding desynchronises every subsequent IE in the chain — a beacon that still transmits and still carries a byte-perfect RPS, while everything after it parses at the wrong offset. Written that way first in S1 and caught before flashing; see `docs/worklog/2026-07-31-raw-s1-rps-encoder.md`. RANN (`umac_mesh.c:2336-2348`) is fixed-length and ignores the split; an AID-derived RPS cannot. Detect the sizing pass with `consbuf_reserve(buf, 0) == NULL` and over-reserve the worst case, exactly as `s1g_tim.c:420-425` does.
- **[MED] IE ordering.** Linux *reorders* the RPS into S1G-beacon position (after TIM/FMS, before the S1G Capabilities/SSID tail) via `dot11ah/ie.c:29-58` (long) and `:60-69` (short); the planned ESP hook appends at the tail. If Q2 passes but the STA ignores the element, **retry at the Linux-equivalent ordered position before concluding BLOCKED**.
- **[LOW] Config surface is net-new API.** `mmwlan_ap_args` has no RAW field and there's no AP RPS function today (Map 1 §7, Map 2 Q3); adding them is additive but touches the public header — needs CMake `target_compile_definitions` discipline for any test overrides (the CMake `target_compile_definitions` pattern).

---

## 6. THE S0 SPLIT (S0a — done)

**S0 splits in two — run S0a first.** The four decidables are not equally expensive. The capability read needs one board, no sniffer, no Linux reference, and no beacon hook; the other three need the full on-air rig. Since a cleared RAW capability bit would make the rest moot, do the cheap one first and let it gate the expensive one.

- **S0a — capability probe (no bench rig).** Read whether the loaded `.mbin` advertises `MORSE_CAPS_RAW` / `MORSE_CAPS_PAGE_SLICING`. Single board, no monitor, no peer.
  - The bits exist host-side at `mmdrv.h:548-549`; the values come off the chip's host table (`ext_host_table.c:20`, `capabilities.flags[i] = le32toh(caps->flags[i])`) and are fetched during the first interface-add (`umac_interface.c:262`).
  - **The capabilities are DEVICE-GLOBAL, not per-vif** — the fetch passes `MMDRV_VIF_ID_INVALID` and `umac_interface_get_capabilities()` takes the global `umac_data`. So *a* vif must be up for the fetch to have happened, but it need not be an AP vif. (An AP vif is still the natural choice, being the direction of interest, but nothing in the read requires it.)
  - `mmprobe_dump_fw_caps()` already exists (`driver.c:155`) but prints **undecoded** `flags[i]` words via `MMLOG_ERR`, and morselib's log does not reliably reach the ESP console — which is why the read-only accessors exist. Mirror `mmwlan_ampdu_capability_advertised()` (`umac.c:1663`, nine lines) instead; `protected_syms.txt` already globs `mmwlan*`, so no export edit is needed.
  - **What S0a can and cannot settle.** A set bit says the blob *claims* RAW. It does **not** distinguish direction, so it cannot answer whether AP-direction enforcement is present — that stays Q3/on-air. A **clear** bit is the decisive outcome: it is strong evidence RAW is absent from the shipped image, and S0b should not be built until it is resolved.
- **S0b — the on-air spike.** Scoped 2026-07-30; the plan is §7 below. Run only if S0a does not already answer BLOCKED (it did not — `MORSE_CAPS_RAW=1`).

---

## 7. S0b — THE ON-AIR SPIKE (scoped 2026-07-30)

**Headline change from the original §6 sketch: the cheapest arm of S0b needs no ESP code at all, and it
is the arm that can BLOCK the port.** The chip firmware is the same file on the ESP and on the Linux
bench nodes, and RAW reaches the chip only as beacon bytes — no chip command, on either host. So
"does the MM6108's RAW engine work" is decidable on Linux hardware alone. The original ordering
(build the ESP hook → then stand up the Linux reference → then measure) spends a submodule patch, a
build, a flash and a capture cycle *before* the gate is even approached, which inverts the same
cost-first logic that justified splitting S0a out of S0.

### 7.0 Blocking pre-edits — do these before any code (all now applied to this doc)

| | |
|---|---|
| **RPS EID is 208, not 66** | `WLAN_EID_S1G_RPS (208)` — `chronium:~/halow/morse_driver/dot11ah/dot11ah.h:37`. 66 is Measurement Pilot Transmission in mainline. A golden constant built with 66 fails the byte-diff on octet 0, is ignored by every STA and analyser, and reads most naturally as *"the FW stripped our IE"* — **a false BLOCKED on the one question the spike exists to answer.** Corrected at `:75`, `:82`, `:85`, `:115`, `:138`, `:157`, plus the S0a worklog and the two backlog docs. |
| **chronium cannot be both sniffer and reference AP** | It has one HaLow radio and monitor is exclusive with operation. The reference AP moves to **chronite** — already the automated `hostapd_s1g` node (`tools/regtest/linux_peer.py:232 bring_up_ap`), a Pi 5, not power-marginal. chronium stays in monitor for every run. |
| **Version attribution — ✅ CLOSED by S0b-0 (2026-07-30)** | Running stack (module + blob) = **1.17.8** on every node; the doc's anchors were read from **1.17.9** source. **This is a non-issue: `raw.c`, `raw.h` and `dot11ah/dot11ah.h` have IDENTICAL git blob OIDs at tag 1.17.8 and tag 1.17.9** — `raw.c 030b5e3a1672828203323c93014a29d7c1fa3684`, `raw.h 5d097b83c8c0aac3d9ecf0a85dce0ecca2f44abf`, `dot11ah/dot11ah.h 51d44b711decaccd957c49103a3f1940e447d769`, verified on both nodes. Only **5 of 134** tracked files changed between the releases (`Makefile`, `dot11ah/Makefile`, `dot11ah/main.c`, `page_slicing.c`, `vendor.c`), and all three C changes are **RX-side input-validation hardening** against malformed *received* IEs — a security-patch release, not a feature release. `raw.o`'s compiled `.text` and `.rodata` are byte-identical when built from either tree. **Every line-number anchor in this doc is valid verbatim for 1.17.8; a Q2 byte-diff mismatch cannot be a driver-version delta.** Do not spend a bench slot "aligning versions" — there is nothing to align on the RPS path. |

### 7.1 The golden constant

Derived from `morse_driver` `raw.c`, for one GENERIC assignment, `start_aid=1`, `end_aid=255`,
format 0, `num_slots=2`, `slot_duration=10100 µs`:

```c
static const uint8_t k_rps_golden[8] = { 0xD0, 0x06, 0x20, 0x40, 0x09, 0x04, 0xE0, 0x1F };
```

| Octet | Value | Derivation |
|---|---|---|
| `[0]` | `0xD0` | EID 208 (`dot11ah.h:37`) — written by the caller, not the Linux builder |
| `[1]` | `0x06` | `sizeof(assignment)=3 + sizeof(group)=3`; start-time omitted (`start_time_us==0`) and PRAW omitted (`raw.c:399-414`) |
| `[2]` | `0x20` | RAW Control: GENERIC(0), then `GROUP_IND` BIT(5) set **unconditionally** (`raw.c:456`) |
| `[3..4]` | `40 09` | Slot Def LE: `((80<<2)&GENMASK(9,2)) \| ((2<<10)&GENMASK(16,10))` = `0x0940`. cslot 80 because `CSLOT_TO_US(80) = 500+9600 = 10100`, and `80 ≤ 255` keeps format 0 (`raw.c:318-325`, `:341-355`) |
| `[5..6]` | `04 E0` | RAW Group LE: page 0 \| `((1<<2)&GENMASK(12,2))` \| `((255<<13)&GENMASK(15,13))` = `0xE004` (`raw.c:460-467`) |
| `[7]` | `0x1F` | `end_aid >> AID_END_BITS_SHIFT` = `255>>3` = 31 (`raw.c:469`); readback `(31<<3)\|7 = 255` |

**✅ Independently re-derived from 1.17.8 source and mechanically confirmed (S0b-0, 2026-07-30)** — the
encoder region was transcribed verbatim, compiled and executed, and emits all eight octets exactly.
Octet `[2]=0x20` is **stronger than stated**: `GROUP_IND` is set with no guard (`raw.c:456`) and the
PSTA/RAFRAME `TYPE_OPTION` bits are **dead code** in 1.17.8 (defined at `raw.c:25-26,29-30`, zero use
sites), so RAW Control bits 3:2 are always 0 for any generic RAW without start-time/PRAW/channel.
Minor corrections to the table above: `[1]` is written by the **caller** too (`dot11ah/ie.c:468`,
alongside the EID at `:467`), not by `raw.c`; and the format threshold is coded as
`if (cslot > __UINT8_MAX__)` (`raw.c:318`).

`end_aid=255` is chosen because it is what a naturally-configured reference AP emits — the priority→AID
mapping is in **hostapd's** `hostap/src/utils/morse_cli.c:267-271` (*not* the standalone `morse_cli`
tool, whose `raw.c` has no default and requires an explicit `-a`), mapping priority 0 to `aid_start=1` /
`aid_end = __UINT16_MAX__ & MORSE_RAW_AID_DEVICE_MASK(0xFF)` = 255.

<a id="rps-harness-assertions"></a>

> **⚠ THE REFERENCE AP CONFIG IS NOT FREE — hostapd's defaults do NOT emit the golden constant.**
> `hostap/src/ap/ap_config.c:191-199` defaults every RAW to `start_time_us=4096`, `duration_us=26900`,
> `slots=1`, `cross_slot=false`. Those emit a **nine**-byte element — `D0 07 30 70 07 02 04 E0 1F` —
> because `START_IND` BIT(4) raises RAW Control to `0x30` and an extra start-time octet appears. **Left at
> defaults, S0b-2's reference capture will not match `k_rps_golden`, and the mismatch would read as an
> encoder or firmware finding when it is a config error.** The RAW block must explicitly set
> `start_time_us=0`, `duration_us=20200`, `slots=2`, `cross_slot=0`, `praw_period=0`,
> `nominal_stas_per_beacon=0`, `priority=0`. Pre-register this in §7.3: **a captured Length of `0x07`
> with RAW Control `0x30` is a config error, not a firmware finding.**

**Harness assertions — all four, not just the duration one:**
1. `duration_us` an exact multiple of `num_slots`, **and** `duration_us/num_slots == 500 + 120k`. The
   CLI's `-s` first value is the **total** RAW duration, which the driver divides by `num_slots`
   (`raw.c:1080-1082`) — a double truncation. A "round" 10000 µs/slot truncates to cslot 79 (9980 µs) and
   changes `[3]` to `0x3C`, a real diff that looks like a bug.
2. `cross_slot == 0` — `cross_slot=1` flips `[3]` from `0x40` to `0x42` (`raw.c:306-307`), a one-bit diff
   that reads as an encoder mismatch.
3. `start_time_us == 0` and not periodic — otherwise the element grows and `[2]` changes (see the box above).
4. `num_slots ≤ 6`. **The driver silently caps `num_slots` at 6 (format 0) / 3 (format 1)** —
   `raw.c:321,324` assign the *bit-count* constants `IEEE80211_S1G_RPS_RAW_SLOT_NUM_{3,6}BITS` = 3 and 6,
   **not** the field maxima 7/63 — then `raw.c:329-333` caps with only a `WARN` and **writes the cap back
   into the caller's persistent config**. Both `morse_cli` (`raw.c:114`) and hostapd
   (`src/utils/morse.h:154`) advertise 1–63. Requesting 8 slots silently yields 6 and changes `[4]`
   from `0x09` to `0x19`.

> **⚠ A ceiling the recon did not record: RAW gives you at most 6 slots per window on this stack.** That
> is a real constraint on the mesh-gate contention story, which is the whole reason RAW is being ported —
> any design premised on many slots per beacon is capped at 6. `num_slots=2` is unaffected so the golden
> bytes stand, but **S1+ scaling arithmetic must not assume 63**, and whether the *firmware* enforces the
> same 6 is worth confirming on the bench.

**Latent, not biting today:** `raw.c:62` defines the slot-count mask as `GENMASK(16, 10)` — bit 16 is
outside the `__le16` it is OR'd into at `raw.c:349-354`. Harmless only because of the ≤6 cap; a port that
raised the cap to the field's true 63 would silently lose bit 16.

**Toolchain inconsistencies, version-independent** (`morse_cli` tags 1.17.8 and 1.17.9 are the *same
commit* `8f06222`): the CLI validates slot duration on a **200 µs** granularity to 410099 µs
(`cli raw.c:20`) while the driver quantises at **120 µs** and caps at 246140 µs (`raw.c:103,320,335-339`);
`raw.h:169-171`'s "Maximum duration is 246260us" is 120 µs high; and `RAW_CMD_MAX_3BIT_SLOTS` is dead
code, never enforced. **Any ESP-side config API should enforce the DRIVER's caps, not the CLI's.**

### 7.2 Staged plan — each stage gates the next

**S0b-0 — desk, free, no bench. ✅ DONE 2026-07-30 — all three checks passed.** Worklog:
`docs/worklog/2026-07-30-raw-s0b-0-desk-checks.md`.

| Check | Result |
|---|---|
| `.mbin`↔`.bin` segment equivalence | **CONFIRMED.** All **9** loadable PROGBITS sections round-trip byte-identically through the build-time repack — `.host_imem`, `.host_dmem`, **`.mac_imem`** (the RAW-engine-bearing code section, 154912 B @ `0x120000`), `.mac_rodmem`, `.mac_dmem`, `.uphy_imem`, `.uphy_dmem`, `.lphy_imem`, `.lphy_dmem`. Our build passes no `--compress`, so payloads are stored raw. Nothing executable is dropped; only non-ALLOC sections (`*_offchip_stats`, `.symtab`, `.strtab`, `.shstrtab`) are, and neither loader writes those. `.fw_info` is re-encoded losslessly as 85 TLVs. **The only divergence anywhere is 6 bytes of word-alignment padding** (ESP `_pad_data` fills `0x00`, Linux `firmware.c` `memset`s `0xff`) at three addresses, all strictly outside every section's contents — and **`.mac_imem` takes zero padding**, its `filesz 0x25d20` already being word-aligned. |
| 1.17.8 vs 1.17.9 encoder drift | **NONE — the question dissolves.** See §7.0. |
| Spike-edit patch capture | **DONE at the time, and now SUPERSEDED — `git` is the restore path.** The S0a delta was captured as named patch files (each verified to apply in reverse) because the submodule was dirty with uncommitted work. That work has since **landed** (`teapotlaboratories/mm-esp32-halow#28` + rimba `#49`), and the captures lived in a session scratchpad under `/tmp`, which the nightly reboot wipes. **Do not go looking for them.** The discipline still stands for S0b-3+: capture each spike edit as a named patch **outside `/tmp`** before flashing, since those edits will again be uncommitted — but the *baseline* to return to is now simply `main`. |

Two findings came out of it that **change the bench plan** — the hostapd RAW block must be pinned
explicitly (its defaults emit a different, 9-byte element) and `num_slots` is capped at 6, not 63. Both
are written up in §7.1.

**Residual, deliberately not chased:** the ESP `.mbin` was compared against the source ELF, not against
what the chip actually holds after a load; and the 6 padding bytes differ between platforms. Neither is
worth bench time — `.mac_imem` is unaffected by the padding and the ESP demonstrably runs mesh, AP and
TWT off this repack.

**S0b-1 — sniffer calibration, no ESP code, no RAW. ✅ DONE 2026-07-30 — timing is viable, but two
gates in this stage's own design were wrong.** Worklog `docs/worklog/2026-07-30-raw-s0b-1-calibration.md`.
Rig: chronium monitor (`type monitor`, freq 5560 = S1G ch27), chronite plain `hostapd_s1g` AP from the
tracked `docs/reference/captures/hostapd-rimba.conf` (`beacon_int=100`, `dtim_period=1`, no RAW block).
235 s capture, **2222 S1G beacons** decoded across **2290** TBTT slots. RSSI median **−36 dBm** — nowhere
near the ≈−3 dBm overload signature, so **no TX-power capping was needed**.

| Metric | Result | Original gate | |
|---|---|---|---|
| robust σ (1.4826·MAD) on successive intervals | **1.48 µs** | ≤ 20 µs | ✅ **PASS ×13** |
| p99−p1 on successive intervals | **828 µs** | ≤ 100 µs | ❌ FAIL — *but see below* |
| MAD vs a fitted TBTT grid | **0.8 µs** (robust σ **1.12 µs**) | — | the number that matters |
| counter instrument | RAW section **present + structurally complete**, all-zero at baseline | must exist | ✅ **PASS** |

**The p99−p1 failure is not a clock problem, and the gate was measuring the wrong thing.** Residuals
against a fitted TBTT grid are **strongly one-sided**: p1 **−2 µs**, p99 **+415 µs**, min **−3 µs**, max
**+3744 µs**. A beacon can be **late** but essentially never **early** — that is AP-side beacon deferral
(a busy medium at TBTT), not sniffer timestamp jitter. 95.7 % of beacons land within ±50 µs of the grid,
99.7 % within ±500 µs.

> **⚠ METHOD FIX, load-bearing for Q3: measure phase relative to the OBSERVED beacon, never against a
> fitted or idealised TBTT grid.** The RAW window is referenced to the **end of the beacon that carried
> the RPS** (§7.1), so when a beacon is deferred the window moves with it — measuring against the observed
> beacon cancels the deferral exactly. A grid-based "phase mod beacon_interval" would smear ~4 % of frames
> by up to **3.7 ms — a third of a 10100 µs slot — and that reads as leakage.** With the correct method the
> instrument precision is **~1 µs against a 2048 µs A/B-1 separation, a ~2000:1 margin.** Timing is viable.

> **⚠ GATE REPLACED: the morse0 drop counter is nearly useless as an integrity check.** Over this capture
> **58 of 2290 beacons (2.53 %) were genuinely absent from the tap** — confirmed by TBTT-slot occupancy,
> and not explained by the 72 unrelated 33-byte frames, which are spread across the whole beacon interval
> rather than sitting in the empty slots. The `ip -s link show morse0` **`dropped` counter moved by 1.** It
> accounted for **1 of 58** missing frames. The mandatory "require a zero drop delta" gate written here
> **would have passed this capture with 2.5 % of frames silently missing** — precisely the false-GO
> mechanism §5 rules out. **Replace it with frame-count reconciliation against expected**
> (`slots_spanned` from the mactime grid vs frames decoded), which is what actually caught it. Keep the
> drop-counter snapshot as a weak secondary signal only.

**Counter instrument validated on the live 1.17.8 stack** — the RAW section is emitted with correct
structure and reads all-zero with RAW disabled, so it is **not** a vestigial CLI-side formatter and any
non-zero reading in S0b-2 is signal:

```
RAW
    RAW Assignments
        Valid                 : 0 0 0 0 0 0 0 0     <- assignments[8]
        Truncated by TBTT     : 0
        Invalid               : 0                   <- invalid_assignments
        Already past          : 0
    Delayed due to RAW
        From ACI queue        : 0                   <- aci_frames_delayed
        From BC/MC queue      : 0                   <- bc_mc_frames_delayed
```

**Bonus — S0b-0's finding 5a confirmed live.** `hostapd_s1g` logs `RAW Settings: disable 4096 26900 1
disable 0 0 0 0` at startup: exactly the defaults that produce the 9-byte element. Disabled here, so no
RPS was emitted — which is what this stage wanted.

**Also settled (free):** `morse_cli -i wlan1 stats` **fails with the link down** (`NL80211, code -100`),
so the counter probe is **not** usable as a zero-radio pre/post control — it needs the radio up. §7.2's
open question about that is closed, negatively.

**S0b-2 — all-Linux RAW reference. ZERO ESP code. THIS IS THE GATE.** chronite AP with RAW driven by
`morse_cli -i wlan1 raw -s <total_us>,<slots> -a 1,255 -t 0 enable <id>` then `raw enable 0`;
chronogen as STA (`raw_sta_priority`); chronium monitoring. Skip hostapd's `raw={}` block — hostapd
merely shells out to `morse_cli` anyway, and driving the CLI directly gives exact control of the AID
ranges. **Not chronosalt** — documented as browning out on radio-up, and a mid-run reset silently
voids a timing measurement. This one run yields **four** things:
- **Q3a** — do the tag-4210 `raw_stats_t` counters move on the AP? `assignments[]` incrementing ⇒ the
  chip parsed its own outgoing RPS. `aci_frames_delayed`/`bc_mc_frames_delayed`/
  `frame_crosses_slot_delayed` incrementing ⇒ the chip is actively gating its own TX to the RAW.
  **A negative here BLOCKS the port with no ESP code written.**
- **Q4** — does the STA self-restrict? Its **own** `aci_frames_delayed`, corroborated by uplink timing.
- **The real golden bytes** for the Q2 diff, captured off air rather than hand-built — which
  simultaneously validates §7.1's 1.17.9-derived constant against the 1.17.8 encoder the bench runs.
- **The reference AP's own S1G Capabilities octet[6] bit 3**, i.e. what the ESP must replicate.
Pre-check the captured element's Length octet is `0x06` before any payload comparison — anything else
means the reference is emitting more than one concatenated assignment and the config needs trimming.

### ✅ S0b-2 RESULT — 2026-07-30. **Q3a PASS, Q4 PASS.** Worklog `docs/worklog/2026-07-30-raw-s0b-2-gate.md`

Rig: chronite AP (`morse_cli -i wlan1 raw -s 20200,2 -a 1,255 -t 0 enable 1` + `raw enable 0`),
chronogen STA (`raw_sta_priority=0`, associated with **AID 1** — inside the advertised group, so
confound (e) is excluded), chronium monitor. RSSI −53/−57 dBm. Driver readback confirmed the config
landed exactly: `start_time_us=0 aid: start=1 end=255 x-slot=0 num=2 duration_us=10100`.

**Counters — the primary instrument, both ends, after a driven uplink load:**

| | AP (chronite) | STA (chronogen) | baseline |
|---|---|---|---|
| `assignments[0]` | **2179** | **2083** | 0 |
| `invalid_assignments` | **0** | **0** | 0 |
| `aci_frames_delayed` | **92** | **239** | 0 |
| `frame_crosses_slot` | **27** | **27** | 0 |

**Q3a PASS** — `assignments[0]` climbing ≈ once per beacon proves the chip **parses the RPS out of its
own outgoing beacon**, and `invalid_assignments = 0` proves the bytes are accepted. `aci_frames_delayed`
and `frame_crosses_slot` moving prove it **actively gates its own TX** to the window.
**Q4 PASS** — the STA's *own* `assignments[]` climbed, so it parses a **received** RPS, and its own
`aci_frames_delayed = 239` proves it self-restricts. This retires the §5 "[MED] STA-side UL
self-restriction is UNMEASURED" caveat.

**Golden constant validated on air.** 236/236 S1G beacons carried **`D0 06 20 40 09 04 E0 1F`**,
byte-identical to §7.1, single distinct element, in a single IE chain `[213, 5, 208, 217, 232, 214, 0,
221]` — **RPS at position 3, directly after TIM**, matching Linux's ordering table. The S1G header offset
was a constant 15 bytes on every frame (the conditional presence bits were *not* in play here).
**Negative control:** the RAW-*disabled* S0b-1 capture contains **zero** EID-208 elements across 2222
beacons, so the element is attributable to the RAW config and not to something the FW emits anyway.

> **⚠ THE TIMING ARM WAS VOIDED — and the reason is a finding in its own right.** The uplink phase
> histogram put only 9.2 % of frames inside the advertised window, which would read as *no confinement* —
> but the capture is not admissible: the STA's own counters report **1194 packets sent (netdev) / 1681
> frames (MAC)** while the capture holds **403**. That is **66 % data-frame loss**, versus the **2.5 %**
> beacon loss S0b-1 measured — a **26× difference**. Per row (g) the run is voided and **no confinement
> conclusion may be drawn in either direction.** (The parse itself was sound — tshark independently found
> exactly 403 frames with the STA's TA.)
>
> **Consequence for the plan: S0b-1's beacon-based calibration does NOT transfer to data frames.** Any
> future timing measurement must reconcile *data-frame* counts against the transmitter's own TX counters
> before a phase histogram is even computed — beacon-count reconciliation is not sufficient. This is
> exactly the caveat S0b-1 flagged as untested, now measured and much worse than feared.
>
> This does **not** weaken the verdict: the counters were designated the primary instrument precisely
> because they are immune to capture loss, and they are unambiguous.

**Also observed:** `frame_crosses_slot = 27` on both ends means real frames are **too long for a 10100 µs
slot** at 1 MHz — pair this with the ≤6-slot cap (§7.1) when sizing any real RAW schedule.

**S0b-3 — ESP caps-bit only. ✅ DONE 2026-07-31, FOLDED INTO S0b-4 — see the joint result below.** The
stage as written below is superseded: the flip is **not** `if (false)` → `if (true)`, because the Linux
driver treats that bit as **live state** (set only while a RAW schedule is actually running), so an
unconditional flip advertises RAW on an AP with no schedule. It needs a schedule to exist first, which is
exactly what S0b-4's splice provides — hence one fixture, one flash, both answers. Original text follows
for the record.

Flip the `if (false)` at
`s1g_capabilities.c:276-280` to `if (true)`, restart the AP, capture one beacon, diff octet[6] bit 3
(`DOT11_MASK_S1G_CAP6_RAW_OPERATION_SUPPORT`). This settles a risk the original §6 did not list:
Linux **deletes and regenerates** the S1G Capabilities element during conversion (`ie.c:518` +
`tx_11n_to_s1g.c:922`), so the ESP FW plausibly does too — in which case the AP can never advertise
RAW from the host and **S3's "wire the advertise bit on when enabled" is dead as written**. Note the
flip is *not* a beacon-path change: `ie_s1g_capabilities_build_ap` has exactly one caller,
`mmwpas_get_hw_feature_data_ap` (`driver_ap.c:132`), whose output is memcpy'd into hostapd's
`hw_mode->s1g_capab` (`:139`) — so it is an **AP-start-time** hw-capability report, and observing its
effect requires a full AP restart. S3 must therefore state that a runtime RAW enable/disable knob
cannot be a bool read per beacon.

**S0b-4 — ESP RPS on air (Q2).** Splice `k_rps_golden` at `umac_ap.c:418/419` (~6 lines,
`consbuf_append`, modelled on the RANN branch `umac_mesh.c:2341-2342`) and byte-diff against S0b-2's
captured reference. Run **both APs beaconing simultaneously** so one capture holds both elements
under identical radiotap conditions — but that dual-AP capture is **Q2-only and must never be reused
for a timing histogram**, since a second BSS on ch27 adds contention indistinguishable from slot
leakage. Capture a full DTIM cycle and classify per frame: Linux's short-beacon allowlist **does**
include RPS (`ie.c:63`), so a capture that only samples DTIM beacons could report Q2=PASS while
non-DTIM beacons carry nothing. **Take the no-hook baseline capture first** — without it, an EID-208
element on air cannot be attributed to the host splice, and a *missing* element gives no evidence the
capture/decode path works at all.

### ✅ S0b-3 + S0b-4 RESULT — 2026-07-31. **Q2 PASS, S0b-3 PASS.** Worklog `docs/worklog/2026-07-31-raw-s0b-4-esp-rps.md`

Rig: board0 as the ESP AP (`rimba-rawrps`, beacon SA `6a:24:99:44:6b:b7`, beacon_int 100 TU, dtim 3, TX
capped to 1 dBm) **and** chronite as the Linux RAW reference AP (`rimba-ping`, tracked `hostapd-rimba.conf`
+ `morse_cli -i wlan1 raw -s 20200,2 -a 1,255 -t 0 enable 1`) beaconing **simultaneously**, with chronium
in monitor on `morse0` — so one capture holds both elements under identical radiotap conditions. Bench left
radio-silent on every node.

| | baseline (no hook) | ESP spike | Linux reference (same capture) |
|---|---|---|---|
| beacons decoded / undecodable | 467 / 0 | **519** / 0 | **515** / 0 |
| EID-208 present | **0** | **519 / 519** | **515 / 515** |
| RPS bytes | — | **`D0 06 20 40 09 04 E0 1F`** | **`D0 06 20 40 09 04 E0 1F`** |
| S1G caps octet[6] | `0x00` | **`0x08`** | **`0x08`** |
| DTIM / non-DTIM | 158 / 309 | **171 / 348** | 515 / 0 |
| capture completeness | 97.29 % | 98.11 % | 97.35 % |

- **Q2 = PASS.** The blob does **not** rewrite or strip IEs in a host-supplied beacon. The host splice is
  the cheap path; **the mesh host-built-S1G-beacon fallback is not needed.**
- **S0b-3 = PASS.** The caps bit reaches the air from the host: `… 00 08 00 02 …` → `… 00 08 **08** 02 …`,
  **one bit, everything else byte-identical**, matching the Linux A/B signature. **S3 is NOT dead as
  written.**
- **RPS lands directly after the TIM on both stacks** — ESP `[0, 5, 208, 48, 127, 244, 213, 217, 232, 214,
  221]`, Linux `[213, 5, 208, 217, 232, 214, 0, 221]`. ⚠ **The Q2 diff is the eight RPS octets plus the
  TIM→RPS adjacency, NOT the whole IE-chain vector.** The overall chains differ because on the ESP
  213/217/232/214 sit inside hostapd's `tail` (`beacon.c:2562-2569`) and land after the TIM — a
  long-standing ordering difference unrelated to RAW. Diffing the vector reports a false mismatch.
- **The short-beacon worry is measured, not argued:** `dtim_period=3` put **171 DTIM and 348 non-DTIM**
  beacons in one capture and **all 519** carry the RPS. (Source agrees: morselib has no short-beacon TX
  path and `umac_ap_build_beacon()` is the single unconditional builder.)

**How it is armed — §7.4's `IDF_EXTRA_D` claim is half wrong.** `IDF_EXTRA_D` passes CMake **cache
variables**, which today's apps individually convert to *app-private* defines; that privacy is a property of
those apps, not of the mechanism. A **build-global** define *does* reach morselib, because IDF applies the
build-wide `COMPILE_DEFINITIONS` as a directory-scoped `add_compile_definitions()` inside every
`idf_component_register` (`component.cmake:468,476`) and `components/halow/CMakeLists.txt:29` adds the
morselib subdirectory *after* registering at `:15-22`. So the spike is armed by **one line in the fixture's
own top-level CMakeLists** — `idf_build_set_property(COMPILE_DEFINITIONS "RIMBA_RAW_S0B_SPIKE=1" APPEND)`
— with **zero submodule CMake edits**. Verified from `compile_commands.json`: both guarded files carry
`-DRIMBA_RAW_S0B_SPIKE=1`, and the other AP apps' build trees do not. **Do not use a CMake cache var via
`IDF_EXTRA_D`** — cache entries are sticky per build dir and `t0_build.py:242` reuses that dir, so one
spike build would keep the RPS compiled into T0 until a `fullclean`.

**Deliverables now tracked:** `tools/raw_s1g_beacon_decode.py` (the §7.6 item-1 radiotap-aware pcap decoder,
**calibrated against both archived S0b captures** — it reproduces the 236-beacon reference and the
2222-beacon negative control to the frame); `firmware/test-raw-rps/` + its `manifest.py` entry; the two
guarded morselib edits captured at `docs/worklog/artifacts/raw-s0b/s0b-4-morselib-spike.patch`; and the
S0b-1/S0b-2 pcaps, **rescued from `chronium:/tmp` (tmpfs, 19-day uptime) into
`docs/worklog/artifacts/raw-s0b/`** before a reboot took them.

**Corrections this stage forced into this document:** §7.4's "20 apps set `CONFIG_HALOW_AP_MODE=y`" is
stale — it is **27** (only 7 instantiate a SoftAP vif); and §7.5's "the ESP fixtures cap to 1 dBm" was
**false** for the AP fixture family (`test-raw-cap`, `rimba-halow-ap`, `rimba-halow-ap-perf` never call
`mmwlan_override_max_tx_power`) — `test-raw-rps` does, and the call must sit between `set_config` and
`wifi_start` or it returns `MMWLAN_UNAVAILABLE`.

### ✅ S0b-5 RESULT — 2026-07-31. **Q3b PASS — HARD GO.** Worklog `docs/worklog/2026-07-31-raw-s0b-5-esp-counters.md`

Both halves reproduce the Linux behaviour with the **ESP** as AP, so §7.7's *"ESP counters move like the
Linux arm's → **HARD GO** — stage S1→S4"* row is satisfied in full.

**S0b-5a — arming. One board, no STA, no traffic, no sniffer needed.**

| read | `assignments[0]` | Beacons TX (tag 4170) | ratio | `invalid_assignments` |
|---|---|---|---|---|
| t+60 s | 586 | 586 | **1.000** | 0 |
| t+240 s | **2344** | **2344** | **1.000** | **0** |

**Exactly one assignment per beacon transmitted, across 2344 beacons.** Not "≈1 per beacon" — tag **4170
"Beacons TX"** supplies the chip's own denominator, which turns S0b-2's *inferred* ratio into a measured
one. Beacon punctuality is clean: `missing from host = 0`, `late from host count = 0`, `max delay = 0`,
so §7.7's *"beacon-lateness ≈ slot duration"* branch is excluded outright. A concurrent capture excluded
confound (d): **571/571 beacons carried the golden RPS**, only the ESP on air.

**S0b-5b — gating. ESP AP + Linux STA (chronite) + a 120 s `ping -i 0.002 -s 512` flood.**

| | pre-flood | flood | after |
|---|---|---|---|
| `aci_frames_delayed` | 0 | **77** | **139** |
| `frame_crosses_slot` | 0 | **19** | **37** |
| assignments/Beacons TX | 1.000 | 1.000 | 1.000 |

Linux read `aci = 92`, `frame_crosses_slot = 27` — same order, same firmware. ⚠ **A blocker had to be fixed
first: `test-raw-rps` assigned no IP, so a ping flood produced pure uplink and every AP-side deferral
counter would have stayed at zero — a silent null result that looks like a clean negative.** The AP now
pins `192.168.12.1` and brings the netif up explicitly (mmhalow never fires a link-up event in AP mode).

**Instrument notes, all verified against the firmware rather than this document:**
- `mmwlan_get_morse_stats(uint32_t core_num, bool reset)` — **`core_num` is NOT a vif id**; the driver maps
  **0 = HOST, 1 = MAC, 2 = UPHY** (`driver.c:1510-1548`). RAW is a MAC stat ⇒ core 1. Needs the radio up.
- TLV = **u16 LE tag, u16 LE len, value**; `raw_stats_t` = 15 × le32 = 60 B.
- **Tag 4210 was confirmed from the firmware's own descriptor table**, section `.mac_offchip_stats` in
  `vendor/morse-firmware/firmware/mm6108.bin` — 164 records of
  `{char type_str[50]; char name[50]; char key[100]; u32 format; u16 tag;}`, giving
  `tag=4210 type=raw_stats_t key=RAW`. `morse_cli` never hardcodes tags; it resolves them through this
  table, so it is the authority.
- ⚠ **`stats->len` overstates the TLV payload by 4 bytes** — `mmdrv_get_stats` sets `buf = resp->data`
  (past `resp->status`) but `len = hdr.len`, which *includes* the status
  (`driver.c:1704` synthesises an empty response as `hdr.len = sizeof(resp->status)`). Walk
  `stats->len - 4`. With the correction the blob parses as **exactly 164 TLVs in 1473 bytes — matching the
  164 descriptor records**, which is a three-way cross-check (table, TLV count, per-element lengths).

**Also now tracked:** `docs/reference/captures/wpa-sta-raw.conf` — the Linux infra-STA config §7.6 item 4
called for, which had only ever existed in `/tmp` on the Pis.

**S0b-5 — Q3b, ESP as the AP.** Repeat S0b-2's measurement with the ESP AP. Read the ESP's own
counters via `mmwlan_get_morse_stats(1, …)` — already public and already exported by the `mmwlan*`
glob, so **no submodule change**; the work is app-side (TLV walk, tag 4210, 15×LE-u32, `TEST|`
reporting). Also read tags 4171-4180 (*"Beacons late from host max delay (usec)"*, *"Beacons TX
late"*, *"Beacons missing from host"*): because the RAW window is referenced to the **end of the
beacon that carried the RPS**, a late or jittered host-served beacon corrupts the time reference even
with perfect enforcement — and without this control, "the ESP cannot serve beacons punctually enough
for RAW" is indistinguishable from "the FW has no AP-direction engine." They are very different
problems, and the first is the fixable one.

### 7.3 The confound matrix — pre-register this before booking bench time

A spike that cannot tell *"the FW doesn't enforce"* from *"my IE was wrong"* is worthless. If a STA
is **not** confined, the cause is one of:

| | Cause | Disambiguator |
|---|---|---|
| (a) | No AP-direction engine | Only concludable after every other row is excluded; positively indicated by AP-side `aci_frames_delayed`/`bc_mc_frames_delayed` flat while the RPS is provably valid and on air |
| (b) | STA ignored the RPS | The **STA's own** counters — if its `aci_frames_delayed` increments the STA *is* self-restricting; all-zero with a valid on-air RPS **is** (b), and that is a Q4 answer, not a Q3 answer |
| (c) | RPS malformed | AP-side `invalid_assignments` increments ⇒ the chip parsed and **rejected** your bytes. **The EID-66 error would have landed exactly here** |
| (d) | RPS never reached air | Q2's capture. No EID-208 element ⇒ **Q3 is not evaluable; do not record a Q3 verdict** |
| (e) | STA never assigned | Read the actual AID; use `[1,255]` in the single-STA arm so no STA can fall outside |
| (f) | Window past/truncated | `assignments_truncated_from_tbtt`, `already_past_assignment`; static check `start_time + num_slots·slot_dur < beacon_interval` |
| (g) | Capture incomplete | **Frame-count reconciliation against expected** — derive TBTT slots spanned from the mactime grid and compare to frames decoded. **Measured 2.53 % beacon loss in S0b-1 while the morse0 `dropped` counter moved by 1**, so the drop counter is a weak secondary signal only, never the gate. **Produces a FALSE GO — check every run** |
| (h) | STA had no uplink | Fixed-rate offered load, reconcile sent-vs-captured. ⚠ **neither Pi Zero has `iperf3`** — install it or specify a ping-flood with an explicit rate |
| (i) | STA associated elsewhere | Assert BSSID in the capture |
| (j) | **Reference AP misconfigured** — added by S0b-0 | A captured element of Length `0x07` with RAW Control `0x30` is **hostapd's defaults leaking through** (`start_time_us=4096`, `slots=1`, `duration_us=26900`), *not* a firmware finding. Length `0x06` with RAW Control `0x20` is the golden shape. Assert the Length octet **before** any payload comparison. Likewise `[4]=0x19` instead of `0x09` means the driver silently capped `num_slots` from 8 to 6. See §7.1 |

**A Linux STA is not a clean control for the ESP STA path.** `mac.c:1392-1393` unconditionally
**clears** the S1G RAW Operation Support bit on a STA vif (`morse_raw_is_enabled` requires
`mors_vif->ap`, NULL on a STA), and `morse_raw_process_cmd` hard-rejects non-AP vifs
(`raw.c:1564-1573`). Its only RAW opt-in, `raw_sta_priority`, sets a *different* field (QoS Traffic
Capability UP). The **ESP** STA does the opposite — `raw_sta_priority >= 0` sets exactly that cap bit
(`s1g_capabilities.c:134-139`). So if the blob's confinement engine keys on the STA's advertised cap
bit, an all-Linux STA arm is **already negative for a reason unrelated to AP direction**. Run the STA
leg with **both** a Linux STA and an ESP STA against the same Linux RAW AP in the same capture: if
only the ESP STA confines, the cap bit is the discriminator and the Linux-STA arm must be dropped as
a reference.

### 7.4 Scope boundaries

- **S0b is a hand-run spike, not a scored tier.** The harness structurally cannot run it: no monitor
  `linux_setup` exists (`t2_onair.py:413-434`), a Linux reporter role is rejected outright
  (`:269-273`), and there is no pcap/radiotap parsing anywhere in `tools/regtest`. It also needs a
  **temporary morselib patch**, and `IDF_EXTRA_D` can only pass `-D` defines into the app build,
  never patch the submodule. Precedent: the mesh-gate S6 RANN byte-diff is still manual for exactly
  this reason. **Consequence: `t2_onair.py`'s automatic `go_radio_silent` does not apply — the
  radio-silent rule becomes a manual checklist item for a multi-hour, four-radio session.**
- **Gate the spike hook behind a compile-time define.** `umac_ap_build_beacon()` is shared morselib,
  not fixture code — **27 apps under `firmware/` set `CONFIG_HALOW_AP_MODE=y`** (recounted 2026-07-31;
  "20" was stale. Only **7** of them actually instantiate a SoftAP vif — the rest need the flag because
  morselib gates `umac_ap.c` and friends on it and mesh/IBSS link against them), and while the hook is
  in the tree every one of them emits the golden RPS and the flipped caps bit. That contaminates any
  T0 build or T2 run during the spike window and puts a RAW advertisement on air from fixtures nobody
  is watching. Wrap both edits in `#ifdef RIMBA_RAW_S0B_SPIKE`.
  ✅ **SUPERSEDED BY S3 (2026-07-31) — the `#ifdef` is gone.** Containment is now that RAW is a field
  on `mmwlan_ap_args` which `MMWLAN_AP_ARGS_INIT` zeroes, so an application that never asks for RAW
  never advertises it. That is per-deployment rather than per-build, which the feature needs. Verified
  with a config-only A/B on one board: an app that never sets `cfg.ap.raw` emitted **0 EID-208 across
  351 beacons** with the caps bit clear, while the fixture emitted **387/387** with it set. The only
  remaining define is `RIMBA_RAW_SELFTEST`, which gates the encoder self-test export and nothing about
  RAW behaviour.
- **Any S0b fixture needs a `tools/regtest/manifest.py` entry** — a `firmware/` dir without one is a
  hard T0 failure — and it will be built by the T0 matrix, so it must compile clean even if never scored.
- **Q2 is a cost question, not a gate.** §3-S0 said "RPS stripped by re-conversion ⇒ BLOCKED", but §5
  and §6 both name the host-built-S1G-beacon fallback (the mesh pattern, `umac_mesh.c:210-218`) for
  exactly that failure. So Q2's two outcomes are *"splice 6 lines"* vs *"port the mesh beacon builder
  to the AP vif"* — neither changes GO/BLOCKED. **Sequence it after the gate, and stop calling it a
  decidable.**
- **Do not chase "wake up the hostapd RAW ops" as a cheaper path** — traced through, it dead-ends at
  opcode `0xA021`, the very thing Linux never sends to the chip. It needs two `#ifdef` edits to
  compile at all, net-new plumbing from `mmwlan_ap_args` to `hapd->conf->raw[]`, and a net-new
  morselib op behind `raw_priority_enable`. Strictly more work than the beacon splice, for nothing.
- **S5 IS A SETTER, NOT A PORT — confirmed by source 2026-07-30.** The AP-side RAW *input* path is
  **already compiled into the ESP build and merely dormant**, under `CONFIG_IEEE80211AH` (in
  BUILD_DEFINES) and in files that ARE in SRCS: the RAW-aware AID allocator `hostapd_get_raw_aid()`
  (`ieee802_11.c:3833`, dispatched `:3949`), the EID-89 parser `process_qos_traffic_cap()` (`:4518`,
  called `:5049`), plus `conf->raw_enabled` and `sta->raw_priority`. Both call sites are gated on
  `hapd->conf->raw_enabled`, which defaults **false** (`ap_config.c:190`) and whose only setters live in
  `hostapd/config_file.c` — **not compiled for ESP**. So S5 needs a setter reachable from
  `mmwlan_ap_args`, not a from-scratch port. *(This is a different layer from the dead
  `hostapd_send_raw_config()` path, which really is `#ifdef`'d out behind the absent
  `CONFIG_DRIVER_NL80211_MORSE` — do not conflate them.)*
  **Still blocked as written:** hostapd's groups are 256 AIDs wide up to 2007, while morselib caps AIDs
  at 255 (`traffic_bitmap.h:22`) and at `max_stas` (default 20), so any STA with `raw_priority ≥ 1` gets
  AID ≥ 256 and is rejected at `umac_ap.c:667`. S5 needs an ESP-specific priority→AID mapping inside
  1..255, not a straight reuse of `hostapd_get_raw_aid`.
- **Throughput A/B, nearly free alongside the counter run.** `aci_frames_delayed`/`bc_mc_frames_delayed`
  count the AP's chip withholding its **own** downlink. For the mesh-gate use case the gate is already
  the throughput bottleneck, so RAW could be a net negative. Measure RAW-off vs RAW-on on the same rig
  and load. A GO later reversed on throughput is worse than a slower decision.

### 7.5 Rig and radio budget

Four distinct radios, never more than three concurrent; fits one session if S0b-0/1 are done first.

| Run | Radios | Notes |
|---|---|---|
| S0b-1 (calibration) | chronium monitor + chronite plain AP | 2 |
| S0b-2 (all-Linux gate) | chronite AP + chronogen STA + chronium monitor | 3. **chronosalt unused** (power-marginal) |
| S0b-3/4 (ESP caps + Q2) | ESP AP + chronite Linux RAW AP + chronium monitor | 3. Beacon-only load ⇒ board0/board1 suffice |
| S0b-5 (Q3b) | ESP AP + STA(s) + chronium monitor | 3. chronite **stops beaconing** first. Sustained uplink ⇒ may need **board2**, which costs a `tools/ppk2_hold.py` session for the whole run |

chronium stays in monitor throughout. **Only chronium has `tcpdump`/`tshark`** (4.99.5 / 4.4.15) — the
other three nodes have neither, so the sniffer is not relocatable without installing tooling.
`docs/reference/rimba-linux-halow-monitor.md:71` ("No tcpdump/tshark on the Pis") is stale for chronium
and should be corrected. **TX-power discipline is a prerequisite, not a nicety**, and the earlier
claim here that "the ESP fixtures cap to 1 dBm" was **false — corrected 2026-07-31: the AP fixture family
does not.** `test-raw-cap`, `rimba-halow-ap` and `rimba-halow-ap-perf` never call
`mmwlan_override_max_tx_power`; `test-raw-rps` does, following the `test-apsta-ap` precedent, and the call
must sit **between `set_config` and `wifi_start`** because the override returns `MMWLAN_UNAVAILABLE` once
the WLAN subsystem is active. Nothing on the Linux side caps either, and the bench's documented
RX-overload at RSSI ≈ −3 dBm
produces retries **indistinguishable from slot leakage** in a timing capture. Cap both Linux ends and
record the observed RSSIs alongside every verdict. Also pin beacon interval, `dtim_period` and the
short-beacon decision **identically** across the Linux-AP and ESP-AP arms — the RAW window is
referenced to the beacon, so all three enter both the Q2 comparison and the Q3 histogram.

### 7.6 New tooling S0b needs (item 1 ✅ BUILT 2026-07-31 — `tools/raw_s1g_beacon_decode.py`)

**Items 1 and 2 are done and tracked.** `tools/raw_s1g_beacon_decode.py` is the radiotap-aware pcap
decoder, and it **computes** the conditional S1G offset from Frame Control rather than searching for it.
It is **calibrated**: it reproduces both archived S0b captures to the frame — the 236-beacon reference
(golden constant, byte-identical) and the 2222-beacon negative control (zero EID-208). Item 4's tracked
`hostapd-rimba-raw.conf` was **not** needed: driving `morse_cli` directly (as S0b-2 did) gives exact
control of the AID ranges and avoids hostapd's defaults entirely. Item 3's *timing* analysis is still
net-new — S0b-4 needed none of it, being a byte-diff.

⚠ **The S0b-1/S0b-2 analysis scripts did not survive** — they lived in a dev-box session scratchpad under
`/tmp` that the nightly reboot wipes. That is exactly why the decoder is now a tracked repo tool. The
*captures* were rescued from `chronium:/tmp` (tmpfs, 19-day uptime) into `docs/worklog/artifacts/raw-s0b/`.

Original list, for the record:

1. **A radiotap-aware decoder.** All four in-repo capture scripts (`tools/mesh_beacon_cap.py:31-32`,
   `tools/mesh_rann_cap.py:47-48`, `tools/mesh_grab_fwd.py:18`, and the recipe in the monitor doc)
   read `rtlen` and then **skip the entire radiotap header**, and all read a live `AF_PACKET` socket
   rather than a pcap. Q2 needs an IE-chain dump; Q3 needs `radiotap.mactime` out of a pcap.
2. **The conditional S1G-beacon offset.** `ies = f[15:]` is valid only when NEXT_TBTT / COMPRESS_SSID /
   ANO are clear (`morse_driver/dot11ah/ie.c:442-457` adds 3/4/1 bytes). **A wrong offset fabricates a
   false "RPS stripped" result** — the exact failure the spike must avoid. Record the observed FC per
   capture as evidence.
3. **Analysis scripts** — phase histogram, leakage fraction, two-sample KS, drop-delta integrity,
   frame-count reconciliation. Roughly 1-2 hours for the Q2 decoder; the Q3 analysis is net-new.
4. **A tracked `hostapd-rimba-raw.conf`** and a Linux infra-STA wpa config with `raw_sta_priority`.
   (An untracked working infra-STA conf exists at `chronium:~/wpa-sta.conf` for SSID `rimba-ping` —
   the SSID the ESP AP fixtures already use — so Linux-STA-to-ESP-AP has bench precedent, but it lives
   only on the sniffer node and has no `raw_sta_priority`.)

### 7.7 Decision rules — write these down before the bench, not after

| Observation | Verdict |
|---|---|
| S0b-1: timing σ fails **and** RAW stats section missing | **Not decidable on this bench** — re-scope or escalate. Do not run to an ambiguous negative |
| S0b-2: AP `assignments[]` flat, `invalid_assignments` flat, RPS provably on air | **BLOCKED** — no AP-direction engine in the blob. Zero ESP code spent |
| S0b-2: AP counters move + STA confines | **Q3a/Q4 GO** — proceed to the ESP arm |
| S0b-3: caps bit does not reach air | Not BLOCKED, but **S3 is dead as written** — reshape toward the host-built-S1G-beacon fallback before any RPS work |
| S0b-4: no EID-208 element on air | **Q3 not evaluable.** Implement the mesh fallback, re-capture. **Not a BLOCKED verdict** |
| S0b-4: element present, byte-matches Linux | Q2 PASS — host splice is the cheap path |
| S0b-5: ESP counters move like the Linux arm's | **HARD GO** — stage S1→S4 |
| S0b-5: ESP counters flat, element provably on air, beacon-lateness small | **Host-format mismatch** — port the mesh beacon builder to the AP vif; still not BLOCKED |
| S0b-5: ESP counters flat, beacon-lateness ≈ slot duration | **"The ESP can't serve beacons punctually enough for RAW"** — a different, more fixable problem. Do not report as BLOCKED |
| Any run whose **frame-count reconciliation** shows material loss | **Void the run.** No verdict. (Not the drop counter — S0b-1 measured 2.53 % loss against a drop delta of 1. Baseline loss on a healthy capture is ~2.5 %, so set the threshold against that, not against zero.) |
| A timing verdict computed against a **fitted TBTT grid** rather than the observed beacon | **Void the analysis.** S0b-1 measured one-sided beacon deferral to +3.7 ms — a third of a slot — which a grid-based phase would report as leakage |

**On GO:** open the S1 branch (superproject + `components/halow` submodule), fill §4's `:___` anchors
by grepping both trees, and proceed S1→S4, compile-verifying each stage and on-air re-verifying at S4.
Keep `k_rps_golden` as a **committed constant** so S1's builder has a deterministic unit test.

**On BLOCKED:** document the gap, and file it as a vendor/FW ask — **do not ship a RAW advertisement
the AP cannot enforce.** Note that the whole-S1G-beacon-host-build fallback answers a *Q2* failure only;
it does nothing for a Q3a failure.

**Radio-silent afterwards:** `rimba-hello` to every ESP + `ip link set wlan1 down` on chronium and
chronite. Manual — the harness safety net does not cover a hand-run spike.

**Bottom line:** the advertise-half is a clean, byte-diffable host-IE port modeled on the mesh RANN and
the existing S1G-TIM builder — low risk, ready to stage. The gate is **S0b-2**, and it costs no ESP
code: if the shared blob's RAW engine does not arm from a host-authored beacon on Linux hardware, it
will not arm on the ESP either, and the port stops there.