# test-raw-rps — the ESP puts an RPS element on the air (RAW S0b-3 / S0b-4)

**Status: hardware-verified, NOT wired into a scored tier** (2026-07-31, board0, fw 1.17.8).
**Q2 = PASS and S0b-3 = PASS.** This is a **feasibility probe**, not a regression test — see "How it is
scored" below before adding it to a tier.

The board is a beacon source and nothing else: no STA, no traffic, no peer. All the evidence is off-air.

## Rig

| role | device | app / config |
|---|---|---|
| dut | one board (board0 in the verified run) | `test-raw-rps`, AP vif on `rimba-rawrps`, S1G ch27 |
| monitor | chronium `morse0` | `tcpdump -i morse0 -w cap.pcap`, S1G ch27 (`iw freq 5560`) |
| Linux reference AP *(optional)* | chronite `wlan1` | `hostapd_s1g` + `morse_cli -i wlan1 raw …` |

The Linux arm is optional but it is what makes this a **live-device** diff rather than a spec diff: run
both APs simultaneously and one capture holds both elements under identical radiotap conditions.

## What it proves / does not prove

- **Proves the ADVERTISE plane.** Whether an RPS element (EID 208) that the host splices into the S1G
  beacon reaches the air **byte-identical** to what a live Linux `hostapd_s1g` AP transmits — i.e.
  whether the MM6108 blob rewrites or strips IEs in a host-supplied beacon. Same question for the S1G
  Capabilities RAW Operation Support bit (octet[6] bit 3), which Linux emits only while RAW is actually
  running.
- **Does NOT prove the chip acts on the ESP's schedule.** That is Q3b / **S0b-5**, and it is read from
  the chip's own **tag-4210 RAW counters** via `mmwlan_get_morse_stats()`, not from a capture. A capture
  can only ever show what was advertised. This fixture is already the right shape for S0b-5 — it parks
  forever and carries the `TEST|` contract — but that work is not here.

Do not read "enforcement" into a captured RPS either. 802.11ah has no AP primitive that polices peers: a
STA restricts *itself* on parsing the element, and the chip's RAW counters only ever count the **local**
transmitter deferring its own frames.

## THE SPIKE IS COMPILE-GATED — and its edits are not in the tree

This is the single most confusing thing about the fixture. Read it before running anything.

The two source edits the spike depends on live in **shared morselib**, inside the `components/halow`
submodule, wrapped in `#ifdef RIMBA_RAW_S0B_SPIKE`:

| file (inside `components/halow`) | edit |
|---|---|
| `…/morselib/src/umac/ap/umac_ap.c` | in `umac_ap_build_beacon()`, appends the 8-byte golden RPS directly after the TIM |
| `…/morselib/src/umac/ies/s1g_capabilities.c` | in `ie_s1g_capabilities_build_ap()`, asserts `DOT11_MASK_S1G_CAP6_RAW_OPERATION_SUPPORT` |

**Both edits are landed on the submodule's `main`, so a normal checkout builds the armed spike** — they
are inert everywhere else, because nothing but this app defines `RIMBA_RAW_S0B_SPIKE`. They are also
captured standalone as `docs/worklog/artifacts/raw-s0b/s0b-4-morselib-spike.patch` (32 insertions, 2
files), which is what regenerates the **no-hook baseline** without editing anything by hand:

```sh
# disarm — reproduce the no-hook baseline build (from the superproject root)
git -C components/halow apply --reverse ../../docs/worklog/artifacts/raw-s0b/s0b-4-morselib-spike.patch

# re-arm
git -C components/halow apply ../../docs/worklog/artifacts/raw-s0b/s0b-4-morselib-spike.patch
```

**Taking that baseline is not optional — capture it FIRST.** You get a binary identical in every other
respect (same SSID, same BSSID, same beacon interval, same DTIM period) emitting plain AP beacons. Without
that control an EID-208 element on air cannot be attributed to the host splice at all, and a *missing*
element gives no evidence the capture/decode path even works.

