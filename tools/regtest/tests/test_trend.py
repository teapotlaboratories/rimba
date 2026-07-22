"""Unit tests for the 'numbers that drift' trend + gitlink-diff ledger view (no hardware).

Run directly:   python tools/regtest/tests/test_trend.py
or discovered:  make test-unit

Covers: the numeric meta a run records is persisted + bounded; a scalar/list metric is extracted for
trending; a metric that DRIFTS across runs or MOVES across two SDK gitlinks is surfaced (the exact class
-- e.g. the fw-1.17.9 ~2x power-save regression -- that every pass/fail test misses).
"""

import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[3]))
from tools.regtest import ledger  # noqa: E402


def _rec(name, hl, meta, status="PASS", git="sha"):
    return {"name": name, "status": status, "tier": "TP", "git": git, "halow_gitlink": hl, "meta": meta}


class TestMetaPersistence(unittest.TestCase):
    def test_ledger_meta_keeps_numbers_drops_junk(self):
        m = ledger._ledger_meta({
            "median_ma": 23.9, "wake_cycles": 3, "twt_installed": True, "wake": "commanded-c6",
            "latencies_ms": list(range(100)),           # long numeric list -> truncated to 32
            "huge_str": "x" * 500,                       # long string -> dropped
            "nested": {"a": 1},                          # non-scalar -> dropped
        })
        self.assertEqual(m["median_ma"], 23.9)
        self.assertEqual(m["wake_cycles"], 3)
        self.assertIs(m["twt_installed"], True)
        self.assertEqual(m["wake"], "commanded-c6")
        self.assertEqual(len(m["latencies_ms"]), 32)
        self.assertNotIn("huge_str", m)
        self.assertNotIn("nested", m)

    def test_scalar_metrics_lists_reduce_to_mean_bool_dropped(self):
        s = ledger._scalar_metrics({"median_ma": 20.0, "latencies_ms": [10, 20, 30], "twt_installed": True})
        self.assertEqual(s["median_ma"], 20.0)
        self.assertEqual(s["latencies_ms"], 20.0)   # mean of 10,20,30
        self.assertNotIn("twt_installed", s)         # bool is not a trend metric


class TestTrendAndDiff(unittest.TestCase):
    def test_metric_trends_in_order(self):
        hist = [_rec("tp:no-ps", "sdkA", {"median_ma": 62}),
                _rec("tp:no-ps", "sdkA", {"median_ma": 64}),
                _rec("tp:no-ps", "sdkB", {"median_ma": 63})]
        tr = ledger.metric_trends(hist)["tp:no-ps"]["median_ma"]
        self.assertEqual([v for (_ts, _g, _hl, v) in tr], [62.0, 64.0, 63.0])

    def test_diff_gitlinks_flags_a_2x_regression(self):
        # the canonical drift: No-PS current doubles across an SDK bump while the test still "passes".
        hist = [_rec("tp:no-ps (No-PS)", "aaaa1111beef", {"median_ma": 63.0}),
                _rec("tp:no-ps (No-PS)", "bbbb2222cafe", {"median_ma": 126.0}),
                _rec("tp:dyn-ps (Dyn-PS)", "aaaa1111beef", {"median_ma": 24.0}),
                _rec("tp:dyn-ps (Dyn-PS)", "bbbb2222cafe", {"median_ma": 25.0})]
        rows, (a, b) = ledger.diff_gitlinks("aaaa", "bbbb", hist)   # prefixes
        self.assertEqual((a, b), ("aaaa1111beef", "bbbb2222cafe"))
        by = {r["test"]: r for r in rows}
        self.assertAlmostEqual(by["tp:no-ps (No-PS)"]["pct"], 100.0)     # doubled
        self.assertAlmostEqual(by["tp:dyn-ps (Dyn-PS)"]["pct"], 100.0 * 1 / 24)
        self.assertEqual(rows[0]["test"], "tp:no-ps (No-PS)")           # biggest move first
        self.assertIn(">20% MOVE", ledger.format_diff("aaaa", "bbbb", hist))

    def test_diff_unresolvable_gitlink(self):
        hist = [_rec("t", "aaaa1111", {"x": 1})]
        rows, _ = ledger.diff_gitlinks("aaaa", "zzzz", hist)
        self.assertIsNone(rows)
        self.assertIn("Could not resolve", ledger.format_diff("aaaa", "zzzz", hist))

    def test_format_trend_empty_and_populated(self):
        self.assertIn("No numeric metrics", ledger.format_trend([]))
        out = ledger.format_trend([_rec("tp:x", "s", {"median_ma": 20}),
                                   _rec("tp:x", "s", {"median_ma": 40})])
        self.assertIn("median_ma", out)
        self.assertIn("+100%", out)


class TestReporterStoresMeta(unittest.TestCase):
    def test_reporter_add_persists_meta_for_trending(self):
        from tools.regtest import common
        with tempfile.TemporaryDirectory() as d:
            dp = Path(d)
            old_rd, old_hp = common.RESULTS_DIR, ledger.HISTORY_PATH
            common.RESULTS_DIR = dp
            ledger.HISTORY_PATH = dp / "history.jsonl"
            try:
                rep = common.Reporter("TP", quiet=True)
                rep.add(common.Result("TP", "tp:no-ps", common.PASS, meta={"median_ma": 62.5}))
                hist = ledger.load_history()
            finally:
                common.RESULTS_DIR, ledger.HISTORY_PATH = old_rd, old_hp
            self.assertEqual(hist[0]["meta"]["median_ma"], 62.5)
            self.assertEqual(ledger.metric_trends(hist)["tp:no-ps"]["median_ma"][0][3], 62.5)


if __name__ == "__main__":
    unittest.main()
