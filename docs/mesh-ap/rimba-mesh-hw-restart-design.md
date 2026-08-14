# Mesh `hw_restart` recovery — what actually happens, and what has to be built

**Status:** **S1 + S2 done and merged (2026-08-05/06)** — mesh now survives a chip restart on both
probes (datapath 5/5, beacons 1568/190 s), post-code-review. **S3 REPRODUCED 2026-08-09 and is TWO
defects** (the assert, plus a boot-time ISR watchdog no ESP reset clears); the fix is not written. Blocks the relay Interrupt-WDT fix (FIX-1). Read the S1 sections bottom-up: the last one
supersedes the two above it, which are kept for the dead ends. The S2 code map is at the end.

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
`umac_connection_handle_hw_restarted()` runs — and restores **none of the mesh-specific state**. No
mesh BSS config, no `set_bssid`, no `MESH_CONFIG`, no beaconing, no peer re-registered, no mesh key
reinstalled. The host stack still believes it is meshing. Nothing is on air.

That is precisely the bench-observed "silently deaf", now with a mechanism rather than a symptom.

> ⚠ **CORRECTION (2026-08-06, adversarial review).** Every earlier revision of this section said the
> connection handler's *"entire body is skipped"* because
> `umac_interface_get_vif_id(umacd, UMAC_INTERFACE_STA)` returns `MMDRV_VIF_ID_INVALID` on a mesh node.
> **That is false, and it was repeated for the whole investigation.** `UMAC_INTERFACE_STA` is inside
> `VIF_STA_INTERFACE_TYPES_MASK` (`umac_interface.c:118-120`), so `umac_interface_get_vif_id()` returns
> `vif_data_sta->vif_id` **without ever consulting `active_interface_types`**
> (`umac_interface.c:492`) — and mesh *shares the STA slot*. The guard is therefore **true** on a
> mesh node and the body **does** run. Confirmed on hardware with a temporary probe: `BODY RUNNING
> vif_id=0` on a meshing node.
>
> The *conclusion* survives — a mesh node comes back deaf and needs its own restore — but the mechanism
> is "the handler restores STA things, none of which are the mesh's" rather than "the handler does
> nothing". The difference is not cosmetic: it means the two handlers were both running, which is what
> made the ordering hazard in D11 possible.

### (b) The shipped gate asserts outright

`rimba-halow-mesh-ap` runs mesh **plus** an AP vif, so `UMAC_INTERFACE_AP` is valid and the handler hits
`MMOSAL_ASSERT(false)` before doing anything. The gate does not go deaf — it panics. **This is arguably
the more urgent of the two**, because the gate is the node with clients depending on it.

#### REPRODUCED 2026-08-09 — and it is worse than "it panics"

`test-mesh-hwrestart AP_VIF=1` brings an AP vif up alongside the mesh (same SSID/PSK/channel as the
shipped gate) and then triggers. The console shows the assert's own log line and then the board dies:

```
TEST|INFO|s3-expect-panic|an AP vif is active, so the handler should assert before recovering
TEST|INFO|restart-trigger|forcing a chip restart ... with 1 peer(s) established
E 36390 ev hw_restart_evt_handler[77] Unable to recover from hardware restart with AP interface active
rst:0xc (RTC_SW_CPU_RST),boot:0x8 (SPI_FAST_FLASH_BOOT)
Guru Meditation Error: Core  0 panic'ed (Interrupt wdt timeout on CPU0).
rst:0xc (RTC_SW_CPU_RST) ...        <-- and again, ~1.5 s apart, indefinitely
```

Three findings beyond "the assert fires":

1. **The assert itself fails CLEANLY.** `MMOSAL Assert` banner, backtrace, `rst:0xc` software reset. No
   watchdog is involved at this point.
2. **The reset can leave the node in a boot-time ISR watchdog loop.** When it happens, every boot dies
   ~0.4 s into radio bring-up, in ISR context, immediately after the first chip GPIO is configured —
   before mesh, before AP, before any trigger — and does not clear (30+ and 26 consecutive cycles
   observed in two separate runs).

   ⚠ **It is INTERMITTENT.** A third run, on the same binary and rig, recovered cleanly through
   repeated assert cycles: after each assert the board rebooted, brought mesh and AP back up, re-peered
   and passed `data-before` 4/5, then asserted again on the next trigger. So "every boot after the
   reset dies" is **wrong** — an earlier revision said that and is corrected here. What is established
   is that the loop is reachable and self-sustaining once entered, not that it always follows.

   **The control that makes this readable:** the same binary boots fine normally, and the loop
   reproduces with the fixture's TX-vif configuration *fixed* — so it is neither a property of the
   build nor an artifact of the earlier mis-configured arm.
