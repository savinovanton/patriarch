// The frame of the map screen (UI-D04, board 01 of the mockup): top bar with the resources, left navigation
// and bottom bar, on an offscreen WARP target; uses the game fonts from assets/fonts.

#include "pat/engine/dpi.hpp"
#include "pat/engine/renderer.hpp"
#include "pat/engine/ui.hpp"
#include "pat/ui/hud_window.hpp"
#include "pat/ui/text.hpp"

#include <doctest/doctest.h>

#include "ui_layout_fixture.hpp"
#include "ui_loc_fixture.hpp"

TEST_CASE("design_ui_scale: min(w/1920, h/1080) times the user setting of 80-150 %") {
    using namespace pat::engine;
    CHECK(design_ui_scale({1920, 1080}) == 1.0f);
    CHECK(design_ui_scale({1280, 720}) == doctest::Approx(0.625f));  // 0.667 snapped to 1/8 steps
    CHECK(design_ui_scale({3840, 2160}) == 2.0f);
    CHECK(design_ui_scale({1920, 1080}, 1.5f) == 1.5f);
    CHECK(design_ui_scale({2560, 1080}) == 1.0f);                     // ultra-wide: the height decides
    CHECK(design_ui_scale({1920, 1200}) == 1.0f);                     // 16:10: the width decides
    CHECK(design_ui_scale({1280, 1080}) == doctest::Approx(0.625f));  // narrow window: the width decides
    CHECK(design_ui_scale({1920, 1080}, 2.0f) == 1.5f);               // the user setting stops at 150 %
    CHECK(design_ui_scale({1920, 1080}, 0.5f) == 0.75f);              // ... and at 80 % (snapped to 1/8)
    CHECK(design_ui_scale({100, 50}) == kMinUiScale);
    CHECK(design_ui_scale({8000, 8000}) == kMaxUiScale);
}

TEST_CASE("text helpers: the minus sign of a delta and upper case for Latin and Cyrillic") {
    const pat::loc::Localizer& loc = pat::test::ui_localizer();
    CHECK(pat::ui::format_delta(loc, 42) == "+42");
    CHECK(pat::ui::format_delta(loc, 0) == "0");
    CHECK(pat::ui::format_delta(loc, -12) == "\xE2\x88\x92"
                                             "12");  // U+2212, not a hyphen
    CHECK(pat::ui::upper_text("Голицыных") == "ГОЛИЦЫНЫХ");
    CHECK(pat::ui::upper_text("ёж Patriarch 1703") == "ЁЖ PATRIARCH 1703");
}

#ifdef _WIN32

#include <RmlUi/Core.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

#include "test_fonts.hpp"
#include "ui_theme_fixture.hpp"

namespace {

using namespace pat::engine;
using pat::test::find_layer;
using pat::test::Layer;
using pat::ui::HudNote;
using pat::ui::HudSnapshot;
using pat::ui::HudWindow;

constexpr const char* kNbsp = "\xC2\xA0";  // the group separator of {n:group}
constexpr const char* kBoard = "board_01_map_province.pd";

struct HudFixture {
    std::unique_ptr<IRenderer> renderer;
    std::unique_ptr<UiContext> ui;
    std::unique_ptr<HudWindow> hud;

