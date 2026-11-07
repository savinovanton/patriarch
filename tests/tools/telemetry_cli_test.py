"""Check one real run's JSON/CSV, bounded window and the direct/threaded/replay interfaces."""
import csv
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile

exe = sys.argv[1]


def run(*args, ok=True):
    p = subprocess.run([exe, *map(str, args)], text=True, capture_output=True, timeout=30)
    assert (p.returncode == 0) == ok, (args, p.returncode, p.stderr)
    return p.stdout.strip()


with tempfile.TemporaryDirectory(prefix="patriarch-telemetry-") as directory:
    root = Path(directory)
    telemetry_json, telemetry_csv, replay = (root / name for name in ("timing.json", "timing.csv", "test.prp"))
    expected = run("--days", 400, "--seed", 7, "--hash-only")
    actual = run("--days", 400, "--seed", 7, "--hash-only", "--telemetry-window", 64,
                 "--telemetry-json", telemetry_json, "--telemetry-csv", telemetry_csv, "--record", replay)
    assert actual == expected
    data = json.loads(telemetry_json.read_text())
    assert data["schema"] == 1 and data["unit"] == "ns" and not data["saturated"]
    assert (data["window_capacity"], data["frames"], data["first_frame"], data["last_frame"]) == (64, 64, 337, 400)
    metrics = {m["name"]: m for m in data["metrics"]}
    assert metrics["sim.tick"]["calls"] == 64
    assert metrics["system.demo.ageing"]["calls"] == 64
    assert metrics["system.demo.births"]["calls"] == metrics["sim.tick.monthly"]["calls"] > 0
    assert metrics["system.demo.rewards"]["calls"] == metrics["sim.tick.quarterly"]["calls"] > 0
    assert metrics["sim.tick.yearly"]["calls"] == 1
    csv_rows = list(csv.DictReader(io.StringIO(telemetry_csv.read_text())))
    assert len(csv_rows) == 2 * len(metrics)
    for row in csv_rows:
        m = metrics[row["metric"]]
        for key, value in m[row["kind"]].items():
            assert row[key] == (str(value).lower() if isinstance(value, bool) else str(value)), (row, key)
        assert int(row["calls"]) == m["calls"]
        d = m[row["kind"]]
        assert 0 <= d["p50_ns"] <= d["p95_ns"] <= d["p99_ns"] <= d["max_ns"]

    assert run("--days", 400, "--seed", 7, "--speed", "unlimited", "--hash-only",
               "--telemetry-json", root / "threaded.json", "--telemetry-window", 64) == expected
    threaded = json.loads((root / "threaded.json").read_text())
    assert threaded["last_frame"] == 400 and threaded["frames"] == 64
    assert run("--replay", replay, "--hash-only", "--telemetry-json", root / "replay.json") == expected
    assert json.loads((root / "replay.json").read_text())["frames"] == 400
    run("--days", 0, "--quiet", "--telemetry-json", root / "empty.json")
    assert json.loads((root / "empty.json").read_text())["frames"] == 0
    run("--telemetry-window", 0, "--telemetry", ok=False)
    run("--telemetry-window", 16, ok=False)
    run("--days", 0, "--telemetry-json", root / "absent" / "no.json", ok=False)
