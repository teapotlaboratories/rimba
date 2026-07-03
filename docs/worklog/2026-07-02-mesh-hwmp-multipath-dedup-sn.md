# 2026-07-02 — HWMP multi-path reply-dedup / per-reply-SN fix (port from net/mac80211)

Fixes morselib's HWMP path selection **flapping** in a multi-path topology, found by the P6c
routing-decision test (`2026-07-02-mesh-p6c-airtime-metric.md`). After this fix the airtime metric can
cleanly *decide* a route — the originator settles on the lower-metric path and holds it.

## The bug
In a triangle where a destination is reachable both **directly** and **via a relay**, the target replies
to *every* copy of one flooded PREQ (morselib has no received-PREQ reply-dedup) and stamps each PREP with
`target_sn = ++mesh_hwmp_sn` — a **per-reply** increment (`umac_mesh.c:2264`, pre-fix). So the direct and
relayed PREPs of a single discovery carry *different, climbing* SNs; the originator's fresh-info rule
(`mesh_path_update`) then takes whichever arrived with the newer SN — **short-circuiting the metric
compare** (`SN_GT` wins before `metric <` is even evaluated). Observed: the installed path oscillated
direct(27307)↔relay(5462) roughly every 7 s, direct-dominant (4:2 in a 40 s window). Single-path
multi-hop was unaffected (no competing PREQ copies).

## The mac80211 reference (re-grepped on chronite `rpi-linux @372414fd`)
`hwmp_preq_frame_process` (`mesh_hwmp.c:660`) does **not** dedup by replying once; instead the target-SN
bump is **time-rate-limited** (`:685-701`):
```c
if (SN_GT(target_sn, ifmsh->sn)) ifmsh->sn = target_sn;     // adopt a newer requested SN
if (time_after(jiffies, ifmsh->last_sn_update + net_traversal_jiffies(...))) {
    ++ifmsh->sn; ifmsh->last_sn_update = jiffies;            // bump AT MOST once per net-traversal window
}
target_sn = ifmsh->sn;
```
So every reply within one discovery (the copies arrive within ms) shares the **same** `target_sn`, and the
originator selects by **metric**. `hwmp_route_info_get` (`:499-509`) additionally applies a `mult_frac(new_
metric, 10, 9)` (~11%) hysteresis on a same-SN copy that arrived via a **different next hop**, and returns
0 (⇒ caller replies/forwards nothing) for a stale/duplicate copy — the only dup suppressor.

## The fix (morselib `umac_mesh.c`, 5 edits)
1. **`MESH_HWMP_NET_TRAVERSAL_MS = 500`** (near `MESH_PATH_REFRESH_MS`) — the ms analog of
   `net_traversal_time`; >> a flood's arrival spread (tens of ms), << the ~24 s refresh.
2. **`mesh_hwmp_last_sn_update_ms`** global (`ifmsh->last_sn_update` analog).
3. **`mesh_metric_mul_10_9()`** helper (64-bit intermediate + saturation).
4. **`mesh_path_update` now returns `bool fresh`** with the mac80211 fresh test: inactive / strictly-newer
   SN / (equal SN AND metric better — by the 10/9 margin when the next hop differs, raw otherwise). The
   PREP-branch caller ignores the return (unchanged).
5. **PREQ branch:** `if (!fresh) return;` gates the PREP reply *and* the re-flood (mac80211's dup
   suppressor); the target SN becomes our own `mesh_hwmp_sn`, adopted-up to a higher request then bumped
   at most once per `MESH_HWMP_NET_TRAVERSAL_MS` (`since < 0` reproduces the `time_before` wrap guard).

Legitimate paths preserved: a genuinely newer `orig_sn` PREQ (SN_GT branch) still replies + floods; the
PREP + PERR branches and the PREQ *originate* SN bump (`++mesh_hwmp_sn` for `orig_sn`) are untouched.

## Verification (3-board triangle, software RSSI override, no RF work)
Same rig as the P6c decision test: board0/1/2 all peered (triangle), a compile-time override forcing the
**direct** board1↔board2 link weak (−90 → 27307) while the board1→board0→board2 relay stays strong
(2×2731 = 5462); board1 pings board2; a TEMP `printf` in `mesh_path_update`'s fresh block logs each
installed path.

**Result — settles on the relay, no flapping:**
```
INSTALL via board2 (DIRECT) 27307  →  INSTALL via BOARD0 (relay) 5462   (direct arrives first; relay, same SN, wins on metric)
uptime 75–95s: (no installs)                                             (path holds on the relay ~20 s)
INSTALL via board2 (DIRECT) 27307  →  INSTALL via BOARD0 (relay) 5462   (next discovery, settles on relay again)
```
Every discovery round settles on the relay (5462) and holds stable ~20 s until the next refresh — vs the
pre-fix ~7 s direct-dominant oscillation. Ping 0% loss throughout. The brief direct→relay transient per
round is normal reactive HWMP (the 1-hop PREP arrives first, the better-metric relay PREP then replaces it
at the same SN). Build: `make build APP=rimba-halow-mesh` compiles + links clean (PREP-branch bare call —
no unused-result warning). TEMP verify-scaffolding reverted after.

## Post-review hardening (adversarial code review)
An adversarial review (3 dimensions × per-finding verify) of the uncommitted diff found 4 issues, all
fixed + re-verified on the bench:
1. **Newer-SN adopted unconditionally.** mac80211 `hwmp_route_info_get` ORs the metric test independently
   of the SN test, so it applies the 10/9 different-next-hop hysteresis to *any* not-older SN — not just
   equal-SN. The first port did `else if (SN_GT) fresh=true`, adopting every newer-SN copy; that is what
   produced the per-round direct→relay transient. Rewrote the fresh test: reject if the stored SN is
   strictly newer; otherwise adopt only via the SAME next hop or a >10%-better metric (uniform across
   equal/newer SN).
2. **PREP branch not freshness-gated.** The `if (!fresh) return;` gate was on the PREQ handler only;
   mac80211 gates PREP processing too. Added it to the PREP branch — no duplicate PREP re-forwarding.
3. **`==0` SN-timestamp sentinel** aliased a real `mmosal_get_time_ms()==0` (the 32-bit ms clock passes
   through 0 every ~49.7 days; also early boot) → a spurious extra SN bump. Replaced with a separate
   `mesh_hwmp_sn_bumped` bool.
4. **`tx_time` associativity** aligned to Linux's `(10*tfl)/rate` (was `10*(tfl/rate)`); the `>>16`
   absorbs the ≤1-unit intermediate difference so the tier values are unchanged (27307/6827/2731).

**Re-verified (triangle):** with fix #1 the per-round transient is **gone** — after the single initial
install of an empty path, refreshes install the **relay only** (the newer-SN direct copy is now rejected
by the hysteresis, matching mac80211). Settles on the relay (5462), stable ~20 s, ping 0% loss (relay 2 :
direct 1 in a ~50 s window, the direct being only the first-ever install).

## Code-map
Rows in `docs/mesh-ap/rimba-mesh-80211s-code-map.md` (HWMP section); Linux refs are @pinned `rpi-linux`
and must be re-grepped on the chronite reference before the code-map is considered final.
