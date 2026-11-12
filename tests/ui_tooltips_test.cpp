// Tooltips in the new design (UI-D03, #258): the tokens against the boards "Подсказка / ..." and
// "Персонаж / подсказка / ...", the four board variants as static samples of the gallery page, a live tooltip
// driven by data (hover, keyboard focus, click to pin, the close button), and the warning tone of an
// unavailability reason.

#include "pat/data/pd_parser.hpp"
#include "pat/engine/renderer.hpp"
#include "pat/engine/ui.hpp"
#include "pat/ui/component_gallery.hpp"
#include "pat/ui/theme.hpp"
#include "pat/ui/tooltip_manager.hpp"

#include <doctest/doctest.h>

#include "ui_loc_fixture.hpp"

#ifdef _WIN32

#include <RmlUi/Core.h>

#include <cmath>
#include <filesystem>
#include <memory>
#include <string>

#include "test_fonts.hpp"
#include "ui_layout_fixture.hpp"
#include "ui_theme_fixture.hpp"

namespace {

using namespace pat::engine;
using pat::ui::ComponentGallery;
using pat::ui::TooltipManager;

constexpr int kScancodeTab = 43;
constexpr int kScancodeEscape = 41;

bool fonts_ok() {
    if (!pat::test::game_fonts_present()) {
        MESSAGE("Game fonts not found, skipping");
        return false;
    }
    return true;
}

bool is_colour(const Rml::Colourb& c, const char* token) {
    static const std::shared_ptr<const pat::ui::Theme> theme = pat::test::load_game_theme();
    const auto* theme_color = theme->find_color(token);
    REQUIRE(theme_color != nullptr);
    return c.red == theme_color->r && c.green == theme_color->g && c.blue == theme_color->b;
}

struct Fixture {
    std::unique_ptr<IRenderer> renderer;
    std::unique_ptr<UiContext> ui;
    std::unique_ptr<ComponentGallery> gallery;
    std::unique_ptr<TooltipManager> tooltips;
    Rml::ElementDocument* doc = nullptr;

    explicit Fixture(bool tips_page) {
        RendererDesc rd;
        rd.size = {1920, 1080};
        rd.software = true;
        auto created = create_renderer(rd);
        REQUIRE(created.has_value());
        renderer = std::move(created).value();
        UiDesc ud;
        ud.size = {1920, 1080};
        ud.font_files = pat::test::game_font_files();
        auto ui_created = UiContext::create(*renderer, nullptr, ud);
        REQUIRE(ui_created.has_value());
        ui = std::move(ui_created).value();
        pat::test::install_game_theme(*ui);
        const std::filesystem::path icons =
            std::filesystem::path(PATRIARCH_ASSETS_DIR) / "ui" / "patriarch" / "icons";
        auto page = ComponentGallery::create(*ui,
                                             std::filesystem::path(PATRIARCH_SPIKE_UI_DIR) / "gallery.rml",
                                             icons,
                                             pat::test::ui_localizer());
        REQUIRE(page.has_value());
        gallery = std::move(page).value();
        if (tips_page) {
            gallery->set_page("tips");
        }
        auto tm = TooltipManager::create(*ui, PATRIARCH_SPIKE_UI_DIR);
        REQUIRE(tm.has_value());
        tooltips = std::move(tm).value();
        tooltips->set_icons_directory(icons);
        tooltips->set_provider([this](std::string_view key) { return gallery->explanation(key); });
        ui->update();
        doc = gallery->document();
        REQUIRE(doc != nullptr);
    }
    ~Fixture() {
        tooltips.reset();
        gallery.reset();
        ui.reset();
    }

    Rml::Element* el(const char* id) const {
        Rml::Element* e = doc->GetElementById(id);
        REQUIRE_MESSAGE(e != nullptr, "no element #" << id);
        return e;
    }

