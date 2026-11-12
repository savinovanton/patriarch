#pragma once

#include "pat/core/result.hpp"
#include "pat/engine/ui.hpp"
#include "pat/loc/localizer.hpp"

#include <array>
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
/// Part 3: "Настройки" (four tabs) joins the same stack.
enum class MenuScreen { Main, Load, About, Overview, NewGame, Custom, Goal, World, Settings };

/// A dialog over the current screen; the screen below stays but takes neither mouse nor Tab. `SaveChanges`,
/// `ResetDefaults` and `Rebind` belong to the settings.
enum class MenuDialog { None, Exit, SaveUnavailable, SaveChanges, ResetDefaults, Rebind };

/// What Esc did (the screen stack answers it so that the rules can be tested without a window). `Held`: the
/// screen keeps the key without leaving (the ready world waits for "Вступить в игру"). `AskedSave`: the
/// settings have unapplied changes and the dialog "Сохранить изменения?" is open.
enum class MenuBack { Nothing, ClosedDialog, WentBack, AskedExit, Held, AskedSave };

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

/// The tabs of "Настройки".
enum class SettingsTab { Video, Interface, Audio, Controls };
inline constexpr std::size_t kSettingsTabCount = 4;

/// The actions that have a key binding (the rows of the tab "Управление").
enum class KeyAction { Pause, Camera, Zoom, Select, Back, MapMode };
inline constexpr std::size_t kKeyActionCount = 6;

/// The values of "Настройки": plain data, no RmlUi and no files. The window edits a copy and hands it to
/// `MenuActions::apply_settings` on "Применить"; the host keeps what it applied and gives it back with
/// `MenuWindow::set_settings()`. Choices from a list are indices (`window_mode`, `map_quality`, `shadows`,
/// `text_size`, `tooltips`, `output_device`); the lists and their captions belong to the window
/// (`ui.menu.settings.*`). Most of it is a model-only stub for now: the host applies `ui_scale`, the rest
/// waits for the systems that read it.
struct SettingsModel {
    // "Изображение"
    int width = 1920;
    int height = 1080;
    int window_mode = 0;   ///< 0 borderless, 1 windowed, 2 fullscreen
    int map_quality = 1;   ///< 0 low, 1 medium, 2 high
    int shadows = 2;       ///< 0 off, 1 low, 2 medium, 3 high
    int frame_limit = 60;  ///< 30, 60, 120 or 0 (no limit)
    bool vsync = true;
    // "Интерфейс"
    int ui_scale = 100;  ///< percent, 80..150 (engine::kMinUserUiScale / kMaxUserUiScale)
    std::string language = "ru";
    int text_size = 1;  ///< 0 small, 1 standard, 2 large
    int tooltips = 1;   ///< 0 off, 1 contextual, 2 detailed
    bool pause_on_events = true;
    bool reduce_motion = false;
    // "Звук" (percent)
    int master_volume = 80;
    int music_volume = 65;
    int ambient_volume = 70;
    int interface_volume = 75;
    int output_device = 0;  ///< 0: the default device
    bool mute_unfocused = true;
    // "Управление": a key id per `KeyAction` ("space", "m", "f5", "left", ...)
    std::array<std::string, kKeyActionCount> keys = {"space", "wasd", "wheel", "lmb", "rmb_esc", "m"};

    friend bool operator==(const SettingsModel&, const SettingsModel&) = default;
};

/// True when `a` and `b` differ in a value of `tab`.
[[nodiscard]] bool settings_differ(const SettingsModel& a, const SettingsModel& b, SettingsTab tab);
/// Puts the defaults back into the values of `tab` only.
void reset_settings_tab(SettingsModel& model, SettingsTab tab);
/// Gives `key` (a key id) to `action`; when another action already has it, that one gets the key `action` had
/// (the two swap, so that no key serves two actions).
void rebind_key(SettingsModel& model, KeyAction action, const std::string& key);

/// The actions that need a world; the menu only calls them (the spike logs them). After `start_game`,
/// `continue_game` and `load_save` the world screen shows the preparation: the host reports it with
/// `MenuWindow::set_progress()` and `finish_preparation()`. Every callback may be empty.
struct MenuActions {
    std::function<void(const MenuStart&)> start_game;   ///< "Начать историю"
    std::function<void()> continue_game;                ///< "Продолжить": the newest save
    std::function<void(const std::string&)> load_save;  ///< "Загрузить историю": the id of a compatible save
    std::function<void()> cancel_prepare;               ///< "Отмена" on the world screen
    std::function<void()> enter_game;                   ///< "Вступить в игру" / "Продолжить игру"
    /// "Применить" of the settings: the values to apply (a real host also writes them to its options file).
    std::function<void(const SettingsModel&)> apply_settings;
    std::function<void()> quit;  ///< "Выйти из игры" confirmed
};

/// The start menu on RmlUi: one full-screen document (apps/spike/ui/menu.rml) that shows the current screen
/// of a MenuNavigator. A modal document: while it is shown the windows below get neither mouse nor keys.
/// Keys: Up and Down move the highlight (main menu, list of saves); on the grids of cards ("Новая игра",
/// "Цель партии") the four arrows move the choice; Tab walks the controls, Enter presses, Esc goes back
/// (closes the dialog, leaves the screen, cancels the preparation of the world, on the main menu asks to
/// exit; on the settings with unapplied changes asks to save them). On the settings Up and Down walk the
/// tabs and the rows, Left and Right change the value of the focused row; while the dialog "Нажмите новую
/// клавишу" is open every key but Esc, Tab and Enter is the new binding.
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
    /// What is applied now (the host's own values); the edited copy starts from it. Replaces unapplied edits.
    void set_settings(const SettingsModel& applied);
    [[nodiscard]] const SettingsModel& settings() const;          ///< what was applied last
    [[nodiscard]] const SettingsModel& pending_settings() const;  ///< the edited copy shown on the screen
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
    // ---- settings: the tab, the rows of the tab (6 per tab, top to bottom), the buttons and the dialogs
    void press_tab(SettingsTab tab);
    /// A row: a dropdown takes the next option (the last wraps), a toggle flips, a key binding opens the
    /// dialog "Нажмите новую клавишу"; a slider ignores a press.
    void press_setting(std::size_t row);
    /// The arrows on a focused row: a dropdown steps `delta` options, a toggle goes on (`delta` > 0) or off,
    /// a slider moves by 5 points per step.
    void press_adjust(std::size_t row, int delta);
    void press_slider(std::size_t row, int percent);  ///< a slider to `percent` (0..100)
    void press_apply();                               ///< "Применить"; ignored while nothing changed
    void press_defaults();                            ///< "По умолчанию": asks first
    /// The dialog "Нажмите новую клавишу": the key that was pressed (a key id; Esc, Tab and Enter never
    /// reach it). The badge shows it; "Сохранить" gives it to the action.
    void capture_key(const std::string& key);
    [[nodiscard]] SettingsTab settings_tab() const;
    [[nodiscard]] bool settings_dirty() const;    ///< the edited copy differs from the applied values
    [[nodiscard]] bool settings_applied() const;  ///< "Изменения применены" is shown
    [[nodiscard]] const std::string& rebind_pending() const;  ///< the key the open dialog would assign
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
