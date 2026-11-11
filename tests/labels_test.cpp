// Map labels (E3-T05): SDF font atlas and text layout, label placement (zoom LOD, overlap culling),
// label generation for a map, and the SDF text renderer. Font-dependent tests skip without the PT Serif file.

#include "pat/engine/camera.hpp"
#include "pat/engine/label_layout.hpp"
#include "pat/engine/map_labels.hpp"
#include "pat/engine/renderer.hpp"
#include "pat/engine/sdf_font.hpp"
#include "pat/mapdata/map_gen.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>

#include "test_fonts.hpp"

using namespace pat::engine;
namespace mapdata = pat::mapdata;

namespace {

bool have_times() {
    return std::filesystem::exists(pat::test::game_label_font());
}

/// An atlas without a font file: every glyph is a 24-pixel square tile of a synthetic distance field.
SdfAtlas make_block_atlas() {
    SdfAtlas atlas;
    atlas.width = 64;
    atlas.height = 64;
    atlas.font_px = 40.0f;
    atlas.spread = 8.0f;
    atlas.ascent = 30.0f;
    atlas.descent = 8.0f;
    atlas.line_height = 40.0f;
    atlas.pixels.assign(64 * 64, 0);
    // A 32x32 tile at (0,0): a square of half-size 12 centred in it; distance mapped like FreeType's SDF.
    for (int y = 0; y < 32; ++y) {
        for (int x = 0; x < 32; ++x) {
            const float dx = std::fabs(static_cast<float>(x) + 0.5f - 16.0f) - 12.0f;
            const float dy = std::fabs(static_cast<float>(y) + 0.5f - 16.0f) - 12.0f;
            const float outside =
                std::sqrt(std::max(dx, 0.0f) * std::max(dx, 0.0f) + std::max(dy, 0.0f) * std::max(dy, 0.0f));
            const float inside = std::min(std::max(dx, dy), 0.0f);
            const float signed_distance = outside + inside;  // negative inside
            const float v = 0.5f - signed_distance / (2.0f * atlas.spread);
            atlas.pixels[static_cast<std::size_t>(y) * 64 + static_cast<std::size_t>(x)] =
                static_cast<std::uint8_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f);
        }
    }
    const auto add_range = [&](char32_t first, char32_t last) {
        for (char32_t cp = first; cp <= last; ++cp) {
            SdfGlyph g;
            g.codepoint = cp;
            g.u0 = 0.0f;
            g.v0 = 0.0f;
            g.u1 = 32.0f / 64.0f;
            g.v1 = 32.0f / 64.0f;
            g.width = 32.0f;
            g.height = 32.0f;
            g.bearing_x = 0.0f;
            g.bearing_y = 28.0f;
            g.advance = cp == U' ' ? 12.0f : 26.0f;
            if (cp == U' ') {
                g.width = 0.0f;  // like real fonts: a space only advances
                g.height = 0.0f;
            }
            atlas.glyphs.push_back(g);
        }
    };
    add_range(0x20, 0x7E);
    add_range(0x0401, 0x0401);
    add_range(0x0410, 0x044F);
    add_range(0x0451, 0x0451);
    std::sort(atlas.glyphs.begin(), atlas.glyphs.end(), [](const SdfGlyph& a, const SdfGlyph& b) {
        return a.codepoint < b.codepoint;
    });
    return atlas;
}

std::vector<MapLabel> grid_labels(int cols, int rows, float spacing, float extent, const SdfAtlas& atlas) {
    std::vector<MapLabel> labels;
    std::uint32_t id = 1;
    for (int y = 0; y < rows; ++y) {
        for (int x = 0; x < cols; ++x) {
            MapLabel l;
            l.id = id++;
            l.kind = LabelKind::Province;
            l.text = utf8_to_utf32("Ярославский");
            l.anchor = {static_cast<float>(x) * spacing + spacing * 0.5f,
                        static_cast<float>(y) * spacing + spacing * 0.5f,
                        0.0f};
            l.world_extent = extent;
            l.width_per_px = measure_text(atlas, l.text, 1.0f);
            labels.push_back(std::move(l));
        }
    }
    return labels;
}

LabelLayoutInput camera_input(OrbitCamera& cam, float w, float h) {
    LabelLayoutInput in;
    in.view_proj = cam.view_projection(w / h);
    in.eye = cam.eye();
    in.viewport_w = w;
    in.viewport_h = h;
    in.pixels_per_unit_at_one = h / (2.0f * std::tan(cam.fov_y() * 0.5f));
    return in;
}

}  // namespace

