"""Append-only run-history + flake ledger for the regression suite.

The per-tier ``build/regtest/<tier>-latest.json`` baselines are **latest-wins**: a passing retry silently
overwrites a failed run, so a flake leaves no trace (this is exactly what erased the first
``rimba-halow-sta`` FAIL). This ledger is the durable counterpart — every result is **appended, never
overwritten**, to ``build/regtest/history.jsonl`` as it is produced (hooked into ``Reporter.add``), so a
retried transient is *recorded*. ``run.py flakes`` then surfaces which tests flip verdict run-to-run.

One JSONL line per result. A "run" is the set of lines sharing a ``run_id`` (one per Reporter). The file
is append-only and read tolerantly (a torn last line from a reaped mid-write run is skipped), so it needs
no locking and survives the process being killed — the same crash-safety the baselines have.

This is a pure-Python, dependency-free view over recorded verdicts — no hardware, so it is unit-testable
off-bench and needs no live monitoring.
"""

from __future__ import annotations

import json
from pathlib import Path

from .common import RESULTS_DIR

#: The append-only history. Sibling of the <tier>-latest.json baselines.
HISTORY_PATH = RESULTS_DIR / "history.jsonl"

def _ledger_meta(meta) -> dict:
    """A bounded, trendable subset of a result's meta to persist: scalar numbers, short strings, and short
    numeric lists. So a recorded NUMBER that drifts (tp median_ma, dscycle latencies_ms) survives in the
    history and can be trended/diffed across runs + SDK versions -- not just the pass/fail."""
    if not isinstance(meta, dict):
        return {}
    out: dict = {}
    for k, v in meta.items():
        if isinstance(v, bool):
            out[k] = v
        elif isinstance(v, (int, float)):
            out[k] = v
        elif isinstance(v, str) and len(v) <= 80:
            out[k] = v
        elif (isinstance(v, (list, tuple)) and v
              and all(isinstance(x, (int, float)) and not isinstance(x, bool) for x in v)):
            out[k] = list(v[:32])   # e.g. latencies_ms
    return out


def _scalar_metrics(meta) -> dict:
    """{metric: float} for trending -- scalar numbers as-is; a numeric list reduces to its MEAN (so
    latencies_ms trends as the mean reconnect latency). bool is not a trend metric."""
    out: dict = {}
    if not isinstance(meta, dict):
        return out
    for k, v in meta.items():
        if isinstance(v, bool):
            continue
        if isinstance(v, (int, float)):
            out[k] = float(v)
        elif (isinstance(v, (list, tuple)) and v
              and all(isinstance(x, (int, float)) and not isinstance(x, bool) for x in v)):
            out[k] = sum(v) / len(v)
    return out


#: Outcome classes for flake detection. GOOD = actually ran and was acceptable (PASS, or the
#: expected-known-broken XFAIL). BAD = a real bad outcome. SKIP is NEITHER — it means "did not run" (rig
#: absent), so it must not rescue a test that FAILs whenever it *does* run (that is consistently-broken,
#: not flaky), nor make an occasionally-skipped passing test look flaky.
_GOOD = frozenset({"PASS", "XFAIL"})
_BAD = frozenset({"FAIL", "INCONCLUSIVE", "XPASS"})


def append_result(run_id: str, tier: str, git: str, halow_gitlink: str, result,
                  path: "Path | None" = None) -> None:
    """Append ONE result to the history (append-only, best-effort). Never raises to the caller."""
    p = Path(path) if path is not None else HISTORY_PATH
    try:
        import time  # noqa: PLC0415
        p.parent.mkdir(parents=True, exist_ok=True)
        rec = {
            "ts": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
            "run_id": run_id,
            "tier": tier,
            "git": git,
            "halow_gitlink": halow_gitlink,
            "name": result.name,
            "status": result.status,
            "duration_s": round(getattr(result, "duration_s", 0.0) or 0.0, 1),
            "detail": (getattr(result, "detail", "") or "")[:200],
            "meta": _ledger_meta(getattr(result, "meta", None)),
        }
        with p.open("a") as f:
            f.write(json.dumps(rec) + "\n")
    except Exception:
        pass


