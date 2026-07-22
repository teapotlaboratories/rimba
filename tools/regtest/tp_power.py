"""tp — the PPK2 power-save regression tier.

A power verdict comes from a HOST-side PPK2 current stream the firmware physically cannot see,
so it can never be a firmware TEST|RESULT (that is T2's model). This tier owns the PPK2, runs
the `test-power` C6-triggered 4-tier PS ladder on board2 (No-PS -> Dyn-PS -> TWT -> WNM+chip-
powerdown, 18 s each), segments the current stream by the DUT's `TEST|INFO|phase=N` console
markers, reduces each tier to a median mA, and scores a GROSS current regression (the kind the
fw-1.17.9 ~2x PS regression was) against the calibrated bands in manifest.POWER_BANDS.

WHAT IT PROVES: the STA power-save tiers still draw about what they did on the known-good fw --
Dyn-PS and WNM+chip-powerdown did not silently ~2x (a stack/fw bump regressing PS).
WHAT IT DOES NOT PROVE: absolute doze depth (that is a benchmark, deliberately NOT a pass/fail
gate). The raw mA is always recorded; the gate is wide (FAIL only on a gross multiple).

Rig: board2 (require_wired) on the PPK2 rail; the C6 (firmware/test-c6-trigger, MODE_TRIGGER) pulses
board2 D5 to fire the ladder; an AP the STA associates to (ESP `test-apsta-ap` on board0, or
Linux `hostapd_s1g` on chronite). Mirrors the proven ~/pwr_test/rf_run.py take-over + sample loop.
"""

from __future__ import annotations

import os
import signal
import statistics
import subprocess
import sys
import time
from typing import Optional

if __package__ in (None, ""):
    from pathlib import Path
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
    from tools.regtest import manifest as M  # type: ignore
    from tools.regtest import common, linux_peer  # type: ignore
else:
    from . import manifest as M
    from . import common, linux_peer

from .common import FAIL, INCONCLUSIVE, PASS, SKIP, Reporter, Result

_PPK2_VID = 0x1915
_PPK2_PID = 0xC00A
_VBUS_V = 5.0

# The DUT's TEST| markers the host segments on / reads structural facts from.
import re

_RE_PHASE = re.compile(r"TEST\|INFO\|phase=(\d+)\s+(\S+)\s+start")
_RE_STEP = re.compile(r"TEST\|STEP\|([^|]+)\|(PASS|FAIL)\|(.*)")
_RE_DONE = re.compile(r"TEST\|INFO\|phase=5\s+ladder-done")
_RE_FW = re.compile(r"(?:Morse firmware|Chip firmware|fw)\D*(\d+\.\d+\.\d+)", re.I)

# Per-tier plateau window inside the 18 s tier: discard the first 2 s (PS-mode transition settles)
# and the last 1 s (next phase_mark bleed), so [+2, +17] of a tier is the steady plateau.
_SETTLE_S = 2.0
_TAIL_S = 1.0


# --------------------------------------------------------------------------
# PPK2 ownership + sampling (mirrors ~/pwr_test/rf_run.py + ppk2_mon2.py)
# --------------------------------------------------------------------------


def _find_ppk2_port() -> Optional[str]:
    try:
        import serial.tools.list_ports as lp  # noqa: PLC0415
    except ImportError:
        return None
    cands = [p for p in lp.comports() if p.vid == _PPK2_VID and p.pid == _PPK2_PID]
    if not cands:
        return None
    # The control CDC is the interface whose USB location ends in '.1' (rf_run.py's selection).
    return ([p.device for p in cands if str(p.location or "").endswith(".1")] or [cands[0].device])[0]


