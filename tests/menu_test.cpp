// The start menu (UI-D12, #284; boards "МЕНЮ / ..." of the mockup), parts 1 to 3: the screen stack, the main
// menu, "Об игре", "Загрузка", the dialogs, the overview page, "Новая игра", "Свой род", "Цель партии", the
// world screen and "Настройки" (four tabs and their dialogs), on an offscreen WARP target. Rectangles are
// compared with data/ui/layout/menu_boards.pd (1 px at 1920x1080), texts come from the string table, and the
// actions that need a world only reach the callbacks.

#include "pat/ui/menu.hpp"

#include <doctest/doctest.h>

#include "ui_loc_fixture.hpp"

TEST_CASE("menu navigator: back closes the dialog, then the screen, then asks to exit") {
    using namespace pat::ui;
    MenuNavigator nav(MenuScreen::Main);
    CHECK(nav.top() == MenuScreen::Main);
    CHECK(nav.depth() == 1);
    nav.push(MenuScreen::Load);
    nav.push(MenuScreen::Load);  // a repeat of the current screen is ignored
    CHECK(nav.depth() == 2);
    nav.open_dialog(MenuDialog::SaveUnavailable);
    CHECK(nav.back() == MenuBack::ClosedDialog);
    CHECK(nav.top() == MenuScreen::Load);
    CHECK(nav.back() == MenuBack::WentBack);
    CHECK(nav.top() == MenuScreen::Main);
    CHECK(nav.back() == MenuBack::AskedExit);
    CHECK(nav.dialog() == MenuDialog::Exit);
    CHECK(nav.back() == MenuBack::ClosedDialog);
    CHECK_FALSE(nav.pop());

    MenuNavigator overview(MenuScreen::Overview);
    CHECK(overview.back() == MenuBack::Nothing);  // only the main menu asks to leave
    CHECK(overview.dialog() == MenuDialog::None);
}

TEST_CASE("menu: the placeholder snapshot is the three saves of the mockup") {
    const pat::ui::MenuSnapshot snapshot = pat::ui::make_placeholder_menu(pat::test::ui_localizer());
    REQUIRE(snapshot.saves.size() == 3);
    CHECK(snapshot.placeholder);
    CHECK(snapshot.saves[0].kind == pat::ui::MenuSaveKind::Auto);
    CHECK(snapshot.saves[1].name == "Перед переговорами");
    CHECK_FALSE(snapshot.saves[2].compatible);
    CHECK(snapshot.saves[0].house_gen == "Голицыных");
}

TEST_CASE("settings model: the defaults are the values drawn on the boards") {
    const pat::ui::SettingsModel m;
    CHECK(m.width == 1920);
    CHECK(m.height == 1080);
    CHECK(m.frame_limit == 60);
    CHECK(m.vsync);
    CHECK(m.ui_scale == 100);
    CHECK_FALSE(m.reduce_motion);
    CHECK(m.master_volume == 80);
    CHECK(m.music_volume == 65);
    CHECK(m.ambient_volume == 70);
    CHECK(m.interface_volume == 75);
    CHECK(m.keys[static_cast<std::size_t>(pat::ui::KeyAction::Pause)] == "space");
    CHECK(m.keys[static_cast<std::size_t>(pat::ui::KeyAction::MapMode)] == "m");
}

TEST_CASE("settings model: a tab is reset and compared on its own") {
    using namespace pat::ui;
    SettingsModel edited;
    edited.ui_scale = 125;
    edited.reduce_motion = true;
    edited.master_volume = 10;
    const SettingsModel defaults;
    CHECK(settings_differ(edited, defaults, SettingsTab::Interface));
    CHECK(settings_differ(edited, defaults, SettingsTab::Audio));
    CHECK_FALSE(settings_differ(edited, defaults, SettingsTab::Video));
    CHECK_FALSE(settings_differ(edited, defaults, SettingsTab::Controls));
    reset_settings_tab(edited, SettingsTab::Interface);
    CHECK(edited.ui_scale == 100);
    CHECK_FALSE(edited.reduce_motion);
    CHECK(edited.master_volume == 10);  // the other tabs keep their edits
    CHECK_FALSE(settings_differ(edited, defaults, SettingsTab::Interface));
}

