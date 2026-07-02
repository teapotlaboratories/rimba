#!/usr/bin/env python3
"""
ppk2_hold.py — power the bench's 3rd ESP mesh board (board2) on/off via the Nordic PPK2.

board2 (efuse E0:72:A1:F8:F0:08 / mesh e2:72:a1:f8:f0:08 / 10.9.9.108) is NOT directly USB-powered:
it is fed from the Nordic PPK2 (Power Profiler Kit II, VID:PID 1915:c00a) DUT rail in *ampere-meter*
mode with DUT power ON. board2 only enumerates as an Espressif /dev/ttyACM* (VID 303a) while this hold
is running, and the hold is dropped on a host USB replug — after which board2 is dark until it is
re-run. Ampere-meter mode passes the external supply through to the DUT, so there is no source voltage
to set (no wrong-voltage risk). See docs/reference/rimba-bench-devices.md.

Run with the ESP-IDF python env (it has the ppk2-api pkg), e.g.:
    PY=/home/quartz/.espressif/python_env/idf5.4_py3.13_env/bin/python
    $PY tools/ppk2_hold.py            # ampere-meter, DUT ON, HOLD forever (Ctrl-C releases -> board2 off)
    $PY tools/ppk2_hold.py off        # DUT OFF (power board2 down), then exit
A clean power-cycle of board2 = `off`, then run the hold again.

Nohup it to keep board2 powered across your session:
    nohup $PY tools/ppk2_hold.py >/tmp/ppk2_hold.log 2>&1 &
"""
import sys
import time

import serial.tools.list_ports
from ppk2_api.ppk2_api import PPK2_API

PPK2_VID, PPK2_PID = 0x1915, 0xC00A


def find_ppk2_control_port():
    """The PPK2 exposes two CDC interfaces; the control interface is #01 (USB location '...:1.1')."""
    cands = [p for p in serial.tools.list_ports.comports()
             if p.vid == PPK2_VID and p.pid == PPK2_PID]
    if not cands:
        return None
    for p in cands:
        if str(p.location or "").endswith(".1"):  # interface 01 = control
            return p.device
    return cands[0].device  # fall back to whichever enumerated


def main():
    mode = (sys.argv[1] if len(sys.argv) > 1 else "hold").lower()
    port = find_ppk2_control_port()
    if port is None:
        sys.exit("PPK2 (Nordic 1915:c00a) not found on USB — is it plugged in?")

    ppk2 = PPK2_API(port)
    ppk2.get_modifiers()
    ppk2.use_ampere_meter()  # pass-through supply; DUT rail = external 5 V, no source voltage to set

    if mode == "off":
        ppk2.toggle_DUT_power("OFF")
        print(f"PPK2 {port}: DUT power OFF — board2 down.")
        return

    ppk2.toggle_DUT_power("ON")
    print(f"PPK2 {port}: DUT power ON — board2 up @ 5 V (ampere-meter). "
          "board2 enumerates as an Espressif /dev/ttyACM* while this holds.", flush=True)
    try:
        while True:
            time.sleep(3600)
    except KeyboardInterrupt:
        print("\nreleased.")


if __name__ == "__main__":
    main()
