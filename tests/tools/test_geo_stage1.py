"""Tests for tools/geo/stage1.py: pure logic only (no GDAL/PROJ, no network)."""

import array
import contextlib
import hashlib
import importlib.util
import io
import json
import sys
import tempfile
import unittest
from decimal import Decimal
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT = REPO_ROOT / "tools" / "geo" / "stage1.py"
spec = importlib.util.spec_from_file_location("geo_stage1_under_test", SCRIPT)
stage1 = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = stage1  # dataclasses looks the module up by name
spec.loader.exec_module(stage1)

SMALL = stage1.Canvas(width=8, height=4, pixel_size_mm=1_000_000, center_x_mm=0, center_y_mm=0)


class ProjectionTests(unittest.TestCase):
    def test_udeg_and_mm_text_are_exact(self):
        self.assertEqual(stage1.udeg_text(58_000_000), "58")
        self.assertEqual(stage1.udeg_text(47_500_000), "47.5")
        self.assertEqual(stage1.udeg_text(-169_700_000), "-169.7")
        self.assertEqual(stage1.udeg_text(1), "0.000001")
        self.assertEqual(stage1.mm_text(1_300_000), "1300")
        self.assertEqual(stage1.mm_text(-1500), "-1.5")

    def test_candidates_match_geor_codes_of_mapdata(self):
        # pat::mapdata::Projection: albers = 1, lambert_conformal = 2, lambert_azimuthal = 3
        self.assertEqual({k: v.geor_code for k, v in stage1.PROJECTIONS.items()}, {"albers": 1, "lcc": 2, "laea": 3})

    def test_proj_strings(self):
        self.assertEqual(
            stage1.proj_string(stage1.PROJECTIONS["albers"]),
            "+proj=aea +lat_0=58 +lon_0=100 +lat_1=47 +lat_2=69 +x_0=0 +y_0=0 +datum=WGS84 +units=m +no_defs +type=crs",
        )
        laea = stage1.proj_string(stage1.PROJECTIONS["laea"])
        self.assertIn("+proj=laea", laea)
        self.assertNotIn("+lat_1", laea)

    def test_canvas_extent_and_origin(self):
        canvas = stage1.Canvas(width=8192, height=4096, pixel_size_mm=1_300_000, center_y_mm=680_000_000)
        xmin, ymin, xmax, ymax = canvas.extent_mm()
        self.assertEqual((xmax - xmin) // canvas.pixel_size_mm, 8192)
        self.assertEqual((ymax - ymin) // canvas.pixel_size_mm, 4096)
        self.assertEqual((xmin + xmax) // 2, 0)
        self.assertEqual((ymin + ymax) // 2, 680_000_000)
        self.assertEqual(canvas.origin_x_mm, xmin)
        self.assertEqual(canvas.origin_y_mm, ymax)
        with self.assertRaises(ValueError):
            stage1.Canvas(width=7)

    def test_geor_fields_fit_the_mapdata_validator(self):
        geor = stage1.geor_fields(stage1.PROJECTIONS["lcc"])
        self.assertEqual(geor["projection"], 2)
        self.assertEqual(geor["datum"], 1)
        self.assertEqual(geor["pixel_size_x_mm"], geor["pixel_size_y_mm"])
        # The map area crosses 180 E; GEOR needs -180 <= min < max <= 180 (physical_layer.cpp).
        self.assertEqual((geor["bbox_min_lon_udeg"], geor["bbox_max_lon_udeg"]), (-180_000_000, 180_000_000))
        self.assertLess(geor["bbox_min_lat_udeg"], geor["bbox_max_lat_udeg"])
        for key, value in geor.items():
            if key.endswith(("_udeg", "_mm")):
                self.assertIsInstance(value, int, key)
        plain = stage1.GeoBox(10_000_000, 40_000_000, 50_000_000, 60_000_000)
        geor = stage1.geor_fields(stage1.PROJECTIONS["albers"], plain)
        self.assertEqual((geor["bbox_min_lon_udeg"], geor["bbox_max_lon_udeg"]), (10_000_000, 50_000_000))

    def test_clip_wkt_splits_at_antimeridian(self):
        self.assertEqual(
            stage1.clip_wkt(),
            "MULTIPOLYGON(((15 28, 180 28, 180 82, 15 82, 15 28)), ((-180 28, -165 28, -165 82, -180 82, -180 28)))",
        )
        self.assertTrue(stage1.clip_wkt(stage1.GeoBox(0, 0, 10_000_000, 10_000_000)).startswith("POLYGON(("))

    def test_params_hash_is_canonical_fnv1a(self):
        self.assertEqual(stage1.params_hash({}), "0x" + format(_fnv1a64(b"{}"), "016x"))
        self.assertEqual(stage1.params_hash({"a": 1, "b": 2}), stage1.params_hash({"b": 2, "a": 1}))
        self.assertNotEqual(stage1.params_hash({"a": 1}), stage1.params_hash({"a": 2}))


def _fnv1a64(data: bytes) -> int:
    value = 0xCBF29CE484222325
    for byte in data:
        value = ((value ^ byte) * 0x100000001B3) % (1 << 64)
    return value


class TileTests(unittest.TestCase):
    def test_tile_names_round_trip(self):
        self.assertEqual(stage1.tile_name(55, 37), "Copernicus_DSM_COG_30_N55_00_E037_00_DEM")
        self.assertEqual(stage1.tile_name(66, -170), "Copernicus_DSM_COG_30_N66_00_W170_00_DEM")
        self.assertEqual(stage1.tile_name(-1, -1), "Copernicus_DSM_COG_30_S01_00_W001_00_DEM")
        for cell in [(55, 37), (66, -170), (-1, -1), (0, 0)]:
            self.assertEqual(stage1.parse_tile_name(stage1.tile_name(*cell) + ".tif"), cell)
        self.assertIsNone(stage1.parse_tile_name("ne_10m_ocean.zip"))

    def test_tiles_for_area_wrap_the_antimeridian(self):
        cells = stage1.tiles_for_area()
        self.assertEqual(len(cells), (82 - 28) * (195 - 15))
        self.assertEqual(len(set(cells)), len(cells))
        lons = {lon for _, lon in cells}
        self.assertEqual(min(lons), -180)
        self.assertIn(179, lons)
        self.assertIn(-166, lons)
        self.assertNotIn(-165, lons)
        self.assertEqual(cells[0], (81, 15))  # north-west first

    def test_manifest_entries_filtered_by_tile_list(self):
        area = stage1.GeoBox(37_000_000, 55_000_000, 39_000_000, 56_000_000)
        listing = ["Copernicus_DSM_COG_30_N55_00_E038_00_DEM", ""]
        entries = stage1.tile_manifest_entries(area, listing)
        self.assertEqual(len(entries), 1)
        name = "Copernicus_DSM_COG_30_N55_00_E038_00_DEM"
        self.assertEqual(entries[0]["path"], f"{name}.tif")
        self.assertEqual(entries[0]["url"], f"https://copernicus-dem-90m.s3.amazonaws.com/{name}/{name}.tif")
        self.assertIsNone(entries[0]["sha256"])
        self.assertEqual(len(stage1.tile_manifest_entries(area)), 2)

    def test_shifted_ullr_moves_east_exactly(self):
        info = {"geoTransform": [-180.0, 0.000833333333333, 0.0, 67.0, 0.0, -0.000833333333333], "size": [1200, 2400]}
        ulx, uly, lrx, lry = (Decimal(v) for v in stage1.shifted_ullr(info))
        self.assertEqual(ulx, Decimal(180))
        self.assertEqual(uly, Decimal(67))
        self.assertAlmostEqual(float(lrx), 181.0, places=6)
        self.assertAlmostEqual(float(lry), 65.0, places=6)


class VersionTests(unittest.TestCase):
    def test_parse_versions(self):
        self.assertEqual(stage1.parse_gdal_version("GDAL 3.13.3, released 2026/10/01"), (3, 13, 3))
        self.assertEqual(stage1.parse_proj_version("Rel. 9.9.0, September 1st, 2026\nusage: proj"), (9, 9, 0))
        self.assertIsNone(stage1.parse_gdal_version("command not found"))

    def test_version_policy(self):
        self.assertEqual(stage1.version_problems((3, 13, 3), (9, 9, 0), False), [])
        self.assertEqual(len(stage1.version_problems((3, 12, 0), (9, 9, 0), False)), 1)
        self.assertEqual(stage1.version_problems((3, 12, 0), (9, 9, 0), True), [])
        self.assertEqual(len(stage1.version_problems((3, 13, 0), (9, 4, 0), False)), 1)
        self.assertEqual(len(stage1.version_problems(None, None, True)), 2)

    def test_tool_search_order(self):
        home = Path("/home/u")
        self.assertEqual(stage1.candidate_bin_dirs("/opt/gdal", {}, home, False), [("--gdal-bin", Path("/opt/gdal"))])
        env = {stage1.GEO_PYTHON_ENV: "C:/m/envs/patriarch-geo/python.exe"}
        self.assertEqual(
            stage1.candidate_bin_dirs(None, env, home, True),
            [(stage1.GEO_PYTHON_ENV, Path("C:/m/envs/patriarch-geo/Library/bin"))],
        )
        env = {stage1.GDAL_BIN_ENV: "/x", stage1.GEO_PYTHON_ENV: "/y/python"}
        self.assertEqual(stage1.candidate_bin_dirs(None, env, home, False), [(stage1.GDAL_BIN_ENV, Path("/x"))])
        default = stage1.candidate_bin_dirs(None, {}, home, False)
        self.assertEqual(default[0], ("PATH", None))
        self.assertIn(("~/miniforge3", home / "miniforge3" / "envs" / "patriarch-geo" / "bin"), default)

    def test_data_env_for_unactivated_conda(self):
        with tempfile.TemporaryDirectory() as tmp:
            prefix = Path(tmp)
            (prefix / "Library" / "share" / "proj").mkdir(parents=True)
            (prefix / "Library" / "bin").mkdir()
            env = stage1.data_env(prefix / "Library" / "bin", {"PATH": "base"})
            self.assertEqual(env["PROJ_DATA"], str(prefix / "Library" / "share" / "proj"))
            self.assertNotIn("GDAL_DATA", env)
            self.assertTrue(env["PATH"].endswith("base"))
            self.assertIsNone(stage1.data_env(prefix / "Library" / "bin", {"PROJ_DATA": "set"}))
            self.assertIsNone(stage1.data_env(None, {}))


class ManifestTests(unittest.TestCase):
    MANIFEST = {
        "sources": [
            {"id": "dem", "status": "unverified", "version": "2021", "files": [{"path": "a.tif", "sha256": None}]},
            {"id": "ne", "status": "ready", "version": None, "version_date": "2026-01-01",
             "files": [{"path": "ne_10m_ocean.zip", "sha256": "f" * 64}, {"path": "sub/ne_10m_lakes.shp", "sha256": None}]},
            {"id": "gone", "status": "rejected", "files": []},
            {"id": "empty", "status": "ready", "files": []},
        ]
    }  # fmt: skip

    def test_status_rules(self):
        cache = Path("/cache")
        with self.assertRaises(stage1.Stage1Error):
            stage1.source_files(self.MANIFEST, "dem", cache, allow_unverified=False)
        dem = stage1.source_files(self.MANIFEST, "dem", cache, allow_unverified=True)
        self.assertEqual(dem.files, [("a.tif", None, cache / "dem" / "a.tif")])
        for source_id in ("gone", "empty", "missing"):
            with self.assertRaises(stage1.Stage1Error):
                stage1.source_files(self.MANIFEST, source_id, cache, allow_unverified=True)
        self.assertEqual(stage1.source_files(self.MANIFEST, "ne", cache, False).version, "2026-01-01")

    def test_natural_earth_dataset_paths(self):
        ne = stage1.source_files(self.MANIFEST, "ne", Path("/cache"), False)
        dataset, layer = stage1.ne_dataset(ne, "ne_10m_ocean")
        self.assertEqual((dataset, layer), ("/vsizip//cache/ne/ne_10m_ocean.zip", "ne_10m_ocean"))
        dataset, _ = stage1.ne_dataset(ne, "ne_10m_lakes")
        self.assertTrue(dataset.endswith("/cache/ne/sub/ne_10m_lakes.shp"))
        with self.assertRaises(stage1.Stage1Error):
            stage1.ne_dataset(ne, "ne_10m_coastline")


class PolylineTests(unittest.TestCase):
    def test_slug_key(self):
        self.assertEqual(stage1.slug_key("Volga"), "volga")
        self.assertEqual(stage1.slug_key("Ob' / Irtysh"), "ob-irtysh")
        self.assertEqual(stage1.slug_key("Dnipro (Dnepr)"), "dnipro-dnepr")
        self.assertEqual(stage1.slug_key("Élan"), "elan")
        self.assertEqual(stage1.slug_key("Волга"), "-")
        self.assertEqual(stage1.slug_key(None), "-")
        self.assertLessEqual(len(stage1.slug_key("x" * 40)), stage1.KEY_MAX)

    def test_to_subpixel_rounds_half_up(self):
        # SMALL: origin x = -4000 m, origin y = +2000 m, 1000 m per pixel
        self.assertEqual(stage1.to_subpixel(Decimal("-4000"), Decimal("2000"), SMALL), (0, 0))
        self.assertEqual(stage1.to_subpixel(Decimal("0"), Decimal("0"), SMALL), (64, 32))
        self.assertEqual(stage1.to_subpixel(Decimal("-3968.75"), Decimal("2000"), SMALL), (1, 0))  # 0.5 -> 1
        self.assertEqual(stage1.to_subpixel(Decimal("-3968.8"), Decimal("1968.7"), SMALL), (0, 1))

    def test_split_inside(self):
        points = [(0, 0), (0, 0), (5, 5), (999, 5), (6, 6), (7, 7), (8, 8), (-1, 0), (1, 1)]
        self.assertEqual(stage1.split_inside(points, 8, 4), [[(0, 0), (5, 5)], [(6, 6), (7, 7), (8, 8)]])

    def test_geojson_to_pgl_round_trip(self):
        data = {
            "features": [
                {"properties": {"scalerank": 3, "name": "Volga"},
                 "geometry": {"type": "LineString", "coordinates": [[Decimal(-4000), Decimal(2000)], [Decimal(0), Decimal(0)]]}},
                {"properties": {"scalerank": None},
                 "geometry": {"type": "MultiLineString", "coordinates": [[[Decimal(0), Decimal(0)], [Decimal(1000), Decimal(0)]]]}},
                {"properties": {}, "geometry": {"type": "Polygon", "coordinates": []}},
            ]
        }  # fmt: skip
        lines = stage1.geojson_polylines(data, SMALL)
        self.assertEqual([(l.rank, l.key, l.points) for l in lines], [(3, "volga", [(0, 0), (64, 32)]), (0, "-", [(64, 32), (80, 32)])])
        text = stage1.format_pgl("river", SMALL, lines)
        self.assertTrue(text.startswith("PGL1 river 8 4 16 2\nL 1 3 2 volga\n0 0\n64 32\n"))
        head, parsed = stage1.parse_pgl(text)
        self.assertEqual(head, {"kind": "river", "width": 8, "height": 4})
        self.assertEqual([l.points for l in parsed], [l.points for l in lines])

    def test_parse_pgl_rejects_bad_files(self):
        good = "PGL1 coastline 8 4 16 1\nL 1 0 2 -\n0 0\n1 1\n"
        stage1.parse_pgl(good)
        for bad in (
            "",
            "PGL2 coastline 8 4 16 1\n",
            "PGL1 lake 8 4 16 0\n",
            good.replace("L 1 0 2", "L 2 0 2"),
            good.replace("1 1\n", "999 1\n"),
            good.replace("L 1 0 2", "L 1 0 1").replace("1 1\n", ""),
            good + "5 5\n",
            good.replace("\n1 1\n", "\n"),
        ):
            with self.assertRaises(ValueError, msg=repr(bad)):
                stage1.parse_pgl(bad)


def _write_output(directory: Path, heights, water, *, width=8, height=4, tamper=None) -> None:
    """Write a tiny but complete stage-1 output directory."""
    h = array.array("h", heights)
    if sys.byteorder != "little":
        h.byteswap()
    (directory / "height.i16").write_bytes(h.tobytes())
    (directory / "water.u8").write_bytes(bytes(water))
    canvas = stage1.Canvas(width=width, height=height, pixel_size_mm=1_000_000)
    line = [stage1.Polyline(0, "-", [(0, 0), (16, 16)])]
    (directory / "coastline.pgl").write_text(stage1.format_pgl("coastline", canvas, line), encoding="ascii")
    (directory / "rivers.pgl").write_text(stage1.format_pgl("river", canvas, line), encoding="ascii")
    files = {}
    for role, name in (("height", "height.i16"), ("water", "water.u8"), ("coastline", "coastline.pgl"), ("rivers", "rivers.pgl")):
        data = (directory / name).read_bytes()
        files[role] = {"path": name, "sha256": hashlib.sha256(data).hexdigest(), "size": len(data)}
    meta = {
        "format": stage1.FORMAT_NAME,
        "format_version": stage1.FORMAT_VERSION,
        "width": width,
        "height": height,
        "georef": {"pixel_size_x_mm": 1_000_000, "pixel_size_y_mm": 1_000_000},
        "files": files,
    }  # fmt: skip
    if tamper:
        tamper(meta)
    (directory / stage1.METADATA_FILE).write_text(json.dumps(meta), encoding="utf-8")


class ValidateTests(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self._tmp.name)

    def tearDown(self):
        self._tmp.cleanup()

    def heights(self):
        return [-32768] * 8 + [10, 20, 30, 40, 50, 60, 70, 80] * 3  # first row is sea without DEM data

    def water(self):
        return [1] * 8 + [0] * 22 + [2, 2]

    def test_valid_output(self):
        _write_output(self.dir, self.heights(), self.water())
        report = stage1.validate_dir(self.dir)
        self.assertEqual(report.errors, [])
        self.assertEqual(report.warnings, [])
        self.assertEqual(report.stats["height_min_m"], 10)
        self.assertEqual(report.stats["sea_px"], 8)
        self.assertEqual(report.stats["lake_px"], 2)
        self.assertEqual(report.stats["land_holes_px"], 0)

    def test_holes_on_land(self):
        heights = self.heights()
        heights[10] = -32768
        _write_output(self.dir, heights, self.water())
        report = stage1.validate_dir(self.dir)
        # 1 hole in 22 land pixels is above the 1% limit
        self.assertTrue(any("without DEM data" in e for e in report.errors))

    def test_bad_values_and_sizes(self):
        water = self.water()
        water[9] = 7
        heights = self.heights()
        heights[12] = 9500
        _write_output(self.dir, heights, water)
        errors = stage1.validate_dir(self.dir).errors
        self.assertTrue(any("codes outside" in e for e in errors))
        self.assertTrue(any("outside -450..9000" in e for e in errors))

        _write_output(self.dir, self.heights()[:-1], self.water())
        self.assertTrue(any("expected 64 bytes" in e for e in stage1.validate_dir(self.dir).errors))

    def test_hash_and_metadata_checks(self):
        _write_output(self.dir, self.heights(), self.water())
        (self.dir / "water.u8").write_bytes(bytes([1] * 32))
        self.assertTrue(any("sha256 differs" in e for e in stage1.validate_dir(self.dir).errors))
        self.assertEqual(stage1.validate_dir(self.dir, check_hashes=False).errors, [])

        _write_output(self.dir, self.heights(), self.water(), tamper=lambda m: m.update(format_version=99))
        self.assertTrue(stage1.validate_dir(self.dir).errors)
        _write_output(self.dir, self.heights(), self.water(), tamper=lambda m: m["files"].pop("rivers"))
        self.assertTrue(any("files.rivers" in e for e in stage1.validate_dir(self.dir).errors))
        (self.dir / stage1.METADATA_FILE).unlink()
        self.assertTrue(stage1.validate_dir(self.dir).errors)

    def test_envi_byte_order(self):
        _write_output(self.dir, self.heights(), self.water())
        (self.dir / "height.hdr").write_text("ENVI\nbyte order = 1\n", encoding="ascii")
        self.assertTrue(any("byte order" in e for e in stage1.validate_dir(self.dir).errors))


class CommandLineTests(unittest.TestCase):
    def run_main(self, *argv):
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = stage1.main(list(argv))
        return code, out.getvalue(), err.getvalue()

    def test_projections_and_tiles_need_no_gdal(self):
        code, out, _ = self.run_main("projections", "--projection", "lcc")
        self.assertEqual(code, 0)
        self.assertIn("+proj=lcc", out)
        code, out, _ = self.run_main("tiles")
        self.assertEqual(code, 0)
        self.assertEqual(len(json.loads(out)), (82 - 28) * (195 - 15))

    def test_unknown_projection(self):
        code, _, err = self.run_main("projections", "--projection", "mercator")
        self.assertEqual(code, 2)
        self.assertIn("unknown projection", err)

    def test_run_refuses_work_dir_inside_repo(self):
        code, _, err = self.run_main("run", "--dry-run", "--work", str(REPO_ROOT / "geodata-work"))
        self.assertEqual(code, 2)
        self.assertIn("outside the repository", err)

    def test_dry_run_prints_the_pipeline(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            manifest = json.loads((REPO_ROOT / "data-sources" / "geodata.manifest.json").read_text(encoding="utf-8"))
            by_id = {e["id"]: e for e in manifest["sources"]}
            by_id["copernicus-dem-glo90"]["files"] = stage1.tile_manifest_entries(
                stage1.GeoBox(179_000_000, 65_000_000, 181_000_000, 66_000_000)
            )
            by_id["natural-earth-10m-physical"]["files"] = [
                {"path": f"{stem}.zip", "url": f"https://example.org/{stem}.zip", "sha256": None, "size": None}
                for stem in stage1.NE_LAYERS.values()
            ]  # fmt: skip
            (root / "m").mkdir()
            path = root / "m" / "geodata.manifest.json"
            path.write_text(json.dumps(manifest), encoding="utf-8")
            args = ("run", "--dry-run", "--allow-unverified", "--projection", "albers",
                    "--manifest", str(path), "--cache", str(root / "cache"), "--work", str(root / "work"))  # fmt: skip
            code, out, err = self.run_main(*args)
            self.assertEqual(code, 0, err)
            self.assertIn("gdalwarp", out)
            self.assertIn("+lon_wrap=180", out)
            self.assertIn("-a_ullr", out)  # the W180 tile is shifted across the antimeridian
            self.assertEqual(out.count("ogr2ogr"), len(stage1.NE_LAYERS))
            self.assertEqual(out.count("gdal_rasterize"), 3)
            self.assertFalse((root / "work").exists())  # a dry run writes nothing

            code, _, err = self.run_main(*args[:2], *args[3:])
            self.assertEqual(code, 2)
            self.assertIn("not ready", err)


if __name__ == "__main__":
    unittest.main()
