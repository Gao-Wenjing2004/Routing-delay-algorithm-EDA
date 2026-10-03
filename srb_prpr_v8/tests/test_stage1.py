#!/usr/bin/env python3
from __future__ import annotations

import csv
import json
from pathlib import Path
import sys
import unittest


ROOT = Path(__file__).resolve().parents[1]
ANALYSIS = ROOT / "analysis" / "prpr"
sys.path.insert(0, str(ROOT / "tools"))

from prpr_arch import load_architecture


class StageOneArtifactsTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.model = json.loads((ANALYSIS / "prpr_model.json").read_text(encoding="utf-8"))

    def test_quotient_graph_shape(self) -> None:
        self.assertEqual(len(self.model["state_to_input"]), 160)
        self.assertEqual(len(set(self.model["state_classes"])), 16)
        with (ANALYSIS / "state_transitions.csv").open(encoding="utf-8", newline="") as stream:
            self.assertEqual(sum(1 for _ in csv.DictReader(stream)), 2752)

    def test_primitive_limits_and_scale_library(self) -> None:
        self.assertEqual(self.model["primitive_count"], 512)
        runtime = self.model["runtime_primitives"]
        self.assertEqual(len(runtime), 80)
        self.assertEqual({int(row["direction_bin"]) for row in runtime}, set(range(16)))
        self.assertTrue(all(abs(int(row["dx"])) <= 128 and abs(int(row["dy"])) <= 128 for row in runtime))

    def test_no_forbidden_atlas_payload(self) -> None:
        forbidden = {"atlas", "port_pair_delay", "absolute_source_region", "absolute_target_region"}
        self.assertTrue(forbidden.isdisjoint(self.model))
        self.assertLess((ROOT / "include" / "prpr_model_data.hpp").stat().st_size, 1_000_000)

    def test_required_reports_exist(self) -> None:
        required = [
            ROOT / "docs" / "prpr_design.md",
            ANALYSIS / "quotient_graph_report.md",
            ANALYSIS / "routing_primitives.csv",
            ANALYSIS / "primitive_coverage_report.md",
            ANALYSIS / "p0_p1_comparison.csv",
            ANALYSIS / "error_breakdown.csv",
            ANALYSIS / "first_stage_conclusion.md",
        ]
        self.assertEqual([str(path) for path in required if not path.is_file()], [])

    def test_every_output_has_a_terminal_connector(self) -> None:
        architecture = load_architecture(ROOT.parent / "legacy_versions" / "v5" / "arch")
        outputs = [
            port for port, direction in enumerate(architecture.port_directions)
            if direction == "output"
        ]
        self.assertEqual(len(outputs), 288)
        self.assertEqual(
            [architecture.port_names[port] for port in outputs if not architecture.target_arcs[port]],
            [],
        )

    def test_submission_size(self) -> None:
        executable = ROOT / "submission" / "bin" / "estimate"
        self.assertTrue(executable.is_file())
        self.assertLess(executable.stat().st_size, 90_000_000)
        self.assertEqual(executable.read_bytes()[:4], b"\x7fELF")


if __name__ == "__main__":
    unittest.main()
