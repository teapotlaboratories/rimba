# Rimba Linux HaLow node — Raspberry Pi (5 / Zero 2 W) + MM6108 setup guide

Reproducible build/setup for the **Linux MM6108 reference/interop node** (`chronium`)
used by the open-IBSS test plan (P0.5). This is the canonical, from-scratch recipe;
the day-by-day narrative is in
[`worklog/2026-06-19-linux-node-halow-bringup.md`](../worklog/2026-06-19-linux-node-halow-bringup.md).

**Status: validated 2026-06-20** — ESP32 (`rimba-halow-sta`) ↔ this node
(`hostapd_s1g` AP) associated with WPA3-SAE over 802.11ah, bidirectional ping
(~12 ms RTT) on S1G ch27 / 915.5 MHz / 1 MHz / US.

---

## 0. Hardware

> **Pi Zero 2 W nodes (chronosalt / chronogen):** §0–§12 below are the Pi 5 recipe; the
> Zero-2-W deltas (kernel/DTB/overlay + a **critical `morse_driver` caveat**) are in the
> [Raspberry Pi Zero 2 W variant](#raspberry-pi-zero-2-w-variant-chronosalt--chronogen) section at the end.

- **Host:** Raspberry Pi 5 Model B (8 GB), Debian 13 (trixie), aarch64.
- **Radio:** Seeed **Wio-WM6108** (Quectel **FGH100M-H** / Morse Micro **MM6108**) in
  the Seeed **WM1302 Pi HAT**, connected **over SPI** (not PCIe/SDIO).
- **GPIO (RP1, `&spi0`):** reset=GPIO17 (active-low), power=GPIO23/24, spi-irq=GPIO5,
  CS0=GPIO8. BCF: `bcf_fgh100mhaamd.bin` (the "H" = FGH100M-H).
- `wlan0` = onboard Broadcom CYW43455 (brcmfmac) — keep it for SSH/remote; the HaLow
  radio enumerates separately as **`wlan1`** (MAC OUI `3c:22:7f`, Morse Micro).

## 1. Component versions — ALL 1.17.8 (must match)

> **Hard rule: driver, firmware, `morse_cli`, and hostap must be the SAME release.**
> A mismatch silently breaks things (we burned hours on a `morse_cli`/driver/firmware
> version chase — see Gotchas). The public repos are **misaligned**, so pin exactly:

| Component | Repo (github.com/MorseMicro/…) | Ref | Commit |
|---|---|---|---|
| Kernel | `rpi-linux.git` | branch `mm/rpi-6.12.21/1.17.x` | `372414fd42cdd4d8bfcf888cac62db9da947fdb6` |
| morse_driver | `morse_driver.git` | tag `1.17.8` | `3eef5a0a43645808e501ff4b83f29d675588bd9b` |
| MM6108 firmware | `morse-firmware.git` | branch `1.17` → **commit** | `fd41e1cffa7ab3cda88503f37e1cb05a3098be31` |
| morse_cli | `morse_cli.git` | tag `1.17.8` | `8f06222bee104327b5f09a9339f24bac1ef3420d` |
| hostap | `hostap.git` | tag `1.17.8` | `4acb6f6f46380d3c9fe50da77aa15a6ba565c49d` |

Resulting versions on the box: kernel `6.12.21-v8-16k+`; driver/cli/hostapd/fw all
`rel_1_17_8_2026_Mar_24`; libnl-3 `3.7.0`; iw `6.9`.

> **Firmware gotcha:** `morse-firmware` is a rolling repo — `main` only has the latest
> (1.17.9) and there are **no version tags**. The 1.17.8 `mm6108.bin` lives in the
> **`1.17` branch history** at commit `fd41e1c` ("Morse Micro 1.17.8 firmware (MM6108)").
> A shallow clone hides this. (1.17.6 — the ESP32's version — is also in that branch.)

## 2. Build prerequisites

```sh
sudo apt install -y bc bison flex libssl-dev libncurses-dev device-tree-compiler \
                    libnl-3-dev libnl-genl-3-dev libnl-route-3-dev libusb-1.0-0-dev \
                    pkg-config build-essential git iw
# NOTE: iw installs to /usr/sbin — not in a non-login SSH PATH; call it with a full
# PATH or /usr/sbin/iw.
```

Clone all repos at the pinned refs into `~/halow/`:

```sh
mkdir -p ~/halow && cd ~/halow
git clone -b mm/rpi-6.12.21/1.17.x https://github.com/MorseMicro/rpi-linux.git
git clone https://github.com/MorseMicro/morse_driver.git && git -C morse_driver checkout 1.17.8
git clone https://github.com/MorseMicro/morse_cli.git    && git -C morse_cli   checkout 1.17.8
git clone https://github.com/MorseMicro/hostap.git       && git -C hostap      checkout 1.17.8
# firmware: fetch the 1.17 branch (NOT just main) to reach the 1.17.8 blob
git clone https://github.com/MorseMicro/morse-firmware.git
git -C morse-firmware fetch origin 1.17:refs/remotes/origin/1.17
```

## 3. Kernel (Morse-patched 6.12.21)

**Why a custom kernel:** the out-of-tree `morse_driver` needs mac80211 S1G channel
flags (`IEEE80211_CHAN_{1,2,4,8}MHZ`) that exist only in Morse's patched tree. The
stock Pi `6.18` kernel **cannot build the driver** (flags undeclared). Morse's newest
branch is `mm/rpi-6.12.21/1.17.x` → build + boot **6.12.21** (a downgrade from 6.18).

```sh
cd ~/halow/rpi-linux
make bcm2712_defconfig
# Ensure: mac80211=m, cfg80211=m, CRC7=m, CRYPTO_CCM=y, CRYPTO_GCM=y, CRYPTO_AES=y,
# BRCMFMAC=m (so onboard wifi / SSH survives), and the S1G CHAN flags present.
make -j$(nproc) Image.gz modules dtbs            # module dir: 6.12.21-v8-16k+
sudo make modules_install
```

## 4. morse_driver (build against the new kernel)

```sh
cd ~/halow/morse_driver
make KERNEL_SRC=~/halow/rpi-linux \
     CONFIG_WLAN_VENDOR_MORSE=m CONFIG_MORSE_SPI=y \
     CONFIG_MORSE_USER_ACCESS=y CONFIG_MORSE_VENDOR_COMMAND=y
sudo cp morse.ko dot11ah/dot11ah.ko \
        /lib/modules/6.12.21-v8-16k+/kernel/drivers/net/wireless/morse/
sudo depmod 6.12.21-v8-16k+
```

## 5. morse_cli — **MUST build with the nl80211 transport**

```sh
cd ~/halow/morse_cli
make clean
make all CONFIG_MORSE_TRANS_NL80211=1        # <-- without this: "No transports supported"
sudo cp morse_cli /usr/local/bin/morse_cli
```

> A plain `make all` (or `make install`) builds **no transport** and every command
> prints `No transports supported`. The flag links libnl and the nl80211 vendor
> transport. (libnl-3-dev / libnl-genl-3-dev must be installed.)

## 6. hostapd_s1g (+ cli)

```sh
cd ~/halow/hostap/hostapd
# .config needs: CONFIG_DRIVER_NL80211=y, CONFIG_IEEE80211AH=y, CONFIG_SAE=y, CONFIG_DPP=y
make                                          # produces hostapd_s1g + hostapd_cli_s1g
sudo cp hostapd_s1g hostapd_cli_s1g /usr/local/bin/
```
(`wpa_supplicant_s1g` is deferred — its DPP code hits an OpenSSL-3 `EC_KEY`
deprecation-as-error; not needed for the AP test.)

## 7. Firmware + BCF + modprobe conf

```sh
sudo mkdir -p /lib/firmware/morse
# 1.17.8 firmware blob from the 1.17 branch history:
git -C ~/halow/morse-firmware show fd41e1c:firmware/mm6108.bin | sudo tee /lib/firmware/morse/mm6108.bin >/dev/null
sudo cp ~/halow/morse-firmware/bcf/bcf_fgh100mhaamd.bin /lib/firmware/morse/   # or your module's BCF
sudo ln -sf bcf_fgh100mhaamd.bin /lib/firmware/morse/bcf_default.bin
echo 'options morse bcf=bcf_fgh100mhaamd.bin country=US spi_clock_speed=20000000 enable_ps=0' \
     | sudo tee /etc/modprobe.d/morse.conf
```

## 8. Device-tree SPI overlay (Pi 5 / RP1)

`compatible = "morse,mm610x-spi"`, target `&spi0`, reset=GPIO17 (active-low),
power=GPIO23/24, spi-irq=GPIO5, CS0=GPIO8; the overlay also disables kernel spidev on
CE0/CE1. Compile and install:

```sh
dtc -@ -I dts -O dtb -o wm6108-spi-pi5.dtbo wm6108-spi-pi5.dts
sudo cp wm6108-spi-pi5.dtbo /boot/firmware/overlays/
```

## 9. Boot the Morse kernel (safe: tryboot → promote)

Install kernel + overlay under a separate name (default `config.txt` stays stock 6.18):

```sh
sudo cp ~/halow/rpi-linux/arch/arm64/boot/Image.gz /boot/firmware/kernel-morse.img
sudo cp ~/halow/rpi-linux/arch/arm64/boot/dts/broadcom/bcm2712-rpi-5-b.dtb /boot/firmware/
sudo cp /boot/firmware/config.txt /boot/firmware/config.txt.bak
# build initramfs-morse for 6.12.21 (root drivers ext4/mmc/sdhci-brcmstb =y)
```

`tryboot.txt` = `config.txt` + these lines (boots once, auto-reverts on cold power-cycle):
```
dtparam=spi=on
dtoverlay=wm6108-spi-pi5
kernel=kernel-morse.img
initramfs initramfs-morse followkernel
```
Then:
```sh
sudo reboot '0 tryboot'                        # boots Morse kernel ONCE
# verify (below); if good, promote to permanent default:
sudo cp /boot/firmware/config.txt /boot/firmware/config.txt.pre-morse
sudo cp /boot/firmware/tryboot.txt /boot/firmware/config.txt
# revert anytime: sudo cp /boot/firmware/config.txt.bak /boot/firmware/config.txt && sudo reboot
```

## 10. Verify bring-up

```sh
uname -r                                        # 6.12.21-v8-16k+
sudo dmesg | grep -iE 'Loaded firmware|Loaded BCF'   # mm6108.bin + bcf loaded, no cmd53 -71
ip -br link | grep 3c:22:7f                      # -> wlan1 (the HaLow radio)
sudo /usr/sbin/iw dev wlan1 info
sudo morse_cli -i wlan1 hw_version               # -> HW Version: MM6108A1   (SUDO REQUIRED)
sudo morse_cli -i wlan1 channel                  # -> Operating Frequency: 915500 kHz, BW 1 MHz
```

## 11. AP test (interop with ESP32 `rimba-halow-sta`) — VALIDATED

```sh
sudo nmcli dev set wlan1 managed no              # release from NetworkManager
sudo pkill -f 'wpa_supplicant.*wlan1'; sudo ip link set wlan1 down
sudo hostapd_s1g -dd /tmp/hostapd-rimba.conf > /tmp/hostapd.log 2>&1 &   # see config below
sudo ip addr add 192.168.12.1/24 dev wlan1       # AP static IP (STA self-assigns .2)
```

`/tmp/hostapd-rimba.conf` (matches the ESP32 `rimba-halow-ap`/`-sta` apps):
```
ctrl_interface=/var/run/hostapd_s1g
interface=wlan1
driver=nl80211
country_code=US
hw_mode=a
ieee80211ah=1
channel=27
op_class=68
s1g_prim_chwidth=0          # 0 = 1 MHz primary (op_class 68 is 1 MHz). NOT 1.
s1g_prim_1mhz_chan_index=0
ssid=rimba-ping
beacon_int=100
dtim_period=1
ap_max_inactivity=65536
s1g_capab=[SHORT-GI-ALL]
wpa=2
wpa_key_mgmt=SAE
rsn_pairwise=CCMP
sae_password=rimbahalow
ieee80211w=2
sae_pwe=2
wnm_sleep_mode=1
```
ESP32 side: `make flash APP=rimba-halow-sta BOARD=proto1-fgh100m PORT=/dev/ttyACM0`.
Expect `AP-STA-CONNECTED <mac>` + `EAPOL-4WAY-HS-COMPLETED` in `hostapd.log`, and
`reply from 192.168.12.1 … ~12 ms` on the ESP32 console.

## 12. IBSS interop (P0.5 / I.1–I.5) — VALIDATED

The open-IBSS interop path (this node as the `morse_driver`/mac80211 **reference IBSS
node** joining the ESP32 cell). Cell parameters must match every ESP32 node: SSID
`rimba-ibss`, **pinned** BSSID `02:12:34:56:78:9a` (no TSF merge either side — provisioned),
S1G ch27 / 915.5 MHz / 1 MHz / US, OPEN. IP = `192.168.13.<octet>`, octet = `mac[5]`.

**Method A — `iw` ad-hoc join (the validated path).** S1G ch27 maps to the 5 GHz-model
frequency **5560** (`dot11ah CHANS1GHZ(27,…,112)`; on-air 915.5 MHz):
```sh
sudo nmcli dev set wlan1 managed no
sudo iw dev wlan1 set type ibss; sudo ip link set wlan1 up
sudo iw dev wlan1 ibss join rimba-ibss 5560 fixed-freq 02:12:34:56:78:9a
sudo ip addr add 192.168.13.66/24 dev wlan1     # octet from this node's MAC, like the ESP32s
```
Because the ESP32 app auto-pings **every** discovered peer at `192.168.13.<octet>`, once this
node holds its octet IP the ESP32s start pinging it with no change — "just another peer."

**Method B — `wpa_supplicant_s1g` ad-hoc (`mode=1`), OPEN — fallback** if a given `iw`/driver
build rejects the S1G IBSS join (preserved from the old interop runbook; uses the same S1G
channel fields as the hostapd example). Needs `wpa_supplicant_s1g` from the Morse hostap fork:
```
ctrl_interface=/var/run/wpa_supplicant_s1g
network={
    ssid="rimba-ibss"
    mode=1
    bssid=02:12:34:56:78:9a
    frequency=915500            # or: channel=27 op_class=68 s1g_prim_chwidth=0 s1g_prim_1mhz_chan_index=0
    key_mgmt=NONE               # OPEN — Phase 1
}
```
```sh
wpa_supplicant_s1g -D nl80211 -i wlan1 -c wpa_ibss.conf
```

**Secured 802.11s mesh interop (chronite ↔ ESP, P3d).** Persisted at `chronite:~/wpa-interop.conf`
(copy to `/tmp` before use — `/tmp` is cleared on reboot):
```
ctrl_interface=/run/wpa_supplicant_s1g
country=US                       # GLOBAL — REQUIRED (enables the S1G channels)
sae_pwe=0                        # GLOBAL — H2E off, matches the ESP
sae_groups=19                    # GLOBAL
network={
    ssid="rimba-mesh"
    mode=5                       # WPAS_MODE_MESH
    key_mgmt=SAE
    sae_password="rimbamesh2026"
    country="US"
    op_class=68                  # S1G global op-class, ch27 / 1 MHz — NOT frequency=
    channel=27
    s1g_prim_chwidth=0
    s1g_prim_1mhz_chan_index=0
    dtim_period=1                # REQUIRED (mesh join bails / firmware rejects without it)
    beacon_int=100
}
```
```sh
cp ~/wpa-interop.conf /tmp/; sudo iw dev wlan1 set type managed; sudo ip link set wlan1 up
sudo wpa_supplicant_s1g -B -D nl80211 -i wlan1 -c /tmp/wpa-interop.conf -f /tmp/wpa-interop.log -dd -K
# expect "MESH-GROUP-STARTED", then: sudo ip addr add 10.9.9.2/24 dev wlan1
```
**⚠ Gotcha (cost a long debug):** a *wrong* channel form in the config (e.g. `frequency=5560` — the mapped
5 GHz freq, NOT the S1G channel) and/or omitting `country=US` makes the nl80211 mesh-join **hard-wedge the
morse chip** (it blocks trying to tune an un-enabled S1G channel → `rmmod morse` hangs uninterruptibly →
only a reboot recovers; it *looks* like a HW fault but is purely the config). Use `op_class=68 channel=27`
(or `frequency=915500`, the actual sub-GHz value), `country=US`, and `dtim_period=1`. Rapid
`wpa_supplicant_s1g` restarts can also wedge it — bring it up once and leave it.

Results (I.1 discovery, I.2 beacon interop, I.3 data, I.4 on-air, I.5 mixed 4-node cell) are
tabulated in [`rimba-ibss-test-plan.md`](../ibss/rimba-ibss-test-plan.md) §5. Key interop findings:
discovery is **data-driven** (the firmware doesn't surface peer beacons; morse S1G beacons
carry `source_addr = BSSID`), and I.4 on-air capture needs an external sniffer (morse monitor
mode isn't compiled into this build).

---

## Gotchas (each cost real time — read before debugging)

1. **`morse_cli` needs `sudo`.** nl80211 vendor commands require `CAP_NET_ADMIN`.
   Without root it fails with `NL80211, code -28: Failed to rcvmsgs` — which looks
   exactly like a version/ABI bug but is **just permissions**. Always `sudo morse_cli`.
2. **`morse_cli` transport flag** (§5): no `CONFIG_MORSE_TRANS_NL80211=1` → "No
   transports supported".
3. **Version lockstep** (§1): cli ↔ driver ↔ firmware ↔ hostap must all match. Public
   repos are misaligned (cli capped 1.17.8, firmware rolling 1.17.9-only); pull the
   1.17.8 firmware from the `1.17` branch commit `fd41e1c`.
4. **Stock 6.18 kernel can't build the driver** — needs the Morse 6.12.x patched tree.
5. **Warm reset wedges the chip.** A `modprobe -r/​modprobe` reload leaves the MM6108
   unresponsive (`cmd53 ... ret:-71`, MISO reads `0xffffffff`). A full **`sudo reboot`
   recovers it**; a cold power-cycle is the nuclear option. (A `reboot` does NOT
   power-cycle the chip but cycles reset enough.)
6. **SPI clock** (10 vs 20 MHz) made no difference to the issues we hit.
7. **Undervoltage** dmesg warnings appeared but were a red herring (Pi 5 wants 5 V/5 A
   PD specifically; comms were fine regardless).
8. **`hostapd` `s1g_prim_chwidth`**: use **0** for a 1 MHz channel (op_class 68); `1`
   means 2 MHz and errors with "primary channel BW cannot exceed operating BW".
9. **S1G ↔ 5 GHz channel mapping** (morse internal): **ch27 ↔ 5 GHz ch 112 (5560 MHz)**
   — `iw phy` shows the radio as 5 GHz channels; this is the freq for an `iw` IBSS join.
10. **`iw` lives in `/usr/sbin`** (not in a non-login SSH PATH); `morse_cli`/`hostapd_s1g`
    are in `/usr/local/bin`.

---

## Raspberry Pi Zero 2 W variant (chronosalt / chronogen)

The two Pi Zero 2 W nodes run the SAME MM6108 **1.17.8** stack, but as a **Pi Zero 2 W
(BCM2837), not a Pi 5** — so the kernel, DTB, and SPI overlay differ. Validated **2026-06-30**:
chronosalt + chronogen both bring up `wlan1` / MM6108A1 clean on boot. References: the Morse
community thread [Raspberry Pi Zero 2W with FGH100M](https://community.morsemicro.com/t/raspberry-pi-zero-2w-with-fgh100m/938)
(Aldwin's solution) + the [chip-reset discussion](https://community.morsemicro.com/t/how-to-use-both-the-mm6108-and-the-built-in-wi-fi-on-the-raspberry-pi-zero-2w/485).

**Radio:** FGH100M-H over SPI (Seeed XIAO HaLow module); BUSY/WAKE not bridged (module powered
continuously). Wiring, matching the overlay below: reset=GPIO5, power=GPIO3 + GPIO7,
spi-irq=GPIO25, CS0=GPIO8. `wlan0` = onboard CYW43455 (brcmfmac — keeps SSH alive under 6.12).

### Z1. Kernel — a Pi Zero 2 W (BCM2837) Morse kernel, cross-built on a Pi 5
Build the Morse `mm/rpi-6.12.21/1.17.x` tree with the **arm64 multi-platform / `bcm2711` config**
(covers Pi 2/3/Zero 2 W) — NOT `bcm2712_defconfig` (Pi 5). Kernel release is **`6.12.21-v8+`**
(the Pi 5 build is `-v8-16k+`). **512 MB RAM + one A53 die cannot build a kernel natively (OOMs)
— always cross-build on a Pi 5** (chronium/chronite, 8 GB) and copy the artifacts. On chronium
this is pre-built at `~/halow/rpi-linux-pi3/` (`Image.gz`, `bcm2710-rpi-zero-2-w.dtb`,
`pi3-modules.tar.gz`).

### Z2. morse_driver — DO **NOT** apply the forum's `spi.c` chip-reset on 1.17.8 ⚠️
The forum's "chip reset changes" are two edits: `hw.c` (`morse_hw_reset` delays 20→100 ms + a
1000 ms settle) and `spi.c` (a `morse_spi_reset()` call in `morse_spi_probe`). **On the 1.17.8
driver the `spi.c` reset is HARMFUL — it is the classic "chip won't respond" trap.** In 1.17.8
the detect step (which sets the func-address-base + reads `chip ID`) runs *before* `morse_of_probe`,
so a `morse_spi_reset()` added after `morse_of_probe` fires *after* detection and wipes the
address-base — the next register read (OTP `0x4120`) fails with `find_token failed` /
`cmd53 … ret:-71` / `OTP check failed`, and probe aborts (`-5`). It *looks* like a dead/silent
chip, but `insmod morse.ko … debug_mask=0xffffffff` proves the chip answered fine
(`Morse Micro SPI device found, chip ID=0x0306`) — the reset broke it. **Build with the stock
1.17.8 `spi.c` (no added reset).** The `hw.c` delay bump is inert (no reset runs in probe) so it's
harmless either way. If a unit genuinely needs a reset before detection, place it *before*
`morse_chip_cfg_detect_and_init`, never after `morse_of_probe`.

```sh
cd ~/halow/morse_driver          # stock 1.17.8 spi.c (no morse_spi_reset in probe)
make KERNEL_SRC=~/halow/rpi-linux-pi3 \
     CONFIG_WLAN_VENDOR_MORSE=m CONFIG_MORSE_SPI=y \
     CONFIG_MORSE_USER_ACCESS=y CONFIG_MORSE_VENDOR_COMMAND=y
modinfo -F vermagic morse.ko     # MUST read 6.12.21-v8+  (not -v8-16k+)
```

### Z3. Device-tree SPI overlay — MUST disable spidev on CE0 **and** CE1
MM6108 on `&spi0` CE0 (GPIO8). **CE1 = GPIO7, which the MM6108 uses as a power-gpio**, so a live
spidev on CE1 holds that pin (symptoms: `chipselect 0 already in use`, `/dev/spidev0.1`). The
overlay disables both spidev nodes. `wm6108-spi-pi02w.dts`:
```dts
/dts-v1/;
/plugin/;
/ {
	compatible = "brcm,bcm2835", "brcm,bcm2836", "brcm,bcm2708", "brcm,bcm2709", "brcm,bcm2711";
	fragment@0 {
		target = <&spi0>;
		__overlay__ {
			pinctrl-0 = <&spi0_pins &spi0_cs_pins>;
			cs-gpios = <&gpio 8 1>;
			#address-cells = <1>;
			#size-cells = <0>;
			status = "okay";
			mm6108@0 {
				compatible = "morse,mm610x-spi";
				reg = <0>;                        /* CE0 */
				reset-gpios = <&gpio 5 0>;
				power-gpios = <&gpio 3 0>, <&gpio 7 0>;
				spi-irq-gpios = <&gpio 25 0>;
				spi-max-frequency = <50000000>;
				status = "okay";
			};
		};
	};
	fragment@1 { target = <&spidev0>; __overlay__ { status = "disabled"; }; };
	fragment@2 { target = <&spidev1>; __overlay__ { status = "disabled"; }; };
};
```
```sh
dtc -@ -I dts -O dtb -o wm6108-spi-pi02w.dtbo wm6108-spi-pi02w.dts
```

### Z4. Firmware / BCF / modprobe
Same blobs as the Pi 5: `mm6108.bin` (1.17.8) + `bcf_fgh100mhaamd.bin` (the `-H` variant, md5
`4e128ad5…`). **SPI clock 20 MHz** (`spi_clock_speed=20000000`; 50 MHz fails per the forum):
```sh
echo 'options morse bcf=bcf_fgh100mhaamd.bin country=US spi_clock_speed=20000000 enable_ps=0' \
     | sudo tee /etc/modprobe.d/morse.conf
```

### Z5. Boot config (tryboot-safe) — NO `dtparam=spi=on`
The overlay enables `&spi0` itself, so `dtparam=spi=on` is redundant AND re-adds the conflicting
spidev — **leave it out.** Install the Zero-2-W kernel as `kernel-morse.img`, the Zero-2-W DTB
(under a distinct name), and the overlay; `config.txt` stays stock (auto-rollback fallback) while
`tryboot.txt` carries:
```
[all]
device_tree=bcm2710-rpi-zero-2-w-morse.dtb
dtoverlay=wm6108-spi-pi02w
kernel=kernel-morse.img
```
`sudo reboot '0 tryboot'` → verify → `sudo cp tryboot.txt config.txt` to promote.

### Z6. Verify
```sh
uname -r                                                  # 6.12.21-v8+
dmesg | grep -iE 'Loaded firmware|Loaded BCF|chip ID'     # fw + bcf, chip ID=0x0306, NO find_token/OTP errors
ip -br link | grep 68:24:99                               # wlan1 up
sudo morse_cli -i wlan1 hw_version                        # HW Version: MM6108A1
```

**Prebuilt bundle:** chronium `~/halow/pi02w-bundle*` (kernel, the fixed `morse.ko`/`dot11ah.ko`,
overlay, firmware, userspace binaries) + `pi02w-install.sh` — re-imaging a Zero 2 W is a
copy-and-boot, no rebuild.
