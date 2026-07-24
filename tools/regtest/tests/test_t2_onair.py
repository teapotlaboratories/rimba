"""Unit tests for the T2 orchestrator's build-config plumbing (no hardware, no bench).

Run directly:   python tools/regtest/tests/test_t2_onair.py
or discovered:  make test-unit

Two mechanisms added with the mesh-gate tier:
  * _flash() drops CMakeCache.txt when a build's config changed since its last flash, so a dual-mode
    fixture flashed in two configs across two tests reconfigures cleanly instead of silently reusing a
    stale binary. THE regression these guard: a flash that FAILS after a config change must not leave a
    stamp that lets a later same-config flash skip the reset (the bug fixed in review).
  * Role.build_vars threads fixed per-role make vars (e.g. STA_IP=/NO_PING=) through _resolve_build_vars
    section (c), so a fixture can be pinned into responder mode without a duplicate clone.

Both are exercised with common.make mocked out, so nothing is built or flashed.
"""

import json
import sys
import tempfile
import types
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[3]))
from tools.regtest import t2_onair  # noqa: E402


def _fake_make(returncode, calls):
    """Stand-in for common.make: record the call and return a CompletedProcess-like. It does NOT
    recreate the cache, so 'was the cache present afterwards?' cleanly reflects whether _flash unlinked
    it (the real idf.py would reconfigure, but that is idf.py's job, not _flash's decision logic)."""
    def run(target, app, board, port=None, timeout=0, make_vars=None):
        calls.append({"target": target, "app": app, "make_vars": make_vars})
        return types.SimpleNamespace(returncode=returncode, stdout="", stderr="simulated")
    return run


class FlashReset(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self._patch = mock.patch.object(t2_onair, "BUILD_DIR", Path(self._tmp.name))
        self._patch.start()
        self.app, self.board = "test-app", "proto1"
        self.dir = Path(self._tmp.name) / self.app / self.board

    def tearDown(self):
        self._patch.stop()
        self._tmp.cleanup()

    def _seed(self, cache=None, stamp=None):
        self.dir.mkdir(parents=True, exist_ok=True)
        if cache is not None:
            (self.dir / "CMakeCache.txt").write_text(cache)
        if stamp is not None:
            (self.dir / ".regtest_build_config").write_text(json.dumps(stamp, sort_keys=True))

    @property
    def cache(self):
        return self.dir / "CMakeCache.txt"

    @property
    def stamp(self):
        return self.dir / ".regtest_build_config"

    def _run(self, make_vars, rc=0):
        calls = []
        with mock.patch.object(t2_onair, "make", _fake_make(rc, calls)):
            t2_onair._flash(self.app, self.board, "port", 60, make_vars=make_vars)
        return calls

    def test_same_config_does_not_reset(self):
        # stamp matches want -> the pre-existing cache is left untouched (no reconfigure churn)
        self._seed(cache="SENTINEL", stamp={"NO_PING": "1"})
        calls = self._run({"NO_PING": "1"})
        self.assertEqual(self.cache.read_text(), "SENTINEL")   # never unlinked
        self.assertEqual(json.loads(self.stamp.read_text()), {"NO_PING": "1"})
        self.assertEqual(len(calls), 1)

    def test_changed_config_resets_and_restamps(self):
        # stamp differs -> cache dropped, then re-stamped to the new config on success
        self._seed(cache="OLD", stamp={})
        self._run({"NO_PING": "1"})
        self.assertFalse(self.cache.exists())                  # unlinked (fake make doesn't recreate)
        self.assertEqual(json.loads(self.stamp.read_text()), {"NO_PING": "1"})

    def test_absent_stamp_is_treated_as_mismatch(self):
        # a cache from a prior manual/T0 build with no stamp -> reset, so we never reuse an unknown config
        self._seed(cache="OLD")   # no stamp
        self._run({})
        self.assertFalse(self.cache.exists())
        self.assertEqual(json.loads(self.stamp.read_text()), {})

    def test_first_flash_no_cache_just_stamps(self):
        # no build dir yet -> nothing to reset; make runs and the stamp is written
        calls = self._run({"X": "1"})
        self.assertEqual(json.loads(self.stamp.read_text()), {"X": "1"})
        self.assertEqual(len(calls), 1)

    def test_failed_flash_after_change_invalidates_stamp(self):
        # THE regression: a config change whose flash then FAILS must leave NO stamp, so the next flash
        # still resets instead of trusting a stamp that no longer matches the (already reconfigured) cache.
        self._seed(cache="OLD", stamp={})                      # prior successful flagless flash
        self._run({"NO_PING": "1"}, rc=1)                      # reconfigure to NO_PING, then flash fails
        self.assertFalse(self.stamp.exists())                  # <-- not rewritten (rc != 0) AND unlinked
        # and prove the consequence: a later flash of the OLD config now resets rather than skipping it
        self._seed(cache="RECONFIGURED")                       # a cache exists again, still no stamp
        self._run({})
        self.assertFalse(self.cache.exists())                  # reset ran (would have been skipped by the bug)
        self.assertEqual(json.loads(self.stamp.read_text()), {})


class ResolveBuildVars(unittest.TestCase):
    def test_build_vars_thread_to_the_owning_app_only(self):
        from tools.regtest.t2_tests import T2_BY_SLUG
        t = T2_BY_SLUG["mesh-gate-b2"]
        assign = {r.name: ("esp", "board0") for r in t.roles}  # section (b) is skipped here (no build_mac_var)
        by_app = t2_onair._resolve_build_vars(t, assign, None, None)
        self.assertEqual(by_app.get("test-mesh-gate-sta"), {"STA_IP": "10.9.9.50", "NO_PING": "1"})
        self.assertNotIn("test-mesh-gate-node", by_app)        # the reporter has no build_vars

    def test_case_c_responder_gets_no_ping(self):
        from tools.regtest.t2_tests import T2_BY_SLUG
        t = T2_BY_SLUG["mesh-gate-bridge"]
        assign = {r.name: ("esp", "board0") for r in t.roles}
        by_app = t2_onair._resolve_build_vars(t, assign, None, None)
        self.assertEqual(by_app.get("test-mesh-gate-node"), {"NO_PING": "1"})
        self.assertNotIn("test-mesh-gate-sta", by_app)         # the DHCP reporter is flag-less


if __name__ == "__main__":
    unittest.main()
