# RAW S1 + S3 — a real encoder, then real configuration

**Date:** 2026-07-31
**Stages:** S1 (+S2, which collapses into it) and S3 of the RAW AP-side port — design doc
[`rimba-raw-apside-design.md`](../design-specification/rimba-raw-apside-design.md) §3.
**Code map:** [`rimba-raw-code-map.md`](../mesh-ap/rimba-raw-code-map.md) — mandatory deliverable per
`.ai/AGENTS.md`, and the place every `file:line` anchor and deliberate divergence is recorded.

## Decision context — this proceeds against my own recommendation, deliberately

The [break-even analysis](2026-07-31-raw-breakeven-model.md) earlier today recommended **not** staging
S1→S4 yet: the payoff can't be demonstrated on this bench, the ~22 % cost figure was measured on a Linux
AP rather than an ESP one, and the gate's client ceiling (8) sits below the range RAW serves. The owner
reviewed that and elected to proceed anyway.

**That analysis is not withdrawn** — it stays open in `docs/rimba-todo.md`, and the traffic-mix question
remains the cheapest thing that could settle the feature. What changed is the order of work, not the
evidence. The single condition attached to proceeding was that the port be **faithful to the Linux
implementation**, which is what this stage is measured against.

---

## What S1 replaces

S0b-4 spliced a hardcoded eight-byte constant into the beacon:

```c
static const uint8_t k_rps_golden[8] = { 0xD0, 0x06, 0x20, 0x40, 0x09, 0x04, 0xE0, 0x1F };
consbuf_append(buf, k_rps_golden, sizeof(k_rps_golden));
```

That answered "do host-authored bytes reach the air", which was the spike's job. It is not a RAW
implementation — it cannot express any schedule but one. S1 replaces it with an encoder ported from
`morse_driver`'s `raw.c`, so the bytes are *computed* from a configuration.

**New:** `components/halow/…/morselib/src/umac/ies/ie_rps.{c,h}`, registered in
`morselib/CMakeLists.txt:55` alongside the other IE builders, plus `DOT11_IE_S1G_RPS = 208` in
`dot11/dot11.h:2341`. The splice site is unchanged — still directly after the TIM, which is where Linux
puts it — but now calls `ie_rps_build()` (`umac_ap.c:444`).

## Fidelity — what "follow Linux" meant in practice

Ported from `morse_driver` at tag **1.17.8**, read on `chronite`, which is the revision the bench radios
actually run. Four functions map one-to-one (`morse_raw_calc_rps_ie_size`,
`morse_raw_generate_slot_definition`, `morse_raw_generate_assignment_with_aid_range`,
`morse_raw_generate_rps_ie`); every mask and shift is transcribed from `raw.c:15-79` rather than
re-derived from the standard, so a disagreement with the reference is a porting bug rather than an
interpretation difference.

Three reference behaviours were replicated **including their warts**, because byte-fidelity is the point:

- **The slot-count cap is 6, not 63.** Linux assigns `max_slots` from
  `IEEE80211_S1G_RPS_RAW_SLOT_NUM_{3,6}BITS` — the field *widths*, not the field maxima — so the count is
  silently capped at 6 (format 0) or 3 (format 1) even though `morse_cli` and hostapd both advertise 1-63.
- **The cap writes back into the caller's config.** A caller asking for 8 slots finds its own config
  permanently reading 6. Idempotent, so it cannot change the emitted bytes, but it is real.
- **The 6-bit slot-count mask spans bit 16**, outside the `__le16` it is OR'd into. Harmless only because
  of the cap above. Transcribed as-is rather than silently "fixed", with the hazard recorded.

Nine deliberate divergences are listed in the code map with reasons — the substantive ones being no
persistent element buffer (the ESP rebuilds the beacon every TBTT, so Linux's `kmalloc`'d cache and its
invalidation dance have no analogue), EID framing done by the builder to match its sibling
`ie_s1g_tim_build()`, and the wire structs flattened into explicit little-endian byte writes rather than
packed-struct pointer arithmetic.

---

## ⚠ A bug I wrote, caught before it reached the air — and the doc wording that invited it

The design doc §5 warns:

> *"a pass-1/pass-2 size disagreement is a **hard hang**"*

I took that literally and made the two passes agree by **padding the element out to the reserved length**.
That is wrong, and it would have been much worse than a hang.

**The actual contract is an upper bound, not an equality.** `build_frame_with_class()`
(`frame_constructor.c:17-32`) runs the builder twice, but the frame's final length comes from **pass two**
— `mmpkt_append(view, cbuf.offset)` at `:32`. Pass one only sizes the *allocation*. `ie_s1g_tim_build()`
proves it: it reserves **256** bytes and writes at most **45**.

So only **under**-reserving is dangerous, and that direction does hang, on the live `MMOSAL_ASSERT` in
`consbuf_append()` (`consbuf.c:30`).

**What the padding would have done:** the bytes immediately after this element are hostapd's `tail` blob.
Four zero pad bytes between them would have desynchronised the entire IE chain — every element after the
RPS parsed at the wrong offset, with a zero byte read as an SSID element of length zero. The beacon would
still transmit. Clients would still see an AP. The RPS itself would still be byte-perfect. It would have
looked fine right up until something tried to parse the S1G Capabilities or Operation elements that follow.

