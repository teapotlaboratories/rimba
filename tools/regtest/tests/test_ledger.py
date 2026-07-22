"""Unit tests for the run-history flake ledger (no hardware, no bench).

Run directly:   python tools/regtest/tests/test_ledger.py
or discovered:  make test-unit

Covers the core promise: the ledger is append-only (a retried transient is recorded, not overwritten),
and a test that FLIPS verdict run-to-run is surfaced as flaky while a stable-pass or a consistently-broken
test is not.
"""

import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[3]))
from tools.regtest import ledger  # noqa: E402


class _FakeResult:
    def __init__(self, name, status, detail="", duration_s=0.0):
        self.name = name
        self.status = status
        self.detail = detail
        self.duration_s = duration_s


def _hist(rows):
    """rows: list of (name, status, run_id) -> history dicts."""
    return [{"name": n, "status": s, "tier": "T2", "git": "abc123", "run_id": r} for (n, s, r) in rows]


class TestFlakeDetection(unittest.TestCase):
    def test_flip_verdict_is_flaky(self):
        hist = _hist([
            ("mesh-linux", "FAIL", "r1"), ("mesh-linux", "PASS", "r2"), ("mesh-linux", "PASS", "r3"),
            ("sta", "PASS", "r1"), ("sta", "PASS", "r2"),
            ("broken", "FAIL", "r1"), ("broken", "FAIL", "r2"),
        ])
        s = ledger.summarize(hist)
        self.assertTrue(s["mesh-linux"]["flaky"], "a test that both passed and failed is flaky")
        self.assertFalse(s["sta"]["flaky"], "always-pass is not flaky")
        self.assertFalse(s["broken"]["flaky"], "always-fail is broken, not flaky")
        self.assertTrue(s["broken"]["broken"])
        self.assertEqual(set(ledger.flakes(hist)), {"mesh-linux"})
        self.assertEqual(set(ledger.broken(hist)), {"broken"})
        # counts survive across runs (nothing overwritten)
        self.assertEqual(s["mesh-linux"]["by_status"], {"FAIL": 1, "PASS": 2})
        self.assertEqual(s["mesh-linux"]["last_status"], "PASS")

    def test_inconclusive_counts_as_bad(self):
        hist = _hist([("dscycle", "PASS", "r1"), ("dscycle", "INCONCLUSIVE", "r2")])
        self.assertTrue(ledger.summarize(hist)["dscycle"]["flaky"])

    def test_skip_is_neutral_not_good(self):
        s = ledger.summarize
        # SKIP + PASS: ran once and passed; SKIP is "didn't run" -> neither flaky nor broken.
        x = s(_hist([("x", "SKIP", "r1"), ("x", "PASS", "r2")]))["x"]
        self.assertFalse(x["flaky"])
        self.assertFalse(x["broken"])
        # SKIP + FAIL: a rig-gated test that FAILs 100% when it runs must be BROKEN, not flaky — SKIP
        # (rig absent) must not rescue it into "flaky/transient". This is the core review finding.
        y = s(_hist([("y", "SKIP", "r1"), ("y", "FAIL", "r2"), ("y", "FAIL", "r3")]))["y"]
        self.assertFalse(y["flaky"], "SKIP must not make a fail-when-run test look flaky")
        self.assertTrue(y["broken"])
        # all-SKIP: never actually ran -> neither.
        z = s(_hist([("z", "SKIP", "r1"), ("z", "SKIP", "r2")]))["z"]
        self.assertFalse(z["flaky"])
        self.assertFalse(z["broken"])
        # XFAIL is an expected-good outcome.
        self.assertFalse(s(_hist([("w", "XFAIL", "r1"), ("w", "XFAIL", "r2")]))["w"]["flaky"])

    def test_same_code_flake_vs_crosscommit(self):
        def rows(triples):   # (status, run_id, git)
            return [{"name": "t", "status": st, "tier": "T2", "git": g, "run_id": r}
                    for (st, r, g) in triples]
        same = ledger.summarize(rows([("PASS", "r1", "shaA"), ("FAIL", "r2", "shaA")]))["t"]
        self.assertTrue(same["flaky"])
        self.assertTrue(same["same_code_flake"], "PASS+FAIL at one SHA is a genuine transient")
        cross = ledger.summarize(rows([("PASS", "r1", "shaA"), ("FAIL", "r2", "shaB")]))["t"]
        self.assertTrue(cross["flaky"])
        self.assertFalse(cross["same_code_flake"], "a flip straddling a code change may be a regression")

    def test_single_run_not_flaky(self):
        self.assertEqual(ledger.flakes(_hist([("z", "FAIL", "r1")])), {})

    def test_same_code_flake_keys_on_halow_gitlink_not_just_git(self):
        # During a forward-port the superproject is `<sha>-dirty` on every hop, so keying same_code on git
        # alone would merge different morselib SDK states. A flip at the SAME git but a DIFFERENT halow
        # gitlink must NOT be called a same-code transient (it may be a real cross-SDK regression).
        def rec(status, hl, git="app-dirty"):
            return {"name": "t", "status": status, "tier": "T2", "git": git, "halow_gitlink": hl}
        cross = ledger.summarize([rec("PASS", "sdk_2112"), rec("FAIL", "sdk_2123")])["t"]
        self.assertTrue(cross["flaky"])
        self.assertFalse(cross["same_code_flake"], "same git, different halow gitlink -> possible regression")
        same = ledger.summarize([rec("PASS", "sdk_2123"), rec("FAIL", "sdk_2123")])["t"]
        self.assertTrue(same["same_code_flake"], "same git AND same halow gitlink -> a transient")


