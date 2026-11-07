import contextlib
import hashlib
import importlib.util
import io
import json
import os
import sys
import tempfile
import threading
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT = REPO_ROOT / "tools" / "geo" / "fetch.py"
spec = importlib.util.spec_from_file_location("geo_fetch_under_test", SCRIPT)
geo_fetch = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = geo_fetch  # dataclasses looks the module up by name
spec.loader.exec_module(geo_fetch)

PAYLOAD = bytes(range(256)) * 40  # 10240 bytes
PAYLOAD_SHA = hashlib.sha256(PAYLOAD).hexdigest()


class FixtureServer:
    """Tiny local HTTP server (127.0.0.1) with Range support that counts requests."""

    def __init__(self, files):
        outer = self
        self.files = files
        self.requests = []  # (path, range header)

        class Handler(BaseHTTPRequestHandler):
            def do_GET(self):  # noqa: N802 (http.server API)
                outer.requests.append((self.path, self.headers.get("Range")))
                body = outer.files.get(self.path)
                if body is None:
                    self.send_error(404)
                    return
                start = 0
                range_header = self.headers.get("Range")
                if range_header:
                    start = int(range_header.split("=")[1].rstrip("-"))
                    if start >= len(body):
                        self.send_error(416)
                        return
                    self.send_response(206)
                    self.send_header("Content-Range", f"bytes {start}-{len(body) - 1}/{len(body)}")
                else:
                    self.send_response(200)
                chunk = body[start:]
                self.send_header("Content-Length", str(len(chunk)))
                self.end_headers()
                self.wfile.write(chunk)

            def log_message(self, *args):
                pass

        self.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)

    def __enter__(self):
        self.thread.start()
        return self

    def __exit__(self, *exc):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join()

    def url(self, path):
        return f"http://127.0.0.1:{self.server.server_port}{path}"


def make_source(url, sha256=PAYLOAD_SHA, size=len(PAYLOAD), path="data/dem.bin", **overrides):
    source = {
        "id": "test-dem",
        "name": "Test",
        "provider": "Tester",
        "layer": "terrain",
        "status": "unverified",
        "version": None,
        "version_date": None,
        "source_page": None,
        "files": [{"path": path, "url": url, "sha256": sha256, "size": size}],
        "license": "CC0-1.0",
        "license_url": None,
        "attribution": {"required": False, "text": None},
        "checked_on": None,
        "distribution": {"may_commit_raw": False, "may_distribute_derivatives": "unknown"},
        "transformations": [],
        "notes": "",
    }
    source.update(overrides)
    return source


def make_manifest(*sources):
    return {"manifest_version": 1, "sources": list(sources)}


