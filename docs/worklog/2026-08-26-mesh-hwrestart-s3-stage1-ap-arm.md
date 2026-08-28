# Mesh hw_restart S3 stage 1 — an AP arm, and the dispatch that had no room for the gateway

**Date:** 2026-08-26
**Task:** delete the `MMOSAL_ASSERT(false)` that panicked a mesh+AP node on a hardware restart, and
give the AP host-slot a real restore arm — after resolving the structural blocker recorded at the end
of the S3 reproducer worklog.
**Outcome:** done and bench-verified. The gateway shape now survives a chip restart with **both** vifs
back on air. A follow-on stage (1b, appended at the end) then closed most of the verification gap this
first pass had to leave open — the AP's **group key is now proven on air** to survive the restart. The
pairwise half is still not directly verified, but for a *measured* reason rather than an inferred one.

---

## The blocker, and why it was not a matter of adding a third branch

The handover note said the dispatch

```c
if (umac_mesh_is_active()) umac_mesh_handle_hw_restarted(umacd);
else                       umac_connection_handle_hw_restarted(umacd);
```

had "no room for the gate's mesh + AP case", and that it "needs revisiting, not extending". That is
right, and the reason is visible in the reference.

**`ieee80211_reconfig()` does not choose an interface type. It loops over interfaces.**
`net/mac80211/util.c:1902` — `list_for_each_entry(sdata, &local->interfaces, list)` — and every
interface independently takes its own arm of the type switch. A Linux mesh gateway is two `sdata`
entries and both are walked. The morselib code had collapsed that loop into a single either/or, which
is exactly the construct that cannot express "both".

morselib has exactly two host slots (`VIF_STA_INTERFACE_TYPES_MASK`, `umac_interface.c:118-120`):

| slot | holds | mesh gateway |
|---|---|---|
| `MMWLAN_VIF_STA` | STA · IBSS · mesh · scan · the boot vif | the mesh |
| `MMWLAN_VIF_AP` | AP | the AP |

So the fix is to restore **per slot**, not per type:

```c
/* VIF_STA slot -- STA/IBSS/mesh share one FW vif, so this one IS exclusive */
if (umac_mesh_is_active()) umac_mesh_handle_hw_restarted(umacd);
else                       umac_connection_handle_hw_restarted(umacd);

#if !(defined(MMWLAN_AP_DISABLED) && MMWLAN_AP_DISABLED)
/* VIF_AP slot -- independent, and concurrent with the mesh on the gateway */
umac_ap_handle_hw_restarted(umacd);
#endif
```

Two arms, not three. The exclusivity S2 fought so hard to enforce is real but it is **within** the STA
slot, not across the radio — conflating those two is what produced the dead end.

### The `#if` is load-bearing

`umac_ap.c` is not compiled when `CONFIG_HALOW_AP_MODE=n` (`components/morselib/CMakeLists.txt:120`
defines `MMWLAN_AP_DISABLED=1`), so an unguarded call here is an undefined symbol at link. This is the
same trap already documented on `umac_mmdrv_get_beacon()`, which the S2 work tripped once by pulling
`umac_mesh.o` back into a build that had been getting away with `--gc-sections`. Verified by building
`test-idle` (`# CONFIG_HALOW_AP_MODE is not set`) — links clean.

---

## The channel had to move, and a second caller nearly made that a silent regression

With two slots restoring, `umac_interface_reconfigure_channel()` could not stay inside the arms: on the
gateway the AP arm would have re-programmed the channel a second time, **after** the mesh arm had
already armed beaconing. One radio, one channel, one restore.

It is genuinely HW-global, which is what licenses the hoist: `mmdrv_set_channel()` is issued with
`MMDRV_VIF_ID_INVALID` (`driver/driver.c:864`), and so are the txpower / duty-cycle / mpsw commands
that ride with it (`umac_interface.c:834-866`). Linux puts `ieee80211_hw_config()` in the same place —
once, before the per-vif loop. It now sits in `hw_restart_evt_handler()` beside the frag threshold and
power-save restores, which were hoisted for exactly this argument in S2.

