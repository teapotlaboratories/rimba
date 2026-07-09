# 2026-07-08 — 1.17.9 userspace build + clean matched-1.17.9 STA power-save retest

> **⚠️ RETRACTION (same day, later) — the CONCLUSIONS in this worklog are WRONG.** The §3–§6 claims
> ("TWT doesn't engage / the 6.8 mA is a mislabel / all doze tiers cluster at a ~15 mA host floor / the APs
> are power-save peers") were measured on a bench that turned out to be **1.17.9-regressed**. Reverting to
> 1.17.8 showed the doze current there is ~2× lower (Dyn-PS 9 vs 20, WNM 5 vs 17.5) and the original 1.17.8
> figures are REAL. The correct result — **1.17.9 regresses STA power-save; 1.17.8 is the good baseline** —
> is in `2026-07-08-1178-ps-baseline-and-deepsleep.md`. The **build/deploy mechanics** in §1–§2 below (how
> the 1.17.9 userspace was built, the ABI/one-build-for-all-nodes finding, the bench-scripting gotchas) are
> still accurate and reusable; only the power *conclusions* are retracted.

Two pieces of work, both self-contained here:
1. Built + deployed the **1.17.9 Linux userspace** to all four nodes (the bench was NOT actually matched
   before — the userspace was still 1.17.8 while the docs claimed 1.17.9).
2. Re-ran the STA power-save ladder against both APs at **true matched 1.17.9** using the C6-harness /
   triggered-ladder rig — the "sleep-robust harness" the comparison doc kept saying was missing. The clean
   pass **overturns the doc's headline TWT finding**.

---

## 1. Userspace 1.17.9 build (the bench was version-skewed)

Investigating "is everything 1.17.9 on Linux?" found a split: kernel side (morse.ko / dot11ah.ko / mm6108
fw) was `rel_1_17_9_2026_Apr_20`, but the **userspace (hostapd_s1g / wpa_supplicant_s1g / morse_cli) was
still `rel_1_17_8`** on all four nodes. So `rimba-linux-node-setup.md`'s "ALL components 1.17.9 (2026-07-07)"
was an over-claim, and the PS comparison doc's "matched 1.17.9" 2026-07-05 numbers were taken with a **1.17.8
userspace** (hostapd AP = 1.17.8).

Build (on chronium, Pi 5 native):
- `~/halow/hostap` — the tree carries the **SAE-injector patches** (`sae.c/ieee802_11.c/mesh_*`). Saved them
  to `~/hostap_sae_injector_1178.patch` + `git stash`, then `git checkout 1.17.9` (`beed5f8c8`). Reused the
  existing `hostapd/.config` + `wpa_supplicant/.config`. `make` → `hostapd_s1g` + `wpa_supplicant_s1g`
  `rel_1_17_9` (0 errors; same byte-size as 1.17.8 → the change is tiny).
- `~/halow/morse_cli` — the **1.17.9 tag points at the same commit as 1.17.8** (`8f06222`); morse_cli is
  unchanged between the two, so the running `rel_1_17_8` binary IS the 1.17.9 source (cosmetic version
  string only). No rebuild needed.
- **One build serves all four nodes**: chronium/chronite (Pi 5) and chronosalt/chronogen (Pi Zero 2 W) are
  ALL aarch64 / Debian 13 trixie / glibc 2.41 / libnl-3 so.200 / libssl so.3 → identical ABI, so the Pi 5
  binaries run as-is on the Pi Zeros (no cross-build). Deployed via `rm`+`cp` (avoids ETXTBSY on a live
  hostapd), 1.17.8 backed up to `/root/us_1178_rollback/` on each node.
- Restarted chronite's hostapd so the running AP is the 1.17.9 binary (a live process keeps its old inode
  through `rm`+`cp` — must restart).

Result: **all four nodes fully `rel_1_17_9`** (driver + fw + dot11ah + hostapd_s1g + wpa_supplicant_s1g;
morse_cli = shared 1.17.9 commit). Combined with the ESPs' mm6108.bin 481040 (1.17.9) the whole bench is
genuinely matched for the first time.

## 2. The clean retest rig

board2 STA (XIAO ESP32-S3 + FGH100M), TX-capped 1 dBm, on the PPK2 (5 V, 1 s avg). `firmware/rimba-halow-sta`
triggered ladder: boots host-AWAKE, connects, then on each C6 GPIO20→D5 trigger runs a fixed 4-tier ladder
(P1 No-PS → P2 Dyn-PS → P3 TWT 10 s SP → P4 WNM+chip-powerdown, 18 s each), marking each phase entry on D5 +
a timestamped `ESP_LOGW` on the console. Because it never self-sleeps host-awake, the console stays
enumerated → phase markers align cleanly to the PPK2 epoch trace. `HOST_LIGHT_SLEEP` compile flag adds
`esp_pm_configure(160/40, light_sleep_enable=true)` for §3c (PM_ENABLE + tickless already in sdkconfig).

