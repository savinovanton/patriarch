// The shared UI components (UI-D02, #257): theme tokens, geometry against data/ui/layout (boards 00, 01, 04,
// 09), the states hover / focus / disabled / selected, keyboard order and activation, localisation of the
// gallery page.

#include "pat/data/pd_parser.hpp"
#include "pat/engine/renderer.hpp"
#include "pat/engine/ui.hpp"
#include "pat/ui/component_gallery.hpp"
#include "pat/ui/theme.hpp"

#include <doctest/doctest.h>

#include "ui_loc_fixture.hpp"

#ifdef _WIN32

#include <RmlUi/Core.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "test_fonts.hpp"
#include "ui_layout_fixture.hpp"
#include "ui_theme_fixture.hpp"

namespace {

using namespace pat::engine;
using pat::ui::ComponentGallery;

std::string read_text(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    REQUIRE_MESSAGE(in.good(), "cannot open " << path.string());
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

struct Fixture {
    std::unique_ptr<IRenderer> renderer;
    std::unique_ptr<UiContext> ui;
    pat::loc::Localizer loc;
    std::unique_ptr<ComponentGallery> gallery;
    Rml::ElementDocument* doc = nullptr;

    explicit Fixture(bool empty_table = false)
        : loc(empty_table ? pat::test::load_empty_localizer() : pat::test::load_ui_localizer()) {
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
        auto page = ComponentGallery::create(*ui,
                                             std::filesystem::path(PATRIARCH_SPIKE_UI_DIR) / "gallery.rml",
                                             std::filesystem::path(PATRIARCH_ASSETS_DIR) / "ui" /
                                                 "patriarch" / "icons",
                                             loc);
        REQUIRE(page.has_value());
        gallery = std::move(page).value();
        ui->update();
        doc = gallery->document();
        REQUIRE(doc != nullptr);
    }
    ~Fixture() {
        gallery.reset();
        ui.reset();
    }

    Rml::Element* el(const char* id) const {
        Rml::Element* e = doc->GetElementById(id);
        REQUIRE_MESSAGE(e != nullptr, "no element #" << id);
        return e;
    }
    void click(const char* id) {
        Rml::Element* e = el(id);
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
    [[nodiscard]] std::string focused() const {
        Rml::Element* f = ui->context()->GetFocusElement();
        return f != nullptr ? f->GetId() : std::string();
    }
};

bool is_colour(const Rml::Colourb& c, const char* token) {
    static const std::shared_ptr<const pat::ui::Theme> theme = pat::test::load_game_theme();
    const auto* theme_color = theme->find_color(token);
    REQUIRE(theme_color != nullptr);
    return c.red == theme_color->r && c.green == theme_color->g && c.blue == theme_color->b;
}

bool fonts_ok() {
    if (!pat::test::game_fonts_present()) {
        MESSAGE("Game fonts not found, skipping");
        return false;
    }
    return true;
}

TEST_CASE("components: the theme carries the component tokens and they match the layout of the boards") {
    const auto theme = pat::test::load_game_theme();
    const auto token = [&](const char* name) {
        const pat::ui::ThemeNumber* n = theme->find_number(name);
        REQUIRE_MESSAGE(n != nullptr, "no token " << name);
        return n->milli / 1000.0;
    };
    const auto layer_size = [](const char* file, const std::string& name) {
        return pat::test::find_layer(file, name);
    };
    const auto primary = layer_size("board_00_foundation.pd", "Button / Patriarch / Button / Primary");
    REQUIRE(primary.found);
    CHECK(token("btn_h") == primary.h);
    const auto action =
        layer_size("board_09_action_states.pd", "Button / Patriarch / Action / Send intermediary / Default");
    REQUIRE(action.found);
    CHECK(token("btn_h_action") == action.h);
    const auto badge = layer_size("board_01_map_province.pd", "Badge / НА ПАУЗЕ");
    REQUIRE(badge.found);
    CHECK(token("badge_h") == badge.h);
    CHECK(token("chip_h") == badge.h);
    const auto tab = layer_size("board_01_map_province.pd", "Tab / Обзор");
    REQUIRE(tab.found);
    CHECK(token("tab_h") == tab.h);
    const auto field = layer_size("board_04_marriage_search.pd", "Search field / имя или род");
    REQUIRE(field.found);
    CHECK(token("field_h") == field.h);
    const auto inspector_rule = layer_size("board_01_map_province.pd", "Inspector top rule");
    REQUIRE(inspector_rule.found);
    CHECK(token("rule_window") == inspector_rule.h);
    CHECK(token("focus_ring") == 2.0);
    CHECK(token("icon_size") == 24.0);
}

TEST_CASE("components: sizes on the page follow the mockup") {
    if (!fonts_ok()) {
        return;
    }
    Fixture f;
    const auto height = [&](const char* id) { return static_cast<double>(f.el(id)->GetOffsetHeight()); };
    CHECK(height("btn-primary") == 48.0);
    CHECK(height("btn-secondary") == 48.0);
    CHECK(height("btn-disabled") == 48.0);
    CHECK(height("act-default") == 56.0);
    CHECK(height("act-hover") == 56.0);  // the geometry does not change with the state
    CHECK(height("act-focus") == 56.0);
    CHECK(height("act-disabled") == 56.0);
    CHECK(height("badge-dark") == 28.0);
    CHECK(height("tabs") == 46.0);  // "Province tabs" of board 01
    CHECK(height("tab-overview") == 44.0);
    CHECK(height("search") == 44.0);
    CHECK(height("chip-availability") == 28.0);
    CHECK(height("track-fill") == 5.0);
    CHECK(static_cast<double>(f.el("field-wrap")->GetOffsetWidth()) == 210.0);
    // The mockup's secondary button is as wide as the column it stands in (530 of 590 px wide there).
    CHECK(f.el("btn-primary")->GetOffsetWidth() == f.el("btn-secondary")->GetOffsetWidth());
    CHECK(f.el("act-default")->GetOffsetWidth() == f.el("act-focus")->GetOffsetWidth());
}

TEST_CASE("components: fills, hover, disabled and focus use theme colours") {
    if (!fonts_ok()) {
        return;
    }
    Fixture f;
    const auto bg = [&](const char* id) { return f.el(id)->GetComputedValues().background_color(); };
    const auto text = [&](const char* id) { return f.el(id)->GetComputedValues().color(); };
    CHECK(is_colour(bg("btn-primary"), "green"));
    CHECK(is_colour(text("btn-primary"), "light"));
    CHECK(is_colour(bg("btn-secondary"), "paper"));
    CHECK(is_colour(text("btn-secondary"), "text"));
    CHECK(is_colour(bg("btn-disabled"), "paper2"));
    CHECK(is_colour(text("btn-disabled"), "muted"));
    CHECK(is_colour(bg("act-hover"), "green_hover"));
    CHECK(is_colour(bg("act-disabled"), "paper2"));
    CHECK(is_colour(bg("badge-green"), "green"));
    CHECK(is_colour(bg("badge-dark"), "raised"));
    CHECK(is_colour(text("badge-dark"), "gold"));
    CHECK(is_colour(text("badge-red"), "red"));
    CHECK(is_colour(text("reason"), "red"));
    CHECK(is_colour(bg("panel-dark"), "panel"));
    CHECK(is_colour(bg("sheet-window"), "paper"));
    CHECK(is_colour(bg("track-fill"), "green"));

    // Gold rules on top: 4 px for a window, 2 px for a panel.
    const auto& window_rule = f.el("sheet-window")->GetComputedValues();
    CHECK(window_rule.border_top_width() == 4.0f);
    CHECK(is_colour(window_rule.border_top_color(), "gold"));
    CHECK(f.el("sheet-panel")->GetComputedValues().border_top_width() == 2.0f);
    CHECK(f.el("panel-dark")->GetComputedValues().border_top_width() == 2.0f);

    // The pointer over a primary button changes only the fill.
    const float width_before = f.el("btn-primary")->GetOffsetWidth();
    Rml::Element* primary = f.el("btn-primary");
    f.ui->handle_event(
        event::MouseMove{primary->GetAbsoluteLeft() + 20.0f, primary->GetAbsoluteTop() + 20.0f, 0, 0});
    f.ui->update();
    CHECK(is_colour(bg("btn-primary"), "green_hover"));
    CHECK(f.el("btn-primary")->GetOffsetWidth() == width_before);
    CHECK(f.el("btn-primary")->GetOffsetHeight() == 48.0f);

    // The forced focus state: a gold ring of 2 px in the same geometry.
    const auto& focus = f.el("act-focus")->GetComputedValues();
    CHECK(focus.border_top_width() == 2.0f);
    CHECK(is_colour(focus.border_top_color(), "gold"));
    CHECK(is_colour(bg("act-focus"), "green"));
}

TEST_CASE("components: Tab walks the controls in order, the ring follows the keyboard and not the mouse") {
    if (!fonts_ok()) {
        return;
    }
    Fixture f;
    const std::vector<std::string> expected = {"btn-primary",
                                               "btn-secondary",
                                               "act-default",
                                               "act-hover",
                                               "act-focus",
                                               "tab-overview",
                                               "tab-income",
                                               "tab-buildings",
                                               "card-1",
                                               "card-2",
                                               "search",
                                               "chip-availability",
                                               "chip-faith"};
    std::vector<std::string> seen;
    for (std::size_t i = 0; i < expected.size(); ++i) {
        f.key(Rml::Input::KI_TAB);
        seen.push_back(f.focused());
    }
    CHECK(seen == expected);  // disabled buttons are skipped
    f.key(Rml::Input::KI_TAB);
    CHECK(f.focused() == "btn-primary");  // wraps around
    f.key(Rml::Input::KI_TAB);
    f.key(Rml::Input::KI_TAB);
    CHECK(f.focused() == "act-default");

    // After the keyboard the focused button wears the gold ring; its size does not change.
    const auto& ring = f.el("act-default")->GetComputedValues();
    CHECK(ring.border_top_width() == 2.0f);
    CHECK(is_colour(ring.border_top_color(), "gold"));
    CHECK(f.el("act-default")->GetOffsetHeight() == 56.0f);

    // A mouse press removes the ring (RmlUi has no :focus-visible).
    f.click("chip-faith");
    CHECK(f.focused() == "chip-faith");
    CHECK(f.el("chip-faith")->GetComputedValues().border_top_width() == 1.0f);
    // The keyboard brings it back on the next Tab.
    f.key(Rml::Input::KI_TAB);
    f.key(Rml::Input::KI_TAB);
    CHECK(f.focused() == "btn-secondary");
    CHECK(f.el("btn-secondary")->GetComputedValues().border_top_width() == 2.0f);
}

TEST_CASE("components: Enter presses a focused button, clicks switch tabs, cards and chips") {
    if (!fonts_ok()) {
        return;
    }
    Fixture f;
    CHECK(f.gallery->presses() == 0);
    f.key(Rml::Input::KI_TAB);
    REQUIRE(f.focused() == "btn-primary");
    f.key(Rml::Input::KI_RETURN);
    CHECK(f.gallery->presses() == 1);
    f.click("btn-secondary");
    CHECK(f.gallery->presses() == 2);
    f.click("btn-disabled");  // a disabled button reacts to nothing
    CHECK(f.gallery->presses() == 2);

    CHECK(f.gallery->tab() == "overview");
    CHECK(f.el("tab-overview")->IsClassSet("active"));
    f.click("tab-income");
    CHECK(f.gallery->tab() == "income");
    CHECK(f.el("tab-income")->IsClassSet("active"));
    CHECK_FALSE(f.el("tab-overview")->IsClassSet("active"));
    const auto& active_tab = f.el("tab-income")->GetComputedValues();
    CHECK(is_colour(active_tab.color(), "green"));
    CHECK(active_tab.border_bottom_width() == 2.0f);
    CHECK(is_colour(f.el("tab-overview")->GetComputedValues().color(), "muted"));

    // The selected card is paper with a 2 px green border, the other one paper2 with a hairline.
    CHECK(f.gallery->selected_card() == 1);
    CHECK(is_colour(f.el("card-1")->GetComputedValues().background_color(), "paper"));
    CHECK(f.el("card-1")->GetComputedValues().border_top_width() == 2.0f);
    CHECK(is_colour(f.el("card-2")->GetComputedValues().background_color(), "paper2"));
    CHECK(f.el("card-2")->GetComputedValues().border_top_width() == 1.0f);
    f.click("card-2");
    CHECK(f.gallery->selected_card() == 2);
    CHECK(is_colour(f.el("card-2")->GetComputedValues().background_color(), "paper"));
    CHECK(f.el("card-2")->GetComputedValues().border_top_width() == 2.0f);
    CHECK(f.el("card-2")->GetOffsetWidth() == f.el("card-1")->GetOffsetWidth());

    f.click("chip-availability");
    CHECK(f.el("chip-availability")->IsClassSet("active"));
    f.click("chip-availability");
    CHECK_FALSE(f.el("chip-availability")->IsClassSet("active"));
}

TEST_CASE("components: every text of the page comes from the string table") {
    if (!fonts_ok()) {
        return;
    }
    Fixture f;
    // The real table has every key: no [[key]] on the page, and the hint of the empty search field shows.
    const std::string rml = f.doc->GetInnerRML();
    CHECK(rml.find("[[") == std::string::npos);
    CHECK(f.el("search-hint")->GetInnerRML() == "Все роды");
    CHECK(f.el("btn-primary")->GetInnerRML().find("Начать переговоры") != std::string::npos);

    // The document source is plain RML without literals: a hard-coded sentence would not change with the
    // table.
    const std::string source = read_text(std::filesystem::path(PATRIARCH_SPIKE_UI_DIR) / "gallery.rml");
    CHECK(source.find("Кнопки") == std::string::npos);
    CHECK(source.find("{{ t.") != std::string::npos);
}

TEST_CASE("components: a table that knows no key shows the keys and the page still lays out") {
    if (!fonts_ok()) {
        return;
    }
    Fixture f(true);
    CHECK(f.el("btn-secondary")->GetInnerRML().find("[[ui.gallery.btn_secondary]]") != std::string::npos);
    CHECK(f.el("btn-secondary")->GetOffsetHeight() == 48.0f);
}

}  // namespace

#endif  // _WIN32