**The trap, caught before it shipped:** `umac_connection_handle_hw_restarted()` has a *second* caller
that never passes through the restart handler — `wnm_sleep_fsm_active_exit()`
(`umac_wnm_sleep.c:281-286`) re-inits the chip after a WNM chip-powerdown and drives the connection
restore directly. Removing the channel call from that function would have silently dropped the channel
restore on the **power-save wake path**, a path this bench cannot easily exercise. So the connection
arm keeps its own copy; the cost is one redundant `SET_CHANNEL` with the same value on a STA restart,
issued before any vif is beaconing. Both the shim and `umac_connection.c` carry the reason inline so
the "duplicate" is not tidied away by a later reader.

⚠ And it must be `reconfigure_channel()`, never `set_channel_from_regdb()` — the fourth instance on
this path of the same host-cache trap: `ie_s1g_operation_is_equal()` against `current_s1g_operation`
short-circuits, programs nothing, and returns SUCCESS.

---

## What the AP arm does

`umac_ap_handle_hw_restarted()` (`umac_ap.c`), mirroring `ieee80211_reconfig()`:

| # | step | Linux |
|---|---|---|
| 1 | `umac_interface_reinstall_vif(MMWLAN_VIF_AP)`, then re-tag every stad with the new vif id | `drv_add_interface()` — `util.c:1853/1865` |
| 2 | `mmdrv_cfg_bss(vif, beacon_interval, dtim, cssid)` | the BSS-config half of the notify |
| 3 | `mmdrv_start_beaconing(vif)` | `drv_start_ap()` — `util.c:2036-2038` |
| 4 | per-STA ladder `AUTHENTICATED → … → its current state`, **after** beaconing | `ieee80211_reconfig_stations()` in the **second** loop — `util.c:2093-2103` |
| 5 | `umac_keys_reinstall_keys()` for `sta_common` (group keys) and each STA, plus the seq-number spaces | `ieee80211_reenable_keys()` — `util.c:2108` |

Three details worth keeping:

- **AP stations are re-added after the beacon, not with everyone else.** Linux's first switch has
  `case NL80211_IFTYPE_AP:` carrying the comment *"AP stations are handled later"* and deliberately
  **not** falling into `ieee80211_reconfig_stations()`. Copying the mesh arm's ordering here would have
  diverged from the reference for no reason.
- **Re-push keys, never rebuild them.** Same PN trap S2 documented: `connection_keys_install_key()`
  copies `tx_seq`/`rx_seq` out of the struct it is handed (`connection_keys.c:118-121`), so a freshly
  built key rewinds this node's CCMP PN to 0 while the associated STAs — which did not restart — keep
  their replay windows. Every install would return success and nothing would be logged.
- **`sta_common` is not optional.** It carries the group keys. Skipping it gives an AP that beacons and
  answers unicast while every group-addressed frame dies — the AP-side twin of the mesh `common_stad`
  defect S2 found only *after* a beacon-only capture had looked green.

**Abort is deliberately not `umac_ap_disable_ap()`.** That path is written for an app-initiated
shutdown and is unsafe from inside the restart event loop: it issues `mmdrv_stop_beaconing()` at a chip
that just failed a command, tears down the hostapd interface, and sleeps 100 ms. `umac_ap_abort_restore()`
instead reports `MMWLAN_LINK_DOWN` through the vif-state callback and leaves the allocation intact so a
later `mmwlan_ap_disable()` still works. The asymmetry with `umac_mesh_abort_restore()` (which *does*
tear down) is justified: a half-restored mesh leaves self-rescheduling plink/RANN timers armed and every
peer stad leaked; the AP has neither.

---

## Bench

