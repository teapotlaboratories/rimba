"""Unit tests for the manifest's bench-config validation (no hardware, no bench).

Run directly:   python tools/regtest/tests/test_manifest.py
or discovered:  python -m unittest discover -s tools/regtest/tests

These cover the "values were passed but are wrong" cases that used to slip through
_build_bench's non-empty check and produce a silently-broken bench (a WIRED_BOARD typo
with no fully-wired DUT, a malformed BOARDx_MAC that derives an empty mesh_mac/mesh_ip,
or two boards colliding on the same derived mesh IP).
"""

import os
import sys
import unittest
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import manifest  # noqa: E402


#: A well-formed bench config; individual tests override one field to break it.
GOOD = {
    "BENCH_BOARD": "proto1-fgh100m",
    "BOARD0_MAC": "e0:72:a1:f8:ef:a4",  # -> 10.9.9.136
    "BOARD1_MAC": "e0:72:a1:f8:f9:40",  # -> 10.9.9.100
    "BOARD2_MAC": "e0:72:a1:f8:f0:08",  # -> 10.9.9.108
    "WIRED_BOARD": "board2",
}


def build_with(**overrides):
    """Build BENCH from GOOD with the given overrides applied to the environment."""
    env = dict(GOOD)
    env.update(overrides)
    with mock.patch.dict(os.environ, env):
        return manifest._build_bench()


class ValidateBenchTests(unittest.TestCase):
    def test_good_config_builds_and_validates(self):
        bench = build_with()
        self.assertEqual(set(bench), {"board0", "board1", "board2"})
        self.assertTrue(bench["board2"].fully_wired)
        for b in bench.values():
            self.assertTrue(b.mesh_mac and b.mesh_ip, f"{b.name} has empty derived fields")
        # distinct IPs, a real wired board, valid MACs -> no raise
        manifest._validate_bench(bench)

    def test_wired_board_typo_raises(self):
        bench = build_with(WIRED_BOARD="board3")
        self.assertTrue(bench, "a typo'd WIRED_BOARD still builds a (non-empty) bench today")
        self.assertFalse(any(b.fully_wired for b in bench.values()))
        with self.assertRaises(manifest.BenchNotConfigured) as cm:
            manifest._validate_bench(bench)
        self.assertIn("WIRED_BOARD", str(cm.exception))

    def test_malformed_mac_raises(self):
        bench = build_with(BOARD1_MAC="not-a-mac")
        with self.assertRaises(manifest.BenchNotConfigured) as cm:
            manifest._validate_bench(bench)
        self.assertIn("BOARD1_MAC", str(cm.exception))

    def test_short_mac_raises(self):
        bench = build_with(BOARD0_MAC="e0:72:a1:f8:ef")  # only 5 octets
        with self.assertRaises(manifest.BenchNotConfigured) as cm:
            manifest._validate_bench(bench)
        self.assertIn("BOARD0_MAC", str(cm.exception))

    def test_mesh_ip_collision_raises(self):
        # 0xef:08 and 0xf9:48 both have (last octet & 0x3f) == 8 -> both 10.9.9.108;
        # board2 pinned to a distinct IP so the collision is exactly board0/board1.
        bench = build_with(
            BOARD0_MAC="e0:72:a1:f8:ef:08",
            BOARD1_MAC="e0:72:a1:f8:f9:48",
            BOARD2_MAC="e0:72:a1:f8:f0:01",
        )
        self.assertEqual(bench["board0"].mesh_ip, bench["board1"].mesh_ip)
        self.assertNotEqual(bench["board2"].mesh_ip, bench["board0"].mesh_ip)
        with self.assertRaises(manifest.BenchNotConfigured) as cm:
            manifest._validate_bench(bench)
        msg = str(cm.exception)
        self.assertIn("both derive mesh IP", msg)
        self.assertIn("board0", msg)
        self.assertIn("board1", msg)

    def test_absent_config_builds_empty_not_error(self):
        # No vars set -> {} (no bench configured); require_bench handles the message.
        with mock.patch.dict(os.environ, {k: "" for k in GOOD}):
            self.assertEqual(manifest._build_bench(), {})

    def test_partial_config_builds_empty(self):
        # BENCH_BOARD set but MACs absent -> still {} (incomplete, not malformed).
        with mock.patch.dict(os.environ, {**{k: "" for k in GOOD}, "BENCH_BOARD": "proto1-fgh100m"}):
            self.assertEqual(manifest._build_bench(), {})


class RequireBenchTests(unittest.TestCase):
    def test_require_bench_rejects_malformed(self):
        bad = build_with(WIRED_BOARD="board3")
        with mock.patch.object(manifest, "BENCH", bad):
            with self.assertRaises(manifest.BenchNotConfigured):
                manifest.require_bench()

    def test_require_bench_accepts_good(self):
        good = build_with()
        with mock.patch.object(manifest, "BENCH", good):
            self.assertIs(manifest.require_bench(), good)

    def test_require_bench_missing_lists_vars(self):
        with mock.patch.object(manifest, "BENCH", {}):
            with mock.patch.dict(os.environ, {k: "" for k in GOOD}):
                with self.assertRaises(manifest.BenchNotConfigured) as cm:
                    manifest.require_bench()
                self.assertIn("BENCH_BOARD", str(cm.exception))


if __name__ == "__main__":
    unittest.main(verbosity=2)
