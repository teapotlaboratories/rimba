# Mesh `hw_restart` recovery — what actually happens, and what has to be built

**Status:** **S1 done (2026-08-05)** — the reproducer exists and is red on air; S2 (mesh recovery) and
S3 (the AP assert) are open. Blocks the relay Interrupt-WDT fix (FIX-1). Read the S1 sections bottom-up:
the last one supersedes the two above it, which are kept for the dead ends.

## The failure, traced in source

`umac_mmdrv_shim.c:71` `hw_restart_evt_handler()` is the single recovery point. It runs:

```c
if (umac_interface_get_vif_id(umacd, UMAC_INTERFACE_AP) != MMDRV_VIF_ID_INVALID)
{
    MMLOG_ERR("Unable to recover from hardware restart with AP interface active\n");
    MMOSAL_ASSERT(false);                 /* <-- the gate dies here */
}
if (umac_interface_is_active(umacd))
{
    mmdrv_deinit();
    MMOSAL_ASSERT(mmdrv_init(NULL, country_code) == MMWLAN_SUCCESS);   /* every vif is now GONE */
    umac_health_check_start(umacd);
    umac_stats_increment_hw_restart_counter(umacd);
    umac_scan_handle_hw_restarted(umacd);
    umac_connection_handle_hw_restarted(umacd);
}
```

**Two distinct defects, not one.**

### (a) A mesh node comes back deaf, silently

Mesh has its **own** interface type — `UMAC_INTERFACE_MESH = 32` (`umac_interface.h:35`), installed by
`umac_interface_add(umacd, UMAC_INTERFACE_MESH, …)` at `umac_mesh.c:3766`. It does **not** occupy the STA
slot at this layer.

> ⚠ Do not confuse this with the gate app's `MMWLAN_VIF_STA` comment. That is the *host datapath vif tag*
> used for RX demux — a different abstraction from `UMAC_INTERFACE_*`. Reading the app comment and
> concluding the mesh vif is a STA vif here is wrong, and it inverts the conclusion below.

So for a mesh node the sequence is: `mmdrv_deinit()`/`mmdrv_init()` wipes every vif, then
`umac_connection_handle_hw_restarted()` opens with
`umac_interface_get_vif_id(umacd, UMAC_INTERFACE_STA)`, which is **`MMDRV_VIF_ID_INVALID`** — so its
entire body is skipped. **Nothing is restored.** The chip is re-initialised, the mesh vif is never
reinstalled, no peer is re-registered, no key is reinstalled. The host stack still believes it is
meshing. Nothing is on air.

That is precisely the bench-observed "silently deaf", now with a mechanism rather than a symptom.

### (b) The shipped gate asserts outright

`rimba-halow-mesh-ap` runs mesh **plus** an AP vif, so `UMAC_INTERFACE_AP` is valid and the handler hits
`MMOSAL_ASSERT(false)` before doing anything. The gate does not go deaf — it panics. **This is arguably
the more urgent of the two**, because the gate is the node with clients depending on it.

## The template already exists

`umac_connection_handle_hw_restarted()` (`umac_connection.c:1689`) is the model, and it is five steps:

| # | step | STA | mesh equivalent |
|---|---|---|---|
| 1 | `umac_interface_reinstall_vif()` | the STA vif | the MESH vif |
| 2 | `mmdrv_update_sta_state(…, MORSE_STA_NONE)` | one BSSID | **loop over every established peer** |
| 3 | `umac_interface_reconfigure_channel()` | once | once |
| 4 | `umac_keys_reinstall_keys(stad, vif_id)` | one peer's keys | **per-peer AMPE keys** |
| 5 | `mmdrv_update_sta_state(…, MORSE_STA_AUTHORIZED)` | if FSM connected | per peer, if the plink is ESTAB |

**The one structural difference is N peers instead of one BSSID.** Everything else maps directly, which
makes this a smaller job than the backlog entry implies — the plumbing convention
(`*_handle_hw_restarted(umacd)`) is already uniform across scan / connection / ps / datapath.

## Staging

- **S1 — a reproducer first.** There is no way to trigger a chip restart on demand; the 2026-07-15
  finding came from an actual crash. A fixture calling `mmdrv_host_hw_restart_required()` on a serial
  command turns this from "wait for a fault" into a repeatable T2 test. **Do this before S2** — without
  it neither fix is verifiable.
