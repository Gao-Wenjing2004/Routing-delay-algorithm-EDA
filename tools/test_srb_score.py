import tempfile
import unittest
from pathlib import Path

import numpy as np

from tools.srb_score import ScoreContract, accuracy_metrics, load_aligned_csv, point_scores, total_score


class SrbScoreTests(unittest.TestCase):
    def test_positive_golden_formula(self):
        golden = np.asarray([100.0, 200.0])
        predicted = np.asarray([100.0, 220.0])
        expected = np.asarray([1.0, 1.0 - np.tanh(0.4)])
        np.testing.assert_allclose(point_scores(golden, predicted), expected)

    def test_zero_golden_policy_is_explicit(self):
        score = point_scores(np.asarray([0.0, 0.0]), np.asarray([0.0, 1.0]))
        np.testing.assert_array_equal(score, np.asarray([1.0, 0.0]))
        with self.assertRaises(ValueError):
            point_scores(
                np.asarray([0.0]),
                np.asarray([0.0]),
                ScoreContract(zero_golden_policy="error"),
            )

    def test_weighted_total_score(self):
        result = total_score(90.0, 180.0, True)
        self.assertAlmostEqual(result["time_score"], 90.0)
        self.assertAlmostEqual(result["estimated_total_score"], 90.5)
        self.assertEqual(total_score(90.0, 180.0, True, invalid_runs=2)["estimated_total_score"], 0.0)

    def test_accuracy_metrics(self):
        result = accuracy_metrics(np.asarray([100.0, 200.0]), np.asarray([100.0, 220.0]))
        self.assertEqual(result["rows"], 2)
        self.assertEqual(result["exact_matches"], 1)
        self.assertAlmostEqual(result["mae"], 10.0)

    def test_csv_order_is_strict(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            golden = root / "golden.csv"
            prediction = root / "prediction.csv"
            golden.write_text("From,To,delay\nA,B,10\n", encoding="utf-8")
            prediction.write_text("From,To,delay\nA,C,10\n", encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "endpoint/order mismatch"):
                load_aligned_csv(golden, prediction)


if __name__ == "__main__":
    unittest.main()

