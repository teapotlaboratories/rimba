# Full bench run of the renamed suite + the test-apsta-ap up-marker fix

**2026-07-18 · bench run + a root-caused false-FAIL**

Ran the whole regression suite on the bench against the restructured/renamed (`test-*`) tree —
T0, T1, T2, then dscycle + tp. T0/T1 came up green; T2 first showed **3 FAILs** that all
root-caused to a single **false-FAIL**: the ESP SoftAP support app (`test-apsta-ap`) is fully
functional but its `ap-ready` up-marker no longer reached the orchestrator, so three AP-gated
tests aborted before the STA ever ran. Fixed by emitting the marker before the call that stalls
the print stream; T2 went **15/15**.

## Bench state

Full bench present: board0 + board1 (bus-powered) + board2 (PPK2-rail, via `tools/ppk2_hold.py`),
the PPK2, the C6 trigger on `/dev/ttyUSB0` (running `MODE_TRIGGER`, pulsing D5 every 30 s), and
chronite reachable over ssh. Bench pinned at Morse fw 1.17.8 / chip 0x0306 / S1G ch27.

## Results

| Tier | Result |
|---|---|
| **T0 build** | ✅ 28 pass / 0 fail / 1 skip / 27 XFAIL (proto1 known-broken; `test-c6-trigger` esp32c6-excluded) |
| **T1 smoke** (board2) | ✅ 12 pass / 0 fail / 2 skip (the 2 sleep apps) |
| **T2 on-air** | first run **12 pass / 3 FAIL**; after the marker fix **15 pass / 0 fail / 0 skip** (12 feature tests + 3 silence) |
| **dscycle** | ⚠ **INCONCLUSIVE** — 1/2 deep-sleep→wake→reassoc cycles in budget (one genuine cycle, reconnect 4590 ms + a real deep-sleep gap; the C6 D5-wake is known-flaky and falls to the 60 s backup timer, so the 2nd cycle didn't fit). Non-gating — an honest flaky-wake non-result, not a code regression. Its AP came up cleanly (the marker fix applied here too). |
| **tp** (--ap esp) | ✅ **4 pass** — No-PS 62.7 mA (validity anchor, in the 55–72 window, associated) · Dyn-PS 23.9 mA ≤ 32 band · TWT 23.2 mA (recorded, not scored) · WNM+powerdown 17.1 mA ≤ 24 band. The ESP AP (the fixed `test-apsta-ap`) came up, so the fully-automated `--ap esp` path ran end-to-end. |

Net: every tier **green** except dscycle, which is honestly **INCONCLUSIVE** (flaky C6 wake). All exit
codes 0 (INCONCLUSIVE does not gate). The single `test-apsta-ap` marker fix unblocked all five
AP-gated cases (the 3 T2 tests + `tp --ap esp` + `dscycle`'s AP).

The 3 first-run FAILs were exactly the tests whose AP is `test-apsta-ap` on board0 — `ap-sta-ping`,
`twt`, `multi-twt` — all failing at an identical ~52 s with `support role 'ap' (board0) did not come
up: up-marker 'ap-ready' not seen within 25s`. Every mesh/gate/Linux-AP test passed.

## Root cause — a false-FAIL, the AP is fine

The failure detail already showed the AP had booted (chip 0x0306, fw 1.17.8, `netif up=1`). Direct
reproduction on board0 (full raw console capture) pinned it precisely:

- The AP prints `TEST|INFO|AP static IP 192.168.12.1, netif up=1` at **3.4 s**, then the console goes
  **completely silent** — no panic, no reset — and the `ap-ready` line (the very next executable
  statement) never appears.
- Two injected markers (A: just before `assign_static_ip` returns; B: back in `app_main` right after)
  **both printed** at 3.4 s, yet `ap-ready` two lines later did not. Adding statements let execution
  proceed *further in source* but it still went quiet at the same ~3.4 s **wall-clock** → a
  **time-based stall of the app_main task** right as the AP netif is brought up
  (`esp_netif_action_connected`), not a linear-code bug.
- It reproduced on **all three boards** including fully-wired board2 (so not the WAKE/BUSY wiring),
  and it is **not** the rename (the `test-apsta-ap` code path is byte-identical to the pre-rename
  `regtest-apsta-ap` modulo the `REGTEST→TEST` string rename), and **not** the close-bench TX cap
  (removing `mmwlan_override_max_tx_power(1)` did not change it).

**The decisive test:** with board2 running the "hung" AP, a real STA (`test-apsta-sta` on board1)
**associated (SAE, 3.75 s) and got 15/15 ICMP replies**, RESULT=PASS. So the SoftAP is fully alive
and serving — only the **app_main task stops emitting console output** after the netif comes up, so
the `ap-ready` up-marker never reaches the orchestrator, which then declares the support role dead
and aborts before flashing the STA. A textbook false-FAIL.

This is a **real, separate firmware issue** (app_main stalls after `esp_netif_action_connected` in AP
mode on the current `components/halow`; it did *not* stall in the 2026-07-17 14:00 run, so it was
surfaced by a clean rebuild against the currently-modified submodule) — but it does not affect the
SoftAP's function.

## The fix (interim, harness-side)

Emit the `ap-ready` up-marker **before** `assign_static_ip()` (i.e. before the netif-up call that
stalls the print stream), instead of after it — `firmware/test-apsta-ap/main/app_main.c`. The static
IP is still pinned microseconds later, and the orchestrator only starts flashing the STA tens of
seconds after seeing the marker, so the AP is answering ICMP long before any association. The marker
still means "the SoftAP has started"; the AP's function is unchanged (proven by the 15/15 STA run
above). The comment in the app flags the underlying app_main stall as a separate issue to root-cause.

One firmware file changed; it unblocks five gated cases at once: the three T2 tests plus `tp --ap esp`
and `dscycle` (both wait on the same `ap-ready` marker).

**Re-run after the fix:** `ap-sta-ping`, `twt`, `multi-twt` all PASS → T2 **15 pass / 0 fail / 0 skip**.

## Open follow-up

- **Root-cause the app_main stall.** `app_main` stops producing console output right after
  `esp_netif_action_connected` brings the AP netif up (the SoftAP itself keeps running). Candidates:
  a main-task stack overflow exposed by the rebuild's slightly different layout, or an esp_netif/
  morselib AP-path change in the currently-modified `components/halow`. It worked in the 2026-07-17
  14:00 run, so a diff of the submodule state (or bumping `CONFIG_ESP_MAIN_TASK_STACK_SIZE`) is the
  place to start. The interim marker move keeps the suite honest in the meantime.

## Update 2026-07-21 — RESOLVED (a mid-migration artifact; workaround reverted)

The stall **does not reproduce on the shipped 2.12.3 SDK** (`components/halow` @ `7d7f76ad`, on `main`).
Reproduced on-bench on board2 with an instrumented build (a heartbeat + a stack/heap probe right after
the netif-up), captured from a fresh boot **twice**:

- `app_main` printed **straight through** `esp_netif_action_connected`: `ap-ready` and `netif up=1` at
  3.8 s, then `DIAG post-netif: heap=8.6 MB, main_stack_hwm=1528`, then a **heartbeat every 1 s for 30+ s
  past the netif-up** — no silence, no crash. Consistent across both runs (30 heartbeats total).
- The main-task stack high-water-mark is **1528 B free** (of 3584) — comfortable, so the suspected
  main-task **stack overflow is ruled out**; and there is no `esp_pm`/light-sleep on the ESP build
  (`CONFIG_PM_ENABLE` unset; the only tickless-idle code is morselib's own ARM FreeRTOS, not compiled for
  the ESP32) — so the "console powered down by light-sleep" theory is ruled out too.

**Diagnosis:** the 2026-07-18 stall was an artifact of the **mid-migration** `components/halow` (the
"currently-modified" submodule *during* the 2.10.4→2.12.3 forward-port). The exact intermediate cause
can't be pinned — that uncommitted build state no longer exists — but it is **absent on the merged 2.12.3
that ships**, which is the honest, verifiable conclusion (an unrooted mechanism on a build that's gone,
vs. a proven-clean shipped SDK).

**Fix:** the interim workaround is **reverted** — the `ap-ready` up-marker is back at its natural position
(after `assign_static_ip`, so it truthfully means "SoftAP up + static IP pinned"); the diagnostics were
removed (`firmware/test-apsta-ap/main/app_main.c`). **Verified end-to-end:** `make test-t2
TEST="ap-sta-ping"` → **PASS** (213.5 s) — the orchestrator saw `ap-ready` in its natural position and the
STA associated (SAE) + pinged. Bench left radio-silent. (Uncommitted; held for a post-17:00 commit.)
