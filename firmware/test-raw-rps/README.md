# test-raw-rps — the ESP advertises a RAW schedule, and the chip acts on it

**Status: hardware-verified, NOT wired into a scored tier** (2026-07-31, board0, fw 1.17.8).
**Q2 = PASS, S0b-3 = PASS, Q3b = PASS (HARD GO).** This is a **feasibility probe**, not a regression
test — see "How it is scored" below before adding it to a tier.

It covers several stages with **three different instruments**, and they must not be confused:

| stage | question | instrument | rig |
|---|---|---|---|
| **S1** | does the ported encoder produce the same bytes as Linux? | an **on-device self-test** against reference bytes | **no radio at all** |
| **S0b-3 / S0b-4** | do the RPS element and the S1G-caps RAW bit reach the air unmodified? | an **off-air capture**, decoded with `tools/raw_s1g_beacon_decode.py` | one board beaconing; a sniffer |
| **S0b-5a** | does the chip's RAW engine **arm** on an ESP-authored beacon? | the chip's **own counters** on this console (tag 4210) | **one board, nothing else** |
| **S0b-5b** | does the chip **gate its own TX** to the window? | same counters, under load | + a STA and a ping flood |
| **S3** | is RAW genuinely off unless configured on? | a **config-only A/B** off air | two builds, one sniffer |

## Rig

| role | device | app / config |
|---|---|---|
| dut | one board (board0 in the verified run) | `test-raw-rps`, AP vif on `rimba-rawrps`, S1G ch27 |
| monitor | chronium `morse0` | `tcpdump -i morse0 -w cap.pcap`, S1G ch27 (`iw freq 5560`) |
| Linux reference AP *(S0b-4, optional)* | chronite `wlan1` | `hostapd_s1g` + `morse_cli -i wlan1 raw …` |
| Linux STA *(S0b-5b only)* | chronite `wlan1` | `wpa_supplicant_s1g -c docs/reference/captures/wpa-sta-raw.conf`, `192.168.12.2/24`, then `ping -i 0.002 -s 512 192.168.12.1` |

For S0b-4 the Linux **AP** arm is optional, but it is what makes the byte-diff a **live-device** diff
rather than a spec diff: run both APs simultaneously and one capture holds both elements under identical
radiotap conditions. For S0b-5b the Linux **STA** arm is required — see the note on the AP's static IP.

⚠ **S0b-5b needs the AP to TRANSMIT, not just beacon.** `aci_frames_delayed` counts the local transmitter
withholding its *own* frames, so a ping flood at an AP with no IP produces pure uplink and leaves every
AP-side deferral counter at zero — a null result that reads exactly like a clean negative. That is why
this fixture pins `192.168.12.1` and brings the netif up explicitly (in AP mode mmhalow never fires a
link-up event, so lwIP otherwise never answers ICMP).

## What it proves / does not prove

- **Proves the ADVERTISE plane.** Whether an RPS element (EID 208) that the host splices into the S1G
  beacon reaches the air **byte-identical** to what a live Linux `hostapd_s1g` AP transmits — i.e.
  whether the MM6108 blob rewrites or strips IEs in a host-supplied beacon. Same question for the S1G
  Capabilities RAW Operation Support bit (octet[6] bit 3), which Linux emits only while RAW is actually
  running.
- **Proves the ENFORCE plane too, as of S0b-5.** Whether the chip's RAW engine actually *arms and gates*
  on an ESP-authored schedule. A capture can only ever show what was advertised, so that answer comes
  from the chip's own firmware counters — `mmwlan_get_morse_stats(core 1 = MAC)`, TLV tag 4210
  (`raw_stats_t`) — reported on this fixture's console. **Q3b = PASS:** `assignments[0]` / `Beacons TX`
  = **exactly 1.000 across 2344 beacons** with `invalid_assignments = 0`, and under a STA ping flood
  `aci_frames_delayed` 0→**139** / `frame_crosses_slot` 0→**37** (the Linux reference arm read 92 / 27).
