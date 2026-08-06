# Mesh hw_restart S1 — the fixture that passed a deaf node, and the metric that fixed it

**Date:** 2026-08-05
**Task:** unblock S1 of the mesh `hw_restart` work. The `test-mesh-hwrestart` fixture returned **PASS**
on a node that was probably deaf, because it scored recovery on a host-side array.
**Outcome:** S1 done. The trigger is proven correct, the recovery verdict moved off-board onto a
chronium `morse0` capture, and the fixture is now **red on air** — which is what S1 was always supposed
to deliver. Defect (a) is confirmed on air rather than inferred from source.

---

## The two hypotheses, and which one died

The previous session left the fixture reporting `estab_peers=1` before *and* after a forced chip
restart, with two readings that the output could not separate:

1. the restart ran and the host peer table is merely stale (**metric** wrong), or
2. `mmwlan_force_hw_restart()` returned `SUCCESS` but the queued event never executed (**hook** wrong).

The cheap decisive probe was the one already sitting in the handler: `umac_stats`' hw-restart counter,
bumped by `hw_restart_evt_handler()` (`umac_mmdrv_shim.c:71`) **after** `mmdrv_deinit()` and
`mmdrv_init()` have both run. Reachable from an app through the public `mmwlan_get_umac_stats()`
(`mmwlan.h:3052`) → `struct mmwlan_stats_umac_data.hw_restart_counter` (`mmwlan_stats.h:67`), so no new
morselib surface was needed for it.

```
TEST|INFO|hw_restart_counter=0 before the trigger
TEST|STEP|restart-ran|PASS|hw_restart_counter 0 -> 1
```

**Hypothesis 2 is dead.** The hook added last session is correct: the handler really does execute
through the teardown and re-init. Everything wrong was in the metric.

That ordering was worth keeping. Had I gone straight to building the on-air probe, a red result would
have been ambiguous between "the mesh cannot recover" and "my trigger never fired", and I would have
had to come back and run this anyway — with a much more expensive experiment already built on top of it.

---

## Why no on-device probe can score this

`mmwlan_mesh_peer_count()` (`umac_mesh.c:1404`) walks `mesh_peers[]`, a **host-side array**.
`mmdrv_deinit()`/`mmdrv_init()` tears down the chip and never touches it, so it reads 1 whether the node
is meshing or transmitting nothing at all. Measured this session: it held at 1 for the full 45 s window
while the monitor saw the board emit **zero frames**.

The trap is structural rather than an oversight, and that is the part worth carrying forward:

> **"Silently deaf" is by definition the state in which every host-side view still looks healthy.**

So any replacement metric that reads RAM on the node under test falls into it again. The first version
picked the peer table to dodge the documented ping/empty-mpath trap and landed somewhere strictly worse
— a fixture that certifies a broken node as healthy is worse than no fixture, because it spends
credibility instead of building it.

---

## The split: firmware asserts the trigger, the air asserts the recovery

| | scores | verdict |
|---|---|---|
| `test-mesh-hwrestart` (firmware) | did the trigger really restart the chip (`hw_restart_counter`) | **PASS** — the fixture works |
| `tools/mesh_hwrestart_cap.py` (chronium) | do the node's own beacons ever return | **FAIL** — the S2 defect |

This is not a new pattern in the repo — it is the `test-raw-rps` shape, whose `TEST|` gates are also
preconditions with the real verdict decoded from a capture (`manifest.py:383`). The firmware still
prints `mmwlan_mesh_peer_count()`, demoted to INFO and explicitly labelled unscored, precisely so the
stale number stays visible next to the on-air truth. That contrast *is* the finding; deleting it would
hide the lesson.

The fixture also now beacons for a 20 s settle window after peering before triggering. In the first run
peering completed **0.4 s** after the node's first beacon went out, leaving an eight-beacon baseline —
too thin to distinguish "went silent" from "never really started".

---

## The measurement

