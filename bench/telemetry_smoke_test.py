"""Compare observer accounting to the same timed intervals in the real benchmark runner."""
import json
from pathlib import Path
import subprocess
import sys

exe, destination = sys.argv[1:]
output = Path(destination)
output.unlink(missing_ok=True)
subprocess.run([exe, "--benchmark_filter=^sim.telemetry_tick/",
                "--benchmark_min_time=0.05s", "--benchmark_min_warmup_time=0.01",
                "--benchmark_repetitions=1", "--benchmark_out=" + str(output),
                "--benchmark_out_format=json", "--benchmark_format=console"],
               check=True, timeout=45)
rows = json.loads(output.read_text())["benchmarks"]
assert len(rows) == 2 and not any(row.get("error_occurred") for row in rows), rows
assert rows[0]["label"] == rows[1]["label"], rows
for row in rows:
    enabled = "/1" in row["run_name"]
    assert 0 <= row["observer_ns_per_tick"] <= row["wall_ns_per_tick"]
    assert row["wall_ns_per_tick"] > 0
    assert row["observed_ticks"] == (row["iterations"] * 32 if enabled else 0)
    assert (row["observer_ns_per_tick"] > 0) == enabled
