#!/usr/bin/env python3
"""Power-cycle a bench ESP board's USB hub port (cuts VBUS), giving a warm-reset-wedged MM6108 a true
cold power cycle without physical access. The dev-host USB hub 7-1 supports per-port power switching, so
writing its per-port `disable` sysfs attribute drops/restores VBUS. Those files are root-owned, so this
script must run as root — a passwordless-sudo entry (/etc/sudoers.d/esp-usb-power) is set up for it.

Port map (verified 2026-07-12, identify boards by efuse MAC, not ACM number):
  board0 = 7-1.1 -> 7-1-port1   (E0:72:A1:F8:EF:A4, bus-powered)
  board1 = 7-1.2 -> 7-1-port2   (E0:72:A1:F8:F9:40, bus-powered)
  board2 = 7-1.3 -> 7-1-port3   (E0:72:A1:F8:F0:08 — but board2's POWER is the PPK2 DUT rail; cutting its
                                 USB port only drops DATA, not power. Use tools/ppk2_hold.py for board2.)

Usage:  sudo tools/esp_usb_power.py <board0|board1|board2|port1|port2|port3> <off|on|cycle> [off_seconds]
Example: sudo tools/esp_usb_power.py board0 cycle 4      # VBUS off 4s then on = cold power cycle
"""
import sys, time, os

PORTMAP = {"board0": "7-1-port1", "board1": "7-1-port2", "board2": "7-1-port3",
           "port1": "7-1-port1", "port2": "7-1-port2", "port3": "7-1-port3"}
BASE = "/sys/bus/usb/devices/7-1:1.0"


def disable_path(port):
    return f"{BASE}/{port}/disable"


def setp(port, off):
    with open(disable_path(port), "w") as f:
        f.write("1\n" if off else "0\n")  # 1 = disable (VBUS off), 0 = enable (VBUS on)


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    board, action = sys.argv[1], sys.argv[2]
    off_s = float(sys.argv[3]) if len(sys.argv) > 3 else 4.0
    if board not in PORTMAP:
        sys.exit(f"unknown board '{board}'; one of {sorted(PORTMAP)}")
    port = PORTMAP[board]
    if not os.path.exists(disable_path(port)):
        sys.exit(f"no per-port disable attr at {disable_path(port)} (hub topology changed?)")
    if action == "off":
        setp(port, True);  print(f"{board} ({port}) VBUS OFF")
    elif action == "on":
        setp(port, False); print(f"{board} ({port}) VBUS ON")
    elif action == "cycle":
        setp(port, True);  print(f"{board} ({port}) VBUS OFF, waiting {off_s}s...", flush=True)
        time.sleep(off_s)
        setp(port, False); print(f"{board} ({port}) VBUS ON (cold power cycle done)")
    else:
        sys.exit("action must be off | on | cycle")


if __name__ == "__main__":
    main()