// ---------------------------------------------------------------- text layout

TEST_CASE("utf8_to_utf32: Cyrillic, 4-byte sequences and broken input") {
    CHECK(utf8_to_utf32("abc") == U"abc");
    CHECK(utf8_to_utf32("Ёжик «ё» — №") == U"Ёжик «ё» — №");
    CHECK(utf8_to_utf32("\xF0\x9F\x98\x80") == std::u32string(1, char32_t(0x1F600)));
    CHECK(utf8_to_utf32("\xD0") == std::u32string(1, char32_t(0xFFFD)));  // truncated sequence
    CHECK(utf8_to_utf32("").empty());
}

TEST_CASE("text layout: widths scale with the size and the line is centred") {
    const SdfAtlas atlas = make_block_atlas();
    const std::u32string text = U"Привет";
    CHECK(measure_text(atlas, text, 40.0f) == doctest::Approx(6 * 26.0f));
    CHECK(measure_text(atlas, text, 20.0f) == doctest::Approx(3 * 26.0f));
    CHECK(measure_text(atlas, U"", 20.0f) == 0.0f);

    std::vector<TextQuadGeometry> quads;
    layout_text(atlas, text, 40.0f, 500.0f, 300.0f, quads);
    REQUIRE(quads.size() == 6);
    // Symmetric around the requested centre (all glyphs are the same size here).
    const float left = quads.front().x;
    const float right = quads.back().x + quads.back().w;
    CHECK((left + right) * 0.5f == doctest::Approx(500.0f + (32.0f - 26.0f) * 0.5f).epsilon(0.01));
    // Glyphs advance left to right without overlapping their pen positions.
    for (std::size_t i = 1; i < quads.size(); ++i) {
        CHECK(quads[i].x - quads[i - 1].x == doctest::Approx(26.0f));
    }
    // Vertical position follows the baseline: top = baseline - bearing_y.
    CHECK(quads[0].y == doctest::Approx(300.0f - 28.0f));

    // Half the size: half the geometry.
    std::vector<TextQuadGeometry> small;
    layout_text(atlas, text, 20.0f, 500.0f, 300.0f, small);
    CHECK(small[0].w == doctest::Approx(quads[0].w * 0.5f));

    // Glyphs missing from the atlas are skipped, spaces only advance.
    std::vector<TextQuadGeometry> odd;
    layout_text(atlas, U"a\u4E2D b", 40.0f, 0.0f, 0.0f, odd);
    CHECK(odd.size() == 2);
}

TEST_CASE("SDF atlas from a real font: Cyrillic glyphs with sensible distance values") {
    if (!have_times()) {
        MESSAGE("PT Serif not found, skipping");
        return;
    }
    auto built = build_sdf_atlas(pat::test::game_label_font());
    REQUIRE(built.has_value());
    const SdfAtlas& atlas = built.value();
    CHECK(atlas.width == 1024);
    CHECK(atlas.height >= 64);
    CHECK(atlas.pixels.size() ==
          static_cast<std::size_t>(atlas.width) * static_cast<std::size_t>(atlas.height));
    CHECK(atlas.ascent > 0.0f);

    for (const char32_t cp : {U'A', U'z', U'Ж', U'ё', U'Ё', U'ъ', U'«', U'—', U'7', U' '}) {
        CAPTURE(static_cast<unsigned>(cp));
        const SdfGlyph* g = atlas.find(cp);
        REQUIRE(g != nullptr);
        CHECK(g->advance > 0.0f);
    }
    CHECK(atlas.find(U'\u4E2D') == nullptr);  // not requested

    // A glyph's bitmap has both inside (>128) and outside (<128) values, and its quad lies in the atlas.
    const SdfGlyph* zh = atlas.find(U'Ж');
    REQUIRE(zh != nullptr);
    CHECK(zh->width > 0.0f);
    CHECK(zh->u0 >= 0.0f);
    CHECK(zh->u1 <= 1.0f);
    CHECK(zh->v1 <= 1.0f);
    int inside = 0;
    int outside = 0;
    const int x0 = static_cast<int>(zh->u0 * static_cast<float>(atlas.width) + 0.5f);
    const int y0 = static_cast<int>(zh->v0 * static_cast<float>(atlas.height) + 0.5f);
    for (int y = 0; y < static_cast<int>(zh->height); ++y) {
        for (int x = 0; x < static_cast<int>(zh->width); ++x) {
            const std::uint8_t v =
                atlas.pixels[static_cast<std::size_t>(y0 + y) * static_cast<std::size_t>(atlas.width) +
                             static_cast<std::size_t>(x0 + x)];
            inside += v > 140 ? 1 : 0;
            outside += v < 110 ? 1 : 0;
        }
    }
    CHECK(inside > 50);
    CHECK(outside > 50);

    // Distinct code points are distinct glyphs and the table is sorted.
    for (std::size_t i = 1; i < atlas.glyphs.size(); ++i) {
        CHECK(atlas.glyphs[i - 1].codepoint < atlas.glyphs[i].codepoint);
    }
    CHECK(measure_text(atlas, utf8_to_utf32("Новгородский"), 40.0f) > 150.0f);

    CHECK_FALSE(build_sdf_atlas("C:/does/not/exist.ttf").has_value());
}

