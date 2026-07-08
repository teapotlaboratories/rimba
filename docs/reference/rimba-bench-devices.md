# Rimba HaLow bench — device inventory (for agents)

Practical reference to the physical test bench: what's connected, how to reach each
device, and the gotchas. Stable facts (ports, MACs, addresses) are reliable; the
"currently running" notes are a snapshot — verify live before depending on them.

Last verified: 2026-06-29.

---

## At a glance

- **3× ESP32 HaLow nodes** (XIAO ESP32-S3 + FGH100M / MM6108) on the dev host's USB — chip fw **1.17.9**
  (from `vendor/morse-firmware`; the "1.17.6" in older notes was the unused SDK mbin).
- **4× Linux HaLow nodes** on the LAN — **ALL Morse components at 1.17.9** (driver
  `rel_1_17_9_2026_Apr_20`, fw `mm6108.bin` 481040, dot11ah + `morse_cli`/`hostapd_s1g`/`wpa_supplicant_s1g`;
  BCF unchanged). **Re-deployed 2026-07-07** — the nodes had been downgraded to 1.17.8 for the mesh-gate test.
  **The Pi 5 nodes (chronium, chronite) carry the `hw.c` gpiod reset patch**
  (`docs/reference/patches/morse-driver-pi5-reset-gpiod.patch`) — it is **NEEDED, not dormant**: stock
  `morse_hw_reset` fails on the Pi 5 RP1 controller, so a driver reload wedges the chip; the patch makes
  `modprobe -r morse; modprobe morse` reset + re-probe cleanly with **no reboot** (the Pi Zeros reset fine on
  stock GPIO5, so they run unpatched 1.17.9). Whole bench (ESP + Linux) matches at **1.17.9** — see
  `rimba-linux-node-setup.md §1` for the per-arch build/deploy recipe (build on chronium; Pi 5 native
  `v8-16k+`, Pi Zero cross-built vs `rpi-linux-pi3` `v8+`; `CONFIG_WLAN_VENDOR_MORSE=m` +
  `CONFIG_MORSE_SPI/VENDOR_COMMAND/USER_ACCESS/MONITOR`). Steady-state roles:
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
- **1× ESP32-C6-DevKitC-1** — measurement-harness companion for board2 (`/dev/ttyUSB0`, target `esp32c6`):
  drives the trigger pin / reads phase markers over a single wire, **GPIO20 → board2 D5 + GND** (link
  verified 2026-07-07). See the **Measurement harness** section below.

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

### board2 won't flash ("No serial data received") — un-wedge it

A **sleep/deep-sleep app** on board2 (e.g. a host-light-sleep or deep-sleep PS ladder) powers the
ESP32-S3 **native USB down and re-enumerates it constantly**, so esptool's reset can never land it in
download mode — every flash attempt fails **`No serial data received`**, on any port, and a plain
monitor-kill "power-cycle" does **not** fix it. This blocked the whole bench for a session; the recovery:

**Run `python ~/pwr_test/reflash_hello.py`** (IDF python env). It does a **genuine PPK2 power-cycle**
(`toggle_DUT_power("OFF")` for 2.5 s → `ON`), which cold-boots board2 with a **clean USB**; esptool's reset
then works in the fresh-boot window. It **auto-detects board2's re-enumerated port** (the `303a` device that
appears when power comes on), flashes rimba-hello, and leaves board2 powered + silent. **Verified 2026-07-07:
recovered a fully-wedged board2 remotely — no physical BOOT button needed.**

Two gotchas the script encodes (and you must respect if doing it by hand):
- **Killing the PPK2 monitor does NOT cut board2's power** — the DUT rail latches ON. Only
  `ppk2.toggle_DUT_power("OFF")` (via `ppk2-api`) truly de-powers it. A real OFF→(2.5 s)→ON is the whole trick.
- **Never hardcode board2's `ttyACM`** — it re-enumerates on every power cycle; detect the ESP32-S3
  (VID `303a`) that appears *after* you power it on.

