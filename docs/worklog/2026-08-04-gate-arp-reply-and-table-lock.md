# A malformed proxy-ARP reply that passed every test, and an unguarded table beside it

**Date:** 2026-08-04
**Task:** review `esp-halow-examples` PR #2 — the mesh-gate example split.
**Outcome:** the review found a **real defect in shipped Rimba code**, present in three apps across
two repositories, plus a data race the example had already fixed and Rimba had not. Both fixed in
both places.

---

## The defect

`gw_arp_build_reply()` / `arp_build_reply()` writes the ARP **target hardware address** at absolute
offset **34**. It belongs at **32**.

| field | correct | written |
|---|---|---|
| sender hardware | 22..27 | 22..27 ✓ |
| sender protocol | 28..31 | 28..31 ✓ |
| **target hardware** | **32..37** | **34..39** ✗ |
| target protocol | 38..41 | 38..41 ✓ |

Consequences, per frame:

- `out[32..33]` **never written** — uninitialised stack goes on air
- the requester MAC lands two bytes late, truncated to four bytes by the next write
- `out[38..39]` written twice; the target protocol address wins, so *that* field is correct

Every proxy-ARP reply the gate sent to an AP client was malformed.

## Why it passed everything

lwIP's `etharp_input` decides *"is this for us"* from the target **protocol** address — correct here —
and populates its cache from the **sender** hardware/protocol pair — also correct. **Nothing on the
receive path reads the field that was wrong.**

So the bridge resolved, pings replied, and the T2 gate tier passed for weeks. This is the exact
failure class that log-level evidence cannot see: *a malformed field that no receiver happens to read*.

## What actually exposed it

**The file disagreed with itself.** The mesh-side SNAP builder in the same function pair writes the
same field at ARP-relative `+18`, which is right; the Ethernet builder writes it at ARP-relative `+20`.
Two builders of one structure, off by two. That contradiction is cheaper to notice than the offset
itself is to re-derive.

## The second finding, in the other direction

The example had **already fixed something Rimba still shipped**: the proxy-ARP table is guarded there
and was not here. Rimba's `g_gw_arp` has three concurrent accessors — the mesh RX task and the AP RX
task both snoop into it, the announce task ages and reads it — with no lock. Worse, the announce loop
read entries **live across its TX calls**, so a slot could be evicted and refilled mid-push and teach a
host an IP/MAC pair that never coexisted.

Backported: every access guarded, and the push snapshots under the lock and transmits afterwards.

## The locking cost real delivery, and the guess about it was wrong

I wrote that the added per-frame critical section was "negligible against a 1 MHz HaLow frame time".
**It was not.** `taskENTER_CRITICAL` disables interrupts, and it was on the per-frame path scanning up
to 48 slots. T2 `mesh-gate-b2` measured it:

| build | replies | missing | median RTT |
|---|---|---|---|
| offset fix + **portMUX critical sections** | 28/30, then **28/30** | seq 1,2 / seq 1,22 | 46 ms |
| offset fix **only**, no locking | **30/30** | — | 43 ms |
| offset fix + **mutex** | **30/30** | — | 43 ms |

**I nearly filed the two 28/30s as bench variance.** They ran at 00:40 against a 19:50 baseline and the
median RTT had risen 33 → 46 ms, which is a real confound — the documented RX-overload variance would
explain exactly that shape. Building the no-locking control and running it in the same hour is what
settled it: same conditions, full delivery. The environment was not the cause.

**The fix is to stop disabling interrupts at all.** Every accessor is a task — both RX callbacks are
dispatched from the one UMAC event loop (`umac_datapath.c:1021-1035`, a single dispatch site for both
vifs), `forget_mac` runs on the AP status task, the push on its own — so no ISR is involved and a mutex
is legal. It costs a take/give and blocks nothing at the driver level. The lock is still never held
across a TX, which would block the datapath for the whole O(n²) sweep.

## What this run of the loop is worth recording

Three of tonight's conclusions came from **building the control instead of reasoning about it**: the
offset bug (found by noticing one file contradicting itself, not by re-deriving the layout), the
locking regression, and the mutex being sufficient. The cheap version of each — "the offsets look
right", "28/30 is variance", "a few µs cannot matter" — was wrong every time, and each was about
fifteen minutes of bench to disprove.

⚠ The **same critical-section pattern** was carried into `esp-halow-examples` earlier the same night and
approved there. Its table is smaller and its testing is 15 pings, so the cost would likely have been
invisible — a latent version of a defect already measured elsewhere. Converted there too rather than
left in the code people copy.