    void step(float dt) {
        tooltips->update(dt);
        ui->update();
    }
    void run(float seconds) {
        for (float t = 0.0f; t < seconds - 1e-4f; t += 0.05f) {
            step(0.05f);
        }
    }
    void send(const Event& e) {
        ui->handle_event(e);
        tooltips->handle_event(e);
    }
    void move_to(float x, float y) {
        send(event::MouseMove{x, y, 0, 0});
        step(0.0f);
    }
    void hover(Rml::Element* e) {
        move_to(e->GetAbsoluteLeft() + e->GetOffsetWidth() * 0.5f,
                e->GetAbsoluteTop() + e->GetOffsetHeight() * 0.5f);
    }
    void click_at(float x, float y) {
        move_to(x, y);
        send(event::MouseButton{1, true, 1, x, y});
        send(event::MouseButton{1, false, 1, x, y});
        step(0.0f);
    }
    void tab() {
        const Event e = event::Key{kScancodeTab, true, false, 0};
        ui->context()->ProcessKeyDown(Rml::Input::KI_TAB, 0);
        ui->context()->ProcessKeyUp(Rml::Input::KI_TAB, 0);
        tooltips->handle_event(e);  // RmlUi got the key above; the manager only needs to know about it
        step(0.0f);
    }
    [[nodiscard]] std::string focused() const {
        Rml::Element* e = ui->context()->GetFocusElement();
        return e != nullptr ? e->GetId() : std::string();
    }
    /// The first element with `cls` inside the document of an open tooltip level.
    [[nodiscard]] Rml::Element* in_level(std::size_t level, const char* cls) const {
        Rml::ElementDocument* d = tooltips->level_document(level);
        REQUIRE(d != nullptr);
        Rml::ElementList list;
        d->GetElementsByClassName(list, cls);
        REQUIRE_MESSAGE(!list.empty(), "no ." << cls << " in the tooltip");
        return list.front();
    }
};

double token(const char* name) {
    static const std::shared_ptr<const pat::ui::Theme> theme = pat::test::load_game_theme();
    const pat::ui::ThemeNumber* n = theme->find_number(name);
    REQUIRE_MESSAGE(n != nullptr, "no token " << name);
    return n->milli / 1000.0;
}

}  // namespace

TEST_CASE("tooltips: the theme tokens match the boards of the mockup") {
    const auto income = pat::test::find_layer("tooltips_income.pd", "Подсказка / расходы на посредника");
    REQUIRE(income.found);
    CHECK(token("tip_w") == income.w);
    const auto skill = pat::test::find_layer("tooltips_character.pd", "Персонаж / подсказка / Хозяйство");
    REQUIRE(skill.found);
    CHECK(token("tip_w_wide") == skill.w);
    const auto title = pat::test::find_layer("tooltips_income.pd", "Расходы на переговоры", true);
    REQUIRE(title.found);
    CHECK(token("tip_pad") == title.x);
    const auto close = pat::test::find_layer("tooltips_income.pd", "Close tooltip");
    REQUIRE(close.found);
    CHECK(token("tip_close") == close.w);
    CHECK(token("tip_close") == close.h);
}

