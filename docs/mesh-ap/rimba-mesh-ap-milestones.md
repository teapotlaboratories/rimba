# Mesh-gate (Mesh + AP) on the MM6108: milestones & porting

The **Mesh-gate** is Rimba's second candidate L2: relays run **802.11s mesh** to
each other and a **SoftAP** that leaf nodes associate to and **TWT**-sleep under.
It is the alternative to the **IBSS** L2 (see
[`rimba-ibss-milestones.md`](../ibss/rimba-ibss-milestones.md)). This is the single doc
for the Mesh-gate on **ESP32-S3 + MM6108** (`mm-iot-sdk`/morselib): the milestone
view, the **new-code ↔ Linux** porting maps, and the **TODO**. It also houses the
standalone **802.11s mesh** status + the full Linux/ESP32 feature comparison — see
[§802.11s mesh point (morselib)](#80211s-mesh-point-morselib).

**Headline (2026-07-01): both halves exist; the mesh half is built + secured.** An ESP32
joins a Linux HaLow 802.11s mesh and pings it, **originates** multi-hop traffic, **relays**
others' traffic, does **group/multicast forwarding**, **PERR** teardown, a runtime **leaf
toggle**, and a **secured (SAE+AMPE+CCMP) mesh** single- and multi-hop — phases **P0–P6b, P6d ✅**,
**P6c (security ✅; airtime-metric/power-save/proxy-gate ⬜) 🟡** ([§802.11s mesh point](#80211s-mesh-point-morselib)).
The one remaining structural gap for a full all-ESP32 Mesh-gate is **mesh + AP concurrency on one
ESP32 radio** ([§A3](#a3--mesh--ap-concurrency-on-one-radio)) — both halves work individually on the
ESP32; running them co-channel together is proven on Linux but not yet on the ESP32.

**Governing requirement (memory `proper-fix-follow-linux`, same as IBSS):** the
implementation is **derived from the Linux side** — MorseMicro's `morse_driver`
(out-of-tree mac80211 driver) + the mac80211 fork — not improvised from morselib
internals. Every change is verified on hardware (or the reason it can't be is
documented), and ported code carries a new-code ↔ Linux mapping (below).

## Hardware / bench

- **3× Seeed XIAO ESP32-S3 + FGH100M** (`boards/proto1-fgh100m`, `bcf_fgh100mhaamd`) — the
  device under test. **ESP firmware 1.17.9** (the build ships the vendored `vendor/morse-firmware`
  `mm6108` blob via `CONFIG_MM_FW_FILE`, overriding morselib's stock 1.17.6 — one minor rev *ahead* of
  the Linux 1.17.8 reference; the earlier AP/TWT milestones were validated on stock 1.17.6).
- **4× Raspberry Pi HaLow reference nodes** — all **Seeed Wio-WM6108** (Quectel FGH100M-H / MM6108),
  `morse_driver` + firmware **1.17.8**, the interop oracle
  ([`reference/rimba-linux-node-setup.md`](../reference/rimba-linux-node-setup.md)):
  - **chronium** (Pi 5) — dedicated `morse0` on-air monitor + interop node.
  - **chronite** (Pi 5) — interop mesh/AP node; the primary Linux mesh peer.
  - **chronosalt**, **chronogen** (Pi Zero 2 W) — secondary mesh nodes (10.9.9.3 / 10.9.9.4), added for
    multi-node runs (`reference/rimba-linux-node-setup.md` §"Pi Zero 2 W variant"). *Caveat:* chronogen
    has an intermittent morse-SPI fault (`-19 ENODEV`) that clears on a physical power-cycle.

**Radio:** US 915.5 MHz, 1 MHz BW, S1G ch27 / op-class 68. **AP:** SSID `rimba-ping`, WPA3-SAE, subnet
`192.168.12.0/24`. **Mesh:** Mesh ID `rimba-mesh`, subnet `10.9.9.0/24`.

Paths: `MORSE = components/halow/components/mm-iot-sdk/framework/morselib/src`;
Linux driver = `morse_driver/` (tag 1.17.8, on the Pi 5 nodes at `~/halow/morse_driver`).

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
| Status | **RESOLVED + hardened + soaked** (RISK-01) | **AP + TWT + STA-scaling proven**; mesh built + secured (P0–P6d); mesh+AP concurrency proven on Linux, not yet on ESP32 |

The signal so far: IBSS's dead-end is **leaf power-save** — the morse firmware has
no IBSS radio power-save ([`design-specification/rimba-mm6108-powersave-analysis.md`](../design-specification/rimba-mm6108-powersave-analysis.md)).
The Mesh-gate dissolves that (TWT + AP buffering), at the cost of always-on relays
and more moving parts. Neither is chosen yet; this milestone set exists to make
the Mesh-gate comparable on the same hardware.

---

## Capability status — ESP32/morselib vs. Linux

Markers (throughout): ✅ done · ◑ both halves exist, not yet combined · 🟡 core done, hardening left ·
⬜ not implemented · ☐ open (backlog).

| Capability | Linux (`morse_driver`/mac80211) | ESP32 (morselib) | Notes |
|---|---|---|---|
| **AP mode** (S1G, SAE) | ✅ | ✅ | morselib 2.10.4+ (hostapd-backed) |
| **STA mode** + TWT requester | ✅ | ✅ | `mmwlan_twt_add_configuration` |
| **802.11s mesh point** | ✅ (`mesh.c`, `CONFIG_MAC80211_MESH`) | ✅ **ported (P0–P6b, P6d)** | full control + data plane: MPM peering, HWMP (PREQ/PREP/PERR + path table), 4/3-addr forwarding, group forwarding, link-failure teardown, leaf toggle — [§below](#80211s-mesh-point-morselib) |
| **Mesh security** (SAE + AMPE + CCMP) | ✅ | ✅ **ported** (host SW-CCMP, P6c) | crypto done; hardening (airtime/PS/proxy-gate) 🟡 · single- + multi-hop, ESP↔ESP + cross-vendor — [§Mesh security](#mesh-security--sae--ampe--ccmp-p6c) |
| **Mesh + AP concurrent** | ✅ (`iface_combination` AP\|MESH, `#chan<=1`) | ◑ (both halves exist; not yet co-channel together) | one radio, co-channel; the Mesh-gate pattern ([§A3](#a3--mesh--ap-concurrency-on-one-radio)) |
| **AP TWT responder** (leaf power-save) | ✅ (host-side, `mac80211` + `twt.c`) | ✅ **ported** (below) | stock fw, no firmware change |
| **AP STA-count ceiling** | up to `IEEE80211_MAX_AID = 2007` | ✅ **255** (`uint8_t max_stas`) | four-block S1G TIM (below) |

**Bottom line:** AP, the AP-TWT-responder, a 255-STA ceiling, **and the 802.11s mesh point
(P0–P6d, secured)** are all usable on the ESP32. The last structural item for a full all-ESP32
Mesh-gate is **mesh + AP concurrency on one radio** (A3): each half works on the ESP32 alone;
running a mesh point and a SoftAP co-channel on the same MM6108 is proven on Linux but not yet
brought up on the ESP32.

---

## Milestones

Concise index; each entry points to its detail section below.

| # | Milestone | Status |
|---|---|---|
| **A1** | SoftAP bring-up (SAE) — SSID `rimba-ping`, WPA3-SAE, S1G ch27; host-built beacon (bundled hostapd) | ✅ |
| **A2** | AP ↔ STA association + bidirectional IP (SAE 4-way, DHCP, ping) — [worklog](../worklog/2026-06-18-halow-ap-sta-ping.md) | ✅ |
| **A3** | [Mesh + AP concurrency on one radio](#a3--mesh--ap-concurrency-on-one-radio) — proven on Linux, pending on ESP32 | ◑ |
| **T1–T3** | [AP-side TWT responder](#ap-twt-responder-port--detail-t1t3) — port, leaf actually sleeps, multi-STA | ✅ |
| **S1** | [STA-count scaling 63 → 127 → 255](#ap-sta-count-scaling-63--127--255--multi-block-s1g-tim-port-s1) — multi-block S1G TIM | ✅ |
| **V1** | Multi-node validation — 1 ESP AP + 2 ESP STA + 1 chronium Linux STA (3 STAs, all SAE), TWT active, no regression across 127/255 builds — [worklog](../worklog/2026-06-23-ap-multinode-twt-hwtest.md) | ✅ |
| **P0–P6d** | [802.11s mesh point](#80211s-mesh-point-morselib) — vif+beacon → MPM → HWMP → forwarding → group → PERR → security → leaf toggle | ✅ / 🟡 (P6c hardening) |

---

## AP TWT-responder port — detail (T1–T3)

**T1** — ported the TWT responder into morselib around hostapd (map below). **Firmware finding:**
`TWT_AGREEMENT_INSTALL` (cmd `0x26`) is gated to STA vifs in the MM6108 firmware (present in both
1.17.6 and 1.17.8/1.17.9 `.mbin`) — so the SP is served **host-side** on Linux too.
**T2** — the load-bearing fix (row 9): hostapd's transient `sta_remove` during (re)assoc freed the
just-accepted, still-`PENDING` TWT slot before the assoc-resp IE was built, plus a missing
flush-on-wake in the AP datapath → the STA never established TWT. Fixed → STA deep-sleeps (AP→STA RTT
rises toward the ~1 s TWT interval, was flat ~10 ms), matching the Linux AP.
**T3** — per-STA agreement table allocated by SA on assoc, freed on leave; HW-validated with 1 AP
holding two TWT-requester ESP32 STAs concurrently.

**Status:** works end-to-end on ESP32 AP ↔ ESP32 STA, **stock firmware, no firmware change**. A
mid-session TWT-Setup **action** frame path was later added too ([Action-frame TWT](#action-frame-twt-path)).

### Architecture
Linux does the whole TWT responder **in the driver, around hostapd**: parse the
STA's TWT IE from the (re)assoc-request, splice the ACCEPT IE into the
(re)assoc-response on TX, serve the dozing STA's downlink during its service period
(SP). morselib has no AP-side MLME of its own (the bundled hostapd builds assoc
frames), so the port mirrors Linux's driver-layer approach: hook morselib's RX-mgmt
and TX-mgmt paths around hostapd, plus the AP power-save datapath.

### Code map — morselib (new) ↔ Linux `morse_driver`
*Re-verified 2026-06-23 against `morse_driver` 1.17.8 (`~/halow/morse_driver`,
`version 0-rel_1_17_8_2026_Mar_24`); AP/TWT line refs spot-refreshed 2026-07-01.*

| # | Role | morselib (NEW code) | Linux `morse_driver` equivalent |
|---|---|---|---|
| 1 | **Advertise TWT-responder cap** in AP S1G caps | `ies/s1g_capabilities.c:318` `ie_s1g_capabilities_build_ap` → `DOT11_S1G_CAP_INFO_8_SET_TWT_RESPONDER_SUPPORT` | `mac.c:1275` `s1g_capab->capab_info[8] \|= S1G_CAP8_TWT_RESPOND` |
| 2 | **Enable responder role** on the AP vif | `twt/umac_twt.c:63` `umac_twt_init_vif(…, is_responder)`; called `interface/umac_interface.c:141` | `twt.c:2031` `morse_twt_init_vif()` (is_ap → `responder=true`); called `mac.c:3308` |
| 3 | **RX hook** — parse STA's TWT IE from (re)assoc-req | `supplicant_shim/supplicant_core.c:396` → `umac_twt_responder_handle_assoc_req` (`twt/umac_twt.c:389`, `ie_twt_find`) | `mac.c:6405` → `morse_mac_process_rx_twt_mgmt` (`twt.c:1727`) → `morse_mac_process_twt_ie` (`twt.c:1612`) |
| 4 | **Accept/reject policy** (REQUEST/SUGGEST→accept; DEMAND/GROUPING→reject) | `twt/umac_twt.c:389` (in `…_handle_assoc_req`) | `twt.c:1144-1178` `morse_twt_enter_state_consider_{request,suggest,demand,grouping}` |
| 5 | **Build ACCEPT IE** | `twt/umac_twt.c:445` `umac_twt_responder_build_response_ie` (`DOT11_TWT_SETUP_CMD_ACCEPT`) | `twt.c:1043` `morse_twt_send_accept` + `twt.c:101` `morse_twt_set_command` |
| 6 | **TX hook** — splice ACCEPT IE into (re)assoc-resp | `supplicant_shim/driver_ap.c:451` `mmwpas_send_mlme` | `mac.c:1861` (peek tx queue, `morse_twt_insert_ie` `twt.c:572`) |
| 7 | **Install agreement to fw** (cmd `0x26`) — *gated on AP vif; harmless* | `twt/umac_twt.c:491` `umac_twt_responder_install` → `umac_twt_install_agreement` (`:286`); hook `driver_ap.c:277` | `mac.c:5024` → `morse_twt_process_pending_cmds` (`twt.c:1355`) → `morse_cmd_twt_agreement_install_req` (`command.c:2211`) |
| 8 | **Agreement blob format** (15 B) | `twt/umac_twt.c:286` `umac_twt_install_agreement` | `twt.c:2326` `morse_twt_initialise_agreement` |
| 9 | **AP-side serving — deliver buffered downlink at the SP** ⭐ the load-bearing fix | `ap/umac_ap.c:890` `umac_ap_set_stad_sleep_state`: asleep→awake w/ queued frames → `umac_core_evt_wake()` (PM-bit at `umac_datapath.c:1528`) | `mac80211` PS buffering (`ieee80211_sta_ps_deliver_wakeup`) + `twt.c` wake-interval tree (`morse_twt_agreement_wake_interval_add` `twt.c:941`) |
| 10 | **Data structures** (per-STA table) | `twt/umac_twt_data.h`: `agreements[MMWLAN_AP_MAX_STAS_LIMIT]` + parallel `responder_peers[][6]`; alloc/lookup by SA; freed on leave (`mmwpas_sta_remove`) | `morse_twt` per-STA agreement (on the sta) + per-vif `twt_wake_interval_tree` |

**The decisive fix (row 9).** morselib's AP already buffers a dozing STA's downlink
(`umac_ap_queue_pkt` → traffic bitmap; `umac_ap_is_stad_paused` gates TX) and tracks sleep state from
each frame's PM bit (`umac_datapath.c:1528`). The bug: when the STA woke (PM=0 at its SP),
`umac_ap_set_stad_sleep_state` cleared the TIM bit but **never kicked the TX loop**, so buffered
downlink waited for the next beacon/DTIM rather than the STA's short SP → timeout. Fix = call
`umac_core_evt_wake()` on the asleep→awake transition when frames are queued — the morselib stand-in
for mac80211's `ieee80211_sta_ps_deliver_*`, driven by the TWT SP.

### Action-frame TWT path
*(done — see [`worklog/2026-06-24-twt-action-frame.md`](../worklog/2026-06-24-twt-action-frame.md).)* Added the
requester-side action-frame sender to morselib (`umac_twt_requester_tx_setup` / `_tx_teardown` /
`_handle_action`, public `mmwlan_twt_setup_request()` / `mmwlan_twt_teardown()`), mirroring
`morse_mac_send_twt_action_frame` + the requester half of `morse_mac_process_rx_twt_mgmt`. Verified on
HW: STA connects **without** assoc-IE TWT (flat ~12 ms), then a mid-session TWT-Setup **action** frame
engages doze (RTT spikes to ~1 s = the wake interval), and Teardown frees the agreement — confirmed
with two ESP32 STAs concurrently against the ESP32 AP. **Caveats:** (1) Linux-AP interop is blocked by
PMF — the Morse AP sends the Setup *response* CCMP-protected; morselib delivers it un-decrypted so the
STA can't install (needs RMF RX decryption). The request itself is accepted by the Linux AP. (2)
Teardown frees the AP + local slot but not the firmware agreement (fw cmd `0x27` unwired). The
**responder** also answers S1G unprotected action frames (cat 22) — TWT-Setup (action 6) with a
Setup-response carrying the ACCEPT IE, TWT-Teardown (action 7) frees the agreement
(`umac_twt_responder_handle_action`) — *responder action-frame path review-verified only* (the test STA
negotiates in assoc IEs; the two-STA HW test above drove Setup via action frames, `.159`/`.86` both
reaching "authorized STAs: 2").

### Caveats
- **Multi-STA, bounded table** — `UMAC_TWT_NUM_AGREEMENTS == MMWLAN_AP_MAX_STAS_LIMIT`;
  when full, the responder drops the IE (implicit reject; STA still associates without
  TWT). Linux keeps a dynamic per-STA list. HW-validated 1 AP + 2 STA.
- **No SP-overlap scheduling** — Linux's `twt_wi_tree` (`twt.c:941`) spaces SPs; this
  port serves each STA reactively on wake.
- **Downlink latency = TWT interval** — inherent TWT trade-off (buffered → next SP).
- **BCF board reference** — the runtime "BCF board description: mf16858…" is *correct*:
  `BOARD=proto1-fgh100m` loads the genuine FGH100M-H calibration (`bcf_fgh100mhaamd`, board ref
  `mf16858…`, from `vendor/morse-firmware`). *(Once cleared as a false lead.)*

Diff: 8 morselib files, ~245 insertions (`twt/umac_twt.{c,h}`, `twt/umac_twt_data.h`,
`ies/s1g_capabilities.c`, `interface/umac_interface.c`,
`supplicant_shim/{supplicant_core,driver_ap}.c`, `ap/umac_ap.c`).

---

## AP STA-count scaling (63 → 127 → 255) — multi-block S1G TIM port (S1)

The Mesh-gate only scales if the AP can hold many leaves. morselib originally capped
the AP at a **single S1G TIM block** (`MAX_SUPPORTED_AID = 64`, AIDs 1..63). This work
raised it to **two** (128 / AID ≤ 127) then **four** blocks (`MAX_SUPPORTED_AID = 256`,
AIDs 1..255 — 255 being the ceiling of the public `uint8_t mmwlan_ap_args.max_stas`).

**Key finding — morselib's S1G TIM is a port of the Linux `dot11ah/tim.c`.** They
share the constant *name and value* `S1G_TIM_MAX_BLOCK_SIZE = 256`
(`s1g_tim.h:10` ↔ `dot11ah/tim.h`), the 8-subblocks/block · 8-AIDs/subblock ·
64-AIDs/block geometry, the entire-page page-slice sentinel `31`/`0x1F`, and the
**same four PVB encoding modes** (`ENC_MODE_BLOCK/AID/OLB/ADE` ↔ morselib's four
`ie_s1g_tim_*_has_aid` parsers). The encoder loop was *already* generic over
`MAX_ENCODED_BLOCKS`, mirroring the driver — so morselib's single-block cap was a
self-imposed embedded-footprint limit, **not** a behaviour difference.

### Code map — morselib (changed) ↔ Linux `morse_driver` / `dot11ah`

| Concern | morselib (this work) | Linux equivalent |
|---|---|---|
| **AID space / per-AP traffic bitmap** | `ap/traffic_bitmap.h:22` `MAX_SUPPORTED_AID` 64→128→256; `S1G_BITMAP_SUBBLOCKS`; `ap/umac_ap_data.h:25` `bitmap[…]` | `morse.h:398` `MORSE_AP_AID_BITMAP_SIZE = AID_LIMIT + 1`; `:415` `DECLARE_BITMAP(aid_bitmap, …)` |
| **Block / subblock geometry** | `ies/s1g_tim.c` `MAX_SUBBLOCKS_IN_BLOCK = 8`; 8 bits/subblock → 64 AIDs/block | `dot11ah/tim.h:52` `S1G_TIM_NUM_SUBBLOCKS_PER_BLOCK = 8`; `…_AID_PER_SUBBLOCK = 8`; `…_AID_PER_BLOCK = 64` |
| **Per-block encoded-size limit** | `ies/s1g_tim.h:10` `S1G_TIM_MAX_BLOCK_SIZE = 256` (checked `s1g_tim.c:474`) | `dot11ah/tim.h` `S1G_TIM_MAX_BLOCK_SIZE = 256` *(identical name + value)* |
| **PVB encoder (build multi-block TIM)** | `ies/s1g_tim.c:397` `ie_s1g_tim_build` — `for (block < MAX_ENCODED_BLOCKS)` w/ `…_SET_BLOCK_OFFSET` | `dot11ah/tim.c:1030` `morse_dot11ah_insert_s1g_tim`; `block_offset = S1G_TIM_AID_TO_BLOCK_OFFSET(...)`, `block_k = block_offset + subblock_m/8` (`tim.c:278`) |
| **PVB encoding modes** | parsers `ie_s1g_tim_{block_bitmap,single_aid,olb,ade}_has_aid` | `dot11ah/tim.h` `enum dot11ah_tim_encoding_mode { ENC_MODE_BLOCK, _AID, _OLB, _ADE }` |
| **Whole-page TIM (no page slicing)** | `s1g_tim.c` `page_slice = 0x1F`, `page_index = 0` | `beacon.c:256` `page_slice_no = S1G_TIM_PAGE_SLICE_ENTIRE_PAGE (31)`, `page_index = 0` (`dot11ah/tim.h:63`) |
| **AID assignment / largest-AID** | `ap/umac_ap.c:513` `stas[aid]`, `aid < max_stas` (dense 1..N) | `mac.c:4934` `test_and_set_bit(aid, aid_bitmap)`; `mac.c:4614` `morse_aid_bitmap_update` (`find_last_bit`) |
| **Max-STA ceiling** | `mmwlan.h` `uint8_t max_stas` → 255; `Kconfig HALOW_AP_MAX_STAS` 1..255 | `hostap morse.h:164` `MAX_AID = 2007` (`= IEEE80211_MAX_AID`); `RAW_CMD_MAX_AID = 2007` |

### Divergence — per-STA state in PSRAM (no Linux equivalent)
`CONFIG_HALOW_STA_DATA_IN_PSRAM` routes each `umac_sta_data` **and** the per-vif TWT
agreement table to PSRAM (`umac_data.c`, `MALLOC_CAP_SPIRAM`, strict — no fallback).
**No Linux counterpart by design:** the kernel driver allocates per-STA state with
`kmalloc`/`GFP_KERNEL` in one address space, the `aid_bitmap` is a static
`DECLARE_BITMAP`, and per-STA TWT lives on mac80211's `ieee80211_sta`. The PSRAM split
is an ESP32-S3 adaptation to keep a large cap out of internal SRAM — a *platform*
change, not a port. (Likewise, the **fixed** `agreements[…]` table vs. Linux's *dynamic*
per-STA list is an embedded-RAM choice.)

### Progression + validation
| Cap | `MAX_SUPPORTED_AID` | Blocks | `MAX_SUPPORTED_PVB_LEN` | Status |
|---|---|---|---|---|
| 63 | 64 | 1 | 10 | original morselib |
| 127 | 128 | 2 | 20 | build + 3-node HW test |
| 255 | 256 | 4 | 40 | build + 3-node HW test |

Builds clean at each step (four-block static asserts pass); on-air, 1 ESP32 AP + 2
ESP32 STA + 1 chronium Linux STA associate concurrently (SAE), TWT power-save active,
no regression (V1). Worklogs: [`worklog/2026-06-23-ap-sta-ceiling-100-psram.md`](../worklog/2026-06-23-ap-sta-ceiling-100-psram.md),
[`worklog/2026-06-23-ap-sta-ceiling-255.md`](../worklog/2026-06-23-ap-sta-ceiling-255.md).

---

## 802.11s mesh point (morselib)

The mesh half of the Mesh-gate, built bottom-up in morselib to mirror the **Linux
`net/mac80211` mesh** + **morse_driver** reference. The Linux nodes (chronium/chronite) are
the oracle; the ESP firmware is matched against them. Test apps use `firmware/rimba-halow-mesh`
on the mesh subnet `10.9.9.0/24` (the AP work above uses `192.168.12.0/24`).

> **Function-level porting map:** the side-by-side **new-code ↔ Linux** table (each `umac_mesh_*`
> / datapath function ↔ its `net/mac80211` / `morse_driver` `file:line` equivalent) lives in
> [`rimba-mesh-80211s-code-map.md`](rimba-mesh-80211s-code-map.md); the security port in
> [`rimba-mesh-security-codemap.md`](rimba-mesh-security-codemap.md). The tables below are the
> *feature/status* view; those docs are the *code* view.

**An ESP32 joins a Linux 802.11s HaLow mesh and pings it:**
```
board0 (ESP, 10.9.9.136)  ->  chronite (Linux, 10.9.9.2)
reply from 10.9.9.2: seq=10 time=11 ms  (steady ~11-30 ms; first packets higher = ARP/HWMP setup)
```
Full control + data plane: beacon → peer (MPM) → path (HWMP) → IP, both as an **endpoint**
(`board0 → chronite(relay) → chronium`, 2-hop) and as a **relay** forwarding others' traffic
(`board1 → board0(ESP relay) → board2`), unicast and group/broadcast, open **and** secured.

### Phase checklist

| Phase | What | Status |
|---|---|---|
| P0 | Firmware accepts MESH(5) vif type | ✅ |
| P1 | Mesh vif up + periodic **S1G** beacon (Mesh ID/Config IEs) | ✅ |
| P2 | **MPM** peering (Open/Confirm/Close, FSM, timers) — ESP↔ESP + ESP↔Linux ESTAB | ✅ |
| P3 | S1G-beacon peer discovery (initiator) + S1G mesh beacon (not legacy) | ✅ |
| P4 | Mesh **data path** (4-addr/3-addr + Mesh Control), link-up, **HWMP** path resolution, **IP ping** | ✅ (single-hop) |
| P5 | HWMP **source role** (PREQ originate + PREP install + path table) + forwarding; **multi-hop ping** ESP→relay→far | ✅ (ESP endpoint) |
| P5b | ESP **data-frame relay** (ESP as an intermediate hop forwarding others' traffic) | ✅ |
| P6a | **Group/multicast forwarding** (re-broadcast + RMC dedup) — ARP traverses a relay, no static ARP | ✅ |
| P6b | **PERR broken-link teardown** + peer-inactivity link-failure detection | ✅ |
| P6c | **Mesh security** (SAE + AMPE + CCMP) ✅; **hardening** (airtime metric / power save / proxy-gate) ⬜ — [§Mesh security](#mesh-security--sae--ampe--ccmp-p6c) | 🟡 |
| P6d | **Single-hop / leaf toggle** (`mmwlan_mesh_set_multihop`) — runtime opt-out of relay + HWMP (keeps 1-hop peering, never relays, HWMP-invisible, no black hole; default-on no-op). On-air A/B verified — [worklog](../worklog/2026-06-30-mesh-leaf-single-hop-toggle.md) | ✅ |

### Mesh security — SAE + AMPE + CCMP (P6c)

The secured mesh is **implemented and on-air verified**, single-hop **and** multi-hop relay, both
ESP↔ESP and cross-vendor (ESP relay ↔ Linux endpoint), ported line-by-line from the Morse `hostap`
fork (`wpa_supplicant_s1g` mesh RSN) + `net/mac80211` AMPE. Codemap:
[`rimba-mesh-security-codemap.md`](rimba-mesh-security-codemap.md); worklogs
`2026-06-27-mesh-security-phase{1,2,3}-*.md`, `2026-06-30-mesh-security-sae-hardening.md`.

**What works.** **SAE** authentication (Dragonfly Commit/Confirm) → **AMPE** (Authenticated Mesh
Peering Exchange, encrypted peering) → **CCMP** data. `MMWLAN_MESH_SEC_PHASE1` compiles + links at
both 0 (open) and 1 (secured) (submodule `4bc732f7`). CCMP runs **host-side (SW-CCMP, P5)**, not the FW
key offload — required for multi-hop, because the MM6108 FW keys decryption by the mesh-SA/A4 and drops
forwarded (A4≠TA) frames (→ [#20](#backlog)). Open-vs-secured relay parity fixed (submodule
`e15870d0`): per-peer stads now allocated in both builds.

**Data path verified (2026-06-30).** chronite→chronogen (Linux↔Linux) 0% loss + 3 mpaths;
chronite→board0 (ESP) 0% loss; chronite→board1 **via board0** — multi-hop relay confirmed. (The
apparent "no mpath / 100% loss everywhere" during debugging was a **test artifact**: repeated
`wpa_supplicant_s1g` restarts had cleared chronite's manually-assigned mesh IP, so pings had no source
address → no HWMP demand → empty mpath. **Bench gotcha:** mesh IPs are **not** persistent across a wpa
restart/reboot — re-`sudo ip addr add 10.9.9.X/24 dev wlan1`; and `morse_cli mesh_config -2` is benign,
mac80211 does the forwarding.) **On air:** chronium captured the
secured line — beacons + broadcast DATA confirmed **CCMP-encrypted on the wire**.

**SAE hardening (GAP-C / #14 / #15)** — three hostap-parity fixes on the SAE state machine
(implemented 2026-06-30; codemap §"SAE hardening"):
- **GAP-C** — run `sae_parse_commit` (scalar-range + on-curve, on a throwaway SAE) before the
  ACCEPTED-state reauth free, so a *malformed* Commit can't flap a live link.
- **#14** — Sc/Rc + big_sync anti-replay on the ACCEPTED+Confirm resend (the `sae_rc` gate,
  `umac_mesh.c:1143`).
- **#15** — drop an unsolicited MPM Open from an untracked peer in a secured build (await beacon).

On air (chronite peer): board0+board1 reach ESTAB (no regression) and **re-ESTAB after a chronite
restart** (the validate gate doesn't deadlock).

**Defense-efficacy A/B (2026-07-01)** — a `wpa_supplicant_s1g` **"malicious-peer" injector** drove
definitive HARDENED-vs-BASELINE A/Bs on board0. The injector (3 `MESH_ATTACK` modes on chronium
`~/halow/hostap`, driven via a python `wpa_ctrl` DGRAM client) is a reusable tool; its committed diff is
[`docs/worklog/artifacts/sae-injector.patch`](../worklog/artifacts/sae-injector.patch) (memory
`sae-injector-tool`):
- **GAP-C** (`malformed-commit`): HARDENED keeps the attacker plink (`estab 1→1` through 5 malformed
  Commits); BASELINE (validate gate neutralised) tears it down (`1→0`, `mesh_sae_reauth_free`); control
  peer untouched. hostap (chronite) rejects the same Commit (`Invalid peer scalar`, 0 teardowns).
- **#14** (`confirm-replay`, cache-and-replay): HARDENED 5 replays → **0 resends**; BASELINE (gate off)
  → **4 resends**.
- **#15** (`unsolicited-open`): injector built, but a clean A/B is **not achievable on this live
  auto-peering bench** (board0 SAE-completes any beaconing node, so an Open never arrives untracked —
  disabling both beacon-peer-opens did not isolate the victim; a clean test needs raw src-spoofed
  injection). #15 stays **source-verified** (the drop is a simple untracked-peer gate).

Open security residuals are in the [backlog](#backlog): airtime metric / power-save / proxy-gate (P6c
hardening); the well-formed-forged-Commit reauth-DoS residual; #15 empirical A/B; #20 HW-crypto
multi-hop.

### Feature comparison: Linux (`net/mac80211`) vs ESP32 (morselib)

Legend: ✅ implemented · 🟡 partial/minimal · ⬜ not implemented · n/a not needed here

**Mesh interface & beaconing**
| Feature | Linux | ESP32 | Notes |
|---|---|---|---|
| Mesh vif (`NL80211_IFTYPE_MESH_POINT`) | ✅ | ✅ | `MMDRV_INTERFACE_TYPE_MESH` |
| S1G short beacon (ext type3/sub1) | ✅ | ✅ | host-built; firmware only auto-S1G's AP vifs |
| Mesh ID (114) + Mesh Configuration (113) IEs | ✅ | ✅ | path=HWMP, metric=airtime, sync=nbr-offset |
| S1G Capabilities/Operation + Beacon Compatibility IEs | ✅ | ✅ | |
| MBCA (beacon collision avoidance) | n/a (Linux TSF) | ✅ | morse firmware MESH_CONFIG/MBCA; ESP relies on it |
| Beacon timing / TBTT selection | ✅ | ✅ (fw) | firmware-served beacons |

**Peering — MPM (`mesh_plink.c`)**
| Feature | Linux | ESP32 | Notes |
|---|---|---|---|
| Peer link FSM (LISTEN→OPN_SNT→OPN_RCVD→CNF_RCVD→ESTAB→HOLDING) | ✅ | ✅ | mirrors mesh_plink.c |
| Self-Protected Open/Confirm/Close (cat 15) + Mesh Peering Mgmt IE (117) | ✅ | ✅ | link-id (llid/plid) handling |
| Open on heard candidate beacon (initiator) | ✅ | ✅ | from S1G-beacon discovery |
| Retransmit / holding timers (retry, MaxRetries) | ✅ | ✅ | + interval jitter |
| Stale-session guard (peer reboot → close/reopen) | ✅ | ✅ | llid-echo mismatch |
| Peer-inactivity expiry (link-failure detection) | ✅ | ✅ | ESTAB idle > 6 s → Close + flush paths (`ieee80211_sta_expire`) |
| `user_mpm` vs driver MPM | both | driver/host | ESP runs MPM in morselib |
| Max peer links | configurable | 🟡 4 | small fixed table |
| **Authenticated peering (AMPE/SAE)** | ✅ | ✅ | secured mesh (host SW-CCMP) — [§Mesh security](#mesh-security--sae--ampe--ccmp-p6c) |

**Path selection — HWMP (`mesh_hwmp.c`)**
| Feature | Linux | ESP32 | Notes |
|---|---|---|---|
| Reply to PREQ targeting us → **PREP** (target role) | ✅ | ✅ | lets a peer resolve a path to us |
| Originate PREQ for an unknown dest (source role) | ✅ | ✅ | `umac_mesh_start_discovery`; path-based TX with direct fallback |
| Accept PREP / build mesh **path table** | ✅ | ✅ | `mesh_path_*`; fresh-info rule (SN/metric); next-hop lookup |
| PERR (path error / broken link) | ✅ | ✅ | `umac_mesh_invalidate_paths_via` flushes paths via a lost next hop + announces each (one dest/PERR); RX tears down + floods on |
| RANN / root mode / proactive PREP | ✅ | ⬜ | |
| Airtime link metric | ✅ | 🟡 | fixed per-hop cost (accumulates correctly); not rate-derived |
| Gate announcement protocol | ✅ | ⬜ | |

**Forwarding & data path (`mesh.c`, `mesh_pathtbl.c`)**
| Feature | Linux | ESP32 | Notes |
|---|---|---|---|
| Mesh Control header (flags/ttl/seqnum) on data | ✅ | ✅ | TX insert + RX strip; QoS "Mesh Ctrl Present" bit |
| 4-addr unicast (toDS=fromDS) | ✅ | ✅ | A1=nexthop, A2=us, A3=DA, A4=SA |
| 3-addr group-addressed (fromDS) | ✅ | ✅ | bcast/mcast (ARP etc.) |
| Address Extension (AE_A4 / AE_A5_A6) for proxy/multi-hop | ✅ | 🟡 | RX parses AE length; TX uses flags=0 (no proxy) |
| Multi-hop forwarding — **ESP as endpoint** over a relay | ✅ | ✅ | demoed board0→chronite→chronium, 2-hop ping |
| Multi-hop forwarding — **ESP as relay** (forward others' data) | ✅ | ✅ | `umac_mesh_forward_data`; demoed board1→**board0(ESP)**→board2 |
| Proxy path table (mpp — non-mesh STAs behind us) | ✅ | ⬜ | the Mesh-gate's eventual bridge to AP leaves |
| Mesh gate / portal to external net | ✅ | ⬜ | |
| Mesh seqnum + duplicate cache (RMC) | ✅ | ✅ | per-(SA,seqnum) cache; suppresses bcast loops |
| Group/multicast forwarding (re-broadcast bcast/mcast) | ✅ | ✅ | `umac_mesh_handle_group_data`; ARP traverses a relay (no static ARP) |

**Synchronization, power save, security**
| Feature | Linux | ESP32 | Notes |
|---|---|---|---|
| Mesh synchronization (neighbor offset, `mesh_sync.c`) | ✅ | n/a | advertised sync=nbr-offset; timing handled by firmware/MBCA |
| Mesh power save (`mesh_ps.c`, peer service periods) | ✅ | ⬜ | always-on |
| SAE authentication + AMPE | ✅ | ✅ | secured mesh, host SW-CCMP — [§Mesh security](#mesh-security--sae--ampe--ccmp-p6c) |
| Management Frame Protection (MFP/PMF) | ✅ | 🟡 | AMPE installs group keys (MGTK/IGTK); full unicast-mgmt PMF not separately audited |
| Congestion control | ✅ (mode field) | ⬜ | advertised "none" |

### Known limitations / next
- **HWMP gaps**: fixed per-hop metric (not airtime), no RANN/root/gate. PERR teardown is one
  destination per frame (mac80211 packs many). Unicast-relayed frames regenerate TTL/seqnum
  (group frames preserve the originator's seqnum for RMC dedup) — fine for trees/lines. Paths
  expire after 30 s; a broken next hop is torn down within ~6 s (peer-inactivity) instead.
- **No proxy / gate** (`mpp`), so an ESP can't yet bridge non-mesh traffic or act as a portal —
  this is exactly what the Mesh-gate needs to bridge its AP leaves onto the mesh.
- **No mesh power save.** Security is done (SAE+AMPE+CCMP, host SW-CCMP); MFP is partial (see table).

### Forced-topology test note
On an all-in-range bench HWMP resolves a direct 1-hop path even between non-peers (PREQ/PREP +
data flood over RF regardless of plinks). To demo real multi-hop the ESP firmware has test
scaffolding (app `MESH_LINE_RELAY_DEMO` / `MESH_MULTIHOP_DEMO`): a per-node **peer allowlist**
(`mmwlan_mesh_set_peer_allowlist`) so a node only *peers* with its chosen neighbour(s), plus
filters that ignore HWMP and data frames whose immediate transmitter isn't an allowed peer, so a
node only *reaches* others via its relay. Two topologies demoed: `board0→chronite→chronium` (ESP
endpoint, Linux relay) and `board1→board0(ESP)→board2` (ESP relay; ARP relayed via group
forwarding). None of this is production behaviour — it only forces a line on an all-in-range bench.
**RF-forcing does *not* work here:** even at 1 dBm (`tx_max_power_mbm` / `mmwlan_override_max_tx_power`)
board1↔board2 stays ~−52 dBm, ~40 dB above sensitivity — the boards are too close, so the allowlist
is still required for a multi-hop demo (#23).

### On-air frame verification (chronium monitor)
Every ESP mesh frame type was captured **on the air by a third node** — chronium as a
`CONFIG_MORSE_MONITOR` sniffer on S1G ch27 ([`reference/rimba-linux-halow-monitor.md`](../reference/rimba-linux-halow-monitor.md)) — and
decoded byte-for-byte against both the ESP encoders (`umac_mesh.c`) and the Linux `mesh_hwmp.c`
element layouts: **4-addr data** (QoS Mesh-Control-Present bit, ttl=31, seqnum, A4/mesh-SA
preserved through the relay), **PREQ** (lifetime, per-hop metric +100, ttl decrement),
**PREP** (target/orig, metric accumulation), **PERR** (ttl/num_dest/target/sn, reason 62, plus
forward at ttl−1), and **peering** (cat 15). The element **structure** is byte-identical to the
Linux layouts — not just functionally accepted by a peer.

A further A/B against a **live Linux node** (chronite brought up as a mesh node, its frames
captured on the same monitor) confirmed the structure but surfaced **3 value-level deltas** the
source-format check missed — **all since fixed and re-verified on air**: group/bcast **QoS Ack
Policy** (No-Ack `0x20`), **PREQ Target-Only flag** (set on refresh), **PREQ lifetime** in TUs
(`MSEC_TO_TU`). The fixed board0 now byte-matches live chronite (`target_flags=01`,
`lifetime=0x7270`, group `QoS=0x20`). Also captured live from chronite: **beacon, group ARP, PREQ,
and PREP**. A live Linux **PERR** was *not* elicitable — it's a forwarder's frame, and a single
Linux mesh endpoint doesn't PERR on its own next-hop loss (the morse driver gives mac80211 no
TX-failure feedback). Decode tables:
[`worklog/2026-06-26-mesh-mpm-peering-frames.md`](../worklog/2026-06-26-mesh-mpm-peering-frames.md)
Updates 13–15.

---

## A3 — Mesh + AP concurrency on one radio

**The one remaining structural gap for a full all-ESP32 Mesh-gate.** On chronium one MM6108 ran AP
(`hostapd_s1g`) + open 802.11s mesh-point (`iw … mesh join`) **co-channel** (ch27) + a TWT'ing ESP32
STA, all at once — recipe + gotchas (`type mp` needs explicit `iw set type`; distinct
locally-administered MAC; bare `freq` not `HT20`) in
[`reference/rimba-linux-node-setup.md`](../reference/rimba-linux-node-setup.md) §12.

The earlier blocker — **no 802.11s in morselib** — is resolved: the ESP32 mesh point is built + secured
(P0–P6d). What's left for A3 is bringing up an ESP32 **mesh vif and a SoftAP vif co-channel on the same
MM6108** (the firmware allows one channel; the host must run both MLME paths at once) and re-running the
concurrency + TWT test all-ESP32. ([`worklog/2026-06-22-mesh-ap-twt.md`](../worklog/2026-06-22-mesh-ap-twt.md))

---

## Performance — mesh vs AP↔STA (iperf goodput + ping RTT)

Goodput/latency matrix across node types on 1 MHz S1G (72 Mbit/s PHY, but airtime-bound → sub-Mbps).
Full run: [`worklog/2026-06-26-mesh-ap-perf-iperf-ping.md`](../worklog/2026-06-26-mesh-ap-perf-iperf-ping.md).

**TCP goodput (1 MHz S1G)**
| | Linux↔Linux | Linux↔ESP32 | ESP32↔ESP32 |
|---|---|---|---|
| **Mesh** (1 hop) | **0.20 Mbps** (open) | **0.13 Mbps** | **0.16 Mbps** |
| **AP↔STA** | **0.79 Mbps** (WPA3) | **1.10 Mbps** | **0.84 Mbps** |
| **Mesh multi-hop** (2 hops) | — | — | **~0.03–0.06 Mbps** (board1→board0→board2) |

**Ping RTT (median)**
| | Linux↔Linux | Linux↔ESP32 | ESP32↔ESP32 |
|---|---|---|---|
| **Mesh** (1 hop) | ~25 ms | ~11–20 ms | ~18 ms (0% loss) |
| **AP↔STA** | ~14 ms | ~10–15 ms | ~15 ms (0% loss) |
| **Mesh multi-hop** (2 hops) | — | — | ~50 ms (35–59 ms, occasional ~850 ms spike, ~8% loss) |

**Headlines.** (1) **AP↔STA single-hop (~0.8–1.1 Mbps) is ~5× the mesh single-hop (~0.13–0.20 Mbps)** —
the mesh's 4-addr + Mesh-Control overhead and HWMP cost real airtime on a 1 MHz channel. (2) The
**second hop roughly quarters goodput** (0.16 → ~0.04) and **doubles RTT** (~18 → ~50 ms), and multi-hop
runs stall mid-test on path re-discovery under load (30 s path lifetime). (3) Within a row **node type
barely matters** — channel airtime dominates, not Linux-vs-ESP.

**Secured-vs-open relay (iperf UDP, ESP line board1→board0(relay)→board2).** open-plaintext relay
**~0.14 Mbps** vs secured-CCMP relay **~0.23 Mbps** — same order of magnitude (high 1 MHz variance), so
**SW-CCMP crypto is NOT the dominant relay cost** — per-hop forwarding + airtime dominate; security is
cheap in throughput terms. A concurrent-load run showed the multi-hop relay saturates (board1 88% loss
when its relay ESP also carries its own traffic) — the host SW-CCMP relay bottleneck (→ [#20](#backlog)).

**Linux↔Linux baseline** (signal −21 dBm, PHY 72 Mbit/s VHT-MCS7): mesh (open, `10.9.9.x`) = TCP
**203 Kbit/s** / UDP **582 Kbit/s** @0% loss; AP↔STA (WPA3-SAE/CCMP, `192.168.12.x`) = TCP **786 Kbit/s**
(downlink, `-R`) / UDP **472 Kbit/s** @2.6% loss.

**Method.** Dedicated perf firmware keeps the production apps clean:
`firmware/rimba-halow-{mesh,ap,sta}-perf`, each = the production app's bring-up + an `esp_console` REPL
with `iperf` (`espressif/iperf-cmd`, v2, port 5001) **and `ping` (`esp-qa/ping-cmd`)**, no auto-ping/TWT.
Drive over serial; **ping first to warm ARP/HWMP, then iperf** (the no-auto-ping build's first TCP
connect otherwise fails `errno 118`). L↔L uses `iperf3` 3.18 (chronite/chronium); the ESP ends use
iperf v2 — don't cross v2 ⇎ iperf3. **Test gotchas:** offered UDP rate must stay under the link ceiling
(`-b 5M` → 70% loss, `-b 500k` clean); report both TCP directions (an AP→STA 786 / STA→AP ~0 asymmetry
was seen). Multi-hop reuses the line allowlist + needs all 3 boards reset together (else stale peer
state breaks the line). (`rimba-halow-ap-perf` needs a 2 MB app partition — hostapd + the two cmd
components overflow the default ~1 MB.)

---

## Backlog

Open items only (resolved milestones are above). Each = marker + one line + pointer.

**Mesh**
- ☐ **P6c mesh hardening** — rate-derived **airtime metric** (currently a fixed per-hop cost,
  unverifiable on a single-path forced line), **RANN/root** mode, **proxy/gate (`mpp`)** so the
  Mesh-gate can bridge AP leaves onto the mesh, **mesh power save**. Derive each from
  `net/mac80211/mesh*.c` + `morse_driver/mesh.c`.
- ☐ **SAE hardening residual** — a *well-formed* forged Commit still tears a live link down (hostap
  itself reaches `ap_free_sta` for any such frame), so closing it fully needs a **non-hostap rate-limit
  on ACCEPTED-state reauth** (a deliberate divergence from the line-by-line port, deferred); plus the
  **#15 unsolicited-Open A/B**, which needs raw src-spoofed injection (see
  [§Mesh security](#mesh-security--sae--ampe--ccmp-p6c)).
- ☐ **#20 — HW-crypto multi-hop forward (FW A4-sensitivity)** — backlog, low priority. In HW-crypto mode
  the MM6108 FW drops a foreign-A4 (A4≠TA) 4-addr mesh forward; the secured mesh ships on **host SW-CCMP
  (P5)** which sidesteps it, so it's **not a blocker**. The drop is A4-sensitive *firmware* behaviour
  (morselib's RX path is A4-agnostic) and the same FW delivers the forward on Linux, so the cause is an
  unpinned closed-FW config difference. Revisit only if a HW-crypto multi-hop path is ever wanted (worklog
  §#20/#25/#26 + codemap #20).
- ✅ **ESP↔ESP-direct peering — RESOLVED / was a visibility misdiagnosis (verified 2026-07-01).** Two ESP
  secured-mesh nodes bootstrap a mesh between themselves with **no Linux anchor**: cold-reset both (chronite
  down), each reaches full secured **ESTAB** within ~5 s (SAE+AMPE, both directions, ~10 beacons/s mutual
  reception), and a **direct ESP↔ESP ping runs 0% loss** (~27 ms steady). The 2026-06-30 "both beacon but
  never peer" was *absence of evidence*: morselib `MMLOG` doesn't reach the UART console and the default app
  exposed no peer state or ping, so with two ESPs alone there was no console-visible peering signal at all
  (the 1.17.9 firmware was already in the build then; no peering-path code changed since). Now guarded by an
  `estab_peers` + peer-MAC line in the app heartbeat (`mmwlan_mesh_peer_count`). Same shape as the earlier
  monitor-build RX misdiagnosis.
- ◑ **Multi-node stress + tighter secured-vs-open table.** *Tighter table DONE (2026-07-01)* via a forced
  3-ESP line (board1→board0(relay)→board2, `MESH_LINE_RELAY_DEMO`, `-perf` app driven over the iperf console),
  on-air S1G ch27. **Multi-hop UDP goodput: secured (SAE+AMPE+CCMP) ~0.26 Mbit/s (0.24–0.27, 4 samples) vs open
  plaintext ~0.16 Mbit/s (0.16–0.17, 3 samples)** — reproducibly the same order, secured even slightly *faster*
  (matches the prior 0.23/0.14), so **CCMP is not the relay bottleneck** (per-hop forwarding + 1 MHz airtime
  dominate). **TCP collapses to 0** through the relay (both builds — congestion collapse under saturation).
  Separately, the **3-Linux secured mesh (chronite/chronosalt/chronogen, chronosalt recovered) forms + holds**
  stably. Bench notes: board2 is PPK2-powered → restore with **`tools/ppk2_hold.py`** (enumerates as `ttyACM4`,
  cf. `docs/reference/rimba-bench-devices.md`); the `-perf` iperf console resets the ESP if the serial helper
  toggles DTR/RTS — leave them alone. ☐ **Still open:** an *unconstrained* full ESP+Linux mesh under load — the
  all-in-range full-mesh settles into a stable *partial* topology (flappy-full-mesh), so a clean N-node stress
  needs a forced topology or full-mesh stability work.

**Cleanup (before merge)**
- ☐ **Revert forced-topology scaffolding** — `MESH_LINE_RELAY_DEMO` / `MESH_MULTIHOP_DEMO` (peer
  allowlist + HWMP/data TA filters).
- ☐ **Remove unused firmware scaffolding** — the `MESH_IPERF` (+ `espressif/iperf-cmd` dep + `console`
  REQUIRES), `MESH_LINUX_INTEROP` app toggles; the disabled static-ARP path in `app_main.c`
  (`g_static_arp_*`, superseded by group forwarding); one-off test apps (`rimba-halow-meshscan`,
  `rimba-halow-mesh-monitor`). Goal: a clean "bring up a mesh + use it" demo with no dead toggles.

**AP / TWT / RAW**
- ☐ **AID ≥ 64 on air** — the 2nd–4th TIM blocks aren't exercised (the dense allocator only reaches
  block 1+ at 64+ live associations, beyond the 3-board bench).
- ☐ **Linux STA as TWT *requester*** vs the ESP32 AP responder — the strongest responder interop test.
  Needs the morse driver's requester-role bring-up (`twt_requester=1` global + assoc-time negotiation;
  `morse_cli twt conf` alone returns -1 / "non-requester").
- ☐ **RAW (Restricted Access Window) — AP-side, port from Linux.** morselib has only RAW *types/caps*
  today (`MORSE_CAPS_RAW`, the S1G cap-6 bit, `raw_priority`) — **no implementation**. Port from
  `morse_driver` `raw.c` (1742 lines) + `page_slicing.c`: RPS IE (`morse_raw_get_rps_ie` /
  `_generate_rps_ie` / `_generate_assignment`), AID-list grouping (`morse_generate_aid_list`), slot
  definition, beacon insert. Follow Linux exactly; recon/feasibility pass first; big feature — own
  branch + PR.
- ☐ **SP-overlap scheduling** — port Linux's `twt_wi_tree` SP spacing (`twt.c:941`); matters only when
  many leaves share tight wake intervals.
- ☐ **µA current measurement** of a fully-idle TWT link — blocked by no bench power-enable line / meter
  (board: RESET_N only, BUSY/WAKE on DNP pads — RISK-02).

**Infra**
- ☐ **Regression suite** for every built feature (hello / scan / AP-STA / IBSS / TWT / Mesh+AP) so
  firmware/morselib bumps don't silently regress earlier milestones.

**Known unknown (not a task):** the MM6108 firmware's *true* concurrent-STA capacity is unpublished
(Linux caps at 2007; spec 8191) — 255 is a build/structural ceiling, not a firmware guarantee.

---

## Methodology — how future Mesh-gate (and Rimba) features get built

Codified in [`.ai/AGENTS.md`](../../.ai/AGENTS.md):

1. **Derive from Linux.** Root-cause against `morse_driver` / `net/mac80211` (same silicon
   Linux drives) and follow it; don't tolerate a symptom with a divergent local hack.
2. **Verify on hardware** (or unit test) — and if you *can't*, document why (e.g. "AID ≥ 64
   needs 64+ associations; not reproducible on a 3-board bench").
3. **On-air verify against Linux** (two tiers; the gold standard is the bar). For any frame the
   ESP transmits, capture it on the air with **chronium's `morse0` monitor**
   ([`reference/rimba-linux-halow-monitor.md`](../reference/rimba-linux-halow-monitor.md)) and byte-diff it.
   *Floor:* match the Linux **source layout** (`net/mac80211` + `morse_driver` element definitions).
   ***Gold standard:*** match what a **live Linux device actually transmits** on the bench — this pins
   field *values*, units, and flag bits the spec check misses (it's how the TO-flag / lifetime-units /
   No-Ack deltas were found *after* the structure already matched; see [§On-air frame verification](#on-air-frame-verification-chronium-monitor)).
   Endpoint serial logs + a working ping only prove a tolerant peer *accepted* the frame. Record, per
   frame, which tier it reached (log-only / source layout / live device).
4. **Write the new-code ↔ Linux map** for any port (as the two code-map tables above), and
   call out deliberate divergences (PSRAM, fixed-size tables) with the reason.
5. **Cite sources** — `file:line`/SHA for code, command+output for hardware, URLs for
   external facts; prefer authoritative (vendored source) over marketing.

---

## Build / test

```bash
make build APP=rimba-halow-ap  BOARD=proto1-fgh100m   # SoftAP (cap/PSRAM via sdkconfig.defaults)
make build APP=rimba-halow-sta BOARD=proto1-fgh100m   # a TWT-requester leaf
make build APP=rimba-halow-mesh BOARD=proto1-fgh100m  # 802.11s mesh point
make flash APP=rimba-halow-ap  BOARD=proto1-fgh100m PORT=/dev/ttyACM0
```

chronium/chronite as Linux nodes (interop oracle) — `wpa_supplicant_s1g`, SAE, **freq 5560**
(S1G ch27 in the 5 GHz model; on-air 915.5 MHz); full recipe in
[`worklog/2026-06-23-ap-multinode-twt-hwtest.md`](../worklog/2026-06-23-ap-multinode-twt-hwtest.md) §3.
Mesh bring-up + the Pi Zero 2 W nodes: [`reference/rimba-linux-node-setup.md`](../reference/rimba-linux-node-setup.md).

## Reference source revisions

The ESP32 AP/TWT and 802.11s mesh code is matched line-for-line against these exact Morse Micro
upstream revisions (checked out on the Pi 5 reference nodes chronite/chronium). The two **primary**
references are the patched kernel's `net/mac80211` stack (mesh + TWT/PS) and the out-of-tree
`morse_driver`:

| Component | Repo | Branch / version | Commit |
|---|---|---|---|
| **Kernel** (`net/mac80211/{mesh*,twt}.c`, dot11ah) | `github.com/MorseMicro/rpi-linux` | `mm/rpi-6.12.21/1.17.x` (Linux 6.12.21) | `372414fd42cdd4d8bfcf888cac62db9da947fdb6` |
| **morse_driver** | `github.com/MorseMicro/morse_driver` | `1.17.8` (`0-rel_1_17_8_2026_Mar_24`) | `3eef5a0a43645808e501ff4b83f29d675588bd9b` |

Supporting references (S1G beacon conversion, peering/SAE supplicant, on-air checks):

| Component | Repo | Commit |
|---|---|---|
| morse-firmware (mm6108 + BCF) | `github.com/MorseMicro/morse-firmware` | `ea18605f53387281c1029ea9eb8e0de2bda7bae2` |
| hostap (`wpa_supplicant_s1g` / hostapd + mesh RSN) | `github.com/MorseMicro/hostap` | `4acb6f6f46380d3c9fe50da77aa15a6ba565c49d` |
| morse_cli | `github.com/MorseMicro/morse_cli` | `8f06222bee104327b5f09a9339f24bac1ef3420d` |

Mesh files leaned on: `mesh_plink.c` (MPM FSM/timers), `mesh_hwmp.c` (PREQ/PREP/PERR,
`mesh_path_error_tx`, `hwmp_perr_frame_process`, airtime metric), `mesh_pathtbl.c` (path table),
`mesh.c` (`ieee80211_sta_expire`/`mesh_sta_cleanup` inactivity, group forwarding, `mesh_rmc_check`),
and the driver's `morse_dot11ah_beacon_to_s1g` (S1G short-beacon build). AP/TWT files:
`morse_driver/{twt.c,mac.c,command.c,beacon.c,dot11ah/tim.c}`. Mesh security: the `hostap` fork's
`wpa_supplicant/{mesh_rsn,mesh_mpm}.c` + `src/common/sae.c` + `net/mac80211/mesh_plink.c` (AMPE).

## References

- Mesh worklogs (decoded frames + per-phase implementation): [`worklog/2026-06-26-mesh-mpm-peering-frames.md`](../worklog/2026-06-26-mesh-mpm-peering-frames.md) (P2–P6b), [`worklog/2026-06-25-mesh-p1-vif-beacon.md`](../worklog/2026-06-25-mesh-p1-vif-beacon.md) (P1), [`worklog/2026-06-26-linux-mesh-reference.md`](../worklog/2026-06-26-linux-mesh-reference.md) (Linux bring-up), [`worklog/2026-06-24-mesh-80211s-port-recon.md`](../worklog/2026-06-24-mesh-80211s-port-recon.md) (recon/P0)
- Mesh security (SAE/AMPE/CCMP + SAE hardening + injector A/B): [`worklog/2026-06-27-mesh-security-phase3-sae.md`](../worklog/2026-06-27-mesh-security-phase3-sae.md), [`worklog/2026-06-30-mesh-security-sae-hardening.md`](../worklog/2026-06-30-mesh-security-sae-hardening.md), codemap [`rimba-mesh-security-codemap.md`](rimba-mesh-security-codemap.md), injector [`worklog/artifacts/sae-injector.patch`](../worklog/artifacts/sae-injector.patch)
- Leaf / single-hop toggle (P6d): [`worklog/2026-06-30-mesh-leaf-single-hop-toggle.md`](../worklog/2026-06-30-mesh-leaf-single-hop-toggle.md)
- Performance (iperf throughput + ping latency, mesh vs AP↔STA): [`worklog/2026-06-26-mesh-ap-perf-iperf-ping.md`](../worklog/2026-06-26-mesh-ap-perf-iperf-ping.md)
- Worklog (Mesh+AP+TWT blow-by-blow + firmware byte-comparison): [`worklog/2026-06-22-mesh-ap-twt.md`](../worklog/2026-06-22-mesh-ap-twt.md)
- Worklogs (STA-count): [`worklog/2026-06-23-ap-sta-ceiling-100-psram.md`](../worklog/2026-06-23-ap-sta-ceiling-100-psram.md), [`worklog/2026-06-23-ap-sta-ceiling-255.md`](../worklog/2026-06-23-ap-sta-ceiling-255.md), multi-node test [`worklog/2026-06-23-ap-multinode-twt-hwtest.md`](../worklog/2026-06-23-ap-multinode-twt-hwtest.md)
- TWT action-frame path: [`worklog/2026-06-24-twt-action-frame.md`](../worklog/2026-06-24-twt-action-frame.md)
- Linux node + Mesh/AP/TWT bring-up (incl. Pi Zero 2 W + TX power): [`reference/rimba-linux-node-setup.md`](../reference/rimba-linux-node-setup.md)
- Power-save context (why TWT matters for leaves): [`design-specification/rimba-mm6108-powersave-analysis.md`](../design-specification/rimba-mm6108-powersave-analysis.md)
- Linux driver source on the Pi 5 nodes: `~/halow/rpi-linux/net/mac80211/`, `~/halow/morse_driver/`
