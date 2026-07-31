# 2026-07-30 — RAW S0b-0: the desk checks pass, and two findings change the bench plan

**Goal.** Run the off-bench stage of the RAW S0b spike (design doc §7.2) before booking a multi-radio
session: settle the `.mbin`↔`.bin` segment equivalence that the whole reordered plan rests on, settle
whether the RPS encoder drifted between the driver version the anchors were read from and the one the
bench runs, and make the already-dirty submodule tree restorable.

**Outcome.** All three pass. The version question turned out not to exist. But two things came out that
**change how the bench session must be configured**, and neither was in the recon: hostapd's RAW defaults
do not emit the golden constant, and the driver silently caps `num_slots` at **6**, not 63.

**Nothing was committed.** No radio was brought up; the bench was read-only throughout.

---

## 1. Patch capture — do this first, not last

The submodule was **already dirty** with the uncommitted S0a accessors, and S0b will stack a beacon hook
plus the S1G-caps flip on top of it. Three unrelated uncommitted changes in one submodule with no way to
revert one without the others is not a state to start a bench session in.

Captured to the session scratchpad, each verified with `git apply --check --reverse`:

| | |
|---|---|
| `00-s0a-morselib-accessors.patch` | the submodule delta (both accessors) |
| `01-s0a-manifest-entry.patch` | the `manifest.py` APPS row |
| `02-s0b-scoping-docs.patch` | every doc edit from the scoping pass |
| `03-s0a-fixture-test-raw-cap.tar.gz` | the untracked fixture |
| `04-s0a-worklog.md` + both HTML renders | |

## 2. Is the ESP's firmware the same image the Linux nodes load?

**CONFIRMED at the executable-segment level.** This is the load-bearing premise of the reordered plan —
"the RAW engine's behaviour measured on Linux hardware transfers to the ESP chip" — so it was worth
proving rather than asserting.

The source ELF was already known identical. Three independent confirmations:

```
md5sum vendor/morse-firmware/firmware/mm6108.bin   → cfe56db2706458050dcc60baddabc632  (480664 B)
ssh chronium md5sum /lib/firmware/morse/mm6108.bin → cfe56db2706458050dcc60baddabc632  (480664 B)
ssh chronium dmesg | grep morse → "Loaded firmware from morse/mm6108.bin, size 480664, crc32 0x9edcc720"
   local zlib.crc32 of the same file → 0x9edcc720
```

**The open half was the repack.** The ESP build converts that ELF to Morse's TLV `.mbin` at build time
(`cmake/mm-fw-gen/firmware/CMakeLists.txt:37,55,77`), and nobody had checked the conversion was lossless.
It is. A dependency-free ELF32 parser plus an independent TLV walker reconstructed a flat address→byte
image from the container and compared it against the raw section bytes at `sh_offset`:

| Section | Addr | Size | |
|---|---|---|---|
| `.host_imem` | `0x100000` | 97726 | byte-identical |
| `.host_dmem` | `0x80100000` | 8012 | byte-identical |
| **`.mac_imem`** | `0x120000` | **154912** | **byte-identical** — this is the code section a RAW engine would live in |
| `.mac_rodmem` | `0x150000` | 4166 | byte-identical |
| `.mac_dmem` | `0x80200000` | 21416 | byte-identical |
| `.uphy_imem` | `0x1f0000` | 64744 | byte-identical |
| `.uphy_dmem` | `0x80300000` | 14648 | byte-identical |
| `.lphy_imem` | `0x158000` | 28610 | byte-identical |
| `.lphy_dmem` | `0x80400000` | 5344 | byte-identical |

Details worth recording so this is not re-litigated:

- **Our build passes no `--compress`**, so payloads are stored raw — 16 `FW_SEGMENT` (`0x8001`) TLVs, zero
  `FW_SEGMENT_DEFLATED` (`0x8002`). The converter's own log agrees: *"Wrote 16 uncompressed segments,
  0 compressed segments, 103 TLVs."* The deflate path in `firmware_mbin.c` exists but is not exercised here.
- **Nothing executable is dropped.** All nine ALLOC+EXEC PROGBITS sections map 1:1 onto `PT_LOAD` segments,
  and no segment address hits the converter's `SKIP_ADDRESSES = [0x00400000, 0x00C00000, 0x08000000]`
  (`convert-bin-to-mbin.py:41`). The `Ignoring section .mac_imem` lines in the converter log are a red
  herring — the *section* loop ignores them and the separate *segment* loop is what emits them.
- **What is dropped is inert:** `*_offchip_stats`, `.symtab`, `.strtab`, `.shstrtab` — all non-ALLOC, and
  the Linux loader's section loop has an empty body, so it writes nothing from sections at all.
- `.fw_info` is not lost either — it is re-encoded losslessly as 85 TLVs, payload-identical and in order.

**The one divergence, stated rather than glossed:** the two loaders pad the word-alignment tail
differently — ESP fills `0x00` (`convert-bin-to-mbin.py:135-138`), Linux fills `0xff`
(`firmware.c`, `memset(fw_buf + phdr.p_filesz, 0xff, padded_size - phdr.p_filesz)`). That is **exactly 6
bytes at 3 addresses** (`0x117dbe`, `0x151046`, `0x15efc2`), all strictly outside any section's contents.
**`.mac_imem` is unaffected** — its `filesz 0x25d20` is already word-aligned, so it receives zero padding.

