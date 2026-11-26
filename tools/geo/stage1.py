#!/usr/bin/env python3
"""Stage 1 of the geodata pipeline (ADR-0004 section 3): GDAL/PROJ preparation.

Reads the source datasets listed in data-sources/geodata.manifest.json from the local
cache (tools/geo/fetch.py), reprojects them into one of the candidate map projections,
resamples to the base raster (8192x4096 by default) and writes the intermediate files
that `mapgen import` (stage 2, Geo-3) reads without GDAL. Format and usage:
docs/dev/geodata-pipeline.md.

GDAL and PROJ are EXTERNAL offline tools (owner decision 2026-11-26, issue #80): they are
called through their command-line programs, are not a build dependency, are not in
vcpkg and never run in CI. This script itself uses only the Python standard library.

Commands:
    check-tools   verify that the GDAL/PROJ programs are installed in the pinned versions
    projections   list the candidate projections and their parameters (no GDAL needed)
    tiles         print manifest `files` entries for the Copernicus GLO-90 tiles of the
                  map area (no download; optionally filtered by the bucket tile list)
    frame         project reference points and report whether they fit the canvas (GDAL)
    run           build the intermediate files for one or more projections (GDAL);
                  `--dry-run` prints the commands without running them
    validate      check a directory of intermediate files (no GDAL needed)

Exit code: 0 on success, 1 on validation problems, 2 on usage/environment errors.
"""

from __future__ import annotations

import argparse
import array
import hashlib
import json
import math
import os
import re
import shutil
import subprocess
import sys
import unicodedata
from dataclasses import dataclass, field
from decimal import Decimal
from fractions import Fraction
from pathlib import Path
from typing import Callable, Iterable, Mapping, Sequence

_HERE = Path(__file__).resolve().parent
if str(_HERE) not in sys.path:
    sys.path.insert(0, str(_HERE))

import fetch  # noqa: E402
import geo_manifest  # noqa: E402

REPO_ROOT = geo_manifest.REPO_ROOT

FORMAT_NAME = "patriarch-geo-stage1"
FORMAT_VERSION = 1
METADATA_FILE = "stage1.json"
PGL_MAGIC = "PGL1"
SUBPIXEL = 16  # polyline coordinates are stored in 1/16 pixel (same unit as the RVEC chunk, ADR-0007 3.4)
HEIGHT_NODATA = -32768
HEIGHT_MIN_M = -450  # Dead Sea shore is about -430 m
HEIGHT_MAX_M = 9000  # Everest is 8849 m
WATER_LAND, WATER_SEA, WATER_LAKE = 0, 1, 2
WATER_CODES = {WATER_LAND: "land", WATER_SEA: "sea", WATER_LAKE: "lake"}
KEY_MAX = 28  # same limit as WTBL keys (ADR-0007, Geo-11)
MAX_HOLE_FRACTION = Fraction(1, 100)  # nodata heights on land above 1% of land pixels is an error

WORK_ENV = "PATRIARCH_GEO_WORK"
GDAL_BIN_ENV = "PATRIARCH_GDAL_BIN"
GEO_PYTHON_ENV = "PATRIARCH_GEO_PYTHON"
CONDA_ENV = "patriarch-geo"
DEFAULT_CONDA_BASES = ("miniforge3", "miniconda3", "mambaforge")

# Pinned tool versions (docs/dev/geodata-pipeline.md section 2; conda-forge environment
# installed by the owner on 2026-11-26). GDAL must match major.minor, PROJ must be at least
# this version. Other GDAL versions need --allow-other-gdal and are recorded in stage1.json,
# so results can be told apart.
PINNED_GDAL = (3, 13)
MIN_PROJ = (9, 9, 0)
GDAL_TOOLS = ("gdalinfo", "gdalbuildvrt", "gdal_translate", "gdalwarp", "gdal_rasterize", "ogr2ogr", "gdaltransform")
PROJ_TOOL = "proj"

DEM_SOURCE = "copernicus-dem-glo90"
NE_SOURCE = "natural-earth-10m-physical"
NE_LAYERS = {
    "ocean": "ne_10m_ocean",
    "minor_islands": "ne_10m_minor_islands",
    "lakes": "ne_10m_lakes",
    "coastline": "ne_10m_coastline",
    "rivers": "ne_10m_rivers_lake_centerlines",
}

GLO90_BUCKET = "https://copernicus-dem-90m.s3.amazonaws.com"
TILE_NAME = re.compile(r"Copernicus_DSM_COG_(\d\d)_([NS])(\d\d)_00_([EW])(\d\d\d)_00_DEM")

WGS84_LONLAT = "+proj=longlat +datum=WGS84 +no_defs +type=crs"
# Source CRS for the DEM mosaic whose western-hemisphere tiles were shifted by +360 degrees:
# longitudes live in [0, 360), so Chukotka (east of 180) is contiguous with the rest of Russia.
WGS84_LONLAT_WRAP = "+proj=longlat +datum=WGS84 +lon_wrap=180 +no_defs +type=crs"

UDEG = 1_000_000


class Stage1Error(Exception):
    """Environment or input problem that stops the pipeline (exit code 2)."""


# --------------------------------------------------------------------------------------
# Projections and canvas
# --------------------------------------------------------------------------------------


@dataclass(frozen=True)
class GeoBox:
    """Geographic area in micro-degrees. `east_udeg` may exceed 180 degrees (crosses the antimeridian)."""

    west_udeg: int
    south_udeg: int
    east_udeg: int
    north_udeg: int

    @property
    def crosses_antimeridian(self) -> bool:
        return self.east_udeg > 180 * UDEG


@dataclass(frozen=True)
class Canvas:
    """Target raster: size in pixels, square pixel size and canvas centre in projection millimetres."""

    width: int = 8192
    height: int = 4096
    pixel_size_mm: int = 1_300_000
    center_x_mm: int = 0
    center_y_mm: int = 0

    def __post_init__(self) -> None:
        if self.width <= 0 or self.height <= 0 or self.width % 2 or self.height % 2:
            raise ValueError("canvas width and height must be positive and even")
        if self.pixel_size_mm <= 0:
            raise ValueError("pixel size must be positive")

    @property
    def origin_x_mm(self) -> int:
        """Projected X of the left edge of pixel column 0."""
        return self.center_x_mm - self.width * self.pixel_size_mm // 2

    @property
    def origin_y_mm(self) -> int:
        """Projected Y of the top edge of pixel row 0 (row 0 is north)."""
        return self.center_y_mm + self.height * self.pixel_size_mm // 2

    def extent_mm(self) -> tuple[int, int, int, int]:
        """(xmin, ymin, xmax, ymax) in projection millimetres."""
        xmin = self.origin_x_mm
        ymax = self.origin_y_mm
        return xmin, ymax - self.height * self.pixel_size_mm, xmin + self.width * self.pixel_size_mm, ymax


@dataclass(frozen=True)
class ProjectionSpec:
    """One candidate projection. Angles are micro-degrees, so GEOR fields are exact integers."""

    id: str
    title: str
    geor_code: int  # pat::mapdata::Projection (ADR-0007 3.3)
    proj_name: str  # PROJ operation name
    lat_origin_udeg: int
    lon_central_udeg: int
    lat_std1_udeg: int = 0
    lat_std2_udeg: int = 0
    canvas: Canvas = field(default_factory=Canvas)

    @property
    def is_conic(self) -> bool:
        return self.proj_name in {"aea", "lcc"}


