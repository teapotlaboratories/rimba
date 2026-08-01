# RAW S0b-5 — does the engine arm with the ESP as AP? (Q3b)

**Date:** 2026-07-31
**Stage:** S0b-5, the last stage of the RAW S0b spike (design doc
[`rimba-raw-apside-design.md`](../design-specification/rimba-raw-apside-design.md) §7.2).
**Question (Q3b):** the MM6108's RAW engine demonstrably arms from a **host-authored beacon on Linux**
(S0b-2) and the **ESP demonstrably puts a byte-identical RPS on the air** (S0b-4, merged this morning).
Does the engine arm when the **ESP** is the AP?

**The instrument is the chip's own firmware counters, not a packet capture.** A capture can only ever show
what was *advertised*; whether the engine parsed and acted on it is in the firmware's tag-4210
`raw_stats_t`. Read on Linux via `morse_cli -i wlan1 stats`, and on the ESP via
`mmwlan_get_morse_stats(1, …)` — already public and exported by the `mmwlan*` glob, so **no submodule
change is needed**; the work is app-side.

**Reference values from the equivalent all-Linux run (S0b-2, AP side):**

| counter | Linux AP | meaning |
|---|---|---|
| `assignments[0]` | **2179** | engine parsed the RPS out of its own outgoing beacon, ≈1/beacon |
| `invalid_assignments` | **0** | the bytes were accepted, not rejected |
| `aci_frames_delayed` | 92 | chip gated its own TX to the window |
| `frame_crosses_slot` | 27 | real frames too long for a 10100 µs slot at 1 MHz |

**Pre-registered decision rules (§7.7), unchanged:**

| Observation | Verdict |
|---|---|
| ESP counters move like the Linux arm's | **HARD GO** — stage S1→S4 |
| ESP counters flat, element provably on air, beacon-lateness small | **Host-format mismatch** — port the mesh beacon builder to the AP vif. **Still not BLOCKED** |
| ESP counters flat, beacon-lateness ≈ slot duration | **"the ESP can't serve beacons punctually enough for RAW"** — a different, more fixable problem. **Do not report as BLOCKED** |

That third row is why tags **4171-4180** (*beacons late from host max delay*, *beacons TX late*, *beacons
missing from host*) are read alongside 4210 and are **not** optional: the RAW window is referenced to the
**end of the beacon carrying the RPS**, so a late or jittered host-served beacon corrupts the time
reference even with a perfect engine. Without that control the two failure modes are indistinguishable,
and only one of them is fixable.

---

## Staging — the cheap arm first

S0b-2 recorded a separation worth exploiting: **with RAW enabled and beaconing but before any STA
associated, the Linux AP already showed `Valid: 1516, Invalid: 0` with every `Delayed` counter at zero.**
The engine arms on the **advertisement alone**; the deferral counters only move once there is traffic to
defer.

So Q3b splits:

- **S0b-5a — one board, no STA, no sniffer.** Does `assignments[]` climb ≈1/beacon with
  `invalid_assignments = 0`? That alone answers *"does the engine arm from an ESP-authored beacon"*.
- **S0b-5b — add a STA and uplink load.** Do `aci_frames_delayed` / `bc_mc_frames_delayed` move? That is
  the *gating* half, and it is what the throughput cost is made of.

5a is decisive for the headline question and costs one board.

---

## Bench state at session start — verified 2026-07-31 ~10:20 PDT

| Node | State | Note |
|---|---|---|
| board0 `E0:72:A1:F8:EF:A4` | present, `/dev/ttyACM0` | the AP |
| board1 `E0:72:A1:F8:F9:40` | present, `/dev/ttyACM1` | candidate STA for 5b |
| board2 `E0:72:A1:F8:F0:08` | **not enumerated** | expected — PPK2-rail gated |
| chronium | `wlan1` **DOWN** | sniffer if needed |
| chronite | `wlan1` **DOWN** | must **stop beaconing** for this stage (§7.5) |
| chronogen | `wlan1` **DOWN** | ⚠ carries a **stale `wpa_supplicant_s1g` process** with the radio down |

Bench pinned at Morse fw **1.17.8**, chip `0x0306`, S1G ch27. The `wpa_supplicant` seen on all three nodes
is NetworkManager's for `wlan0` — **never kill it, it drops the ssh management link.** Only the `_s1g`
variants are ours.

---

## Log

### 10:20 — the instrument, verified from the firmware rather than from the doc