class Ppk2Sampler:
    """Owns the PPK2 in ampere-meter mode. Takes over from any prior holder, power-cycles the DUT
    rail, streams samples in a background thread, and answers 'median mA in [t0, t1]'."""

    #: process names of prior PPK2 holders to kill before seizing the single control CDC.
    _HOLDERS = ("ppk2_hold.py", "ppk2_mon_1s.py", "ppk2_mon2.py", "ppk2_power.py", "rf_run.py")

    def __init__(self, port: str):
        self._port = port
        self._ppk2 = None
        self._series: list[tuple[float, float]] = []   # (host_time, batch_mean_mA)
        self._running = False
        self._thread = None

    @staticmethod
    def _drain(ppk2) -> None:
        for attr in ("ser", "rttser", "serial"):
            s = getattr(ppk2, attr, None)
            if s is None:
                continue
            try:
                quiet = 0
                for _ in range(80):
                    n = getattr(s, "in_waiting", 0)
                    if n:
                        s.read(n)
                        quiet = 0
                    else:
                        quiet += 1
                        if quiet >= 6:
                            break
                    time.sleep(0.05)
            except Exception:
                pass

    def take_over(self) -> None:
        """Kill prior holders, open the control CDC, read metadata (with retry), ampere-meter, 5 V."""
        from ppk2_api.ppk2_api import PPK2_API  # noqa: PLC0415

        self_pid = os.getpid()
        for name in self._HOLDERS:
            try:
                for p in subprocess.check_output(["pgrep", "-f", name]).decode().split():
                    if int(p) != self_pid:
                        try:
                            os.kill(int(p), signal.SIGKILL)
                        except ProcessLookupError:
                            pass
            except subprocess.CalledProcessError:
                pass
        time.sleep(1.5)

        self._ppk2 = PPK2_API(self._port)
        for _ in range(8):
            try:
                try:
                    self._ppk2.stop_measuring()
                except Exception:
                    pass
                time.sleep(0.3)
                self._drain(self._ppk2)
                self._ppk2.get_modifiers()
                break
            except Exception as e:
                print(f"  PPK2 meta retry: {e}", flush=True)
                time.sleep(0.5)
        self._ppk2.use_ampere_meter()
        self._ppk2.set_source_voltage(5000)   # MANDATORY: start_measuring() raises without it

    def power_cycle(self) -> None:
        """OFF -> ON so board2 boots fresh (also clears any stale take-over state on the rail)."""
        self._ppk2.toggle_DUT_power("OFF")
        time.sleep(2.5)
        self._ppk2.toggle_DUT_power("ON")

    def start(self) -> None:
        import threading  # noqa: PLC0415

        self._ppk2.start_measuring()
        self._running = True
        self._thread = threading.Thread(target=self._loop, daemon=True)
        self._thread.start()

    def _loop(self) -> None:
        while self._running:
            try:
                data = self._ppk2.get_data()
                if data:
                    res = self._ppk2.get_samples(data)
                    samples = res[0] if isinstance(res, tuple) else res
                    if samples:
                        self._series.append((time.time(), sum(samples) / len(samples) / 1000.0))
            except Exception:
                pass
            time.sleep(0.01)

    def stop(self) -> None:
        self._running = False
        if self._thread:
            self._thread.join(timeout=2.0)
        try:
            self._ppk2.stop_measuring()
        except Exception:
            pass
        self._drain(self._ppk2)   # leave the PPK2 stream clean so the next holder/take-over is happy

    def close(self) -> None:
        """Release the control CDC so a fresh ppk2_hold can re-grab it. Board2 stays powered
        (the DUT-power latch holds across a serial disconnect -- bench-devices.md:235-236)."""
        for attr in ("ser", "rttser", "serial"):
            s = getattr(self._ppk2, attr, None)
            try:
                if s is not None:
                    s.close()
            except Exception:
                pass

    def window_stats(self, t0: float, t1: float) -> Optional[tuple[float, float, float, int]]:
        vals = sorted(ma for (t, ma) in self._series if t0 <= t <= t1)
        if not vals:
            return None
        n = len(vals)
        return (statistics.median(vals), vals[int(0.10 * (n - 1))], vals[int(0.90 * (n - 1))], n)


# --------------------------------------------------------------------------
# Console capture — timestamp each phase marker + collect the structural STEP facts
# --------------------------------------------------------------------------


