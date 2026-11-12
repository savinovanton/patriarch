// The overlays of the map screen (UI-D05, #260; boards 01 and 07 of the mockup): the panel "Дела рода", the
// season badge, the caption over the realm, the plate of the selected province and the minimap, on an
// offscreen WARP target. Geometry is compared with data/ui/layout, texts come from the string table, and the
// empty parts of the documents must not take the mouse.

#include "pat/ui/agenda_panel.hpp"
#include "pat/ui/map_overlays.hpp"

#include <doctest/doctest.h>

#include "ui_loc_fixture.hpp"

#ifdef _WIN32

#include "pat/engine/dpi.hpp"
#include "pat/engine/renderer.hpp"
#include "pat/engine/ui.hpp"

#include <RmlUi/Core.h>

#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

#include "test_fonts.hpp"
#include "ui_layout_fixture.hpp"
#include "ui_theme_fixture.hpp"

namespace {

using namespace pat::engine;
using pat::test::find_layer;
using pat::test::Layer;
using pat::ui::AgendaPanel;
using pat::ui::MapOverlays;

constexpr const char* kBoard = "board_01_map_province.pd";

struct Fixture {
    std::unique_ptr<IRenderer> renderer;
    std::unique_ptr<UiContext> ui;
    std::unique_ptr<AgendaPanel> agenda;
    std::unique_ptr<MapOverlays> overlays;

    explicit Fixture(PixelSize size = {1920, 1080},
                     const pat::loc::Localizer& loc = pat::test::ui_localizer()) {
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
        const std::filesystem::path icons =
            std::filesystem::path(PATRIARCH_ASSETS_DIR) / "ui" / "patriarch" / "icons";
        auto a = AgendaPanel::create(
            *ui, std::filesystem::path(PATRIARCH_SPIKE_UI_DIR) / "agenda.rml", icons, loc);
        REQUIRE(a.has_value());
        agenda = std::move(a).value();
        auto o = MapOverlays::create(
            *ui, std::filesystem::path(PATRIARCH_SPIKE_UI_DIR) / "overlays.rml", icons, loc);
        REQUIRE(o.has_value());
        overlays = std::move(o).value();
        ui->update();
    }
    ~Fixture() {
        overlays.reset();
        agenda.reset();
        ui.reset();
        renderer.reset();
    }

