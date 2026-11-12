#pragma once

#include "pat/core/result.hpp"
#include "pat/engine/ui.hpp"
#include "pat/loc/localizer.hpp"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Rml {
class ElementDocument;
}

namespace pat::ui {

/// The screens of the start menu (UI-D12, #284; boards "МЕНЮ / …"). Part 1: the main menu, "Загрузка", "Об
/// игре" and the overview page of the mockup. Part 2: "Новая игра" (choice of the house), "Свой род", "Цель
/// партии" and the world screen ("Подготовка мира" / "Мир готов", also for the restore of a save).
/// "Настройки" follows in the next part and joins the same stack.
enum class MenuScreen { Main, Load, About, Overview, NewGame, Custom, Goal, World };

/// A dialog over the current screen; the screen below stays but takes neither mouse nor Tab.
enum class MenuDialog { None, Exit, SaveUnavailable };

/// What Esc did (the screen stack answers it so that the rules can be tested without a window). `Held`: the
/// screen keeps the key without leaving (the ready world waits for "Вступить в игру").
enum class MenuBack { Nothing, ClosedDialog, WentBack, AskedExit, Held };

/// The stack of screens and the dialog on top of it: pure logic, no RmlUi. The root is the screen the menu
/// was opened with; every other screen is pushed above it and "back" pops it. Esc closes the dialog first,
/// then goes back, and on the root main menu asks whether to leave the game.
class MenuNavigator {
public:
    explicit MenuNavigator(MenuScreen root = MenuScreen::Main) { reset(root); }

    void reset(MenuScreen root);
    /// Opens `screen` above the current one (a repeat of the current screen is ignored).
    void push(MenuScreen screen);
    /// Goes back one screen; false at the root. Closes no dialog.
    bool pop();
    void open_dialog(MenuDialog dialog) { dialog_ = dialog; }
    void close_dialog() { dialog_ = MenuDialog::None; }
    /// The Esc key.
    MenuBack back();

