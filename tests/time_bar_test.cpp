// The date and speed controls of the top bar (UI-D04, board 01) on an offscreen WARP target; uses the game
// fonts from assets/fonts.

#include "pat/engine/renderer.hpp"
#include "pat/engine/ui.hpp"
#include "pat/ui/time_bar.hpp"

#include <doctest/doctest.h>

#include "ui_layout_fixture.hpp"
#include "ui_loc_fixture.hpp"

#ifdef _WIN32

#include <RmlUi/Core.h>

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
using pat::ui::TimeBar;

constexpr const char* kBoard = "board_01_map_province.pd";

struct BarFixture {
    std::unique_ptr<IRenderer> renderer;
    std::unique_ptr<UiContext> ui;
    std::unique_ptr<TimeBar> bar;
    Rml::ElementDocument* doc = nullptr;

    explicit BarFixture(const pat::loc::Localizer& loc, PixelSize size = {1920, 1080}) {
        RendererDesc rd;
        rd.size = size;
        rd.software = true;
        auto created = create_renderer(rd);
        REQUIRE(created.has_value());
        renderer = std::move(created).value();
        UiDesc ud;
        ud.size = size;
        ud.font_files = pat::test::game_font_files();
        auto ui_created = UiContext::create(*renderer, nullptr, ud);
        REQUIRE(ui_created.has_value());
        ui = std::move(ui_created).value();
        pat::test::install_game_theme(*ui);
        auto bar_created =
            TimeBar::create(*ui,
                            std::filesystem::path(PATRIARCH_SPIKE_UI_DIR) / "timebar.rml",
                            std::filesystem::path(PATRIARCH_ASSETS_DIR) / "ui" / "patriarch" / "icons",
                            loc);
        REQUIRE(bar_created.has_value());
        bar = std::move(bar_created).value();
        ui->update();
        doc = bar->document();
        REQUIRE(doc != nullptr);
    }
    ~BarFixture() {
        bar.reset();
        ui.reset();
        renderer.reset();
    }