    Rml::Element* agenda_el(const char* id) const {
        Rml::Element* e = agenda->document()->GetElementById(id);
        REQUIRE_MESSAGE(e != nullptr, "no element #" << id);
        return e;
    }
    Rml::Element* overlay_el(const char* id) const {
        Rml::Element* e = overlays->document()->GetElementById(id);
        REQUIRE_MESSAGE(e != nullptr, "no element #" << id);
        return e;
    }
    void move_to(float x, float y) {
        ui->handle_event(event::MouseMove{x, y, 0, 0});
        ui->update();
    }
    void click(Rml::Element* e, float fx = 0.5f, float fy = 0.5f) {
        const float x = e->GetAbsoluteLeft() + e->GetOffsetWidth() * fx;
        const float y = e->GetAbsoluteTop() + e->GetOffsetHeight() * fy;
        move_to(x, y);
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

void check_rect(Rml::Element* e, const Layer& board, double tolerance = 2.0) {
    REQUIRE(board.found);
    CHECK(std::abs(e->GetAbsoluteLeft() - board.x) <= tolerance);
    CHECK(std::abs(e->GetAbsoluteTop() - board.y) <= tolerance);
    CHECK(std::abs(e->GetOffsetWidth() - board.w) <= tolerance);
    CHECK(std::abs(e->GetOffsetHeight() - board.h) <= tolerance);
}

bool fonts_ok() {
    if (!pat::test::game_fonts_present()) {
        MESSAGE("Game fonts not found, skipping");
        return false;
    }
    return true;
}

}  // namespace

TEST_CASE("agenda: the panel and its cards sit where board 01 puts them") {
    if (!fonts_ok()) {
        return;
    }
    Fixture f;
    f.agenda->set_snapshot(pat::ui::make_placeholder_agenda());
    f.ui->update();
    check_rect(f.agenda_el("agenda"), find_layer(kBoard, "Priority agenda"));
    check_rect(f.agenda_el("agenda-badge"), find_layer(kBoard, "Badge / 2"));
    check_rect(f.agenda_el("agenda-card-0"), find_layer(kBoard, "Hit area / dynasty"));
    check_rect(f.agenda_el("agenda-card-1"), find_layer(kBoard, "Hit area / event"));
    // The third card follows at the same pitch (111 dp).
    Rml::Element* third = f.agenda_el("agenda-card-2");
    CHECK(std::abs(third->GetAbsoluteTop() - (find_layer(kBoard, "Hit area / event").y + 111.0)) <= 2.0);
    // The link "Открыть журнал" and the title.
    const Layer link = find_layer(kBoard, "Открыть журнал", true);
    REQUIRE(link.found);
    Rml::Element* journal = f.agenda_el("agenda-journal");
    CHECK(std::abs(journal->GetAbsoluteLeft() - link.x) <= 2.0);
    CHECK(std::abs(journal->GetAbsoluteTop() - link.y) <= 3.0);
    const Layer title = find_layer(kBoard, "Дела рода", true);
    REQUIRE(title.found);
    CHECK(std::abs(f.agenda_el("agenda-title")->GetAbsoluteLeft() - title.x) <= 2.0);
    // The gold rule on top of the panel: 2 dp.
    CHECK(f.agenda_el("agenda")->GetComputedValues().border_top_width() == 2.0f);
}

TEST_CASE("agenda: the placeholder affairs, their texts and the counter come from the string table") {
    if (!fonts_ok()) {
        return;
    }
    const pat::loc::Localizer& loc = pat::test::ui_localizer();
    Fixture f;
    const pat::ui::AgendaSnapshot snapshot = pat::ui::make_placeholder_agenda();
    CHECK(snapshot.placeholder);
    f.agenda->set_snapshot(snapshot);
    f.ui->update();
    CHECK(f.agenda->attention_count() == 2);  // two affairs need attention, the third is informational
    CHECK(f.agenda_el("agenda-badge")->GetInnerRML() == "2");
    CHECK(f.agenda_el("agenda-title")->GetInnerRML() == loc.format("ui.agenda.title"));
    CHECK(f.agenda_el("agenda-subtitle")->GetInnerRML() == loc.format("ui.agenda.subtitle"));
    CHECK(f.agenda_el("agenda-journal")->GetInnerRML() == loc.format("ui.agenda.journal"));
    Rml::ElementList numbers;
    f.agenda->document()->GetElementsByClassName(numbers, "agenda-number");
    REQUIRE(numbers.size() == 3);
    CHECK(numbers[0]->GetInnerRML() == "01");
    CHECK(numbers[2]->GetInnerRML() == "03");
    Rml::ElementList titles;
    f.agenda->document()->GetElementsByClassName(titles, "agenda-card-title");
    REQUIRE(titles.size() == 3);
    CHECK(titles[0]->GetInnerRML() == loc.format("ui.agenda.placeholder.heir.title"));
    Rml::ElementList details;
    f.agenda->document()->GetElementsByClassName(details, "agenda-card-detail");
    REQUIRE(details.size() == 3);
    CHECK(details[2]->GetInnerRML() ==
          loc.format("ui.agenda.placeholder.taxes.detail", {pat::loc::Arg("n", 12)}));
    CHECK(details[2]->GetInnerRML().find("12 дней") != std::string::npos);
    Rml::ElementList chips;
    f.agenda->document()->GetElementsByClassName(chips, "agenda-chip");
    REQUIRE(chips.size() == 3);
    CHECK(chips[0]->GetInnerRML() == loc.format("ui.nav.dynasty"));
    CHECK(chips[1]->GetInnerRML() == loc.format("ui.nav.court"));
    CHECK(chips[2]->GetInnerRML() == loc.format("ui.nav.treasury"));

    // No affairs: the counter is hidden and the panel says so; more than three affairs show three cards.
    f.agenda->set_snapshot({});
    f.ui->update();
    CHECK(f.agenda_el("agenda-badge")->GetComputedValues().display() == Rml::Style::Display::None);
    CHECK(f.agenda_el("agenda-empty")->GetComputedValues().display() != Rml::Style::Display::None);
    pat::ui::AgendaSnapshot many = snapshot;
    many.items.push_back(many.items.front());
    f.agenda->set_snapshot(many);
    f.ui->update();
    Rml::ElementList cards;
    f.agenda->document()->GetElementsByClassName(cards, "agenda-card");
    CHECK(cards.size() == 3);
    CHECK(f.agenda->attention_count() == 3);
}

TEST_CASE("agenda: a click or Enter on a card opens its section, Tab walks cards then the journal link") {
    if (!fonts_ok()) {
        return;
    }
    Fixture f;
    f.agenda->set_snapshot(pat::ui::make_placeholder_agenda());
    std::vector<std::string> opened;
    int journal = 0;
    f.agenda->set_open_handler([&](const std::string& section) { opened.push_back(section); });
    f.agenda->set_journal_handler([&] { ++journal; });
    f.ui->update();

    f.click(f.agenda_el("agenda-card-1"));
    REQUIRE(opened.size() == 1);
    CHECK(opened[0] == "court");
    f.click(f.agenda_el("agenda-journal"));
    CHECK(journal == 1);

    // The cards are tab stops in the order of the panel; the link comes last.
    std::vector<std::string> order;
    for (int i = 0; i < 4; ++i) {
        f.key(Rml::Input::KI_TAB);
        order.push_back(f.focused());
    }
    // (the minimap buttons belong to the other document; the first stop of this one is the first card)
    CHECK(order[0] == "agenda-card-0");
    CHECK(order[1] == "agenda-card-1");
    CHECK(order[2] == "agenda-card-2");
    CHECK(order[3] == "agenda-journal");
    f.key(Rml::Input::KI_TAB);
    f.key(Rml::Input::KI_TAB);
    f.key(Rml::Input::KI_TAB);
    f.key(Rml::Input::KI_TAB);
    // Back on the first card: Enter presses it.
    f.key(Rml::Input::KI_TAB);
    f.key(Rml::Input::KI_RETURN);
    CHECK(opened.back() == "dynasty");

    f.agenda->press_item(2);
    CHECK(opened.back() == "treasury");
    f.agenda->press_item(9);  // out of range: nothing
    CHECK(opened.size() == 3);
}

TEST_CASE("overlays: the season badge, the caption over the realm and the plate of the selected province") {
    if (!fonts_ok()) {
        return;
    }
    const pat::loc::Localizer& loc = pat::test::ui_localizer();
    Fixture f;
    // Season: "ЗИМА · 1703", all four seasons from the table.
    f.overlays->set_season(pat::ui::Season::Winter, 1703);
    f.ui->update();
    Rml::Element* season = f.overlay_el("season");
    CHECK(season->GetInnerRML() == "ЗИМА · 1703");
    const Layer badge = find_layer(kBoard, "Badge / ЗИМА · 1703");
    REQUIRE(badge.found);
    CHECK(std::abs(season->GetAbsoluteLeft() - badge.x) <= 2.0);
    CHECK(std::abs(season->GetAbsoluteTop() - badge.y) <= 2.0);
    CHECK(season->GetOffsetHeight() == 28.0f);
    CHECK(std::abs(season->GetOffsetWidth() - badge.w) <= 16.0);
    f.overlays->set_season(pat::ui::Season::Autumn, 1704);
    f.ui->update();
    CHECK(season->GetInnerRML() == "ОСЕНЬ · 1704");
    CHECK(pat::ui::MapOverlays::season_key(pat::ui::Season::Spring) == "spring");
    CHECK(loc.format("ui.overlay.season.summer") == "ЛЕТО");

    // The caption over the realm is hidden until it has a name, then centred on its anchor.
    CHECK(f.overlay_el("region")->GetComputedValues().display() == Rml::Style::Display::None);
    f.overlays->set_region(loc.format("ui.overlay.placeholder.region"));
    f.overlays->set_region_anchor(1000.0f, 400.0f);
    f.ui->update();
    Rml::Element* region = f.overlay_el("region");
    CHECK(region->GetComputedValues().display() != Rml::Style::Display::None);
    CHECK(f.overlay_el("region-title")->GetInnerRML() == "НОВГОРОДСКАЯ ЗЕМЛЯ");
    CHECK(f.overlay_el("region-sub")->GetInnerRML() == loc.format("ui.overlay.owned"));
    CHECK(std::abs(region->GetAbsoluteLeft() + region->GetOffsetWidth() * 0.5f - 1000.0f) <= 1.0f);
    CHECK(std::abs(region->GetAbsoluteTop() + region->GetOffsetHeight() * 0.5f - 400.0f) <= 40.0f);

    // The plate: 207 x 58, the gold line 2 dp at its bottom, hanging below the marker.
    CHECK(f.overlay_el("plate")->GetComputedValues().display() == Rml::Style::Display::None);
    f.overlays->set_selection("Новгород");
    f.overlays->set_selection_anchor(1200.0f, 500.0f);
    f.ui->update();
    Rml::Element* plate = f.overlay_el("plate");
    const Layer label = find_layer(kBoard, "Map label / Новгород");
    REQUIRE(label.found);
    CHECK(plate->GetOffsetWidth() == label.w);
    CHECK(plate->GetOffsetHeight() == label.h);
    CHECK(f.overlay_el("plate-name")->GetInnerRML() == "Новгород");
    CHECK(std::abs(plate->GetAbsoluteLeft() + plate->GetOffsetWidth() * 0.5f - 1200.0f) <= 1.0f);
    CHECK(plate->GetAbsoluteTop() > 500.0f);
    CHECK(f.overlay_el("plate-line")->GetOffsetHeight() == 2.0f);
    f.overlays->set_selection({});
    f.ui->update();
    CHECK(f.overlay_el("plate")->GetComputedValues().display() == Rml::Style::Display::None);
}

TEST_CASE("overlays: the minimap sits where board 01 puts it, shows the view and reports clicks") {
    if (!fonts_ok()) {
        return;
    }
    Fixture f;
    check_rect(f.overlay_el("minimap"), find_layer(kBoard, "Overview minimap"));
    Rml::Element* area = f.overlay_el("mm-area");
    const Layer outline = find_layer(kBoard, "Schematic realm outline");
    REQUIRE(outline.found);
    CHECK(std::abs(area->GetAbsoluteLeft() - outline.x) <= 2.0);
    CHECK(std::abs(area->GetAbsoluteTop() - outline.y) <= 2.0);
    CHECK(area->GetOffsetWidth() == 222.0f);
    CHECK(area->GetOffsetHeight() == 104.0f);
    // The buttons - and + and the scale label sit at the positions of the board.
    const Layer minus = find_layer(kBoard, "\xE2\x88\x92", true);
    const Layer plus = find_layer(kBoard, "+", true);
    REQUIRE(minus.found);
    REQUIRE(plus.found);
    CHECK(std::abs(f.overlay_el("mm-out")->GetAbsoluteLeft() - minus.x) <= 2.0);
    CHECK(std::abs(f.overlay_el("mm-in")->GetAbsoluteLeft() - plus.x) <= 2.0);
    CHECK(f.overlay_el("mm-out")->GetOffsetWidth() >= 26.0f);  // the target is at least 26 dp
    CHECK(f.overlay_el("mm-out")->GetOffsetHeight() >= 26.0f);

    // The map is fitted into the picture area: a map twice as wide as the area is tall fills the width.
    pat::ui::MinimapOverview overview;
    overview.aspect = 2.0f;
    f.overlays->set_overview(overview);
    pat::ui::MinimapView view;
    view.center_x = 0.5f;
    view.center_y = 0.5f;
    view.width = 0.5f;
    view.height = 0.5f;
    f.overlays->set_view(view);
    f.ui->update();
    Rml::Element* rect = f.overlay_el("mm-view");
    const float fit_h = 222.0f / 2.0f;  // 111 > 104: the height of the area limits it
    CHECK(fit_h > 104.0f);
    const float map_h = 104.0f;
    const float map_w = 104.0f * 2.0f;
    CHECK(std::abs(rect->GetOffsetWidth() - map_w * 0.5f) <= 2.0f);
    CHECK(std::abs(rect->GetOffsetHeight() - map_h * 0.5f) <= 2.0f);
    // The centre of the rectangle is the middle of the picture area.
    CHECK(std::abs(rect->GetAbsoluteLeft() + rect->GetOffsetWidth() * 0.5f -
                   (area->GetAbsoluteLeft() + area->GetOffsetWidth() * 0.5f)) <= 2.0f);
    CHECK(std::abs(rect->GetAbsoluteTop() + rect->GetOffsetHeight() * 0.5f -
                   (area->GetAbsoluteTop() + area->GetOffsetHeight() * 0.5f)) <= 2.0f);
    // A view that sees the whole map covers the whole picture.
    view.width = 1.0f;
    view.height = 1.0f;
    f.overlays->set_view(view);
    f.ui->update();
    CHECK(std::abs(rect->GetOffsetWidth() - map_w) <= 2.0f);
    CHECK(std::abs(rect->GetOffsetHeight() - map_h) <= 2.0f);

    // The scale label: "100 км", hidden for 0.
    f.overlays->set_scale_km(100);
    f.ui->update();
    CHECK(f.overlay_el("mm-scale")->GetInnerRML() == "100 км");
    f.overlays->set_scale_km(0);
    f.ui->update();
    CHECK(f.overlay_el("mm-scale")->GetComputedValues().display() == Rml::Style::Display::None);

    // The buttons zoom, a click on the picture moves the camera to that point of the map.
    int zoom = 0;
    float jump_x = -1.0f;
    float jump_y = -1.0f;
    f.overlays->set_zoom_handler([&](int d) { zoom += d; });
    f.overlays->set_jump_handler([&](float x, float y) {
        jump_x = x;
        jump_y = y;
    });
    f.click(f.overlay_el("mm-in"));
    CHECK(zoom == 1);
    f.click(f.overlay_el("mm-out"));
    f.click(f.overlay_el("mm-out"));
    CHECK(zoom == -1);
    f.overlays->press_zoom(1);
    CHECK(zoom == 0);
    f.click(area, 0.5f, 0.5f);
    CHECK(jump_x == doctest::Approx(0.5f).epsilon(0.03));
    CHECK(jump_y == doctest::Approx(0.5f).epsilon(0.03));
    f.click(area, 0.25f, 0.5f);  // a quarter of the way across the area: map_w 208 of 222 is fitted at 7 dp
    CHECK(jump_x < 0.5f);
    CHECK(jump_x > 0.15f);
}

TEST_CASE("overlays: only the panels take the mouse, the rest of the map stays operable") {
    if (!fonts_ok()) {
        return;
    }
    Fixture f;
    f.agenda->set_snapshot(pat::ui::make_placeholder_agenda());
    f.overlays->set_season(pat::ui::Season::Winter, 1703);
    f.overlays->set_region("Новгородская земля");
    f.overlays->set_region_anchor(900.0f, 500.0f);
    f.overlays->set_selection("Новгород");
    f.overlays->set_selection_anchor(900.0f, 600.0f);
    f.ui->update();

    f.move_to(900.0f, 450.0f);  // the middle of the map
    CHECK_FALSE(f.ui->mouse_over_ui());
    f.move_to(200.0f, 130.0f);  // on the season badge: decoration, not a target
    CHECK_FALSE(f.ui->mouse_over_ui());
    f.move_to(900.0f, 500.0f);  // on the caption over the realm
    CHECK_FALSE(f.ui->mouse_over_ui());
    f.move_to(900.0f, 625.0f);  // on the plate of the selected province
    CHECK_FALSE(f.ui->mouse_over_ui());
    f.move_to(1700.0f, 300.0f);  // on the panel "Дела рода"
    CHECK(f.ui->mouse_over_ui());
    f.move_to(1750.0f, 900.0f);  // on the minimap
    CHECK(f.ui->mouse_over_ui());
    f.move_to(1700.0f, 700.0f);  // between the two panels
    CHECK_FALSE(f.ui->mouse_over_ui());
    f.move_to(1600.0f, 150.0f);  // right of the panel's left edge but above the first card: still the panel
    CHECK(f.ui->mouse_over_ui());
}

TEST_CASE("overlays: a table that knows no key shows the keys and the documents still lay out") {
    if (!fonts_ok()) {
        return;
    }
    const pat::loc::Localizer empty = pat::test::load_empty_localizer();
    Fixture f({1920, 1080}, empty);
    f.agenda->set_snapshot(pat::ui::make_placeholder_agenda());
    f.overlays->set_season(pat::ui::Season::Winter, 1703);
    f.ui->update();
    CHECK(f.agenda_el("agenda-title")->GetInnerRML().find("[[ui.agenda.title]]") != std::string::npos);
    CHECK(f.overlay_el("minimap")->GetOffsetWidth() == 260.0f);
}

TEST_CASE("overlays: the layout holds at 1280x720 and on a wide screen, anchored to the corners") {
    if (!fonts_ok()) {
        return;
    }
    for (const PixelSize size : {PixelSize{1280, 720}, PixelSize{2560, 1080}}) {
        CAPTURE(size.w);
        Fixture f(size);
        f.agenda->set_snapshot(pat::ui::make_placeholder_agenda());
        f.ui->update();
        const float s = design_ui_scale(size);
        Rml::Element* panel = f.agenda_el("agenda");
        CHECK(std::abs(panel->GetAbsoluteLeft() + panel->GetOffsetWidth() -
                       (static_cast<float>(size.w) - 32.0f * s)) <= 2.0f);
        CHECK(std::abs(panel->GetOffsetWidth() - 352.0f * s) <= 2.0f);
        Rml::Element* mini = f.overlay_el("minimap");
        CHECK(std::abs(mini->GetAbsoluteLeft() + mini->GetOffsetWidth() -
                       (static_cast<float>(size.w) - 32.0f * s)) <= 2.0f);
        CHECK(std::abs(mini->GetAbsoluteTop() + mini->GetOffsetHeight() -
                       (static_cast<float>(size.h) - 68.0f * s)) <= 2.0f);
    }
}

#else

TEST_CASE("map overlays: windows only") {
    CHECK(true);
}

#endif
