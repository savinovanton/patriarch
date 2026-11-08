"""Build and measure two committed revisions on this Windows runner, then compare them.

The stored reference supplies a source revision, never timings from another machine.
Each capture runs once. Inconclusive/noisy results are reported without automatic retries.
"""
import argparse
import html
import json
import os
from pathlib import Path
import re
import subprocess
import sys

import bench_capture
import bench_compare


def git(repo, *args):
    return subprocess.check_output(["git", "-C", str(repo), *args], text=True).strip()


def exact_revision(repo, value):
    if not re.fullmatch(r"[0-9a-fA-F]{40}", value):
        raise ValueError("A full 40-character commit SHA is required")
    resolved = git(repo, "rev-parse", "--verify", value + "^{commit}")
    if resolved.lower() != value.lower():
        raise ValueError("Commit could not be resolved exactly")
    return resolved


def source_worktree(repo, destination, revision):
    if destination.exists():
        if Path(git(destination, "rev-parse", "--show-toplevel")).resolve() != destination.resolve():
            raise ValueError("Expected the exact worktree root, not a subdirectory of another checkout")
        if git(destination, "rev-parse", "HEAD") != revision:
            raise ValueError("Work directory contains another revision; choose a fresh --work-dir")
        if git(destination, "status", "--porcelain", "--untracked-files=no"):
            raise ValueError("Source worktree has tracked modifications")
    else:
        subprocess.run(["git", "-C", str(repo), "worktree", "add", "--detach", str(destination), revision],
                       check=True)
    return destination


def build(source, build_dir, installed_dir, log):
    configure = ["cmake", "--preset", "win-msvc", "-S", str(source), "-B", str(build_dir),
                 "-DPATRIARCH_BUILD_ENGINE=OFF", "-DPATRIARCH_BUILD_TESTS=OFF",
                 "-DPATRIARCH_BUILD_TOOLS=OFF", "-DPATRIARCH_BUILD_BENCHMARKS=ON",
                 "-DPATRIARCH_SANITIZE_ADDRESS=OFF", "-DPATRIARCH_ENABLE_TRACY=OFF",
                 "-DVCPKG_INSTALLED_DIR=" + str(installed_dir)]
    compile_command = ["cmake", "--build", str(build_dir), "--config", "Release",
                       "--target", "bench", "--parallel", "1", "--", "/nr:false"]
    with log.open("w", encoding="utf-8") as output:
        subprocess.run(configure, cwd=source, stdout=output, stderr=subprocess.STDOUT,
                       check=True, timeout=1800)
        subprocess.run(compile_command, cwd=source, stdout=output, stderr=subprocess.STDOUT,
                       check=True, timeout=1800)


def record_summary(report):
    destination = os.environ.get("GITHUB_STEP_SUMMARY")
    if not destination:
        return
    lines = ["## Performance: " + report["status"], "",
             "Baseline: `" + str(report.get("baseline_sha", "unknown")) + "`  ",
             "Candidate: `" + str(report.get("candidate_sha", "unknown")) + "`", "",
             "| Benchmark | Change | Status |", "|---|---:|---|"]
    for row in report.get("benchmarks", []):
        change = row.get("change_fraction")
        value = "—" if change is None else "{:+.2f}%".format(change * 100)
        # Names are controlled suite values; remove Markdown table separators before presentation.
        name = str(row["name"]).replace("|", "\\|").replace("\n", " ")
        lines.append("| " + name + " | " + value + " | " + row["status"] + " |")
    for error in report.get("errors", []):
        lines.extend(["", str(error)])
    lines.extend(["", "Полный HTML/SVG и исходные JSON доступны в артефакте performance-report."])
    with open(destination, "a", encoding="utf-8") as output:
        output.write("\n".join(lines) + "\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--suite", type=Path, required=True)
    parser.add_argument("--reference", type=Path)
    parser.add_argument("--baseline-sha")
    parser.add_argument("--candidate-sha")
    parser.add_argument("--work-dir", type=Path, required=True)
    args = parser.parse_args()
    repo, work = args.repo.resolve(), args.work_dir.resolve()
    reports = work / "reports"
    reports.mkdir(parents=True, exist_ok=True)
    report = None
    try:
        if sys.platform != "win32":
            raise ValueError("The paired build runner uses the project's Windows/MSVC preset")
        suite_path = args.suite.resolve()
        suite = bench_compare.validate_suite(bench_compare.read_json(suite_path))
        candidate = exact_revision(repo, args.candidate_sha or git(repo, "rev-parse", "HEAD"))
        reference_sha = None
        if args.reference:
            frozen = bench_compare.freeze_measurement(bench_compare.read_json(args.reference.resolve()), suite)
            reference_sha = exact_revision(repo, frozen["source_sha"])
            # Check this even on PR runs, so an invalid reference update cannot pass then break nightly.
            subprocess.run(["git", "-C", str(repo), "merge-base", "--is-ancestor", reference_sha, "origin/main"],
                           check=True)
        if args.baseline_sha:
            baseline = exact_revision(repo, args.baseline_sha)
        else:
            if reference_sha is None:
                raise ValueError("Provide --baseline-sha or the reviewed --reference file")
            baseline = reference_sha
        subprocess.run(["git", "-C", str(repo), "merge-base", "--is-ancestor", baseline, candidate], check=True)
        sources = []
        for label, revision in (("baseline", baseline), ("candidate", candidate)):
            source = source_worktree(repo, work / (label + "-source"), revision)
            build_dir = work / (label + "-build")
            print("Building", label, revision, flush=True)
            build(source, build_dir, work / "vcpkg_installed", reports / (label + "-build.log"))
            sources.append((label, source, build_dir))
        # Keep compilation outside both sampling phases, so one side is not measured directly
        # after a hot compiler while the other is measured after an idle/cached configure.
        for label, source, build_dir in sources:
            print("Measuring", label, flush=True)
            bench_capture.capture(source, build_dir, suite_path, reports / (label + ".json"))
        report = bench_compare.compare_measurements(bench_compare.read_json(reports / "baseline.json"),
                                                    bench_compare.read_json(reports / "candidate.json"), suite)
    except (ValueError, KeyError, OSError, subprocess.SubprocessError) as error:
        # Do not put local command lines/user paths in a public report. Logs remain separate.
        print("bench_gate:", error, file=sys.stderr)
        report = {"schema": 1, "status": "invalid", "exit_code": 2,
                  "benchmarks": [], "warnings": [],
                  "errors": ["Build/capture/input validation failed; see the build/capture log."]}
    bench_compare.write_json_atomic(reports / "report.json", report)
    if "profile" in report:
        (reports / "report.html").write_text(bench_compare.render_html(report), encoding="utf-8")
    else:
        (reports / "report.html").write_text("<!doctype html><meta charset=utf-8><title>Performance invalid</title>"
            "<h1>Недостаточно данных для сравнения</h1><p>" +
            html.escape(report["errors"][0]) + "</p>", encoding="utf-8")
    record_summary(report)
    print("Performance:", report["status"], flush=True)
    return report["exit_code"]


if __name__ == "__main__":
    sys.exit(main())
