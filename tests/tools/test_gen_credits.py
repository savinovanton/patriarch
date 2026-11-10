import importlib.util
import json
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parents[2] / "tools" / "gen_credits.py"
spec = importlib.util.spec_from_file_location("gen_credits", SCRIPT)
gen_credits = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gen_credits)

REPO_ROOT = SCRIPT.parents[1]
CSV_HEADER = "path,title,source_url,license,author,attribution_required,checked_on,notes\n"


def source(source_id, required, text="Credit text", name="Name"):
    return {
        "id": source_id,
        "name": name,
        "provider": "Provider",
        "license": "CC-BY-4.0",
        "attribution": {"required": required, "text": text if required else None},
    }


class GenCreditsTest(unittest.TestCase):
    def make_root(self, sources=(), csv_rows="", credits="", loc="") -> Path:
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        root = Path(tmp.name)
        (root / "data-sources").mkdir()
        (root / "data-sources" / "geodata.manifest.json").write_text(
            json.dumps({"manifest_version": 1, "sources": list(sources)}), encoding="utf-8"
        )
        (root / "assets").mkdir()
        (root / "assets" / "LICENSES.csv").write_text(CSV_HEADER + csv_rows, encoding="utf-8")
        (root / "data" / "credits").mkdir(parents=True)
        (root / "data" / "credits" / "credits.pd").write_text("format_version = 1\n" + credits, encoding="utf-8")
        (root / "data" / "loc" / "ru").mkdir(parents=True)
        (root / "data" / "loc" / "ru" / "credits.pd").write_text("format_version = 1\n" + loc, encoding="utf-8")
        return root

    def write(self, root: Path) -> None:
        self.assertEqual(gen_credits.main(["--root", str(root)]), 0)

    def check(self, root: Path) -> list[str]:
        return gen_credits.check(root)

    def test_every_source_with_attribution_gets_an_entry_and_others_do_not(self):
        root = self.make_root([source("a-src", True), source("b-src", False)])
        self.write(root)
        text = (root / "data" / "credits" / "generated.pd").read_text(encoding="utf-8")
        self.assertIn('credit "source_a_src"', text)
        self.assertIn('source = "a-src"', text)
        self.assertNotIn("b-src", text)
        self.assertEqual(self.check(root), [])

    def test_generated_text_is_verbatim_and_escaped(self):
        root = self.make_root([source("q", True, text='He said "hi" \\ done', name="N")])
        self.write(root)
        credits = gen_credits.parse_credits((root / "data" / "credits" / "generated.pd").read_text(encoding="utf-8"))
        self.assertEqual(credits[0].fields["text"], 'He said "hi" \\ done')

    def test_asset_row_with_attribution_gets_an_entry(self):
        rows = "ui/a.png,Picture A,https://example.org,CC-BY-4.0,Jane Doe,yes,2026-11-10,\n"
        rows += "ui/b.png,Picture B,,Original,Owner,no,2026-11-10,\n"
        root = self.make_root(csv_rows=rows)
        self.write(root)
        credits = gen_credits.parse_credits((root / "data" / "credits" / "generated.pd").read_text(encoding="utf-8"))
        self.assertEqual([c.fields["asset"] for c in credits], ["ui/a.png"])
        self.assertEqual(credits[0].fields["provider"], "Jane Doe")
        self.assertEqual(credits[0].fields["section"], "assets")
        self.assertEqual(self.check(root), [])

    def test_stale_generated_file_is_reported(self):
        root = self.make_root([source("a", True)])
        self.write(root)
        manifest = root / "data-sources" / "geodata.manifest.json"
        manifest.write_text(json.dumps({"manifest_version": 1, "sources": [source("a", True), source("b", True)]}))
        errors = self.check(root)
        self.assertTrue(any("out of date" in e for e in errors))

    def test_missing_generated_file_is_reported(self):
        root = self.make_root([source("a", True)])
        self.assertTrue(any("out of date" in e for e in self.check(root)))

    def test_missing_entry_for_a_required_source_is_reported(self):
        root = self.make_root([source("a", True)])
        self.write(root)
        generated = root / "data" / "credits" / "generated.pd"
        generated.write_text("format_version = 1\n", encoding="utf-8")
        errors = self.check(root)
        self.assertTrue(any("source 'a' requires attribution but has no credits entry" in e for e in errors))

    def test_missing_entry_for_a_required_asset_is_reported(self):
        rows = "ui/a.png,Picture A,https://example.org,CC-BY-4.0,Jane,yes,2026-11-10,\n"
        root = self.make_root(csv_rows=rows)
        self.write(root)
        (root / "data" / "credits" / "generated.pd").write_text("format_version = 1\n", encoding="utf-8")
        self.assertTrue(any("asset 'ui/a.png' requires attribution" in e for e in self.check(root)))

    def test_handwritten_entry_covers_a_source_and_is_not_duplicated(self):
        credits = 'credit "mine" {\n    section = data\n    source = "a"\n    name_key = "credits.a"\n}\n'
        loc = 'credits.a = "Credit text"\n'
        root = self.make_root([source("a", True)], credits=credits, loc=loc)
        self.write(root)
        generated = (root / "data" / "credits" / "generated.pd").read_text(encoding="utf-8")
        self.assertNotIn("credit ", generated.replace("# ", ""))  # no generated block for "a"
        self.assertEqual(self.check(root), [])

    def test_handwritten_entry_must_show_the_manifest_attribution(self):
        credits = 'credit "mine" {\n    section = data\n    source = "a"\n    name_key = "credits.a"\n}\n'
        root = self.make_root([source("a", True, text="Exact wording")], credits=credits, loc='credits.a = "Other"\n')
        self.write(root)
        self.assertTrue(any("must show the attribution text" in e for e in self.check(root)))

    def test_handwritten_entry_may_carry_the_text_in_a_text_key(self):
        credits = 'credit "mine" {\n    section = data\n    source = "a"\n    name_key = "n"\n    text_key = "t"\n}\n'
        loc = 'n = "Name"\nt = "About: Exact wording here"\n'
        root = self.make_root([source("a", True, text="Exact wording")], credits=credits, loc=loc)
        self.write(root)
        self.assertEqual(self.check(root), [])

    def test_unknown_source_asset_and_key_are_reported(self):
        credits = (
            'credit "x" {\n    section = data\n    source = "nope"\n    name_key = "missing.key"\n}\n'
            'credit "y" {\n    section = assets\n    asset = "no/file.png"\n    name = "Y"\n}\n'
        )
        root = self.make_root(credits=credits)
        self.write(root)
        errors = self.check(root)
        self.assertTrue(any("unknown source 'nope'" in e for e in errors))
        self.assertTrue(any("unknown asset 'no/file.png'" in e for e in errors))
        self.assertTrue(any("name_key 'missing.key' is not defined" in e for e in errors))

    def test_comments_and_braces_in_strings_do_not_confuse_the_parser(self):
        text = (
            '# credit "ghost" { section = data }\n'
            'credit "a" {  # trailing comment\n    section = data\n    name = "Has } brace # and hash"\n'
            '    source = "s"\n}\n'
        )
        credits = gen_credits.parse_credits(text)
        self.assertEqual([c.id for c in credits], ["a"])
        self.assertEqual(credits[0].fields["name"], "Has } brace # and hash")
        self.assertEqual(credits[0].fields["source"], "s")

    def test_the_repository_credits_are_complete_and_up_to_date(self):
        self.assertEqual(gen_credits.check(REPO_ROOT), [])

    def test_the_national_library_of_russia_is_credited_exactly(self):
        loc = gen_credits.load_loc(REPO_ROOT)
        entries = gen_credits.read_credits(REPO_ROOT / "data" / "credits" / "credits.pd")
        rnb = [e for e in entries if e.fields.get("source") == "nlr-historical-maps"]
        self.assertEqual(len(rnb), 1)
        self.assertEqual(loc[rnb[0].fields["name_key"]], "Российская национальная библиотека")


if __name__ == "__main__":
    unittest.main()
