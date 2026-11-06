import importlib.util
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parents[2] / "tools" / "check_assets.py"
spec = importlib.util.spec_from_file_location("check_assets", SCRIPT)
check_assets = importlib.util.module_from_spec(spec)
spec.loader.exec_module(check_assets)

HEADER = "path,title,source_url,license,author,attribution_required,checked_on,notes\n"


class CheckAssetsTest(unittest.TestCase):
    def make_dir(self, registry_rows: str = "", files: tuple[str, ...] = ()) -> Path:
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        root = Path(tmp.name)
        (root / "LICENSES.csv").write_text(HEADER + registry_rows, encoding="utf-8")
        for name in files:
            target = root / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(b"x")
        return root

    def test_empty_registry_is_valid(self):
        self.assertEqual(check_assets.check(self.make_dir()), [])

    def test_registered_file_is_valid(self):
        row = "fonts/a.ttf,Font A,https://example.org/a,OFL-1.1,Author,yes,2026-11-06,\n"
        root = self.make_dir(row, ("fonts/a.ttf",))
        self.assertEqual(check_assets.check(root), [])

    def test_unregistered_file_is_reported(self):
        root = self.make_dir("", ("img/x.png",))
        errors = check_assets.check(root)
        self.assertTrue(any("unregistered asset: img/x.png" in e for e in errors))

    def test_registered_but_missing_file_is_reported(self):
        row = "img/gone.png,Gone,https://example.org,CC0-1.0,,no,2026-11-06,\n"
        errors = check_assets.check(self.make_dir(row))
        self.assertTrue(any("does not exist" in e for e in errors))

    def test_noncommercial_license_is_rejected(self):
        row = "img/n.png,N,https://example.org,CC-BY-NC-4.0,A,yes,2026-11-06,\n"
        errors = check_assets.check(self.make_dir(row, ("img/n.png",)))
        self.assertTrue(any("not allowed for a commercial project" in e for e in errors))

    def test_unknown_license_is_rejected(self):
        row = "img/u.png,U,https://example.org,Whatever,A,yes,2026-11-06,\n"
        errors = check_assets.check(self.make_dir(row, ("img/u.png",)))
        self.assertTrue(any("unknown license" in e for e in errors))

    def test_source_required_for_third_party_assets(self):
        row = "img/s.png,S,,CC0-1.0,,no,2026-11-06,\n"
        errors = check_assets.check(self.make_dir(row, ("img/s.png",)))
        self.assertTrue(any("source_url is required" in e for e in errors))

    def test_original_work_needs_author_but_no_source(self):
        ok = "img/o.png,O,,Original,Me,no,2026-11-06,\n"
        self.assertEqual(check_assets.check(self.make_dir(ok, ("img/o.png",))), [])
        bad = "img/o.png,O,,Original,,no,2026-11-06,\n"
        errors = check_assets.check(self.make_dir(bad, ("img/o.png",)))
        self.assertTrue(any("author is required" in e for e in errors))

    def test_bad_date_is_reported(self):
        row = "img/d.png,D,https://example.org,CC0-1.0,,no,06.11.2026,\n"
        errors = check_assets.check(self.make_dir(row, ("img/d.png",)))
        self.assertTrue(any("checked_on" in e for e in errors))

    def test_duplicate_entry_is_reported(self):
        row = "img/d.png,D,https://example.org,CC0-1.0,,no,2026-11-06,\n"
        errors = check_assets.check(self.make_dir(row + row, ("img/d.png",)))
        self.assertTrue(any("duplicate entry" in e for e in errors))

    def test_backslash_path_is_rejected(self):
        row = "img\\b.png,B,https://example.org,CC0-1.0,,no,2026-11-06,\n"
        errors = check_assets.check(self.make_dir(row))
        self.assertTrue(any("forward slashes" in e for e in errors))

    def test_commercial_requires_notes(self):
        row = "img/c.png,C,,Commercial,Studio,yes,2026-11-06,\n"
        errors = check_assets.check(self.make_dir(row, ("img/c.png",)))
        self.assertTrue(any("proof of purchase" in e for e in errors))

    def test_missing_columns_are_reported(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        root = Path(tmp.name)
        (root / "LICENSES.csv").write_text("path,title\n", encoding="utf-8")
        errors = check_assets.check(root)
        self.assertTrue(any("missing columns" in e for e in errors))


if __name__ == "__main__":
    unittest.main()
