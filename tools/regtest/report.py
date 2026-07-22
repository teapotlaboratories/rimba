"""Generate a self-contained HTML report from the regression-suite JSON baselines.

Reads whichever of build/regtest/{T0,T1,T2}-latest.json exist and writes a single
build/regtest/report.html — no external CSS/JS/fonts, theme-aware (light/dark), so it opens
anywhere and can be shared as one file. Called automatically at the end of every tier run
(run.py), and available standalone via `python tools/regtest/run.py report`.

The report is a VIEW of the baselines; it never re-runs anything. Each tier section reflects
that tier's last run (the JSON carries its own git/gitlink stamp + timestamp).
"""

from __future__ import annotations

import html
import json
import time
from pathlib import Path
from typing import Optional

from .common import RESULTS_DIR
from .manifest import (
    EXPECTED_CHIP_ID,
    EXPECTED_FW_SHA256,
    EXPECTED_FW_SIZE,
    EXPECTED_FW_VERSION,
    POWER_NOPS_VALID_MA,
)

TIERS = ("T0", "T1", "T2", "TP")

#: status -> (badge css class, short label)
_STATUS = {
    "PASS": ("ok", "PASS"),
    "FAIL": ("err", "FAIL"),
    "SKIP": ("muted", "SKIP"),
    "INCONCLUSIVE": ("bug", "INCONCLUSIVE"),
    "XFAIL": ("muted", "XFAIL"),
    "XPASS": ("err", "XPASS"),
}

_TIER_BLURB = {
    "T0": "Build matrix — every app × board compiles via <code>make</code>, with a real country "
          "code asserted. No hardware; catches port-forward breakage.",
    "T1": "Smoke — flash + boot + the radio comes up (chip id / fw / MAC / runtime country) on real "
          "silicon. One board, no peer.",
    "T2": "On-air feature tests — each milestone claim, self-reported by a <code>test-*</code> "
          "app over the <code>TEST|</code> contract. Needs a rig.",
    "TP": "Power-save tier — the PPK2 meters board2's PS-ladder current; the host segments the "
          "stream and gates a <em>gross</em> current regression (raw mA always recorded; doze depth "
          "is a benchmark, not a gate). Needs board2 + the PPK2 + the C6 + an AP.",
}


def _load(tier: str) -> Optional[dict]:
    p = RESULTS_DIR / f"{tier}-latest.json"
    if not p.exists():
        return None
    try:
        return json.loads(p.read_text())
    except Exception:
        return None


def _esc(s) -> str:
    return html.escape(str(s), quote=False)


def _counts_line(counts: dict) -> str:
    parts = []
    for k in ("PASS", "FAIL", "SKIP", "INCONCLUSIVE", "XFAIL", "XPASS"):
        v = counts.get(k, 0)
        if v:
            cls = _STATUS.get(k, ("muted", k))[0]
            parts.append(f'<span class="pill {cls}">{v} {k.lower()}</span>')
    return " ".join(parts) or '<span class="pill muted">no results</span>'


def _tier_gated(tier_data: dict) -> bool:
    """A tier is green when nothing FAILed and nothing XPASSed."""
    c = tier_data.get("counts", {})
    return c.get("FAIL", 0) == 0 and c.get("XPASS", 0) == 0


def _ma(x) -> str:
    try:
        return f"{float(x):.1f} mA"
    except (TypeError, ValueError):
        return "—"


def _tp_cells(meta: dict) -> tuple[str, str]:
    """(measured, needs-to-pass) for a power-tier row, from its structured meta."""
    measured = _ma(meta.get("median_ma"))
    p10, p90 = meta.get("p10_ma"), meta.get("p90_ma")
    if isinstance(p10, (int, float)) and isinstance(p90, (int, float)):
        measured += f' <span class="pxx">(p10 {p10:.0f} / p90 {p90:.0f})</span>'
    band = meta.get("band")
    tname = meta.get("tier")
    if isinstance(band, (list, tuple)) and len(band) == 2:
        need = f"&le; {band[0]:.0f} mA<span class='pxx'> · fail &ge; {band[1]:.0f}</span>"
    elif tname == "no-ps":
        lo, hi = POWER_NOPS_VALID_MA
        need = f"{lo:.0f}&ndash;{hi:.0f} mA<span class='pxx'> · validity anchor</span>"
    elif tname == "twt":
        need = "<span class='pxx'>recorded · AP-dependent, not scored</span>"
    else:
        need = "—"
    return measured, need


