// The window of an important event (UI-D07, #262; board "03 / Событие и последствия" of the mockup) on an
// offscreen WARP target: geometry compared with data/ui/layout/board_03_event.pd (1 px at 1920x1080), texts
// from the string table, the highlighted option, the keys 1-3, Enter, Esc, the Tab trap, the autopause hook
// and the consequences tooltips. The snapshot is the placeholder of make_placeholder_event(): the event
// engine (E11) does not exist yet.

#include "pat/ui/event_window.hpp"

#include <doctest/doctest.h>

#include "ui_loc_fixture.hpp"

TEST_CASE("event window: the placeholder event is the three options of the board") {
    const pat::ui::EventSnapshot snapshot = pat::ui::make_placeholder_event(pat::test::ui_localizer());
    CHECK(snapshot.placeholder);
    const pat::ui::EventView& ev = snapshot.event;
    CHECK(ev.kind == "family");
    CHECK(ev.title == "Продолжение рода");
    CHECK(ev.autopause);
    REQUIRE(ev.options.size() == 3);
    CHECK(ev.options[0].title == "Начать переговоры о браке");
    CHECK(ev.options[2].title == "Отложить решение");
    for (const pat::ui::EventOptionView& option : ev.options) {
        CHECK(option.available);
        CHECK_FALSE(option.consequences.label.empty());  // the consequences are there before the choice
        CHECK_FALSE(option.consequences.terms.empty());
    }
    CHECK(ev.options[0].consequences.terms[0].value == "\xE2\x88\x92"
                                                       "120 \xE2\x82\xBD");  // U+2212 and the rouble sign
}

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
using pat::ui::EventSnapshot;
using pat::ui::EventWindow;

constexpr const char* kFile = "board_03_event.pd";
constexpr const char* kBoard = "03 / Событие и последствия";

Layer layer(const std::string& name, int occurrence = 0) {
    return find_layer(kFile, name, false, kBoard, occurrence);
}
Layer text_layer(const std::string& text, int occurrence = 0) {
    return find_layer(kFile, text, true, kBoard, occurrence);
}

struct Fixture {
    std::unique_ptr<IRenderer> renderer;
    std::unique_ptr<UiContext> ui;
    std::unique_ptr<EventWindow> window;
    std::vector<std::size_t> chosen;      // the indices the choice handler got
    std::vector<bool> shown_autopause;    // `autopause` of every event the shown handler got
    std::vector<bool> visible_in_choice;  // was the window already hidden when the handler ran

    explicit Fixture(PixelSize size = {1920, 1080}) {
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
        auto w =
            EventWindow::create(*ui,
                                std::filesystem::path(PATRIARCH_SPIKE_UI_DIR) / "event.rml",
                                std::filesystem::path(PATRIARCH_ASSETS_DIR) / "ui" / "patriarch" / "icons",
                                pat::test::ui_localizer());
        REQUIRE(w.has_value());
        window = std::move(w).value();
        window->set_event_shown_handler(
            [this](const pat::ui::EventView& ev) { shown_autopause.push_back(ev.autopause); });
        window->set_choice_handler([this](std::size_t index) {
            chosen.push_back(index);
            visible_in_choice.push_back(window->visible());
        });
    }
    ~Fixture() {
        window.reset();
        ui.reset();
        renderer.reset();
    }

    void settle() {
        ui->update();
        ui->update();
    }
    void show(const EventSnapshot& snapshot) {
        window->show_event(snapshot);
        settle();
    }
    void show() { show(pat::ui::make_placeholder_event(pat::test::ui_localizer())); }