    HudFixture(const pat::loc::Localizer& loc, const HudSnapshot& snapshot, PixelSize size = {1920, 1080}) {
        RendererDesc rd;
        rd.size = size;
        rd.software = true;
        auto created = create_renderer(rd);
        REQUIRE(created.has_value());
        renderer = std::move(created).value();
        UiDesc ud;
        ud.size = size;
        ud.ui_scale = design_ui_scale(size);
        ud.font_files = pat::test::game_font_files();
        auto ui_created = UiContext::create(*renderer, nullptr, ud);
        REQUIRE(ui_created.has_value());
        ui = std::move(ui_created).value();
        pat::test::install_game_theme(*ui);
        auto hud_created =
            HudWindow::create(*ui,
                              std::filesystem::path(PATRIARCH_SPIKE_UI_DIR) / "hud.rml",
                              std::filesystem::path(PATRIARCH_ASSETS_DIR) / "ui" / "patriarch" / "icons",
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

    Rml::Element* by_id(const char* id) const {
        Rml::Element* e = hud->document()->GetElementById(id);
        REQUIRE_MESSAGE(e != nullptr, "no element #" << id);
        return e;
    }
    // The first element of a class below `parent`.
    Rml::Element* first_of(Rml::Element* parent, const char* cls) const {
        Rml::ElementList found;
        parent->GetElementsByClassName(found, cls);
        REQUIRE_MESSAGE(!found.empty(), "no ." << cls);
        return found.front();
    }
    Rml::Element* first_tag(Rml::Element* parent, const char* tag) const {
        Rml::ElementList found;
        parent->GetElementsByTagName(found, tag);
        REQUIRE_MESSAGE(!found.empty(), "no <" << tag << ">");
        return found.front();
    }
    void click(const char* id) {
        Rml::Element* e = by_id(id);
        const float x = e->GetAbsoluteLeft() + e->GetOffsetWidth() * 0.5f;
        const float y = e->GetAbsoluteTop() + e->GetOffsetHeight() * 0.5f;
        ui->handle_event(event::MouseMove{x, y, 0, 0});
        ui->update();
        ui->handle_event(event::MouseButton{1, true, 1, x, y});
        ui->handle_event(event::MouseButton{1, false, 1, x, y});
        ui->update();
    }
    void tab() {
        ui->context()->ProcessKeyDown(Rml::Input::KI_TAB, 0);
        ui->context()->ProcessKeyUp(Rml::Input::KI_TAB, 0);
        ui->update();
    }
    [[nodiscard]] std::string focused() const {
        Rml::Element* f = ui->context()->GetFocusElement();
        return f != nullptr ? f->GetId() : std::string();
    }
};

bool near_px(double a, double b) {
    return std::abs(a - b) <= 1.0;
}

// The laid-out rectangle of an element equals the layer of the mockup (the acceptance of UI-D04: 1 px at
// 1920x1080).
void check_rect(Rml::Element* e, const Layer& layer, const char* what) {
    INFO(what);
    REQUIRE(layer.found);
    CHECK(near_px(e->GetAbsoluteLeft(), layer.x));
    CHECK(near_px(e->GetAbsoluteTop(), layer.y));
    CHECK(near_px(e->GetOffsetWidth(), layer.w));
    CHECK(near_px(e->GetOffsetHeight(), layer.h));
}
void check_pos(Rml::Element* e, const Layer& layer, const char* what) {
    INFO(what);
    REQUIRE(layer.found);
    CHECK(near_px(e->GetAbsoluteLeft(), layer.x));
    CHECK(near_px(e->GetAbsoluteTop(), layer.y));
}

bool fonts_ok() {
    if (!pat::test::game_fonts_present()) {
        MESSAGE("Game fonts not found, skipping");
        return false;
    }
    return true;
}

TEST_CASE("HUD: the frame is laid out like board 01 at 1920x1080") {
    if (!fonts_ok()) {
        return;
    }
    HudFixture fx(pat::test::ui_localizer(), pat::ui::make_test_hud());
    fx.hud->set_active_nav("holdings");
    fx.hud->set_nav_available("holdings", true);
    fx.ui->update();

    check_rect(fx.by_id("topbar"), find_layer(kBoard, "Top resource bar"), "top bar");
    check_rect(fx.by_id("topbar-rule"), find_layer(kBoard, "Brass top rule"), "brass rule");
    check_rect(fx.by_id("logo-icon"), find_layer(kBoard, "Icon / shield"), "shield");
    check_pos(
        fx.by_id("logo"), {true, 100, 13, 0, 0}, "PATRIARCH");  // the text layer starts at x = 100, y = 13
    check_pos(fx.by_id("divider-house"), find_layer(kBoard, "Header divider"), "divider after the house");
    check_pos(fx.by_id("divider-date"), find_layer(kBoard, "Date divider"), "divider before the date");

    // Five resources: icon, label and value follow the layers of the board.
    const char* keys[] = {"treasury", "prestige", "influence", "piety", "militia"};
    const char* icons[] = {"Icon / coin", "Icon / crown", "Icon / people", "Icon / church", "Icon / sword"};
    const char* labels[] = {"Казна", "Престиж", "Влияние", "Благочестие", "Ополчение"};
    for (int i = 0; i < 5; ++i) {
        Rml::Element* res = fx.by_id((std::string("res-") + keys[i]).c_str());
        check_rect(fx.first_tag(res, "svg"), find_layer(kBoard, icons[i]), icons[i]);
        Layer label = find_layer(kBoard, labels[i], true);
        check_pos(fx.first_of(res, "res-label"), label, labels[i]);
        CHECK(near_px(res->GetAbsoluteLeft(), 386.0 + 174.0 * i));
        CHECK(near_px(res->GetOffsetWidth(), 174.0));
    }
    check_pos(
        fx.first_of(fx.by_id("res-treasury"), "res-value"), find_layer(kBoard, "1 248 ₽", true), "value");
    check_pos(
        fx.first_of(fx.by_id("res-treasury"), "res-note"), find_layer(kBoard, "+42 / мес.", true), "note");

    // Left navigation: the bar, the first and the last entry, the gold indicator and the gear.
    check_rect(fx.by_id("nav"), find_layer(kBoard, "System navigation"), "navigation");
    check_rect(fx.by_id("nav-dynasty"), find_layer(kBoard, "Nav / Род"), "Nav / Род");
    check_rect(fx.by_id("nav-holdings"), find_layer(kBoard, "Nav / Владения"), "Nav / Владения");
    check_rect(fx.by_id("nav-chronicle"), find_layer(kBoard, "Nav / Журнал"), "Nav / Журнал");
    check_rect(fx.first_of(fx.by_id("nav-holdings"), "nav-mark"),
               find_layer(kBoard, "Active indicator"),
               "indicator");
    check_pos(
        fx.first_tag(fx.by_id("nav-dynasty"), "svg"), find_layer(kBoard, "Icon / dynasty"), "dynasty icon");
    check_pos(
        fx.first_tag(fx.by_id("nav-chronicle"), "svg"), find_layer(kBoard, "Icon / scroll"), "scroll icon");
    check_pos(fx.first_tag(fx.by_id("nav-system"), "svg"), find_layer(kBoard, "Icon / gear"), "gear");

    // Bottom bar.
    check_rect(fx.by_id("statusbar"), find_layer(kBoard, "Map status bar"), "status bar");
    check_pos(fx.by_id("map-title"), find_layer(kBoard, "Карта владений", true), "map title");
    CHECK(near_px(fx.by_id("mode-political")->GetAbsoluteLeft(), 344.0));
    CHECK(near_px(fx.by_id("hints")->GetAbsoluteLeft() + fx.by_id("hints")->GetOffsetWidth(), 1880.0));
}

TEST_CASE("HUD: the frame follows a wider and a smaller window") {
    if (!fonts_ok()) {
        return;
    }
    {
        // 21:9 at the same height: the bars stretch, the navigation keeps its width, the hints stay at the
        // right edge.
        HudFixture fx(pat::test::ui_localizer(), pat::ui::make_test_hud(), {2560, 1080});
        CHECK(near_px(fx.by_id("topbar")->GetOffsetWidth(), 2560.0));
        CHECK(near_px(fx.by_id("statusbar")->GetOffsetWidth(), 2560.0));
        CHECK(near_px(fx.by_id("nav")->GetOffsetWidth(), 96.0));
        CHECK(near_px(fx.by_id("nav")->GetOffsetHeight(), 940.0));
        CHECK(near_px(fx.by_id("hints")->GetAbsoluteLeft() + fx.by_id("hints")->GetOffsetWidth(), 2520.0));
    }
    {
        // 1280x720: everything is scaled by 0.625 (the scale is snapped to 1/8).
        HudFixture fx(pat::test::ui_localizer(), pat::ui::make_test_hud(), {1280, 720});
        const double s = design_ui_scale({1280, 720});
        CHECK(near_px(fx.by_id("topbar")->GetOffsetHeight(), 96.0 * s));
        CHECK(near_px(fx.by_id("nav")->GetOffsetWidth(), 96.0 * s));
        CHECK(near_px(fx.by_id("statusbar")->GetOffsetHeight(), 44.0 * s));
        CHECK(near_px(fx.by_id("nav-holdings")->GetAbsoluteTop(), 208.0 * s));
    }
}

TEST_CASE("HUD: resources show value, delta or caption with the colour of the sign") {
    if (!fonts_ok()) {
        return;
    }
    const pat::loc::Localizer& loc = pat::test::ui_localizer();
    HudFixture fx(loc, pat::ui::make_test_hud());
    const auto text_of = [&](const char* res, const char* cls) {
        return fx.first_of(fx.by_id(res), cls)->GetInnerRML();
    };
    CHECK(fx.by_id("logo")->GetInnerRML() == "PATRIARCH");
    CHECK(fx.by_id("house")->GetInnerRML() == "РОД ГОЛИЦЫНЫХ");
    CHECK(text_of("res-treasury", "res-label") == "Казна");
    CHECK(text_of("res-treasury", "res-value") == std::string("1") + kNbsp + "248 ₽");
    CHECK(text_of("res-treasury", "res-note") == "+42 / мес.");
    CHECK(text_of("res-prestige", "res-value") == "68");
    CHECK(text_of("res-influence", "res-note") == "при дворе");  // a fixed caption
    CHECK(text_of("res-piety", "res-note") == "благосклонность");
    CHECK(text_of("res-militia", "res-value") == std::string("1") + kNbsp + "240");
    CHECK(text_of("res-militia", "res-note") == std::string("из 1") + kNbsp + "600");  // a limit
    CHECK(fx.by_id("res-treasury")->GetAttribute<Rml::String>("title", "") ==
          std::string("Казна: 1") + kNbsp + "248 ₽ (+42 / мес.)");

    // A growth is green, a loss is red and has the true minus sign; a caption stays quiet.
    CHECK(fx.first_of(fx.by_id("res-treasury"), "res-note")->IsClassSet("pos"));
    HudSnapshot snap = pat::ui::make_test_hud();
    snap.resources[0].delta = -120;
    snap.resources[1].delta = 0;
    fx.hud->set_snapshot(snap);
    fx.ui->update();
    CHECK(text_of("res-treasury", "res-note") == "\xE2\x88\x92"
                                                 "120 / мес.");
    CHECK(fx.first_of(fx.by_id("res-treasury"), "res-note")->IsClassSet("neg"));
    CHECK_FALSE(fx.first_of(fx.by_id("res-prestige"), "res-note")->IsClassSet("pos"));
    CHECK_FALSE(fx.first_of(fx.by_id("res-influence"), "res-note")->IsClassSet("pos"));

    // The resource list follows the snapshot.
    snap.resources.resize(2);
    fx.hud->set_snapshot(snap);
    fx.ui->update();
    CHECK(fx.hud->document()->GetElementById("res-militia") == nullptr);
    CHECK(fx.hud->snapshot().resources.size() == 2);
}

TEST_CASE("HUD: theme colours of the bars and the pointer: the bars are UI, the map between them is not") {
    if (!fonts_ok()) {
        return;
    }
    HudFixture fx(pat::test::ui_localizer(), pat::ui::make_test_hud());
    const auto bg = [&](const char* id) { return fx.by_id(id)->GetComputedValues().background_color(); };
    for (const char* id : {"topbar", "nav", "statusbar"}) {
        const Rml::Colourb c = bg(id);
        CHECK(c.red == 0x1B);  // ink #1B2523
        CHECK(c.green == 0x25);
        CHECK(c.blue == 0x23);
    }
    CHECK(bg("topbar-rule").red == 0xD2);  // gold #D2B77D

    fx.ui->handle_event(event::MouseMove{900.0f, 600.0f, 0, 0});  // the map
    fx.ui->update();
    CHECK_FALSE(fx.ui->mouse_over_ui());
    for (const std::pair<float, float> p :
         {std::pair{900.0f, 40.0f}, std::pair{40.0f, 500.0f}, std::pair{900.0f, 1060.0f}}) {
        fx.ui->handle_event(event::MouseMove{p.first, p.second, 0, 0});
        fx.ui->update();
        CHECK(fx.ui->mouse_over_ui());  // top bar, navigation, bottom bar: no click falls through to the map
    }
}

TEST_CASE("HUD: navigation - active entry, entries without a screen, presses and hotkeys") {
    if (!fonts_ok()) {
        return;
    }
    HudFixture fx(pat::test::ui_localizer(), pat::ui::make_test_hud());
    HudWindow& hud = *fx.hud;
    CHECK(HudWindow::nav_key(0) == "dynasty");
    CHECK(HudWindow::nav_key(8) == "chronicle");
    CHECK(HudWindow::nav_key(9) == "system");
    CHECK(HudWindow::nav_key(10).empty());
    CHECK(hud.nav_name(0) == "Род");
    CHECK(hud.nav_name(3) == "Казна");
    CHECK(hud.nav_name(9) == "Настройки");

    // Nothing has a screen yet: every entry is dimmed with the hint, the document shows no active entry.
    CHECK(hud.active_nav().empty());
    for (int i = 0; i <= HudWindow::kNavCount; ++i) {
        const std::string id = "nav-" + std::string(HudWindow::nav_key(i));
        CHECK(fx.by_id(id.c_str())->IsClassSet("is-disabled"));
    }
    CHECK(fx.by_id("nav-court")->GetAttribute<Rml::String>("title", "") == "Скоро: экран ещё не готов");

    std::vector<std::string> pressed;
    hud.set_nav_handler([&](const std::string& key) { pressed.push_back(key); });
    fx.click("nav-court");
    hud.press_nav("court");
    CHECK(pressed.empty());  // a disabled entry ignores the mouse and the hotkey

    hud.set_nav_available("dynasty", true);
    hud.set_nav_available("holdings", true);
    hud.set_nav_available("system", true);
    hud.set_nav_available("no-such", true);  // ignored
    hud.set_active_nav("holdings");
    fx.ui->update();
    CHECK(hud.nav_available("dynasty"));
    CHECK_FALSE(hud.nav_available("court"));
    CHECK_FALSE(fx.by_id("nav-dynasty")->IsClassSet("is-disabled"));
    CHECK(fx.by_id("nav-holdings")->IsClassSet("active"));
    CHECK_FALSE(fx.by_id("nav-dynasty")->IsClassSet("active"));
    CHECK(fx.by_id("nav-dynasty")->GetAttribute<Rml::String>("title", "").empty());
    // The active entry is raised and shows the gold indicator; the others are on ink.
    CHECK(fx.by_id("nav-holdings")->GetComputedValues().background_color().red == 0x34);  // raised #34423B
    CHECK(fx.first_of(fx.by_id("nav-holdings"), "nav-mark")->IsVisible());
    CHECK_FALSE(fx.first_of(fx.by_id("nav-dynasty"), "nav-mark")->IsVisible());

    fx.click("nav-dynasty");
    fx.click("nav-system");
    hud.press_nav("holdings");
    REQUIRE(pressed.size() == 3);
    CHECK(pressed[0] == "dynasty");
    CHECK(pressed[1] == "system");
    CHECK(pressed[2] == "holdings");
    hud.set_active_nav("");
    fx.ui->update();
    CHECK_FALSE(fx.by_id("nav-holdings")->IsClassSet("active"));
}

TEST_CASE("HUD: Tab goes through the available navigation entries and then the map modes, Enter presses") {
    if (!fonts_ok()) {
        return;
    }
    HudFixture fx(pat::test::ui_localizer(), pat::ui::make_test_hud());
    HudWindow& hud = *fx.hud;
    hud.set_nav_available("dynasty", true);
    hud.set_nav_available("treasury", true);
    hud.set_nav_available("system", true);
    hud.set_mode_available("relief", true);
    fx.ui->update();

    // Disabled entries are no tab stops; the order is the order of the screen: navigation top to bottom, the
    // gear, then the modes of the bottom bar (the date and speed controls are the document of the time bar).
    const std::vector<std::string> expected = {
        "nav-dynasty", "nav-treasury", "nav-system", "mode-political", "mode-relief"};
    std::vector<std::string> seen;
    for (std::size_t i = 0; i < expected.size(); ++i) {
        fx.tab();
        seen.push_back(fx.focused());
    }
    CHECK(seen == expected);
    fx.tab();
    CHECK(fx.focused() == "nav-dynasty");  // wraps around

    // The ring after the keyboard: the border of the focused entry is gold; a mouse press removes it.
    CHECK(fx.by_id("nav-dynasty")->GetComputedValues().border_top_color().red == 0xD2);
    fx.click("nav-treasury");
    CHECK(fx.by_id("nav-treasury")->GetComputedValues().border_top_color().alpha == 0);

    std::vector<std::string> pressed;
    hud.set_nav_handler([&](const std::string& key) { pressed.push_back(key); });
    fx.tab();  // from nav-treasury to nav-system
    fx.tab();
    REQUIRE(fx.focused() == "mode-political");
    hud.set_mode_handler([&](const std::string& key) { pressed.push_back("mode:" + key); });
    fx.ui->context()->ProcessKeyDown(Rml::Input::KI_RETURN, 0);
    fx.ui->context()->ProcessKeyUp(Rml::Input::KI_RETURN, 0);
    fx.ui->update();
    REQUIRE(pressed.size() == 1);
    CHECK(pressed[0] == "mode:political");
}

TEST_CASE("HUD: map modes - the active one is marked, unavailable ones ignore presses, M cycles") {
    if (!fonts_ok()) {
        return;
    }
    HudFixture fx(pat::test::ui_localizer(), pat::ui::make_test_hud());
    HudWindow& hud = *fx.hud;
    CHECK(HudWindow::mode_key(0) == "political");
    CHECK(HudWindow::mode_key(3) == "faith");
    CHECK(hud.map_mode() == "political");
    CHECK(fx.by_id("mode-political")->IsClassSet("active"));
    CHECK(fx.by_id("mode-political")->GetInnerRML() == "Политическая");
    CHECK(fx.by_id("mode-income")->IsClassSet("is-disabled"));
    CHECK(fx.by_id("mode-income")->GetAttribute<Rml::String>("title", "") ==
          "Скоро: наложение ещё не готово");
    CHECK(fx.by_id("mode-political")->GetComputedValues().border_bottom_color().red ==
          0xD2);  // gold underline
    CHECK(fx.by_id("map-title")->GetInnerRML() == "Карта владений");
    CHECK(fx.by_id("hints")->GetInnerRML() == "ЛКМ — выбрать · ПКМ — назад · M — режим карты");

    std::vector<std::string> pressed;
    hud.set_mode_handler([&](const std::string& key) {
        pressed.push_back(key);
        hud.set_map_mode(key);
    });
    fx.click("mode-income");  // not available
    hud.press_mode("faith");
    CHECK(pressed.empty());
    CHECK(hud.next_mode() == "political");  // the only available one

    hud.set_mode_available("relief", true);
    CHECK(hud.mode_available("relief"));
    CHECK_FALSE(hud.mode_available("income"));
    CHECK(hud.next_mode() == "relief");
    fx.click("mode-relief");
    fx.ui->update();
    REQUIRE(pressed.size() == 1);
    CHECK(pressed[0] == "relief");
    CHECK(fx.by_id("mode-relief")->IsClassSet("active"));
    CHECK_FALSE(fx.by_id("mode-political")->IsClassSet("active"));
    CHECK(hud.next_mode() == "political");  // wraps around over the unavailable ones
    hud.set_map_mode("no-such");            // ignored
    CHECK(hud.map_mode() == "relief");
}

TEST_CASE("HUD: every text comes from the string table and follows the language") {
    if (!fonts_ok()) {
        return;
    }
    pat::loc::Localizer loc = pat::test::load_ui_localizer_with_english();
    HudFixture fx(loc, pat::ui::make_test_hud());
    CHECK(fx.hud->document()->GetInnerRML().find("[[") == std::string::npos);
    CHECK(fx.hud->nav_name(0) == "Род");

    const std::string_view english[] = {"en", "ru"};
    REQUIRE(loc.set_chain(english) == 2);
    fx.hud->relocalize();
    fx.ui->update();
    CHECK(fx.hud->nav_name(0) == "Dynasty");
    CHECK(fx.hud->nav_name(1) == "Владения");  // not in the English table: falls back to Russian
    CHECK(fx.by_id("map-title")->GetInnerRML() == "Realm map");
    CHECK(fx.first_of(fx.by_id("res-treasury"), "res-label")->GetInnerRML() == "Treasury");

    const std::string_view russian[] = {"ru", "en"};
    loc.set_chain(russian);
    fx.hud->relocalize();
    fx.ui->update();
    CHECK(fx.hud->nav_name(0) == "Род");
    CHECK(fx.by_id("map-title")->GetInnerRML() == "Карта владений");
}

TEST_CASE("HUD: a missing key shows as [[key]] and does not break the window") {
    if (!fonts_ok()) {
        return;
    }
    const pat::loc::Localizer empty = pat::test::load_empty_localizer();
    HudSnapshot snap;
    snap.house = "Голицыных";
    snap.resources = {{"treasury", 100, 5, 0, HudNote::Delta}};
    HudFixture fx(empty, snap);
    CHECK(fx.hud->nav_name(0) == "[[ui.nav.dynasty]]");
    CHECK(fx.by_id("map-title")->GetInnerRML() == "[[ui.hud.map_title]]");
    CHECK(fx.first_of(fx.by_id("res-treasury"), "res-label")->GetInnerRML() == "[[ui.res.treasury]]");
    CHECK(near_px(fx.by_id("topbar")->GetOffsetHeight(), 96.0));
    const auto missing = empty.requested_missing_keys();
    CHECK(std::find(missing.begin(), missing.end(), "ui.nav.dynasty") != missing.end());
}

}  // namespace

#endif  // _WIN32