def _cells(tier: str, r: dict) -> tuple[str, str]:
    """Resolve (measured, needs-to-pass) HTML for one result.

    Prefers explicit observed/required fields (future baselines carry them); otherwise
    derives from the structured data already in the baseline — TP from meta, T2 from the
    test catalogue's gate, T1/T0 from the read-back vs the expected constants.
    """
    obs = _esc((r.get("observed") or "").strip())
    req = _esc((r.get("required") or "").strip())
    if obs or req:
        return obs or "—", req or "—"

    meta = r.get("meta") or {}
    status = r.get("status", "")
    name = r.get("name", "")
    detail = _esc((r.get("detail") or "").strip())

    if tier == "TP":
        return _tp_cells(meta)

    if tier == "T2":
        from .t2_tests import T2_BY_SLUG
        t = T2_BY_SLUG.get(name)
        if t is not None:
            measured = detail or f'<span class="pxx">{_esc(status.lower())}</span>'
            return measured, (_esc(t.pass_if) if t.pass_if else "—")
        return (detail or "—"), "—"

    if tier == "T1":
        if "chip=" in detail:
            need = (f"chip {_esc(EXPECTED_CHIP_ID)} · fw {_esc(EXPECTED_FW_VERSION)}"
                    f"<span class='pxx'> · country&ne;?? · real MAC</span>")
            return detail, need
        if name.endswith("present"):
            return (detail or "enumerated"), "board present"
        return (detail or "—"), "—"

    if tier == "T0":
        if "fw-blob" in name or "mm6108" in detail:
            need = (f"{EXPECTED_FW_SIZE} B<span class='pxx'> · sha {_esc(EXPECTED_FW_SHA256[:8])}… "
                    f"· v{_esc(EXPECTED_FW_VERSION)}</span>")
            return (detail or "—"), need
        if status == "PASS":
            return "compiled", "compiles"
        if status == "XFAIL":
            return (detail[:70] or "build error"), \
                   "compiles<span class='pxx'> · XFAIL known-broken</span>"
        if status in ("FAIL", "XPASS"):
            return (detail[:70] or status.lower()), "compiles"
        return (detail or "—"), "—"

    return (detail or "—"), "—"


def _result_rows(results: list, tier: str) -> str:
    rows = []
    for r in results:
        status = r.get("status", "?")
        cls, lbl = _STATUS.get(status, ("muted", status))
        name = _esc(r.get("name", ""))
        dur = r.get("duration_s") or 0
        dur_s = f"{dur:.1f}s" if dur else ""
        detail = _esc(r.get("detail", "")).strip()
        ev = (r.get("evidence") or "").strip()
        measured, need = _cells(tier, r)
        # The detail line stays only for non-PASS rows (the failure/inconclusive explanation),
        # matching the console — and only when it adds more than the measured cell already shows
        # (for T2/T1 the measured cell IS the detail, so it would just duplicate).
        detail_html = (f'<div class="detail">{detail}</div>'
                       if detail and status != "PASS" and detail not in measured else "")
        ev_html = ""
        if ev:
            ev_html = (f'<details class="ev"><summary>evidence</summary>'
                       f'<pre>{_esc(ev)}</pre></details>')
        rows.append(
            f'<tr class="r-{cls}">'
            f'<td><span class="badge {cls}">{lbl}</span></td>'
            f'<td>{name}{detail_html}{ev_html}</td>'
            f'<td class="mcol {cls}">{measured}</td>'
            f'<td class="pcol">{need}</td>'
            f'<td class="num">{dur_s}</td>'
            f'</tr>')
    return "\n".join(rows)