def load_history(path: "Path | None" = None) -> list[dict]:
    """Read every recorded result. Tolerates a torn final line (a run reaped mid-write)."""
    p = Path(path) if path is not None else HISTORY_PATH
    if not p.exists():
        return []
    out: list[dict] = []
    for line in p.read_text().splitlines():
        line = line.strip()
        if not line:
            continue
        try:
            obj = json.loads(line)
        except Exception:
            continue   # skip a corrupt/partial line rather than fail the whole read
        if isinstance(obj, dict):   # a shape-valid non-object line (null/number/list) must not crash summarize()
            out.append(obj)
    return out


def summarize(history: "list[dict] | None" = None) -> dict:
    """Per-test verdict history. Returns {name: {tier, total, by_status, runs, good, bad, last_status,
    recent, flaky, broken, same_code_flake}}.

    runs is a list of (ts, git, status, run_id) in file order. `flaky` = both a good (PASS/XFAIL) AND a bad
    (FAIL/INCONCLUSIVE/XPASS) outcome across runs (SKIP is neutral); `broken` = a bad outcome and never a
    good one (consistently failing when it runs, not a flake). `same_code_flake` = the flip happened at a
    single git SHA (a real transient); a flip whose good and bad outcomes never share a SHA straddles a
    code change and may be a regression, not a transient.
    """
    hist = load_history() if history is None else history
    tests: dict[str, dict] = {}
    for rec in hist:
        name = rec.get("name")
        if not name:
            continue
        t = tests.setdefault(name, {"tier": rec.get("tier", ""), "total": 0,
                                    "by_status": {}, "runs": []})
        st = rec.get("status", "")
        t["tier"] = rec.get("tier", t["tier"])
        t["total"] += 1
        t["by_status"][st] = t["by_status"].get(st, 0) + 1
        t["runs"].append((rec.get("ts", ""), rec.get("git", ""), st, rec.get("run_id", ""),
                          rec.get("halow_gitlink", "")))
    for t in tests.values():
        good = sum(v for s, v in t["by_status"].items() if s in _GOOD)
        bad = sum(v for s, v in t["by_status"].items() if s in _BAD)
        # "same code" = the SAME app SHA *and* the same morselib (halow) gitlink. Keying on git alone
        # would merge different SDK states during a forward-port (the superproject is `<sha>-dirty` across
        # hops), mislabelling a real cross-SDK regression as a same-code bench transient.
        good_code = {(git, hl) for (_ts, git, st, _rid, hl) in t["runs"] if st in _GOOD}
        bad_code = {(git, hl) for (_ts, git, st, _rid, hl) in t["runs"] if st in _BAD}
        t["good"] = good
        t["bad"] = bad
        t["flaky"] = good > 0 and bad > 0
        t["broken"] = bad > 0 and good == 0
        t["same_code_flake"] = t["flaky"] and bool(good_code & bad_code)
        t["last_status"] = t["runs"][-1][2] if t["runs"] else ""
        t["recent"] = [st for (_ts, _g, st, _r, _hl) in t["runs"][-6:]]
    return tests


def flakes(history: "list[dict] | None" = None) -> dict:
    """Only the tests that flaked (both passed and failed/inconclusive across different runs)."""
    return {n: t for n, t in summarize(history).items() if t["flaky"]}


def broken(history: "list[dict] | None" = None) -> dict:
    """Tests that are consistently BAD across every recorded run (broken, not flaky)."""
    return {n: t for n, t in summarize(history).items() if t["broken"]}


def _status_counts(by_status: dict) -> str:
    order = ["PASS", "FAIL", "INCONCLUSIVE", "SKIP", "XFAIL", "XPASS"]
    parts = [f"{by_status[s]} {s}" for s in order if by_status.get(s)]
    parts += [f"{v} {s}" for s, v in by_status.items() if s not in order]
    return ", ".join(parts)


