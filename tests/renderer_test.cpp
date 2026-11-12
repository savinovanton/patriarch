// Renderer tests run on an offscreen target with the WARP software rasterizer, so they work on
// machines (and CI runners) without a GPU or a display.

#include "pat/engine/camera.hpp"
#include "pat/engine/renderer.hpp"
#include "pat/mapdata/map_gen.hpp"

#include <doctest/doctest.h>

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#ifdef _WIN32

namespace {

using pat::engine::Color;
using pat::engine::IRenderer;
using pat::engine::PixelSize;
using pat::engine::Quad;
using pat::engine::RendererDesc;

std::unique_ptr<IRenderer> make_renderer(std::filesystem::path shader_dir = {}, PixelSize size = {64, 64}) {
    RendererDesc desc;
    desc.size = size;
    desc.software = true;
    desc.shader_dir = std::move(shader_dir);
    auto created = pat::engine::create_renderer(desc);
    REQUIRE(created.has_value());
    return std::move(created).value();
}

struct Rgba {
    int r, g, b, a;
};

Rgba pixel(const std::vector<std::uint8_t>& px, int width, int x, int y) {
    const std::size_t i =
        (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)) * 4;
    return {px[i], px[i + 1], px[i + 2], px[i + 3]};
}

std::filesystem::path make_temp_dir(const char* name) {
    const auto dir = std::filesystem::temp_directory_path() / name;
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}

std::string read_file(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Gives the file a strictly newer modification time than any previous touch() (file times can have
// coarse granularity, so two quick writes may otherwise carry the same timestamp).
void touch(const std::filesystem::path& p) {
    static int generation = 0;
    ++generation;
    std::filesystem::last_write_time(
        p, std::filesystem::file_time_type::clock::now() + std::chrono::seconds(10 * generation));
}

void write_file(const std::filesystem::path& p, const std::string& text) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << text;
}

// Draws one full-target quad of `c` over black and returns the centre pixel.
Rgba draw_fill(IRenderer& r, Color c) {
    REQUIRE(r.begin_frame({0, 0, 0, 1}));
    const Quad q{0, 0, static_cast<float>(r.size().w), static_cast<float>(r.size().h), c};
    r.draw_quads(std::array{q});
    const auto px = r.capture();
    r.end_frame();
    REQUIRE(px.size() == static_cast<std::size_t>(r.size().w) * static_cast<std::size_t>(r.size().h) * 4);
    return pixel(px, r.size().w, r.size().w / 2, r.size().h / 2);
}

}  // namespace

TEST_CASE("Renderer: interface reports the backend and the target size") {
    auto r = make_renderer();
    CHECK(r->backend_name() == "Direct3D 11");
    CHECK_FALSE(r->adapter_name().empty());
    CHECK(r->size() == PixelSize{64, 64});
    CHECK(r->shader_generation() == 1);
}

TEST_CASE("Renderer: clear colour and a coloured quad land on the right pixels") {
    auto r = make_renderer();
    REQUIRE(r->begin_frame({0.0f, 0.0f, 1.0f, 1.0f}));      // blue background
    const Quad q{16, 8, 16, 16, {1.0f, 0.0f, 0.0f, 1.0f}};  // red square: x 16..31, y 8..23
    r->draw_quads(std::array{q});
    const auto px = r->capture();
    CHECK(r->last_frame_stats().draw_calls == 1);
    CHECK(r->last_frame_stats().quads == 1);
    r->end_frame();

    REQUIRE(px.size() == 64u * 64u * 4u);
    const Rgba inside = pixel(px, 64, 20, 12);
    CHECK(inside.r == 255);
    CHECK(inside.g == 0);
    CHECK(inside.b == 0);
    const Rgba outside = pixel(px, 64, 40, 40);
    CHECK(outside.r == 0);
    CHECK(outside.b == 255);
    // Pixel (x=15) is left of the quad, (x=31) is its last column, (x=32) is outside; same for y (top-left
    // origin).
    CHECK(pixel(px, 64, 15, 12).b == 255);
    CHECK(pixel(px, 64, 31, 12).r == 255);
    CHECK(pixel(px, 64, 32, 12).b == 255);
    CHECK(pixel(px, 64, 20, 7).b == 255);
    CHECK(pixel(px, 64, 20, 23).r == 255);
    CHECK(pixel(px, 64, 20, 24).b == 255);
}

