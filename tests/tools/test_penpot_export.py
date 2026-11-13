"""Tests for tools/penpot_export.py on a tiny synthetic Penpot snapshot (the real file is tens of megabytes
and is never committed)."""

import contextlib
import importlib.util
import io
import json
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parents[2] / "tools" / "penpot_export.py"
spec = importlib.util.spec_from_file_location("penpot_export", SCRIPT)
pe = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pe)

ROOT = pe.ROOT_ID


def shape(sid, kind, name, parent, x, y, w, h, **extra):
    out = {
        "id": sid,
        "name": name,
        "type": kind,
        "x": x,
        "y": y,
        "width": w,
        "height": h,
        "rotation": 0,
        "selrect": {"x": x, "y": y, "width": w, "height": h},
        "parentId": parent,
        "frameId": parent,
    }
    out.update(extra)
    return out


def text_content(*paragraphs, fill="#f3ead6", **leaf):
    base = {
        "fontFamily": "PT Serif",
        "fontSize": "28",
        "fontWeight": "400",
        "lineHeight": "1.35",
        "letterSpacing": "0",
        "textAlign": "left",
        "textTransform": "none",
        "textDecoration": "none",
        "fontStyle": "normal",
        "fills": [{"fillColor": fill, "fillOpacity": 1}],
    }
    base.update(leaf)
    return {
        "type": "root",
        "verticalAlign": "top",
        "children": [
            {
                "type": "paragraph-set",
                "children": [
                    {"type": "paragraph", "children": [dict(base, text=t)]} for t in paragraphs
                ],
            }
        ],
    }


