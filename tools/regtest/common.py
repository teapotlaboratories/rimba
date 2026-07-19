"""Shared plumbing for the regression suite: ports, make, results, reporting.

Deliberately dependency-free (stdlib only) so it runs outside the IDF python env.
pyserial is imported lazily and only by the tiers that actually touch a board.
"""

from __future__ import annotations

import json
import re
import shutil
import subprocess
import sys
import time
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Iterable, Optional

REPO_ROOT = Path(__file__).resolve().parents[2]
FIRMWARE_DIR = REPO_ROOT / "firmware"
BOARDS_DIR = REPO_ROOT / "boards"
BUILD_DIR = REPO_ROOT / "build"
RESULTS_DIR = REPO_ROOT / "build" / "regtest"

SERIAL_BY_ID = Path("/dev/serial/by-id")

# --------------------------------------------------------------------------
# Result records
# --------------------------------------------------------------------------

PASS = "PASS"
FAIL = "FAIL"
SKIP = "SKIP"
#: The test ran but its result cannot be trusted as pass/fail — e.g. a noisy RF
#: measurement outside its margin. Distinct from FAIL on purpose: a bench artifact
#: must not read as a code regression.
INCONCLUSIVE = "INCONCLUSIVE"
#: Failed, and it was ALREADY known to fail for a documented reason (manifest
#: KNOWN_BROKEN_*). Counted and printed, but does not gate the run.
XFAIL = "XFAIL"
#: A known-broken case unexpectedly PASSED. Someone fixed it (or the reason is stale) —
#: gates the run so the stale exclusion gets removed rather than rotting.
XPASS = "XPASS"


@dataclass
class Result:
    tier: str
    name: str
    status: str
    detail: str = ""
    duration_s: float = 0.0
    #: Free-form evidence — the command run + the relevant output, per the
    #: "cite your sources" rule in .ai/AGENTS.md.
    evidence: str = ""
    meta: dict = field(default_factory=dict)

    @property
    def ok(self) -> bool:
        return self.status in (PASS, SKIP, XFAIL)


class Reporter:
    """Collects results, prints a live line per test, writes a JSON baseline."""

    def __init__(self, tier: str, quiet: bool = False):
        self.tier = tier
        self.quiet = quiet
        self.results: list[Result] = []
        self.started = time.time()
        #: When set (via seed_from / --append), pre-existing results keyed by name that new
        #: results replace, so a run can accumulate across invocations.
        self._seed: dict[str, dict] = {}

    def seed_from(self, prior_results: list) -> None:
        """Pre-load results from a prior baseline (for --append). New results with the same
        name replace the seeded one; the rest carry forward into the written baseline."""
        for pr in prior_results:
            name = pr.get("name")
            if name:
                self._seed[name] = pr

    def add(self, r: Result) -> Result:
        self.results.append(r)
        self._seed.pop(r.name, None)   # a fresh result supersedes any seeded one
        # Crash-safe: persist after every result so a killed run still leaves a baseline of
        # what completed (the environment reaps long processes; a run may not reach the end).
        try:
            self.write()
        except Exception:
            pass
        if not self.quiet:
            mark = {
                PASS: "\033[32mPASS\033[0m",
                FAIL: "\033[31mFAIL\033[0m",
                SKIP: "\033[33mSKIP\033[0m",
                INCONCLUSIVE: "\033[35mINCO\033[0m",
                XFAIL: "\033[90mXFAI\033[0m",
                XPASS: "\033[31mXPAS\033[0m",
            }.get(r.status, r.status)
            dur = f"{r.duration_s:6.1f}s" if r.duration_s else "       "
            print(f"  [{mark}] {dur}  {r.name}", flush=True)
            if r.detail and r.status != PASS:
                for line in r.detail.strip().splitlines():
                    print(f"          {line}", flush=True)
        return r

    def _merged(self) -> list[dict]:
        """This run's results (as dicts) plus any seeded results not superseded this run."""
        return [asdict(r) for r in self.results] + list(self._seed.values())

    def counts(self) -> dict[str, int]:
        c = {PASS: 0, FAIL: 0, SKIP: 0, INCONCLUSIVE: 0, XFAIL: 0, XPASS: 0}
        for r in self._merged():
            st = r["status"] if isinstance(r, dict) else r.status
            c[st] = c.get(st, 0) + 1
        return c

    def summary(self) -> str:
        c = self.counts()
        parts = [f"{c[PASS]} pass", f"{c[FAIL]} fail", f"{c[SKIP]} skip"]
        if c[INCONCLUSIVE]:
            parts.append(f"{c[INCONCLUSIVE]} inconclusive")
        if c[XFAIL]:
            parts.append(f"{c[XFAIL]} xfail (known-broken)")
        if c[XPASS]:
            parts.append(f"{c[XPASS]} XPASS -- a known-broken case now passes; "
                         "remove its manifest entry")
        return f"{self.tier}: {', '.join(parts)}  ({time.time() - self.started:.0f}s)"

    @property
    def green(self) -> bool:
        """Only real failures gate. XFAIL is documented + expected; XPASS gates because
        a stale known-broken entry hides future regressions."""
        c = self.counts()
        return c[FAIL] == 0 and c[XPASS] == 0

    def write(self, path: Optional[Path] = None) -> Path:
        RESULTS_DIR.mkdir(parents=True, exist_ok=True)
        path = path or RESULTS_DIR / f"{self.tier}-latest.json"
        payload = {
            "tier": self.tier,
            "generated": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
            "git": git_describe(),
            "halow_gitlink": submodule_sha("components/halow"),
            "counts": self.counts(),
            "duration_s": round(time.time() - self.started, 1),
            "results": self._merged(),
        }
        path.write_text(json.dumps(payload, indent=2) + "\n")
        return path


