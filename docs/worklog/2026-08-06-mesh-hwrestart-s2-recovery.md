# Mesh hw_restart S2 — a node that beacons perfectly and cannot pass a packet

**Date:** 2026-08-06
**Task:** S2 of the mesh `hw_restart` work — make a mesh node actually survive a chip restart, turning
S1's red test green.
**Outcome:** done and green on both probes, after a code review that found eight further defects —
two of which were concealing each other. Two earlier defects were found *after* the first "it works",
both invisible to the metric I had at the time. The session's real lesson is that a second probe is
not redundancy — it is what makes the first one honest.

---

## The design changed before a line was written

The backlog framing was "add `umac_mesh_handle_hw_restarted()`, mirroring the connection one, looping
over N peers instead of one BSSID". Reading `ieee80211_reconfig()` (`util.c:1753`) first said otherwise.

Linux's mesh case is **three lines** — re-enable the beacon — because everything a mesh vif actually
needs restored happens **generically, before the type switch**: `drv_add_interface()`,
`ieee80211_reconfig_stations()`, `ieee80211_reenable_keys()`. morselib had put that same generic work
*inside* `umac_connection_handle_hw_restarted()`, a STA-only function keyed on `UMAC_INTERFACE_STA`.

**That is the root cause, stated properly.** A mesh node gets nothing not because a mesh handler is
missing, but because the generic restore was written into a STA-specific function.

One trap in reading the reference, worth recording because I fell into it for several minutes:
`ieee80211_reconfig` has **two** switches on `vif.type`. The first (station re-add) has no
`MESH_POINT` label, so mesh takes its `default:` arm and *is* re-added; the `MESH_POINT` case at
`util.c:2043` belongs to the second switch and only touches beaconing. Conflating them says "Linux
never restores mesh peers", which is the opposite of the truth.

## What got built

A **hoist plus a real mesh arm**, not a mirror:

- `mesh_chip_configure_bss()` / `mesh_chip_start_beaconing()` — the chip sequence factored out of
  `mmwlan_mesh_start()`, extracted so start's statement order is **unchanged**. Two phases rather than
  one because start must populate `mesh_ctx` and arm the host beacon engine between them: the firmware
  must not be told to beacon before the host can serve one.
- `umac_mesh_peer_reinstall_on_chip()` — the chip-side subset of the existing ESTAB path.
- `umac_mesh_handle_hw_restarted()` — reinstall vif → re-tag stads with the new vif id → chip BSS
  config → common keys → per-peer restore → arm beaconing.

`umac_interface_reinstall_vif()` needed no change: it already maps `UMAC_INTERFACE_MESH` →
`MMDRV_INTERFACE_TYPE_MESH`. It had simply never been reachable for mesh, because its only caller was
the STA-gated handler.

Full function-level mapping, pinned reference SHAs and fifteen deliberate divergences are in the code map
in `docs/mesh-ap/rimba-mesh-hw-restart-design.md`.

---

## The part worth reading: "it works" was wrong twice

### First "it works" — beacons only

The S1 scorer went green. Beacons stopped for 2.94 s at the trigger, then ran for 140 s. Against the
pre-S2 baseline of *112 s of silence and never returning*, that looked conclusive, and I said so.

It wasn't. **Beaconing proves the vif is on air. It proves nothing about the datapath.** So the fixture
gained a ping — before the restart and after, the before-ping being what makes the after-ping readable
(a bare post-restart failure is ambiguous with "this rig never carried traffic", which is exactly the
documented empty-mpath trap).

Result: **190 s of continuous beaconing, and 0/5 pings**, having answered 5/5 a minute earlier. A node
that looks perfect on air and is dead as a network node.

### Defect 1 — the common stad's keys were never replayed

`umac_mesh_install_common_keys()` runs at start, and its own comment says it plainly: *"The common_stad
drives ALL mesh TX (every mesh frame dequeues from it)."* Recovery never re-pushed them.

Beacons are host-generated and unencrypted, so they sailed through while every data frame died. **This
defect is invisible in a beacon capture by construction** — which is precisely why the on-air metric,
the thing S1 was built around, could not see it.

### Defect 2 — I was resetting our CCMP packet number to zero

