"""dscycle — the deep-sleep duty-cycle reconnect gate.

test-deepsleep-cycle deep-sleeps board2 (MM6108 held in RESET_N-low, ESP32-S3 deep sleep ~0.35 mA); on
each wake it cold-boots, re-associates, and logs "RECONNECTED in <ms>". This runner proves the
battery-leaf actually rejoins on EVERY wake: over N cycles it counts deep-sleep→wake→reassoc CYCLES —
a cycle is a long port-gone gap (the board genuinely deep-sleeping) *followed by* a reconnect (the
reassoc). The fresh-boot association that happens once after flashing is NOT a cycle. It is a STRUCTURAL
gate — no PPK2 measurement.

Wake is COMMANDED, not free-running (hardened 2026-07-21). The C6 (firmware/test-c6-trigger) now takes
serial commands: when board2's USB port disappears (it deep-slept), this runner waits a short settle so
the ext1 D5-low wake is armed, then sends `pulse` over the C6 → a deterministic wake at the right instant,
instead of a free-running ~30 s pulse that raced board2's short awake window and usually fell through to
the DUT's 60 s backup timer. If the C6 doesn't answer (old firmware / unplugged) the runner warns loudly
and falls back to the DUT's 60 s backup timer (slow but still works).

Self-contained (hardened 2026-07-21). board2 is PPK2-powered via tools/ppk2_hold.py. This runner now
STARTS the holder itself if board2 isn't powered (so `make test-all`, which runs `tp` right before this
and leaves board2 holderless, no longer finds board2 un-enumerated and SKIPs silently), frees the PPK2
PRECISELY at teardown (the process actually holding the PPK2 fd — not a broad `pgrep -f ppk2_hold.py`
that could hit any process merely mentioning the holder), power-cycles + reflashes board2 to the
radio-silent idle app (test-idle), and RESTARTS the holder so board2 is left powered + silent.

Why the port-gone gap, not the wake_cause: the USB port drops while board2 deep-sleeps and re-enumerates
on each wake; re-opening the native USB-JTAG can reset the just-woken board, so the firmware's wake_cause
is unreliable. The DURATION the port is absent can't be faked (a real deep-sleep waits for the pulse +
cold boot; a bare reset re-enumerates in ~2 s), and the DUT's "DEEP SLEEP" log line printed just before
it sleeps is direct proof of a genuine deep-sleep entry.
"""

from __future__ import annotations

import os
import re
import signal
import subprocess
import sys
import time
from pathlib import Path

if __package__ in (None, ""):
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
_RE_SLEEP = re.compile(r"DEEP SLEEP")
#: A port-gone gap longer than this (seconds) corroborates a real deep-sleep (vs a ~2 s bare reset). The
#: primary proof is the DUT's "DEEP SLEEP" marker; this covers the case where that line is missed.
_DEEPSLEEP_GAP_S = 3.0
#: After the port vanishes (board2 asleep), wait this long so the ext1 D5-low wake is armed, then command
#: the C6 pulse. Long enough to be safely past esp_deep_sleep_start(), short enough to keep cycles quick.
_WAKE_SETTLE_S = 1.5
#: If a commanded pulse doesn't bring board2 back within this long, re-pulse (the pulse can be missed —
#: e.g. a rail wobble). Recovers a missed wake in ~8 s instead of waiting ~60 s for the DUT backup timer.
_WAKE_RETRY_S = 8.0

_AP_APP = "test-apsta-ap"
_AP_BOARD = "board0"
_AP_MARKER = "ap-ready"
_DUT_APP = "test-deepsleep-cycle"

_PPK2_VID, _PPK2_PID = 0x1915, 0xC00A
#: The bench C6 (ESP32-C6-DevKitC-1) CP2102N bridge; override with C6_PORT= if it moves.
_C6_VID, _C6_PID = 0x10C4, 0xEA60
_C6_SERIAL = "7831fdc6c21ced11ba20c6b4bbdd3192"
_PPK2_PIDFILE = Path("/tmp/ppk2_hold.pid")


