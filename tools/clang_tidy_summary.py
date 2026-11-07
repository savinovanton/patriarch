#!/usr/bin/env python3
"""Summarizes a clang-tidy log (as written by run-clang-tidy) as Markdown for the GitHub job summary.

Usage: python tools/clang_tidy_summary.py <log-file>
Prints the number of findings, a per-check table and the top files. Always exits with 0: the clang-tidy
job is report-only (issue #30) until the warning count has been brought down.
"""

import collections
import re
import sys

# "/path/to/file.cpp:12:5: warning: message [check-name]" (also "error:" for checks promoted by config).
FINDING = re.compile(r"^(?P<file>.+?):(?P<line>\d+):(?P<col>\d+): (?P<level>warning|error): (?P<msg>.*) \[(?P<check>[\w.,-]+)\]$")


def parse(lines):
    """Returns a list of unique (file, line, col, check) findings with their messages.

    run-clang-tidy prints the same diagnostic once per translation unit that includes a header, so the
    findings are de-duplicated by location and check.
    """
    seen = {}
    for raw in lines:
        match = FINDING.match(raw.rstrip())
        if not match:
            continue
        key = (match["file"], int(match["line"]), int(match["col"]), match["check"])
        seen.setdefault(key, match["msg"])
    return seen


def short_path(path):
    marker = "/src/"
    index = path.replace("\\", "/").rfind(marker)
    return path.replace("\\", "/")[index + 1 :] if index >= 0 else path


def render(findings):
    by_check = collections.Counter(key[3] for key in findings)
    by_file = collections.Counter(short_path(key[0]) for key in findings)
    out = ["## clang-tidy (report only)", "", f"**Findings: {len(findings)}** (de-duplicated by location and check)", ""]
    if findings:
        out += ["| Check | Count |", "|---|---:|"]
        out += [f"| `{check}` | {count} |" for check, count in by_check.most_common(25)]
        out += ["", "Top files:", ""]
        out += [f"- `{path}`: {count}" for path, count in by_file.most_common(10)]
        out += ["", "The full log is attached to the run as the `clang-tidy-report` artifact."]
    return "\n".join(out) + "\n"


def main(argv):
    if len(argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    with open(argv[1], encoding="utf-8", errors="replace") as handle:
        findings = parse(handle)
    sys.stdout.write(render(findings))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
