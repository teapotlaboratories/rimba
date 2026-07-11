# 2026-07-10 — Mesh-gate coverage (task #3): soak PASS; multi-STA PASS (2 concurrent STAs)

Coverage testing of the all-ESP L3 Mesh-gate (the follow-up **task #3**, after task #5 DHCP landed).
Outcome across the four scenarios: **#1 soak PASS**, **#3 multi-STA PASS** (two concurrent STAs, Linux +
ESP, distinct DHCP leases, forwarding at once), **#4 multi-hop PASS** (STA→gate→board1(relay)→far-node,
2 hops, A/B-proven — *after a morselib fix* to the forced-topology RX filter so it no longer drops the
gate's AP-client frames), plus a new **foundation PASS** (a Linux node joins the gate's SECURED
`rimba-mesh`). **#2 power-save-behind-gate — PASS for TWT doze** (the gate buffers + delivers a
mesh-originated downlink to a TWT-dozing STA: 20/20, RTT elevated ~40→260 ms = the buffering signature;
deeper WNM+powerdown keeps the STA associated but too deep for timely delivery — the expected tradeoff).
**So all four coverage scenarios are green.** (Multi-STA, multi-hop, and a #2 "forwarding gap" were each
briefly mis-reported "blocked" mid-session — respectively a self-inflicted `pkill -f` bug, the allowlist
bug (fixed with a morselib change), and a flaky-dozing-Linux-client misread — each corrected on re-test.)