class FetchTestCase(unittest.TestCase):
    def make_cache(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        return Path(tmp.name).resolve() / "cache"

    def fetch(self, manifest, cache, **kwargs):
        return geo_fetch.fetch_sources(manifest, cache, **kwargs)


class FetchOverHttpTest(FetchTestCase):
    def setUp(self):
        self.server = FixtureServer({"/dem.bin": PAYLOAD})
        self.server.__enter__()
        self.addCleanup(self.server.__exit__, None, None, None)

    def manifest(self, **kwargs):
        return make_manifest(make_source(self.server.url("/dem.bin"), **kwargs))

    def test_downloads_and_verifies(self):
        cache = self.make_cache()
        results = self.fetch(self.manifest(), cache)
        self.assertEqual([r.outcome for r in results], ["downloaded"])
        self.assertEqual((cache / "test-dem" / "data" / "dem.bin").read_bytes(), PAYLOAD)
        self.assertFalse((cache / "test-dem" / "data" / "dem.bin.part").exists())

    def test_rerun_does_not_download_again(self):
        cache = self.make_cache()
        self.fetch(self.manifest(), cache)
        hits = len(self.server.requests)
        results = self.fetch(self.manifest(), cache)
        self.assertEqual([r.outcome for r in results], ["cached"])
        self.assertEqual(len(self.server.requests), hits)

    def test_corrupt_cached_file_is_downloaded_again(self):
        cache = self.make_cache()
        self.fetch(self.manifest(), cache)
        target = cache / "test-dem" / "data" / "dem.bin"
        target.write_bytes(b"x" * len(PAYLOAD))  # right size, wrong content
        results = self.fetch(self.manifest(), cache)
        self.assertEqual([r.outcome for r in results], ["downloaded"])
        self.assertEqual(target.read_bytes(), PAYLOAD)

    def test_hash_mismatch_fails_and_leaves_nothing(self):
        cache = self.make_cache()
        results = self.fetch(self.manifest(sha256="0" * 64), cache)
        self.assertEqual(results[0].outcome, "failed")
        self.assertIn("sha256 mismatch", results[0].message)
        self.assertFalse((cache / "test-dem" / "data" / "dem.bin").exists())
        self.assertFalse((cache / "test-dem" / "data" / "dem.bin.part").exists())

    def test_size_mismatch_fails(self):
        results = self.fetch(self.manifest(size=len(PAYLOAD) + 1), self.make_cache())
        self.assertEqual(results[0].outcome, "failed")
        self.assertIn("size mismatch", results[0].message)

    def test_null_hash_is_refused_without_flag(self):
        cache = self.make_cache()
        results = self.fetch(self.manifest(sha256=None, size=None), cache)
        self.assertEqual(results[0].outcome, "refused")
        self.assertEqual(self.server.requests, [])
        self.assertFalse(cache.exists())

    def test_allow_unverified_reports_computed_hash(self):
        cache = self.make_cache()
        results = self.fetch(self.manifest(sha256=None, size=None), cache, allow_unverified=True)
        self.assertEqual(results[0].outcome, "downloaded")
        self.assertEqual(results[0].sha256, PAYLOAD_SHA)
        self.assertEqual(results[0].size, len(PAYLOAD))
        self.assertTrue(results[0].needs_recording)
        # A second run reuses the file and still reports the hash to record.
        hits = len(self.server.requests)
        again = self.fetch(self.manifest(sha256=None, size=None), cache, allow_unverified=True)
        self.assertEqual(again[0].outcome, "present")
        self.assertEqual(again[0].sha256, PAYLOAD_SHA)
        self.assertEqual(len(self.server.requests), hits)

    def test_dry_run_touches_nothing(self):
        cache = self.make_cache()
        results = self.fetch(self.manifest(), cache, dry_run=True)
        self.assertEqual([r.outcome for r in results], ["would-download"])
        self.assertEqual(self.server.requests, [])
        self.assertFalse(cache.exists())

    def test_dry_run_recognises_cached_files(self):
        cache = self.make_cache()
        self.fetch(self.manifest(), cache)
        hits = len(self.server.requests)
        results = self.fetch(self.manifest(), cache, dry_run=True)
        self.assertEqual([r.outcome for r in results], ["cached"])
        self.assertEqual(len(self.server.requests), hits)

    def test_interrupted_download_is_resumed_with_range(self):
        cache = self.make_cache()
        part = cache / "test-dem" / "data" / "dem.bin.part"
        part.parent.mkdir(parents=True)
        part.write_bytes(PAYLOAD[:4000])
        results = self.fetch(self.manifest(), cache)
        self.assertEqual(results[0].outcome, "downloaded")
        self.assertEqual(self.server.requests, [("/dem.bin", "bytes=4000-")])
        self.assertEqual((cache / "test-dem" / "data" / "dem.bin").read_bytes(), PAYLOAD)

    def test_garbage_part_file_is_discarded_after_failed_verification(self):
        cache = self.make_cache()
        part = cache / "test-dem" / "data" / "dem.bin.part"
        part.parent.mkdir(parents=True)
        part.write_bytes(b"garbage" * 10)
        results = self.fetch(self.manifest(), cache)
        self.assertEqual(results[0].outcome, "failed")  # resumed garbage does not match the hash
        self.assertFalse(part.exists())
        retry = self.fetch(self.manifest(), cache)
        self.assertEqual(retry[0].outcome, "downloaded")

    def test_http_error_is_reported(self):
        source = make_source(self.server.url("/missing.bin"))
        results = self.fetch(make_manifest(source), self.make_cache())
        self.assertEqual(results[0].outcome, "failed")
        self.assertIn("download error", results[0].message)


class FetchPolicyTest(FetchTestCase):
    def test_blocked_and_rejected_sources_are_never_fetched(self):
        calls = []

        def downloader(*args):
            calls.append(args)

        sources = [
            make_source("https://example.org/a", id="blocked", status="blocked-on-owner", decision_issue=96),
            make_source("https://example.org/b", id="rejected", status="rejected"),
        ]
        results = self.fetch(make_manifest(*sources), self.make_cache(), allow_unverified=True, downloader=downloader)
        self.assertEqual([r.outcome for r in results], ["skipped", "skipped"])
        self.assertIn("#96", results[0].message)
        self.assertEqual(calls, [])

    def test_source_without_files_is_skipped(self):
        source = make_source("https://example.org/a", files=[])
        results = self.fetch(make_manifest(source), self.make_cache())
        self.assertEqual(results[0].outcome, "skipped")

    def test_only_filter(self):
        sources = [make_source("https://example.org/a", id="one"), make_source("https://example.org/b", id="two")]
        results = self.fetch(make_manifest(*sources), self.make_cache(), only=["two"], dry_run=True)
        self.assertEqual([r.source_id for r in results], ["two"])

    def test_file_url_fixture(self):
        cache = self.make_cache()
        fixture = cache.parent / "fixture.bin"
        fixture.write_bytes(PAYLOAD)
        source = make_source(fixture.as_uri())
        results = self.fetch(make_manifest(source), cache)
        self.assertEqual([r.outcome for r in results], ["downloaded"])
        self.assertEqual((cache / "test-dem" / "data" / "dem.bin").read_bytes(), PAYLOAD)

    def test_unsupported_scheme_is_rejected_by_downloader(self):
        with self.assertRaises(ValueError):
            geo_fetch.http_download("ftp://example.org/x", Path("unused"), 0, 1.0)

    def test_path_traversal_cannot_escape_the_cache(self):
        source = make_source("https://example.org/a", path="../escape.bin")
        results = self.fetch(make_manifest(source), self.make_cache(), dry_run=True)
        self.assertEqual(results[0].outcome, "failed")


class CacheLocationTest(unittest.TestCase):
    def test_env_variable_selects_the_cache(self):
        self.assertEqual(geo_fetch.default_cache_dir({"PATRIARCH_GEO_CACHE": "/some/where"}), Path("/some/where"))

    def test_default_is_under_the_home_directory(self):
        default = geo_fetch.default_cache_dir({})
        self.assertIn(Path.home(), default.parents)
        self.assertNotIn(REPO_ROOT, default.parents)

    def test_cache_inside_the_repository_is_refused(self):
        with self.assertRaises(ValueError):
            geo_fetch.resolve_cache(str(REPO_ROOT / "cache"), {})
        with self.assertRaises(ValueError):
            geo_fetch.resolve_cache(str(REPO_ROOT), {})

    def test_cache_outside_the_repository_is_accepted(self):
        with tempfile.TemporaryDirectory() as tmp:
            self.assertEqual(geo_fetch.resolve_cache(tmp, {}), Path(tmp).resolve())


class CliTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name).resolve()
        self.fixture = self.root / "fixture.bin"
        self.fixture.write_bytes(PAYLOAD)

    def write_manifest(self, *sources):
        manifest_dir = self.root / "manifest"
        manifest_dir.mkdir(exist_ok=True)
        path = manifest_dir / "geodata.manifest.json"
        path.write_text(json.dumps(make_manifest(*sources)), encoding="utf-8")
        return path

    def run_cli(self, *args, env=None):
        out, err = io.StringIO(), io.StringIO()
        old = os.environ.get("PATRIARCH_GEO_CACHE")
        try:
            if env is not None:
                os.environ["PATRIARCH_GEO_CACHE"] = env
            with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
                code = geo_fetch.main(list(args))
        finally:
            if old is None:
                os.environ.pop("PATRIARCH_GEO_CACHE", None)
            else:
                os.environ["PATRIARCH_GEO_CACHE"] = old
        return code, out.getvalue(), err.getvalue()

    def test_end_to_end_with_env_cache_and_rerun(self):
        manifest = self.write_manifest(make_source(self.fixture.as_uri()))
        cache = self.root / "cache"
        args = ("--manifest", str(manifest), "--allow-file-urls")
        code, out, _ = self.run_cli(*args, env=str(cache))
        self.assertEqual(code, 0)
        self.assertIn("[downloaded]", out)
        code, out, _ = self.run_cli(*args, env=str(cache))
        self.assertEqual(code, 0)
        self.assertIn("[cached]", out)

    def test_allow_unverified_prints_values_to_record(self):
        manifest = self.write_manifest(make_source(self.fixture.as_uri(), sha256=None, size=None))
        args = ("--manifest", str(manifest), "--cache", str(self.root / "cache"), "--allow-file-urls")
        code, out, _ = self.run_cli(*args)
        self.assertEqual(code, 1)  # refused without the flag
        code, out, _ = self.run_cli(*args, "--allow-unverified")
        self.assertEqual(code, 0)
        self.assertIn(f'"sha256": "{PAYLOAD_SHA}", "size": {len(PAYLOAD)}', out)

    def test_dry_run_creates_no_cache(self):
        manifest = self.write_manifest(make_source(self.fixture.as_uri()))
        cache = self.root / "cache"
        code, out, _ = self.run_cli("--manifest", str(manifest), "--cache", str(cache), "--allow-file-urls", "--dry-run")
        self.assertEqual(code, 0)
        self.assertIn("[would-download]", out)
        self.assertFalse(cache.exists())

    def test_invalid_manifest_and_unknown_id_and_repo_cache_exit_2(self):
        manifest = self.write_manifest(make_source("https://example.org/a", license="CC-BY-NC-4.0"))
        code, _, err = self.run_cli("--manifest", str(manifest), "--cache", str(self.root / "c"))
        self.assertEqual(code, 2)
        self.assertIn("not allowed for a commercial project", err)

        manifest = self.write_manifest(make_source("https://example.org/a"))
        code, _, err = self.run_cli("--manifest", str(manifest), "--cache", str(self.root / "c"), "--only", "nope")
        self.assertEqual(code, 2)
        self.assertIn("unknown source id", err)

        code, _, err = self.run_cli("--manifest", str(manifest), "--cache", str(REPO_ROOT / "cache"))
        self.assertEqual(code, 2)
        self.assertIn("outside the repository", err)

    def test_committed_manifest_is_a_no_op_dry_run(self):
        code, out, _ = self.run_cli("--cache", str(self.root / "cache"), "--dry-run")
        self.assertEqual(code, 0)
        self.assertFalse((self.root / "cache").exists())
        self.assertIn("blocked-on-owner", out)


if __name__ == "__main__":
    unittest.main()
