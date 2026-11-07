#!/usr/bin/env python3
"""Validate the geodata source manifest (data-sources/geodata.manifest.json).

The manifest lists the third-party source datasets for the real-geography map
(ADR-0004). Raw datasets are NEVER committed: the repository only holds this
manifest and the scripts that download the data into a local cache outside the
repository and verify it by SHA-256 (tools/geo/fetch.py).

Checks:
  * manifest_version is supported, the schema is complete, no unknown keys;
  * source ids are unique slugs, statuses and layers are known;
  * licenses come from the asset vocabulary (assets/README.md) or are an explicit
    custom `LicenseRef-*` id; NC / ND licenses are rejected;
  * hashes are 64 hex characters or an explicit null, sizes are integers or null;
  * file paths are relative, URLs are https (http only for loopback hosts);
  * an `unverified` source cannot pretend to be `ready`: `ready` needs every file
    with url + sha256 + size, a license with its page, a check date and attribution;
  * `blocked-on-owner` sources reference the open decision issue;
  * raw data is never committed: `may_commit_raw` must be false and the manifest
    directory may only contain the manifest and README.

Usage:
    python tools/geo/geo_manifest.py [--manifest data-sources/geodata.manifest.json]
Exit code: 0 if the manifest is consistent, 1 otherwise.
"""

from __future__ import annotations

import argparse
import datetime
import importlib.util
import json
import re
import sys
from pathlib import Path
from urllib.parse import urlsplit

MANIFEST_VERSION = 1

REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_MANIFEST = REPO_ROOT / "data-sources" / "geodata.manifest.json"