Four facts had to be right before a single number could be trusted, and each was verified first-hand:

- **`mmwlan_get_morse_stats(uint32_t core_num, bool reset)`** (`mmwlan.h:2951`) returns a malloc'd
  `struct mmwlan_morse_stats {uint8_t *buf; uint32_t len;}` which must be freed with
  `mmwlan_free_morse_stats()`. The caller supplies **no** buffer — the implementation allocates
  `MORSE_STATS_BUF_LEN` itself (`umac.c:1776-1813`).
- **`core_num` is NOT a vif id.** The design doc's `mmwlan_get_morse_stats(1, …)` is right, but for a
  different reason than assumed: the driver dispatches it as **0 = HOST, 1 = MAC, 2 = UPHY**
  (`driver.c:1510-1548`, `MORSE_CMD_ID_{HOST,MAC,UPHY}_STATS_LOG`). RAW is a MAC statistic ⇒ core 1.
  The call also needs `driver_data.started`, which mirrors the Linux side where `morse_cli stats` fails
  with the link down.
- **TLV layout: u16 LE tag, u16 LE length, then value** — the same 4-byte convention morselib's own
  `umac_stats.c:76-96 append_tlv()` uses, and what `morse_cli` walks (`stats.c:281-329`,
  `offchip_statistics.h:14-16`).
- **`raw_stats_t` = 15 × `__le32` = 60 bytes** — `assignments[8]`, then
  `assignments_truncated_from_tbtt`, `invalid_assignments`, `already_past_assignment`,
  `aci_frames_delayed`, `bc_mc_frames_delayed`, `abs_frames_delayed`, `frame_crosses_slot_delayed`
  (`chronium:~/halow/morse_cli/stats_format.h:83-100`).

#### Tag 4210 was confirmed from the firmware's own descriptor table, not taken on trust

**`morse_cli` never hardcodes a tag.** It resolves tag → name through `get_stats_offchip()`, a lookup into
a descriptor table compiled into the firmware image: `struct statistics_offchip_data { char type_str[50];
char name[50]; char key[100]; u32 format; u16 tag; }` (`offchip_statistics.h:46-53`). So the authoritative
source for "which tag is RAW" is the blob itself.

Extracted it directly from `vendor/morse-firmware/firmware/mm6108.bin`, section **`.mac_offchip_stats`**
(offset `0x6ca34`, `0x83f8` = 33784 bytes). 206-byte records, **164 of them, dividing exactly** — which is
itself a check that the struct layout is right:

```
tag=4210   type=raw_stats_t   name=raw           key=RAW
tag=4126   type=uint32_t      name=n_cross_raw   key=AGG crosses RAW slot
```

Tag range in that section is 4097..4260. Types are **not** uniformly `uint32_t` — `uint16_t`, `uint64_t`,
`int32_t` and several structs appear — so a generic walker must read by the TLV's length and never assume
four bytes.

#### The dig turned up a better control than the plan had

The plan called for tags 4171-4180. The table shows the whole family, and **tag 4170 "Beacons TX"** is the
one that matters most:

```
4170  Beacons TX                                    <- the DENOMINATOR
4171  Beacons missing from host
4172  Beacons late from host delay (average usec)
4173  Beacons late from host max delay (usec)
4174  Beacons late from host count (packets)
4175  Beacons expired before queuing
4176  Beacons TX queued late
4177  Beacons TX attempts delayed
4178  Beacons TX late
4179  Beacons TX failed count
4180  Beacons TX lifetime expired
```

S0b-2 called the Linux `assignments[0] = 2179` "≈ once per beacon", but that was **inferred** from elapsed
time. With 4170 the ratio is **measured against the chip's own beacon count**. That turns a soft
"approximately" into an exact test, for free.

#### The walker refuses to guess

If the tag or the field offsets were wrong, the reader would return zeros from the wrong place — and
"all counters zero" is precisely the reading that means *the RAW engine never armed*. That is a
**false BLOCKED on the one question this stage exists to answer**. So the implementation treats a missing
tag 4210, or one whose length is not 60, as an **error** — it dumps the full tag inventory and reports the
instrument as broken, never as a zero measurement. The first read also dumps the whole inventory
unconditionally, so the read path proves itself before any verdict is drawn.

### 10:24 — S0b-5a: **Q3b PASS.** One board, no STA, no traffic

