"""Validate a real concurrent run and JSON accounting without timing thresholds."""
import json
import math
import subprocess
import sys

run = subprocess.run([sys.argv[1], "--seconds", "1", "--repetitions", "1",
                      "--iterations", "32", "--rate", "120", "--hold-frames", "2"],
                     capture_output=True, text=True, timeout=45, check=True)
data = json.loads(run.stdout)
assert data["correct"] and data["rows"] == 500 and data["ui_payload_bytes"] == 16000
for method in data["methods"].values():
    for phase in method.values():
        for key in ("publish_us", "acquire_us", "read_us", "age_at_acquire_us"):
            stats = phase[key]
            assert all(math.isfinite(v) and v >= 0 for v in stats.values())
            assert stats["p50"] <= stats["p95"] <= stats["p99"] <= stats["max"]
        assert phase["owned_bytes_peak"] > 0 and phase["object_allocations"] > 0
    paced = method["paced"]
    assert paced["read_us"]["samples"] > 0 and paced["distinct_versions_read"] > 0
    assert paced["publications_per_second"] > 0
    assert paced["distinct_versions_read"] + paced["repeated_frames"] == paced["read_us"]["samples"]
assert data["methods"]["diff"]["paced"]["queue_peak"] <= 64
