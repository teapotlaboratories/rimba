# 2026-07-30 — RAW S0a: the firmware advertises RAW, so the on-air spike is justified

**Goal.** Answer the cheapest of S0's four decidables before building anything: does the shipped
MM6108 blob advertise `MORSE_CAPS_RAW` at all? A cleared bit would make the whole S0b harness — beacon
hook, Linux RAW reference, sniffer session — wasted effort.

**Outcome.** `MORSE_CAPS_RAW=1`, `MORSE_CAPS_PAGE_SLICING=1` on fw 1.17.8. **GO-CANDIDATE**: S0b is
now justified rather than speculative. It does **not** establish AP-direction enforcement — see §4.

**Landed 2026-07-30** — the morselib accessors via `teapotlaboratories/mm-esp32-halow#28`, the probe
fixture + manifest entry + gitlink bump via [`#49`](https://github.com/teapotlaboratories/rimba/pull/49).
Both went through `/review`; findings and fixes are recorded on the PRs.

---

## 1. Why S0 was split

The design doc's §6 treated S0 as one harness: hand-build an RPS IE into the beacon, flip the AP
S1G-caps bit, stand up a Linux RAW reference on chronium, capture on `morse0`, and byte-diff. That
bundles a question needing **one board and no sniffer** with three needing the full rig.

Split accordingly:

- **S0a** — read the capability bits. One board, no monitor, no peer, no beacon hook.
- **S0b** — the on-air spike (the original steps 1–3), run only if S0a doesn't already answer BLOCKED.

Recorded in the design doc §6.

## 2. How the read works

```
FW  →  ext_host_table.c:20    capabilities.flags[i] = le32toh(caps->flags[i])
    →  umac_interface.c:262   mmdrv_get_capabilities(MMDRV_VIF_ID_INVALID, &data->capabilities)
    →  MORSE_CAP_SUPPORTED(umac_interface_get_capabilities(umacd), RAW)
```

Two assumptions from the recon turned out to need correcting, both checked in-tree:

- **The capabilities are device-global, not per-vif.** The fetch passes `MMDRV_VIF_ID_INVALID` and the
  accessor takes the global `umac_data`. *Some* vif must be up for the fetch to have happened — it
  need not be an AP. The probe uses an AP vif regardless: it is the direction of interest, and it is
  the defensive choice if that global reading is ever wrong.
- **`mmprobe_dump_fw_caps()` does exist ESP-side** (`driver.c:155`) — I had assumed it was a Linux-only
  symbol and was about to "correct" the design doc. It prints undecoded `flags[]` words via `MMLOG`,
  which does not reliably reach the ESP console, so a named accessor is still the right instrument.

## 3. What was added

| | |
|---|---|
| `mmwlan_raw_capability_advertised()` + page-slicing companion | **submodule** `components/halow/.../src/umac/umac.c` |
| `firmware/test-raw-cap/` | AP vif up, reads both bits, reports over `TEST\|` |
| `tools/regtest/manifest.py` entry | a dir under `firmware/` without one is a hard T0 failure |
| design doc §6 | the S0a/S0b split + the two corrections above |

The accessors mirror `mmwlan_ampdu_capability_advertised()` (`umac.c:1663`) almost line for line.
`protected_syms.txt` already globs `mmwlan*`, so **no export edit was needed** — confirmed by the probe
linking.

**The probe is scored on whether the READ succeeded, not on the value.** "FW does not advertise RAW" is
a legitimate feasibility answer, not a regression, so `RAW=0` would report PASS with a loud
BLOCKED-SIGNAL info line. Scoring it as FAIL would poison the suite for a correct result if it were
ever added to a scored tier. The manifest note says so.

## 4. Result, and what it does not mean

```
TEST|INFO|MORSE_CAPS_RAW=1  MORSE_CAPS_PAGE_SLICING=1
TEST|STEP|cap-read|PASS|raw=1 page_slicing=1
TEST|RESULT|PASS|FW advertises MORSE_CAPS_RAW — S0a answered, S0b is justified
```

**The capability word carries no direction.** STA-side RAW is already known to work, so a set bit is
entirely consistent with the AP-direction scheduler being absent from the same image. This is
necessary, not sufficient. AP-direction enforcement remains exactly as open as it was, and stays
source-undecidable.

Page slicing also being set is a weak positive: the blob's S1G advertisement is fairly complete, which
makes a vestigial RAW bit slightly less likely. Not evidence of enforcement.

## 5. Verification status

- Off-bench: `make test-lint` clean (21 files), `make test-unit` 53/53, probe builds and links,
  tree↔manifest drift none.
- On hardware: flashed to board0 via `make … BOARD=proto1-fgh100m`, ran, verdict captured.
- **On-air frame verification: not applicable, and stated rather than skipped.** This change transmits
  no new or altered frame — the accessors are a pure capability read and the probe brings up a stock
  SoftAP with no RPS element. There is no frame to byte-diff. That obligation belongs to S0b, whose
  entire purpose is to put a hand-built RPS IE on the air and diff it against a live Linux RAW AP.

## 6. Bench state

Reconciled with `make test-bench` against `docs/reference/rimba-bench-devices.md` before starting.

| | |
|---|---|
| board0 `E0:72:A1:F8:EF:A4` | present, `/dev/ttyACM0`, WAKE+BUSY unwired — used |
| board1 `E0:72:A1:F8:F9:40` | present, `/dev/ttyACM1`, untouched |
| board2 `E0:72:A1:F8:F0:08` | **not enumerated** — PPK2 rail unheld; its documented normal state, and not needed (no power-save, no wired pins) |
| chronite | up, unused |
| fw / chip / channel | 1.17.8, `0x0306`, ch27 — matches the inventory |

Nothing on the bench was changed beyond flashing board0 and returning it to `rimba-hello`.
`ppk2_hold` was **not** started. Radio silence confirmed both before and after: `rimba_hello` banner
present, zero `mmwlan`/HaLow/Morse lines.

## 7. Next — S0b

**S0b was scoped on 2026-07-30, and the scoping pass overturned four things in this section. The plan
now lives in the design doc §7; what follows records what was wrong here and why, so the corrections
are not silently absorbed.**

The sketch below was:

> 1. Hand-build a minimal valid RPS IE (EID 66 + one GENERIC all-AIDs assignment, slot-format-0,
>    2 slots) as a golden byte constant; temporary hook at `umac_ap.c:418`; flip the AP S1G-caps RAW
>    bit (`s1g_capabilities.c:276-280`, an `if(false)` today).
> 2. Stand up a live Linux RAW AP on chronium as the byte-diff reference; re-scp the Linux reference
>    each session (non-persistent). Whole bench on 1.17.8.
> 3. Decide Q2 / Q3 / Q4. 4. Radio-silent afterwards.

**Corrections:**

- **The RPS element ID is 208, not 66.** `WLAN_EID_S1G_RPS (208)` —
  `chronium:~/halow/morse_driver/dot11ah/dot11ah.h:37`; 66 is Measurement Pilot Transmission in
  mainline. A golden constant built with 66 fails the byte-diff on octet 0, is recognised by no STA
  and no analyser, and reads most naturally as *"the FW stripped our IE"* — **a false BLOCKED on the
  exact question the spike exists to answer**, after a full multi-radio bench session. Corrected in
  the design doc (six occurrences), `docs/rimba-todo.md` and the mesh-AP milestones.
- **chronium cannot be both the sniffer and the reference AP.** Step 2 assigned it both; it has one
  HaLow radio and monitor is exclusive with operation. The reference AP moves to **chronite**.
- **"Whole bench on 1.17.8" is true of the running stack but not of the source.** Module + blob are
  1.17.8 on every node; chronium's `morse_driver` and `hostap` trees are both at tag **1.17.9**, and
  that is what every anchor in the design doc was read from. chronite carries a 1.17.8 driver tree.
- **The ordering was backwards on cost.** Step 1 spends a submodule patch, a build, a flash and a
  capture *before* the gate is approached. But the MM6108 blob is the **same file** on the ESP and on
  the Linux nodes (`md5 cfe56db2706458050dcc60baddabc632`, and
  `cmake/mm-fw-gen/firmware/CMakeLists.txt:37,51-57,77-82` converts that very ELF at build time), and
  RAW reaches the chip **only as beacon bytes** — no chip command, on either host. So "does the RAW
  engine arm at all" is decidable **on Linux hardware with zero ESP code**, and a negative there
  BLOCKS the port before anything is written. That is now S0b-2, and it is the gate.

**And Q3 as phrased was mis-specified.** "Does the AP-direction FW-MAC *confine* a RAW-assigned STA
to its slot" names a mechanism that may not exist — 802.11ah RAW is honoured by the **STA restricting
itself**, and the firmware's own instrumentation agrees: every field of `raw_stats_t`
(`chronium:~/halow/morse_cli/stats_format.h:81-99`) counts the **local** transmitter deferring its
own frames, and there is no "peer violated RAW" counter anywhere. A GO/BLOCKED gate written against
AP-policing would return **BLOCKED for a feature that works as specified**. Split into Q3a (does the
chip's engine arm from a host-authored beacon — answerable all-Linux) and Q3b (does it arm with the
ESP as AP).

**What survives unchanged:** the S0a headline still holds — a set capability bit is necessary, not
sufficient, and AP-direction behaviour remains source-undecidable. The outcome to watch for is still
*advertisement reaching air while nothing arms*; shipping a RAW advert the AP cannot enforce is still
explicitly ruled out. What changed is that this is now measurable by the firmware's **own counters**
(tag 4210) rather than only by a timing capture — and the counters are immune to the tap-drop failure
mode that would otherwise manufacture a false GO.