# Map area used to select source data (ADR-0004 section 4: Russia to the Pacific plus the
# neighbouring powers of 1703 as background). [предл.] — confirmed by the projection spike (Geo-5).
MAP_AREA = GeoBox(west_udeg=15 * UDEG, south_udeg=28 * UDEG, east_udeg=195 * UDEG, north_udeg=82 * UDEG)

# Candidate projections for Geo-5 (#81). All parameters are proposals [предл.]: central meridian
# 100 E keeps Chukotka about +90 degrees from the centre (ADR-0007 3.3), standard parallels by the
# 1/6 rule over roughly 42-75 N; the canvas centre is shifted north because the conic "banana"
# of Russia rises towards Kaliningrad and Chukotka. Pixel size 1.3 km (8192 px = 10 650 km).
PROJECTIONS: dict[str, ProjectionSpec] = {
    spec.id: spec
    for spec in (
        ProjectionSpec(
            id="albers",
            title="Albers equal-area conic",
            geor_code=1,
            proj_name="aea",
            lat_origin_udeg=58 * UDEG,
            lon_central_udeg=100 * UDEG,
            lat_std1_udeg=47 * UDEG,
            lat_std2_udeg=69 * UDEG,
            canvas=Canvas(center_y_mm=680_000_000),
        ),
        ProjectionSpec(
            id="lcc",
            title="Lambert conformal conic",
            geor_code=2,
            proj_name="lcc",
            lat_origin_udeg=58 * UDEG,
            lon_central_udeg=100 * UDEG,
            lat_std1_udeg=47 * UDEG,
            lat_std2_udeg=69 * UDEG,
            canvas=Canvas(center_y_mm=680_000_000),
        ),
        ProjectionSpec(
            id="laea",
            title="Lambert azimuthal equal-area (fallback)",
            geor_code=3,
            proj_name="laea",
            lat_origin_udeg=62 * UDEG,
            lon_central_udeg=100 * UDEG,
            canvas=Canvas(center_y_mm=300_000_000),
        ),
    )
}


def udeg_text(value: int) -> str:
    """Exact decimal text of a micro-degree value: 58000000 -> '58', 47500000 -> '47.5'."""
    sign = "-" if value < 0 else ""
    whole, frac = divmod(abs(value), UDEG)
    if frac == 0:
        return f"{sign}{whole}"
    return f"{sign}{whole}.{frac:06d}".rstrip("0")


def mm_text(value: int) -> str:
    """Exact metres text of a millimetre value: 1300000 -> '1300', -1500 -> '-1.5'."""
    sign = "-" if value < 0 else ""
    whole, frac = divmod(abs(value), 1000)
    if frac == 0:
        return f"{sign}{whole}"
    return f"{sign}{whole}.{frac:03d}".rstrip("0")


def proj_string(spec: ProjectionSpec) -> str:
    """PROJ string of the target CRS (WGS84 ellipsoid, metres, no false easting/northing)."""
    parts = [f"+proj={spec.proj_name}", f"+lat_0={udeg_text(spec.lat_origin_udeg)}"]
    parts.append(f"+lon_0={udeg_text(spec.lon_central_udeg)}")
    if spec.is_conic:
        parts += [f"+lat_1={udeg_text(spec.lat_std1_udeg)}", f"+lat_2={udeg_text(spec.lat_std2_udeg)}"]
    parts += ["+x_0=0", "+y_0=0", "+datum=WGS84", "+units=m", "+no_defs", "+type=crs"]
    return " ".join(parts)


def geor_fields(spec: ProjectionSpec, area: GeoBox = MAP_AREA) -> dict[str, object]:
    """GEOR chunk fields (ADR-0007 3.3, pat::mapdata::GeoRef) for this projection and canvas.

    Longitudes are in [-180, 180]. An area crossing the antimeridian is written as min_lon > max_lon
    (RFC 7946 section 5.2), which the C++ validator accepts (validate_georef in physical_layer.cpp);
    for example 15 E .. 165 W is (15_000_000, -165_000_000).
    """
    canvas = spec.canvas
    min_lon, max_lon = area.west_udeg, area.east_udeg
    if area.crosses_antimeridian:
        max_lon -= 360 * UDEG
    return {
        "version": 1,
        "projection": spec.geor_code,
        "datum": 1,
        "lat_origin_udeg": spec.lat_origin_udeg,
        "lon_central_udeg": spec.lon_central_udeg,
        "lat_std1_udeg": spec.lat_std1_udeg,
        "lat_std2_udeg": spec.lat_std2_udeg,
        "false_easting_mm": 0,
        "false_northing_mm": 0,
        "origin_x_mm": canvas.origin_x_mm,
        "origin_y_mm": canvas.origin_y_mm,
        "pixel_size_x_mm": canvas.pixel_size_mm,
        "pixel_size_y_mm": canvas.pixel_size_mm,
        "bbox_min_lon_udeg": min_lon,
        "bbox_max_lon_udeg": max_lon,
        "bbox_min_lat_udeg": area.south_udeg,
        "bbox_max_lat_udeg": area.north_udeg,
        "epsg": 0,
        "proj_text": proj_string(spec),
    }


def clip_wkt(area: GeoBox = MAP_AREA) -> str:
    """Clip polygon in plain WGS84 longitudes; an antimeridian-crossing area becomes two boxes."""

    def box(w: int, s: int, e: int, n: int) -> str:
        w_, s_, e_, n_ = (udeg_text(v) for v in (w, s, e, n))
        return f"(({w_} {s_}, {e_} {s_}, {e_} {n_}, {w_} {n_}, {w_} {s_}))"

    s, n = area.south_udeg, area.north_udeg
    if not area.crosses_antimeridian:
        return f"POLYGON{box(area.west_udeg, s, area.east_udeg, n)}"
    east = area.east_udeg - 360 * UDEG
    return f"MULTIPOLYGON({box(area.west_udeg, s, 180 * UDEG, n)}, {box(-180 * UDEG, s, east, n)})"