def format_report(history: "list[dict] | None" = None) -> str:
    """Human-readable flake ledger for `run.py flakes`."""
    hist = load_history() if history is None else history
    tests = summarize(hist)
    run_ids = {r.get("run_id") for r in hist if r.get("run_id")}
    lines = [
        f"Flake ledger — {len(hist)} results across {len(run_ids)} run(s), {len(tests)} distinct test(s).",
        f"  ({HISTORY_PATH})",
        "",
    ]
    flaky = {n: t for n, t in tests.items() if t["flaky"]}
    brk = {n: t for n, t in tests.items() if t["broken"]}
    if not hist:
        lines.append("No history yet — run a tier (make test-t0/t1/t2/tp/dscycle) and results accumulate here.")
        return "\n".join(lines)
    def _row(name, t):
        return (f"  [{t['tier']}] {name}: {_status_counts(t['by_status'])}  "
                f"recent[{' '.join(t['recent'])}]  (last {t['last_status']})")

    if flaky:
        transient = sorted(((n, t) for n, t in flaky.items() if t["same_code_flake"]), key=lambda kv: -kv[1]["bad"])
        crosscommit = sorted(((n, t) for n, t in flaky.items() if not t["same_code_flake"]), key=lambda kv: -kv[1]["bad"])
        if transient:
            lines.append(f"FLAKY ({len(transient)}) — flipped verdict at the SAME code SHA "
                         f"(a bench/RF/rig transient, not a code regression):")
            lines += [_row(n, t) for n, t in transient]
        if crosscommit:
            if transient:
                lines.append("")
            lines.append(f"FLIPPED ACROSS CODE CHANGES ({len(crosscommit)}) — verdict changed between git "
                         f"revisions; could be a real regression, verify before dismissing as a transient:")
            lines += [_row(n, t) for n, t in crosscommit]
    else:
        lines.append("FLAKY: none — every test that ran more than once gave a consistent verdict.")
    if brk:
        lines.append("")
        lines.append(f"CONSISTENTLY FAILING ({len(brk)}) — a bad outcome and never a good one, so a real "
                     f"defect, not a flake (a rig-gated test that SKIPs when absent but FAILs when it runs "
                     f"lands here, not in FLAKY):")
        for name, t in sorted(brk.items()):
            lines.append(_row(name, t))
    lines.append("")
    lines.append("Flake stats are all-time; reset with:  rm build/regtest/history.jsonl  (recent[] shows "
                 "the last few outcomes so a long-resolved flake is easy to tell from a live one).")
    return "\n".join(lines)


# ---------------------------------------------------------------------------
# Numbers that drift — trend a recorded metric over runs, and diff it across SDK gitlinks.
# The <tier>-latest.json baselines + the FLAKY view above catch pass<->fail flips; these catch a metric
# that DRIFTS while the test still PASSes (tp median_ma creeping up, a dscycle reconnect getting slower).
# ---------------------------------------------------------------------------

_SPARK = "▁▂▃▄▅▆▇█"


def sparkline(vals: list) -> str:
    if not vals:
        return ""
    lo, hi = min(vals), max(vals)
    if hi == lo:
        return _SPARK[0] * len(vals)
    return "".join(_SPARK[min(7, int((v - lo) / (hi - lo) * 7.999))] for v in vals)


def metric_trends(history: "list[dict] | None" = None, name: "str | None" = None) -> dict:
    """{test: {metric: [(ts, git, halow_gitlink, value), ...]}} in run order, for scalar numeric metrics."""
    hist = load_history() if history is None else history
    out: dict = {}
    for rec in hist:
        nm = rec.get("name")
        if not nm or (name is not None and nm != name):
            continue
        for mk, mv in _scalar_metrics(rec.get("meta")).items():
            out.setdefault(nm, {}).setdefault(mk, []).append(
                (rec.get("ts", ""), rec.get("git", ""), rec.get("halow_gitlink", ""), mv))
    return out


def _gitlinks_in(hist: list) -> list:
    """Distinct halow gitlinks present in the history, in first-seen order."""
    return [hl for hl in dict.fromkeys(r.get("halow_gitlink", "") for r in hist) if hl]


