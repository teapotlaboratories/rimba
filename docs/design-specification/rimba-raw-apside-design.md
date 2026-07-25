# RAW (Restricted Access Window) — AP-side port: feasibility + design recon

**Status:** recon / feasibility **DONE 2026-07-24** — *not started*, gated on an on-air S0 spike. This is a
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
- **BUT the ESP beacon path is architecturally different from Linux, and that difference sits exactly on the enforcement boundary.** On Linux the beacon + RPS are FW-offloaded; the FW builds and serves them and enforces the slots it authored. On ESP the beacon is **host-built and host-served every TBTT** (Map 3 spine, `beacon.c:12-77`, `umac_ap.c:406-420`), then the FW *re-converts* the host's legacy-format beacon into an S1G beacon (Map 3 §1, `umac_mesh.c:214-218`). Two unknowns fall out: (a) does a host-appended RPS IE even survive that FW conversion, and (b) if the RPS reaches air, does the AP-direction FW-MAC actually gate the medium to it — the STA-side enforcement being proven does not prove the AP-side scheduler is present/active in the same image (Map 1 §7 part 2, Map 2 Q2).
- **No CONFIG_RAW chip command exists in morselib.** The morselib chip-protocol enum `enum morse_cmd_id` has no RAW opcode (Map 2 Q2, `morse_commands.h:15-99`); `MORSE_CMD_ID_BSS_BEACON_CONFIG=0x003D` carries only an `enable` flag, no beacon body/IE payload (`:379-383`). `MORSE_CMD_ID_CONFIG_RAW=0xA021` exists *only* in hostapd's vendored copy of the Linux protocol and is sent *only* by `nl80211_raw_priority_enable()` in `driver_nl80211.c` — **which is not compiled for ESP** (Map 2 Q2, `CMakeLists.txt` builds only `drivers_morse.c` + `driver_common.c`). The compiled-in hostapd RAW code (`hostapd_get_raw_aid`, `hostapd_send_raw_config`) is a dead no-op: it guards on `driver->raw_global_enable`/`raw_priority_enable`, which `mmwlan_wpas_ops_ap` does not define, so it logs "Driver interface not defined" and returns (Map 2 Q1c, `hostapd.c:2148-2151,2167-2171`; `driver_ap.c:767`).

**What this resolves to:** if the RPS IE is a *beacon-borne advertisement the FW enforces* (like it evidently is for STAs), the port is a tractable host-IE feature directly analogous to the mesh RANN — **GO**. If AP-direction enforcement turns out to be absent from the ESP `.mbin` (only STA-direction present), then RAW is **BLOCKED**: it would need host-side airtime scheduling the chip does not expose (no host TSF/TBTT clock, no medium-access gate — Map 3 §3, `umac_ap.c:376-389`, `umac_mesh.c:227`) and an FW change we cannot make.

**What remains an on-air-spike unknown (source-undecidable):**
1. Does the shipped ESP `.mbin` advertise `MORSE_CAPS_RAW` / `MORSE_CAPS_PAGE_SLICING`? (Map 2 Q1a — runtime `mmprobe_dump_fw_caps`, `driver.c:155`).
2. Does a host-appended RPS IE survive the FW's legacy→S1G beacon re-conversion and reach air intact? (Map 3 §1).
3. Does the AP-direction FW-MAC actually confine a RAW-assigned STA to its slot? (Map 1 §7 part 2, Map 2 Q2/item 3).
4. Does the MM6108 STA firmware honor a received RPS for UL self-restriction end-to-end? (Map 2 item 4 — the STA half).

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

**S0 — FEASIBILITY SPIKE (the true gate; blocking).**
Author a minimal, hand-built valid RPS IE (EID 66 + Length + one GENERIC assignment covering all AIDs) into the host AP beacon tail, flip the AP S1G-caps RAW bit, and take it on-air:
- Boot-probe `mmprobe_dump_fw_caps()` — record whether `MORSE_CAPS_RAW`/`MORSE_CAPS_PAGE_SLICING` are set (Map 2 Q1a, `driver.c:155`).
- Capture the ESP AP's S1G beacon on **chronium's morse0 monitor**; confirm the RPS element survives the FW legacy→S1G re-conversion (Map 3 §1) and **byte-diff it against a live Linux RAW AP beacon** (gold standard — the on-air frame-verification rule, `.ai/AGENTS.md`).
- Join a RAW-capable STA and **measure whether it actually confines TX to its slot** (airtime capture), byte-diffed vs the Linux RAW AP's STA behavior.
- **Decision:** RPS survives + STA restricts ⇒ proceed to S1 (GO). RPS stripped by re-conversion, or STA ignores it ⇒ BLOCKED; escalate (FW-offloaded-beacon path or vendor FW ask) before any further work.

