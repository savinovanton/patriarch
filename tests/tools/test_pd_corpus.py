"""Consistency checks for the .pd test corpus (tests/data/pd) against docs/dev/data-format.md."""

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CORPUS = ROOT / "tests" / "data" / "pd"
SPEC = ROOT / "docs" / "dev" / "data-format.md"

EXPECT_LINE = re.compile(r"^(\d+):(\d+) (PD\d{3})$")
# Errors that cannot be provoked by a small committed file; their unit tests generate the input.
GENERATED_IN_TESTS = {"PD021", "PD022", "PD023"}


def spec_codes() -> set[str]:
    text = SPEC.read_text(encoding="utf-8")
    return set(re.findall(r"^\| `(PD\d{3})` \|", text, re.MULTILINE))


class PdCorpusTest(unittest.TestCase):
    def stems(self, directory: str, suffix: str) -> set[str]:
        return {p.stem for p in (CORPUS / directory).glob(f"*{suffix}")}

    def test_spec_has_error_codes(self):
        self.assertGreaterEqual(len(spec_codes()), 15)

    def test_valid_files_have_trees(self):
        self.assertEqual(self.stems("valid", ".pd"), self.stems("valid", ".tree"))
        self.assertTrue(self.stems("valid", ".pd"))

    def test_invalid_files_have_expectations(self):
        self.assertEqual(self.stems("invalid", ".pd"), self.stems("invalid", ".expect"))
        self.assertTrue(self.stems("invalid", ".pd"))

    def test_valid_files_are_utf8_with_version_first(self):
        for path in (CORPUS / "valid").glob("*.pd"):
            text = path.read_bytes().decode("utf-8")
            code = [ln for ln in text.splitlines() if ln.strip() and not ln.lstrip().startswith("#")]
            self.assertEqual(code[0], "format_version = 1", path.name)

    def test_expectations_are_well_formed(self):
        codes = spec_codes()
        for path in (CORPUS / "invalid").glob("*.expect"):
            lines = path.read_text(encoding="utf-8").splitlines()
            self.assertTrue(lines, path.name)
            source = (CORPUS / "invalid" / (path.stem + ".pd")).read_bytes().split(b"\n")
            for line in lines:
                m = EXPECT_LINE.match(line)
                self.assertIsNotNone(m, f"{path.name}: bad expectation '{line}'")
                row, col, code = int(m.group(1)), int(m.group(2)), m.group(3)
                self.assertIn(code, codes, f"{path.name}: unknown code {code}")
                self.assertTrue(1 <= row <= len(source), f"{path.name}: line {row} outside the file")
                width = len(source[row - 1].decode("utf-8", errors="replace"))
                self.assertTrue(1 <= col <= width + 1, f"{path.name}: column {col} outside line {row}")

    def test_every_error_code_is_covered(self):
        covered = set()
        for path in (CORPUS / "invalid").glob("*.expect"):
            covered.update(ln.split()[1] for ln in path.read_text(encoding="utf-8").splitlines())
        missing = spec_codes() - covered - GENERATED_IN_TESTS
        self.assertEqual(missing, set(), "error codes without a corpus file")

    def test_tree_files_use_canonical_value_kinds(self):
        value = re.compile(r'(=|!=|<=|>=|<|>) (int|dec|bool|str|date|ident|ref):|^\s*- (int|dec|bool|str|date|ident|ref):|[{\[]$|^\s*[}\]]$')
        for path in (CORPUS / "valid").glob("*.tree"):
            for n, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
                self.assertRegex(line, value, f"{path.name}:{n}")


if __name__ == "__main__":
    unittest.main()