    [[nodiscard]] MenuScreen top() const { return stack_.back(); }
    [[nodiscard]] MenuScreen root() const { return stack_.front(); }
    [[nodiscard]] MenuDialog dialog() const { return dialog_; }
    [[nodiscard]] std::size_t depth() const { return stack_.size(); }

private:
    std::vector<MenuScreen> stack_;
    MenuDialog dialog_ = MenuDialog::None;
};

enum class MenuSaveKind { Auto, Manual };

/// One entry of the list of saves. Like the other snapshots it holds data, not phrases: the strings about the
/// world (names of houses and people, the date, the treasury) are already formatted by the game side
/// (src/game), the captions around them come from the string table.
struct MenuSave {
    std::string id;    ///< stable id handed to the load callback
    std::string name;  ///< the player's name of a manual save; empty for the autosave (the kind is its name)
    MenuSaveKind kind = MenuSaveKind::Auto;
    bool compatible = true;  ///< false: the file is from a newer version of the game and cannot be loaded
    std::string house;       ///< "Голицыны" (nominative)
    std::string house_gen;   ///< "Голицыных" (genitive: "Род Голицыных")
    std::string date;        ///< "1 января 1703"
    std::string season;      ///< "Зима"
    std::string head;        ///< head of the house
    std::string treasury;    ///< "1 248 ₽"
};

/// What the menu shows. `placeholder` is true while the data does not come from the save catalogue.
struct MenuSnapshot {
    std::vector<MenuSave> saves;  ///< newest first; empty on the first run
    bool placeholder = false;
};

/// A house of the choice "Новая игра" (board 03). Strings about the world are formatted by the game side; the
/// age and the number of members are numbers so that the string table writes the plural ("47 лет", "12
/// членов").
struct MenuHouse {
    std::string id;        ///< stable id handed to the start callback
    std::string name;      ///< "Голицыны"
    std::string name_gen;  ///< "Голицыных" ("Род Голицыных")
    std::string line;      ///< one-line invitation under the name
    std::string badge;     ///< "ЗНАТНЫЙ РОД"
    std::string icon;      ///< file stem of the icon in the icons directory ("shield", "crown", "dynasty")
    std::string region;    ///< "Россия · 1703"
    std::string head;      ///< head of the house
    int head_age = 0;
    int members = 0;
    std::string treasury;  ///< "1 248 ₽"
    std::string prestige;  ///< "68 / 100"
    std::string heir;
    std::string portrait;  ///< file name in the art directory
};

/// A goal of the game (board 05); the boards fix the place of each of the five cards by its index.
struct MenuGoal {
    std::string id;
    std::string title;
    std::string line;
    std::string icon;
};

/// The fields of the own house (board 04). `look` and `faith` are indices into the option lists of
/// `MenuNewGame`.
struct MenuCustomHouse {
    std::string surname;     ///< "Соколин"
    std::string house_name;  ///< genitive plural: "Соколиных"
    std::string name;
    std::string patronymic;
    std::string motto;
    int look = 0;
    int faith = 0;
};

/// What "Новая игра" offers: the houses, the goals and the defaults of the own house. `placeholder` is true
/// while the data does not come from a scenario.
struct MenuNewGame {
    std::vector<MenuHouse> houses;
    std::vector<MenuGoal> goals;
    std::size_t default_goal = 0;  ///< index of the goal chosen at the start
    std::string start_date;        ///< "1 января 1703"
    MenuCustomHouse custom_defaults;
    std::vector<std::string> looks;   ///< options of "Образ основателя" ("Мужчина · 22 года")
    std::vector<std::string> faiths;  ///< options of "Вероисповедание"
    std::string custom_treasury;      ///< treasury of a new house ("420 ₽")
    std::string custom_portrait;      ///< file name in the art directory
    bool placeholder = false;
};

/// The choice that starts a game ("Начать историю").
struct MenuStart {
    bool custom = false;  ///< own house: `house_id` is empty and `custom_house` holds the founder
    std::string house_id;
    std::string goal_id;
    MenuCustomHouse custom_house;
};

/// What the world screen is about: a new game or the restore of a save.
enum class MenuWorldKind { NewGame, Restore };

/// PLACEHOLDER data of the mockup (boards 02 and 07: the autosave and a manual save of the Golitsyns, one
/// save of a newer version); the texts are the `ui.menu.placeholder.*` keys. Replace with the save catalogue
/// when the loading of games exists.
[[nodiscard]] MenuSnapshot make_placeholder_menu(const loc::Localizer& loc);

/// PLACEHOLDER data of boards 03 to 06 (Golitsyns, Sheremetevs, Dolgorukovs, the five goals, the founder
/// "Иван Михайлович Соколин"); the texts are the `ui.menu.placeholder.house.*`, `.custom.*` and
/// `ui.menu.goal.*` keys. Replace with the scenario data of E12.
[[nodiscard]] MenuNewGame make_placeholder_new_game(const loc::Localizer& loc);

/// The actions that need a world; the menu only calls them (the spike logs them). After `start_game`,
/// `continue_game` and `load_save` the world screen shows the preparation: the host reports it with
/// `MenuWindow::set_progress()` and `finish_preparation()`. "Настройки" joins in the next part. Every
/// callback may be empty.
struct MenuActions {
    std::function<void(const MenuStart&)> start_game;   ///< "Начать историю"
    std::function<void()> continue_game;                ///< "Продолжить": the newest save
    std::function<void(const std::string&)> load_save;  ///< "Загрузить историю": the id of a compatible save
    std::function<void()> cancel_prepare;               ///< "Отмена" on the world screen
    std::function<void()> enter_game;                   ///< "Вступить в игру" / "Продолжить игру"
    std::function<void()> open_settings;
    std::function<void()> quit;  ///< "Выйти из игры" confirmed
};

/// The start menu on RmlUi: one full-screen document (apps/spike/ui/menu.rml) that shows the current screen
/// of a MenuNavigator. A modal document: while it is shown the windows below get neither mouse nor keys.
/// Keys: Up and Down move the highlight (main menu, list of saves); on the grids of cards ("Новая игра",
/// "Цель партии") the four arrows move the choice; Tab walks the controls, Enter presses, Esc goes back
/// (closes the dialog, leaves the screen, cancels the preparation of the world, on the main menu asks to
/// exit).
class MenuWindow {
public:
    /// `icons` is the directory with the SVG icons (assets/ui/patriarch/icons); the art and decor directories
    /// are its siblings.
    [[nodiscard]] static core::Result<std::unique_ptr<MenuWindow>>
    create(engine::UiContext& ui,
           const std::filesystem::path& document,
           const std::filesystem::path& icons,
           const loc::Localizer& loc);
    ~MenuWindow();
    MenuWindow(const MenuWindow&) = delete;
    MenuWindow& operator=(const MenuWindow&) = delete;

