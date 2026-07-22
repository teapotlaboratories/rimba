"""Unit tests for the pyflakes lint gate (no hardware, no bench).

Run directly:   python tools/regtest/tests/test_lint.py
or discovered:  make test-unit

The gate exists because py_compile does NOT catch an undefined name (a `bench`-out-of-scope edit passed
py_compile and only failed on the bench). These assert pyflakes catches that class, and that clean code
and the real harness pass.
"""

import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[3]))
from tools.regtest import lint  # noqa: E402

try:
    import pyflakes.api  # noqa: F401
    _HAS_PYFLAKES = True
except Exception:
    _HAS_PYFLAKES = False

_NEEDS = unittest.skipUnless(_HAS_PYFLAKES, "pyflakes not installed in this python (the gate degrades "
                                            "to a warning; run `make test-lint` under the IDF env)")


class TestLintGate(unittest.TestCase):
    @_NEEDS
    def test_catches_undefined_name(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "bad.py"
            p.write_text("def f():\n    return undefined_name\n")   # py_compile would pass this
            ok, report = lint.check([p])
            self.assertFalse(ok)
            self.assertIn("undefined name", report.lower())

    @_NEEDS
    def test_catches_unused_import(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "unused.py"
            p.write_text("import os\n\n\ndef f():\n    return 1\n")
            ok, report = lint.check([p])
            self.assertFalse(ok)
            self.assertIn("imported but unused", report)

    @_NEEDS
    def test_clean_file_passes(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "good.py"
            p.write_text("def f():\n    x = 1\n    return x\n")
            ok, report = lint.check([p])
            self.assertTrue(ok)
            self.assertEqual(report, "")

    @_NEEDS
    def test_real_harness_is_clean(self):
        # The gate must pass on the checked-in harness (a failing gate blocks every run).
        ok, report = lint.check()
        self.assertTrue(ok, f"the harness has pyflakes findings:\n{report}")

    def test_targets_cover_the_package(self):
        names = {p.name for p in lint.targets()}
        self.assertIn("common.py", names)
        self.assertIn("ledger.py", names)
        self.assertIn("run.py", names)


if __name__ == "__main__":
    unittest.main()
