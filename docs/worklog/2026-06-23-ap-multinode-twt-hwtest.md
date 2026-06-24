# Worklog — 2026-06-23 — Multi-node HW test: ESP32 AP + 2 ESP32 STA + Linux STA, TWT

**Author:** Aldwin (with Claude Code)
**Goal:** on-air validation that the rebuilt ESP32 HaLow AP (two-block S1G TIM, MAX_SUPPORTED_AID
128, per-STA state + TWT table in PSRAM — see
[`2026-06-23-ap-sta-ceiling-100-psram.md`](2026-06-23-ap-sta-ceiling-100-psram.md)) still
associates multiple STAs and drives TWT power-save, and that a Linux `morse_driver` STA
interoperates with it.
**Status:** PASS for the core goals. 3 STAs concurrent (2 ESP32 + 1 Linux), TWT power-save
confirmed on the ESP32 STA. Linux-STA-*as-TWT-requester* not achieved (separate Morse-driver
requester-role setup; follow-up noted).

Self-contained record. AP = XIAO ESP32-S3 + FGH100M on `/dev/ttyACM0`, firmware
`rimba-halow-ap` `BOARD=proto1-fgh100m` (the cap=100 + `CONFIG_HALOW_STA_DATA_IN_PSRAM=y` build,
morselib fw 1.17.6). STAs = two identical boards on ACM1/ACM2 running `rimba-halow-sta` (TWT
requester, 1 s interval / 65 ms wake). Linux STA = chronium (Raspberry Pi 5 + Wio-WM6180 MM6108,
`wlan1`, morse driver/fw/cli 1.17.8, 192.168.7.187 mgmt). Network: SSID `rimba-ping`, WPA3-SAE
(PSK `rimbahalow`), S1G ch27 / 915.5 MHz / 1 MHz / op-class 68 / US; HaLow subnet 192.168.12.0/24
(AP .1, ESP32 STAs .2/.3 via DHCP, chronium static .50).

HaLow MACs observed: **AP** `6a:24:99:44:6b:b7`; ESP32 STAs `bc:2a:33:96:b2:9f`,
`68:24:99:44:6a:56`; chronium `wlan1` `3c:22:7f:37:50:42`. (Note the AP's MM6108 HaLow MAC differs
from the ESP32-S3 Wi-Fi MAC `e0:72:a1:f8:ef:a4` — different radios.)

---

## 1. Flashing + roles

- ACM0 ← `rimba-halow-ap` (cap=100/PSRAM build). 1.48 MB image, hash-verified.
- ACM1, ACM2 ← `rimba-halow-sta` (built once, flashed to both; shared build dir → sequential).
- All three boards identical: ESP32-S3 (rev v0.2), 8 MB octal PSRAM.

Caveat learned: pulsing reset with `esptool ... read_mac` re-enumerates the native USB-CDC, which
kills any concurrent `cat /dev/ttyACMx` log capture — so STA boot banners were missed (not a
firmware fault; the AP log + association state confirm the STAs run the new firmware).

## 2. Association — PASS (3 STAs, SAE)

The freshly-flashed AP came up and immediately re-admitted the two ESP32 STAs (they auto-associate
to SSID `rimba-ping`). After reflashing both STAs to the current build, the AP's periodic monitor
(`=== authorized STAs: N (max 4) ===`) showed a steady **2**, then **3** once chronium joined:
```
=== authorized STAs: 3 (max 4) ===
    sta[0] 68:24:99:44:6a:56      (ESP32 STA #2)
    sta[1] bc:2a:33:96:b2:9f      (ESP32 STA #1)
    sta[2] 3c:22:7f:37:50:42      (chronium / Linux STA)
```
So the two-block-TIM / PSRAM rebuild did **not** regress multi-STA association.

## 3. Linux STA interop — PASS

