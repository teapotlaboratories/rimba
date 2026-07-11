# Worklog — 2026-07-05 — board2 STA power-save vs a Linux **Mesh+AP** gateway

**Author:** Aldwin
**Goal:** measure board2's full PS ladder against a Linux node running **mesh + AP concurrently** on one
MM6108, with the mesh actually peering and board2's traffic routing **through** the mesh.

> **⚠️ This worklog was rewritten (2026-07-05, later).** The first draft framed everything around a
> "1.17.8 → 1.17.9 concurrency regression" that required downgrading the gateway. **That framing was wrong**
> — the mesh-gate works on stock 1.17.8, and every "it's broken" symptom during the original debugging was a
> self-inflicted test error, not a driver regression. The corrections are called out below; the PS-ladder
> measurements (which were taken against a genuinely-working Mesh+AP) still stand.

## Result

- **Mesh + AP concurrency on one MM6108 WORKS** (stock 1.17.8). Role assignment: **mesh on the primary vif
  (`wlan1`), AP on a secondary vif (`ap0`)**. An ESP32 STA under the AP routes **through** the mesh to a
  second mesh node. Verified end-to-end: **board1 → chronite AP → ip_forward → HaLow mesh → chronosalt =
  8/8 pings**; board2 the same path = 6/8 (some close-node RF loss). Full copy-paste recipe now lives in
  [`mesh-ap/rimba-mesh-ap-milestones.md` §A3](../mesh-ap/rimba-mesh-ap-milestones.md#a3--mesh--ap-concurrency-on-one-radio).
- **PS ladder measured** (below). *(An initial "a Mesh+AP raises the STA's doze floors" claim was later
  DISPROVED by a same-gateway mesh-on/off A/B — the mesh has no measurable PS effect; see the retraction.)*

## Corrections — the "regression" was self-inflicted (read before trusting old notes)

The original debugging chased a deterministic "1.17.9 broke concurrency" cause for what were actually
**three separate test-harness bugs**. None is a driver regression:

1. **The mesh IP was never on `wlan1`.** The bring-up scripts used an escaped `\$IP` that expanded to empty
   on the remote node, so `ip addr add /24 dev wlan1` silently did nothing. With no mesh IP, `ip route get
   <peer>` resolved out the **management interface `wlan0`** via the default route — pings never hit the
   mesh radio (`tx_packets` delta = 0), the mpath stayed empty, and it looked exactly like a
   forwarding/HWMP bug. It is **not**. Always check `ip route get <peer>` + `ip -4 addr show wlan1` first.
2. **Chip-wedge from churn.** Rapid `wpa_supplicant_s1g` restarts wedge the MM6108 (mesh stuck `SCANNING`).
   The docs already warn "bring it up once and leave it"; the repeated restarts manufactured the flakiness.
3. **Broken ESP console capture.** A `grep | head` pipe on the live tty swallowed board output, which read
   as a "dead board" / "AP can't route" — an invented hardware limitation. Capturing to a **file**
   (`cat /dev/ttyACMx > /tmp/log`) showed the STA connecting, taking its IP, and pinging fine all along.

**So:** the plain mesh forwards, the AP+mesh concurrency works, and the STA routes through — all on stock
1.17.8. The original "root cause: 1.17.9 driver, not firmware" isolation table is **retracted** (it was
built on the confounded symptoms above); whether 1.17.9 differs at all is now **unverified** — re-test
cleanly before believing it. The recipe itself is version-agnostic.

## Bonus finding — Pi 5 MM6108 reset bug (fixed)

While chasing the (self-inflicted) wedges, found a **real** driver bug: on the **Pi 5** nodes
(chronium/chronite), stock `morse_hw_reset()` never actually resets the chip on a warm driver reload — it
uses the legacy int-GPIO API (fails on the RP1 controller) and floats the pin to release (the Wio HAT's
`RESET_N` has no pull-up). So a driver reload wedged the chip (`cfg_detect_and_init -5`) and only a reboot
recovered it. **Fix:** `docs/reference/patches/morse-driver-pi5-reset-gpiod.patch` (gpiod API + drive-high
release) — built + deployed to chronite and chronium, verified: `modprobe -r morse; modprobe morse` now
resets the chip and re-probes in ~6 s, no reboot. (Pi Zero nodes were never affected — their GPIO5 reset
works stock.) Full root-cause in the patch README.

## Topology (all S1G ch27 / 915.5 MHz, 1 MHz)

```
 board2 (ESP STA, PPK2)  --HaLow AP-->  chronite (Mesh+AP GATEWAY)  --HaLow mesh-->  chronosalt (peer)
   192.168.12.2                          ap0 = AP    192.168.12.1                     wlan1 = mesh 10.9.9.4
   gw .12.1                              wlan1 = mesh 10.9.9.3  (ip_forward=1)         route .12.0/24 via .3
```
Gateway bring-up per §A3: mesh on `wlan1` first (COMPLETED, IP set), then AP on `ap0`
(`iw phy phy1 interface add ap0 type __ap`; first `hostapd_s1g` start often needs one retry to reach
AP-ENABLED), `ip_forward=1`; peer runs the mesh + a return route.

## PS ladder — board2 @ 5.00 V, associated to the Mesh+AP gateway

| Mode | Current | Power | vs a plain AP (matched-1.17.9 doc) |
|---|---|---|---|
| No PS | 64.8 mA | 324 mW | ≈ (65) |
| Dynamic PS | 21.8 mA (floor 21) | 109 mW | **higher** (was ~14.5) |
| TWT, 10 s SP | 23.4 mA (floor 20) | 117 mW | ≈ dyn-PS — mid-session TWT not engaged (like the plain Linux AP) |
| WNM + chip powerdown | 15.0 mA (floor 14) | 75 mW | **much higher** (was ~4.0) |

**❌ Key finding RETRACTED — "a Mesh+AP raises the STA's PS floors" is WRONG.** I claimed the higher doze
tiers (esp. WNM ~16 vs ~4 mA) were the mesh's airtime tax. A **same-gateway mesh-on/off A/B** (measure
board2 against the SAME AP `ap0`, only toggling the mesh vif) **disproves it**: No-PS 66/68, Dyn-PS ~31/36,
**WNM+powerdown 15.9 (mesh on) / 16.9 (mesh off) mA** — the mesh changes nothing (WNM-first ladder, enter
ret=0, so not the after-TWT hang either). The elevated tiers vs the plain-AP doc are a **confounded
comparison** (this setup: AP on the secondary vif `ap0` + 1.17.8 + TX-cap; the doc: primary-vif AP + 1.17.9
+ full TX). **The ~16 mA WNM is a setup artifact — most likely the secondary-vif AP not letting board2
reach the deep powerdown — NOT the mesh.** Which of {secondary-vif AP, 1.17.8, TX-cap} causes it is open.
TWT read ≈ dynamic PS (`hostapd_s1g` doesn't engage the mid-session responder — same as the plain AP).
Relates to [`reference/rimba-halow-ps-esp32-vs-linux-ap.md`](../reference/rimba-halow-ps-esp32-vs-linux-ap.md).

## State + caveats

- **Bench:** stock **1.17.8** on all four Linux nodes (Pi 5 nodes carry the reset patch); radio-silenced
  after. board2 back to `rimba-hello`.
- **Fresh clean re-measure (2026-07-05, final all-1.17.8 bench) — DONE, and it corrected a wrong theory.**
  The first re-run read a flat **~14 mA** in the No-PS phase with **0/8** routing. I wrongly blamed a PPK2
  "USB back-feed" — but the PPK2 is in **ampere-meter mode** (in series, measures the *full* current, no
  back-feed path). The ~14 mA was real: board2 was **dozing, not full-on**. Root cause: **RX overload** —
  board2 sits right against the gateway, so at full ESP TX the gateway's receiver saw it at **−8 dBm**, too
  hot; the link was too marginal for the STA to hold No-PS, so it fell to a dynamic-doze (station dump: stays
  *associated*, but inactive-time grows to 13 s, radio only spikes to 66 mA on active TX). **Confirmed by
  A/B:** adding `mmwlan_override_max_tx_power(1)` (cap ESP TX to 1 dBm) dropped the gateway's RX to **−28 dBm**
  (healthy), and **No-PS jumped ~14 → ~56 mA**, routing went **0/8 → 8/8** through the mesh-gate. Clean ladder
  vs the Mesh+AP at −28 dBm: **No-PS ~56, Dyn-PS 20.4, TWT 20.4 (≈dyn — no mid-session TWT), WNM+powerdown
  16.3 mA** — consistent with the earlier run (doze tiers match; No-PS lower because the TX cap reduces PA
  current). **Bench lesson:** on this close bench, a board2 PS measurement is only valid with the TX capped
  (or the boards physically separated) — otherwise RX overload makes every doze tier read artificially low.
- **R3 downlink-while-dozing through the mesh-gate — VERIFIED.** chronosalt (mesh peer) pinged the dozing
  leaf at 1 Hz across the full path `chronosalt → mesh → chronite(gateway) → ip_forward → AP → dozing
  board2`: **28/28, 0% loss** in **both** dyn-PS and TWT phases, RTT 31/87/261 ms (DTIM buffering + mesh
  hop), STA ~30 mA. So the gateway's AP buffers downlink for the dozing leaf and flushes at DTIM even when
  the traffic comes from the far side of the mesh — no drops. TWT shows no deep-doze/burst (Linux
  `hostapd_s1g` doesn't engage mid-session TWT, so board2 stays dyn-PS-like). Deep-sleep + host-light-sleep
  vs the Mesh+AP are reasoned in `reference/rimba-halow-ps-esp32-vs-linux-ap.md` §3a′ (deep tiers are
  radio-off → AP-independent; host light sleep backfires harder under the mesh's extra beacon/peering IRQs).
- **Bench gotcha (cost real time) — a mesh peer that won't come up.** Symptom: `wpa_supplicant_s1g -B`
  starts but `wpa_cli` can't reach it (empty/`unreachable` state, 2 zombie wpa processes, no ctrl socket).
  Two causes, both must be cleared: **(1)** a stuck `wpa_supplicant_s1g` from a prior run holding `wlan1`
  (survives `pkill`) → **reboot** the node; **(2)** **NetworkManager is managing `wlan1`** and sits in
  `DISCONNECTED` (actively scanning), fighting the mesh instance → `sudo nmcli device set wlan1 managed no`
  before starting the mesh. (A node whose NM shows `wlan1` `INACTIVE` — no saved network — meshes without
  the release; one in `DISCONNECTED` does not.) Reboot **+** NM-release together fixed it; either alone
  did not. Worth making the NM exclusion persistent in the node setup.
- The temporary route-check block added to `rimba-halow-sta` for this test (`sta_ip_and_route` +
  `ping_mesh_dst`) should be reverted before shipping.