TEST_CASE("settings model: a key that is taken moves to the action that gave it up") {
    using namespace pat::ui;
    SettingsModel m;
    rebind_key(m, KeyAction::Pause, "p");
    CHECK(m.keys[static_cast<std::size_t>(KeyAction::Pause)] == "p");
    rebind_key(m, KeyAction::MapMode, "p");  // "p" was the pause: the two swap
    CHECK(m.keys[static_cast<std::size_t>(KeyAction::MapMode)] == "p");
    CHECK(m.keys[static_cast<std::size_t>(KeyAction::Pause)] == "m");
    rebind_key(m, KeyAction::Pause, "m");  // the key it already has: nothing moves
    CHECK(m.keys[static_cast<std::size_t>(KeyAction::MapMode)] == "p");
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
using pat::ui::MenuDialog;
using pat::ui::MenuScreen;
using pat::ui::MenuWindow;

constexpr const char* kFile = "menu_boards.pd";
constexpr const char* kFirst = "МЕНЮ / 01 Главное меню / первый запуск";
constexpr const char* kSaved = "МЕНЮ / 02 Главное меню / есть сохранение";
constexpr const char* kLoad = "МЕНЮ / 07 Загрузка / список сохранений";
constexpr const char* kLoadManual = "МЕНЮ / Загрузка / выбрано ручное сохранение";
constexpr const char* kLoadEmpty = "МЕНЮ / 08 Загрузка / нет сохранений";
constexpr const char* kUnavailable = "МЕНЮ / Диалог / Сохранение недоступно";
constexpr const char* kExit = "МЕНЮ / Диалог / Выйти из Patriarch?";
constexpr const char* kAbout = "МЕНЮ / Об игре";
constexpr const char* kOverview = "МЕНЮ / Обзор стартовых экранов";

Layer layer(const char* board, const std::string& name) {
    return find_layer(kFile, name, false, board);
}
Layer text_layer(const char* board, const std::string& text) {
    return find_layer(kFile, text, true, board);
}

struct Fixture {
    std::unique_ptr<IRenderer> renderer;
    std::unique_ptr<UiContext> ui;
    std::unique_ptr<MenuWindow> menu;
    std::vector<std::string> log;  // what the actions reported
    std::vector<pat::ui::MenuStart> starts;
    std::vector<pat::ui::SettingsModel> applied;  // what "Применить" handed over

    explicit Fixture(PixelSize size = {1920, 1080}, bool with_saves = true) {
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
        auto m =
            MenuWindow::create(*ui,
                               std::filesystem::path(PATRIARCH_SPIKE_UI_DIR) / "menu.rml",
                               std::filesystem::path(PATRIARCH_ASSETS_DIR) / "ui" / "patriarch" / "icons",
                               pat::test::ui_localizer());
        REQUIRE(m.has_value());
        menu = std::move(m).value();
        pat::ui::MenuSnapshot snapshot;
        if (with_saves) {
            snapshot = pat::ui::make_placeholder_menu(pat::test::ui_localizer());
        }
        menu->set_snapshot(snapshot);
        menu->set_new_game(pat::ui::make_placeholder_new_game(pat::test::ui_localizer()));
        pat::ui::MenuActions actions;
        actions.start_game = [this](const pat::ui::MenuStart& start) { starts.push_back(start); };
        actions.cancel_prepare = [this] { log.push_back("cancel"); };
        actions.enter_game = [this] { log.push_back("enter"); };
        actions.continue_game = [this] { log.push_back("continue"); };
        actions.load_save = [this](const std::string& id) { log.push_back("load:" + id); };
        actions.apply_settings = [this](const pat::ui::SettingsModel& settings) {
            log.push_back("apply");
            applied.push_back(settings);
        };
        actions.quit = [this] { log.push_back("quit"); };
        menu->set_actions(actions);
    }
    ~Fixture() {
        menu.reset();
        ui.reset();
        renderer.reset();
    }

    void settle() {
        ui->update();
        menu->update();
        ui->update();
    }
    void open(MenuScreen root = MenuScreen::Main) {
        menu->open(root);
        settle();
    }
    Rml::Element* el(const char* id) const {
        Rml::Element* e = menu->document()->GetElementById(id);
        REQUIRE_MESSAGE(e != nullptr, "no element #" << id);
        return e;
    }
    // data-if hides with display: none, so "there" means "laid out".
    bool has(const char* id) const {
        Rml::Element* e = menu->document()->GetElementById(id);
        return e != nullptr && e->GetComputedValues().display() != Rml::Style::Display::None &&
               e->GetOffsetWidth() > 0;
    }
    void click(const char* id) {
        Rml::Element* e = el(id);
        const float x = e->GetAbsoluteLeft() + e->GetOffsetWidth() * 0.5f;
        const float y = e->GetAbsoluteTop() + e->GetOffsetHeight() * 0.5f;
        ui->handle_event(event::MouseMove{x, y, 0, 0});
        settle();
        ui->handle_event(event::MouseButton{1, true, 1, x, y});
        ui->handle_event(event::MouseButton{1, false, 1, x, y});
        settle();
    }
    void hover(const char* id) {
        Rml::Element* e = el(id);
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

// A child of a dialog: the board gives it relative to the dialog, the dialog itself is centred.
void check_inside(Rml::Element* e, Rml::Element* parent, const Layer& board, double tolerance = 1.0) {
    REQUIRE(board.found);
    CHECK(std::abs(e->GetAbsoluteLeft() - parent->GetAbsoluteLeft() - board.x) <= tolerance);
    CHECK(std::abs(e->GetAbsoluteTop() - parent->GetAbsoluteTop() - board.y) <= tolerance);
    CHECK(std::abs(e->GetOffsetWidth() - board.w) <= tolerance);
    CHECK(std::abs(e->GetOffsetHeight() - board.h) <= tolerance);
}

void check_text_inside(Rml::Element* e, Rml::Element* parent, const Layer& board) {
    REQUIRE(board.found);
    CHECK(std::abs(e->GetAbsoluteLeft() - parent->GetAbsoluteLeft() - board.x) <= 1.0);
    CHECK(std::abs(e->GetAbsoluteTop() - parent->GetAbsoluteTop() - board.y) <= 1.0);
    CHECK(std::abs(e->GetOffsetWidth() - board.w) <= 1.0);
    CHECK(std::abs(e->GetOffsetHeight() - board.h) <= 6.0);
}

Rml::Element* child_of_class(Rml::Element* parent, const char* cls) {
    Rml::ElementList found;
    parent->GetElementsByClassName(found, cls);
    REQUIRE_MESSAGE(!found.empty(), "no ." << cls);
    return found.front();
}

std::string text_of(Rml::Element* e) {
    return e->GetInnerRML();
}

int visible_count(Rml::ElementDocument* doc, const char* cls) {
    Rml::ElementList found;
    doc->GetElementsByClassName(found, cls);
    int count = 0;
    for (Rml::Element* e : found) {
        if (e->GetComputedValues().display() != Rml::Style::Display::None && e->GetOffsetWidth() > 0) {
            ++count;
        }
    }
    return count;
}

bool fonts_ok() {
    if (!pat::test::game_fonts_present()) {
        MESSAGE("Game fonts not found, skipping");
        return false;
    }
    return true;
}

}  // namespace

TEST_CASE("menu: the main menu of the first run sits where board 01 puts it") {
    if (!fonts_ok()) {
        return;
    }
    Fixture f({1920, 1080}, false);
    f.open();
    REQUIRE(f.menu->screen() == MenuScreen::Main);
    CHECK_FALSE(f.menu->has_save_variant());
    check_rect(f.el("mi-new"), layer(kFirst, "Menu item / Новая игра"));
    check_rect(f.el("mi-load"), layer(kFirst, "Menu item / Загрузить игру"));
    check_rect(f.el("mi-settings"), layer(kFirst, "Menu item / Настройки"));
    check_rect(f.el("mi-about"), layer(kFirst, "Menu item / Об игре"));
    check_rect(f.el("mi-exit"), layer(kFirst, "Menu item / Выйти из игры"));
    check_text(f.el("menu-title"), text_layer(kFirst, "PATRIARCH"));
    check_text(f.el("menu-tagline"), text_layer(kFirst, "РОД · ВЛАСТЬ · НАСЛЕДИЕ"));
    check_rect(f.el("menu-title-rule"), layer(kFirst, "Engraved title rule"));
    check_text(f.el("menu-lead"), text_layer(kFirst, "Россия. Начало XVIII века."));
    check_rect(f.el("menu-shield"), layer(kFirst, "Icon / shield"));
    check_rect(f.el("menu-contrast"), layer(kFirst, "Left navigation contrast"));
    check_rect(f.el("menu-lang"), layer(kFirst, "Language selector / RU"));
    check_text(f.el("menu-copyright"), text_layer(kFirst, "© 2026 savinovanton · Все права защищены"));
    check_text(f.el("menu-region"), text_layer(kFirst, "Россия · 1703"));
    check_text(child_of_class(f.el("mi-load"), "mi-note"), text_layer(kFirst, "Пока нет сохранений"));
    CHECK(text_of(f.el("menu-lead")) == "Россия. Начало XVIII века.");
    // The primary entry is highlighted and holds the focus; the disabled one is dimmed and not a tab stop.
    CHECK(f.menu->highlighted() == 0);
    CHECK(f.focused() == "mi-new");
    CHECK(f.el("mi-load")->IsClassSet("is-disabled"));
    CHECK(f.el("mi-new")->IsClassSet("selected"));
}

TEST_CASE("menu: the main menu with a save sits where board 02 puts it") {
    if (!fonts_ok()) {
        return;
    }
    Fixture f;
    f.open();
    CHECK(f.menu->has_save_variant());
    check_rect(f.el("mi-continue"), layer(kSaved, "Menu item / Продолжить"));
    check_rect(f.el("mi-new"), layer(kSaved, "Menu item / Новая игра"));
    check_rect(f.el("mi-load"), layer(kSaved, "Menu item / Загрузить игру"));
    check_rect(f.el("mi-settings"), layer(kSaved, "Menu item / Настройки"));
    check_rect(f.el("mi-about"), layer(kSaved, "Menu item / Об игре"));
    check_rect(f.el("mi-exit"), layer(kSaved, "Menu item / Выйти из игры"));
    check_text(f.el("menu-lead"), text_layer(kSaved, "Ваша история продолжается."));
    check_text(child_of_class(f.el("mi-continue"), "mi-note"),
               text_layer(kSaved, "Голицыны · 1 января 1703 · Автосохранение"));
    CHECK(text_of(child_of_class(f.el("mi-continue"), "mi-note")) ==
          "Голицыны · 1 января 1703 · Автосохранение");
    CHECK_FALSE(f.has("menu-hint"));  // the greeting of the first run is not shown
    CHECK_FALSE(f.el("mi-load")->IsClassSet("is-disabled"));
}

TEST_CASE("menu: the first run shows the greeting of board 01") {
    if (!fonts_ok()) {
        return;
    }
    Fixture f({1920, 1080}, false);
    f.open();
    const Layer hint =
        find_layer(kFile, "Империя меняется. Вашему роду предстоит остаться в истории.", false, kFirst);
    check_text(f.el("menu-hint"), hint);
}

TEST_CASE("menu: Up and Down move the highlight, Enter presses, the actions only reach the callbacks") {
    if (!fonts_ok()) {
        return;
    }
    Fixture f;
    f.open();
    CHECK(f.focused() == "mi-continue");
    f.key(Rml::Input::KI_DOWN);
    CHECK(f.menu->highlighted() == 1);
    CHECK(f.focused() == "mi-new");
    f.key(Rml::Input::KI_UP);
    f.key(Rml::Input::KI_UP);  // wraps to the last entry
    CHECK(f.menu->highlighted() == 5);
    CHECK(f.focused() == "mi-exit");
    f.key(Rml::Input::KI_DOWN);
    CHECK(f.menu->highlighted() == 0);
    f.key(Rml::Input::KI_RETURN);
    REQUIRE(f.log.size() == 1);
    CHECK(f.log[0] == "continue");
    CHECK(f.menu->screen() == MenuScreen::World);  // the restore of the newest save
    f.key(Rml::Input::KI_ESCAPE);
    REQUIRE(f.log.size() == 2);
    CHECK(f.log[1] == "cancel");
    CHECK(f.menu->screen() == MenuScreen::Main);
    f.click("mi-settings");  // "Настройки" open their own screen (part 3): nothing reaches the game
    CHECK(f.menu->screen() == MenuScreen::Settings);
    CHECK(f.log.size() == 2);
    f.key(Rml::Input::KI_ESCAPE);
    CHECK(f.menu->screen() == MenuScreen::Main);
    f.click("mi-new");  // "Новая игра" opens its own screen (part 2): nothing reaches the game yet
    CHECK(f.menu->screen() == MenuScreen::NewGame);
    CHECK(f.starts.empty());
}

TEST_CASE("menu: the disabled entry is skipped and cannot be pressed") {
    if (!fonts_ok()) {
        return;
    }
    Fixture f({1920, 1080}, false);
    f.open();
    f.key(Rml::Input::KI_DOWN);
    CHECK(f.menu->highlighted() == 2);  // "Настройки": "Загрузить игру" is dimmed
    CHECK(f.focused() == "mi-settings");
    f.click("mi-load");
    CHECK(f.menu->screen() == MenuScreen::Main);
    f.menu->press_item("load");
    CHECK(f.menu->screen() == MenuScreen::Main);
}

TEST_CASE("menu: hovering an entry highlights it and moves the focus") {
    if (!fonts_ok()) {
        return;
    }
    Fixture f;
    f.open();
    f.hover("mi-about");
    CHECK(f.menu->highlighted() == 4);
    CHECK(f.focused() == "mi-about");
    CHECK(f.el("mi-about")->IsClassSet("selected"));
    CHECK_FALSE(f.el("mi-continue")->IsClassSet("selected"));
}

TEST_CASE("menu: the exit dialog opens from the entry and from Esc, and Esc or 'Остаться' closes it") {
    if (!fonts_ok()) {
        return;
    }
    Fixture f;
    f.open();
    f.click("mi-exit");
    REQUIRE(f.menu->dialog() == MenuDialog::Exit);
    CHECK(f.focused() == "dlg-stay");  // the safe answer is the default one
    Rml::Element* dialog = f.el("dialog-exit");
    CHECK(std::abs(dialog->GetOffsetWidth() - 700) <= 1);
    CHECK(std::abs(dialog->GetOffsetHeight() - 350) <= 1);
    CHECK(std::abs(dialog->GetAbsoluteLeft() + dialog->GetOffsetWidth() / 2 - 960) <= 1);  // centred
    CHECK(std::abs(dialog->GetAbsoluteTop() + dialog->GetOffsetHeight() / 2 - 540) <= 1);
    check_inside(f.el("dlg-stay"), dialog, layer(kExit, "Button / Остаться"));
    check_inside(f.el("dlg-exit"), dialog, layer(kExit, "Button / Выйти из игры"));
    check_text_inside(
        child_of_class(dialog, "dialog-title"), dialog, text_layer(kExit, "Выйти из Patriarch?"));
    check_text_inside(f.el("dialog-exit-text"),
                      dialog,
                      text_layer(kExit, "Игра закроется, и вы вернётесь на рабочий стол."));
    f.key(Rml::Input::KI_ESCAPE);
    CHECK(f.menu->dialog() == MenuDialog::None);
    CHECK(f.log.empty());

    f.key(Rml::Input::KI_ESCAPE);  // on the main menu Esc asks to leave
    CHECK(f.menu->dialog() == MenuDialog::Exit);
    f.click("dlg-stay");
    CHECK(f.menu->dialog() == MenuDialog::None);
    CHECK(f.log.empty());

    f.click("mi-exit");
    f.click("dlg-exit");
    REQUIRE(f.log.size() == 1);
    CHECK(f.log[0] == "quit");
}

TEST_CASE("menu: a dialog traps Tab and the screen behind takes no clicks") {
    if (!fonts_ok()) {
        return;
    }
    Fixture f;
    f.open();
    f.click("mi-exit");
    REQUIRE(f.menu->dialog() == MenuDialog::Exit);
    for (int i = 0; i < 4; ++i) {
        f.key(Rml::Input::KI_TAB);
        INFO("focused '" << f.focused() << "'");
        CHECK((f.focused() == "dlg-stay" || f.focused() == "dlg-exit"));
    }
    Rml::Element* behind = f.el("mi-about");
    const float x = behind->GetAbsoluteLeft() + 40;
    const float y = behind->GetAbsoluteTop() + 20;
    f.ui->handle_event(event::MouseMove{x, y, 0, 0});
    f.ui->handle_event(event::MouseButton{1, true, 1, x, y});
    f.ui->handle_event(event::MouseButton{1, false, 1, x, y});
    f.settle();
    CHECK(f.menu->screen() == MenuScreen::Main);
    CHECK(f.menu->dialog() == MenuDialog::Exit);
}

TEST_CASE("menu: 'Об игре' sits where its board puts it, and Esc or the button goes back") {
    if (!fonts_ok()) {
        return;
    }
    Fixture f;
    f.open();
    f.click("mi-about");
    REQUIRE(f.menu->screen() == MenuScreen::About);
    CHECK(f.menu->depth() == 2);
    CHECK(f.focused() == "btn-main");
    check_rect(f.el("panel"), layer(kAbout, "Front panel"));
    check_rect(f.el("panel-rule"), layer(kAbout, "Panel gold rule"));
    check_text(f.el("page-title"), text_layer(kAbout, "Истории создают роды"));
    check_text(f.el("page-subtitle"),
               text_layer(kAbout, "Patriarch · Династическая стратегия в России начала XVIII века"));
    check_text(f.el("page-eyebrow"), text_layer(kAbout, "PATRIARCH / НАЧАЛО ИСТОРИИ"));
    check_text(f.el("page-brand"), text_layer(kAbout, "PATRIARCH"));
    check_text(f.el("page-caption"), text_layer(kAbout, "ИСТОРИИ СОЗДАЮТ РОДЫ"));
    check_rect(f.el("page-shield"), layer(kAbout, "Icon / shield"));
    check_rect(f.el("btn-main"), layer(kAbout, "Button / Главное меню"));
    check_rect(f.el("about-story"), layer(kAbout, "About / story"));
    check_rect(f.el("about-credits"), layer(kAbout, "About / credits"));
    check_text(f.el("about-story-title"), text_layer(kAbout, "Оставьте после себя имя"));
    check_text(f.el("about-value-era"), text_layer(kAbout, "Россия · Старт в 1703 году"));
    check_text(f.el("about-key-people"), text_layer(kAbout, "Персонажи"));
    check_text(f.el("credit-value-author"), text_layer(kAbout, "savinovanton"));
    check_text(f.el("credit-value-art"), text_layer(kAbout, "Созданы с помощью ИИ"));
    check_text(f.el("credits-link"), text_layer(kAbout, "github.com/savinovanton/patriarch"));
    CHECK(text_of(f.el("page-title")) == "Истории создают роды");

    f.key(Rml::Input::KI_ESCAPE);
    CHECK(f.menu->screen() == MenuScreen::Main);
    CHECK(f.menu->depth() == 1);
    CHECK(f.focused() == "mi-about");  // the entry that led away keeps the highlight
    f.click("mi-about");
    f.click("btn-main");
    {
        Rml::Element* hover = f.ui->context()->GetHoverElement();
        INFO("hover " << (hover ? hover->GetId() + "/" + hover->GetTagName() : std::string("none")));
        CHECK(f.menu->screen() == MenuScreen::Main);
    }
}

TEST_CASE("menu: 'Загрузка' sits where board 07 puts it") {
    if (!fonts_ok()) {
        return;
    }
    Fixture f;
    f.open();
    f.click("mi-load");
    REQUIRE(f.menu->screen() == MenuScreen::Load);
    CHECK(f.focused() == "save-0");
    check_rect(f.el("panel"), layer(kLoad, "Front panel"));
    check_text(f.el("page-title"), text_layer(kLoad, "Продолжить родовую летопись"));
    check_text(f.el("page-subtitle"),
               text_layer(kLoad, "Выберите сохранение, чтобы вернуться к истории вашего рода."));
    check_text(f.el("page-caption"), text_layer(kLoad, "ПРОДОЛЖИТЬ РОДОВУЮ ЛЕТОПИСЬ"));
    check_rect(f.el("save-0"), layer(kLoad, "Save record / Автосохранение"));
    check_rect(f.el("save-1"), layer(kLoad, "Save record / Перед переговорами"));
    check_rect(f.el("save-2"), layer(kLoad, "Save record / История другого времени"));
    check_text(child_of_class(f.el("save-0"), "save-name"), text_layer(kLoad, "Автосохранение"));
    check_rect(child_of_class(f.el("save-0"), "save-badge"), layer(kLoad, "Badge / Совместимо"));
    check_rect(child_of_class(f.el("save-2"), "save-badge"), layer(kLoad, "Badge / Не загрузить"));
    check_rect(child_of_class(f.el("save-0"), "save-icon"), layer(kLoad, "Icon / scroll"));
    check_text(child_of_class(f.el("save-0"), "save-line"), text_layer(kLoad, "Голицыны · 1 января 1703"));
    check_text(child_of_class(f.el("save-0"), "save-sub"), text_layer(kLoad, "Последнее сохранение"));
    check_text(child_of_class(f.el("save-2"), "save-sub"),
               text_layer(kLoad, "Недоступно для этой версии игры"));
    check_rect(f.el("save-summary"), layer(kLoad, "Selected save summary"));
    check_rect(f.el("save-preview"), layer(kLoad, "Save preview / world map"));
    check_text(f.el("save-house"), text_layer(kLoad, "Род Голицыных"));
    check_text(f.el("save-date"), text_layer(kLoad, "1 января 1703 · Зима"));
    check_text(f.el("save-keys"), layer(kLoad, "Глава рода Вид сохранения Казна"));
    check_text(f.el("save-values"), layer(kLoad, "Михаил Голицын Автосохранение 1 248 ₽"));
    check_rect(f.el("btn-main"), layer(kLoad, "Button / Главное меню"));
    check_rect(f.el("btn-load"), layer(kLoad, "Button / Загрузить историю"));
    CHECK(f.el("save-0")->IsClassSet("selected"));
    CHECK(f.el("save-2")->IsClassSet("bad"));
}

TEST_CASE("menu: selecting the manual save changes the summary (board 'выбрано ручное сохранение')") {
    if (!fonts_ok()) {
        return;
    }
    Fixture f;
    f.open();
    f.click("mi-load");
    f.key(Rml::Input::KI_DOWN);
    CHECK(f.menu->highlighted() == 1);
    CHECK(f.focused() == "save-1");
    CHECK(f.el("save-1")->IsClassSet("selected"));
    CHECK_FALSE(f.el("save-0")->IsClassSet("selected"));
    check_rect(f.el("save-1"), layer(kLoadManual, "Save record / Перед переговорами"));
    Rml::Element* values = f.el("save-values");
    REQUIRE(values->GetNumChildren() == 3);
    CHECK(text_of(values->GetChild(1)) == "Ручное сохранение");
    f.click("save-0");
    CHECK(f.menu->highlighted() == 0);
    CHECK(text_of(f.el("save-values")->GetChild(1)) == "Автосохранение");
}

TEST_CASE("menu: loading a save calls the callback; a save of a newer version opens the dialog") {
    if (!fonts_ok()) {
        return;
    }
    Fixture f;
    f.open();
    f.click("mi-load");
    f.click("save-1");
    f.click("btn-load");
    REQUIRE(f.log.size() == 1);
    CHECK(f.log[0] == "load:manual-1");
    CHECK(f.menu->screen() == MenuScreen::World);  // the restore of the save is shown (boards of part 2)
    f.key(Rml::Input::KI_ESCAPE);                  // "Отмена": back to the list
    REQUIRE(f.menu->screen() == MenuScreen::Load);
    CHECK(f.log.size() == 2);
    f.log.clear();

    f.click("save-2");
    f.click("btn-load");
    CHECK(f.log.empty());  // nothing was loaded
    REQUIRE(f.menu->dialog() == MenuDialog::SaveUnavailable);
    CHECK(f.focused() == "dlg-back");
    Rml::Element* dialog = f.el("dialog-unavailable");
    CHECK(std::abs(dialog->GetOffsetWidth() - 700) <= 1);
    CHECK(std::abs(dialog->GetOffsetHeight() - 394) <= 1);
    check_inside(f.el("dlg-back"), dialog, layer(kUnavailable, "Button / К сохранениям"));
    check_text_inside(
        child_of_class(dialog, "dialog-title"), dialog, text_layer(kUnavailable, "Сохранение недоступно"));
    f.key(Rml::Input::KI_ESCAPE);  // closes the dialog, stays on the list
    CHECK(f.menu->dialog() == MenuDialog::None);
    CHECK(f.menu->screen() == MenuScreen::Load);

    f.click("save-2");
    f.click("btn-load");
    REQUIRE(f.menu->dialog() == MenuDialog::SaveUnavailable);
    f.click("dlg-back");
    CHECK(f.menu->dialog() == MenuDialog::None);
    CHECK(f.menu->screen() == MenuScreen::Load);
}

TEST_CASE("menu: the empty list of saves sits where board 08 puts it") {
    if (!fonts_ok()) {
        return;
    }
    Fixture f({1920, 1080}, false);
    f.open(MenuScreen::Load);
    check_text(f.el("page-title"), text_layer(kLoadEmpty, "Сохранённые истории"));
    check_text(f.el("page-caption"), text_layer(kLoadEmpty, "СОХРАНЁННЫЕ ИСТОРИИ"));
    check_rect(f.el("load-empty-icon"), layer(kLoadEmpty, "Icon / scroll"));
    check_text(f.el("load-empty-title"), text_layer(kLoadEmpty, "Здесь пока пусто"));
    check_rect(f.el("btn-new"), layer(kLoadEmpty, "Button / Новая игра"));
    check_rect(f.el("btn-main"), layer(kLoadEmpty, "Button / Главное меню"));
    CHECK_FALSE(f.has("btn-load"));
    CHECK_FALSE(f.has("save-summary"));
    CHECK(f.focused() == "btn-new");
    f.click("btn-new");  // "Новая игра": the screen of the choice of the house
    CHECK(f.log.empty());
    REQUIRE(f.menu->screen() == MenuScreen::NewGame);
    CHECK(f.menu->depth() == 2);
}

TEST_CASE("menu: the overview page lists the eight cards of its board and opens the screens that exist") {
    if (!fonts_ok()) {
        return;
    }
    Fixture f({1920, 1080}, false);
    f.open(MenuScreen::Overview);
    REQUIRE(f.menu->screen() == MenuScreen::Overview);
    check_text(f.el("ov-title"), text_layer(kOverview, "До первой страницы летописи"));
    check_text(f.el("ov-eyebrow"), text_layer(kOverview, "PATRIARCH / ВХОД В ИГРУ"));
    check_text(f.el("ov-discuss"), text_layer(kOverview, "ДЛЯ ОБСУЖДЕНИЯ"));
    const char* names[8] = {"Menu index / Первый запуск",
                            "Menu index / Продолжить",
                            "Menu index / Новая игра",
                            "Menu index / Свой род",
                            "Menu index / Сохранения",
                            "Menu index / Пустой список",
                            "Menu index / Настройки",
                            "Menu index / Загрузка мира"};
    for (int i = 0; i < 8; ++i) {
        INFO("card " << i);
        check_rect(f.el(("card-" + std::to_string(i)).c_str()), layer(kOverview, names[i]));
    }
    CHECK_FALSE(f.el("card-6")->IsClassSet("is-disabled"));  // "Настройки" exist since part 3
    CHECK_FALSE(f.el("card-0")->IsClassSet("is-disabled"));
    CHECK(f.focused() == "card-0");

    f.click("card-6");
    CHECK(f.menu->screen() == MenuScreen::Settings);
    f.key(Rml::Input::KI_ESCAPE);
    CHECK(f.menu->screen() == MenuScreen::Overview);

    f.click("card-0");  // the first run: no saves, "Новая игра" is the primary entry
    REQUIRE(f.menu->screen() == MenuScreen::Main);
    CHECK_FALSE(f.menu->has_save_variant());
    CHECK(f.has("mi-new"));
    f.key(Rml::Input::KI_ESCAPE);
    REQUIRE(f.menu->screen() == MenuScreen::Overview);

    f.click("card-1");  // "Продолжить": the demo saves of the mockup stand in for the real ones
    REQUIRE(f.menu->screen() == MenuScreen::Main);
    CHECK(f.menu->has_save_variant());
    f.key(Rml::Input::KI_ESCAPE);
    f.click("card-4");
    REQUIRE(f.menu->screen() == MenuScreen::Load);
    CHECK(f.has("save-2"));
    f.key(Rml::Input::KI_ESCAPE);
    f.click("card-5");
    REQUIRE(f.menu->screen() == MenuScreen::Load);
    CHECK(f.has("load-empty-title"));
    f.key(Rml::Input::KI_ESCAPE);
    CHECK(f.menu->screen() == MenuScreen::Overview);
    CHECK(f.menu->snapshot().saves.empty());  // the previews never touched the real data
}

TEST_CASE("menu: closing hides the document; the menu fits the window at 1280x720 and 21:9") {
    if (!fonts_ok()) {
        return;
    }
    {
        Fixture f;
        f.open();
        CHECK(f.menu->visible());
        f.menu->close();
        f.settle();
        CHECK_FALSE(f.menu->visible());
        CHECK_FALSE(f.menu->document()->IsVisible());
    }
    {
        Fixture f({1280, 720});
        f.open();
        const float scale = design_ui_scale({1280, 720});
        Rml::Element* item = f.el("mi-new");
        CHECK(std::abs(item->GetOffsetWidth() - 548 * scale) <= 1.5);
        CHECK(item->GetAbsoluteLeft() >= 0);
        CHECK(item->GetAbsoluteLeft() + item->GetOffsetWidth() <= 1280);
        Rml::Element* strip = f.el("menu-contrast");
        CHECK(std::abs(strip->GetOffsetWidth() - 910 * scale) <= 1.5);
        CHECK(strip->GetAbsoluteLeft() >= 0);
    }
    {
        Fixture f({2560, 1080});
        f.open();
        // The stage is centred: the items keep their distance from the left edge of the 1920 canvas.
        Rml::Element* item = f.el("mi-new");
        CHECK(std::abs(item->GetAbsoluteLeft() - (320 + 146)) <= 1.5);
        Rml::Element* contrast = f.el("menu-contrast");
        CHECK(std::abs(contrast->GetAbsoluteLeft() - 320) <= 1.5);
        CHECK(std::abs(contrast->GetOffsetWidth() - 910) <= 1.5);
        Rml::Element* extension =
            f.el("menu-contrast-left");  // the strip goes on to the left edge of the window
        CHECK(extension->GetAbsoluteLeft() + extension->GetOffsetWidth() <= 320 + 1.5);
    }
}

// ---- part 2: "Новая игра", "Свой род", "Цель партии", the world screen
// --------------------------------------

TEST_CASE("menu: the placeholder new game is three houses, five goals and the founder of board 04") {
    const pat::ui::MenuNewGame data = pat::ui::make_placeholder_new_game(pat::test::ui_localizer());
    CHECK(data.placeholder);
    REQUIRE(data.houses.size() == 3);
    CHECK(data.houses[0].name_gen == "Голицыных");
    CHECK(data.houses[1].name == "Шереметевы");
    REQUIRE(data.goals.size() == 5);
    CHECK(data.goals[data.default_goal].id == "dynasty");
    CHECK(data.custom_defaults.surname == "Соколин");
    CHECK(data.custom_defaults.house_name == "Соколиных");
    CHECK(data.looks.size() == 2);
    CHECK(data.faiths.size() == 2);
}

namespace {

struct FlowBoards {
    static constexpr const char* kNew = "МЕНЮ / 03 Новая игра / выбор рода";
    static constexpr const char* kCustom = "МЕНЮ / 04 Новая игра / собственный род";
    static constexpr const char* kGoal = "МЕНЮ / 05 Новая игра / цель партии";
    static constexpr const char* kGoalSandbox = "МЕНЮ / Цель / Свободная история";
    static constexpr const char* kGoalThrone = "МЕНЮ / Цель / Занять престол";
    static constexpr const char* kGoalOffice = "МЕНЮ / Цель / Высшая должность";
    static constexpr const char* kGoalSurvive = "МЕНЮ / Цель / Пережить кризис";
    static constexpr const char* kGoalOwn = "МЕНЮ / 06 Цель партии / свой род";
    static constexpr const char* kPrepare = "МЕНЮ / Подготовка мира / готовый род";
    static constexpr const char* kPrepareOwn = "МЕНЮ / Подготовка мира / свой род";
    static constexpr const char* kRestore = "МЕНЮ / Восстановление сохранения";
    static constexpr const char* kReady = "МЕНЮ / Мир готов / готовый род";
    static constexpr const char* kReadyOwn = "МЕНЮ / Мир готов / свой род";
    static constexpr const char* kSaved = "МЕНЮ / Сохранение готово";
};

}  // namespace

TEST_CASE("menu: 'Новая игра' sits where board 03 puts it") {
    if (!fonts_ok()) {
        return;
    }
    using B = FlowBoards;
    Fixture f;
    f.open();
    f.click("mi-new");
    REQUIRE(f.menu->screen() == MenuScreen::NewGame);
    CHECK(f.menu->depth() == 2);
    CHECK(f.focused() == "house-0");
    CHECK(f.menu->house_selected() == 0);
    check_rect(f.el("panel"), layer(B::kNew, "Front panel"));
    check_text(f.el("page-title"), text_layer(B::kNew, "С чьего имени начнётся история?"));
    check_text(f.el("page-caption"), text_layer(B::kNew, "С ЧЬЕГО ИМЕНИ НАЧНЁТСЯ ИСТОРИЯ?"));
    check_text(f.el("page-subtitle"),
               layer(B::kNew, "1  Род    →    2  Цель партии    →    3  Начало · 1 января 1703, юлиан"));
    check_rect(f.el("house-0"), layer(B::kNew, "Family choice / Голицыны"));
    check_rect(f.el("house-1"), layer(B::kNew, "Family choice / Шереметевы"));
    check_rect(f.el("house-2"), layer(B::kNew, "Family choice / Долгоруковы"));
    check_rect(f.el("house-3"), layer(B::kNew, "Family choice / Основать свой род"));
    check_rect(child_of_class(f.el("house-0"), "house-badge"), layer(B::kNew, "Badge / ВЫБРАНО"));
    check_rect(child_of_class(f.el("house-1"), "house-badge"), layer(B::kNew, "Badge / ЗНАТНЫЙ РОД"));
    check_rect(child_of_class(f.el("house-3"), "house-badge"), layer(B::kNew, "Badge / СОБСТВЕННЫЙ РОД"));
    check_text(child_of_class(f.el("house-0"), "house-name"), text_layer(B::kNew, "Голицыны"));
    check_text(child_of_class(f.el("house-0"), "house-line"),
               text_layer(B::kNew, "Продолжите историю старого рода."));
    check_text(child_of_class(f.el("house-3"), "house-name"), text_layer(B::kNew, "Основать свой род"));
    check_text(child_of_class(f.el("house-3"), "house-note"), text_layer(B::kNew, "Имя · Основатель · Герб"));
    check_rect(f.el("house-summary"), layer(B::kNew, "Selected family summary"));
    check_rect(f.el("hsum-portrait"), layer(B::kNew, "Portrait / Михаил Голицын"));
    check_text(f.el("hsum-name"), text_layer(B::kNew, "Род Голицыных"));
    check_text(f.el("hsum-head"), text_layer(B::kNew, "Михаил Голицын"));
    check_text(f.el("hsum-head-line"), text_layer(B::kNew, "Глава рода · 47 лет"));
    check_rect(f.el("hsum-members"), layer(B::kNew, "Badge / 12 членов рода"));
    check_text(f.el("hsum-value-0"), text_layer(B::kNew, "1 248 ₽"));
    check_text(f.el("hsum-value-1"), text_layer(B::kNew, "68 / 100"));
    check_text(f.el("hsum-key-2"), text_layer(B::kNew, "Наследник"));
    check_text(f.el("hsum-value-2"), text_layer(B::kNew, "Иван Голицын"));
    check_text(f.el("hsum-blurb"),
               layer(B::kNew, "Сохраните имя рода, распоряжайтесь владениями и решайте судьбы семьи."));
    check_rect(f.el("btn-main"), layer(B::kNew, "Button / Главное меню"));
    check_rect(f.el("btn-next"), layer(B::kNew, "Button / Далее: цель партии"));
    CHECK(text_of(f.el("hsum-head-line")) == "Глава рода · 47 лет");
    CHECK(text_of(child_of_class(f.el("house-0"), "house-badge")) == "ВЫБРАНО");
    CHECK(text_of(child_of_class(f.el("house-1"), "house-badge")) == "ЗНАТНЫЙ РОД");
    CHECK(f.el("house-0")->IsClassSet("selected"));
    CHECK(f.el("btn-next")->IsClassSet("is-disabled") == false);
}

TEST_CASE(
    "menu: choosing a house changes the summary; arrows move the choice, the last card opens the form") {
    if (!fonts_ok()) {
        return;
    }
    Fixture f;
    f.open();
    f.click("mi-new");
    f.click("house-1");
    CHECK(f.menu->house_selected() == 1);
    CHECK(f.el("house-1")->IsClassSet("selected"));
    CHECK_FALSE(f.el("house-0")->IsClassSet("selected"));
    CHECK(text_of(f.el("hsum-name")) == "Род Шереметевых");
    CHECK(text_of(f.el("hsum-head")) == "Фёдор Шереметев");
    CHECK(text_of(child_of_class(f.el("house-1"), "house-badge")) == "ВЫБРАНО");
    CHECK(text_of(child_of_class(f.el("house-0"), "house-badge")) == "ЗНАТНЫЙ РОД");

    f.key(Rml::Input::KI_DOWN);  // two columns: down is two cards on, the "Основать свой род" card
    CHECK(f.focused() == "house-3");
    CHECK(f.menu->house_selected() == 1);  // that card is opened by a press, it is not a chosen house
    f.key(Rml::Input::KI_LEFT);
    CHECK(f.focused() == "house-2");
    CHECK(f.menu->house_selected() == 2);
    CHECK(text_of(f.el("hsum-name")) == "Род Долгоруковых");
    f.key(Rml::Input::KI_UP);
    CHECK(f.menu->house_selected() == 0);
    f.key(Rml::Input::KI_UP);  // clamped at the top
    CHECK(f.focused() == "house-0");

    f.click("house-3");
    REQUIRE(f.menu->screen() == MenuScreen::Custom);
    CHECK(f.menu->depth() == 3);
    f.key(Rml::Input::KI_ESCAPE);
    CHECK(f.menu->screen() == MenuScreen::NewGame);
    CHECK(f.menu->depth() == 2);
}

TEST_CASE("menu: 'Свой род' sits where board 04 puts it; the founder is edited live") {
    if (!fonts_ok()) {
        return;
    }
    using B = FlowBoards;
    Fixture f;
    f.open();
    f.click("mi-new");
    f.click("house-3");
    REQUIRE(f.menu->screen() == MenuScreen::Custom);
    CHECK(f.focused() == "in-surname");
    check_text(f.el("page-title"), text_layer(B::kCustom, "Основать свой род"));
    check_text(f.el("page-caption"), text_layer(B::kCustom, "ОСНОВАТЬ СВОЙ РОД"));
    check_text(f.el("cl-surname"), text_layer(B::kCustom, "Фамилия основателя"));
    check_rect(f.el("in-surname"), layer(B::kCustom, "Input / Фамилия основателя"));
    check_text(f.el("cl-house"), text_layer(B::kCustom, "Название рода"));
    check_rect(f.el("in-house"), layer(B::kCustom, "Input / Название рода"));
    check_rect(f.el("in-name"), layer(B::kCustom, "Input / Имя"));
    check_rect(f.el("in-patronymic"), layer(B::kCustom, "Input / Отчество"));
    check_text(f.el("cl-motto"), text_layer(B::kCustom, "Девиз рода"));
    check_rect(f.el("in-motto"), layer(B::kCustom, "Input / Девиз рода"));
    check_text(f.el("cl-look"), text_layer(B::kCustom, "Образ основателя"));
    check_rect(f.el("sel-look"), layer(B::kCustom, "Control / Мужчина · 22 года"));
    check_text(f.el("cl-faith"), text_layer(B::kCustom, "Вероисповедание"));
    check_rect(f.el("sel-faith"), layer(B::kCustom, "Control / Православие"));
    check_rect(f.el("custom-summary"), layer(B::kCustom, "Founder and heraldry"));
    check_rect(f.el("csum-portrait"), layer(B::kCustom, "Founder portrait / fictional"));
    check_text(f.el("csum-name"), layer(B::kCustom, "Иван Михайлович Соколин"));
    check_text(f.el("csum-role"), text_layer(B::kCustom, "Глава нового рода"));
    check_rect(f.el("csum-badge"), layer(B::kCustom, "Badge / ОСНОВАТЕЛЬ"));
    check_text(f.el("csum-caption"), text_layer(B::kCustom, "ГЕРБ РОДА"));
    check_text(f.el("csum-heraldry"), text_layer(B::kCustom, "Зелень и старое золото"));
    check_text(f.el("csum-motto"), text_layer(B::kCustom, "«Делом крепнет род»"));
    check_rect(f.el("csum-green"), layer(B::kCustom, "Heraldic colour / green"));
    check_rect(f.el("csum-gold"), layer(B::kCustom, "Heraldic colour / gold"));
    check_text(f.el("csum-note"), text_layer(B::kCustom, "Начальные условия задаёт сценарий."));
    check_rect(f.el("btn-main"), layer(B::kCustom, "Button / К выбору рода"));
    check_rect(f.el("btn-next"), layer(B::kCustom, "Button / Далее: цель партии"));
    CHECK(text_of(f.el("btn-main")->GetChild(0)) == "К выбору рода");

    // Typing into the focused input changes the summary at once.
    f.ui->context()->ProcessTextInput(Rml::String("ж"));
    f.settle();
    CHECK(text_of(f.el("csum-name")).find("ж") != std::string::npos);

    // The drop-downs take the next option on a press.
    CHECK(text_of(child_of_class(f.el("sel-look"), "cselect-text")) == "Мужчина · 22 года");
    f.click("sel-look");
    CHECK(text_of(child_of_class(f.el("sel-look"), "cselect-text")) == "Мужчина · 35 лет");
    f.click("sel-look");
    CHECK(text_of(child_of_class(f.el("sel-look"), "cselect-text")) == "Мужчина · 22 года");
    f.menu->press_cycle("faith");
    f.settle();
    CHECK(text_of(child_of_class(f.el("sel-faith"), "cselect-text")) == "Старообрядчество");
    CHECK(f.menu->current_start().custom_house.faith == 1);
}

TEST_CASE("menu: the own house needs a surname, a name of the house and a name before 'Далее'") {
    if (!fonts_ok()) {
        return;
    }
    Fixture f;
    f.open();
    f.click("mi-new");
    f.click("house-3");
    CHECK(f.menu->next_enabled());
    CHECK_FALSE(f.has("cust-reason"));
    f.menu->set_field("surname", "");
    f.settle();
    CHECK_FALSE(f.menu->next_enabled());
    CHECK(f.has("cust-reason"));
    CHECK(f.el("btn-next")->IsClassSet("is-disabled"));
    f.click("btn-next");
    CHECK(f.menu->screen() == MenuScreen::Custom);  // a disabled control ignores the press
    f.menu->set_field("surname", "Рюмин");
    f.settle();
    CHECK(f.menu->next_enabled());
    CHECK(text_of(f.el("csum-name")).find("Рюмин") != std::string::npos);
    f.click("btn-next");
    REQUIRE(f.menu->screen() == MenuScreen::Goal);
    CHECK(f.menu->current_start().custom);
}

TEST_CASE("menu: 'Цель партии' sits where board 05 puts it") {
    if (!fonts_ok()) {
        return;
    }
    using B = FlowBoards;
    Fixture f;
    f.open();
    f.click("mi-new");
    f.click("btn-next");
    REQUIRE(f.menu->screen() == MenuScreen::Goal);
    CHECK(f.menu->goal_selected() == 2);  // "Величие рода" is chosen at the start
    CHECK(f.focused() == "goal-2");
    check_text(f.el("page-title"), text_layer(B::kGoal, "Каким запомнят ваш род?"));
    check_text(f.el("page-caption"), text_layer(B::kGoal, "КАКИМ ЗАПОМНЯТ ВАШ РОД?"));
    check_rect(f.el("goal-0"), layer(B::kGoal, "Goal choice / survive"));
    check_rect(f.el("goal-1"), layer(B::kGoal, "Goal choice / office"));
    check_rect(f.el("goal-2"), layer(B::kGoal, "Goal choice / dynasty"));
    check_rect(f.el("goal-3"), layer(B::kGoal, "Goal choice / throne"));
    check_rect(f.el("goal-4"), layer(B::kGoal, "Goal choice / sandbox"));
    check_text(child_of_class(f.el("goal-0"), "goal-title"), text_layer(B::kGoal, "Пережить кризис"));
    check_text(child_of_class(f.el("goal-0"), "goal-line"),
               text_layer(B::kGoal, "Сохраните род в эпоху потрясений."));
    check_text(child_of_class(f.el("goal-4"), "goal-title"), text_layer(B::kGoal, "Свободная история"));
    check_text(child_of_class(f.el("goal-4"), "goal-line"),
               text_layer(B::kGoal, "Ведите род без заданной цели."));
    check_rect(child_of_class(f.el("goal-2"), "goal-badge"), layer(B::kGoal, "Badge / ВЫБРАНО"));
    check_rect(f.el("goal-summary"), layer(B::kGoal, "Launch summary"));
    check_text(f.el("gsum-caption"), text_layer(B::kGoal, "НАЧАЛО ПАРТИИ"));
    check_text(f.el("gsum-house"), text_layer(B::kGoal, "Род Голицыных"));
    check_text(f.el("gsum-head"), text_layer(B::kGoal, "Глава рода · Михаил Голицын"));
    check_text(f.el("gsum-keys"), layer(B::kGoal, "Начальная дата Цель Режим"));
    check_text(f.el("gsum-values"), layer(B::kGoal, "1 января 1703 Величие рода Одиночная игра"));
    check_text(f.el("gsum-blurb"),
               layer(B::kGoal, "Ваша история не закончится со смертью одного человека. Поражение — вым"));
    check_text(f.el("gsum-paused"), text_layer(B::kGoal, "Мир начнётся на паузе."));
    check_rect(f.el("btn-main"), layer(B::kGoal, "Button / Назад"));
    check_rect(f.el("btn-next"), layer(B::kGoal, "Button / Начать историю"));
    CHECK(f.el("goal-2")->IsClassSet("selected"));
    CHECK(text_of(f.el("gsum-values")->GetChild(1)) == "Величие рода");

    // Every other goal moves the badge and the summary exactly as its own board does.
    f.click("goal-4");
    CHECK(f.menu->goal_selected() == 4);
    check_rect(child_of_class(f.el("goal-4"), "goal-badge"), layer(B::kGoalSandbox, "Badge / ВЫБРАНО"));
    check_text(f.el("gsum-values"), layer(B::kGoalSandbox, "1 января 1703 Величие рода Одиночная игра"));
    CHECK(text_of(f.el("gsum-values")->GetChild(1)) == "Свободная история");
    f.click("goal-3");
    check_rect(child_of_class(f.el("goal-3"), "goal-badge"), layer(B::kGoalThrone, "Badge / ВЫБРАНО"));
    CHECK(text_of(f.el("gsum-values")->GetChild(1)) == "Занять престол");
    f.click("goal-1");
    check_rect(child_of_class(f.el("goal-1"), "goal-badge"), layer(B::kGoalOffice, "Badge / ВЫБРАНО"));
    f.click("goal-0");
    check_rect(child_of_class(f.el("goal-0"), "goal-badge"), layer(B::kGoalSurvive, "Badge / ВЫБРАНО"));
    CHECK(text_of(f.el("gsum-values")->GetChild(1)) == "Пережить кризис");
    CHECK(visible_count(f.menu->document(), "goal-badge") == 1);  // exactly one badge is laid out
    CHECK(f.el("goal-0")->IsClassSet("selected"));
    CHECK_FALSE(f.el("goal-2")->IsClassSet("selected"));

    // Arrows move the choice like they do in the grid of houses.
    f.key(Rml::Input::KI_RIGHT);
    CHECK(f.menu->goal_selected() == 1);
    f.key(Rml::Input::KI_DOWN);
    CHECK(f.menu->goal_selected() == 3);
    f.key(Rml::Input::KI_DOWN);
    CHECK(f.menu->goal_selected() == 4);
    CHECK(f.focused() == "goal-4");

    f.key(Rml::Input::KI_ESCAPE);  // back to the choice of the house
    CHECK(f.menu->screen() == MenuScreen::NewGame);
}

TEST_CASE("menu: the goal of the own house shows the founder (board 06)") {
    if (!fonts_ok()) {
        return;
    }
    using B = FlowBoards;
    Fixture f;
    f.open();
    f.click("mi-new");
    f.click("house-3");
    f.click("btn-next");
    REQUIRE(f.menu->screen() == MenuScreen::Goal);
    check_text(f.el("gsum-house"), text_layer(B::kGoalOwn, "Род Соколиных"));
    check_text(f.el("gsum-head"), text_layer(B::kGoalOwn, "Основатель · Иван Соколин"));
    check_text(f.el("gsum-values"), layer(B::kGoalOwn, "1 января 1703 Величие рода Одиночная игра"));
    CHECK(text_of(f.el("gsum-values")->GetChild(3)) == "420 ₽");
    f.key(Rml::Input::KI_ESCAPE);  // back to the form, not to the choice of the house
    CHECK(f.menu->screen() == MenuScreen::Custom);
    CHECK(f.menu->depth() == 3);
}

TEST_CASE("menu: 'Начать историю' calls the callback and shows the preparation of the world (boards "
          "'Подготовка мира')") {
    if (!fonts_ok()) {
        return;
    }
    using B = FlowBoards;
    Fixture f;
    f.open();
    f.click("mi-new");
    f.click("house-2");
    f.click("btn-next");
    f.click("goal-3");
    f.click("btn-next");
    REQUIRE(f.menu->screen() == MenuScreen::World);
    REQUIRE(f.starts.size() == 1);
    CHECK_FALSE(f.starts[0].custom);
    CHECK(f.starts[0].house_id == "dolgorukov");
    CHECK(f.starts[0].goal_id == "throne");
    CHECK(f.menu->progress() == 0);
    CHECK_FALSE(f.menu->world_ready());
    CHECK(f.focused() == "btn-cancel");
    CHECK(text_of(f.el("wd-house")) == "Род Долгоруковых · 1 января 1703");

    f.menu->set_progress(68, 1);  // the host reports the progress; the board shows 68 %
    f.settle();
    check_text(f.el("page-caption"), text_layer(B::kPrepare, "ПОДГОТОВКА ИСТОРИИ"));
    check_text(f.el("wd-title"), text_layer(B::kPrepare, "Готовим родовую летопись"));
    check_text(f.el("wd-s0"), text_layer(B::kPrepare, "Карта загружена"));
    check_text(f.el("wd-s1"), text_layer(B::kPrepare, "Создаются семьи и персонажи"));
    check_text(f.el("wd-s2"), text_layer(B::kPrepare, "Подготавливаются дела рода"));
    check_rect(f.el("wd-track"), layer(B::kPrepare, "Loading progress track"));
    check_rect(f.el("wd-bar"), layer(B::kPrepare, "Loading progress value"));
    check_text(f.el("wd-percent"), text_layer(B::kPrepare, "68%"));
    check_text(f.el("wd-hint"),
               layer(B::kPrepare, "Подсказка: нажмите на показатель, чтобы увидеть, из чего он складывает"));
    check_rect(f.el("btn-cancel"), layer(B::kPrepare, "Button / Отмена"));
    CHECK(f.el("wd-s1")->IsClassSet("active"));
    CHECK_FALSE(f.el("wd-s0")->IsClassSet("active"));
    CHECK_FALSE(f.has("btn-enter"));

    f.menu->set_progress(100, 2);  // "Мир готов"
    f.settle();
    REQUIRE(f.menu->world_ready());
    CHECK(f.focused() == "btn-enter");
    check_text(f.el("page-caption"), text_layer(B::kReady, "МИР ГОТОВ"));
    check_text(f.el("wd-title"), text_layer(B::kReady, "История начинается"));
    check_text(f.el("wd-house"), text_layer(B::kReady, "Род Голицыных · 1 января 1703"));
    check_text(f.el("wd-ready-text"), layer(B::kReady, "Распоряжайтесь настоящим. Берегите будущее рода."));
    check_rect(f.el("wd-bar"), layer(B::kReady, "Complete loading track"));
    check_text(f.el("wd-percent"), text_layer(B::kReady, "100%"));
    check_text(f.el("wd-note"), text_layer(B::kReady, "Время остановлено. Первые дела ждут вашего решения."));
    check_rect(f.el("btn-enter"), layer(B::kReady, "Button / Вступить в игру"));
    CHECK_FALSE(f.has("btn-cancel"));
    CHECK_FALSE(f.has("wd-s0"));
    CHECK(f.menu->press_back() == pat::ui::MenuBack::Held);  // only the button leaves the ready world
    CHECK(f.menu->screen() == MenuScreen::World);
    f.click("btn-enter");
    REQUIRE(f.log.size() == 1);
    CHECK(f.log[0] == "enter");
}

TEST_CASE("menu: the preparation can be cancelled and the own house is named on its boards") {
    if (!fonts_ok()) {
        return;
    }
    using B = FlowBoards;
    Fixture f;
    f.open();
    f.click("mi-new");
    f.click("house-3");
    f.click("btn-next");
    f.click("btn-next");
    REQUIRE(f.menu->screen() == MenuScreen::World);
    REQUIRE(f.starts.size() == 1);
    CHECK(f.starts[0].custom);
    CHECK(f.starts[0].custom_house.surname == "Соколин");
    CHECK(f.starts[0].goal_id == "dynasty");
    f.menu->set_progress(68, 1);
    f.settle();
    check_text(f.el("wd-house"), text_layer(B::kPrepareOwn, "Род Соколиных · 1 января 1703"));
    f.click("btn-cancel");  // back to the goal screen
    CHECK(f.menu->screen() == MenuScreen::Goal);
    REQUIRE(f.log.size() == 1);
    CHECK(f.log[0] == "cancel");

    f.click("btn-next");
    f.menu->finish_preparation();
    f.settle();
    check_text(f.el("wd-house"), text_layer(B::kReadyOwn, "Род Соколиных · 1 января 1703"));
}

TEST_CASE("menu: loading a save shows the restore of the world (boards 'Восстановление сохранения', "
          "'Сохранение готово')") {
    if (!fonts_ok()) {
        return;
    }
    using B = FlowBoards;
    Fixture f;
    f.open();
    f.click("mi-load");
    f.click("save-1");
    f.click("btn-load");
    REQUIRE(f.menu->screen() == MenuScreen::World);
    REQUIRE(f.log.size() == 1);
    CHECK(f.log[0] == "load:manual-1");
    f.menu->set_progress(68, 1);
    f.settle();
    check_text(f.el("wd-title"), text_layer(B::kRestore, "Возвращаемся к вашей истории"));
    check_text(f.el("wd-house"), text_layer(B::kRestore, "Род Голицыных · 1 января 1703"));
    check_text(f.el("wd-s0"), text_layer(B::kRestore, "Сохранение прочитано"));
    check_text(f.el("wd-s1"), text_layer(B::kRestore, "Восстанавливается мир"));
    check_text(f.el("wd-s2"), text_layer(B::kRestore, "Подготавливаются окна"));
    check_rect(f.el("wd-track"), layer(B::kRestore, "Loading progress track"));
    check_rect(f.el("wd-bar"), layer(B::kRestore, "Loading progress value"));

    f.menu->finish_preparation();
    f.settle();
    check_text(f.el("wd-title"), text_layer(B::kSaved, "История продолжается"));
    check_rect(f.el("btn-enter"), layer(B::kSaved, "Button / Вступить в игру"));
    CHECK(text_of(f.el("btn-enter")->GetChild(1)) == "Продолжить игру");
    f.key(Rml::Input::KI_RETURN);  // the button holds the focus
    REQUIRE(f.log.size() == 2);
    CHECK(f.log[1] == "enter");
}

TEST_CASE("menu: 'Продолжить' loads the newest save and the cancel goes back to the main menu") {
    if (!fonts_ok()) {
        return;
    }
    Fixture f;
    f.open();
    f.key(Rml::Input::KI_RETURN);  // "Продолжить" holds the focus
    REQUIRE(f.menu->screen() == MenuScreen::World);
    REQUIRE(f.log.size() == 1);
    CHECK(f.log[0] == "continue");
    CHECK(text_of(f.el("wd-title")) == "Возвращаемся к вашей истории");
    f.key(Rml::Input::KI_ESCAPE);
    CHECK(f.menu->screen() == MenuScreen::Main);
    REQUIRE(f.log.size() == 2);
    CHECK(f.log[1] == "cancel");
}

TEST_CASE("menu: the overview opens the screens of part 2 without calling a callback") {
    if (!fonts_ok()) {
        return;
    }
    Fixture f({1920, 1080}, false);
    f.open(MenuScreen::Overview);
    CHECK_FALSE(f.el("card-2")->IsClassSet("is-disabled"));
    CHECK_FALSE(f.el("card-3")->IsClassSet("is-disabled"));
    CHECK_FALSE(f.el("card-7")->IsClassSet("is-disabled"));
    CHECK_FALSE(f.el("card-6")->IsClassSet("is-disabled"));
    f.click("card-2");
    REQUIRE(f.menu->screen() == MenuScreen::NewGame);
    f.key(Rml::Input::KI_ESCAPE);
    REQUIRE(f.menu->screen() == MenuScreen::Overview);
    f.click("card-3");
    REQUIRE(f.menu->screen() == MenuScreen::Custom);
    f.key(Rml::Input::KI_ESCAPE);
    f.click("card-7");
    REQUIRE(f.menu->screen() == MenuScreen::World);
    CHECK(f.menu->progress() == 68);
    CHECK(text_of(f.el("wd-percent")) == "68%");
    f.key(Rml::Input::KI_ESCAPE);  // a preview is left without "Отмена" reaching the game
    CHECK(f.menu->screen() == MenuScreen::Overview);
    CHECK(f.log.empty());
}

TEST_CASE("menu: the start screens fit the window at 1280x720 and in 21:9") {
    if (!fonts_ok()) {
        return;
    }
    {
        Fixture f({1280, 720});
        f.open(MenuScreen::NewGame);
        const float scale = design_ui_scale({1280, 720});
        Rml::Element* card = f.el("house-3");
        CHECK(std::abs(card->GetOffsetWidth() - 510 * scale) <= 1.5);
        CHECK(card->GetAbsoluteLeft() >= 0);
        CHECK(card->GetAbsoluteTop() + card->GetOffsetHeight() <= 720);
        Rml::Element* next = f.el("btn-next");
        CHECK(next->GetAbsoluteLeft() + next->GetOffsetWidth() <= 1280);
        CHECK(std::abs(next->GetOffsetWidth() - 408 * scale) <= 1.5);
        f.menu->open_world(pat::ui::MenuWorldKind::NewGame, 68);
        f.settle();
        Rml::Element* track = f.el("wd-track");
        CHECK(std::abs(track->GetOffsetWidth() - 1660 * scale) <= 1.5);
        CHECK(track->GetAbsoluteLeft() + track->GetOffsetWidth() <= 1280);
        CHECK(std::abs(f.el("wd-bar")->GetOffsetWidth() - 1129 * scale) <= 1.5);
    }
    {
        Fixture f({2560, 1080});
        f.open(MenuScreen::Goal);
        // The stage is centred: the cards keep their distance from the left edge of the 1920 canvas.
        CHECK(std::abs(f.el("goal-0")->GetAbsoluteLeft() - (320 + 128)) <= 1.5);
        CHECK(std::abs(f.el("goal-summary")->GetAbsoluteLeft() - (320 + 1206)) <= 1.5);
        f.menu->open_world(pat::ui::MenuWorldKind::Restore, 100);
        f.settle();
        CHECK(std::abs(f.el("btn-enter")->GetAbsoluteLeft() - (320 + 1382)) <= 1.5);
    }
}

// ---- settings (part 3) ------------------------------------------------------------------------

namespace {

struct SettingsBoards {
    static constexpr const char* kVideo = "МЕНЮ / Настройки / Изображение";
    static constexpr const char* kInterface = "МЕНЮ / Настройки / Интерфейс";
    static constexpr const char* kChanged = "МЕНЮ / Настройки / есть изменения";
    static constexpr const char* kSaved = "МЕНЮ / Настройки / изменения сохранены";
    static constexpr const char* kAudio = "МЕНЮ / Настройки / Звук";
    static constexpr const char* kControls = "МЕНЮ / Настройки / Управление";
    static constexpr const char* kDlgSave = "МЕНЮ / Диалог / Сохранить изменения?";
    static constexpr const char* kDlgReset = "МЕНЮ / Диалог / Вернуть настройки по умолчанию?";
    static constexpr const char* kDlgRebind = "МЕНЮ / Диалог / Нажмите новую клавишу";
};

struct RowBoard {
    const char* row;      // "Setting row / …"
    const char* control;  // "Control / …", "Toggle / …", "Slider / …", "Key binding / …"
    const char* title;
    const char* hint;
    const char* value;  // the text on the control
};

using Rows = std::array<RowBoard, 6>;

const Rows kVideoRows = {{
    {"Setting row / Разрешение",
     "Control / 1920 × 1080",
     "Разрешение",
     "Размер изображения на экране",
     "1920 × 1080"},
    {"Setting row / Режим окна",
     "Control / Безрамочное окно",
     "Режим окна",
     "Поведение окна игры",
     "Безрамочное окно"},
    {"Setting row / Качество карты",
     "Control / Среднее",
     "Качество карты",
     "Общий уровень детализации ландшафта",
     "Среднее"},
    {"Setting row / Тени и освещение",
     "Control / Средние",
     "Тени и освещение",
     "Детализация теней на карте",
     "Средние"},
    {"Setting row / Частота кадров",
     "Control / 60 FPS",
     "Частота кадров",
     "Ограничение частоты отображения",
     "60 FPS"},
    {"Setting row / Вертикальная синхронизация",
     "Toggle / Вертикальная синхронизация",
     "Вертикальная синхронизация",
     "Уменьшает разрывы изображения",
     "Вкл."},
}};
const Rows kInterfaceRows = {{
    {"Setting row / Масштаб интерфейса",
     "Control / 100%",
     "Масштаб интерфейса",
     "Размер панелей и элементов управления",
     "100%"},
    {"Setting row / Язык интерфейса",
     "Control / Русский",
     "Язык интерфейса",
     "Язык текста и названий",
     "Русский"},
    {"Setting row / Размер текста",
     "Control / Стандартный",
     "Размер текста",
     "Читаемость описаний и сообщений",
     "Стандартный"},
    {"Setting row / Подсказки",
     "Control / Контекстные",
     "Подсказки",
     "Пояснения к значениям и действиям",
     "Контекстные"},
    {"Setting row / Пауза при важных событиях",
     "Toggle / Пауза при важных событиях",
     "Пауза при важных событиях",
     "Останавливать время перед важным выбором",
     "Вкл."},
    {"Setting row / Уменьшение анимации",
     "Toggle / Уменьшение анимации",
     "Уменьшение анимации",
     "Сократить декоративное движение",
     "Выкл."},
}};
const Rows kAudioRows = {{
    {"Setting row / Общая громкость", "Slider / Общая громкость", "Общая громкость", "Все звуки игры", "80%"},
    {"Setting row / Музыка", "Slider / Музыка", "Музыка", "Музыкальное сопровождение", "65%"},
    {"Setting row / Окружение", "Slider / Окружение", "Окружение", "Звуки мира и атмосфера", "70%"},
    {"Setting row / Интерфейс", "Slider / Интерфейс", "Интерфейс", "Сигналы действий и уведомлений", "75%"},
    {"Setting row / Устройство вывода",
     "Control / По умолчанию",
     "Устройство вывода",
     "Куда воспроизводится звук",
     "По умолчанию"},
    {"Setting row / Тишина вне окна",
     "Toggle / Тишина вне окна",
     "Тишина вне окна",
     "Приглушать игру, когда окно неактивно",
     "Вкл."},
}};
const Rows kControlRows = {{
    {"Setting row / Пауза / продолжить",
     "Key binding / Пауза / продолжить",
     "Пауза / продолжить",
     "Управление ходом времени",
     "Пробел"},
    {"Setting row / Перемещение камеры",
     "Key binding / Перемещение камеры",
     "Перемещение камеры",
     "Движение по карте",
     "W   A   S   D"},
    {"Setting row / Масштаб карты",
     "Key binding / Масштаб карты",
     "Масштаб карты",
     "Приближение и отдаление",
     "Колесо мыши"},
    {"Setting row / Выбрать объект",
     "Key binding / Выбрать объект",
     "Выбрать объект",
     "Провинция, персонаж или маркер",
     "ЛКМ"},
    {"Setting row / Назад / закрыть окно",
     "Key binding / Назад / закрыть окно",
     "Назад / закрыть окно",
     "Возврат из открытого окна",
     "ПКМ / Esc"},
    {"Setting row / Режим карты",
     "Key binding / Режим карты",
     "Режим карты",
     "Переключение представления карты",
     "M"},
}};

/// The frame of a tab: the categories, the panel, the six rows with their texts and controls, the buttons.
void check_settings_tab(Fixture& f,
                        const char* board,
                        const char* tab_name,
                        const char* content_name,
                        const Rows& rows,
                        const char* subtitle) {
    INFO("board " << board);
    REQUIRE(f.menu->screen() == MenuScreen::Settings);
    check_text(f.el("page-caption"), text_layer(board, "НАСТРОЙКИ"));
    {
        // The title of these boards is 63 high (the line box of 42 px at 1.5), the other boards draw 57.
        const Layer title = text_layer(board, "Настройки");
        REQUIRE(title.found);
        Rml::Element* e = f.el("page-title");
        CHECK(std::abs(e->GetAbsoluteLeft() - title.x) <= 1.0);
        CHECK(std::abs(e->GetAbsoluteTop() - title.y) <= 1.0);
        CHECK(std::abs(e->GetOffsetWidth() - title.w) <= 1.0);
    }
    check_text(f.el("page-subtitle"), text_layer(board, subtitle));
    check_rect(f.el("panel"), layer(board, "Front panel"));
    check_rect(f.el("set-cats"), layer(board, "Settings categories"));
    const char* cats[4] = {"Settings category / video",
                           "Settings category / interface",
                           "Settings category / audio",
                           "Settings category / controls"};
    for (int i = 0; i < 4; ++i) {
        const std::string id = "cat-" + std::to_string(i);
        check_rect(f.el(id.c_str()), layer(board, cats[i]));
    }
    check_text(f.el("set-note"), layer(board, "Изменения вступают в силу после применения."));
    check_rect(f.el("set-content"), layer(board, content_name));
    for (int i = 0; i < 6; ++i) {
        INFO("row " << i << " of " << tab_name);
        const RowBoard& r = rows[static_cast<std::size_t>(i)];
        Rml::Element* row = f.el(("row-" + std::to_string(i)).c_str());
        Rml::Element* ctl = f.el(("ctl-" + std::to_string(i)).c_str());
        check_rect(row, layer(board, r.row));
        Layer title = text_layer(board, r.title);
        if (std::string(r.title) == "Интерфейс") {
            title = Layer{true, 462, 601, 738, 31.5};  // the tab of that name comes first on the board
        }
        check_text(child_of_class(row, "set-title"), title);
        check_text(child_of_class(row, "set-hint"), text_layer(board, r.hint));
        check_rect(ctl, layer(board, r.control));
        CHECK(text_of(child_of_class(ctl, "ctl-text")) == r.value);
    }
    check_rect(f.el("btn-main"), layer(board, "Button / Главное меню"));
    check_rect(f.el("btn-defaults"), layer(board, "Button / По умолчанию"));
    check_rect(f.el("btn-apply"), layer(board, "Button / Применить"));
}

void mouse_at(Fixture& f, float x, float y) {
    f.ui->handle_event(event::MouseMove{x, y, 0, 0});
    f.settle();
}

}  // namespace

TEST_CASE("menu: 'Настройки' open from the main menu and Esc leaves them") {
    if (!fonts_ok()) {
        return;
    }
    Fixture f;
    f.open();
    f.click("mi-settings");
    REQUIRE(f.menu->screen() == MenuScreen::Settings);
    CHECK(f.menu->depth() == 2);
    CHECK(f.menu->settings_tab() == pat::ui::SettingsTab::Video);
    CHECK(f.focused() == "cat-0");
    CHECK_FALSE(f.menu->settings_dirty());
    CHECK_FALSE(f.has("set-msg"));
    f.key(Rml::Input::KI_ESCAPE);  // nothing changed: no question
    CHECK(f.menu->screen() == MenuScreen::Main);
    CHECK(f.menu->dialog() == MenuDialog::None);
    CHECK(f.log.empty());
}

TEST_CASE("menu: the tab 'Изображение' sits where its board puts it") {
    if (!fonts_ok()) {
        return;
    }
    using B = SettingsBoards;
    Fixture f;
    f.open(MenuScreen::Settings);
    check_settings_tab(f,
                       B::kVideo,
                       "video",
                       "Settings content / video",
                       kVideoRows,
                       "Изображение · Настройте игру под свой экран и привычки");
    // The chevron of a dropdown and the toggle with its track, thumb and caption.
    check_rect(child_of_class(f.el("ctl-0"), "ctl-chev"), layer(B::kVideo, "Dropdown chevron"));
    Rml::Element* toggle = f.el("ctl-5");
    CHECK(toggle->IsClassSet("on"));
    check_rect(child_of_class(toggle, "tg-track"), layer(B::kVideo, "Toggle track"));
    check_rect(child_of_class(toggle, "tg-thumb"), layer(B::kVideo, "Toggle thumb"));
    check_text(child_of_class(toggle, "tg-text"), text_layer(B::kVideo, "Вкл."));
    // Nothing changed: "Применить" is the disabled button of the board and no message shows.
    CHECK(f.el("btn-apply")->IsClassSet("is-disabled"));
    CHECK_FALSE(f.has("set-msg"));
    CHECK(f.el("cat-0")->IsClassSet("selected"));
}

TEST_CASE("menu: the tab 'Интерфейс' sits where its board puts it") {
    if (!fonts_ok()) {
        return;
    }
    using B = SettingsBoards;
    Fixture f;
    f.open(MenuScreen::Settings);
    f.click("cat-1");
    REQUIRE(f.menu->settings_tab() == pat::ui::SettingsTab::Interface);
    check_settings_tab(f,
                       B::kInterface,
                       "interface",
                       "Settings content / interface",
                       kInterfaceRows,
                       "Интерфейс · Настройте игру под свой экран и привычки");
    // A switched-off toggle keeps its thumb on the left (board: 1643 - 1638).
    Rml::Element* off = f.el("ctl-5");
    CHECK_FALSE(off->IsClassSet("on"));
    CHECK(std::abs(child_of_class(off, "tg-thumb")->GetAbsoluteLeft() - off->GetAbsoluteLeft() - 5) <= 1.0);
    CHECK(f.el("cat-1")->IsClassSet("selected"));
    CHECK_FALSE(f.el("cat-0")->IsClassSet("selected"));
}

TEST_CASE("menu: the tab 'Звук' sits where its board puts it") {
    if (!fonts_ok()) {
        return;
    }
    using B = SettingsBoards;
    Fixture f;
    f.open(MenuScreen::Settings);
    f.menu->press_tab(pat::ui::SettingsTab::Audio);
    f.settle();
    check_settings_tab(f,
                       B::kAudio,
                       "audio",
                       "Settings content / audio",
                       kAudioRows,
                       "Звук · Настройте игру под свой экран и привычки");
    Rml::Element* master = f.el("ctl-0");
    check_rect(child_of_class(master, "sl-track"), layer(B::kAudio, "Slider track"));
    check_rect(child_of_class(master, "sl-value"), layer(B::kAudio, "Slider value"));
    check_rect(child_of_class(master, "sl-thumb"), layer(B::kAudio, "Slider thumb"));
    check_text(child_of_class(master, "sl-text"), text_layer(B::kAudio, "80%"));
    check_text(child_of_class(f.el("ctl-1"), "sl-text"), text_layer(B::kAudio, "65%"));
    CHECK(f.menu->settings().master_volume == 80);
}

TEST_CASE("menu: the tab 'Управление' sits where its board puts it") {
    if (!fonts_ok()) {
        return;
    }
    using B = SettingsBoards;
    Fixture f;
    f.open(MenuScreen::Settings);
    f.click("cat-3");
    check_settings_tab(f,
                       B::kControls,
                       "controls",
                       "Settings content / controls",
                       kControlRows,
                       "Управление · Настройте игру под свой экран и привычки");
}

TEST_CASE("menu: a change shows the red message and the green button, 'Применить' hands the values over") {
    if (!fonts_ok()) {
        return;
    }
    using B = SettingsBoards;
    Fixture f;
    f.open(MenuScreen::Settings);
    f.click("cat-1");
    f.click("ctl-5");  // "Уменьшение анимации" goes on
    CHECK(f.menu->settings_dirty());
    CHECK(f.menu->pending_settings().reduce_motion);
    CHECK_FALSE(f.menu->settings().reduce_motion);  // not applied yet
    CHECK(f.el("ctl-5")->IsClassSet("on"));
    check_text(f.el("set-msg"), text_layer(B::kChanged, "Есть неприменённые изменения"));
    CHECK(text_of(f.el("set-msg")) == "Есть неприменённые изменения");
    CHECK_FALSE(f.el("set-msg")->IsClassSet("ok"));
    CHECK_FALSE(f.el("btn-apply")->IsClassSet("is-disabled"));
    check_rect(f.el("btn-apply"), layer(B::kChanged, "Button / Применить"));
    CHECK(f.log.empty());

    f.click("btn-apply");
    REQUIRE(f.log.size() == 1);
    CHECK(f.log[0] == "apply");
    REQUIRE(f.applied.size() == 1);
    CHECK(f.applied[0].reduce_motion);
    CHECK_FALSE(f.menu->settings_dirty());
    CHECK(f.menu->settings_applied());
    CHECK(f.menu->settings().reduce_motion);
    check_text(f.el("set-msg"), text_layer(B::kSaved, "Изменения применены"));
    CHECK(text_of(f.el("set-msg")) == "Изменения применены");
    CHECK(f.el("set-msg")->IsClassSet("ok"));
    CHECK(f.el("btn-apply")->IsClassSet("is-disabled"));
    f.click("btn-apply");  // nothing to apply any more
    CHECK(f.log.size() == 1);

    f.click("ctl-3");  // the next change takes the confirmation away
    CHECK_FALSE(f.menu->settings_applied());
    CHECK(text_of(f.el("set-msg")) == "Есть неприменённые изменения");
}

TEST_CASE("menu: the dropdowns step through their options and the UI scale comes out in percent") {
    if (!fonts_ok()) {
        return;
    }
    Fixture f;
    f.open(MenuScreen::Settings);
    f.click("ctl-0");  // the resolution: next in the list
    CHECK(f.menu->pending_settings().width == 2560);
    CHECK(f.menu->pending_settings().height == 1440);
    CHECK(text_of(child_of_class(f.el("ctl-0"), "ctl-text")) == "2560 × 1440");
    f.click("ctl-4");  // the frame limit: 60 -> 120
    CHECK(f.menu->pending_settings().frame_limit == 120);
    f.click("ctl-4");  // -> no limit
    CHECK(f.menu->pending_settings().frame_limit == 0);
    CHECK(text_of(child_of_class(f.el("ctl-4"), "ctl-text")) == "Без ограничения");
    f.click("ctl-4");  // the last one wraps
    CHECK(f.menu->pending_settings().frame_limit == 30);
    f.click("ctl-5");  // vertical sync off
    CHECK_FALSE(f.menu->pending_settings().vsync);
    CHECK(text_of(child_of_class(f.el("ctl-5"), "tg-text")) == "Выкл.");

    f.click("cat-1");
    f.click("ctl-0");  // 100% -> 110%
    CHECK(f.menu->pending_settings().ui_scale == 110);
    CHECK(text_of(child_of_class(f.el("ctl-0"), "ctl-text")) == "110%");
    f.click("ctl-0");
    f.click("ctl-0");  // 125%, 150%
    CHECK(f.menu->pending_settings().ui_scale == 150);
    f.click("ctl-0");  // wraps to 80%
    CHECK(f.menu->pending_settings().ui_scale == 80);
    f.click("ctl-1");  // one language: nothing to step to
    CHECK(f.menu->pending_settings().language == "ru");
    f.click("ctl-2");
    CHECK(f.menu->pending_settings().text_size == 2);
    f.click("ctl-3");
    CHECK(f.menu->pending_settings().tooltips == 2);
}

TEST_CASE("menu: the arrows change the focused row and walk the tabs and rows") {
    if (!fonts_ok()) {
        return;
    }
    Fixture f;
    f.open(MenuScreen::Settings);
    CHECK(f.focused() == "cat-0");
    f.key(Rml::Input::KI_DOWN);  // Up and Down on the tabs walk them
    CHECK(f.menu->settings_tab() == pat::ui::SettingsTab::Interface);
    CHECK(f.focused() == "cat-1");
    f.key(Rml::Input::KI_UP);
    CHECK(f.menu->settings_tab() == pat::ui::SettingsTab::Video);
    f.key(Rml::Input::KI_UP);  // the first one stays
    CHECK(f.menu->settings_tab() == pat::ui::SettingsTab::Video);
    for (int i = 0; i < 5; ++i) {
        f.key(Rml::Input::KI_DOWN);
    }
    CHECK(f.menu->settings_tab() == pat::ui::SettingsTab::Controls);
    f.key(Rml::Input::KI_UP);  // Audio
    f.key(Rml::Input::KI_UP);  // Interface
    CHECK(f.menu->settings_tab() == pat::ui::SettingsTab::Interface);

    f.el("ctl-0")->Focus();
    f.settle();
    f.key(Rml::Input::KI_RIGHT);  // a dropdown steps (and does not wrap with the arrows)
    CHECK(f.menu->pending_settings().ui_scale == 110);
    f.key(Rml::Input::KI_LEFT);
    f.key(Rml::Input::KI_LEFT);
    CHECK(f.menu->pending_settings().ui_scale == 90);
    f.key(Rml::Input::KI_DOWN);  // the next row
    CHECK(f.focused() == "ctl-1");
    f.key(Rml::Input::KI_UP);
    f.key(Rml::Input::KI_UP);  // the first row goes up to the tab
    CHECK(f.focused() == "cat-1");

    f.el("ctl-5")->Focus();
    f.settle();
    f.key(Rml::Input::KI_RIGHT);  // a toggle: right is on, left is off
    CHECK(f.menu->pending_settings().reduce_motion);
    f.key(Rml::Input::KI_RIGHT);
    CHECK(f.menu->pending_settings().reduce_motion);
    f.key(Rml::Input::KI_LEFT);
    CHECK_FALSE(f.menu->pending_settings().reduce_motion);
    f.key(Rml::Input::KI_DOWN);  // below the last row: the buttons
    CHECK(f.focused() == "btn-defaults");
}

TEST_CASE("menu: a slider follows the mouse and the arrows") {
    if (!fonts_ok()) {
        return;
    }
    Fixture f;
    f.open(MenuScreen::Settings);
    f.menu->press_tab(pat::ui::SettingsTab::Audio);
    f.settle();
    Rml::Element* track = child_of_class(f.el("ctl-1"), "sl-track");
    const float left = track->GetAbsoluteLeft();
    const float width = track->GetOffsetWidth();
    const float y = track->GetAbsoluteTop() + 2;
    mouse_at(f, left + width * 0.20f, y);
    f.ui->handle_event(event::MouseButton{1, true, 1, left + width * 0.20f, y});
    CHECK(f.menu->pending_settings().music_volume == 20);
    mouse_at(f, left + width * 0.90f, y + 300);  // the drag goes on outside the control
    CHECK(f.menu->pending_settings().music_volume == 90);
    f.ui->handle_event(event::MouseButton{1, false, 1, left + width * 0.90f, y + 300});
    mouse_at(f, left + width * 0.10f, y);  // released: the pointer no longer drags
    CHECK(f.menu->pending_settings().music_volume == 90);
    CHECK(text_of(child_of_class(f.el("ctl-1"), "sl-text")) == "90%");
    CHECK(f.menu->settings_dirty());
    CHECK(std::abs(f.el("ctl-1")->GetOffsetWidth() - 430) <= 1.0);  // the control keeps its size

    f.el("ctl-2")->Focus();
    f.settle();
    f.key(Rml::Input::KI_RIGHT);  // 70 -> 75
    CHECK(f.menu->pending_settings().ambient_volume == 75);
    for (int i = 0; i < 8; ++i) {
        f.key(Rml::Input::KI_RIGHT);
    }
    CHECK(f.menu->pending_settings().ambient_volume == 100);  // stops at the end
    f.menu->press_slider(2, 40);
    CHECK(f.menu->pending_settings().ambient_volume == 40);
    f.menu->press_setting(2);  // a press on a slider does nothing
    CHECK(f.menu->pending_settings().ambient_volume == 40);
}

TEST_CASE("menu: Esc or 'Главное меню' with unapplied changes asks, and the answers do what they say (dialog "
          "'Сохранить изменения?')") {
    if (!fonts_ok()) {
        return;
    }
    using B = SettingsBoards;
    Fixture f;
    f.open();
    f.click("mi-settings");
    f.click("cat-1");
    f.click("ctl-5");
    REQUIRE(f.menu->settings_dirty());
    CHECK(f.menu->press_back() == pat::ui::MenuBack::AskedSave);
    REQUIRE(f.menu->dialog() == MenuDialog::SaveChanges);
    f.settle();
    CHECK(f.focused() == "dlg-save-return");  // the safe answer is the default one
    Rml::Element* dialog = f.el("dialog-save");
    CHECK(std::abs(dialog->GetOffsetWidth() - 760) <= 1);
    CHECK(std::abs(dialog->GetOffsetHeight() - 386) <= 1);
    CHECK(std::abs(dialog->GetAbsoluteLeft() + dialog->GetOffsetWidth() / 2 - 960) <= 1);
    CHECK(std::abs(dialog->GetAbsoluteTop() + dialog->GetOffsetHeight() / 2 - 540) <= 1);
    check_text_inside(
        child_of_class(dialog, "dialog-title"), dialog, text_layer(B::kDlgSave, "Сохранить изменения?"));
    check_text_inside(
        f.el("dialog-save-text"),
        dialog,
        layer(B::kDlgSave, "Вы изменили настройки интерфейса. Применить изменения перед выходом?"));
    CHECK(text_of(f.el("dialog-save-text")) ==
          "Вы изменили настройки интерфейса.<br />Применить изменения перед выходом?");
    check_inside(f.el("dlg-save-apply"), dialog, layer(B::kDlgSave, "Button / Применить"));
    check_inside(f.el("dlg-save-discard"), dialog, layer(B::kDlgSave, "Button / Без сохранения"));
    check_inside(f.el("dlg-save-return"), dialog, layer(B::kDlgSave, "Button / Вернуться"));

    f.key(Rml::Input::KI_ESCAPE);  // Esc = "Вернуться"
    CHECK(f.menu->dialog() == MenuDialog::None);
    CHECK(f.menu->screen() == MenuScreen::Settings);
    CHECK(f.menu->settings_dirty());

    f.click("btn-main");  // the button asks too
    REQUIRE(f.menu->dialog() == MenuDialog::SaveChanges);
    f.click("dlg-save-return");
    CHECK(f.menu->screen() == MenuScreen::Settings);
    CHECK(f.log.empty());

    f.click("btn-main");
    f.click("dlg-save-discard");  // "Без сохранения": the edits are gone, the game is not told
    CHECK(f.menu->screen() == MenuScreen::Main);
    CHECK(f.menu->dialog() == MenuDialog::None);
    CHECK(f.log.empty());
    CHECK_FALSE(f.menu->settings_dirty());
    CHECK_FALSE(f.menu->settings().reduce_motion);

    f.click("mi-settings");  // a new visit starts from what was applied
    CHECK_FALSE(f.menu->pending_settings().reduce_motion);
    CHECK(f.menu->settings_tab() == pat::ui::SettingsTab::Video);
    f.click("cat-1");
    f.click("ctl-5");
    f.key(Rml::Input::KI_ESCAPE);
    f.click("dlg-save-apply");  // "Применить": applies and leaves
    CHECK(f.menu->screen() == MenuScreen::Main);
    REQUIRE(f.log.size() == 1);
    CHECK(f.log[0] == "apply");
    REQUIRE(f.applied.size() == 1);
    CHECK(f.applied[0].reduce_motion);
    CHECK(f.menu->settings().reduce_motion);
}

TEST_CASE("menu: the dialog names the section when only one tab changed, and says nothing when several did") {
    if (!fonts_ok()) {
        return;
    }
    Fixture f;
    f.open(MenuScreen::Settings);
    f.menu->press_setting(0);  // the resolution of the tab "Изображение"
    f.menu->press_back();
    f.settle();
    CHECK(text_of(f.el("dialog-save-text")) ==
          "Вы изменили настройки изображения.<br />Применить изменения перед выходом?");
    f.menu->press_dialog("return");
    f.menu->press_tab(pat::ui::SettingsTab::Audio);
    f.menu->press_slider(0, 10);
    f.menu->press_back();
    f.settle();
    CHECK(text_of(f.el("dialog-save-text")) ==
          "Вы изменили настройки.<br />Применить изменения перед выходом?");
}

TEST_CASE("menu: 'По умолчанию' asks first and puts back the values of the open tab only (dialog 'Вернуть "
          "настройки по умолчанию?')") {
    if (!fonts_ok()) {
        return;
    }
    using B = SettingsBoards;
    Fixture f;
    f.open(MenuScreen::Settings);
    f.menu->press_setting(0);  // video: 2560 x 1440
    f.click("cat-1");
    f.click("ctl-0");  // interface: 110%
    f.click("ctl-5");
    f.click("btn-defaults");
    REQUIRE(f.menu->dialog() == MenuDialog::ResetDefaults);
    CHECK(f.focused() == "dlg-reset-cancel");
    Rml::Element* dialog = f.el("dialog-reset");
    CHECK(std::abs(dialog->GetOffsetWidth() - 760) <= 1);
    CHECK(std::abs(dialog->GetOffsetHeight() - 386) <= 1);
    check_text_inside(child_of_class(dialog, "dialog-title"),
                      dialog,
                      text_layer(B::kDlgReset, "Вернуть настройки по умолчанию?"));
    check_text_inside(
        f.el("dialog-reset-text"),
        dialog,
        layer(B::kDlgReset, "Текущие значения выбранного раздела будут заменены. Изменения можно пр"));
    check_inside(f.el("dlg-reset-cancel"), dialog, layer(B::kDlgReset, "Button / Отмена"));
    check_inside(f.el("dlg-reset-do"), dialog, layer(B::kDlgReset, "Button / Вернуть"));

    f.key(Rml::Input::KI_ESCAPE);  // Esc = "Отмена"
    CHECK(f.menu->dialog() == MenuDialog::None);
    CHECK(f.menu->pending_settings().ui_scale == 110);
    CHECK(f.focused() == "btn-defaults");  // the focus comes back to the button that asked

    f.click("btn-defaults");
    f.click("dlg-reset-cancel");
    CHECK(f.menu->pending_settings().ui_scale == 110);
    f.click("btn-defaults");
    f.click("dlg-reset-do");
    CHECK(f.menu->dialog() == MenuDialog::None);
    CHECK(f.menu->pending_settings().ui_scale == 100);
    CHECK_FALSE(f.menu->pending_settings().reduce_motion);
    CHECK(f.menu->pending_settings().width == 2560);  // the other tab keeps its edit
    CHECK(f.menu->settings_dirty());
    CHECK(f.log.empty());
}

TEST_CASE("menu: a binding is chosen in the dialog 'Нажмите новую клавишу' (board)") {
    if (!fonts_ok()) {
        return;
    }
    using B = SettingsBoards;
    Fixture f;
    f.open(MenuScreen::Settings);
    f.click("cat-3");
    f.click("ctl-0");
    REQUIRE(f.menu->dialog() == MenuDialog::Rebind);
    CHECK(f.focused() == "dlg-rebind-cancel");
    Rml::Element* dialog = f.el("dialog-rebind");
    CHECK(std::abs(dialog->GetOffsetWidth() - 700) <= 1);
    CHECK(std::abs(dialog->GetOffsetHeight() - 394) <= 1);
    CHECK(std::abs(dialog->GetAbsoluteLeft() + dialog->GetOffsetWidth() / 2 - 960) <= 1);
    CHECK(std::abs(dialog->GetAbsoluteTop() + dialog->GetOffsetHeight() / 2 - 540) <= 1);
    check_text_inside(
        child_of_class(dialog, "dialog-title"), dialog, text_layer(B::kDlgRebind, "Нажмите новую клавишу"));
    check_text_inside(f.el("dialog-rebind-text"),
                      dialog,
                      layer(B::kDlgRebind, "Пауза / продолжить Текущее назначение: Пробел"));
    CHECK(text_of(f.el("dialog-rebind-text")) == "Пауза / продолжить<br />Текущее назначение: Пробел");
    check_inside(f.el("rebind-badge"), dialog, layer(B::kDlgRebind, "Badge / ПРОБЕЛ"));
    check_text_inside(f.el("rebind-badge-text"), dialog, text_layer(B::kDlgRebind, "ПРОБЕЛ"));
    check_inside(f.el("dlg-rebind-cancel"), dialog, layer(B::kDlgRebind, "Button / Отмена"));
    check_inside(f.el("dlg-rebind-save"), dialog, layer(B::kDlgRebind, "Button / Сохранить"));
    CHECK(f.menu->rebind_pending() == "space");

    f.key(Rml::Input::KI_F);  // the key is the new binding (the dialog stays open)
    CHECK(f.menu->dialog() == MenuDialog::Rebind);
    CHECK(f.menu->rebind_pending() == "f");
    CHECK(text_of(f.el("rebind-badge-text")) == "F");
    CHECK(f.menu->pending_settings().keys[0] == "space");  // until "Сохранить"
    f.key(Rml::Input::KI_SPACE);  // Space would press the focused button: here it is a key like any other
    CHECK(f.menu->dialog() == MenuDialog::Rebind);
    CHECK(f.menu->rebind_pending() == "space");
    CHECK(text_of(f.el("rebind-badge-text")) == "ПРОБЕЛ");
    f.key(Rml::Input::KI_F5);
    CHECK(text_of(f.el("rebind-badge-text")) == "F5");
    f.key(Rml::Input::KI_LSHIFT);  // a key that cannot be bound changes nothing
    CHECK(f.menu->rebind_pending() == "f5");

    f.key(Rml::Input::KI_ESCAPE);  // Esc cancels: the old key stays
    CHECK(f.menu->dialog() == MenuDialog::None);
    CHECK(f.menu->pending_settings().keys[0] == "space");
    CHECK_FALSE(f.menu->settings_dirty());
    CHECK(f.focused() == "ctl-0");

    f.click("ctl-0");
    f.key(Rml::Input::KI_F5);
    f.key(Rml::Input::KI_RETURN);  // Enter is not a binding: it presses the focused "Отмена"
    CHECK(f.menu->dialog() == MenuDialog::None);
    CHECK(f.menu->pending_settings().keys[0] == "space");

    f.click("ctl-0");
    f.key(Rml::Input::KI_F5);
    f.click("dlg-rebind-save");
    CHECK(f.menu->dialog() == MenuDialog::None);
    CHECK(f.menu->pending_settings().keys[0] == "f5");
    CHECK(f.menu->settings_dirty());
    CHECK(text_of(child_of_class(f.el("ctl-0"), "ctl-text")) == "F5");
    CHECK(f.log.empty());

    // A key that another action has goes to it in exchange: no key serves two actions.
    f.click("ctl-5");
    CHECK(text_of(f.el("dialog-rebind-text")) == "Режим карты<br />Текущее назначение: M");
    f.key(Rml::Input::KI_F5);
    f.click("dlg-rebind-save");
    CHECK(f.menu->pending_settings().keys[5] == "f5");
    CHECK(f.menu->pending_settings().keys[0] == "m");

    f.menu->press_defaults();  // the defaults of the tab bring the keys back
    f.settle();
    f.click("dlg-reset-do");
    CHECK(f.menu->pending_settings().keys[0] == "space");
    CHECK(f.menu->pending_settings().keys[5] == "m");
    CHECK_FALSE(f.menu->settings_dirty());
}

TEST_CASE("menu: the host's values fill the screen and the applied scale reaches the callback") {
    if (!fonts_ok()) {
        return;
    }
    Fixture f;
    pat::ui::SettingsModel host;
    host.ui_scale = 125;
    host.master_volume = 30;
    f.menu->set_settings(host);
    f.open(MenuScreen::Settings);
    f.click("cat-1");
    CHECK(text_of(child_of_class(f.el("ctl-0"), "ctl-text")) == "125%");
    f.click("ctl-0");  // 150%
    f.click("btn-apply");
    REQUIRE(f.applied.size() == 1);
    CHECK(f.applied[0].ui_scale == 150);
    CHECK(f.applied[0].master_volume == 30);
    CHECK(f.menu->settings().ui_scale == 150);
}

TEST_CASE("menu: the settings fit the window at 1280x720 and in 21:9") {
    if (!fonts_ok()) {
        return;
    }
    {
        Fixture f({1280, 720});
        f.open(MenuScreen::Settings);
        const float scale = design_ui_scale({1280, 720});
        Rml::Element* ctl = f.el("ctl-0");
        CHECK(std::abs(ctl->GetOffsetWidth() - 430 * scale) <= 1.5);
        CHECK(ctl->GetAbsoluteLeft() + ctl->GetOffsetWidth() <= 1280);
        Rml::Element* apply = f.el("btn-apply");
        CHECK(apply->GetAbsoluteLeft() + apply->GetOffsetWidth() <= 1280);
        CHECK(apply->GetAbsoluteTop() + apply->GetOffsetHeight() <= 720);
        f.menu->press_tab(pat::ui::SettingsTab::Audio);
        f.settle();
        CHECK(std::abs(child_of_class(f.el("ctl-0"), "sl-track")->GetOffsetWidth() - 326 * scale) <= 1.5);
        f.menu->press_slider(0, 10);
        f.menu->press_back();
        f.settle();
        Rml::Element* dialog = f.el("dialog-save");
        CHECK(dialog->GetAbsoluteLeft() >= 0);
        CHECK(dialog->GetAbsoluteLeft() + dialog->GetOffsetWidth() <= 1280);
        CHECK(std::abs(dialog->GetOffsetWidth() - 760 * scale) <= 1.5);
    }
    {
        Fixture f({2560, 1080});
        f.open(MenuScreen::Settings);
        // The stage is centred: the panel keeps its distance from the left edge of the 1920 canvas.
        CHECK(std::abs(f.el("set-content")->GetAbsoluteLeft() - (320 + 462)) <= 1.5);
        CHECK(std::abs(f.el("btn-apply")->GetAbsoluteLeft() - (320 + 1362)) <= 1.5);
        f.menu->press_setting(5);
        f.settle();
        CHECK(std::abs(f.el("set-msg")->GetAbsoluteLeft() - (320 + 446)) <= 1.5);
    }
}

#endif  // _WIN32
