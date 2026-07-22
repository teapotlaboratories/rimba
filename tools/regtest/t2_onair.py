"""T2 — on-air feature tests, with a multi-role orchestrator.

Each feature has a firmware app (or a set of role apps) that runs the feature and
self-reports a verdict on the console via the TEST| contract
(firmware/test-common/include/test_report.h). This module resolves each role to a
bench device, flashes it, boots the roles in order, scrapes the REPORTER role's verdict,
and returns every board it touched to rimba-hello.

Three shapes of test:
  - single-board deterministic (test-swccmp): flash one board, scrape its verdict.
  - multi-role orchestrated (ap-sta-ping): flash a support role (the AP), verify it came
    up, then flash + capture the reporter role (the STA).
  - defined-but-not-automated: reported SKIP with the concrete blocker.

WHY THE VERDICT LIVES IN THE FIRMWARE: only the device can see association / plink /
peer-table / crypto state. A host-side scraper guessing from log prose is the exact
"endpoint logs are not proof" anti-pattern the project already has a rule about. The
firmware asserts; the orchestrator transports and combines.
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
    capture_until,
    go_radio_silent,
    make,
    resolve_port,
)
from .t2_tests import T2_BY_SLUG, T2_TESTS

RE_RESULT = re.compile(r"TEST\|RESULT\|(PASS|FAIL|INCONCLUSIVE)\|(.*)")
RE_STEP = re.compile(r"TEST\|STEP\|(\S+?)\|(PASS|FAIL)\|(.*)")

#: How long to wait for a reporter's TEST|RESULT after flashing it. Covers connect +
#: the in-app measurement window. The mesh multi-hop tests are the long pole (peering can take
#: ~30-45 s cold + HWMP path discovery + a ping window), so this is generous; capture_until
#: returns as soon as it sees TEST|END, so a high cap only costs time on an actual hang.
REPORTER_TIMEOUT_S = 130.0


def run(tests: Optional[list[str]] = None, dry_run: bool = False,
        quiet: bool = False, append: bool = False,
        linux_mac: Optional[str] = None, linux_ip: Optional[str] = None) -> Reporter:
    rep = Reporter("T2", quiet=quiet, persist=not dry_run)
    if append and not dry_run:
        # Accumulate onto the existing baseline (build a full-suite report across short runs,
        # since the environment can reap a long single run). Seeded results carry forward;
        # a re-run of the same test name supersedes its old result.
        import json
        from .common import RESULTS_DIR
        prior = RESULTS_DIR / "T2-latest.json"
        if prior.exists():
            try:
                rep.seed_from(json.loads(prior.read_text()).get("results", []))
            except Exception:
                pass

    selected = [t for t in T2_TESTS if not tests or t.slug in tests]
    if tests:
        for unknown in sorted(set(tests) - set(T2_BY_SLUG)):
            rep.add(Result("T2", unknown, FAIL, detail="no such T2 test slug"))

    if dry_run:
        _describe(selected, linux_mac=linux_mac, linux_ip=linux_ip)
        return rep

    M.require_bench()
    # Only demand the Linux interop node if a selected test actually uses one.
    if not linux_mac and any(r.device.startswith("linux:") for t in selected for r in t.roles):
        M.require_linux()

    used_boards: set[str] = set()
    try:
        for t in selected:
            try:
                _dispatch(rep, t, used_boards, linux_mac=linux_mac, linux_ip=linux_ip)
            except Exception as e:
                # A bench transient -- e.g. a PPK2-rail board2 re-enumeration invalidating a cached
                # /dev/ttyACM* so a serial open raises past the capture retry -- must fail THIS test, not
                # crash the whole tier and skip every test after it. INCONCLUSIVE (a rig hiccup, not a code
                # regression); the flake ledger records it so the underlying rate stays visible.
                rep.add(Result("T2", t.slug, INCONCLUSIVE,
                               detail=f"harness/bench transient: {type(e).__name__}: {e}"[:300]))
    finally:
        # Always leave the bench silent, even if a test raised.
        if used_boards:
            for r in go_radio_silent(sorted(used_boards), M.BENCH, M.IDLE_APP,
                                     M.BENCH_BOARD, verbose=not quiet):
                rep.add(r)
    return rep


def _dispatch(rep: Reporter, t, used_boards: set[str],
              linux_mac: Optional[str] = None, linux_ip: Optional[str] = None) -> None:
    """Route a test to the right runner: orchestrated, single-board, or skip."""
    if t.automated:
        # Every role app must exist (not just be declared) before we try to run.
        missing = [r.app for r in t.roles
                   if r.app and r.app not in M.APPS_BY_NAME]
        if missing:
            rep.add(Result("T2", t.slug, SKIP,
                           detail=f"role app(s) not in the manifest: {missing}"))
            return
        _run_orchestrated(rep, t, used_boards, linux_mac=linux_mac, linux_ip=linux_ip)
        return

    blocker = M.T2_NOT_IMPLEMENTED.get(t.slug)
    if blocker:
        rep.add(Result("T2", t.slug, SKIP,
                       detail=f"NOT IMPLEMENTED: {blocker}",
                       evidence=f"defined in tools/regtest/t2_tests.py; rig={t.rig}; "
                                f"README: firmware/{t.app}/README.md",
                       meta={"defined": True, "implemented": False}))
        return

    # A single-app test that has an app but no roles (e.g. test-swccmp).
    if t.app in M.APPS_BY_NAME:
        _run_single(rep, t, used_boards)
    else:
        rep.add(Result("T2", t.slug, SKIP, detail="no roles and no single-board app"))


# ---------------------------------------------------------------------------
# Single-board deterministic tests (test-swccmp)
# ---------------------------------------------------------------------------


def _run_single(rep: Reporter, t, used_boards: set[str]) -> None:
    t0 = time.time()
    # These are radio-free / single-DUT; any present board works, prefer board2 then 0/1.
    board_name = None
    for cand in ("board2", "board0", "board1"):
        if resolve_port(M.BENCH[cand].efuse_mac):
            board_name = cand
            break
    if not board_name:
        rep.add(Result("T2", t.slug, SKIP, detail="no ESP board enumerated"))
        return

    port = resolve_port(M.BENCH[board_name].efuse_mac)
    cp = make("flash", t.app, M.BENCH_BOARD, port=port, timeout=600)
    if cp.returncode != 0:
        rep.add(Result("T2", t.slug, FAIL, duration_s=time.time() - t0,
                       detail=f"flash failed (exit {cp.returncode})\n"
                              + "\n".join(cp.stderr.strip().splitlines()[-4:])))
        return
    used_boards.add(board_name)
    log, _ = capture_until(port, 20.0, "TEST|END", efuse_mac=M.BENCH[board_name].efuse_mac)
    _record_verdict(rep, t.slug, log, time.time() - t0, {"board": board_name})


# ---------------------------------------------------------------------------
# Multi-role orchestration
# ---------------------------------------------------------------------------


def _run_orchestrated(rep: Reporter, t, used_boards: set[str],
                      linux_mac: Optional[str] = None, linux_ip: Optional[str] = None) -> None:
    t0 = time.time()

    # One OR MORE reporter roles. Most tests have exactly one; a multi-STA test (e.g. multi-twt)
    # has several, each an ESP whose TEST|RESULT is captured and AND-ed into the verdict.
    reporters = [r for r in t.roles if r.reporter]
    if not reporters:
        rep.add(Result("T2", t.slug, FAIL, detail="rig has no reporter role (a bug in the "
                                                   "test definition)"))
        return

    # 1. Resolve every role to a concrete device; bail (SKIP) if anything is missing.
    assign: dict[str, tuple[str, str]] = {}   # role.name -> (kind, handle)
    esp_used: set[str] = set()
    for role in t.roles:
        kind, handle, why = _resolve_role(role, esp_used)
        if kind is None:
            rep.add(Result("T2", t.slug, SKIP, duration_s=time.time() - t0,
                           detail=f"role '{role.name}': {why}",
                           evidence=f"rig: {t.rig}"))
            return
        assign[role.name] = (kind, handle)
        if kind == "esp":
            esp_used.add(handle)

    # 1b. Build-time arguments, keyed by the app they target. Every ESP flash of that app (support
    # AND reporter) gets them, so all boards running a symmetric self-selecting app (e.g. the 3-node
    # relay line) agree on the topology without any MAC hardcoded in firmware.
    build_vars_by_app = _resolve_build_vars(t, assign, linux_mac, linux_ip)

    linux_teardowns: list = []
    try:
        # 2. Bring up support roles first, in declaration order.
        for role in t.roles:
            if role.reporter:
                continue
            kind, handle = assign[role.name]
            if kind == "esp":
                ok, detail = _bring_up_esp_support(
                    role, handle, used_boards,
                    make_vars=build_vars_by_app.get(role.app))
            else:
                ok, detail, td = _bring_up_linux(role, handle)
                if td:
                    linux_teardowns.append(td)
            if not ok:
                rep.add(Result("T2", t.slug, FAIL, duration_s=time.time() - t0,
                               detail=f"support role '{role.name}' ({handle}) did not come "
                                      f"up: {detail}"))
                return

        # 3. Flash + capture EVERY reporter, in declaration order. For a multi-STA test the first
        # reporter stays associated (park_forever) while the next is flashed, so by the last capture
        # all STAs coexist on the AP -- the concurrency the multi-STA gate needs.
        captures = []   # (role, handle, log)
        for reporter in reporters:
            kind, handle = assign[reporter.name]
            if kind != "esp":
                rep.add(Result("T2", t.slug, SKIP, duration_s=time.time() - t0,
                               detail="a Linux reporter role is not supported yet (the verdict "
                                      "must come from an ESP TEST| line)"))
                return
            port = resolve_port(M.BENCH[handle].efuse_mac)
            make_vars = build_vars_by_app.get(reporter.app)
            cp = make("flash", reporter.app, M.BENCH_BOARD, port=port, timeout=600,
                      make_vars=make_vars or None)
            if cp.returncode != 0:
                rep.add(Result("T2", t.slug, FAIL, duration_s=time.time() - t0,
                               detail=f"reporter '{reporter.name}' ({handle}) flash failed "
                                      f"(exit {cp.returncode})\n"
                                      + "\n".join(cp.stderr.strip().splitlines()[-4:])))
                return
            used_boards.add(handle)
            log, _ = capture_until(port, REPORTER_TIMEOUT_S, "TEST|END",
                                   efuse_mac=M.BENCH[handle].efuse_mac)
            captures.append((reporter, handle, log))

        meta = {"reporters": [f"{r.name}@{h}" for r, h, _ in captures],
                "roles": {r.name: assign[r.name][1] for r in t.roles}}
        if len(captures) == 1:
            r0, _, log0 = captures[0]
            mv = build_vars_by_app.get(r0.app)
            if mv:
                meta["build_args"] = mv
            _record_verdict(rep, t.slug, log0, time.time() - t0, meta)
        else:
            _record_multi_verdict(rep, t.slug, captures, time.time() - t0, meta)
    finally:
        for td in linux_teardowns:
            try:
                td()
            except Exception:
                pass


def _resolve_build_vars(t, assign: dict, linux_mac: Optional[str],
                        linux_ip: Optional[str]) -> dict[str, dict]:
    """Compute the build-time args for a test, keyed by the app they target.

    Two sources, both derived from the manifest so no MAC/IP is hardcoded in firmware:
      (a) a Linux interop reporter needs the peer's MAC/IP (from LINUX_NODES, or a
          --linux-mac/--linux-ip override) so the ESP can recognise + ping it;
      (b) a symmetric self-selecting app (roles carrying `build_mac_var`, e.g. the 3-node
          relay line) needs every participant's mesh MAC, so each board knows the whole
          topology. Each such role contributes its ASSIGNED board's mesh_mac, and the full
          set is applied to every flash of that app.
    """
    by_app: dict[str, dict] = {}

    def add(app, key, val):
        if app and val:
            by_app.setdefault(app, {})[key] = val

    # (a) Linux MESH interop peer -> the reporter app (needs the peer MAC/IP to recognise + ping
    # it). A hostapd-ap Linux role (twt-assoc) is NOT an interop peer -- the STA associates by SSID
    # and needs no MAC/IP -- so it is excluded here (no unused -D on that reporter's build).
    reporter = t.reporter_role
    linux_role = next((r for r in t.roles if r.device.startswith("linux:")
                       and r.linux_setup == "mesh-peer"), None)
    if linux_role is not None and reporter is not None:
        host = M.DEFAULT_LINUX_PEER
        node = M.LINUX_NODES.get(host)
        add(reporter.app, "LINUX_MAC", linux_mac or (node.mesh_mac if node else None))
        add(reporter.app, "LINUX_IP", linux_ip or (node.mesh_ip if node else None))

    # (b) Symmetric MAC-topology roles -> their shared app (all get all the MACs).
    for role in t.roles:
        if role.build_mac_var:
            kind, handle = assign[role.name]
            board = M.BENCH.get(handle) if kind == "esp" else None
            if board is not None:
                add(role.app, role.build_mac_var, board.mesh_mac)

    return by_app


def _resolve_role(role, esp_used: set[str]):
    """(kind, handle, why). kind in {'esp','linux',None}. None => cannot run (why explains).

    An unreachable Linux node is a bench-availability issue (like a missing ESP board), so it
    resolves to None -> the caller SKIPs the test, not FAILs it.
    """
    if role.device.startswith("linux:"):
        host = M.DEFAULT_LINUX_PEER
        from . import linux_peer
        if not linux_peer.reachable(host):
            return None, None, (f"Linux node {host} not reachable over ssh -- the interop test "
                                "needs it on the bench (a bench-availability SKIP, not a failure)")
        return "linux", host, ""

    # ESP role: pick the device.
    if role.device == "any-esp":
        for cand in ("board2", "board0", "board1"):
            if cand not in esp_used and resolve_port(M.BENCH[cand].efuse_mac):
                cand_ok = (not role.require_wired) or M.BENCH[cand].fully_wired
                if cand_ok:
                    return "esp", cand, ""
        return None, None, "no free/present ESP board matches this role"

    board = M.BENCH.get(role.device)
    if board is None:
        return None, None, f"unknown device '{role.device}'"
    if role.require_wired and not board.fully_wired:
        return None, None, (f"{role.device} has WAKE+BUSY unwired but this role needs a "
                            f"fully-wired board (use board2) -- refusing to produce a false bug")
    if not resolve_port(board.efuse_mac):
        hint = ("; board2 needs tools/ppk2_hold.py running" if not board.usb_bus_powered else "")
        return None, None, f"{role.device} not enumerated{hint}"
    return "esp", role.device, ""


def _bring_up_esp_support(role, board_name: str, used_boards: set[str],
                          make_vars: Optional[dict] = None) -> tuple[bool, str]:
    """Flash a support ESP role and confirm it started (via its up_marker, if any)."""
    port = resolve_port(M.BENCH[board_name].efuse_mac)
    cp = make("flash", role.app, M.BENCH_BOARD, port=port, timeout=600,
              make_vars=make_vars or None)
    if cp.returncode != 0:
        return False, f"flash exit {cp.returncode}: " + "; ".join(
            cp.stderr.strip().splitlines()[-2:])
    used_boards.add(board_name)

    if role.up_marker:
        log, matched = capture_until(port, role.boot_wait_s, role.up_marker,
                                     efuse_mac=M.BENCH[board_name].efuse_mac)
        if not matched:
            tail = "\n".join(l.rstrip() for l in log.splitlines()[-6:])
            return False, (f"up-marker {role.up_marker!r} not seen within "
                           f"{role.boot_wait_s:.0f}s\n{tail}")
        return True, f"up ({role.up_marker!r} seen)"
    # No marker to check: just give it time to boot.
    time.sleep(role.boot_wait_s)
    return True, "booted (no up-marker to verify)"


def _bring_up_linux(role, host: str):
    """Bring up a Linux peer over ssh. Returns (ok, detail, teardown_callable_or_None).

    v1 supports linux_setup == 'mesh-peer' (the documented wpa_supplicant_s1g mesh recipe).
    The recipe is runtime-only and does NOT survive a reboot, so it is (re)applied here.
    NOTE: this half is scaffolding for the mesh T2 tests; those also need board2 as the
    reporter, so they stay SKIP until that hardware is present. It is implemented so that
    enabling a mesh test is declarative, not a rewrite.
    """
    from . import linux_peer

    if role.linux_setup == "mesh-peer":
        return linux_peer.bring_up_mesh(host)
    if role.linux_setup == "hostapd-ap":
        # A Linux hostapd_s1g SoftAP (rimba-ping/SAE/dtim1/PMF/ch27) the ESP STA associates to --
        # used by twt-assoc to prove the assoc-embedded path on a REAL Linux AP.
        return linux_peer.bring_up_ap(host)
    return False, f"unknown linux_setup {role.linux_setup!r}", None


# ---------------------------------------------------------------------------
# Verdict parsing
# ---------------------------------------------------------------------------


def _record_verdict(rep: Reporter, slug: str, log: str, dur: float, meta: dict) -> None:
    lines = [l.rstrip() for l in log.splitlines() if l.startswith("TEST|")]
    ev = "\n".join(lines) or "\n".join(log.splitlines()[-12:])
    verdict = RE_RESULT.search(log)
    failed_steps = [m.group(1) for m in RE_STEP.finditer(log) if m.group(2) == "FAIL"]

    if not verdict:
        rep.add(Result("T2", slug, FAIL, duration_s=dur,
                       detail="no TEST|RESULT line within the window -- the reporter hung, "
                              "crashed, or never ran. Silence is not a pass."
                              + (f" [failed steps: {', '.join(failed_steps)}]" if failed_steps else ""),
                       evidence=ev, meta=meta))
        return

    status = {"PASS": PASS, "FAIL": FAIL, "INCONCLUSIVE": INCONCLUSIVE}[verdict.group(1)]
    rep.add(Result("T2", slug, status, duration_s=dur,
                   detail=verdict.group(2)
                          + (f"  [failed steps: {', '.join(failed_steps)}]" if failed_steps else ""),
                   evidence=ev, meta=meta))


def _record_multi_verdict(rep: Reporter, slug: str, captures: list, dur: float, meta: dict) -> None:
    """Combine several reporters' verdicts into ONE Result (a multi-STA test).

    PASS iff EVERY reporter PASSed; FAIL if any FAILed or emitted no RESULT (silence is not a pass);
    else INCONCLUSIVE. Each reporter's own verdict is kept in the detail + evidence so a failure
    names which STA fell over.
    """
    per, all_lines = [], []
    for role, handle, log in captures:
        all_lines.append(f"--- {role.name}@{handle} ---")
        all_lines.extend(l.rstrip() for l in log.splitlines() if l.startswith("TEST|"))
        m = RE_RESULT.search(log)
        per.append((role.name, handle, m.group(1) if m else None))
    statuses = [s for _, _, s in per]
    if any(s is None or s == "FAIL" for s in statuses):
        status = FAIL
    elif any(s == "INCONCLUSIVE" for s in statuses):
        status = INCONCLUSIVE
    else:
        status = PASS
    n_pass = sum(1 for s in statuses if s == "PASS")
    summary = "; ".join(f"{n}@{h}={s or 'NO-RESULT'}" for n, h, s in per)
    rep.add(Result("T2", slug, status, duration_s=dur,
                   detail=f"{n_pass}/{len(per)} reporters INSTALLED concurrently: {summary}",
                   evidence="\n".join(all_lines), meta=meta))


def _describe(tests, linux_mac: Optional[str] = None, linux_ip: Optional[str] = None) -> None:
    """--dry-run: print the catalogue without touching hardware."""
    for t in tests:
        impl = M.T2_NOT_IMPLEMENTED.get(t.slug)
        status = ("automated" if t.automated
                  else f"NOT AUTOMATED — {impl}" if impl else "single-board")
        print(f"\n=== test-{t.slug} — {t.title}")
        print(f"    status : {status}")
        if t.automated:
            print("    roles  :")
            for r in t.roles:
                tag = " [REPORTER]" if r.reporter else ""
                appt = f" app={r.app}" if r.app else ""
                wired = " (must be fully-wired)" if r.require_wired else ""
                print(f"               {r.name:8s} {r.device}{appt}{wired}{tag}")
            # Only a MESH interop reporter builds against the peer's MAC/IP; a hostapd-ap Linux
            # role (twt-assoc) is a plain AP the STA associates to by SSID.
            lr = next((r for r in t.roles if r.device.startswith("linux:")
                       and r.linux_setup == "mesh-peer"), None)
            if lr is not None:
                host = M.DEFAULT_LINUX_PEER
                node = M.LINUX_NODES.get(host)
                mac = linux_mac or (node.mesh_mac if node else "?")
                ip = linux_ip or (node.mesh_ip if node else "?")
                src = ("--linux-mac/--linux-ip override" if (linux_mac or linux_ip)
                       else f"LINUX_NODES[{host}]")
                print(f"    peer   : reporter builds against MAC {mac} / IP {ip}  ({src})")
            ap_role = next((r for r in t.roles if r.device.startswith("linux:")
                            and r.linux_setup == "hostapd-ap"), None)
            if ap_role is not None:
                host = M.DEFAULT_LINUX_PEER
                print(f"    AP     : linux:{host} hostapd_s1g (rimba-ping/SAE); STA associates by SSID")
            mac_roles = [r for r in t.roles if r.build_mac_var]
            if mac_roles:
                print("    build  : MAC topology compiled in (from BENCH), applied to every flash:")
                for r in mac_roles:
                    b = M.BENCH.get(r.device)
                    mac = b.mesh_mac if b else "?"
                    print(f"               {r.build_mac_var:16s} = {mac}  ({r.device} / {r.name})")
        else:
            print(f"    app    : firmware/{t.app}/  (README.md)")
            print("    rig    :")
            for role, dev in t.rig.items():
                print(f"               {role:10s} {dev}")
        print(f"    proves : {t.what_it_proves}")
        print(f"    does NOT prove:\n               {t.what_it_does_not_prove}")
        if t.manual_steps:
            print(f"    manual : {t.manual_steps}")
        for e in t.expectations:
            tag = "NOISY" if e.noisy else "stable"
            print(f"    expect [{tag}] {e.metric} = {e.value}")
            print(f"               source: {e.source}")
            print(f"               assert: {e.assertion}")