3. **⛔ This Interrupt WDT is NOT the FIX-1 relay teardown**, and the two must not be merged on the
   strength of both saying "Interrupt wdt". The AP check is the **first statement** in
   `hw_restart_evt_handler()`, so `mmdrv_deinit()` — the SPI-host teardown behind FIX-1 — **never runs
   on this path**. Combined with (1), the assert path contains no watchdog at all. What the WDT belongs
   to is the boot that *follows* the reset.

   > An earlier revision of this section speculated the opposite ("plausibly the same teardown path…
   > S3 and FIX-1 may be one problem"). That was written before the question was chased and is
   > **withdrawn** — it would have scoped S3 wrongly.

### So S3 is TWO defects

| stage | what | consequence |
|---|---|---|
| 1 | AP vif active → assert → clean reset | the documented defect; fix = recover the AP arm, as Linux does |
| 2 | the reset **can** leave something wedged; later boots then fault in ISR during chip bring-up, indefinitely | intermittent, but once entered the node cannot reboot its way out |

Stage 1's fix **avoids** stage 2 by never rebooting, but does not fix it: stage 2 stays reachable by any
reset landing mid-restart, and is tracked separately.

**NOT established, and not claimed:** whether stage 2 is chip state or host state, and whether a power
cycle clears it. Only that an ESP **software** reset does not — 30+ cycles is the evidence. Answering the
power-cycle question needs board2 on the PPK2 rail; board1 is directly USB-powered. (An earlier revision
called this "permanently bricked until power-cycled" — withdrawn as untested.)

Rig note: the S3 arm keeps the full datapath gate, but only after one extra line.
`mmhalow_set_tx_vif(MMWLAN_VIF_STA)` is **required** once both a mesh (STA host-slot) and an AP vif are
valid — otherwise morselib cannot infer which vif an egress packet belongs to, logs
`Unable to infer VIF ID` and drops it, and the ping fails with the datapath dead.
`test-mesh-ap-gate:291` does the same for the same shape.

> An earlier revision of this arm mis-diagnosed that as "no per-vif RX demux" and *disabled* the
> datapath gate to work around it. RX was never the problem — `mmhalow` registers a plain rx cb and
> `umac_datapath` falls back to it for any vif. Worse, the workaround left the *after* ping still
> scoring, so once S3 is fixed and the board survives, the fixture would have reported **FAIL for the
> very fix it exists to certify**.

Build note: the AP arm overflowed the default `SINGLE_APP_LARGE` app partition by `0x1ab0`. The fixture
now carries its own 2 MB `partitions.csv` (precedent: `test-raw-rps`, `rimba-halow-ap-perf`), applied to
both arms so switching arms does not silently reflash a different layout.

⚠ The growth is **not** "the AP arm compiles the AP-mode sources in" — `CONFIG_HALOW_AP_MODE=y` was
already in this fixture's `sdkconfig.defaults` before the arm existed, so those sources were always
built. It is `--gc-sections`: referencing `mmwlan_ap_enable()` makes the linker **retain** the AP and
hostap subgraph it had previously discarded. Toggling `HALOW_AP_MODE` would not recover the flash.

## The template already exists

`umac_connection_handle_hw_restarted()` (`umac_connection.c:1689`) is the model, and it is five steps:

| # | step | STA | mesh equivalent |
|---|---|---|---|
| 1 | `umac_interface_reinstall_vif()` | the STA vif | the MESH vif |
| 2 | `mmdrv_update_sta_state(…, MORSE_STA_NONE)` | one BSSID | **loop over every established peer** |
| 3 | `umac_interface_reconfigure_channel()` | once | once — **and it must be this function, not `set_channel_from_regdb()`; see D12** |
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

- ~~What `ieee80211_reconfig` does for a mesh vif in the Linux reference~~ — **answered 2026-08-06, and
  it reframes S2. See below.**
- ~~Whether `umac_interface_reinstall_vif()` is safe to call for `UMAC_INTERFACE_MESH` as-is.~~ —
  **answered 2026-08-06: yes, and it is already mesh-aware. But it is nowhere near sufficient. See below.**
- Whether a *concurrent* mesh+AP node can be recovered at all, or whether S3 is a vendor/firmware ask.

### What Linux actually does — the restore is GENERIC, not per-vif-type

Read on chronium at `~/halow/rpi-linux/net/mac80211/util.c`, `ieee80211_reconfig()` (`util.c:1753`).
Offsets below are lines within the function body.

The **mesh case is almost empty** — it only re-enables the beacon:

```c
case NL80211_IFTYPE_MESH_POINT:                     /* body line 291 */
        if (sdata->vif.bss_conf.enable_beacon) {
                changed |= BSS_CHANGED_BEACON | BSS_CHANGED_BEACON_ENABLED;
                ieee80211_bss_info_change_notify(sdata, changed);
        }
        break;
```

It is small because everything the mesh actually needs restored is done **generically, for every vif
type, before the switch**:

| what | where (body line) | scope |
|---|---|---|
| reinstall the vif | `drv_add_interface()` — 101/113 | all types |
| re-add every station | `ieee80211_reconfig_stations()` — 213, 347 | all types |
| reinstall keys | `ieee80211_reenable_keys()` — 356 | all types |
| per-type extras | the `switch` — 291 for mesh | type-specific |

**This is the structural divergence, and it is the root cause.** morselib put the generic work — vif
reinstall → `mmdrv_update_sta_state` → channel → keys → `AUTHORIZED` — *inside*
`umac_connection_handle_hw_restarted()`, a **STA-shaped** function keyed on
`get_vif_id(umacd, UMAC_INTERFACE_STA)`. Linux keeps that work outside the type switch. So the generic
restore was written into a connection-specific function, and the mesh-specific half of it
(`cfg_bss` / `set_bssid` / `MESH_CONFIG` / beaconing / peers / mesh keys) has no home at all.

⚠ Note the nuance established by the correction in §(a): that key does **not** evaluate to
`MMDRV_VIF_ID_INVALID` on a mesh node, so the handler does run and does restore the vif and the
channel. What a mesh node gets is *STA* restoration — which is why it comes back with a live vif and
still cannot beacon. "A mesh node gets nothing" was the earlier, wrong shorthand.

### Consequences for staging

- **S2 is a hoist, not a mirror.** The faithful port is to lift the vif/station/key restore out of
  `umac_connection_handle_hw_restarted()` into a type-agnostic step that runs for whatever interface is
  active, then add a *small* mesh arm (re-enable beaconing). Writing a parallel
  `umac_mesh_handle_hw_restarted()` that duplicates the five steps would diverge from the reference and
  create a second copy of the same logic — the failure mode the T0 clone-mirror guard exists to prevent
  elsewhere in this repo.
- **The "N peers instead of one BSSID" framing in the table above is superseded.** Linux does not loop
  per-peer in the mesh path; `ieee80211_reconfig_stations()` already walks every station on the vif,
  which for a mesh vif *is* the peer set. The loop is generic, not mesh-specific.
- **S3 may fall out of the same hoist.** Linux handles AP in that same generic path plus a
  `drv_start_ap()` in the type switch — it does not refuse to recover an AP. That makes the
  `MMOSAL_ASSERT(false)` look like a morselib scoping decision rather than a hardware limitation, so S3
  should be re-examined *after* the hoist rather than planned as an independent stage. Not yet
  confirmed on the chip.

⚠ Line numbers above are **body-relative** (from `awk '/^int ieee80211_reconfig/,/^}/'`), not file
absolute — re-derive them against the tree before quoting them in the S2 code-map, which the porting
rule requires to cite verified `file:line` pairs on both sides.

### `umac_interface_reinstall_vif()` is mesh-safe — and not nearly enough

**Safe, and already mesh-aware.** `umac_interface.c:560` branches on the active type and maps
`UMAC_INTERFACE_MESH` → `MMDRV_INTERFACE_TYPE_MESH` before calling `mmdrv_add_if()`. Nothing about it is
STA-specific. It is simply never reached for mesh today, because its **only** call site is inside the
STA-gated `umac_connection_handle_hw_restarted()` (`umac_connection.c:1703`). The primitive needs no
change — only a caller.

**But it restores almost none of what a mesh vif needs**, because `umac_interface_init_vif()` — the
helper it finishes with — has arms for `SCAN`, `STA` and `AP` and **no `MESH` arm**. So a reinstall
brings back the vif plus the generic bits (PS mode, TX-status watermark, health check, dynamic-PS
timeout) and stops there. Everything `mmwlan_mesh_start()` pushed to the chip is skipped:

| chip-side step at mesh start | restored by `reinstall_vif`? |
|---|---|
| `mmdrv_add_if(…, MMDRV_INTERFACE_TYPE_MESH)` | ✅ |
| `umac_interface_set_channel_from_regdb()` | ❌ — but `umac_interface_reconfigure_channel()` (`umac_interface.c:925`) is generic; reuse it, as the STA handler does |
| `mmdrv_cfg_bss(vif_id, beacon_interval_tu, 1, 0)` | ❌ |
| `mmdrv_set_bssid(vif_id, mesh_mac)` | ❌ |
| `mmdrv_config_beacon_timer(vif_id, true)` | ❌ (non-fatal on 1.17.8) |
| `mmdrv_start_beaconing(vif_id)` | ❌ |
| `mmdrv_cfg_mesh(vif_id, true, true)` | ❌ |

That table *is* the observed symptom: the chip re-inits, the vif could be re-added, and **nothing
beacons**.

**So the mesh arm is legitimately larger than Linux's**, and the earlier "small mesh arm" reading above
is corrected here. mac80211 collapses all of this into one
`bss_info_change_notify(BSS_CHANGED_BEACON | BSS_CHANGED_BEACON_ENABLED)` because the driver reacts to
those flags; morselib has no such notify layer and issues the `mmdrv_*` commands explicitly. Record it
as a **deliberate divergence** in the S2 code-map — same intent, different plumbing — not a porting gap.
The hoist conclusion still holds for the vif/station/key portion; it is only the "mesh arm is nearly
empty" part that does not survive contact with morselib.

### ⚠ The trap: S2 must NOT re-run `mmwlan_mesh_start()`

The start path wipes host state as its first act — "fresh MBSS" (`umac_mesh.c` ~3812-3816):

```c
memset(&mesh_ctx, 0, sizeof(mesh_ctx));
memset(mesh_peers, 0, sizeof(mesh_peers)); /* fresh peer table for this MBSS */
mesh_path_tbl_reset();                     /* fresh HWMP path table */
memset(mpp_paths, 0, sizeof(mpp_paths));   /* fresh MPP table */
memset(mesh_rmc, 0, sizeof(mesh_rmc));     /* fresh duplicate cache */
```

Re-running start as a recovery shortcut would **destroy the peer, path and MPP state the recovery
exists to preserve**, turning a deaf node into a deaf *and* amnesiac one — and it would do so silently,
since the host tables would then agree with the chip that there are no peers. (Note the irony: that is
the one thing which would make `mmwlan_mesh_peer_count()` finally tell the truth.)

**S2 is therefore a factoring job with a clear seam**: split the chip-configuration sequence out of
`mmwlan_mesh_start()` into a helper callable from both start and recovery, leaving the host-table
resets behind in start only. Then the recovery path is: reinstall vif → reconfigure channel → that
helper → per-ESTAB-peer `mmdrv_update_sta_state` + `umac_keys_reinstall_keys`.

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

---

# S2 — code map

**Reference revisions (pinned).** `rpi-linux` `372414fd42cdd4d8bfcf888cac62db9da947fdb6` (Linux 6.12.21,
`net/mac80211`) · `morse_driver` `7a636e45833a5855bab20da72eb037562578c866`, both as checked out on
chronium under `~/halow/`. New code: `components/halow` branch `feat/mesh-hw-restart-s2`.

**Every `file:line` below was grepped in both trees on the date stamped here — none is cited from
memory.** Rows marked *(call site)* or *(case label)* are exactly that, not definitions; lines drift,
so re-verify before quoting these elsewhere.

**Verified 2026-08-06.**

## Recovery entry point

| new code | Linux |
|---|---|
| `umac_mesh_handle_hw_restarted()` — `umac_mesh.c:4133` | `ieee80211_reconfig()` — `net/mac80211/util.c:1753` (the mesh-relevant subset) |
| declared `umac_mesh.h:114`; invoked from `umac_mmdrv_shim.c:101` *(call site)* | invoked from the driver's restart-completion path |
| STA counterpart it sits beside: `umac_connection_handle_hw_restarted()` — `umac_connection.c:1689` | same function, other vif types |

## Per-step mapping

| # | new code | Linux |
|---|---|---|
| 1 | `umac_interface_reinstall_vif()` — `umac_interface.c:560`, called at `umac_mesh.c:4145` *(call site)* | `drv_add_interface()` — `driver-ops.c:57`, called at `util.c:1853` and `:1865` *(call sites)* |
| 2 | channel via `umac_interface_reconfigure_channel()` (D12), then `mesh_chip_configure_bss()` (cfg_bss + set_bssid) | `drv_add_chanctx()` — `util.c:1888`; BSS/beacon folded into `ieee80211_bss_info_change_notify()` — see D1 |
| 3 | `umac_keys_reinstall_keys(common_stad)` — `umac_keys.c:124`, called at `umac_mesh.c:4196` *(call site)* | `ieee80211_reenable_keys()` — `net/mac80211/key.c:965`, called at `util.c:2108` *(call site)* |
| 4 | `umac_mesh_peer_reinstall_on_chip()` — `umac_mesh.c:3759`, called at `umac_mesh.c:4232` *(call site)* | `ieee80211_reconfig_stations()` — `util.c:1662`, called at `util.c:1965` *(call site, the `default:` arm)* |
| 4a | state ladder `mmdrv_update_sta_state()` — `umac_mesh.c:3783` | `drv_sta_state()` — `driver-ops.c:127`, stepped in `util.c:1662`'s loop |
| 4b | `umac_keys_reinstall_keys(peer->stad)` — called at `umac_mesh.c:3810` *(call site)* | `ieee80211_reenable_keys()` — `key.c:965` |
| 5 | `mesh_chip_start_beaconing()` — `umac_mesh.c:3876`, called at `umac_mesh.c:4247` *(call site)* | `case NL80211_IFTYPE_MESH_POINT:` — `util.c:2043` *(case label)*, setting `BSS_CHANGED_BEACON \| BSS_CHANGED_BEACON_ENABLED` |

⚠ **`util.c` contains TWO switches on `sdata->vif.type` inside `ieee80211_reconfig`, and conflating them
inverts the reading.** The first (station re-add) has **no** `MESH_POINT` label, so a mesh vif takes its
`default:` arm at `util.c:1965` — mesh stations *are* re-added. The `MESH_POINT` case at `util.c:2043`
belongs to the **second** switch and only re-enables beaconing. The second `ieee80211_reconfig_stations()`
at `util.c:2099` is guarded to `AP`/`AP_VLAN` ("APs are now beaconing, add back stations") and does not
apply to mesh.

## Refactor for reuse (not ported code)

| new code | why |
|---|---|
| `mesh_chip_configure_bss()` — `umac_mesh.c:3849` | extracted from `mmwlan_mesh_start()` (`umac_mesh.c:3916`) so start and recovery share one description of the chip sequence |
| `mesh_chip_start_beaconing()` — `umac_mesh.c:3876` | same; kept a *separate* phase because start must populate `mesh_ctx` and arm the host beacon engine between the two |

## HW-global restores (hoisted out of the STA path)

Linux restores hw-global settings in `ieee80211_reconfig()` **unconditionally**, after `drv_start()` and
before the vif loop. morselib had one of them inside the STA-gated body of
`umac_connection_handle_hw_restarted()`, so a mesh node never got it.

| new code | Linux | note |
|---|---|---|
| frag threshold, `umac_mmdrv_shim.c` `hw_restart_evt_handler()` — moved out of `umac_connection.c:1771` | `drv_set_frag_threshold()` — `util.c:1836` | hw-global: value from umacd config, `mmdrv_set_frag_threshold()` takes no vif |
| — *not restored, any vif type* | `drv_set_rts_threshold()` — `util.c:1839` | **pre-existing morselib gap**, affects STA too; tracked in `docs/rimba-todo.md`, not fixed in passing |
| — *no counterpart* | `drv_set_coverage_class()` — `util.c:1842` | morselib has no coverage-class surface |
| `umac_ps_handle_hw_restarted()`, moved out of `umac_connection.c` into `hw_restart_evt_handler()` | Linux restores PS generically | hw-global; the `init_vif` path alone is a no-op — see D10 |

**This was the fix the analysis called for and the first implementation did not deliver.** The stated
conclusion was "S2 is a hoist, not a mirror", yet the first cut added a parallel mesh function and left
genuinely generic state stranded in the STA path — reproducing, in miniature, the exact defect being
fixed. Found by an adversarial review of the faithfulness claim, 2026-08-06.

## Deliberate divergences

**D1 — the mesh arm is an explicit `mmdrv_*` sequence, not three lines.** Linux's mesh case is tiny
because mac80211 pushes channel/BSS/beacon through one `bss_info_change_notify()` and the driver reacts
to the changed-flags. morselib has no notify layer, so the same intent is spelled out as
`umac_interface_set_channel_from_regdb` → `mmdrv_cfg_bss` → `mmdrv_set_bssid` →
`mmdrv_config_beacon_timer` → `mmdrv_start_beaconing` → `mmdrv_cfg_mesh`. Same effect, different
plumbing.

**D2 — the station ladder starts at `AUTHENTICATED`, not `NOTEXIST`.** Linux replays every transition
from `NOTEXIST` upward (`util.c:1662`). `morse_driver` discards exactly the low ones —
*"Ignore both NOTEXIST to NONE and NONE to NOTEXIST"*, `morse_driver/mac.c:4812-4814` — so those
commands would be dropped anyway. The ladder here is the same three states the normal ESTAB path sends.

**D3 — keys go on before beaconing; both references do the reverse.** Linux re-enables keys at
`util.c:2108`, *after* the mesh beacon re-enable at `util.c:2043`, and morselib's own
`mmwlan_mesh_start()` likewise arms beaconing before calling `umac_mesh_install_common_keys()`. This
recovery path installs the common-stad keychain and the per-peer keychains **first**, then arms
beaconing last.

The reason is deliberate: at this point the node still holds live peers that never restarted and will
resume sending to it the moment it is back on air, so beaconing first would open a window in which it
advertises presence with no keys on the chip. Ordering keys first closes that window. Peer keys also
ride with the state ladder in `umac_mesh_peer_reinstall_on_chip()` rather than a later global pass,
because the normal ESTAB path already pairs them and one description of "a peer is installed" beats two.

⚠ An earlier revision justified this as "beaconing is armed last, which the host-beacon-engine ordering
requires". **That was wrong** — the host-beacon-engine constraint is a *start-path* one (`mesh_ctx` must
be populated before `MESH_CONFIG(START)`), and in recovery `mesh_ctx` is already populated and
`active` is already true. The ordering is a choice, not a constraint, and is recorded as such.