class TestAppendLoad(unittest.TestCase):
    def test_append_is_additive_and_roundtrips(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "history.jsonl"
            ledger.append_result("run1", "T2", "abc", "def", _FakeResult("t", "PASS"), path=p)
            ledger.append_result("run2", "T2", "abc", "def", _FakeResult("t", "FAIL", "boom", 1.25), path=p)
            hist = ledger.load_history(p)
            self.assertEqual([r["status"] for r in hist], ["PASS", "FAIL"])  # both preserved, in order
            self.assertEqual(hist[1]["duration_s"], 1.2)
            self.assertTrue(ledger.summarize(hist)["t"]["flaky"])

    def test_load_tolerates_torn_final_line(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "history.jsonl"
            p.write_text('{"name":"a","status":"PASS"}\n{"name":"b","status":')  # reaped mid-write
            self.assertEqual(len(ledger.load_history(p)), 1)

    def test_load_skips_nondict_lines(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "history.jsonl"
            # shape-valid JSON that isn't an object must be skipped, and summarize must not crash on it.
            p.write_text('{"name":"a","status":"PASS"}\n123\nnull\n["x"]\n{"name":"b","status":"FAIL"}\n')
            hist = ledger.load_history(p)
            self.assertEqual([r["name"] for r in hist], ["a", "b"])
            ledger.summarize(hist)   # no AttributeError

    def test_missing_file_is_empty(self):
        self.assertEqual(ledger.load_history(Path("/nonexistent/history.jsonl")), [])

    def test_format_report_empty_and_flaky(self):
        self.assertIn("No history", ledger.format_report([]))
        rep = ledger.format_report(_hist([("q", "FAIL", "r1"), ("q", "PASS", "r2")]))
        self.assertIn("FLAKY", rep)
        self.assertIn("q", rep)


class TestReporterHook(unittest.TestCase):
    """The real Reporter.add() records to the ledger — verified off-bench by redirecting both the
    baseline dir and the ledger path to a temp dir (so it never touches build/regtest)."""

    def test_reporter_add_records_each_result_under_one_run_id(self):
        from tools.regtest import common
        with tempfile.TemporaryDirectory() as d:
            dp = Path(d)
            old_rd, old_hp = common.RESULTS_DIR, ledger.HISTORY_PATH
            common.RESULTS_DIR = dp
            ledger.HISTORY_PATH = dp / "history.jsonl"
            try:
                rep = common.Reporter("T2", quiet=True)
                rep.add(common.Result("T2", "demo-a", common.PASS))
                rep.add(common.Result("T2", "demo-b", common.FAIL, detail="boom"))
                hist = ledger.load_history()
            finally:
                common.RESULTS_DIR, ledger.HISTORY_PATH = old_rd, old_hp
            self.assertEqual([r["name"] for r in hist], ["demo-a", "demo-b"])
            self.assertEqual([r["status"] for r in hist], ["PASS", "FAIL"])
            self.assertEqual({r["run_id"] for r in hist}, {rep.run_id})  # one run

    def test_finalize_marks_complete_so_a_reaped_run_is_detectable(self):
        import json
        from tools.regtest import common
        with tempfile.TemporaryDirectory() as d:
            dp = Path(d)
            old_rd, old_hp = common.RESULTS_DIR, ledger.HISTORY_PATH
            common.RESULTS_DIR = dp
            ledger.HISTORY_PATH = dp / "history.jsonl"
            try:
                rep = common.Reporter("T2", quiet=True)
                rep.add(common.Result("T2", "a", common.PASS))
                # only the crash-safe write has run so far -> a reaped run would stop here, marked incomplete
                mid = json.loads((dp / "T2-latest.json").read_text())
                self.assertIs(mid["complete"], False)
                rep.finalize()
                done = json.loads((dp / "T2-latest.json").read_text())
                self.assertIs(done["complete"], True)
            finally:
                common.RESULTS_DIR, ledger.HISTORY_PATH = old_rd, old_hp

    def test_dry_run_reporter_does_not_record(self):
        from tools.regtest import common
        with tempfile.TemporaryDirectory() as d:
            dp = Path(d)
            old_rd, old_hp = common.RESULTS_DIR, ledger.HISTORY_PATH
            common.RESULTS_DIR = dp
            ledger.HISTORY_PATH = dp / "history.jsonl"
            try:
                rep = common.Reporter("T2", quiet=True, persist=False)
                rep.add(common.Result("T2", "dry", common.SKIP))
                hist = ledger.load_history()
            finally:
                common.RESULTS_DIR, ledger.HISTORY_PATH = old_rd, old_hp
            self.assertEqual(hist, [])  # a dry/persist=False run leaves no ledger + no baseline
            self.assertFalse((dp / "T2-latest.json").exists())


if __name__ == "__main__":
    unittest.main()
