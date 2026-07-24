"""Unit tests for the T0 clone-drift guard (no hardware, no bench).

Run directly:   python tools/regtest/tests/test_clone_mirror.py
or discovered:  make test-unit

Covers _mirror_body's header-stripping robustness (banner length, CRLF, trailing newline, whitespace
preserved, drift-can't-hide-above-the-boundary), that _check_clone_mirrors FAILs on a real body diff,
that CLONE_MIRRORS still lists the mesh-gate fixture, and that the SHIPPED clone matches its source now.
"""

import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[3]))
from tools.regtest.t0_build import _mirror_body, _check_clone_mirrors  # noqa: E402
from tools.regtest import manifest as M  # noqa: E402
from tools.regtest.common import FAIL, PASS  # noqa: E402


class _FakeRep:
    """Minimal Reporter stand-in: just collects the Results the check emits."""

    def __init__(self):
        self.results = []

    def add(self, r):
        self.results.append(r)
        return r


class MirrorBody(unittest.TestCase):
    def test_identical_body_despite_banner_crlf_and_trailing_nl(self):
        source = b"/* prod banner */\n\n#include <string.h>\nint main(){return 0;}\n"
        clone = b"/* clone banner, deliberately longer */\n\n#include <string.h>\r\nint main(){return 0;}"
        bs, es = _mirror_body(source)
        bc, ec = _mirror_body(clone)
        self.assertIsNone(es)
        self.assertIsNone(ec)
        self.assertEqual(bs, bc)  # per-app banner, CRLF, and the trailing newline are all normalized away

    def test_one_body_line_differs(self):
        a = b"/* a */\n#include <string.h>\nint x=1;\n"
        b = b"/* b */\n#include <string.h>\nint x=2;\n"
        self.assertNotEqual(_mirror_body(a)[0], _mirror_body(b)[0])

    def test_interior_whitespace_drift_is_caught(self):
        a = b"/* a */\n#include <string.h>\nint x = 1;\n"
        b = b"/* b */\n#include <string.h>\nint x =  1;\n"  # one extra interior space
        self.assertNotEqual(_mirror_body(a)[0], _mirror_body(b)[0])

    def test_missing_include_sentinel_is_error(self):
        self.assertIsNotNone(_mirror_body(b"/* only a banner, no code */\n")[1])

    def test_drift_hidden_above_boundary_is_error(self):
        # a #define sneaked between the banner and the first #include must FAIL, not hide in the region
        self.assertIsNotNone(_mirror_body(b"/* b */\n#define SNEAK 1\n#include <string.h>\nx\n")[1])

    def test_content_before_banner_is_error(self):
        self.assertIsNotNone(_mirror_body(b"junk\n/* b */\n#include <string.h>\nx\n")[1])


class CheckCloneMirrors(unittest.TestCase):
    def test_allowlist_covers_mesh_gate(self):
        # guards against someone silencing the check by deleting the row
        self.assertIn("test-mesh-gate-ap", M.CLONE_MIRRORS)

    def test_gates_on_a_real_body_diff(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            for app, tail in (("cl", b"int x=1;\n"), ("src", b"int x=2;\n")):
                p = root / "firmware" / app / "main"
                p.mkdir(parents=True)
                (p / "app_main.c").write_bytes(b"/* " + app.encode() + b" banner */\n#include <string.h>\n" + tail)
            mirrors = {"cl": ("src", ("main/app_main.c",))}
            with mock.patch.object(M, "CLONE_MIRRORS", mirrors), \
                 mock.patch("tools.regtest.common.REPO_ROOT", root):
                rep = _FakeRep()
                _check_clone_mirrors(rep)
        self.assertTrue(any(r.status == FAIL for r in rep.results))

    def test_shipped_clone_matches_source_now(self):
        # the real firmware/ files: proves the guard passes on the current (correctly-synced) tree
        rep = _FakeRep()
        _check_clone_mirrors(rep)
        self.assertTrue(rep.results, "the check produced no results")
        bad = [r for r in rep.results if r.status != PASS]
        self.assertEqual(bad, [], f"clone drift / shape failures: {[r.detail for r in bad]}")


if __name__ == "__main__":
    unittest.main()
