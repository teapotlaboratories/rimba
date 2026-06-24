# Linux MM6108 node — HaLow bring-up (Path B kernel) — 2026-06-19

Bringing up the **Raspberry Pi 5** dev host as the **Linux MM6108 reference/interop
node** the open-IBSS test plan calls for (P0.5). This is the durable handoff for
resuming **after the reboot** — the Claude session runs *on* the Pi, so the reboot
ends it; this doc + `~/halow/staging/` + the `memory/` notes are how the next
session continues.

---

## Hardware (what this node actually is)

- **Host:** Raspberry Pi 5, 8 GB, was running stock `6.18.34+rpt-rpi-2712`.
- **HaLow radio:** Seeed **Wio-WM6108** (Quectel **FGH100M-H**, Morse Micro **MM6108**),
  seated in the **Seeed WM1302 Pi hat**, talking to the Pi **over SPI** (not PCIe/SDIO).
  Nothing enumerated on any bus at start — SPI was disabled (`nospi10`, no `dtparam=spi`).
- `wlan0` = onboard **Broadcom CYW43455** (brcmfmac) — the SSH/remote link rides on it,
  NOT the HaLow radio. The HaLow radio comes up as a separate `wlanN`.
- Module BCF: **`bcf_fgh100mhaamd.bin`** (the "H" = FGH100M-H). Chip FW: `mm6108.bin`.

## The decision that shaped everything (Path A dead → Path B)

The out-of-tree `morse_driver` **cannot build against the stock 6.18 kernel** — it needs
mac80211 S1G channel-bandwidth flags (`IEEE80211_CHAN_{1,2,4,8}MHZ`) that only exist in
Morse Micro's patched kernel tree. (Confirmed empirically: the build died on those flags
being undeclared; they're absent from `/usr/src/linux-headers-6.18.34*/include/net/cfg80211.h`.)
Morse's newest patched branch is **`mm/rpi-6.12.21/1.17.x`** (no 6.18). So the node
**requires building + booting Morse's patched 6.12.21 kernel** (a downgrade from 6.18.34).
This is also the porting guide's recommended path.

## Sources followed (authority order)

1. **`docs/design-specification/MM_APPNOTE-24_Linux_Porting_Guide.pdf`** — Morse's official Linux Porting Guide
   v2 (governing source; §2 reqs/kernels, §3 patches, §4 driver, §5 firmware/BCF, §6 DT, §7 bring-up).
2. **`morse_driver` source** — ground truth for the DT contract (`spi.c`, `of.c`).
3. **teapotlaboratories worklog** (WM6180→Luckfox over SPI) — same flow, different SoC/pins.
4. **Morse community thread** (exact WM6108+WM1302+Pi5 combo) — only for the board-specific
   GPIO pin numbers (reset=17, power/wake+busy=23/24, spi-irq=5; on RP1 `&spi0`).

