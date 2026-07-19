"""preflight — check you can actually reach a device BEFORE running a test.

Point it at YOUR setup — a serial port, an efuse MAC, a bench-node name, or an ssh
host — and it verifies the harness can really talk to it, with a clear PASS/FAIL and a
fix hint. The point is to fail fast on an unreachable device instead of 10 minutes into
a long test run.

    make test-conn PORT=/dev/ttyACM0         # a board on a known serial port
    make test-conn MAC=E0:72:A1:F8:F0:08     # a board by its efuse MAC (resolves the port)
    make test-conn NODE=board2               # a bench node from the manifest
    make test-conn HOST=chronite             # a Linux HaLow node over ssh
    make test-conn                           # everything the manifest knows, that's present

Or directly:

    python tools/regtest/run.py preflight --port /dev/ttyACM0 [--chip esp32s3]
    python tools/regtest/run.py preflight --mac E0:72:A1:F8:F0:08
    python tools/regtest/run.py preflight --node board2
    python tools/regtest/run.py preflight --host chronite

Exit code is 0 only when every requested check PASSes. An ESP check talks to the chip
(esptool chip_id) rather than merely finding the port, so a dead radio / wrong chip /
permission problem is caught here, not on the bench.
"""

from __future__ import annotations

import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Optional

if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
    from tools.regtest import manifest as M  # type: ignore
    from tools.regtest import common  # type: ignore
else:
    from . import manifest as M
    from . import common

# Colours (match common.Reporter's live line).
_G, _R, _Y, _0 = "\033[32m", "\033[31m", "\033[33m", "\033[0m"

_ESPTOOL = common.REPO_ROOT / "vendor/esp-idf/components/esptool_py/esptool/esptool.py"


@dataclass
class Check:
    target: str          # what we probed (a port, a host, a board name)
    ok: bool
    detail: str          # the finding (chip id + MAC, or the failure)
    hint: str = ""       # how to fix it, when it failed


# --------------------------------------------------------------------------
# ESP-over-serial: prove the chip responds, don't just find the port
# --------------------------------------------------------------------------


def _esptool_chip_id(port: str) -> subprocess.CompletedProcess:
    """Run esptool chip_id against `port`. Reads chip type + MAC from a live chip.

    Uses the vendored esptool with the current interpreter. If pyserial isn't importable
    (not in the IDF python env), esptool exits non-zero with an ImportError we surface as a
    hint rather than a mystery failure.
    """
    return subprocess.run(
        [sys.executable, str(_ESPTOOL), "--port", port, "--after", "hard_reset",
         "--no-stub", "chip_id"],
        capture_output=True, text=True, timeout=60,
    )


def _parse_chip(out: str) -> tuple[Optional[str], Optional[str]]:
    """Pull (chip type, MAC) out of esptool chip_id output."""
    chip = mac = None
    for line in out.splitlines():
        s = line.strip()
        if s.startswith("Chip is ") and chip is None:
            chip = s[len("Chip is "):].split()[0]
        elif s.startswith("MAC:") and mac is None:
            mac = s.split("MAC:", 1)[1].strip()
    return chip, mac


def check_esp(label: str, port: Optional[str], chip_expect: Optional[str]) -> Check:
    """Verify an ESP board is reachable AND its chip actually answers on `port`."""
    if not port:
        return Check(label, False, "no serial port",
                     hint="plug the board in (or, for board2, run tools/ppk2_hold.py); "
                          "check /dev/serial/by-id and pass PORT=/dev/ttyACMx or MAC=<efuse>")
    try:
        cp = _esptool_chip_id(port)
    except FileNotFoundError:
        return Check(label, False, f"esptool not found at {_ESPTOOL}",
                     hint="run from the repo root; the vendored esp-idf submodule must be checked out")
    except subprocess.TimeoutExpired:
        return Check(label, False, f"{port}: no response within 60 s (esptool timeout)",
                     hint="is the board in a flashable state? a sleep app wedges the USB — "
                          "PPK2 power-cycle board2; otherwise reseat the cable")

    if cp.returncode != 0:
        tail = (cp.stderr or cp.stdout).strip().splitlines()
        why = tail[-1] if tail else f"esptool exited {cp.returncode}"
        hint = f"could not open/talk to {port}"
        low = (cp.stderr + cp.stdout).lower()
        if "permission denied" in low:
            hint = f"permission denied on {port} — add your user to the 'dialout' group (or sudo)"
        elif "importerror" in low or "no module named 'serial'" in low:
            hint = "pyserial missing — run under the IDF env: source vendor/esp-idf/export.sh"
        elif "could not open" in low or "no such file" in low:
            hint = f"{port} is gone — re-check the port (it may have re-enumerated); try MAC=<efuse>"
        elif "failed to connect" in low or "no serial data" in low:
            hint = "chip not answering — reseat the cable; a sleep app needs a PPK2 power-cycle"
        return Check(label, False, why, hint=hint)

    chip, mac = _parse_chip(cp.stdout)
    if not chip:
        return Check(label, False, f"{port}: chip_id gave no chip line",
                     hint="unexpected esptool output — run `python tools/regtest/run.py preflight "
                          f"--port {port}` by hand to see it")
    where = f"{port}"
    detail = f"{where}: {chip}" + (f", MAC {mac}" if mac else "")
    # A wrong chip target is a real setup error worth flagging (the whole bench is esp32s3).
    if chip_expect and chip_expect.replace("esp32", "").lower() not in chip.replace("-", "").lower():
        return Check(label, False, detail + f" — expected {chip_expect}",
                     hint=f"this board reports {chip}, not {chip_expect}; wrong board or wrong CHIP=")
    return Check(label, True, detail)


