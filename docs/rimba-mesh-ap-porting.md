# Mesh + AP porting (MM6108: Linux `morse_driver` → ESP32 morselib)

How the **Mesh-gate** topology (802.11s mesh backbone + an AP that lets leaf STAs power-save
via TWT) maps onto the ESP32/morselib stack, what is ported vs. not, and — in detail — the
**AP TWT-responder port**, with a line-by-line comparison against the Linux `morse_driver`.

Governing rule (memory `proper-fix-follow-linux`): behaviour is derived from the Linux side
(`net/mac80211`, `morse_driver`). Companion worklog with the blow-by-blow:
[`worklog/2026-06-22-mesh-ap-twt.md`](worklog/2026-06-22-mesh-ap-twt.md). Linux node bring-up:
[`rimba-linux-node-setup.md`](rimba-linux-node-setup.md) §12.

Paths below: `MORSE = components/halow/components/mm-iot-sdk/framework/morselib/src`;
Linux driver = `morse_driver/` (tag 1.17.8, on the chronium reference node `~/halow/morse_driver`).

---

## 1. Why — the Mesh-gate relay

A Rimba relay wants to be, on one MM6108 radio at once:
- a **mesh point** (802.11s) for the relay↔relay backbone, and
- an **AP** that leaf STAs associate to and **TWT-power-save** under.