def make_snapshot():
    objs = {}
    ids = []

    def add(s):
        objs[s["id"]] = s
        return s["id"]

    # Board A at (1000, 500) 1920x1080.
    a = "A"
    children = []

    def child(s):
        children.append(add(s))

    child(shape("bg", "rect", "Panel", a, 1100, 600, 300, 200,
                fills=[{"fillColor": "#eee5d2", "fillOpacity": 1}],
                strokes=[{"strokeColor": "#c3b695", "strokeOpacity": 1, "strokeWidth": 1, "strokeAlignment": "inner"}],
                r1=2, r2=2, r3=2, r4=2))
    child(shape("corners", "rect", "Tab", a, 1100, 700, 80, 40,
                fills=[{"fillColor": "#365847", "fillOpacity": 0.5}], r1=4, r2=4, r3=0, r4=0, opacity=0.4))
    child(shape("hid", "rect", "Hidden thing", a, 0, 0, 10, 10, hidden=True, fills=[{"fillColor": "#000000"}]))
    child(shape("t1", "text", "Казна", a, 1110, 610, 135, 18,
                growType="auto-height", content=text_content("Казна")))
    child(shape("t2", "text", "186", a, 1110, 640, 135, 34, growType="fixed",
                content=text_content("68", fontSize="25", fontWeight="700", letterSpacing="2",
                                     textAlign="center", lineHeight="1.5", fill="#d2b77d")))
    child(shape("t3", "text", "Описание", a, 1110, 680, 200, 40, growType="auto-height",
                content=text_content("Первая строка", "Вторая \"строка\"")))
    # An icon group: one visible path, one hidden path, an extra circle that must be skipped.
    child(shape("icon", "group", "Icon / coin", a, 1120, 560, 24, 24, shapes=["p1", "p2", "c1"]))
    add(shape("p1", "path", "svg-path", "icon", None, None, None, None,
              selrect={"x": 1122, "y": 562, "width": 20, "height": 20},
              strokes=[{"strokeColor": "#d2b77d", "strokeOpacity": 1, "strokeWidth": 1.5,
                        "strokeCapStart": "round", "strokeCapEnd": "round"}], fills=[]))
    add(shape("p2", "path", "svg-path", "icon", 1, 1, 1, 1, hidden=True))
    add(shape("c1", "circle", "dot", "icon", 1125, 565, 4, 4))
    # Circle with shadow, rotation and a gradient.
    child(shape("circ", "circle", "Badge", a, 1300, 600, 20.666, 20.666, rotation=45,
                shadow=[{"style": "drop-shadow", "offsetX": 0, "offsetY": 4, "blur": 8, "spread": 0,
                         "color": {"color": "#000000", "opacity": 0.25}}],
                fills=[{"fillColorGradient": {"type": "linear", "startX": 0.5, "startY": 0, "endX": 0.5, "endY": 1,
                                              "stops": [{"color": "#ffffff", "opacity": 1, "offset": 0},
                                                        {"color": "#000000", "opacity": 0.5, "offset": 1}]}}]))
    child(shape("img", "rect", "Portrait", a, 1100, 800, 68, 84,
                fills=[{"fillOpacity": 1, "fillImage": {"id": "x", "name": "Patriarch v1 portrait"}}]))
    # Flex container with children.
    child(shape("flex", "frame", "Tabs", a, 1100, 900, 328, 46, layout="flex", layoutFlexDir="row",
                layoutWrapType="nowrap", layoutAlignItems="center", layoutJustifyContent="space-between",
                layoutAlignContent="stretch", layoutGap={"rowGap": 0, "columnGap": 14},
                layoutPadding={"p1": 1, "p2": 2, "p3": 3, "p4": 4}, layoutItemHSizing="fix", layoutItemVSizing="auto",
                shapes=["f1"]))
    add(shape("f1", "frame", "Tab / Обзор", "flex", 1100, 901, 77, 44, layoutItemHSizing="fill",
              layoutItemAlignSelf="center", layoutItemMargin={"m1": 0, "m2": 0, "m3": 0, "m4": 6},
              showContent=True, shapes=[]))
    # Component instance and a main instance.
    child(shape("inst", "frame", "Art / Shared corner", a, 1880, 1540, 24, 24, componentId="comp1",
                shapeRef="main1", shapes=[]))
    child(shape("main", "frame", "Button / Primary", a, 1500, 1000, 100, 40, componentId="comp2",
                mainInstance=True, shapes=[]))
    add(shape(a, "frame", "15 / Сохранить игру", ROOT, 1000, 500, 1920, 1080,
              fills=[{"fillColor": "#1b2523", "fillOpacity": 1}], shapes=children, componentId="compA",
              mainInstance=True))
    ids.append(a)
    # Board B: another top-level board, only to test lookup.
    add(shape("B", "frame", "МЕНЮ / Диалог", ROOT, 5000, 500, 700, 350, shapes=[]))
    add(shape("C", "frame", "15 / Сохранить игру — копия", ROOT, 5000, 900, 700, 350, shapes=[]))
    add(shape("D", "frame", "150 / Не путать", ROOT, 5000, 1300, 700, 350, shapes=[]))
    add(shape(ROOT, "frame", "Root Frame", None, 0, 0, 0, 0, shapes=["A", "B", "C", "D"]))
    return {
        "name": "Patriarch Design UX/UI",
        "data": {
            "pagesIndex": {
                "page-1": {"name": "00 · Сквозной прототип", "objects": objs},
                "page-2": {"name": "01 · Другая", "objects": {}},
            },
            "components": {
                "comp1": {"name": "corner", "path": "Art / Shared"},
                "comp2": {"name": "Primary", "path": "Button / Patriarch"},
                "compA": {"name": "Сохранить игру", "path": "15"},
            },
        },
    }


class FormattingTest(unittest.TestCase):
    def test_numbers(self):
        self.assertEqual(pe.num(3), "3")
        self.assertEqual(pe.num(3.0), "3")
        self.assertEqual(pe.num(74.66666666666788), "74.67")
        self.assertEqual(pe.num(1.5), "1.5")
        self.assertEqual(pe.num("1.35"), "1.35")
        self.assertEqual(pe.num(-12.6666), "-12.67")
        self.assertEqual(pe.num(0.004), "0")

    def test_strings_are_escaped(self):
        self.assertEqual(pe.quote('a"b\\c\nd'), '"a\\"b\\\\c\\nd"')
        self.assertEqual(pe.bare_or_quoted("inner"), "inner")
        self.assertEqual(pe.bare_or_quoted("space-between"), '"space-between"')

    def test_colours_with_opacity(self):
        self.assertEqual(pe.color_string("#aabbcc", 1), "#aabbcc")
        self.assertEqual(pe.color_string("#aabbcc", 0.38), "#aabbcc@0.38")


