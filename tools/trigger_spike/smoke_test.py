"""Check the actual report and exact semantics; do not gate on hardware timings."""

import json
import math
import subprocess
import sys


def main():
    result = subprocess.run(
        [sys.argv[1], "--characters", "100", "--days", "31", "--repetitions", "1"],
        capture_output=True, text=True, check=True, timeout=30,
    )
    data = json.loads(result.stdout)
    methods = data["methods"]
    assert data["events"] == 500
    assert methods["naive"]["hash"] == methods["indexed"]["hash"]
    assert methods["naive"]["action_hits"] == methods["sampled"]["action_hits"]
    assert methods["indexed"]["predicates_per_run"] == methods["naive"]["predicates_per_run"]
    assert methods["sampled"]["pulse_hits"] <= methods["indexed"]["pulse_hits"]
    for row in methods.values():
        stats = row["tick_us"]
        assert stats["samples"] == 31
        assert math.isfinite(stats["mean"]) and stats["mean"] > 0
        assert 0 <= stats["p50"] <= stats["p95"] <= stats["p99"] <= stats["max"]
    audit = data["pulse_audit"]
    assert audit["sampled_distinct_pairs"] + audit["undetected_pairs"] == audit["qualified_distinct_pairs"]
    assert 0 <= audit["recall_percent"] <= 100


if __name__ == "__main__":
    main()