**Prevention:** measurement firmware should **never auto-run into sleep**. Boot into a host-AWAKE idle and
start the ladder only on a **trigger** (GPIO6 / pad D5), ending host-awake — a fresh boot then keeps the USB
enumerated and board2 stays reflashable. That's what `rimba-halow-sta`'s triggered ladder does.

**Flash-hold guard (the deterministic escape hatch, in `rimba-halow-sta`).** The fw reads **D5/GPIO6 at boot
(pull-DOWN)** *before any radio/NVS init*: if **D5 is HIGH → it sits in an infinite host-awake idle** and
never runs the app; if **LOW (default) → it runs normally**. So to recover a board2 stuck in *any* bad fw,
**drive D5 HIGH (from the C6 GPIO20, or a jumper D5→3V3) and power-cycle** — it boots straight into a
guaranteed flashable idle, no fresh-boot-window race, no physical BOOT. `D5 HIGH` is the *special* hold
state; the pull-down keeps the default = run. (Flip to pull-up if you want boot-into-hold as the safe
default.) Pairs with `reflash_hello.py`: assert D5 HIGH → power-cycle → clean flash every time.
**Verified on hardware 2026-07-07** via the C6 harness: D5 HIGH→FLASH-HOLD, LOW→run, and a C6 trigger pulse →
the full P1–P4 ladder, back to idle.

---

## Measurement harness — ESP32-C6-DevKitC-1 (board2 trigger / phase companion)

An **ESP32-C6-DevKitC-1** on the dev host's USB (**`/dev/ttyUSB0`**, its CP210x bridge; build target
**`esp32c6`**) is the digital companion for board2's PPK2 power measurements. The PPK2 measures current but
its 8 logic pins are **input-only**; the C6 can **drive** a pin, so it supplies the *trigger* that starts
board2's ladder (and can timestamp the *phase markers* board2 pulses back). The PPK2 still does the power
measurement — the C6 is the control/digital side, in parallel on the same DUT.

**Wiring — one signal pin + ground, nothing else** (board2 is powered by the PPK2, NOT the C6):

| C6 | board2 | note |
|---|---|---|
| **GPIO20** | **D5 / GPIO6** | the single free XIAO pad — MM6108 uses D0–D4 + D8–D10 |
| **GND** | **GND** | common with the PPK2 ground — mandatory, or logic levels are meaningless |

No UART, no reset/BOOT wires, no power line. **Link verified 2026-07-07:** C6 toggling GPIO20 at 1 Hz →
board2 (monitoring D5) tracked it 1:1, **11 clean edges in 10 s**. So the trigger direction (C6 drives →
board2 reads) is proven end-to-end.

**Measurement-integrity rules** (board2's rail is PPK2-measured — don't inject current into it):
- Drive GPIO20 only to **trigger** (a brief low pulse) *before/between* measurement windows; during the
  actual current capture, **tri-state** it (set the C6 pin to input) so it injects nothing.
- **Common GND mandatory**; both sides 3.3 V (no level shifting); the C6 supplies **no power** to board2.

**Build / flash the C6** — the project lives at **`firmware/c6-harness/`** (standalone IDF; the repo `make`
is S3-only). Its `MODE` selects **TRIGGER** / **HOLD_HIGH** (force board2 flash-hold) / **HOLD_LOW** /
**TOGGLE** (link test):
```sh
cd firmware/c6-harness
export IDF_PATH=<repo>/vendor/esp-idf && source $IDF_PATH/export.sh
idf.py set-target esp32c6 build && idf.py -p /dev/ttyUSB0 flash monitor   # C6 = CP210x on ttyUSB0
```

**board2 free XIAO pads** (not used by the FGH100M): **D5/GPIO6** (the harness pin — RTC-capable, so it can
be latched through sleep), **D6/GPIO43** + **D7/GPIO44** (UART0 TX/RX — free because the console is on the
native USB; usable for an external-UART flash path if ever needed).

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