def _capture_ladder(port: str, timeout_s: float) -> tuple[dict, dict, str, bool]:
    """Read the DUT console until 'ladder-done' or timeout. Returns:
      phase_t : {phase_key -> host_time of its 'phase=N <key> start' marker}
      steps   : {step_name -> bool}  (associated / twt-installed / wnm-accepted)
      fw      : the firmware version string if seen, else ""
      done    : whether the ladder completed.
    Same dtr=False/rts=False caveat as common.capture_serial (these XIAO boards)."""
    try:
        import serial  # noqa: PLC0415
    except ImportError:
        raise RuntimeError("pyserial not importable; run under the IDF python env")
    phase_t: dict[str, float] = {}
    steps: dict[str, bool] = {}
    fw = ""
    done = False
    s = serial.Serial()
    s.port = port
    s.baudrate = 115200
    s.timeout = 0.5
    s.dtr = False
    s.rts = False
    s.open()
    try:
        t0 = time.time()
        while time.time() - t0 < timeout_s:
            raw = s.readline()
            if not raw:
                continue
            line = raw.decode("utf-8", "replace")
            now = time.time()
            m = _RE_PHASE.search(line)
            if m:
                key = m.group(2)
                phase_t.setdefault(key, now)   # first occurrence of each tier (one ladder run)
                continue
            m = _RE_STEP.search(line)
            if m:
                steps.setdefault(m.group(1).strip(), m.group(2) == "PASS")
                continue
            if not fw:
                mf = _RE_FW.search(line)
                if mf:
                    fw = mf.group(1)
            if _RE_DONE.search(line):
                done = True
                break
    finally:
        s.close()
    return phase_t, steps, fw, done


# --------------------------------------------------------------------------
# Scoring
# --------------------------------------------------------------------------


def _score(rep: Reporter, ap: str, tier_stats: dict, steps: dict, fw: str, dur: float,
           light_sleep: bool = False) -> None:
    lo, hi = M.POWER_NOPS_VALID_MA
    nops = tier_stats.get("no-ps")
    associated = steps.get("associated", False)
    # Validity = the measurement is trustworthy: the STA associated AND No-PS drew the expected
    # ~65 mA (proves the radio is on + not RX-overloaded into a bogus reading). We deliberately do
    # NOT require a scored tier to be far below No-PS: a real regression pushes doze current UP
    # toward No-PS, so gating on tier-separation would mask the exact failure the tier exists to
    # catch (a ~2x regression scored INCONCLUSIVE instead of FAIL -- caught by the synthetic test).
    # A high scored median is what the band's fail_min is for.
    valid = associated and nops is not None and lo <= nops[0] <= hi

    bands = (M.POWER_BANDS_LS if light_sleep else M.POWER_BANDS).get(ap, {})
    calibrated = M.POWER_BANDS_LS_CALIBRATED if light_sleep else M.POWER_BANDS_CALIBRATED
    prov = "" if calibrated else " [PROVISIONAL bands — not yet calibrated on-rig]"

    for tier in M.POWER_TIERS:
        st = tier_stats.get(tier.key)
        base_meta = {"ap": ap, "fw": fw or "?", "tier": tier.key, "scored": tier.scored}
        if st is None:
            rep.add(Result("TP", f"tp:{tier.key} ({tier.label})", INCONCLUSIVE,
                           detail=f"no PPK2 samples segmented for this tier (fw={fw or '?'}, ap={ap})",
                           duration_s=0.0, meta=base_meta))
            continue
        med, p10, p90, n = st
        meta = {**base_meta, "median_ma": round(med, 2), "p10_ma": round(p10, 2),
                "p90_ma": round(p90, 2), "n": n}
        num = f"{med:.1f} mA (p10 {p10:.1f} / p90 {p90:.1f}, n={n})"

        if tier.key == "no-ps":
            status = PASS if valid else INCONCLUSIVE
            why = (f"validity anchor {num}; associated={associated}, "
                   f"window {lo:.0f}-{hi:.0f} mA -> {'in band' if valid else 'OUT OF BAND'}")
            if not valid:
                why += " (unassociated / PS-not-off / RX-overloaded -> whole run untrustworthy)"
            rep.add(Result("TP", f"tp:{tier.key} ({tier.label})", status, detail=why,
                           duration_s=dur if tier.phase == 1 else 0.0, meta=meta,
                           evidence=tier.provenance))
            continue

        if not tier.scored:   # TWT — recorded, not scored
            twt_inst = steps.get("twt-installed")
            rep.add(Result("TP", f"tp:{tier.key} ({tier.label})", PASS,
                           detail=f"recorded (not scored — AP-dependent): {num}; "
                                  f"twt-installed={twt_inst}",
                           meta={**meta, "twt_installed": twt_inst}, evidence=tier.provenance))
            continue

        # Scored tier (Dyn-PS, WNM+powerdown).
        band = bands.get(tier.key)
        if not valid or band is None:
            reason = ("run not trustworthy (No-PS validity failed: unassociated / RX-overloaded)"
                      if band is not None else "no band configured for this AP")
            rep.add(Result("TP", f"tp:{tier.key} ({tier.label})", INCONCLUSIVE,
                           detail=f"{num}; {reason}{prov}", meta=meta, evidence=tier.provenance))
            continue

        pass_max, fail_min = band
        # False-PASS guard: a low mA whose structural step failed is 'failed to doze', not a win.
        step_ok = True
        if tier.key == "wnm-powerdown":
            step_ok = steps.get("wnm-accepted", True)
        if med >= fail_min:
            status, why = FAIL, (f"{num} >= FAIL ceiling {fail_min:.0f} mA (band <={pass_max:.0f}) "
                                 f"— a GROSS current regression on {ap} AP{prov}")
        elif med <= pass_max:
            if not step_ok:
                status, why = INCONCLUSIVE, (f"{num} is in-band BUT the structural step failed "
                                             f"(wnm-accepted={steps.get('wnm-accepted')}) — a low "
                                             f"number that is really 'failed to doze', not a PASS{prov}")
            else:
                status, why = PASS, f"{num} <= band {pass_max:.0f} mA (FAIL >= {fail_min:.0f}){prov}"
        else:
            status, why = INCONCLUSIVE, (f"{num} in the grey zone {pass_max:.0f}-{fail_min:.0f} mA "
                                         f"— not clearly good or a gross regression{prov}")
        rep.add(Result("TP", f"tp:{tier.key} ({tier.label})", status, detail=why,
                       meta={**meta, "band": [pass_max, fail_min]}, evidence=tier.provenance))