chronium had **no documented infrastructure-STA recipe** (prior docs only cover it as AP or IBSS
peer), so this is new. Working `wpa_supplicant_s1g` STA config:
```
ctrl_interface=/var/run/wpa_supplicant_s1g
country=US
sae_pwe=2                      # GLOBAL field in this fork, NOT per-network
network={
    ssid="rimba-ping"
    key_mgmt=SAE
    sae_password="rimbahalow"
    ieee80211w=2
    scan_freq=5560             # S1G ch27 is represented to nl80211 as the 5 GHz-MODEL freq 5560,
    freq_list=5560             # NOT 915500 (parser max is 70200; 915500 fails to parse)
}
```
```sh
sudo iw reg set US; sudo nmcli dev set wlan1 managed no
sudo iw dev wlan1 set type managed; sudo ip link set wlan1 up
sudo wpa_supplicant_s1g -D nl80211 -i wlan1 -c /tmp/wpa-rimba-sta.conf &
sudo ip addr add 192.168.12.50/24 dev wlan1
```
Result: full SAE (H2E) + 4-way handshake, `CTRL-EVENT-CONNECTED` to BSSID `6a:24:99:44:6b:b7`
(`nl80211: Associated on 5560 MHz (5 GHz mapped)`), `wpa_state=COMPLETED`, pmf=2, CCMP. Data path
both ways: chronium → AP ping 7/8 then 5/5, RTT ~7–280 ms.

Two config gotchas worth remembering: `sae_pwe` is **global** (per-network → "unknown network
field"); S1G channel is the **5 GHz-model freq 5560**, not the on-air 915500 (which exceeds the
parser's 70200 cap). `iw` is in `/usr/sbin` — not on the non-interactive SSH PATH.

## 4. TWT power-save — PASS (ESP32 STA)

The AP app pings only its hardcoded `STA_IP = 192.168.12.2` (ESP32 STA #1). AP→STA RTT:
- Window A: 115–148 ms steady (avg 122 ms over 30 samples), with a 1220 ms spike in an earlier
  window.
- The dozing signature: buffered downlink delivered at the STA's ~1 s TWT service period →
  RTT rises toward the wake interval, vs the **flat ~10 ms** measured before the AP-side
  power-save fix (see [`2026-06-22-mesh-ap-twt.md`](2026-06-22-mesh-ap-twt.md)).

Independent corroboration from the same test: the **non-TWT** Linux STA pinged the AP at a flat
~7–8 ms (it doesn't request TWT), while the **TWT-requesting** ESP32 STA sat at 120 ms+. That
latency gap is the TWT buffering, observed directly side-by-side. So TWT power-save still works
after moving the agreement table to PSRAM — no regression.

## 5. NOT achieved — Linux STA as TWT *requester* (follow-up)

Tried to make chronium a TWT requester against the ESP32 AP responder (the strongest interop
test). The morse driver rejected it: `morse_spi: TWT non-requester trying to send request`, and
`morse_cli -i wlan1 twt conf -w … -d … -c 1` returned -1. Findings:
- `twt_requester=1` is a **global** `wpa_supplicant` option (`config.c`: `BOOL(twt_requester)`),
  not per-network. Setting it did not by itself flip the driver's requester role in my attempts.
- `morse_cli twt conf` wants `-d` (min wake duration) **≤ 65280 µs** (65536 is out of range).
- The morse driver exposes AP/responder tables at
  `/sys/kernel/debug/ieee80211/phy1/morse/{twt_sta_agreements,twt_wi_tree}` (both empty on a STA).
- The requester role is almost certainly negotiated **at association** from the supplicant
  capability, with `morse_cli twt conf` being the wrong/secondary path — needs the correct
  Morse-stack requester bring-up sequence, which isn't documented here yet.

This is a Linux-side enablement detail, **orthogonal to validating the ESP32 AP** (whose TWT
responder is already proven against the ESP32 requester). Deferred.

Side effect during the attempt: restarting the supplicant + a failed `twt conf` left `wlan1` DOWN
and dropped chronium (AP fell to 2 STAs); restoring the clean SAE config above reconnected it
(`wpa_state=COMPLETED`, 5/5 ping) and the AP returned to 3 STAs. **Left state:** all three STAs
associated; chronium `wlan1` is now a connected STA (was not before this session).

## 6. Coverage / limits

- High AID path (AID ≥ 64, the new second TIM block) still NOT exercised — needs 64+ associations;
  this test ran 3 STAs (low AIDs, first TIM block only).
- TWT RTT measured for one ESP32 STA only (AP app pings a single hardcoded IP).
- No µA current measurement (no host power-enable line / meter).