    Rml::Element* el(const char* id) const {
        Rml::Element* e = window->document()->GetElementById(id);
        REQUIRE_MESSAGE(e != nullptr, "no element #" << id);
        return e;
    }
    // data-if hides with display: none, so "there" means "laid out".
    bool has(const char* id) const {
        Rml::Element* e = window->document()->GetElementById(id);
        return e != nullptr && e->GetComputedValues().display() != Rml::Style::Display::None &&
               e->GetOffsetWidth() > 0;
    }
    static std::string opt(int i) { return "event-opt-" + std::to_string(i); }
    Rml::Element* option(int i) const { return el(opt(i).c_str()); }
    [[nodiscard]] bool selected(int i) const { return option(i)->IsClassSet("selected"); }
    void click(Rml::Element* e) {
        const float x = e->GetAbsoluteLeft() + e->GetOffsetWidth() * 0.5f;
        const float y = e->GetAbsoluteTop() + e->GetOffsetHeight() * 0.5f;
        ui->handle_event(event::MouseMove{x, y, 0, 0});
        settle();
        ui->handle_event(event::MouseButton{1, true, 1, x, y});
        ui->handle_event(event::MouseButton{1, false, 1, x, y});
        settle();
    }
    void hover(Rml::Element* e) {
        ui->handle_event(event::MouseMove{e->GetAbsoluteLeft() + 10, e->GetAbsoluteTop() + 10, 0, 0});
        settle();
    }
    void key(Rml::Input::KeyIdentifier k) {
        ui->context()->ProcessKeyDown(k, 0);
        ui->context()->ProcessKeyUp(k, 0);
        settle();
    }
    [[nodiscard]] std::string focused() const {
        Rml::Element* f = ui->context()->GetFocusElement();
        return f != nullptr ? f->GetId() : std::string();
    }
};

void check_rect(Rml::Element* e, const Layer& board, double tolerance = 1.0) {
    REQUIRE(board.found);
    CHECK(std::abs(e->GetAbsoluteLeft() - board.x) <= tolerance);
    CHECK(std::abs(e->GetAbsoluteTop() - board.y) <= tolerance);
    CHECK(std::abs(e->GetOffsetWidth() - board.w) <= tolerance);
    CHECK(std::abs(e->GetOffsetHeight() - board.h) <= tolerance);
}

// A text layer: the box of Penpot is the line box of its font, RmlUi's differs by a few pixels in height.
void check_text(Rml::Element* e, const Layer& board) {
    REQUIRE(board.found);
    CHECK(std::abs(e->GetAbsoluteLeft() - board.x) <= 1.0);
    CHECK(std::abs(e->GetAbsoluteTop() - board.y) <= 1.0);
    CHECK(std::abs(e->GetOffsetWidth() - board.w) <= 1.0);
    CHECK(std::abs(e->GetOffsetHeight() - board.h) <= 6.0);
}

// A block of several lines: the board gives the box of its first line only.
void check_block(Rml::Element* e, const Layer& board) {
    REQUIRE(board.found);
    CHECK(std::abs(e->GetAbsoluteLeft() - board.x) <= 1.0);
    CHECK(std::abs(e->GetAbsoluteTop() - board.y) <= 1.0);
    CHECK(std::abs(e->GetOffsetWidth() - board.w) <= 1.0);
    CHECK(e->GetOffsetHeight() >= board.h);
}

bool fonts_ok() {
    if (!pat::test::game_fonts_present()) {
        MESSAGE("Game fonts not found, skipping");
        return false;
    }
    return true;
}

EventSnapshot with_unavailable_second_option() {
    EventSnapshot snapshot = pat::ui::make_placeholder_event(pat::test::ui_localizer());
    snapshot.event.options[1].available = false;
    snapshot.event.options[1].unavailable_reason = "Не хватает 40 \xE2\x82\xBD";
    return snapshot;
}

}  // namespace

TEST_CASE("event window: the sheet, the label, the badge and the texts sit where board 03 puts them") {
    if (!fonts_ok()) {
        return;
    }
    Fixture f;
    f.show();
    REQUIRE(f.window->visible());
    CHECK(f.window->document()->IsVisible());
    check_rect(f.el("event-dim"), layer("Automatic pause / dimmed world"));
    check_rect(f.el("event"), layer("Important event / Продолжение рода"));
    check_rect(f.el("event-strip"), layer("Event top ink strip"));
    check_text(f.el("event-kind"), text_layer("СЕМЕЙНОЕ ДЕЛО"));
    check_rect(f.el("event-badge"), layer("Badge / АВТОПАУЗА"));
    check_rect(f.el("event-rule-top"),
               layer("Divider",
                     7));  // the board has seven dividers of the inspector, the agenda and the minimap first
    check_rect(f.el("event-rule-left"), layer("Divider", 8));
    check_rect(f.el("event-portrait"), layer("Portrait / head of household"));
    check_text(f.el("event-speaker"), text_layer("Михаил Голицын", 1));
    check_text(f.el("event-role"), text_layer("Глава вашего рода"));
    check_block(f.el("event-quote"), text_layer("«Союз домов\nскрепляется не\nсловом, но родством». "));
    check_rect(f.el("event-shield"), layer("Icon / shield", 1));
    check_text(f.el("event-title"), text_layer("Продолжение рода"));
    check_text(f.el("event-subtitle"), text_layer("Будущее семьи решается сегодня"));
    check_block(f.el("event-text"),
                text_layer(pat::test::ui_localizer().format("ui.event.placeholder.text")));
    check_text(f.el("event-choose"), text_layer("КАК ВЫ ПОСТУПИТЕ?"));
    check_text(f.el("event-footer"), text_layer("1 января 1703 · Родовые дела"));
    CHECK(f.el("event-kind")->GetInnerRML() == "СЕМЕЙНОЕ ДЕЛО");
    CHECK(f.el("event-badge")->GetInnerRML() == "АВТОПАУЗА");
    CHECK(f.el("event-choose")->GetInnerRML() == "КАК ВЫ ПОСТУПИТЕ?");
    CHECK(f.el("event-footer")->GetInnerRML() == "1 января 1703 · Родовые дела");
    CHECK(f.el("event")->GetComputedValues().background_color().red == 0xEE);  // paper
    CHECK(f.has("event-portrait"));
}

