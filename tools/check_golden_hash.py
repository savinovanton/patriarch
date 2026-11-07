#!/usr/bin/env python3
"""Cross-platform determinism check (NFR-DET-1, issue #31).

Runs the simcli binary and compares its output with the golden constants committed in
tests/golden_test.cpp (the single source of truth; nothing is duplicated here):

  1. `simcli --verify-determinism --days 800 --seed 7`: the several ways of producing one history
     (direct, threaded, save/load, replay) must agree; the final hash line is echoed so that the
     logs of the Windows and Linux jobs can be compared side by side.
  2. `simcli --seed <kSeed> --days <kDays>`: every part of the final world hash must equal kExpectedHash.

Usage: python tools/check_golden_hash.py <path-to-simcli>
Exit code 0 when everything matches, 1 otherwise.
"""

import os
import re
import subprocess
import sys
from pathlib import Path

GOLDEN_TEST = Path(__file__).resolve().parent.parent / "tests" / "golden_test.cpp"
PARTS = ("total", "calendar", "rng", "dynasties", "provinces", "characters")


def read_golden(text):
    """Returns (seed, days, {part: int}) parsed from tests/golden_test.cpp."""
    seed = re.search(r"constexpr\s+std::uint64_t\s+kSeed\s*=\s*(\d+)\s*;", text)
    days = re.search(r"constexpr\s+int\s+kDays\s*=\s*(\d+)\s*;", text)
    block = re.search(r"kExpectedHash\s*=\s*\{(.*?)\};", text, re.S)
    if not (seed and days and block):
        raise SystemExit("cannot parse kSeed/kDays/kExpectedHash from " + str(GOLDEN_TEST))
    values = re.findall(r"/\*\s*(\w+)\s*\*/\s*0x([0-9a-fA-F]+)ULL", block.group(1))
    expected = {name: int(digits, 16) for name, digits in values}
    if set(expected) != set(PARTS):
        raise SystemExit("kExpectedHash has unexpected parts: " + ", ".join(sorted(expected)))
    return int(seed.group(1)), int(days.group(1)), expected


def run(simcli, *args):
    result = subprocess.run([simcli, *args], capture_output=True, text=True)
    sys.stdout.write(result.stdout)
    sys.stderr.write(result.stderr)
    return result


def parse_hash_line(line):
    return {name: int(digits, 16) for name, digits in re.findall(r"(\w+)=([0-9a-fA-F]{16})", line)}


def main(argv):
    if len(argv) != 2:
        print(__doc__)
        return 1
    simcli = os.path.abspath(argv[1])
    seed, days, expected = read_golden(GOLDEN_TEST.read_text(encoding="utf-8"))
    summary = []
    failed = False

    print("== simcli --verify-determinism --days 800 --seed 7")
    verify = run(simcli, "--verify-determinism", "--days", "800", "--seed", "7")
    final = re.search(r"^final hash\s+(.*)$", verify.stdout, re.M)
    if verify.returncode != 0 or not final:
        print("FAIL: the determinism self-check failed (exit code %d)" % verify.returncode)
        failed = True
    else:
        summary.append("verify-determinism (seed 7, 800 days): `%s`" % final.group(1))

    print("== golden scenario: simcli --seed %d --days %d" % (seed, days))
    golden = run(simcli, "--seed", str(seed), "--days", str(days))
    line = re.search(r"^hash\s+(.*)$", golden.stdout, re.M)
    actual = parse_hash_line(line.group(1)) if line else {}
    if golden.returncode != 0 or set(actual) != set(PARTS):
        print("FAIL: cannot read the world hash from simcli (exit code %d)" % golden.returncode)
        failed = True
    else:
        for part in PARTS:
            ok = actual[part] == expected[part]
            print("%-11s expected %016x actual %016x %s" % (part, expected[part], actual[part], "OK" if ok else "MISMATCH"))
            failed = failed or not ok
        summary.append("golden (seed %d, %d days): `%s`" % (seed, days, line.group(1)))

    print("RESULT:", "FAILED - golden hashes differ from tests/golden_test.cpp" if failed else "OK - hashes match the committed golden values")

    summary_path = os.environ.get("GITHUB_STEP_SUMMARY")
    if summary_path:
        with open(summary_path, "a", encoding="utf-8") as out:
            out.write("### Determinism (%s)\n\n" % os.environ.get("RUNNER_OS", "local"))
            out.write("\n".join("- " + item for item in summary) + "\n")
            out.write("\n**%s**\n" % ("FAILED" if failed else "OK"))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
