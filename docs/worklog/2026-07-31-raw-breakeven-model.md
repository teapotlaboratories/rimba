# RAW break-even — at what client count does it stop costing more than it saves?

**Date:** 2026-07-31
**Follows:** the RAW S0b spike, which is now complete and GO on every firmware question
([`s0b-5`](2026-07-31-raw-s0b-5-esp-counters.md)).
**Question:** RAW costs the AP **~22 % of its downlink** as a standing cost, measured with **one STA**,
where there is no contention to save. The mesh-gate targets **16-32 clients**. **At what client count does
the contention saving exceed the cost?**

**Why this is being modelled rather than measured:** the bench physically cannot produce 30+ contending
clients — there are three ESP boards and four Linux nodes, one of which must be the sniffer. Buying
many-STA hardware to answer it is a real cost, so the point of the model is to decide whether that is worth
doing, and to say what the answer plausibly is if it is not.

---

## The standard this has to meet, and the way it could go wrong

A model is easy to make *look* authoritative and hard to make *true*. The specific failure mode here is a
plausible-looking curve built on unsourced constants, which would then justify (or kill) a feature on
numbers nobody measured. This project has already eaten two instrument errors that looked plausible — the
S0b-1 grid-phase gate and the S0b-2 66 %-loss capture — so the discipline is the same one that caught
those:

1. **Every parameter is labelled `MEASURED` / `SOURCE-DERIVED` / `ASSUMED`.** No bare numbers.
2. **The model must reproduce the measurements we already have** before it is allowed to extrapolate —
   specifically the ~22 % single-STA cost, which is a point we know.
3. **The output is a range with a sensitivity analysis, not a number.** If the break-even moves from 8 to
   40 clients under a plausible parameter swing, that is the finding, and it means "go measure it".
4. **A refusal is a valid outcome.** If the assumptions that decide the answer cannot be grounded, the
   honest deliverable is "not decidable without many-STA hardware, and here is exactly which parameter
   decides it".

---

## Log

### Two things in the existing A/B data that change the model before it is even built

Re-reading [`2026-07-31-raw-throughput-ab.md`](2026-07-31-raw-throughput-ab.md) with the model in mind
turns up two issues, and both push in the same direction — **the headline "22 %" is probably not the number
that belongs in a break-even calculation.**

#### 1. The 22 % is a TCP figure, and TCP amplifies

The two arms disagree, and the disagreement is informative:

| instrument | baseline | RAW on | delta |
|---|---|---|---|
| TCP downlink | 1.63 Mbit/s | 1.27 Mbit/s | **−22 %** |
| UDP downlink loss @ 2 Mbit/s offered | 0.12 % | 7.2 % | **−7.1 pp of offered** |