TEST_CASE("Renderer: alpha blending and submission order") {
    auto r = make_renderer();
    // 50% white over black is mid grey.
    const Rgba grey = draw_fill(*r, {1.0f, 1.0f, 1.0f, 0.5f});
    CHECK(grey.r >= 126);
    CHECK(grey.r <= 129);

    // Later quads draw over earlier ones.
    REQUIRE(r->begin_frame({0, 0, 0, 1}));
    const std::array<Quad, 2> quads = {Quad{0, 0, 64, 64, {1, 0, 0, 1}}, Quad{0, 0, 64, 64, {0, 1, 0, 1}}};
    r->draw_quads(quads);
    const auto px = r->capture();
    r->end_frame();
    const Rgba top = pixel(px, 64, 10, 10);
    CHECK(top.r == 0);
    CHECK(top.g == 255);
}

TEST_CASE("Renderer: batching more quads than one batch holds") {
    auto r = make_renderer();
    REQUIRE(r->begin_frame({0, 0, 0, 1}));
    std::vector<Quad> quads(20000, Quad{0, 0, 64, 64, {0.0f, 0.0f, 0.0f, 0.0f}});  // invisible
    quads.back() = Quad{0, 0, 64, 64, {1, 1, 0, 1}};
    r->draw_quads(quads);
    CHECK(r->last_frame_stats().quads == 20000);
    CHECK(r->last_frame_stats().draw_calls == 3);  // 8192 + 8192 + 3616
    const auto px = r->capture();
    r->end_frame();
    CHECK(pixel(px, 64, 5, 5).r == 255);
    CHECK(pixel(px, 64, 5, 5).g == 255);
}

TEST_CASE("Renderer: resize rebuilds the target, zero size skips frames") {
    auto r = make_renderer();
    r->resize({32, 16});
    CHECK(r->size() == PixelSize{32, 16});
    CHECK(draw_fill(*r, {1, 0, 0, 1}).r == 255);

    r->resize({0, 0});  // minimized
    CHECK_FALSE(r->begin_frame({0, 0, 0, 1}));

    r->resize({48, 48});
    CHECK(draw_fill(*r, {0, 1, 0, 1}).g == 255);
}

TEST_CASE("Renderer: GPU timing and memory queries are sane") {
    auto r = make_renderer();
    CHECK(r->last_gpu_ms() < 0.0f);  // nothing measured yet
    for (int i = 0; i < 20; ++i) {
        REQUIRE(r->begin_frame({0, 0, 0, 1}));
        r->draw_quads(std::array{Quad{0, 0, 64, 64, {1, 0, 0, 1}}});
        r->end_frame();
    }
    // Timestamp queries may be unsupported on a software device; when available the value is finite and
    // small.
    const float gpu = r->last_gpu_ms();
    CHECK(gpu == gpu);  // not NaN
    CHECK(gpu < 1000.0f);
    CHECK(r->gpu_memory_mb() >= 0.0f);
}

TEST_CASE("Renderer: markers and vsync toggles are safe no-ops offscreen") {
    auto r = make_renderer();
    r->set_vsync(false);
    CHECK_FALSE(r->vsync());
    REQUIRE(r->begin_frame({0, 0, 0, 1}));
    r->push_marker("test region");
    r->pop_marker();
    r->end_frame();
}

TEST_CASE("Renderer: shaders hot-reload from disk and bad shaders keep the old one") {
    const auto dir = make_temp_dir("patriarch_shader_reload_test");
    const auto shader = dir / "quad.hlsl";
    const std::string original = read_file(std::filesystem::path(PATRIARCH_ENGINE_SHADER_DIR) / "quad.hlsl");
    REQUIRE_FALSE(original.empty());
    write_file(shader, original);

    auto r = make_renderer(dir);
    CHECK(r->shader_generation() == 1);
    CHECK_FALSE(r->reload_shaders_if_changed());  // nothing changed yet
    const Rgba before = draw_fill(*r, {1, 0, 0, 1});
    CHECK(before.r == 255);
    CHECK(before.b == 0);

    // New version swaps the red and blue channels.
    std::string swapped = original;
    const std::string needle = "return input.color;";
    const auto pos = swapped.find(needle);
    REQUIRE(pos != std::string::npos);
    swapped.replace(pos, needle.size(), "return float4(input.color.bgr, input.color.a);");
    write_file(shader, swapped);
    touch(shader);

    CHECK(r->reload_shaders_if_changed());
    CHECK(r->shader_generation() == 2);
    const Rgba after = draw_fill(*r, {1, 0, 0, 1});
    CHECK(after.r == 0);
    CHECK(after.b == 255);

    // A syntax error must not break rendering: the swapped shader stays active.
    write_file(shader, "this is not hlsl");
    touch(shader);
    CHECK_FALSE(r->reload_shaders_if_changed());
    CHECK(r->shader_generation() == 2);
    const Rgba still = draw_fill(*r, {1, 0, 0, 1});
    CHECK(still.b == 255);

    // Fixing the file recovers.
    write_file(shader, original);
    touch(shader);
    CHECK(r->reload_shaders_if_changed());
    CHECK(draw_fill(*r, {1, 0, 0, 1}).r == 255);

    r.reset();
    std::filesystem::remove_all(dir);
}