**S1 — RPS payload builder (follow-Linux, byte-identical).**
Port `raw.c`'s RPS-payload assembly into a net-new `umac/ies/ie_rps.c` (sibling of `s1g_tim.c`): RAW Control octet (Map 1 §1c), Slot Definition (§1d), RAW Group hierarchical-AID packing (§1e-2). Add `DOT11_IE_RPS=66` to `dot11/dot11.h` and a `struct dot11_ie_rps` (all net-new — Map 3 §1). GENERIC-only, format-0, single group. Compile-verify. Unit-diff the produced bytes against the S0 hand-built reference.

**S2 — EID framing + beacon splice.**
Add the `Element ID(66)+Length` wrapper (payload-only from the builder — Map 1 §1a/§5 footgun) and the insertion call `ie_rps_build(buf, …)` at the exact hook: **`umac_ap_build_beacon()` between line 418 and 419**, after `ie_s1g_tim_build` and before the `config.tail` append (Map 3 §1, `umac_ap.c:406-420`), using the `consbuf_reserve`/`consbuf_append` primitives modeled on the RANN branch (`umac_mesh.c:2341-2346`). Compile-verify; re-run the S0 capture to confirm the dynamically-built IE matches.

**S3 — AP config surface + enable knob.**
Add a RAW field to `mmwlan_ap_args` (absent today — Map 1 §7, `mmwlan.h:2012-2135`) and an enable/disable + config entry point mirroring `morse_raw_process_cmd` / `morse_raw_enable` (Map 1 §4, `raw.c:1559-1583,1137-1141`), AP-iftype-gated (`raw.c:1567`). Wire the AP S1G-caps RAW advertise bit on when enabled (Map 3 §2, `s1g_capabilities.c:276-280`). Compile-verify.

**S4 — AID-list + single-group mapping.**
Build the ordered connected-AID list from the ESP dense-AID allocator/bitmap (`umac_ap_alloc_sta umac_ap.c:663-688`, `data->bitmap` `umac_ap_data.h:30`, `aid_is_valid traffic_bitmap.h:27`) — the ESP analog of `morse_generate_aid_list` (Map 1 §2, `raw.c:247-272`). Map config `start_aid/end_aid` to the live range; refresh on STA join/leave (analog of `morse_raw_refresh_aids raw.c:841-871`). Compile-verify. **This is the MVP finish line** — on-air re-verify the full path.

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
| EID(66)+Length framing + splice | dot11ah beacon-rewrite (NOT in recon files — Map 1 §5) | `umac_ap.c` between `:418` and `:419` (Map 3 §1) `:___` |
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

