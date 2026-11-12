#pragma once

#include "pat/core/result.hpp"
#include "pat/engine/ui.hpp"
#include "pat/loc/localizer.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace Rml {
class ElementDocument;
}

namespace pat::ui {

/// One affair of the house in the "Дела рода" panel. Like the other snapshots it holds no ready-made phrases:
/// the texts are string-table keys with a number for plurals, the target is the key of the window it opens.
struct AgendaItem {
    std::string title_key;   ///< "Наследник не обручён"
    std::string detail_key;  ///< "Заключите союз с другим родом"; `{n}` and `{n:plural(...)}` take `count`
    std::int64_t count = 0;
    /// Navigation key of the related window ("dynasty", "court", "treasury", ...): the chip shows its name
    /// (`ui.nav.<section>`) and a click on the card opens it.
    std::string section;
    bool attention = false;  ///< needs the player's attention: counted in the badge
};

/// What the panel shows. `placeholder` is true while the data does not come from the world (the dynasty,
/// events and economy bridges of E7/E8/E9 do not exist yet): see make_placeholder_agenda().
struct AgendaSnapshot {
    std::vector<AgendaItem> items;
    bool placeholder = false;
};

/// PLACEHOLDER data of the mockup (board 01): three affairs with the texts of the board under the keys
/// `ui.agenda.placeholder.*`. Replace with the priority affairs of the world when the bridges exist.
[[nodiscard]] AgendaSnapshot make_placeholder_agenda();

/// The panel "Дела рода" over the map (UI-D05, boards 01 and 07): a bell, the title, a counter badge, the
/// subtitle, up to three cards (number, title, line, chip of the target window) and the link "Открыть
/// журнал". The cards are tab stops and open the window of their section through the handler; the empty parts
/// of the document do not take the mouse, so the map below stays operable.
class AgendaPanel {
public:
    [[nodiscard]] static core::Result<std::unique_ptr<AgendaPanel>>
    create(engine::UiContext& ui,
           const std::filesystem::path& document,
           const std::filesystem::path& icons,
           const loc::Localizer& loc);
    ~AgendaPanel();
    AgendaPanel(const AgendaPanel&) = delete;
    AgendaPanel& operator=(const AgendaPanel&) = delete;

    void set_snapshot(const AgendaSnapshot& snapshot);
    [[nodiscard]] const AgendaSnapshot& snapshot() const;
    /// Re-reads every text from the Localizer.
    void relocalize();

    /// Called with the section key of a card when it is pressed (mouse, Enter).
    void set_open_handler(std::function<void(const std::string&)> handler);
    /// Called when "Открыть журнал" is pressed.
    void set_journal_handler(std::function<void()> handler);
    /// What a press of card `index` does (the handler with its section).
    void press_item(std::size_t index);
    void press_journal();

    /// The number in the badge: the affairs that need attention.
    [[nodiscard]] int attention_count() const;

    [[nodiscard]] Rml::ElementDocument* document() const;
    void show(bool visible);

private:
    struct Impl;
    explicit AgendaPanel(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

}  // namespace pat::ui
