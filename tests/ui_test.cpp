// UI (RmlUi) tests run on an offscreen WARP target, so they need no GPU or display. Text needs a font:
// the tests use Arial from the Windows fonts folder (present on Windows CI images) and skip otherwise.

#include "pat/engine/renderer.hpp"
#include "pat/engine/ui.hpp"

#include <doctest/doctest.h>

#ifdef _WIN32

#include <RmlUi/Core.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

using namespace pat::engine;

constexpr int kW = 480;
constexpr int kH = 270;

std::filesystem::path arial() {
    return std::filesystem::path("C:/Windows/Fonts/arial.ttf");
}

struct Rgb {
    int r, g, b;
};

Rgb pixel(const std::vector<std::uint8_t>& px, int x, int y) {
    const std::size_t i = (static_cast<std::size_t>(y) * kW + static_cast<std::size_t>(x)) * 4;
    return {px[i], px[i + 1], px[i + 2]};
}

void write_text(const std::filesystem::path& p, const std::string& text) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << text;
}

void touch_newer(const std::filesystem::path& p) {
    static int generation = 0;
    ++generation;
    std::filesystem::last_write_time(
        p, std::filesystem::file_time_type::clock::now() + std::chrono::seconds(10 * generation));
}

const char* kRml = R"(<rml>
<head><link type="text/rcss" href="ui_test.rcss"/></head>
<body>
<div id="box"></div>
<div id="list"></div>
<input id="field" type="text" value=""/>
</body>
</rml>)";