**D4 — the common stad has no Linux counterpart.** morselib routes *all* mesh TX through a synthetic
`common_stad`; mac80211 has no such object. Its keychain therefore needs its own re-push (step 3), with
nothing to map to. **Omitting it is invisible in a beacon capture and fatal to everything else** —
beacons are host-generated and unencrypted, so the node beacons perfectly while every data frame dies.
Measured 2026-08-06: 190 s of beaconing, 0/5 pings.

**D5 — keys are RE-PUSHED, never rebuilt, and this is load-bearing.** `umac_keys_reinstall_keys()`
walks the surviving host keychain and re-issues each key to the driver. Constructing fresh
`struct umac_key` values instead resets the CCMP PN, because `connection_keys_install_key()` copies
`tx_seq`/`rx_seq` out of the struct it is handed (`connection_keys.c:118-121`) — and the peer, which
never restarted, still holds its replay window. Every frame we send is then dropped as a replay, with
every install returning success and nothing logged. This matches Linux, where `ieee80211_reenable_keys()`
re-enables existing key objects rather than reinstalling them. Cost one bench cycle to find.

**D6 — recovery must never call `mmwlan_mesh_start()`.** That path opens by memset-ing `mesh_ctx`,
`mesh_peers[]`, the HWMP path table, `mpp_paths` and `mesh_rmc` for a fresh MBSS. Reusing it as a
shortcut would destroy the peer/path state recovery exists to preserve — silently, since the host tables
would then agree with the chip that there are no peers.