class ExportTest(unittest.TestCase):
    def setUp(self):
        self.snapshot = pe.Snapshot(make_snapshot())
        self.page = self.snapshot.page("00")
        self.exporter = pe.BoardExporter(self.snapshot, self.page)
        self.boards = self.exporter.top_level_boards()
        self.board = pe.find_board(self.boards, "15 / Сохранить игру")
        self.elements = self.exporter.export(self.board)
        self.lines = [pe.render_element(e) for e in self.elements]

    def line(self, needle):
        found = [ln for ln in self.lines if needle in ln]
        self.assertEqual(len(found), 1, f"{needle!r}: {found}")
        return found[0]

    def test_root_is_at_origin_and_children_are_relative(self):
        self.assertEqual(self.elements[0]["x"], 0)
        self.assertEqual(self.elements[0]["y"], 0)
        self.assertEqual((self.elements[0]["w"], self.elements[0]["h"]), (1920, 1080))
        self.assertIn('{ n = "Panel" t = rectangle x = 100 y = 100 w = 300 h = 200', self.line('"Panel"'))

    def test_root_main_instance_is_not_reported(self):
        self.assertNotIn("main =", self.lines[0])

    def test_fill_stroke_radius(self):
        line = self.line('"Panel"')
        self.assertIn('f = ["#eee5d2"]', line)
        self.assertIn('s = [{ c = "#c3b695" w = 1 a = inner }]', line)
        self.assertIn(" r = 2", line)

    def test_per_corner_radius_opacity_and_fill_alpha(self):
        line = self.line('"Tab"')
        self.assertIn('f = ["#365847@0.5"]', line)
        self.assertIn("rc = [4 4 0 0]", line)
        self.assertNotIn(" r = ", line)
        self.assertIn("op = 0.4", line)

    def test_hidden_layers_are_skipped(self):
        self.assertFalse([ln for ln in self.lines if "Hidden thing" in ln])

    def test_text_named_by_its_text_has_no_n(self):
        line = self.line('txt = "Казна"')
        self.assertTrue(line.startswith("    { t = text x = 110 y = 110"))
        self.assertIn('grow = "auto-height"', line)
        self.assertIn('ff = "PT Serif" fs = 28 fw = 400', line)
        self.assertNotIn(" lh = ", line)
        self.assertNotIn(" ta = ", line)

    def test_renamed_text_keeps_n_and_non_default_attributes(self):
        line = self.line('n = "186"')
        self.assertIn('txt = "68"', line)
        self.assertIn("fs = 25 fw = 700 lh = 1.5 ls = 2 ta = center", line)
        self.assertIn('f = ["#d2b77d"]', line)
        self.assertNotIn("grow", line)

    def test_multi_paragraph_text_and_escapes(self):
        line = self.line('n = "Описание"')
        self.assertIn('txt = "Первая строка\\nВторая \\"строка\\""', line)

    def test_icon_group_is_collapsed(self):
        line = self.line('"Icon / coin"')
        self.assertIn("paths = 1", line)
        self.assertIn('s = [{ c = "#d2b77d" w = 1.5 cap = round }]', line)
        self.assertEqual(sum("svg-path" in ln for ln in self.lines), 0)
        self.assertEqual(sum('"dot"' in ln for ln in self.lines), 0)

    def test_shadow_rotation_gradient(self):
        line = self.line('"Badge"')
        self.assertIn("t = ellipse", line)
        self.assertIn("w = 20.67 h = 20.67", line)
        self.assertIn("rot = 45", line)
        self.assertIn('sh = [{ st = "drop-shadow" x = 0 y = 4 b = 8 sp = 0 c = "#000000@0.25" }]', line)
        self.assertIn('grad = "linear"', line)
        self.assertIn('stops = ["#ffffff/0" "#000000@0.5/1"]', line)

    def test_image_fill(self):
        self.assertIn('f = ["img:Patriarch v1 portrait"]', self.line('"Portrait"'))

    def test_flex_container_and_layout_child(self):
        line = self.line('"Tabs"')
        self.assertIn(
            'flex = { dir = row wrap = nowrap ai = center jc = "space-between" ac = stretch rg = 0 cg = 14 '
            'pad = [1 2 3 4] hs = fix vs = auto }',
            line,
        )
        child = self.line('"Tab / Обзор"')
        self.assertIn('lc = { hs = fill as = center m = [0 0 0 6] }', child)
        self.assertIn("clip = 0", child)

    def test_components(self):
        self.assertIn('inst = "corner"', self.line('"Art / Shared corner"'))
        self.assertIn('main = "Button / Patriarch / Primary"', self.line('"Button / Primary"'))

    def test_board_block_header(self):
        text = pe.render_board("15 / Сохранить игру", self.elements)
        self.assertTrue(text.startswith('board "15 / Сохранить игру" {\n  w = 1920 h = 1080\n  el = [\n'))
        self.assertTrue(text.endswith("  ]\n}"))

    def test_unrounded_geometry_of_paths_comes_from_selrect(self):
        paths = [e for e in self.exporter.visible_paths(self.exporter.objects["icon"])]
        self.assertEqual(len(paths), 1)
        self.assertEqual(pe.geometry(paths[0]), (1122.0, 562.0, 20.0, 20.0))


