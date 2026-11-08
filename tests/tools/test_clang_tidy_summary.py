import importlib.util
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parents[2] / "tools" / "clang_tidy_summary.py"
spec = importlib.util.spec_from_file_location("clang_tidy_summary", SCRIPT)
summary = importlib.util.module_from_spec(spec)
spec.loader.exec_module(summary)

LOG = """\
/usr/bin/clang-tidy-18 -p=out/linux-clang -quiet /w/src/a.cpp
30282 warnings generated.
/w/src/core/a.hpp:19:45: warning: widening [bugprone-implicit-widening-of-multiplication-result]
   19 |     explicit Arena(std::size_t block_size = 64 * 1024);
/w/src/core/a.hpp:19:45: warning: widening [bugprone-implicit-widening-of-multiplication-result]
/w/src/sim/b.cpp:3:1: warning: move has no effect [performance-move-const-arg]
/w/src/sim/b.cpp:3:1: note: some note [performance-move-const-arg]
"""


class ClangTidySummaryTest(unittest.TestCase):
    def test_findings_are_deduplicated_by_location_and_check(self):
        findings = summary.parse(LOG.splitlines())
        self.assertEqual(len(findings), 2)

    def test_render_reports_count_and_checks(self):
        text = summary.render(summary.parse(LOG.splitlines()))
        self.assertIn("**Findings: 2**", text)
        self.assertIn("`performance-move-const-arg` | 1", text)
        self.assertIn("`src/core/a.hpp`: 1", text)

    def test_empty_log_reports_zero(self):
        text = summary.render(summary.parse([]))
        self.assertIn("**Findings: 0**", text)


if __name__ == "__main__":
    unittest.main()