# --------------------------------------------------------------------------
# Repo / git helpers — a baseline is worthless without knowing what it baselined
# --------------------------------------------------------------------------


def git_describe() -> str:
    try:
        sha = subprocess.run(
            ["git", "rev-parse", "--short", "HEAD"],
            cwd=REPO_ROOT, capture_output=True, text=True, timeout=10,
        ).stdout.strip()
        dirty = subprocess.run(
            ["git", "status", "--porcelain"],
            cwd=REPO_ROOT, capture_output=True, text=True, timeout=15,
        ).stdout.strip()
        return f"{sha}{'-dirty' if dirty else ''}"
    except Exception:
        return "unknown"


def submodule_sha(path: str) -> str:
    try:
        out = subprocess.run(
            ["git", "rev-parse", "HEAD"],
            cwd=REPO_ROOT / path, capture_output=True, text=True, timeout=10,
        ).stdout.strip()
        return out[:12]
    except Exception:
        return "unknown"


# --------------------------------------------------------------------------
# Board / port resolution
# --------------------------------------------------------------------------


def resolve_port(efuse_mac: str) -> Optional[str]:
    """Resolve an ESP's serial port from its efuse MAC via /dev/serial/by-id.

    The ttyACM* numbers re-enumerate on every hotplug, so a cached path is a latent
    bug (docs/reference/rimba-bench-devices.md:175). The by-id symlink embeds the
    efuse MAC, which is stable per board — that is the only correct lookup key.

    Returns the resolved /dev/ttyACM* real path, or None if the board is absent
    (e.g. board2 with no tools/ppk2_hold.py running).
    """
    if not SERIAL_BY_ID.is_dir():
        return None
    want = f"usb-Espressif_USB_JTAG_serial_debug_unit_{efuse_mac}-if00"
    link = SERIAL_BY_ID / want
    if link.exists():
        return str(link.resolve())
    # Tolerate case drift in the MAC without ever falling back to a guessed ttyACM.
    for p in SERIAL_BY_ID.iterdir():
        if p.name.lower() == want.lower():
            return str(p.resolve())
    return None


def present_boards(bench: dict) -> dict[str, str]:
    """Map board name -> port for every bench ESP currently enumerated."""
    out = {}
    for name, b in bench.items():
        port = resolve_port(b.efuse_mac)
        if port:
            out[name] = port
    return out


# --------------------------------------------------------------------------
# Build
# --------------------------------------------------------------------------


