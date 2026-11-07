#!/usr/bin/env python3
"""Download the geodata sources of the manifest into a local cache and verify them.

The cache lives OUTSIDE the repository (raw third-party data is never committed):
  * `--cache DIR`, else the PATRIARCH_GEO_CACHE environment variable,
    else `~/.patriarch/geodata-cache`;
  * layout: <cache>/<source id>/<file path>.

Behaviour:
  * a file that is already present with the expected SHA-256 (and size) is skipped,
    so a re-run downloads nothing;
  * interrupted downloads are kept as `<file>.part` and resumed with an HTTP Range
    request when the server supports it; a finished download is verified and only
    then moved into place, a corrupt one is deleted;
  * files whose sha256 is null are refused unless `--allow-unverified` is given; then the
    computed sha256 and size are printed so the owner can record them in the manifest;
  * `blocked-on-owner` and `rejected` sources are never downloaded;
  * `--dry-run` shows the plan without touching the network or the disk.

This script is never run by CI. Usage:
    python tools/geo/fetch.py [--manifest M] [--cache DIR] [--only ID ...]
                              [--dry-run] [--allow-unverified]
Exit code: 0 on success, 1 if any file failed or was refused, 2 on bad arguments or manifest.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import shutil
import sys
import urllib.error
import urllib.request
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Mapping
from urllib.parse import urlsplit

_HERE = Path(__file__).resolve().parent
if str(_HERE) not in sys.path:
    sys.path.insert(0, str(_HERE))

import geo_manifest  # noqa: E402

REPO_ROOT = geo_manifest.REPO_ROOT
CACHE_ENV = "PATRIARCH_GEO_CACHE"
USER_AGENT = "patriarch-geo-fetch/1"
CHUNK = 1 << 20
SKIPPED_STATUSES = {"blocked-on-owner", "rejected"}

# downloader(url, part_path, resume_from, timeout): append/write the body of `url` into
# `part_path`, resuming from byte `resume_from` when the server allows it.
Downloader = Callable[[str, Path, int, float], None]


@dataclass
class FileResult:
    source_id: str
    path: str
    outcome: str  # cached | downloaded | present | would-download | refused | skipped | failed
    message: str = ""
    sha256: str | None = None
    size: int | None = None
    needs_recording: bool = False  # computed hash of a file whose manifest sha256 is null

    @property
    def is_problem(self) -> bool:
        return self.outcome in {"refused", "failed"}


def default_cache_dir(env: Mapping[str, str] | None = None) -> Path:
    env = os.environ if env is None else env
    value = env.get(CACHE_ENV)
    if value:
        return Path(value).expanduser()
    return Path.home() / ".patriarch" / "geodata-cache"


def resolve_cache(arg: str | None, env: Mapping[str, str] | None = None, repo_root: Path = REPO_ROOT) -> Path:
    """Pick the cache directory and refuse locations inside the repository."""
    cache = (Path(arg).expanduser() if arg else default_cache_dir(env)).resolve()
    root = repo_root.resolve()
    if cache == root or root in cache.parents:
        raise ValueError(f"cache directory must be outside the repository ({root}): {cache}")
    return cache


def sha256_file(path: Path) -> tuple[str, int]:
    digest = hashlib.sha256()
    size = 0
    with path.open("rb") as handle:
        while True:
            chunk = handle.read(CHUNK)
            if not chunk:
                break
            digest.update(chunk)
            size += len(chunk)
    return digest.hexdigest(), size


def http_download(url: str, part: Path, resume_from: int, timeout: float) -> None:
    """Fetch `url` into `part` (the only place that touches the network)."""
    scheme = urlsplit(url).scheme
    if scheme not in {"http", "https", "file"}:
        raise ValueError(f"unsupported URL scheme: {scheme!r}")
    headers = {"User-Agent": USER_AGENT}
    if resume_from > 0 and scheme != "file":
        headers["Range"] = f"bytes={resume_from}-"
    request = urllib.request.Request(url, headers=headers)
    try:
        response = urllib.request.urlopen(request, timeout=timeout)  # noqa: S310 (scheme checked above)
    except urllib.error.HTTPError as exc:
        exc.close()
        if exc.code == 416 and resume_from > 0:  # the .part is not a valid prefix: start over
            part.unlink(missing_ok=True)
            http_download(url, part, 0, timeout)
            return
        raise
    with response:
        resumed = resume_from > 0 and getattr(response, "status", None) == 206
        with part.open("ab" if resumed else "wb") as handle:
            shutil.copyfileobj(response, handle, CHUNK)


def _fetch_file(
    source: dict,
    item: dict,
    cache: Path,
    *,
    dry_run: bool,
    allow_unverified: bool,
    downloader: Downloader,
    timeout: float,
) -> FileResult:
    source_id = source["id"]
    rel = item["path"]
    expected_hash = item["sha256"].lower() if item["sha256"] else None
    expected_size = item["size"]

    dest = (cache / source_id / rel).resolve()
    if (cache / source_id).resolve() not in dest.parents:
        return FileResult(source_id, rel, "failed", "path escapes the cache directory")
    if expected_hash is None and not allow_unverified:
        return FileResult(source_id, rel, "refused", "sha256 is null in the manifest (use --allow-unverified)")
    if item["url"] is None:
        return FileResult(source_id, rel, "refused", "url is null in the manifest")

    if dest.is_file():
        size_matches = expected_size is None or dest.stat().st_size == expected_size
        if size_matches:
            digest, size = sha256_file(dest)
            if expected_hash is None:
                return FileResult(source_id, rel, "present", "already in the cache, hash not verified", digest, size, True)
            if digest == expected_hash:
                return FileResult(source_id, rel, "cached", "already in the cache, hash verified", digest, size)
        if dry_run:
            return FileResult(source_id, rel, "would-download", "cached file does not match, would re-download")
        dest.unlink()
    elif dry_run:
        return FileResult(source_id, rel, "would-download", f"{item['url']} -> {dest}")

    part = dest.with_name(dest.name + ".part")
    dest.parent.mkdir(parents=True, exist_ok=True)
    resume_from = part.stat().st_size if part.is_file() else 0
    if expected_size is not None and resume_from > expected_size:
        part.unlink()
        resume_from = 0
    try:
        if expected_size is None or resume_from < expected_size:
            downloader(item["url"], part, resume_from, timeout)
    except (OSError, ValueError) as exc:  # URLError and HTTPError are OSError; keep the .part to resume
        return FileResult(source_id, rel, "failed", f"download error: {exc}")

    digest, size = sha256_file(part)
    if expected_size is not None and size != expected_size:
        part.unlink()
        return FileResult(source_id, rel, "failed", f"size mismatch: expected {expected_size}, got {size}")
    if expected_hash is not None and digest != expected_hash:
        part.unlink()
        return FileResult(source_id, rel, "failed", f"sha256 mismatch: expected {expected_hash}, got {digest}")
    os.replace(part, dest)
    return FileResult(source_id, rel, "downloaded", f"saved to {dest}", digest, size, expected_hash is None)


def fetch_sources(
    manifest: dict,
    cache: Path,
    *,
    only: list[str] | None = None,
    dry_run: bool = False,
    allow_unverified: bool = False,
    downloader: Downloader = http_download,
    timeout: float = 60.0,
) -> list[FileResult]:
    results: list[FileResult] = []
    for source in manifest["sources"]:
        if only and source["id"] not in only:
            continue
        if source["status"] in SKIPPED_STATUSES:
            reason = f"status is {source['status']}"
            if source.get("decision_issue"):
                reason += f" (issue #{source['decision_issue']})"
            results.append(FileResult(source["id"], "-", "skipped", reason))
            continue
        if not source["files"]:
            results.append(FileResult(source["id"], "-", "skipped", "no files listed in the manifest yet"))
            continue
        for item in source["files"]:
            results.append(
                _fetch_file(
                    source,
                    item,
                    cache,
                    dry_run=dry_run,
                    allow_unverified=allow_unverified,
                    downloader=downloader,
                    timeout=timeout,
                )
            )
    return results


def format_result(result: FileResult) -> str:
    return f"[{result.outcome}] {result.source_id}/{result.path}: {result.message}"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--manifest", default=str(geo_manifest.DEFAULT_MANIFEST))
    parser.add_argument("--cache", help=f"cache directory outside the repository (default: ${CACHE_ENV} or ~/.patriarch/geodata-cache)")
    parser.add_argument("--only", action="append", metavar="ID", help="fetch only this source id (repeatable)")
    parser.add_argument("--dry-run", action="store_true", help="show what would be done; no network, no writes")
    parser.add_argument("--allow-unverified", action="store_true", help="also fetch files whose sha256 is null and print the computed hash")
    parser.add_argument("--allow-file-urls", action="store_true", help="accept file:// URLs (local fixtures only)")
    parser.add_argument("--timeout", type=float, default=60.0, help="network timeout in seconds")
    args = parser.parse_args(argv)

    manifest_path = Path(args.manifest)
    data, errors = geo_manifest.load_manifest(manifest_path)
    if not errors:
        errors = geo_manifest.validate_manifest(data, allow_file_urls=args.allow_file_urls)
    if errors:
        for message in errors:
            print(f"ERROR: {message}", file=sys.stderr)
        print("Manifest is invalid; nothing was fetched.", file=sys.stderr)
        return 2

    known = {source["id"] for source in data["sources"]}
    unknown = sorted(set(args.only or []) - known)
    if unknown:
        print(f"ERROR: unknown source id(s): {', '.join(unknown)}", file=sys.stderr)
        return 2

    try:
        cache = resolve_cache(args.cache)
    except ValueError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2

    print(f"cache: {cache}{' (dry run)' if args.dry_run else ''}")
    results = fetch_sources(
        data,
        cache,
        only=args.only,
        dry_run=args.dry_run,
        allow_unverified=args.allow_unverified,
        timeout=args.timeout,
    )
    for result in results:
        print(format_result(result))

    to_record = [r for r in results if r.needs_recording]
    if to_record:
        print("\nComputed values for manifest entries with null sha256 (verify the source, then record them):")
        for r in to_record:
            print(f'  {r.source_id}/{r.path}: "sha256": "{r.sha256}", "size": {r.size}')

    problems = [r for r in results if r.is_problem]
    if problems:
        print(f"\n{len(problems)} problem(s).", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