const char* kRcss = R"(
body { font-family: Arial; font-size: 16dp; margin: 0; }
div { display: block; }
#box { position: absolute; left: 10dp; top: 10dp; width: 50dp; height: 30dp; background-color: #ff0000; }
#box:hover { background-color: #0000ff; }
#list { position: absolute; left: 100dp; top: 10dp; width: 160dp; height: 100dp; overflow-y: scroll; background-color: #202020; }
.row { height: 20dp; color: #ffffff; }
scrollbarvertical { width: 12dp; }
scrollbarvertical slidertrack { background-color: #101010; }
scrollbarvertical sliderbar { width: 12dp; background-color: #808080; }
scrollbarvertical sliderarrowdec, scrollbarvertical sliderarrowinc { width: 0; height: 0; }
input { position: absolute; left: 10dp; top: 150dp; width: 200dp; height: 24dp; background-color: #303060; color: #ffffff; }
)";

struct UiFixture {
    std::filesystem::path dir;
    std::unique_ptr<IRenderer> renderer;
    std::unique_ptr<UiContext> ui;
    Rml::ElementDocument* doc = nullptr;

    explicit UiFixture(const char* name, float scale = 1.0f) {
        dir = std::filesystem::temp_directory_path() / name;
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);
        write_text(dir / "ui_test.rml", kRml);
        write_text(dir / "ui_test.rcss", kRcss);

        RendererDesc rd;
        rd.size = {kW, kH};
        rd.software = true;
        auto created = create_renderer(rd);
        REQUIRE(created.has_value());
        renderer = std::move(created).value();

        UiDesc ud;
        ud.size = {kW, kH};
        ud.ui_scale = scale;
        ud.font_files.push_back(arial());
        auto ui_created = UiContext::create(*renderer, nullptr, ud);
        REQUIRE(ui_created.has_value());
        ui = std::move(ui_created).value();
        doc = ui->load_document(dir / "ui_test.rml");
        REQUIRE(doc != nullptr);

        std::string rows;
        for (int i = 1; i <= 100; ++i) {
            rows += "<div class=\"row\">Строка " + std::to_string(i) + " Ёжик</div>";
        }
        doc->GetElementById("list")->SetInnerRML(rows);
    }

    ~UiFixture() {
        ui.reset();  // before the renderer
        renderer.reset();
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }

    std::vector<std::uint8_t> frame() {
        REQUIRE(renderer->begin_frame({0, 0, 0, 1}));
        ui->update();
        ui->render();
        auto px = renderer->capture();
        renderer->end_frame();
        REQUIRE(px.size() == static_cast<std::size_t>(kW) * kH * 4);
        return px;
    }
};

}  // namespace

TEST_CASE("UI: documents render with RmlUi geometry, colours and text") {
    if (!std::filesystem::exists(arial())) {
        MESSAGE("Arial not found, skipping");
        return;
    }
    UiFixture fx("patriarch_ui_basic");
    const auto px = fx.frame();

    // The red box (10..60 x 10..40 dp) at 100% scale.
    const Rgb inside = pixel(px, 30, 25);
    CHECK(inside.r == 255);
    CHECK(inside.g == 0);
    CHECK(inside.b == 0);
    CHECK(pixel(px, 70, 25).r == 0);  // right of the box: clear colour

    // The list background and some white text pixels (Cyrillic rows) are present.
    int white = 0;
    for (int y = 12; y < 108; ++y) {
        for (int x = 102; x < 250; ++x) {
            const Rgb p = pixel(px, x, y);
            white += (p.r > 200 && p.g > 200 && p.b > 200) ? 1 : 0;
        }
    }
    CHECK(white > 200);
    CHECK(pixel(px, 240, 100).r == 0x20);  // list background colour

    // The rest of the screen stays untouched.
    CHECK(pixel(px, 400, 250).r == 0);
}

TEST_CASE("UI: hover styles apply and the UI reports whether it consumed the mouse") {
    if (!std::filesystem::exists(arial())) {
        return;
    }
    UiFixture fx("patriarch_ui_hover");
    (void)fx.frame();

    CHECK_FALSE(fx.ui->handle_event(event::MouseMove{400.0f, 250.0f, 0, 0}));  // empty area
    CHECK_FALSE(fx.ui->mouse_over_ui());
    CHECK(fx.ui->handle_event(event::MouseMove{30.0f, 25.0f, 0, 0}));  // over the box
    const auto hovered = fx.frame();
    CHECK(fx.ui->mouse_over_ui());
    CHECK(pixel(hovered, 30, 25).b == 255);
    CHECK(pixel(hovered, 30, 25).r == 0);

    fx.ui->handle_event(event::MouseMove{400.0f, 250.0f, 0, 0});
    const auto left = fx.frame();
    CHECK(pixel(left, 30, 25).r == 255);
}

TEST_CASE("UI: density-independent pixels follow the UI scale (100% to 200%)") {
    if (!std::filesystem::exists(arial())) {
        return;
    }
    UiFixture fx("patriarch_ui_scale");
    Rml::Element* box = fx.doc->GetElementById("box");
    for (const float scale : {1.0f, 1.25f, 1.5f, 2.0f}) {
        fx.ui->resize({kW, kH}, scale);
        const auto px = fx.frame();
        CHECK(fx.ui->ui_scale() == scale);
        CHECK(box->GetOffsetWidth() == doctest::Approx(50.0f * scale).epsilon(0.01));
        CHECK(box->GetOffsetHeight() == doctest::Approx(30.0f * scale).epsilon(0.01));
        // The box's far corner is red; just outside it is not.
        const int x_in = static_cast<int>(60.0f * scale) - 3;
        const int y_in = static_cast<int>(40.0f * scale) - 3;
        CHECK(pixel(px, x_in, y_in).r == 255);
        CHECK(pixel(px, x_in + 8, y_in + 8).r == 0);
    }
}

TEST_CASE("UI: wheel scrolls the list under the cursor and the event is consumed") {
    if (!std::filesystem::exists(arial())) {
        return;
    }
    UiFixture fx("patriarch_ui_scroll");
    Rml::Element* list = fx.doc->GetElementById("list");
    const auto before = fx.frame();
    CHECK(list->GetScrollTop() == 0.0f);

    CHECK(fx.ui->handle_event(event::MouseMove{150.0f, 60.0f, 0, 0}));
    CHECK(fx.ui->handle_event(event::MouseWheel{0.0f, -3.0f}));  // wheel towards the user = scroll down
    const auto after = fx.frame();
    CHECK(list->GetScrollTop() > 0.0f);

    std::size_t different = 0;
    for (int y = 12; y < 108; ++y) {
        for (int x = 102; x < 250; ++x) {
            const Rgb a = pixel(before, x, y);
            const Rgb b = pixel(after, x, y);
            different += (a.r != b.r || a.g != b.g || a.b != b.b) ? 1U : 0U;
        }
    }
    CHECK(different > 100);

    // Wheel over empty space is not consumed (the game would zoom the map).
    fx.ui->handle_event(event::MouseMove{400.0f, 250.0f, 0, 0});
    CHECK_FALSE(fx.ui->handle_event(event::MouseWheel{0.0f, -1.0f}));
}

TEST_CASE("UI: Cyrillic text input reaches a text field") {
    if (!std::filesystem::exists(arial())) {
        return;
    }
    UiFixture fx("patriarch_ui_input");
    (void)fx.frame();
    CHECK(fx.ui->handle_event(event::MouseMove{50.0f, 162.0f, 0, 0}));
    CHECK(fx.ui->handle_event(event::MouseButton{1, true, 1, 50.0f, 162.0f}));
    CHECK(fx.ui->handle_event(event::MouseButton{1, false, 1, 50.0f, 162.0f}));
    CHECK(fx.ui->handle_event(event::TextInput{"Пётр Алексеевич"}));
    (void)fx.frame();
    Rml::Element* field = fx.doc->GetElementById("field");
    REQUIRE(field != nullptr);
    CHECK(field->GetAttribute<Rml::String>("value", "") == "Пётр Алексеевич");
}

TEST_CASE("UI: edited .rcss files are hot-reloaded") {
    if (!std::filesystem::exists(arial())) {
        return;
    }
    UiFixture fx("patriarch_ui_reload");
    CHECK_FALSE(fx.ui->reload_if_changed());  // nothing changed yet
    const auto before = fx.frame();
    CHECK(pixel(before, 30, 25).r == 255);

    std::string edited = kRcss;
    const std::string needle = "background-color: #ff0000;";
    edited.replace(edited.find(needle), needle.size(), "background-color: #00ff00;");
    write_text(fx.dir / "ui_test.rcss", edited);
    touch_newer(fx.dir / "ui_test.rcss");

    CHECK(fx.ui->reload_if_changed());
    const auto after = fx.frame();
    CHECK(pixel(after, 30, 25).g == 255);
    CHECK(pixel(after, 30, 25).r == 0);
    CHECK_FALSE(fx.ui->reload_if_changed());  // settled
}

TEST_CASE("UI: only one context can exist at a time") {
    RendererDesc rd;
    rd.size = {64, 64};
    rd.software = true;
    auto renderer = create_renderer(rd);
    REQUIRE(renderer.has_value());
    UiDesc ud;
    auto first = UiContext::create(*renderer.value(), nullptr, ud);
    REQUIRE(first.has_value());
    auto second = UiContext::create(*renderer.value(), nullptr, ud);
    CHECK_FALSE(second.has_value());
    first.value().reset();
    auto third = UiContext::create(*renderer.value(), nullptr, ud);  // allowed again after the first is gone
    CHECK(third.has_value());
}

#endif  // _WIN32
