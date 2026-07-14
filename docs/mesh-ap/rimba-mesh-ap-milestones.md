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
The last structural gap for a full all-ESP32 Mesh-gate — **mesh + AP concurrency on one ESP32 radio**
([§A3](#a3--mesh--ap-concurrency-on-one-radio)) — is now **PROVEN end-to-end on the ESP32 (2026-07-10)**:
concurrent co-channel mesh+AP beaconing, plus a STA under the AP routing to a 2nd mesh node and back
(ping 10/10 + iperf both ways). Remaining work is hardening (de-hardcode the return gw, broadcast
forwarding, wider coverage) + the deferred per-stad/per-vif ops dispatch for untested mixed-flow cases.

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
| Status | **RESOLVED + hardened + soaked** (RISK-01) | **AP + TWT + STA-scaling proven**; mesh built + secured (P0–P6d); mesh+AP concurrency **proven on Linux end-to-end w/ traffic routing** (§A3 recipe), not yet on ESP32 |

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
| **A3** | [Mesh + AP concurrency on one radio](#a3--mesh--ap-concurrency-on-one-radio) — **PROVEN end-to-end on ESP32 (2026-07-10)**: co-channel mesh+AP + STA routes through the gate to a 2nd mesh node & back (ping 10/10 + iperf both ways). Linux gateway also proven (8/8). Hardening + deferred per-stad ops dispatch remain | ✅ |
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
| Peer-inactivity expiry (link-failure detection) | ✅ | ✅ | ESTAB idle > `plink_timeout` **1800 s** (= Linux `MESH_DEFAULT_PLINK_TIMEOUT`, `ieee80211_sta_expire`) → Close + flush paths; `last_rx` refreshed on beacon **+ any received data frame**. Was **6 s / beacon-only** — ~300× too aggressive, flapped → fresh SAE on marginal RX; matched to Linux 2026-07-12. [liveness code-map](rimba-mesh-plink-liveness-codemap.md) · [worklog](../worklog/2026-07-12-mesh-peering-flap-bisect-no-regression.md) |
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

**Linux side: PROVEN end-to-end with real traffic (2026-07-05, stock 1.17.8).** One MM6108 (chronite) runs
an 802.11s **mesh point** and a **SoftAP** co-channel (ch27), and an ESP32 STA under the AP **routes through
the mesh** to a second mesh node: `board1 → chronite AP → ip_forward → HaLow mesh → chronosalt`, **8/8
pings**. This section is the **source-of-truth bring-up recipe** — the Linux gateway is the reference
oracle for the all-ESP32 A3 port. The ESP32 side (mesh vif + SoftAP vif co-channel on one MM6108) is still
pending. ([`worklog/2026-06-22-mesh-ap-twt.md`](../worklog/2026-06-22-mesh-ap-twt.md))

### Working recipe — Linux Mesh+AP gateway (copy-paste; verified)

Config files persist on each node (`chronite:~/wpa-mesh.conf`, `~/hostapd-rimba.conf`,
`chronosalt:~/wpa-mesh.conf`); full templates + the mesh config fields are in
[`reference/rimba-linux-node-setup.md`](../reference/rimba-linux-node-setup.md) (§802.11s mesh + AP
sections). This recipe was **verified on stock 1.17.8**. (An earlier session reported a "1.17.9 broke
concurrency" regression, but that debugging was confounded by the chip-wedge + mesh-IP artifacts below, so
the version claim is **suspect and unverified** — re-test cleanly on 1.17.9 before trusting it. The recipe
itself is version-agnostic.) Connect to nodes by hostname (`ssh chronite`), never raw IP.

Roles: **mesh on the PRIMARY vif (`wlan1`), AP on a SECONDARY vif (`ap0`)** — this ordering is the one that
works. (Mesh on a *secondary* vif stays `INACTIVE`/won't join; AP on the primary + mesh on secondary leaves
the mesh dead. Primary=mesh, secondary=AP.)

**0. Fresh chip — do NOT churn.** Rapid `wpa_supplicant_s1g` restarts **wedge the MM6108** (mesh gets stuck
`SCANNING`/won't peer). Bring each daemon up *once* and leave it. This single rule prevents ~all of the
"mesh won't peer / concurrency flaky" ghosts. **To un-wedge — the fast way beats a reboot on the Pi Zeros:**
- **Pi Zero (chronosalt/chronogen)** — the reset-GPIO (BCM **GPIO5**) is wired, so a **driver re-probe
  resets the chip** in ~6 s (reloads fw, no reboot, no `/tmp` wipe):
  `echo spi0.0 | sudo tee /sys/bus/spi/drivers/morse_spi/unbind; echo spi0.0 | sudo tee /sys/bus/spi/drivers/morse_spi/bind`
  (or `sudo modprobe -r morse; sudo modprobe morse`). Verify `dmesg | grep "mm6108.bin, size"` reloaded.
- **Pi 5 (chronium/chronite)** — **with the driver patch** (`reference/patches/morse-driver-pi5-reset-gpiod.patch`,
  applied to the current bench build), the Pi 5 driver reload/re-probe resets the chip automatically, same
  as the Pi Zero: `unbind/bind` or `modprobe -r morse; modprobe morse` → `Resetting Morse Chip` → fw reloads,
  no reboot. On a **stock (unpatched)** driver the Pi 5's reset silently no-ops (`morse_chip_cfg_detect_and_init
  failed: -5`) because stock `morse_hw_reset()` uses the legacy integer-GPIO API (fails on the RP1 controller)
  and floats the pin to release (the HAT's `RESET_N` has no pull-up); recover it by pulsing GPIO17 by hand
  between unbind and bind — `sudo pinctrl set 17 op dl; sleep 0.3; sudo pinctrl set 17 op dh` (BCM numbering
  via `pinctrl`, NOT `gpiofind`/`gpioset`) — or reboot. The patch removes the need for either. See the patch
  README for the full root-cause.

**1. Gateway (chronite) — mesh on `wlan1` (primary):**
```sh
sudo wpa_supplicant_s1g -B -D nl80211 -i wlan1 -c wpa-mesh.conf   # mode=5, user_mpm=1, op_class=68 ch27 dtim_period=1
sudo ip addr add 10.9.9.3/24 dev wlan1        # ← MANDATORY (see gotcha #2). Verify: wpa_state=COMPLETED
```

**2. Gateway — AP on `ap0` (secondary):**
```sh
WMAC=$(cat /sys/class/net/wlan1/address); AMAC=$(echo $WMAC | sed 's/^../3a/')   # distinct locally-admin MAC
sudo iw phy phy1 interface add ap0 type __ap addr $AMAC
sed 's/^interface=.*/interface=ap0/' hostapd-rimba.conf > /tmp/hap0.conf         # SSID rimba-ping, SAE, ch27
sudo hostapd_s1g -B /tmp/hap0.conf            # ← FIRST start often "nl80211 driver initialization failed"
# if it failed: sudo pkill -x hostapd_s1g; sleep 2; sudo hostapd_s1g -B /tmp/hap0.conf   → AP-ENABLED (gotcha #3)
sudo ip addr add 192.168.12.1/24 dev ap0; sudo ip link set ap0 up
sudo sysctl -w net.ipv4.ip_forward=1
```

**3. Second mesh node (chronosalt) — mesh + return route:**
```sh
sudo wpa_supplicant_s1g -B -D nl80211 -i wlan1 -c wpa-mesh.conf
sudo ip addr add 10.9.9.4/24 dev wlan1
sudo ip route replace 192.168.12.0/24 via 10.9.9.3 dev wlan1   # so mesh→AP-subnet return traffic routes
```

**4. Verify** (each MUST pass before blaming anything):
```sh
sudo wpa_cli -p /var/run/wpa_supplicant_s1g -i wlan1 status | grep wpa_state   # COMPLETED (both nodes)
pidof hostapd_s1g && ping -c4 10.9.9.4                                         # AP up + mesh forwards 0% loss
```
STA leaf (`rimba-halow-sta`, static IP `192.168.12.2`, gw `192.168.12.1`) joins `rimba-ping` and pings a
mesh host (`10.9.9.4`) → traverses the gate.

### Gotchas that WILL waste your day (all cost real debugging time 2026-07-05)

1. **Chip-wedge from churn** — see step 0. "Mesh stuck `SCANNING`, won't peer" is almost always this, not RF
   or a driver bug. Reboot; bring up once.
2. **Mesh IP is mandatory + not persistent.** With no `10.9.9.x/24` on the mesh vif, `ip route get <peer>`
   resolves the peer out the **management interface (`wlan0`) via the default route** → the ping never hits
   the radio → **empty mpath + 100% loss that looks exactly like a mesh-forwarding/HWMP bug but is NOT**. A
   `wpa_supplicant` restart or reboot **wipes** the manual IP — re-add it. **ALWAYS run `ip route get
   <peer>` and `ip -4 addr show <mesh-vif>` before concluding a forwarding bug.** (See [§Mesh security P6c]
   note; same trap bit two separate debugging sessions.)
3. **hostapd's first start on `ap0` often fails `nl80211 driver initialization failed`** — just `pkill` it
   and start again (one retry, not churn) → `AP-ENABLED`.
4. **Reading an ESP console: capture to a FILE, not a grep-in-a-pipe.** `cat /dev/ttyACMx > /tmp/log`, then
   grep the file. A `grep | head` pipe on the live tty swallows/rethrows output and reads as a *dead board* —
   the board is fine. (This mis-read invented a whole fake "AP secondary vif can't route" limitation.)

### ESP32 port plan — mesh + AP co-channel on one MM6108 (derived from morse_driver; recon 2026-07-08)

**Feasibility (recon).** The MM6108 **firmware command layer is already multi-vif**: `MORSE_CMD_ID_ADD_INTERFACE`
returns a FW-assigned `vif_id`, the fw interface-type enum has `MESH=5`, per-frame vif tagging
(`MORSE_TX/RX_..._FLAGS_VIF_ID`) and per-vif beacon IRQs exist, and the repo already carries probe helpers
(`mmprobe_add_iface_raw` / `mmprobe_iface_type_supported`, `src/driver/driver.c`) to test a 2nd concurrent vif.
The blocker is morselib's **single-vif umac abstraction**: one `struct umac_interface_data.vif_id`; STA/AP/MESH
**swap** on it (`umac_interface_add` does `rm_if`→`add_if`); `umac_interface_type_is_compatible_with_active()`
rejects the {AP, MESH} pair; and `mmwlan_mesh_start` calls `umac_mesh_tear_down_active_interfaces()`. Scope
**(b)**: widen the abstraction — FW very likely supports it, but gate it empirically first.

- ✅ **Stage 0 — feasibility gate: GO (2026-07-08).** Probed the FW via `mmint_mmprobe_iface_type_supported`
  (the mangled-but-exported morselib helper), invoked from `rimba-halow-ap` with the AP vif up: the FW
  **grants a 2nd concurrent vif of type MESH** (`ret=0, fw_status=0`) on the pinned 1.17.8 fw — also AP and STA
  as a 2nd vif. So **the firmware is NOT the blocker; scope (b) is confirmed.** (Caveat: the probe adds +
  auto-removes the vif — this proves FW *allocation* of a concurrent vif, not yet co-channel beacon/traffic,
  which is Stage 3's on-air check on chronium's `morse0`.)
  - **Build note (an earlier "mangle bug" was misdiagnosed — retracted).** The
    `undefined reference to umac_ap_enable_ap` was simply the throwaway probe app **missing
    `CONFIG_HALOW_AP_MODE=y`**: `umac_ap.c` (which defines it) is compiled into libmorse only under that
    config (morselib `CMakeLists.txt` `if(CONFIG_HALOW_AP_MODE)`), and the probe called the AP path without
    it. No build-system bug, no fresh-mangle race. **Implication:** a mesh+AP app just needs
    `CONFIG_HALOW_AP_MODE=y` in its `sdkconfig.defaults` — `umac_mesh.c` is always compiled, so one AP-mode
    build carries **both** the AP and mesh code. **Stage 1 is not blocked.**
- ✅ **Stage 1 — concurrent vifs in umac: IMPLEMENTED + builds (2026-07-08).** Primary+secondary
  vif model landed on `components/halow` branch `feat/mesh-ap-concurrency`; function-level
  new-code↔reference map in [`rimba-mesh-ap-esp32-stage1-codemap.md`](rimba-mesh-ap-esp32-stage1-codemap.md).
  `ap_vif_id`/`ap_mac_addr` slot added; `{AP,MESH}` allowed (mesh-first enforced); a concurrent
  AP allocs a 2nd FW vif (distinct locally-admin MAC) instead of the rm→add swap; getters +
  `umac_interface_remove` + mesh tear-down route AP→secondary. Compiles clean with
  `CONFIG_HALOW_AP_MODE=y` (`rimba-halow-mesh`, board `proto1-fgh100m`). **On-air = Stage 3, not
  yet done.** Design (unchanged) below. `umac_interface_data`
  holds a SINGLE `vif_id`/`mac_addr` today (STA/AP/MESH **swap** on it via `mmdrv_rm_if`→`mmdrv_add_if`);
  `active_interface_types` is *already* a bitmask. **Blast radius: 90 direct `data->vif_id` uses across 9 files**
  (connection 29, offload 21, interface 21, ap 8, twt 4, mesh 2, ibss 2, skbq 2, datapath 1) → a full N-vif array
  is the multi-day cost. **Tractable design — primary + secondary vif, mirroring the Linux recipe (mesh on
  primary `wlan1`, AP on secondary `ap0`):**
  - Keep `data->vif_id` as the **PRIMARY** (mesh in the Mesh-gate) → mesh/STA code untouched (the 29+21
    STA-side uses aren't on a gateway anyway).
  - Add a **SECONDARY** slot to `umac_interface_data` (`ap_vif_id` + `ap_mac_addr`) for the concurrent AP.
  - `umac_interface_type_is_compatible_with_active()` (`umac_interface.c:152`): allow the **AP+MESH** pair —
    relax the MESH-exclusive check at `:181-189` (mirror Linux `iface_combination {AP,MESH} #chan≤1`).
  - `umac_interface_add()` (`:194`): when AP is added alongside MESH, `mmdrv_add_if` a **2nd** vif with a
    distinct locally-administered MAC → store in `ap_vif_id` (NOT the swap at `:280-306`).
  - Getters `umac_interface_get_vif_id(type_mask)` (`:413`) / `_get_vif_type_mask` (`:426`): resolve
    AP→`ap_vif_id`, else→`vif_id` — fixes the 11 getter-callers for free.
  - AP-subsystem routing: `umac_ap.c` (only **8** `data->vif_id` uses) must use `ap_vif_id` when AP is
    secondary — the main per-vif edit.
  - `mmwlan_mesh_start` (`umac_mesh.c`): relax `umac_mesh_tear_down_active_interfaces()` so the AP vif survives.
- ◑ **Stage 2 — per-vif beacon: IMPLEMENTED + builds (2026-07-08); `mmwlan_ap_disable` DEFERRED.**
  `driver_data.beacon` is now per-vif (`enabled_vif_mask` + atomic `pending_vif_mask`); the beacon
  ISR latches which vif fired, the work handler drains all pending vifs, and
  `mmdrv_host_get_beacon(vif_id)` dispatches mesh vs AP by the firing vif's type (was a global
  `is_active` check). Details/anchors in the [code-map](rimba-mesh-ap-esp32-stage1-codemap.md#stage-2--per-vif-beacon).
  `mmwlan_ap_disable` stays a stub — not on the bring-up path, and its supplicant-teardown ordering
  needs on-air validation first (see code-map §Deferred). **On-air = Stage 3, not yet done.**
- ◑ **Stage 3 — concurrent beaconing PROVEN ON AIR (2026-07-08); STA-routing follow-up pending.** New app
  [`firmware/rimba-halow-mesh-ap`](../../firmware/rimba-halow-mesh-ap/main/app_main.c) brings up the
  all-ESP Mesh-gate: `mmhalow_init → mmwlan_mesh_start` (mesh, primary vif) `→ mmwlan_ap_enable`
  (SoftAP, secondary vif), co-channel on chan 27; AP BSSID auto-derived (mesh_mac ^ 0x02).
  - **Bring-up PASS (board1, `proto1-fgh100m`):** boot log shows `MESH vif up (primary)` +
    `AP vif up (secondary) BSSID=e0:72:a1:f8:f9:40 — CONCURRENT`, no crash, heap stable 20 s. Proves
    Stage 1 (`mmwlan_ap_enable` allocates the 2nd vif while mesh is active).
  - **On-air PASS (chronium `morse0` monitor, ch27):** 14 s raw capture saw **136 mesh-MAC beacons**
    (SA `e2:72:a1:f8:f9:40`, Mesh ID `rimba-mesh`, 149 B) **+ 135 AP-MAC beacons** (BSSID
    `e0:72:a1:f8:f9:40`, SSID `rimba-ping`, 183 B), ~9.7/s each (100 TU), 0 frames carrying both —
    i.e. **both vifs beacon concurrently from one MM6108**. Proves Stage 2 per-vif beacon on air.
    (Radio-silenced after: board1→`rimba-hello`, chronium `wlan1`/`morse0` down. Byte-count/payload
    check done; a full byte-for-byte IE diff vs a live Linux gateway is the remaining polish under
    [[verify-onair-chronium-monitor]].)
  - ✅ **End-to-end routing — DONE (2026-07-10,
    [worklog](../worklog/2026-07-10-esp32-mesh-gate-returnleg-fixed-forward-rootcaused.md)).** All-ESP rig:
    board0 gateway (`rimba-halow-mesh-ap`, 2nd AP netif + `ip_forward`) / board1 2nd mesh node
    (`rimba-halow-mesh`) / board2 STA (`rimba-halow-sta-perf`; `rimba-halow-sta` is netif-free, can't
    ping). **STA↔2nd-mesh-node ping = 10/10, 0% loss, ttl=63, repeatable; iperf UDP ~0.3 Mbit/s and TCP
    both traverse the gate in both directions** (throughput radio-limited, matches the perf table below;
    heap-stable, no leak/crash under a 300-frame flood). Two root-caused fixes closed it (the 2026-07-09
    session had narrowed the break to the reply return-leg and exonerated the gateway 4-addr TX build,
    lwIP `ip_forward`-*decision*, ARP, and the entire CCMP crypto/decrypt path):
    - **Return leg** — `rimba-halow-mesh` `mesh_net_task` hard-coded the mesh netif default gw to the
      phantom `10.9.9.1`; an off-subnet echo-reply (to the STA on `192.168.12.x`) routes only via that gw,
      so board1 ARPed a nonexistent host and dropped the reply before `halow_transmit`. Fix: gw =
      `10.9.9.136` (the gate's mesh IP). *(Bench-pinned value — see Stage-3 TODO: de-hardcode via DHCP /
      gate-announcement.)*
    - **Forward leg** — lwIP `ip4_forward` forwards every frame (`ip.fw=10`), but the esp_netif zero-copy
      RX pbuf is a non-contiguous `PBUF_REF`, and `pbuf_add_header` refuses to prepend an L2 header onto a
      non-contiguous pbuf (`pbuf.c:511-518`, headroom-independent) → `ethernet_output` can't add the mesh
      L2 header → the forward is dropped before `halow_transmit`. Fix (`rimba-halow-mesh-ap`
      `gw_rx_deliver`): copy RX into a contiguous `PBUF_LINK`/`PBUF_RAM` and inject via `netif->input`
      (one edit fixes both directions; ownership: we own the mmpkt, free it once, never hand it to
      esp_netif). This **reframes** the earlier "ip_forward works (exonerated)" note — it *decides* to
      forward but the zero-copy pbuf can't be re-transmitted.
    Also confirmed: the `umac_keys.c` AP-downlink fix is **mesh-non-regressive** (board0↔board1 mesh ping
    5/5). NB the ESP mesh is **SAE**, so chronite's open mesh can't be the 2nd node.
  - **TODO (still open):** (a) **de-hardcode the mesh-node return gateway** — `rimba-halow-mesh` pins gw
    `10.9.9.136`. **PLANNED (next big task): a proper 802.11s mesh-gate port** — RANN + `IS_GATE` + learned
    MPP proxy + L2-bridge single subnet (NOT literal GANN/PXU; follow-Linux). Full design + code-map:
    [`rimba-mesh-ap-mesh-gate-discovery-design.md`](rimba-mesh-ap-mesh-gate-discovery-design.md); ~12-19
    session-days (S1-S6). Pragmatic interim = DHCP — **STA zero-config DONE + hardware-verified
    2026-07-10** (task #5): the gateway AP runs a DHCP server (IP+gw in the inherent netif config), the
    STA is a DHCP client (`DHCP lease 192.168.12.2 gw 192.168.12.1`), end-to-end ping 10/10 ttl=63 on a
    cold boot; keeps L3. Worklog: [`2026-07-10-dhcp-de-hardcode-wip.md`](../worklog/2026-07-10-dhcp-de-hardcode-wip.md).
    The mesh **node's** gw stays a `#define MESH_GATE_IP` (deployment param) — its dynamic de-hardcode is
    deferred to the 802.11s port above (DHCP-over-mesh would mean editing shared `mmhalow`).
    (b) **broadcast/multicast across the gate — SUBSUMED by the L2-bridge (#1); no standalone fix.**
    lwIP `ip4_forward`→`ip4_canforward` (ip4.c:251-257) hard-drops LL broadcast (`PBUF_FLAG_LLBCAST`)
    and multicast (`PBUF_FLAG_LLMCAST`/`IP_MULTICAST`), so mDNS (224.0.0.251)/SSDP/NetBIOS won't
    traverse (unicast does). An L3 `LWIP_HOOK_IP4_CANFORWARD` hook would be needed, but `ip4_forward`
    routes to a single next-hop netif and can't replicate → fragile *and* throwaway once #1 lands. The
    proper fix is the L2 bridge (esp_netif `bridgeif` is in-tree — `CONFIG_LWIP_BRIDGEIF_MAX_PORTS=7`,
    `CONFIG_ESP_NETIF_BRIDGE_EN` off), which floods bcast/mcast natively; but bridging the 802.11s mesh
    to the AP BSS at L2 still needs the AP-client-MAC-into-mesh proxy (6-addr AE = the learned-MPP
    stage), i.e. it *is* the #1 port. Verdict (2026-07-10): don't build a standalone L3 hack; fold into #1.
    (c) **coverage of the L3 gate (task #3) — test matrix + status** (worklog
    [`2026-07-10-mesh-gate-coverage-soak-multista.md`](../worklog/2026-07-10-mesh-gate-coverage-soak-multista.md)):
      1. *Soak* — **DONE ✅ 2026-07-10:** 450-ping (~7.5 min) end-to-end soak, **449/450 (0% loss, 1 warmup
         miss), all ttl=63, no crash/reboot, mesh_peers & ap_stas stable, gateway heap drift +32 B (no leak)**.
      2. *Power-save behind the gate* — **PASS (TWT doze) 2026-07-10.** The gate's AP buffers + delivers a
         mesh-originated downlink to a **dozing** STA behind it: chronite(mesh) → board2(ESP STA in TWT doze,
         1 s wake, stays associated) = **20/20, 0% loss**, RTT elevated ~40→260 ms (avg) = the PS-buffering
         signature. Deeper **WNM+powerdown** keeps the STA associated but too deep for timely unicast
         (0/20 within 3 s) — expected deep-sleep tradeoff, not a gate defect. (An earlier "mesh→AP-client
         forwarding gap" was a MISDIAGNOSIS off a flaky/dozing *Linux* STA — the gate forwards correctly to
         a fresh ESP client, 12/12 with the full `RX_MESH→AP_TX→RX_AP` chain logged; no gate code change.)
      2b. *Foundation (enables #2/#4)* — **DONE ✅ 2026-07-10:** a Linux node joins the gate's SECURED
         `rimba-mesh` (SAE password `rimbamesh2026` = morselib `umac_mesh.c:81`, cross-vendor). chronite
         (Pi5) peered with gate+board1 in 1 s, mesh-ping 4/4. `scratchpad/wpa-rimba-mesh.conf`.
      3. *Multi-STA* — **DONE ✅ 2026-07-10:** two concurrent STAs behind the gate — **chronite (Linux
         S1G STA, SAE H2E via `sae_pwe=2`) leased .2 + board2 (ESP STA) leased .3** (distinct pool leases,
         `ap_stas=2`), both pinged 10.9.9.100 concurrently = **30/30, 0% loss, ttl=63**. Proves the
         (previously unproven) Linux-S1G-infra-STA→morselib-SoftAP SAE path + `dhcpcd` interop with the
         #5 DHCP server. (The AP requires H2E; a hunt-and-peck commit is rejected `status_code=1`.)
      4. *Multi-hop (STA→gate→relay→node)* — **DONE ✅ 2026-07-10 (after a morselib fix):** forced a 2-hop
         line gate→board1(relay)→chronite (allowlist = board1). board2 (STA behind gate) → chronite (2 hops
         via board1) = **11/12, ttl=63**; **A/B-proven** (relay board1 down → 0/8). Required fixing the
         forced-topology RX drop in morselib `umac_datapath.c`: it was guarded by `umac_mesh_is_active()`
         (always true on a mesh+AP node) so it dropped ALL non-allowlisted RX data incl. the gate AP's
         client EAPOL → STA couldn't associate. Fix = gate on `mesh_ctrl_present` (802.11s Mesh Control bit)
         instead, so the allowlist filters ONLY mesh frames, never AP-client frames. (Submodule, branch
         `feat/mesh-ap-concurrency`.) A Linux far mesh-node needs a return route `192.168.12.0/24 via
         10.9.9.136` (same as board1's gw, task #17).
    (d) `mmwlan_ap_disable` (Stage 2 deferral) + the 2% app-partition headroom; (e) byte-for-byte on-air
    IE diff vs a live Linux gateway ([[verify-onair-chronium-monitor]]).
    (f) **Whole-network stress test — DONE ✅ 2026-07-10** (worklog
    [`2026-07-10-mesh-gate-stress-permutation-matrix.md`](../worklog/2026-07-10-mesh-gate-stress-permutation-matrix.md)):
    every source→destination permutation (5-node mesh + board2 AP-STA, both subnets through the gate)
    **connects**; ≈**0% loss** under moderate concurrent load (1/600); under **flood** it degrades gracefully
    to **80.8% delivery, no crashes** — the **gate is the throughput bottleneck** (single radio doing
    mesh RX + AP TX + forward: cross-gate paths lose 30–62%, mesh-to-mesh only 3–13%). NB chronosalt (Pi Zero)
    is power-marginal (reboots on radio bring-up).
- ◑ **Stage 4 — DATA-plane routing: designed + RX-demux landed; TX refactor pending (2026-07-08).**
  Full staged design (gaps A–E, anchored edit sites, risks, verify matrix) in
  [`rimba-mesh-ap-esp32-stage4-datapath-design.md`](rimba-mesh-ap-esp32-stage4-datapath-design.md). The
  cheap **RX demux is IMPLEMENTED** (`umac_datapath.c:887`: mesh-vs-AP by `mesh_ctrl_present` when both
  active → VIF_STA/VIF_AP; builds; backward-compatible), and **Gaps A+B are IMPLEMENTED**: Gap A —
  AP-client + group stads carry `ap_vif_id` via `umac_sta_data_set_vif_id` (`umac_ap.c`, mirroring mesh +
  Linux `sta->sdata->vif`); Gap B — `process_tx_frame` now frames per the stad's egress vif TYPE
  (`…_mesh` vs `…_ap`) + stamps `tx_metadata->vif_id`, with every per-frame mesh branch (Mesh Control
  header/QoS bit, next-hop keying, SW-CCMP) re-gated on the frame's vif — guarded to be byte-identical
  off the gateway path. Both build clean. **UPDATE 2026-07-10: Gap C (gateway lookup/dequeue over BOTH
  stad sets) + Gap E (app 2-netif + `ip_forward`) LANDED and traffic routes END-TO-END** — STA↔mesh-node
  ping 10/10 + iperf both ways (see Stage 3). The gateway *happy path* (AP downlink + mesh uplink) frames
  correctly per-vif, so Gap B is effective for it. **The deeper global-`ops` last-writer-wins caution
  below still holds for UNTESTED mixed-flow cases** (simultaneous AP+mesh flows, edge framing, multiple
  STAs/hops) — full per-stad/per-vif ops dispatch remains the deferred hard 20% if those surface a
  failure. Findings (grep-verified; Linux reference on chronite `~/halow`):
  - **RX is not per-vif:** `umac_datapath.c:887-903` picks the host RX vif from *which interface type is
    active globally*, not the frame's vif — comment "MESH reuse the AP per-vif slot", so mesh + AP data
    both surface as `MMWLAN_VIF_AP`. Fixable with a `mesh_ctrl_present`-based demux (~15 lines) so two
    esp_netifs can be fed. **Moderate.**
  - **TX framing is single-mode (the real blocker):** `umac_datapath_data.ops` is ONE global pointer
    (`umac_datapath_data.h:82`) set by `configure_{sta,ap,mesh,ibss}_mode` — **last writer wins**, so in the
    gateway `mesh_start`→mesh ops then `ap_start`→AP ops leaves `ops = datapath_ops_ap`. And
    `umac_datapath_process_tx_frame` (`:2084`) prepends the mesh header + frames per **global**
    `umac_mesh_is_active()`, applied to *every* stad. So an AP-client downlink would get mesh 4-addr framing
    and a mesh-peer uplink would get AP framing — both wrong. The ops (`lookup_stad`, `enqueue`/`dequeue`,
    `construct_80211_data_header`) differ mesh-vs-AP (`:3147/3287/3410` dequeue variants), so making them
    coexist means dispatching ops **per-stad/per-vif** through the whole TX (and RX) datapath. **Multi-day,
    high-risk core refactor** — the deferred hard 20%, akin to [[mesh-no-ampdu-aggregation]] /
    [[mesh-real-rc-feasible-design]]. TX vif routing itself is otherwise destination/stad-driven
    (`umac_datapath_tx_frame` looks up stad by DA/RA), and `metadata->vif` is only an active-check — so
    per-stad ops dispatch is the crux, not vif tagging at the API.

**Governing rules:** derive each change from the Linux side ([[proper-fix-follow-linux]]); ship a
new-code↔Linux code-map ([[porting-ships-verified-codemap]]); on-air byte-diff every frame vs a live Linux
device ([[verify-onair-chronium-monitor]]). Recon detail: the interface-model findings above.

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
**SW-CCMP crypto is NOT the dominant *single-flow* relay cost** — per-hop forwarding + airtime dominate;
security is cheap in single-flow throughput terms. A concurrent-load run showed the multi-hop relay saturates
(board1 88% loss when its relay ESP also carries its own traffic) — a host SW-CCMP relay **CPU** bottleneck
(distinct from #20, which is the HW-crypto A4≠TA *withhold*). **(2026-07-11:** that per-frame SW-CCMP CPU cost
was ~14-28× reduced by a **bulk-DMA AES-CCM** rewrite — see the ✅ backlog item + worklog
[`2026-07-11-esp32-mesh-swccmp-bulk-aes.md`](../worklog/2026-07-11-esp32-mesh-swccmp-bulk-aes.md). It cuts the
concurrent-load relay CPU/latency but does **not** raise single-flow throughput — confirming the ceiling is
**airtime / no A-MPDU**, now the top mesh backlog item.)**

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
- ☐ **#20 — HW-crypto multi-hop forward (A4≠TA)** — backlog, low priority; **re-verified + re-scoped
  2026-07-02** (worklog `2026-07-02-mesh-20-hwcrypto-reverify.md`). In HW-crypto mode the MM6108 FW
  withholds a foreign-A4 (A4≠TA) 4-addr mesh forward from the host; the secured mesh ships on **host
  SW-CCMP (P5)** which sidesteps it, so it's **not a blocker**. The 2026-07-02 re-run (reliable RX-entry
  probe — §#26's was broken — plus a deterministic synth-forward, on-air) confirms the withhold is real
  AND corrects §#25/§#26: it is the **host stack (morselib)**, *not* the FW version (1.17.8 on the ESP
  still withholds) and *not* the BCF (ESP runs `bcf_fgh100mhaamd`, the same BCF chronosalt delivers on) —
  **but it is NOT a driver→FW command difference** (morse_driver-vs-morselib command streams diff
  byte-equivalent). The remaining cause is a host-side interaction *outside* the `morse_cmd_tx` command
  interface. Revisit only if a HW-crypto multi-hop path is ever wanted. **(2026-07-11: re-scoped again —
  bench-proven that Linux `morse_driver` withholds the A4≠TA forward too, so #20 is a UNIVERSAL MM6108
  firmware limitation, NOT a morselib bug; morselib correctly follows Linux, and SW-CCMP is the permanent
  universal answer. Worklog `2026-07-11-mesh-20-linux-also-withholds-fw-limitation.md`. Close unless Morse
  ships an FW fix.)**
- ☐ **Mesh A-MPDU aggregation — the dominant remaining relay-throughput limiter (NEXT).** Mesh sends single
  MPDUs; per-frame preamble/IFS/backoff/ACK caps single-flow + relay goodput (~0.04 Mbit/s 2-hop) and, now
  that SW-CCMP crypto is cheap (below), **this is the airtime ceiling.** **Re-scoped 2026-07-11** (verified
  feasibility + staged plan + Linux code-map: **`docs/mesh-ap/rimba-mesh-ampdu-aggregation-design.md`**;
  memory `mesh-no-ampdu-aggregation`): **feasible ~6–10 d, and the old blocker-map was stale.** The
  `MMWLAN_STA_CONNECTED` check is **not** the blocker — mesh's `get_sta_state` returns CONNECTED
  unconditionally (`umac_datapath.c:3386-3390`) so single-hop already fires ADDBA on-air; and A-MPDU is
  **FW-assembled**, not host-assembled (host only sets `MMDRV_TX_FLAG_AMPDU_ENABLED`, `:2274`, consumed at
  `skbq.c:832/:845`). The **real unlock is routing inbound `BLOCK_ACK` action frames** to the BA handler on a
  mesh vif (`umac_mesh_handle_action` drops them; `:273-274` never reached) **+** a PMF/robust-mgmt exemption
  on a secured mesh (`:372-374`) — after which single-hop A-MPDU lights up. Staged **S0** FW go/no-go spike →
  **S1** BLOCK_ACK RX routing → **S2** multi-hop next-hop stad (avoid the self-addressed `common_stad`) →
  **S3** relay data-path retag (the relay win; combine with the **in-place mesh forward** — rewrite headers
  on the RX mmpkt vs `build_mgmt_frame` alloc+copy in `umac_mesh_forward_data`) → **S4** teardown → **S5**
  polish. **Status 2026-07-11 (bench; PRs `teapotlaboratories/mm-esp32-halow#23` + `teapotlaboratories/rimba#33`,
  draft): S0 = GO** (FW advertises mesh AMPDU cap + on-air A-MPDU proven for 4-addr mesh data, worklog
  `2026-07-11-mesh-ampdu-s0-fw-capability-spike.md`); **S1 code done + SECURED-mesh on-air verified** (real
  A-MPDU from a completed BA session, no force; the mesh is SAE+PMF+SW-CCMP via `MMWLAN_MESH_SEC_PHASE1=1`, and
  the aggregated frames are 100% `protected=1` → **SW-CCMP composes with A-MPDU, validated**). The ADDBA is sent
  CCMP-protected on the all-ESP mesh, so the routing edit is the critical path and the unprotected-BA exemption
  was not exercised. Follow-ups: ADDBA byte-diff vs Linux; then S2 (multi-hop) + S3 (relay).
- ✅ **Mesh SW-CCMP bulk-DMA AES-CCM — DONE 2026-07-11** (worklog `2026-07-11-esp32-mesh-swccmp-bulk-aes.md`).
  Root-caused the host SW-CCMP relay crypto cost: the CCM ran AES **one 16-byte ECB block at a time**
  (~187 single-block HW-AES ops per 1442 B frame, each paying the full `esp_aes`
  acquire/DMA-setup/lock/release wrapper ⇒ **~384 `AES_LOCK` acquire/release per RELAYED frame**). Replaced
  with a **bulk-DMA CCM** — 1 `mbedtls_aes_crypt_cbc` (CBC-MAC) + 1 `mbedtls_aes_crypt_ctr` + 1 ECB (~3 HW ops,
  ~6 lock acquires), self-contained in `ccmp.c`, **byte-exact vs RFC-3610 + `mbedtls_ccm`** (so MICs still
  interop with Linux). Per-frame crypto: **enc avg 7038→197 µs (~36×), dec 4401→285 µs (~15×)**; min→avg gap
  21×→1.6× (contention gone). + CPU 160→240 MHz (perf/relay sdkconfig only) + in-place crypto (−3.2 KB RAM).
  **Scope: does NOT raise single-flow throughput (airtime-bound — the open-vs-secured A/B in §Performance
  already showed CCMP isn't the single-flow limiter); it cuts relay CPU/latency/contention (the concurrent-load
  "SW-CCMP relay bottleneck") and is the prerequisite for A-MPDU to pay off.** Handshake/TLS keep the per-block
  path. Uncommitted on bench.
- ✅ **P6c — mesh airtime link metric (rate-derived HWMP cost)** — DONE + hardware-verified 2026-07-02. Replaces the
  fixed per-hop `MESH_PATH_LINK_METRIC` (100) with a port of net/mac80211 `airtime_link_metric_get`
  (`mesh_hwmp.c:338-381`, formula + constants `TEST_FRAME_LEN=8192`/`ARITH_SHIFT=8`/`MAX_METRIC` clamp
  re-verified on chronite): the exact fixed-point ETT, fed a per-link rate. **Deliberate approximation:**
  mesh has no per-peer rate control (mmrc never starts for mesh stads), so the rate is seeded from the
  peer's per-frame RSSI via mmrc's own cold-start tiers (≥−70→MCS7, ≥−85→MCS3, else MCS0 → 3000/1200/300
  Kbps at 1 MHz/LGI/SS1 through `mmrc_calculate_theoretical_throughput`) — the *same scale* as Linux
  nodes' airtime (interop-preserving). Per-hop costs 2731/6827/27307 (strictly positive → monotonic).
  Integration = the two accumulation sites (`umac_mesh.c:2164` PREQ, `:2219` PREP) + overflow clamp + a
  new per-peer `last_rssi_dbm` recorded from the PREQ/PREP RX. **Verified:** formula unit-test
  {27307,6827,2731} + build + a 3-board line (board1→board0→board2) where the resolved metric read **5462**
  (real strong RSSI, not the old 200) and **30038** with a forced-weak board0↔board2 hop — proving the
  airtime metric + real-RSSI sampling on hardware, no RF arrangement needed. Worklog:
  `docs/worklog/2026-07-02-mesh-p6c-airtime-metric.md`.
- ✅ **HWMP multi-path reply-dedup / per-reply SN — FIXED + hardware-verified 2026-07-02** (found by the
  P6c routing-decision test). Bug: in a multi-path topology (a dest reachable both directly *and* via a
  relay), morselib re-replied to every PREQ copy stamping `target_sn = ++mesh_hwmp_sn` (per-reply
  increment), so the originator selected by newer-SN not metric → the installed path **flapped**. Fix
  (following net/mac80211 `hwmp_preq_frame_process` / `hwmp_route_info_get`): `mesh_path_update` now
  returns `fresh` and the PREQ handler gates its PREP reply + re-flood on it (mac80211's only duplicate
  suppressor); the target SN is our OWN `mesh_hwmp_sn`, bumped at most once per net-traversal window
  (`MESH_HWMP_NET_TRAVERSAL_MS=500`) so every copy of one discovery shares a stable `target_sn`; plus the
  10/9 hysteresis on same-SN different-next-hop. **Verified:** re-ran the triangle — board1→board2 now
  SETTLES on the relay (via board0, 5462) each discovery and holds ~20 s between refreshes with no
  flapping (was ~every 7 s, direct-dominant). Worklog `docs/worklog/2026-07-02-mesh-hwmp-multipath-dedup-sn.md`.
- ✅ **P6c + HWMP fix — validated at scale + on-air 2026-07-02.** Full **6-node secured ESP+Linux mesh**
  (3 ESP + chronite/chronosalt/chronogen): all 5-peer, 0% single-flow loss, stable under concurrent load.
  **Forced-topology diamond** (board0 → {board1, board2} → chronite, board2 = weak relay): airtime routes
  the multi-hop path **via board1**, confirmed both in the path table and **on-air** on chronium's `morse0`
  (board0 DATA frames RA=board1 ×25 / board2 ×0). Worklog `docs/worklog/2026-07-02-mesh-multinode-onair-validation.md`.
- ✅ **Mesh dynamic path table 2026-07-02.** Replaced the fixed 8-entry linear path table with a 64-entry
  pool + a dest-MAC hash index (O(1) lookup, `int16_t`-chained) — lifts the reachable-destination ceiling
  past 8 (was: 9th dest evicted a live path). Peer table (neighbour-bounded, ~630 B/entry SAE) + RMC left.
  Host-tested (insert/chain/evict past 8, no cycles), built, bench-verified (peer + 0% datapath). Worklog
  `docs/worklog/2026-07-02-mesh-dynamic-path-table.md`.
- ✅ **Mesh peer-table growth 2026-07-02.** `UMAC_MESH_MAX_PEERS` 4 → 8 — headroom for >4 direct
  neighbours. Option A (raise the count); the SAE bodies stay per-peer (read in the ESTAB reauth/resync
  steady state, `umac_mesh.c:1073-1077/1179-1181`, can't be pooled). nm: `mesh_peers` 2544 → 5088 B
  (+2.6 KB). Build + nm verified; **>4-peer bench check done** in the real-RC run (board0 held 5 peers). Worklog
  `docs/worklog/2026-07-02-mesh-peer-table-growth.md`.
- ✅ **Mesh table caps pushed 2026-07-02.** After the SRAM analysis (mesh tables were ~2% of DRAM), raised
  the ceilings: **paths 64 → 256**, **peers 8 → 16** (+ `MESH_MAX_PLINKS`/`MESH_ALLOWLIST_MAX` → 16 to
  match the advertised capability); **hops stay 31** (Linux `dot11MeshTTL` interop). nm: mesh tables now
  ~21 KB (peers 10176 + paths 10240 + buckets 512 + allowlist 96) ≈ 6% of the ESP32-S3 DRAM. Host-tested
  at 256 (chaining/evict/no-cycles), build + nm verified. Peers is RAM-comfortable at 16 but RF/airtime-
  bound in practice; 256 routes covers a large mesh.
- ✅ **Real per-peer RC feeding the airtime metric 2026-07-02.** mmrc now runs per mesh peer and feeds the
  metric — mmrc's **learned** best-throughput rate + **real success probability** replace P6c's RSSI-seed
  (removes divergences #1/#2). Edit 1 start/stop mmrc at ESTAB/teardown; Edit 2 per-AID mesh stad lookup so
  tx-status feedback lands (+ relay rate-table so relays learn); Edit 3 a learned-metric accessor +
  `prob`-parametrized ETT (`prob=100` ⇒ identical to today). Feasibility: NOT blocked like A-MPDU
  (tx-status reaches mesh ungated). Hardware-verified: board0's RC for chronite converged to MCS7 with
  `prob=93` → metric 2934 (tracks the real ~7% loss vs the old fixed 2731). **On-air confirmed** (chronium
  `morse0`): the HWMP PREQ metric field carries RC-learned **non-tier** values (`3035`/`8441`) the RSSI-seed
  can't emit. Worklog
  `docs/worklog/2026-07-02-mesh-real-rc-airtime-metric.md`.
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