⚠ **The console line reports the DEFINE, not the edits.** The first `TEST|INFO` says `built WITH …` vs
`built WITHOUT RIMBA_RAW_S0B_SPIKE -- plain AP beacons, this is the no-hook baseline` — but with the
define armed and the patch *reversed* it still says `WITH`, while the beacons are baseline. **The capture
is the authority, never the banner.**

### How the define is armed

One line, in this app's own top-level `CMakeLists.txt`:

```cmake
idf_build_set_property(COMPILE_DEFINITIONS "RIMBA_RAW_S0B_SPIKE=1" APPEND)
```

Deliberately **build-global**, not app-private. The guarded edits are in the submodule, not under
`main/`, so the `target_compile_definitions(${COMPONENT_LIB} PRIVATE …)` idiom the other fixtures use
would never reach them; IDF applies the build-wide `COMPILE_DEFINITIONS` property with directory scope
inside every `idf_component_register`, which is the same route by which `ESP_PLATFORM` and `IDF_VER`
already land on `umac_ap.c`'s compile line.

Build-global is still **scoped to this app**, because every app gets its own build tree
(`build/<APP>/<BOARD>`). The other 26 apps that set `CONFIG_HALOW_AP_MODE=y` never see the define and
never emit an RPS — verified from `compile_commands.json` on both sides, not inferred.

A CMake cache var via the Makefile's `IDF_EXTRA_D` was rejected for exactly this: cache entries are
**sticky per build dir**, and the T0 builder rebuilds each app into that same dir with no vars, so one
spike build would silently keep the RPS compiled into T0 until a `fullclean`.

`CONFIG_HALOW_AP_MODE=y` is required (`sdkconfig.defaults`) — without it `umac_ap.c` is not in morselib's
`SRCS` at all and the splice compiles to nothing.

## How it is scored — the beacon, not the RPS

The `TEST|` output emits exactly one gate:

```
TEST|STEP|ap-beaconing|PASS|AP vif up on ch27; capture on chronium morse0 and decode with tools/raw_s1g_beacon_decode.py
```

It asserts **only that the board is beaconing**, which is the sole precondition the capture has. There is
no `TEST|RESULT` on the success path and deliberately no `TEST|` gate on the RPS. The only failing verdict
the app can produce is `TEST|RESULT|INCONCLUSIVE` when `mmhalow_set_config(AP)` fails — no beacons, so
nothing to capture.

**The verdict is off-air.** Capture on chronium and decode with the tracked
`tools/raw_s1g_beacon_decode.py`, which computes the IE offset from Frame Control (never searches for it),
strips the 4-byte trailing FCS, and reports an IE walk that does not land exactly on the end as
undecodable rather than retrying at another offset. It is calibrated against the two archived S0b controls.

```sh
# on chronium
sudo tcpdump -i morse0 -w cap.pcap

# on the dev box
python3 tools/raw_s1g_beacon_decode.py cap.pcap --sa <AP vif SA> --beacon-int-tu 100
```

This is a feasibility probe. **It must not gate a tier on the RPS.** The spike's schedule is eight
constant bytes that nothing enforces or updates, and it only exists at all while an unlanded patch is
applied to the submodule — a tier that asserted on it would fail on a clean tree, which is the normal
state. The manifest entry says the same thing.

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
forever. Everything interesting happens in the submodule, behind `RIMBA_RAW_S0B_SPIKE`.

- Spike patch: `docs/worklog/artifacts/raw-s0b/s0b-4-morselib-spike.patch`
- Decoder: `tools/raw_s1g_beacon_decode.py`
- Captures: `docs/worklog/artifacts/raw-s0b/s0b-4-esp-baseline-nohook.pcap`,
  `docs/worklog/artifacts/raw-s0b/s0b-4-esp-spike-dualap.pcap`
- Run worklog: `docs/worklog/2026-07-31-raw-s0b-4-esp-rps.md`
- Design doc: `docs/design-specification/rimba-raw-apside-design.md` §7
