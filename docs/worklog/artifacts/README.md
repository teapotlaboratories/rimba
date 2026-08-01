# Worklog artifacts — what belongs here

Binary artifacts committed alongside worklogs. Git history is permanent, so the bar is deliberate.

## The rule

> **A capture the tooling is *validated against* is a test fixture and belongs here.
> A capture that is *evidence for a conclusion* belongs in the worklog as numbers.**

An artifact earns its place by having **ongoing function** — something re-runs against it. A capture whose
entire content is a count ("387 of 387 beacons carried the element") does not: the count is the artifact,
and it is already in the worklog, where it is searchable and readable without a decoder.

## Applying it

**Keep** when at least one is true:

- **The tooling is calibrated against it.** `tools/raw_s1g_beacon_decode.py` is pinned to
  `raw-s0b/s0b-2-reference-rawenabled.pcap` (RAW enabled, golden element present) *and*
  `raw-s0b/s0b-1-cal-rawdisabled.pcap` (RAW disabled, zero occurrences). Both halves, or neither — a
  decoder validated only on captures containing the element is not validated for the "element absent"
  reading, and that reading is the one that maps to a blocking verdict.
- **It cannot practically be reproduced.** `raw-s0b/s0b-4-esp-spike-dualap.pcap` holds an ESP AP and a
  Linux AP transmitting *simultaneously*, which needs three radios with both APs up. The two files above
  came off a bench session that no longer exists, and were recovered from a tmpfs hours before a reboot
  would have destroyed them.
- **It is a patch or config that reproduces a build**, e.g. `raw-s0b/s0b-4-morselib-spike.patch`.

**Do not keep** a capture that is regenerable in a few minutes of bench time and whose content is a
count, a ratio, or a pass/fail already stated in the worklog. State the number, name the fixture and the
command that produced it, and let the next person regenerate it if they need the bytes.

## Why this matters more than the bytes

Any single capture is cheap. The practice is not: "ran a bench session, committed the pcap" compounds
every session and never self-corrects, because deleting later reclaims nothing — the objects stay in
history. The cost is paid once, at the moment of committing, so that is where the judgement belongs.

If a dropped capture turns out to be needed, the worklog names the fixture, the rig and the exact
command. Regenerating is minutes. Un-committing is never.
