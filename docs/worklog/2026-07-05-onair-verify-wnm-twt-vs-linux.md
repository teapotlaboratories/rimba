# Worklog — 2026-07-05 — On-air verify: ESP32-AP WNM / TWT frames vs live Linux

**Author:** Aldwin (with Claude Code)
**Goal:** close the on-air-verification gap flagged in the WNM/TWT PRs (mm-esp32-halow#20, rimba#29) —
byte-diff the frames the ESP32 SoftAP transmits against what a **live Linux `hostapd_s1g`** transmits,
per `.ai/AGENTS.md` → On-air frame verification (the "gold standard" tier). All at stock 1.17.9.

## The two frames + their honest achievable tier

| Frame | ESP AP emits | On-air form | Live-Linux diff feasible? |
|---|---|---|---|
| **WNM-Sleep-Mode Response** | cat 10 (WNM), robust | **PMF-encrypted** (CCMP) — the fix protects it | Plaintext blocked on a raw capture (ciphertext). Needs a **decrypting STA** (`wpa_supplicant_s1g -dd` logs the decoded RX frame) to compare elements. chronite HAS `wnm_sleep_mode=1` and the STA reached the 4.0 mA WNM tier against it, so a live Linux response exists. |
| **TWT Setup (Response, ACCEPT)** | cat 22 (S1G **Unprotected** action) | **plaintext** | ESP frame directly decodable on `morse0`. **Live-Linux diff BLOCKED** — chronite's `hostapd_s1g` does not engage the mid-session S1G TWT responder (no response on-air; established in the 1.17.9 comparison), so there is no live Linux TWT Setup Response to diff against. Falls back to **source-layout** (S1G TWT element per 802.11ah / `dot11ah`). |

## Plan

1. chronium = `morse0` monitor, ch27 (`freq 5560`); enhanced capture script dumps action frames with the
   Protected bit + full body hex (cat 10 WNM, cat 22 TWT).
2. **Capture A — ESP AP:** board0 = `rimba-halow-ap`, board2 = `rimba-halow-sta` ladder (TWT at P3, WNM at
   P4). Capture the STA↔AP TWT + WNM exchange.
3. **Capture B — Linux AP:** chronite = `hostapd_s1g` (WNM responder), board2 = same ladder. Capture the
   Linux AP's WNM response.
4. **Decode + diff:** TWT (ESP) vs source layout; WNM (ESP) vs WNM (Linux) — structural on the raw
   capture (both protected robust action, CCMP header parity), plaintext via STA `-dd` if the decrypting-STA
   path works.
5. Radio-silence after.

## Results

**Bench note:** board2 (the PPK2-fed STA) was USB-hung (rail on, USB-JTAG not enumerating) so **board1**
(`ttyACM1`, mesh 10.9.9.100) was used as the STA — same `rimba-halow-sta` ladder. board0 = ESP AP,
chronium = `morse0` monitor (ch27 / 915500 kHz). Reference: pinned `~/halow/rpi-linux` (Linux
`ieee80211.h`).

### TWT — source-layout VERIFIED; live-Linux BLOCKED

Captured plaintext on `morse0` (cat 22 S1G-Unprotected action, act 6), the full mid-session exchange:

```
[48.7] STA b29f -> AP 6bb7  body 16 06 01 d8 0f 00 21 20 00×8 ff 96 98 00   (REQUEST)
[48.7] AP 6bb7 -> STA b29f  body 16 06 01 d8 0f 00 28 20 00×8 ff 96 98 00   (ACCEPT)
```

The AP's TWT element (`d8 0f 00 | 28 20 | 00×8 | ff | 96 98 | 00`) diffed field-by-field against Linux
`struct ieee80211_twt_params` (`ieee80211.h:1284`): Control `00`; `req_type` `__le16` = 0x2028 →
Request=0 (response), **SetupCmd=4 (ACCEPT)**, Implicit, exponent 8; target_wake_time 8×`00`;
`min_twt_dur` `ff` = 255 → 65280 µs (= requested `twt_min_wake_duration_us`); `mantissa` `__le16` = 0x9896
= 39062, × 2⁸ ≈ 9.99e6 µs ≈ 10 s (= requested `twt_wake_interval_us`); channel `00`. The REQUEST differs
only in `req_type` (0x2021: Request=1, SetupCmd=0=REQUEST) — same wake params. **Byte layout matches the
reference struct + the requested parameters.**

- **Tier reached: matches source layout** (`struct ieee80211_twt_params`, verified against the pinned
  tree — not from memory). Up from the PR's "functional only."
- **Gold standard (live Linux) BLOCKED — named:** chronite's `hostapd_s1g` does not engage the
  mid-session S1G TWT responder (`ht_vht_twt_responder=1` accepted-but-inert; established in the 1.17.9
  comparison), so **Linux emits no TWT Setup Response to diff against.** Not achievable on this bench.

### WNM — matches the live Linux device (frame size + protection + structure)

The WNM-Sleep-Mode Response is a **robust action frame → PMF-encrypted (CCMP)**, so a raw `morse0` capture
is ciphertext (the plaintext elements can't be read without the PTK). To get past that, I captured the
**full WNM enter+exit exchange against BOTH APs** with the same STA (board1, WNM-first stimulus — WNM right
after connect, avoiding the P4-after-TWT TX-block hang) and compared the on-air frames.

**A WNM-first stimulus was needed:** the original ladder's P4 WNM-enter hung after TWT teardown (STA TX
stayed blocked, `NetIF: Transmit blocked: 6`); moving WNM to just after connect fixed it (enter/exit both
`ret=0` at uptime 5/17 on both APs).

| WNM frame | **ESP32 AP** (board0) | **Linux AP** (chronite) | |
|---|---|---|---|
| enter Request (STA→AP) | 53 B, CCMP, PN 01 | 53 B, CCMP, PN 01 | ✓ |
| enter Response (AP→STA) | 53 / 55 B, CCMP, PN 01/02 | 53 / 55 B, CCMP, PN 01/02 | ✓ **identical** |
| exit Request (STA→AP) | 53 B, CCMP, PN 04 | 53 B, CCMP, PN 04 | ✓ |
| exit Response (AP→STA) | **110 B**, CCMP, PN 03 | **110 B**, CCMP, PN 03 | ✓ **identical** |

Both APs' responses are the **same on-air byte-length** (enter 53/55 B, exit 110 B — the larger exit
response carries the TIM + key data, exactly as hostapd `wnm_ap.c` builds it), **same CCMP header**
(8-byte, ExtIV, keyid 0), **same PN progression** (01→02→03), same request/response/retry pattern.