Rig: **board1** = NUT (`test-mesh-hwrestart AP_VIF=1`), **board0** = `test-mesh-gate-node NO_PING=1`
(mesh peer + the capture's liveness control), **chronium** `morse0` monitor on ch27. Third run added
**board2** (PPK2-powered) as a SAE client on the NUT's AP.

MACs, all read back rather than derived: NUT device `bc:2a:33:96:b2:92`, its mesh SA
`e2:72:a1:f8:f9:40`, its **AP BSSID `be:2a:33:96:b2:92`** (device MAC with `[0] ^= 0x02`, per
`umac_ap_enable_ap()`), control `e2:72:a1:f8:ef:a4`, client `bc:2a:33:96:b2:33`.

> ⚠ `docs/reference/rimba-bench-devices.md` lists board2's mesh MAC as `e2:72:a1:f8:f0:08`, derived
> from its efuse MAC. Its actual HaLow MAC is `bc:2a:33:96:b2:33` — the MM6108 module has its own MAC
> and it is not the ESP efuse MAC ± a bit. The same is true of board1 (`bc:2a:33:96:b2:92`, not
> `e2:72:a1:f8:f9:40` — that one is the mesh MAC the fixture *sets*). Deriving the AP BSSID from the
> doc would have pointed the scorer at an address nothing transmits.

### Result — on device

No panic. Reproduced three times.

```
TEST|INFO|s3-expect-recovery|an AP vif is active; the handler must restore BOTH host-slots and return
TEST|STEP|restart-ran|PASS|hw_restart_counter 0 -> 1
TEST|STEP|data-after|PASS|5/5 replies (was 4/5 before the restart)
TEST|RESULT|PASS
```

### Result — off air

`tools/mesh_hwrestart_cap.py` now takes `--ap <bssid>` and scores both vifs against the same capture.
Two runs, both `RESULT|PASS` on mesh **and** AP. Gaps measured directly from the pcap
(`docs/worklog/artifacts/mesh-hwrestart/2026-08-26-s3-stage1-both-vifs-recover.pcap`):

| SA | beacons | outage at the restart |
|---|---|---|
| AP `be:2a:33:96:b2:92` | 1831 | **0.98 s** |
| mesh `e2:72:a1:f8:f9:40` | 1271 | **3.88 s** |
| control `e2:72:a1:f8:ef:a4` | 1878 | none — largest gap in the whole window 0.31 s |

The control never gapped, so both silences are the node's and both recoveries are real.

**The AP recovering ~2.9 s faster than the mesh looked wrong and had to be explained before the PASS
was worth anything** — the AP arm runs *after* the mesh arm, so it should be slower. It is not a
paradox: the mesh's first beacon lags its arming. The same ordering appears on a plain cold boot in the
same capture (AP back after 3.67 s, mesh after 5.00 s) where the mesh is started *first* — so the lag
is a property of the mesh beacon engine, not of the restore. Consistent with
`mesh_chip_start_beaconing` logging `config_beacon_timer fw_status=14` and falling back to
`MESH_CONFIG` to arm beaconing.

Also measured, incidentally: with an AP vif sharing the radio the node's **mesh** beacon rate drops to
6.3/s (S2 measured 8.25/s mesh-only), while its AP beacons run at 9.1/s. Total ~15.4 beacons/s off one
radio. Airtime sharing, not a regression, but worth knowing before reading a mesh beacon count from a
gateway.

### The AP client run — what it proved, and what it did not

`docs/worklog/artifacts/mesh-hwrestart/2026-08-26-s3-stage1-ap-client.pcap`. board2 ran
`test-apsta-sta` and associated to the NUT's AP over SAE (`TEST|STEP|associate|PASS`). Its pings fail
by construction — the hwrestart fixture has no AP-side netif and pins all TX to the mesh vif — so the
client is an **association**, not a datapath.

Timeline, anchored on the AP beacon gap at pcap `+38.0 → +39.1`:

| pcap t | event |
|---|---|
| +38.0 | chip restart; AP beacons stop (mesh stops +38.1) |
| **+39.09** | **client → AP deauth** — 1.1 s into the outage, while the AP is still off air |
| +39.1 / +41.2 | AP / mesh beacons resume |
| +43.47 | AP → client **probe response** — the AP's management path is alive |
| +44.92 | client → AP auth ×5, **unanswered** |
| +49.91 | client → AP deauth ×5 |
| +59.11 → +60.62 | probe-resp, auth both ways, assoc-req/resp — **client re-associated** |

So the client's link did **not** survive, and it is worth being precise about why that is not evidence
against the restore:

- The client gave up **1.1 s** into the outage, before the AP had finished coming back. No AP-side
  restore can prevent that; a chip restart is ~1–4 s of silence and this STA's tolerance is shorter.
- The AP was answering probe requests **5 s** after the restart (+43.47), so its management path
  recovered. The 15 s during which it ignored the client's auths is consistent with 802.11w
  SA-Query / association-comeback on a PMF-required BSS whose hostapd still believed that STA
  associated — which is what a *successful* station restore looks like from the outside.
- No `AP: … FAILED` or `AP: hw-restart restore ABORTED` appeared on the console. Every failure path in
  the new arm is `MMLOG_ERR`, and `MMLOG_ERR` demonstrably reaches this UART (the
  `mesh_chip_start_beaconing` line does). Their absence says the arm ran without hitting an error.

**Stated plainly: steps 4 and 5 of the AP arm — the per-STA ladder, the per-STA keys, the group keys —
are NOT directly verified.** They are inferred from the three points above. The concrete blocker is
that no client on this bench tolerates a multi-second AP outage, so the restored station entry is made
moot by the client's own deauth before anything can be sent over it. Closing it properly needs either a
client with a longer beacon-loss tolerance, or the trigger wired into `test-mesh-ap-gate` (which has a
working AP-side datapath and IP forwarding) so a ping across the AP link can be scored before and after.
That is the right next step for the AP side and is deliberately not being claimed here.

An attempt to get direct evidence by rebuilding at `MMLOG_LEVEL_INF` to capture the arm's own
`AP: restored vif %u with %d associated STA(s)` summary **failed to link** —
`undefined reference to 'mm_hexdump'` from `umac_datapath.c`. That is a pre-existing morselib gap at
INF level, unrelated to this change; the temporary edit was reverted.

---

## The scorer change

`tools/mesh_hwrestart_cap.py` gained `--ap`, and the per-phase logic moved into a `score()` function
run once per address. One design point matters:

**"Did a restart happen at all?" is evidence about the radio, not about one vif.** The gapless-capture
INCONCLUSIVE branch — which exists because every early-exit in the fixture parks *before* triggering a
restart, so a mis-rigged run would otherwise score PASS — now consults `restart_observed`, computed
across **every** scored address. Keyed per-SA it would have called this very run's AP arm INCONCLUSIVE:
the AP's 0.98 s outage is under the 2.0 s threshold while the mesh's 3.88 s is plainly a restart. The
single-address behaviour is unchanged, because with one SA the global test reduces to the local one.

INCONCLUSIVE dominates FAIL in the combined verdict: a capture that does not contain the trigger cannot
condemn anything in it.

---

## State

- morselib (submodule, uncommitted): `umac_mmdrv_shim.c`, `umac_ap.c`, `umac_ap.h`, `umac_mesh.c`,
  `umac_connection.c`.
- rimba: `firmware/test-mesh-hwrestart/main/app_main.c` (the arm now expects recovery and prints
  `ap-mac`), `tools/mesh_hwrestart_cap.py`, `tools/regtest/manifest.py`, this worklog + its render,
  two archived pcaps.
- ⚠ **One line was added after the bench runs:** a NULL guard on `data->sta_common` in the AP arm, with
  an abort rather than the `MMOSAL_ASSERT` that `umac_sta_data_set_vif_id()` /
  `umac_keys_reinstall_keys()` would otherwise fire mid-recovery. It is on a path that could not have
  executed in the tested configuration — the AP was up and beaconing, so `sta_common` was non-NULL —
  but the flashed binary did not contain it. Rebuilt clean; not re-flashed.
- `make test-unit` **53/53**. Builds clean in all three configurations that matter: the mesh+AP fixture
  (`AP_VIF=1`), the shipped gate (`rimba-halow-mesh-ap`), and an AP-disabled app (`test-idle`).
- Bench radio-silent: `rimba-hello` on board0/board1/board2, `wlan1` + `morse0` down on chronium, the
  PPK2 hold released so board2 is dark.

**Not done:** stage 2 (the intermittent boot-time ISR watchdog loop) is untouched. Stage 1 *avoids* it
by never rebooting, but it stays reachable by any reset landing mid-restart.


---

# Stage 1b — closing the verification gap (same day, later)

Stage 1 left steps 4 and 5 of the AP arm — the per-STA ladder and the keys — inferred rather than
measured, because `test-mesh-hwrestart` has no AP-side datapath. `test-mesh-ap-gate` does: a second
`esp_netif` on 192.168.12.1, per-vif RX demux and per-vif TX tagging. So the restart trigger went there,
behind `HW_RESTART=1`.

## Two probes, because one of them cannot work on this bench

The obvious probe — ping the client across the AP link before and after — **cannot stand alone**, and
stage 1 already measured why: a client deauths ~1.1 s into the outage and re-associates ~22 s later. An
after-ping that succeeds may be talking to a *fresh* association and would prove nothing. There is no
public morselib knob to lengthen a STA's beacon-loss tolerance (`umac_connection_set_monitor_disable()`
is internal), so the client cannot simply be told to hold on.