TEST_CASE("event window: the three options sit where the board puts them, the first one is highlighted") {
    if (!fonts_ok()) {
        return;
    }
    Fixture f;
    f.show();
    const char* const boards[] = {"Choice / Начать переговоры о браке",
                                  "Choice / Узнать больше о наследнике",
                                  "Choice / Отложить решение"};
    const char* const titles[] = {
        "Начать переговоры о браке", "Узнать больше о наследнике", "Отложить решение"};
    const char* const details[] = {"Расходы: 120 ₽   ·   Возможен союз с Долгоруковыми",
                                   "Личность, способности и отношения Ивана",
                                   "Сохранить письмо в журнале дел рода"};
    for (int i = 0; i < 3; ++i) {
        INFO("option " << i);
        Rml::Element* opt = f.option(i);
        check_rect(opt, layer(boards[i]));
        Rml::ElementList found;
        opt->GetElementsByClassName(found, "choice-number");
        REQUIRE(found.size() == 1);
        // The number "2" of the board comes after the badge "2" of the agenda: the second layer with that
        // text.
        check_text(found.front(), text_layer(std::to_string(i + 1), i == 1 ? 1 : 0));
        found.clear();
        opt->GetElementsByClassName(found, "choice-title");
        REQUIRE(found.size() == 1);
        check_text(found.front(), text_layer(titles[i]));
        CHECK(found.front()->GetInnerRML() == titles[i]);
        found.clear();
        opt->GetElementsByClassName(found, "choice-detail");
        REQUIRE(found.size() == 1);
        check_text(found.front(), text_layer(details[i]));
        CHECK(found.front()->GetInnerRML() == details[i]);
    }
    // The highlighted option is green with light text; the others are paper with a hairline frame.
    CHECK(f.selected(0));
    CHECK_FALSE(f.selected(1));
    CHECK_FALSE(f.selected(2));
    CHECK(f.window->selected() == 0);
    const auto& green = f.option(0)->GetComputedValues().background_color();
    CHECK(green.red == 0x36);
    CHECK(green.green == 0x58);
    CHECK(green.blue == 0x47);
    CHECK(f.option(1)->GetComputedValues().background_color().red == 0xEE);
    // Nothing is chosen by showing the window, and the document (not an option) holds the focus.
    CHECK(f.chosen.empty());
    CHECK(f.focused().empty());
}

TEST_CASE("event window: the mouse highlights an option, a click chooses it and closes the window") {
    if (!fonts_ok()) {
        return;
    }
    Fixture f;
    f.show();
    f.hover(f.option(2));
    CHECK(f.selected(2));
    CHECK_FALSE(f.selected(0));
    CHECK(f.window->selected() == 2);
    CHECK(f.chosen.empty());  // pointing is not choosing
    f.click(f.option(1));
    REQUIRE(f.chosen.size() == 1);
    CHECK(f.chosen[0] == 1);
    CHECK_FALSE(
        f.visible_in_choice[0]);  // hidden before the handler ran, so the handler may show the next event
    CHECK_FALSE(f.window->visible());
    CHECK_FALSE(f.window->document()->IsVisible());
}

