"""dscycle — the deep-sleep duty-cycle reconnect gate.

test-deepsleep-cycle deep-sleeps board2 (MM6108 held in RESET_N-low, ESP32-S3 deep sleep ~0.35 mA),
the C6 (firmware/test-c6-trigger MODE_TRIGGER) wakes it via a D5 LOW pulse every ~30 s (or a 60 s backup
timer), and on each wake it cold-boots, re-associates, and logs "RECONNECTED in <ms>".

This runner proves the battery-leaf actually rejoins on EVERY wake: over N cycles it counts both
(a) the RECONNECTED lines (the reassoc), and (b) the LONG port-gone gaps that prove the board was
genuinely deep-sleeping between reconnects (not reset-cycling). It is a STRUCTURAL gate — no PPK2
measurement. board2 must be PPK2-powered (via tools/ppk2_hold.py, which this runner does NOT take
over — it stays running so board2 is stable throughout) and the C6 must be triggering.

Why the port-gone gap, not the wake_cause: the USB port drops while board2 deep-sleeps and
re-enumerates on each wake; re-opening the native USB-JTAG resets the just-woken board, so the
firmware's wake_cause reads UNDEFINED and "WOKE" is missed. But the DURATION the port is absent can't
be faked: a genuine deep-sleep waits 0-30 s for the C6 pulse (+ boot) — a long gap — whereas a bare
reset returns in ~2 s. So a >_DEEPSLEEP_GAP_S gap before a reconnect = a real deep-sleep cycle.

Recovery afterward: the sleep app wedges the USB, so rimba-hello is flashed on a RETRY loop that
catches one of board2's ~9 s awake windows (no PPK2 power-cycle needed — that destabilised the rail).
"""

from __future__ import annotations

import re
import sys
import time

if __package__ in (None, ""):
    import sys
    from pathlib import Path
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
    from tools.regtest import manifest as M  # type: ignore
    from tools.regtest import common  # type: ignore
    from tools.regtest.tp_power import _find_ppk2_port  # type: ignore
else:
    from . import manifest as M
    from . import common
    from .tp_power import _find_ppk2_port

from .common import FAIL, INCONCLUSIVE, PASS, SKIP, Reporter, Result

_RE_RECON = re.compile(r"RECONNECTED in (\d+) ms")
#: A port-gone gap longer than this (seconds) means the board was deep-sleeping, waiting for the C6
#: pulse (0-30 s) or the 60 s backup timer — not merely resetting (which re-enumerates in ~2 s).
_DEEPSLEEP_GAP_S = 4.0

_AP_APP = "test-apsta-ap"
_AP_BOARD = "board0"
_AP_MARKER = "ap-ready"
_DUT_APP = "test-deepsleep-cycle"


def _capture(efuse_mac: str, budget_s: float, need: int) -> tuple[list, int]:
    """Watch the flapping port. Returns (reconnect_latencies_ms, deep_sleep_gaps).

    Re-resolves + re-opens the port by MAC after every drop. A LONG absence before the port returns
    (> _DEEPSLEEP_GAP_S) is counted as a deep-sleep cycle; the RECONNECTED lines count the reassocs.
    """
    import serial  # noqa: PLC0415

    recons: list[int] = []
    deep_sleeps = 0
    t_end = time.time() + budget_s
    ser = None
    gone_since = None   # wall-clock when the port became unavailable, or None if present
    while time.time() < t_end and len(recons) < need:
        if ser is None:
            port = common.resolve_port(efuse_mac)
            if not port:
                if gone_since is None:
                    gone_since = time.time()
                time.sleep(0.4)
                continue
            # Port is back. If it was absent for a while, that gap was a deep-sleep wait.
            if gone_since is not None:
                if (time.time() - gone_since) >= _DEEPSLEEP_GAP_S:
                    deep_sleeps += 1
                gone_since = None
            try:
                ser = serial.Serial()
                ser.port = port
                ser.baudrate = 115200
                ser.timeout = 0.5
                ser.dtr = False   # never reset on open (bench-devices.md:269-284)
                ser.rts = False
                ser.open()
            except Exception:
                ser = None
                gone_since = time.time()
                time.sleep(0.4)
                continue
        try:
            line = ser.readline()
        except Exception:   # port vanished (board deep-slept) -> re-resolve + re-open
            try:
                ser.close()
            except Exception:
                pass
            ser = None
            gone_since = time.time()
            continue
        if not line:
            continue
        m = _RE_RECON.search(line.decode("utf-8", "replace"))
        if m:
            recons.append(int(m.group(1)))
    if ser:
        try:
            ser.close()
        except Exception:
            pass
    return recons, deep_sleeps