`tools/mesh_hwrestart_cap.py` reads `morse0` over `AF_PACKET` (same shape as the existing
`mesh_beacon_cap.py` / `mesh_rann_cap.py`), filters S1G beacons by SA, and scores the shape
*beacons → gap → beacons?* — so it needs no clock sync with the DUT.

Rig: board1 = NUT, board0 = `test-mesh-gate-node NO_PING=1`, chronium in monitor type on ch27
(freq 5560). No board2, so no PPK2.

```
captured 150s: nut=317 beacons, peer(control)=1367 beacons
nut beacon window: first=+0.0s last=+37.9s  (trailing silence 112.1s)
control rate: 9.1/s before the node went quiet, 9.1/s during its silence
RESULT|FAIL|the node's beacons stopped at +37.9s and NEVER returned ...
```

Beacons stop ~5 s after the trigger and never return, while the host reported `estab_peers=1`
throughout. **Defect (a) confirmed on air.**

### The liveness control is what makes this a measurement

The peer board is unaffected by the restart and beacons throughout. Its rate holding at **9.1/s across
the node's silence** is what proves the monitor still had the channel — so zero NUT beacons is *silence*
and not a capture that quietly died. Without the control, "no beacons" would be unfalsifiable: the same
absence-of-evidence mistake that produced the original bad PASS, just wearing a different costume. The
tool reports INCONCLUSIVE rather than FAIL when the control drops, for exactly that reason.

### The PASS branch is not dead code

A scorer that has only ever emitted FAIL is not yet a scorer. Validated by rebooting a plain mesh node
mid-capture and pointing the tool at it:

```
nut beacon window: first=+0.0s last=+75.0s  (trailing silence 0.0s)
  gap 4.9s  (+20.0s -> +24.9s)
RESULT|PASS|the node's beacons stopped for 4.9s and RESUMED at +24.9s (469 beacons since)
```

Both branches now fire on real air, so the FAIL above is a finding rather than a tool stuck on one
answer.

---

## Dead ends and bench notes

- **First run of the day was lost.** I piped the serial capture through `grep`, and the capture script
  died on `ModuleNotFoundError: No module named 'serial'` — the traceback went into the same grep and
  vanished. pyserial lives in the IDF venv
  (`~/.espressif/python_env/idf5.4_py3.13_env/bin/python`), not system python3. The app parks forever
  after `TEST|END`, so the verdict was unrecoverable and the run had to be repeated. Same family as the
  documented "never pipe a tier through `tail`" footgun.
- **`morse0` will not come up until `wlan1` is up and tuned.** `tcpdump` failed with *"That device is
  not up"* on the first attempt: the driver only forwards to `morse0` while `mors->monitor_mode` is
  true, which needs a live monitor-type vif. Order is `ip link set wlan1 up` → `iw dev wlan1 set freq
  5560` → `ip link set morse0 up`, and the setup must be re-verified with `ip -br link` rather than
  assumed from the absence of an error.
- **tcpdump cannot decode S1G beacons** — they print as *"unknown 802.11 frame type (3)"*, because an
  S1G beacon is a type-3 (extension) frame. Parse the bytes: SA is at frame offset `[4:10]`,
  `fc == 0x1c`. Nothing here needed Wireshark.

---

## State

S1 is **done and red**, which is the deliverable — it converts the 2026-07-15 crash observation into a
repeatable test that S2 has to turn green.

Still open, unchanged by this session:

- **S2** — `umac_mesh_handle_hw_restarted()`, derived from `ieee80211_reconfig`'s mesh path, shipping
  the code-map the porting rule requires.
- **S3** — the AP-side `MMOSAL_ASSERT(false)`, still **unreproduced**. This fixture is mesh-only, so it
  never instantiates an AP vif and never reaches the assert. S3 needs its own reproducer, or this one
  extended onto `rimba-halow-mesh-ap`.

Everything is uncommitted (weekday work-hours hold): the design doc, `firmware/test-mesh-hwrestart/`,
`tools/mesh_hwrestart_cap.py`, this worklog, and `components/halow` on submodule branch
`feat/mesh-hw-restart`.