**D7 — `drv_conf_tx()` is not replayed; morselib's queue-param surface is STA-only.** A mesh vif takes
switch #1's `default:` arm and then **falls through into `case NL80211_IFTYPE_AP:`**, which restores
per-AC TX (EDCA) parameters: `for (i = 0; i < IEEE80211_NUM_ACS; i++) drv_conf_tx(...)`
(`util.c:1968-1970`, under the `case NL80211_IFTYPE_AP:` label at `util.c:1967`). That fallthrough is
easy to miss and it means Linux *does* restore EDCA for a mesh vif.

morselib **does** have queue-parameter plumbing — `umac_connection.c:911-951` builds `aifs` / `cw_min` /
`cw_max`, with defaults in `umac_config.c:27,34`. (An earlier revision of this row claimed no such
surface existed; that was an under-scoped grep and is corrected here.) But it is **STA-path only**: the
values are parsed from an AP's EDCA Parameter Set element, which a mesh node never receives, and
`mmwlan_mesh_start()` never sets per-AC parameters. So there is no mesh queue configuration to lose and
nothing for recovery to replay. **If mesh ever gains per-AC TX configuration, this becomes a real gap.**

**D8 — only ESTAB peers are re-pushed; Linux re-adds every station.**
`ieee80211_reconfig_stations()` walks all stations on the vif and steps each back to whatever
`sta_state` it held, including partial ones. This restores only peers at `MESH_PLINK_ESTAB`
(`umac_mesh.c:4228`). A peer mid-handshake has no chip state worth recreating — its `aid`/keys are not
final — and the plink retry tick (`umac_mesh_plink_tick`, registered at start and unaffected by the
restart) is still running and will drive it to ESTAB exactly as it would have. Re-pushing a half-formed
peer would instead install a station the handshake then contradicts.