Caught by asking why `ie_s1g_tim_build` doesn't need to pad, then reading `frame_constructor.c` instead of
trusting the summary. **The design doc §5 wording has been corrected**, and the code map carries the
precise contract plus an explicit "padding is a bug, not a safety measure" note.

---

## Verification

### 1. Unit — the encoder against a live-Linux reference, on device

`TEST|STEP|rps-encoder-golden` runs at boot, **before any radio work** (it needs no hardware, and if the
encoder is wrong there is no point spending a bench slot capturing its output):

```
TEST|INFO|RPS encoder built 8 bytes: D0 06 20 40 09 04 E0 1F
TEST|INFO|live-Linux reference  : D0 06 20 40 09 04 E0 1F
TEST|STEP|rps-encoder-golden|PASS|ie_rps_build() output is byte-identical to the RPS a live Linux AP transmits
```

**Test design matters here.** The expected bytes live in the **application**
(`firmware/test-raw-rps/main/app_main.c`), not next to the encoder. Had they lived in `ie_rps.c` the test
would compare the encoder against itself and pass for any self-consistent-but-wrong encoding. The
application-side constant is the byte string captured off air from a live Linux AP in S0b-2, so the
assertion is against an external reference rather than the port's own opinion. morselib exposes only the
*producer*, via `mmwlan_rps_build_reference()` — prefixed to survive the library mangler's glob, and
compile-gated to the spike build.

### 2. On air — the dynamically-built element

board0 as AP, chronium monitor, 45 s:

| | |
|---|---|
| S1G beacons decoded | **425**, all from the ESP (`6a:24:99:44:6b:b7`) |
| undecodable | **0** |
| EID-208 present | **425 / 425** |
| RPS bytes | **`D0 06 20 40 09 04 E0 1F`** — byte-identical |
| IE chain | `[0, 5, 208, 48, 127, 244, 213, 217, 232, 214, 221]` — **unchanged** from the hardcoded-constant runs |
| S1G caps octet[6] | `0x08` on all 425 |
| capture completeness | 98.84 % |

Capture not committed (see `docs/worklog/artifacts/README.md`) — it is regenerable: flash `test-raw-rps`, then on chronium `sudo tcpdump -i morse0 -w cap.pcap`, decode with `python3 tools/raw_s1g_beacon_decode.py cap.pcap --sa <beacon SA>`.

**The chain row is the load-bearing one.** It is what proves the two-pass sizing is right: had the padding
shipped, the chain would have been desynchronised and the decoder — which requires the IE walk to land
exactly on the frame end — would have reported every frame undecodable. 0 undecodable across 425 beacons
is that check passing.

### 3. Host-side

`make test-unit` 53/53. Fixture builds clean. ⚠ App partition now at **1 % free** (`0x2310` bytes) — not
caused by this stage, but S3+ will need the partition table revisited.

## Bench state left behind — radio-silent, verified

board0 reflashed to `rimba-hello`; chronium `morse0` down, `wlan1` down and restored to `type managed`;
chronite and chronogen untouched and down.

## Next

**S3** — the config surface: a RAW field on `mmwlan_ap_args` and an enable/disable knob mirroring
`morse_raw_process_cmd` / `morse_raw_enable`, AP-iftype-gated. That is also what lets the S1G-caps RAW bit
stop being a spike `#ifdef` and become the honest *"is RAW enabled for this AP"* predicate Linux uses —
the thing S0b-3 could not do on its own. The static config at `umac_ap.c:433` is the placeholder S3
replaces.

Then **S4** (AID-list from the live `umac_ap` bitmap) is the MVP finish line.

**Still open and unchanged:** the break-even question. Every firmware unknown is answered, but nothing in
this stage moves the economics — see [the break-even worklog](2026-07-31-raw-breakeven-model.md).


---

# S3 — RAW becomes ordinary configuration, and the spike `#ifdef` is retired

## What it changes

After S1 the encoder was real but what it encoded was not: a hardcoded schedule, always on, compile-gated
behind `RIMBA_RAW_S0B_SPIKE`. S3 makes RAW **ordinary AP configuration**.

- **`struct mmwlan_ap_raw_args`** (`mmwlan.h:2025`), carried on `mmwlan_ap_args` (`:2172`): enable flag,
  AID range, slot count, slot duration, cross-slot bleed, start offset.
- **Validation** at AP-enable (`umac_ap.c:91-109`), porting `morse_raw_is_config_valid()`
  (`raw.c:1046-1052`) — including the reference's own caveat that AID ranges *"are not required for the
  spec, but we do require it for now"*. Validating here rather than at beacon-build means a bad schedule
  fails `mmwlan_ap_enable()` loudly instead of silently emitting a malformed element on every beacon.
