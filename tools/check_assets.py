#!/usr/bin/env python3
"""Validate the asset license registry (assets/LICENSES.csv).

Checks:
  * every file under assets/ (except README.md / LICENSES.csv / .gitkeep) is registered;
  * every registered path exists, is unique and uses forward slashes;
  * license ids are allowed (NC / ND licenses are rejected for a commercial project);
  * required fields are present for the chosen license;
  * checked_on is a valid ISO date.

Usage:
    python tools/check_assets.py [--assets-dir assets]
Exit code: 0 if the registry is consistent, 1 otherwise.
"""

from __future__ import annotations

import argparse
import csv
import datetime
import sys
from pathlib import Path

REQUIRED_COLUMNS = [
    "path",
    "title",
    "source_url",
    "license",
    "author",
    "attribution_required",
    "checked_on",
    "notes",
]

ALLOWED_LICENSES = {
    "CC0-1.0",
    "Public-Domain",
    "CC-BY-4.0",
    "CC-BY-3.0",
    "CC-BY-SA-4.0",
    "MIT",
    "Apache-2.0",
    "OFL-1.1",
    "Original",
    "Commercial",
}

# Licenses that do not need a source URL (own work or paid work with proof in notes).
NO_SOURCE_NEEDED = {"Original", "Commercial"}

IGNORED_FILES = {"README.md", "LICENSES.csv", ".gitkeep"}


def load_registry(csv_path: Path) -> tuple[list[dict[str, str]], list[str]]:
    errors: list[str] = []
    if not csv_path.is_file():
        return [], [f"registry not found: {csv_path}"]

    with csv_path.open(newline="", encoding="utf-8") as handle:
        reader = csv.DictReader(handle)
        header = reader.fieldnames or []
        missing = [c for c in REQUIRED_COLUMNS if c not in header]
        if missing:
            return [], [f"{csv_path.name}: missing columns: {', '.join(missing)}"]
        rows = [{k: (v or "").strip() for k, v in row.items()} for row in reader]
    return rows, errors


def list_asset_files(assets_dir: Path) -> set[str]:
    files: set[str] = set()
    for path in assets_dir.rglob("*"):
        if path.is_file() and path.name not in IGNORED_FILES:
            files.add(path.relative_to(assets_dir).as_posix())
    return files


def validate_row(row: dict[str, str], line: int, assets_dir: Path) -> list[str]:
    errors: list[str] = []
    where = f"LICENSES.csv:{line}"
    path = row["path"]

    if not path:
        return [f"{where}: empty path"]
    if "\\" in path or path.startswith("/") or ".." in Path(path).parts:
        errors.append(f"{where}: path must be relative with forward slashes: {path}")
    elif not (assets_dir / path).is_file():
        errors.append(f"{where}: registered file does not exist: {path}")

    lic = row["license"]
    if not lic:
        errors.append(f"{where}: license is empty ({path})")
    elif lic.startswith(("CC-BY-NC", "CC-BY-ND")):
        errors.append(f"{where}: license {lic} is not allowed for a commercial project ({path})")
    elif lic not in ALLOWED_LICENSES:
        errors.append(f"{where}: unknown license '{lic}' ({path}); allowed: {', '.join(sorted(ALLOWED_LICENSES))}")

    if lic and lic not in NO_SOURCE_NEEDED and not row["source_url"]:
        errors.append(f"{where}: source_url is required for license {lic} ({path})")

    attribution = row["attribution_required"].lower()
    if attribution not in {"yes", "no"}:
        errors.append(f"{where}: attribution_required must be 'yes' or 'no' ({path})")
    elif (attribution == "yes" or lic in {"Original", "CC-BY-4.0", "CC-BY-3.0", "CC-BY-SA-4.0"}) and not row["author"]:
        errors.append(f"{where}: author is required ({path})")

    if lic == "Commercial" and not row["notes"]:
        errors.append(f"{where}: notes must reference proof of purchase/commission ({path})")

    checked = row["checked_on"]
    try:
        datetime.date.fromisoformat(checked)
    except ValueError:
        errors.append(f"{where}: checked_on must be YYYY-MM-DD, got '{checked}' ({path})")

    if not row["title"]:
        errors.append(f"{where}: title is empty ({path})")

    return errors


def check(assets_dir: Path) -> list[str]:
    rows, errors = load_registry(assets_dir / "LICENSES.csv")
    if errors:
        return errors

    seen: dict[str, int] = {}
    for index, row in enumerate(rows, start=2):  # header is line 1
        errors.extend(validate_row(row, index, assets_dir))
        path = row["path"]
        if path in seen:
            errors.append(f"LICENSES.csv:{index}: duplicate entry for {path} (first on line {seen[path]})")
        elif path:
            seen[path] = index

    for unregistered in sorted(list_asset_files(assets_dir) - set(seen)):
        errors.append(f"unregistered asset: {unregistered} (add it to assets/LICENSES.csv)")

    return errors


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--assets-dir", default=str(Path(__file__).resolve().parent.parent / "assets"))
    args = parser.parse_args(argv)

    assets_dir = Path(args.assets_dir)
    if not assets_dir.is_dir():
        print(f"assets directory not found: {assets_dir}", file=sys.stderr)
        return 1

    errors = check(assets_dir)
    if errors:
        for message in errors:
            print(f"ERROR: {message}", file=sys.stderr)
        print(f"{len(errors)} problem(s) found.", file=sys.stderr)
        return 1

    count = len(list_asset_files(assets_dir))
    print(f"assets OK ({count} registered file(s)).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