Rig (same as task #5, boards mapped by efuse MAC not ttyACM): gateway `rimba-halow-mesh-ap` ACM0
(`…ef:a4`, mesh 10.9.9.136 + AP 192.168.12.1 + DHCP server), board1 `rimba-halow-mesh` ACM1 (`…f9:40`,
mesh 10.9.9.100), board2 STA `rimba-halow-sta-perf` ACM4 (`…f0:08`, PPK2-powered). Morse fw 1.17.8.

## Scenario 1 — SOAK (PASS)
Cold-booted the rig, then drove `ping -c 450 10.9.9.100` (~7.5 min continuous end-to-end:
STA→AP→gate `ip_forward`→mesh→board1→reply) from the STA console while sampling the gateway heartbeat
every 30 s. Harness `scratchpad/soak.py`; logs `scratchpad/soak_ttyACM{0,1,4}.txt`. All checks green:

- **449/450 replies, 0% loss** (integer; the single miss is the seq=1 cold-start warmup), all **ttl=63**.
- **No crash on any board**; gateway **uptime monotonic** (no reboot).
- **mesh_peers=1 throughout** (no HWMP flap); **ap_stas=1 throughout** (no STA disassoc); board1
  **estab_peers=1 throughout**.
- **Gateway heap flat: drift = +32 B over 7.5 min** (91 samples, first 8 587 616 / last 8 587 648 /
  min 8 586 868; band ~800 B) — **no leak**.
- RTT min/avg/max = **25 / 37 / 321 ms**.
- Bonus: the rig was left running afterward and re-observed at uptime ~20 min — heap still 8 587 648,
  mesh_peers=1, ap_stas=1 (extra passive stability confirmation).

Verdict: the L3 mesh-gate is stable under sustained end-to-end traffic — no leak, flap, disassoc, or loss
(bar the one cold-start warmup miss). A multi-hour soak remains a separate longer run.

## Scenario 3 — MULTI-STA (PASS: two concurrent STAs, one Linux + one ESP)
Goal: two STAs behind the gate each take a distinct DHCP lease and ping 10.9.9.100 concurrently — directly
exercises the task-#5 DHCP pool + concurrent forwarding. Only board2 is an ESP STA, so the 2nd STA is a
Linux HaLow node: **chronite** as an S1G infrastructure STA (`wpa_supplicant_s1g`, config
`scratchpad/wpa-rimba-sta.conf`: SSID `rimba-ping`, key_mgmt=SAE, sae_password `rimbahalow`, ieee80211w=2,
op_class 68/ch27, **`sae_pwe=2`**). Result:

- **chronite associates (SAE H2E):** `wpa_state=COMPLETED`, bssid `e0:72:a1:f8:ef:a4`, key_mgmt=SAE,
  pairwise=CCMP. **This proves the previously-unproven Linux-S1G-infra-STA → ESP(morselib)-SoftAP SAE
  path** (the reverse dirs were already proven; this direction was new). The one real requirement:
  **`sae_pwe=2`** (H2E) — the AP advertises `[SAE-H2E]`; a default hunt-and-peck commit is rejected
  (`status_code=1`), H2E is accepted.
- **DHCP interop with a standard Linux client:** chronite's `dhcpcd` got `leased 192.168.12.2 from
  192.168.12.1` (7200 s) + default route via the gate — the task-#5 DHCP server works with a non-ESP
  client, not just the ESP `dhcpc`.
- **Distinct leases from the pool:** with chronite holding **.2**, board2 (ESP STA) then leased
  **192.168.12.3** — the pool hands out distinct addresses; gateway heartbeat shows **`ap_stas=2`**.
- **Concurrent end-to-end forwarding:** both STAs pinged 10.9.9.100 at the same time —
  **chronite (.2): 15/15, 0% loss (avg 34 ms); board2 (.3): 15/15, 0% loss, ttl=63** → **30/30 total,
  0% loss**, both routing STA→AP→gate `ip_forward`→mesh→board1 concurrently.

**Root-cause of the mid-session "blocked" mis-report (kept as a lesson):** the first H2E retries appeared
to fail because the driving ssh commands used **`pkill -f wpa_supplicant_s1g`** — `-f` matches the whole
command line, so it matched and killed the *remote shell itself* (its cmdline contains that string)
*before* wpa_supplicant ever launched. So those attempts produced no output and never ran the test; it
looked like an association failure and the repeated ssh attempts + a `-dd` log wedged chronite (needed a
power-cycle). Fix: kill by `killall wpa_supplicant_s1g` (or exact PID), never `pkill -f <string-in-your-own-cmdline>`.
Once corrected, `sae_pwe=2` associated on the first try. (Same self-match class of bug also bit
`pkill -f ppk2_hold.py` locally.)

## Foundation (for scenarios 2 & 4) — Linux node joins the ESP gate's SECURED mesh — PASS
Both scenarios need a Linux node in the gate's `rimba-mesh` (only gate+board1 are ESP mesh nodes). The
gate mesh is SAE+AMPE; the password is **`rimbamesh2026`** (morselib `umac_mesh.c:81`
`MESH_SAE_PASSWORD`, *"matches the Linux sae_password for cross-vendor interop"*), mesh_id `rimba-mesh`.
Config `scratchpad/wpa-rimba-mesh.conf` (wpa_supplicant_s1g, mode=5, key_mgmt=SAE,
sae_password=rimbamesh2026, op_class 68/ch27, dtim_period=1, user_mpm=1). **chronite (Pi5) joined and
peered with BOTH ESP nodes within 1 s** (`iw station dump` → gate `e2:…:ef:a4` + board1 `e2:…:f9:40`),
mesh-ping board1 4/4. So a Linux node joining the ESP mesh-gate's secured mesh works — a new capability.
(Careful-bring-up notes below: never `killall wpa_supplicant_s1g` on a node whose *wlan0* is also managed
by wpa_supplicant_s1g — it drops the LAN; a fresh node needs no kill at all.)

## Scenario 4 — MULTI-HOP (STA→gate→relay→node) — PASS (after a morselib fix)
Plan: force a 2-hop line gate→board1(relay)→far-node (chronite) so a STA behind the gate reaches the far
node via board1. On a close bench everything is in RF range (= 1 hop), so the topology must be *forced*;
the mechanism is the mesh **peer allowlist** (`mmwlan_mesh_set_peer_allowlist`). A test toggle
`MESH_FORCE_LINE` on the gate sets the allowlist to board1's mesh MAC → the gate peers ONLY with board1.

**Bug found + fixed (the reason this was first reported blocked):** with the allowlist set, board2 (the
AP-client STA) could not associate — it looped "STA connecting…", `ap_stas=0`. Root cause: the
forced-topology RX drop in `morselib umac_datapath.c` (`umac_datapath_process_rx_data_frame_after_reorder`)
was guarded by `umac_mesh_is_active() && !umac_mesh_peer_allowed(TA)` — always true on a mesh+AP node, so
it dropped **every** RX data frame whose TA wasn't allowlisted, including board2's EAPOL 4-way frames.
**Fix:** move the check *after* `mesh_ctrl_present` is parsed (the 802.11s Mesh Control bit) and gate on
it: `if (mesh_ctrl_present && !umac_mesh_peer_allowed(TA)) drop;`. Now it filters only 802.11s mesh
frames, never the concurrent AP's client frames (an AP client's data has no Mesh Control header). This
matches the drop's own stated intent ("drop mesh data frames…") and doesn't regress mesh-only nodes
(`rimba-halow-mesh-perf`), whose frames all carry mesh control. (Submodule `components/halow`, branch
`feat/mesh-ap-concurrency`.)

**Verification (with the fix + allowlist active):**
- **Fix works:** board2 associates immediately (link up, lease .2) *with* `MESH_FORCE_LINE` set —
  previously impossible.
- **Forced line holds:** gate `mesh_peers=1` (board1 only); chronite's plink to the gate stays **LISTEN**
  (never ESTAB — rejected), to board1 is **ESTAB**; chronite reaches the gate 10.9.9.136 3/3 despite no
  direct peer → **via board1 (2 hops)**.
- **Multi-hop end-to-end:** board2 (STA, 192.168.12.2) → gate → board1(relay) → chronite (10.9.9.60) =
  **11/12, ttl=63** (1 warmup miss). *(chronite needs a return route `192.168.12.0/24 via 10.9.9.136` —
  the same mesh-node return-leg config as board1's gw in task #17; not a bug, expected mesh routing.)*
- **A/B proof it's 2-hop:** with the relay (board1) taken off the mesh, board2→chronite = **0/8, 100%
  loss** (the gate can't reach chronite directly). Relay back = works. So the path is genuinely
  STA→gate→relay→node.

## Scenario 2 — POWER-SAVE BEHIND THE GATE — PASS (TWT doze); deep WNM+powerdown trades off downlink
Question: does the gate's AP buffer + deliver a **mesh-originated** downlink to a STA that is **dozing**
behind the gate? Rig: gate + board1 mesh; **chronite = mesh-side originator** (10.9.9.60, return route
`192.168.12.0/24 via 10.9.9.136`); **board2 = the STA under test** with a *controllable* doze
(`rimba-doze-hold`, temporarily given a DHCP client so it's pingable, TWT 1 s wake, 90 s holds; changes
reverted after). board2 stayed **ASSOCIATED throughout both doze modes** (0 disassoc events, gate
`ap_stas=1`). chronite→board2(192.168.12.2), each mode:

| board2 state | result | RTT | reading |
|---|---|---|---|
| **awake** (PS off, perf app) | **12/12, 0%** | ~40 ms | baseline |
| **TWT doze** (1 s scheduled wake) | **20/20, 0%** | avg **260 ms**, max 1139 ms | **buffered + delivered on the TWT SP** ✅ |
| dyn-PS (leftover, PS on) | 1/12 | ~350 ms | poor |
| **WNM+powerdown** (chip off between DTIM) | **0/20** | — | associated but too deep to receive within 3 s |

**Result: PS-behind-gate WORKS for TWT** — the gate's AP correctly buffers the mesh-originated downlink and
delivers it on the dozing STA's TWT service period (0% loss, RTT elevated ~6× to the wake interval = the
buffering signature). Deeper modes (WNM+chip-powerdown) keep the STA **associated** but the radio sleeps
too deeply/irregularly for timely unicast delivery (0% within a 3 s timeout) — the expected deep-sleep
tradeoff, **not** a gate defect (awake + TWT both prove the gate delivers).

### Correction (kept for the record): the earlier "mesh→AP-client forwarding gap" was a MISDIAGNOSIS
A first pass (chronosalt-mesh → chronite-Linux-STA) gave 0% and was wrongly written up as a gate forwarding
bug. Re-testing with a fresh ESP client + packet-level gate instrumentation showed the gate forwards
correctly (awake board2: 12/12; gate log `RX_MESH → AP_TX(da=board2) → RX_AP`). The 0% was the flaky Linux
STA client, which — as the doze table above now shows — was almost certainly just **dozing** (morse Linux
default PS, uncontrollable via `iw`). No gate bug; **no gate code change**. Diagnostic instrumentation was
removed; the doze-hold test tweaks were reverted.

## Bench teardown (clean)
ESP rig radio-silenced (`rimba-hello` ×3 on ACM0/1/4); board2 powered off (`ppk2_hold.py off` — ACM4
gone); chronite silenced (`killall wpa_supplicant_s1g` + `wlan1` down, confirmed) and reachable.
chronite was power-cycled once mid-session (safe — `/tmp` is tmpfs holding only throwaway files, cf.
[[dont-reboot-linux-mesh-nodes]]) after the `pkill -f` self-match wedged it. No commits (weekday work
hours); changes/docs held in the working tree.
