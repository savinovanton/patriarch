"""Raw Google Benchmark conversion and literal CMake metadata contracts."""
import importlib.util
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
SPEC = importlib.util.spec_from_file_location("bench_capture", ROOT / "tools" / "bench_capture.py")
capture = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(capture)


def row(index=0, value=2.5, unit="us"):
    return {"run_type": "iteration", "run_name": "work/1", "repetition_index": index,
            "real_time": value, "time_unit": unit, "iterations": 12, "label": "same"}


class BenchmarkCaptureTests(unittest.TestCase):
    def test_raw_units_are_normalized_and_aggregates_cannot_replace_iterations(self):
        raw = {"benchmarks": [row(), row(1, 0.003, "ms"),
                               {"run_type": "aggregate", "aggregate_name": "median", "real_time": 9999}]}
        data = capture.normalize(raw, ["work/1"], "a" * 40, {"configuration": "Release"})
        self.assertEqual(data["benchmarks"][0]["samples"], [2500, 3000])
        self.assertEqual(data["benchmarks"][0]["label"], "same")
        with self.assertRaises(ValueError):
            capture.normalize({"benchmarks": raw["benchmarks"][2:]}, ["work/1"], "a" * 40, {})

    def test_duplicate_error_and_nonfinite_iterations_are_rejected(self):
        cases = [[row(), row()], [dict(row(), error_occurred=True)], [row(value=float("nan"))],
                 [row(value=0)], [row(unit="unknown")], [dict(row(), iterations=True)],
                 [row(), dict(row(1), label="changed")], [row(), {key: value for key, value in row(1).items() if key != "label"}]]
        for rows in cases:
            with self.subTest(rows=rows), self.assertRaises(ValueError):
                capture.normalize({"benchmarks": rows}, ["work/1"], "a" * 40, {})
        with self.assertRaises(ValueError):
            capture.normalize({"benchmarks": [row(), row(2)]}, ["work/1"], "a" * 40, {}, repetitions=2)

    def test_generated_cmake_metadata_handles_bare_architecture_without_execution(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            generated = root / "CMakeFiles" / "4.4.4"
            generated.mkdir(parents=True)
            (generated / "CMakeCXXCompiler.cmake").write_text(
                'set(CMAKE_CXX_COMPILER_ID "MSVC")\n'
                'set(CMAKE_CXX_COMPILER_VERSION "19.44.35229.0")\n'
                'set(CMAKE_CXX_COMPILER_ARCHITECTURE_ID x64)\n'
                'message(FATAL_ERROR "must never execute this file")\n')
            (root / "CMakeCache.txt").write_text(
                'CMAKE_CACHE_MAJOR_VERSION:INTERNAL=4\nCMAKE_CACHE_MINOR_VERSION:INTERNAL=4\n'
                'CMAKE_CACHE_PATCH_VERSION:INTERNAL=4\n')
            data = capture.compiler_profile(root, capture.read_cache(root))
            self.assertEqual(data, {"id": "MSVC", "version": "19.44.35229.0", "architecture_id": "x64"})
            # An obsolete compiler folder may not be selected by arbitrary directory order.
            old = root / "CMakeFiles" / "3.9.0"
            old.mkdir()
            (old / "CMakeCXXCompiler.cmake").write_text('set(CMAKE_CXX_COMPILER_ID "wrong")')
            self.assertEqual(capture.compiler_profile(root, capture.read_cache(root)), data)
            with self.assertRaises(ValueError):
                capture.compiler_profile(root, {})


if __name__ == "__main__":
    unittest.main()
