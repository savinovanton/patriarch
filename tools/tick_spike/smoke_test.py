"""Validate the actual CLI report without asserting a hardware timing threshold."""

import json
import math
import subprocess
import sys


def main():
    result = subprocess.run(
        [sys.argv[1], "--profile", "year60", "--mode", "stagger", "--threads", "1",
         "--grain", "64", "--days", "31", "--repetitions", "1", "--ai-wu", "64"],
        check=True, capture_output=True, text=True, timeout=30,
    )
    data = json.loads(result.stdout)
    assert data["tick"]["samples"] == 31
    assert data["stages"]["ai.daily"]["work_per_run"] == 31 * 64
    assert data["stages"]["economy.monthly"]["work_per_run"] == 400
    assert data["stages"]["trade.monthly"]["work_per_run"] == 300
    assert len(data["hash"]) == 16
    assert len(data["affinity_cpus"]) == 1
    assert data["tick"]["mean_us"] > 0
    for stats in [data["tick"], *data["cadence"].values(),
                  *(stage["active"] for stage in data["stages"].values())]:
        assert all(math.isfinite(stats[key]) for key in ("mean_us", "p50_us", "p95_us", "p99_us", "max_us"))
        assert 0 <= stats["p50_us"] <= stats["p95_us"] <= stats["p99_us"] <= stats["max_us"]


if __name__ == "__main__":
    main()
