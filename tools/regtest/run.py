#!/usr/bin/env python3
"""Rimba regression suite — CLI entry point.

ALWAYS drive the suite through `make test-*` — never call this script directly. `make` sources the
vendored ESP-IDF env for you (pyserial / ppk2-api), so no `source export.sh` step is needed:

    make test-t0                          # build matrix (no hardware)
    make test-t1 BOARD_NAME=board2        # smoke (BOARD_NAME REQUIRED: board0|board1|board2)
    make test-t2                          # on-air feature tests (TEST="slug ..." for a subset)
    make test-tp AP=esp                   # power-save tier (AP REQUIRED: esp|linux)
    make test-dscycle CYCLES=2            # deep-sleep reconnect gate (CYCLES REQUIRED)
    make test    BOARD_NAME=board2        # t0 + t1
    make test-bench / test-conn / test-silence / test-report / test-interop

Test-run parameters have NO defaults -- you always state the board / AP / cycle count, so a run
never silently picks one for you. Extra knobs are make variables (DRY_RUN=1, APPEND=1, LIGHT_SLEEP=1,
INCLUDE_SLEEP=1, TEST=, LINUX_MAC=/LINUX_IP=). `make help` lists the targets.

Exit code is 0 only when nothing FAILed. SKIP and INCONCLUSIVE do not fail the run:
a skipped tier (no hardware attached) and a noisy RF measurement outside its margin are
both honest non-results, and conflating either with a real regression is how a suite
starts getting ignored.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

#: The reporter's machine-readable verdict line (same contract as the T2 orchestrator).
_RE_RESULT = re.compile(r"TEST\|RESULT\|(PASS|FAIL|INCONCLUSIVE)\|(.*)")

# Allow `python tools/regtest/run.py` as well as `python -m tools.regtest.run`.
if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
    from tools.regtest import manifest as M  # type: ignore
    from tools.regtest import common, t0_build, t1_smoke  # type: ignore
else:
    from . import manifest as M
    from . import common, t0_build, t1_smoke


def cmd_bench(args) -> int:
    """Report what is physically present. Run this first when a tier says SKIP."""
    M.require_bench()
    print("ESP32 bench nodes (identified by efuse MAC, never by ttyACM number):")
    present = 0
    for name, b in M.BENCH.items():
        port = common.resolve_port(b.efuse_mac)
        if port:
            present += 1
        wire = "fully wired" if b.fully_wired else "WAKE+BUSY UNWIRED"
        status = port or "-- not enumerated"
        print(f"  {name:8s} {b.efuse_mac}  {status:20s}  [{wire}]")
        if not port and not b.usb_bus_powered:
            print("           ^ powered by the PPK2 rail: run tools/ppk2_hold.py to enumerate it")
        if not b.fully_wired:
            print("           ^ light-load endpoint / spare ONLY -- never a relay or power-save DUT")
    print(f"\n{present}/{len(M.BENCH)} ESP nodes present.")
    print("\nLinux HaLow interop node (reach by hostname only, never a raw IP):")
    if M.LINUX_NODES:
        for host, node in M.LINUX_NODES.items():
            print(f"  {host:12s} mesh {node.mesh_ip:10s} {node.mesh_mac}  {node.role}")
    else:
        print("  (none configured -- pass LINUX_HOST / LINUX_MAC / LINUX_IP for the interop tests)")
    print(f"\nBench pinned at Morse fw {M.EXPECTED_FW_VERSION}, chip {M.EXPECTED_CHIP_ID}, "
          f"S1G ch{M.RADIO_CHANNEL_S1G} (iw freq {M.RADIO_FREQ_IW}).")
    return 0


def cmd_silence(args) -> int:
    """Return every present ESP to the radio-free idle app."""
    M.require_bench()
    names = args.board_name or [n for n in M.BENCH if common.resolve_port(M.BENCH[n].efuse_mac)]
    if not names:
        print("No ESP nodes enumerated -- nothing to silence.")
        return 0
    print(f"Flashing {M.IDLE_APP} to: {', '.join(names)}")
    results = common.go_radio_silent(names, M.BENCH, M.IDLE_APP, M.BENCH_BOARD)
    failed = [r for r in results if r.status == common.FAIL]
    print("\nReminder: this covers the ESPs only. Any Linux node you brought up still "
          "needs `sudo ip link set wlan1 down`.")
    return 1 if failed else 0


def _write_report() -> None:
    """Refresh the human-readable HTML report from the current baselines."""
    if __package__ in (None, ""):
        from tools.regtest import report  # type: ignore
    else:
        from . import report
    try:
        p = report.generate()
        print(f" html report: {p}")
    except Exception as e:  # a report failure must never fail the run
        print(f" (HTML report skipped: {e})")


def _lint_gate() -> int:
    """Run the pyflakes lint gate over the harness. 0 = clean (or linter absent), non-zero = findings.

    Called automatically before every run-producing command so a bad harness edit (an undefined name that
    py_compile misses) is caught in ~0.1 s, off-bench, instead of 40 min into a run. No opt-out."""
    if __package__ in (None, ""):
        from tools.regtest import lint  # type: ignore
    else:
        from . import lint
    ok, report = lint.check()
    if not ok:
        print("LINT GATE FAILED — fix these harness issues before running (caught off-bench, "
              "not mid-run):\n", flush=True)
        print(report, flush=True)
        print("\n(pyflakes over tools/regtest + tools; py_compile does not catch undefined names. "
              "This gate has no bypass on purpose.)", flush=True)
        return 2
    if report:   # a non-blocking notice, e.g. pyflakes-not-installed
        print(report, flush=True)
    return 0


def cmd_lint(args) -> int:
    """Run the harness lint gate on demand (`make test-lint`)."""
    rc = _lint_gate()
    if rc == 0:
        if __package__ in (None, ""):
            from tools.regtest import lint  # type: ignore
        else:
            from . import lint
        print(f"lint: clean — {len(lint.targets())} harness files, no pyflakes findings.")
    return rc


def cmd_flakes(args) -> int:
    """Print the run-history flake ledger (which tests flipped verdict run-to-run)."""
    if __package__ in (None, ""):
        from tools.regtest import ledger  # type: ignore
    else:
        from . import ledger
    print(ledger.format_report())
    return 0


def cmd_trend(args) -> int:
    """Trend a recorded numeric metric over runs, or diff it across two halow gitlinks (the 'numbers that
    drift' view: a metric that moves while the test still PASSes)."""
    if __package__ in (None, ""):
        from tools.regtest import ledger  # type: ignore
    else:
        from . import ledger
    if args.diff:
        print(ledger.format_diff(args.diff[0], args.diff[1]))
    else:
        print(ledger.format_trend(name=args.test, last=args.last))
    return 0


def cmd_t0(args) -> int:
    print("T0 -- build matrix (every app x board via make; asserts the country code)\n")
    rep = t0_build.run(apps=args.app, boards=args.board)
    path = rep.finalize()   # final write: marks the run complete
    print(f"\n{rep.summary()}\n baseline: {path}")
    _write_report()
    return 0 if rep.green else 1


def cmd_t1(args) -> int:
    print(f"T1 -- smoke on {args.board_name} (flash + boot + radio up + country)\n")
    rep = t1_smoke.run(
        board_name=args.board_name,
        apps=args.app,
        include_sleep_apps=args.include_sleep_apps,
        keep_radio_on=args.keep_radio_on,
    )
    path = rep.finalize()   # final write: marks the run complete
    print(f"\n{rep.summary()}\n baseline: {path}")
    _write_report()
    return 0 if rep.green else 1


def cmd_t2(args) -> int:
    # Imported lazily (it pulls in the rig helpers). Support both `python run.py`
    # (no package parent) and `python -m tools.regtest.run`.
    if __package__ in (None, ""):
        from tools.regtest import t2_onair  # type: ignore
    else:
        from . import t2_onair

    print("T2 -- on-air feature tests\n")
    rep = t2_onair.run(tests=args.test, dry_run=args.dry_run, append=args.append,
                       linux_mac=args.linux_mac, linux_ip=args.linux_ip)
    if args.dry_run:
        # A dry run must NOT touch the baseline -- writing here would clobber the real results with
        # the dry run's empty ones (which then poisons a later --append seed).
        print(f"\n{rep.summary()}")
        return 0
    path = rep.finalize()   # final write: marks the run complete
    print(f"\n{rep.summary()}\n baseline: {path}")
    _write_report()
    return 0 if rep.green else 1


def cmd_tp(args) -> int:
    """tp — the PPK2 power-save tier: measure board2's PS-ladder current, gate a gross regression."""
    if __package__ in (None, ""):
        from tools.regtest import tp_power  # type: ignore
    else:
        from . import tp_power

    print("tp -- PPK2 power-save ladder\n")
    rep = tp_power.run(ap=args.ap, dry_run=args.dry_run, append=args.append,
                       light_sleep=args.light_sleep)
    if args.dry_run:
        print(f"\n{rep.summary()}")   # dry run must not write/clobber the baseline
        return 0
    path = rep.finalize()   # final write: marks the run complete
    print(f"\n{rep.summary()}\n baseline: {path}")
    _write_report()
    return 0 if rep.green else 1


def cmd_dscycle(args) -> int:
    """The deep-sleep duty-cycle reconnect gate: N deep-sleep→C6-wake→reassoc cycles on board2."""
    if __package__ in (None, ""):
        from tools.regtest import dscycle  # type: ignore
    else:
        from . import dscycle

    print("dscycle -- deep-sleep duty-cycle reconnect gate\n")
    rep = dscycle.run(cycles=args.cycles, append=not args.no_append)
    path = rep.finalize()   # final write: marks the run complete
    print(f"\n{rep.summary()}\n baseline: {path}")
    _write_report()
    return 0 if rep.green else 1


def cmd_preflight(args) -> int:
    """Check you can reach a device (a port / MAC / bench node / ssh host) before a test."""
    if __package__ in (None, ""):
        from tools.regtest import preflight  # type: ignore
    else:
        from . import preflight
    return preflight.run(args)


def cmd_report(args) -> int:
    """Regenerate the HTML report from the existing baselines (no run)."""
    if __package__ in (None, ""):
        from tools.regtest import report  # type: ignore
    else:
        from . import report
    p = report.generate()
    print(f"HTML report written: {p}")
    return 0


def cmd_flash_interop(args) -> int:
    """Flash test-mesh-linux at a Linux node whose MAC is queried live over ssh.

    You pass the node's ssh host and the board's serial port; the peer MAC (and mesh IP) are read
    off the node, so you never look them up. With --run it also brings the node onto the
    rimba-smesh mesh first, captures the board's TEST verdict, and takes both sides off the air.
    """
    if __package__ in (None, ""):
        from tools.regtest import linux_peer  # type: ignore
    else:
        from . import linux_peer

    M.require_bench()   # needs BENCH_BOARD to build/flash test-mesh-linux
    host, port = args.host, args.port

    # With --run, bring the node up first so wlan1 carries a MAC + the mesh IP before we query.
    teardown = None
    if args.run:
        print(f"Bringing {host} up on the rimba-smesh mesh over ssh...")
        ok, detail, teardown = linux_peer.bring_up_mesh(host)
        print(f"  {detail}")
        if not ok:
            print("Linux bring-up failed -- aborting (the ESP would have no peer).")
            return 1

    # Query the peer MAC live -- this is the lookup the manual --linux-mac step replaced.
    mac = args.linux_mac or linux_peer.live_wlan1_mac(host)
    if not mac:
        print(f"Could not read {host}'s wlan1 MAC over ssh -- is `ssh {host}` working and wlan1 "
              f"present? Pass --linux-mac to bypass the query.")
        if teardown:
            teardown()
        return 1

    # Mesh IP: explicit > the node's live wlan1 IPv4 (accurate once the mesh is up) > its manifest
    # entry. No MAC-derived guess -- that convention is for ESP nodes and would be wrong for a Linux
    # node, and a silently-wrong ping target is worse than an honest ask for --ip.
    ip, ip_src = args.ip, "supplied"
    if not ip:
        ip, ip_src = linux_peer.live_wlan1_ipv4(host), "queried over ssh"
    if not ip:
        node = M.LINUX_NODES.get(host)
        ip, ip_src = (node.mesh_ip, "from manifest") if node else (None, None)
    if not ip:
        print(f"Could not determine {host}'s mesh IP (wlan1 has no IPv4 and it is not in the "
              f"manifest). Pass --ip, or use --run so the mesh (and its IP) is brought up first.")
        if teardown:
            teardown()
        return 1

    mac_src = "supplied" if args.linux_mac else "queried over ssh"
    print(f"Peer {host}: MAC {mac} ({mac_src}), mesh IP {ip} ({ip_src})")
    print(f"Flashing test-mesh-linux to {port} against that peer...")
    cp = common.make("flash", "test-mesh-linux", M.BENCH_BOARD, port=port, timeout=600,
                     make_vars={"LINUX_MAC": mac, "LINUX_IP": ip})
    if cp.returncode != 0:
        print(f"Flash failed (exit {cp.returncode}):")
        print("\n".join(cp.stderr.strip().splitlines()[-5:]))
        if teardown:
            teardown()
        return 1
    print("Flashed.")

    rc = 0
    if args.run or args.monitor:
        print("Capturing the board's TEST output...")
        # Budget: ~15s boot + up to PEER_WAIT_S(50) peering + ~27s pinging. Match the T2
        # orchestrator's REPORTER_TIMEOUT_S so a slow-but-valid peering isn't cut off.
        log, _ = common.capture_until(port, 130.0, "TEST|END")
        for line in log.splitlines():
            if line.startswith("TEST|"):
                print("  " + line.rstrip())
        m = _RE_RESULT.search(log)
        if m:
            print(f"\nVerdict: {m.group(1)} -- {m.group(2)}")
            rc = 0 if m.group(1) in ("PASS", "INCONCLUSIVE") else 1
        else:
            print("\nNo TEST|RESULT within the window (the reporter hung or never ran).")
            rc = 1

    if args.run:
        print("\nReturning both sides to radio-silent...")
        common.make("flash", M.IDLE_APP, M.BENCH_BOARD, port=port, timeout=600)
        if teardown:
            teardown()
        print("Bench silent.")
    else:
        print(f"\nNote: the ESP is now on the air. Silence it with:  "
              f"make flash APP={M.IDLE_APP} BOARD={M.BENCH_BOARD} PORT={port}")
        print(f"      and the Linux node with:  ssh {host} 'sudo ip link set wlan1 down'")
    return rc


def cmd_all(args) -> int:
    rc = cmd_t0(args)
    print()
    rc |= cmd_t1(args)
    return rc


def main() -> int:
    p = argparse.ArgumentParser(
        prog="regtest",
        description="Rimba regression suite. Tiers: T0 build / T1 smoke / T2 on-air.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    sub = p.add_subparsers(dest="cmd", required=True)

    p0 = sub.add_parser("t0", help="build matrix (no hardware)")
    p0.add_argument("--app", action="append", help="limit to these apps (repeatable)")
    p0.add_argument("--board", action="append", help="limit to these boards (repeatable)")
    p0.set_defaults(func=cmd_t0)

    p1 = sub.add_parser("t1", help="smoke: flash + boot + radio up (needs a board)")
    p1.add_argument("--board-name", required=True,
                    help="bench node to smoke on: board0 | board1 | board2 (board2 is the only "
                         "fully-wired one). REQUIRED -- no default, so the board is always explicit.")
    p1.add_argument("--app", action="append", help="limit to these apps (repeatable)")
    p1.add_argument("--include-sleep-apps", action="store_true",
                    help="also flash the sleep apps. These wedge the USB and need a PPK2 "
                         "power-cycle to recover -- board2 only.")
    p1.add_argument("--keep-radio-on", action="store_true",
                    help="do NOT return the board to the idle app afterwards (debugging only; "
                         "leaves the bench on the air, against the standing rule)")
    p1.set_defaults(func=cmd_t1)

    p2 = sub.add_parser("t2", help="on-air feature tests (needs a rig)")
    p2.add_argument("--test", action="append", help="limit to these test slugs (repeatable)")
    p2.add_argument("--dry-run", action="store_true",
                    help="print each test's rig + expectations without touching hardware")
    p2.add_argument("--append", action="store_true",
                    help="accumulate onto the existing T2 baseline instead of replacing it "
                         "(build a full-suite report across several short runs; a re-run of the "
                         "same test supersedes its old result)")
    p2.add_argument("--linux-mac",
                    help="the Linux interop peer's wlan1 MAC (aa:bb:cc:dd:ee:ff). The reporter "
                         "firmware is (re)built against it. Default: the manifest LINUX_NODES entry "
                         "for the test's linux:<host> role.")
    p2.add_argument("--linux-ip",
                    help="the Linux interop peer's mesh IP (default: its LINUX_NODES entry).")
    p2.set_defaults(func=cmd_t2)

    pp = sub.add_parser("tp", help="PPK2 power-save tier (needs board2 + the PPK2 + the C6 + an AP)")
    pp.add_argument("--ap", choices=("esp", "linux"), required=True,
                    help="the AP the STA ladder associates to: esp (test-apsta-ap on board0, "
                         "fully automated) or linux (hostapd_s1g on chronite, the authoritative "
                         "reference). Bands are per-AP. REQUIRED -- no default, so the AP is always explicit.")
    pp.add_argument("--dry-run", action="store_true",
                    help="print the rig + tiers + bands without touching hardware")
    pp.add_argument("--append", action="store_true",
                    help="accumulate onto the existing TP baseline (e.g. run both AP paths)")
    pp.add_argument("--light-sleep", action="store_true",
                    help="build the DUT with HOST_LIGHT_SLEEP=1 and score against the light-sleep "
                         "bands — a STRONGER secondary gate (light-sleep backfires ~3-4x on 1.17.9)")
    pp.set_defaults(func=cmd_tp)

    pd = sub.add_parser("dscycle",
                        help="deep-sleep duty-cycle reconnect gate (board2 + the C6 + an AP)")
    pd.add_argument("--cycles", type=int, required=True,
                    help="deep-sleep→wake→reassoc cycles to require. REQUIRED -- no default; the rig's "
                         "C6-wake is unreliable so the cadence is ~130 s/cycle (more cycles = a longer run)")
    pd.add_argument("--no-append", action="store_true",
                    help="write a fresh T2 baseline instead of seeding from the existing one")
    pd.set_defaults(func=cmd_dscycle)

    pa = sub.add_parser("all", help="t0 + t1")
    pa.add_argument("--app", action="append")
    pa.add_argument("--board", action="append")
    pa.add_argument("--board-name", required=True,
                    help="bench node for the T1 half: board0 | board1 | board2. REQUIRED -- no default.")
    pa.add_argument("--include-sleep-apps", action="store_true")
    pa.add_argument("--keep-radio-on", action="store_true")
    pa.set_defaults(func=cmd_all)

    pb = sub.add_parser("bench", help="what hardware is present right now")
    pb.set_defaults(func=cmd_bench)

    pfl = sub.add_parser("preflight",
                         help="check you can reach a device before a test (a port / MAC / node / ssh host)")
    pfl.add_argument("--port", help="a serial port to check (e.g. /dev/ttyACM0)")
    pfl.add_argument("--mac", help="an ESP efuse MAC; its port is resolved via /dev/serial/by-id")
    pfl.add_argument("--node", action="append", help="a bench node name from the manifest (repeatable)")
    pfl.add_argument("--host", action="append", help="a Linux node ssh host to check (repeatable)")
    pfl.add_argument("--chip", default="esp32s3",
                     help="the ESP chip target to expect (default: esp32s3, the whole bench)")
    pfl.set_defaults(func=cmd_preflight)

    ps = sub.add_parser("silence", help="return every ESP to the radio-free idle app")
    ps.add_argument("--board-name", action="append", help="limit to these boards")
    ps.set_defaults(func=cmd_silence)

    pr = sub.add_parser("report", help="(re)generate the HTML report from the latest baselines")
    pr.set_defaults(func=cmd_report)

    pl = sub.add_parser("lint", help="pyflakes lint gate over the harness (no hardware) — runs "
                                     "automatically before every tier too")
    pl.set_defaults(func=cmd_lint)

    pfk = sub.add_parser("flakes", help="run-history flake ledger: which tests flip verdict run-to-run")
    pfk.set_defaults(func=cmd_flakes)

    ptr = sub.add_parser("trend", help="trend a recorded numeric metric over runs, or diff it across "
                                       "two halow gitlinks (numbers that drift while a test still passes)")
    ptr.add_argument("--test", help="limit to one test slug/name")
    ptr.add_argument("--last", type=int, default=8, help="recent runs to show per metric (default 8)")
    ptr.add_argument("--diff", nargs=2, metavar=("GITLINK_A", "GITLINK_B"),
                     help="compare each metric's value at halow gitlink A vs B (SHA prefixes accepted)")
    ptr.set_defaults(func=cmd_trend)

    pf = sub.add_parser("flash-interop",
                        help="flash test-mesh-linux at a Linux node, MAC auto-queried over ssh")
    pf.add_argument("--host", required=True,
                    help="ssh host of the Linux node (reachable as `ssh <host>`); its wlan1 MAC is "
                         "read live, so you don't look it up")
    pf.add_argument("--port", required=True,
                    help="the ESP board's serial port to flash (e.g. /dev/ttyACM0)")
    pf.add_argument("--ip",
                    help="peer mesh IP to compile in (default: the node's live wlan1 IPv4, else its "
                         "manifest entry, else derived from the MAC)")
    pf.add_argument("--linux-mac",
                    help="skip the live query and use this MAC (e.g. the node is off but you know it)")
    pf.add_argument("--run", action="store_true",
                    help="also bring the node onto the rimba-smesh mesh first, capture the board's "
                         "verdict, and take both sides off the air afterwards (the full loop)")
    pf.add_argument("--monitor", action="store_true",
                    help="capture + print the board's TEST output after flashing (implied by --run)")
    pf.set_defaults(func=cmd_flash_interop)

    args = p.parse_args()
    # Automatic off-bench gate: lint the harness before any command that produces results / touches the
    # bench, so an undefined-name bug fails in ~0.1 s instead of mid-run. Cheap enough to always run.
    _RUN_PRODUCING = {"t0", "t1", "t2", "tp", "dscycle", "all", "flash-interop"}
    if args.cmd in _RUN_PRODUCING:
        rc = _lint_gate()
        if rc:
            return rc
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
