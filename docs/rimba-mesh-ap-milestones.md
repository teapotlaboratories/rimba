# Mesh-gate (Mesh + AP) on the MM6108: milestones

The **Mesh-gate** is Rimba's second candidate L2: relays run **802.11s mesh**
to each other and a **SoftAP** that leaf nodes associate to and **TWT**-sleep
under. It is the alternative to the **IBSS** L2 (see
[`rimba-ibss-milestones.md`](rimba-ibss-milestones.md)); this doc tracks its
bring-up on **ESP32-S3 + MM6108** (`mm-iot-sdk`/morselib) and the Linux reference.

**Governing requirement (same as IBSS):** the implementation is **derived from
the Linux side** — MorseMicro's `morse_driver` (out-of-tree mac80211 driver) and
the mac80211 fork — not improvised from morselib internals. Every change is
verified on hardware (or the reason it can't be is documented), and ported code
carries a new-code ↔ Linux mapping. The detailed mappings live in
[`rimba-mesh-ap-porting.md`](rimba-mesh-ap-porting.md) (§4.2 TWT, §5 STA-count);
this doc is the narrative + milestone view.

**Hardware:** up to 3× Seeed XIAO ESP32-S3 + FGH100M (`boards/proto1-fgh100m`,
`bcf_fgh100mhaamd`, fw **1.17.6**) **+ a Raspberry Pi 5 + Wio-WM6180 (MM6108)
Linux reference node** (`morse_driver`/cli/fw **1.17.8** — the interop oracle,
[`rimba-linux-node-setup.md`](rimba-linux-node-setup.md)). US 915.5 MHz, 1 MHz BW,
S1G ch27 / op-class 68; SSID `rimba-ping`, WPA3-SAE; HaLow subnet 192.168.12.0/24.

---

## Why two L2s — IBSS vs Mesh-gate

Rimba is at the **L2 phase**: pick the link layer the mesh rides on. Two viable
options on the MM6108, each with real trade-offs — so we are implementing **both**
on hardware and comparing, rather than betting early.

| | **IBSS / ad-hoc** | **Mesh-gate (802.11s mesh + AP)** |
|---|---|---|
| Topology | Symmetric peers, no infrastructure | Relays mesh; leaves are STAs under a relay-AP |
| Leaf power-save | **None usable** — no TWT/ATIM/AP buffering; only sub-µA path is an RTC cold-boot each wake (≈1.39 s rejoin tax, measured) | **TWT** — scheduled wake with the **AP buffering downlink**; leaf dozes *and* keeps traffic |
| Coordinator | None needed (provisioned BSSID) | Relays must be in range + always-on |
| Departure from current base | It *is* the current Phase-1 foundation | Larger departure; adds AP/mesh + association |
| Status | **RESOLVED + hardened + soaked** (RISK-01) | **AP + TWT + STA-scaling proven**; mesh+AP concurrency proven on Linux, not yet on ESP32 |

The signal so far: IBSS's dead-end is **leaf power-save** — the morse firmware has
no IBSS radio power-save ([`rimba-mm6108-powersave-analysis.md`](rimba-mm6108-powersave-analysis.md)).
The Mesh-gate dissolves that (TWT + AP buffering), at the cost of always-on relays
and more moving parts. Neither is chosen yet; this milestone set exists to make
the Mesh-gate comparable on the same hardware.

---

## Milestones

### A1 — SoftAP bring-up (SAE) ✅
morselib SoftAP on the MM6108: SSID `rimba-ping`, WPA3-SAE, S1G ch27 / op-class
68, host-built beacon via the bundled hostapd. Boots and beacons stably.

### A2 — AP ↔ STA association + bidirectional IP ✅
`rimba-halow-ap` + `rimba-halow-sta` apps: a STA associates (SAE 4-way) and
exchanges IP with the AP (DHCP, ping). The AP-STA path is the RISK-01 fallback for
IBSS and the foundation for the Mesh-gate. ([`worklog/2026-06-18-halow-ap-sta-ping.md`](worklog/2026-06-18-halow-ap-sta-ping.md))

### A3 — Mesh + AP concurrency — proven on Linux, pending on ESP32 ◑
The MM6108 driver advertises `{managed, AP, mesh point} ≤ 2` co-channel; on
chronium an AP (`wlan1`) + mesh-point (`mesh0`) + a TWT'ing ESP32 STA ran
**concurrently**. The same combination on morselib (ESP32 as the relay running
mesh+AP) is **not yet built** — the open structural item for the Mesh-gate.
([`worklog/2026-06-22-mesh-ap-twt.md`](worklog/2026-06-22-mesh-ap-twt.md))

### T1 — AP-side TWT responder port ✅
Ported the TWT responder into morselib around hostapd (mirroring the driver's
around-hostapd approach): advertise responder cap, parse the STA's TWT IE from the
(re)assoc-req, splice the ACCEPT IE into the (re)assoc-resp, plus the S1G
TWT-Setup/Teardown **action-frame** path. New-code ↔ `morse_driver/twt.c` map in
[porting §4.2](rimba-mesh-ap-porting.md). **Firmware finding:** `TWT_AGREEMENT_INSTALL`
(cmd `0x26`) is gated to STA vifs in *every* MM6108 `.mbin` (1.17.6 = 1.17.8, byte-
identical to the Linux `.bin`) — so the SP is served **host-side** on Linux too.

### T2 — Leaf actually TWT-sleeps (the load-bearing fix) ✅
The decisive bug: hostapd's transient `sta_remove` during (re)assoc freed the
just-accepted, still-`PENDING` TWT slot before the assoc-resp IE was built, so the
STA never established TWT. Plus a missing **flush-on-wake** in the AP datapath
(`umac_ap_set_stad_sleep_state` cleared the TIM bit but never kicked TX). Fixed →
STA deep-sleeps: AP→STA RTT rises toward the ~1 s TWT interval (was a flat ~10 ms),
matching the Linux AP. morselib stand-in for mac80211's `ieee80211_sta_ps_deliver_*`.

### T3 — Multi-STA TWT responder ✅
Per-STA agreement table (`agreements[MMWLAN_AP_MAX_STAS_LIMIT]` + parallel
`responder_peers[]`), allocated by SA on assoc, freed on leave. HW-validated: one
AP held two TWT-requester ESP32 STAs concurrently. (Linux keeps a dynamic per-STA
list; the fixed table is an embedded-RAM choice.)

### S1 — STA-count scaling 63 → 127 → 255 ✅
Raised morselib's S1G TIM from one block (AID ≤ 63) to two (≤ 127) then four
(`MAX_SUPPORTED_AID = 256`, AIDs 1..255 — the `uint8_t max_stas` ceiling). **The
real limit was the multi-block TIM, not firmware capacity or the `20` `#define`** —
and morselib's TIM is a *port of Linux `dot11ah/tim.c`* (shared `S1G_TIM_MAX_BLOCK_SIZE
= 256`, the 8-subblocks/64-AIDs-per-block geometry, the four BLOCK/AID/OLB/ADE
encoding modes, entire-page slice 31). Both made configurable via Kconfig; per-STA
state + the TWT table route to **PSRAM** (the one ESP32-specific divergence — no
Linux counterpart). Linux code map in [porting §5](rimba-mesh-ap-porting.md).

### V1 — Multi-node validation ✅
1 ESP32 AP + 2 ESP32 STA + 1 chronium **Linux STA** associate concurrently (3
STAs, all SAE); TWT power-save active; no regression across the 127 and 255 builds.
Also a new **chronium-as-infra-STA** recipe (`wpa_supplicant_s1g`, SAE, S1G ch27 ≡
nl80211 freq 5560). ([`worklog/2026-06-23-ap-multinode-twt-hwtest.md`](worklog/2026-06-23-ap-multinode-twt-hwtest.md))

---

## Open items (Mesh-gate)

- **Mesh + AP concurrency on ESP32** (A3) — the relay role itself; proven on Linux only.
- **AID ≥ 64 on air** — the 2nd–4th TIM blocks aren't exercised (needs 64+ live STAs).
- **Linux STA as TWT *requester*** vs the ESP32 AP responder — needs the Morse driver's
  requester-role bring-up (a STA-side knob), deferred.
- **No SP-overlap scheduling** — Linux's `twt_wi_tree` spaces SPs; this port serves each
  STA reactively on wake.
- **µA current** of a fully-idle TWT link — no bench power-enable line / meter.
- **MM6108 firmware's true concurrent-STA capacity** is unknown (Linux caps at
  `IEEE80211_MAX_AID = 2007`; spec is 8191) — 255 is a build/structural ceiling, not a
  firmware guarantee.

---

## Methodology — how future Mesh-gate (and Rimba) features get built

The repeatable process this milestone set follows, codified in
[`.ai/AGENTS.md`](../.ai/AGENTS.md):

1. **Derive from Linux.** Root-cause against `morse_driver` / `net/mac80211` (same
   silicon Linux drives) and follow it; don't tolerate a symptom with a local hack
   that diverges from the reference.
2. **Verify on hardware** (or unit test) — and if you *can't*, document why (e.g.
   "AID ≥ 64 needs 64+ associations; not reproducible on a 3-board bench").
3. **Write the new-code ↔ Linux map** for any port, as in
   [`rimba-mesh-ap-porting.md`](rimba-mesh-ap-porting.md) §4.2 / §5, and call out
   deliberate divergences (e.g. PSRAM, fixed-size tables) with the reason.
4. **Cite sources** — `file:line`/SHA for code, command+output for hardware, URLs
   for external facts; prefer authoritative (vendored source) over marketing.

---

## Build / test

```bash
make build APP=rimba-halow-ap  BOARD=proto1-fgh100m   # the SoftAP (cap/PSRAM via sdkconfig.defaults)
make build APP=rimba-halow-sta BOARD=proto1-fgh100m   # a TWT-requester leaf
make flash APP=rimba-halow-ap  BOARD=proto1-fgh100m PORT=/dev/ttyACM0
make flash APP=rimba-halow-sta BOARD=proto1-fgh100m PORT=/dev/ttyACM1
```

chronium as a Linux STA (interop oracle) — `wpa_supplicant_s1g`, SAE, **freq 5560**
(S1G ch27 in the 5 GHz model; on-air 915.5 MHz); full recipe in
[`worklog/2026-06-23-ap-multinode-twt-hwtest.md`](worklog/2026-06-23-ap-multinode-twt-hwtest.md) §3.
