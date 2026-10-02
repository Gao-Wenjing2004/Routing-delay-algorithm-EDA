#!/usr/bin/env python3
from __future__ import annotations

import json
from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]
P2 = ROOT / "analysis" / "p2_memory_fast"
TREE_MODEL = ROOT / "analysis" / "p2_8"


class P2ArtifactsTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.ablation = json.loads((P2 / "p2_ablation.json").read_text(encoding="utf-8"))
        cls.score = json.loads((P2 / "p2_cpp_score.json").read_text(encoding="utf-8"))
        cls.benchmark = json.loads((P2 / "runtime_benchmark.json").read_text(encoding="utf-8"))

    def test_fixed_validation_improves_over_no_atlas_base(self) -> None:
        selection = self.ablation["selection"]
        self.assertEqual(
            selection["selected"],
            "dijkstra_plus_golden_lgbm_plus_port_family_macro10_family_phase",
        )
        self.assertGreater(selection["selected_validation_acc"], selection["base_validation_acc"])

    def test_selected_models_have_eight_trees_each(self) -> None:
        for name in ("p2_teacher_lightgbm_model.json", "p2_lightgbm_model.json"):
            model = json.loads((TREE_MODEL / name).read_text(encoding="utf-8"))
            self.assertEqual(len(model["tree_info"]), 8)

    def test_exported_header_is_compact_and_has_valid_float_literals(self) -> None:
        header = ROOT / "p2_runtime" / "p2_student_data_fast.hpp"
        text = header.read_text(encoding="utf-8")
        self.assertLess(header.stat().st_size, 500_000)
        self.assertIsNone(re.search(r"(?:^|[,{ ])[-+]?\d+f(?=[,}])", text))
        self.assertIn("kTeacherTreeCount = 8", text)
        self.assertIn("kStudentTreeCount = 8", text)
        self.assertIn("kPostScale", text)
        self.assertIn("kPostSourceFamilyDistance", text)

    def test_cpp_result_is_deterministic_and_within_limits(self) -> None:
        self.assertGreater(self.score["accuracy"]["acc_score"], 94.6)
        self.assertEqual(self.benchmark["unique_output_hashes"], 1)
        self.assertLess(self.benchmark["projected_100m_seconds"], 120.0)
        self.assertLess(self.benchmark["linux_static_binary_bytes"], 90_000_000)


if __name__ == "__main__":
    unittest.main()
