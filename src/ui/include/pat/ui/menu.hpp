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

/// The screens of the start menu (UI-D12, #284; boards "МЕНЮ / …"). Part 1 holds the main menu, "Загрузка",
/// "Об игре" and the overview page of the mockup; "Новая игра", "Цель партии" and "Настройки" follow in the
/// next parts and join the same stack.
enum class MenuScreen { Main, Load, About, Overview };

/// A dialog over the current screen; the screen below stays but takes neither mouse nor Tab.
enum class MenuDialog { None, Exit, SaveUnavailable };

/// What Esc did (the screen stack answers it so that the rules can be tested without a window).
enum class MenuBack { Nothing, ClosedDialog, WentBack, AskedExit };

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

/// PLACEHOLDER data of the mockup (boards 02 and 07: the autosave and a manual save of the Golitsyns, one
/// save of a newer version); the texts are the `ui.menu.placeholder.*` keys. Replace with the save catalogue
/// when the loading of games exists.
[[nodiscard]] MenuSnapshot make_placeholder_menu(const loc::Localizer& loc);

/// The actions that need a world. Part 1 only calls them (the spike logs them); the next parts connect "Новая
/// игра" to the screen of the same name and "Настройки" to the settings. Every callback may be empty.
struct MenuActions {
    std::function<void()> new_game;
    std::function<void()> continue_game;                ///< "Продолжить": the newest save
    std::function<void(const std::string&)> load_save;  ///< "Загрузить историю": the id of a compatible save
    std::function<void()> open_settings;
    std::function<void()> quit;  ///< "Выйти из игры" confirmed
};

/// The start menu on RmlUi: one full-screen document (apps/spike/ui/menu.rml) that shows the current screen
/// of a MenuNavigator. A modal document: while it is shown the windows below get neither mouse nor keys.
/// Keys: Up and Down move the highlight (main menu, list of saves), Tab walks the controls, Enter presses,
/// Esc goes back (closes the dialog, leaves the screen, on the main menu asks to exit).
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
    void set_actions(MenuActions actions);
    /// Re-reads every text from the Localizer.
    void relocalize();

    /// Shows the menu at `root` (the stack is reset) with the keyboard focus on the first control.
    void open(MenuScreen root = MenuScreen::Main);
    void close();
    [[nodiscard]] bool visible() const;

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
