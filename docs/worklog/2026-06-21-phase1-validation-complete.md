# Worklog — 2026-06-21: Phase-1 IBSS validation complete (P0.4, I.4, soak) + power-save plan

Short session closing out the open-IBSS foundation validation, plus recording the
power-save architecture decisions. Continues
[`2026-06-20-ibss-adoption-interop-phantom.md`](2026-06-20-ibss-adoption-interop-phantom.md).

## Tests run

- **P0.4 per-peer dedup → ☑ (forced cross-peer probe).** Instrumented the dedup site
  (`umac_datapath_is_rx_frame_duplicate`) with a 256-frame cross-peer ring; drove ACM0 with
  a chronium unicast flood (8464 frames, 0.06 % loss) + ACM1/ACM2 pings. **208 cross-peer
  `(tid,seq)` collisions correctly ACCEPTED** (not deduped) + **9 genuine same-peer dedup
  drops**. Dedup state is per-peer (`sta_data->rx_seq_num_spaces`) → independence proven.
  Probe reverted.

- **I.4 on-air frame diff → ✗ BLOCKED.** chronium monitor mode on the right S1G channel
  (`morse_cli` = 915500 kHz) + raw `AF_PACKET` capture → **0 frames** (rx delta 0, promisc
  no help, no `morse_cli` monitor cmd). **Confirmed not IBSS-specific** — an ESP32 SoftAP
  beacon also captured 0. The same `wlan1` RX's fine in connected modes ⇒ morse **monitor
  mode** is the blocker. Closing #11 (verify the S1G beacon on-wire — specifically our
  ESP32 beacon's `source_addr`, MAC vs BSSID) needs an **external S1G sniffer**.

- **P1.4 recovery → ☑** (done in the 2026-06-20 stress test: mid-stream node drop, survivors
  unaffected, rejoin).

- **P1.5 soak → ☑ (~6.5 h, 4-node).** 3 ESP32 + chronium, continuous traffic, ESP32
  instrumented with 30 s heap/uptime telemetry (kept). **Uptime 6 h 31–32 m, 0 reboots, 0
  asserts**, data to the end (chronium `icmp_seq ~47 000`, 12–19 ms RTT). **No heap leak** —
  `heap_min` Δ over 6.5 h: 0 / −108 / −548 B (transient noise).

**Verdict: the open-IBSS foundation is validated and ready for Phase 2.** Full suite:
P0.1–0.7 ☑, I.1–I.3 ☑, I.5 ☑, P0.4 ☑, P1.4 ☑, P1.5 ☑; only I.4 ✗ (blocked, external gear).

## Decisions recorded (see hardening-todo)

- **#4 IBSS merge — OUT OF SCOPE.** Rimba is a *provisioned* network (agreed BSSID), so
  coordinator-free TSF merge isn't needed. Documented in code + docs.
- **Create/join role is dynamic, not provisioned** (#7 → scan → join-else-create,
  `ieee80211_sta_find_ibss`) — field relays are equal; first up creates, rest join.
- **Power-save is an early focus, via RTC, not chip PS.** The morse driver has **no IBSS
  radio power-save** (TWT is STA/AP-only, no ATIM). So Scheduled mode = **ESP32 + RTC
  power-cycles the radio** (#8). Bypasses chip PS + TSF sync. Gating unknown = radio
  **cold-boot-to-joined time (#9 / RISK-02)**.

## Git state
All on `main`: `4dd855c` (I.4 blocked), `254f2b6` (P0.4), `ce37dd6` (soak + heap telemetry),
plus this doc-update commit. Radios silent, chronium down.

## Next
**#9 — RISK-02 cold-boot-to-IBSS-joined time** (the gating number for RTC-scheduled
power-save), then Phase 2 (link security, #3 CCMP). Backlog item still open: I.4/#11 needs
an external S1G sniffer.
