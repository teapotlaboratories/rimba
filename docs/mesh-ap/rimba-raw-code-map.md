# RAW (Restricted Access Window) — Linux ↔ ESP code map

Function-level mapping for the RAW AP-side port, as required by
[`.ai/AGENTS.md` → Porting Linux code](../../.ai/AGENTS.md#porting-linux-code). Every `file:line` below
was verified by grepping **both** trees on the date stamped here — none is cited from memory.

| | |
|---|---|
| **Linux reference** | `morse_driver` at tag **1.17.8**, read on `chronite:~/halow/morse_driver` — the same revision the bench radios run |
| **New code** | `components/halow` (submodule `teapotlaboratories/mm-esp32-halow`) |
| **Verified** | 2026-07-31 |
| **Stage** | **S1 + S2 + S3 complete.** S4–S6 rows are listed with empty targets so the map stays the working document the port fills in. |

**Why 1.17.8 anchors are safe:** `raw.c`, `raw.h` and `dot11ah/dot11ah.h` have **identical git blob OIDs**
at tags 1.17.8 and 1.17.9 (`raw.c 030b5e3a…`), verified on both bench nodes during S0b-0. Only 5 of 134
driver files changed between the releases and all three C changes are RX-side input validation. So every
anchor here is valid verbatim for either release.

---

## S1 + S2 — the RPS element encoder (done)

| Concern | Linux (`morse_driver` 1.17.8) | New code |
|---|---|---|
| RPS payload size calculation | `morse_raw_calc_rps_ie_size()` `raw.c:378-418` | `rps_calc_payload_len()` `ies/ie_rps.c:192` |
| Slot Definition encode | `morse_raw_generate_slot_definition()` `raw.c:287-357` | `rps_generate_slot_definition()` `ies/ie_rps.c:118` |
| Assignment + Group encode | `morse_raw_generate_assignment_with_aid_range()` `raw.c:425-488` | `ie_rps_build()` `ies/ie_rps.c:214` |
| Element assembly | `morse_raw_generate_rps_ie()` `raw.c:616-672` | `ie_rps_build()` `ies/ie_rps.c:214` |
| PRAW predicate | `morse_raw_cfg_is_periodic()` `raw.h:303-306` | `rps_cfg_is_periodic()` `ies/ie_rps.c:103` |
| RAW Control masks / flags | `raw.c:15-16,33-38` | `ies/ie_rps.c:24-30` |
| Slot Definition masks | `raw.c:47-66` | `ies/ie_rps.c:33-67` |
| RAW Group hierarchical-AID masks | `raw.c:69-79` | `ies/ie_rps.c:70-77` |
| Duration quantisation `500 + 120k` | `MORSE_RAW_MIN_SLOT_DURATION_US` / `US_TO_CSLOT` `raw.c:95,103` | `ies/ie_rps.c:80-81` |
| Start time in 2 TU units | `US_TO_TWO_TU` `raw.c:122` | `ies/ie_rps.c:84` |
| Wire structs (assignment / start-time / group / periodic) | `raw.c:165-169,180-182,198-201,229-235` | flattened into a byte writer — see divergence 3 |
| RAW type enum (GENERIC = 0) | `enum ieee80211_s1g_rps_raw_type` `raw.h:57-62` | `RPS_RAW_TYPE_GENERIC` `ies/ie_rps.c:87` |
| Element ID 208 | `WLAN_EID_S1G_RPS` `dot11ah/dot11ah.h:37` | `DOT11_IE_S1G_RPS` `dot11/dot11.h:2341` |
| Element ID + Length framing | caller's job — `dot11ah/ie.c:467-468` | emitted by the builder — see divergence 2 |
| Beacon splice point | ordered insert `dot11ah/ie.c:29-58` | `umac_ap.c:444` (`ie_rps_build`), config at `:433` |
| Public config type | `struct morse_raw_config` `raw.h:124-215` | `struct ie_rps_config` `ies/ie_rps.h:53` |

## S3 — config surface and enable knob (done)

| Concern | Linux (`morse_driver` 1.17.8) | New code |
|---|---|---|
| Public RAW config | `struct morse_raw_config` `raw.h:124-215` | `struct mmwlan_ap_raw_args` `include/mmwlan.h:2025`, carried on `mmwlan_ap_args` `:2172` |
| Config validity gate | `morse_raw_is_config_valid()` `raw.c:1046-1052` | inside `umac_ap_validate_ap_args()` `umac_ap.c:91-109` |
| AP-iftype gate | `morse_raw_process_cmd()` rejects non-AP twice — `raw.c:1566-1573` | implicit: the config lives on `mmwlan_ap_args`, and `umac_data_get_ap()` returns NULL off an AP vif |
| "Is RAW enabled" predicate | `morse_raw_is_enabled()` `raw.c:1630-1632` | `umac_ap_raw_is_enabled()` `umac_ap.c:73` |
| AP S1G-caps RAW advertise bit | set `mac.c:1228-1229`, re-cleared unless live `mac.c:1392-1393` | `ies/s1g_capabilities.c:294` — now gated on the predicate, see divergence 10 |
| Schedule → element | `morse_raw_generate_rps_ie` config list | built from `data->args.raw` at `umac_ap.c:467-488` |

## S4–S6 — not yet ported

| Concern | Linux | New code |
|---|---|---|
| Config command parse (netlink) | `morse_raw_process_cmd` `raw.c:1559-1583`; `morse_raw_cmd_to_config` `raw.c:1062-1131` | **not ported** — see divergence 11 |
| AID list build | `morse_generate_aid_list` `raw.c:247-272` | *(S4)* — reuse `umac_ap` dense AID + `data->bitmap` |
| AID refresh on join/leave | `morse_raw_refresh_aids` `raw.c:841-871` | *(S4)* |
| QoS-UP → priority learn | `morse_raw_process_rx_mgmt` `raw.c:873-915` | *(S5)* |
| Priority → AID group | AID bits[10:8] `raw.h:20-27` | *(S5)* |
| Beacon-sent cadence hook | `morse_raw_beacon_sent` `raw.c:1585-1597` | *(S6)* |
| PRAW cadence | `morse_raw_start_praw_transmission` `raw.c:715-734` | *(S6)* — the encoder already emits the PRAW field |

---

## Deliberate divergences

Every difference from the reference, with the reason. Nothing here is an accident.

**1. No persistent element buffer, and no allocation.**
Linux `kmalloc`s an `rps_ie` buffer onto the vif and regenerates it only when the config changes
(`raw.c:637-655`), then the beacon path copies the cached bytes in. The ESP builder writes straight into
the beacon's `consbuf` on every TBTT. *Why:* the ESP beacon is assembled from scratch each time by
`umac_ap_build_beacon()`, so a cache would be pure overhead, and morselib avoids heap churn on the beacon
path. *Consequence:* `raw->rps_ie` / `rps_ie_len` and their invalidation dance have no analogue, and
neither do the `WARN_ON`s that guard them (`raw.c:666,669`) — replaced by a single length assertion at
`ie_rps.c:325`.

**2. The builder emits Element ID and Length; Linux's does not.**
Linux's builder produces the element **payload only**, and the EID + Length are written by the IE
serialiser (`dot11ah/ie.c:467-468`). *Why:* the ESP sibling `ie_s1g_tim_build()` is EID-framed, and the
splice site appends a complete element. Matching the sibling keeps the two beacon IE builders
symmetrical. *Consequence:* `IE_RPS_MAX_LEN` includes the 2-byte header, and the design doc's separate
"S2 — EID framing" stage collapses into S1, as §3 anticipated it would.

**3. Wire structs flattened into a byte writer.**
Linux casts a `u8 *` to `struct ieee80211_s1g_rps_raw_assignment` / `morse_raw_group_t` etc. and advances
pointers (`raw.c:429-487`). The port writes bytes explicitly. *Why:* those structs are `__packed` with
`__le16` members, and the pointer-advance idiom depends on packed layout and host endianness holding.
Explicit little-endian byte writes are endian-correct by construction and make the on-air layout readable
next to the field diagram. *Consequence:* field **order** is now enforced by code order, so the assignment
comment at `ie_rps.c:281-286` records what the struct used to encode.

**4. One GENERIC assignment, not a config list.**
Linux iterates `config_list[num_configs]` and concatenates assignments (`raw.c:661-664`). *Why:* S1 scope;
multiple assignments arrive with per-priority grouping at S5. *Consequence:* Linux's unsupported-type
`WARN_ON`s (`raw.c:393-399`) become a compile-time constant instead of a runtime branch.

**5. Two-pass sizing — an ESP mechanism with no Linux counterpart.**
`build_frame_with_class()` (`frames/frame_constructor.c:17-32`) runs every beacon builder twice: once with
a NULL buffer to size the allocation, then again to fill it. The builder detects the sizing pass with
`consbuf_reserve(buf, 0) == NULL` and reserves `IE_RPS_MAX_LEN`, copying `ie_s1g_tim_build()`
(`ies/s1g_tim.c:420-425`) rather than the mesh RANN branch, which is fixed-length and ignores the split.

> ⚠ **The contract is an UPPER BOUND, not an equality — and the design doc's §5 wording ("a pass-1/pass-2
> size disagreement is a hard hang") is imprecise enough to mislead.** The frame's final length is taken
> from **pass two** (`mmpkt_append(view, cbuf.offset)`, `frame_constructor.c:32`); pass one only sizes the
> allocation. `ie_s1g_tim_build` proves it — it reserves 256 bytes and writes at most 45. Only
> **under**-reserving is dangerous, and that direction really does hang: `consbuf_append`'s
> `MMOSAL_ASSERT` (`common/consbuf.c:30`) is live in this build and expands to an infinite loop.
> **Padding the element out to the reserved length to "balance" the passes is a bug**, not a safety
> measure: the bytes that follow are hostapd's tail, so padding desynchronises every subsequent element
> in the chain. This was written that way first and caught before it reached the air.

**6. The slot-count cap is replicated, including its write-back.**
Linux assigns `max_slots` from `IEEE80211_S1G_RPS_RAW_SLOT_NUM_{3,6}BITS` — the field *widths* 3 and 6,
not the field maxima 7 and 63 (`raw.c:318-325`) — then caps and **writes the capped value back into the
caller's config** (`raw.c:329-333`). Both behaviours are reproduced at `ie_rps.c:159-167`. *Why:*
byte-fidelity with the reference is the point of the port, and the cap is observable on air. *Note:* the
write-back is idempotent, so it cannot change the emitted bytes — but it does mean a caller that asks for
8 slots finds its own config permanently reading 6.

**7. The 6-bit slot-count mask spans a bit outside its field, and that is preserved.**
`IEEE80211_S1G_RPS_RAW_SLOT_NUM_6` is `GENMASK(16, 10)` (`raw.c:65`) — bit 16 lies outside the `__le16`
it is OR'd into. Transcribed as a 16-bit mask (`ie_rps.c:65`) so behaviour is identical. Harmless only
because of the ≤6 cap above; a port that raised the cap to the field's true 63 would silently lose bit 16
on both stacks.

**8. Logging dropped.** Linux's `MORSE_RAW_WARN`/`MORSE_RAW_DBG` calls on the capping paths
(`raw.c:296,329,335`) have no analogue. *Why:* morselib's log does not reliably reach the ESP console —
the same reason S0a added read-only accessors instead of using `mmprobe_dump_fw_caps()`. The capping
conditions are silent; the code map and the header comment carry the warning instead.

**10. The S1G-caps RAW bit is evaluated ONCE, at AP start — Linux re-evaluates it per frame.**
Linux sets the bit per vif from the firmware capability (`mac.c:1228-1229`) and then **re-clears it on
every beacon and every management frame** unless RAW is running (`mac.c:1392-1393`). The ESP cannot: the
capabilities element is built by `ie_s1g_capabilities_build_ap()` and memcpy'd into the supplicant's
`hw_mode->s1g_capab` (`driver_ap.c:130-139`), which caches it for the AP's lifetime — hostapd then emits
those cached bytes from `hostapd_eid_s1g_capab()` into every beacon tail. There is no per-beacon
capabilities rebuild to hook. *Why it is still correct:* RAW is configured through `mmwlan_ap_args` and
therefore fixed for the AP's lifetime, so a value evaluated once at start is never stale. *Consequence,
and it is a real API constraint:* **there is no runtime RAW enable/disable.** Toggling it after
`mmwlan_ap_enable()` would leave the advertisement lying, which is worse than not offering the knob. The
public header says so. The ordering that makes even the once-at-start evaluation safe is that
`umac_ap_enable_ap()` stores the args (`umac_ap.c:261`) before starting the supplicant (`:326`) — and the
caps builder is reached only through the supplicant, twice, both after that point.

**11. No command-parse layer.**
Linux's config arrives as a netlink command and is translated by `morse_raw_cmd_to_config()`
(`raw.c:1062-1131`) after `morse_raw_process_cmd()` (`raw.c:1559-1583`) rejects non-AP vifs. morselib has
no command layer, so the "command" is a struct on the public AP args and the translation is a field copy
at `umac_ap.c:467-488`. The two iftype rejections collapse into the structure itself: RAW config only
exists on `mmwlan_ap_args`, and `umac_data_get_ap()` returns NULL unless an AP vif is up.

**12. Validation happens at AP-enable, not at beacon-build.**
Linux validates when the command arrives. The port validates inside `umac_ap_validate_ap_args()`, so a bad
schedule fails `mmwlan_ap_enable()` loudly instead of silently emitting a malformed element on every
beacon thereafter. The predicate itself is Linux's, including its caveat: *"Note that AID ranges are not
required for the spec, but we do require it for now"* (`raw.c:1047-1049`).

**9. Channel Indication is never emitted — same as Linux.** Not a divergence, recorded because it is an
easy thing to "fix" wrongly: the reference explicitly declines to emit the 2-byte subfield
("channel indication subfield not supported", `raw.c:474`) while still stepping the pointer past it. The
port simply omits it.

---

## Verification

| Check | Result |
|---|---|
| Encoder output vs live-Linux reference bytes | **PASS** — `D0 06 20 40 09 04 E0 1F`, byte-identical, on-device self-test `TEST\|STEP\|rps-encoder-golden` |
| Encoder output on air | **PASS** — 425/425 beacons, byte-identical, 0 undecodable |
| IE chain intact after the swap | **PASS** — `[0, 5, 208, 48, 127, 244, 213, 217, 232, 214, 221]`, unchanged from the hardcoded-constant runs |
| Two-pass sizing (no hang, no chain desync) | **PASS** — implied by the two rows above; a mis-sized pass would hang the AP task or corrupt every following element |
| **S3: RAW off by default** (`rimba-halow-ap`, never sets `cfg.ap.raw`) | **PASS** — **0 EID-208 across 351 beacons**, caps bit clear 0/351, IE chain identical to the pre-RAW baseline |
| **S3: RAW on via `mmwlan_ap_args`** | **PASS** — **387/387 beacons**, byte-identical element, caps bit set 387/387, 0 undecodable |

**Why the default-off row matters.** S3 removed the `#ifdef RIMBA_RAW_S0B_SPIKE` that had been keeping RAW
out of the other 26 AP-capable apps. The containment is now `MMWLAN_AP_ARGS_INIT` zeroing the config —
which is the honest mechanism rather than a build guard, but only if it actually holds. The two rows above
are the same board and the same morselib, differing **only in configuration**.

**Test design note.** The expected bytes live in the *application*
(`firmware/test-raw-rps/main/app_main.c`), not next to the encoder. Had they lived in `ie_rps.c` the test
would compare the encoder against itself and pass for any self-consistent-but-wrong encoding. The
application-side constant is the byte string captured off air from a live Linux AP, so the assertion is
against an external reference rather than against the port's own opinion.
