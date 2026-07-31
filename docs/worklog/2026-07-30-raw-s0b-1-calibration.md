# 2026-07-30 — RAW S0b-1: the sniffer is good to ~1 µs, and two of this stage's own gates were wrong

**Goal.** Calibrate the instruments before the RAW spike's decisive stage. Two questions, both of which
could kill the timing arm of Q3: what is the *jitter* (not the resolution) of the sniffer's
`radiotap.mactime`, and is the firmware's RAW counter block real or a vestigial CLI-side formatter? The
plan said: if both fail, S0b is not decidable on this bench as designed and must be re-scoped rather than
run to an ambiguous negative.

**Outcome.** Timing is **viable with ~2000:1 margin** — but the stage's own pass/fail gates were measuring
the wrong things, and both had to be replaced. The counter instrument is **real and structurally
complete**. Nothing was flashed; no ESP board was involved; the bench was returned to radio silence.

**Nothing was committed.**

---

## 1. Rig and bench state

`make test-bench` reconciled against `docs/reference/rimba-bench-devices.md` before starting: board0 and
board1 present, **board2 not enumerated** (PPK2 rail unheld — its documented normal state, and this stage
needs no ESP at all), chronite up, bench pinned at fw 1.17.8 / chip `0x0306` / S1G ch27.

| | |
|---|---|
| **chronium** | sniffer — `iw dev wlan1 set type monitor`, `set freq 5560`, `morse0` up. Confirmed `type monitor`, `channel 112 (5560 MHz)` |
| **chronite** | plain reference AP — `hostapd_s1g` from the tracked `docs/reference/captures/hostapd-rimba.conf` (`beacon_int=100`, `dtim_period=1`, SAE/PMF, **no RAW block**). Reached `AP-ENABLED` in 1 s |
| ESP boards | **none used** |
| Capture | 235 s, `tcpdump -i morse0 -w`, 2294 frames |

The midnight `daily-reboot.timer` was armed and due in 3 h 24 min — noted, and comfortably clear of a
4-minute capture.

**Only `wpa_supplicant_s1g` was killed on chronium, never the system `wpa_supplicant`** — the latter is
NetworkManager's, managing the node's wlan0, and killing it would have taken the management link down.

## 2. The sniffer clock is excellent — ~1 µs

2222 S1G beacons (`0x0031`) decoded across 2290 TBTT slots. RSSI median **−36 dBm**, min −38, max −35 —
nowhere near the ≈−3 dBm RX-overload signature the bench notes warn about, so **no TX-power capping was
needed** (chronite was left at its default 27 dBm).

On successive beacon intervals, against the expected 102400 µs:

```
median        102400 us   (offset from expected: +0 us)
MAD                1 us
robust sigma   1.483 us   (1.4826*MAD)      GATE <= 20us   -> PASS by 13x
stdev (raw)  179.873 us
min / max     98655 / 106145 us
p1 / p99     101986 / 102814 us
p99 - p1         828 us                     GATE <= 100us  -> FAIL
```

So: the **bulk** of the distribution is within ±1 µs of nominal, and there is a fat tail. The two gates
disagree because they are sensitive to different things.

## 3. The p99−p1 failure is not the clock — and the gate was the wrong test

Fitting an absolute TBTT grid (slope 102400.320 µs/beacon) and taking each beacon's residual against it:

```
MAD 0.8 us      robust sigma 1.12 us
p1 / p99      -2 / +415 us
min / max     -3 / +3744 us
within +/-50us  of grid : 95.72%
within +/-500us of grid : 99.68%
```

**The residuals are strongly one-sided.** A beacon is essentially never *early* (min −3 µs) but can be
*late* by up to 3.7 ms. A jittering clock would be symmetric; a transmitter deferring on a busy medium is
one-sided. This is **AP-side beacon deferral, not sniffer timestamp jitter**.

*(A weaker corroboration: of 159 large deviations with a valid successor, 75 (47 %) sum back to 2×BI
within 100 µs — a late beacon followed by an early one, i.e. the grid holds and the beacon slipped. I am
not leaning on this; the one-sidedness is the real evidence, and 47 % is only partial support.)*

**Why this matters, and it is load-bearing for Q3.** The RAW window is referenced to **the end of the
beacon that carried the RPS**, not to an abstract TBTT. So when a beacon is deferred, *the window moves
with it* — and a STA obeying that window moves with it too. Measuring a STA's transmit phase **relative to
the observed beacon** cancels the deferral exactly.

Measuring it against a **fitted or idealised TBTT grid** does the opposite: it would smear ~4 % of frames
by up to **3.7 ms**, which is **a third of a 10100 µs slot** — and that reads as leakage. A Q3 negative
produced that way would be an artefact of the analysis method.

**Method fix, now written into the design doc §7.2 and the §7.7 decision table:** measure phase relative to
the observed beacon, never against a grid. With that method the instrument precision is the **~1 µs** MAD
figure, against the **2048 µs** separation the A/B-1 design programs between two STAs' slots — a **~2000:1
margin**. Timing is viable, comfortably.

## 4. The drop-counter integrity gate is nearly useless — replace it

This is the finding I did not expect, and it invalidates a check the plan made mandatory for *every*
capture.

The plan says: snapshot `ip -s link show morse0` before and after, and **require a zero drop delta**,
because frames the tap loses look exactly like confinement and manufacture a false GO.