// ---------------------------------------------------------------- label placement

TEST_CASE("layout_labels: zoom decides what is visible, regions first, no overlaps") {
    const SdfAtlas atlas = make_block_atlas();
    std::vector<MapLabel> labels = grid_labels(12, 8, 100.0f, 90.0f, atlas);
    // A few region labels of a much larger extent.
    for (int i = 0; i < 4; ++i) {
        MapLabel r;
        r.id = 1000 + static_cast<std::uint32_t>(i);
        r.kind = LabelKind::Region;
        r.text = utf8_to_utf32("Поволжье");
        r.anchor = {200.0f + static_cast<float>(i) * 250.0f, 400.0f, 0.0f};
        r.world_extent = 350.0f;
        r.width_per_px = measure_text(atlas, r.text, 1.0f);
        labels.push_back(std::move(r));
    }

    CameraLimits lim;
    lim.map_width = 1200;
    lim.map_height = 800;
    lim.min_distance = 30;
    lim.max_distance = 6000;
    OrbitCamera cam(lim);
    cam.set_focus(600, 400);

    LabelStyle style;
    std::vector<PlacedLabel> placed;

    // Far away: provinces are too small to label, regions are not.
    cam.set_distance(5500);
    LabelLayoutStats far_stats = layout_labels(labels, camera_input(cam, 1280, 720), style, placed);
    std::size_t far_provinces = 0;
    for (const PlacedLabel& p : placed) {
        far_provinces += p.label->kind == LabelKind::Province ? 1U : 0U;
    }
    CHECK(far_provinces == 0);
    CHECK(far_stats.hidden_zoom > 50);

    // Close: provinces are labelled, regions have faded out.
    cam.set_distance(320);
    LabelLayoutStats near_stats = layout_labels(labels, camera_input(cam, 1280, 720), style, placed);
    std::size_t near_provinces = 0;
    std::size_t near_regions = 0;
    for (const PlacedLabel& p : placed) {
        near_provinces += p.label->kind == LabelKind::Province ? 1U : 0U;
        near_regions += p.label->kind == LabelKind::Region ? 1U : 0U;
    }
    CHECK(near_provinces > 5);
    CHECK(near_regions == 0);
    CHECK(near_stats.placed == placed.size());

    // At every zoom: no two placed labels overlap and everything stays on screen.
    for (const float distance : {900.0f, 500.0f, 320.0f, 160.0f, 80.0f}) {
        cam.set_distance(distance);
        const LabelLayoutInput in = camera_input(cam, 1280, 720);
        layout_labels(labels, in, style, placed);
        for (std::size_t i = 0; i < placed.size(); ++i) {
            const PlacedLabel& a = placed[i];
            CHECK(a.alpha > 0.0f);
            CHECK(a.size_px >= 10.0f);
            CHECK(a.x + a.width_px * 0.5f >= 0.0f);
            CHECK(a.x - a.width_px * 0.5f <= 1280.0f);
            for (std::size_t j = i + 1; j < placed.size(); ++j) {
                const PlacedLabel& b = placed[j];
                const bool apart_x = std::fabs(a.x - b.x) >= (a.width_px + b.width_px) * 0.5f;
                const bool apart_y = std::fabs(a.y - b.y) >= (a.size_px + b.size_px) * 0.6f;
                CHECK((apart_x || apart_y));
            }
        }
    }

    // Deterministic: the same camera gives the same labels in the same order.
    cam.set_distance(500);
    std::vector<PlacedLabel> again;
    layout_labels(labels, camera_input(cam, 1280, 720), style, placed);
    layout_labels(labels, camera_input(cam, 1280, 720), style, again);
    REQUIRE(placed.size() == again.size());
    for (std::size_t i = 0; i < placed.size(); ++i) {
        CHECK(placed[i].label == again[i].label);
        CHECK(placed[i].x == again[i].x);
    }
}