board0 running `test-raw-rps` (spike armed) alone on the air. chronite and chronogen down throughout.
Counters read with `reset=true` to open the window, then four cumulative 60 s windows.

| read | `assignments[0]` | Beacons TX | ratio | `invalid_assignments` |
|---|---|---|---|---|
| reset | 28 | 28 | **1.000** | 0 |
| t+60 s | 586 | 586 | **1.000** | 0 |
| t+120 s | 1172 | 1172 | **1.000** | 0 |
| t+180 s | 1758 | 1758 | **1.000** | 0 |
| t+240 s | **2344** | **2344** | **1.000** | **0** |

**The MM6108's RAW engine arms from an ESP-authored beacon.** Not "approximately once per beacon" —
**exactly one assignment registered per beacon transmitted, on all 2344 beacons**, with
`invalid_assignments = 0` proving the ESP's eight octets are *accepted*, not rejected. 586 per 60 s is
9.77/s, exactly the beacon rate at `beacon_interval_tus = 100` (102400 µs). For comparison the Linux arm
read 2179 assignments over ~3.7 min.

Every other RAW counter is zero: `truncated_from_tbtt`, `already_past`, `aci_frames_delayed`,
`bc_mc_frames_delayed`, `abs_frames_delayed`, `frame_crosses_slot`. **That is the expected reading with no
STA and no traffic** — there is nothing to defer — and it reproduces the separation S0b-2 recorded on
Linux, where the AP showed `Valid: 1516, Invalid: 0` with every `Delayed` counter still at zero before any
STA associated. The engine arms on the **advertisement alone**.

#### Beacon punctuality — the control that decides which failure mode you are looking at

| counter | after reset, over 2344 beacons |
|---|---|
| Beacons missing from host | **0** |
| Beacons late from host count (packets) | **0** |
| Beacons late from host max delay (usec) | **0** |
| Beacons expired before queuing | **0** |
| Beacons TX failed / lifetime expired | **0 / 0** |

**The host serves beacons punctually.** §7.7's *"ESP counters flat, beacon-lateness ≈ slot duration"*
branch — the one that would have meant "the ESP can't serve beacons punctually enough for RAW" — is
excluded outright rather than argued away. (The pre-reset read showed a single
`late from host max delay = 1207750 µs` with `count = 1`: one 1.2 s-late beacon during bring-up, before
the AP settled. It does not recur once running.)

Two chip-side counters do move and are worth carrying forward, because they are **not** host lateness:

| | t+60 | t+120 | t+180 | t+240 |
|---|---|---|---|---|
| Beacons TX attempts delayed | 1192 | 1293 | 1379 | 1491 |
| Beacons TX late | 225 | 247 | 271 | 295 |

The first window is anomalous (1192 delayed attempts in 586 beacons — more than one each), then the rate
collapses to ~100 delayed attempts and ~22 late beacons per minute, i.e. **~3.8 % of beacons TX-late in
steady state**. That is the chip deferring its own beacon TX on a busy medium, not the host being late —
`late from host count` stays at 0 throughout. Worth watching in S1+, since the RAW window is anchored to
the beacon.

#### Confound (d) excluded by a concurrent capture

A capture was taken on chronium's `morse0` **during** the counter run, so a flat counter could never be
blamed on the element not reaching the air. It did not need to be: **571/571 beacons carried
`D0 06 20 40 09 04 E0 1F`**, byte-identical, caps octet[6] `0x08` on all of them, single IE chain
`[0, 5, 208, 48, 127, 244, 213, 217, 232, 214, 221]`, 190 DTIM / 381 non-DTIM, 0 undecodable, 98.45 %
complete, and **only one source MAC on the air** (`6a:24:99:44:6b:b7`). Artifact:
Capture not committed (see `docs/worklog/artifacts/README.md`) — it is regenerable: flash `test-raw-rps`, then on chronium `sudo tcpdump -i morse0 -w cap.pcap`, decode with `python3 tools/raw_s1g_beacon_decode.py cap.pcap --sa <beacon SA>`.

**Per §7.7 this is the HARD GO row for the arming half.** What it does *not* cover is the **gating** half —
`aci_frames_delayed` / `bc_mc_frames_delayed` need traffic to defer, which needs a STA. That is S0b-5b,
below.

### 10:40 — S0b-5b: **the gating half PASSES too.** ESP AP + Linux STA + ping flood

