# Mesh-gate (Mesh + AP) on the MM6108: milestones & porting

The **Mesh-gate** is Rimba's second candidate L2: relays run **802.11s mesh** to
each other and a **SoftAP** that leaf nodes associate to and **TWT**-sleep under.
It is the alternative to the **IBSS** L2 (see
[`rimba-ibss-milestones.md`](../ibss/rimba-ibss-milestones.md)). This is the single doc
for the Mesh-gate on **ESP32-S3 + MM6108** (`mm-iot-sdk`/morselib): the milestone
view, the **new-code ↔ Linux** porting maps, and the **TODO**.

**Governing requirement (memory `proper-fix-follow-linux`, same as IBSS):** the
implementation is **derived from the Linux side** — MorseMicro's `morse_driver`
(out-of-tree mac80211 driver) + the mac80211 fork — not improvised from morselib
internals. Every change is verified on hardware (or the reason it can't be is
documented), and ported code carries a new-code ↔ Linux mapping (below).

**Hardware:** up to 3× Seeed XIAO ESP32-S3 + FGH100M (`boards/proto1-fgh100m`,
`bcf_fgh100mhaamd`, fw **1.17.6**) **+ a Raspberry Pi 5 + Wio-WM6180 (MM6108)
Linux reference node** (`morse_driver`/cli/fw **1.17.8** — the interop oracle,
[`design-specification/rimba-linux-node-setup.md`](../design-specification/rimba-linux-node-setup.md)). US 915.5 MHz, 1 MHz BW,
S1G ch27 / op-class 68; SSID `rimba-ping`, WPA3-SAE; HaLow subnet 192.168.12.0/24.

Paths: `MORSE = components/halow/components/mm-iot-sdk/framework/morselib/src`;
Linux driver = `morse_driver/` (tag 1.17.8, on chronium at `~/halow/morse_driver`).

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
no IBSS radio power-save ([`design-specification/rimba-mm6108-powersave-analysis.md`](../design-specification/rimba-mm6108-powersave-analysis.md)).
The Mesh-gate dissolves that (TWT + AP buffering), at the cost of always-on relays
and more moving parts. Neither is chosen yet; this milestone set exists to make
the Mesh-gate comparable on the same hardware.

---

## Capability status — ESP32/morselib vs. Linux

| Capability | Linux (`morse_driver`/mac80211) | ESP32 (morselib) | Notes |
|---|---|---|---|
| **AP mode** (S1G, SAE) | ✅ | ✅ | morselib 2.10.4+ (hostapd-backed) |
| **STA mode** + TWT requester | ✅ | ✅ | `mmwlan_twt_add_configuration` |
| **802.11s mesh point** | ✅ (`mesh.c`, `CONFIG_MAC80211_MESH`) | ❌ **not ported** | morselib has only the bare `MORSE_CMD_INTERFACE_TYPE_MESH=5` enum — no 802.11s code (peering/HWMP/mpath) |
| **Mesh + AP concurrent** | ✅ (`iface_combination` AP\|MESH, `#chan<=1`) | ❌ (blocked on mesh) | one radio, co-channel; the Mesh-gate pattern |
| **AP TWT responder** (leaf power-save) | ✅ (host-side, `mac80211` + `twt.c`) | ✅ **ported** (below) | stock fw 1.17.6, no firmware change |
| **AP STA-count ceiling** | up to `IEEE80211_MAX_AID = 2007` | ✅ **255** (`uint8_t max_stas`) | four-block S1G TIM (below) |

**Bottom line:** AP, the AP-TWT-responder, and a 255-STA ceiling are usable on the
ESP32 today. **Mesh (802.11s) is the missing piece** for a full ESP32 Mesh-gate —
a substantial morselib feature (no 802.11s exists there); the open structural item.

---

## Milestones

### A1 — SoftAP bring-up (SAE) ✅
morselib SoftAP: SSID `rimba-ping`, WPA3-SAE, S1G ch27 / op-class 68, host-built
beacon via the bundled hostapd. Boots and beacons stably.

