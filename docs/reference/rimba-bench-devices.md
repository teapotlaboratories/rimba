# Rimba HaLow bench — device inventory (for agents)

Practical reference to the physical test bench: what's connected, how to reach each
device, and the gotchas. Stable facts (ports, MACs, addresses) are reliable; the
"currently running" notes are a snapshot — verify live before depending on them.

Last verified: 2026-06-25.

---

## At a glance

- **3× ESP32 HaLow nodes** (XIAO ESP32-S3 + FGH100M / MM6108) on the dev host's USB.
- **2× Linux HaLow nodes** (Raspberry Pi 5 + MM6108 HAT) on the LAN — both fully set up,
  **same 1.17.8 morse stack**. Both can run a real morse mesh and **peer with each other**
  (chronium↔chronite plink ESTAB, ping 0% loss — the working 802.11s reference).
  - **chronium** `192.168.7.187` — `CONFIG_MORSE_MONITOR=y` build (the `morse0` raw tap is
    currently broken / 0 frames; observe via the normal vif `station dump`/`mpath` instead).
  - **chronite** `192.168.7.191` — the **Linux testing + code-comparison** node (run mesh /
    IBSS / AP / STA here; its `~/halow` source trees are the reference for comparing against
    the ESP morselib). Set up 2026-06-25 by cloning chronium's stack.
- **Dev host** — the machine these run from: holds the `rimba` repo + ESP-IDF
  toolchain; all builds/flashing happen here; SSH to the Pis over the LAN.

---

## ESP32 nodes (3×)

XIAO ESP32-S3 + FGH100M (MM6108). `BOARD=proto1-fgh100m` for all of them.

| Board | Serial port | ESP32 efuse serial | Mesh MAC (derived) |
|---|---|---|---|
| board0 | `/dev/ttyACM0` | `E0:72:A1:F8:EF:A4` | `e2:72:a1:f8:ef:a4` |
| board1 | `/dev/ttyACM1` | `E0:72:A1:F8:F9:40` | `e2:72:a1:f8:f9:40` |
| board2 | `/dev/ttyACM2` | `E0:72:A1:F8:F0:08` | `e2:72:a1:f8:f0:08` |

**Important:** all three MM6108 modules share the **same factory MAC
`bc:2a:33:96:b2:9f`**. A receiver drops frames whose source == its own MAC, so every
self-beaconing app must set a **unique locally-administered MAC** — we derive it from
each ESP32's efuse MAC (`esp_read_mac(.., ESP_MAC_WIFI_STA)` then `mac[0] |= 0x02`),
which gives the "Mesh MAC" column above.

### Build / flash / console
```sh
# from the repo root, on the dev host:
make build   APP=<app> BOARD=proto1-fgh100m
make flash   APP=<app> BOARD=proto1-fgh100m PORT=/dev/ttyACM0
make monitor APP=<app> BOARD=proto1-fgh100m PORT=/dev/ttyACM0   # idf serial console
```
Apps live in `firmware/<app>/`. Relevant ones: `rimba-halow-mesh`,
`rimba-halow-mesh-monitor`, `rimba-halow-ibss`, `rimba-halow-ap`, `rimba-halow-sta`,
`rimba-halow-scan`.

Non-interactive serial capture (when `make monitor` would block): reset with esptool
then read the port with a short pyserial loop, e.g.
```python
import serial, time
s = serial.Serial('/dev/ttyACM0', 115200, timeout=1); s.dtr=True; s.rts=False
t0=time.time()
while time.time()-t0 < 12:
    l = s.readline()
    if l: print(l.decode('utf-8','replace').rstrip())
s.close()
```
`make flash` sometimes fails on a busy port — just retry. A reset helper:
`python vendor/esp-idf/components/esptool_py/esptool/esptool.py --port <p> --after hard_reset --no-stub chip_id`.

---

## Linux nodes (2×) — Raspberry Pi 5 + MM6108 HAT

Both use **SSH key auth** from the dev host (no password needed). chronium can also SSH
directly to chronite (key installed for direct rsync):
```sh
ssh chronium@192.168.7.187     # monitor / sniffer node
ssh chronite@192.168.7.191     # testing / code-comparison node
```

**Gotchas that bite every session (prepend to remote commands):**
- Locale is broken → `export LC_ALL=C` (otherwise noisy `setlocale` warnings).
- `iw`, `morse_cli`, `hostapd_s1g` are **not on the default non-login PATH** →
  `export PATH=$PATH:/usr/sbin:/usr/local/bin`.
- `morse_cli` and most `iw` ops need **`sudo`** (CAP_NET_ADMIN).
- `iw … ibss join` can **wedge the morse driver** (SSH dies) — historically tied to
  undervoltage; recover with a reboot. `iw … mesh join` does **not** wedge it.
