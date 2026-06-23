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
| 10 | **Data structures** | `twt/umac_twt_data.h` `struct umac_twt_data` (added `responder_peer[6]`; `agreements[1]`) | `morse_twt` per-STA list + per-vif `twt_wake_interval_tree` |

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

- **Single-STA agreement slot.** `umac_twt_data` holds one agreement + one `responder_peer`
  (`UMAC_TWT_NUM_AGREEMENTS == 1`). Multi-STA needs the per-STA list Linux keeps (row 10).
- **No SP-overlap scheduling.** Linux's `twt_wi_tree` spaces multiple STAs' SPs (`twt.c:941`);
  this port uses the requested target-wake-time as-is (fine for one STA).
- **Downlink latency = TWT interval.** Buffered downlink is delivered at the STA's next SP — the
  inherent TWT trade-off (fine for a leaf that wakes every N minutes; a short interval was used
  on the bench only to fit a 2 s ping timeout).
- **Transport = assoc-IE only.** TWT is negotiated in (re)assoc IEs (matching the morselib STA
  requester); the standalone TWT-Setup *action-frame* path Linux also supports is not ported.
- **Bench-verified only** (ping workload); no radio-rail current measurement of the µA draw.

### 4.5 Diff summary

8 morselib files, ~245 insertions: `twt/umac_twt.c` (+responder), `twt/umac_twt.h`,
`twt/umac_twt_data.h`, `ies/s1g_capabilities.c`, `interface/umac_interface.c`,
`supplicant_shim/{supplicant_core,driver_ap}.c`, and the one-idea `ap/umac_ap.c` flush-on-wake.

---

## 5. References

- Worklog (blow-by-blow + firmware byte-comparison): [`worklog/2026-06-22-mesh-ap-twt.md`](worklog/2026-06-22-mesh-ap-twt.md)
- Linux node + Mesh/AP/TWT bring-up: [`rimba-linux-node-setup.md`](rimba-linux-node-setup.md) §11–§12
- Power-save context (why TWT matters for leaves): [`rimba-mm6108-powersave-analysis.md`](rimba-mm6108-powersave-analysis.md)
- Linux driver source (reference): `morse_driver/{twt.c,mac.c,command.c}`, `mesh.c`; `net/mac80211` TWT/PS