# --------------------------------------------------------------------------
# Orchestration
# --------------------------------------------------------------------------


def _dry_run(rep: Reporter, ap: str, light_sleep: bool = False) -> Reporter:
    variant = "host-light-sleep" if light_sleep else "host-awake"
    print(f"tp -- PPK2 power-save ladder ({variant}) (dry run)\n")
    print(f"  DUT      : {M.POWER_DUT_BOARD} ({M.BENCH[M.POWER_DUT_BOARD].efuse_mac}) "
          f"app={M.POWER_DUT_APP} ({variant})  [require_wired; on the PPK2 rail]")
    print(f"  trigger  : the C6 (firmware/test-c6-trigger MODE_TRIGGER, GPIO20 -> D5) every "
          f"{M.POWER_C6_TRIGGER_PERIOD_S} s")
    if ap == "esp":
        print(f"  AP       : {M.POWER_ESP_AP_BOARD} app={M.POWER_ESP_AP_APP} (up-marker "
              f"'{M.POWER_ESP_AP_MARKER}')")
    else:
        print(f"  AP       : linux:{M.POWER_LINUX_AP_HOST} hostapd_s1g (rimba-ping / SAE / dtim1)")
    print(f"\n  tiers ({M.POWER_PHASE_S}s each): "
          f"{'  '.join(f'{t.phase}:{t.key}{'*' if t.scored else ''}' for t in M.POWER_TIERS)} "
          f"(* = scored)")
    band_src = M.POWER_BANDS_LS if light_sleep else M.POWER_BANDS
    calibrated = M.POWER_BANDS_LS_CALIBRATED if light_sleep else M.POWER_BANDS_CALIBRATED
    cal = "CALIBRATED" if calibrated else "PROVISIONAL (calibrate on-rig)"
    print(f"  No-PS validity window: {M.POWER_NOPS_VALID_MA[0]:.0f}-{M.POWER_NOPS_VALID_MA[1]:.0f} mA")
    print(f"  bands ({ap} AP, {variant}, {cal}):")
    for k, (pm, fm) in band_src.get(ap, {}).items():
        print(f"    {k:14s} PASS <= {pm:.0f} mA   FAIL >= {fm:.0f} mA")
    rep.add(Result("TP", f"tp dry-run (ap={ap}, {variant})", SKIP,
                   detail="dry run — no hardware touched", meta={"ap": ap, "light_sleep": light_sleep}))
    return rep


