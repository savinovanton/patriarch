#!/usr/bin/env python3
"""Compact task/PR status for agents: one call instead of several gh/git turns.

Subcommands:
  claim <issue>        Is the task free? Checks issue labels/state, open PRs that
                       mention the issue or its task ID, and remote branches with the ID.
                       Prints one FREE/BUSY line plus reasons. Exit 0 = free, 1 = busy.
  pr <pr> [--wait]     PR state and CI summary. With --wait blocks in the foreground
                       until checks finish (gh output is suppressed), then prints the
                       summary and up to --max-lines error lines from failed jobs.
                       Exit 0 = all checks pass (or none), 1 = failed, 2 = pending.

Usage:
    python tools/agent/agent_status.py claim 609 [--agent claude]
    python tools/agent/agent_status.py pr 612 --wait

See docs/dev/agents-workflow.md section 3.1 and docs/dev/usage-audit.md.
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys

TASK_ID_RE = re.compile(r"^\s*([A-Za-z][A-Za-z0-9]*-[A-Za-z]?\d+[a-z]?)\b")
RUN_ID_RE = re.compile(r"/actions/runs/(\d+)")
ERROR_RE = re.compile(r"(error|FAILED|fatal|Traceback|assert)", re.IGNORECASE)
# gh --log-failed lines look like "<job>\t<step>\t<timestamp> <text>"; keep only the text.
LOG_PREFIX_RE = re.compile(r"^[^\t]*\t[^\t]*\t(\S+Z )?")


def task_id(title: str) -> str | None:
    """Extracts a task ID such as E13-T04, UI-D08 or Geo-4 from an issue title."""
    m = TASK_ID_RE.match(title or "")
    return m.group(1) if m else None


def claim_reasons(issue: dict, prs: list[dict], branches: list[str], agent: str) -> list[str]:
    """Returns the reasons the task is busy; an empty list means it is free."""
    reasons = []
    number = issue.get("number")
    if issue.get("state", "OPEN").upper() != "OPEN":
        reasons.append(f"issue #{number} is {issue.get('state', '').lower()}")
    labels = [lbl.get("name", "") for lbl in issue.get("labels", [])]
    own = f"agent:{agent}" in labels
    for name in labels:
        if name.startswith("agent:") and name != f"agent:{agent}":
            reasons.append(f"label {name}")
        elif name == "needs:owner" or (name == "status:in-progress" and not own):
            reasons.append(f"label {name}")
    tid = task_id(issue.get("title", ""))
    keys = [f"#{number}"] + ([tid.lower()] if tid else [])
    for pr in prs:
        text = " ".join([pr.get("title", ""), pr.get("headRefName", ""), pr.get("body", "") or ""]).lower()
        if any(k.lower() in text for k in keys) or re.search(rf"\b{number}\b", pr.get("headRefName", "")):
            reasons.append(f"open PR #{pr.get('number')} ({pr.get('headRefName', '')})")
    if tid:
        for br in branches:
            if tid.lower() in br.lower():
                reasons.append(f"remote branch {br}")
    return reasons


def parse_ls_remote(text: str) -> list[str]:
    """Branch names from `git ls-remote --heads` output."""
    out = []
    for line in text.splitlines():
        parts = line.split("\t")
        if len(parts) == 2 and parts[1].startswith("refs/heads/"):
            out.append(parts[1][len("refs/heads/"):])
    return out


def summarize_checks(checks: list[dict]) -> tuple[str, int, list[str]]:
    """Returns (summary line, exit code, run ids of failed checks)."""
    if not checks:
        return "checks: none reported (markdown-only change?)", 0, []
    buckets: dict[str, list[str]] = {}
    for c in checks:
        buckets.setdefault(c.get("bucket") or c.get("state", "?").lower(), []).append(c.get("name", "?"))
    order = ["pass", "fail", "cancel", "pending", "skipping"]
    parts = [f"{k}={len(buckets[k])}" for k in order if k in buckets]
    parts += [f"{k}={len(v)}" for k, v in buckets.items() if k not in order]
    line = "checks: " + " ".join(parts)
    bad = buckets.get("fail", []) + buckets.get("cancel", [])
    if bad:
        line += "; failed: " + ", ".join(sorted(set(bad)))
    failed_runs = []
    for c in checks:
        if (c.get("bucket") in ("fail", "cancel")) and c.get("link"):
            m = RUN_ID_RE.search(c["link"])
            if m and m.group(1) not in failed_runs:
                failed_runs.append(m.group(1))
    code = 1 if bad else 2 if "pending" in buckets else 0
    return line, code, failed_runs


def error_lines(log: str, limit: int) -> list[str]:
    """First unique error-looking lines of a failed-job log, prefixes stripped."""
    seen: list[str] = []
    for raw in log.splitlines():
        text = LOG_PREFIX_RE.sub("", raw).strip()
        if text and ERROR_RE.search(text) and text not in seen:
            seen.append(text[:240])
            if len(seen) >= limit:
                break
    return seen


def run(args: list[str]) -> tuple[int, str]:
    p = subprocess.run(args, capture_output=True, text=True, encoding="utf-8", errors="replace")
    return p.returncode, p.stdout if p.stdout.strip() else p.stderr


def gh_json(args: list[str]):
    code, out = run(["gh", *args])
    if code != 0 and not out.strip().startswith(("[", "{")):
        return None
    try:
        return json.loads(out)
    except json.JSONDecodeError:
        return None


def cmd_claim(ns) -> int:
    issue = gh_json(["issue", "view", str(ns.issue), "--json", "number,title,state,labels"])
    if issue is None:
        print(f"ERROR: cannot read issue #{ns.issue}")
        return 2
    tid = task_id(issue.get("title", ""))
    search = f"{ns.issue} in:title,body" + (f" OR {tid}" if tid else "")
    prs = gh_json(["pr", "list", "--state", "open", "--search", search,
                   "--json", "number,title,headRefName,body", "--limit", "20"]) or []
    _, ls = run(["git", "ls-remote", "--heads", "origin"])
    reasons = claim_reasons(issue, prs, parse_ls_remote(ls), ns.agent)
    head = f"#{issue['number']} {tid or ''}".strip()
    if reasons:
        print(f"BUSY {head}: " + "; ".join(reasons))
        return 1
    print(f"FREE {head}")
    return 0


def cmd_pr(ns) -> int:
    if ns.wait:
        subprocess.run(["gh", "pr", "checks", str(ns.pr), "--watch", "--interval", "30"],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    view = gh_json(["pr", "view", str(ns.pr), "--json", "state,mergeable,mergeStateStatus,headRefName"]) or {}
    print(f"PR #{ns.pr} {view.get('headRefName', '?')}: state={view.get('state', '?')} "
          f"mergeable={view.get('mergeable', '?')} merge_state={view.get('mergeStateStatus', '?')}")
    checks = gh_json(["pr", "checks", str(ns.pr), "--json", "name,state,bucket,link"]) or []
    line, code, failed_runs = summarize_checks(checks)
    print(line)
    for rid in failed_runs[:3]:
        _, log = run(["gh", "run", "view", rid, "--log-failed"])
        lines = error_lines(log, ns.max_lines)
        print(f"run {rid}: {len(lines)} error line(s)")
        for t in lines:
            print("  " + t)
    return code


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("claim", help="is the task free?")
    c.add_argument("issue", type=int)
    c.add_argument("--agent", default="claude", help="own agent label suffix (claude|codex)")
    p = sub.add_parser("pr", help="PR state and CI summary")
    p.add_argument("pr", type=int)
    p.add_argument("--wait", action="store_true", help="block until checks finish")
    p.add_argument("--max-lines", type=int, default=30)
    ns = ap.parse_args(argv)
    return cmd_claim(ns) if ns.cmd == "claim" else cmd_pr(ns)


if __name__ == "__main__":
    sys.exit(main())