The MM6108 supports `#{ managed, AP, mesh point } <= 2, #channels <= 1` concurrently (verified
on-air on the Linux node). The question is what of that is reachable from the ESP32/morselib
stack (which is a different host stack than Linux's `mac80211` + `morse_driver` + hostapd).

## 2. Capability status — ESP32/morselib vs. Linux

| Capability | Linux (`morse_driver`/mac80211) | ESP32 (morselib) | Notes |
|---|---|---|---|
| **AP mode** (S1G, SAE) | ✅ | ✅ | morselib 2.10.4+ (hostapd-backed) |
| **STA mode** + TWT requester | ✅ | ✅ | `mmwlan_twt_add_configuration` |
| **802.11s mesh point** | ✅ (`mesh.c`, `CONFIG_MAC80211_MESH`) | ❌ **not ported** | morselib has only the bare `MORSE_CMD_INTERFACE_TYPE_MESH=5` enum — no 802.11s code (peering/HWMP/mpath) |
| **Mesh + AP concurrent** | ✅ (`iface_combination` AP\|MESH, `#chan<=1`) | ❌ (blocked on mesh) | one radio, co-channel; the Mesh-gate pattern |
| **AP TWT responder** (leaf power-save) | ✅ (host-side, `mac80211` + `twt.c`) | ✅ **ported this work** (§4) | stock fw 1.17.6, no firmware change |

**Bottom line:** AP + AP-TWT-responder are usable on the ESP32 today (TWT after this port).
**Mesh (802.11s) is the missing piece** for a full ESP32 Mesh-gate — it is a substantial
morselib feature (no 802.11s exists there) and is out of scope here.

## 3. Mesh + AP concurrency — verified on Linux, not yet on ESP32

On the Linux node (chronium) one MM6108 ran an AP (`hostapd_s1g`) **and** an open 802.11s mesh
point (`iw … mesh join`) co-channel (ch27/915.5 MHz), and an ESP32 STA associated to the AP —
all at once. Recipe + the gotchas (`type mp` needs an explicit `iw set type`; distinct
locally-administered MAC; bare `freq` not `HT20` to stay co-channel) are in the worklog and
[`rimba-linux-node-setup.md`](rimba-linux-node-setup.md) §12.

On the ESP32 this is blocked only by the **absence of an 802.11s implementation in morselib**
(the AP half works; the mesh half does not exist). Porting 802.11s into morselib (mesh peering,
HWMP path selection, the mesh beacon/PLINK frames) is the remaining work for an all-ESP32
Mesh-gate and is **not** attempted here.

---

## 4. AP TWT-responder port (this work) — WORKS on stock firmware

### 4.1 Architecture

Linux does the whole TWT responder **in the driver, around hostapd**: it parses the STA's TWT
IE out of the received (re)assoc-request, splices the ACCEPT IE into the (re)assoc-response on
TX, and serves the dozing STA's downlink during its service period (SP). morselib has no AP-side
MLME of its own (the bundled hostapd builds assoc frames), so the port mirrors Linux's
driver-layer approach: hook morselib's RX-mgmt and TX-mgmt paths around hostapd, plus the AP
power-save datapath.

**Key finding:** the firmware's `TWT_AGREEMENT_INSTALL` (cmd `0x26`) is gated to
`interface_type==STA` in *every* MM6108 firmware (1.17.6 *and* 1.17.8 `.mbin`, byte-identical to
the Linux `.bin`) — so the AP-vif install fails (`0xffff8000`) on Linux too. It is **not needed**:
both Linux and this port serve the SP **host-side** (buffer the dozing STA's downlink, deliver
when it wakes). The decisive fix was a missing flush-on-wake in morselib's AP datapath.

**Status:** leaf TWT power-save works end-to-end on an ESP32 AP ↔ ESP32 STA, **stock fw 1.17.6,
no firmware change** — 18–23/18–23 ping replies, 0 timeouts, ~15 ms RTT, STA in ~93% doze.

### 4.2 Code map — morselib (new) ↔ Linux `morse_driver` (equivalent)

| # | Role | morselib (NEW code) | Linux `morse_driver` equivalent |
|---|---|---|---|
| 1 | **Advertise TWT-responder cap** in AP S1G caps | `ies/s1g_capabilities.c:318` `ie_s1g_capabilities_build_ap` → `DOT11_S1G_CAP_INFO_8_SET_TWT_RESPONDER_SUPPORT` (gated on `MORSE_CAP_SUPPORTED(…,TWT_RESPONDER)`) | `mac.c:1275` `s1g_capab->capab_info[8] \|= S1G_CAP8_TWT_RESPOND` (gated on `MORSE_CAPAB_SUPPORTED(…,TWT_RESPONDER)`) |
| 2 | **Enable responder role** on the AP vif | `twt/umac_twt.c:56` `umac_twt_init_vif(…, is_responder)` (AP→responder, STA→requester); called from `interface/umac_interface.c:141` for `UMAC_INTERFACE_AP` | `twt.c:2031` `morse_twt_init_vif()` (is_ap → `twt->responder=true`); called `mac.c:3308` |
| 3 | **RX hook** — parse the STA's TWT IE from the (re)assoc-request | `supplicant_shim/supplicant_core.c:396` `umac_supp_process_mgmt_frame` → `umac_twt_responder_handle_assoc_req` (`twt/umac_twt.c:389`, finds the IE via `ie_twt_find`) | `mac.c:6405` → `morse_mac_process_rx_twt_mgmt` (`twt.c:1727`) → `morse_mac_process_twt_ie` (`twt.c:1612`) [assoc-IE path] |
| 4 | **Accept/reject policy** (REQUEST/SUGGEST→accept; DEMAND/GROUPING→reject) | `twt/umac_twt.c:389` (in `…_handle_assoc_req`) | `twt.c:1144-1178` `morse_twt_enter_state_consider_{request,suggest,demand,grouping}` |
| 5 | **Build ACCEPT IE** (setup_cmd=ACCEPT, clear request bit; params as-is) | `twt/umac_twt.c:445` `umac_twt_responder_build_response_ie` (sets `DOT11_TWT_SETUP_CMD_ACCEPT`) | `twt.c:1043` `morse_twt_send_accept` + `twt.c:101` `morse_twt_set_command` |
| 6 | **TX hook** — splice the ACCEPT IE into the (re)assoc-response | `supplicant_shim/driver_ap.c:451` `mmwpas_send_mlme` (alloc `data_len + ie_len`, append IE) | `mac.c:1861` (peek tx queue, `morse_twt_insert_ie` `twt.c:572`) |
| 7 | **Install agreement to firmware** (cmd `0x26`) — *gated on AP vif; harmless* | `twt/umac_twt.c:491` `umac_twt_responder_install` → `umac_twt_install_agreement` (`:286`) → `mmdrv_twt_agreement_install_req`; install hook at `driver_ap.c:277` (on `WPA_STA_AUTHORIZED`) | `mac.c:5024` → `morse_twt_process_pending_cmds` (`twt.c:1355`) → `morse_cmd_twt_agreement_install_req` (`command.c:2211`) |
| 8 | **Agreement blob format** (15 B: control, req_type, twt, min_dur, mantissa, channel) | `twt/umac_twt.c:286` `umac_twt_install_agreement` (packs `&agr->control` + packed `params`) | `twt.c:2326` `morse_twt_initialise_agreement` |
| 9 | **AP-side serving — deliver buffered downlink at the SP** ⭐ the load-bearing fix | `ap/umac_ap.c:890` `umac_ap_set_stad_sleep_state`: on STA asleep→awake with queued frames, `umac_core_evt_wake()` to flush now (PM-bit tracked at `datapath/umac_datapath.c:1528`) | `mac80211` PS buffering (`ieee80211_sta_ps_deliver_wakeup`) + `twt.c` wake-interval tree (`morse_twt_agreement_wake_interval_add` `twt.c:941`, dump `twt.c:201`) |
| 10 | **Data structures** (per-STA table) | `twt/umac_twt_data.h`: `agreements[MMWLAN_AP_MAX_STAS_LIMIT]` (20, = the AP's max-STA cap) + parallel `responder_peers[20][6]`; slot allocated by SA in `umac_twt.c` `umac_twt_responder_alloc_slot`, looked up by `umac_twt_responder_slot_for_peer`, freed on STA leave via `umac_twt_responder_free_agreement` (hooked in `driver_ap.c` `mmwpas_sta_remove`) | `morse_twt` per-STA agreement (stored on the sta) + per-vif `twt_wake_interval_tree` |

### 4.3 The decisive fix (row 9)

morselib's AP already buffers a dozing STA's downlink (`umac_ap_queue_pkt` → traffic bitmap;
`umac_ap_is_stad_paused` gates TX) and tracks sleep state from each frame's PM bit
(`umac_datapath.c:1528`). The bug: when the STA woke (PM=0 at its SP),
`umac_ap_set_stad_sleep_state` cleared the TIM bit but **never kicked the TX loop**, so buffered
downlink waited for the next beacon/DTIM rather than being delivered inside the STA's short SP →
the STA timed out. Fix = call `umac_core_evt_wake()` on the asleep→awake transition when frames
are queued (mirroring the group-addressed release in `umac_ap_get_beacon`). The normal TX path
already wakes the loop (`umac_datapath.c:1978`); the PS-wake path was the one place it was missing.

This is the morselib stand-in for what `mac80211` does on Linux via `ieee80211_sta_ps_deliver_*`,
driven by the TWT SP. The firmware `0x26` install (row 7) — which would let the *firmware*
schedule the SP — is not required and is gated for AP vifs anyway; host-side delivery suffices.

### 4.4 Caveats / open items

- **Multi-STA, bounded table.** ✅ The responder now keeps a per-STA agreement table
  (`UMAC_TWT_NUM_AGREEMENTS == MMWLAN_AP_MAX_STAS_LIMIT == 20`, matching the AP's max-STA cap so
  every associable leaf can hold TWT; parallel `responder_peers[]`), allocated by SA on the
  assoc-req and freed on STA leave — so several leaves can hold TWT at once. The table is
  bounded for embedded RAM: when full, the responder logs and drops the IE (implicit reject,
  STA still associates without TWT). Linux keeps a dynamic per-STA list.
  **Hardware-validated (1 AP + 2 STA):** one ESP32 AP held two TWT-requester ESP32 STAs
  (`68:24:99:44:6a:56` and `bc:2a:33:96:b2:9f`) concurrently associated + authorized, stable
  for ~69 s, with downlink served (ping RTT 10–201 ms — the spikes are TWT doze buffering) and
  no disconnect/crash. (Aside: morselib `MMLOG_INF` doesn't reach the ESP console and the
  USB-CDC console freezes during the association window, so the per-slot grant log isn't
  capturable; validation is via the AP's authorized-STA callback count instead.)
- **No SP-overlap scheduling.** Linux's `twt_wi_tree` spaces multiple STAs' SPs (`twt.c:941`);
  this port serves each STA's downlink reactively on wake (per-STA, via the flush-on-wake) but
  does not yet *schedule* SPs to avoid overlap — fine until many leaves share tight intervals.
- **Downlink latency = TWT interval.** Buffered downlink is delivered at the STA's next SP — the
  inherent TWT trade-off (fine for a leaf that wakes every N minutes; a short interval was used
  on the bench only to fit a 2 s ping timeout).
- **Transport: assoc-IE + TWT action frames.** TWT is negotiated in (re)assoc IEs (the common
  requester flow) *and* via **S1G unprotected action frames** (category 22): a received **TWT Setup**
  (action 6) is accepted and answered with a TWT-Setup response action frame carrying the ACCEPT IE
  (`umac_twt_responder_tx_setup_response` → `umac_datapath_build_and_tx_mgmt_frame`); a **TWT
  Teardown** (action 7) frees the STA's agreement. Both dispatch from `umac_datapath.c` →
  `umac_twt_responder_handle_action`; the accept policy + IE build are shared with the assoc path
  (`umac_twt_responder_accept_ie` / `_fill_ie`). *Not yet exercised on hardware* — our test STA app
  negotiates TWT in the assoc IEs, not mid-session, so the action-frame path is review-verified only.
- **Bench-verified only** (ping workload); no radio-rail current measurement of the µA draw.

### 4.5 Diff summary

8 morselib files, ~245 insertions: `twt/umac_twt.c` (+responder), `twt/umac_twt.h`,
`twt/umac_twt_data.h`, `ies/s1g_capabilities.c`, `interface/umac_interface.c`,
`supplicant_shim/{supplicant_core,driver_ap}.c`, and the one-idea `ap/umac_ap.c` flush-on-wake.

> **Linux mapping re-verified (2026-06-23)** against `morse_driver` **1.17.8**
> source on the reference node (`/home/chronium/halow/morse_driver`, the exact
> driver loaded, `version 0-rel_1_17_8_2026_Mar_24`). Every §4.2 row's symbol +
> line is current: `morse_twt_init_vif` `twt.c:2031`, `morse_mac_process_twt_ie`
> `twt.c:1612`, `morse_mac_process_rx_twt_mgmt` `twt.c:1727`,
> `morse_twt_process_pending_cmds` `twt.c:1355`, `morse_twt_agreement_wake_interval_add`
> `twt.c:941`, responder gate `twt.c:1451` (`if (twt->responder)`), `twt.c:1457`
> "Not a TWT responder but received a request". The port follows the driver.

---

## 5. AP STA-count scaling (63 → 127 → 255) — multi-block S1G TIM port

The Mesh-gate only scales if the AP can hold many leaves. morselib originally
capped the AP at a **single S1G TIM block** (`MAX_SUPPORTED_AID = 64`, AIDs 1..63).
This work raised it to **two** (128 / AID ≤ 127) then **four** blocks
(`MAX_SUPPORTED_AID = 256`, AIDs 1..255) — 255 being the ceiling of the public
`uint8_t mmwlan_ap_args.max_stas` field.

**Key finding — morselib's S1G TIM is a port of the Linux `dot11ah/tim.c`.** They
share the constant *name and value* `S1G_TIM_MAX_BLOCK_SIZE = 256`
(`s1g_tim.h:10` ↔ `dot11ah/tim.h`), the 8-subblocks/block · 8-AIDs/subblock ·
64-AIDs/block structure, the entire-page page-slice sentinel `31`/`0x1F`, and the
**same four PVB encoding modes** (`ENC_MODE_BLOCK/AID/OLB/ADE` ↔ morselib's four
`ie_s1g_tim_*_has_aid` parsers). The encoder loop was *already* generic over
`MAX_ENCODED_BLOCKS`, mirroring the driver — so morselib's single-block cap was a
self-imposed embedded-footprint limit, **not** a behaviour difference. Raising it
brings morselib toward what Linux always did (the driver sizes its AID bitmap to
the full `AID_LIMIT`; mac80211 caps associations at `IEEE80211_MAX_AID = 2007`).

### 5.1 Code map — morselib (changed) ↔ Linux `morse_driver` / `dot11ah`

| Concern | morselib (this work) | Linux equivalent |
|---|---|---|
| **AID space / per-AP traffic bitmap** | `ap/traffic_bitmap.h:19` `MAX_SUPPORTED_AID` 64→128→256; `:20` `S1G_BITMAP_SUBBLOCKS`; `ap/umac_ap_data.h:25` `bitmap[S1G_BITMAP_SUBBLOCKS]` | `morse.h:398` `MORSE_AP_AID_BITMAP_SIZE = AID_LIMIT + 1`; `:415` `DECLARE_BITMAP(aid_bitmap, …)` — driver sizes to the full AID space |
| **Block / subblock geometry** | `ies/s1g_tim.c` `MAX_SUBBLOCKS_IN_BLOCK = 8`; 8 bits/subblock = 8 AIDs → 64 AIDs/block | `dot11ah/tim.h:52` `S1G_TIM_NUM_SUBBLOCKS_PER_BLOCK = 8`; `S1G_TIM_NUM_AID_PER_SUBBLOCK = 8`; `S1G_TIM_NUM_AID_PER_BLOCK = 64` |
| **Per-block encoded-size limit** | `ies/s1g_tim.h:10` `S1G_TIM_MAX_BLOCK_SIZE = 256` (checked at `s1g_tim.c:474`) | `dot11ah/tim.h` `S1G_TIM_MAX_BLOCK_SIZE = 256` *(identical name + value)* |
| **PVB encoder (build the multi-block TIM)** | `ies/s1g_tim.c:397` `ie_s1g_tim_build` — `for (block < MAX_ENCODED_BLOCKS)` w/ `DOT11_TIM_BLOCK_HDR_SET_BLOCK_OFFSET` | `dot11ah/tim.c:1030` `morse_dot11ah_insert_s1g_tim`; block loop `block_offset = S1G_TIM_AID_TO_BLOCK_OFFSET(aid_base)`, `block_k = block_offset + subblock_m/8` (`tim.c:278`) |
| **PVB encoding modes** | parsers `ie_s1g_tim_block_bitmap_has_aid` / `_single_aid_has_aid` / `_olb_has_aid` / `_ade_has_aid` | `dot11ah/tim.h` `enum dot11ah_tim_encoding_mode { ENC_MODE_BLOCK, _AID, _OLB, _ADE }` |
| **Whole-page TIM (no page slicing)** | `s1g_tim.c` `page_slice = 0x1F`, `page_index = 0` | `beacon.c:256` `page_slice_no = S1G_TIM_PAGE_SLICE_ENTIRE_PAGE (31)`, `page_index = 0` (`dot11ah/tim.h:63`) |
| **AID assignment / largest-AID** | `ap/umac_ap.c:513` `stas[aid]`, `aid < max_stas` (dense 1..N) | `mac.c:4934` `test_and_set_bit(aid, aid_bitmap)`; `mac.c:4614` `morse_aid_bitmap_update` (`find_last_bit` → `largest_aid`) |
| **Max-STA ceiling** | `mmwlan.h` `uint8_t max_stas` → 255; `Kconfig HALOW_AP_MAX_STAS` 1..255 | `hostap morse.h:164` `MAX_AID = 2007` (`= IEEE80211_MAX_AID`); driver/RAW use `RAW_CMD_MAX_AID = 2007` |

### 5.2 Divergence — per-STA state in PSRAM (no Linux equivalent)

`CONFIG_HALOW_STA_DATA_IN_PSRAM` routes each `umac_sta_data` **and** the per-vif
TWT agreement table to PSRAM (`umac_data.c`, `MALLOC_CAP_SPIRAM`, strict — no
internal-SRAM fallback). **This has no Linux counterpart by design:** the kernel
driver allocates per-STA state with `kmalloc`/`GFP_KERNEL` from one address space,
the `aid_bitmap` is a static `DECLARE_BITMAP`, and per-STA TWT agreements live on
mac80211's `ieee80211_sta`. The PSRAM split is an ESP32-S3-specific adaptation to
keep a large cap out of the scarce internal-SRAM pool — a *platform* change, not a
port. (Likewise, morselib's **fixed** `agreements[MMWLAN_AP_MAX_STAS_LIMIT]` table
vs. Linux's *dynamic* per-STA list is an embedded-RAM choice, §4.4.)

### 5.3 Progression + validation

| Cap | `MAX_SUPPORTED_AID` | Blocks | `MAX_SUPPORTED_PVB_LEN` | Status |
|---|---|---|---|---|
| 63 | 64 | 1 | 10 | original morselib |
| 127 | 128 | 2 | 20 | #6 — build + 3-node HW test |
| 255 | 256 | 4 | 40 | build + 3-node HW test |

Validated: builds clean at each step (four-block static asserts pass); on-air,
1 ESP32 AP + 2 ESP32 STA + 1 chronium Linux STA associate concurrently (SAE) with
TWT power-save active and no regression. **Not yet exercised: AID ≥ 64** (the 2nd–4th
blocks) — the dense allocator only reaches block 1+ at 64+ live associations, beyond
the 3-board bench. Worklogs: [`worklog/2026-06-23-ap-sta-ceiling-100-psram.md`](worklog/2026-06-23-ap-sta-ceiling-100-psram.md),
[`worklog/2026-06-23-ap-sta-ceiling-255.md`](worklog/2026-06-23-ap-sta-ceiling-255.md).

---

## 6. Open items / backlog

Tracked TWT / AP work not yet done (mirrors the live task list):

- **TWT power-save — RESOLVED on hardware.** ✅ The STA now **deep-TWT-sleeps against our ESP32 AP**:
  AP→STA ping RTT 17–953 ms (max ≈ the 1 s TWT interval), matching the Linux AP (~890 ms). Before the
  fix it was a flat ~10 ms (the STA never slept). **Root cause:** hostapd calls `sta_remove` transiently
  during (re)association cleanup; our `mmwpas_sta_remove` hook freed the STA's just-accepted TWT slot
  (still `PENDING_INSTALLATION`) *before* `build_response_ie` ran, so the assoc-resp carried **no ACCEPT
  IE** and the STA never established TWT. **Fix:** `umac_twt_responder_free_agreement` now frees only an
  `INSTALLED` agreement — the transient assoc-time removal (PENDING) is ignored; a real post-authorized
  departure (INSTALLED) is freed; `alloc_slot` reuse-by-SA still prevents leaks for re-associating STAs.
  Isolated via a Linux-AP comparison (same STA sleeps there) + freeze-proof AP-side counters logged from
  the beacon path; the STA + firmware were always fine — the bug was purely AP-side. *Remaining
  nice-to-have:* a current-meter µA reading on a fully-idle link to quantify the floor (board caveat: no
  host power-enable line, RESET_N only, BUSY/WAKE on DNP pads — RISK-02).
  - **BCF is correct (cleared a false lead).** The runtime "BCF board description: mf16858" is *not*
    a wrong/mislabeled file — `components/firmware/mm6108/bcf_fgh100mhaamd.mbin` is byte-identical to
    the genuine `bcf/quectel/bcf_fgh100mhaamd.bin` from Morse's `morse-firmware` repo (now vendored at
    `vendor/morse-firmware` as the BCF source of truth), and that genuine FGH100M-H BCF simply carries
    `.board_desc = "mf16858"` (the FGH100M-H shares the mf16858 board reference). So `BOARD=proto1-fgh100m`
    loads the right calibration; flaky association must be explained elsewhere (PS config / RF env).
  - Levers to try for real sleep: explicit `mmwlan_set_power_save_mode` + `mmwlan_set_dynamic_ps_timeout`,
    a longer AP beacon/DTIM + STA `mmwlan_set_listen_interval` (applied *post-assoc*), or WNM sleep.
- **AP STA-count ceiling + PSRAM — RESOLVED (build-verified; HW-at-scale deferred).** ✅ Cap and
  per-STA RAM placement are `sdkconfig`-selectable; structural ceiling raised 63 → **127** → **255**
  (the `uint8_t max_stas` limit). The real ceiling was the multi-block **S1G TIM**, not firmware/`#define`
  — and morselib's TIM is a port of Linux `dot11ah/tim.c`, so the fix follows Linux. **Full write-up +
  Linux code map in §5 above.** *Deferred:* on-air validation with AID ≥ 64 (needs 64+ associations).
- **Action-frame path not yet HW-exercised.** Mid-session TWT-Setup + teardown action frames are
  implemented and review-verified, but our test STA negotiates TWT in assoc IEs, not mid-session.
- **Regression suite** for every built feature (hello / scan / AP-STA / IBSS / TWT / Mesh+AP) so
  firmware/morselib bumps don't silently regress earlier milestones.

## 7. References

- Worklog (blow-by-blow + firmware byte-comparison): [`worklog/2026-06-22-mesh-ap-twt.md`](worklog/2026-06-22-mesh-ap-twt.md)
- Worklog (STA-count ceiling → 100, two-block S1G TIM, per-STA PSRAM): [`worklog/2026-06-23-ap-sta-ceiling-100-psram.md`](worklog/2026-06-23-ap-sta-ceiling-100-psram.md)
- Linux node + Mesh/AP/TWT bring-up: [`rimba-linux-node-setup.md`](rimba-linux-node-setup.md) §11–§12
- Power-save context (why TWT matters for leaves): [`rimba-mm6108-powersave-analysis.md`](rimba-mm6108-powersave-analysis.md)
- Linux driver source (reference): `morse_driver/{twt.c,mac.c,command.c}`, `mesh.c`; `net/mac80211` TWT/PS