Corrections APPNOTE-24 made to the first-pass plan: driver flags add `CONFIG_MORSE_VENDOR_COMMAND=y`
(drop SDIO, SPI-only); BCF fallback symlink is `bcf_default.bin` not `bcf_boardtype_0801`
(0801 = Morse's own MF08651; explicit `bcf=` param is the primary selector anyway); load
order needs `modprobe mac80211 && modprobe crc7` before the morse modules (`crc7` is `=m`).

---

## What was done (all non-destructive; default boot still 6.18)

Working dir `~/halow/` (clones: `morse_driver` 1.17.9, `morse_cli` rel_1_17_8, `morse-firmware`,
`hostap`, `rpi-linux@mm/rpi-6.12.21/1.17.x`). Staging: `~/halow/staging/`.

- Build deps installed (bc/bison/flex/libssl/ncurses/dtc/libnl-3/libnl-route-3/libusb-1.0/poppler-utils).
- **Kernel `6.12.21-v8-16k+` built** (`bcm2712_defconfig`; mac80211/cfg80211=m, CRYPTO CCM/GCM/AES,
  CRC7=m, **BRCMFMAC=m so onboard wifi/remote link survives**, S1G CHAN flags present).
  `Image.gz` + Pi5 dtb + 350 overlays. `make modules_install` → `/lib/modules/6.12.21-v8-16k+`.
- **`morse_driver` built vs the new kernel** (vermagic matches) → `morse.ko` + `dot11ah.ko`
  installed into that module tree + `depmod`. (This was the exact build that FAILED on 6.18.)
- **`morse_cli`**, **`hostapd_s1g`** + `hostapd_cli_s1g` built → `/usr/local/bin`.
  (`wpa_supplicant_s1g` deferred: OpenSSL-3.0 `EC_KEY` deprecation-as-error in DPP; not needed
  for the AP test — fix for the IBSS/open-join phase, e.g. disable CONFIG_DPP or -Wno-deprecated.)
- Firmware/BCF in `/lib/firmware/morse` (+ `bcf_default.bin` → `bcf_fgh100mhaamd.bin`).
  `/etc/modprobe.d/morse.conf`:
  `options morse bcf=bcf_fgh100mhaamd.bin country=US spi_clock_speed=20000000 enable_ps=0`.
- **Pi-5 SPI overlay** authored + dtc-validated → `/boot/firmware/overlays/wm6108-spi-pi5.dtbo`
  (compatible `morse,mm610x-spi`; reset=GPIO17 active-low, power=GPIO23/24, spi-irq=GPIO5,
  CS0=GPIO8, target `&spi0`/RP1). Source: `~/halow/staging/wm6108-spi-pi5-overlay.dts`.
- **`/boot/firmware`:** `kernel-morse.img`, `initramfs-morse` (built for 6.12.21; root drivers
  ext4/mmc/sdhci-brcmstb verified `=y`), `config.txt.bak` (stock 6.18, the UNTOUCHED default),
  and **`tryboot.txt`** = config.txt + `dtoverlay=wm6108-spi-pi5` + `kernel=kernel-morse.img`
  + `initramfs initramfs-morse followkernel`.

Boot safety: default `config.txt` is unchanged (boots 6.18). `tryboot` boots the Morse kernel
ONCE; a power-cycle auto-reverts to 6.18. Nothing permanent until `promote-kernel.sh`.

---

## RESUME HERE after reboot

1. **Reboot into the Morse kernel (once):**
   ```bash
   sudo reboot '0 tryboot'
   ```
2. **New session — verify bring-up:**
   ```bash
   ~/halow/staging/verify-bringup.sh
   ```
   Checks: `uname` = 6.12.21-v8-16k+ · dmesg shows `mm6108.bin` + BCF loaded · new `wlanN`
   (S1G) in `iw dev` · `morse_cli -i wlanN stats` responds · onboard `wlan0` link survived.
3. **If good → make permanent:** `~/halow/staging/promote-kernel.sh`
4. **AP test (APPNOTE-24 §7.2):** start `hostapd_s1g` (US op_class), confirm beaconing.
   Association leg needs a STA peer — flash an ESP32 with `rimba-halow-sta`, OR just confirm
   beaconing as the minimal gate.
5. **Then IBSS** (the actual rimba goal): diverge from the guide's AP/STA path to
   `iw dev wlanN ibss join rimba-ibss <freq> ... fixed-freq 02:12:34:56:78:9a` to match the
   existing 3-board cell (SSID `rimba-ibss`, BSSID `02:12:34:56:78:9a`, S1G ch27 / 1 MHz /
   op-class 68 / US, OPEN; IP `192.168.13.<octet(mac)>`). Needs `morse_cli`-driven S1G chan setup.

## Gotchas (community thread, this exact combo)
- First SPI bring-up wants a **cold boot / power-cycle** (warm reboot → `cmd63` SPI errors).
  Recovery if the tryboot pass hits cmd63 but kernel+network are fine: `promote-kernel.sh`
  then a cold power-cycle (safe now — boot+network already validated this pass).
- Brownout under load → `cmd53_write ret:-71`: use a solid 5 V supply.
- Overlay already disables kernel spidev on CE0/CE1 (else "chipselect 0 in use").
- For hostapd/mesh later: `nmcli dev set wlanN managed no` first.

## Pointers
- Full runbook: `~/halow/staging/POST-BUILD-RUNBOOK.md`
- Memory: `memory/halow-linux-node-hardware.md`, `memory/halow-linux-node-bringup-status.md`
- Kernel build log: `~/halow/kbuild.log`

---

## UPDATE 2026-06-19 (later) — bring-up DONE + promoted

Resumed from quartz (Armbian board) over SSH and drove the reboot + verification.

**Result: SUCCESS.** Warm `sudo reboot '0 tryboot'` booted `6.12.21-v8-16k+`; onboard
`wlan0`/SSH survived (brcmfmac=m). No `cmd63`/`cmd53` SPI errors (the warm-boot gotcha
did **not** trigger). dmesg:
```
morse_spi spi0.0: Loaded firmware from morse/mm6108.bin, size 481040, crc32 0xa4993663
morse_spi spi0.0: Loaded BCF from morse/bcf_fgh100mhaamd.bin, size 1251, crc32 0x941b2a82
```
- HaLow iface = **`wlan1`** (MAC `3c:22:7f:37:50:42`, Morse OUI), `iw dev` OK.
- **IBSS mode IS supported** (`iw phy` → Supported interface modes: IBSS, …) — good for rimba.
- Promoted via `promote-kernel.sh`: `config.txt` now boots the Morse kernel by default.
  Backups: `config.txt.pre-morse`, `config.txt.bak` (stock 6.18). Revert = restore `.bak` + reboot.

**Two gaps before the IBSS join (next session):**
1. **`morse_cli` is built without a transport** — runtime prints "No transports supported";
   can't talk to the chip. Needs a rebuild with the nl80211/transport backend. Required for
   S1G channel setup.
2. **S1G channel mapping.** `iw phy` exposes the radio as **5 GHz-mapped channels**
   (5180 MHz [36] …), not literal 900 MHz S1G. The IBSS join freq for our **ch27 / op_class 68
   / 1 MHz** must come from morse's S1G↔5 GHz mapping (via `morse_cli`), not "915.5". Resolve
   the mapped freq, then `iw dev wlan1 set type ibss` + join the rimba cell
   (SSID `rimba-ibss`, BSSID `02:12:34:56:78:9a`, OPEN, IP `192.168.13.<octet(mac)>`).

Note: `iw` lives in `/usr/sbin` (not in non-login SSH PATH); `morse_cli` in `/usr/local/bin`.

---

## UPDATE 2026-06-20 — version-matched + AP↔ESP32 interop PASSED

Driven from quartz over SSH. Reproducible recipe extracted to
`docs/design-specification/rimba-linux-node-setup.md`.

### morse_cli `-28` root cause: it was `sudo` all along (not version skew)
A long detour: `morse_cli -i wlan1 hw_version` returned `NL80211 code -28: Failed to
rcvmsgs`. Chased it as a version mismatch (rebuilt driver→1.17.8, firmware→1.17.8) —
**all wrong**. nl80211 vendor commands need **CAP_NET_ADMIN**; running as the
`chronium` user is rejected at the netlink layer. **`sudo morse_cli …` works:**
`HW Version: MM6108A1`, FW `rel_1_17_8`. Lesson recorded as Gotcha #1 in the setup doc.
(Also: the system-wide `morse_cli` was a stale no-transport build; replaced with the
`CONFIG_MORSE_TRANS_NL80211=1` build.)

### Stack now version-matched at 1.17.8
driver / firmware / morse_cli / hostapd_s1g all `rel_1_17_8_2026_Mar_24`; kernel
`6.12.21-v8-16k+` (`mm/rpi-6.12.21/1.17.x`). Got the 1.17.8 firmware blob from the
`morse-firmware` **`1.17` branch** commit `fd41e1c` (main only has 1.17.9; no tags).
Displaced 1.17.9 binaries backed up in `~/halow/staging/`. Git hashes in the setup doc §1.

### AP↔STA interop — PASSED ★ (P0.5-adjacent, the real milestone)
`hostapd_s1g` AP on `wlan1` (SSID `rimba-ping`, WPA3-SAE/CCMP, ch27/915.5 MHz/1 MHz/US,
IP 192.168.12.1) ↔ ESP32 `rimba-halow-sta` (ACM0):
- `AP-STA-CONNECTED 68:24:99:44:6b:b7` + `EAPOL-4WAY-HS-COMPLETED` (full WPA3-SAE handshake)
- station: signal −39 dBm, tx 21.7 / rx 72.2 Mbit/s, authorized
- bidirectional ping: ESP32→AP ~12 ms; AP→ESP32 3/3 0% loss ~10 ms
→ **our ESP32 port interoperates with the reference Linux morse_driver on-air.**

`hostapd` log also revealed the **S1G ch27 → 5 GHz ch 112 (5560 MHz)** mapping — the freq
to feed `iw` for the IBSS join. `hostapd` config fix: `s1g_prim_chwidth=0` (1 MHz), not 1.

### Bench state
Radio-silent: chronium hostapd stopped + `wlan1` down; ESP32 ACM0 reflashed `rimba-hello`.

### Next
IBSS interop (P0.5 proper): `sudo /usr/sbin/iw dev wlan1 set type ibss` + join
`rimba-ibss` on freq **5560 MHz** fixed-freq `02:12:34:56:78:9a` (OPEN), IP
`192.168.13.<octet(mac)>`. Framing compat is now de-risked by the AP-STA success.

---

## UPDATE 2026-06-20 (later) — IBSS interop run (P0.5 proper)

Joined the cell from Linux: `sudo iw dev wlan1 set type ibss; sudo iw dev wlan1 ibss
join rimba-ibss 5560 fixed-freq 02:12:34:56:78:9a` (5 GHz ch112 = S1G ch27; `morse_cli
channel` confirmed **915500 kHz / 1 MHz** — the `iw` join lands on the right S1G
channelization), IP `192.168.13.66`. ESP32 `rimba-halow-ibss` on ACM0.

**Result (details + scorecard in test plan §5):**
- **Data path interoperates** — Linux→ESP32 ping 3/3, 0% loss; the ESP32 receives
  Linux frames and replies. Linux correctly discovers the ESP32 (`iw station dump`).
- **Bug found (backlog #16)** — the ESP32 misparses Linux's S1G **beacons**, reading
  the TA from an offset in the TSF timestamp → floods its peer table with phantom
  peers, never records Linux's real MAC. ESP32-side fix; data path unaffected.

Radio-silent after: `iw dev wlan1 ibss leave` + `wlan1` down; ESP32 ACM0 → `rimba-hello`.

## UPDATE 2026-06-20 — #16 fixed, IBSS interop fully passing

Fixed the ESP32 S1G-beacon TA parse (`umac_datapath_rx_frame_filter` reads
`source_addr` for `(EXT, S1G_BEACON)` instead of `dot11_get_ta`'s addr2 offset, which
landed in the beacon timestamp). Re-ran against chronium: ESP32 forms **1 clean peer**
(`3c:22:7f`, 0 phantoms), discovers it passively from the beacon, and pings
bidirectionally (ESP32→Linux 30/30, Linux→ESP32 4/4 0% loss, ~12 ms). **I.1/I.2/I.3
pass.** Fix lives in the `mm-esp32-halow` submodule. Next: I.4 (frame diff, #11), I.5
(mixed 4-node cell). Radio-silent after.