# ---------------------------------------------------------------------------
# C6 serial control (the commanded wake)
# ---------------------------------------------------------------------------


def _c6_port() -> str | None:
    """Resolve the C6's serial port by VID/PID (env C6_PORT wins), preferring the known bench unit."""
    env = os.environ.get("C6_PORT")
    if env:
        return env
    try:
        import serial.tools.list_ports as lp  # noqa: PLC0415
    except Exception:
        return None
    cands = [p for p in lp.comports() if p.vid == _C6_VID and p.pid == _C6_PID]
    if not cands:
        return None
    for p in cands:
        if (p.serial_number or "").lower() == _C6_SERIAL:
            return p.device
    return cands[0].device


class _C6:
    """A thin line-protocol client for firmware/test-c6-trigger over the C6 UART0 console.

    Opened ONCE per run and held open (opening can auto-reset the C6 via DTR/RTS, so we do it once and
    tolerate a single boot). dtr/rts are deasserted to minimise that reset.
    """

    def __init__(self, port: str):
        import serial  # noqa: PLC0415
        self._ser = serial.Serial()
        self._ser.port = port
        self._ser.baudrate = 115200
        self._ser.timeout = 0.3
        self._ser.dtr = False
        self._ser.rts = False
        self._ser.open()

    def cmd(self, text: str, read_ms: int = 300) -> list[str]:
        try:
            self._ser.reset_input_buffer()
            self._ser.write((text + "\n").encode())
            self._ser.flush()
        except Exception:
            return []
        out: list[str] = []
        t_end = time.time() + read_ms / 1000.0
        while time.time() < t_end:
            try:
                line = self._ser.readline()
            except Exception:
                break
            if line:
                s = line.decode("utf-8", "replace").strip()
                if s:
                    out.append(s)
        return out

    def ping(self) -> bool:
        for _ in range(4):
            resp = self.cmd("ping", read_ms=400)
            if any("C6|" in r for r in resp):
                return True
            time.sleep(0.3)
        return False

    def pulse(self, ms: int = 120) -> list[str]:
        return self.cmd(f"pulse {ms}", read_ms=ms + 350)

    def hiz(self) -> list[str]:
        return self.cmd("hiz", read_ms=300)

    def trigger(self) -> list[str]:
        return self.cmd("trigger", read_ms=300)

    def close(self) -> None:
        try:
            self._ser.close()
        except Exception:
            pass


def _open_c6() -> _C6 | None:
    """Open + probe the C6. Returns a live client, or None (caller warns + falls back to the backup timer)."""
    port = _c6_port()
    if not port:
        return None
    try:
        c6 = _C6(port)
    except Exception:
        return None
    time.sleep(1.2)   # tolerate a DTR/RTS reset-on-open before probing
    if not c6.ping():
        c6.close()
        return None
    return c6


# ---------------------------------------------------------------------------
# PPK2 holder management (self-contained, precise kill)
# ---------------------------------------------------------------------------


def _ppk2_tty_devices() -> list[str]:
    """All /dev/tty* nodes of the PPK2 (both CDC interfaces)."""
    try:
        import serial.tools.list_ports as lp  # noqa: PLC0415
    except Exception:
        return []
    return [p.device for p in lp.comports() if p.vid == _PPK2_VID and p.pid == _PPK2_PID]


def _pids_holding(devs: list[str]) -> set[int]:
    """PIDs (excluding self + parent) that currently hold any of `devs` open, via /proc/<pid>/fd.

    This is precise and cmdline-independent: it finds exactly the process holding the PPK2 serial fd, so
    it never hits a bystander that merely mentions 'ppk2_hold.py' in its command line (the old broad
    `pgrep -f` once killed an orchestrating stress script that way).
    """
    me, parent = os.getpid(), os.getppid()
    real = {os.path.realpath(d) for d in devs}
    if not real:
        return set()
    pids: set[int] = set()
    for entry in os.listdir("/proc"):
        if not entry.isdigit():
            continue
        pid = int(entry)
        if pid in (me, parent):
            continue
        fddir = f"/proc/{pid}/fd"
        try:
            for fd in os.listdir(fddir):
                try:
                    tgt = os.readlink(f"{fddir}/{fd}")
                except OSError:
                    continue
                if tgt in real or os.path.realpath(tgt) in real:
                    pids.add(pid)
                    break
        except (PermissionError, FileNotFoundError, ProcessLookupError):
            continue
    return pids