def _load_asset_vocabulary() -> set[str]:
    """Reuse the allowed-license vocabulary of tools/check_assets.py (single source of truth)."""
    path = Path(__file__).resolve().parents[1] / "check_assets.py"
    spec = importlib.util.spec_from_file_location("_patriarch_check_assets", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return set(module.ALLOWED_LICENSES)


ALLOWED_LICENSES = _load_asset_vocabulary()
# Custom licenses that have no SPDX id (e.g. the Copernicus DEM licence) are written as
# `LicenseRef-<name>`; they must always carry an explicit attribution decision and, when
# the source is `ready`, a license page.
LICENSE_REF = re.compile(r"^LicenseRef-[A-Za-z0-9][A-Za-z0-9.-]*$")
NONCOMMERCIAL_OR_NODERIVS = re.compile(r"(^|-)(NC|ND)(-|$)", re.IGNORECASE)
CC_BY_FAMILY = re.compile(r"^CC-BY(-SA)?-\d")

STATUSES = {"unverified", "ready", "blocked-on-owner", "rejected"}
LAYERS = {"terrain", "coastline", "hydrography", "lakes", "historical"}
DERIVATIVES = {"yes", "no", "conditional", "unknown"}

ID_PATTERN = re.compile(r"^[a-z0-9][a-z0-9-]*$")
SHA256_PATTERN = re.compile(r"^[0-9a-fA-F]{64}$")
LOOPBACK_HOSTS = {"127.0.0.1", "localhost", "::1"}

TOP_KEYS = {"manifest_version", "description", "sources"}
REQUIRED_ENTRY_KEYS = [
    "id",
    "name",
    "provider",
    "layer",
    "status",
    "version",
    "version_date",
    "source_page",
    "files",
    "license",
    "license_url",
    "attribution",
    "checked_on",
    "distribution",
    "transformations",
    "notes",
]
OPTIONAL_ENTRY_KEYS = ["components", "decision_issue"]
FILE_KEYS = {"path", "url", "sha256", "size"}
ATTRIBUTION_KEYS = {"required", "text"}
DISTRIBUTION_KEYS = {"may_commit_raw", "may_distribute_derivatives"}

# Files that may live next to the manifest; anything else is treated as raw data.
TREE_ALLOWED_NAMES = {"README.md", ".gitkeep"}
TREE_ALLOWED_SUFFIXES = (".manifest.json",)


def _is_str(value: object) -> bool:
    return isinstance(value, str) and value.strip() != ""


def _is_int(value: object) -> bool:
    return isinstance(value, int) and not isinstance(value, bool)


def _check_date(value: object, where: str, errors: list[str]) -> None:
    if value is None:
        return
    if not isinstance(value, str):
        errors.append(f"{where}: must be YYYY-MM-DD or null, got {value!r}")
        return
    try:
        datetime.date.fromisoformat(value)
    except ValueError:
        errors.append(f"{where}: must be YYYY-MM-DD or null, got {value!r}")


def _check_url(value: object, where: str, errors: list[str], allow_file_urls: bool) -> None:
    """Validate a URL that is not null."""
    if not _is_str(value):
        errors.append(f"{where}: must be a non-empty string or null")
        return
    parts = urlsplit(value)
    if parts.scheme == "https" and parts.netloc:
        return
    if parts.scheme == "http" and parts.hostname in LOOPBACK_HOSTS:
        return
    if parts.scheme == "file" and allow_file_urls:
        return
    errors.append(f"{where}: URL must be https:// (http:// only for loopback hosts), got {value!r}")


def _check_file(
    entry_id: str, index: int, item: object, errors: list[str], seen_paths: set[str], allow_file_urls: bool
) -> None:
    where = f"source '{entry_id}' file #{index}"
    if not isinstance(item, dict):
        errors.append(f"{where}: must be an object")
        return
    missing = sorted(FILE_KEYS - item.keys())
    unknown = sorted(item.keys() - FILE_KEYS)
    if missing:
        errors.append(f"{where}: missing keys: {', '.join(missing)} (use an explicit null if unknown)")
    if unknown:
        errors.append(f"{where}: unknown keys: {', '.join(unknown)}")

    path = item.get("path")
    if not _is_str(path):
        errors.append(f"{where}: path must be a non-empty string")
    else:
        parts = Path(path).parts
        if "\\" in path or path.startswith("/") or ".." in parts or ":" in path or not parts:
            errors.append(f"{where}: path must be relative with forward slashes and no '..': {path!r}")
        elif path in seen_paths:
            errors.append(f"{where}: duplicate path {path!r}")
        seen_paths.add(path)

    if item.get("url") is not None:
        _check_url(item["url"], f"{where}: url", errors, allow_file_urls)

    digest = item.get("sha256")
    if digest is not None and not (isinstance(digest, str) and SHA256_PATTERN.match(digest)):
        errors.append(f"{where}: sha256 must be 64 hex characters or null, got {digest!r}")

    size = item.get("size")
    if size is not None and not (_is_int(size) and size >= 0):
        errors.append(f"{where}: size must be a non-negative integer or null, got {size!r}")


def _check_license(entry: dict, entry_id: str, errors: list[str]) -> None:
    license_id = entry.get("license")
    status = entry.get("status")
    if license_id is None:
        if status == "ready":
            errors.append(f"source '{entry_id}': ready source needs a license")
        return
    if not isinstance(license_id, str) or not license_id:
        errors.append(f"source '{entry_id}': license must be a string or null")
        return
    if NONCOMMERCIAL_OR_NODERIVS.search(license_id) and status != "rejected":
        errors.append(f"source '{entry_id}': license {license_id} is not allowed for a commercial project")
    elif license_id not in ALLOWED_LICENSES and not LICENSE_REF.match(license_id):
        errors.append(
            f"source '{entry_id}': unknown license '{license_id}'; use one of "
            f"{', '.join(sorted(ALLOWED_LICENSES))} or a custom 'LicenseRef-<name>' id"
        )


def _check_attribution(entry: dict, entry_id: str, errors: list[str]) -> None:
    attribution = entry.get("attribution")
    where = f"source '{entry_id}': attribution"
    if not isinstance(attribution, dict):
        errors.append(f"{where} must be an object {{required, text}}")
        return
    missing = sorted(ATTRIBUTION_KEYS - attribution.keys())
    unknown = sorted(attribution.keys() - ATTRIBUTION_KEYS)
    if missing:
        errors.append(f"{where} is missing keys: {', '.join(missing)}")
    if unknown:
        errors.append(f"{where} has unknown keys: {', '.join(unknown)}")
    required = attribution.get("required")
    text = attribution.get("text")
    if required is not None and not isinstance(required, bool):
        errors.append(f"{where}.required must be true, false or null")
    if text is not None and not _is_str(text):
        errors.append(f"{where}.text must be a non-empty string or null")

    license_id = entry.get("license")
    must_decide = entry.get("status") == "ready" or (
        isinstance(license_id, str) and (LICENSE_REF.match(license_id) or CC_BY_FAMILY.match(license_id))
    )
    if must_decide and required is None:
        errors.append(f"{where}.required must be true or false (explicit decision) for license {license_id}")
    if isinstance(license_id, str) and CC_BY_FAMILY.match(license_id) and required is False:
        errors.append(f"{where}.required must be true for license {license_id}")
    if required is True and text is None:
        errors.append(f"{where}.text is required when attribution is required")


def _check_distribution(entry: dict, entry_id: str, errors: list[str]) -> None:
    distribution = entry.get("distribution")
    where = f"source '{entry_id}': distribution"
    if not isinstance(distribution, dict):
        errors.append(f"{where} must be an object {{may_commit_raw, may_distribute_derivatives}}")
        return
    missing = sorted(DISTRIBUTION_KEYS - distribution.keys())
    unknown = sorted(distribution.keys() - DISTRIBUTION_KEYS)
    if missing:
        errors.append(f"{where} is missing keys: {', '.join(missing)}")
    if unknown:
        errors.append(f"{where} has unknown keys: {', '.join(unknown)}")
    if distribution.get("may_commit_raw") is not False:
        errors.append(f"{where}.may_commit_raw must be false: raw third-party data is never committed")
    derivatives = distribution.get("may_distribute_derivatives")
    if derivatives not in DERIVATIVES:
        errors.append(f"{where}.may_distribute_derivatives must be one of {', '.join(sorted(DERIVATIVES))}")
    elif entry.get("status") == "ready" and derivatives == "unknown":
        errors.append(f"{where}.may_distribute_derivatives is 'unknown' but the source is marked ready")


def _check_ready(entry: dict, entry_id: str, errors: list[str]) -> None:
    where = f"source '{entry_id}'"
    files = entry.get("files")
    if not isinstance(files, list) or not files:
        errors.append(f"{where}: ready source must list at least one file")
    else:
        for index, item in enumerate(files, start=1):
            if not isinstance(item, dict):
                continue
            for key in ("url", "sha256", "size"):
                if item.get(key) is None:
                    errors.append(
                        f"{where} file #{index}: {key} is null but the source is marked ready "
                        f"(an unverified source cannot be ready)"
                    )
    if not _is_str(entry.get("license_url")):
        errors.append(f"{where}: ready source needs license_url")
    if entry.get("checked_on") is None:
        errors.append(f"{where}: ready source needs checked_on (date the license was checked)")
    if not _is_str(entry.get("version")) and not _is_str(entry.get("version_date")):
        errors.append(f"{where}: ready source needs version or version_date")


def validate_entry(entry: object, index: int, allow_file_urls: bool) -> list[str]:
    errors: list[str] = []
    if not isinstance(entry, dict):
        return [f"sources[{index}]: must be an object"]

    entry_id = entry.get("id")
    label = entry_id if isinstance(entry_id, str) and entry_id else f"#{index}"
    missing = [key for key in REQUIRED_ENTRY_KEYS if key not in entry]
    unknown = sorted(set(entry) - set(REQUIRED_ENTRY_KEYS) - set(OPTIONAL_ENTRY_KEYS))
    if missing:
        errors.append(f"source '{label}': missing keys: {', '.join(missing)} (use an explicit null if unknown)")
    if unknown:
        errors.append(f"source '{label}': unknown keys: {', '.join(unknown)}")

    if not (isinstance(entry_id, str) and ID_PATTERN.match(entry_id)):
        errors.append(f"source '{label}': id must match [a-z0-9][a-z0-9-]*")
    for key in ("name", "provider"):
        if key in entry and not _is_str(entry[key]):
            errors.append(f"source '{label}': {key} must be a non-empty string")
    if "layer" in entry and entry["layer"] not in LAYERS:
        errors.append(f"source '{label}': layer must be one of {', '.join(sorted(LAYERS))}")
    status = entry.get("status")
    if "status" in entry and status not in STATUSES:
        errors.append(f"source '{label}': status must be one of {', '.join(sorted(STATUSES))}")

    for key in ("version",):
        if entry.get(key) is not None and not _is_str(entry[key]):
            errors.append(f"source '{label}': {key} must be a non-empty string or null")
    _check_date(entry.get("version_date"), f"source '{label}': version_date", errors)
    _check_date(entry.get("checked_on"), f"source '{label}': checked_on", errors)
    for key in ("source_page", "license_url"):
        if entry.get(key) is not None:
            _check_url(entry[key], f"source '{label}': {key}", errors, allow_file_urls)

    files = entry.get("files")
    if "files" in entry:
        if not isinstance(files, list):
            errors.append(f"source '{label}': files must be a list")
        else:
            seen_paths: set[str] = set()
            for number, item in enumerate(files, start=1):
                _check_file(label, number, item, errors, seen_paths, allow_file_urls)

    _check_license(entry, label, errors)
    _check_attribution(entry, label, errors)
    _check_distribution(entry, label, errors)

    for key in ("transformations", "components"):
        if key in entry:
            value = entry[key]
            if not (isinstance(value, list) and all(_is_str(item) for item in value)):
                errors.append(f"source '{label}': {key} must be a list of non-empty strings")
    if "notes" in entry and not isinstance(entry["notes"], str):
        errors.append(f"source '{label}': notes must be a string")

    issue = entry.get("decision_issue")
    if issue is not None and not (_is_int(issue) and issue > 0):
        errors.append(f"source '{label}': decision_issue must be a positive issue number")
    if status == "blocked-on-owner" and issue is None:
        errors.append(f"source '{label}': blocked-on-owner source must reference its decision_issue")
    if status in {"blocked-on-owner", "rejected"} and not _is_str(entry.get("notes")):
        errors.append(f"source '{label}': notes must explain why the source is {status}")

    if status == "ready":
        _check_ready(entry, label, errors)
    return errors


def validate_manifest(data: object, allow_file_urls: bool = False) -> list[str]:
    if not isinstance(data, dict):
        return ["manifest: top level must be a JSON object"]
    errors: list[str] = []
    version = data.get("manifest_version")
    if version != MANIFEST_VERSION or isinstance(version, bool):
        errors.append(f"manifest: manifest_version must be {MANIFEST_VERSION}, got {version!r}")
        return errors
    unknown = sorted(set(data) - TOP_KEYS)
    if unknown:
        errors.append(f"manifest: unknown top-level keys: {', '.join(unknown)}")
    sources = data.get("sources")
    if not isinstance(sources, list):
        errors.append("manifest: 'sources' must be a list")
        return errors

    seen: dict[str, int] = {}
    for index, entry in enumerate(sources, start=1):
        errors.extend(validate_entry(entry, index, allow_file_urls))
        entry_id = entry.get("id") if isinstance(entry, dict) else None
        if isinstance(entry_id, str):
            if entry_id in seen:
                errors.append(f"source '{entry_id}': duplicate id (first at position {seen[entry_id]})")
            else:
                seen[entry_id] = index
    return errors


def check_tree(directory: Path) -> list[str]:
    """Raw data must never live in the repository: only the manifest and README may sit next to it."""
    errors: list[str] = []
    for path in sorted(directory.rglob("*")):
        if not path.is_file():
            continue
        name = path.name
        if name in TREE_ALLOWED_NAMES or name.endswith(TREE_ALLOWED_SUFFIXES):
            if path.parent == directory:
                continue
        errors.append(
            f"unexpected file in {directory.name}/: {path.relative_to(directory).as_posix()} "
            f"(raw third-party data must not be committed; keep it in the local cache)"
        )
    return errors


def load_manifest(path: Path) -> tuple[object | None, list[str]]:
    if not path.is_file():
        return None, [f"manifest not found: {path}"]
    try:
        with path.open(encoding="utf-8") as handle:
            return json.load(handle), []
    except (OSError, ValueError) as exc:
        return None, [f"{path.name}: cannot read JSON: {exc}"]


def check(path: Path, allow_file_urls: bool = False, tree_check: bool = True) -> list[str]:
    data, errors = load_manifest(path)
    if errors:
        return errors
    errors = validate_manifest(data, allow_file_urls=allow_file_urls)
    if tree_check:
        errors.extend(check_tree(path.parent))
    return errors


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--manifest", default=str(DEFAULT_MANIFEST))
    parser.add_argument("--allow-file-urls", action="store_true", help="accept file:// URLs (local fixtures only)")
    parser.add_argument(
        "--no-tree-check", action="store_true", help="do not require the manifest directory to be free of raw data"
    )
    args = parser.parse_args(argv)

    manifest = Path(args.manifest)
    errors = check(manifest, allow_file_urls=args.allow_file_urls, tree_check=not args.no_tree_check)
    if errors:
        for message in errors:
            print(f"ERROR: {message}", file=sys.stderr)
        print(f"{len(errors)} problem(s) found.", file=sys.stderr)
        return 1

    data, _ = load_manifest(manifest)
    sources = data["sources"]
    by_status: dict[str, int] = {}
    for entry in sources:
        by_status[entry["status"]] = by_status.get(entry["status"], 0) + 1
    summary = ", ".join(f"{count} {status}" for status, count in sorted(by_status.items())) or "empty"
    print(f"geodata manifest OK ({len(sources)} source(s): {summary}).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