# --------------------------------------------------------------------------
# Linux node: reachable over ssh (by hostname), and HaLow-capable
# --------------------------------------------------------------------------


def check_ssh(host: str) -> Check:
    """Verify a Linux HaLow node is reachable by hostname over ssh and looks HaLow-ready."""
    try:
        cp = subprocess.run(
            ["ssh", "-o", "ConnectTimeout=6", "-o", "BatchMode=yes", host,
             "ip -o link show wlan1 >/dev/null 2>&1 && echo WLAN1_OK; "
             "lsmod 2>/dev/null | grep -q '^morse' && echo MORSE_OK; true"],
            capture_output=True, text=True, timeout=20,
        )
    except subprocess.TimeoutExpired:
        return Check(f"ssh:{host}", False, "ssh timed out",
                     hint=f"is {host} up + reachable? check ~/.ssh/config resolves it (never a raw IP)")
    if cp.returncode != 0:
        why = (cp.stderr.strip().splitlines() or ["ssh failed"])[-1]
        return Check(f"ssh:{host}", False, why,
                     hint=f"`ssh {host}` must work passwordless (key auth) — add it to ~/.ssh/config; "
                          "reach nodes by hostname, never a raw IP")
    out = cp.stdout
    wlan1 = "WLAN1_OK" in out
    morse = "MORSE_OK" in out
    if wlan1 and morse:
        return Check(f"ssh:{host}", True, "reachable; wlan1 present, morse module loaded")
    # Reachable but not fully HaLow-ready — a warning, still a usable finding for a plain reachability test.
    missing = []
    if not wlan1:
        missing.append("no wlan1")
    if not morse:
        missing.append("morse module not loaded")
    return Check(f"ssh:{host}", True, "reachable, but " + " + ".join(missing),
                 hint="ssh works; bring the HaLow stack up (modprobe morse) before an interop test")


# --------------------------------------------------------------------------
# Orchestration
# --------------------------------------------------------------------------


def _targets(args) -> list[Check]:
    checks: list[Check] = []
    chip = args.chip

    if args.port:
        checks.append(check_esp(f"port:{args.port}", args.port, chip))
    if args.mac:
        checks.append(check_esp(f"mac:{args.mac}", common.resolve_port(args.mac), chip))
    if args.node:
        M.require_bench()
    for node in (args.node or []):
        b = M.BENCH.get(node)
        if not b:
            checks.append(Check(f"node:{node}", False, "unknown bench node",
                                hint=f"known: {', '.join(M.BENCH)}"))
        else:
            checks.append(check_esp(f"node:{node}", common.resolve_port(b.efuse_mac), chip))
    for host in (args.host or []):
        checks.append(check_ssh(host))

    # No target given: probe every bench node make configured.
    if not (args.port or args.mac or args.node or args.host):
        M.require_bench()
        print("No target given — probing every configured bench node.\n")
        for name, b in M.BENCH.items():
            port = common.resolve_port(b.efuse_mac)
            if port:
                checks.append(check_esp(f"node:{name}", port, chip))
            else:
                checks.append(Check(f"node:{name}", False, "not enumerated",
                                    hint="not plugged in"
                                         + ("; run tools/ppk2_hold.py" if not b.usb_bus_powered else "")))
        for host in M.LINUX_NODES:
            checks.append(check_ssh(host))
    return checks


def run(args) -> int:
    checks = _targets(args)
    print("preflight — device connection check\n")
    worst_ok = True
    for c in checks:
        mark = f"{_G}OK{_0}" if c.ok else f"{_R}FAIL{_0}"
        print(f"  [{mark}]  {c.target:22s}  {c.detail}")
        if c.hint and not c.ok:
            print(f"          → {c.hint}")
        worst_ok = worst_ok and c.ok
    n_ok = sum(1 for c in checks if c.ok)
    print(f"\n{n_ok}/{len(checks)} reachable.")
    if not worst_ok:
        print("Fix the FAILs above before running a test that needs those devices.")
    return 0 if worst_ok else 1
