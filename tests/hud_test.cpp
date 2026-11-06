// The heads-up display (design Olive Stone) on an offscreen WARP target; needs Arial (skipped without it).

#include "pat/engine/dpi.hpp"
#include "pat/engine/renderer.hpp"
#include "pat/engine/ui.hpp"
#include "pat/ui/hud_window.hpp"

#include <doctest/doctest.h>

TEST_CASE("design_ui_scale: follows the surface height relative to 1080p") {
    using namespace pat::engine;
    CHECK(design_ui_scale({1920, 1080}) == 1.0f);
    CHECK(design_ui_scale({1280, 720}) == doctest::Approx(0.625f));  // 0.667 snapped to 1/8 steps
    CHECK(design_ui_scale({3840, 2160}) == 2.0f);
    CHECK(design_ui_scale({1920, 1080}, 1.5f) == 1.5f);
    CHECK(design_ui_scale({100, 50}) == kMinUiScale);
    CHECK(design_ui_scale({8000, 8000}) == kMaxUiScale);
}

#ifdef _WIN32

#include <RmlUi/Core.h>

#include <filesystem>
#include <string>
#include <vector>

namespace {

using namespace pat::engine;
using pat::ui::HudResource;
using pat::ui::HudSnapshot;
using pat::ui::HudWindow;

struct Rgb {
    int r, g, b;
};

// data-for keeps its template element (hidden) next to the generated ones: count only visible rows.
Rml::ElementList visible_resources(Rml::ElementDocument* doc) {
    Rml::ElementList all;
    Rml::ElementList rows;
    doc->GetElementsByClassName(all, "res");
    for (Rml::Element* el : all) {
        if (el->IsVisible()) {
            rows.push_back(el);
        }
    }
    return rows;
}

TEST_CASE("HUD: shows the snapshot, hides empty badges, highlights and reports menu presses") {
    if (!std::filesystem::exists("C:/Windows/Fonts/arial.ttf")) {
        MESSAGE("Arial not found, skipping");
        return;
    }
    constexpr int kW = 1920;
    constexpr int kH = 1080;
    RendererDesc rd;
    rd.size = {kW, kH};
    rd.software = true;
    auto created = create_renderer(rd);
    REQUIRE(created.has_value());
    std::unique_ptr<IRenderer> renderer = std::move(created).value();

    UiDesc ud;
    ud.size = {kW, kH};
    ud.ui_scale = design_ui_scale({kW, kH});
    ud.font_files.emplace_back("C:/Windows/Fonts/arial.ttf");
    ud.font_files.emplace_back("C:/Windows/Fonts/arialbd.ttf");
    ud.font_files.emplace_back("C:/Windows/Fonts/times.ttf");
    ud.font_files.emplace_back("C:/Windows/Fonts/timesbd.ttf");
    auto ui_created = UiContext::create(*renderer, nullptr, ud);
    REQUIRE(ui_created.has_value());
    std::unique_ptr<UiContext> ui = std::move(ui_created).value();

    auto hud_created = HudWindow::create(*ui,
                                         std::filesystem::path(PATRIARCH_SPIKE_UI_DIR) / "hud.rml",
                                         std::filesystem::path(PATRIARCH_ASSETS_DIR) / "ui" / "olive");
    REQUIRE(hud_created.has_value());
    std::unique_ptr<HudWindow> hud = std::move(hud_created).value();

    HudSnapshot snap = pat::ui::make_test_hud();
    hud->set_snapshot(snap);
    ui->update();

    Rml::ElementDocument* doc = hud->document();
    REQUIRE(doc != nullptr);
    CHECK(doc->GetElementById("realm-name")->GetInnerRML() == "Московское царство");
    Rml::ElementList resources = visible_resources(doc);
    REQUIRE(resources.size() == 5);
    CHECK(resources[0]->GetChild(1)->GetInnerRML() == "Казна");
    CHECK(resources[0]->GetChild(2)->GetInnerRML() == "12 480");

    // Badges: the bell and the charter have pending items, the embassy has none.
    Rml::Element* bell = doc->GetElementById("alert-bell");
    Rml::Element* embassy = doc->GetElementById("alert-embassy");
    REQUIRE(bell != nullptr);
    REQUIRE(embassy != nullptr);
    CHECK(bell->GetChild(1)->IsVisible());
    CHECK_FALSE(embassy->GetChild(1)->IsVisible());

    // A negative delta gets the "neg" class; the resource list follows the snapshot.
    snap.resources.resize(2);
    snap.resources[1].delta = "-12";
    hud->set_snapshot(snap);
    ui->update();
    resources = visible_resources(doc);
    REQUIRE(resources.size() == 2);
    CHECK(resources[1]->GetChild(3)->IsClassSet("neg"));
    CHECK_FALSE(resources[0]->GetChild(3)->IsClassSet("neg"));

    // The left menu: the active entry is marked and presses reach the handler.
    CHECK(hud->active_menu() == "world");
    CHECK(doc->GetElementById("menu-world")->IsClassSet("active"));
    std::vector<std::string> pressed;
    hud->set_menu_handler([&](const std::string& key) {
        pressed.push_back(key);
        hud->set_active_menu(key);
    });
    Rml::Element* realm = doc->GetElementById("menu-realm");
    REQUIRE(realm != nullptr);
    const float x = realm->GetAbsoluteLeft() + realm->GetOffsetWidth() * 0.5f;
    const float y = realm->GetAbsoluteTop() + realm->GetOffsetHeight() * 0.5f;
    ui->handle_event(event::MouseMove{x, y, 0, 0});
    ui->update();
    ui->handle_event(event::MouseButton{1, true, 1, x, y});
    ui->handle_event(event::MouseButton{1, false, 1, x, y});
    ui->update();
    REQUIRE(pressed.size() == 1);
    CHECK(pressed[0] == "realm");
    CHECK(doc->GetElementById("menu-realm")->IsClassSet("active"));
    CHECK_FALSE(doc->GetElementById("menu-world")->IsClassSet("active"));

    // The plaque is drawn with the panel colour (#282F26) and the frame with its gold edge.
    REQUIRE(renderer->begin_frame({0, 0, 0, 1}));
    ui->update();
    ui->render();
    const auto px = renderer->capture();
    renderer->end_frame();
    REQUIRE(px.size() == static_cast<std::size_t>(kW) * kH * 4);
    const auto at = [&](int px_x, int px_y) {
        const std::size_t i = (static_cast<std::size_t>(px_y) * kW + static_cast<std::size_t>(px_x)) * 4;
        return Rgb{px[i], px[i + 1], px[i + 2]};
    };
    const Rgb plaque = at(240, 70);  // inside the realm plaque, away from text
    CHECK(plaque.r == doctest::Approx(0x28).epsilon(0.2));
    CHECK(plaque.g == doctest::Approx(0x2F).epsilon(0.2));
    CHECK(plaque.b == doctest::Approx(0x26).epsilon(0.2));
    CHECK(at(900, 3).r > 90);  // top edge of the frame

    hud.reset();
    ui.reset();
}

}  // namespace

#endif  // _WIN32