If RAW denied 22 % of downlink airtime, a fixed 2 Mbit/s offered load should have lost ≈ 22 %, not 7.2 %.
TCP is a *reactive* instrument: it responds to added loss and latency by backing off, so a ~7 % airtime
denial plausibly produces a ~22 % TCP throughput drop. The throughput-A/B worklog already flags this in its
own caveats ("part of the 22 % is TCP responding to added loss/latency, not raw airtime denial. The UDP arm
… is the cleaner statement of the same effect") — but the **~7 %** number is the one that never got
promoted into the headline, and it is the one a model of *airtime* should use.

**Consequence: the standing cost may be ~7 % of airtime, not ~22 %.** That is a 3× difference in the thing
the break-even is measured against, and it moves the answer a long way. Both numbers will be carried
through the model as a range rather than picking one.

#### 2. Two mechanisms fit the measurement equally well, and they imply opposite models

The A/B worklog reads the result as *"RAW is not hard-gating the AP … if the AP's downlink were strictly
confined to [the window], throughput would fall by ~80 %. It fell by 22 %. So the chip is imposing a
partial cost."* That assumes the AP is **confined to** the RAW window.

There is a second reading that fits the arithmetic at least as well:

| reading | what the AP does | predicted cost | measured |
|---|---|---|---|
| **A — confined to the window** | AP may transmit only inside the 19.7 % window | **−80.3 %** | −22 % (TCP) / −7.1 pp (UDP) |
| **B — excluded from the window** | window is reserved for assigned STAs' uplink; AP defers its own downlink during it | **−19.7 %** | −22 % (TCP) / −7.1 pp (UDP) |

Reading **B** predicts −19.7 % against a measured TCP −22 %, which is a much better fit than A's −80 %, and
it is the more natural 802.11ah semantic: a RAW slot is reserved for the STA assigned to it, so the AP
withholds its own downlink — which is precisely what `aci_frames_delayed` counts (the *local* transmitter
deferring its *own* frames; it counted **37 897** in that run).

**Why this is not a detail.** Under reading B, RAW is not "wasting" 20 % of airtime — it is
**time-division-multiplexing the medium between AP downlink and STA uplink**. The cost is airtime
*reallocated to uplink*, not airtime destroyed. A break-even model that treats it as pure waste would
understate RAW badly on an uplink-heavy workload, and the mesh-gate's traffic mix is not known to be
downlink-dominated.

⚠ **Neither reading is confirmed from source yet.** This is the parameter that most decides the answer, so
it is being verified against `morse_driver/raw.c` before any curve is drawn. If it cannot be settled, the
model does not get built — see the standard above.

### Attempting to settle it by measurement — and why the measurement does not bear the weight

The mechanism question turns out to be *measurable* rather than arguable, because the firmware's descriptor
table (extracted for S0b-5) carries a denominator: **tag 4133 "TX Total"**. `aci_frames_delayed / TX Total`
discriminates the readings directly — ≈80 % for (A), ≈20 % for (B). Added that plus a set of contention
observables (**4159 "TX average backoff slots"**, **4105 "DCF medium busy before TX"**, 4143/4144 ACK
valid/timeout) to the fixture and ran it: ESP AP, Linux STA associated, 150 s ping flood.

| read | `aci_frames_delayed` | TX Total | ratio |
|---|---|---|---|
| t+60 s | 40 | 860 | **4.65 %** |
| t+120 s | 91 | 1744 | **5.21 %** |

**Neither ≈80 % nor ≈20 %.** ~5 %. Which would be a striking third answer — except the run cannot support
any answer, and the reason is worth recording:

> **The load was ~1 % of link capacity.** `ping -i 0.002` requests 500 pps but achieved **~4 pps** (604
> replies in 150 s), i.e. ~17 kbit/s against a link that does ~1.68 Mbit/s. TX Total ran at 14.5 frames/s.
> At that offered load the AP is idle almost all the time, so *of course* few frames collide with the RAW
> window — the ratio is measuring the load, not the mechanism.

**And the bench cannot fix this.** A saturating downlink against the ESP AP needs an iperf server on the
ESP, which exists only in `rimba-halow-ap-perf` (console REPL + iperf-cmd component) and not in this
fixture, and cannot be added to it without also pulling in the REPL. `ping` is the only generator available
and it delivers ~4 pps. **So the mechanism question is not answerable with the ESP AP on this bench**, and
the ~5 % figure is recorded as *what a 1 %-load run produced*, not as a result.

There is also a **unit ambiguity** that would have to be resolved even at saturation: A-MPDU is enabled, so
`TX Total` may count aggregates while `aci_frames_delayed` counts MPDUs (or vice versa). At 4 pps offered
and 14.5 TX/s the two are the same order, so they are probably both MPDU-ish here — but that is inference,
and at saturation with real aggregation the ratio could be off by the aggregation factor. **A ratio between
two counters whose units are unconfirmed is not a measurement.**

---

## Conclusion: the break-even is NOT computable from what exists — and building the model anyway would have been the mistake

Per the standard pre-registered at the top ("a refusal is a valid outcome"), this is the honest deliverable.
Four independent gaps, any *one* of which is enough to sink the calculation:

| # | gap | why it is fatal to the model |
|---|---|---|
| 1 | **The 22 % is a Linux-AP number** | The throughput A/B was chronite (Linux AP) → chronium (Linux STA). **Zero ESP boards were involved.** No ESP throughput A/B exists anywhere. Transferring 22 % to an ESP32 gate assumes the cost is a chip-level property — plausible (same blob; same-order counters, `aci` 92 vs 139 and `frame_crosses_slot` 27 vs 37) but **unproven**. |
| 2 | **The cost figure is transport-dependent and unresolved** | TCP says −22 %, UDP at fixed offered rate says −7.1 pp. TCP is reactive, so the *airtime* cost is likely nearer 7 % than 22 %. A 3× spread in the quantity the break-even is measured against. |
| 3 | **One RAW configuration was ever measured** | 2 slots × 10100 µs, 19.7 % window, `cross_slot=0`, all AIDs. Cost *and* benefit both scale with window fraction and slot count, and **that curve was never measured**. The measured config is not even a good one — see below. |
| 4 | **The benefit side has never been observed at all** | RAW's payoff is reduced collisions among many contenders. The bench maxes out at ~3 contending nodes. Nothing in this project has ever measured RAW helping. |

Gaps 1 and 4 cannot be closed by analysis — only by hardware.

### Gap 5, found last and decisive on its own: the contention model has no clock

A DCF/EDCA contention model is parameterised almost entirely by **`aSlotTime`** and **SIFS** — every
backoff, DIFS and collision cost is expressed in them. A search of all four trees
(`components/halow/**` incl. the vendored hostapd, `chronium:~/halow/morse_driver/**`,
`chronium:~/halow/hostap/**`) finds **neither constant as a microsecond value anywhere**. The only hits are
`SHORT_SLOT_TIME` *capability bits* (`dot11.h:116-117,210-221`), a reported BSS flag
(`morse_driver/wiphy.c:1033`), and — tellingly — an admitted placeholder in hostapd's TSPEC path:
`hostap/src/ap/wmm.c:253`, `50 /* FIX: proper SIFS + ACK duration */`.

**So the timing constants a contention model runs on are not knowable from this codebase.** They live in
the MM6108 blob. Any model would have to *assume* S1G slot/SIFS values, and those assumptions would then
silently determine the break-even. That is gap 3's "unsourced constant decides the answer" failure in its
purest form, and it alone is sufficient grounds not to publish a curve.

### And the sharpest finding: cost and benefit fall on OPPOSITE SIDES of the link

The EDCA parameters are asymmetric, deliberately, and nobody had noticed what that implies here. From
`hostap/src/ap/ap_config.c` (the bench conf sets no `wmm_ac_*` keys, so compiled defaults apply verbatim):

| best-effort (AC_BE) | AIFSN | CWmin | CWmax | TXOP |
|---|---|---|---|---|
| what the AP **advertises to STAs** (`:211-222`) | 3 | 15 | **1023** | 0 |
| what the Linux AP **uses for itself** (`:223-231`) | 3 | 15 | **63** | 0 |

**The STAs' contention window can grow 16× further than the AP's own.** Under n-STA collision load the
STAs back off into a much larger window while the AP does not, so **the AP retains a growing share of
medium wins as n rises**. Uncontrolled EDCA contention therefore degrades **STA uplink**, not the AP
downlink.

**This breaks the break-even question as posed.** The measured ~22 % cost is *AP downlink*. RAW's benefit
is *reduced uplink collisions among STAs*. Those are different sides of the link, so "at what client count
does the saving exceed the cost" is comparing two different quantities unless the traffic mix is known.
It also means the AP is already partly self-protecting against the very problem RAW is being ported to
solve — an alternative that was never on the ledger.

**A second-order consequence for gap 1.** The ESP AP is not merely *different* hardware from the Linux AP
the 22 % was measured on — it is **configured to contend differently**. morselib's defaults
(`umac_config.c:13-51`) give the ESP AC_BE **CWmax 1023 and TXOP 15008 µs**, against the Linux AP's
**63 and 0 µs**. So the ESP AP is markedly *less* aggressive and *does* burst, where the Linux AP is more
aggressive and sends exactly one PPDU per win. Transferring the Linux number to the ESP is not a small
extrapolation; it crosses a real configuration difference in exactly the parameters that govern the effect
being measured.

### What can be said without a model, and it is more useful than a curve

**1. The measured configuration was chosen for byte-diffability, not for benefit.** The golden constant
uses **2 slots of a possible 6** and a **19.7 % window**. A deployment config would use ~6 slots over a much
larger window — dividing contenders by ~6 instead of ~2, over ~5× more of the airtime. **The measured 22 %
is therefore not "the cost of RAW"; it is the cost of the one schedule that happened to be convenient for a
byte-diff.** Reading it as the standing cost of the feature overstates the cost and understates the benefit
simultaneously.

**2. `cross_slot = 0` in every run, and it is the obvious cost lever that was never pulled.**
`frame_crosses_slot_delayed` is the vendor's own *"frames that could've been sent in the RAW but were too
long for slot"* — it moved on both stacks (Linux 27, ESP 37). Setting `cross_slot = 1` lets a frame span a
slot boundary, which is precisely the deferral that counter measures. **Untried.**

**3. RAW is not currently the binding constraint on the thing it is being ported for — and this is the
finding that should change the plan.** The mesh-gate's scaling analysis names *three* blockers to 16-32
clients, and RAW addresses only one:

| blocker | status | fixed by RAW? |
|---|---|---|
| `GATE_MAX_CLIENTS = 8` and friends (`HALOW_AP_MAX_STAS`, `ARP_TABLE_MAX`, DHCP pool) | **binding today** — the gate cannot reach 16 clients at all | ❌ config work, listed as the "easy + medium tiers" |
| Proxy-ARP proactive push is **O(n_ap × n_mesh)**, one unicast per pair every 3 s | binds from ~30 clients | ❌ |
| EDCA contention | the reason RAW exists | ✅ |

**The gate's ceiling is 8 clients today.** RAW is a fix for contention at 16-32 — a regime the gate cannot
currently enter for reasons that have nothing to do with RAW. Building S1→S4 now optimises a constraint
that is not yet reachable, and it would be measured on a gate that still cannot host the client count where
the benefit appears.

### Recommendation

**Do not stage S1→S4 yet.** Not because RAW looks bad — the firmware answer is a clean GO and the cost is
probably nearer 7 % than 22 % on a schedule nobody has tuned — but because the payoff cannot be
demonstrated, and the cheaper blocker is upstream of it.

**Order of work that actually moves the ceiling:**

1. **Raise the gate ceiling to 16-32** (the tracked "easy + medium tiers": `HALOW_AP_MAX_STAS`, PSRAM,
   Kconfig range, `ARP_TABLE_MAX`, replace the gate's client table with `mmwlan_ap_get_sta_status()`).
   Until this lands, no RAW benefit is observable **in principle**.
2. **Then** the minimal experiment that settles the break-even, which needs only what step 1 produces:
   - an **ESP-AP** throughput A/B (the missing one), RAW off vs on, at saturation;
   - swept over **window fraction and slot count** (the curve that was never measured), including
     `cross_slot = 1`;
   - at **increasing client counts** up to the new ceiling, which is the only way the benefit term ever
     becomes visible.
   That experiment is cheap *once the ceiling is raised* and impossible before it.
3. Keep the S0b spike fixture and tooling as-is — they are exactly what step 2 needs, and `test-raw-rps`
   already reads every counter that experiment wants.

**What would change this recommendation — and it is now the cheapest and highest-value question open:**
**characterise the expected traffic mix.** This started as a hunch and the EDCA asymmetry above turned it
into a mechanism:

- RAW's measured **cost** falls on **AP downlink**.
- RAW's **benefit** is fewer uplink collisions among STAs.
- The AP's own CWmax (63) is **16× tighter** than what it advertises to its clients (1023), so rising
  contention degrades **STA uplink** while the AP keeps winning the medium.

So on a **downlink-heavy** gate, RAW pays a real cost to fix a problem the AP is already partly insulated
from — a poor trade. On an **uplink-heavy** gate (sensors reporting, which is the archetypal HaLow
workload), the cost side of the ledger is much smaller than these downlink-only measurements imply and the
benefit lands exactly where the pain is.

**Nothing in the project records the expected mix.** It needs no hardware, no bench slot and no firmware —
and it plausibly decides the feature. It should be answered before anything else on this list.