- **S2 — `umac_mesh_handle_hw_restarted()`**, derived from `net/mac80211`'s `ieee80211_reconfig` mesh
  path per the porting rule, shipping the function-level code-map that rule requires.
- **S3 — the AP assert**, which is what actually unblocks the gate.

Submodule work: morselib PR first, then the rimba gitlink bump (⚠ the rebase-SHA repoint has bitten
twice now — check `git merge-base --is-ancestor` before merging the superproject).

## Not yet checked

- What `ieee80211_reconfig` does for a mesh vif in the Linux reference — the port must derive from it.
- Whether `umac_interface_reinstall_vif()` is safe to call for `UMAC_INTERFACE_MESH` as-is.
- Whether a *concurrent* mesh+AP node can be recovered at all, or whether S3 is a vendor/firmware ask.

---

## S1 status — fixture written, blocked on a link error (resume here)

`firmware/test-mesh-hwrestart/` exists and compiles; it **fails to link**:

```
undefined reference to `mmdrv_host_hw_restart_required'
```

**This is not a missing symbol.** The defining TU is compiled into the build and the symbol is present
and global:

```
$ xtensa-esp32s3-elf-nm .../umac_mmdrv_shim.c.obj | grep hw_restart
00000000 t hw_restart_evt_handler
00000000 T mmdrv_host_hw_restart_required      <-- defined, global
         U mmdrv_hw_restart_completed