def _pidfile_holder() -> int | None:
    """The live PID recorded in the ppk2_hold pidfile, or None (missing / stale / self)."""
    try:
        pid = int(_PPK2_PIDFILE.read_text().strip())
    except Exception:
        return None
    if pid in (os.getpid(), os.getppid()):
        return None
    try:
        os.kill(pid, 0)
    except OSError:
        return None
    return pid


def _holder_running() -> bool:
    return bool(_pids_holding(_ppk2_tty_devices()) or _pidfile_holder())


def _free_ppk2() -> set[int]:
    """Kill EXACTLY the process(es) holding the PPK2 (fd-owner scan + pidfile) so we can take the rail.

    Replaces the old broad `pgrep -f ppk2_hold.py | kill -9`, which matched any process merely mentioning
    the holder in its cmdline.
    """
    targets = set(_pids_holding(_ppk2_tty_devices()))
    ph = _pidfile_holder()
    if ph:
        targets.add(ph)
    for pid in targets:
        try:
            os.kill(pid, signal.SIGTERM)
        except OSError:
            pass
    if targets:
        time.sleep(1.0)
        for pid in targets:
            try:
                os.kill(pid, signal.SIGKILL)
            except OSError:
                pass
        time.sleep(0.5)
    if _pidfile_holder() is None:
        try:
            _PPK2_PIDFILE.unlink(missing_ok=True)
        except Exception:
            pass
    return targets


def _start_holder() -> int | None:
    """Start tools/ppk2_hold.py DETACHED (start_new_session) so it outlives this run and keeps board2
    powered afterwards. sys.executable is the IDF python env (ppk2-api + pyserial) since make sourced it."""
    holder = common.REPO_ROOT / "tools" / "ppk2_hold.py"
    if not holder.exists():
        return None
    try:
        with open("/tmp/ppk2_hold.log", "ab") as logf:   # child dup's the fd; parent needn't keep it
            p = subprocess.Popen(
                [sys.executable, str(holder)],
                stdout=logf, stderr=logf, stdin=subprocess.DEVNULL,
                start_new_session=True, cwd=str(common.REPO_ROOT))
        return p.pid
    except Exception:
        return None


def _await_board2(efuse_mac: str, timeout_s: float) -> bool:
    t_end = time.time() + timeout_s
    while time.time() < t_end:
        if common.resolve_port(efuse_mac):
            return True
        time.sleep(0.5)
    return False


def _ensure_holder(efuse_mac: str) -> tuple[bool, str]:
    """Guarantee board2 is powered + enumerated by a holder. Returns (ready, detail).

    Makes the tier self-contained: if board2 isn't powered we start ppk2_hold ourselves (fixes the
    test-all silent SKIP). If it's powered but wedged (a leftover deep-sleep app re-enumerating the USB)
    we cold-recover it. If the PPK2 itself is absent, we can't fix it — the caller SKIPs loudly.
    """
    if common.resolve_port(efuse_mac):
        if not _holder_running():
            print("dscycle -- board2 up but no PPK2 holder; starting tools/ppk2_hold.py ...", flush=True)
            _start_holder()
            time.sleep(2.0)
        return True, "board2 already powered"

    if not _ppk2_tty_devices():
        return False, "no PPK2 found (Nordic 1915:C00A) — board2 is PPK2-powered; cannot self-power it"

    if not _holder_running():
        print("dscycle -- board2 not powered; starting tools/ppk2_hold.py ...", flush=True)
        _start_holder()
        if _await_board2(efuse_mac, 18.0):
            return True, "started ppk2_hold; board2 enumerated"

    # Powered but dark (or a holder is up yet board2 never enumerated) -> wedged; cold-recover it.
    print("dscycle -- board2 still dark; cold-recovering (power-cycle + rimba-hello) ...", flush=True)
    _recover_board2(efuse_mac)
    _start_holder()
    if _await_board2(efuse_mac, 18.0):
        return True, "cold-recovered + re-held board2"
    return False, "board2 will not enumerate even after a power-cycle (check the PPK2 rig)"