- **Does NOT prove RAW is worth enabling.** It costs the AP **~22 % of its downlink** as a standing cost,
  measured with one STA where there is no contention to buy it back. That is a design question, not a
  firmware one, and this fixture cannot answer it — the bench cannot produce the 30+ contending clients
  where RAW is supposed to pay for itself.

Do not read "enforcement" into a captured RPS either. 802.11ah has no AP primitive that polices peers: a
STA restricts *itself* on parsing the element, and the chip's RAW counters only ever count the **local**
transmitter deferring its own frames.

## RAW is ordinary configuration now — no compile-time gate

**As of S3 there is no `#ifdef`.** RAW is a field on the public `mmwlan_ap_args`, and
`MMWLAN_AP_ARGS_INIT` zeroes it, so an application that never mentions RAW never advertises it:

```c
cfg.ap.raw.enabled          = true;
cfg.ap.raw.start_aid        = 1;
cfg.ap.raw.end_aid          = 255;
cfg.ap.raw.num_slots        = 2;
cfg.ap.raw.slot_duration_us = 10100;
```

That replaced the earlier `#ifdef RIMBA_RAW_S0B_SPIKE`, which existed only to keep RAW out of the other
26 AP-capable apps while the spike was in the tree. Default-off is the honest version of the same
containment, and it is per-deployment rather than per-build — which the feature needs, because whether
RAW pays depends on the traffic mix and only the integrator knows that.

**The containment was measured, not assumed** (2026-07-31, same board, same morselib, config-only
difference):

| | RAW off — `rimba-halow-ap`, never sets `cfg.ap.raw` | RAW on — this fixture |
|---|---|---|
| EID-208 on air | **0 / 351 beacons** | **387 / 387** |
| S1G caps octet[6] bit 3 | **set in 0 / 351** | **set in 387 / 387** |

⚠ **There is deliberately NO RUNTIME enable/disable.** The S1G Capabilities element is built once at AP
start and memcpy'd into the supplicant's cached `hw_mode->s1g_capab` (`driver_ap.c:130-139`); Linux
re-clears the bit on every beacon (`mac.c:1392-1393`) but the ESP has no per-beacon capabilities rebuild
to hook. Toggling RAW after `mmwlan_ap_enable()` would leave the advertisement lying, which is worse than
not offering the knob. Restart the AP to change it.

### The one define that remains

`RIMBA_RAW_SELFTEST`, set build-globally by this app's own `CMakeLists.txt`:

```cmake
idf_build_set_property(COMPILE_DEFINITIONS "RIMBA_RAW_SELFTEST=1" APPEND)
```

It gates **only** the encoder self-test export (`mmwlan_rps_build_reference()` in morselib's
`ies/ie_rps.c`) — nothing about RAW behaviour. It is build-global rather than app-private because the
symbol lives in the submodule, not under `main/`, so the `target_compile_definitions(${COMPONENT_LIB}
PRIVATE …)` idiom the other fixtures use would never reach it. Scoped to this app regardless, because
every app builds into its own tree.

A CMake cache var via the Makefile's `IDF_EXTRA_D` was rejected: cache entries are **sticky per build
dir**, and the T0 builder rebuilds each app into that same dir with no vars, so one build would keep the
define set until a `fullclean`.

`CONFIG_HALOW_AP_MODE=y` is still required (`sdkconfig.defaults`) — without it `umac_ap.c` is not in
morselib's `SRCS` at all. This fixture also carries its own **2 MB `partitions.csv`**: the default
`SINGLE_APP_LARGE` app partition was within a few KB of full once the encoder and the counter reader were
compiled in.

## How it is scored — the preconditions, never the verdict

The `TEST|` output emits three gates. Two assert **preconditions**; only the first is an actual result,
and even that one is about the encoder, never about what the chip did with it:

```
TEST|STEP|rps-encoder-golden|PASS|ie_rps_build() output is byte-identical to the RPS a live Linux AP transmits
TEST|STEP|ap-beaconing|PASS|AP vif up on ch27; capture on chronium morse0 and decode with tools/raw_s1g_beacon_decode.py
TEST|STEP|stats-read|PASS|mmwlan_get_morse_stats(core 1) returned a parseable blob containing tag 4210
```

