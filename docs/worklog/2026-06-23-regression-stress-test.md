# Worklog — 2026-06-23 — Regression + stress test (IBSS + Mesh-gate/AP)

**Author:** Aldwin (with Claude Code)
**Goal:** regression + stress test every feature built so far on both L2s — the
Mesh-gate (AP + TWT + STA-scaling) and IBSS — plus the radio-free / scan sanity apps.
**Status:** PASS overall. One real finding: a **drop/rejoin survivor-side rediscovery
gap** in IBSS (logged as TODO #20). No crashes/reboots anywhere.

Hardware: 3× XIAO ESP32-S3 + FGH100M (`/dev/ttyACM0..2`, `BOARD=proto1-fgh100m`,
fw 1.17.6) + chronium (RPi5 + MM6108, `morse_driver` 1.17.8). The AP build under test
is the cap=255 four-block-TIM firmware.

---

## Results matrix

| # | Feature | Test | Result |
|---|---|---|---|
| AP1 | SoftAP (SAE) | boots + beacons; STAs associate | ✅ |
| AP2 | Multi-STA assoc | 2 ESP32 STA + chronium STA = **3 authorized** | ✅ |
| AP3 | AP↔STA data | AP→STA ping, bidirectional | ✅ 33/33 replies, 0 timeouts |
| AP4 | TWT power-save | AP→STA RTT doze signature | ✅ 12–1190 ms (max ≈ 1 s wake interval) vs ~10 ms baseline |
| AP5 | Linux STA interop | chronium `wpa_supplicant_s1g` SAE → ESP32 AP | ✅ associated + in authorized list |
| AP-stress | sustained load | chronium→AP 600 pings @5/s + 2 min watch | ✅ **0% loss**, 3 STAs stable (42/42 samples), **0 reboots** |
| IB1 | IBSS bring-up | boots, joins cell | ✅ |
| IB2 | 3-board mesh | each node discovers both peers | ✅ ACM0=.183, ACM1=.159, ACM2=.86 |
| IB3 | All-pairs data | bidirectional ping every pair | ✅ (ACM1/ACM2 0 timeouts; ACM0 a few at settle) |
| IB4 | `0x88B5` raw frame | EtherType 0x88B5 exchange | ⚠️ not re-observed this run (its shared IP datapath is healthy) |
| IB5 | Drop/rejoin | drop + rejoin one node, survivors recover | ⚠️ **partial — see finding** |
| IB6 | Linux IBSS interop | chronium IBSS node ↔ 3 ESP32 | ✅ chronium reaches all 3 (.183/.159 5/5, .86 4/5), `station dump`=3 |
| IB-stress | mixed 4-node + load | 3 ESP32 + chronium, sustained ping, drop/rejoin | ✅ **no phantom peers**, **0 survivor reboots** |
| H1 | `rimba-hello` | radio-free boot + PSRAM | ✅ PSRAM 8 MB, no panic |
| SC1 | `rimba-halow-scan` | radio boots + scans | ✅ chip 0x0306, scans, no panic |

## Stress detail

- **AP** — chronium→AP `192.168.12.1`: **600/600 pings, 0% loss** over 120 s; AP held **3
  STAs the whole window** (42/42 "authorized STAs: 3" samples), AP→STA(.2) 125 replies / 0
  timeouts, **0 reboots/panics**.
- **IBSS** — mixed 4-node cell (ACM0 .183, ACM1 .159, ACM2 .86, chronium .66) under chronium
  load. Survivors over ~100 s spanning a drop/rejoin: ACM0 saw **only 2 distinct peer IPs
  (.159, .66) — zero phantoms** (the #17 mixed-cell regression stays fixed), 2 timeouts; ACM1
  similar, 3 timeouts; **0 reboots**. Both survivors actively ping chronium (.66) — 500+
  replies each — confirming ESP32→Linux discovery (it just needs longer than a short window).

## Finding — drop/rejoin survivor-side rediscovery gap (→ IBSS TODO #20)

Dropping then rejoining ACM2 (.86) in the mixed cell:
- **ACM2 fully recovers** — after reboot it rejoins and pings all three peers (.183, .159,
  .66) with **0 timeouts**.
- **Survivors still serve ACM2** — ACM0/ACM1 reply to ACM2's pings (so their RX/TX datapath
  and routing to .86 work).
- **But survivors did not re-add ACM2 to their own active-ping set** within ~90 s — ACM0/ACM1
  never resumed pinging .86. So the survivor→returned-peer *rediscovery* (app peer-table
  re-add) didn't re-fire, even though the RX path clearly sees ACM2's frames.

P0.6 (2026-06-20) reported survivors re-acquire a returned peer as a fresh record; this run —
under a mixed cell with chronium present — shows that not happening for the active-ping list.
Re-confirm and root-cause (likely the get-or-add-on-RX-TA path vs. the app's ping list, or an
8-slot/age-out interaction). Not a crash; the datapath stays healthy. Recorded as TODO #20.

## Bench state left

ACM0/ACM1 on IBSS, ACM2 last flashed `rimba-halow-scan`; chronium `wlan1` in **IBSS mode**
(`iw … ibss join rimba-ibss 5560 …`, IP .66). Per the radio-silent workflow, idle boards
should be returned to `rimba-hello` and chronium's IBSS torn down (`sudo iw dev wlan1 ibss
leave`) when done.

## Coverage caveats

- `0x88B5` (IB4) not re-observed in-window — the app logs it infrequently; its IP datapath
  (same path) is proven. Worth an explicit re-check.
- No long soak this run (minutes, not the earlier ~6.5 h); no µA measurement (no bench meter).
- AID ≥ 64 (4-block TIM, AP) still not exercised (needs 64+ STAs).