def make(target: str, app: str, board: str, port: Optional[str] = None,
         timeout: int = 900, make_vars: Optional[dict] = None) -> subprocess.CompletedProcess:
    """Invoke the repo Makefile. NEVER call idf.py directly.

    The board overlay (boards/<BOARD>/sdkconfig.defaults) is load-bearing: it sets the
    chip target, the MM6108 pin map, and CONFIG_HALOW_COUNTRY_CODE. A bare idf.py build
    falls back to country "??", and morselib then refuses to bring the radio up
    (MMWLAN_CHANNEL_LIST_NOT_SET) — which masquerades as dead hardware. See
    .ai/AGENTS.md "Building".

    make_vars: extra `KEY=VALUE` args passed to the Makefile (e.g. LINUX_MAC/LINUX_IP,
    which the Makefile threads to idf.py as -D TEST_LINUX_MAC/IP compile defines).
    """
    cmd = ["make", target, f"APP={app}", f"BOARD={board}"]
    if port:
        cmd.append(f"PORT={port}")
    for k, v in (make_vars or {}).items():
        cmd.append(f"{k}={v}")
    return subprocess.run(
        cmd, cwd=REPO_ROOT, capture_output=True, text=True, timeout=timeout,
    )


def sdkconfig_path(app: str, board: str) -> Path:
    return BUILD_DIR / app / board / "sdkconfig"


COUNTRY_RE = re.compile(r'^CONFIG_HALOW_COUNTRY_CODE="(?P<cc>[^"]*)"', re.M)
TARGET_RE = re.compile(r'^CONFIG_IDF_TARGET="(?P<t>[^"]*)"', re.M)


def read_sdkconfig_value(app: str, board: str, key: str) -> Optional[str]:
    p = sdkconfig_path(app, board)
    if not p.exists():
        return None
    m = re.search(rf'^{re.escape(key)}=(?:"([^"]*)"|(\S+))', p.read_text(), re.M)
    if not m:
        return None
    return m.group(1) if m.group(1) is not None else m.group(2)


# --------------------------------------------------------------------------
# Serial capture
# --------------------------------------------------------------------------


def _await_port(prev_port: str, efuse_mac: Optional[str], timeout: float = 15.0) -> Optional[str]:
    """Wait for the DUT's serial node to reappear after a mid-capture USB re-enumeration.

    The PPK2-powered board (board2) can transiently drop + re-enumerate on a rail wobble
    -- measured ~2-3% of captures, worse right after dscycle cycles the rail. Re-enumeration
    RENUMBERS the /dev/ttyACM* node, so we re-resolve by efuse MAC when we know it; without a
    MAC (should not happen for the bench DUTs) we fall back to waiting for the prior path.
    Returns the (possibly new) port, or None if it never came back within `timeout`.
    """
    t0 = time.time()
    while time.time() - t0 < timeout:
        if efuse_mac:
            p = resolve_port(efuse_mac)
            if p:
                return p
        elif Path(prev_port).exists():
            return prev_port
        time.sleep(0.5)
    return None


def _capture(port: str, seconds: float, marker: Optional[str] = None,
             baud: int = 115200, efuse_mac: Optional[str] = None,
             retries: int = 1) -> tuple[str, bool]:
    """Read a board's console for `seconds` (or until a line contains `marker`).

    Tolerates up to `retries` mid-capture USB re-enumerations of the DUT. pyserial raises
    SerialException ("device reports readiness to read but returned no data ... multiple
    access on port?") when the /dev node drops mid-read; that is the PPK2 board glitching on
    the bus, NOT a firmware fault (it clears on the very next attempt, and a genuinely dead
    board fails deterministically anyway). Re-enumeration resets the ESP, so on such an error
    we re-resolve the port (_await_port) and recapture a FRESH full window -- the post-reset
    boot banner / radio-up line / reporter verdict prints again. Exhausting `retries` re-raises,
    so a persistently unstable board still FAILs.

    No reset is issued here: callers capture immediately after `make flash`, whose own
    end-of-flash hard reset is the boot trigger. Re-triggering here would be wrong.

    dtr/rts handling is a real trap on these XIAO ESP32-S3 boards: opening the port with
    DTR/RTS asserted resets the ESP32 but does NOT cleanly reset the MM6108, which then comes
    up garbage (chip id 0x8200fXXX -> 0x0000, MAC 00:00:00:00:00:00, "Channel list not set").
    Only a true VBUS/PPK2 power cycle clears that latch. So: dtr=False, rts=False
    (docs/reference/rimba-bench-devices.md:269-280).
    """
    try:
        import serial  # noqa: PLC0415  (lazy: stdlib-only import at module level)
    except ImportError:
        raise RuntimeError(
            "pyserial not importable. Run under the IDF python env, e.g.:\n"
            "  source vendor/esp-idf/export.sh && python tools/regtest/run.py ..."
        )
    attempt = 0
    while True:
        buf: list[str] = []
        matched = False
        s = serial.Serial()
        s.port = port
        s.baudrate = baud
        s.timeout = 0.5
        s.dtr = False
        s.rts = False
        try:
            s.open()
            t0 = time.time()
            while time.time() - t0 < seconds:
                line = s.readline()
                if not line:
                    continue
                text = line.decode("utf-8", "replace")
                buf.append(text)
                if marker is not None and marker in text:
                    matched = True
                    break
        except (serial.SerialException, OSError) as e:
            if attempt >= retries:
                raise
            attempt += 1
            # Visible, not silent: a recovered flake must still be countable (the bench's USB
            # re-enumeration rate is real data, not something to paper over).
            print(f"    [capture] {port} dropped mid-read ({type(e).__name__}); "
                  f"re-resolving + retrying ({attempt}/{retries})", file=sys.stderr, flush=True)
            new_port = _await_port(port, efuse_mac)
            if new_port and new_port != port:
                print(f"    [capture] re-enumerated: {port} -> {new_port}",
                      file=sys.stderr, flush=True)
            if new_port:
                port = new_port
            continue
        finally:
            try:
                s.close()
            except Exception:
                pass
        return "".join(buf), matched


