// The start menu (UI-D12, #284; boards "МЕНЮ / ..." of the mockup), part 1: the screen stack, the main menu,
// "Об игре", "Загрузка", the dialogs and the overview page, on an offscreen WARP target. Rectangles are
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
        pat::ui::MenuActions actions;
        actions.new_game = [this] { log.push_back("new"); };
        actions.continue_game = [this] { log.push_back("continue"); };
        actions.load_save = [this](const std::string& id) { log.push_back("load:" + id); };
        actions.open_settings = [this] { log.push_back("settings"); };
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
    f.click("mi-new");
    f.click("mi-settings");
    REQUIRE(f.log.size() == 3);
    CHECK(f.log[1] == "new");
    CHECK(f.log[2] == "settings");
    CHECK(f.menu->screen() == MenuScreen::Main);
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

    f.click("save-2");
    f.click("btn-load");
    CHECK(f.log.size() == 1);  // nothing was loaded
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
    f.click("btn-new");
    REQUIRE(f.log.size() == 1);
    CHECK(f.log[0] == "new");
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
    CHECK(f.el("card-2")->IsClassSet("is-disabled"));  // "Новая игра" arrives in part 2
    CHECK_FALSE(f.el("card-0")->IsClassSet("is-disabled"));
    CHECK(f.focused() == "card-0");

    f.click("card-2");
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

#endif  // _WIN32