TEST_CASE("event window: the keys 1-3 highlight and focus an option, Enter chooses it, Esc does nothing") {
    if (!fonts_ok()) {
        return;
    }
    Fixture f;
    f.show();
    f.key(Rml::Input::KI_2);
    CHECK(f.selected(1));
    CHECK(f.focused() == "event-opt-1");
    CHECK(f.chosen.empty());  // the digit selects, it does not decide
    f.key(Rml::Input::KI_3);
    CHECK(f.selected(2));
    CHECK(f.focused() == "event-opt-2");
    f.key(Rml::Input::KI_NUMPAD1);
    CHECK(f.selected(0));
    f.key(Rml::Input::KI_9);  // no such option: nothing moves
    CHECK(f.selected(0));
    f.key(Rml::Input::KI_DOWN);
    CHECK(f.selected(1));
    f.key(Rml::Input::KI_DOWN);
    f.key(Rml::Input::KI_DOWN);  // the end of the list: stays
    CHECK(f.selected(2));
    f.key(Rml::Input::KI_UP);
    CHECK(f.selected(1));
    f.key(Rml::Input::KI_ESCAPE);
    CHECK(f.window->visible());
    CHECK(f.chosen.empty());
    // The focus ring shows after the keyboard was used.
    CHECK(f.ui->context()->GetRootElement()->IsClassSet("kbd"));
    f.key(Rml::Input::KI_RETURN);
    REQUIRE(f.chosen.size() == 1);
    CHECK(f.chosen[0] == 1);
    CHECK_FALSE(f.window->visible());
}

TEST_CASE("event window: Esc is swallowed so the host does not take it for 'quit'") {
    if (!fonts_ok()) {
        return;
    }
    Fixture f;
    f.show();
    // The engine maps the scancode of Escape to KI_ESCAPE; the key listener of the window stops it, so the
    // engine reports it as consumed and the spike does not end the program.
    CHECK(f.ui->handle_event(event::Key{scancode::escape, true, false, 0}));
    CHECK(f.window->visible());
    CHECK(f.chosen.empty());
}

TEST_CASE("event window: the window is modal, Tab stays on the three options") {
    if (!fonts_ok()) {
        return;
    }
    Fixture f;
    f.show();
    CHECK(f.window->document()->IsModal());
    std::vector<std::string> order;
    for (int i = 0; i < 4; ++i) {
        f.key(Rml::Input::KI_TAB);
        INFO("focused '" << f.focused() << "'");
        CHECK(f.focused().rfind("event-opt-", 0) == 0);
        order.push_back(f.focused());
    }
    CHECK(order[0] == "event-opt-0");
    CHECK(order[1] == "event-opt-1");
    CHECK(order[2] == "event-opt-2");
    CHECK(order[3] == "event-opt-0");  // wraps inside the window
    // The focus moved the highlight with it.
    CHECK(f.selected(0));
}

TEST_CASE("event window: the keyboard focus draws the gold ring only after a key") {
    if (!fonts_ok()) {
        return;
    }
    Fixture f;
    f.show();
    f.click(f.option(0));  // chooses (closes); show again for the check
    f.show();
    f.key(Rml::Input::KI_TAB);
    f.key(Rml::Input::KI_TAB);
    REQUIRE(f.focused() == "event-opt-1");
    Rml::ElementList frames;
    f.option(1)->GetElementsByClassName(frames, "choice-frame");
    REQUIRE(frames.size() == 1);
    const auto& ring = frames.front()->GetComputedValues();
    CHECK(ring.border_top_width() == 2.0f);
    CHECK(ring.border_top_color().red == 0xD2);  // gold
    // The content did not move: the ring is its own element.
    Rml::ElementList titles;
    f.option(1)->GetElementsByClassName(titles, "choice-title");
    CHECK(std::abs(titles.front()->GetAbsoluteLeft() - 834.0f) <= 1.0f);
}

TEST_CASE("event window: the autopause hook runs on every show and the badge follows the flag") {
    if (!fonts_ok()) {
        return;
    }
    Fixture f;
    f.show();
    REQUIRE(f.shown_autopause.size() == 1);
    CHECK(f.shown_autopause[0]);
    CHECK(f.has("event-badge"));
    f.window->close();
    f.settle();
    CHECK_FALSE(f.window->visible());
    EventSnapshot quiet = pat::ui::make_placeholder_event(pat::test::ui_localizer());
    quiet.event.autopause = false;
    f.show(quiet);
    REQUIRE(f.shown_autopause.size() == 2);
    CHECK_FALSE(f.shown_autopause[1]);
    CHECK_FALSE(f.has("event-badge"));
    CHECK(f.chosen.empty());  // closing by the host is not a choice
}