**D9 — the AP beacon dispatcher is now `MMWLAN_AP_DISABLED`-guarded.** `mmdrv_host_get_beacon()`
(`umac_mmdrv_shim.c`) called `umac_ap_get_beacon()` unguarded, even though `umac_ap.c` is not compiled
when `CONFIG_HALOW_AP_MODE=n`. That build only ever linked because `--gc-sections` dropped the whole
dispatcher when nothing referenced it — an undefined symbol surviving on an accident of dead-code
elimination. Adding the mesh recovery call to `hw_restart_evt_handler()` pulled `umac_mesh.o`, and with
it the dispatcher, back into the link and broke `rimba-halow-sta`. Guarded with the idiom the tree
already uses (`umac.c:487`, `s1g_capabilities.c:13`, `config.c:295`). No Linux counterpart — mac80211
has no compile-time AP-removal — so this is a build-configuration fix, not a port decision.

**D10 — power save is restored in the shim, hoisted out of the STA path.** ⚠ **An earlier revision of
this row said the opposite and was wrong.** It argued the mesh arm needed no PS work because
`umac_interface_reinstall_vif()` → `umac_interface_init_vif()` already calls `umac_ps_update_mode()`.
It does — and that call is a **no-op**. `umac_ps_update_mode()` early-returns on
`data->pwr_mode == new_mode`, and `pwr_mode` is HOST state that survives the restart. The chip comes
back with PS off while the host still believes it is on, so no `mmdrv_set_chip_power_save_enabled()` is
ever issued, and every later `update_mode()` short-circuits identically — nothing can fix it
afterwards. `umac_ps_handle_hw_restarted()` exists precisely to break that: it calls `umac_ps_reset()`,
clearing `pwr_mode`, *before* `update_mode()`. It now runs in `hw_restart_evt_handler()` beside the
fragmentation threshold, so every interface type gets it.

