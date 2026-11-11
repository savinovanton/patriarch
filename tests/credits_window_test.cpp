// Credits window tests (UI-213): the snapshot built from data/credits (game::credits_snapshot), the window
// bound to it, the close button, the captions from the string table and a language change, on an offscreen
// WARP target. Text needs a font: the game fonts from assets/fonts.

#include "pat/content/credits_content.hpp"
#include "pat/data/content_source.hpp"
#include "pat/engine/renderer.hpp"
#include "pat/engine/ui.hpp"
#include "pat/game/credits_view.hpp"
#include "pat/ui/credits_window.hpp"

#include <doctest/doctest.h>

#include "ui_loc_fixture.hpp"

#ifdef _WIN32

#include <RmlUi/Core.h>

#include <algorithm>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "test_fonts.hpp"
#include "ui_theme_fixture.hpp"

namespace {

using namespace pat::engine;
using pat::ui::CreditsSnapshot;
using pat::ui::CreditsWindow;

constexpr int kW = 900;
constexpr int kH = 700;

bool have_font() {
    return pat::test::game_fonts_present();
}

pat::content::CreditsList shipped_credits() {
    const pat::data::DirectorySource dir{std::string(PATRIARCH_DATA_DIR)};
    pat::content::CreditsResult result = pat::content::load_credits_content(dir);
    REQUIRE(result.has_value());
    return std::move(result).value();
}

struct Fixture {
    std::filesystem::path dir;
    std::unique_ptr<IRenderer> renderer;
    std::unique_ptr<UiContext> ui;
    std::unique_ptr<CreditsWindow> window;
    pat::loc::Localizer loc;

    explicit Fixture(const char* name, pat::loc::Localizer texts = pat::test::load_ui_localizer())
        : loc(std::move(texts)) {
        dir = std::filesystem::temp_directory_path() / name;
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);
        const std::filesystem::path source(PATRIARCH_SPIKE_UI_DIR);
        for (const char* file : {"credits.rml", "credits.rcss"}) {
            std::filesystem::copy_file(source / file, dir / file);
        }

        RendererDesc rd;
        rd.size = {kW, kH};
        rd.software = true;
        auto created = create_renderer(rd);
        REQUIRE(created.has_value());
        renderer = std::move(created).value();

        UiDesc ud;
        ud.size = {kW, kH};
        ud.ui_scale = 1.0f;
        ud.font_files = pat::test::game_font_files();
        auto ui_created = UiContext::create(*renderer, nullptr, ud);
        REQUIRE(ui_created.has_value());
        ui = std::move(ui_created).value();
        pat::test::install_game_theme(*ui);

        auto w = CreditsWindow::create(*ui, dir / "credits.rml", loc);
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