def capture_serial(port: str, seconds: float, baud: int = 115200,
                   efuse_mac: Optional[str] = None) -> str:
    """Read a board's console for `seconds` and return the text.

    Pass `efuse_mac` so a mid-capture USB re-enumeration is re-resolved + retried once
    instead of failing the run (see _capture). Same dtr/rts caveat as _capture.
    """
    text, _ = _capture(port, seconds, marker=None, baud=baud, efuse_mac=efuse_mac)
    return text


def capture_until(port: str, seconds: float, marker: str, baud: int = 115200,
                  efuse_mac: Optional[str] = None) -> tuple[str, bool]:
    """Read a board's console until a line contains `marker` or `seconds` elapse.

    Returns (text, matched). Used by the T2 orchestrator to stop as soon as a reporter
    emits its TEST|RESULT line (or a support role prints its up-marker), instead of always
    waiting the full window. Pass `efuse_mac` to get the re-resolve-and-retry-once behaviour
    (see _capture). Same dtr/rts caveat.
    """
    return _capture(port, seconds, marker=marker, baud=baud, efuse_mac=efuse_mac)


def esptool_reset(port: str) -> subprocess.CompletedProcess:
    """Hard-reset a board via esptool (a WARM reset — see the caveat in capture_serial)."""
    esptool = REPO_ROOT / "vendor/esp-idf/components/esptool_py/esptool/esptool.py"
    return subprocess.run(
        [sys.executable, str(esptool), "--port", port, "--after", "hard_reset",
         "--no-stub", "chip_id"],
        capture_output=True, text=True, timeout=60,
    )


# --------------------------------------------------------------------------
# Radio-silence — the standing rule, enforced in code
# --------------------------------------------------------------------------


def go_radio_silent(boards: Iterable[str], bench: dict, idle_app: str,
                    board_cfg: str, verbose: bool = True) -> list[Result]:
    """Return every named ESP to the radio-free idle app.

    The rule (.ai/AGENTS.md "Leave the bench radio-silent when a test ends"): after
    EVERY hardware test, nothing is left beaconing, associating, peering, or
    monitoring. Linux nodes additionally need `ip link set wlan1 down` — that half is
    done by the caller that brought them up, since this function only owns the ESPs.
    """
    out = []
    for name in boards:
        t0 = time.time()
        b = bench[name]
        port = resolve_port(b.efuse_mac)
        if not port:
            out.append(Result("silence", f"{name} -> {idle_app}", SKIP,
                              detail="board not enumerated", duration_s=time.time() - t0))
            continue
        cp = make("flash", idle_app, board_cfg, port=port, timeout=300)
        ok = cp.returncode == 0
        out.append(Result(
            "silence", f"{name} -> {idle_app}", PASS if ok else FAIL,
            detail="" if ok else cp.stderr[-500:],
            duration_s=time.time() - t0,
            evidence=f"make flash APP={idle_app} BOARD={board_cfg} PORT={port}",
        ))
        if verbose:
            print(f"  radio-silent: {name} -> {idle_app} "
                  f"{'ok' if ok else 'FAILED'}", flush=True)
    return out


def have(cmd: str) -> bool:
    return shutil.which(cmd) is not None