> **This is the third instance of one pattern, and it is the real lesson of S2.** morselib's restart
> path is full of HOST-side caches that survive `mmdrv_init()`, so "re-apply the same value" silently
> does nothing:
> | cache | the no-op | fix |
> |---|---|---|
> | `current_s1g_operation` | `set_channel_from_regdb()` returns success, programs nothing (D12) | `umac_interface_reconfigure_channel()` clears it first |
> | key `tx_seq`/`rx_seq` | a rebuilt key rewinds the CCMP PN (D5) | `umac_keys_reinstall_keys()` re-pushes the surviving chain |
> | `pwr_mode` | `umac_ps_update_mode()` early-returns (this row) | `umac_ps_handle_hw_restarted()` resets first |
>
> All three fail **silently and successfully**. Anything else restored on this path should be checked
> against the same question: *does re-applying the value actually reach the chip, or does a host cache
> swallow it?*

**D11 — handler exclusivity is ENFORCED in the shim, not inferred.** `hw_restart_evt_handler()` now
dispatches `if (umac_mesh_is_active()) mesh else connection`, mirroring `ieee80211_reconfig()`'s
per-type switch where no vif takes two arms. It must be enforced: as the correction at the top of this
doc records, the connection handler does **not** no-op on a mesh node. Left unguarded it re-ran
`umac_interface_reinstall_vif()` immediately after the mesh restore had reinstalled the vif,
reprogrammed the BSS, re-pushed every peer and armed beaconing — re-adding the FW interface underneath
all of it, and driving `mmdrv_update_sta_state()` with the connection stad's aid 0 and all-zero BSSID.
It happened to survive on the bench; that is not a property to depend on.

