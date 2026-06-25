# Worklog — 2026-06-24 — Action-frame TWT (requester) on hardware

**Author:** Aldwin (with Claude Code)
**Goal:** exercise the **mid-session, action-frame** TWT path on real hardware. Until now the
ESP32 STA only ever negotiated TWT in **association IEs** (join-time). This adds the
**requester-side action-frame sender** that morselib was missing, so a STA can set up *and*
tear down a TWT agreement **without re-associating** — and proves it engages power-save (doze)
mid-session against the ESP32 AP.
**Status:** ✅ **PASS** against the ESP32 AP (validated with **two STAs concurrently**).
⚠️ Linux-AP interop is blocked by PMF (documented under Caveats — the request is accepted, the
*response* is encrypted and can't yet be installed).

Hardware: 3× XIAO ESP32-S3 + FGH100M (`/dev/ttyACM0..2`, `BOARD=proto1-fgh100m`) +
chronium (RPi5 + MM6108, `morse_driver` 1.17.8). AP build on ACM0; action-frame STA build on
ACM1 (`bc:2a:33:96:b2:9f`) and ACM2 (`68:24:99:44:6a:56`).

---

## What "action-frame TWT" is and why it matters

TWT (Target Wake Time) lets a STA sleep (doze) between scheduled wake windows. There are two
ways to negotiate it:

- **Assoc-IE TWT** — the agreement is carried in the (Re)Association Request/Response IEs. It
  is fixed at *join time*; changing it means tearing down and re-associating.
- **Action-frame TWT** — a standalone **TWT-Setup** management *action* frame negotiates the
  agreement **mid-session**, association-preserving. A **TWT-Teardown** action ends it. This is
  what lets a node adapt its wake schedule on the fly (e.g. switch to a low-power schedule when
  idle, or renegotiate without dropping the link) — essential for a mesh leaf that shouldn't
  re-auth every time its duty cycle changes.

morselib already had the **responder** (AP) half (`umac_twt_responder_handle_action` /
`_tx_setup_response`). It had **no requester (STA) action-frame sender** — that is what this
work added, mirroring the Linux `morse_driver`.

## New code ↔ Linux map

Per the porting rule, the new morselib code is derived from `morse_driver` / `net/mac80211`:

| New morselib code | Linux reference | Role |
|---|---|---|
| `umac_twt_requester_tx_setup()` (`umac_twt.c`) | `morse_mac_send_twt_action_frame()` (setup) | Build agreement via CONFIGURE cmd, then TX a TWT-Setup action (cmd REQUEST/SUGGEST/DEMAND) |
| `umac_twt_requester_tx_teardown()` (`umac_twt.c`) | `morse_mac_send_twt_action_frame()` (teardown) | TX `[cat][action 7][flow_id]`, free local slot |
| `umac_twt_requester_handle_action()` (`umac_twt.c`) | requester half of `morse_mac_process_rx_twt_mgmt()` | RX cat-22 action 6 (Setup *response*) → `umac_twt_process_ie` + install pending agreement |
| dispatch in `umac_datapath.c` (`DOT11_ACTION_CATEGORY_S1G_UNPROTECTED`) | `morse_mac_rx_s1g_action()` | Route S1G action to both responder + requester handlers |
| `mmwlan_twt_setup_request()` / `_teardown()` (`umac.c`, `mmwlan.h`) | (nl80211/`morse_cli twt`) app entry | App→umac-task entry; queue evt that runs the TX on the umac task |

**The key fix:** `umac_twt_add_configuration()` only *stores* config (a `memcpy`) — it does
**not** build a PENDING_RESPONSE agreement. The assoc path builds the agreement via a
**CONFIGURE** command. So `tx_setup` first issues `UMAC_TWT_CMD_TYPE_CONFIGURE`
(`umac_twt_handle_command`) to create the pending agreement, fills the TWT IE from that slot
with `umac_twt_responder_fill_ie`, and TXes it as an action frame via
`umac_datapath_build_and_tx_mgmt_frame(... frame_action_build ...)`.

## Test method

STA app (`firmware/rimba-halow-sta`): connect **without** assoc-IE TWT (flat awake baseline)
→ 12 s baseline → `mmwlan_twt_setup_request()` (action frame) → 30 s doze window →
`mmwlan_twt_teardown()`. Requester role, 1 s wake interval, ~65 ms min wake.

Each STA pins a **MAC-derived static IP** (`192.168.12.<mac[5]>`) so two STAs don't collide on
`.2`; the AP tracks each authorized STA's MAC and pings it at the same address. The AP→STA ping
RTT is the doze probe: a ping that lands while the STA is dozing waits for the next TWT service
period (~1 s), so RTT spikes; a ping that lands inside the ~65 ms wake window returns flat.

## Results

Both STAs held concurrently — AP logged **"authorized STAs: 2"** the whole window.

**STA `.159` baseline → doze transition** (AP→STA RTT, ms):

```
seq=229  22 ms        <- baseline (no TWT yet)
seq=230  19 ms
seq=231  966 ms   <-- DOZE engaged (ping waits for next ~1 s wake)
seq=232  13 ms        <- ping landed inside wake window
seq=235  956 ms   <-- DOZE
seq=239  948 ms   <-- DOZE
seq=243  966 ms   <-- DOZE  (regular ~1 s cadence = the wake interval)
```

RTT distribution after setup:

| STA | flat (<100 ms) | elevated (≥100 ms, doze) | max RTT |
|---|---|---|---|
| `.159` (`bc:2a:33:96:b2:9f`) | 86 | 9 | **1057 ms** |
| `.86`  (`68:24:99:44:6a:56`) | 76 | 10 | **1972 ms** (two missed intervals) |

The ~1 s spikes are absent from a non-TWT baseline (flat ~10–22 ms), so they are caused by the
action-frame TWT agreement, not link jitter. Capture saved at `/tmp/ap-multi-twt.log`.

## Caveats / known gaps

1. **Linux-AP interop blocked by PMF.** With the Morse AP (PMF required), the STA's TWT-Setup
   **request** is accepted (the AP builds the agreement with the exact requested params — Flow 0,
   1 000 000 µs interval, 65 280 µs duration), but the AP's Setup **response** is sent
   **CCMP-protected** (robust management frame). morselib delivers it to the action handler
   **un-decrypted**, so the requester can't parse the TWT IE and never installs → no doze on a
   Linux AP. The Linux RX path decrypts first
   (`twt_action = skb->data + (is_protected ? IEEE80211_CCMP_HDR_LEN : 0)`); morselib's STA RX
   has no robust-management-frame decryption hook here. The ESP32 AP (morselib responder) sends
   the response **unprotected** (category 22), so STA↔ESP32-AP works end-to-end. *Fix later:*
   wire RMF RX decryption for S1G action frames.
2. **Teardown firmware-remove unwired.** `umac_twt_requester_tx_teardown()` TXes the Teardown
   action and frees the AP-side + local agreement slot (state → EMPTY), but does **not** issue
   the firmware agreement-remove command (`0x27`). The MAC stops the agreement on Teardown RX in
   practice; wiring `0x27` is a cleanup follow-up.

## Files changed (morselib submodule + apps)

- `morselib/.../umac/twt/umac_twt.c` / `.h` — requester `tx_setup` / `tx_teardown` /
  `handle_action`; constants `UMAC_S1G_ACTION_TWT_SETUP` (6) / `_TEARDOWN` (7).
- `morselib/.../umac/datapath/umac_datapath.c` — requester branch in the S1G action dispatch.
- `morselib/.../umac/umac.c` + `include/mmwlan.h` — `mmwlan_twt_setup_request()` /
  `mmwlan_twt_teardown()` + their umac-task evt handlers.
- `firmware/rimba-halow-sta/main/app_main.c` — connect without assoc-IE TWT; baseline →
  setup-request → teardown sequence; MAC-derived static IP.
- `firmware/rimba-halow-ap/main/app_main.c` — per-STA ping (`192.168.12.<mac[5]>`) so multiple
  leaves can be probed at once.

## Next

- RAW (Restricted Access Window), AP-side — port from `morse_driver` `raw.c` + `page_slicing.c`,
  follow Linux exactly. Recon/feasibility pass first. Separate branch + PR.
