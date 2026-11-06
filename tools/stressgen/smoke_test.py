"""Check the CLI overrides and compatibility of its save with the existing simcli."""

import json
from pathlib import Path
import subprocess
import sys
import tempfile


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
        process = subprocess.run(
            [executable, "--characters=-1", "--output", str(Path(folder) / "invalid.psv")],
            capture_output=True, timeout=5,
        )
        assert process.returncode != 0
        assert not (Path(folder) / "invalid.psv").exists()


if __name__ == "__main__":
    main()