def _tier_section(tier: str, data: Optional[dict]) -> str:
    if data is None:
        return (f'<section class="tier"><h2>{tier} '
                f'<span class="pill muted">not run</span></h2>'
                f'<p class="blurb">{_TIER_BLURB[tier]}</p>'
                f'<p class="muted">No <code>{tier}-latest.json</code> — this tier has not been run '
                f'in this tree.</p></section>')
    green = _tier_gated(data)
    # complete is explicitly False only on a run reaped before it finished (old baselines lack the field
    # → None → treated as unknown, not warned). A partial all-PASS prefix would otherwise read as green.
    incomplete = data.get("complete") is False
    stamp = (f'{_esc(data.get("generated", "?"))} · git '
             f'<code>{_esc(data.get("git", "?"))}</code> · halow '
             f'<code>{_esc(data.get("halow_gitlink", "?"))}</code> · '
             f'{int(data.get("duration_s", 0))}s')
    warn = ('<p style="margin:.4em 0;padding:.6em .9em;border-radius:8px;border:1px solid var(--err);'
            'color:var(--err);background:color-mix(in srgb,var(--err) 12%,transparent);font-weight:600">'
            '⚠ INCOMPLETE — this run did not finish (reaped / killed mid-way); the counts below are a '
            'partial prefix, not a full result. Re-run this tier.</p>' if incomplete else '')
    pill = ('<span class="pill err">INCOMPLETE</span>' if incomplete
            else f'<span class="pill {"ok" if green else "err"}">{"green" if green else "RED"}</span>')
    return (
        f'<section class="tier">'
        f'<h2>{tier} {pill}</h2>'
        f'<p class="blurb">{_TIER_BLURB[tier]}</p>'
        f'{warn}'
        f'<div class="stamp">{stamp}</div>'
        f'<div class="counts">{_counts_line(data.get("counts", {}))}</div>'
        f'<div class="tw"><table><thead><tr><th>status</th><th>test</th><th>measured</th>'
        f'<th>needs to pass</th><th class="num">time</th></tr></thead>'
        f'<tbody>{_result_rows(data.get("results", []), tier)}</tbody></table></div>'
        f'</section>')


def _summary_cards(loaded: dict) -> str:
    cards = []
    for tier in TIERS:
        d = loaded.get(tier)
        if d is None:
            cards.append(f'<div class="card"><div class="cnum muted">—</div>'
                         f'<div class="clbl">{tier}: not run</div></div>')
            continue
        c = d.get("counts", {})
        green = _tier_gated(d)
        headline = f'{c.get("PASS", 0)} pass'
        if c.get("FAIL", 0):
            headline += f' / {c["FAIL"]} fail'
        cards.append(
            f'<div class="card"><div class="cnum {"ok" if green else "err"}">{headline}</div>'
            f'<div class="clbl">{tier} — {_counts_line(c)}</div></div>')
    return f'<div class="cards">{"".join(cards)}</div>'