    void set_snapshot(const MenuSnapshot& snapshot);
    [[nodiscard]] const MenuSnapshot& snapshot() const;
    void set_new_game(const MenuNewGame& data);
    [[nodiscard]] const MenuNewGame& new_game() const;
    void set_actions(MenuActions actions);
    /// Re-reads every text from the Localizer.
    void relocalize();

    /// Shows the menu at `root` (the stack is reset) with the keyboard focus on the first control.
    void open(MenuScreen root = MenuScreen::Main);
    void close();
    [[nodiscard]] bool visible() const;

    /// Shows the world screen at the root of the stack (the spike uses it for screenshots, a host that loads
    /// a game by itself may too): `percent` 0..100, 100 is "Мир готов".
    void open_world(MenuWorldKind kind, int percent, const std::string& house_line = {});
    /// Progress of the preparation: `percent` 0..100 and `stage` 0..2, the line that is being done; 100 shows
    /// "Мир готов". Ignored unless the world screen is open.
    void set_progress(int percent, int stage);
    void finish_preparation() { set_progress(100, 2); }
    /// Chooses what the goal screen is about: the own house (true) or the chosen one.
    void set_custom_flow(bool custom);

    /// Call once per frame after UiContext::update(): moves the keyboard focus to the control a transition
    /// chose (the new elements exist only after the update).
    void update();

    // ---- what the menu does on a press (the same as a click; used by the keys and the tests)
    /// Esc: closes the dialog, goes back, or asks to exit.
    MenuBack press_back();
    /// Moves the highlight of the main menu or the list of saves by `delta` (wraps around).
    void press_move(int delta);
    /// A main-menu entry: "continue", "new", "load", "settings", "about", "exit"; disabled ones are ignored.
    void press_item(const std::string& id);
    void press_save(std::size_t index);        ///< selects a row of the list of saves
    void press_load();                         ///< "Загрузить историю"
    void press_card(std::size_t index);        ///< a card of the overview page
    void press_dialog(const std::string& id);  ///< "stay", "exit", "back" (К сохранениям)
    void press_house(std::size_t index);  ///< a card of "Новая игра"; the last one is "Основать свой род"
    void press_goal(std::size_t index);   ///< a card of "Цель партии"
    void press_next();                    ///< "Далее: цель партии" / "Начать историю"
    void press_enter();                   ///< "Вступить в игру" / "Продолжить игру"
    /// Sets a field of the own house: "surname", "house", "name", "patronymic" or "motto" (typing in the
    /// inputs does the same); `press_cycle("look" | "faith")` takes the next option of a drop-down.
    void set_field(const std::string& field, const std::string& value);
    void press_cycle(const std::string& field);
    [[nodiscard]] MenuStart current_start() const;
    [[nodiscard]] bool next_enabled() const;  ///< false while the own house lacks a required field
    [[nodiscard]] int progress() const;       ///< percent shown on the world screen
    [[nodiscard]] bool world_ready() const;
    [[nodiscard]] int house_selected() const;  ///< index of the chosen house, or -1
    [[nodiscard]] int goal_selected() const;

    [[nodiscard]] MenuScreen screen() const;
    [[nodiscard]] MenuDialog dialog() const;
    [[nodiscard]] std::size_t depth() const;
    /// Index of the highlighted row (main menu entry or save), or -1.
    [[nodiscard]] int highlighted() const;
    /// True when the main menu shows the "есть сохранение" variant (board 02).
    [[nodiscard]] bool has_save_variant() const;

    [[nodiscard]] Rml::ElementDocument* document() const;

private:
    struct Impl;
    explicit MenuWindow(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

}  // namespace pat::ui