- **morse Linux mesh is started by `wpa_supplicant_s1g`, not `iw … mesh join`** — bare
  `iw mesh join` never starts the firmware mesh BSS, so it neither beacons nor enables RX
  (looks like "deaf"/broken RX but isn't). Recipe + config in the worklog below.
- Reloading the module (`modprobe -r morse`) **unbinds the SPI device** (bind → I/O
  error) — to load a rebuilt module, **reboot** rather than reload.

### chronium — `192.168.7.187` ✅
- Kernel **6.12.21-v8-16k+** (Morse-patched tree at `~/halow/rpi-linux`).
- morse device = **phy1 / wlan1** (`morse_spi`, SPI); phy0/wlan0 = the Pi's brcmfmac.
  HaLow Wi-Fi MAC `3c:22:7f:37:50:42`. Reachable on **wired `eth0`** (primary) + wlan0.
- `morse_driver` rebuilt + installed with **`CONFIG_MORSE_MONITOR=y`** (2026-06-26), so the
  `morse0` netdev exists. Source `~/halow/morse_driver`.
- **The `morse0` raw monitor tap currently delivers 0 frames** (verified 2026-06-26 with an
  active mesh on-channel) — do NOT rely on it as a sniffer. Whatever worked once doesn't now;
  treat raw-tap capture as broken/unreliable. NB the monitor build does **not** break normal
  operation: chronium peers + passes traffic over mesh fine on this build (see below).
- For mesh/peer observation use the **normal vif** instead: `iw dev wlan1 station dump`
  (plink state, signal), `iw dev wlan1 mpath dump`, `page_stats` (Beacon Tx). Both Pis can run
  a real morse mesh and peer with each other — that's the working reference, not morse0.

### chronite — `192.168.7.191` (the Linux TESTING / code-comparison node) ✅
Set up 2026-06-25 by cloning chronium's stack (faster than a from-scratch kernel build,
since the hardware is identical). Now a full second morse node, same 1.17.8 stack.
- Kernel **6.12.21-v8-16k+** (the same Morse-patched kernel — cloned from chronium and
  made the default boot, see "Boot" below).
- morse device = **wlan1**, HaLow MAC **`3c:22:7f:37:51:38`** (distinct from chronium — good
  for interop). `morse_cli -i wlan1 hw_version` → `MM6108A1`. firmware `mm6108.bin` + BCF
  `bcf_fgh100mhaamd.bin` load on boot.
- **`~/halow` source trees present** (rpi-linux, morse_driver, morse_cli, hostap,
  morse-firmware) — this is the **reference for code comparison** vs the ESP morselib, and
  lets you rebuild the driver on-box. Binaries installed: `morse_cli`, `hostapd_s1g`,
  `hostapd_cli_s1g`, `wpa_supplicant_s1g`.
- Its cloned driver also has `CONFIG_MORSE_MONITOR` (so `morse0` exists here too), but by
  convention **chronium is the sniffer**; run mesh/IBSS/AP/STA tests on chronite.
- **Reachable on WiFi `wlan0` only** (no wired `eth0` cable; netplan profile). So a kernel
  swap is the one risky op — if wlan0 doesn't reconnect you lose access until a power-cycle.
  In practice it reconnects on its own in ~45–90 s after a reboot (WiFi timing varies).
- **Boot:** the morse kernel is the persistent default (`/boot/firmware/config.txt` carries
  `dtoverlay=wm6108-spi-pi5` + `kernel=kernel-morse.img` + `initramfs initramfs-morse`).
  Backups: `config.txt.bak` (stock 6.18) and `config.txt.pre-morse`. To test a kernel change
  safely, use **tryboot** (one-shot, firmware auto-reverts on cold power-cycle):
  `sudo systemctl reboot --reboot-argument="0 tryboot"` boots `/boot/firmware/tryboot.txt`
  once — NOTE the plain `sudo reboot "0 tryboot"` from the setup doc does **not** pass the
  argument under systemd; use `--reboot-argument`. Promote with `cp tryboot.txt config.txt`.

---

## Radio / channel reference
- Link params: **S1G channel 27**, **915.5 MHz**, **1 MHz BW**, US, global op-class 68.
- In the Linux morse/dot11ah "5 GHz model": **ch27 ↔ 5 GHz ch112 ↔ freq `5560`** — use
  `5560` for `iw … freq` on the Pis. On-air is 915.5 MHz / 1 MHz.

## Related docs
- `docs/reference/rimba-linux-node-setup.md` — full Linux node build (kernel, driver, cli).
- `docs/worklog/2026-06-25-mesh-p1-vif-beacon.md` — ESP mesh P1 (vif up + firmware beacon).
- `docs/worklog/2026-06-26-linux-mesh-reference.md` — **working Linux mesh reference**
  (wpa_supplicant_s1g recipe, chronium↔chronite peer + ping, the two fork pitfalls).
- `docs/ibss/rimba-ibss-milestones.md` — IBSS milestones + TODO backlog.