**D12 — the restart path uses `umac_interface_reconfigure_channel()`, NOT
`umac_interface_set_channel_from_regdb()`.** This is the one that bites hardest and silently.
`umac_interface_set_channel_internal()` short-circuits on
`ie_s1g_operation_is_equal(&data->current_s1g_operation, s1g_operation)` and returns **success without
touching the chip**. `mmdrv_init()` wipes the chip's channel but leaves that HOST cache intact, so
re-requesting the same channel after a restart programs nothing and reports success.
`umac_interface_reconfigure_channel()` exists for exactly this — it clears the cached operation, then
re-applies it — which is why the STA restart path calls it (`umac_connection.c:1713`) and Linux's
equivalent re-adds the chanctx (`drv_add_chanctx()`, `util.c:1888`) rather than re-requesting the same
one. `mesh_chip_configure_bss()` therefore does **not** set the channel at all; each caller picks its
own primitive, so the choice cannot be made by accident.

> This was a live defect in the mesh restore from the moment it was written, and it passed anyway —
> because the un-enforced double dispatch in D11 meant the connection handler re-did the channel
> afterwards. Fixing D11 removed the cover and the node came back **on no channel at all**: beaconing,
> and 0/5 on the datapath. Two bugs that concealed each other.

**D13 — every abort path tears the mesh down rather than leaving it "active".**
`umac_mesh_abort_restore()` clears `mesh_ctx.active` and calls `umac_interface_remove()`, mirroring
`mmwlan_mesh_start()`'s `fail:` label. Without it a failed restore left `umac_mesh_is_active()` true
over a dead chip — beacon builder still routed to, plink/RANN ticks still running, datapath still
queueing at an unconfigured vif — i.e. it would have reintroduced the exact silently-deaf state this
work exists to remove, on the error path.