- **`umac_ap_raw_is_enabled()`** (`umac_ap.c:73`), porting `morse_raw_is_enabled()` (`raw.c:1630-1632`).
  Linux requires both an AP vif and the enabled state; here `umac_data_get_ap() != NULL` carries the first
  (that pointer is allocated only for AP vifs, exactly as Linux's `mors_vif->ap` is) and the config flag
  the second.
- **The caps bit is now that predicate**, not an `#ifdef`.

### Your answer shaped this

Asked whether the gate's traffic is downlink- or uplink-heavy, the answer was **"it can be both"**. That
settles the API shape: RAW cannot be a build-time decision or a default-on behaviour, because whether it
pays depends on a deployment property only the integrator knows. So it is **off unless asked for**, and
the header says why in as many words.

## The `#ifdef` is gone, and that needed proving

The spike guard existed because `umac_ap_build_beacon()` is shared morselib and 27 apps build the AP path
— unguarded, every one of them would have advertised RAW. S3 replaces that guard with
`MMWLAN_AP_ARGS_INIT` zeroing the config. That is the honest mechanism rather than a build-time hack, and
it is per-deployment rather than per-build — but it is only containment **if it actually holds**.

So it was measured, same board, same morselib, differing only in configuration:

| | RAW off — `rimba-halow-ap`, never touches `cfg.ap.raw` | RAW on — `test-raw-rps` |
|---|---|---|
| beacons decoded | **351** | **387** |
| EID-208 present | **0 / 351** | **387 / 387** |
| RPS bytes | — | `D0 06 20 40 09 04 E0 1F`, byte-identical |
| S1G caps octet[6] bit 3 | **set in 0 / 351** | **set in 387 / 387** |
| IE chain | `[0, 5, 48, 127, 244, 213, 217, 232, 214, 221]` — identical to the pre-RAW baseline | `[0, 5, 208, 48, …]` |
| undecodable | 0 | 0 |

Capture not committed (see `docs/worklog/artifacts/README.md`) — it is regenerable: flash `test-raw-rps`, then on chronium `sudo tcpdump -i morse0 -w cap.pcap`, decode with `python3 tools/raw_s1g_beacon_decode.py cap.pcap --sa <beacon SA>`.

`RIMBA_RAW_S0B_SPIKE` no longer exists in morselib. The define that remains is **`RIMBA_RAW_SELFTEST`**,
which now gates only the encoder self-test export — renamed because its meaning changed, and a define
whose name lies about what it does is exactly the kind of thing that misleads six months later.

## Two structural findings

**1. The caps bit is evaluated once, at AP start — Linux re-evaluates it per frame.** Linux re-clears the
bit on every beacon and management frame unless RAW is running (`mac.c:1392-1393`). The ESP cannot: the
element is built by `ie_s1g_capabilities_build_ap()` and memcpy'd into the supplicant's
`hw_mode->s1g_capab` (`driver_ap.c:130-139`), which caches it for the AP's lifetime. There is no
per-beacon capabilities rebuild to hook.

That is safe **only because** RAW is fixed for the AP's lifetime — which is why there is deliberately
**no runtime enable/disable**. A toggle would leave the advertisement lying, which is worse than not
offering the knob. The public header documents the constraint.

Confirmed the ordering that makes even the once-at-start evaluation correct: `umac_ap_enable_ap()` stores
the args at `umac_ap.c:261`, and the supplicant — the only route to the caps builder — starts at `:326`.
The builder in fact runs **twice** (wpa_supplicant iface-init, then hostapd interface-setup; only the
second reaches air), both after the store.

**2. The partition table needed raising.** `test-raw-rps` was at 1 % free on the `SINGLE_APP_LARGE`
default. It now carries its own 2 MB `partitions.csv`, following `rimba-halow-ap-perf` — **27 % free**.
⚠ `rimba-halow-ap` is still at 1 % free on the default table; not urgent, but it is the next app that
will hit this.

## A mistake worth recording

While wiring the fixture I ran a scripted edit that matched the **file header comment** rather than the
runtime `#ifdef`, and it deleted everything between: the includes, all the defines, and every helper
function. The build caught it instantly, and the file was reconstructed from the merged baseline plus
today's additions — but the lesson is that index-based text surgery on a file with the same token in prose
and in code is a bad tool for the job. An anchored edit on a unique string would not have done it.

## Verification summary

| Check | Result |
|---|---|
| Encoder vs live-Linux bytes (on device) | **PASS** — `rps-encoder-golden` |
| RAW enabled via config, on air | **PASS** — 387/387 byte-identical, caps bit set |
| RAW off by default, on air | **PASS** — 0/351 EID-208, caps bit clear |
| `make test-unit` | 53/53 |
| Bench left radio-silent | board0 on `rimba-hello`, all Linux radios down |

## Next

**S4** — build the AID list from the live `umac_ap` bitmap instead of the configured 1–255, and refresh it
on STA join/leave (Linux: `morse_generate_aid_list` `raw.c:247-272`, `morse_raw_refresh_aids`
`raw.c:841-871`). That is the MVP finish line and the first stage with a lifecycle: the element length
becomes genuinely variable, so the two-pass sizing gets its first real exercise.

**Unchanged:** the break-even question. S3 makes RAW configurable; it does not make it worth enabling.