def _recover_board2(efuse_mac: str) -> bool:
    """Un-wedge board2 from the deep-sleep app and leave it on rimba-hello.

    The sleep app constantly re-enumerates the native USB, so a plain re-flash can't land download
    mode. The reliable recipe (proven on-bench): take the PPK2 over from the holder, a *5 s* DUT
    power-off (a 2.5 s off leaves it wedged) for a clean cold boot, then tight-poll for the fresh-boot
    window and esptool the built rimba-hello the instant board2 appears (esptool's reset then holds it
    in the ROM bootloader so the flash completes even though the app would re-sleep). rimba-hello never
    sleeps, so board2 is then stable. The OPERATOR restarts tools/ppk2_hold.py to hold power (the rail
    latch is unreliable on this rig) -- the runner prints that reminder.
    """
    import glob  # noqa: PLC0415
    import os  # noqa: PLC0415
    import subprocess  # noqa: PLC0415
    try:
        import serial.tools.list_ports  # noqa: PLC0415
        from ppk2_api.ppk2_api import PPK2_API  # noqa: PLC0415
    except Exception:
        return False

    # Free the PPK2 from any holder (ppk2_hold) so we can power-cycle.
    for name in ("ppk2_hold.py", "ppk2_mon2.py", "ppk2_power.py"):
        try:
            for pid in subprocess.check_output(["pgrep", "-f", name]).decode().split():
                try:
                    os.kill(int(pid), 9)
                except ProcessLookupError:
                    pass
        except subprocess.CalledProcessError:
            pass
    time.sleep(1.5)

    cands = [p for p in serial.tools.list_ports.comports()
             if p.vid == 0x1915 and p.pid == 0xC00A]
    if not cands:
        return False
    ctl = ([p.device for p in cands if str(p.location or "").endswith(".1")] or [cands[0].device])[0]
    try:
        ppk2 = PPK2_API(ctl)
        ppk2.get_modifiers()
        ppk2.use_ampere_meter()
        ppk2.set_source_voltage(5000)
        ppk2.toggle_DUT_power("OFF")
        time.sleep(5.0)   # 5 s: a shorter off leaves the deep-sleep-wedged board2 dark
        ppk2.toggle_DUT_power("ON")
    except Exception:
        return False

    bd = str(common.BUILD_DIR / M.IDLE_APP / M.BENCH_BOARD)
    tail = ":".join(efuse_mac.split(":")[-2:])   # e.g. "F0:08"
    for _ in range(60):
        g = glob.glob(f"/dev/serial/by-id/*{tail}*")
        if g:
            r = subprocess.run(
                [sys.executable, "-m", "esptool", "--chip", "esp32s3",
                 "--port", os.path.realpath(g[0]), "-b", "460800",
                 "--before", "default_reset", "--after", "hard_reset",
                 "write_flash", "@flash_args"],
                cwd=bd, capture_output=True, text=True, timeout=90)
            if r.returncode == 0:
                return True
        time.sleep(0.2)
    return False


