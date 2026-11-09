"""Check the CLI overrides and compatibility of its save with the existing simcli."""

import json
from pathlib import Path
import subprocess
import sys
import tempfile


def check_economy(executable, simcli, folder):
    """--economy: a valid economy world with the memory counters, loadable by simcli, same hash per seed."""
    common = ["--profile", "year1", "--seed", "9", "--dynasties", "5", "--provinces", "30",
              "--characters", "60", "--armies", "0", "--schemes", "0", "--contracts", "0", "--events", "0"]
    summaries = []
    for name in ("economy_a.psv", "economy_b.psv"):
        output = folder / name
        process = subprocess.run([executable, *common, "--economy", "--output", str(output)],
                                 check=True, capture_output=True, text=True, timeout=20)
        summaries.append(json.loads(process.stdout))
    first, second = summaries
    assert first["economy"] is True and first["closed_months"] == 12, first
    assert first["economy_bytes"] > first["ledger_bytes"] > 0, first
    assert 0 < first["bytes_per_province"] < 100_000, first
    assert first["world_hash"] == second["world_hash"] and first["economy_hash"] == second["economy_hash"]
    loaded = subprocess.run([simcli, "--load", str(folder / "economy_a.psv"), "--days", "0", "--hash-only"],
                            check=True, capture_output=True, text=True, timeout=10)
    assert loaded.stdout.strip() == first["world_hash"], loaded.stdout
    plain = subprocess.run([executable, *common, "--output", str(folder / "plain.psv")],
                           check=True, capture_output=True, text=True, timeout=20)
    assert "economy" not in json.loads(plain.stdout)

    # --bench: the report of the measurements, without --output (a temporary file for the disk timings)
    bench = subprocess.run([executable, *common, "--bench", "--bench-repeats", "2",
                            "--bench-heavy-repeats", "2", "--bench-days", "40"],
                           check=True, capture_output=True, text=True, timeout=60)
    report = json.loads(bench.stdout)
    assert report["memory"]["provinces"] == 30, report
    assert report["skeleton"]["deterministic"] is True, report
    assert report["loaded_hash_equal"] is True, report
    names = {metric["name"] for metric in report["skeleton"]["metrics"]}
    assert {"system.economy.settlement", "system.economy.construction", "system.economy.provinces"} <= names, names
    bad = subprocess.run([executable, *common, "--closed-months", "13", "--economy", "--output", str(folder / "bad.psv")],
                         capture_output=True, timeout=20)
    assert bad.returncode != 0
    assert not (folder / "bad.psv").exists()


def main():
    executable, simcli = sys.argv[1:]
    with tempfile.TemporaryDirectory(prefix="patriarch-stressgen-") as folder:
        output = Path(folder) / "world.psv"
        process = subprocess.run(
            [executable, "--characters=17", "--profile", "year1", "--seed", "9",
             "--dynasties", "3", "--provinces", "7", "--armies", "4", "--schemes", "5",
             "--contracts", "6", "--events", "7", "--output", str(output)],
            check=True, capture_output=True, text=True, timeout=20,
        )
        summary = json.loads(process.stdout)
        for key, value in dict(characters=17, dynasties=3, provinces=7, armies=4,
                               schemes=5, contracts=6, events=7, seed=9).items():
            assert summary[key] == value, summary
        assert output.read_bytes().startswith(b"PSV1")
        assert Path(str(output) + ".psl").read_bytes().startswith(b"PSL1")
        loaded = subprocess.run(
            [simcli, "--load", str(output), "--days", "0", "--hash-only"],
            check=True, capture_output=True, text=True, timeout=10,
        )
        assert loaded.stdout.strip() == summary["world_hash"], loaded.stdout
        check_economy(executable, simcli, Path(folder))
        process = subprocess.run(
            [executable, "--characters=-1", "--output", str(Path(folder) / "invalid.psv")],
            capture_output=True, timeout=5,
        )
        assert process.returncode != 0
        assert not (Path(folder) / "invalid.psv").exists()


if __name__ == "__main__":
    main()
