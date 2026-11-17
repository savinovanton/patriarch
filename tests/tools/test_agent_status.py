import importlib.util
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parents[2] / "tools" / "agent" / "agent_status.py"
spec = importlib.util.spec_from_file_location("agent_status", SCRIPT)
agent_status = importlib.util.module_from_spec(spec)
spec.loader.exec_module(agent_status)


def issue(labels=(), title="E13-T04: SaveCatalog", state="OPEN", number=376):
    return {"number": number, "title": title, "state": state, "labels": [{"name": n} for n in labels]}


class TaskIdTest(unittest.TestCase):
    def test_ids(self):
        self.assertEqual(agent_status.task_id("E13-T04: SaveCatalog"), "E13-T04")
        self.assertEqual(agent_status.task_id("UI-D08: states"), "UI-D08")
        self.assertEqual(agent_status.task_id("Geo-4: stage 1"), "Geo-4")
        self.assertIsNone(agent_status.task_id("Аудит расхода лимитов агентов"))


class ClaimTest(unittest.TestCase):
    def test_free(self):
        self.assertEqual(agent_status.claim_reasons(issue(["status:ready", "agent:claude"]), [], ["main"], "claude"), [])

    def test_own_claim_is_not_busy(self):
        labels = ["agent:claude", "status:in-progress"]
        self.assertEqual(agent_status.claim_reasons(issue(labels), [], [], "claude"), [])
        self.assertEqual(agent_status.claim_reasons(issue(labels), [], [], "codex"),
                         ["label agent:claude", "label status:in-progress"])

    def test_busy_by_labels_pr_and_branch(self):
        prs = [{"number": 9, "title": "E13-T04: catalog", "headRefName": "codex/E13-T04-x", "body": ""}]
        reasons = agent_status.claim_reasons(
            issue(["agent:codex", "status:in-progress"]), prs, ["main", "codex/E13-T04-x"], "claude")
        self.assertIn("label agent:codex", reasons)
        self.assertIn("label status:in-progress", reasons)
        self.assertTrue(any(r.startswith("open PR #9") for r in reasons))
        self.assertIn("remote branch codex/E13-T04-x", reasons)

    def test_closed_and_pr_by_number(self):
        prs = [{"number": 5, "title": "Docs", "headRefName": "claude/usage", "body": "Closes #376"}]
        reasons = agent_status.claim_reasons(issue(state="CLOSED"), prs, [], "claude")
        self.assertIn("issue #376 is closed", reasons)
        self.assertTrue(any(r.startswith("open PR #5") for r in reasons))

    def test_parse_ls_remote(self):
        text = "abc\trefs/heads/main\ndef\trefs/heads/claude/E1-T01-x\nbad line\n"
        self.assertEqual(agent_status.parse_ls_remote(text), ["main", "claude/E1-T01-x"])


class ChecksTest(unittest.TestCase):
    def test_none(self):
        line, code, runs = agent_status.summarize_checks([])
        self.assertEqual(code, 0)
        self.assertEqual(runs, [])
        self.assertIn("none", line)

    def test_failed_and_pending(self):
        checks = [
            {"name": "linux", "bucket": "pass", "link": "https://x/actions/runs/1/job/2"},
            {"name": "windows", "bucket": "fail", "link": "https://x/actions/runs/7/job/3"},
            {"name": "asan", "bucket": "fail", "link": "https://x/actions/runs/7/job/4"},
            {"name": "perf", "bucket": "pending", "link": ""},
        ]
        line, code, runs = agent_status.summarize_checks(checks)
        self.assertEqual(code, 1)
        self.assertEqual(runs, ["7"])
        self.assertIn("pass=1 fail=2 pending=1", line)
        self.assertIn("failed: asan, windows", line)

    def test_pending_only(self):
        _, code, _ = agent_status.summarize_checks([{"name": "a", "bucket": "pending"}])
        self.assertEqual(code, 2)

    def test_error_lines(self):
        log = ("build\tcompile\t2026-11-17T10:00:00.0Z foo.cpp(3): error C2065: x\n"
               "build\tcompile\t2026-11-17T10:00:01.0Z ok line\n"
               "build\tcompile\t2026-11-17T10:00:02.0Z foo.cpp(3): error C2065: x\n"
               "test\trun\t2026-11-17T10:00:03.0Z 1 test FAILED\n")
        self.assertEqual(agent_status.error_lines(log, 30), ["foo.cpp(3): error C2065: x", "1 test FAILED"])
        self.assertEqual(len(agent_status.error_lines(log, 1)), 1)


if __name__ == "__main__":
    unittest.main()
