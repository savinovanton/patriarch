"""Run the bounded E2-T03 matrix; preserve raw results and verify cross-case state hashes."""

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
    parser.add_argument("--exe", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--profiles", default="year1,year60,year60x3")
    parser.add_argument("--threads", default="1,2,4,6")
    parser.add_argument("--grains", default="64,256")
    parser.add_argument("--years", type=int, default=10)
    parser.add_argument("--repetitions", type=int, default=3)
    args = parser.parse_args()
    cases = list(itertools.product(args.profiles.split(","), ("burst", "stagger"),
                                   map(int, args.threads.split(",")), map(int, args.grains.split(","))))
    random.Random(20261106).shuffle(cases)  # Fixed order randomization, independent of simulation RNG.
    results = []
    expected = {}
    root = Path(__file__).resolve().parent
    sources = {p.name: hashlib.sha256(p.read_bytes()).hexdigest()
               for p in root.iterdir() if p.suffix in (".cpp", ".hpp") and p.name != "spike_test.cpp"}
    for index, (profile, mode, threads, grain) in enumerate(cases, 1):
        process = subprocess.run(
            [str(args.exe.resolve()), "--profile", profile, "--mode", mode,
             "--threads", str(threads), "--grain", str(grain), "--years", str(args.years),
             "--repetitions", str(args.repetitions), "--seed", "7"],
            check=True, capture_output=True, text=True, timeout=300,
        )
        row = json.loads(process.stdout)
        if row["build"] not in ("Release", "RelWithDebInfo"):
            raise RuntimeError("Measurements require Release/RelWithDebInfo without ASan")
        if profile in expected and expected[profile] != row["hash"]:
            raise RuntimeError(f"State hash differs for {profile}: {mode}/{threads}/{grain}")
        expected[profile] = row["hash"]
        results.append(row)
        print(f"{index}/{len(cases)} {profile:8} {mode:7} t={threads} g={grain} "
              f"mean={row['tick']['mean_us']:.2f}us p99={row['tick']['p99_us']:.2f}us "
              f"RSD={row['repeat_rsd_percent']:.1f}%", flush=True)
        args.output.write_text(json.dumps({"schema": 1, "started_utc": started,
                                           "source_sha256": sources, "runs": results}, indent=2) + "\n",
                               encoding="utf-8")


started = datetime.now(timezone.utc).isoformat()
if __name__ == "__main__":
    main()