Also refuted: the earlier concern that the ESP might flash the SDK-bundled
`components/halow/.../morsefirmware/mm6108.mbin`. It does not — that file is referenced only by the
PlatformIO builder (`utils.py:318`) and there is no `platformio.ini` in the repo. It is also demonstrably a
*different* firmware (md5 `67f10d6a…`, 399796 B, vs the built 401076 B).

## 3. Did the RPS encoder change between 1.17.8 and 1.17.9?

**No — and the question dissolves entirely.** The concern was that every anchor in the design doc, and the
golden constant, came from chronium's 1.17.9 source while the bench runs 1.17.8, so a Q2 byte-diff mismatch
could be a version delta rather than an ESP defect.

The three relevant files have **identical git blob OIDs at both tags**:

```
raw.c              030b5e3a1672828203323c93014a29d7c1fa3684
raw.h              5d097b83c8c0aac3d9ecf0a85dce0ecca2f44abf
dot11ah/dot11ah.h  51d44b711decaccd957c49103a3f1940e447d769
```

verified by `git ls-tree 1.17.8` on chronite and `git ls-tree 1.17.9` on chronium. Widening the check:
**only 5 of 134 tracked files changed** between the releases — `Makefile`, `dot11ah/Makefile`,
`dot11ah/main.c`, `page_slicing.c`, `vendor.c` — and all three C changes are **RX-side input-validation
hardening** against malformed *received* IEs (length guards before an S1G-caps parse, a bound before an OUI
`memcmp`, a TIM-length guard plus a literal-3 → `offsetof()` swap that is arithmetically identical). It is a
security-patch release, not a feature release. `beacon.c` and `dot11ah/ie.c` — the only sites that write EID
208 and the IE ordering table — are unchanged.

Belt and braces: `raw.o` compiled inside each tree has byte-identical `.text`
(`sha256 34a898c3…`) and `.rodata` (`5742e551…`); the disassembly differs only in the ELF header banner line.

**Consequence:** every `raw.c`/`raw.h`/`dot11ah.h` line-number anchor in the design doc is valid verbatim
for 1.17.8, and a Q2 mismatch cannot be blamed on driver version. Do not spend a bench slot aligning
versions — there is nothing to align on the RPS path.

Incidental, worth knowing: `morse_cli` tags 1.17.8 and 1.17.9 are the **same commit** (`8f06222`), and
`hostap` is at tag 1.17.9 on both chronium and chronite. So the bench will run a 1.17.9 hostapd against a
1.17.8 driver — but since hostapd only shells out `morse_cli raw …`, the coupling is a text command line,
not an ABI.

## 4. The golden constant, re-derived independently

An independent agent, deliberately not told the diff result, re-derived all eight octets from **1.17.8**
source alone, transcribed the encoder region verbatim into a compilable C file, and ran it:

```
{ 0xD0, 0x06, 0x20, 0x40, 0x09, 0x04, 0xE0, 0x1F }   payload_len=6  writer_advanced=6
```

Exact match. One point is **stronger than the design doc claimed**: octet `[2]=0x20` is not merely correct
for these parameters, it is unconditional for any generic RAW without start-time/PRAW/channel — `GROUP_IND`
is set with no guard (`raw.c:456`) and the PSTA/RAFRAME `TYPE_OPTION` bits are **dead code** in 1.17.8
(defined at `raw.c:25-26,29-30`, zero use sites), so RAW Control bits 3:2 are always 0.

Two small corrections to the §7.1 table, no byte changes: octet `[1]` is written by the **caller** too
(`dot11ah/ie.c:468`, alongside the EID at `:467`), not by `raw.c`; and the format threshold is coded as
`if (cslot > __UINT8_MAX__)` (`raw.c:318`).

## 5. The two findings that change the bench plan

Neither was in the recon, and both would have cost a session.

### 5a. hostapd's RAW defaults do not emit the golden constant

`hostap/src/ap/ap_config.c:191-199` defaults every RAW block to:

```c
bss->raw[ii].start_time_us = 4096;
bss->raw[ii].duration_us   = 26900;
bss->raw[ii].slots         = 1;
bss->raw[ii].cross_slot    = false;
```

Compiled against the real encoder, those emit a **nine**-byte element — `D0 07 30 70 07 02 04 E0 1F` —
because `START_IND` BIT(4) raises RAW Control to `0x30` (`raw.c:449`) and an extra start-time octet
`0x02 = US_TO_TWO_TU(4096)` appears.

**Left at defaults, the S0b-2 reference capture will not match `k_rps_golden`, and the mismatch reads as an
encoder or firmware finding when it is a config error.** The RAW block must explicitly pin
`start_time_us=0`, `duration_us=20200`, `slots=2`, `cross_slot=0`, `praw_period=0`,
`nominal_stas_per_beacon=0`, `priority=0`. Added to the confound matrix as row (j): a captured Length of
`0x07` with RAW Control `0x30` is hostapd's defaults leaking through.