    void frame() {
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

    /// Texts of the shown elements of a class (RmlUi keeps the hidden template element of a data-for loop
    /// and hides the elements of a false data-if instead of removing them).
    std::vector<std::string> shown_texts(const char* cls) {
        Rml::ElementList list;
        window->document()->GetElementsByClassName(list, cls);
        std::vector<std::string> out;
        for (Rml::Element* el : list) {
            if (el->IsVisible(true)) {
                out.push_back(el->GetInnerRML());
            }
        }
        return out;
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

#endif  // _WIN32

TEST_CASE(
    "credits snapshot: sections follow the entries, keys come from the table, plain texts stay as they are") {
    const pat::content::CreditsList credits = shipped_credits();
    const pat::loc::Localizer& texts = pat::test::ui_localizer();
    const pat::ui::CreditsSnapshot snapshot = pat::game::credits_snapshot(texts, credits);

    REQUIRE_FALSE(snapshot.sections.empty());
    CHECK(snapshot.sections.front().title == "Авторы");
    std::size_t items = 0;
    for (const pat::ui::CreditsSection& section : snapshot.sections) {
        CHECK_FALSE(section.items.empty());
        for (const pat::ui::CreditsItem& item : section.items) {
            CHECK_FALSE(item.name.empty());
            CHECK(item.name.find("[[") == std::string::npos);  // no missing key
            CHECK(item.text.find("[[") == std::string::npos);
            CHECK(item.meta.find("[[") == std::string::npos);
        }
        items += section.items.size();
    }
    CHECK(items == credits.entries.size());

    const auto data_section =
        std::ranges::find(snapshot.sections, "Источники данных", &pat::ui::CreditsSection::title);
    REQUIRE(data_section != snapshot.sections.end());
    const auto rnb = std::ranges::find(
        data_section->items, "Российская национальная библиотека", &pat::ui::CreditsItem::name);
    CHECK(rnb != data_section->items.end());
    const auto lakes = std::ranges::find_if(data_section->items, [](const pat::ui::CreditsItem& item) {
        return item.name.starts_with("HydroLAKES");
    });
    REQUIRE(lakes != data_section->items.end());
    CHECK(lakes->meta.find("Лицензия: CC-BY-4.0") != std::string::npos);
}

TEST_CASE("credits snapshot: an empty list gives no sections") {
    CHECK(
        pat::game::credits_snapshot(pat::test::ui_localizer(), pat::content::CreditsList{}).sections.empty());
}

#ifdef _WIN32

TEST_CASE("CreditsWindow: starts hidden, show and toggle open it, the close button hides it") {
    if (!have_font()) {
        MESSAGE("Game fonts not found, skipping");
        return;
    }
    Fixture fx("patriarch_credits_visibility");
    fx.window->set_snapshot(pat::game::credits_snapshot(fx.loc, shipped_credits()));
    fx.frame();
    CHECK_FALSE(fx.window->visible());
    CHECK_FALSE(fx.window->document()->IsVisible());

    fx.window->show(true);
    fx.frame();
    CHECK(fx.window->visible());
    CHECK(fx.window->document()->IsVisible());

    fx.window->toggle();
    fx.frame();
    CHECK_FALSE(fx.window->visible());
    fx.window->toggle();
    fx.frame();

    int closed = 0;
    fx.window->set_close_handler([&closed] { ++closed; });
    Rml::Element* close = fx.by_id("close");
    REQUIRE(close != nullptr);
    fx.click(close);
    fx.frame();
    CHECK(closed == 1);
    CHECK_FALSE(fx.window->visible());
    CHECK_FALSE(fx.window->document()->IsVisible());
}

TEST_CASE("CreditsWindow: the shipped credits are listed, with the National Library of Russia") {
    if (!have_font()) {
        MESSAGE("Game fonts not found, skipping");
        return;
    }
    Fixture fx("patriarch_credits_content");
    const pat::content::CreditsList credits = shipped_credits();
    fx.window->set_snapshot(pat::game::credits_snapshot(fx.loc, credits));
    fx.window->show(true);
    fx.frame();

    CHECK(fx.by_id("title")->GetInnerRML() == "Титры");
    CHECK(fx.by_id("close")->GetInnerRML() == "Закрыть");
    CHECK_FALSE(fx.by_id("empty")->IsVisible(true));

    const std::vector<std::string> names = fx.shown_texts("item-name");
    CHECK(names.size() == credits.entries.size());
    CHECK(std::ranges::find(names, "Российская национальная библиотека") != names.end());
    CHECK(fx.shown_texts("section-title").size() == fx.window->snapshot().sections.size());
}

TEST_CASE("CreditsWindow: an empty list shows the empty note; a snapshot replaces it live") {
    if (!have_font()) {
        MESSAGE("Game fonts not found, skipping");
        return;
    }
    Fixture fx("patriarch_credits_empty");
    fx.window->show(true);
    fx.frame();
    CHECK(fx.by_id("empty")->IsVisible(true));
    CHECK(fx.by_id("empty")->GetInnerRML() == "Список пуст");
    CHECK(fx.shown_texts("item").empty());

    CreditsSnapshot s;
    s.sections.push_back({"Раздел", {{"Имя", "Мета", "Текст"}, {"Другое", "", ""}}});
    fx.window->set_snapshot(s);
    fx.frame();
    CHECK_FALSE(fx.by_id("empty")->IsVisible(true));
    CHECK(fx.shown_texts("item-name") == std::vector<std::string>{"Имя", "Другое"});
    CHECK(fx.shown_texts("item-meta") == std::vector<std::string>{"Мета"});  // an empty line is not shown
    CHECK(fx.shown_texts("item-text") == std::vector<std::string>{"Текст"});
}

TEST_CASE("CreditsWindow: captions follow the language chain, a missing key shows as [[key]]") {
    if (!have_font()) {
        MESSAGE("Game fonts not found, skipping");
        return;
    }
    pat::data::DirectorySource base(PATRIARCH_DATA_DIR);
    pat::data::MemorySource english("english");
    english.add("loc/en/credits.pd", "format_version = 1\nui.credits.title = \"Credits\"\n");
    const pat::data::ContentSource* roots[] = {&base, &english};
    pat::loc::LocLoadResult loaded = pat::loc::load_localization(roots);
    REQUIRE(loaded.ok());
    {
        Fixture fx("patriarch_credits_language", std::move(loaded.localizer));
        fx.frame();
        CHECK(fx.by_id("title")->GetInnerRML() == "Титры");
        const std::string_view english_chain[] = {"en", "ru"};
        REQUIRE(fx.loc.set_chain(english_chain) == 2);
        fx.window->relocalize();
        fx.frame();
        CHECK(fx.by_id("title")->GetInnerRML() == "Credits");
        CHECK(fx.by_id("close")->GetInnerRML() == "Закрыть");  // not translated: falls back to Russian
    }

    // A second UI context cannot exist next to the first one: the table without keys gets its own scope.
    Fixture bare("patriarch_credits_missing", pat::test::load_empty_localizer());
    bare.frame();
    CHECK(bare.by_id("title")->GetInnerRML() == "[[ui.credits.title]]");
}

#endif  // _WIN32