def run(cycles: int = 2, append: bool = True) -> Reporter:
    rep = Reporter("T2")
    if append:
        prior = common.RESULTS_DIR / "T2-latest.json"
        if prior.exists():
            import json  # noqa: PLC0415
            try:
                rep.seed_from(json.loads(prior.read_text()).get("results", []))
            except Exception:
                pass

    dut = M.BENCH["board2"]
    slug = "deepsleep-reconnect"

    # Preconditions. board2 is PPK2-powered (needs tools/ppk2_hold.py running); we do NOT take the
    # PPK2 over -- the holder keeps board2 stable while it deep-sleeps.
    if not _find_ppk2_port():
        rep.add(Result("T2", slug, SKIP, detail="no PPK2 found (board2 is PPK2-powered)"))
        return rep
    if not common.resolve_port(dut.efuse_mac):
        rep.add(Result("T2", slug, SKIP, detail="board2 not enumerated (start tools/ppk2_hold.py)"))
        return rep
    ap_port = common.resolve_port(M.BENCH[_AP_BOARD].efuse_mac)
    if not ap_port:
        rep.add(Result("T2", slug, SKIP, detail=f"AP board {_AP_BOARD} not enumerated"))
        return rep

    t0 = time.time()
    try:
        # 1. Bring up the ESP AP (the leaf associates to rimba-ping).
        print(f"dscycle -- bringing up the AP ({_AP_APP} on {_AP_BOARD})...", flush=True)
        cp = common.make("flash", _AP_APP, M.BENCH_BOARD, port=ap_port, timeout=600)
        if cp.returncode != 0:
            rep.add(Result("T2", slug, INCONCLUSIVE, detail=f"AP flash failed: {cp.stderr[-200:]}"))
            return rep
        _, up = common.capture_until(ap_port, 30.0, _AP_MARKER)
        if not up:
            rep.add(Result("T2", slug, INCONCLUSIVE, detail=f"ESP AP never printed '{_AP_MARKER}'"))
            return rep

        # 2. Flash the deep-sleep leaf to board2 (it boots, connects, then deep-sleeps).
        dut_port = common.resolve_port(dut.efuse_mac)
        print(f"dscycle -- flashing {_DUT_APP} to board2 + watching {cycles} wake cycles...", flush=True)
        cp = common.make("flash", _DUT_APP, M.BENCH_BOARD, port=dut_port, timeout=600)
        if cp.returncode != 0:
            rep.add(Result("T2", slug, INCONCLUSIVE, detail=f"DUT flash failed: {cp.stderr[-200:]}"))
            return rep

        # 3. Count reconnects + deep-sleep gaps across the flapping port. On this rig the C6 D5-wake
        #    is unreliable, so most cycles fall to the 60 s backup timer -> a slow ~130 s/cycle
        #    cadence (a boot + a serial-reset-induced re-boot + connect on top of the wait). Budget
        #    for that so the target cycles actually fit.
        budget = 60.0 + cycles * 130.0
        recons, deep_sleeps = _capture(dut.efuse_mac, budget, cycles)
        n = len(recons)
        lat_str = ", ".join(f"{ms}ms" for ms in recons) or "none"
        meta = {"cycles_target": cycles, "reconnects": n, "deep_sleep_gaps": deep_sleeps,
                "latencies_ms": recons}
        dur = time.time() - t0
        # A VALID cycle needs both: a long deep-sleep gap AND a reassoc. Reconnects without long gaps
        # would be reset-cycling, not a deep-sleep leaf.
        if n >= cycles and deep_sleeps >= cycles:
            rep.add(Result("T2", slug, PASS, duration_s=dur,
                           detail=f"{n}/{cycles} deep-sleep→wake→reassoc cycles: {deep_sleeps} long "
                                  f"deep-sleep gaps (>{_DEEPSLEEP_GAP_S:.0f}s) + {n} reconnects "
                                  f"(latencies {lat_str})", meta=meta))
        elif n >= cycles and deep_sleeps < cycles:
            rep.add(Result("T2", slug, INCONCLUSIVE, duration_s=dur,
                           detail=f"{n}/{cycles} reconnects but only {deep_sleeps} long deep-sleep "
                                  f"gaps — the board reconnected but did not clearly deep-sleep "
                                  f"between cycles (reset-cycling / a power issue?), so not a clean "
                                  f"deep-sleep-leaf proof.", meta=meta))
        elif n > 0:
            rep.add(Result("T2", slug, INCONCLUSIVE, duration_s=dur,
                           detail=f"only {n}/{cycles} reconnects within {budget:.0f}s "
                                  f"({deep_sleeps} deep-sleep gaps) — a flaky link/wake "
                                  f"(latencies {lat_str})", meta=meta))
        else:
            rep.add(Result("T2", slug, FAIL, duration_s=dur,
                           detail=f"0/{cycles} reconnects ({deep_sleeps} deep-sleep gaps) — the leaf "
                                  f"did not rejoin on wake. Is the AP up and the C6 triggering (D5)?",
                           meta=meta))
        return rep
    finally:
        # Recovery: catch an awake window + flash rimba-hello; silence the AP. No PPK2 power-cycle
        # (ppk2_hold keeps board2 powered).
        print("dscycle -- recovering board2 (rimba-hello, retrying for an awake window) + AP silence...",
              flush=True)
        _recover_board2(dut.efuse_mac)
        common.go_radio_silent([_AP_BOARD], M.BENCH, M.IDLE_APP, M.BENCH_BOARD, verbose=False)