    Rml::Element* by_id(const char* id) const {
        Rml::Element* e = doc->GetElementById(id);
        REQUIRE_MESSAGE(e != nullptr, "no element #" << id);
        return e;
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
    void key(Rml::Input::KeyIdentifier k) {
        ui->context()->ProcessKeyDown(k, 0);
        ui->context()->ProcessKeyUp(k, 0);
        ui->update();
    }
};

bool near_px(double a, double b) {
    return std::abs(a - b) <= 1.0;
}

void check_rect(Rml::Element* e, const Layer& layer, const char* what) {
    INFO(what);
    REQUIRE(layer.found);
    CHECK(near_px(e->GetAbsoluteLeft(), layer.x));
    CHECK(near_px(e->GetAbsoluteTop(), layer.y));
    CHECK(near_px(e->GetOffsetWidth(), layer.w));
    CHECK(near_px(e->GetOffsetHeight(), layer.h));
}

bool fonts_ok() {
    if (!pat::test::game_fonts_present()) {
        MESSAGE("Game fonts not found, skipping");
        return false;
    }
    return true;
}

bool lit(Rml::Element* e) {  // a speed segment lit gold (#D2B77D) rather than raised (#34423B)
    return e->GetComputedValues().background_color().red == 0xD2;
}

TEST_CASE("TimeBar: the controls are laid out like board 01 at 1920x1080") {
    if (!fonts_ok()) {
        return;
    }
    BarFixture fx(pat::test::ui_localizer());
    fx.bar->set_date("1 января 1703");
    fx.bar->set_speed(0);
    fx.ui->update();
    check_rect(fx.by_id("badge-paused"), find_layer(kBoard, "Badge / НА ПАУЗЕ"), "pause badge");
    check_rect(fx.by_id("pause"), find_layer(kBoard, "Pause / Space"), "pause button");
    for (int n = 1; n <= 5; ++n) {
        const std::string id = "spd-" + std::to_string(n);
        check_rect(fx.by_id(id.c_str()), find_layer(kBoard, "Speed " + std::to_string(n)), id.c_str());
    }
    const Layer date = find_layer(kBoard, "1 января 1703", true);
    CHECK(near_px(fx.by_id("date")->GetAbsoluteLeft(), date.x));
    CHECK(near_px(fx.by_id("date")->GetAbsoluteTop(), date.y));
    const Layer calendar = find_layer(kBoard, "ЮЛИАНСКИЙ КАЛЕНДАРЬ", true);
    CHECK(near_px(fx.by_id("calendar")->GetAbsoluteLeft(), calendar.x));
    CHECK(near_px(fx.by_id("calendar")->GetAbsoluteTop(), calendar.y));
    const Layer hint = find_layer(kBoard, "Пробел — продолжить", true);
    CHECK(near_px(fx.by_id("hint")->GetAbsoluteLeft(), hint.x));
    CHECK(near_px(fx.by_id("hint")->GetAbsoluteTop(), hint.y));
}

TEST_CASE("TimeBar: shows the date, the pause state and the speed, and reports presses") {
    if (!fonts_ok()) {
        return;
    }
    pat::loc::Localizer loc = pat::test::load_ui_localizer_with_english();
    BarFixture fx(loc);
    TimeBar& bar = *fx.bar;
    std::vector<int> requested;
    bar.set_speed_handler([&](int speed) { requested.push_back(speed); });

    bar.set_date("12 января 1703");
    bar.set_speed(3);
    fx.ui->update();
    CHECK(fx.by_id("date")->GetInnerRML() == "12 января 1703");
    CHECK(fx.by_id("calendar")->GetInnerRML() == "ЮЛИАНСКИЙ КАЛЕНДАРЬ");
    CHECK(bar.speed() == 3);
    // Running: segments 1..3 are lit, there is no pause badge, the hint says what Space does.
    CHECK(lit(fx.by_id("spd-1")));
    CHECK(lit(fx.by_id("spd-3")));
    CHECK_FALSE(lit(fx.by_id("spd-4")));
    CHECK_FALSE(fx.by_id("badge-paused")->IsVisible());
    CHECK(fx.by_id("hint")->GetInnerRML() == "Пробел — пауза");

    // Paused: the badge appears, the segments keep showing the speed the game resumes at.
    bar.set_speed(0);
    fx.ui->update();
    CHECK(fx.by_id("badge-paused")->IsVisible());
    CHECK(fx.by_id("badge-paused")->GetInnerRML() == "НА ПАУЗЕ");
    CHECK(fx.by_id("hint")->GetInnerRML() == "Пробел — продолжить");
    CHECK(bar.resume_speed() == 3);
    CHECK(lit(fx.by_id("spd-3")));
    CHECK_FALSE(lit(fx.by_id("spd-5")));
    bar.set_speed(9);  // out of range: ignored
    CHECK(bar.speed() == 0);

    // Buttons call the handler; the bar itself does not change the speed.
    fx.click("spd-5");
    fx.click("pause");  // paused: resumes at the remembered speed
    bar.set_speed(2);
    fx.ui->update();
    fx.click("pause");  // running: pauses
    bar.toggle_pause();
    bar.press_speed(4);
    bar.press_speed(7);  // ignored
    REQUIRE(requested.size() == 5);
    CHECK(requested[0] == 5);
    CHECK(requested[1] == 3);  // resume speed at the time of the press
    CHECK(requested[2] == 0);
    CHECK(requested[3] == 0);  // the bar was still running at 2 (the handler did not change it): pause
    CHECK(requested[4] == 4);
    CHECK(bar.speed() == 2);

    // The language: the badge and the hint follow after relocalize().
    bar.set_speed(0);
    fx.ui->update();
    const std::string_view english[] = {"en", "ru"};
    REQUIRE(loc.set_chain(english) == 2);
    bar.relocalize();
    fx.ui->update();
    CHECK(fx.by_id("badge-paused")->GetInnerRML() == "PAUSED");
    CHECK(fx.by_id("hint")->GetInnerRML() == "Space - resume");
    CHECK(fx.by_id("calendar")->GetInnerRML() == "ЮЛИАНСКИЙ КАЛЕНДАРЬ");  // not translated: falls back
    const std::string_view russian[] = {"ru", "en"};
    loc.set_chain(russian);
    bar.relocalize();
    fx.ui->update();
    CHECK(fx.by_id("badge-paused")->GetInnerRML() == "НА ПАУЗЕ");

    // Setting the same date again is a no-op; a new date shows.
    bar.set_date("1 февраля 1703");
    fx.ui->update();
    CHECK(fx.by_id("date")->GetInnerRML() == "1 февраля 1703");
}

TEST_CASE(
    "TimeBar: Tab goes pause then the speeds, Enter presses, the ring appears only after the keyboard") {
    if (!fonts_ok()) {
        return;
    }
    BarFixture fx(pat::test::ui_localizer());
    std::vector<int> requested;
    fx.bar->set_speed_handler([&](int speed) { requested.push_back(speed); });
    const std::vector<std::string> expected = {"pause", "spd-1", "spd-2", "spd-3", "spd-4", "spd-5"};
    std::vector<std::string> seen;
    for (std::size_t i = 0; i < expected.size(); ++i) {
        fx.key(Rml::Input::KI_TAB);
        seen.push_back(fx.ui->context()->GetFocusElement()->GetId());
    }
    CHECK(seen == expected);
    // The focused segment wears a light ring (border of 2 px), the others none.
    CHECK(fx.by_id("spd-5")->GetComputedValues().border_top_width() == 2.0f);
    CHECK(fx.by_id("spd-4")->GetComputedValues().border_top_width() == 0.0f);
    CHECK(near_px(fx.by_id("spd-5")->GetOffsetWidth(), 17.0));  // the ring does not change the size
    fx.key(Rml::Input::KI_RETURN);
    REQUIRE(requested.size() == 1);
    CHECK(requested[0] == 5);
    fx.click("spd-2");  // a press with the mouse takes the ring away
    CHECK(fx.by_id("spd-2")->GetComputedValues().border_top_width() == 0.0f);
    CHECK(requested.back() == 2);
}

TEST_CASE("TimeBar: the controls keep to the right edge of a wider window") {
    if (!fonts_ok()) {
        return;
    }
    BarFixture fx(pat::test::ui_localizer(), {2560, 1080});
    fx.bar->set_speed(0);
    fx.ui->update();
    CHECK(near_px(fx.by_id("spd-5")->GetAbsoluteLeft(), 1852.0 + 640.0));
    CHECK(near_px(fx.by_id("date")->GetAbsoluteLeft(), 1290.0));  // the date stays with the resources
}

}  // namespace

#endif  // _WIN32