TEST_CASE("event window: an unavailable option is dimmed with its reason, it cannot be picked") {
    if (!fonts_ok()) {
        return;
    }
    Fixture f;
    f.show(with_unavailable_second_option());
    Rml::Element* second = f.option(1);
    CHECK(second->IsClassSet("is-disabled"));
    CHECK(second->IsClassSet("has-reason"));
    CHECK(second->GetComputedValues().background_color().red == 0xE1);  // paper2
    Rml::ElementList detail;
    second->GetElementsByClassName(detail, "choice-detail");
    REQUIRE(detail.size() == 1);
    CHECK(detail.front()->GetInnerRML() == "Не хватает 40 \xE2\x82\xBD");  // the reason replaces the line
    CHECK(detail.front()->GetComputedValues().color().red == 0x8D);        // red
    f.key(Rml::Input::KI_2);
    CHECK(f.selected(0));
    f.window->select(1);
    CHECK(f.window->selected() == 0);
    f.click(second);
    CHECK(f.chosen.empty());
    CHECK(f.window->visible());
    CHECK_FALSE(f.window->choose(1));
    // Down skips it.
    f.key(Rml::Input::KI_DOWN);
    CHECK(f.selected(2));
    // Tab skips it too.
    f.key(Rml::Input::KI_TAB);
    CHECK(f.focused() != "event-opt-1");
    f.key(Rml::Input::KI_TAB);
    CHECK(f.focused() != "event-opt-1");
    f.key(Rml::Input::KI_TAB);
    CHECK(f.focused() != "event-opt-1");
}

TEST_CASE(
    "event window: the default option can be another one, and a disabled default falls to the first free") {
    if (!fonts_ok()) {
        return;
    }
    Fixture f;
    EventSnapshot snapshot = pat::ui::make_placeholder_event(pat::test::ui_localizer());
    snapshot.event.default_option = 2;
    f.show(snapshot);
    CHECK(f.selected(2));
    snapshot.event.options[2].available = false;
    f.show(snapshot);
    CHECK(f.selected(0));
}

TEST_CASE("event window: the consequences of an option are a tooltip behind its key, before the choice") {
    if (!fonts_ok()) {
        return;
    }
    Fixture f;
    f.show();
    for (int i = 0; i < 3; ++i) {
        const std::string key = "event:" + std::to_string(i);
        const pat::ui::Explanation* tip = f.window->explanation(key);
        REQUIRE_MESSAGE(tip != nullptr, key);
        CHECK(tip == &f.window->snapshot().event.options[static_cast<std::size_t>(i)].consequences);
        CHECK(f.option(i)->GetAttribute<Rml::String>("data-tip", "") == key);
    }
    CHECK(f.window->explanation("event:0")->label == "Если начать переговоры");
    CHECK(f.window->explanation("event:3") == nullptr);
    CHECK(f.window->explanation("event:") == nullptr);
    CHECK(f.window->explanation("event:x") == nullptr);
    CHECK(f.window->explanation("skill:0") == nullptr);
    // An option without consequences has no tooltip key.
    EventSnapshot bare = pat::ui::make_placeholder_event(pat::test::ui_localizer());
    bare.event.options[2].consequences = pat::ui::Explanation();
    f.show(bare);
    CHECK(f.window->explanation("event:2") == nullptr);
    CHECK(f.option(2)->GetAttribute<Rml::String>("data-tip", "").empty());
}

TEST_CASE("event window: unknown kind, no category, no portrait and no quote still make a window") {
    if (!fonts_ok()) {
        return;
    }
    Fixture f;
    EventSnapshot snapshot = pat::ui::make_placeholder_event(pat::test::ui_localizer());
    snapshot.event.kind = "unheard-of";
    snapshot.event.category.clear();
    snapshot.event.portrait.clear();
    snapshot.event.quote.clear();
    f.show(snapshot);
    CHECK(f.el("event-kind")->GetInnerRML() == "СОБЫТИЕ");
    CHECK(f.el("event-footer")->GetInnerRML() == "1 января 1703");
    CHECK_FALSE(f.has("event-portrait"));
    CHECK_FALSE(f.has("event-quote"));
}

TEST_CASE("event window: more options than the board holds are cut to three") {
    if (!fonts_ok()) {
        return;
    }
    Fixture f;
    EventSnapshot snapshot = pat::ui::make_placeholder_event(pat::test::ui_localizer());
    snapshot.event.options.push_back(snapshot.event.options[0]);
    f.show(snapshot);
    CHECK(f.has("event-opt-2"));
    CHECK(f.window->document()->GetElementById("event-opt-3") == nullptr);
    CHECK(f.window->explanation("event:3") == nullptr);
}

