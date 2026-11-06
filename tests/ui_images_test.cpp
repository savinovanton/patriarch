// Images in the UI: PNG through the Windows Imaging Component and SVG through the RmlUi SVG element,
// drawn from the real Olive Stone assets on an offscreen WARP target.

#include "pat/engine/renderer.hpp"
#include "pat/engine/ui.hpp"

#include <doctest/doctest.h>

#ifdef _WIN32

#include <RmlUi/Core.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

using namespace pat::engine;

constexpr int kW = 480;
constexpr int kH = 270;

struct Rgb {
    int r, g, b;
};

Rgb pixel(const std::vector<std::uint8_t>& px, int x, int y) {
    const std::size_t i = (static_cast<std::size_t>(y) * kW + static_cast<std::size_t>(x)) * 4;
    return {px[i], px[i + 1], px[i + 2]};
}

const char* kRml = R"(<rml>
<head><style>
body { width: 100%; height: 100%; }
div, img { display: block; }
#frame { position: absolute; left: 0; top: 0; width: 480dp; height: 270dp; }
#icon { position: absolute; left: 200dp; top: 100dp; width: 40dp; height: 38dp; }
</style></head>
<body>
<img id="frame" src="frame.png"/>
<svg id="icon" src="menu_world.svg"/>
</body>
</rml>)";

}  // namespace

TEST_CASE("UI images: PNG frame and SVG icon from the Olive Stone assets are drawn") {
    const std::filesystem::path assets = std::filesystem::path(PATRIARCH_ASSETS_DIR) / "ui" / "olive";
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "patriarch_ui_images";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    std::filesystem::copy_file(assets / "frame.png", dir / "frame.png");
    std::filesystem::copy_file(assets / "icons" / "menu_world.svg", dir / "menu_world.svg");
    {
        std::ofstream out(dir / "images.rml", std::ios::binary | std::ios::trunc);
        out << kRml;
    }

    RendererDesc rd;
    rd.size = {kW, kH};
    rd.software = true;
    auto created = create_renderer(rd);
    REQUIRE(created.has_value());
    std::unique_ptr<IRenderer> renderer = std::move(created).value();

    UiDesc ud;
    ud.size = {kW, kH};
    auto ui_created = UiContext::create(*renderer, nullptr, ud);
    REQUIRE(ui_created.has_value());
    std::unique_ptr<UiContext> ui = std::move(ui_created).value();
    REQUIRE(ui->load_document(dir / "images.rml") != nullptr);

    REQUIRE(renderer->begin_frame({0, 0, 0, 1}));
    ui->update();
    ui->render();
    const auto px = renderer->capture();
    renderer->end_frame();
    REQUIRE(px.size() == static_cast<std::size_t>(kW) * kH * 4);

    // The frame: its gilded edge is bright, its centre is transparent (the clear colour shows through).
    int bright_edge = 0;
    for (int x = 0; x < kW; ++x) {
        const Rgb p = pixel(px, x, 2);
        bright_edge += p.r > 90 ? 1 : 0;
    }
    CHECK(bright_edge > kW / 2);
    const Rgb centre = pixel(px, 100, 60);
    CHECK(centre.r < 12);
    CHECK(centre.g < 12);
    CHECK(centre.b < 12);

    // The SVG button: a 40x38 tile filled with #3D4736 whose pixel at (204,104) is inside the fill.
    const Rgb tile = pixel(px, 204, 104);
    CHECK(tile.r == doctest::Approx(0x3D).epsilon(0.25));
    CHECK(tile.g == doctest::Approx(0x47).epsilon(0.25));
    CHECK(tile.b == doctest::Approx(0x36).epsilon(0.25));

    ui.reset();
    renderer.reset();
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

#endif  // _WIN32
