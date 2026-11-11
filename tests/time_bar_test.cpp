// The time bar (E3-T12) on an offscreen WARP target; uses the game fonts from assets/fonts.

#include "pat/engine/renderer.hpp"
#include "pat/engine/ui.hpp"
#include "pat/ui/time_bar.hpp"

#include <doctest/doctest.h>

#include "ui_loc_fixture.hpp"

#ifdef _WIN32

#include <RmlUi/Core.h>

#include <filesystem>
#include <vector>

#include "test_fonts.hpp"
#include "ui_theme_fixture.hpp"

namespace {

using namespace pat::engine;
using pat::ui::TimeBar;

TEST_CASE("TimeBar: shows the date, highlights the speed and reports button presses") {
    if (!pat::test::game_fonts_present()) {
        MESSAGE("Game fonts not found, skipping");
        return;
    }
    RendererDesc rd;
    rd.size = {1000, 300};
    rd.software = true;
    auto created = create_renderer(rd);
    REQUIRE(created.has_value());
    std::unique_ptr<IRenderer> renderer = std::move(created).value();

    UiDesc ud;
    ud.size = {1000, 300};
    ud.font_files = pat::test::game_font_files();
    auto ui_created = UiContext::create(*renderer, nullptr, ud);
    REQUIRE(ui_created.has_value());
    std::unique_ptr<UiContext> ui = std::move(ui_created).value();
    pat::test::install_game_theme(*ui);

    pat::loc::Localizer loc = pat::test::load_ui_localizer_with_english();
    auto bar_created =
        TimeBar::create(*ui, std::filesystem::path(PATRIARCH_SPIKE_UI_DIR) / "timebar.rml", loc);
    REQUIRE(bar_created.has_value());
    std::unique_ptr<TimeBar> bar = std::move(bar_created).value();

    std::vector<int> requested;
    bar->set_speed_handler([&](int speed) { requested.push_back(speed); });
    bar->set_date("12 января 1703");
    bar->set_speed(3);
    ui->update();

    Rml::ElementDocument* doc = bar->document();
    REQUIRE(doc != nullptr);
    CHECK(doc->GetElementById("date")->GetInnerRML() == "12 января 1703");
    CHECK(doc->GetElementById("spd-3")->IsClassSet("active"));
    CHECK_FALSE(doc->GetElementById("spd-1")->IsClassSet("active"));
    CHECK(bar->speed() == 3);

    // The pause caption comes from the string table and follows the language.
    CHECK(doc->GetElementById("spd-0")->GetInnerRML() == "Пауза");
    const std::string_view english[] = {"en", "ru"};
    REQUIRE(loc.set_chain(english) == 2);
    bar->relocalize();
    ui->update();
    CHECK(doc->GetElementById("spd-0")->GetInnerRML() == "Pause");
    const std::string_view russian[] = {"ru", "en"};
    loc.set_chain(russian);
    bar->relocalize();
    ui->update();
    CHECK(doc->GetElementById("spd-0")->GetInnerRML() == "Пауза");

    // Changing the model updates the view; setting the same value again is a no-op.
    bar->set_date("1 февраля 1703");
    bar->set_speed(0);
    ui->update();
    CHECK(doc->GetElementById("date")->GetInnerRML() == "1 февраля 1703");
    CHECK(doc->GetElementById("spd-0")->IsClassSet("active"));
    CHECK_FALSE(doc->GetElementById("spd-3")->IsClassSet("active"));

    // Buttons are clickable and call the handler with the speed (the bar itself does not change speed).
    const auto click = [&](const char* id) {
        Rml::Element* el = doc->GetElementById(id);
        const float x = el->GetAbsoluteLeft() + el->GetOffsetWidth() * 0.5f;
        const float y = el->GetAbsoluteTop() + el->GetOffsetHeight() * 0.5f;
        ui->handle_event(event::MouseMove{x, y, 0, 0});
        ui->update();
        ui->handle_event(event::MouseButton{1, true, 1, x, y});
        ui->handle_event(event::MouseButton{1, false, 1, x, y});
        ui->update();
    };
    click("spd-5");
    click("spd-0");
    click("spd-2");
    REQUIRE(requested.size() == 3);
    CHECK(requested[0] == 5);
    CHECK(requested[1] == 0);
    CHECK(requested[2] == 2);
    CHECK(bar->speed() == 0);

    bar.reset();
    ui.reset();
}

}  // namespace

#endif  // _WIN32