`rps-encoder-golden` runs **before any radio work** — it needs no hardware, and if the encoder is wrong
there is no point spending a bench slot capturing its output. Its expected bytes live in this app rather
than beside the encoder: were they next to the encoder, the test would compare it against itself and pass
for any self-consistent-but-wrong encoding.

They assert only that the board is **beaconing** (the sole precondition the capture has) and that the
**instrument works** — that the stats read succeeded and the RAW counter set is present. `TEST|RESULT|
INCONCLUSIVE` is emitted when `mmhalow_set_config(AP)` fails (no beacons, nothing to capture) or when the
counter read fails / tag 4210 is absent (the instrument is broken, so Q3b is not evaluable).

**That last distinction is the point.** A missing tag 4210, or one whose length is not 60, is reported as
an ERROR with the full TLV inventory dumped — **never as zeros**. Zeros in the wrong place read exactly
like "the RAW engine never armed", which would be a false BLOCKED on the question the stage exists to
answer.

**The verdict is off-air.** Capture on chronium and decode with the tracked
`tools/raw_s1g_beacon_decode.py`, which computes the IE offset from Frame Control (never searches for it),
strips the 4-byte trailing FCS, and reports an IE walk that does not land exactly on the end as
undecodable rather than retrying at another offset. It is calibrated against the two archived S0b controls.

S0b-5's verdict, by contrast, is **on this console** — the counters are immune to capture loss, which is
exactly why they were designated the primary instrument.

```sh
# on chronium
sudo tcpdump -i morse0 -w cap.pcap

# on the dev box
python3 tools/raw_s1g_beacon_decode.py cap.pcap --sa <AP vif SA> --beacon-int-tu 100
```

This is a feasibility probe. **It must not gate a tier on the RPS.** The spike's schedule is eight
constant bytes that nothing enforces or updates — asserting on them would score a hardcoded advertisement,
not a working feature, and it would break the moment the real RAW work replaces the constant. Scoring the
RPS also needs a sniffer, which no tier has. The manifest entry says the same thing.

## Why `dtim_period=3`

A **deliberate divergence** from the reference AP's `dtim_period=1`. Linux's short-beacon IE allowlist
*does* include RPS, so a capture that only sampled DTIM beacons could report Q2=PASS while non-DTIM
beacons carried nothing. With `dtim_period=1` every beacon is a DTIM beacon and that check is unrunnable;
3 puts both classes in one capture.

Safe to diverge because S0b-4 is a byte-diff of an element, not a timing measurement — DTIM period does
not enter the RPS bytes. Source says the ESP cannot have this failure at all (morselib has no short-beacon
TX path, and `umac_ap_build_beacon()` is the single unconditional builder for every beacon), but the point
of the fixture is to measure it rather than argue it.

`beacon_interval_tus=100` is pinned to match the tracked `docs/reference/captures/hostapd-rimba.conf` so
the two captures are comparable. TX power is capped to 1 dBm between `set_config` and `wifi_start` — the
override can only reduce below the regulatory max and returns `MMWLAN_UNAVAILABLE` once the WLAN subsystem
is active, so calling it after start would silently do nothing.

## Footgun: the AP vif does not beacon from the MAC it prints

The board reports `68:24:99:44:6b:b7` and **beacons as `6a:24:99:44:6b:b7`** — the AP vif sets the
locally-administered bit. Filtering a capture on the console-reported MAC finds nothing, which looks
exactly like *"the ESP never beaconed"*.

The app waits 3 s after `wifi_start` and prints the MAC precisely so the capture is filtered on a value
that was read, not guessed; flip bit 1 of octet 0 before using it as `--sa`, or filter on SSID
`rimba-rawrps` instead.

## How to run

```sh
make build APP=test-raw-rps BOARD=proto1-fgh100m
make flash APP=test-raw-rps BOARD=proto1-fgh100m PORT=/dev/ttyACM0
# or, with the console attached:
make flash-monitor APP=test-raw-rps BOARD=proto1-fgh100m PORT=/dev/ttyACM0
```

