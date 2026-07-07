# morse_driver local patches

Carry-forward patches applied to the stock `morse_driver` tree on the build host (chronium,
`~/halow/morse_driver`) before building. Re-apply after every version bump (`git checkout <tag>`).

## `morse-driver-pi5-reset-gpiod.patch` — fix the chip hardware reset on the Pi 5

**Problem.** On the **Pi 5** nodes (chronium, chronite — MM6108 reset line = GPIO17 on the **RP1**
PCIe-attached gpio controller), the stock `morse_hw_reset()` (`hw.c`) **never actually resets the chip**:

1. It uses the **legacy integer-GPIO API** (`gpio_request()` / `gpio_direction_*()`), which fails on the
   RP1 controller (the reset is silently skipped — no `Resetting Morse Chip` in dmesg).
2. Even when driven, it **releases the reset by floating** the pin (`gpio_direction_input`), but the Pi 5
   Wio HAT's `RESET_N` has **no usable pull-up**, so the line stays low and the chip stays in reset.

Net effect: a warm driver re-probe (`unbind`/`bind`, or `modprobe -r morse; modprobe morse`) fails with
`morse_chip_cfg_detect_and_init failed: -5` and `wlan1` disappears — so the only recovery was a **full
reboot** (which power-cycles the chip's rail). The Pi Zero nodes (reset = GPIO5 on the native SoC gpio)
were unaffected — the legacy API works there.

**Fix.** Rewrite `morse_hw_reset()` to (a) use the modern **gpiod** descriptor API (`gpio_to_desc` +
`gpiod_direction_output_raw`), which drives the RP1 pin correctly, and (b) **release by driving HIGH**
instead of floating. Both changes were verified on-bench by A/B test (float release → `-5`; drive-high →
clean fw reload).

**Result.** The Pi 5 driver now resets the chip itself, so `unbind`/`bind` and `modprobe -r/modprobe`
recover a wedged chip **without a reboot** — the same fast un-wedge the Pi Zeros already had.

### Apply + build + verify
```sh
# on chronium, in ~/halow/morse_driver (stock tree at the target tag):
git apply /path/to/morse-driver-pi5-reset-gpiod.patch
make -j4 KERNEL_SRC=/home/chronium/halow/rpi-linux CONFIG_WLAN_VENDOR_MORSE=m CONFIG_MORSE_SPI=y \
     CONFIG_MORSE_VENDOR_COMMAND=y CONFIG_MORSE_USER_ACCESS=y CONFIG_MORSE_DEBUGFS=y CONFIG_MORSE_MONITOR=y
# ^ Pi 5 tree = ~/halow/rpi-linux (kernel.release 6.12.21-v8-16k+); Pi Zero tree = ~/halow/rpi-linux-pi3.
#   make clean first if the tree was last built for the other kernel (else you get the wrong vermagic).
strip --strip-debug morse.ko   # deploy to the node's module dir, depmod, then modprobe -r/modprobe
# verify (Pi 5): unbind then bind must succeed with NO manual GPIO pulse:
echo spi0.0 | sudo tee /sys/bus/spi/drivers/morse_spi/unbind
echo spi0.0 | sudo tee /sys/bus/spi/drivers/morse_spi/bind   # -> dmesg "Resetting Morse Chip"/"Done", fw reloads
```

**Applies to the Pi 5 nodes only** (chronium, chronite). It is harmless on the Pi Zeros (same code path,
their reset already worked), so a single patched build can be deployed bench-wide per kernel.

Verified 2026-07-05 on chronite (srcver of the patched build differs from stock `405F9B14`).