**A blocker had to be fixed first, and it would have produced a silent null result.** `aci_frames_delayed`
counts the *local* transmitter withholding **its own** frames. `test-raw-rps` assigned **no IP at all**, so
a ping flood at it produces STA uplink and **nothing back** — every AP-side deferral counter would have
stayed at zero and the run would have measured nothing while looking like a clean negative. Fixed by
pinning a static IP (`192.168.12.1`) on the AP, modelled on `rimba-halow-ap`: mmhalow's netif is a DHCP
client and in AP mode mmhalow never fires a link-up event, so the netif must be brought up by hand with
`esp_netif_action_connected()` or lwIP silently never answers ICMP.

**Rig:** board0 = `test-raw-rps` (spike armed, static IP). chronite = Linux STA via `wpa_supplicant_s1g`,
associated at `wpa_state=COMPLETED`, `bssid=6a:24:99:44:6b:b7`, `192.168.12.2/24`. Load = a 120 s
`ping -i 0.002 -s 512` flood — 512-byte payloads chosen deliberately, because `frame_crosses_slot_delayed`
counts frames too long for the 10100 µs slot at 1 MHz, so bigger frames exercise it directly. Baseline
reachability first: 3/3 replies, RTT 13/87/220 ms.

| | t+240 s (pre-flood) | t+300 s (flood) | t+360 s |
|---|---|---|---|
| `assignments[0]` / Beacons TX | 2245 / 2245 = **1.000** | 2826 / 2826 = **1.000** | 3412 / 3412 = **1.000** |
| `invalid_assignments` | 0 | 0 | 0 |
| **`aci_frames_delayed`** | **0** | **77** | **139** |
| `bc_mc_frames_delayed` | 6 | 6 | 8 |
| **`frame_crosses_slot`** | **0** | **19** | **37** |

**The chip actively gates its own TX to the window when the ESP is the AP.** `aci_frames_delayed` goes
0 → 77 → 139 exactly across the flood, `frame_crosses_slot` 0 → 19 → 37, and the 1:1 assignments-per-beacon
ratio is untouched by the load. The Linux arm read `aci = 92` and `frame_crosses_slot = 27` — **the same
order of magnitude on the same firmware**, which is the like-for-like comparison §7.7 asks for.

`frame_crosses_slot` moving confirms on the ESP what S0b-2 found on Linux: **real frames are too long for a
10100 µs slot at 1 MHz.** Pair that with the ≤6-slot cap when sizing any real schedule.

**⇒ §7.7's HARD GO row is satisfied in full: "S0b-5: ESP counters move like the Linux arm's → HARD GO —
stage S1→S4."** Both halves — arming and gating — reproduce the Linux behaviour with the ESP as AP.

### 10:52 — a real parser bug, caught by the adversarial recon, and the fix validates beautifully

**`stats->len` overstates the TLV payload by exactly 4 bytes.** `mmdrv_get_stats()` sets
`*buf = resp->data` — already past the 4-byte `resp->status` — but `*len = le16toh(resp->hdr.len)`, and
`hdr.len` counts *everything* after the 12-byte command header, status included. Pinned three ways:
`struct morse_cmd_resp { hdr; uint32_t status; uint8_t data[]; }` (`morse_commands.h:146-151`);
`command.c:182` computes the total as `hdr.len + sizeof(struct morse_cmd_header)`; and decisively,
`driver.c:1704` synthesises an empty response as `resp->hdr.len = htole16(sizeof(resp->status))` = 4.

So the walker was reading **4 bytes past the end of the payload**, parsing whatever followed as a phantom
TLV header. Fixed: `tlv_len = stats->len - sizeof(uint32_t)`, with an underflow guard.

**Did it corrupt the S0b-5a/5b results? No — and that is demonstrable, not asserted.** Every expected tag
was found and every value was internally consistent, most sharply the exact 1.000 assignments-per-beacon
ratio at all nine reads. The over-read could only ever affect the tail, after all real TLVs were consumed.

**The fixed build was re-run on hardware, and the correction validates against the firmware itself:**

```
[reset] MAC stats blob: 1477 bytes reported, 1473 bytes of TLV payload
  tag=4170  len=4
  tag=4210  len=60
  (164 TLVs in 1473 bytes)
[reset] ==> assignments[0]/Beacons TX = 28/28
[t+60s] ==> assignments[0]/Beacons TX = 586/586
```

