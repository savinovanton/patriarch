// The heads-up display (design Olive Stone) on an offscreen WARP target; uses the game fonts from
// assets/fonts.

#include "pat/engine/dpi.hpp"
#include "pat/engine/renderer.hpp"
#include "pat/engine/ui.hpp"
#include "pat/ui/hud_window.hpp"

#include <doctest/doctest.h>

#include "ui_loc_fixture.hpp"

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

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

#include "test_fonts.hpp"

namespace {

using namespace pat::engine;
using pat::ui::HudSnapshot;
using pat::ui::HudWindow;

constexpr const char* kNbsp = "\xC2\xA0";  // the group separator of {n:group}

struct HudFixture {
    static constexpr int kW = 1920;
    static constexpr int kH = 1080;
    std::unique_ptr<IRenderer> renderer;
    std::unique_ptr<UiContext> ui;
    std::unique_ptr<HudWindow> hud;

    HudFixture(const pat::loc::Localizer& loc, const HudSnapshot& snapshot) {
        RendererDesc rd;
        rd.size = {kW, kH};
        rd.software = true;
        auto created = create_renderer(rd);
        REQUIRE(created.has_value());
        renderer = std::move(created).value();
        UiDesc ud;
        ud.size = {kW, kH};
        ud.ui_scale = design_ui_scale({kW, kH});
        ud.font_files = pat::test::game_font_files();
        auto ui_created = UiContext::create(*renderer, nullptr, ud);
        REQUIRE(ui_created.has_value());
        ui = std::move(ui_created).value();
        auto hud_created = HudWindow::create(*ui,
                                             std::filesystem::path(PATRIARCH_SPIKE_UI_DIR) / "hud.rml",
                                             std::filesystem::path(PATRIARCH_ASSETS_DIR) / "ui" / "olive",
                                             loc);
        REQUIRE(hud_created.has_value());
        hud = std::move(hud_created).value();
        hud->set_snapshot(snapshot);
        ui->update();
    }

    ~HudFixture() {
        hud.reset();  // window before ui, ui before renderer
        ui.reset();
        renderer.reset();
    }

