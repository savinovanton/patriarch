// Renderer tests run on an offscreen target with the WARP software rasterizer, so they work on
// machines (and CI runners) without a GPU or a display.

#include "pat/engine/renderer.hpp"

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

#endif  // _WIN32