**D14 — chip sequence-number spaces are re-pushed for the mesh stads.**
`umac_datapath_handle_hw_restarted()` (`umac_datapath.c:3512`) re-issues `mmdrv_set_seq_num_spaces()`;
the STA path calls it at `umac_connection.c:1796`, and it is now called for `mesh_ctx.common_stad` and
each restored peer, **after** the station exists on the chip. Same hazard class as the CCMP PN reset in
D5 — the chip restarts its counters at 0 while peers keep their duplicate-detection state — but for the
numbers the chip stamps rather than the host.

**D15 — S3 scope boundary: this path is unreachable on a mesh+AP gate.**
`hw_restart_evt_handler()` still opens with `MMOSAL_ASSERT(false)` when an AP vif is active, so on the
shipped `rimba-halow-mesh-ap` a restart panics *before* `umac_mesh_handle_hw_restarted()` is reached.
S2 therefore fixes mesh-only nodes; the gate remains defect (b) and is S3's problem.

## Verification

Rig: board1 = NUT, board0 = `test-mesh-gate-node NO_PING=1`, chronium `morse0` on ch27. Two independent
probes, because neither alone is sufficient — beaconing does not prove the datapath, and a ping does not
prove the node is beaconing:

| probe | before S2 | after S2 |
|---|---|---|
| `hw_restart_counter` | 0 → 1 | 0 → 1 |
| datapath (ping the peer) | *not measured* | **5/5 before, 5/5 after** |
| beacons (`tools/mesh_hwrestart_cap.py`) | stopped, **112 s silence to end of capture** | **PASS** — 1568 beacons/190 s, 3.1 s outage, 1262 after |

Re-run in full after the code review's eight fixes (2026-08-06), not carried over from the pre-review build — the review changed handler dispatch, the channel primitive and the start-path BSSID, any of which could have moved the result. It did: the intermediate build regressed to **0/5** and that is how D12 was found.

Reference SHAs re-checked and every `file:line` in this map re-derived after the review edits; 13 citations had drifted and were corrected. **Verified 2026-08-06.**

### On-air byte check, re-run after the code review (2026-08-07)

The review changed something that reaches the chip — the start-path BSSID — so the earlier byte check
(taken on a pre-review build) could not be carried over. Both questions were re-answered from fresh
captures, archived under `docs/worklog/artifacts/mesh-hwrestart/`.

**Q1 — does the node still come back byte-identical to its former self?** Last pre-restart beacon vs a
mid-window post-recovery beacon, same capture: length 97 → 97, differing offsets **only 11-12 and
93-96** (S1G timestamp and FCS), IE chain `[213, 217, 232, 48, 114, 113]` unchanged, **6/6 IEs
byte-identical** including Mesh Configuration, Mesh ID and RSN.

**Q2 — did programming a real BSSID change any transmitted byte?** The archived pre-fix capture had
been lost with the scratchpad, so rather than diff against something stale the pre-fix behaviour was
rebuilt and A/B'd on the bench in the same session. Real-BSSID vs zero-BSSID steady-state beacons:
length 97 vs 97, **no differing offsets at all once the timestamp and FCS are excluded**, 6/6 IEs
byte-identical.

**So the BSSID fix changes what the chip is PROGRAMMED with and nothing that is transmitted.** That is
what makes it safe to have landed alongside S2: it cannot have invalidated the earlier on-air work, and
the recovery bench now validates the same configuration the node normally runs.

⚠ Archive captures under `docs/worklog/artifacts/`, not the scratchpad — the scratchpad is cleared
without warning and the first S2 capture was lost that way.


### On-air byte check — the recovered node is byte-identical to its former self

S2 changes *when* the chip is configured, not what goes on the wire: the recovered node emits frames
from the same builders as before. So the on-air rule's meaningful form here is **identity across the
restart**, not a fresh byte-diff against Linux — the mesh beacon's Linux conformance was established by
the 802.11s port and is unchanged by this work. A restart that silently altered the node's advertised
identity (BSSID, Mesh ID, mesh capability, RSN) is exactly what this catches.

Last pre-trigger beacon vs a mid-window post-recovery beacon, same capture (`s2.pcap`, 220 before /
1325 after):

| | result |
|---|---|
| frame length | 97 → 97 |
| differing byte offsets | **only 11-13 and 93-96** — the S1G timestamp and the FCS |
| IE chain | `[213, 217, 232, 48, 114, 113]` → identical |
| IEs byte-identical | **6/6**, including 113 Mesh Configuration, 114 Mesh ID and 48 RSN |

Every non-volatile byte survives the restart. Nothing about the node's on-air identity is rebuilt
differently by the recovery path than by the start path.

---
