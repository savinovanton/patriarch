import contextlib
import copy
import importlib.util
import io
import json
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT = REPO_ROOT / "tools" / "geo" / "geo_manifest.py"
spec = importlib.util.spec_from_file_location("geo_manifest_under_test", SCRIPT)
geo_manifest = importlib.util.module_from_spec(spec)
spec.loader.exec_module(geo_manifest)

HASH_A = "a" * 64


def unverified_entry(**overrides):
    entry = {
        "id": "dem-a",
        "name": "Test DEM",
        "provider": "Tester",
        "layer": "terrain",
        "status": "unverified",
        "version": None,
        "version_date": None,
        "source_page": None,
        "files": [],
        "license": "CC0-1.0",
        "license_url": None,
        "attribution": {"required": False, "text": None},
        "checked_on": None,
        "distribution": {"may_commit_raw": False, "may_distribute_derivatives": "unknown"},
        "transformations": [],
        "notes": "",
    }
    entry.update(overrides)
    return entry


def ready_entry(**overrides):
    entry = unverified_entry(
        status="ready",
        version="1.0",
        files=[{"path": "a/dem.tif", "url": "https://example.org/dem.tif", "sha256": HASH_A, "size": 10}],
        license_url="https://example.org/license",
        checked_on="2026-11-06",
        distribution={"may_commit_raw": False, "may_distribute_derivatives": "yes"},
    )
    entry.update(overrides)
    return entry


def manifest(*entries):
    return {"manifest_version": 1, "sources": list(entries)}


