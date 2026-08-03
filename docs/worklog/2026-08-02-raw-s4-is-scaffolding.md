# RAW S4 — the stage is misconceived, and S3 was the finish line

**Date:** 2026-08-02
**Outcome:** **S4 dropped.** No code written. The AID-list work moves to S6, where it is actually
consumed. **S3 is the MVP finish line.**

## What S4 was supposed to be

From the design doc §3:

> **S4 — AID-list + single-group mapping.** Build the ordered connected-AID list from the ESP dense-AID
> allocator/bitmap … Map config `start_aid/end_aid` to the live range; refresh on STA join/leave …
> **This is the MVP finish line.**

The stated motivation, carried through several worklogs including my own: *"the advertised group is a
well-formed fiction that mostly names AIDs nobody holds"* — advertise 1–255 while three STAs are
connected, and the schedule doesn't describe reality.

## Why it is wrong

**The connected-AID list does not narrow the advertised group. Its only consumer is beacon spreading.**

Every use of `raw->aid_list`, and of `start_aid_idx` / `end_aid_idx`, sits behind a beacon-spreading
guard:

| site | guard |
|---|---|
| `morse_raw_generate_assignment()` `raw.c:519` | `if (config->beacon_spreading.nominal_sta_per_beacon && !(start_aid_idx < 0) && !(end_aid_idx < 0))` |
| `morse_raw_refresh_aids()` `raw.c:862` | `if (config_ptr->beacon_spreading.nominal_sta_per_beacon)` — refreshes indices *only* for spreading configs |
| `raw_update_aid_indexes()` `raw.c:818-833` | called only from the above |

And the non-spreading path is explicit, with the reference's own comment (`raw.c:595-599`):

```c
    /* If not using beacon spreading or no connected STAs use the full AID range. */
} else {
    current_beacon_start_aid = config->start_aid;
    current_beacon_end_aid   = config->end_aid;
    config->beacon_spreading.last_aid = config->end_aid;
}

return morse_raw_generate_assignment_with_aid_range(mors_vif, config, rps_ie_start,
    current_beacon_start_aid, current_beacon_end_aid);
```

`morse_raw_generate_assignment_with_aid_range()` is the function S1 already ported, and it receives the
**configured** range. So for a plain GENERIC RAW — which is the whole MVP scope — the advertised group is
the configured one, and the ESP already emits exactly that.

Building `morse_generate_aid_list()` plus join/leave refresh hooks now would be **dead scaffolding**: no
on-air change, nothing to verify it against, and its only consumer two stages away.

## The premise was wrong too, not just the implementation

The "well-formed fiction" framing — mine, repeated in the S1/S3 worklog and the backlog — misreads what
the RAW group is for.

**The group expresses *which AIDs are subject to RAW*.** That is a policy the integrator sets, exactly
like the slot count and duration. It is not a report of who is currently associated. `1–255` means "every
station that can associate is subject to this schedule", which is a correct and complete statement of
that policy, not a placeholder.

Narrowing it to the connected set would actively make things worse: the beacon would churn on every join
and leave, and the advertisement would carry two different meanings at once — policy and census.

## What this changes

- **S3 is the MVP finish line**, not S4. After S3 the ESP advertises a correct, configurable RAW schedule
  and the chip arms and gates on it. There is no further stage needed to make it *work*.
- **The AID-list work moves into S6**, alongside beacon spreading and PRAW cadence, where it is
  load-bearing.
- **S5** (per-priority AID grouping, multiple assignments per element) is unaffected and remains a real
  feature.

## What I got wrong, and why it survived this long

I wrote "S4 is the MVP finish line" into the backlog, the code map, two worklogs and a PR description
before reading the code that consumes the AID list. The design doc said it, and it *sounded* right — an
advertisement naming absent stations is an obvious-looking defect. It took twenty minutes of grepping
`raw.c` to find that the reference deliberately does the opposite, and says so in a comment.

The same pattern produced the break-even analysis two days ago: a plausible framing carried forward
several documents before anyone checked it against the source. Cheap to catch by reading first; expensive
to catch after the code exists.

## Where the port now stands

Every firmware question is answered and the implementation works:

| | |
|---|---|
| S0a–S0b-5 | spike complete, **HARD GO** — engine arms and gates with the ESP as AP |
| S1 + S2 | RPS encoder, ported from Linux, byte-identical on air |
| S3 | RAW as configurable AP state, default off, containment measured |
| **S4** | **dropped — misconceived** |
| S5, S6 | real features, not started |

**The open question is unchanged and is not a firmware one:** the break-even analysis of 2026-07-31 still
says RAW's payoff cannot be demonstrated, the ~22 % cost figure is a Linux-AP number, and the gateway caps
at 8 clients today for reasons unrelated to contention. S5 and S6 are genuine work, but nothing has moved
the case for doing them.

**Recommendation: stop here.** The port has reached a natural, working, documented stopping point. Resume
at S5/S6 only if the traffic-mix question or a raised client ceiling changes the economics.