Fixing defect 1 changed nothing: still 0/5. Every command returned success. Nothing logged an error.

`connection_keys_install_key()` **copies `tx_seq`/`rx_seq` out of the `struct umac_key` it is handed**
(`connection_keys.c:118-121`). I had been rebuilding keys from fresh stack structs, so those counters
were zero — our PN restarted at 0 while the peer, which never restarted, kept its replay window where
it was. **Every frame we sent was discarded by the peer as a replay.**

The fix is to stop rebuilding keys at all. Only the *chip* lost its copy; the host keychain survived
intact, PN and all. `umac_keys_reinstall_keys()` re-pushes that existing chain to the driver without
touching the counters — the same primitive the STA path already uses (`umac_connection.c:1721`) and the
same intent as `ieee80211_reenable_keys()`.

**So the bug was a divergence from the reference, and the fix was to stop diverging.** Linux re-enables
existing key objects; I had invented a rebuild. The porting rule earns its keep here — not as
paperwork, but because the reference's *shape* encoded a constraint I didn't know existed.

### Defect 3 — my own scorer's PASS was structurally unsound

The scorer required a gap to award PASS. Two consequences, both bad:

- A recovery **too fast** to drop `GAP_S` worth of beacons was reported INCONCLUSIVE. Better behaviour,
  worse verdict.