// ---------------------------------------------------------------- map view (terrain)

namespace {

struct MapFixture {
    pat::mapdata::MapData map;
    std::unique_ptr<IRenderer> renderer;
    std::unique_ptr<pat::engine::IMapView> view;
    pat::engine::OrbitCamera camera;

    MapFixture() : camera(limits_for_small_map()) {
        pat::mapdata::GenParams gp;
        gp.width = 256;
        gp.height = 128;
        gp.province_count = 24;
        gp.seed = 3;
        gp.threads = 2;
        map = pat::mapdata::generate_map(gp);
        renderer = make_renderer({}, {640, 360});
        auto created = renderer->create_map_view(map, {});
        REQUIRE(created.has_value());
        view = std::move(created).value();
        camera.set_focus(128.0f, 64.0f);
        camera.set_distance(260.0f);
    }

    static pat::engine::CameraLimits limits_for_small_map() {
        pat::engine::CameraLimits l;
        l.map_width = 256;
        l.map_height = 128;
        l.min_distance = 10;
        l.max_distance = 500;
        return l;
    }

    pat::engine::MapFrame frame(std::uint16_t hovered = 0, std::uint16_t selected = 0) const {
        pat::engine::MapFrame f;
        f.view_proj = camera.view_projection(640.0f / 360.0f);
        f.eye = camera.eye();
        f.zoom_fraction = camera.zoom_fraction();
        f.hovered = hovered;
        f.selected = selected;
        return f;
    }

    std::vector<std::uint8_t> render(const pat::engine::MapFrame& f) {
        REQUIRE(renderer->begin_frame({0.02f, 0.02f, 0.02f, 1.0f}));
        view->draw(f);
        auto px = renderer->capture();
        renderer->end_frame();
        REQUIRE(px.size() == 640u * 360u * 4u);
        return px;
    }
};

std::size_t differing_pixels(const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b) {
    std::size_t n = 0;
    for (std::size_t i = 0; i + 3 < a.size(); i += 4) {
        n += (a[i] != b[i] || a[i + 1] != b[i + 1] || a[i + 2] != b[i + 2]) ? 1U : 0U;
    }
    return n;
}

}  // namespace

TEST_CASE("Map view: draws terrain with water and land, uses a bounded number of nodes") {
    MapFixture fx;
    fx.view->set_mode(pat::engine::MapMode::Owners);
    const auto px = fx.render(fx.frame());
    CHECK(fx.view->last_node_count() > 0);
    CHECK(fx.view->last_node_count() <= 500);

    // The map (continent plus surrounding sea) fills a good part of the frame; the rest is the clear colour.
    std::size_t clear_pixels = 0;
    std::size_t bluish = 0;
    std::size_t other = 0;
    for (std::size_t i = 0; i + 3 < px.size(); i += 4) {
        if (px[i] <= 8 && px[i + 1] <= 8 && px[i + 2] <= 8) {
            ++clear_pixels;
        } else if (px[i + 2] > px[i] && px[i + 2] > px[i + 1]) {
            ++bluish;
        } else {
            ++other;
        }
    }
    CHECK(clear_pixels < 640u * 360u * 3u / 4u);
    CHECK(bluish > 1000);  // sea
    CHECK(other > 1000);   // land
}