# ---------------------------------------------------------------------------
# Capture (commanded-wake)
# ---------------------------------------------------------------------------


def _capture(efuse_mac: str, c6: "_C6 | None", budget_s: float, need: int) -> tuple[list, int, int]:
    """Watch the flapping port and drive the commanded wake.

    Returns (reconnect_latencies_ms, deep_sleep_gaps, wake_cycles). A wake cycle = a genuine deep-sleep
    (a long port-gone gap and/or the DUT's "DEEP SLEEP" marker) FOLLOWED by a reconnect. The one-off
    fresh-boot association after flashing is deliberately NOT counted.
    """
    import serial  # noqa: PLC0415

    recons: list[int] = []
    deep_sleeps = 0
    cycles = 0
    pending_gap = False      # observed a deep-sleep gap; the next reconnect proves the wake
    saw_ds_marker = False    # saw "DEEP SLEEP" before the port vanished (direct deep-sleep proof)
    last_pulse_at = None     # wall-clock of the last commanded wake pulse this sleep (None = not yet)
    last_gap = 0.0           # the most recent deep-sleep gap (for the per-cycle progress line)
    t_end = time.time() + budget_s
    ser = None
    gone_since = None        # wall-clock when the port went absent, or None if present
    while time.time() < t_end and cycles < need:
        if ser is None:
            port = common.resolve_port(efuse_mac)
            if not port:
                # board2 is asleep. Arm-and-command the wake once ext1 is surely up; re-pulse every
                # _WAKE_RETRY_S if it doesn't come back (a pulse can be missed on a rail wobble).
                now = time.time()
                if gone_since is None:
                    gone_since = now
                    last_pulse_at = None
                elif c6 and (now - gone_since) >= _WAKE_SETTLE_S and (
                        last_pulse_at is None or (now - last_pulse_at) >= _WAKE_RETRY_S):
                    print("dscycle -- board2 asleep; commanding C6 wake pulse"
                          + (" (retry)" if last_pulse_at is not None else ""), flush=True)
                    c6.pulse(120)
                    last_pulse_at = now
                time.sleep(0.3)
                continue
            # A path exists, but it can be a STALE by-id symlink lingering through the vanish transition.
            # Only a SUCCESSFUL open proves board2 is genuinely back, so a blip can't reset the gap timer
            # (which would misreport the deep-sleep duration and count too early).
            try:
                s = serial.Serial()
                s.port = port
                s.baudrate = 115200
                s.timeout = 0.5
                s.dtr = False   # never reset on open (bench-devices.md:269-284)
                s.rts = False
                s.open()
            except Exception:
                if gone_since is None:
                    gone_since = time.time()
                    last_pulse_at = None
                time.sleep(0.3)
                continue
            ser = s
            # Genuinely back. A real absence (a long gap OR a DEEP SLEEP marker before it) = a deep-sleep.
            if gone_since is not None:
                gap = time.time() - gone_since
                if saw_ds_marker or gap >= _DEEPSLEEP_GAP_S:
                    deep_sleeps += 1
                    pending_gap = True
                    last_gap = gap
                saw_ds_marker = False
                gone_since = None
        try:
            line = ser.readline()
        except Exception:   # port vanished (board deep-slept) -> re-resolve + re-open
            try:
                ser.close()
            except Exception:
                pass
            ser = None
            gone_since = time.time()
            last_pulse_at = None
            continue
        if not line:
            continue
        text = line.decode("utf-8", "replace")
        if _RE_SLEEP.search(text):
            saw_ds_marker = True
        m = _RE_RECON.search(text)
        if m:
            recons.append(int(m.group(1)))
            if pending_gap:
                cycles += 1
                pending_gap = False
                print(f"dscycle -- cycle {cycles}/{need}: deep-slept {last_gap:.1f}s -> "
                      f"reconnected in {m.group(1)}ms", flush=True)
    if ser:
        try:
            ser.close()
        except Exception:
            pass
    return recons, deep_sleeps, cycles


