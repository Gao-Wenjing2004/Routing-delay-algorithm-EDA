from __future__ import annotations

import csv
import math
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import unique_eval  # noqa: E402


class UniqueEvalTests(unittest.TestCase):
    def test_point_score_matches_contest_formula(self) -> None:
        expected = 100.0 * (1.0 - math.tanh(4.0 * 10.0 / 1000.0))
        self.assertAlmostEqual(100.0 * unique_eval.point_score(1000, 1010), expected, places=12)
        self.assertEqual(unique_eval.point_score(0, 0), 1.0)
        self.assertEqual(unique_eval.point_score(0, 1), 0.0)

    def test_largest_remainder_is_exact(self) -> None:
        quotas = unique_eval.largest_remainder(101, [1, 2, 3, 4])
        self.assertEqual(sum(quotas), 101)
        self.assertEqual(quotas, [10, 20, 30, 41])

    def test_affine_target_selection_has_no_duplicates(self) -> None:
        source = unique_eval.parse_endpoint("SRB_10_10/A")
        targets = [unique_eval.parse_endpoint(f"SRB_{x}_{y}/B") for y in range(20) for x in range(20)]
        selected, _ = unique_eval.select_targets(
            source, targets, 100, [17, 17, 17, 17, 16, 16], 7,
            "balanced-distance", unique_eval.Architecture(),
        )
        self.assertEqual(len(selected), 100)
        self.assertEqual(len({item.text for item in selected}), 100)
        self.assertNotIn(source.text, {item.text for item in selected})

    def test_performance_selection_skips_environment_without_changing_pairs(self) -> None:
        source = unique_eval.parse_endpoint("SRB_10_10/A")
        targets = [unique_eval.parse_endpoint(f"SRB_{x}_{y}/B") for y in range(20) for x in range(20)]
        architecture = unique_eval.Architecture((5, 10), (5, 10), ((3, 7, 3, 7),))
        full, full_stats = unique_eval.select_targets(
            source, targets, 100, [17, 17, 17, 17, 16, 16], 7,
            "balanced-distance", architecture,
        )
        fast, fast_stats = unique_eval.select_targets(
            source, targets, 100, [17, 17, 17, 17, 16, 16], 7,
            "balanced-distance", architecture, collect_environment=False,
        )
        self.assertEqual([item.text for item in full], [item.text for item in fast])
        self.assertEqual(
            {key: value for key, value in full_stats.items() if key.startswith("distance:")},
            dict(fast_stats),
        )

    def test_splitmix64_is_deterministic_and_spreads(self) -> None:
        values = [unique_eval.splitmix64(index) for index in range(1000)]
        self.assertEqual(values, [unique_eval.splitmix64(index) for index in range(1000)])
        self.assertGreater(len({value % 128 for value in values}), 120)

    def test_linear_projection_recovers_fixed_cost(self) -> None:
        result = unique_eval.fit_line([(100, 3.0), (200, 5.0), (400, 9.0)], 1000)
        self.assertAlmostEqual(float(result["intercept_seconds"]), 1.0, places=12)
        self.assertAlmostEqual(float(result["seconds_per_row"]), 0.02, places=12)
        self.assertAlmostEqual(float(result["estimate_seconds"]), 21.0, places=12)

    def test_benchmark_report_adds_cold_start_once(self) -> None:
        cases = []
        for rows, seconds in ((10_000_000, 3.0), (50_000_000, 11.0), (100_000_000, 21.0)):
            cases.append({
                "rows": rows,
                "median_wall_seconds": seconds,
                "min_wall_seconds": seconds,
                "max_wall_seconds": seconds,
                "consistent": True,
            })
        report = unique_eval.benchmark_report(
            "/tmp/estimate", 5, cases, 1_000_000_000,
            "Linux-test", "linux", "ELF", 7.0,
        )
        self.assertEqual(report["reference_quality"], "release-reference")
        projection = report["extrapolation"]
        self.assertIsInstance(projection, dict)
        self.assertAlmostEqual(float(projection["steady_state_estimate_seconds"]), 201.0)
        self.assertAlmostEqual(float(projection["estimate_seconds"]), 208.0)

    def test_strict_aligned_evaluation(self) -> None:
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            golden = root / "golden.csv"
            prediction = root / "prediction.csv"
            rows = [
                ("SRB_1_1/A", "SRB_2_2/B", 100, 100),
                ("SRB_1_1/A", "SRB_3_4/B", 200, 210),
                ("SRB_9_9/A", "SRB_8_7/B", 300, 290),
                ("SRB_9_9/A", "SRB_7_6/B", 400, 400),
            ]
            for path, delay_index in ((golden, 2), (prediction, 3)):
                with path.open("w", encoding="utf-8", newline="") as handle:
                    writer = csv.writer(handle, lineterminator="\n")
                    writer.writerow(["From", "To", "Delay"])
                    for row in rows:
                        writer.writerow([row[0], row[1], row[delay_index]])
            result = unique_eval.evaluate_files(
                golden, prediction, unique_eval.Architecture(), 100, 11,
            )
            self.assertEqual(result["overall"]["rows"], 4)
            self.assertEqual(result["group_diagnostics"]["source"]["groups"], 2)
            expected = 25.0 * sum(
                unique_eval.point_score(row[2], row[3]) for row in rows
            )
            self.assertAlmostEqual(float(result["overall"]["accuracy"]), expected, places=12)


if __name__ == "__main__":
    unittest.main()