TEST_CASE("Map view: switching the map mode recolours without rebuilding geometry") {
    MapFixture fx;
    fx.view->set_mode(pat::engine::MapMode::Terrain);
    const auto terrain = fx.render(fx.frame());
    const std::size_t nodes_terrain = fx.view->last_node_count();

    fx.view->set_mode(pat::engine::MapMode::Owners);
    CHECK(fx.view->mode() == pat::engine::MapMode::Owners);
    const auto owners = fx.render(fx.frame());
    fx.view->set_mode(pat::engine::MapMode::Mood);
    const auto mood = fx.render(fx.frame());
    fx.view->set_mode(pat::engine::MapMode::Random);
    const auto random = fx.render(fx.frame());

    CHECK(fx.view->last_node_count() == nodes_terrain);  // same geometry in every mode
    CHECK(differing_pixels(terrain, owners) > 5000);
    CHECK(differing_pixels(owners, mood) > 5000);
    CHECK(differing_pixels(mood, random) > 5000);
}

TEST_CASE("Map view: lighting - sun direction, relief shadows and haze change the picture") {
    MapFixture fx;
    fx.view->set_mode(pat::engine::MapMode::Terrain);
    auto lit = [&](pat::engine::MapLighting light) {
        pat::engine::MapFrame f = fx.frame();
        f.light = light;
        return fx.render(f);
    };
    pat::engine::MapLighting base;
    base.haze = 0.0f;
    base.sun_elevation_deg = 10.0f;  // a low sun casts long shadows even on this gentle map
    pat::engine::MapLighting no_shadows = base;
    no_shadows.shadow_steps = 0;
    const auto with_shadows = lit(base);
    const auto without_shadows = lit(no_shadows);
    CHECK(differing_pixels(with_shadows, without_shadows) > 200);

    // Shadows only darken: no pixel gets brighter when they are switched on.
    std::size_t brighter = 0;
    std::size_t darker = 0;
    for (std::size_t i = 0; i + 3 < with_shadows.size(); i += 4) {
        const int a = with_shadows[i] + with_shadows[i + 1] + with_shadows[i + 2];
        const int b = without_shadows[i] + without_shadows[i + 1] + without_shadows[i + 2];
        brighter += a > b + 3 ? 1U : 0U;
        darker += a + 3 < b ? 1U : 0U;
    }
    CHECK(brighter == 0);
    CHECK(darker > 100);

    // The sun direction and the haze matter.
    pat::engine::MapLighting east = no_shadows;
    east.sun_azimuth_deg = 90.0f;
    CHECK(differing_pixels(without_shadows, lit(east)) > 1000);
    pat::engine::MapLighting hazy = no_shadows;
    hazy.haze = 2.0f;
    CHECK(differing_pixels(without_shadows, lit(hazy)) > 1000);

    // Shadow strength 0 equals no shadows.
    pat::engine::MapLighting weak = base;
    weak.shadow_strength = 0.0f;
    CHECK(differing_pixels(lit(weak), without_shadows) == 0);
}

TEST_CASE("Map view: art style - off by default, winter palette and grading change the picture") {
    MapFixture fx;
    fx.view->set_mode(pat::engine::MapMode::Terrain);
    const pat::engine::MapFrame plain = fx.frame();
    const auto neutral = fx.render(plain);
    CHECK(differing_pixels(neutral, fx.render(plain)) == 0);  // the default style adds nothing and is stable

    pat::engine::MapFrame styled = plain;
    styled.style = pat::engine::winter_style_1703();
    CHECK(differing_pixels(neutral, fx.render(styled)) > 5000);

    // The vignette only darkens, and only towards the edges.
    pat::engine::MapFrame vignette = plain;
    vignette.style.vignette = 1.0f;
    const auto dark = fx.render(vignette);
    std::size_t brighter = 0;
    std::size_t darker = 0;
    for (std::size_t i = 0; i + 3 < neutral.size(); i += 4) {
        const int a = dark[i] + dark[i + 1] + dark[i + 2];
        const int b = neutral[i] + neutral[i + 1] + neutral[i + 2];
        brighter += a > b ? 1U : 0U;
        darker += a + 3 < b ? 1U : 0U;
    }
    CHECK(brighter == 0);
    CHECK(darker > 1000);
}