def params_hash(params: Mapping[str, object]) -> str:
    """FNV-1a 64 of canonical JSON of step parameters, as 0x-hex (PROV step params_hash)."""
    data = json.dumps(params, sort_keys=True, separators=(",", ":"), ensure_ascii=True).encode("ascii")
    value = 0xCBF29CE484222325
    for byte in data:
        value ^= byte
        value = (value * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return f"0x{value:016x}"


# --------------------------------------------------------------------------------------
# Copernicus tiles
# --------------------------------------------------------------------------------------


def tile_name(lat: int, lon: int, arcsec: int = 30) -> str:
    """Copernicus DEM COG tile name for the 1x1 degree cell with south-west corner (lat, lon)."""
    ns = "N" if lat >= 0 else "S"
    ew = "E" if lon >= 0 else "W"
    return f"Copernicus_DSM_COG_{arcsec:02d}_{ns}{abs(lat):02d}_00_{ew}{abs(lon):03d}_00_DEM"


def parse_tile_name(text: str) -> tuple[int, int] | None:
    """(lat, lon) of the south-west corner from a tile name or path, or None."""
    match = TILE_NAME.search(text)
    if not match:
        return None
    lat = int(match.group(3)) * (1 if match.group(2) == "N" else -1)
    lon = int(match.group(5)) * (1 if match.group(4) == "E" else -1)
    return lat, lon


def tiles_for_area(area: GeoBox = MAP_AREA) -> list[tuple[int, int]]:
    """All 1x1 degree cells (lat, lon in -180..179) that touch the area, ordered north-west first."""
    south = math.floor(Fraction(area.south_udeg, UDEG))
    north = math.ceil(Fraction(area.north_udeg, UDEG))
    west = math.floor(Fraction(area.west_udeg, UDEG))
    east = math.ceil(Fraction(area.east_udeg, UDEG))
    cells = []
    for lat in range(north - 1, south - 1, -1):
        for lon in range(west, east):
            cells.append((lat, (lon + 180) % 360 - 180))
    return cells


def tile_manifest_entries(area: GeoBox = MAP_AREA, existing: Iterable[str] | None = None) -> list[dict]:
    """Manifest `files` entries (sha256/size null until the first download) for GLO-90 tiles.

    `existing` is the content of the bucket's tile list (one tile name per line); ocean cells
    have no tile and are skipped when it is given.
    """
    present = None
    if existing is not None:
        present = {line.strip().rsplit("/", 1)[-1].removesuffix(".tif") for line in existing if line.strip()}
    entries = []
    for lat, lon in tiles_for_area(area):
        name = tile_name(lat, lon)
        if present is not None and name not in present:
            continue
        entries.append({"path": f"{name}.tif", "url": f"{GLO90_BUCKET}/{name}/{name}.tif", "sha256": None, "size": None})
    return entries


def shifted_ullr(info: Mapping[str, object], shift_deg: int = 360) -> list[str]:
    """`gdal_translate -a_ullr` values moving a raster east by `shift_deg` (from `gdalinfo -json`)."""
    gt = info["geoTransform"]
    width, height = info["size"]
    ulx = Decimal(repr(gt[0])) + shift_deg
    uly = Decimal(repr(gt[3]))
    lrx = ulx + Decimal(repr(gt[1])) * width
    lry = uly + Decimal(repr(gt[5])) * height
    return [str(v) for v in (ulx, uly, lrx, lry)]


# --------------------------------------------------------------------------------------
# Tool versions
# --------------------------------------------------------------------------------------

GDAL_VERSION = re.compile(r"GDAL (\d+)\.(\d+)\.(\d+)")
PROJ_VERSION = re.compile(r"Rel\. (\d+)\.(\d+)\.(\d+)")


def parse_gdal_version(text: str) -> tuple[int, int, int] | None:
    match = GDAL_VERSION.search(text)
    return tuple(int(g) for g in match.groups()) if match else None  # type: ignore[return-value]


def parse_proj_version(text: str) -> tuple[int, int, int] | None:
    match = PROJ_VERSION.search(text)
    return tuple(int(g) for g in match.groups()) if match else None  # type: ignore[return-value]


def version_problems(
    gdal: tuple[int, int, int] | None, proj: tuple[int, int, int] | None, allow_other_gdal: bool
) -> list[str]:
    problems = []
    pinned = ".".join(map(str, PINNED_GDAL))
    if gdal is None:
        problems.append("cannot read the GDAL version from `gdalinfo --version`")
    elif gdal[:2] != PINNED_GDAL and not allow_other_gdal:
        problems.append(
            f"GDAL {'.'.join(map(str, gdal))} found, the pipeline is pinned to GDAL {pinned}.x "
            f"(docs/dev/geodata-pipeline.md section 2; --allow-other-gdal to run anyway)"
        )
    if proj is None:
        problems.append("cannot read the PROJ version from `proj` (is PROJ installed with GDAL?)")
    elif proj < MIN_PROJ:
        problems.append(f"PROJ {'.'.join(map(str, proj))} found, at least {'.'.join(map(str, MIN_PROJ))} is required")
    return problems


@dataclass
class Tools:
    paths: dict[str, str]
    gdal: tuple[int, int, int] | None = None
    proj: tuple[int, int, int] | None = None
    env: dict[str, str] | None = None  # environment for the child processes (None = inherit)

    def exe(self, name: str) -> str:
        return self.paths.get(name, name)

    def versions(self) -> dict[str, str | None]:
        return {
            "gdal": ".".join(map(str, self.gdal)) if self.gdal else None,
            "proj": ".".join(map(str, self.proj)) if self.proj else None,
        }


def env_bin_dir(prefix: Path, windows: bool) -> Path:
    """Directory with the GDAL programs inside a conda environment prefix."""
    return prefix / "Library" / "bin" if windows else prefix / "bin"


def candidate_bin_dirs(
    gdal_bin: str | None, env: Mapping[str, str], home: Path, windows: bool
) -> list[tuple[str, Path | None]]:
    """Where to look for GDAL/PROJ, in order: (label, directory or None for PATH).

    --gdal-bin > PATRIARCH_GDAL_BIN > PATRIARCH_GEO_PYTHON (python of the conda environment)
    > PATH > the default Miniforge/Miniconda environment `patriarch-geo` in the home directory.
    """
    if gdal_bin:
        return [("--gdal-bin", Path(gdal_bin))]
    if env.get(GDAL_BIN_ENV):
        return [(GDAL_BIN_ENV, Path(env[GDAL_BIN_ENV]))]
    if env.get(GEO_PYTHON_ENV):
        return [(GEO_PYTHON_ENV, env_bin_dir(Path(env[GEO_PYTHON_ENV]).parent, windows))]
    found: list[tuple[str, Path | None]] = [("PATH", None)]
    for base in DEFAULT_CONDA_BASES:
        found.append((f"~/{base}", env_bin_dir(home / base / "envs" / CONDA_ENV, windows)))
    return found


def data_env(bin_dir: Path | None, env: Mapping[str, str]) -> dict[str, str] | None:
    """PROJ_DATA / GDAL_DATA for a conda environment used without `conda activate`."""
    if bin_dir is None:
        return None
    share = bin_dir.parent / "share"
    extra = {}
    for name, sub in (("PROJ_DATA", "proj"), ("GDAL_DATA", "gdal")):
        if not env.get(name) and (share / sub).is_dir():
            extra[name] = str(share / sub)
    if not extra:
        return None
    merged = dict(env)
    merged.update(extra)
    merged["PATH"] = str(bin_dir) + os.pathsep + env.get("PATH", "")  # DLLs next to the programs
    return merged


def find_tools(gdal_bin: str | None, allow_other_gdal: bool) -> Tools:
    """Locate the GDAL/PROJ programs and check their versions; raise Stage1Error with a clear message."""
    names = (*GDAL_TOOLS, PROJ_TOOL)
    candidates = candidate_bin_dirs(gdal_bin, os.environ, Path.home(), os.name == "nt")
    paths: dict[str, str] = {}
    chosen: Path | None = None
    for _, directory in candidates:
        search = str(directory) if directory is not None else None
        paths = {}
        for name in names:
            found = shutil.which(name, path=search) if search else shutil.which(name)
            if found:
                paths[name] = found
        if len(paths) == len(names):
            chosen = directory
            break
    else:
        tried = ", ".join(label if d is None else f"{label} ({d})" for label, d in candidates)
        missing = [n for n in names if n not in paths]
        raise Stage1Error(
            f"GDAL/PROJ programs not found (tried {tried}); missing: {', '.join(missing)}. Install the "
            f"conda environment '{CONDA_ENV}' as described in docs/dev/geodata-pipeline.md section 2, "
            f"or point {GEO_PYTHON_ENV} to its python / {GDAL_BIN_ENV} or --gdal-bin to its programs."
        )
    env = data_env(chosen, os.environ)
    gdal_out = _capture([paths["gdalinfo"], "--version"], env)
    proj_out = _capture([paths[PROJ_TOOL]], env)
    tools = Tools(paths, parse_gdal_version(gdal_out), parse_proj_version(proj_out), env)
    problems = version_problems(tools.gdal, tools.proj, allow_other_gdal)
    if problems:
        raise Stage1Error("; ".join(problems))
    return tools


def _capture(argv: Sequence[str], env: Mapping[str, str] | None = None) -> str:
    try:
        result = subprocess.run(argv, capture_output=True, text=True, timeout=60, check=False, env=env)
    except OSError as exc:
        return f"<cannot run {argv[0]}: {exc}>"
    return (result.stdout or "") + (result.stderr or "")


# --------------------------------------------------------------------------------------
# Manifest sources
# --------------------------------------------------------------------------------------


@dataclass
class SourceFiles:
    id: str
    version: str | None
    files: list[tuple[str, str | None, Path]]  # (manifest path, sha256, local path)


def source_files(manifest: Mapping, source_id: str, cache: Path, allow_unverified: bool) -> SourceFiles:
    """Resolve the cached files of one manifest source; refuse unusable statuses."""
    entry = next((e for e in manifest.get("sources", []) if e.get("id") == source_id), None)
    if entry is None:
        raise Stage1Error(f"source '{source_id}' is not in the manifest")
    status = entry.get("status")
    if status in {"rejected", "blocked-on-owner"}:
        raise Stage1Error(f"source '{source_id}' is {status}: it may not be used")
    if status != "ready" and not allow_unverified:
        raise Stage1Error(f"source '{source_id}' is {status}, not ready (pass --allow-unverified for a trial run)")
    files = entry.get("files") or []
    if not files:
        raise Stage1Error(
            f"source '{source_id}' lists no files in the manifest: record them first "
            f"(docs/dev/geodata-howto.md section 5, `stage1.py tiles` for the DEM)"
        )
    version = entry.get("version") or entry.get("version_date")
    resolved = [(f["path"], f.get("sha256"), cache / source_id / f["path"]) for f in files]
    return SourceFiles(source_id, version, resolved)


def ne_dataset(source: SourceFiles, stem: str) -> tuple[str, str]:
    """(GDAL dataset path, layer name) of a Natural Earth layer, from a .zip or .shp manifest file."""
    for manifest_path, _, local in source.files:
        name = manifest_path.rsplit("/", 1)[-1]
        if name in {f"{stem}.zip", f"{stem}.shp"}:
            posix = local.as_posix()
            return (f"/vsizip/{posix}" if name.endswith(".zip") else posix), stem
    raise Stage1Error(f"source '{source.id}' has no file named {stem}.zip or {stem}.shp in the manifest")


# --------------------------------------------------------------------------------------
# Polylines (PGL1)
# --------------------------------------------------------------------------------------


def slug_key(text: object) -> str:
    """ASCII key [a-z0-9-] of at most KEY_MAX characters; '-' when nothing is left."""
    if not isinstance(text, str):
        return "-"
    folded = unicodedata.normalize("NFKD", text).encode("ascii", "ignore").decode("ascii").lower()
    slug = re.sub(r"[^a-z0-9]+", "-", folded).strip("-")[:KEY_MAX].strip("-")
    return slug or "-"


def to_subpixel(x_m: Decimal, y_m: Decimal, canvas: Canvas) -> tuple[int, int]:
    """Projected metres -> canvas coordinates in 1/SUBPIXEL pixel (x right, y down), rounded half up."""
    fx = (Fraction(x_m) * 1000 - canvas.origin_x_mm) * SUBPIXEL / canvas.pixel_size_mm
    fy = (canvas.origin_y_mm - Fraction(y_m) * 1000) * SUBPIXEL / canvas.pixel_size_mm
    return math.floor(fx + Fraction(1, 2)), math.floor(fy + Fraction(1, 2))


def split_inside(points: Sequence[tuple[int, int]], width: int, height: int) -> list[list[tuple[int, int]]]:
    """Drop consecutive duplicates and cut the line where it leaves the canvas; keep runs of >= 2 points."""
    xmax, ymax = width * SUBPIXEL, height * SUBPIXEL
    runs: list[list[tuple[int, int]]] = []
    current: list[tuple[int, int]] = []
    for point in points:
        x, y = point
        if 0 <= x <= xmax and 0 <= y <= ymax:
            if not current or current[-1] != point:
                current.append(point)
        else:
            if len(current) >= 2:
                runs.append(current)
            current = []
    if len(current) >= 2:
        runs.append(current)
    return runs


@dataclass
class Polyline:
    rank: int
    key: str
    points: list[tuple[int, int]]


def geojson_polylines(data: Mapping, canvas: Canvas) -> list[Polyline]:
    """Convert projected GeoJSON LineString/MultiLineString features into canvas polylines (source order)."""
    lines: list[Polyline] = []
    for feature in data.get("features", []):
        geometry = feature.get("geometry") or {}
        props = feature.get("properties") or {}
        kind = geometry.get("type")
        if kind == "LineString":
            parts = [geometry["coordinates"]]
        elif kind == "MultiLineString":
            parts = geometry["coordinates"]
        else:
            continue
        rank = props.get("scalerank")
        rank = min(max(int(rank), 0), 255) if isinstance(rank, (int, float, Decimal)) else 0
        key = slug_key(props.get("name"))
        for part in parts:
            points = [to_subpixel(Decimal(c[0]), Decimal(c[1]), canvas) for c in part]
            for run in split_inside(points, canvas.width, canvas.height):
                lines.append(Polyline(rank, key, run))
    return lines


def format_pgl(kind: str, canvas: Canvas, lines: Sequence[Polyline]) -> str:
    out = [f"{PGL_MAGIC} {kind} {canvas.width} {canvas.height} {SUBPIXEL} {len(lines)}"]
    for number, line in enumerate(lines, start=1):
        out.append(f"L {number} {line.rank} {len(line.points)} {line.key}")
        out.extend(f"{x} {y}" for x, y in line.points)
    return "\n".join(out) + "\n"


def parse_pgl(text: str) -> tuple[dict, list[Polyline]]:
    """Parse and check a PGL1 file; raise ValueError with the line number on any problem."""
    rows = text.split("\n")
    if rows and rows[-1] == "":
        rows.pop()
    if not rows:
        raise ValueError("empty file")
    head = rows[0].split(" ")
    if len(head) != 6 or head[0] != PGL_MAGIC:
        raise ValueError(f"line 1: expected '{PGL_MAGIC} <kind> <width> <height> <subpixel> <count>'")
    try:
        width, height, subpixel, count = (int(v) for v in head[2:])
    except ValueError:
        raise ValueError("line 1: width, height, subpixel and count must be integers") from None
    if head[1] not in {"coastline", "river"} or subpixel != SUBPIXEL or width <= 0 or height <= 0:
        raise ValueError("line 1: unknown kind, bad size or subpixel")
    xmax, ymax = width * SUBPIXEL, height * SUBPIXEL
    lines: list[Polyline] = []
    index = 1
    for number in range(1, count + 1):
        if index >= len(rows):
            raise ValueError(f"line {index + 1}: expected polyline {number} of {count}")
        parts = rows[index].split(" ")
        if len(parts) != 5 or parts[0] != "L" or parts[1] != str(number):
            raise ValueError(f"line {index + 1}: expected 'L {number} <rank> <points> <key>'")
        rank, npoints = int(parts[2]), int(parts[3])
        key = parts[4]
        if not 0 <= rank <= 255 or npoints < 2 or not re.fullmatch(r"[a-z0-9-]{1,28}", key):
            raise ValueError(f"line {index + 1}: bad rank, point count or key")
        index += 1
        points = []
        for _ in range(npoints):
            if index >= len(rows):
                raise ValueError(f"line {index + 1}: polyline {number} is truncated")
            xy = rows[index].split(" ")
            if len(xy) != 2:
                raise ValueError(f"line {index + 1}: expected '<x> <y>'")
            x, y = int(xy[0]), int(xy[1])
            if not (0 <= x <= xmax and 0 <= y <= ymax):
                raise ValueError(f"line {index + 1}: point outside the canvas")
            points.append((x, y))
            index += 1
        lines.append(Polyline(rank, key, points))
    if index != len(rows):
        raise ValueError(f"line {index + 1}: unexpected data after {count} polylines")
    return {"kind": head[1], "width": width, "height": height}, lines


# --------------------------------------------------------------------------------------
# Validation of intermediate files (pure Python, no GDAL)
# --------------------------------------------------------------------------------------


@dataclass
class Report:
    errors: list[str] = field(default_factory=list)
    warnings: list[str] = field(default_factory=list)
    stats: dict[str, object] = field(default_factory=dict)


def _read_raster(path: Path, typecode: str, count: int) -> array.array:
    values = array.array(typecode)
    with path.open("rb") as handle:
        values.fromfile(handle, count)
    if sys.byteorder != "little":
        values.byteswap()
    return values


def _check_envi_header(path: Path, report: Report) -> None:
    header = path.with_suffix(".hdr")
    if header.is_file():
        text = header.read_text(encoding="ascii", errors="replace")
        match = re.search(r"byte order\s*=\s*(\d)", text)
        if match and match.group(1) != "0":
            report.errors.append(f"{header.name}: byte order must be 0 (little endian)")


def validate_dir(out_dir: Path, check_hashes: bool = True) -> Report:
    """Check a stage-1 output directory against stage1.json (format, sizes, hashes, value ranges)."""
    report = Report()
    meta_path = out_dir / METADATA_FILE
    try:
        meta = json.loads(meta_path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        report.errors.append(f"{METADATA_FILE}: cannot read: {exc}")
        return report
    if meta.get("format") != FORMAT_NAME or meta.get("format_version") != FORMAT_VERSION:
        report.errors.append(f"{METADATA_FILE}: expected format {FORMAT_NAME} version {FORMAT_VERSION}")
        return report
    width, height = meta.get("width"), meta.get("height")
    if not (isinstance(width, int) and isinstance(height, int) and width > 0 and height > 0):
        report.errors.append(f"{METADATA_FILE}: width/height must be positive integers")
        return report
    geor = meta.get("georef", {})
    if geor.get("pixel_size_x_mm", 0) <= 0 or geor.get("pixel_size_y_mm", 0) <= 0:
        report.errors.append("georef: pixel size must be positive")
    files = meta.get("files", {})
    for role in ("height", "water", "coastline", "rivers"):
        if role not in files:
            report.errors.append(f"{METADATA_FILE}: files.{role} is missing")
    if report.errors:
        return report

    for role, item in sorted(files.items()):
        path = out_dir / item["path"]
        if not path.is_file():
            report.errors.append(f"{role}: file {item['path']} is missing")
            continue
        digest, size = fetch.sha256_file(path) if check_hashes else (item.get("sha256"), path.stat().st_size)
        if size != item.get("size"):
            report.errors.append(f"{role}: size {size} differs from stage1.json ({item.get('size')})")
        if digest != item.get("sha256"):
            report.errors.append(f"{role}: sha256 differs from stage1.json")
    if report.errors:
        return report

    pixels = width * height
    height_path = out_dir / files["height"]["path"]
    water_path = out_dir / files["water"]["path"]
    if height_path.stat().st_size != pixels * 2:
        report.errors.append(f"height: expected {pixels * 2} bytes (int16 {width}x{height})")
    if water_path.stat().st_size != pixels:
        report.errors.append(f"water: expected {pixels} bytes (uint8 {width}x{height})")
    if report.errors:
        return report
    _check_envi_header(height_path, report)
    _check_envi_header(water_path, report)

    water = water_path.read_bytes()
    counts = {code: water.count(bytes([code])) for code in WATER_CODES}
    unknown = pixels - sum(counts.values())
    if unknown:
        report.errors.append(f"water: {unknown} pixel(s) with codes outside {sorted(WATER_CODES)}")
    heights = _read_raster(height_path, "h", pixels)
    lo, hi, nodata, holes = None, None, 0, 0
    for h, w in zip(heights, water):  # one pass, no copies: the base raster has 33.5 M pixels
        if h == HEIGHT_NODATA:
            nodata += 1
            holes += w == WATER_LAND
        elif lo is None:
            lo = hi = h
        elif h < lo:
            lo = h
        elif h > hi:
            hi = h
    if lo is not None:
        if lo < HEIGHT_MIN_M or hi > HEIGHT_MAX_M:
            report.errors.append(f"height: values {lo}..{hi} m outside {HEIGHT_MIN_M}..{HEIGHT_MAX_M} m")
        report.stats["height_min_m"], report.stats["height_max_m"] = lo, hi
    else:
        report.errors.append("height: every pixel is nodata")
    land = counts[WATER_LAND]
    if holes:
        message = f"height: {holes} land pixel(s) without DEM data (missing tiles?)"
        if land and Fraction(holes, land) > MAX_HOLE_FRACTION:
            report.errors.append(message)
        else:
            report.warnings.append(message)
    report.stats.update(
        {
            "pixels": pixels,
            "land_px": land,
            "sea_px": counts[WATER_SEA],
            "lake_px": counts[WATER_LAKE],
            "height_nodata_px": nodata,
            "land_holes_px": holes,
        }
    )

    for role, kind in (("coastline", "coastline"), ("rivers", "river")):
        try:
            head, lines = parse_pgl((out_dir / files[role]["path"]).read_text(encoding="ascii"))
        except (ValueError, UnicodeDecodeError) as exc:
            report.errors.append(f"{role}: {exc}")
            continue
        if head["kind"] != kind or head["width"] != width or head["height"] != height:
            report.errors.append(f"{role}: header kind/size does not match stage1.json")
        report.stats[f"{role}_lines"] = len(lines)
        report.stats[f"{role}_points"] = sum(len(line.points) for line in lines)
    return report


# --------------------------------------------------------------------------------------
# Running GDAL
# --------------------------------------------------------------------------------------


class Runner:
    """Runs (or, in dry-run mode, prints) external commands."""

    def __init__(self, tools: Tools | None, dry_run: bool, log: Callable[[str], None] = print) -> None:
        self.tools = tools
        self.dry_run = dry_run
        self.log = log
        self.commands: list[list[str]] = []

    def exe(self, name: str) -> str:
        return self.tools.exe(name) if self.tools else name

    def run(self, name: str, *args: str) -> str:
        argv = [self.exe(name), *args]
        self.commands.append([name, *args])
        self.log("$ " + subprocess.list2cmdline([name, *args]))
        if self.dry_run:
            return ""
        env = self.tools.env if self.tools else None
        result = subprocess.run(argv, capture_output=True, text=True, check=False, env=env)
        if result.returncode != 0:
            tail = (result.stderr or result.stdout or "").strip().splitlines()[-5:]
            raise Stage1Error(f"{name} failed with code {result.returncode}: " + " | ".join(tail))
        return result.stdout


def _remove(runner: Runner, path: Path) -> None:
    if not runner.dry_run and path.exists():
        path.unlink()


def build_dem(runner: Runner, dem: SourceFiles, spec: ProjectionSpec, tmp: Path, out: Path) -> None:
    """Mosaic DEM tiles (western-hemisphere tiles shifted by +360) and warp to the canvas."""
    east, west = [], []
    for manifest_path, _, local in dem.files:
        cell = parse_tile_name(manifest_path)
        (west if cell is not None and cell[1] < 0 else east).append(local)
    if not east and not west:
        raise Stage1Error(f"source '{dem.id}' has no DEM tiles")
    parts = []
    for label, files in (("east", east), ("west", west)):
        if not files:
            continue
        listing = tmp / f"dem_{label}.txt"
        if not runner.dry_run:
            listing.write_text("\n".join(str(p) for p in files) + "\n", encoding="utf-8")
        vrt = tmp / f"dem_{label}.vrt"
        runner.run("gdalbuildvrt", "-overwrite", "-vrtnodata", str(HEIGHT_NODATA), "-input_file_list", str(listing), str(vrt))
        if label == "west":
            info = runner.run("gdalinfo", "-json", str(vrt))
            ullr = shifted_ullr(json.loads(info)) if info else ["<ulx+360>", "<uly>", "<lrx+360>", "<lry>"]
            shifted = tmp / "dem_west360.vrt"
            runner.run("gdal_translate", "-of", "VRT", "-a_ullr", *ullr, str(vrt), str(shifted))
            vrt = shifted
        parts.append(str(vrt))
    mosaic = tmp / "dem_mosaic.vrt"
    runner.run("gdalbuildvrt", "-overwrite", "-resolution", "highest", "-vrtnodata", str(HEIGHT_NODATA), str(mosaic), *parts)
    xmin, ymin, xmax, ymax = (mm_text(v) for v in spec.canvas.extent_mm())
    _remove(runner, out)
    runner.run(
        "gdalwarp", "-overwrite", "-of", "ENVI", "-ot", "Int16",
        "-s_srs", WGS84_LONLAT_WRAP, "-t_srs", proj_string(spec),
        "-te", xmin, ymin, xmax, ymax, "-ts", str(spec.canvas.width), str(spec.canvas.height),
        "-r", "average", "-et", "0", "-wo", "SAMPLE_GRID=YES",
        "-srcnodata", str(HEIGHT_NODATA), "-dstnodata", str(HEIGHT_NODATA),
        "-multi", "-wo", "NUM_THREADS=ALL_CPUS",
        str(mosaic), str(out),
    )  # fmt: skip


# Vertex spacing of the map area box (degrees): its edges along meridians and parallels are curves
# in the conic projections, so a box with corner vertices only would turn into chords there.
AREA_SEGMENT_DEG = "0.25"
AREA_SEGMENT_UDEG = 250_000


def project_vector(
    runner: Runner, dataset: str, layer: str, spec: ProjectionSpec, out: Path, fields: str, clip: Path
) -> None:
    """Clip to the map area (`clip`: the densified area_geojson file) and reproject one layer.

    Polygons for rasterization go to GeoPackage, which keeps the custom CRS (a GeoJSON file
    without an EPSG code is read back as WGS84, and gdal_rasterize would then reproject the
    already projected coordinates and burn nothing). Lines go to GeoJSON, read only by Python.
    """
    _remove(runner, out)
    if out.suffix == ".gpkg":
        driver = ["-f", "GPKG", "-nln", out.stem]
    else:
        driver = ["-f", "GeoJSON", "-lco", "COORDINATE_PRECISION=1", "-lco", "RFC7946=NO"]
    runner.run(
        "ogr2ogr", *driver, "-t_srs", proj_string(spec), "-clipsrc", str(clip),
        "-select", fields, "-dim", "XY", str(out), dataset, layer,
    )  # fmt: skip


def _steps(a: int, b: int, step: int = AREA_SEGMENT_UDEG) -> list[int]:
    """a, a +- step, ..., b (micro-degrees; both ends included)."""
    sign = 1 if b >= a else -1
    out = list(range(a, b, sign * step))
    return out + [b]


def area_geojson(area: GeoBox = MAP_AREA) -> str:
    """The map area as a WGS84 GeoJSON polygon (two boxes across the antimeridian, like clip_wkt),
    with a vertex every AREA_SEGMENT_DEG along each edge. Used as the -clipsrc of every vector layer
    (a clip edge then follows the curved parallel after projection) and rasterized as the land frame.
    """

    def ring(west: int, east: int) -> list:
        s, n = area.south_udeg, area.north_udeg
        pts = [(x, s) for x in _steps(west, east)]
        pts += [(east, y) for y in _steps(s, n)[1:]]
        pts += [(x, n) for x in _steps(east, west)[1:]]
        pts += [(west, y) for y in _steps(n, s)[1:]]
        return [[[x / UDEG, y / UDEG] for x, y in pts]]

    if area.crosses_antimeridian:
        polygons = [ring(area.west_udeg, 180 * UDEG), ring(-180 * UDEG, area.east_udeg - 360 * UDEG)]
    else:
        polygons = [ring(area.west_udeg, area.east_udeg)]
    feature = {"type": "Feature", "properties": {}, "geometry": {"type": "MultiPolygon", "coordinates": polygons}}
    return json.dumps({"type": "FeatureCollection", "features": [feature]})


def build_water(runner: Runner, spec: ProjectionSpec, vectors: Mapping[str, Path], out: Path) -> None:
    """Rasterize the water mask: everything starts as sea, the map area becomes land, then the
    ocean = 1 (clipped to the area), minor islands back to land, lakes = 2.

    Outside the map area there are no sources (the vectors are clipped, the DEM tiles stop), so the
    canvas corners beyond it (the Arctic north of 82 N, the Pacific east of 165 W) are open sea
    rather than land without data.
    """
    xmin, ymin, xmax, ymax = (mm_text(v) for v in spec.canvas.extent_mm())
    _remove(runner, out)
    runner.run(
        "gdal_rasterize", "-of", "ENVI", "-ot", "Byte", "-init", str(WATER_SEA), "-burn", str(WATER_LAND),
        "-a_srs", proj_string(spec), "-te", xmin, ymin, xmax, ymax,
        "-ts", str(spec.canvas.width), str(spec.canvas.height), str(vectors["area"]), str(out),
    )  # fmt: skip
    runner.run("gdal_rasterize", "-burn", str(WATER_SEA), str(vectors["ocean"]), str(out))
    runner.run("gdal_rasterize", "-burn", str(WATER_LAND), str(vectors["minor_islands"]), str(out))
    runner.run("gdal_rasterize", "-burn", str(WATER_LAKE), str(vectors["lakes"]), str(out))


def _file_record(path: Path, **extra: object) -> dict:
    digest, size = fetch.sha256_file(path)
    return {"path": path.name, "sha256": digest, "size": size, **extra}


def run_projection(
    runner: Runner,
    spec: ProjectionSpec,
    dem: SourceFiles,
    ne: SourceFiles,
    work: Path,
    manifest_version: int,
) -> Path:
    out_dir = work / spec.id
    tmp = out_dir / "tmp"
    if not runner.dry_run:
        tmp.mkdir(parents=True, exist_ok=True)
        _remove(runner, out_dir / METADATA_FILE)  # an interrupted run must not leave a stale, valid-looking result
    runner.log(f"== {spec.id}: {spec.title}; {proj_string(spec)}")

    build_dem(runner, dem, spec, tmp, out_dir / "height.i16")

    vectors: dict[str, Path] = {}
    area_src = tmp / "area_wgs84.geojson"
    if not runner.dry_run:
        area_src.write_text(area_geojson(), encoding="utf-8")
    for role, stem in NE_LAYERS.items():
        dataset, layer = ne_dataset(ne, stem)
        fields = "scalerank,name" if role in {"rivers"} else "scalerank"
        vectors[role] = tmp / (f"{role}.geojson" if role in {"coastline", "rivers"} else f"{role}.gpkg")
        project_vector(runner, dataset, layer, spec, vectors[role], fields, area_src)
    vectors["area"] = tmp / "area.gpkg"
    _remove(runner, vectors["area"])
    runner.run(
        "ogr2ogr", "-f", "GPKG", "-nln", "area", "-t_srs", proj_string(spec), str(vectors["area"]), str(area_src),
    )  # fmt: skip
    build_water(runner, spec, vectors, out_dir / "water.u8")
    if runner.dry_run:
        return out_dir

    for role, kind, name in (("coastline", "coastline", "coastline.pgl"), ("rivers", "river", "rivers.pgl")):
        data = json.loads(vectors[role].read_text(encoding="utf-8"), parse_float=Decimal)
        lines = geojson_polylines(data, spec.canvas)
        (out_dir / name).write_text(format_pgl(kind, spec.canvas, lines), encoding="ascii", newline="\n")

    meta = build_metadata(spec, dem, ne, manifest_version, runner.tools.versions() if runner.tools else {})
    meta["files"] = {
        "height": _file_record(out_dir / "height.i16", type="int16le", unit="m", nodata=HEIGHT_NODATA),
        "water": _file_record(out_dir / "water.u8", type="uint8", codes={str(k): v for k, v in WATER_CODES.items()}),
        "coastline": _file_record(out_dir / "coastline.pgl", type="pgl1"),
        "rivers": _file_record(out_dir / "rivers.pgl", type="pgl1"),
    }
    (out_dir / METADATA_FILE).write_text(json.dumps(meta, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    return out_dir


def build_metadata(
    spec: ProjectionSpec, dem: SourceFiles, ne: SourceFiles, manifest_version: int, tools: Mapping[str, object]
) -> dict:
    canvas = spec.canvas
    steps = [
        ("mosaic:glo90-v1", {"antimeridian_shift_deg": 360, "resolution": "highest"}),
        ("reproject:" + spec.id + "-v1", {"proj": proj_string(spec), "resampling": "average", "et": 0}),
        ("resample:" + f"{canvas.width}x{canvas.height}", {"pixel_size_mm": canvas.pixel_size_mm,
                                                           "center_mm": [canvas.center_x_mm, canvas.center_y_mm]}),
        (
            "water:ne10m-v2",
            {
                "order": ["init=1", "area=0", "ocean=1", "minor_islands=0", "lakes=2"],
                "clip_segment_deg": AREA_SEGMENT_DEG,
                "clip": clip_wkt(),
                "area_segment_deg": AREA_SEGMENT_DEG,
            },
        ),
        ("lines:pgl1-v1", {"subpixel": SUBPIXEL, "coordinate_precision_m": 1}),
    ]  # fmt: skip
    return {
        "format": FORMAT_NAME,
        "format_version": FORMAT_VERSION,
        "projection_id": spec.id,
        "width": canvas.width,
        "height": canvas.height,
        "vertical_datum": "EGM2008 (Copernicus DEM heights, metres)",
        "area": {
            "west_udeg": MAP_AREA.west_udeg,
            "south_udeg": MAP_AREA.south_udeg,
            "east_udeg": MAP_AREA.east_udeg,
            "north_udeg": MAP_AREA.north_udeg,
        },
        "georef": geor_fields(spec),
        "provenance": {
            "manifest_version": manifest_version,
            "sources": [
                {"id": s.id, "version": s.version, "files": [{"path": p, "sha256": h} for p, h, _ in s.files]}
                for s in (dem, ne)
            ],
            "steps": [{"id": step_id, "params_hash": params_hash(params)} for step_id, params in steps],
            "tools": dict(tools),
        },
    }


# --------------------------------------------------------------------------------------
# Frame check
# --------------------------------------------------------------------------------------

# Points that must lie on the canvas (Russia of 1703 and its edges) and background points of
# neighbouring powers that should (reported, not required). Coordinates are approximate.
FRAME_POINTS = [
    ("kaliningrad", 20.5, 54.7, True),
    ("kyiv", 30.5, 50.45, True),
    ("astrakhan", 48.0, 46.35, True),
    ("rudolf-island", 58.0, 81.8, True),
    ("cape-chelyuskin", 104.3, 77.7, True),
    ("wrangel-island", -179.5, 71.2, True),
    ("cape-dezhnev", -169.7, 66.1, True),
    ("cape-lopatka", 156.7, 50.9, True),
    ("vladivostok", 131.9, 43.1, True),
    ("stockholm", 18.1, 59.3, False),
    ("istanbul", 29.0, 41.0, False),
    ("isfahan", 51.7, 32.6, False),
    ("beijing", 116.4, 39.9, False),
]


def frame_report(projected: Sequence[tuple[float, float]], canvas: Canvas) -> list[tuple[str, bool, bool, int]]:
    """(name, required, inside, margin_px) per FRAME_POINTS entry; margin is the distance to the nearest edge."""
    xmin, ymin, xmax, ymax = canvas.extent_mm()
    result = []
    for (name, _, _, required), (x, y) in zip(FRAME_POINTS, projected):
        x_mm, y_mm = x * 1000, y * 1000
        margin = min(x_mm - xmin, xmax - x_mm, y_mm - ymin, ymax - y_mm)
        result.append((name, required, margin >= 0, math.floor(margin / canvas.pixel_size_mm)))
    return result


def project_points(tools: Tools, spec: ProjectionSpec) -> list[tuple[float, float]]:
    stdin = "".join(f"{lon} {lat}\n" for _, lon, lat, _ in FRAME_POINTS)
    argv = [tools.exe("gdaltransform"), "-s_srs", WGS84_LONLAT, "-t_srs", proj_string(spec), "-output_xy"]
    result = subprocess.run(argv, input=stdin, capture_output=True, text=True, check=False, env=tools.env)
    if result.returncode != 0:
        raise Stage1Error(f"gdaltransform failed: {result.stderr.strip()}")
    points = [tuple(float(v) for v in line.split()[:2]) for line in result.stdout.splitlines() if line.strip()]
    if len(points) != len(FRAME_POINTS):
        raise Stage1Error("gdaltransform returned an unexpected number of points")
    return points  # type: ignore[return-value]


# --------------------------------------------------------------------------------------
# Command line
# --------------------------------------------------------------------------------------


def _select_projections(names: Sequence[str] | None) -> list[ProjectionSpec]:
    if not names or "all" in names:
        return list(PROJECTIONS.values())
    unknown = [n for n in names if n not in PROJECTIONS]
    if unknown:
        raise Stage1Error(f"unknown projection(s): {', '.join(unknown)}; choose from {', '.join(PROJECTIONS)}")
    return [PROJECTIONS[n] for n in names]


def cmd_check_tools(args: argparse.Namespace) -> int:
    tools = find_tools(args.gdal_bin, args.allow_other_gdal)
    versions = tools.versions()
    print(f"GDAL {versions['gdal']} and PROJ {versions['proj']}: OK")
    for name in (*GDAL_TOOLS, PROJ_TOOL):
        print(f"  {name}: {tools.paths[name]}")
    return 0


def cmd_projections(args: argparse.Namespace) -> int:
    for spec in _select_projections(args.projection):
        canvas = spec.canvas
        xmin, ymin, xmax, ymax = (mm_text(v) for v in canvas.extent_mm())
        print(f"{spec.id}: {spec.title}")
        print(f"  {proj_string(spec)}")
        print(f"  canvas {canvas.width}x{canvas.height}, pixel {mm_text(canvas.pixel_size_mm)} m, "
              f"extent x {xmin}..{xmax} m, y {ymin}..{ymax} m")  # fmt: skip
    return 0


def cmd_tiles(args: argparse.Namespace) -> int:
    existing = None
    if args.tile_list:
        existing = Path(args.tile_list).read_text(encoding="utf-8").splitlines()
    entries = tile_manifest_entries(MAP_AREA, existing)
    print(json.dumps(entries, indent=2))
    print(f"{len(entries)} tile(s)", file=sys.stderr)
    return 0


def cmd_frame(args: argparse.Namespace) -> int:
    tools = find_tools(args.gdal_bin, args.allow_other_gdal)
    status = 0
    for spec in _select_projections(args.projection):
        print(f"{spec.id}:")
        for name, required, inside, margin in frame_report(project_points(tools, spec), spec.canvas):
            tag = "ok" if inside else ("OUTSIDE" if required else "outside (background)")
            print(f"  {name:16} {tag:22} margin {margin} px")
            if required and not inside:
                status = 1
    return status


def cmd_run(args: argparse.Namespace) -> int:
    specs = _select_projections(args.projection)
    cache = fetch.resolve_cache(args.cache)
    work = fetch.resolve_cache(args.work, env_name=WORK_ENV, default_name="geodata-work", what="work")
    manifest, errors = geo_manifest.load_manifest(Path(args.manifest))
    if errors:
        raise Stage1Error("; ".join(errors))
    errors = geo_manifest.validate_manifest(manifest)
    if errors:
        raise Stage1Error("manifest is invalid: " + "; ".join(errors[:5]))
    dem = source_files(manifest, DEM_SOURCE, cache, args.allow_unverified)
    ne = source_files(manifest, NE_SOURCE, cache, args.allow_unverified)
    if not args.dry_run:
        missing = [str(local) for s in (dem, ne) for _, _, local in s.files if not local.is_file()]
        if missing:
            raise Stage1Error(
                f"{len(missing)} source file(s) are not in the cache, e.g. {missing[0]}; run tools/geo/fetch.py first"
            )
    tools = None if args.dry_run else find_tools(args.gdal_bin, args.allow_other_gdal)
    runner = Runner(tools, args.dry_run)
    status = 0
    for spec in specs:
        out_dir = run_projection(runner, spec, dem, ne, work, manifest["manifest_version"])
        if args.dry_run:
            continue
        report = validate_dir(out_dir)
        _print_report(out_dir, report)
        if report.errors:
            status = 1
    return status


def _print_report(out_dir: Path, report: Report) -> None:
    for message in report.errors:
        print(f"ERROR: {message}", file=sys.stderr)
    for message in report.warnings:
        print(f"WARNING: {message}", file=sys.stderr)
    stats = ", ".join(f"{k}={v}" for k, v in report.stats.items())
    verdict = "FAILED" if report.errors else "OK"
    print(f"{out_dir}: {verdict} ({stats})")


def cmd_validate(args: argparse.Namespace) -> int:
    report = validate_dir(Path(args.dir), check_hashes=not args.no_hashes)
    _print_report(Path(args.dir), report)
    return 1 if report.errors else 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)

    def gdal_opts(p: argparse.ArgumentParser) -> None:
        p.add_argument("--gdal-bin", help=f"directory with the GDAL/PROJ programs (default: PATH or {GDAL_BIN_ENV})")
        p.add_argument("--allow-other-gdal", action="store_true", help="run with a GDAL version other than the pinned one")

    def proj_opt(p: argparse.ArgumentParser) -> None:
        p.add_argument("--projection", action="append", help=f"{', '.join(PROJECTIONS)} or all (default: all)")

    p = sub.add_parser("check-tools", help="verify the GDAL/PROJ installation")
    gdal_opts(p)
    p.set_defaults(func=cmd_check_tools)

    p = sub.add_parser("projections", help="list the candidate projections")
    proj_opt(p)
    p.set_defaults(func=cmd_projections)

    p = sub.add_parser("tiles", help="print manifest file entries for the GLO-90 tiles of the map area")
    p.add_argument("--tile-list", help="local copy of the bucket tile list (one tile name per line)")
    p.set_defaults(func=cmd_tiles)

    p = sub.add_parser("frame", help="check that reference points fit the canvas")
    gdal_opts(p)
    proj_opt(p)
    p.set_defaults(func=cmd_frame)

    p = sub.add_parser("run", help="build intermediate files")
    gdal_opts(p)
    proj_opt(p)
    p.add_argument("--manifest", default=str(geo_manifest.DEFAULT_MANIFEST))
    p.add_argument("--cache", help=f"source cache (default: {fetch.CACHE_ENV} or ~/.patriarch/geodata-cache)")
    p.add_argument("--work", help=f"output directory (default: {WORK_ENV} or ~/.patriarch/geodata-work)")
    p.add_argument("--allow-unverified", action="store_true", help="use sources that are not marked ready (trial runs)")
    p.add_argument("--dry-run", action="store_true", help="print the commands without running them")
    p.set_defaults(func=cmd_run)

    p = sub.add_parser("validate", help="check a directory of intermediate files")
    p.add_argument("dir")
    p.add_argument("--no-hashes", action="store_true", help="skip SHA-256 verification")
    p.set_defaults(func=cmd_validate)
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        return args.func(args)
    except (Stage1Error, ValueError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
