"""Exercise the real benchmark runner and validate its machine-readable output."""

import json
import math
from pathlib import Path
import subprocess
import sys


def main():
    executable, output = sys.argv[1:]
    output = Path(output)
    # A stale successful result must never hide a failed invocation.
    output.unlink(missing_ok=True)
    subprocess.run(
        [executable, "--benchmark_filter=^core.pcg32_next/1024$",
         "--benchmark_min_time=0.001s", "--benchmark_min_warmup_time=0.001",
         "--benchmark_repetitions=2", "--benchmark_out=" + str(output),
         "--benchmark_out_format=json", "--benchmark_format=console"],
        check=True, timeout=40,
    )
    result = json.loads(output.read_text(encoding="utf-8"))
    assert "cpu_affinity" in result["context"], result["context"]
    rows = result["benchmarks"]
    assert rows and not any(row.get("error_occurred") for row in rows), rows
    runs = [row for row in rows if row["run_type"] == "iteration"]
    assert len(runs) == 2, runs
    for row in runs:
        assert row["run_name"] == "core.pcg32_next/1024", row
        assert row["iterations"] > 0, row
        assert math.isfinite(row["cpu_time"]) and row["cpu_time"] > 0, row
        assert row["items_per_second"] > 0, row
    assert any(row.get("aggregate_name") == "median" for row in rows), rows

    # JSON is also the default stdout format; CLI flags can override run defaults.
    process = subprocess.run(
        [executable, "--pat_cpu=none", "--benchmark_filter=^core.pcg32_next/1024$",
         "--benchmark_min_time=0.001s", "--benchmark_min_warmup_time=0",
         "--benchmark_repetitions=1"],
        check=True, capture_output=True, text=True, timeout=10,
    )
    result = json.loads(process.stdout)
    assert result["context"]["cpu_affinity"] == "none", result
    assert len(result["benchmarks"]) == 1, result

    for flag in ("--pat_cpu=invalid", "--pat_cpu=-1", "--pat_cpu=99999999999999999999",
                 "--pat_cpu=2147483647",
                 "--benchmark_filter=^does_not_exist$", "--invalid_flag"):
        process = subprocess.run([executable, flag], capture_output=True, timeout=5)
        assert process.returncode != 0, flag
    print("Benchmark smoke checks passed")


if __name__ == "__main__":
    main()
