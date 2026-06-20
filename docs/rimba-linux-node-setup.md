# Rimba Linux HaLow node — Raspberry Pi 5 + MM6108 setup guide

Reproducible build/setup for the **Linux MM6108 reference/interop node** (`chronium`)
used by the open-IBSS test plan (P0.5). This is the canonical, from-scratch recipe;
the day-by-day narrative is in
[`worklog/2026-06-19-linux-node-halow-bringup.md`](worklog/2026-06-19-linux-node-halow-bringup.md).

**Status: validated 2026-06-20** — ESP32 (`rimba-halow-sta`) ↔ this node
(`hostapd_s1g` AP) associated with WPA3-SAE over 802.11ah, bidirectional ping
(~12 ms RTT) on S1G ch27 / 915.5 MHz / 1 MHz / US.

---

## 0. Hardware

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
