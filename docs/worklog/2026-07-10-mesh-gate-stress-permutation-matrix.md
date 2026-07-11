# 2026-07-10 — Mesh-gate stress test: full source→destination permutation matrix + flood

A whole-network stress test of the all-ESP L3 Mesh-gate: exercise **every source→destination permutation**
across both subnets (mesh `10.9.9.0/24` ↔ AP `192.168.12.0/24`, bridged by the gate's `ip_forward`), then
hammer it with concurrent and flood load to find the limits. Follows the task-#3 coverage work (same gate).

## Rig
Mesh (`10.9.9.0/24`): gate `.136`, board1 `.100`, chronite `.60`, chronogen `.40`, chronosalt `.50` (Linux
mesh nodes via `wpa_supplicant_s1g`, SAE `rimbamesh2026`, TX power lowered to 3 dBm, return route
`192.168.12.0/24 via 10.9.9.136`). AP (`192.168.12.0/24`): gate `.1`, board2 `.2` (ESP STA, IPERF console).
**chronium deliberately NOT used** (kept as the dedicated sniffer, off-air). Originators: chronite,
chronogen, chronosalt (ssh `ping`), board2 (serial console `ping`); gate + board1 are responders only.
Harness: throwaway `scratchpad/matrix.py` + `stress.py` + `stress2.py` (ssh floods + serial console).

## Results

### 1) Sequential connectivity matrix (cold, one pair at a time) — 15/15 CONNECT
Rig = gate, board1, chronite, chronogen mesh + board2. Every source→dest pair passes traffic; a few cells
were 5/6 — **first-packet warmup only** (ARP / HWMP path setup on a cold flow), not steady-state loss.
board2 (ESP AP-client) → everything was 6/6.

### 2) Concurrent load (40 pkts/pair, 4/s, all pairs at once) — ≈0% LOSS
Under ~11 simultaneous ping-storms: **14/15 pairs 0% loss, 1 pair dropped a single packet** (chronite→board1
39/40) = **1 lost packet in 600**. The gate's cross-subnet forwarding + the mesh HWMP handle concurrent
multi-source traffic cleanly; the sequential run's 5/6 warmup losses amortize away over 40 packets.

### 3) Flood saturation (30 s, ~19 concurrent flood streams + board2) — 80.8% DELIVERED, GRACEFUL
Added chronosalt (5 mesh nodes). Aggregate **4158/5145 pkts delivered = 80.8% (19.2% loss)**. The loss is
**structured, and the gate is the bottleneck**:
- **To/through the gate = worst:** `→gate.mesh/gate.ap` 32–44%, `→board2` (traverses the gate `ip_forward`)
  43–62%. Expected — the gate's *single* MM6108 does mesh RX + AP TX + forwarding at once and saturates first.
- **Mesh-to-mesh (not through the gate) = robust:** 3–13% loss.
- **board2 (AP→mesh) ≈0–1%** even under flood (it ran 20/s, not full flood; its packets got through).
- **Graceful degradation:** ~5× the earlier load only dropped delivery ~100%→~81%, **no node crashed**, every
  path still passed traffic. A shared 1 MHz S1G medium with 6 nodes flooding is simply oversubscribed — it
  sheds load rather than collapsing.

## Findings
- **Every source→destination permutation works** across both subnets through the gate.
- **The gate is the throughput ceiling** (single-radio mesh+AP+forward); mesh-only traffic scales better than
  cross-gate traffic. Not a defect — an architectural property of the all-in-one gate (the planned L2-bridge /
  #1 port would not change this single-radio limit).
- **chronosalt (Pi Zero 2 W) is power-marginal:** it *reboots* (browns out) intermittently when the HaLow
  radio (`wlan1`) is brought up — the fgh100m board's radio-up current exceeds the supply margin. It recovers
  (~1 min) and can join, but is unreliable and sent the fewest packets under flood (weakest node). Diagnosed
  by `uptime`/`/proc/uptime` showing a fresh boot right after it dropped off the LAN; `vcgencmd` unavailable
  (`/dev/vcio` absent) so undervoltage couldn't be confirmed directly, but the reboot-on-radio-up signature is
  unambiguous. Recorded in `rimba-bench-devices.md`.

## First attempt (superseded, kept as a lesson)
An earlier run used chronium (as a mesh node) + chronite as a *Linux STA* AP-client. Its matrix showed
**everything→chronite dead** while chronite→gate/board1 worked — the **Linux-STA power-save downlink** effect
(the Linux morse STA dozes, PS uncontrollable via `iw`; it receives replies to its own fast flows but not cold
downlink) — the same effect measured cleanly in the coverage worklog's scenario 2. The rig above avoids it by
using only *mesh* Linux nodes + board2 (an ESP AP-client, PS-off, awake) as the one AP endpoint.

## Bench teardown
All radio-silent: ESPs `rimba-hello`, board2 off, all Linux `wlan1` down (chronium was already off-air). Linux
nodes left powered + idle. NB the Pi Zeros' *wlan0* mgmt is itself run by `wpa_supplicant_s1g`, so
`killall wpa_supplicant_s1g` drops their LAN — only `wlan1` was downed on them. No commits (weekday hours).
