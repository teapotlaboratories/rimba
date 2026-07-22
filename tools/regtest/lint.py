"""pyflakes lint gate for the harness — catch an undefined-name / bad edit OFF-BENCH, before a run.

A `bench`-out-of-scope edit once passed ``py_compile`` and only failed on the bench (10/12 FAIL), 40 min
into a run. ``py_compile`` does NOT catch undefined names; ``pyflakes`` does, in ~0.1 s. This gate runs
pyflakes over the harness (``tools/regtest/*.py`` + the bench ``tools/*.py`` it drives) at the start of
every run-producing command, so that class of bug is caught before any hardware is touched — no operator
watching required.

Pure Python, no hardware. If pyflakes itself is unavailable the gate does NOT block a run — it warns
loudly (so it's visible the check didn't run) and continues, rather than making a missing dev-tool stop
the bench.
"""

from __future__ import annotations

import io
from pathlib import Path

from .common import REPO_ROOT


def targets() -> list[Path]:
    """The harness files the gate lints: the regtest package + the bench tools it invokes."""
    reg = sorted((REPO_ROOT / "tools" / "regtest").glob("*.py"))
    tools = sorted((REPO_ROOT / "tools").glob("*.py"))   # ppk2_hold.py, esp_usb_power.py, ...
    seen, out = set(), []
    for p in reg + tools:
        if p.resolve() not in seen:
            seen.add(p.resolve())
            out.append(p)
    return out


def check(paths: "list[Path] | None" = None) -> tuple[bool, str]:
    """Run pyflakes over `paths` (default: the harness). Returns (ok, report).

    ok is True when pyflakes finds nothing. ok is also True (with a warning in the report) when pyflakes
    is not importable — a missing linter must not gate the bench. ok is False only on real findings.
    """
    try:
        from pyflakes.api import checkPath  # noqa: PLC0415
        from pyflakes.reporter import Reporter as PyflakesReporter  # noqa: PLC0415
    except Exception:
        return True, ("[lint] pyflakes not importable — lint gate SKIPPED (the run's python env has no "
                      "pyflakes; `pip install pyflakes` in it to enable off-bench undefined-name checks).")

    files = paths if paths is not None else targets()
    out, err = io.StringIO(), io.StringIO()
    reporter = PyflakesReporter(out, err)
    findings = 0
    for p in files:
        findings += checkPath(str(p), reporter)
    report = (out.getvalue() + err.getvalue()).strip()
    return findings == 0, report
