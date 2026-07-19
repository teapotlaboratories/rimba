"""T1 — flash + boot + the radio actually comes up.

WHAT T1 PROVES: the built binary boots on real silicon and morselib brings the MM6108
up: a real chip id, a real firmware version, a real MAC, and the *runtime* country code
the board overlay intended. It is the tier that separates "compiles" from "the radio
lives".

WHAT T1 DOES NOT PROVE: any on-air feature. T1 boards have no peer. An AP will beacon
into an empty room and an STA will fail to associate — T1 asserts only what happens
*before* any peer interaction, i.e. bring-up.

The markers are read from the real code, not invented:

  components/halow/mmhalow.c:192  ESP_LOGI(TAG, "Setting Channel List %s", CONFIG_HALOW_COUNTRY_CODE)
  components/halow/mmhalow.c:163  "  Morselib version:        %s"
  components/halow/mmhalow.c:164  "  Morse firmware version:  %s"
  components/halow/mmhalow.c:165  "  Morse chip ID:           0x%04lx"
  components/halow/mmhalow.c:174  "Wi-Fi MAC address: ..."
  components/halow/mmhalow.c:160  "Error occured whilst retrieving version info - %d"

Note mmhalow.c:157-165: when mmwlan_get_version() FAILS, the code logs the error and
then prints the version struct anyway — uninitialised. That is precisely the documented
"looks like dead hardware" symptom (garbage chip id, MAC 00:00:00:00:00:00). So T1 must
assert the *values*, not merely that the lines appeared; a line-presence check would pass
on a dead radio.
"""

from __future__ import annotations

import re
import time
from typing import Optional

from . import manifest as M
from .common import (
    FAIL,
    INCONCLUSIVE,
    PASS,
    SKIP,
    Reporter,
    Result,
    capture_serial,
    go_radio_silent,
    make,
    resolve_port,
)

# Markers, anchored to mmhalow.c (see module docstring).
RE_CHANNEL_LIST = re.compile(r"Setting Channel List (\S+)")
RE_CHIP_ID = re.compile(r"Morse chip ID:\s*(0x[0-9a-fA-F]+)")
RE_FW_VER = re.compile(r"Morse firmware version:\s*(\S+)")
RE_MORSELIB_VER = re.compile(r"Morselib version:\s*(\S+)")
RE_MAC = re.compile(r"Wi-Fi MAC address:\s*([0-9a-fA-F:]{17})")
RE_VER_ERROR = re.compile(r"Error occured whilst retrieving version info")
RE_HELLO_BANNER = re.compile(r"Rimba Phase-1 bring-up")

DEAD_MAC = "00:00:00:00:00:00"


def _t1_apps() -> list[M.App]:
    """Apps T1 will flash.

    Excludes:
      - test-c6-trigger (not in the matrix at all: esp32c6 / standalone).
      - the `test-*` feature apps. Those are T2 harness apps: they self-report via the
        TEST| console contract, NOT via the rimba-hello bring-up banner T1 keys on. A
        radio-free one (test-swccmp) would take T1's non-radio branch and be judged
        against rimba-hello's banner, which only rimba-hello prints — a guaranteed false
        FAIL that would gate a healthy tree. They belong to `make test-t2`, so T1 skips
        them by name. (T0 still builds them; T2 still runs them.)
      - the sleep apps. A sleep/deep-sleep app powers the ESP32-S3 native USB down and
        re-enumerates constantly, so esptool can never land it in download mode: every
        subsequent flash fails "No serial data received", on any port, and a monitor-kill
        does not fix it. Recovery needs a genuine PPK2 power-cycle
        (docs/reference/rimba-bench-devices.md:221-236) — which board0/board1 do not have.
        Flashing these unattended would wedge the bench, so T1 skips them by default and
        says so. Use --include-sleep-apps on board2 (PPK2-recoverable) if you want them.
    """
    return [a for a in M.T0_APPS
            if not a.sleeps and not a.name.startswith("test-")]