| probe | scores | needs the client to survive? |
|---|---|---|
| **A — group-key PN continuity, off air** | step 5's group half | **no** — the AP owns the GTK alone |
| **B — unicast ping across the AP link** | step 4 + step 5's pairwise half | **yes** |

**Probe A is the one that works, and it is a direct measurement of the exact S2 defect.** The gate emits
a 5 Hz broadcast train on its AP subnet; those frames are group-addressed, so the AP encrypts them with
the GTK held on `sta_common` — precisely the keychain step 5 re-pushes. The CCMP packet number is in the
clear in every frame's header. PN keeps climbing across the outage ⇒ the surviving keychain was
re-pushed. PN rewinds to a low value ⇒ the key was rebuilt, which is the
`connection_keys_install_key()` copies-`tx_seq` trap, and every associated station would then discard
every group frame as a replay, silently, with every install returning success.

**Probe B is instrumented to refuse a PASS it did not earn.** `ap_sta_status_cb` already fires on state
changes, so the arm counts AUTHORIZED transitions. If the count advances across the restart the client
re-associated, and the verdict is INCONCLUSIVE-for-the-restore no matter how good the ping looked.

## Results

### Probe B — INCONCLUSIVE, and the discriminator is why that is the right answer

```
TEST|STEP|sta-before|PASS|1 client(s) authorized after 6s (1 association event(s))
TEST|STEP|data-before|PASS|5/5 replies from the client across the AP link
TEST|STEP|restart-ran|PASS|hw_restart_counter 0 -> 1
TEST|STEP|data-after|PASS|5/5 replies (was 5/5 before)
TEST|INFO|assoc-churn|1 association event(s) before the restart, 2 after -- the client RE-ASSOCIATED
TEST|RESULT|INCONCLUSIVE|... that ping crossed a FRESH association ...
```

