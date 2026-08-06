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


---

## On-air verification (the `.ai` requirement)

The fix corrects a **frame format**, and the field it corrects is one no receiver reads — so delivery
evidence cannot confirm it. That is precisely the case the on-air rule exists for, and it is the same
argument I had just written into the example's README hours earlier. So it was captured.

**Rig.** Gate = board2 (`test-mesh-gate-ap`), silent static client = board0 at 10.9.9.50, mesh node =
board1 at 10.9.9.100, sniffer = chronium `morse0` on S1G ch27 (5560 MHz). 3,491 frames, 120 s.

⚠ **The AP was run OPEN for this capture** (`AP_OPEN=1`, a new compile-gated fixture mode). The shipped
AP is SAE/PMF, so its data frames are CCMP-encrypted and a monitor capture cannot decode the payload.
The ARP under test lives in the SNAP payload and is **not** affected by the link cipher, so dropping
encryption exposes the bytes without changing them. This is a capture-only build and says so on the
console at boot.

**Result — 9 proxy-ARP replies from the gate's AP BSSID `be:2a:33:96:b2:33`:**

| field | on air | expected |
|---|---|---|
| oper | 2 (REPLY) | ✓ |
| sha / spa | `e2:72:a1:f8:f9:40` / 10.9.9.100 | the announced mesh node ✓ |
| **tha** | **`68:24:99:44:6b:b7`** | **the requester's full MAC, at payload offset 18** ✓ |
| tpa | 10.9.9.50 | the requester's IP ✓ |

**All nine are byte-identical**, which is itself corroboration: under the defect `out[32..33]` were
uninitialised stack and would very likely have varied frame to frame.

### Tier reached, stated per the rule

- **`matches source layout` — REACHED.** Every field sits where RFC 826 and `net/ipv4/arp.c` put it,
  verified byte-by-byte off air rather than by reading the source.
- **`matches live Linux device` — REACHED, in a second capture.** chronite was associated to the open
  gate AP as a Linux STA at 10.9.9.50 (`docs/reference/captures/wpa-sta-open.conf`, now tracked) and made
  to transmit real ARP replies with `arping -A` / `-U`. **Both stacks' replies are in the same capture,
  on the same channel, within seconds of each other:**

  | field | off | ESP (gate proxy reply) | Linux (chronite) | |
  |---|---|---|---|---|
  | htype | 0 | `00 01` | `00 01` | identical |
  | ptype | 2 | `08 00` | `08 00` | identical |
  | hlen / plen | 4 | `06 04` | `06 04` | identical |
  | oper | 6 | `00 02` | `00 02` | identical |
  | sha | 8 | `e2 72 a1 f8 f9 40` | `3c 22 7f 37 51 38` | *names a different host — correct* |
  | spa | 14 | `0a 09 09 64` | `0a 09 09 32` | *names a different host — correct* |
  | **tha** | **18** | **`3c 22 7f 37 51 38`** | **`3c 22 7f 37 51 38`** | **identical** |
  | tpa | 24 | `0a 09 09 32` | `0a 09 09 32` | identical |

  All eight structural bytes byte-identical; every address field at the same offset and width. The only
  two that differ are `sha`/`spa`, and they differ *because they name different hosts* — the gate
  announces the mesh node, chronite announces itself. **That is the gold standard met.**

  Independent corroboration from the same run: chronite's own neighbour table showed
  `10.9.9.100 lladdr e2:72:a1:f8:f9:40 REACHABLE`, i.e. a real Linux stack accepted the gate's reply
  and installed it.

### Getting a Linux ARP reply on air was the hard part

Two dead ends worth recording. **The proactive push defeats the natural path** — the mesh node is taught
about the client before it ever needs to broadcast, so the client is never asked and never replies. The
feature under test suppresses the very frame the reference needed. And **chronosalt has no HaLow netdev**
(`morse`/`dot11ah` loaded, no `wlan1`), so a second Linux client was not available. `arping -A`/`-U` from
the associated node solves it directly: those transmit genuine `oper=2` replies on demand.