**Never bare `idf.py`** — it yields country `"??"`, the radio never boots, and that masquerades as dead
hardware.

Resolve `PORT` by efuse MAC via `/dev/serial/by-id`; `ttyACM` numbers move.

Not in a `make test-t2` tier (see above). Return the board to `rimba-hello` afterwards and bring the Linux
radios down — the automatic radio-silencing only applies to harness-driven runs, and this is a hand-run
spike.

## Result on fw 1.17.8 (2026-07-31, board0)

Both APs beaconing simultaneously; the spike and reference columns are the **same capture**.

| | baseline (no hook) | spike | Linux reference (same capture as spike) |
|---|---|---|---|
| beacons decoded | 467 | **519** | 515 |
| undecodable | 0 | 0 | 0 |
| EID-208 present | **0** | **519 / 519** | **515 / 515** |
| RPS bytes | — | **`D0 06 20 40 09 04 E0 1F`** | **`D0 06 20 40 09 04 E0 1F`** |
| byte-identical to the golden constant | — | **True** | **True** |
| S1G caps octet[6] | **`0x00`** ×467 | **`0x08`** ×519 | **`0x08`** ×515 |
| DTIM / non-DTIM | 158 / 309 | **171 / 348** | 515 / 0 |
| IE offset | 15 (all) | 15 (all) | 15 (all) |
| FC | `0x401c` | `0x401c` | `0x081c` |
| RSSI median | −52 dBm | −52 dBm | −35 dBm |
| capture completeness | 97.29 % | 98.11 % | 97.35 % |

**Q2 = PASS.** The blob does not rewrite or strip IEs in a host-supplied beacon. The eight octets the host
wrote arrive unchanged on every single beacon, and are byte-identical to what a live Linux device
transmitted in the same capture.

**S0b-3 = PASS.** The caps bit reaches the air from the host too. The full element goes
`1e 00 40 00 00 08 00 02 60 00 fd 00 fa 01 00` → `1e 00 40 00 00 08 **08** 02 60 00 fd 00 fa 01 00` —
one bit, everything else byte-identical, the same signature the live-Linux A/B showed.

Placement is right on both stacks — RPS immediately follows the TIM:

```
ESP    [0, 5, 208, 48, 127, 244, 213, 217, 232, 214, 221]
                 ^ RPS directly after TIM
Linux  [213, 5, 208, 217, 232, 214, 0, 221]
                 ^ RPS directly after TIM
```

The **overall** chains differ, and that is pre-existing and unrelated to RAW: on the ESP, 213/217/232/214
live inside hostapd's opaque `tail` blob and therefore land after the TIM, whereas the Linux driver's S1G
converter emits 213 first. **The verdict is the eight RPS octets plus the TIM→RPS adjacency, not the chain
vector** — diffing the whole vector reports a false mismatch on a long-standing ordering difference.

Capture completeness is measured against TBTT slots spanned from `radiotap.mactime`, against the ~2.5 %
healthy bench baseline; the `morse0` drop counter (1 and 4 across the two captures) is a weak secondary
signal only, which is why frame-count reconciliation is the gate.

## Firmware

`firmware/test-raw-rps/` — single board; brings up an AP vif on S1G ch27 with SAE + PMF-required and parks
forever. Everything interesting happens in the submodule's RPS encoder and the beacon builder.

- Spike patch: `docs/worklog/artifacts/raw-s0b/s0b-4-morselib-spike.patch`
- Decoder: `tools/raw_s1g_beacon_decode.py`
- Captures: `docs/worklog/artifacts/raw-s0b/s0b-4-esp-baseline-nohook.pcap`,
  `docs/worklog/artifacts/raw-s0b/s0b-4-esp-spike-dualap.pcap`
- Run worklog: `docs/worklog/2026-07-31-raw-s0b-4-esp-rps.md`
- Design doc: `docs/design-specification/rimba-raw-apside-design.md` §7
