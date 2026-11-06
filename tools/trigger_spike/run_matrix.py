"""One fixed matrix of character counts and on-action densities, without timing gates."""

import argparse
from datetime import datetime, timezone
import hashlib
import itertools
import json
from pathlib import Path
import random
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--characters", default="1000,5000,15000")
    parser.add_argument("--changed-permille", default="10,100,1000")
    parser.add_argument("--days", type=int, default=365)
    parser.add_argument("--repetitions", type=int, default=3)
    args = parser.parse_args()
    cases = list(itertools.product(map(int, args.characters.split(",")),
                                   map(int, args.changed_permille.split(","))))
    random.Random(20261106).shuffle(cases)
    root = Path(__file__).resolve().parent
    sources = {name: hashlib.sha256((root / name).read_bytes()).hexdigest()
               for name in ("triggers.cpp", "triggers.hpp", "main.cpp")}
    artifact = {"schema": 1, "started_utc": datetime.now(timezone.utc).isoformat(),
                "source_sha256": sources, "runs": []}
    for index, (characters, changed) in enumerate(cases, 1):
        process = subprocess.run(
            [str(args.exe.resolve()), "--characters", str(characters), "--changed-permille", str(changed),
             "--days", str(args.days), "--repetitions", str(args.repetitions)],
            text=True, capture_output=True, timeout=600,
        )
        if process.returncode != 0:
            raise RuntimeError(process.stderr.strip())
        row = json.loads(process.stdout)
        if row["build"] not in ("Release", "RelWithDebInfo"):
            raise RuntimeError("Use a Release/RelWithDebInfo binary without ASan")
        artifact["runs"].append(row)
        base = row["methods"]["naive"]["tick_us"]["mean"]
        indexed = row["methods"]["indexed"]["tick_us"]["mean"]
        print(f"{index}/{len(cases)} N={characters:5} changed={changed}/1000 "
              f"naive={base:.1f}us index={indexed:.1f}us speedup={base/indexed:.2f}x "
              f"pulse-recall={row['pulse_audit']['recall_percent']:.1f}%", flush=True)
        args.output.write_text(json.dumps(artifact, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