TEST_CASE("tooltips: the four board variants as samples of the gallery page") {
    if (!fonts_ok()) {
        return;
    }
    Fixture f(true);
    struct Variant {
        const char* id;
        const char* board;
    };
    const Variant variants[] = {{"tip-cost", "Подсказка / расходы на посредника"},
                                {"tip-before", "Подсказка / казна до отправки"},
                                {"tip-after", "Подсказка / казна после отправки"},
                                {"tip-reason", "Подсказка / партия недоступна"}};
    for (const Variant& v : variants) {
        CAPTURE(v.id);
        const auto board = pat::test::find_layer("tooltips_income.pd", v.board);
        REQUIRE(board.found);
        Rml::Element* tip = f.el(v.id);
        // The size is the board's: exact width, height within a third (the rows are a little tighter).
        CHECK(tip->GetOffsetWidth() == board.w);
        CHECK(std::abs(tip->GetOffsetHeight() - board.h) < board.h * 0.3);
        const auto& values = tip->GetComputedValues();
        CHECK(is_colour(values.background_color(), "panel"));
        CHECK(values.border_top_width() == 1.0f);
        CHECK(is_colour(values.border_top_color(), "gold"));

        Rml::ElementList titles;
        tip->GetElementsByClassName(titles, "tip-title");
        REQUIRE(titles.size() == 1);
        CHECK(titles.front()->GetAbsoluteLeft() - tip->GetAbsoluteLeft() == doctest::Approx(21.0));
        CHECK(titles.front()->GetAbsoluteTop() - tip->GetAbsoluteTop() < 30.0f);
        CHECK(is_colour(titles.front()->GetComputedValues().color(), "light"));

        // The close button sits at (332, 8) of a 366 px board, 26 x 26.
        Rml::ElementList close;
        tip->GetElementsByClassName(close, "tip-close");
        REQUIRE(close.size() == 1);
        CHECK(close.front()->GetOffsetWidth() == 26.0f);
        CHECK(close.front()->GetOffsetHeight() == 26.0f);
        CHECK(std::abs(close.front()->GetAbsoluteLeft() - tip->GetAbsoluteLeft() - 332.0f) <= 2.0f);
        CHECK(std::abs(close.front()->GetAbsoluteTop() - tip->GetAbsoluteTop() - 8.0f) <= 2.0f);
    }

    // The tones: a gold total, a green income, a red expense, a gold footer; a red title for a reason.
    Rml::Element* cost = f.el("tip-cost");
    Rml::ElementList list;
    cost->GetElementsByClassName(list, "tip-sum-value");
    REQUIRE(list.size() == 1);
    CHECK(is_colour(list.front()->GetComputedValues().color(), "gold"));
    list.clear();
    cost->GetElementsByClassName(list, "tip-footer");
    REQUIRE(list.size() == 1);
    CHECK(is_colour(list.front()->GetComputedValues().color(), "gold"));
    list.clear();
    f.el("tip-before")->GetElementsByClassName(list, "tip-value");
    REQUIRE(list.size() == 2);
    CHECK(is_colour(list[0]->GetComputedValues().color(), "success"));
    CHECK(is_colour(list[1]->GetComputedValues().color(), "softred"));
    list.clear();
    f.el("tip-before")->GetElementsByClassName(list, "tip-headline");
    REQUIRE(list.size() == 1);
    CHECK(is_colour(list.front()->GetComputedValues().color(), "gold"));

    // The wide variant of the character boards.
    const auto skill = pat::test::find_layer("tooltips_character.pd", "Персонаж / подсказка / Хозяйство");
    CHECK(f.el("tip-skill")->GetOffsetWidth() == skill.w);
    // The shortage reason: the red title and the cost split.
    CHECK(f.el("tip-shortage")->GetOffsetWidth() == 366.0f);
    list.clear();
    f.el("tip-shortage")->GetElementsByClassName(list, "tip-title");
    REQUIRE(list.size() == 1);
    CHECK(is_colour(list.front()->GetComputedValues().color(), "softred"));
}

TEST_CASE("tooltips: hovering an element opens a tooltip built from data, inside the screen") {
    if (!fonts_ok()) {
        return;
    }
    Fixture f(false);
    f.hover(f.el("btn-primary"));
    f.run(0.5f);
    REQUIRE(f.tooltips->open_levels() == 1);
    Rml::Element* rows_owner = f.in_level(0, "tip-title");
    CHECK(rows_owner->GetInnerRML().find("Расходы") != std::string::npos);
    Rml::ElementList rows;
    f.tooltips->level_document(0)->GetElementsByClassName(rows, "tip-row");
    CHECK(rows.size() == 2);
    const pat::ui::RectF rect = f.tooltips->level_rect(0);
    CHECK(rect.x >= 0.0f);
    CHECK(rect.y >= 0.0f);
    CHECK(rect.right() <= 1920.0f);
    CHECK(rect.bottom() <= 1080.0f);
    CHECK(rect.w == 366.0f);
    // The close button is hidden until the tooltip is pinned.
    CHECK(f.in_level(0, "tip-close")->GetComputedValues().display() == Rml::Style::Display::None);

    // The disabled action shows why it is unavailable: the red title "Не хватает 40 ₽" and the split.
    f.hover(f.el("act-disabled"));
    f.run(1.0f);
    REQUIRE(f.tooltips->open_levels() == 1);
    Rml::Element* title = f.in_level(0, "tip-title");
    CHECK(title->GetInnerRML().find("Не хватает") != std::string::npos);
    CHECK(is_colour(title->GetComputedValues().color(), "softred"));
    CHECK(f.in_level(0, "tip-sum-label")->GetInnerRML().find("Списать") != std::string::npos);
}