class LookupTest(unittest.TestCase):
    def setUp(self):
        self.snapshot = pe.Snapshot(make_snapshot())
        self.boards = pe.BoardExporter(self.snapshot, self.snapshot.page("00")).top_level_boards()

    def test_by_number_exact_name_and_prefix(self):
        self.assertEqual(pe.find_board(self.boards, "150")["id"], "D")
        self.assertEqual(pe.find_board(self.boards, "МЕНЮ / Диалог")["id"], "B")
        self.assertEqual(pe.find_board(self.boards, "МЕНЮ / ")["id"], "B")
        self.assertEqual(pe.find_board(self.boards, "15 / Сохранить игру")["id"], "A")

    def test_ambiguous_and_missing(self):
        with self.assertRaises(pe.SnapshotError):
            pe.find_board(self.boards, "15")  # two boards start with '15 / '
        with self.assertRaises(pe.SnapshotError):
            pe.find_board(self.boards, "99")

    def test_page_lookup(self):
        with self.assertRaises(pe.SnapshotError):
            self.snapshot.page("77")
        with self.assertRaises(pe.SnapshotError):
            self.snapshot.page("0")  # both pages start with a zero

    def test_not_a_snapshot(self):
        with self.assertRaises(pe.SnapshotError):
            pe.Snapshot({"nothing": 1})


class FileTest(unittest.TestCase):
    def test_file_header_and_determinism(self):
        snapshot = pe.Snapshot(make_snapshot())
        page = snapshot.page("00")
        entry = {"file": "x.pd", "description": "Test file.", "boards": ["МЕНЮ / Диалог", "15 / Сохранить игру"]}
        text = pe.build_file(snapshot, page, entry, "2026-11-13")
        self.assertEqual(text, pe.build_file(snapshot, page, entry, "2026-11-13"))
        head = text.splitlines()
        self.assertEqual(head[0], "format_version = 1")
        self.assertIn('page "00 · Сквозной прототип" (snapshot 2026-11-13)', head[1])
        self.assertEqual(head[2], "# Test file.")
        self.assertEqual(text.count("\nboard "), 2)
        self.assertLess(text.index('board "МЕНЮ / Диалог"'), text.index('board "15 / Сохранить игру"'))
        self.assertTrue(text.endswith("}\n"))
        lowered = text.lower()
        self.assertNotIn("token", lowered)

    def test_cli_manifest_and_list(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            snap = root / "snap.json"
            snap.write_text(json.dumps(make_snapshot()), encoding="utf-8")
            manifest = root / "m.json"
            manifest.write_text(
                json.dumps({"files": [{"file": "one.pd", "boards": ["МЕНЮ / Диалог"]},
                                      {"file": "two.pd", "boards": ["15 / Сохранить игру"]}]}),
                encoding="utf-8",
            )
            out = root / "out"
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(pe.main([str(snap), "--manifest", str(manifest), "--out-dir", str(out),
                                          "--only", "two.pd"]), 0)
            self.assertEqual(sorted(p.name for p in out.iterdir()), ["two.pd"])
            self.assertIn('board "15 / Сохранить игру"', (out / "two.pd").read_text(encoding="utf-8"))
            listing = io.StringIO()
            with contextlib.redirect_stdout(listing):
                self.assertEqual(pe.main([str(snap), "--list"]), 0)
            self.assertIn("15 / Сохранить игру", listing.getvalue())

    def test_cli_reports_errors_without_traceback(self):
        with tempfile.TemporaryDirectory() as tmp:
            bad = Path(tmp) / "bad.json"
            bad.write_text("{}", encoding="utf-8")
            stderr = io.StringIO()
            with contextlib.redirect_stderr(stderr):
                self.assertEqual(pe.main([str(bad), "--list"]), 1)
            self.assertIn("penpot_export:", stderr.getvalue())


if __name__ == "__main__":
    unittest.main()