def _flake_section() -> str:
    """A flake-ledger block: which tests flipped verdict run-to-run (read from history.jsonl).

    This is the durable counterpart to the latest-wins cards above -- the cards show the LAST run per
    tier; this shows the whole history, so a transient that a retry overwrote is still visible."""
    try:
        from . import ledger
        hist = ledger.load_history()
    except Exception:
        return ""
    intro = ('Every result is appended to <code>build/regtest/history.jsonl</code> (never overwritten), '
             'so a flake a retry masked is still recorded. <code>make test-flakes</code> for the CLI view.')
    if not hist:
        return (f'<section class="tier"><h2>Flake ledger</h2><p class="muted">No run history yet. {intro}'
                f'</p></section>')
    tests = ledger.summarize(hist)
    flaky = sorted(((n, t) for n, t in tests.items() if t["flaky"]), key=lambda nt: -nt[1]["bad"])
    brk = sorted(n for n, t in tests.items() if t["broken"])
    run_ids = {r.get("run_id") for r in hist if r.get("run_id")}
    head = (f'<section class="tier"><h2>Flake ledger '
            f'<span class="sub">{len(hist)} results · {len(run_ids)} runs · {len(tests)} tests</span></h2>'
            f'<p class="muted">{intro}</p>')
    if not flaky and not brk:
        return head + ('<p class="ok">No flakes — every test that ran more than once gave a consistent '
                       'verdict.</p></section>')
    body = ""
    if flaky:
        rows = ""
        for name, t in flaky:
            cnt = ", ".join(f'{v}&nbsp;{s}' for s, v in t["by_status"].items())
            recent = " ".join(t.get("recent", []))
            # A flip at ONE git SHA is a genuine transient; a flip that straddles SHAs may be a regression.
            kind = ('<span class="ok">transient (same code)</span>' if t.get("same_code_flake")
                    else '<span class="err">flipped across code — verify</span>')
            last_cls = "err" if t["last_status"] in ("FAIL", "INCONCLUSIVE", "XPASS") else "ok"
            rows += (f'<tr><td>{_esc(t["tier"])}</td><td><code>{_esc(name)}</code></td>'
                     f'<td>{cnt}</td><td>{kind}</td><td><code>{_esc(recent)}</code></td>'
                     f'<td class="{last_cls}">{_esc(t["last_status"])}</td></tr>')
        body += ('<p><b>Flaky</b> — flipped verdict run-to-run. A flip at the <em>same</em> code SHA is a '
                 'bench/RF/rig transient; one that <em>straddles a code change</em> may be a real '
                 'regression (the <b>kind</b> column):</p>'
                 '<table class="ledger"><thead><tr><th>tier</th><th>test</th><th>outcomes</th>'
                 '<th>kind</th><th>recent</th><th>last</th></tr></thead><tbody>' + rows + '</tbody></table>')
    if brk:
        body += ('<p class="muted"><b>Consistently failing</b> (a bad outcome and never a good one → a '
                 'real defect, not a flake; a rig-gated test that SKIPs-when-absent but FAILs-when-run '
                 'lands here): ' + ", ".join(f'<code>{_esc(n)}</code>' for n in brk) + '.</p>')
    return head + body + '</section>'


def _trend_section() -> str:
    """Numbers-that-drift: recorded numeric metrics with a sparkline over recent runs (from history.jsonl).

    Complements the flake section: that catches pass<->fail flips, this catches a number that moves while
    the test still PASSes (tp median_ma creeping up, a dscycle reconnect getting slower)."""
    try:
        from . import ledger
        trends = ledger.metric_trends()
    except Exception:
        return ""
    if not trends:
        return ""
    rows = ""
    for tname in sorted(trends):
        for metric in sorted(trends[tname]):
            vals = [v for (_ts, _g, _hl, v) in trends[tname][metric][-8:]]
            if len(vals) < 2:            # need at least 2 runs to show a trend
                continue
            first, latest = vals[0], vals[-1]
            pct = 100.0 * (latest - first) / first if first else 0.0
            cls = "err" if abs(pct) >= 20 else ("warn" if abs(pct) >= 5 else "muted")
            rows += (f'<tr><td>{_esc(tname)}</td><td><code>{_esc(metric)}</code></td>'
                     f'<td style="font-size:15px">{_esc(ledger.sparkline(vals))}</td>'
                     f'<td>{latest:g}</td><td class="{cls}">{pct:+.0f}%</td></tr>')
    if not rows:
        return ""
    return ('<section class="tier"><h2>Metric trends <span class="sub">numbers that drift</span></h2>'
            '<p class="muted">A recorded number that moves while the test still PASSes (tp mA, dscycle '
            'latency). Compare across SDK versions with <code>make test-trend DIFF="&lt;gitlinkA&gt; '
            '&lt;gitlinkB&gt;"</code>; &ge;20% is flagged.</p>'
            '<table class="ledger"><thead><tr><th>test</th><th>metric</th><th>recent</th><th>latest</th>'
            '<th>&Delta; first&rarr;last</th></tr></thead><tbody>' + rows + '</tbody></table></section>')