**164 TLVs — exactly the 164 records in `.mac_offchip_stats`.** The chip emits precisely one TLV per
descriptor record, and the corrected length makes the count come out whole. `tag=4210 len=60` matches
`sizeof(raw_stats_t)` exactly, `tag=4170 len=4` matches its `uint32_t`, and the counters reproduce the
earlier run (28/28, 586/586). With the old length the walk would have run into a phantom 165th entry.

That is three independent things agreeing — the firmware's descriptor table, the emitted TLV count, and the
per-element lengths — which is a much better check than "it printed a plausible number".

---

## Bench state left behind — radio-silent, verified

- **board0** reflashed to `rimba-hello`: banner present, **zero** `mmwlan`/HaLow lines.
- **chronite**: `wpa_supplicant_s1g` killed (0 procs), addresses flushed, `wlan1` DOWN.
- **chronium**: `wlan1` DOWN, `morse0` DOWN, back to `type managed`.
- **chronogen**: a **stale `wpa_supplicant_s1g` from 13 h 47 m earlier** was found and killed — it predates
  this session and had `wlan1` down, so it was not on the air, but it was a live process holding S1G state.
  Now 0 procs, `wlan1` DOWN.
- board1 untouched; board2 never powered.

## Footguns hit this session

- **`ttyACM0` re-enumerated mid-run at 10:35** and killed the serial listener with *"device reports
  readiness to read but returned no data (device disconnected or multiple access on port?)"*. Same class as
  the documented board2 capture flake, on board0. Recovered by reattaching **without** a reset — the
  counters are cumulative, so the run did not have to be restarted.
- **Never pipe a live console capture through `tail`.** The first attempt did, and `tail` withheld
  everything until the pipeline exited, so the log read as empty for five minutes while the run was
  perfectly healthy. Use `python3 -u` writing straight to a file.
- **The AP vif's beacon SA is the locally-administered variant again** — the STA associated to
  `6a:24:99:44:6b:b7`, not the chip MAC `68:…`. Same footgun as S0b-4, now confirmed from the STA side.

## Also landed here

`docs/reference/captures/wpa-sta-raw.conf` — the Linux infra-STA config the design doc's §7.6 item 4 asked
for and which had **never been tracked**: it existed only as `/tmp/wpa-sta*.conf` on chronium and
chronogen, i.e. on tmpfs, one reboot from gone — the same way the S0b-1/S0b-2 analysis scripts were lost.
Two fields in it are load-bearing and silently fatal if dropped: `sae_pwe=2` (Hash-to-Element, and a
**global** field in this fork, not per-network — the ESP AP advertises `[SAE-H2E]` and rejects a
hunt-and-peck commit, which presents as a wrong passphrase), and `ieee80211w=2` to match the AP's
`MMWLAN_PMF_REQUIRED`.

---

## What this does NOT settle — and it is the thing that now decides the port

**Every firmware question the RAW port hung on is answered.** The engine exists, it arms from a
host-authored beacon, a STA self-restricts, the ESP can author the advertisement byte-identically, and the
engine arms *and gates* with the ESP as the AP. §7.7's HARD GO row is satisfied.

**None of that says RAW is worth enabling.** The 2026-07-31 throughput A/B measured RAW costing the AP
**~22 % of its downlink** (1.63 → 1.27 → 1.61 Mbit/s, A-B-A, ranges non-overlapping) — and that was with
**one STA, where there is no contention to save**. It is a cost-only floor, not a verdict.

So the open question is economic, not technical:

> **At what client count does the contention saving exceed the ~22 % standing cost?**

It is **not answerable on this bench** — the bench cannot produce the 30+ contending clients where RAW is
supposed to pay for itself, and the mesh-gate's realistic target is 16-32. What it needs is either
many-STA hardware or a model calibrated on the numbers now in hand:

| input | value | source |
|---|---|---|
| standing downlink cost | **~22 %** | throughput A/B, 2026-07-31 |
| max slots per window | **6**, not the field's 63 | driver caps silently at `raw.c:329-333` |
| slot sizing reality | real frames **exceed** a 10100 µs slot at 1 MHz | `frame_crosses_slot` moved on Linux (27) **and** the ESP (37) |
| AP defers its own downlink | yes — `aci_frames_delayed` on the AP side | S0b-2 (92) and S0b-5b (139) |

**Settle that before staging S1→S4.** The gate is already the throughput bottleneck, so a GO later
reversed on throughput is worse than a slower decision now — which is the same reasoning that put the
cheap all-Linux gate ahead of the ESP work in the first place, and it was right then.
