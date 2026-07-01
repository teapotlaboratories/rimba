# Radio-silent workflow (HaLow bench)

When the HaLow firmware is running, the MM6108 transmits RF continuously — the AP
node beacons + pings, the STA associates + pings. **When no test is in progress,
flash the radio-free `rimba-hello` to the boards so nothing is on the air.**

The MM6108 only emits RF once the app boots it (`mmhalow_init` / `mmwlan_boot`).
`rimba-hello` never touches the radio, so flashing it leaves the chip idle.

## Go silent (default idle state)

Flash `rimba-hello` to every connected board:

```bash
make flash APP=rimba-hello PORT=/dev/ttyACM0
make flash APP=rimba-hello PORT=/dev/ttyACM1
```

**Also silence any Linux bench node** used as a sniffer or mesh peer — take its
HaLow radio down so it stops monitoring/beaconing:

```bash
ssh chronium 'sudo ip link set wlan1 down'   # sniffer/monitor radio off
```

Do this after **every** test, not just at end of day: the bench's default resting
state is no-radio on **all** devices (ESP boards *and* Linux nodes).

## Run the ping test (radio ON)

```bash
make flash APP=rimba-halow-ap  PORT=/dev/ttyACM0   # SoftAP,  static 192.168.12.1
make flash APP=rimba-halow-sta PORT=/dev/ttyACM1   # station, static 192.168.12.2
```

Watch results on the STA console: `make monitor APP=rimba-halow-sta PORT=/dev/ttyACM1`
(or read the port directly — see notes). **When done testing, go silent again.**

## Verify a board is silent

Reset + read the console; it should show the `rimba_hello` banner and **no**
HaLow init:

```bash
python -m esptool --chip esp32s3 -p /dev/ttyACM0 --before default_reset --after hard_reset flash_id
# then read /dev/ttyACM0 — expect "rimba_hello" / "Rimba Phase-1 bring-up",
# and zero "Morse Micro HaLow" / "mmwlan" lines.
```

## Notes

- Builds/flashes use the vendored ESP-IDF (`vendor/esp-idf`) automatically — no
  `export.sh` needed; the Makefile sources it.
- Board↔port mapping is arbitrary (both boards are identical); AP vs STA is just
  which firmware you flash to which port.
- `idf.py monitor` needs a real TTY. For scripted/non-interactive capture, reset
  with esptool, then `cat` the port (`stty -F <port> 115200 raw -echo` first).
  USB-Serial-JTAG ignores baud; DTR/RTS toggling does **not** reset these boards
  (native USB-JTAG) — use esptool to reset.