TEST_CASE("tooltips: keyboard focus opens the tooltip of the focused element, below it; Esc closes") {
    if (!fonts_ok()) {
        return;
    }
    Fixture f(false);
    f.move_to(1800.0f, 1000.0f);  // the mouse is somewhere else
    f.run(0.5f);
    CHECK(f.tooltips->open_levels() == 0);

    f.tab();  // the first control is the primary button, which has a tooltip
    CHECK(f.tooltips->keyboard_mode());
    CHECK(f.focused() == "btn-primary");
    f.run(0.5f);
    REQUIRE(f.tooltips->open_levels() == 1);
    Rml::Element* button = f.el("btn-primary");
    const pat::ui::RectF rect = f.tooltips->level_rect(0);
    CHECK(rect.y >= button->GetAbsoluteTop() + button->GetOffsetHeight());  // below the button
    CHECK(rect.x >= button->GetAbsoluteLeft() - 1.0f);
    CHECK(rect.x <= button->GetAbsoluteLeft() + button->GetOffsetWidth());

    // Moving the mouse hands the tooltip back to hovering: away from every element it closes.
    f.move_to(1800.0f, 1000.0f);
    CHECK_FALSE(f.tooltips->keyboard_mode());
    f.run(0.8f);
    CHECK(f.tooltips->open_levels() == 0);

    f.tab();  // focus moves on to the secondary button, which has no tooltip
    f.run(0.8f);
    CHECK(f.tooltips->open_levels() == 0);
    for (int i = 0; i < 14; ++i) {
        f.tab();  // around the ring of 15 controls back to the primary button
    }
    CHECK(f.focused() == "btn-primary");
    f.run(0.5f);
    REQUIRE(f.tooltips->open_levels() == 1);
    f.send(event::Key{kScancodeEscape, true, false, 0});
    CHECK(f.tooltips->open_levels() == 0);
}

TEST_CASE("tooltips: a click on the tooltip pins it and shows the close button; the button closes it") {
    if (!fonts_ok()) {
        return;
    }
    Fixture f(false);
    f.hover(f.el("btn-primary"));
    f.run(0.5f);
    REQUIRE(f.tooltips->open_levels() == 1);
    CHECK_FALSE(f.tooltips->pinned());

    Rml::Element* tip = f.in_level(0, "tip");
    f.click_at(tip->GetAbsoluteLeft() + 100.0f, tip->GetAbsoluteTop() + tip->GetOffsetHeight() - 20.0f);
    CHECK(f.tooltips->pinned());
    Rml::Element* close = f.in_level(0, "tip-close");
    CHECK(close->GetComputedValues().display() == Rml::Style::Display::Block);
    CHECK(close->GetOffsetWidth() == 26.0f);

    // Pinned tooltips stay while the cursor is far away.
    f.move_to(1800.0f, 1000.0f);
    f.run(1.5f);
    CHECK(f.tooltips->open_levels() == 1);

    f.click_at(close->GetAbsoluteLeft() + 13.0f, close->GetAbsoluteTop() + 13.0f);
    CHECK(f.tooltips->open_levels() == 0);
    CHECK_FALSE(f.tooltips->pinned());
}

#else

TEST_CASE("tooltips: windows only") {
    CHECK(true);
}

#endif