def run(board_name: str = "board2", apps: Optional[list[str]] = None,
        include_sleep_apps: bool = False, boot_seconds: float = 12.0,
        quiet: bool = False, keep_radio_on: bool = False) -> Reporter:
    rep = Reporter("T1", quiet=quiet)
    M.require_bench()

    bench = M.BENCH.get(board_name)
    if bench is None:
        rep.add(Result("T1", f"board:{board_name}", FAIL, detail="unknown bench board"))
        return rep

    port = resolve_port(bench.efuse_mac)
    if not port:
        hint = (
            "board2 is powered by the PPK2 DUT rail and only enumerates while "
            "tools/ppk2_hold.py is running -- start it first."
            if not bench.usb_bus_powered
            else f"board is bus-powered via the hub; try: sudo tools/esp_usb_power.py {board_name} cycle 4"
        )
        rep.add(Result("T1", f"{board_name} present", SKIP,
                       detail=f"not enumerated (efuse {bench.efuse_mac}). {hint}"))
        return rep

    rep.add(Result("T1", f"{board_name} present", PASS,
                   detail=f"{port}", evidence=f"/dev/serial/by-id -> {bench.efuse_mac} -> {port}"))

    candidates = _t1_apps()
    if include_sleep_apps:
        candidates = [a for a in M.T0_APPS if not a.name.startswith("test-")]
    if apps:
        candidates = [a for a in M.APPS if a.name in apps]
        # An explicit --app test-* is a wrong-tier request: those apps report via the
        # TEST| contract (T2), not the T1 bring-up banner. Skip, don't false-FAIL.
        for a in list(candidates):
            if a.name.startswith("test-"):
                rep.add(Result("T1", f"{a.name} (wrong tier)", SKIP,
                               detail="test-* apps belong to `make test-t2` (they self-report "
                                      "via the TEST| contract, not T1's bring-up banner)"))
                candidates.remove(a)

    for a in M.APPS:
        if a.sleeps and not include_sleep_apps and (not apps or a.name in apps):
            rep.add(Result("T1", f"{a.name} (skipped)", SKIP,
                           detail="sleep app: flashing it wedges the USB and needs a PPK2 "
                                  "power-cycle to recover. Use --include-sleep-apps on board2."))

    flashed_any = False
    for app in candidates:
        if M.BENCH_BOARD not in app.boards:
            continue
        _smoke_one(rep, app, M.BENCH_BOARD, port, boot_seconds, bench.efuse_mac)
        flashed_any = True

    # The standing rule: nothing left on the air when a test ends.
    if flashed_any and not keep_radio_on:
        for r in go_radio_silent([board_name], M.BENCH, M.IDLE_APP, M.BENCH_BOARD,
                                 verbose=not quiet):
            rep.add(r)

    return rep


