# rimba-hello

Board bring-up / hello-world firmware for the Seeed XIAO ESP32-S3 (Plus). It
proves the ESP-IDF toolchain, build, flash, and USB console pipeline on real
hardware, then validates the board basics — chip identity, PSRAM presence with a
real read/write test, and SRAM/heap headroom — before printing a heartbeat. It
does not touch the HaLow radio, so it is the first thing to run on a new board.

## Build + flash

```sh
make build APP=rimba-hello BOARD=proto1-fgh100m
make flash APP=rimba-hello BOARD=proto1-fgh100m PORT=/dev/ttyACM0
```

Replace `/dev/ttyACMx` with the port your board enumerates as. On the XIAO
ESP32-S3 the native USB-Serial-JTAG shows up as `/dev/ttyACM0` — no external
USB-UART bridge is involved.

Watch the console after flashing (for example `idf.py -p /dev/ttyACM0 monitor`,
or your usual serial monitor). A healthy board prints its chip info, `PSRAM: OK`,
and a repeating heartbeat line.

## What to change for your own board

The defaults target the XIAO ESP32-S3 Plus (ESP32-S3R8, 16 MB QIO flash, 8 MB
octal PSRAM). If your board differs:

- `sdkconfig.defaults.esp32s3` — flash size/mode and PSRAM type/speed.
- `sdkconfig.defaults` — console routing (USB-Serial-JTAG vs. UART bridge).
- `main/rimba_hello.c` — `PSRAM_TEST_BYTES` sets how much PSRAM the probe
  exercises; drop the PSRAM checks entirely on boards without PSRAM.