def generate(out_path: Optional[Path] = None) -> Path:
    """Write the HTML report from the current baselines. Returns the path."""
    loaded = {t: _load(t) for t in TIERS}
    out_path = out_path or (RESULTS_DIR / "report.html")
    RESULTS_DIR.mkdir(parents=True, exist_ok=True)

    # A tree stamp: prefer T2's, else the most recent available.
    stamp_src = next((loaded[t] for t in ("T2", "T1", "T0") if loaded[t]), None)
    git = _esc(stamp_src.get("git", "unknown")) if stamp_src else "unknown"
    generated = time.strftime("%Y-%m-%d %H:%M:%S%z")

    sections = "\n".join(_tier_section(t, loaded[t]) for t in TIERS)
    body = (
        f'<header class="top"><div class="wrap">'
        f'<div class="title">Rimba regression suite <span class="sub">report</span></div>'
        f'<div class="meta">generated {generated} · tree <code>{git}</code></div>'
        f'<button class="tgl" id="themebtn" onclick="tt()" title="Toggle light / dark theme">🌙 Dark</button>'
        f'</div></header>'
        f'<main class="wrap">'
        f'{_summary_cards(loaded)}'
        f'<p class="legendline">Every row shows its <b>measured</b> value and what it '
        f'<b>needs to pass</b>. That reads differently per tier: <b>TP</b> = a current in mA vs '
        f'the calibrated band (<code>&le; pass</code> · <code>fail &ge;</code>); <b>T2</b> = the '
        f'observed outcome vs the structural gate; <b>T1</b> = the radio read-back vs the expected '
        f'chip/fw/country; <b>T0</b> = compiles + the pinned fw-blob. Doze <em>depth</em> and RF '
        f'numbers are benchmarks, reported not gated.</p>'
        f'{sections}'
        f'{_flake_section()}'
        f'{_trend_section()}'
        f'<p class="foot">Generated by <code>tools/regtest/report.py</code> from '
        f'<code>build/regtest/{{T0,T1,T2,TP}}-latest.json</code>. This is a view of the last run per '
        f'tier — re-run a tier to refresh it. Full narrative: '
        f'<code>docs/regression/rimba-regression-results.md</code>.</p>'
        f'</main>')

    out_path.write_text(_PAGE.replace("__BODY__", body))
    return out_path