- **[BLOCKING] AP-direction FW enforcement may be absent.** STA-side enforcement is proven (Map 1 §7); AP-side slot scheduling in the same ESP `.mbin` is unverified. The only per-STA "scheduling" surfaced host-side is TWT, and the FW **refuses AP-side TWT schedule installs** (`umac_twt.c:578-590`, returns `0xffff8000` on AP vifs) — AP serving is degraded to downlink buffer-and-release keyed on a sleep bool, never an airtime gate (Map 3 §3). There is no host TSF/TBTT clock (beacon `time_stamp` left 0 for FW — `umac_mesh.c:227`). If AP enforcement isn't in FW, RAW cannot be made real on this chip from the host. **Settled only by S0 item 3.**
- **[HIGH] FW legacy→S1G beacon re-conversion may strip/mangle the host RPS IE.** ESP AP beacons are host-built then FW-re-converted (Map 3 §1, `umac_mesh.c:214-218`); an appended RPS may not survive. The mesh sidesteps this by emitting the whole S1G beacon host-side — a fallback path if the AP re-conversion drops RPS. **S0 item 2.**
- **[MED] `MORSE_CAPS_RAW` may be unset in the shipped blob.** The enum bit exists (`mmdrv.h:549`) but whether the `.mbin` sets it is runtime-only (Map 2 Q1a). Advertising RAW support with a non-enforcing FW would be actively harmful (STAs restrict, AP doesn't gate → wasted airtime).
- **[MED] STA-side UL self-restriction end-to-end.** morselib has no RPS parser (Map 2 Q1b/Q3); the STA half relies entirely on the MM6108 blob. Must confirm a real STA both parses the RPS and confines uplink contention. **S0 item 4.**
- **[LOW] Byte-fidelity footguns (mechanical, replicate exactly):** builder returns payload only — add EID 66 + Length yourself (Map 1 §1a/§5); replicate the literal num_slots cap 6/3 not the field's 63/7 (Map 1 §1d/§7, `raw.c:312,321,324`); never emit the Channel Indication 2 bytes (Map 1 §1e-3, `raw.c:472-474`); duration is `500 + cslot·120`, start-time in 2-TU units (`raw.c:103,122`).
- **[LOW] Config surface is net-new API.** `mmwlan_ap_args` has no RAW field and there's no AP RPS function today (Map 1 §7, Map 2 Q3); adding them is additive but touches the public header — needs CMake `target_compile_definitions` discipline for any test overrides (the CMake `target_compile_definitions` pattern).

---

## 6. NEXT-STEP TODO

Create **`docs/design-specification/rimba-raw-apside-design.md`** (promote this recon into it), and cross-link from `docs/mesh-ap/` since the mesh-gate scheduling use case is the driver.

1. **Write the S0 spike harness** (before any morselib change):
   - Hand-build a minimal valid RPS IE constant (EID 66 + Length + one GENERIC all-AIDs assignment, format-0, 2 slots) per Map 1 §1 layout; keep it as the golden byte reference.
   - Add a temporary hook at `umac_ap.c:418` to append it, and flip the AP S1G-caps RAW bit (`s1g_capabilities.c:276-280`). Build via `make build/flash APP=… BOARD=proto1-fgh100m` (never bare `idf.py` — the make/BOARD build rule (`.ai/AGENTS.md`)).
   - Add a boot-time `mmprobe_dump_fw_caps()` print (Map 2 item 1).
2. **Stand up the Linux RAW reference** on chronium: a live Morse RAW AP emitting a known RPS, captured on `morse0` monitor, to byte-diff against (the on-air frame-verification rule (`.ai/AGENTS.md`); re-scp the Linux reference from chronium each session (non-persistent)). Confirm all Morse FW + driver at standard **1.17.8** before testing (the matched-firmware rule (whole bench on one fw)).
3. **Run S0 and record the 4 decidables** (caps advertised? RPS survives re-conversion + byte-matches Linux? STA restricts UL to slot? STA parses RPS?) into the design doc's Feasibility section. This flips the verdict to hard GO or hard BLOCKED.
4. **On GO:** open the S1 branch (superproject + `components/halow` submodule), start the code-map table in §4 with real `:___` line numbers grepped in both trees, and proceed S1→S4 to the MVP, compile-verifying each stage and on-air re-verifying at S4.
5. **On BLOCKED:** document the FW-enforcement gap, evaluate the whole-S1G-beacon-host-build fallback (mesh pattern) for item-2 failures only, and file the AP-enforcement question as a vendor/FW ask — do not ship a RAW advertisement the AP cannot enforce.
6. Radio-silent after the S0 bench run: flash `rimba-hello` to the ESPs + `ip link set wlan1 down` on the Linux sniffer/peer (the radio-silent-after-every-test rule).

**Bottom line:** the advertise-half is a clean, byte-diffable host-IE port modeled directly on the mesh RANN and the existing S1G-TIM builder — low risk, ready to stage. The entire feature's viability rests on S0 proving the MM6108 firmware enforces AP-direction RAW slots on-air; that single spike is the gate between GO and BLOCKED, and it is source-undecidable — it must be run on the bench.