**A 5/5 after-ping that means nothing.** Without the association counter this run would have printed a
confident PASS on the strength of traffic crossing a link that had been rebuilt from scratch during the
outage. That is the whole reason the counter is there, and it earned its place on the first run. What
the run *does* establish is that the AP is fully functional after the restart — it beacons, completes
SAE, and carries traffic — which is worth having, just not the thing probe B was built to test.

### Probe A — PASS, with the positive control in the same capture

```
frame lengths seen (>=15 frames): 100B x201, 114B x518
marker boundary: last 100B frame at +43.93s, first 114B frame at +45.01s  ->  outage 1.08s
PN across it: 135 -> 136  (delta +1)   key id 1 -> 1
control beacon rate: 8.5/s before the outage, 8.9/s after
RESULT|PASS|the AP's group key SURVIVED the restart ...
```

The PN advanced by exactly **+1** across a 1.08 s outage on the same key id — consistent with the 0.98 s
AP beacon outage stage 1 measured. **The group key was re-pushed, not rebuilt.**

The same capture contains **two legitimate PN resets** — at +7.09 s (the AP booting) and +17.11 s (the
first client's association triggering a GTK install), both `PN → 1`. Those are the positive control, and
they are what make the negative result mean something: the instrument demonstrably *can* see a rewind,
so not seeing one across the restart is evidence rather than absence of evidence. The tool reports them
rather than filtering them, and warns when a run contains none.

Capture archived at `docs/worklog/artifacts/mesh-hwrestart/2026-08-26-s3-stage1b-ap-gtk-pn.pcap`.

## The tool was wrong on its first run, and the bench is what caught it

The first version of `tools/ap_gtk_pn_cap.py` located the restart as **the largest gap in the train**,
reusing the idea from the mesh scorer. It returned:

```
outage: 3.33s at +3.88s -> +7.21s   PN 1379 -> 1
RESULT|FAIL|the AP's group key was REBUILT across the restart ...
```

That gap is the board's own **reboot**, not the chip restart — and a reboot legitimately installs a
fresh GTK at PN 1. The reboot's hole (3.33 s) is *larger* than the restart's (~1 s), so "largest gap"
picks the wrong event every time, and scored a textbook PASS as a FAIL.

**The fix is to identify the restart rather than infer it from the shape of the trace.** The fixture now
switches its broadcast payload to a longer one immediately before triggering — 18 B → 32 B, which is
100 B → 114 B on air — and the tool measures across that length boundary. It has to be *length* because
the frames are GTK-encrypted: the sniffer can see how big they are and nothing else.

This is the third time on this epic that a verdict keyed on "the biggest/last thing in the window" has
been keyed on whatever else happened to be in the window. The mesh scorer learned it twice (largest-gap
vs last-gap, and requiring a gap at all). Worth stating as a rule: **a scorer must locate its event, not
recognise it by size.**

## Two build notes

- **The gate overflowed its app partition** by `0x2dd0` once the arm pulled in `esp_ping` and the lwIP
  socket API. It now carries its own 2 MB `partitions.csv`, like `test-mesh-hwrestart` and
  `test-raw-rps`. Applied to **both** arms rather than only the armed build, because the partition table
  is chosen by Kconfig at configure time and cannot follow a `-D`; the cost is nothing on a 16 MB flash
  and it keeps one layout for the fixture instead of two that differ by a build flag.
- **`TEST_HW_RESTART` is a CMake cache variable**, so the arm is sticky exactly as `TEST_AP_VIF` is.
  `HW_RESTART=0` or a wiped build dir; omitting it inherits the arm. Documented at the CMake gate and in
  the manifest.

## Regression check

`test-mesh-ap-gate` is a **T2 fixture**, so changing it means re-running its scenario, not just building
it. `make test-t2 TEST=mesh-ap` → **PASS in 391 s**, plus the tier's own radio-silent sweep. The default
arm is unaffected by the addition.

## What is still not verified — now a measurement, not an inference

**Step 4 and the pairwise half of step 5.** The blocker is no longer "no client tolerates the outage" as
an inference from one capture; it is a scored, repeatable INCONCLUSIVE with the association count as
evidence. Closing it needs a client that holds its association across a ~1–4 s AP outage, which on this
stack means either exposing the connection-monitor control through the public API or a non-morselib STA
(a Linux HaLow client) whose tolerance can be configured. Both are larger than this stage and neither is
being claimed.

## State after 1b

- rimba, additionally uncommitted: `firmware/test-mesh-ap-gate/main/app_main.c` (+ its `CMakeLists.txt`,
  `sdkconfig.defaults`, new `partitions.csv`), `tools/ap_gtk_pn_cap.py` (new), `Makefile` (the
  `HW_RESTART` pass-through), manifest + design doc + this worklog, one more archived pcap.
- `make test-unit` 53/53; `make test-t2 TEST=mesh-ap` PASS; both gate arms and all three earlier
  configurations build clean.
- Bench radio-silent: the tier left board0/1/2 on `test-idle`, chronium's `wlan1` and `morse0` are down,
  and the PPK2 hold is released so board2 is dark.


---

# Review round — four findings, all real

`/code-review` at `high` against mm-esp32-halow PR 35, before merging. Every finding was verified
against the source rather than taken on trust; all four stood up, and one was a genuine correctness bug
in code the bench had already passed.

### 1. The AP's sequence-number-space restore went to the WRONG VIF (high)

`umac_datapath_handle_hw_restarted()` derived its vif from
`umac_interface_get_vif_id(umacd, UMAC_INTERFACE_STA)`. That is correct for the STA and mesh callers —
mesh shares the STA host-slot — and **silently wrong for an AP one**, because `UMAC_INTERFACE_STA` is
inside `VIF_STA_INTERFACE_TYPES_MASK` and the lookup returns the STA-slot id whatever the stad belongs
to. On the gateway it aimed an AP client's counters at the **mesh** vif while leaving the AP vif's at
the post-`mmdrv_init()` zero — the exact duplicate-detection rewind the call exists to prevent.

**This is the same trap S2 documented and I walked into it again from the other side.** S2's finding was
that `get_vif_id(UMAC_INTERFACE_STA)` returns the STA slot *without consulting `active_interface_types`*,
so the connection handler does not no-op on a mesh node. I recorded that, then reused a helper built on
the same lookup for an AP stad. The lesson generalises: **any `get_vif_id(UMAC_INTERFACE_STA)` inside a
routine that can be handed an arbitrary stad is a latent bug.** `vif_id` is now a parameter; the STA and
mesh call sites pass exactly the value they resolved before, so their behaviour is bit-identical.

Worth noting what this means about the bench result: stage 1b **passed with this bug present**, because
probe A measures the *group* key (which has no per-peer sequence spaces) and probe B was already
INCONCLUSIVE for an unrelated reason. A green run is not a proof of everything the code does.

### 2. `sta_common` was pushed a sequence-number baseline for an all-zero address (medium)

The AP's `sta_common` never gets a peer address — `umac_ap_enable_ap()` and `umac_ap_start()` only ever
call `umac_sta_data_set_bssid()` on it, and it is `calloc`'d. The mesh common stad *does* carry this
node's own MAC, which is why the mesh arm can make that call meaningfully. I copied the call across
without checking that the premise held. Dropped, with the asymmetry written where a reader will look.

### 3. Hoisting the channel restore downgraded a fatal error to a log line (medium)

Two regressions from one move, and I had talked myself into the first while writing it.

**(a)** Inside the arms this call had consequences — `umac_mesh_abort_restore()` for mesh,
`UMAC_FATAL_ERROR()` for STA. Hoisted, it only logged, and both arms then went on to re-arm beaconing on
a chip whose channel was never programmed. The handler literally printed *"the node will be unable to
transmit or receive"* and continued. That manufactures the silently-deaf state this entire feature
exists to eliminate. It is `UMAC_FATAL_ERROR` again, and the per-slot arms are skipped when it fails.

**(b)** Worse, and I had not seen it: `umac_interface_reconfigure_channel()` memsets the cached S1G
operation up front, and `set_channel_internal()` only rewrites it on a **successful**
`mmdrv_set_channel()`. So a failure leaves the cache zeroed — and every later call then reads
`operating_channel_index == 0`, takes the empty branch, and returns SUCCESS having programmed nothing.
One failure permanently disables the retry, silently. That includes the second caller I had deliberately
preserved on the WNM chip-powerdown wake path, so the "keep the duplicate for safety" reasoning was
protecting a call that a single earlier failure would have neutered. The cache is now restored on
failure.

### 4. A vif alone does not mean the AP started (low)

`umac_ap_start()`'s failure path frees `config.head`/`tail` and memsets the config but does **not**
remove the interface it added — reachable from a missing S1G Operation IE, or a failing `set_channel` /
`cfg_bss` / `start_beaconing`. A restart would then pass my vif-only guard and push
`cfg_bss(beacon_interval=0, dtim_period=0, crc32(""))` plus beaconing on a dead config. The guard now
also requires `config.head`. The interface left behind by that failure path is a **separate leak**,
called out rather than fixed here: it wants its own change and its own verification.

## Re-verified after the fixes

The fixes changed real behaviour, so the bench ran again rather than trusting a clean build:

| | before the fixes | after |
|---|---|---|
| group-key PN across the outage | 135 → 136 (1.08 s) | **133 → 134 (0.99 s)** |
| positive controls in the capture | 2 | **2** |
| mesh datapath | 5/5 | **5/5** |
| AP/mesh restore error lines | 0 | **0** |

Builds clean for the mesh+AP fixture, both gateway arms, a STA-only app and an AP-disabled app.

## Landed

- **mm-esp32-halow PR 35** merged (rebase) — `ece942cd` + `22e84d75` on submodule `main`. ⚠ The rebase
  rewrote both SHAs; the superproject gitlink points at **`22e84d75`**, the post-rebase tip, verified
  with `git merge-base --is-ancestor` before committing.


---

# Second review round — eight findings on the superproject PR, plus three on the follow-up

`/code-review` at `high` against rimba PR 57. Eight findings, all real. One of them was a bug I had
introduced *in the previous round's fix*, which is the part worth recording.

### The one that would have blamed the firmware for the bench's behaviour

The `TEST_FAIL` branch asserted "the client did NOT re-associate, so its chip station entry is exactly
what the restore re-pushed". But `s_auth_events` only counts AUTHORIZED transitions and is **never
decremented**, so "unchanged" cannot distinguish *held its association* from *left and never came back*
— and the client is measured to deauth ~1.1 s into every outage. A client that failed to return within
the window would have printed **"the per-STA restore is broken"** on a run where the station record was
torn down by the client and there was nothing to restore. `s_sta_n` **is** decremented on the deauth
path, so the verdict now consults both, and that case reads INCONCLUSIVE.

### The fix I got wrong last round

Finding 7 was **my own** finding-3b fix. Making `umac_interface_reconfigure_channel()` restore its cache
on failure means the host claims a channel the chip never got — and the next `set_channel()` for the
same operation then short-circuits on `ie_s1g_operation_is_equal()` and returns SUCCESS having
programmed nothing. The same trap, entered from the other side. And the retry it protected does not
exist: restoring `UMAC_FATAL_ERROR` severity in that same round had already closed the hole. Reverted.

**Then the review of the revert caught that my justification for it was false.** I wrote that
`mmwlan_get_vif_channel_info()` — "which the AP uses to inherit the STA's channel" — reads that cache.
It does not: the STA arm reads `bss_cfg.channel_cfg`, the AP arm reads its own args, and `umac_ap.c`
does not reference the cache at all. The real readers are `umac_rc.c:166`/`:410` and the ECSA handler.
Since a comment in that file is the durable record for the path, a wrong mechanism in it sends the next
reader to the wrong struct — so it is corrected to the route that does cause damage: `umac_data_init()`
zeroes the cache only at app boot, **not** on the fatal shutdown, so a stale value survives and a later
`mmwlan_mesh_start()` / `mmwlan_ap_enable()` beacons on an unprogrammed channel.

That review also caught that the empty-cache branch returns `MMWLAN_SUCCESS` while nothing is
programmed, which is reachable after a first failure because `umac_fatal_error()` early-returns once
`fatal_error` is latched. It returns `MMWLAN_UNAVAILABLE` now, and the shim distinguishes "nothing was
configured to restore" (benign — skip the arms, warn) from "the chip rejected a channel it was
operating on" (fatal).

### The rest

| # | finding | fix |
|---|---|---|
| 2 | the capture marker is flipped **before** the trigger, racing the 5 Hz train — a post-length frame can reach air before the chip stops, giving a ~0.2 s "outage" and an INCONCLUSIVE on a good restart | the marker now **narrows** the search and a real gap **confirms** the event |
| 3 | `mesh_hwrestart_cap.py` would score a **panic-and-reboot regression as PASS** — exactly what the repurposed `AP_VIF=1` arm exists to catch, since a rebooted node returns under the same MACs | no cheap marker exists for that fixture, so the PASS text states the limitation and names the two required cross-checks instead of overclaiming |
| 4 | for the AP address "largest gap" is **not** the event — the AP recovers inside `GAP_S`, so an unrelated late dropout could FAIL a recovered AP | verdict rests on a tail window, not on a chosen gap |
| 5 | `HW_RESTART` is the first sticky cache arm on a fixture that **is in T2**, and the harness's stale-cache guard only fires on regtest-supplied vars — a manual armed flash poisons a later tier run | the Makefile always emits a value for both arms |
| 6 | `<=` counted CCMP **retransmissions** (same PN) as resets, which would have permanently silenced the no-positive-control warning | strict `<` |
| 8 | a genuine FAIL was masked whenever another address was unmeasurable — a mistyped `--ap` could hide a real mesh failure | only the global "no restart anywhere" outranks a FAIL |

**Deferred, tracked not fixed:** an ECSA bandwidth comparison at `umac_connection.c:1491` that is always
false, because `old_s1g_info` aliases the struct `set_channel()` overwrites — so a bandwidth change
never restarts rate control. Pre-existing, unrelated to this path beyond sharing the struct, and it
needs an ECSA generator the bench does not have. Recorded in the mesh-ap milestones.

## Verified after each round

Two more bench cycles, because the fixes changed real behaviour both times:

| | round 1 fixes | round 2 fixes |
|---|---|---|
| group-key PN across the outage | 131 → 132 (1.11 s) | **132 → 133 (1.14 s)** |
| positive controls | 2 | **2** |
| mesh datapath | 5/5 | **5/5** |
| restore errors / "no channel" warnings | 0 | **0** |

The scorer changes I could not reach on the bench were exercised by **replaying the archived pcap
through the tool's own code** (a stubbed `AF_PACKET` socket and clock): it reproduces the original PASS,
and the previously-masked FAIL now surfaces as exit 1. The sticky-cache fix was demonstrated directly —
an armed build followed by a bare build now yields the *unarmed* binary, with `TEST_HW_RESTART=0` in the
cache.

## Landed

- **mm-esp32-halow PR 35** — the AP arm — `ece942cd` + `22e84d75`.
- **mm-esp32-halow PR 36** — the cache revert and its own review round — `978fb8b4` + `b9907a94`.
- Superproject gitlink → **`b9907a94`**, verified on the submodule's `main`.