def _recover_board2(efuse_mac: str) -> bool:
    """Un-wedge board2 from the deep-sleep app and leave it on the radio-silent idle app (test-idle).

    The sleep app constantly re-enumerates the native USB, so a plain re-flash can't land download mode.
    The reliable recipe (proven on-bench): take the PPK2 over from the holder (PRECISELY — see
    _free_ppk2), a *5 s* DUT power-off (a 2.5 s off leaves it wedged) for a clean cold boot, then
    tight-poll for the fresh-boot window and esptool the built idle app the instant board2 appears.
    test-idle never sleeps, so board2 is then stable. The caller restarts tools/ppk2_hold.py afterwards.
    """
    import glob  # noqa: PLC0415
    try:
        from ppk2_api.ppk2_api import PPK2_API  # noqa: PLC0415
    except Exception:
        return False

    _free_ppk2()   # precise: only whatever actually holds the PPK2 fd

    ctl = _find_ppk2_port()
    if not ctl:
        return False
    ppk2 = None
    try:
        ppk2 = PPK2_API(ctl)
        ppk2.get_modifiers()
        ppk2.use_ampere_meter()
        ppk2.set_source_voltage(5000)
        ppk2.toggle_DUT_power("OFF")
        time.sleep(5.0)   # 5 s: a shorter off leaves the deep-sleep-wedged board2 dark
        ppk2.toggle_DUT_power("ON")
    except Exception:
        _close_ppk2(ppk2)
        return False

    bd = str(common.BUILD_DIR / M.IDLE_APP / M.BENCH_BOARD)
    tail = ":".join(efuse_mac.split(":")[-2:])   # e.g. "F0:08"
    ok = False
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
                ok = True
                break
        time.sleep(0.2)
    # Release the PPK2 so the freshly-restarted holder can grab it cleanly.
    _close_ppk2(ppk2)
    return ok


def _close_ppk2(ppk2) -> None:
    if ppk2 is None:
        return
    for attr in ("ser", "serial", "_ser"):
        s = getattr(ppk2, attr, None)
        if s is not None:
            try:
                s.close()
            except Exception:
                pass
    time.sleep(0.5)