### A2 — AP ↔ STA association + bidirectional IP ✅
`rimba-halow-ap` + `rimba-halow-sta`: a STA associates (SAE 4-way) and exchanges IP
(DHCP, ping). Foundation for the Mesh-gate (and the RISK-01 IBSS fallback).
([`worklog/2026-06-18-halow-ap-sta-ping.md`](../worklog/2026-06-18-halow-ap-sta-ping.md))

### A3 — Mesh + AP concurrency — proven on Linux, pending on ESP32 ◑
On chronium one MM6108 ran AP (`hostapd_s1g`) + open 802.11s mesh-point
(`iw … mesh join`) **co-channel** (ch27) + a TWT'ing ESP32 STA, all at once. Recipe
+ gotchas (`type mp` needs explicit `iw set type`; distinct locally-administered
MAC; bare `freq` not `HT20`) in [`design-specification/rimba-linux-node-setup.md`](../design-specification/rimba-linux-node-setup.md)
§12. On ESP32 this is blocked only by the **absence of 802.11s in morselib** (the AP
half works; the mesh half doesn't exist) — the open structural item for an all-ESP32
Mesh-gate. ([`worklog/2026-06-22-mesh-ap-twt.md`](../worklog/2026-06-22-mesh-ap-twt.md))

### T1 — AP-side TWT responder port ✅
Ported the TWT responder into morselib around hostapd. Detailed map below ("AP
TWT-responder port"). **Firmware finding:** `TWT_AGREEMENT_INSTALL` (cmd `0x26`) is
gated to STA vifs in *every* MM6108 `.mbin` (1.17.6 = 1.17.8, byte-identical to the
Linux `.bin`) — so the SP is served **host-side** on Linux too.

### T2 — Leaf actually TWT-sleeps (the load-bearing fix) ✅
hostapd's transient `sta_remove` during (re)assoc freed the just-accepted, still-
`PENDING` TWT slot before the assoc-resp IE was built, so the STA never established
TWT — plus a missing **flush-on-wake** in the AP datapath. Fixed → STA deep-sleeps:
AP→STA RTT rises toward the ~1 s TWT interval (was flat ~10 ms), matching the Linux AP.

### T3 — Multi-STA TWT responder ✅
Per-STA agreement table allocated by SA on assoc, freed on leave. HW-validated: one
AP held two TWT-requester ESP32 STAs concurrently.

### S1 — STA-count scaling 63 → 127 → 255 ✅
Raised morselib's S1G TIM from one block (AID ≤ 63) to two (≤ 127) then four
(`MAX_SUPPORTED_AID = 256`, AIDs 1..255 — the `uint8_t max_stas` ceiling). Detailed
map below ("AP STA-count scaling"). The real limit was the multi-block TIM, not
firmware; morselib's TIM is a port of Linux `dot11ah/tim.c`.

### V1 — Multi-node validation ✅
1 ESP32 AP + 2 ESP32 STA + 1 chronium **Linux STA** associate concurrently (3 STAs,
all SAE); TWT power-save active; no regression across the 127 and 255 builds. New
**chronium-as-infra-STA** recipe (`wpa_supplicant_s1g`, SAE, S1G ch27 ≡ nl80211 freq
5560). ([`worklog/2026-06-23-ap-multinode-twt-hwtest.md`](../worklog/2026-06-23-ap-multinode-twt-hwtest.md))

---

## AP TWT-responder port — detail (T1–T3)

### Architecture
Linux does the whole TWT responder **in the driver, around hostapd**: parse the
STA's TWT IE from the (re)assoc-request, splice the ACCEPT IE into the
(re)assoc-response on TX, serve the dozing STA's downlink during its service period
(SP). morselib has no AP-side MLME of its own (the bundled hostapd builds assoc
frames), so the port mirrors Linux's driver-layer approach: hook morselib's RX-mgmt
and TX-mgmt paths around hostapd, plus the AP power-save datapath. **Status:** works
end-to-end on ESP32 AP ↔ ESP32 STA, **stock fw 1.17.6, no firmware change**.

### Code map — morselib (new) ↔ Linux `morse_driver`
*Re-verified 2026-06-23 against `morse_driver` 1.17.8 (`~/halow/morse_driver`,
`version 0-rel_1_17_8_2026_Mar_24`) — every row's symbol/line is current.*

| # | Role | morselib (NEW code) | Linux `morse_driver` equivalent |
|---|---|---|---|
| 1 | **Advertise TWT-responder cap** in AP S1G caps | `ies/s1g_capabilities.c:318` `ie_s1g_capabilities_build_ap` → `DOT11_S1G_CAP_INFO_8_SET_TWT_RESPONDER_SUPPORT` | `mac.c:1275` `s1g_capab->capab_info[8] \|= S1G_CAP8_TWT_RESPOND` |
| 2 | **Enable responder role** on the AP vif | `twt/umac_twt.c:56` `umac_twt_init_vif(…, is_responder)`; called `interface/umac_interface.c:141` | `twt.c:2031` `morse_twt_init_vif()` (is_ap → `responder=true`); called `mac.c:3308` |
| 3 | **RX hook** — parse STA's TWT IE from (re)assoc-req | `supplicant_shim/supplicant_core.c:396` → `umac_twt_responder_handle_assoc_req` (`twt/umac_twt.c:389`, `ie_twt_find`) | `mac.c:6405` → `morse_mac_process_rx_twt_mgmt` (`twt.c:1727`) → `morse_mac_process_twt_ie` (`twt.c:1612`) |
| 4 | **Accept/reject policy** (REQUEST/SUGGEST→accept; DEMAND/GROUPING→reject) | `twt/umac_twt.c:389` (in `…_handle_assoc_req`) | `twt.c:1144-1178` `morse_twt_enter_state_consider_{request,suggest,demand,grouping}` |
| 5 | **Build ACCEPT IE** | `twt/umac_twt.c:445` `umac_twt_responder_build_response_ie` (`DOT11_TWT_SETUP_CMD_ACCEPT`) | `twt.c:1043` `morse_twt_send_accept` + `twt.c:101` `morse_twt_set_command` |
| 6 | **TX hook** — splice ACCEPT IE into (re)assoc-resp | `supplicant_shim/driver_ap.c:451` `mmwpas_send_mlme` | `mac.c:1861` (peek tx queue, `morse_twt_insert_ie` `twt.c:572`) |
| 7 | **Install agreement to fw** (cmd `0x26`) — *gated on AP vif; harmless* | `twt/umac_twt.c:491` `umac_twt_responder_install` → `umac_twt_install_agreement` (`:286`); hook `driver_ap.c:277` | `mac.c:5024` → `morse_twt_process_pending_cmds` (`twt.c:1355`) → `morse_cmd_twt_agreement_install_req` (`command.c:2211`) |
| 8 | **Agreement blob format** (15 B) | `twt/umac_twt.c:286` `umac_twt_install_agreement` | `twt.c:2326` `morse_twt_initialise_agreement` |
| 9 | **AP-side serving — deliver buffered downlink at the SP** ⭐ the load-bearing fix | `ap/umac_ap.c:890` `umac_ap_set_stad_sleep_state`: asleep→awake w/ queued frames → `umac_core_evt_wake()` (PM-bit at `umac_datapath.c:1528`) | `mac80211` PS buffering (`ieee80211_sta_ps_deliver_wakeup`) + `twt.c` wake-interval tree (`morse_twt_agreement_wake_interval_add` `twt.c:941`) |
| 10 | **Data structures** (per-STA table) | `twt/umac_twt_data.h`: `agreements[MMWLAN_AP_MAX_STAS_LIMIT]` + parallel `responder_peers[][6]`; alloc/lookup by SA; freed on leave (`mmwpas_sta_remove`) | `morse_twt` per-STA agreement (on the sta) + per-vif `twt_wake_interval_tree` |

### The decisive fix (row 9)
morselib's AP already buffers a dozing STA's downlink (`umac_ap_queue_pkt` → traffic
bitmap; `umac_ap_is_stad_paused` gates TX) and tracks sleep state from each frame's
PM bit (`umac_datapath.c:1528`). The bug: when the STA woke (PM=0 at its SP),
`umac_ap_set_stad_sleep_state` cleared the TIM bit but **never kicked the TX loop**,
so buffered downlink waited for the next beacon/DTIM rather than the STA's short SP →
timeout. Fix = call `umac_core_evt_wake()` on the asleep→awake transition when frames
are queued. This is the morselib stand-in for mac80211's `ieee80211_sta_ps_deliver_*`,
driven by the TWT SP. (The firmware `0x26` install — row 7 — is gated for AP vifs and
not required; host-side delivery suffices.)

### Caveats
- **Multi-STA, bounded table** — `UMAC_TWT_NUM_AGREEMENTS == MMWLAN_AP_MAX_STAS_LIMIT`;
  when full, the responder drops the IE (implicit reject; STA still associates without
  TWT). Linux keeps a dynamic per-STA list. HW-validated 1 AP + 2 STA.
- **No SP-overlap scheduling** — Linux's `twt_wi_tree` (`twt.c:941`) spaces SPs; this
  port serves each STA reactively on wake.
- **Downlink latency = TWT interval** — inherent TWT trade-off (buffered → next SP).
- **Transport: assoc-IE + TWT action frames** — also handles S1G unprotected action
  frames (cat 22): TWT-Setup (action 6) answered with a Setup-response carrying the
  ACCEPT IE; TWT-Teardown (action 7) frees the agreement (`umac_twt_responder_handle_action`).
  *Action-frame path review-verified only* — the test STA negotiates in assoc IEs.
- **BCF cleared as a false lead** — runtime "BCF board description: mf16858" is *correct*:
  `bcf_fgh100mhaamd.mbin` is byte-identical to the genuine Morse `bcf_fgh100mhaamd.bin`
  (vendored at `vendor/morse-firmware`), which carries `.board_desc = "mf16858"` (FGH100M-H
  shares that board ref). `BOARD=proto1-fgh100m` loads the right calibration.

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
self-imposed embedded-footprint limit, **not** a behaviour difference. Raising it
brings morselib toward what Linux always did (driver sizes its AID bitmap to the full
`AID_LIMIT`; mac80211 caps associations at `IEEE80211_MAX_AID = 2007`).

### Code map — morselib (changed) ↔ Linux `morse_driver` / `dot11ah`

| Concern | morselib (this work) | Linux equivalent |
|---|---|---|
| **AID space / per-AP traffic bitmap** | `ap/traffic_bitmap.h:19` `MAX_SUPPORTED_AID` 64→128→256; `:20` `S1G_BITMAP_SUBBLOCKS`; `ap/umac_ap_data.h:25` `bitmap[…]` | `morse.h:398` `MORSE_AP_AID_BITMAP_SIZE = AID_LIMIT + 1`; `:415` `DECLARE_BITMAP(aid_bitmap, …)` |
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
no regression. Worklogs: [`worklog/2026-06-23-ap-sta-ceiling-100-psram.md`](../worklog/2026-06-23-ap-sta-ceiling-100-psram.md),
[`worklog/2026-06-23-ap-sta-ceiling-255.md`](../worklog/2026-06-23-ap-sta-ceiling-255.md).

---

## TODO / open items (Mesh-gate)

The single backlog for the Mesh-gate L2. (Resolved milestones are above.)

- **802.11s mesh in morselib — the big one.** Mesh + AP concurrency works on Linux but
  needs an all-ESP32 implementation: mesh peering (PLINK), HWMP path selection, mesh
  beacon/`mpath`. No 802.11s exists in morselib today. *Derive from* `net/mac80211/mesh*.c`
  + `morse_driver` mesh paths. Blocks a real ESP32 Mesh-gate (A3).
- **AID ≥ 64 on air.** The 2nd–4th TIM blocks aren't exercised — the dense allocator only
  reaches block 1+ at 64+ live associations, beyond the 3-board bench.
- **Linux STA as TWT *requester*** vs the ESP32 AP responder. Needs the Morse driver's
  requester-role bring-up (`twt_requester=1` global + the correct assoc-time negotiation;
  `morse_cli twt conf` alone returns -1 / "non-requester"). The strongest responder interop test.
- **Action-frame TWT path on hardware.** Mid-session TWT-Setup/Teardown action frames are
  implemented + review-verified but not HW-exercised (the test STA negotiates in assoc IEs).
- **SP-overlap scheduling.** Port Linux's `twt_wi_tree` SP spacing (`twt.c:941`) — matters
  only when many leaves share tight wake intervals.
- **µA current measurement** of a fully-idle TWT link — no bench power-enable line / meter
  (board: RESET_N only, BUSY/WAKE on DNP pads — RISK-02).
- **Regression suite** for every built feature (hello / scan / AP-STA / IBSS / TWT / Mesh+AP)
  so firmware/morselib bumps don't silently regress earlier milestones.

**Known unknown (not a task):** the MM6108 firmware's *true* concurrent-STA capacity is
unpublished (Linux caps at 2007; spec 8191) — 255 is a build/structural ceiling, not a
firmware guarantee.

---

## Methodology — how future Mesh-gate (and Rimba) features get built

Codified in [`.ai/AGENTS.md`](../../.ai/AGENTS.md):

1. **Derive from Linux.** Root-cause against `morse_driver` / `net/mac80211` (same silicon
   Linux drives) and follow it; don't tolerate a symptom with a divergent local hack.
2. **Verify on hardware** (or unit test) — and if you *can't*, document why (e.g. "AID ≥ 64
   needs 64+ associations; not reproducible on a 3-board bench").
3. **Write the new-code ↔ Linux map** for any port (as the two code-map tables above), and
   call out deliberate divergences (PSRAM, fixed-size tables) with the reason.
4. **Cite sources** — `file:line`/SHA for code, command+output for hardware, URLs for
   external facts; prefer authoritative (vendored source) over marketing.

---

## Build / test

```bash
make build APP=rimba-halow-ap  BOARD=proto1-fgh100m   # SoftAP (cap/PSRAM via sdkconfig.defaults)
make build APP=rimba-halow-sta BOARD=proto1-fgh100m   # a TWT-requester leaf
make flash APP=rimba-halow-ap  BOARD=proto1-fgh100m PORT=/dev/ttyACM0
make flash APP=rimba-halow-sta BOARD=proto1-fgh100m PORT=/dev/ttyACM1
```

chronium as a Linux STA (interop oracle) — `wpa_supplicant_s1g`, SAE, **freq 5560**
(S1G ch27 in the 5 GHz model; on-air 915.5 MHz); full recipe in
[`worklog/2026-06-23-ap-multinode-twt-hwtest.md`](../worklog/2026-06-23-ap-multinode-twt-hwtest.md) §3.

## References

- Worklog (Mesh+AP+TWT blow-by-blow + firmware byte-comparison): [`worklog/2026-06-22-mesh-ap-twt.md`](../worklog/2026-06-22-mesh-ap-twt.md)
- Worklogs (STA-count): [`worklog/2026-06-23-ap-sta-ceiling-100-psram.md`](../worklog/2026-06-23-ap-sta-ceiling-100-psram.md), [`worklog/2026-06-23-ap-sta-ceiling-255.md`](../worklog/2026-06-23-ap-sta-ceiling-255.md), multi-node test [`worklog/2026-06-23-ap-multinode-twt-hwtest.md`](../worklog/2026-06-23-ap-multinode-twt-hwtest.md)
- Linux node + Mesh/AP/TWT bring-up: [`design-specification/rimba-linux-node-setup.md`](../design-specification/rimba-linux-node-setup.md) §11–§12
- Power-save context (why TWT matters for leaves): [`design-specification/rimba-mm6108-powersave-analysis.md`](../design-specification/rimba-mm6108-powersave-analysis.md)
- Linux driver source (reference): `morse_driver/{twt.c,mac.c,command.c,beacon.c,dot11ah/tim.c}`; `net/mac80211` TWT/PS/mesh