class ValidateManifestTest(unittest.TestCase):
    def errors(self, *entries, **kwargs):
        return geo_manifest.validate_manifest(manifest(*entries), **kwargs)

    def assertHasError(self, errors, fragment):
        self.assertTrue(any(fragment in e for e in errors), f"{fragment!r} not found in {errors}")

    def test_empty_manifest_is_valid(self):
        self.assertEqual(self.errors(), [])

    def test_unverified_and_ready_entries_are_valid(self):
        self.assertEqual(self.errors(unverified_entry(), ready_entry(id="dem-b")), [])

    def test_unsupported_manifest_version(self):
        errors = geo_manifest.validate_manifest({"manifest_version": 2, "sources": []})
        self.assertHasError(errors, "manifest_version must be 1")

    def test_top_level_must_be_object(self):
        self.assertHasError(geo_manifest.validate_manifest([]), "top level")

    def test_unknown_top_level_key(self):
        data = manifest()
        data["extra"] = 1
        self.assertHasError(geo_manifest.validate_manifest(data), "unknown top-level keys")

    def test_duplicate_ids(self):
        self.assertHasError(self.errors(unverified_entry(), unverified_entry()), "duplicate id")

    def test_bad_id(self):
        self.assertHasError(self.errors(unverified_entry(id="Bad_ID")), "id must match")

    def test_missing_key_requires_explicit_null(self):
        entry = unverified_entry()
        del entry["checked_on"]
        self.assertHasError(self.errors(entry), "missing keys: checked_on")

    def test_unknown_entry_key(self):
        self.assertHasError(self.errors(unverified_entry(typo=1)), "unknown keys: typo")

    def test_unknown_status_and_layer(self):
        errors = self.errors(unverified_entry(status="maybe", layer="moon"))
        self.assertHasError(errors, "status must be one of")
        self.assertHasError(errors, "layer must be one of")

    def test_noncommercial_license_is_rejected(self):
        for lic in ("CC-BY-NC-4.0", "CC-BY-ND-4.0", "LicenseRef-Foo-NC"):
            self.assertHasError(self.errors(unverified_entry(license=lic)), "not allowed for a commercial project")

    def test_unknown_license_is_rejected(self):
        self.assertHasError(self.errors(unverified_entry(license="Whatever")), "unknown license")

    def test_custom_license_ref_needs_explicit_attribution(self):
        entry = unverified_entry(license="LicenseRef-Custom", attribution={"required": None, "text": None})
        self.assertHasError(self.errors(entry), "explicit decision")
        entry = unverified_entry(license="LicenseRef-Custom", attribution={"required": True, "text": "credit"})
        self.assertEqual(self.errors(entry), [])

    def test_cc_by_requires_attribution(self):
        entry = unverified_entry(license="CC-BY-4.0", attribution={"required": False, "text": None})
        self.assertHasError(self.errors(entry), "must be true for license CC-BY-4.0")

    def test_required_attribution_needs_text(self):
        entry = unverified_entry(attribution={"required": True, "text": None})
        self.assertHasError(self.errors(entry), "text is required")

    def test_null_license_is_fine_while_unverified(self):
        entry = unverified_entry(license=None)
        self.assertEqual(self.errors(entry), [])

    def test_hash_must_be_hex_or_null(self):
        for bad in ("abc", "g" * 64, "a" * 63, 5):
            files = [{"path": "x.bin", "url": None, "sha256": bad, "size": None}]
            self.assertHasError(self.errors(unverified_entry(files=files)), "sha256 must be 64 hex")
        files = [{"path": "x.bin", "url": None, "sha256": None, "size": None}]
        self.assertEqual(self.errors(unverified_entry(files=files)), [])

    def test_size_must_be_non_negative_int(self):
        for bad in (-1, "5", 1.5, True):
            files = [{"path": "x.bin", "url": None, "sha256": None, "size": bad}]
            self.assertHasError(self.errors(unverified_entry(files=files)), "size must be")

    def test_file_keys_must_be_explicit(self):
        files = [{"path": "x.bin"}]
        self.assertHasError(self.errors(unverified_entry(files=files)), "missing keys: sha256, size, url")

    def test_file_paths_must_be_safe_and_unique(self):
        for bad in ("../x", "/abs", "a\\b", "C:/x"):
            files = [{"path": bad, "url": None, "sha256": None, "size": None}]
            self.assertHasError(self.errors(unverified_entry(files=files)), "path must be relative")
        item = {"path": "x.bin", "url": None, "sha256": None, "size": None}
        self.assertHasError(self.errors(unverified_entry(files=[item, dict(item)])), "duplicate path")

    def test_urls_must_be_https(self):
        for bad in ("http://example.org/x", "ftp://example.org/x", "file:///tmp/x", "not a url"):
            files = [{"path": "x.bin", "url": bad, "sha256": None, "size": None}]
            self.assertHasError(self.errors(unverified_entry(files=files)), "URL must be https")
        for good in ("https://example.org/x", "http://127.0.0.1:8000/x"):
            files = [{"path": "x.bin", "url": good, "sha256": None, "size": None}]
            self.assertEqual(self.errors(unverified_entry(files=files)), [])

    def test_file_urls_only_when_allowed(self):
        files = [{"path": "x.bin", "url": "file:///tmp/x", "sha256": None, "size": None}]
        entry = unverified_entry(files=files)
        self.assertEqual(self.errors(entry, allow_file_urls=True), [])

    def test_bad_dates(self):
        errors = self.errors(unverified_entry(checked_on="06.11.2026", version_date="2026-13-01"))
        self.assertHasError(errors, "checked_on: must be YYYY-MM-DD")
        self.assertHasError(errors, "version_date: must be YYYY-MM-DD")

    def test_raw_data_must_not_be_committable(self):
        entry = unverified_entry(distribution={"may_commit_raw": True, "may_distribute_derivatives": "yes"})
        self.assertHasError(self.errors(entry), "may_commit_raw must be false")

    def test_distribution_keys_are_required(self):
        entry = unverified_entry(distribution={"may_commit_raw": False})
        self.assertHasError(self.errors(entry), "missing keys: may_distribute_derivatives")

    def test_unverified_data_cannot_be_ready(self):
        files = [{"path": "x.bin", "url": "https://example.org/x", "sha256": None, "size": None}]
        errors = self.errors(ready_entry(files=files))
        self.assertHasError(errors, "sha256 is null but the source is marked ready")
        self.assertHasError(errors, "size is null but the source is marked ready")

    def test_ready_needs_files_license_and_check_date(self):
        errors = self.errors(ready_entry(files=[], license=None, license_url=None, checked_on=None))
        self.assertHasError(errors, "at least one file")
        self.assertHasError(errors, "needs a license")
        self.assertHasError(errors, "license_url")
        self.assertHasError(errors, "checked_on")

    def test_ready_needs_known_derivative_rights_and_attribution_decision(self):
        entry = ready_entry(
            distribution={"may_commit_raw": False, "may_distribute_derivatives": "unknown"},
            attribution={"required": None, "text": None},
        )
        errors = self.errors(entry)
        self.assertHasError(errors, "'unknown' but the source is marked ready")
        self.assertHasError(errors, "explicit decision")

    def test_blocked_on_owner_needs_decision_issue_and_notes(self):
        errors = self.errors(unverified_entry(status="blocked-on-owner"))
        self.assertHasError(errors, "must reference its decision_issue")
        self.assertHasError(errors, "notes must explain")
        entry = unverified_entry(status="blocked-on-owner", decision_issue=96, notes="owner decision pending")
        self.assertEqual(self.errors(entry), [])

    def test_decision_issue_must_be_positive_int(self):
        self.assertHasError(self.errors(unverified_entry(decision_issue="96")), "decision_issue must be")

    def test_lists_must_hold_strings(self):
        self.assertHasError(self.errors(unverified_entry(transformations=[1])), "transformations must be")
        self.assertHasError(self.errors(unverified_entry(components="x")), "components must be")