TEST_CASE("Map view: the sea is animated and only the water changes with time") {
    MapFixture fx;
    fx.view->set_mode(pat::engine::MapMode::Terrain);
    pat::engine::MapFrame a = fx.frame();
    a.time_s = 0.0f;
    pat::engine::MapFrame b = a;
    b.time_s = 7.0f;
    const auto at_0 = fx.render(a);
    const auto at_7 = fx.render(b);
    const std::size_t differing = differing_pixels(at_0, at_7);
    CHECK(differing > 500);  // waves moved

    // Land is not animated: pixels that differ are bluish (water), and the same frame twice is identical.
    std::size_t land_changes = 0;
    for (std::size_t i = 0; i + 3 < at_0.size(); i += 4) {
        const bool changed = at_0[i] != at_7[i] || at_0[i + 1] != at_7[i + 1] || at_0[i + 2] != at_7[i + 2];
        const bool water_like = at_0[i + 2] > at_0[i] && at_0[i + 2] > at_0[i + 1];
        land_changes += changed && !water_like ? 1U : 0U;
    }
    CHECK(land_changes < differing / 20 + 5);
    CHECK(differing_pixels(fx.render(a), at_0) == 0);
}

TEST_CASE("Map view: night is darker than noon and dusk is warmer than noon") {
    MapFixture fx;
    fx.view->set_mode(pat::engine::MapMode::Terrain);
    const auto mean_rgb = [&](float elevation_deg) {
        pat::engine::MapFrame f = fx.frame();
        f.light.sun_elevation_deg = elevation_deg;
        const auto px = fx.render(f);
        double r = 0.0;
        double g = 0.0;
        double b = 0.0;
        std::size_t n = 0;
        for (std::size_t i = 0; i + 3 < px.size(); i += 4) {
            r += px[i];
            g += px[i + 1];
            b += px[i + 2];
            ++n;
        }
        return std::array<double, 3>{
            r / static_cast<double>(n), g / static_cast<double>(n), b / static_cast<double>(n)};
    };
    const auto noon = mean_rgb(55.0f);
    const auto dusk = mean_rgb(4.0f);
    const auto night = mean_rgb(-25.0f);
    const auto luma = [](const std::array<double, 3>& c) { return c[0] * 0.3 + c[1] * 0.6 + c[2] * 0.1; };
    CHECK(luma(night) < luma(noon) * 0.6);
    CHECK(luma(night) < luma(dusk));
    CHECK(dusk[0] / dusk[2] > noon[0] / noon[2]);    // warmer at dusk
    CHECK(night[2] / night[0] > noon[2] / noon[0]);  // bluer at night
}

TEST_CASE("Map view: selection highlight lands on the province the CPU says is under the pixel") {
    MapFixture fx;
    fx.view->set_mode(pat::engine::MapMode::Owners);
    const auto plain = fx.render(fx.frame());

    const pat::engine::TerrainGeometry geometry = fx.view->geometry();
    const pat::engine::HeightfieldPicker picker(fx.map, geometry);
    const pat::engine::Mat4 view_proj = fx.camera.view_projection(640.0f / 360.0f);
    int verified = 0;
    for (const pat::mapdata::Province& p : fx.map.province_list) {
        // Screen pixel of this province's centroid.
        const float wx = static_cast<float>(p.centroid_x) + 0.5f;
        const float wy = static_cast<float>(p.centroid_y) + 0.5f;
        const pat::engine::Vec3 world{wx, wy, picker.height_at(wx, wy)};
        const auto clip = pat::engine::transform_point(view_proj, world);
        if (clip.x < -0.9f || clip.x > 0.9f || clip.y < -0.9f || clip.y > 0.9f || clip.z < 0.0f ||
            clip.z > 1.0f) {
            continue;  // off screen from this camera
        }
        const int sx = static_cast<int>((clip.x * 0.5f + 0.5f) * 640.0f);
        const int sy = static_cast<int>((0.5f - clip.y * 0.5f) * 360.0f);

        // The CPU picker must agree about what is under that pixel...
        pat::engine::Vec3 origin;
        pat::engine::Vec3 dir;
        REQUIRE(pat::engine::make_pick_ray(view_proj,
                                           static_cast<float>(sx) + 0.5f,
                                           static_cast<float>(sy) + 0.5f,
                                           640.0f,
                                           360.0f,
                                           origin,
                                           dir));
        const auto hit = picker.raycast(origin, dir, 5000.0f);
        if (!hit.hit || picker.province_at(hit.point.x, hit.point.y) != p.id) {
            continue;  // a neighbouring province or an occluding ridge: only check unambiguous provinces
        }
        // ...and the GPU highlights exactly that province there.
        const auto selected = fx.render(fx.frame(0, p.id));
        const std::size_t i = (static_cast<std::size_t>(sy) * 640 + static_cast<std::size_t>(sx)) * 4;
        const int dr = selected[i] - plain[i];
        const int db = selected[i + 2] - plain[i + 2];
        CHECK(dr > 0);   // warm highlight tint
        CHECK(dr > db);  // yellowish: red rises more than blue
        // Selecting a different province leaves this pixel untouched.
        const std::uint16_t other = p.id == 1 ? 2 : 1;
        const auto elsewhere = fx.render(fx.frame(0, other));
        CHECK(elsewhere[i] == plain[i]);
        CHECK(elsewhere[i + 1] == plain[i + 1]);
        CHECK(elsewhere[i + 2] == plain[i + 2]);
        if (++verified >= 4) {
            break;
        }
    }
    CHECK(verified >= 3);
}