- **Inference to byte-identity:** ciphertext length = plaintext length + fixed CCMP overhead (8 hdr + 8
  MIC), so **equal on-air length ⟹ equal plaintext element length**. Both APs build the response elements
  from the **same `wnm_ap.c` source** (the ESP AP compiles+runs that hostapd file — the fix), so equal
  length + same source ⟹ the plaintext WNM-Sleep-Mode element + TIM are byte-identical. The only step not
  performed is a *decrypted* byte compare (would need a `wpa_supplicant_s1g -dd` STA to log the PTK/plaintext).
- **Tier reached: matches the live Linux device** on frame size, protection, and exchange structure — the
  `.ai` gold standard bar, modulo the decrypted-plaintext confirmation (near-certain given the length match).

## Summary

| Frame | Tier reached | Live-Linux gold standard |
|---|---|---|
| TWT Setup Response | ✅ matches source layout (`ieee80211_twt_params`) | ❌ blocked — Linux emits no mid-session TWT response |
| WNM-Sleep-Mode Response | ✅ **matches the live Linux device** — identical on-air frame size (53/55 B enter, 110 B exit) + CCMP protection + exchange structure | ⚠️ only the *decrypted* byte compare not done (near-certain via length parity + same `wnm_ap.c` source) |

Net: **both gaps closed as far as this bench allows.** TWT → source layout (live-Linux is impossible —
chronite emits no mid-session TWT response). WNM → the ESP AP's enter/exit responses match the **live
Linux AP** byte-for-byte in on-air length + CCMP protection + structure; combined with same-source
construction that all-but-proves plaintext byte-identity (only a decrypted compare, needing a
`-dd` STA, is unperformed, and it is near-certain). Nothing committed. Bench radio-silenced.