def run(cycles: int = 2, append: bool = True) -> Reporter:
    rep = Reporter("T2")
    M.require_bench()
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

    # Precondition: board2 must be PPK2-powered + enumerated. Self-contained now — start the holder if it
    # isn't (so test-all's tp-before-dscycle order no longer SKIPs silently). Only a missing PPK2 SKIPs.
    ready, why = _ensure_holder(dut.efuse_mac)
    if not ready:
        print(f"dscycle -- SKIP: {why}", flush=True)
        rep.add(Result("T2", slug, SKIP, detail=why))
        return rep
    ap_port = common.resolve_port(M.BENCH[_AP_BOARD].efuse_mac)
    if not ap_port:
        rep.add(Result("T2", slug, SKIP, detail=f"AP board {_AP_BOARD} not enumerated"))
        return rep

    # The commanded wake needs the serial-latch C6. Warn loudly + fall back to the 60 s backup timer if
    # it doesn't answer (old firmware / unplugged), so the tier still runs, just slower.
    c6 = _open_c6()
    if c6 is None:
        print("dscycle -- WARNING: C6 serial not responding (needs firmware/test-c6-trigger with serial "
              "control). Falling back to the DUT's 60 s backup timer — slow. Flash the C6 for a fast, "
              "dependable wake.", flush=True)
    else:
        c6.hiz()   # rest D5 so the DUT boots via its own pull-down (=run) and we wake it on command

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
        print(f"dscycle -- flashing {_DUT_APP} to board2 + watching {cycles} wake cycles "
              f"({'commanded C6 wake' if c6 else '60 s backup-timer wake'})...", flush=True)
        cp = common.make("flash", _DUT_APP, M.BENCH_BOARD, port=dut_port, timeout=600)
        if cp.returncode != 0:
            rep.add(Result("T2", slug, INCONCLUSIVE, detail=f"DUT flash failed: {cp.stderr[-200:]}"))
            return rep

        # 3. Count deep-sleep→wake→reassoc CYCLES across the flapping port. A commanded cycle is ~15-23 s;
        #    the per-cycle budget leaves headroom for the occasional flaked wake/reconnect (rail wobble)
        #    to recover via a re-pulse. Without the C6 it falls to the 60 s backup timer (~130 s/cycle).
        per_cycle = 55.0 if c6 else 130.0
        budget = 45.0 + cycles * per_cycle
        recons, deep_sleeps, wake_cycles = _capture(dut.efuse_mac, c6, budget, cycles)
        lat_str = ", ".join(f"{ms}ms" for ms in recons) or "none"
        meta = {"cycles_target": cycles, "wake_cycles": wake_cycles, "deep_sleep_gaps": deep_sleeps,
                "reconnects_total": len(recons), "latencies_ms": recons,
                "wake": "commanded-c6" if c6 else "backup-timer"}
        dur = time.time() - t0
        if wake_cycles >= cycles:
            rep.add(Result("T2", slug, PASS, duration_s=dur,
                           detail=f"{wake_cycles}/{cycles} deep-sleep→wake→reassoc cycles "
                                  f"({deep_sleeps} long deep-sleep gaps >{_DEEPSLEEP_GAP_S:.0f}s, "
                                  f"{len(recons)} reconnects, latencies {lat_str})", meta=meta))
        elif wake_cycles >= 1:
            rep.add(Result("T2", slug, INCONCLUSIVE, duration_s=dur,
                           detail=f"only {wake_cycles}/{cycles} deep-sleep→wake→reassoc cycles within "
                                  f"{budget:.0f}s ({deep_sleeps} deep-sleep gaps, {len(recons)} reconnects) "
                                  f"— a flaky link/wake (latencies {lat_str})", meta=meta))
        elif recons:
            rep.add(Result("T2", slug, INCONCLUSIVE, duration_s=dur,
                           detail=f"board2 associated ({len(recons)} reconnect(s), latencies {lat_str}) "
                                  f"but completed 0/{cycles} deep-sleep→wake cycles ({deep_sleeps} gaps) "
                                  f"— did it deep-sleep and get woken?", meta=meta))
        else:
            rep.add(Result("T2", slug, FAIL, duration_s=dur,
                           detail=f"0/{cycles} reconnects ({deep_sleeps} deep-sleep gaps) — the leaf did "
                                  f"not rejoin on wake. Is the AP up and the C6 wake reaching D5?",
                           meta=meta))
        return rep
    finally:
        # Recovery: catch an awake window + flash rimba-hello, RESTART the holder (board2 stays powered),
        # silence the AP, release the C6.
        print("dscycle -- recovering board2 (rimba-hello) + restarting ppk2_hold + AP silence...",
              flush=True)
        if c6:
            c6.trigger()   # restore the free-running default the tp tier depends on (bench contract)
            c6.close()
        _recover_board2(dut.efuse_mac)
        if not _holder_running():   # recover freed the PPK2; re-hold so board2 stays powered
            _start_holder()
        common.go_radio_silent([_AP_BOARD], M.BENCH, M.IDLE_APP, M.BENCH_BOARD, verbose=False)