TEST_CASE("layout_labels: labels behind the camera or off screen are culled; overlaps prefer regions") {
    const SdfAtlas atlas = make_block_atlas();
    CameraLimits lim;
    lim.map_width = 1000;
    lim.map_height = 600;
    lim.min_distance = 30;
    lim.max_distance = 3000;
    OrbitCamera cam(lim);
    cam.set_focus(500, 300);
    cam.set_distance(400);

    std::vector<MapLabel> labels;
    MapLabel province;
    province.id = 1;
    province.kind = LabelKind::Province;
    province.text = utf8_to_utf32("Тульский");
    province.anchor = {500, 300, 0};
    province.world_extent = 200;
    province.width_per_px = measure_text(atlas, province.text, 1.0f);
    labels.push_back(province);
    MapLabel region = province;
    region.id = 2;
    region.kind = LabelKind::Region;
    region.world_extent = 160;  // visible at this distance and drawn on top of the province label
    labels.push_back(region);
    MapLabel far_off = province;
    far_off.id = 3;
    far_off.anchor = {500 + 5000, 300, 0};  // far to the east: outside the screen
    labels.push_back(far_off);
    MapLabel behind = province;
    behind.id = 4;
    behind.anchor = {500, 300 + 5000, 3000};  // behind the camera (which looks north from the south)
    labels.push_back(behind);

    std::vector<PlacedLabel> placed;
    const LabelLayoutStats stats = layout_labels(labels, camera_input(cam, 1280, 720), LabelStyle{}, placed);
    CHECK(stats.hidden_offscreen + stats.hidden_zoom >= 2);
    for (const PlacedLabel& p : placed) {
        CHECK(p.label->id != 3);
        CHECK(p.label->id != 4);
    }
    // Province and region labels collide: only the more important (region) one survives.
    REQUIRE(!placed.empty());
    std::size_t at_centre = 0;
    for (const PlacedLabel& p : placed) {
        at_centre += (std::fabs(p.x - 640.0f) < 40.0f) ? 1U : 0U;
    }
    CHECK(at_centre == 1);
    CHECK(placed.front().label->kind == LabelKind::Region);
}

// ---------------------------------------------------------------- label generation

TEST_CASE("make_test_map_labels: one label per province on its own pixels, plus regions") {
    mapdata::GenParams gp;
    gp.width = 512;
    gp.height = 256;
    gp.province_count = 60;
    gp.seed = 5;
    gp.threads = 2;
    const mapdata::MapData map = mapdata::generate_map(gp);
    const TerrainGeometry geometry = TerrainGeometry::from_map(map);
    const HeightfieldPicker picker(map, geometry);
    const SdfAtlas atlas = make_block_atlas();

    const MapLabelSet set = make_test_map_labels(map, geometry, picker, atlas);
    CHECK(set.province_labels == 60);
    CHECK(set.region_labels >= 5);
    CHECK(set.region_labels <= 10);
    CHECK(set.labels.size() == set.province_labels + set.region_labels);

    std::vector<std::uint32_t> ids;
    for (const MapLabel& l : set.labels) {
        ids.push_back(l.id);
        CHECK_FALSE(l.text.empty());
        CHECK(l.width_per_px > 0.0f);
        CHECK(l.world_extent > 0.0f);
        if (l.kind == LabelKind::Province) {
            // The anchor is a pixel of the province itself.
            CHECK(picker.province_at(l.anchor.x, l.anchor.y) == l.id);
        } else {
            CHECK(picker.province_at(l.anchor.x, l.anchor.y) != 0);
        }
    }
    std::sort(ids.begin(), ids.end());
    CHECK(std::adjacent_find(ids.begin(), ids.end()) == ids.end());  // unique ids
}

// ---------------------------------------------------------------- GPU text

#ifdef _WIN32

namespace {

struct Rgb {
    int r, g, b;
};

Rgb pixel(const std::vector<std::uint8_t>& px, int width, int x, int y) {
    const std::size_t i =
        (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)) * 4;
    return {px[i], px[i + 1], px[i + 2]};
}

std::unique_ptr<IRenderer> make_renderer(int w, int h) {
    RendererDesc rd;
    rd.size = {w, h};
    rd.software = true;
    auto created = create_renderer(rd);
    REQUIRE(created.has_value());
    return std::move(created).value();
}

}  // namespace