TEST_CASE("event window: captions come from the string table, a missing key shows as [[key]]") {
    if (!fonts_ok()) {
        return;
    }
    {
        Fixture f;
        f.show();
        CHECK(f.el("event-choose")->GetInnerRML() == pat::test::ui_localizer().format("ui.event.choose"));
    }
    // A table that knows no key: the chrome shows the keys, the texts of the event stay as the bridge made
    // them.
    pat::loc::Localizer empty = pat::test::load_empty_localizer();
    RendererDesc rd;
    rd.size = {1920, 1080};
    rd.software = true;
    auto created = create_renderer(rd);
    REQUIRE(created.has_value());
    auto renderer = std::move(created).value();
    UiDesc ud;
    ud.size = {1920, 1080};
    ud.ui_scale = 1.0f;
    ud.font_files = pat::test::game_font_files();
    auto ui_created = UiContext::create(*renderer, nullptr, ud);
    REQUIRE(ui_created.has_value());
    auto ui = std::move(ui_created).value();
    pat::test::install_game_theme(*ui);
    auto w = EventWindow::create(*ui,
                                 std::filesystem::path(PATRIARCH_SPIKE_UI_DIR) / "event.rml",
                                 std::filesystem::path(PATRIARCH_ASSETS_DIR) / "ui" / "patriarch" / "icons",
                                 empty);
    REQUIRE(w.has_value());
    auto window = std::move(w).value();
    EventSnapshot snapshot;
    snapshot.event.title = "Заголовок";
    snapshot.event.kind = "family";
    snapshot.event.options.push_back({});
    snapshot.event.options.back().title = "Вариант";
    window->show_event(snapshot);
    ui->update();
    ui->update();
    CHECK(window->document()->GetElementById("event-choose")->GetInnerRML() == "[[ui.event.choose]]");
    CHECK(window->document()->GetElementById("event-kind")->GetInnerRML() == "[[ui.event.kind.other]]");
    CHECK(window->document()->GetElementById("event-title")->GetInnerRML() == "Заголовок");
    window.reset();
}

TEST_CASE("event window: the sheet keeps its place at 1280x720 and in 21:9") {
    if (!fonts_ok()) {
        return;
    }
    {
        Fixture f({1280, 720});
        f.show();
        const float scale = design_ui_scale({1280, 720});
        Rml::Element* sheet = f.el("event");
        CHECK(std::abs(sheet->GetOffsetWidth() - 1032 * scale) <= 1.5);
        CHECK(std::abs(sheet->GetOffsetHeight() - 794 * scale) <= 1.5);
        // The same offset from the middle of the screen as on the board.
        CHECK(std::abs(sheet->GetAbsoluteLeft() - (640 + (484 - 960) * scale)) <= 1.5);
        CHECK(std::abs(sheet->GetAbsoluteTop() - (360 + (154 - 540) * scale)) <= 1.5);
        CHECK(sheet->GetAbsoluteLeft() >= 0);
        CHECK(sheet->GetAbsoluteTop() >= 0);
        CHECK(sheet->GetAbsoluteLeft() + sheet->GetOffsetWidth() <= 1280);
        CHECK(sheet->GetAbsoluteTop() + sheet->GetOffsetHeight() <= 720);
        Rml::Element* last = f.option(2);
        CHECK(last->GetAbsoluteTop() + last->GetOffsetHeight() <=
              sheet->GetAbsoluteTop() + sheet->GetOffsetHeight());
    }
    {
        Fixture f({2560, 1080});
        f.show();
        Rml::Element* sheet = f.el("event");
        // The sheet stays centred: the 1920 canvas is shifted by 320.
        CHECK(std::abs(sheet->GetAbsoluteLeft() - (320 + 484)) <= 1.5);
        CHECK(std::abs(sheet->GetAbsoluteTop() - 154) <= 1.5);
        CHECK(std::abs(sheet->GetOffsetWidth() - 1032) <= 1.5);
        Rml::Element* dim = f.el("event-dim");  // the dim goes on to the right edge of the window
        CHECK(std::abs(dim->GetAbsoluteLeft() - 96) <= 1.5);
        CHECK(std::abs(dim->GetAbsoluteLeft() + dim->GetOffsetWidth() - 2560) <= 1.5);
    }
}

#endif  // _WIN32
