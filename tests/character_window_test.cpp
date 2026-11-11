// Character window tests (E3-T07): data binding, tabs, the virtualised 1000-row list, commands and hot
// reload, on an offscreen WARP target. Text uses the game fonts from assets/fonts.

#include "pat/engine/renderer.hpp"
#include "pat/engine/ui.hpp"
#include "pat/ui/character_window.hpp"

#include <doctest/doctest.h>

#include "ui_loc_fixture.hpp"

#ifdef _WIN32

#include <RmlUi/Core.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "test_fonts.hpp"

namespace {

using namespace pat::engine;
using pat::ui::CharacterSnapshot;
using pat::ui::CharacterWindow;

constexpr int kW = 900;
constexpr int kH = 700;

bool have_font() {
    return pat::test::game_fonts_present();
}

void touch_newer(const std::filesystem::path& p) {
    static int generation = 0;
    ++generation;
    std::filesystem::last_write_time(
        p, std::filesystem::file_time_type::clock::now() + std::chrono::seconds(10 * generation));
}

struct Fixture {
    std::filesystem::path dir;
    std::unique_ptr<IRenderer> renderer;
    std::unique_ptr<UiContext> ui;
    std::unique_ptr<CharacterWindow> window;
    pat::loc::Localizer loc = pat::test::load_ui_localizer_with_english();

    explicit Fixture(const char* name, float scale = 1.0f) {
        dir = std::filesystem::temp_directory_path() / name;
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);
        const std::filesystem::path source(PATRIARCH_SPIKE_UI_DIR);
        std::filesystem::copy_file(source / "character.rml", dir / "character.rml");
        std::filesystem::copy_file(source / "character.rcss", dir / "character.rcss");
        std::filesystem::copy_file(source / "olive.rcss", dir / "olive.rcss");

        RendererDesc rd;
        rd.size = {kW, kH};
        rd.software = true;
        auto created = create_renderer(rd);
        REQUIRE(created.has_value());
        renderer = std::move(created).value();

        UiDesc ud;
        ud.size = {kW, kH};
        ud.ui_scale = scale;
        ud.font_files = pat::test::game_font_files();
        auto ui_created = UiContext::create(*renderer, nullptr, ud);
        REQUIRE(ui_created.has_value());
        ui = std::move(ui_created).value();

        auto w = CharacterWindow::create(*ui, dir / "character.rml", loc);
        REQUIRE(w.has_value());
        window = std::move(w).value();
    }

    ~Fixture() {
        window.reset();  // window before ui, ui before renderer
        ui.reset();
        renderer.reset();
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }

    /// One UI frame: window bookkeeping, layout, and a render so scrolling geometry exists.
    void frame() {
        window->update();
        ui->update();
        if (renderer->begin_frame({0, 0, 0, 1})) {
            ui->render();
            renderer->end_frame();
        }
    }

    Rml::Element* by_id(const char* id) {
        Rml::ElementDocument* doc = window->document();
        return doc != nullptr ? doc->GetElementById(id) : nullptr;
    }

    /// Elements of a class that are actually shown: RmlUi keeps the hidden template element of a
    /// data-for loop and hides (display: none) elements of a false data-if instead of removing them.
    std::size_t count_class(const char* cls) {
        Rml::ElementList list;
        window->document()->GetElementsByClassName(list, cls);
        std::size_t shown = 0;
        for (Rml::Element* el : list) {
            shown += el->IsVisible(true) ? 1U : 0U;
        }
        return shown;
    }

    /// The element if it exists and is shown, else null.
    Rml::Element* shown_by_id(const char* id) {
        Rml::Element* el = by_id(id);
        return (el != nullptr && el->IsVisible(true)) ? el : nullptr;
    }

    void click(Rml::Element* el) {
        const float x = el->GetAbsoluteLeft() + el->GetOffsetWidth() * 0.5f;
        const float y = el->GetAbsoluteTop() + el->GetOffsetHeight() * 0.5f;
        ui->handle_event(event::MouseMove{x, y, 0, 0});
        ui->handle_event(event::MouseButton{1, true, 1, x, y});
        ui->handle_event(event::MouseButton{1, false, 1, x, y});
    }
};

}  // namespace