TEST_CASE("SDF text renderer: fill, outline and anti-aliased edges from a distance field") {
    auto renderer = make_renderer(200, 120);
    const SdfAtlas atlas = make_block_atlas();
    auto text = renderer->create_text_renderer(atlas);
    REQUIRE(text.has_value());

    // One glyph tile drawn 2x (64 px) at (50, 20): the square's edge is at 24 px from the tile centre.
    const SdfGlyph* g = atlas.find(U'A');
    REQUIRE(g != nullptr);
    TextQuad q;
    q.geometry = {50, 20, 64, 64, g->u0, g->v0, g->u1, g->v1};
    q.color = {1.0f, 0.0f, 0.0f, 1.0f};
    q.outline = {0.0f, 0.0f, 1.0f, 1.0f};

    REQUIRE(renderer->begin_frame({0, 0, 0, 1}));
    text.value()->draw(std::array{q});
    const auto px = renderer->capture();
    renderer->end_frame();

    const Rgb centre = pixel(px, 200, 82, 52);  // middle of the glyph
    CHECK(centre.r > 240);
    CHECK(centre.b < 20);
    const Rgb halo = pixel(px, 200, 109, 52);  // a few pixels outside the red square's right edge (x = 106)
    CHECK(halo.b > 120);                       // the blue outline
    CHECK(halo.r < 60);
    const Rgb far_out = pixel(px, 200, 190, 110);
    CHECK(far_out.r == 0);
    CHECK(far_out.g == 0);
    CHECK(far_out.b == 0);

    // Scaling the same atlas to half the size keeps the glyph shape (distance fields scale).
    TextQuad small = q;
    small.geometry = {140, 20, 32, 32, g->u0, g->v0, g->u1, g->v1};
    REQUIRE(renderer->begin_frame({0, 0, 0, 1}));
    text.value()->draw(std::array{small});
    const auto px2 = renderer->capture();
    renderer->end_frame();
    CHECK(pixel(px2, 200, 156, 36).r > 240);

    // Drawing nothing is fine, and so is drawing again after other passes.
    REQUIRE(renderer->begin_frame({0, 0, 0, 1}));
    text.value()->draw({});
    renderer->draw_quads(std::array{Quad{0, 0, 10, 10, {0, 1, 0, 1}}});
    text.value()->draw(std::array{q});
    renderer->end_frame();
}

TEST_CASE("MapLabelRenderer: labels are drawn over the map and follow the camera") {
    mapdata::GenParams gp;
    gp.width = 512;
    gp.height = 256;
    gp.province_count = 30;
    gp.seed = 6;
    gp.threads = 2;
    const mapdata::MapData map = mapdata::generate_map(gp);

    auto renderer = make_renderer(1280, 720);
    auto view = renderer->create_map_view(map, {});
    REQUIRE(view.has_value());
    const TerrainGeometry geometry = view.value()->geometry();
    const HeightfieldPicker picker(map, geometry);
    SdfAtlas atlas = make_block_atlas();
    MapLabelSet set = make_test_map_labels(map, geometry, picker, atlas);
    auto labels = MapLabelRenderer::create(*renderer, std::move(atlas), std::move(set));
    REQUIRE(labels.has_value());

    CameraLimits lim;
    lim.map_width = geometry.width;
    lim.map_height = geometry.height;
    lim.min_distance = 40;
    lim.max_distance = 900;
    OrbitCamera cam(lim);
    cam.set_focus(256, 128);
    cam.set_distance(380);

    const auto render = [&](bool with_labels) {
        REQUIRE(renderer->begin_frame({0, 0, 0, 1}));
        MapFrame frame;
        frame.view_proj = cam.view_projection(1280.0f / 720.0f);
        frame.eye = cam.eye();
        frame.zoom_fraction = cam.zoom_fraction();
        view.value()->draw(frame);
        if (with_labels) {
            labels.value()->draw(camera_input(cam, 1280, 720));
        }
        auto px = renderer->capture();
        renderer->end_frame();
        return px;
    };
    const auto without = render(false);
    const auto with = render(true);
    std::size_t different = 0;
    for (std::size_t i = 0; i + 3 < without.size(); i += 4) {
        different += (without[i] != with[i] || without[i + 1] != with[i + 1] || without[i + 2] != with[i + 2])
                         ? 1U
                         : 0U;
    }
    CHECK(different > 500);
    CHECK(!labels.value()->last_placed().empty());

    // Zoomed far out, province labels disappear (regions remain) and the picture changes again.
    cam.set_distance(900);
    (void)render(true);
    std::size_t provinces_far = 0;
    for (const PlacedLabel& p : labels.value()->last_placed()) {
        provinces_far += p.label->kind == LabelKind::Province ? 1U : 0U;
    }
    CHECK(provinces_far < 5);
}

#endif  // _WIN32