Also added a **TX cap to `firmware/rimba-halow-ap`** (`mmwlan_override_max_tx_power(1)`): without it, board0
at full power into a very close board2 overloads board2's RX and the TWT handshake/teardown WEDGES board2
("Transmit blocked: 6" forever). With both sides capped the ladder completes.

Bench-scripting gotcha (cost real time): `reflash_and_flash.py` self-kills via `pgrep -f reflash_and_flash.py`,
which also matches the launching bash shell's command line → the shell dies silently. Fix: run it under a
different basename (`rf_run.py`). Same trap bit `stop_bench.py` (kept "rf_run.py" out of the launcher cmdline).

## 3. Results — host AWAKE (§3a), both APs, matched 1.17.9, 2 ladders each

| Mode      | Linux AP (chronite) | ESP AP (board0) |
|-----------|---------------------|-----------------|
| No-PS     | 53.8 mA             | 50.5 mA         |
| Dyn-PS    | 20.2 mA             | 16.7 mA         |
| TWT (10s) | 20.5 mA             | ~15 mA          |
| WNM+pd    | 17.5 mA             | 14.5 mA         |

(No-PS is TX-capped here; the doc's 65 mA was full-TX.) **Every doze tier clusters at the ~15–20 mA
host-awake floor on BOTH APs.** TWT is at most marginally below Dyn-PS — NOT the doc's 6.8 mA. This is
physically required: with the ESP32 host awake at 160 MHz the CPU alone is ~15 mA, so nothing radio-side can
pull the whole board below that. The doc already corrected its WNM "4–5 mA host-awake" rows as mislabeled
host-light-sleep; the **TWT 6.8 mA "host awake" row is the same class of mislabel** (6.8 < 15 mA floor).

## 4. Results — + host LIGHT SLEEP (§3c) vs the ESP AP

One clean LS ladder captured (LS flaps the USB console — the §3c fragility; markers survived the phase
entries, PPK2 plateaus are unambiguous):

| Mode (+ host LS) | ESP AP | reading |
|------------------|--------|---------|
| No-PS+LS   | 31.2 mA | backfire (radio always on, constant light-sleep exits) |
| Dyn-PS+LS  | 30.8 mA | backfire (per-DTIM wakes) |
| **TWT+LS** | **30.8 mA** | **BACKFIRE — identical to Dyn-PS+LS** |
| WNM+pd+LS  | **3.0 mA**  | WIN (radio off → host sleeps long; AP-independent) |

**TWT+LS = 30.8 mA is the smoking gun.** If TWT installed a 10 s SP schedule, wakes would be rare and host
LS would net ~3 mA (as WNM does). Instead it backfires exactly like Dyn-PS → **board2 is waking at DTIM
cadence, i.e. TWT is NOT engaging.** The doc's headline "TWT + host LS = 3–4 mA against the ESP AP, the key
divergence" does not reproduce.

Since TWT doesn't engage on either AP and WNM+LS is radio-off (AP-independent), the Linux-AP LS profile is
the same shape (all backfire except WNM+LS ≈ 3 mA) — the doc's Linux §3c column already reads that way.

## 5. On-air (inconclusive, consistent)

chronium morse0 monitor on ch27, scanning for the TWT element (EID 216) across a P3: **983 frames, 0
decodable TWT setup exchange.** board2's TX-capped (1 dBm) request is likely under-captured and S1G
action-frame decode is unreliable with the ad-hoc tooling, so this neither confirms nor denies board0's
response at the frame level — but it is consistent with the power evidence (no engagement, no schedule).

## 6. Verdict / doc impact

- Bench is now **genuinely matched 1.17.9** (userspace built+deployed today; docs were over-claiming).
- **Host-awake:** all doze tiers ~15–20 mA on both APs — AP-independent; the host CPU floor dominates.
- **The ESP-AP mid-session-TWT advantage does NOT reproduce at matched 1.17.9.** Host-awake TWT ≈ Dyn-PS;
  TWT+host-LS backfires (31 mA) instead of the claimed 3–4 mA. The doc's 6.8 mA "host-awake" TWT was a
  mislabeled/artefactual light-sleep number below the physical host-awake floor.
- **WNM+powerdown+host-LS ≈ 3.0 mA** on both APs (radio-off, AP-independent) — the real deepest *associated*
  low-power leaf. Deep-sleep floor is ~0.6 mA (RESET_N-low, separate rig).

Still TODO for the "full retest": §3b downlink-while-dozing and §3a′ Mesh+AP at matched 1.17.9.