    Rml::Element* by_id(const char* id) { return hud->document()->GetElementById(id); }
};

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
    if (!pat::test::game_fonts_present()) {
        MESSAGE("Game fonts not found, skipping");
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
    ud.font_files = pat::test::game_font_files();
    auto ui_created = UiContext::create(*renderer, nullptr, ud);
    REQUIRE(ui_created.has_value());
    std::unique_ptr<UiContext> ui = std::move(ui_created).value();

    auto hud_created = HudWindow::create(*ui,
                                         std::filesystem::path(PATRIARCH_SPIKE_UI_DIR) / "hud.rml",
                                         std::filesystem::path(PATRIARCH_ASSETS_DIR) / "ui" / "olive",
                                         pat::test::ui_localizer());
    REQUIRE(hud_created.has_value());
    std::unique_ptr<HudWindow> hud = std::move(hud_created).value();

    HudSnapshot snap = pat::ui::make_test_hud(pat::test::ui_localizer());
    hud->set_snapshot(snap);
    ui->update();

    Rml::ElementDocument* doc = hud->document();
    REQUIRE(doc != nullptr);
    CHECK(doc->GetElementById("realm-name")->GetInnerRML() == "Московское царство");
    Rml::ElementList resources = visible_resources(doc);
    REQUIRE(resources.size() == 5);
    CHECK(resources[0]->GetChild(1)->GetInnerRML() == "Казна");
    CHECK(resources[0]->GetChild(2)->GetInnerRML() == std::string("12") + kNbsp + "480");
    CHECK(resources[0]->GetChild(3)->GetInnerRML() == "+84");
    CHECK(resources[0]->GetAttribute<Rml::String>("title", "") ==
          std::string("Казна: 12") + kNbsp + "480 (+84)");

    // Badges: the bell and the charter have pending items, the embassy has none.
    Rml::Element* bell = doc->GetElementById("alert-bell");
    Rml::Element* embassy = doc->GetElementById("alert-embassy");
    REQUIRE(bell != nullptr);
    REQUIRE(embassy != nullptr);
    CHECK(bell->GetChild(1)->IsVisible());
    CHECK_FALSE(embassy->GetChild(1)->IsVisible());

    // A negative delta gets the "neg" class; the resource list follows the snapshot.
    snap.resources.resize(2);
    snap.resources[1].delta = -12;
    hud->set_snapshot(snap);
    ui->update();
    resources = visible_resources(doc);
    REQUIRE(resources.size() == 2);
    CHECK(resources[1]->GetChild(3)->IsClassSet("neg"));
    CHECK_FALSE(resources[0]->GetChild(3)->IsClassSet("neg"));

    // The HUD covers the screen with its document but must let the mouse through to the map: only its
    // panels count as UI.
    ui->handle_event(event::MouseMove{900.0f, 600.0f, 0, 0});
    ui->update();
    CHECK_FALSE(ui->mouse_over_ui());
    ui->handle_event(event::MouseMove{140.0f, 50.0f, 0, 0});  // the realm plaque
    ui->update();
    CHECK(ui->mouse_over_ui());
    ui->handle_event(event::MouseMove{900.0f, 600.0f, 0, 0});
    ui->update();
    CHECK_FALSE(ui->mouse_over_ui());

    // The right panel: tabs switch the holdings list, the armies block and the cards follow the snapshot.
    const auto visible_rows = [&]() {
        Rml::ElementList all;
        std::size_t n = 0;
        doc->GetElementsByClassName(all, "row");
        for (Rml::Element* el : all) {
            n += el->IsVisible() ? 1 : 0;
        }
        return n;
    };
    snap = pat::ui::make_test_hud(pat::test::ui_localizer());
    hud->set_snapshot(snap);
    ui->update();
    CHECK(hud->tab() == "cities");
    CHECK(visible_rows() == 5 + 1);  // five cities and one army
    hud->set_tab("dynasties");
    hud->set_tab("no-such-tab");  // ignored
    ui->update();
    CHECK(hud->tab() == "dynasties");
    CHECK(visible_rows() == 3 + 1);
    CHECK(doc->GetElementById("tab-dynasties")->IsClassSet("active"));
    CHECK_FALSE(doc->GetElementById("tab-cities")->IsClassSet("active"));
    hud->set_tab("cities");
    ui->update();
    Rml::ElementList cards;
    doc->GetElementsByClassName(cards, "affair");
    std::size_t visible_cards = 0;
    for (Rml::Element* el : cards) {
        visible_cards += el->IsVisible() ? 1 : 0;
    }
    CHECK(visible_cards == 2);
    Rml::ElementList lines;
    doc->GetElementsByClassName(lines, "line");
    std::size_t visible_lines = 0;
    for (Rml::Element* el : lines) {
        visible_lines += el->IsVisible() ? 1 : 0;
    }
    CHECK(visible_lines == 2);

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

TEST_CASE("HUD: every caption, card and chronicle line comes from the string table") {
    if (!pat::test::game_fonts_present()) {
        MESSAGE("Game fonts not found, skipping");
        return;
    }
    const pat::loc::Localizer& loc = pat::test::ui_localizer();
    HudFixture fx(loc, pat::ui::make_test_hud(loc));
    Rml::ElementDocument* doc = fx.hud->document();
    REQUIRE(doc != nullptr);

    CHECK(fx.by_id("affairs-title")->GetInnerRML() == "ДЕЛА ГОСУДАРСТВА");
    CHECK(fx.by_id("tab-cities")->GetInnerRML() == "Города");
    CHECK(fx.by_id("tab-armies")->GetInnerRML() == "Войска");
    CHECK(fx.by_id("tab-dynasties")->GetInnerRML() == "Династии");
    CHECK(fx.hud->menu_name(0) == "Мир");
    CHECK(fx.hud->menu_name(8) == "Меню");
    CHECK(fx.hud->menu_name(9).empty());
    CHECK(fx.by_id("menu-chronicle")->GetAttribute<Rml::String>("title", "") == "Летопись");

    // Cards and chronicle lines use parameters and plural forms: "1 вопрос", not a fixed text.
    const std::string all = doc->GetInnerRML();
    CHECK(all.find("1 вопрос в повестке") != std::string::npos);
    CHECK(all.find("Ожидают приёма при дворе") != std::string::npos);
    CHECK(all.find("Посольство прибыло в Москву") != std::string::npos);
    CHECK(all.find("94 т") != std::string::npos);  // thousands format of the city list

    HudSnapshot snap = pat::ui::make_test_hud(loc);
    snap.affairs[0].count = 5;
    fx.hud->set_snapshot(snap);
    fx.ui->update();
    CHECK(doc->GetInnerRML().find("5 вопросов в повестке") != std::string::npos);
    snap.affairs[0].count = 2;
    fx.hud->set_snapshot(snap);
    fx.ui->update();
    CHECK(doc->GetInnerRML().find("2 вопроса в повестке") != std::string::npos);
}

TEST_CASE("HUD: changing the language changes the texts after relocalize()") {
    if (!pat::test::game_fonts_present()) {
        MESSAGE("Game fonts not found, skipping");
        return;
    }
    pat::loc::Localizer loc = pat::test::load_ui_localizer_with_english();
    HudFixture fx(loc, pat::ui::make_test_hud(loc));
    Rml::ElementDocument* doc = fx.hud->document();
    REQUIRE(doc != nullptr);
    CHECK(fx.by_id("tab-cities")->GetInnerRML() == "Города");

    const std::string_view english[] = {"en", "ru"};
    REQUIRE(loc.set_chain(english) == 2);
    fx.hud->relocalize();
    fx.ui->update();
    CHECK(fx.by_id("tab-cities")->GetInnerRML() == "Cities");
    CHECK(fx.by_id("tab-armies")->GetInnerRML() == "Войска");  // not in the English table: falls back
    Rml::ElementList resources = visible_resources(doc);
    REQUIRE(resources.size() == 5);
    CHECK(resources[0]->GetChild(1)->GetInnerRML() == "Treasury");
    CHECK(doc->GetInnerRML().find("1 item on the agenda") != std::string::npos);  // English plural

    const std::string_view russian[] = {"ru", "en"};
    loc.set_chain(russian);
    fx.hud->relocalize();
    fx.ui->update();
    CHECK(fx.by_id("tab-cities")->GetInnerRML() == "Города");
    CHECK(visible_resources(doc)[0]->GetChild(1)->GetInnerRML() == "Казна");
}

TEST_CASE("HUD: a missing key shows as [[key]] and does not break the window") {
    if (!pat::test::game_fonts_present()) {
        MESSAGE("Game fonts not found, skipping");
        return;
    }
    const pat::loc::Localizer empty = pat::test::load_empty_localizer();
    HudSnapshot snap;
    snap.resources = {{"treasury", 100, 5}};
    snap.chronicle = {{"10:00", "ui.no.such.key", 0}};
    HudFixture fx(empty, snap);
    Rml::ElementDocument* doc = fx.hud->document();
    REQUIRE(doc != nullptr);
    CHECK(fx.by_id("tab-cities")->GetInnerRML() == "[[ui.hud.tab.cities]]");
    CHECK(fx.hud->menu_name(0) == "[[ui.menu.world]]");
    CHECK(visible_resources(doc)[0]->GetChild(1)->GetInnerRML() == "[[ui.res.treasury]]");
    CHECK(doc->GetInnerRML().find("[[ui.no.such.key]]") != std::string::npos);
    const auto missing = empty.requested_missing_keys();
    CHECK(std::find(missing.begin(), missing.end(), "ui.no.such.key") != missing.end());
}

}  // namespace

#endif  // _WIN32