- When an unrelated gap (the board's own reboot) happened to share the capture, PASS was awarded keyed
  on *that*. Right answer, wrong evidence — the exact failure this tool exists to prevent, reproduced
  inside the tool itself.

The verdict now rests on what actually distinguishes the defect: a solid baseline, and beacons still
flowing at the end. The gap is reported as diagnostic detail. `GAP_S` also dropped 3.0 → 2.0 s, on the
measurement that the teardown outage is 2.94 s — the old threshold sat just *above* the thing it was
meant to catch.

---

## Result

| probe | before S2 | after S2 |
|---|---|---|
| `hw_restart_counter` | 0 → 1 | 0 → 1 |
| datapath — ping the peer | *not measured* | **5/5 before, 5/5 after** |
| beacons — chronium `morse0` | stopped, **112 s silence to end of capture** | **PASS** — 1568 over 190 s, 3.1 s outage, 1262 after |

Two probes, because neither is sufficient alone: beaconing does not prove the datapath, and a ping does
not prove the node is beaconing. Defect 1 was invisible to one; defect 2 was invisible to the other.

### On-air byte check (first pass)

S2 changes *when* the chip is configured, not what goes on the wire, so the on-air rule's meaningful
form here is **identity across the restart** rather than a fresh byte-diff against Linux (the mesh
beacon's Linux conformance came from the 802.11s port and is untouched by this work). What it catches is
a recovery that silently rebuilds the node's advertised identity differently.

Result on the pre-review build: length 97 → 97, only the S1G timestamp and FCS differing, 6/6 IEs
byte-identical. **This pass was later invalidated and re-run** — the code review changed the start-path
BSSID, which reaches the chip — see the re-run section below for the numbers that stand.

---

## The adversarial review — which refuted three of my own claims

Having declared the port Linux-faithful, I ran a pass whose goal was to *refute* that. It did, three
times, and the most important finding is that I had violated my own stated design.

**R1 — I wrote "S2 is a hoist, not a mirror", then shipped a mirror.**
`umac_connection_handle_hw_restarted()`'s STA guard spans `umac_connection.c:1697-1797`, and inside it
sat the **fragmentation threshold** restore: value from `umac_config_get_frag_threshold(umacd)`, pushed
via `mmdrv_set_frag_threshold()` — which takes **no vif**. It is hw-global, Linux restores it
unconditionally at `util.c:1836`, and a mesh node never entered the body that restored it. I had
diagnosed exactly this bug class as the root cause, prescribed a hoist, and then added a parallel mesh
function that left it stranded. Now hoisted into `hw_restart_evt_handler()`, before the per-interface
handlers, matching Linux's placement. Latent rather than active — no app in `firmware/` sets a frag
threshold — but a future one would have lost it silently.

**R2 — D7 was factually wrong.** I had written that morselib has "no `conf_tx`/EDCA surface at all".
It does: `umac_connection.c:911-951` builds `aifs`/`cw_min`/`cw_max`, with defaults in
`umac_config.c:27,34`. My grep was too narrow. The *conclusion* survives — that plumbing is fed by an
AP's EDCA element, which a mesh node never receives — but a code map that misstates the reference is
worse than one that omits the row.

**R3 — D3's justification cited a constraint that doesn't apply.** I justified keys-before-beaconing
with "the host-beacon-engine ordering requires beaconing last". That is a *start-path* constraint
(`mesh_ctx` must be populated before `MESH_CONFIG(START)`); in recovery it is already populated. Both
Linux (`util.c:2043` then `:2108`) and morselib's own start do beacon-then-keys. The ordering is a
deliberate choice — live peers resume the moment we are back on air, so keys-first closes a window
where we advertise presence with no crypto — and is now recorded as a choice, not a constraint.

**R4 — D8 survived.** The plink retry tick is a `umac_core` timeout, cancelled only in
`mmwlan_mesh_stop()`; `mmdrv_deinit/init` tears down the driver, not umac_core timers. It genuinely
does drive non-ESTAB peers.

### And the hoist broke a build I had never been building

Adding the hoist, I built `rimba-halow-sta` for the first time and it **failed to link**:
`undefined reference to umac_ap_get_beacon`. Bisected to the shim change rather than guessed at.

Cause: `mmdrv_host_get_beacon()` calls `umac_ap_get_beacon()` **unguarded**, while `umac_ap.c` is not
compiled when `CONFIG_HALOW_AP_MODE=n`. That build only ever linked because `--gc-sections` dropped the
whole dispatcher when nothing referenced it — **an undefined symbol surviving on an accident of
dead-code elimination.** My mesh recovery call pulled `umac_mesh.o`, and with it the dispatcher, back
into the link. Fixed with the `MMWLAN_AP_DISABLED` guard the tree already uses (`umac.c:487`,
`s1g_capabilities.c:13`, `config.c:295`).

The lesson is narrower than "build everything": **a change to a shared shim can retain code that was
previously collected, so its blast radius is every build configuration, not the one you are testing.**

Re-verified on hardware after all of it: datapath **5/5 before, 5/5 after**; beacons **1705 over
190 s**, longest interruption 3.0 s. `rimba-halow-sta`, `rimba-halow-mesh`, `rimba-halow-mesh-ap` and
the fixture all clean-build.

## The code review — and two bugs that were concealing each other

A `/code-review` pass over the working diff returned eight findings. All eight were real. The first
invalidated a claim I had been repeating since the original investigation, and fixing it uncovered a
worse defect of my own.

### The premise was wrong, and it had been wrong all along

I had written — in the design doc, the worklog, the commit message and the shim comment — that
`umac_connection_handle_hw_restarted()` *"skips its whole body on a mesh node"* because its
`UMAC_INTERFACE_STA` vif-id lookup returns `MMDRV_VIF_ID_INVALID`.

It does not. `UMAC_INTERFACE_STA` is inside `VIF_STA_INTERFACE_TYPES_MASK`
(`umac_interface.c:118-120`), so `umac_interface_get_vif_id()` returns `vif_data_sta->vif_id`
**without consulting `active_interface_types`** (`umac_interface.c:492-499`) — and mesh shares that
slot. I settled it on hardware rather than by re-reading, with a temporary probe:

```
E 31250 ev umac_connection_handle_hw_restarted[1699] PROBE: BODY RUNNING vif_id=0
```

So both handlers had been running, and the connection handler was re-running
`umac_interface_reinstall_vif()` immediately after the mesh restore finished. Fixed by making the
dispatch explicit — `if (umac_mesh_is_active()) mesh else connection` — which is what
`ieee80211_reconfig()`'s per-type switch does structurally.

### That fix broke recovery, and the reason is the real find

With exclusivity enforced: **0/5 pings.** I bisected rather than guessed — not the new seq-num call,
not the BSSID change.

It was the channel. `umac_interface_set_channel_internal()` short-circuits on
`ie_s1g_operation_is_equal(&data->current_s1g_operation, s1g_operation)` and returns **success without
touching the chip**. `mmdrv_init()` wipes the chip's channel but leaves that *host* cache intact — so
my `set_channel_from_regdb()` matched the cache, programmed nothing, and reported success.

**The mesh restore had never restored the channel.** It passed every previous bench run only because
the un-enforced double dispatch meant the connection handler re-did the channel afterwards, via
`umac_interface_reconfigure_channel()` — which clears the cache first, precisely for this case.

Two bugs concealing each other: a redundant call I thought was harmless was in fact load-bearing, for
a step I thought I had implemented. Removing one exposed the other. `mesh_chip_configure_bss()` now
does not touch the channel at all, so each caller picks its own primitive and the choice cannot be made
by accident.

### The other six

| finding | outcome |
|---|---|
| abort paths left `mesh_ctx.active` true over a dead chip | `umac_mesh_abort_restore()` clears it and logs loudly — otherwise the error path *recreated* the silently-deaf state |
| start passed a vif id where an `enum mmwlan_vif` was expected → **zero BSSID** | fixed in start too, so both paths agree; peering verified unaffected |
| station ladder replayed unconditionally, but only installed under `MMWLAN_MESH_SEC_PHASE1` | ladder now gated identically |
| `peer->stad` NULL-checked at the call site but not where it is dereferenced | handled where it is used |
| per-peer failures swallowed; `restored++` regardless | failures counted and reported separately |
| chip sequence-number spaces not restored for mesh stads | added, **after** the station exists (my first placement was before it) |

Plus a scope boundary now stated rather than implied: a mesh+AP gate still panics on
`MMOSAL_ASSERT(false)` before this path is reached. That is S3.

The reviewer also independently reached the same all-zero-BSSID defect I had found, and made a sharper
point about it than I had: since the recovery path used the real MAC, start and restart were
programming *different* BSSIDs, so the bench was validating a chip configuration the node never
normally runs. That moved it from "file it" to "fix it now" — and the fix is a one-token correction,
`MMWLAN_VIF_STA` instead of `vif_id`, verified not to disturb peering.

## On-air byte check, re-run after the code review

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

## Found and deliberately NOT fixed

**`mmwlan_mesh_start()` pushes an all-zero BSSID to the firmware.** It calls
`umac_interface_get_vif_mac_addr(umacd, vif_id, mesh_mac)` — passing a `uint16_t` driver vif id into an
`enum mmwlan_vif` parameter. The mesh vif id is 0; `MMWLAN_VIF_STA` is 1 and `MMWLAN_VIF_UNSPECIFIED`
is 0, so it resolves to UNSPECIFIED, `umac_data_get_interface_vif()` returns NULL, the function returns
`MMWLAN_UNAVAILABLE` without writing, the return is discarded with `(void)`, and `mmdrv_set_bssid()`
receives six zero bytes.

Same shape as the proxy-ARP defect: a malformed field nobody reads, silent for as long as nothing
depends on it. **Left alone on purpose** — it is an unrelated change to shipped mesh start behaviour,
it could plausibly alter firmware RX filtering, and it needs its own A/B. Bundling it would have
confounded this session's bench result: if peering had regressed I could not have told which change did
it. The recovery path uses `mesh_ctx.mesh_mac`, which is correct by construction. Needs filing.

## Bench notes

- **The scratchpad was cleared mid-session**, so the serial runner vanished; I piped its
  `No such file` error into a `grep` and got a silent empty run. Second time this session for that
  family of mistake. The rule is not "don't pipe through grep" so much as **never let a run's stderr
  go somewhere you aren't reading**.
- `MMLOG_DBG`/`MMLOG_INF` do not reach the UART; only `MMLOG_ERR` does. Diagnosing the recovery meant
  temporarily elevating the per-peer logs to `MMLOG_ERR`, then reverting. Worth knowing before
  assuming a code path didn't run.

## State

S2 is done and verified. Uncommitted: `components/halow` on `feat/mesh-hw-restart-s2`, plus the fixture
changes, the scorer fix and the design doc + code map in rimba.

Still open: **S3**, the AP-side `MMOSAL_ASSERT(false)`, still unreproduced — this fixture is mesh-only
and never instantiates an AP vif. The S2 work makes S3 look smaller than it did: Linux recovers an AP
through the same generic path plus `drv_start_ap()`, so the assert reads as a morselib scoping decision
rather than a hardware limit. Unconfirmed on the chip.