TEST_CASE("CharacterWindow: snapshot data is bound into the document and updates live") {
    if (!have_font()) {
        MESSAGE("Game fonts not found, skipping");
        return;
    }
    Fixture fx("patriarch_char_binding");
    CharacterSnapshot s = pat::ui::make_test_character(pat::test::ui_localizer(), 3);
    fx.window->set_snapshot(s);
    fx.frame();

    CHECK(fx.by_id("name")->GetInnerRML() == s.full_name());
    CHECK(fx.by_id("subtitle")->GetInnerRML().find(std::to_string(s.age)) != std::string::npos);
    CHECK(fx.count_class("skill") == pat::ui::kSkillCount);
    CHECK(fx.count_class("trait") == s.traits.size());

    // Bar fills are proportional to the skill values.
    Rml::ElementList fills;
    Rml::ElementList bars;
    {
        Rml::ElementList all_fills;
        Rml::ElementList all_bars;
        fx.window->document()->GetElementsByClassName(all_fills, "fill");
        fx.window->document()->GetElementsByClassName(all_bars, "bar");
        for (Rml::Element* e : all_fills) {
            if (e->IsVisible(true)) {
                fills.push_back(e);
            }
        }
        for (Rml::Element* e : all_bars) {
            if (e->IsVisible(true)) {
                bars.push_back(e);
            }
        }
    }
    REQUIRE(fills.size() == pat::ui::kSkillCount);
    for (std::size_t i = 0; i < pat::ui::kSkillCount; ++i) {
        const float full = bars[i]->GetClientWidth();
        CHECK(fills[i]->GetOffsetWidth() ==
              doctest::Approx(full * static_cast<float>(s.skills[i]) / pat::ui::kSkillMax)
                  .epsilon(0.02)
                  .scale(full));
    }

    // The model drives the view: a new snapshot changes text and bars without touching the DOM by hand.
    CharacterSnapshot next = s;
    ++next.age;
    next.skills[0] = 20;
    next.given_name = "Пётр";
    next.traits.push_back("Щедрый");
    fx.window->set_snapshot(next);
    fx.frame();
    CHECK(fx.by_id("name")->GetInnerRML() == next.full_name());
    CHECK(fx.by_id("subtitle")->GetInnerRML().find(std::to_string(next.age)) != std::string::npos);
    CHECK(fx.count_class("trait") == next.traits.size());
    CHECK(fills[0]->GetOffsetWidth() == doctest::Approx(bars[0]->GetClientWidth()).epsilon(0.02));
}

TEST_CASE("CharacterWindow: tabs switch the content") {
    if (!have_font()) {
        return;
    }
    Fixture fx("patriarch_char_tabs");
    const CharacterSnapshot s = pat::ui::make_test_character(pat::test::ui_localizer(), 5);
    fx.window->set_snapshot(s);
    fx.frame();
    CHECK(fx.window->tab() == "overview");
    CHECK(fx.shown_by_id("rel_list") == nullptr);
    CHECK(fx.count_class("skill") == pat::ui::kSkillCount);

    fx.window->set_tab("family");
    fx.frame();
    CHECK(fx.count_class("skill") == 0);
    CHECK(fx.count_class("line") == s.family.size());

    // Tabs are clickable.
    fx.click(fx.by_id("tab-relations"));
    fx.frame();
    CHECK(fx.window->tab() == "relations");
    CHECK(fx.shown_by_id("rel_list") != nullptr);
    fx.click(fx.by_id("tab-overview"));
    fx.frame();
    CHECK(fx.window->tab() == "overview");
    CHECK(fx.count_class("skill") == pat::ui::kSkillCount);
}

