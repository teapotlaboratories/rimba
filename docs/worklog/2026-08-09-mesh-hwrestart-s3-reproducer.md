# Mesh hw_restart S3 — reproducing the AP assert, and finding a second defect behind it

**Date:** 2026-08-09
**Task:** reproduce the AP-side `hw_restart` assert — the last part of this epic never reproduced —
and settle whether its Interrupt WDT is the same fault as the relay FIX-1 one.
**Outcome:** reproduced, and it is **two** defects. The WDT is **not** FIX-1, and proving that changed
S3's scope. The reproducer is merged; the fix is not written.

---

## The reproducer

`test-mesh-hwrestart AP_VIF=1` brings an AP vif up beside the mesh — same SSID, PSK, channel and
`max_stas` as the shipped gate, so this reproduces the real configuration rather than an approximation
— then triggers a restart. It reproduces on the first try.

```
TEST|INFO|ap-vif|up alongside the mesh (ssid="rimba-ping" chan=27), tx vif pinned to STA
TEST|STEP|data-before|PASS|4/5 replies from the peer
TEST|INFO|s3-expect-panic|...
E hw_restart_evt_handler[77] Unable to recover from hardware restart with AP interface active
MMOSAL Assert, CPU 0 (current core) backtrace
```

## The WDT question, which is what the session was actually for

The first reproduction showed `Guru Meditation Error: Interrupt wdt timeout on CPU0` cycling every
~1.5 s, and the obvious suspicion was that S3 and FIX-1 (the relay Interrupt-WDT, root-caused as
hw_restart's SPI-host teardown being fatal under `bus_lock`) were one problem. If true, S3 would be a
much larger stage than "remove an assert".

**They are not the same, and the code settles it without a bench run:**

- The AP check is the **first statement** in `hw_restart_evt_handler()`. `mmdrv_deinit()` — the
  SPI-host teardown behind FIX-1 — sits further down and **never executes** on this path.
- The assert itself fails **cleanly**: `MMOSAL Assert`, backtrace, `rst:0xc`. No watchdog is involved.

So the WDT belongs to the boot that *follows* the reset, not to the assert. Two faults both reporting
"Interrupt wdt" is exactly the coincidence that gets unrelated bugs merged into one wrong theory, and
this one was heading that way — the design doc had already recorded the speculation as a finding.

### So S3 is two defects

| stage | what | consequence |
|---|---|---|
| 1 | AP vif active → assert → clean reset | the documented defect; fix = recover the AP arm as Linux does |
| 2 | the reset **can** leave the node in a boot-time ISR watchdog loop | once entered, it cannot reboot its way out |

Stage 1's fix **avoids** stage 2 by never rebooting, but does not fix it — stage 2 stays reachable by
any reset landing mid-restart. Tracked separately for that reason.

---

## What the review caught, and why it mattered

Six findings. One was a trap worth recording in full.

**The arm disabled the datapath gate for the wrong reason, and disabling it would have made the fixture
fail the fix it exists to certify.** It neutralised the *before* ping but left the *after* one scoring
— so once S3 lands and the board survives, `data_ok` is false by construction and the fixture prints
`TEST|RESULT|FAIL` for a working fix.

**The stated reason was also wrong.** I had written "two vifs with none of the gate's per-vif RX demux,
so mesh RX is unwired". RX was never the problem: `mmhalow` registers a plain rx cb and
`umac_datapath` falls back to it for any vif when no ext cb is registered. What actually breaks is
**TX** — with a mesh (STA host-slot) *and* an AP vif both valid, morselib cannot infer the egress vif,
logs `Unable to infer VIF ID`, and drops the packet.

The fix is one line, `mmhalow_set_tx_vif(MMWLAN_VIF_STA)`, which `test-mesh-ap-gate:291` already does
for the identical shape. With it the arm keeps a real before/after gate — bench-confirmed,
`data-before` passes 4/5 with the AP vif up.

**That one line also answered a question the first reproduction could not.** With the TX vif correctly
pinned, the boot-time WDT *still* reproduces (26 consecutive cycles) — so it is not an artifact of the
mis-configured fixture. Fixing the diagnosis strengthened the finding instead of dissolving it.

## Claims I had to withdraw

Three, all mine, all "a claim outliving its evidence":

1. **"Every boot after that reset dies."** Wrong — it is **intermittent**. A third run on the same
   binary and rig recovered cleanly through repeated assert cycles: reboot, mesh and AP back up,
   re-peered, `data-before` 4/5, assert again on the next trigger. What is established is that the
   loop is reachable and self-sustaining *once entered*, not that it always follows.
2. **"Permanently bricked until power-cycled."** Never tested. Only an ESP *software* reset is known
   not to clear it; the power-cycle question needs board2 on the PPK2 rail, since board1 is directly
   USB-powered.
3. **"Plausibly the same teardown path as FIX-1."** Disproved by the code above — but only the PR
   description had been corrected, while the design doc (the thing the fix will be designed from) still
   carried the guess. Fixed there too.

The pattern in all three: I wrote a section, later learned something that contradicted it, and updated
the *nearest* text rather than every place the claim lived.

## Two build traps

- **The partition growth is `--gc-sections`, not "the arm compiles AP mode in".**
  `CONFIG_HALOW_AP_MODE=y` predates the arm, so those sources were always built; referencing
  `mmwlan_ap_enable()` makes the linker **retain** the AP/hostap subgraph it used to discard. The
  fixture now carries its own 2 MB `partitions.csv`. The wrong explanation would have sent a reader
  toggling `HALOW_AP_MODE` to recover flash.
- **`TEST_AP_VIF` is a CMake *cache* variable, so the panic arm is sticky.** After one `AP_VIF=1`
  build, a later plain `make build` inherits it and silently produces the deliberately-asserting
  binary. Pass `AP_VIF=0` or wipe the build dir; omitting it is not enough. Documented at the gate and
  in the manifest entry.

## A process note worth keeping

I mis-read a **failed** build as successful: my grep filtered out the size-check error, and I took a
`strings` hit on the stale binary as confirmation. The honest signal was the *absent*
"Project build complete". Second time this session that filtering hid a failure — the first was piping
a serial runner's `ModuleNotFoundError` into the same grep that was scraping its output.

## State

Merged: `dc68f25` (reproducer) + `e748626` (review follow-ups), PR #56. `make test-unit` 53/53, no
manifest drift, both arms build clean, bench radio-silent.

**S3's fix is not written.** Next is stage 1 — an AP arm in `hw_restart_evt_handler()` and deleting the
assert. One structural thing to resolve first: the dispatch is currently
`if (umac_mesh_is_active()) mesh else connection`, which has **no room for the gate's mesh + AP case**.
That needs revisiting rather than extending.