`cross_slot=1` is a similar trap on its own — it flips `[3]` from `0x40` to `0x42` (`raw.c:306-307`), a
one-bit diff that looks exactly like an encoder mismatch.

### 5b. RAW gives you at most 6 slots per window on this stack

`raw.c:321,324` assign `max_slots` the **bit-count** constants, not the field maxima:

```c
#define IEEE80211_S1G_RPS_RAW_SLOT_NUM_3BITS  (3)
#define IEEE80211_S1G_RPS_RAW_SLOT_NUM_6BITS  (6)
```

then `raw.c:329-333` caps with only a `WARN` — **and writes the cap back into the caller's persistent
config**, silently changing subsequent beacons. Meanwhile `morse_cli` (`raw.c:114`) and hostapd
(`src/utils/morse.h:154`) both advertise 1–63. Requesting 8 slots yields 6 and changes `[4]` from `0x09`
to `0x19`.

This matters beyond the byte-diff. **RAW is being ported to fix EDCA contention for the mesh-gate**, and a
6-slot ceiling is a real constraint on that story. `num_slots=2` is unaffected so the golden bytes stand,
but S1+ scaling arithmetic must not assume 63, and whether the *firmware* enforces the same 6 is worth
confirming on the bench.

**Latent, not biting today:** `raw.c:62` defines the slot-count mask as `GENMASK(16, 10)` — bit 16 is
outside the `__le16` it is OR'd into at `raw.c:349-354`. Harmless only because of the ≤6 cap; a port that
raised the cap to the field's true 63 would silently lose bit 16.

**Three toolchain inconsistencies, version-independent** (`morse_cli` is the same commit at both tags): the
CLI validates slot duration on a **200 µs** granularity to 410099 µs (`cli raw.c:20`) while the driver
quantises at **120 µs** and caps at 246140 µs; `raw.h:169-171`'s "Maximum duration is 246260us" is 120 µs
high; and `RAW_CMD_MAX_3BIT_SLOTS` is dead code, never enforced. **Any ESP-side config API should enforce
the driver's caps, not the CLI's.**

## 6. Verification status

- **Off-bench, reproducible:** ELF/TLV round-trip comparison (script + artifacts in the session scratchpad),
  `git ls-tree`/`git hash-object` blob comparison on both nodes, `objdump`/`objcopy` section hashes of
  `raw.o`, and a compiled transcription of the 1.17.8 encoder.
- **Bench access was read-only and passive throughout** — `ssh` for `git`/`grep`/`md5sum`/`modinfo`/`lsmod`
  and `scp` to pull files down. **No radio was brought up, no interface configured, no `morse_cli` write
  command issued, nothing flashed, nothing transmitted.** S0b-0 is by design an off-bench stage.
- **On-air frame verification: not applicable.** Nothing was transmitted. The golden constant remains a
  *desk* derivation — confirming that a live 1.17.8 reference AP actually emits
  `D0 06 20 40 09 04 E0 1F` is S0b-2's job, and it stays owed.

## 7. Caveats — what this does not establish

- **Nothing about RAW enforcement.** This is all host-side IE construction and firmware-image plumbing.
  Whether the MM6108 blob *acts* on a RAW it is handed, in either direction, is completely untouched. That
  is still S0b-2.
- The `.mbin` was compared against the **source ELF**, not against what the chip holds after a load.
- The `raw.o` `.text` comparison is between two locally-built objects, not the shipped `.ko`. What is proven
  about the shipped module is narrower: `modinfo` version + `srcversion` match on both nodes, and the two
  installed `morse.ko` files are byte-identical (md5 `b228bfb4…`, 780712 B).
- **chronite's `morse_driver` working tree carries three uncommitted local edits** — `Makefile`,
  `command.c` (adds a `dev_info` command-trace print), `mac.c` (hardcodes `country = "AU"`). None touches
  `raw.c`/`raw.h`/`dot11ah.h`, and the installed `.ko` is byte-identical to chronium's, so they are **not**
  in the running module. **But do not rebuild+install on chronite mid-spike** — the running module would
  change out from under the capture, and the country hardcode is exactly the class of thing that burns a
  session.
- The OCS RAW path (`raw.c:1230`) and the dynamic-RAW TLV path (`raw.c:1463-1473`) were not audited. If
  either is active the RPS would carry additional concatenated assignments and the capture would not be an
  8-byte element.

## 8. Next — S0b-1

Sniffer calibration, on the bench, still with zero ESP code: chronium in monitor, chronite beaconing plain
(no RAW, dtim1), ≥2000 beacons, successive `radiotap.mactime` deltas against the expected ~102400 µs.
Gate: robust σ ≤ 20 µs and p99−p1 ≤ 100 µs. In the same run, validate the counter instrument with
`morse_cli -i wlan1 stats` and RAW **disabled** — if the RAW section is absent, that instrument is void too.
If both fail, S0b is not decidable on this bench as designed and should be re-scoped rather than run to an
ambiguous negative.