TEST_CASE("CharacterWindow: the 1000-row list is virtual - few DOM rows, full scroll range") {
    if (!have_font()) {
        return;
    }
    for (const float scale : {1.0f, 1.5f, 2.0f}) {
        Fixture fx("patriarch_char_list", scale);
        const CharacterSnapshot s = pat::ui::make_test_character(pat::test::ui_localizer(), 9, 1000);
        fx.window->set_snapshot(s);
        fx.window->set_tab("relations");
        fx.frame();
        fx.frame();  // layout, then the window sizes the row window from the real viewport

        Rml::Element* list = fx.by_id("rel_list");
        REQUIRE(list != nullptr);
        CHECK(fx.by_id("rel-total")->GetInnerRML() == "Знакомые и мнения (всего: 1000)");

        const std::size_t rows = fx.window->rows_in_document();
        CHECK(rows > 5);
        CHECK(rows < 60);  // a window around the viewport, not 1000 elements
        CHECK(fx.count_class("row") == rows);

        // The spacers keep the scrollable height equal to 1000 rows at any scale.
        const float row_px = CharacterWindow::kRowHeightDp * scale;
        CHECK(list->GetScrollHeight() == doctest::Approx(1000.0f * row_px).epsilon(0.01));

        // Scroll to the middle: the row window follows, the first visible row has the right text.
        list->SetScrollTop(500.0f * row_px);
        fx.frame();
        fx.frame();
        CHECK(fx.window->first_row_in_document() >= 490);
        CHECK(fx.window->first_row_in_document() <= 500);
        CHECK(fx.window->rows_in_document() < 60);
        Rml::ElementList rows_now;
        fx.window->document()->GetElementsByClassName(rows_now, "row");
        bool found_501 = false;
        for (Rml::Element* r : rows_now) {
            found_501 = found_501 || r->GetInnerRML().rfind("501. ", 0) == 0;
        }
        CHECK(found_501);
        CHECK(list->GetScrollHeight() == doctest::Approx(1000.0f * row_px).epsilon(0.01));  // unchanged

        // Scroll to the very end: the last row exists.
        list->SetScrollTop(list->GetScrollHeight());
        fx.frame();
        fx.frame();
        rows_now.clear();
        fx.window->document()->GetElementsByClassName(rows_now, "row");
        bool found_last = false;
        for (Rml::Element* r : rows_now) {
            found_last = found_last || r->GetInnerRML().rfind("1000. ", 0) == 0;
        }
        CHECK(found_last);
        CHECK(fx.window->first_row_in_document() + fx.window->rows_in_document() == 1000);
    }
}