Measured over this capture:

| | |
|---|---|
| TBTT slots spanned | **2290** |
| beacons decoded | **2222** |
| slots with no beacon | **68** |
| of which explained by the 72 unrelated 33-byte frames | 10 |
| **genuinely absent from the tap** | **58 = 2.53 %** |
| **`morse0` `dropped` counter delta** | **1** |

**The counter accounted for 1 of 58 missing frames.** A "zero drop delta" gate would have *passed* this
capture while 2.5 % of frames were silently absent — the exact false-GO mechanism the gate exists to
prevent.

I checked whether the 72 undecoded frames were truncated beacon receptions that would close the gap: they
are not. All 72 are exactly 33 bytes, all carry mactime, and their offsets from the nearest TBTT slot span
−50651 to +50588 µs — i.e. they are spread across the whole beacon interval rather than sitting at TBTT.
Only 10 land in an empty slot, by coincidence.

**Replacement gate, now in the doc:** reconcile the decoded frame count against the TBTT slots spanned
(derivable from the mactime grid itself). That is what caught this. The drop counter stays as a weak
secondary signal only. And the threshold cannot be "zero loss" — **~2.5 % appears to be the healthy
baseline on this tap**, so later runs must be judged against that, not against perfection.

## 5. The counter instrument is real

`morse_cli -i wlan1 stats` on chronite, RAW **disabled**:

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

The section is **present and structurally complete** on the live 1.17.8 stack, and reads **all-zero** with
RAW disabled — which is exactly correct, and is the baseline any S0b-2 reading will be compared against.
The open question "is this a CLI-side formatter for a section the firmware omits or zeroes?" is answered:
**it is not absent and not structurally missing**, so the instrument is not void. `AGG crosses RAW slot`
(tag 4126) is also present in the main block.

What this still does **not** establish is that the counters *increment* — that needs RAW actually enabled,
which is S0b-2. But the failure mode the plan feared (instrument void, decision falls back to timing
alone) is off the table.

## 6. Two smaller things settled for free

- **`morse_cli -i wlan1 stats` fails with the link down** — `NL80211, code -100: Error callback called`.
  So the counter probe is **not** usable as a zero-radio pre/post control; it needs the radio up. §7.2 had
  this as an open question; it is now closed, negatively.
- **S0b-0's hostapd-defaults finding confirmed on the live stack.** `hostapd_s1g` logs at startup:
  `RAW Settings: disable 4096 26900 1 disable 0 0 0 0` — exactly the `start_time_us=4096`,
  `duration_us=26900`, `slots=1` defaults that produce the 9-byte element instead of the golden 8-byte one.
  Disabled here, so no RPS reached air, which is what this stage wanted. Good independent corroboration of
  a desk finding.

## 7. Verification status

- **On hardware, on air.** Both Linux radios were up; chronite transmitted beacons and chronium captured
  them. No ESP board was involved and nothing was flashed.
- **On-air frame verification: not applicable to this stage, and stated rather than skipped.** Nothing
  *new* was transmitted — chronite emitted stock `hostapd_s1g` beacons from the tracked reference config,
  with no RAW and no hand-built element. There is no frame of ours to byte-diff. That obligation belongs to
  S0b-2 and S0b-4, and it **stays owed**.
- **Bench returned to radio silence:** `hostapd_s1g` killed, `morse0` down, `wlan1` down on both nodes,
  chronium's `wlan1` returned to `type managed`. Verified after teardown. No ESP boards were touched, so
  none needed reflashing to `rimba-hello`.

## 8. Caveats

- **Single AP, single session, one RF geometry.** The deferral tail is a property of *this* AP on *this*
  channel with whatever else was on the air tonight. It should be re-measured if the rig changes, and the
  ~2.5 % baseline loss likewise.
- **The 2.53 % loss figure counts beacons only.** Data frames are shorter, sent at different rates, and may
  well be lost at a different rate — S0b-2's reconciliation must be done against the STA's own TX counters,
  not extrapolated from this number.
- **Nothing here says anything about RAW enforcement.** No RPS was emitted at all. This calibrates
  instruments; it does not touch Q2, Q3a or Q4.
- **The counter block was read on chronite (the AP) only**, and only in the RAW-disabled state.
- The 47 % pairing statistic in §3 is weak corroboration and is flagged as such; the one-sided residual
  distribution is what the conclusion rests on.

## 9. Next — S0b-2, the gate

All-Linux, still zero ESP code: chronite as the RAW AP driven by `morse_cli` directly (not hostapd's
`raw={}` block), chronogen as the STA, chronium monitoring. **Pin the RAW block explicitly** —
`start_time_us=0`, `duration_us=20200`, `slots=2`, `cross_slot=0` — or hostapd's defaults will emit the
9-byte element and the byte-diff will fail for a config reason (§7.3 row (j)).

Read `morse_cli -i wlan1 stats` before and after on both ends and diff the RAW section. **`assignments[]`
moving means the chip parsed its own outgoing RPS**; `aci_frames_delayed`/`bc_mc_frames_delayed` moving
means it is gating its own TX. `invalid_assignments` moving means the bytes are wrong. A flat AP-side
counter set with a provably-valid RPS on air is the **BLOCKED** verdict, reached with no ESP code written.