### One fixture bug found on the way

`MMWLAN_OPEN` alone was not enough. The fixture sets `passphrase`/`passphrase_len` before the security
branch, and with those non-empty **the AP still advertises the Privacy capability bit** — a scanning
Linux STA sees `[WEP][ESS]` and refuses to associate with `key_mgmt=NONE`, which reads exactly like a
wrong-passphrase failure. The open branch now clears both.

Capture archived at `docs/worklog/artifacts/gate-arp/2026-08-04-proxy-arp-postfix.pcap`.
Bench left radio-silent: all three boards on `rimba-hello`, chronium's `wlan1`/`morse0` down, PPK2
hold released and board2 dark.

---

## Review follow-ups (2026-08-05, `/review 53`)

The review confirmed both headline fixes independently — the ARP offsets re-derived from scratch
(Ethernet header 14 + `tha` at ARP-payload offset 18 = absolute 32, `tpa` at 38, and the function now
writes all 42 bytes with no gap), both apps carrying byte-identical changes, and the locking reasoning
holding up: no accessor is an ISR, nothing nests so there is no deadlock, and `gw_arp_forget_mac()` is
called *after* `taskEXIT_CRITICAL` — taking a mutex inside a `portMUX` critical section would have been
fatal rather than merely slow. It also re-decoded both archived pcaps and reproduced the byte-diff above.

Three fixes came out of it. None is a correctness defect in the shipped datapath; all three are cases
where a failure would have been silent.

### 1. An allocation failure silently restored the unguarded table

`g_gw_arp_lock = xSemaphoreCreateMutex()` was unchecked, and the accessors were written
`if (g_gw_arp_lock) xSemaphoreTake(...)`. So if the handle ever failed to allocate, every access would
quietly proceed **unlocked** — reverting to precisely the race this work exists to remove, with no
diagnostic and no crash. The NULL guard was doing the opposite of its apparent job: it converted a loud
failure into an invisible one. Now `configASSERT`ed at creation and the guards are gone.

### 2. The lock was created after an accessor that could already run

It was created next to the announce task, justified by a comment about the RX callbacks — which was
true as far as it went. But `mmwlan_ap_enable()` arms `ap_sta_status_cb` several statements earlier,
and that path reaches `gw_arp_forget_mac()`. A client leaving in that window would have walked the
table with no lock held.

Harmless in practice today, because the RX callbacks are not yet registered so the table is still
empty — but the placement was only *accidentally* safe, and the comment justified it against two of
the three accessors. Creation moved to the top of `app_main()`, before any radio is up, which makes it
unconditionally true and is what lets fix 1 drop the NULL guard.

### 3. `AP_OPEN=0` built an **open** AP

The Makefile forwards `-D TEST_AP_OPEN=$(AP_OPEN)`, and make treats `"0"` as a non-empty string, so
`AP_OPEN=0` still arrived defined; the fixture's CMake tested `if(DEFINED TEST_AP_OPEN)` and forced it
to 1. **The off switch turned security off.** Confirmed by A/B rather than argued from CMake semantics:

| build | result |
|---|---|
| `AP_OPEN=0`, `if(DEFINED TEST_AP_OPEN)` (before) | **OPEN — no security** |
| `AP_OPEN=0`, `if(TEST_AP_OPEN)` (after) | SAE/PMF |
| `AP_OPEN=1`, `if(TEST_AP_OPEN)` (after) | OPEN — no security |

This matches the existing `NO_PING` spelling, but the consequence differs in kind: the other `TEST_*`
flags skip a ping when misspelled, this one drops link encryption. Both fixture `CMakeLists.txt` now
test the value. The shipped gate was never exposed — `rimba-halow-mesh-ap/main/CMakeLists.txt` has no
`TEST_AP_OPEN` block at all, so the flag cannot reach it however it is spelled.

