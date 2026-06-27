# Sniffing HaLow (S1G) raw frames with a Linux morse node

How to turn a Raspberry Pi + MM6108 (morse) node into a working **802.11ah / HaLow raw
packet monitor** — capturing S1G beacons, mesh peering action frames, data, etc. off the air.
This is the bench's only reliable HaLow sniffer (the ESP raw hook delivers nothing usable).

Validated 2026-06-26 on **chronium** (`192.168.7.187`): 364 frames in 12 s — chronite's S1G
beacons + both ESPs' beacons + mesh peering action frames.

## TL;DR recipe

```sh
export LC_ALL=C; export PATH=$PATH:/usr/sbin:/usr/local/bin

# free the radio (stop any mesh/AP/STA using wlan1)
sudo pkill -9 -f wpa_supplicant_s1g 2>/dev/null
sudo iw dev wlan1 mesh leave 2>/dev/null

# put the morse vif into MONITOR type and tune it
sudo ip link set wlan1 down
sudo iw dev wlan1 set type monitor          # <-- MONITOR, not mesh (see gotcha #1)
sudo ip link set wlan1 up
sudo iw dev wlan1 set freq 5560             # S1G ch27 -> 5 GHz-model 5560 (915.5 MHz on air)

# bring up the raw radiotap netdev and read it
sudo ip link set morse0 up
sudo python3 halow-mon.py                    # AF_PACKET reader, see below
```

Verify it's tuned: `iw dev wlan1 info` shows `type monitor` + `channel 112 (5560 MHz)`, and
`sudo morse_cli -i wlan1 channel` shows `Operating Frequency: 915500 kHz`.

## Prerequisite: a monitor-enabled driver build

The `morse0` netdev only exists if `morse_driver` was built with **`CONFIG_MORSE_MONITOR=y`**.
On chronium this is already installed. To (re)build on a node that has the source tree:

```sh
cd ~/halow/morse_driver
make KERNEL_SRC=~/halow/rpi-linux \
     CONFIG_WLAN_VENDOR_MORSE=m CONFIG_MORSE_SPI=y \
     CONFIG_MORSE_USER_ACCESS=y CONFIG_MORSE_VENDOR_COMMAND=y \
     CONFIG_MORSE_MONITOR=y
# install + reboot (reloading the module unbinds the SPI device; reboot, don't reload):
sudo cp morse.ko /lib/modules/$(uname -r)/kernel/drivers/net/wireless/morse/morse.ko
sudo cp dot11ah/dot11ah.ko /lib/modules/$(uname -r)/kernel/drivers/net/wireless/morse/dot11ah.ko
sudo depmod -a && sudo systemctl reboot
```
The monitor build does **not** harm normal operation — the same node can later run mesh/AP/STA
fine. Monitor and normal vif just can't be active *at the same time* (gotcha #1).

## How it works (so the gotchas make sense)

`CONFIG_MORSE_MONITOR=y` adds a `morse0` netdev of type `ARPHRD_IEEE80211_RADIOTAP`
(`morse_driver/monitor.c`). In the RX path (`mac.c:morse_mac_skb_recv`), frames are forwarded
to `morse0` **only when `mors->monitor_mode` is true**, and when it is, normal-vif processing
is skipped entirely ("we only support a single interface"). `mors->monitor_mode` is set true
only when mac80211 enters monitor mode — i.e. when a **monitor-type** interface is up
(`mac.c:3901`, `IEEE80211_CONF_MONITOR`). A mesh/AP/STA vif never sets it.

### Gotchas
1. **Use `type monitor`, NOT `type mp`/mesh.** An older note said "put wlan1 into mesh to tune
   the radio, then morse0". That's wrong — mesh type does not set `monitor_mode`, so `morse0`
   stays empty (0 frames). Monitor type is required.
2. **Monitor is exclusive with normal operation.** While monitoring you cannot also be a mesh
   peer / AP / STA on the same radio. To capture a 2-node exchange you need a *third* node as
   the sniffer. (To inspect a handshake the two participants run, the `wpa_supplicant_s1g -dd`
   logs are an alternative — they log the frames each side TX/RX.)
3. **Channel = `freq 5560` for S1G ch27.** morse uses a "5 GHz model": S1G ch27 ↔ 5 GHz ch112
   ↔ `5560`. On air it's 915.5 MHz / 1 MHz BW. Use the `5560` number for `iw … set freq`.
4. **No `tcpdump`/`tshark` on the Pis.** Read `morse0` with an `AF_PACKET`/`SOCK_RAW` socket
   (script below). `morse0` carries a radiotap header you must skip.

## Reading frames — `halow-mon.py`

Each `morse0` frame is: **radiotap header** (length = u16 little-endian at byte offset 2) then
the **802.11 frame**. HaLow specifics: native **S1G beacons** arrive as Extension frames
(`type=3, subtype=1`) with a 15-byte single-address header; the morse stack also surfaces some
beacons in legacy form (`type=0, subtype=8`); mesh peering frames are Action (`type=0,
subtype=13`, `Category=15` self-protected, `Action` 1=Open / 2=Confirm / 3=Close).

```python
#!/usr/bin/env python3
# sudo python3 halow-mon.py   — dump HaLow frames from morse0
import socket, struct, binascii, time
SUB_MGMT = {8:"Beacon",5:"ProbeResp",13:"Action"}
s = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.ntohs(0x0003))
s.bind(("morse0", 0)); s.settimeout(1)
t0 = time.time()
while time.time()-t0 < 30:
    try: d = s.recv(4096)
    except socket.timeout: continue
    rtlen = struct.unpack_from('<H', d, 2)[0]      # radiotap length
    f = d[rtlen:]
    if len(f) < 2: continue
    fc = f[0]; ftype = (fc>>2)&3; fsub = (fc>>4)&0xf
    if ftype == 3 and fsub == 1:                    # S1G beacon (ext)
        sa = binascii.hexlify(f[4:10]).decode()
        print(f"S1G-BEACON sa={sa}")
    elif ftype == 0:                                # mgmt: A2=SA at offset 10
        sa = binascii.hexlify(f[10:16]).decode()
        name = SUB_MGMT.get(fsub, f"sub{fsub}")
        if fsub == 13 and len(f) >= 26:             # action: category/action after 24-byte hdr
            cat, act = f[24], f[25]
            extra = f" cat={cat} act={act}" + (" (MeshPeering)" if cat==15 else "")
        else: extra = ""
        print(f"{name:9} sa={sa}{extra}")
s.close()
```

Decoding a mesh peering frame body (after the 24-byte mgmt header): `Category(1)=0x0f`,
`Action(1)` (1/2/3), then capability/AID, Supported Rates (`01`), Mesh Config (`71`), Mesh ID
(`72`), and the **Mesh Peering Management element (`75`)** — `75 04 <proto:2> <llid:2>` for
Open, `75 06 … <plid:2>` for Confirm, `75 08 … <reason:2>` for Close. See
`docs/worklog/2026-06-26-mesh-mpm-peering-frames.md` for fully decoded reference frames.

## Restore the node to normal use

```sh
sudo ip link set wlan1 down
sudo iw dev wlan1 set type managed
sudo ip link set wlan1 up
# then start mesh/AP/STA as usual (e.g. wpa_supplicant_s1g for mesh)
```

## Known limitation
The morse `morse0` tap is the supported path. A separate standalone `mon0` vif added with
`iw phy … interface add` sometimes refuses to retune (`-16 EBUSY`); the `wlan1 → type monitor`
path above is the reliable one.