# --- self-contained page shell (inline CSS + theme toggle; no external assets) --------------
_PAGE = """<!doctype html>
<html lang="en" data-theme="auto">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Rimba regression report</title>
<style>
:root{ --bg:#f6f8fb; --fg:#1b2230; --muted:#5a6577; --line:#e3e8f0; --card:#fff; --accent:#2f6feb;
  --ok:#1a8a4a; --ok-bg:#e6f6ec; --err:#c0392b; --err-bg:#fdecea; --warn:#b7791f; --warn-bg:#fbf3e2;
  --bug:#8a3ffc; --bug-bg:#f2ecfe; --code:#0c1018; --code-fg:#dfe6f0;
  --shadow:0 1px 2px rgba(24,39,75,.06),0 6px 18px rgba(24,39,75,.06); }
@media (prefers-color-scheme:dark){:root[data-theme=auto]{ --bg:#0b0e14; --fg:#d6dde8; --muted:#8894a6;
  --line:#1f2733; --card:#131922; --accent:#5b93ff; --ok:#4ade80; --ok-bg:#0f2a1c; --err:#f87171;
  --err-bg:#2c1517; --warn:#f2c14e; --warn-bg:#2c2410; --bug:#c4b5fd; --bug-bg:#1e1636; --code:#080b11;
  --shadow:0 1px 2px rgba(0,0,0,.4);}}
:root[data-theme=dark]{ --bg:#0b0e14; --fg:#d6dde8; --muted:#8894a6; --line:#1f2733; --card:#131922;
  --accent:#5b93ff; --ok:#4ade80; --ok-bg:#0f2a1c; --err:#f87171; --err-bg:#2c1517; --warn:#f2c14e;
  --warn-bg:#2c2410; --bug:#c4b5fd; --bug-bg:#1e1636; --code:#080b11; --shadow:0 1px 2px rgba(0,0,0,.4);}
:root{color-scheme:light dark}
*{box-sizing:border-box} body{margin:0;background:var(--bg);color:var(--fg);
  font:15px/1.6 -apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,Helvetica,Arial,sans-serif;-webkit-font-smoothing:antialiased}
code{font-family:"SF Mono",ui-monospace,Menlo,Consolas,monospace;font-size:.88em;background:var(--line);padding:.08em .35em;border-radius:4px}
.wrap{width:100%;margin:0 auto;padding:0 clamp(16px,2.5vw,40px)}
.top{position:sticky;top:0;z-index:5;background:color-mix(in srgb,var(--bg) 85%,transparent);backdrop-filter:blur(8px);border-bottom:1px solid var(--line);padding:12px 0}
.top .wrap{display:flex;align-items:baseline;gap:14px}
.title{font-size:1.2rem;font-weight:700} .title .sub{color:var(--muted);font-weight:400;font-size:.9rem}
.meta{color:var(--muted);font-size:12.5px} .meta code{background:none;padding:0}
.tgl{margin-left:auto;border:1px solid var(--line);background:var(--card);color:var(--fg);height:30px;padding:0 12px;border-radius:8px;cursor:pointer;font-size:12.5px;font-weight:600;display:inline-flex;align-items:center;gap:6px;white-space:nowrap}
.tgl:hover{border-color:var(--accent);color:var(--accent)}
main.wrap{padding-top:22px;padding-bottom:60px}
.cards{display:grid;grid-template-columns:repeat(auto-fit,minmax(210px,1fr));gap:12px;margin:6px 0 26px}
.card{background:var(--card);border:1px solid var(--line);border-radius:12px;padding:14px 16px;box-shadow:var(--shadow)}
.cnum{font-size:1.35rem;font-weight:700} .cnum.ok{color:var(--ok)} .cnum.err{color:var(--err)} .cnum.muted{color:var(--muted)}
.clbl{font-size:12px;color:var(--muted);margin-top:5px;display:flex;flex-wrap:wrap;gap:5px;align-items:center}
.tier{margin:30px 0} .tier h2{font-size:1.3rem;margin:.2em 0 .3em;display:flex;gap:10px;align-items:center}
.blurb{color:var(--muted);font-size:13.5px;margin:.2em 0 .5em;max-width:92ch} .stamp{color:var(--muted);font-size:12px;margin-bottom:8px}
.counts{display:flex;flex-wrap:wrap;gap:6px;margin-bottom:10px}
.pill{font-size:11.5px;font-weight:600;padding:2px 8px;border-radius:20px;background:var(--line);color:var(--muted);white-space:nowrap}
.pill.ok{background:var(--ok-bg);color:var(--ok)} .pill.err{background:var(--err-bg);color:var(--err)}
.pill.warn{background:var(--warn-bg);color:var(--warn)} .pill.bug{background:var(--bug-bg);color:var(--bug)}
.tw{overflow-x:auto;margin:2px 0}
table{border-collapse:collapse;width:100%;font-size:13.5px}
th,td{border:1px solid var(--line);padding:7px 10px;text-align:left;vertical-align:top}
th{position:relative;white-space:nowrap}
thead th{background:color-mix(in srgb,var(--line) 45%,transparent);font-weight:650}
.resizer{position:absolute;top:0;right:-1px;width:9px;height:100%;cursor:col-resize;user-select:none;z-index:2;touch-action:none}
.resizer::before{content:"";position:absolute;top:22%;right:3px;height:56%;width:2px;background:var(--line);border-radius:2px}
.resizer:hover::before,.resizer.drag::before{background:var(--accent)}
body.resizing{cursor:col-resize;user-select:none}
td.num,th.num{text-align:right;font-variant-numeric:tabular-nums;white-space:nowrap;color:var(--muted)}
td.mcol{font-variant-numeric:tabular-nums;font-weight:600;color:var(--fg)}
td.mcol.ok{color:var(--ok)} td.mcol.err{color:var(--err)} td.mcol.bug{color:var(--bug)} td.mcol.muted{color:var(--muted)}
td.pcol{color:var(--muted)} .pxx{color:var(--muted);font-weight:400;font-size:11px}
.legendline{color:var(--muted);font-size:12.5px;line-height:1.55;margin:2px 0 20px;padding:10px 14px;background:var(--card);border:1px solid var(--line);border-radius:10px;max-width:118ch}
.legendline code{background:var(--line);padding:.05em .3em}
.badge{display:inline-block;font-size:11px;font-weight:700;padding:2px 8px;border-radius:6px;background:var(--line);color:var(--muted);white-space:nowrap}
.badge.ok{background:var(--ok-bg);color:var(--ok)} .badge.err{background:var(--err-bg);color:var(--err)}
.badge.warn{background:var(--warn-bg);color:var(--warn)} .badge.bug{background:var(--bug-bg);color:var(--bug)}
tr.r-err{background:color-mix(in srgb,var(--err-bg) 40%,transparent)}
.detail{color:var(--muted);font-size:12.5px;margin-top:4px;white-space:pre-wrap}
details.ev{margin-top:6px} details.ev summary{cursor:pointer;color:var(--accent);font-size:12px}
details.ev pre{background:var(--code);color:var(--code-fg);border-radius:8px;padding:10px 12px;overflow-x:auto;font-size:12px;line-height:1.5;margin:6px 0 0}
.foot{color:var(--muted);font-size:12.5px;margin-top:34px;border-top:1px solid var(--line);padding-top:16px}
.muted{color:var(--muted)}
</style>
</head>
<body>
__BODY__
<script>
function effTheme(){var t=document.documentElement.getAttribute('data-theme');
  if(t==='dark'||t==='light')return t;
  return window.matchMedia&&window.matchMedia('(prefers-color-scheme: dark)').matches?'dark':'light';}
function paintToggle(){var b=document.getElementById('themebtn');if(!b)return;
  var e=effTheme();b.textContent=e==='dark'?'☀ Light':'🌙 Dark';
  b.setAttribute('aria-label','Switch to '+(e==='dark'?'light':'dark')+' theme');}
function tt(){var r=document.documentElement,n=effTheme()==='dark'?'light':'dark';
  r.setAttribute('data-theme',n);try{localStorage.setItem('rt',n)}catch(e){}paintToggle();}
function initResizers(){
  document.querySelectorAll('table thead th').forEach(function(th){
    if(th.querySelector('.resizer'))return;
    var g=document.createElement('span');g.className='resizer';
    g.title='Drag to resize · double-click to reset';th.appendChild(g);
    g.addEventListener('mousedown',function(ev){
      var sx=ev.pageX,sw=th.offsetWidth;g.classList.add('drag');document.body.classList.add('resizing');
      function mv(e){var w=Math.max(40,sw+(e.pageX-sx));th.style.width=w+'px';th.style.minWidth=w+'px';}
      function up(){g.classList.remove('drag');document.body.classList.remove('resizing');
        document.removeEventListener('mousemove',mv);document.removeEventListener('mouseup',up);}
      document.addEventListener('mousemove',mv);document.addEventListener('mouseup',up);ev.preventDefault();});
    g.addEventListener('dblclick',function(){th.style.width='';th.style.minWidth='';});
  });
}
(function(){try{var t=localStorage.getItem('rt');if(t)document.documentElement.setAttribute('data-theme',t)}catch(e){}
  try{window.matchMedia('(prefers-color-scheme: dark)').addEventListener('change',paintToggle)}catch(e){}
  paintToggle();initResizers();})();
</script>
</body>
</html>
"""