class CheckTreeTest(unittest.TestCase):
    def make_dir(self, extra_files=()):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        root = Path(tmp.name)
        (root / "geodata.manifest.json").write_text(json.dumps(manifest(unverified_entry())), encoding="utf-8")
        (root / "README.md").write_text("readme", encoding="utf-8")
        for name in extra_files:
            target = root / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(b"raw")
        return root

    def test_manifest_and_readme_only_is_ok(self):
        root = self.make_dir()
        self.assertEqual(geo_manifest.check(root / "geodata.manifest.json"), [])

    def test_raw_data_next_to_manifest_is_reported(self):
        root = self.make_dir(("dem.tif", "sub/ne_10m_land.shp"))
        errors = geo_manifest.check(root / "geodata.manifest.json")
        self.assertEqual(len([e for e in errors if "raw third-party data" in e]), 2)

    def test_tree_check_can_be_disabled(self):
        root = self.make_dir(("dem.tif",))
        self.assertEqual(geo_manifest.check(root / "geodata.manifest.json", tree_check=False), [])

    def test_missing_and_malformed_files(self):
        root = self.make_dir()
        self.assertTrue(geo_manifest.check(root / "nope.json"))
        bad = root / "bad.manifest.json"
        bad.write_text("{not json", encoding="utf-8")
        self.assertTrue(any("cannot read JSON" in e for e in geo_manifest.check(bad)))


class RepositoryManifestTest(unittest.TestCase):
    """The committed manifest must stay valid and must not claim unverified data."""

    @classmethod
    def setUpClass(cls):
        cls.path = REPO_ROOT / "data-sources" / "geodata.manifest.json"
        cls.data = json.loads(cls.path.read_text(encoding="utf-8"))

    def test_committed_manifest_is_valid(self):
        self.assertEqual(geo_manifest.check(self.path), [])

    def test_owner_decisions_are_marked_blocked(self):
        by_id = {s["id"]: s for s in self.data["sources"]}
        self.assertEqual(by_id["hydrosheds-v1-family"]["status"], "blocked-on-owner")
        self.assertEqual(by_id["hydrosheds-v1-family"]["decision_issue"], 96)
        self.assertEqual(by_id["nlr-historical-maps"]["status"], "blocked-on-owner")
        self.assertEqual(by_id["nlr-historical-maps"]["decision_issue"], 97)

    def test_no_invented_hashes(self):
        for source in self.data["sources"]:
            if source["status"] != "ready":
                for item in source["files"]:
                    self.assertIsNone(item["sha256"], source["id"])


class CliTest(unittest.TestCase):
    def run_cli(self, path):
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = geo_manifest.main(["--manifest", str(path)])
        return code, out.getvalue(), err.getvalue()

    def make_manifest(self, data):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        path = Path(tmp.name) / "geodata.manifest.json"
        path.write_text(json.dumps(data), encoding="utf-8")
        return path

    def test_exit_code_zero_for_valid(self):
        code, out, _ = self.run_cli(self.make_manifest(manifest(unverified_entry())))
        self.assertEqual(code, 0)
        self.assertIn("geodata manifest OK", out)

    def test_exit_code_one_with_one_message_per_problem(self):
        broken = copy.deepcopy(manifest(unverified_entry(license="Whatever"), unverified_entry()))
        broken["sources"][1]["id"] = "dem-a"  # duplicate of the first entry
        code, _, err = self.run_cli(self.make_manifest(broken))
        self.assertEqual(code, 1)
        lines = [line for line in err.splitlines() if line.startswith("ERROR:")]
        self.assertEqual(len(lines), 2)
        self.assertIn("2 problem(s) found", err)


if __name__ == "__main__":
    unittest.main()