```

So it is a **link-visibility problem, not an availability one**. The likely cause is that morselib is a
PRIVATE dependency of the `halow` component, so an app declaring `REQUIRES halow` gets the headers but
its reference cannot resolve against the nested archive. Next step is to inspect how `components/halow`
links `libmorse` and whether any existing app calls a `mmdrv_*` symbol directly (if none does, that is
the answer — no app ever has).

**Do not "fix" this by widening morselib's public link surface without checking.** `mmdrv.h` lives under
`morselib/src/internal/` and reaching into it from an app is already a layering exception; the right
answer may instead be a thin `mmwlan_*` test hook in morselib, which would also make the trigger
available to the submodule's own tests rather than only to this fixture.

### The rest of S1, once it links

- Rig: this app on one board, any mesh node on the same MESH_ID as the peer
  (`test-mesh-gate-node NO_PING=1` is the cheapest responder).
- Scored on `mmwlan_mesh_peer_count()` before vs after, deliberately **not** on ping — "silently deaf"
  is precisely the state where the host stack looks healthy, and a reachability probe would add the
  documented "source with no IP fails ping with an empty mpath" failure mode on top.
- **Expected verdict today is FAIL**, and that is the deliverable: it converts the 2026-07-15 crash
  observation into a repeatable red test that S2 then turns green.

---

## S1 result — the hook links, but the FIXTURE'S METRIC IS INVALID

**The link problem is solved.** Rather than let an app reach into `morselib/src/internal/mmdrv.h`, a
public `mmwlan_force_hw_restart()` was added (`mmwlan.h`, implemented in `umac_mmdrv_shim.c` beside the
handler it drives). Documented DIAGNOSTIC ONLY, returns `MMWLAN_UNAVAILABLE` when the subsystem is idle.
Widening the internal header's link surface would have made every internal `mmdrv` symbol app-callable
to suit one fixture. No regression: `rimba-halow-mesh-ap` still builds.

**The first run returned PASS, and the PASS is worthless.**

```
TEST|STEP|peer-before|PASS|estab_peers=1
TEST|INFO|forcing a chip restart via mmwlan_force_hw_restart() with 1 peer(s) established
TEST|INFO|  +10s ... +40s since restart: estab_peers=1     <-- never dropped, not even once
TEST|STEP|peer-after|PASS|estab_peers=1 (was 1) after 45s
```

`mmwlan_mesh_peer_count()` (`umac_mesh.c:1404`) walks `mesh_peers[]`, a **HOST-SIDE array**.
`mmdrv_deinit()`/`mmdrv_init()` tears down the CHIP and never touches it. **The count reads 1 whether the
node is meshing or stone deaf.**

⚠ **This is the fixture certifying a broken node as healthy** — strictly worse than the red test it was
meant to be. And it is the exact trap the fixture's own header comment warns about: "silently deaf is
precisely the state where the host stack looks healthy". The metric was chosen to dodge the
ping/empty-mpath trap and landed in a worse one.

**Two indistinguishable hypotheses, both consistent with the output:**
1. The restart ran; the host peer table is merely stale (metric wrong).
2. `mmwlan_force_hw_restart()` returned `SUCCESS` but the queued event never executed (hook wrong).

### Resume here — separate them first, before touching anything else

- **Read `umac_stats`' hw-restart counter** (`umac_stats_increment_hw_restart_counter`, called by the
  handler). Non-zero proves the handler ran and kills hypothesis 2. Do this first; it is decisive and cheap.
- **Then replace the metric with something that reads the AIR, not the host.** Candidates: a chronium
  `morse0` capture showing the node's mesh beacons stopping and not resuming; or peer-side observation
  (the *other* board reporting its peer count drop), which is host-side but on a node that did not restart
  and therefore reflects reality.
- Only once the probe is trustworthy is a FAIL meaningful as the S2 target.

---

## S1 DONE — the metric now reads the air, and the red test is red (2026-08-05)

Both questions above are answered, on the bench, with the two-board rig (board1 = NUT, board0 =
`test-mesh-gate-node NO_PING=1`) plus chronium on `morse0`.

### 1. The trigger works — hypothesis 2 is dead

```
TEST|INFO|hw_restart_counter=0 before the trigger
TEST|STEP|restart-ran|PASS|hw_restart_counter 0 -> 1
```

`mmwlan_force_hw_restart()` really does drive `hw_restart_evt_handler()` all the way through
`mmdrv_deinit()` + `mmdrv_init()` — the counter is bumped *after* both. **The hook is correct; only the
metric was wrong.** Hypothesis 1 stands: the host peer table was merely stale.

### 2. The air says the node is stone deaf

`tools/mesh_hwrestart_cap.py` (new) scores the NUT's own S1G beacons off chronium's `morse0`:

```
captured 150s: nut=317 beacons, peer(control)=1367 beacons
nut beacon window: first=+0.0s last=+37.9s (trailing silence 112.1s)
control rate: 9.1/s before the node went quiet, 9.1/s during its silence
RESULT|FAIL|the node's beacons stopped at +37.9s and NEVER returned ...
```

Beacons stop ~5 s after the trigger and never come back, **while the host reported `estab_peers=1` for
the entire 45 s window**. That contrast is the whole finding: defect (a) is now confirmed on air, not
inferred from source.

**The liveness control is what makes this a measurement.** The peer board is unaffected by the restart
and beacons throughout; its steady 9.1/s proves the monitor still had the channel, so zero NUT beacons
is *silence* and not a capture that quietly died. Without the control, "no beacons" would be
unfalsifiable — the same absence-of-evidence mistake in a new costume.

### How the fixture is now split

The firmware no longer scores recovery at all, because on this defect **no on-device probe can**:
"silently deaf" is by definition the state in which every host-side view still looks healthy. Any
replacement metric that reads RAM on the NUT walks into the same trap.

| | scores | verdict |
|---|---|---|
| `test-mesh-hwrestart` (firmware) | did the trigger really restart the chip (`hw_restart_counter`) | **PASS** — the fixture works |
| `tools/mesh_hwrestart_cap.py` (chronium) | do the NUT's beacons ever return | **FAIL** — the S2 defect |

This is the `test-raw-rps` shape: the `TEST|` gates are preconditions, and the real verdict is decoded
from a capture. `mmwlan_mesh_peer_count()` is still printed — demoted to INFO, labelled unscored —
precisely so the stale number stays visible next to the on-air truth.

**The scorer's PASS branch is not dead code.** It was validated the same day by rebooting a plain mesh
node mid-capture (beacons → 4.9 s gap → 469 beacons): `RESULT|PASS`. So the tool discriminates, and the
FAIL above is a finding rather than a scorer stuck on one answer.

### S1 is now the red test S2 has to turn green

Remaining caveat: the AP-side assert (defect (b)) is still unreproduced — this fixture is mesh-only, so
it never instantiates an AP vif and never reaches `MMOSAL_ASSERT(false)`. S3 needs its own reproducer,
or an extension of this one onto `rimba-halow-mesh-ap`.