def run(ap: str = "esp", dry_run: bool = False, append: bool = False,
        light_sleep: bool = False) -> Reporter:
    rep = Reporter("TP", persist=not dry_run)
    if append:
        prior = common.RESULTS_DIR / "TP-latest.json"
        if prior.exists():
            import json  # noqa: PLC0415
            try:
                rep.seed_from(json.loads(prior.read_text()).get("results", []))
            except Exception:
                pass
    if dry_run:
        return _dry_run(rep, ap, light_sleep)

    M.require_bench()
    if ap == "linux":
        M.require_linux()

    # --- preconditions ------------------------------------------------------
    ppk2_port = _find_ppk2_port()
    if not ppk2_port:
        rep.add(Result("TP", "tp power ladder", SKIP,
                       detail="no PPK2 found (Nordic 1915:C00A). Connect the PPK2 that powers "
                              "board2's rail (docs/reference/rimba-bench-devices.md)."))
        return rep
    import importlib.util  # noqa: PLC0415
    if importlib.util.find_spec("ppk2_api") is None:
        rep.add(Result("TP", "tp power ladder", SKIP,
                       detail="ppk2_api not importable. `pip install ppk2-api` in the IDF venv "
                              "(it is already installed on the bench host)."))
        return rep

    dut = M.BENCH[M.POWER_DUT_BOARD]
    ap_teardown = None
    sampler = Ppk2Sampler(ppk2_port)
    started = time.time()
    try:
        # --- own the PPK2 + power-cycle board2 fresh ------------------------
        print(f"tp -- owning the PPK2 ({ppk2_port}) + power-cycling {M.POWER_DUT_BOARD}...", flush=True)
        sampler.take_over()
        sampler.power_cycle()
        dut_port = None
        for _ in range(60):   # up to ~24 s for board2 to re-enumerate after the power-cycle
            dut_port = common.resolve_port(dut.efuse_mac)
            if dut_port:
                break
            time.sleep(0.4)
        if not dut_port:
            rep.add(Result("TP", "tp power ladder", SKIP,
                           detail=f"{M.POWER_DUT_BOARD} ({dut.efuse_mac}) did not enumerate after "
                                  "the PPK2 power-cycle — check the rail / USB."))
            return rep

        # --- bring up the AP ------------------------------------------------
        print(f"tp -- bringing up the {ap} AP...", flush=True)
        if ap == "esp":
            ap_port = common.resolve_port(M.BENCH[M.POWER_ESP_AP_BOARD].efuse_mac)
            if not ap_port:
                rep.add(Result("TP", "tp power ladder", SKIP,
                               detail=f"AP board {M.POWER_ESP_AP_BOARD} not enumerated."))
                return rep
            cp = common.make("flash", M.POWER_ESP_AP_APP, M.BENCH_BOARD, port=ap_port, timeout=600)
            if cp.returncode != 0:
                rep.add(Result("TP", "tp power ladder", INCONCLUSIVE,
                               detail=f"AP flash failed: {cp.stderr[-300:]}"))
                return rep
            _, up = common.capture_until(ap_port, 30.0, M.POWER_ESP_AP_MARKER)
            if not up:
                rep.add(Result("TP", "tp power ladder", INCONCLUSIVE,
                               detail=f"ESP AP never printed '{M.POWER_ESP_AP_MARKER}' — no AP to "
                                      "associate to."))
                return rep

            def _teardown_esp_ap():
                common.go_radio_silent([M.POWER_ESP_AP_BOARD], M.BENCH, M.IDLE_APP, M.BENCH_BOARD,
                                       verbose=False)
            ap_teardown = _teardown_esp_ap   # assign (not a def) so it cleanly overrides the None default
        else:
            ok, detail, ap_teardown = linux_peer.bring_up_ap(M.POWER_LINUX_AP_HOST)
            print(f"  {detail}", flush=True)
            if not ok:
                rep.add(Result("TP", "tp power ladder", SKIP if "not reachable" in detail
                               else INCONCLUSIVE, detail=f"Linux AP bring-up failed: {detail}"))
                return rep

        # --- flash the DUT + sample the ladder ------------------------------
        variant = "host-light-sleep" if light_sleep else "host-awake"
        print(f"tp -- flashing {M.POWER_DUT_APP} ({variant}) to {M.POWER_DUT_BOARD} + sampling...",
              flush=True)
        # Pass HOST_LIGHT_SLEEP explicitly (0 or 1) so a sticky -D cache var from the other variant
        # never leaks in (the whole gate hinges on which ladder ran).
        cp = common.make("flash", M.POWER_DUT_APP, M.BENCH_BOARD, port=dut_port, timeout=600,
                         make_vars={"HOST_LIGHT_SLEEP": "1" if light_sleep else "0"})
        if cp.returncode != 0:
            rep.add(Result("TP", "tp power ladder", INCONCLUSIVE,
                           detail=f"DUT flash failed: {cp.stderr[-300:]}"))
            return rep
        sampler.start()
        # boot(~3) + connect(~5) + up to one C6 period(30) + 4*18 s ladder + phase_mark overhead.
        budget = 20.0 + M.POWER_C6_TRIGGER_PERIOD_S + 4 * M.POWER_PHASE_S + 20.0
        phase_t, steps, fw, done = _capture_ladder(dut_port, budget)
        sampler.stop()

        if "no-ps" not in phase_t:
            rep.add(Result("TP", "tp power ladder", INCONCLUSIVE,
                           detail="no ladder markers seen — the C6 trigger never fired. Is the C6 "
                                  "powered + flashed (test-c6-trigger MODE_TRIGGER) + wired GPIO20->D5? "
                                  f"(associated={steps.get('associated')}, fw={fw or '?'})"))
            return rep

        # --- segment + score ------------------------------------------------
        tier_stats: dict[str, Optional[tuple]] = {}
        for tier in M.POWER_TIERS:
            t_start = phase_t.get(tier.key)
            if t_start is None:
                tier_stats[tier.key] = None
                continue
            tier_stats[tier.key] = sampler.window_stats(
                t_start + _SETTLE_S, t_start + M.POWER_PHASE_S - _TAIL_S)
        _score(rep, ap, tier_stats, steps, fw, round(time.time() - started, 1), light_sleep)
        if not done:
            rep.add(Result("TP", "tp:ladder-complete", INCONCLUSIVE,
                           detail="the ladder did not reach 'ladder-done' within the window "
                                  "(a tier may be truncated)."))
        return rep
    finally:
        # Teardown: radio-silence the ESPs, take the AP down, release the PPK2, restore ppk2_hold.
        print("tp -- teardown: radio-silence + release PPK2...", flush=True)
        try:
            sampler.stop()
        except Exception:
            pass
        common.go_radio_silent([M.POWER_DUT_BOARD], M.BENCH, M.IDLE_APP, M.BENCH_BOARD, verbose=False)
        if ap_teardown:
            try:
                ap_teardown()
            except Exception:
                pass
        try:
            sampler.close()
        except Exception:
            pass
        # board2 stays powered: the PPK2 DUT-power latch holds across the serial disconnect
        # (docs/reference/rimba-bench-devices.md:235-236), so board2 remains enumerated. We do NOT
        # re-spawn ppk2_hold here -- a bare restart races the just-closed handle and lacks the
        # drain/retry a clean take-over needs (tools/ppk2_hold.py has neither), which crashes it and
        # can leave the rail in a bad state. The next `tp` run's take_over() kills + drains +
        # power-cycles regardless; restart a standing holder by hand if you want one.
        print("tp -- board2 left powered (PPK2 latch). For a standing holder: "
              "nohup python tools/ppk2_hold.py &", flush=True)
