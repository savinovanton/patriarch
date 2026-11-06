"""One paired snapshot matrix; persist noisy results without retries or timing gates."""
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
    args = parser.parse_args()
    cases = [(profile, changed, rate, 0) for profile, changed, rate in itertools.product(
        ("year60", "year60x3"), (10, 100, 1000), (40, 400))]
    cases += [("year60", 10, 400, 2), ("year60x3", 1000, 400, 2)]
    random.Random(20261107).shuffle(cases)
    root = Path(__file__).resolve().parent
    artifact = {"schema": 1, "started_utc": datetime.now(timezone.utc).isoformat(),
                "source_sha256": {name: hashlib.sha256((root / name).read_bytes()).hexdigest()
                                  for name in ("snapshots.cpp", "snapshots.hpp", "main.cpp")}, "runs": []}
    for index, (profile, changed, rate, hold) in enumerate(cases, 1):
        process = subprocess.run([str(args.exe.resolve()), "--profile", profile,
                                  "--changed-permille", str(changed), "--rate", str(rate),
                                  "--hold-frames", str(hold)],
                                 text=True, capture_output=True, timeout=120, check=True)
        row = json.loads(process.stdout)
        if row["build"] not in ("Release", "RelWithDebInfo"):
            raise RuntimeError("Use a Release/RelWithDebInfo binary without ASan")
        artifact["runs"].append(row)
        args.output.write_text(json.dumps(artifact, indent=2) + "\n", encoding="utf-8")
        costs = ", ".join(f"{name}={m['paced']['publish_us']['mean']:.2f}us"
                          for name, m in row["methods"].items())
        print(f"{index}/{len(cases)} {profile} changed={changed}/1000 rate={rate} "
              f"hold={hold}: {costs}", flush=True)


if __name__ == "__main__":
    main()
