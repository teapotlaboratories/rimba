# Worklog — 2026-06-23 — AP STA-count ceiling 127 → 255 (four-block S1G TIM)

**Author:** Aldwin (with Claude Code)
**Goal:** raise the HaLow AP's structural STA ceiling from 127 to **255** (the max of the
public `uint8_t mmwlan_ap_args.max_stas` field) and verify it on hardware.
**Status:** code + build + on-air regression test PASS. AID ≥ 64 path still not exercised
(needs 64+ live associations).

Builds on [`2026-06-23-ap-sta-ceiling-100-psram.md`](2026-06-23-ap-sta-ceiling-100-psram.md)
(the 127/two-block work). Same HW: XIAO ESP32-S3 + FGH100M, morselib fw 1.17.6.

---

## Change — two-block (AID 128) → four-block (AID 256)

Three edits in `components/halow`, all flowing from one `#define`:

- `src/umac/ap/traffic_bitmap.h`: `MAX_SUPPORTED_AID` **128 → 256** (four PVB blocks,
  64 AIDs each → valid AIDs 1..255). Auto-recomputes `S1G_BITMAP_SUBBLOCKS` (16→32),
  `MAX_ENCODED_BLOCKS` (2→4), `MAX_SUPPORTED_PVB_LEN` (20→40) in the encoder.
- `src/umac/ies/s1g_tim.c`: tripwire assert `MAX_SUPPORTED_PVB_LEN == 20 → 40`.
- `Kconfig`: `HALOW_AP_MAX_STAS` range `1 127 → 1 255`; help notes 255 is also the
  public `uint8_t max_stas` ceiling (256+ would need that widened to `uint16_t`).

Invariants re-checked for four blocks (AID 256), all hold:

| Invariant | Value @ AID=256 | OK |
|---|---|---|
| `MAX_SUPPORTED_PVB_LEN` ≤ `S1G_TIM_MAX_BLOCK_SIZE - 5` (251) | 40 | ✓ |
| block-offset field width (5-bit, 0..31) | blocks 0..3 used | ✓ |
| `MAX_SUPPORTED_AID < 512` (single TIM page) | 256 | ✓ |
| `bitmap[S1G_BITMAP_SUBBLOCKS]*8 ≥ MAX_SUPPORTED_AID` | 32 B → 256 ≥ 256 | ✓ |
| `MMWLAN_AP_MAX_STAS_LIMIT < MAX_SUPPORTED_AID` | 255 < 256 | ✓ |
| usable STAs = cap (AID 1..255, `stas[256]`, AID 0 reserved) | 255 | ✓ |

The encoder/parse paths were already generic over `MAX_ENCODED_BLOCKS`, so no logic
change — only the derived sizes and the tripwire moved.

Reference AP app (`firmware/rimba-halow-ap/sdkconfig.defaults`): `CONFIG_HALOW_AP_MAX_STAS=255`
+ `CONFIG_HALOW_STA_DATA_IN_PSRAM=y`. At 255 the per-STA `umac_sta_data` (~230 KB total) and
the 255-entry TWT agreement table both come from PSRAM, not internal SRAM.

## Build — PASS

`make build APP=rimba-halow-ap BOARD=proto1-fgh100m` with cap=255 + PSRAM → exit 0,
`rimba_halow_ap.bin` ~1.48 MB. The four TIM static asserts (which would fail the compile if
the four-block arithmetic were wrong) all passed.

## On-air regression test — PASS

Flashed the cap=255 AP; re-ran the multi-node topology from
[`2026-06-23-ap-multinode-twt-hwtest.md`](2026-06-23-ap-multinode-twt-hwtest.md):

- **3 STAs associated** concurrently — 2 ESP32 (`bc:2a:33:96:b2:9f`, `68:24:99:44:6a:56`)
  + chronium Linux STA (`3c:22:7f:37:50:42`), all WPA3-SAE. No crash/assert despite the
  255-entry TWT table now in PSRAM and the four-block TIM beacon path.
- **TWT power-save active** — AP→STA(.2) ping over two windows: 35/35 then 60/60 replies,
  **0 timeouts**, RTT **62–86 ms** (avg ~71 ms) vs the ~10 ms non-TWT baseline. (Steady
  offset rather than the ~1 s spikes seen earlier = ping period phase-locked to the wake
  interval this run; the TWT code is unchanged from the validated cap=100 build.)

One gotcha: after reflashing the AP, the ESP32 STAs held stale DHCP leases and the AP's
hardcoded `STA_IP=192.168.12.2` ping timed out until the STAs were reset (fresh boot →
re-assoc → re-DHCP). Test-harness artifact, not a cap=255 issue. Also confirmed: chronium
(a STA) can't ARP/ping the ESP32 STAs — the AP doesn't forward STA↔STA, so AP→STA (the app's
own ping) is the only downlink-RTT probe available.

## Not exercised / limits (unchanged from the 127 work)

- **AID ≥ 64** (the 2nd/3rd/4th TIM blocks) still not hit on air — needs 64+ live
  associations; this test used 3 STAs (low AIDs, block 0 only). The four-block encoding is
  proven structurally (asserts + stable beaconing), not by a STA in block 1+.
- MM6108 firmware's true concurrent-STA capacity remains unknown (Linux caps at 2007, spec
  8191); the 255 config number is a build/structural ceiling, not a firmware guarantee.
