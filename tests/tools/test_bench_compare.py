"""Focused contract checks; no hardware timings, sleeps or performance thresholds in tests."""

import copy
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("bench_compare", ROOT / "tools" / "bench_compare.py")
compare = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(compare)


def suite(*names):
    return {"schema": 1, "benchmarks": list(names or ("core.work",)), "threshold": 0.1,
            "max_rsd": 0.05, "min_samples": 5}


def measurement(samples=None, name="core.work"):
    return {"schema": 1, "source_sha": "a" * 40,
            "profile": {"os": "Windows", "configuration": "Release", "compiler": "MSVC 19.44", "cpu": "same CPU"},
            "benchmarks": [{"name": name, "time_unit": "ns", "samples": list(samples or [100] * 5), "label": "same result"}]}


class BenchmarkComparisonTests(unittest.TestCase):
    def test_median_comes_from_raw_samples_and_exact_threshold_passes(self):
        base = measurement([98, 99, 100, 101, 102])
        candidate = measurement([108, 109, 110, 111, 112])
        # Unknown aggregate metadata is ignored; it cannot override the actual iterations.
        candidate["benchmarks"][0]["median"] = 99999
        result = compare.compare_measurements(base, candidate, suite())
        self.assertEqual(result["exit_code"], 0)
        self.assertEqual(result["benchmarks"][0]["candidate"]["median_ns"], 110)
        self.assertAlmostEqual(result["benchmarks"][0]["change_fraction"], 0.1)
        self.assertEqual(compare.compare_measurements(measurement([1] * 5), measurement([1.1] * 5), suite())["exit_code"], 0)
        self.assertEqual(compare.compare_measurements(measurement(), measurement([100 * 1.1] * 5), suite())["exit_code"], 0)
        self.assertEqual(compare.compare_measurements(measurement(), measurement([110.000001] * 5), suite())["exit_code"], 1)
        self.assertEqual(compare.compare_measurements(base, measurement([111] * 5), suite())["exit_code"], 1)

    def test_noise_is_inconclusive_and_does_not_hide_another_stable_regression(self):
        noisy = measurement([50, 75, 100, 150, 250])
        self.assertEqual(compare.compare_measurements(measurement(), noisy, suite())["exit_code"], 3)
        self.assertEqual(compare.compare_measurements(measurement(), measurement([100] * 4), suite())["exit_code"], 3)
        base = measurement()
        base["benchmarks"].append({"name": "sim.work", "time_unit": "ns", "samples": [100] * 5})
        candidate = copy.deepcopy(base)
        candidate["benchmarks"][0]["samples"] = [50, 75, 100, 150, 250]
        candidate["benchmarks"][1]["samples"] = [120] * 5
        result = compare.compare_measurements(base, candidate, suite("core.work", "sim.work"))
        self.assertEqual(result["exit_code"], 1)
        self.assertEqual(result["summary"]["inconclusive"], 1)
        self.assertEqual(result["summary"]["regressions"], 1)

    def test_missing_duplicate_bad_units_and_malformed_samples_are_rejected(self):
        variants = []
        missing = measurement()
        missing["benchmarks"] = []
        variants.append(missing)
        duplicate = measurement()
        duplicate["benchmarks"].append(copy.deepcopy(duplicate["benchmarks"][0]))
        variants.append(duplicate)
        for field, value in (("time_unit", "us"), ("samples", []), ("samples", [0] * 5),
                             ("samples", [True] * 5), ("samples", [float("nan")] * 5),
                             ("samples", [float("inf")] * 5)):
            invalid = measurement()
            invalid["benchmarks"][0][field] = value
            variants.append(invalid)
        aggregate = measurement()
        aggregate["benchmarks"][0].pop("samples")
        aggregate["benchmarks"][0]["aggregate_name"] = "median"
        variants.append(aggregate)
        for invalid in variants:
            with self.subTest(invalid=invalid), self.assertRaises(compare.InputError):
                compare.compare_measurements(measurement(), invalid, suite())

    def test_profiles_must_match_and_changed_labels_are_visible_or_explicitly_required(self):
        candidate = measurement()
        candidate["profile"]["cpu"] = "other CPU"
        with self.assertRaises(compare.InputError):
            compare.compare_measurements(measurement(), candidate, suite())
        candidate = measurement()
        candidate["benchmarks"][0]["label"] = "different result"
        report = compare.compare_measurements(measurement(), candidate, suite())
        self.assertEqual(report["exit_code"], 0)
        self.assertTrue(report["benchmarks"][0]["label_changed"])
        self.assertEqual(len(report["warnings"]), 1)
        strict = suite()
        strict["require_same_label"] = True
        with self.assertRaises(compare.InputError):
            compare.compare_measurements(measurement(), candidate, strict)
        for field, value in (("configuration", "Debug"), ("benchmark_library_build_type", "debug")):
            invalid = measurement()
            invalid["profile"][field] = value
            with self.subTest(field=field), self.assertRaises(compare.InputError):
                compare.freeze_measurement(invalid, suite())

    def test_freeze_is_explicit_stable_and_strips_unknown_metadata(self):
        original = measurement()
        original["host_name"] = "private-host"
        policy = suite()
        policy["capture"] = {"repetitions": 7, "min_time_seconds": 0.25, "warmup_seconds": 0.05}
        self.assertEqual(compare.validate_suite(policy)["capture"], policy["capture"])
        frozen = compare.freeze_measurement(original, suite())
        self.assertNotIn("host_name", frozen)
        self.assertEqual(original["host_name"], "private-host")
        original["profile"]["compiler_path"] = "C:/private/tool.exe"
        with self.assertRaises(compare.InputError):
            compare.freeze_measurement(original, suite())
        with self.assertRaises(compare.InputError):
            compare.freeze_measurement(measurement([1, 2, 3, 4, 5]), suite())

    def test_cli_reports_and_exit_codes_and_never_overwrites_comparison_inputs(self):
        script = ROOT / "tools" / "bench_compare.py"
        with tempfile.TemporaryDirectory(prefix="patriarch-bench-compare-") as directory:
            root = Path(directory)
            baseline, candidate, policy = (root / name for name in ("base.json", "candidate.json", "suite.json"))
            output, html = root / "report.json", root / "report.html"
            baseline.write_text(json.dumps(measurement()), encoding="utf-8")
            policy.write_text(json.dumps(suite()), encoding="utf-8")
            for samples, expected, status in (([100] * 5, 0, "pass"), ([120] * 5, 1, "regression"),
                                               ([50, 75, 100, 150, 250], 3, "inconclusive")):
                candidate.write_text(json.dumps(measurement(samples)), encoding="utf-8")
                process = subprocess.run([sys.executable, str(script), "compare", str(baseline), str(candidate),
                                          "--suite", str(policy), "--json", str(output), "--html", str(html)],
                                         text=True, capture_output=True, timeout=10)
                self.assertEqual(process.returncode, expected, process.stderr)
                self.assertEqual(json.loads(output.read_text())["status"], status)
                self.assertIn("<svg", html.read_text(encoding="utf-8"))
            candidate.write_text('{"schema":1,"schema":1}', encoding="utf-8")
            process = subprocess.run([sys.executable, str(script), "compare", str(baseline), str(candidate),
                                      "--suite", str(policy), "--json", str(output), "--html", str(html)],
                                     capture_output=True, timeout=10)
            self.assertEqual(process.returncode, 2)
            self.assertEqual(json.loads(output.read_text())["status"], "invalid")
            before = baseline.read_bytes()
            process = subprocess.run([sys.executable, str(script), "compare", str(baseline), str(candidate),
                                      "--suite", str(policy), "--json", str(baseline), "--html", str(html)],
                                     capture_output=True, timeout=10)
            self.assertEqual(process.returncode, 2)
            self.assertEqual(baseline.read_bytes(), before)

    def test_html_escapes_names_and_profiles(self):
        unsafe = '<script>alert("x")</script>'
        base, candidate = measurement(name=unsafe), measurement(name=unsafe)
        base["profile"]["cpu"] = candidate["profile"]["cpu"] = unsafe
        page = compare.render_html(compare.compare_measurements(base, candidate, suite(unsafe)))
        self.assertNotIn("<script>", page)
        self.assertIn("&lt;script&gt;", page)


if __name__ == "__main__":
    unittest.main()