def _resolve_gitlink(hist: list, prefix: str) -> "str | None":
    gls = _gitlinks_in(hist)
    if prefix in gls:
        return prefix
    pre = [g for g in gls if g.startswith(prefix)]
    return pre[0] if len(pre) == 1 else None   # None = not found or ambiguous


def diff_gitlinks(gla: str, glb: str, history: "list[dict] | None" = None):
    """Compare each (test, metric)'s value at halow gitlink A vs B (last run at each; prefixes accepted).
    Returns (rows | None, (resolvedA, resolvedB)); rows = [{test, metric, a, b, pct}] sorted by |pct| desc."""
    hist = load_history() if history is None else history
    a, b = _resolve_gitlink(hist, gla), _resolve_gitlink(hist, glb)
    if a is None or b is None:
        return None, (a, b)
    at: dict = {}
    for rec in hist:
        hl = rec.get("halow_gitlink", "")
        if hl not in (a, b):
            continue
        for mk, mv in _scalar_metrics(rec.get("meta")).items():
            at[(rec.get("name"), mk, hl)] = mv   # last wins
    rows = []
    for (nm, mk) in {(n, m) for (n, m, _h) in at}:
        va, vb = at.get((nm, mk, a)), at.get((nm, mk, b))
        if va is None or vb is None:
            continue
        pct = (100.0 * (vb - va) / va) if va else (float("inf") if vb else 0.0)
        rows.append({"test": nm, "metric": mk, "a": va, "b": vb, "pct": pct})
    rows.sort(key=lambda r: -(abs(r["pct"]) if r["pct"] != float("inf") else 1e18))
    return rows, (a, b)


def format_trend(history: "list[dict] | None" = None, name: "str | None" = None, last: int = 8) -> str:
    trends = metric_trends(history, name)
    if not trends:
        return ("No numeric metrics recorded yet. tp (median_ma) and dscycle (latencies_ms, wake_cycles) "
                f"record them; run a tier and they accumulate in {HISTORY_PATH}.")
    out = [f"Metric trends (last {last} runs per test-metric). A number that DRIFTS but stays in-band "
           "shows here even when the test still PASSes:", ""]
    for tname in sorted(trends):
        out.append(f"{tname}:")
        for metric in sorted(trends[tname]):
            vals = [v for (_ts, _g, _hl, v) in trends[tname][metric][-last:]]
            first, latest = vals[0], vals[-1]
            pct = (100.0 * (latest - first) / first) if first else 0.0
            arrow = "→" if abs(pct) < 1 else ("↑" if pct > 0 else "↓")
            valstr = " ".join(f"{v:g}" for v in vals)
            out.append(f"  {metric:18} {sparkline(vals)}  {valstr}   {arrow}{pct:+.0f}% over {len(vals)}")
    return "\n".join(out)


def format_diff(gla: str, glb: str, history: "list[dict] | None" = None) -> str:
    rows, (a, b) = diff_gitlinks(gla, glb, history)
    hist = load_history() if history is None else history
    if rows is None:
        return (f"Could not resolve gitlink(s) (not in history or ambiguous): A={a!r} B={b!r}.\n"
                "Known halow gitlinks: " + (", ".join(g[:12] for g in _gitlinks_in(hist)) or "(none)"))
    if not rows:
        return f"No metric present at BOTH {a[:12]} and {b[:12]} to compare."
    out = [f"Metric diff: halow {a[:12]} (A) -> {b[:12]} (B). A number that MOVED across the SDK change "
           "is the regression class this suite exists to catch:", ""]
    for r in rows:
        pct = "n/a" if r["pct"] == float("inf") else f"{r['pct']:+.0f}%"
        flag = "   <-- >20% MOVE" if (r["pct"] != float("inf") and abs(r["pct"]) > 20) else ""
        out.append(f"  {r['test']:22} {r['metric']:16} A={r['a']:g}  B={r['b']:g}  {pct}{flag}")
    return "\n".join(out)
