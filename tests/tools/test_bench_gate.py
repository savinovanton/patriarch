"""Retry policy of the performance gate (no timings, no builds)."""

import importlib.util
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
SPEC = importlib.util.spec_from_file_location("bench_gate", ROOT / "tools" / "bench_gate.py")
gate = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(gate)


def runner(*statuses):
    calls = []

    def measure(attempt):
        calls.append(attempt)
        status = statuses[len(calls) - 1]
        return {"status": status, "exit_code": 1 if status == "regression" else 0, "benchmarks": []}
    return measure, calls


class CompareWithRetryTests(unittest.TestCase):
    def test_pass_is_measured_once(self):
        measure, calls = runner("pass")
        self.assertEqual(gate.compare_with_retry(measure)["status"], "pass")
        self.assertEqual(calls, [0])

    def test_regression_then_pass_passes_with_warning(self):
        measure, calls = runner("regression", "pass")
        report = gate.compare_with_retry(measure)
        self.assertEqual(report["status"], "pass")
        self.assertEqual(report["exit_code"], 0)
        self.assertEqual(calls, [0, 1])
        self.assertTrue(report["warnings"])

    def test_regression_twice_fails(self):
        measure, calls = runner("regression", "regression")
        report = gate.compare_with_retry(measure)
        self.assertEqual(report["status"], "regression")
        self.assertEqual(report["exit_code"], 1)
        self.assertEqual(calls, [0, 1])

    def test_inconclusive_is_not_retried(self):
        measure, calls = runner("inconclusive")
        self.assertEqual(gate.compare_with_retry(measure)["status"], "inconclusive")
        self.assertEqual(calls, [0])


if __name__ == "__main__":
    unittest.main()