def _smoke_one(rep: Reporter, app: M.App, board: str, port: str,
               boot_seconds: float, efuse_mac: str) -> None:
    name = f"{app.name} @ {board}"
    t0 = time.time()

    cp = make("flash", app.name, board, port=port, timeout=600)
    if cp.returncode != 0:
        rep.add(Result("T1", name, FAIL, duration_s=time.time() - t0,
                       detail=f"flash failed (exit {cp.returncode})\n"
                              + "\n".join(cp.stderr.strip().splitlines()[-5:]),
                       evidence=f"make flash APP={app.name} BOARD={board} PORT={port}"))
        return

    try:
        log = capture_serial(port, boot_seconds, efuse_mac=efuse_mac)
    except Exception as e:
        rep.add(Result("T1", name, FAIL, duration_s=time.time() - t0,
                       detail=f"serial capture failed: {e}"))
        return

    dur = time.time() - t0
    ev = _relevant(log)

    if not app.radio:
        # rimba-hello: no radio at all. Assert the banner AND the absence of radio lines
        # (that absence is what "radio-silent" means in the standing rule).
        if RE_HELLO_BANNER.search(log):
            noise = [l for l in log.splitlines() if "mmwlan" in l.lower() or "halow" in l.lower()]
            if noise:
                rep.add(Result("T1", name, FAIL, duration_s=dur,
                               detail=f"radio-free app emitted {len(noise)} HaLow lines",
                               evidence="\n".join(noise[:5])))
            else:
                rep.add(Result("T1", name, PASS, duration_s=dur,
                               detail="banner present, zero HaLow lines", evidence=ev))
        else:
            rep.add(Result("T1", name, FAIL, duration_s=dur,
                           detail="no 'Rimba Phase-1 bring-up' banner within "
                                  f"{boot_seconds}s", evidence=ev))
        return

    problems = []

    # 1. The runtime country code. The "??" trap is the dead-radio mode; a valid-but-WRONG
    #    country (right build, wrong region) is a subtler regression, so assert it matches
    #    what the board overlay configured, not merely that it isn't "??".
    want_cc = _board_country(board)
    m = RE_CHANNEL_LIST.search(log)
    if not m:
        problems.append("no 'Setting Channel List' line -- mmhalow_init never ran")
    elif m.group(1) == "??":
        problems.append(
            "country '??' at runtime -- morselib will refuse the radio "
            "(MMWLAN_CHANNEL_LIST_NOT_SET). The board overlay was not applied."
        )
    elif want_cc and m.group(1) != want_cc:
        problems.append(
            f"country '{m.group(1)}' at runtime, board overlay configures '{want_cc}' -- "
            "right build, wrong regulatory domain."
        )

    # 2. The version call must have SUCCEEDED. Checking this explicitly matters because
    #    mmhalow.c prints the version struct even when the call failed.
    if RE_VER_ERROR.search(log):
        problems.append("'Error occured whilst retrieving version info' -- the radio did "
                        "not boot; any chip id / MAC printed below it is uninitialised")

    # 3. Real chip id.
    m = RE_CHIP_ID.search(log)
    if not m:
        problems.append("no 'Morse chip ID' line")
    elif m.group(1).lower() != M.EXPECTED_CHIP_ID.lower():
        problems.append(f"chip id {m.group(1)}, expected {M.EXPECTED_CHIP_ID} "
                        "(a garbage/zero id here means the radio never booted)")

    # 4. Firmware version pinned across the whole bench.
    m = RE_FW_VER.search(log)
    if not m:
        problems.append("no 'Morse firmware version' line")
    elif M.EXPECTED_FW_VERSION not in m.group(1):
        problems.append(
            f"fw {m.group(1)}, expected {M.EXPECTED_FW_VERSION}. The whole bench "
            "(ESP + Linux nodes) must be matched -- a half-bumped bench reads as "
            "mystery breakage."
        )

    # 5. A real MAC. Flag both a zero MAC (dead-radio signature) AND an absent MAC line,
    #    symmetric with the chip-id / fw / version-error checks above.
    m = RE_MAC.search(log)
    if not m:
        problems.append("no 'Wi-Fi MAC address' line -- the netif never started")
    elif m.group(1).lower() == DEAD_MAC:
        problems.append(f"MAC {DEAD_MAC} -- the classic dead-radio signature")

    if problems:
        rep.add(Result("T1", name, FAIL, duration_s=dur,
                       detail="\n".join(problems), evidence=ev))
    else:
        chip = RE_CHIP_ID.search(log)
        fw = RE_FW_VER.search(log)
        mac = RE_MAC.search(log)
        cc = RE_CHANNEL_LIST.search(log)
        rep.add(Result(
            "T1", name, PASS, duration_s=dur,
            detail=f"country={cc.group(1) if cc else '?'} chip={chip.group(1) if chip else '?'} "
                   f"fw={fw.group(1) if fw else '?'} mac={mac.group(1) if mac else 'n/a'}",
            evidence=ev,
            meta={
                "country": cc.group(1) if cc else None,
                "chip_id": chip.group(1) if chip else None,
                "fw": fw.group(1) if fw else None,
                "mac": mac.group(1) if mac else None,
            },
        ))


_COUNTRY_CACHE: dict[str, Optional[str]] = {}


def _board_country(board: str) -> Optional[str]:
    """The country code the board overlay configures (what the radio SHOULD report)."""
    if board in _COUNTRY_CACHE:
        return _COUNTRY_CACHE[board]
    from .common import BOARDS_DIR

    p = BOARDS_DIR / board / "sdkconfig.defaults"
    val = None
    if p.exists():
        mm = re.search(r'^CONFIG_HALOW_COUNTRY_CODE="([^"]*)"', p.read_text(), re.M)
        if mm:
            val = mm.group(1)
    _COUNTRY_CACHE[board] = val
    return val


def _relevant(log: str, limit: int = 14) -> str:
    """The identifying lines out of a boot log — enough to audit the verdict."""
    keys = ("Channel List", "Morselib version", "Morse firmware", "Morse chip ID",
            "MAC address", "BCF", "Error occured", "Rimba Phase-1")
    hits = [l.rstrip() for l in log.splitlines() if any(k in l for k in keys)]
    return "\n".join(hits[:limit]) if hits else "\n".join(log.splitlines()[-limit:])