TEST_CASE("CharacterWindow: wheel scrolls the list, small and empty lists work") {
    if (!have_font()) {
        return;
    }
    Fixture fx("patriarch_char_wheel");
    fx.window->set_snapshot(pat::ui::make_test_character(pat::test::ui_localizer(), 2, 1000));
    fx.window->set_tab("relations");
    fx.frame();
    fx.frame();
    Rml::Element* list = fx.by_id("rel_list");
    const float x = list->GetAbsoluteLeft() + 50.0f;
    const float y = list->GetAbsoluteTop() + 50.0f;
    CHECK(fx.ui->handle_event(event::MouseMove{x, y, 0, 0}));
    for (int i = 0; i < 10; ++i) {
        CHECK(fx.ui->handle_event(event::MouseWheel{0.0f, -1.0f}));  // one notch each
    }
    // RmlUi scrolls smoothly over real time, so let some frames pass.
    for (int i = 0; i < 40; ++i) {
        fx.frame();
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
    CHECK(list->GetScrollTop() > 100.0f);
    CHECK(fx.window->first_row_in_document() > 0);

    for (const std::size_t count : {std::size_t{0}, std::size_t{1}, std::size_t{7}}) {
        fx.window->set_snapshot(pat::ui::make_test_character(pat::test::ui_localizer(), 4, count));
        fx.frame();
        fx.frame();
        CHECK(fx.window->rows_in_document() == count);
        CHECK(fx.by_id("rel-total")->GetInnerRML() ==
              "Знакомые и мнения (всего: " + std::to_string(count) + ")");
    }
}

TEST_CASE("CharacterWindow: command buttons call the handler and show a status") {
    if (!have_font()) {
        return;
    }
    Fixture fx("patriarch_char_commands");
    fx.window->set_snapshot(pat::ui::make_test_character(pat::test::ui_localizer(), 6));
    std::vector<std::string> received;
    fx.window->set_command_handler([&](const std::string& id) { received.push_back(id); });
    fx.frame();

    fx.click(fx.by_id("cmd-marry"));
    fx.frame();
    fx.click(fx.by_id("cmd-educate"));
    fx.frame();
    REQUIRE(received.size() == 2);
    CHECK(received[0] == "marry");
    CHECK(received[1] == "educate");
    CHECK(fx.by_id("statusbar")->GetInnerRML().find("educate") != std::string::npos);
}

TEST_CASE("CharacterWindow: survives a hot reload of its document") {
    if (!have_font()) {
        return;
    }
    Fixture fx("patriarch_char_reload");
    const CharacterSnapshot s = pat::ui::make_test_character(pat::test::ui_localizer(), 8);
    fx.window->set_snapshot(s);
    fx.window->set_tab("relations");
    fx.frame();
    fx.frame();

    // Edit the stylesheet: a new accent colour for the name.
    const auto rcss = fx.dir / "character.rcss";
    std::string text;
    {
        std::ifstream in(rcss, std::ios::binary);
        text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    text += "\n#name { color: #00ff00; }\n";
    {
        std::ofstream out(rcss, std::ios::binary | std::ios::trunc);
        out << text;
    }
    touch_newer(rcss);

    CHECK(fx.ui->reload_if_changed());
    fx.frame();
    fx.frame();
    REQUIRE(fx.window->document() != nullptr);
    CHECK(fx.by_id("name")->GetInnerRML() == s.full_name());  // still bound to the model
    CHECK(fx.shown_by_id("rel_list") != nullptr);             // tab state lives in the model
    CHECK(fx.window->rows_in_document() < 60);
}

TEST_CASE("CharacterWindow: captions, subtitle and statuses come from the string table") {
    if (!have_font()) {
        MESSAGE("Game fonts not found, skipping");
        return;
    }
    Fixture fx("patriarch_char_loc");
    CharacterSnapshot s = pat::ui::make_test_character(pat::test::ui_localizer(), 4);
    s.role = "ui.character.role.stolnik";
    s.health = "ui.health.strong";
    s.age = 42;
    fx.window->set_snapshot(s);
    fx.frame();

    CHECK(fx.by_id("subtitle")->GetInnerRML() == "стольник · 42 года · здоровье: крепкое");
    CHECK(fx.by_id("tab-overview")->GetInnerRML() == "Обзор");
    CHECK(fx.by_id("cmd-marry")->GetInnerRML() == "Предложить брак");
    Rml::ElementList names;
    fx.window->document()->GetElementsByClassName(names, "skill-name");
    std::vector<std::string> skill_names;
    for (Rml::Element* e : names) {
        if (e->IsVisible(true)) {
            skill_names.push_back(e->GetInnerRML());
        }
    }
    REQUIRE(skill_names.size() == pat::ui::kSkillCount);
    CHECK(skill_names[0] == "Хозяйство");
    CHECK(skill_names[4] == "Учёность");

    // A command press writes the status line with the id as a parameter.
    fx.click(fx.by_id("cmd-educate"));
    fx.frame();
    CHECK(fx.by_id("statusbar")->GetInnerRML() == "Команда: educate (заглушка)");

    // Another language: relocalize() rewrites captions, subtitle (with its plural) and the status line.
    const std::string_view english[] = {"en", "ru"};
    REQUIRE(fx.loc.set_chain(english) == 2);
    fx.window->relocalize();
    fx.frame();
    CHECK(fx.by_id("subtitle")->GetInnerRML() == "stolnik - 42 years - health: sturdy");
    CHECK(fx.by_id("tab-overview")->GetInnerRML() == "Overview");
    CHECK(fx.by_id("tab-family")->GetInnerRML() == "Семья");  // not in the English table: falls back
    CHECK(fx.by_id("statusbar")->GetInnerRML() == "Команда: educate (заглушка)");
    s.age = 1;
    fx.window->set_snapshot(s);
    fx.frame();
    CHECK(fx.by_id("subtitle")->GetInnerRML() == "stolnik - 1 year - health: sturdy");  // English plural
}

TEST_CASE("CharacterWindow: a missing key shows as [[key]] and does not break the window") {
    if (!have_font()) {
        MESSAGE("Game fonts not found, skipping");
        return;
    }
    Fixture fx("patriarch_char_missing");
    fx.loc = pat::test::load_empty_localizer();  // same object, so the window keeps reading it
    CharacterSnapshot s = pat::ui::make_test_character(pat::test::ui_localizer(), 4);
    fx.window->set_snapshot(s);
    fx.frame();
    CHECK(fx.by_id("tab-overview")->GetInnerRML() == "[[ui.character.tab.overview]]");
    CHECK(fx.by_id("subtitle")->GetInnerRML().find("[[ui.character.subtitle]]") != std::string::npos);
    CHECK(fx.by_id("name")->GetInnerRML() == s.full_name());
    const auto missing = fx.loc.requested_missing_keys();
    CHECK(std::find(missing.begin(), missing.end(), "ui.character.tab.overview") != missing.end());
}

#endif  // _WIN32