TEST_CASE("Map view: palettes can be replaced from simulation values") {
    MapFixture fx;
    const std::size_t n = fx.map.province_list.size();
    const auto solid = [&](std::uint8_t r, std::uint8_t g, std::uint8_t b) {
        std::vector<std::uint32_t> colors(n + 1,
                                          0xFF000000U | (static_cast<std::uint32_t>(b) << 16U) |
                                              (static_cast<std::uint32_t>(g) << 8U) | r);
        return colors;
    };
    // Mean (red - blue) over the frame: land takes on the palette colour, the sea is unaffected.
    const auto red_minus_blue = [](const std::vector<std::uint8_t>& px) {
        double sum = 0.0;
        for (std::size_t i = 0; i + 3 < px.size(); i += 4) {
            sum += static_cast<double>(px[i]) - static_cast<double>(px[i + 2]);
        }
        return sum / static_cast<double>(px.size() / 4);
    };

    fx.view->set_mode(pat::engine::MapMode::Owners);
    fx.view->set_palette(pat::engine::MapMode::Owners, solid(255, 0, 0));
    const double red = red_minus_blue(fx.render(fx.frame()));
    fx.view->set_palette(pat::engine::MapMode::Owners, solid(0, 0, 255));
    const double blue = red_minus_blue(fx.render(fx.frame()));
    CHECK(red - blue > 20.0);  // the current mode's palette updates immediately

    // Updating another mode changes nothing on screen until that mode is selected.
    fx.view->set_palette(pat::engine::MapMode::Mood, solid(255, 0, 0));
    const double still_blue = red_minus_blue(fx.render(fx.frame()));
    CHECK(still_blue == doctest::Approx(blue));
    fx.view->set_mode(pat::engine::MapMode::Mood);
    CHECK(red_minus_blue(fx.render(fx.frame())) - blue > 20.0);

    // Short spans update only the leading entries; extra entries are ignored.
    fx.view->set_palette(pat::engine::MapMode::Mood, solid(0, 0, 255));
    const double all_blue = red_minus_blue(fx.render(fx.frame()));
    std::vector<std::uint32_t> one_red(2, 0xFF0000FFU);  // province 1 red, index 0 irrelevant
    fx.view->set_palette(pat::engine::MapMode::Mood, one_red);
    const double with_one_red = red_minus_blue(fx.render(fx.frame()));
    CHECK(with_one_red >= all_blue);
    std::vector<std::uint32_t> huge(100000, 0xFF0000FFU);
    fx.view->set_palette(pat::engine::MapMode::Mood, huge);  // must not overflow
    CHECK(red_minus_blue(fx.render(fx.frame())) > all_blue);
}

TEST_CASE("Map view: rendering coexists with quads in one frame") {
    MapFixture fx;
    REQUIRE(fx.renderer->begin_frame({0, 0, 0, 1}));
    fx.view->draw(fx.frame());
    const Quad q{10, 10, 50, 50, {1.0f, 0.0f, 1.0f, 1.0f}};
    fx.renderer->draw_quads(std::array{q});  // drawn after the map: pipeline state must have been restored
    const auto px = fx.renderer->capture();
    fx.renderer->end_frame();
    const Rgba p = pixel(px, 640, 30, 30);
    CHECK(p.r == 255);
    CHECK(p.g == 0);
    CHECK(p.b == 255);
}

#endif  // _WIN32