### 4. The branch was shipping a RED T0 test, and reading the diff never showed it

`make test-unit` **failed on the branch head** — `clone-mirror: test-mesh-gate-ap/main/app_main.c has
DRIFTED from its source`. The `#ifdef TEST_AP_OPEN` block had been added to the fixture only, and
`CLONE_MIRRORS` (`manifest.py:497`) requires `test-mesh-gate-ap/main/app_main.c` to stay byte-identical
to `rimba-halow-mesh-ap/main/app_main.c` below the banner.

**I missed this reading the diff and only found it by running the tier** — which is the lesson, not a
footnote. Three review passes over the same hunks did not surface it, because a per-file diff cannot
show a *cross-file* invariant. It cost one command. (Confirmed pre-existing by stashing this session's
edits and re-running: the failure reproduces on the untouched branch head, and the reported drift is
exactly the `TEST_AP_OPEN` hunk.)

The invariant is not incidental — it is the guard that stops the shipped gate and its fixture from
drifting apart, which is *precisely* how one malformed proxy-ARP reply came to live in three copies at
once. Silencing it by dropping the row is explicitly anticipated and tested against
(`test_allowlist_covers_mesh_gate`, "guards against someone silencing the check by deleting the row").

**Fix: the block now exists identically in both files**, which is what the mirror demands, and only
`main/app_main.c` is mirrored — so `CMakeLists.txt` remains free to differ, and that is where the real
containment lives. In the shipped gate the branch is **unreachable**: nothing defines `TEST_AP_OPEN` for
that target, so the compiler takes the SAE/PMF `#else` unconditionally.

That absence is now the single thing keeping the shipped gate secure, so it is documented as a tripwire
in `rimba-halow-mesh-ap/main/CMakeLists.txt` rather than left as a silent gap someone later "completes".
Verified directly rather than assumed:

| build | result |
|---|---|
| `rimba-halow-mesh-ap` with `AP_OPEN=1` | **SAE/PMF — the flag cannot reach it** |

### Verification

`make test-unit` **53/53 OK** (was 52/53 with the clone-mirror failure).
`rimba-halow-mesh-ap`, `test-mesh-gate-ap` and `test-mesh-gate-sta` all build clean from a removed
build directory (the CMakeCache stickiness footgun makes an incremental build untrustworthy for a
`CMakeLists.txt` change). The two `AP_OPEN` A/Bs above are the direct tests of fixes 3 and 4.

⚠ **Fixes 1 and 2 are not bench-verified.** They are init-ordering and assertion changes on the shipped
gate's startup path, and re-running T2 `mesh-gate-b2` needs the full three-board rig with board2 on the
PPK2 hold, which was not set up for this session. The behaviour they change is unreachable on a healthy
boot by construction — an allocation that does not fail, and a window in which the table is empty — so
neither is expected to move the tier. **That expectation is an argument, not a measurement**; run
`mesh-gate-b2` before merging if the gate's startup path is considered load-bearing enough to warrant it.

### Two things left as-is, deliberately

- **A side change on refresh does not wake the announce task.** `gw_arp_learn()` returns early when it
  finds an existing entry for the IP, so a host that moves between the AP and mesh sides has its mapping
  updated but no push until the next period. Pre-existing — the code before this work also returned
  early — but the 3 s → 15 s period change made the wait 5× longer. Out of scope here; worth a tracked
  TODO rather than a silent fix inside an unrelated PR.
- **The snapshot can go stale mid-sweep.** A client that leaves after the snapshot is taken still gets
  pushed for the remainder of that sweep. That is the deliberate cost of never holding the lock across a
  TX, and it is bounded by one period.

### One more convention gap, closed

This worklog had no HTML render and no index entry. Both now exist
(`docs/worklog/html/2026-08-04-gate-arp-reply-and-table-lock.html`), which is the standing requirement
for any added worklog.
