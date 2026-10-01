#!/usr/bin/env python3

from __future__ import annotations

import importlib.util
import sys
import unittest
from pathlib import Path

import numpy as np


SCRIPT = Path(__file__).with_name("analyze_public_queries.py")
SPEC = importlib.util.spec_from_file_location("analyze_public_queries", SCRIPT)
assert SPEC and SPEC.loader
MODULE = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)


class AnalyzePublicQueriesTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.repo = Path(__file__).resolve().parents[1]
        cls.arch = MODULE.load_architecture(
            cls.repo / "plusone-srb_fast_v3" / "srb_fast_v3" / "arch"
        )

    def test_public_endpoint_matches_instance_and_full_port(self) -> None:
        cell, x, y, port, canonical = MODULE.parse_endpoint(
            " SRB_84_356/ZLE[0] ", self.arch
        )
        self.assertEqual((x, y), (84, 356))
        self.assertEqual(self.arch.cell_names[cell], "SRB_84_356")
        self.assertEqual(self.arch.port_names[port], "ZLE[0]")
        self.assertEqual(canonical, "SRB_84_356/ZLE[0]")

    def test_block_hole_is_not_a_valid_endpoint(self) -> None:
        with self.assertRaisesRegex(ValueError, "unknown_instance"):
            MODULE.parse_endpoint("SRB_80_20/ZLE[0]", self.arch)

    def test_exact_and_template_packing_are_reversible(self) -> None:
        source = np.asarray([0, 1, 0xFFFFFFFF], dtype=np.uint32)
        target = np.asarray([7, 0xFFFFFFFF, 3], dtype=np.uint32)
        exact = MODULE.packed_exact(source, target)
        np.testing.assert_array_equal((exact >> np.uint64(32)).astype(np.uint32), source)
        np.testing.assert_array_equal((exact & np.uint64(0xFFFFFFFF)).astype(np.uint32), target)

        sp = np.asarray([0, 495, 17], dtype=np.uint16)
        tp = np.asarray([495, 0, 33], dtype=np.uint16)
        dx = np.asarray([-119, 119, 0], dtype=np.int32)
        dy = np.asarray([-549, 549, 0], dtype=np.int32)
        keys = MODULE.packed_template(sp, tp, dx, dy, self.arch.width, self.arch.height)
        decoded = [MODULE.unpack_template(int(key), self.arch) for key in keys]
        self.assertEqual(decoded, list(zip(sp.tolist(), tp.tolist(), dx.tolist(), dy.tolist())))
        self.assertEqual(len(set(map(int, keys))), 3)

    def test_gap_boundary_is_site_to_site_plus_one(self) -> None:
        # Vertical line at site=9 is crossed by x=9 -> 10, not x=8 -> 9.
        gap = next(g for g in self.arch.gaps if g.direction == "vertical" and g.site == 9)
        self.assertEqual(gap.delay, 78)
        self.assertFalse(min(8, 9) <= gap.site < max(8, 9))
        self.assertTrue(min(9, 10) <= gap.site < max(9, 10))

    def test_configured_blocks_exactly_explain_instance_holes(self) -> None:
        self.assertTrue(self.arch.block_holes_match_instances)
        self.assertEqual(self.arch.missing_cell_count, 5800)
        self.assertEqual(self.arch.block_cell_count, 5800)

    def test_frequency_distribution_conserves_requests(self) -> None:
        counts = np.asarray([1, 2, 2, 7, 101], dtype=np.int64)
        result = MODULE.frequency_distribution(counts)
        self.assertEqual(sum(row["requests"] for row in result["exact_frequency"]), int(counts.sum()))
        self.assertEqual(sum(row["requests"] for row in result["bins"]), int(counts.sum()))


if __name__ == "__main__":
    unittest.main()
