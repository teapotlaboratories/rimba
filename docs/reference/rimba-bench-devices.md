# Rimba HaLow bench — device inventory (for agents)

Practical reference to the physical test bench: what's connected, how to reach each
device, and the gotchas. Stable facts (ports, MACs, addresses) are reliable; the
"currently running" notes are a snapshot — verify live before depending on them.

Last verified: 2026-06-29.

---

## At a glance

- **3× ESP32 HaLow nodes** (XIAO ESP32-S3 + FGH100M / MM6108) on the dev host's USB.
- **4× Linux HaLow nodes** on the LAN, **all 1.17.8 morse stack**. Steady-state roles:
  **chronium = dedicated on-air monitor/sniffer**; the **other 3 form a live SAE+AMPE-encrypted
  802.11s mesh** (`rimba-smesh`, ch27/915.5) — plink ESTAB, encrypted ping 0% loss, mesh IPs
  `10.9.9.2`–`10.9.9.4`. (chronium *can* rejoin as `10.9.9.1` if you need a 4-node mesh instead;
  monitor and mesh can't run on one radio at the same time.)
  - **2× Raspberry Pi 5 + MM6108 HAT** — `wm6108-spi-pi5` wiring:
    - **chronium** — **the sniffer** (`CONFIG_MORSE_MONITOR=y`; `wlan1` in monitor
      type on ch27 → `morse0` raw tap delivers radiotap frames; see the Monitor section). Captures
      S1G beacons + SAE auth + AMPE action + data off-air — the gold standard for the ESP port.
    - **chronite** (mesh `10.9.9.2`) — **testing + code-comparison** node (its
      `~/halow` source trees are the reference for diffing against the ESP morselib).
  - **2× Raspberry Pi Zero 2 W + `fgh100mhaamd` MM6108 board** — `fgh100mhaamd-spi` wiring,
    built 2026-06-27, both power-stable (`throttled=0x0`):
    - **chronosalt** (mesh `10.9.9.3`) — distant node for the airtime test.
    - **chronogen** (mesh `10.9.9.4`) — distant node for the airtime test.
- **Dev host** — the machine these run from: holds the `rimba` repo + ESP-IDF
  toolchain; all builds/flashing happen here; SSH to the Pis over the LAN.

---

## ESP32 nodes (3×)

XIAO ESP32-S3 + FGH100M (MM6108). `BOARD=proto1-fgh100m` for all of them.

| Board | Serial port (volatile!) | ESP32 efuse serial | Mesh MAC (derived) | Mesh IP |
|---|---|---|---|---|
| board0 | `/dev/ttyACM0` | `E0:72:A1:F8:EF:A4` | `e2:72:a1:f8:ef:a4` | `10.9.9.136` |
| board1 | `/dev/ttyACM1` | `E0:72:A1:F8:F9:40` | `e2:72:a1:f8:f9:40` | `10.9.9.100` |
| board2 | `/dev/ttyACM4` | `E0:72:A1:F8:F0:08` | `e2:72:a1:f8:f0:08` | `10.9.9.108` |

**⚠ The `/dev/ttyACM*` numbers RE-ENUMERATE** (USB hotplug order) — the column above is a 2026-06-28
snapshot. **Always identify a board by its efuse MAC, not the ACM number:**
`python vendor/esp-idf/components/esptool_py/esptool/esptool.py --port /dev/ttyACMx --after hard_reset
read_mac` (Espressif VID = `303a`). board2 is currently on **`ttyACM4`** (was `ttyACM2`); `ttyACM2`/`ttyACM3`
are now the **PPK2** (see below). Mesh IP = `10.9.9.{100 + (mesh_mac[5] & 0x3f)}` in app code.

**nRF PPK2 (Nordic `1915:c00a`, `/dev/ttyACM2` + `/dev/ttyACM3`)** powers **board2's DUT rail at 5 V** in
**ampere-meter mode, DUT ON** (held by `/tmp/ppk2_hold.py`; the `ppk2-api` pip pkg is in the IDF python env).
Clean power-cycle of board2 = toggle DUT OFF→ON in ampere-meter mode (**no source voltage to set** → no
wrong-voltage risk). `fuser -k` on the PPK2 port cuts board2's power — identify by VID:PID first.

**chronium RF range (2026-06-28):** chronium's `morse0` monitor now sees **all three ESP boards** on ch27
(board0/1/2 each ~300+ frames in 25 s) — the earlier "board0 out of chronium range" note no longer holds, so
ESP relay/forward frames are byte-capturable for the on-air gold standard.

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

**Reach every Linux node by hostname, never by raw IP** (see `.ai/AGENTS.md` → Bench access).
`~/.ssh/config` on the dev host maps each hostname to its node (SSH key auth, no password):
```sh
ssh chronium      # monitor / sniffer node
ssh chronite      # testing / code-comparison node
ssh chronosalt    # Pi Zero twin
ssh chronogen     # Pi Zero twin
```
The names also work as `chronium.local` (mDNS). The management IPs live **only** in `~/.ssh/config`
(one place to update if DHCP moves them — `avahi-resolve -4 -n chronium.local` re-discovers a current
address). chronium can also SSH directly to chronite (key installed for direct rsync).

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

### chronium ✅
- Kernel **6.12.21-v8-16k+** (Morse-patched tree at `~/halow/rpi-linux`).
- morse device = **phy1 / wlan1** (`morse_spi`, SPI); phy0/wlan0 = the Pi's brcmfmac.
  HaLow Wi-Fi MAC `3c:22:7f:37:50:42`. Reachable on **wired `eth0`** (primary) + wlan0.
- `morse_driver` rebuilt + installed with **`CONFIG_MORSE_MONITOR=y`** (2026-06-26), so the
  `morse0` netdev exists. Source `~/halow/morse_driver`.
- **`morse0` WORKS as a sniffer — it is NOT broken** (re-confirmed 2026-06-27: ~1100 frames in
  32 s, including S1G beacons + SAE auth + AMPE mesh-peering action + data, captured off the live
  mesh). The earlier "0 frames" note was a capture attempted with `wlan1` in mesh type — the driver
  only forwards to `morse0` when `mors->monitor_mode` is true, i.e. **only when `wlan1` is set to
  monitor type** (`mac.c:morse_mac_skb_recv` / `mac.c:3901`). Set monitor mode first (see the
  Monitor section), then capture on `morse0`.
- **`tcpdump` installed** (`/usr/bin/tcpdump` 4.99.5, added 2026-06-27): `sudo tcpdump -i morse0 -e -nn`
  shows radiotap (tsft/MCS/freq/signal) — handy for per-node signal (the 3 mesh nodes show distinct
  dBm). It prints S1G frames as "unknown 802.11 frame type (3)" (no S1G body decode), so for the
  SAE/AMPE bytes use `sudo tcpdump -i morse0 -w cap.pcap` and open in Wireshark (decodes S1G + mesh
  action + SAE). A Python AF_PACKET reader on `morse0` also works for quick frame-type counts.
- For mesh/peer *state* (when not monitoring) use the normal vif: `iw dev wlan1 station dump`
  (plink/signal), `iw dev wlan1 mpath dump`, `page_stats` (Beacon Tx).

### chronite — the Linux TESTING / code-comparison node ✅
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

## Pi Zero 2 W nodes (2×) — `fgh100mhaamd` MM6108 board

**chronosalt** (user `chronosalt`) and **chronogen** (user `chronogen`), both pass `hermanudin`,
SSH key auth + **passwordless sudo**, fresh Debian 13
(trixie). Built 2026-06-27 as identical twins. (chronosalt replaced a retired flaky RPi-3B rig;
chronogen was the old `rpi-linkarta-1`, reflashed.) `iw` is installed but in `/usr/sbin` — same
PATH gotcha as the Pi 5s.

- **Same `6.12.21-v8+` morse kernel + 1.17.8 stack as the Pi 5s** (the Pi Zero 2 W is the same
  BCM2710/aarch64 family, so the kernel + `morse.ko` built for the old Pi-3 chronosalt are reused
  verbatim; the 1.17.8 `morse_cli`/`wpa_supplicant_s1g`/`hostapd_s1g` are copied from chronium and
  run fine on Debian 13). morse device = **wlan1**; SSH rides **wlan0** (brcmfmac43430).
- HaLow MACs: chronosalt `68:24:99:44:6a:56`, chronogen `68:24:99:44:6b:80`. firmware `mm6108.bin`
  (crc32 `0x9edcc720`) + `bcf_fgh100mhaamd.bin` — byte-identical to the Pi 5s.
- **Reliable, unlike the old RPi-3B chronosalt**: chip detects clean first-try @ 10 MHz SPI,
  `vcgencmd get_throttled` = `0x0`. The `fgh100mhaamd` board has on-board pull-ups, so the overlay
  needs **no `output-high` patch** and the micro-USB power holds under the MM6108's TX load.
- **`fgh100mhaamd-spi` overlay wiring** (BCM GPIO ↔ MM6108; *different from the Pi-5 HAT*):
  reset=**GPIO5** (pin 29), wake=**GPIO3** (pin 5), busy=**GPIO7** (pin 26), spi-irq=**GPIO25**
  (pin 22), CS0=**GPIO8** (pin 24), SPI SCLK/MOSI/MISO = GPIO11/10/9 (pins 23/19/21), + 3V3/GND.
  The overlay self-enables spi0 (no `dtparam=spi=on`). `.dtbo` lives in `/boot/firmware/overlays/`.
- **Boot:** morse kernel is the promoted default (`config.txt` carries `kernel=kernel-morse.img` +
  `dtoverlay=fgh100mhaamd-spi`; backup `config.txt.pre-morse` is stock 6.18). Test changes with
  one-shot tryboot: `sudo reboot '0 tryboot'` (plain form works here — these are not systemd's
  `--reboot-argument` case like chronite; it boots `tryboot.txt` once).

---

## Secured mesh — `rimba-smesh` (SAE + AMPE, the working reference)

The live 4-node encrypted mesh (Phase-0 reference for the mesh-security port). All nodes run the
**same** `wpa_supplicant_s1g` config (only the mesh IP differs). Config + full handshake capture:
`docs/reference/captures/wpa-smesh.conf` + `…/secured-mesh-sequence.txt`; details in
`docs/worklog/2026-06-27-linux-secured-mesh-reference.md`.

Bring-up recipe (per node). Kill the daemon with **`pkill -9 wpa_supplicant`** (substring match on
`comm`): `pkill -x wpa_supplicant_s1g` silently 0-matches (the `comm` is truncated to 15 chars,
`wpa_supplicant_`), and `pkill -f wpa_supplicant_s1g` self-kills the launching ssh.
```sh
export LC_ALL=C; export PATH=$PATH:/usr/sbin:/usr/local/bin
# /tmp/wpa-smesh.conf = the open-mesh config with key_mgmt=SAE + sae_password="rimbamesh2026"
sudo iw dev wlan1 mesh leave 2>/dev/null; sudo pkill -9 wpa_supplicant; sleep 1
sudo rm -f /var/run/wpa_supplicant_s1g/wlan1
sudo ip link set wlan1 down; sudo iw dev wlan1 set type managed; sudo ip link set wlan1 up
sudo wpa_supplicant_s1g -B -D nl80211 -i wlan1 -c /tmp/wpa-smesh.conf -f /tmp/wpa-smesh.log -dd
sudo ip addr add 10.9.9.<N>/24 dev wlan1     # N = 1 chronium / 2 chronite / 3 chronosalt / 4 chronogen
```
Verify: `sudo grep MESH-PEER-CONNECTED /tmp/wpa-smesh.log`, `sudo iw dev wlan1 station dump | grep ESTAB`.
**Runtime-only** — the kernel boot is permanent but the mesh JOIN is not a service; re-run after reboot.

---

## Monitor / sniffer (chronium) — capture HaLow frames off-air

chronium is the dedicated sniffer (the bench's only working HaLow monitor). `morse0` only delivers
frames while `wlan1` is in **monitor** type, and monitor ≠ mesh on one radio, so chronium can't be
a mesh member and a sniffer at once. To put chronium into monitor mode:
```sh
export LC_ALL=C; export PATH=$PATH:/usr/sbin:/usr/local/bin
sudo pkill -9 wpa_supplicant; sudo iw dev wlan1 mesh leave 2>/dev/null   # free the radio
sudo ip link set wlan1 down; sudo iw dev wlan1 set type monitor; sudo ip link set wlan1 up
sudo iw dev wlan1 set freq 5560                  # S1G ch27 (915.5 MHz on-air)
sudo ip link set morse0 up
# capture: tcpdump (installed) — live view or pcap for Wireshark:
sudo tcpdump -i morse0 -e -nn                 # live, with radiotap (signal/MCS/freq)
sudo tcpdump -i morse0 -w /tmp/cap.pcap       # pcap -> Wireshark decodes S1G + SAE/AMPE/action
```
Verify tuned: `iw dev wlan1 info` → `type monitor` + `channel 112 (5560 MHz)`. To return chronium to
the mesh, run the mesh bring-up recipe above (it resets `wlan1` to mesh type). Full recipe + a richer
decoder: `docs/reference/rimba-linux-halow-monitor.md`.

---

## Radio / channel reference
- Link params: **S1G channel 27**, **915.5 MHz**, **1 MHz BW**, US, global op-class 68.
- In the Linux morse/dot11ah "5 GHz model": **ch27 ↔ 5 GHz ch112 ↔ freq `5560`** — use
  `5560` for `iw … freq` on the Pis. On-air is 915.5 MHz / 1 MHz.

## Related docs
- `docs/reference/rimba-linux-node-setup.md` — full Linux node build (kernel, driver, cli).
- `docs/worklog/2026-06-25-mesh-p1-vif-beacon.md` — ESP mesh P1 (vif up + firmware beacon).
- `docs/worklog/2026-06-26-linux-mesh-reference.md` — **working OPEN Linux mesh reference**
  (wpa_supplicant_s1g recipe, chronium↔chronite peer + ping, the two fork pitfalls).
- `docs/worklog/2026-06-27-linux-secured-mesh-reference.md` — **SECURED (SAE+AMPE) mesh reference**
  (the Phase-0 gold standard for the mesh-security port: SAE/AMPE/key-install sequence).
- `docs/ibss/rimba-ibss-milestones.md` — IBSS milestones + TODO backlog.
