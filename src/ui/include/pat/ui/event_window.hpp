#pragma once

#include "pat/core/result.hpp"
#include "pat/engine/ui.hpp"
#include "pat/loc/localizer.hpp"
#include "pat/ui/explanation.hpp"

#include <cstddef>
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

/// One answer the player can give to an event. The texts are ready-made (the bridge of the event engine fills
/// them from the event's own string-table keys with its variables substituted); the window adds only its own
/// captions.
struct EventOptionView {
    std::string title;   ///< "Начать переговоры о браке"
    std::string detail;  ///< line under the title: the cost or a short explanation ("Расходы: 120 ₽ · ...")
    /// The consequences, shown as a tooltip before the choice (the automatic tooltip of the option, E11-T13).
    /// Empty (no label, terms or lines) means no tooltip.
    Explanation consequences;
    bool available = true;  ///< false: shown dimmed, cannot be chosen
    /// Why it cannot be chosen; replaces `detail` in red while `available` is false ("Не хватает 40 ₽").
    std::string unavailable_reason;
};

/// An event the player has to answer (the "important event" window, board 03 of the mockup). The shape is
/// what the UI bridge of the event engine (E11-T19) is meant to deliver: `EventView` of one queued modal
/// event with its options, no simulation types.
struct EventView {
    std::string id;    ///< opaque identity of the event instance; only the host reads it
    std::string kind;  ///< id of the label at the top ("family" -> `ui.event.kind.family` "СЕМЕЙНОЕ ДЕЛО")
    std::string category;  ///< id of the category in the footer ("house" -> `ui.event.category.house`)
    std::string title;     ///< "Продолжение рода"
    std::string subtitle;  ///< "Будущее семьи решается сегодня"
    std::string text;      ///< body; a newline is a line break, long lines wrap
    /// The person the event speaks about, on the left: portrait file name below `assets/ui/patriarch/art`
    /// (empty: no portrait), name, role line and a quote (any of them may be empty).
    std::string portrait;
    std::string speaker;
    std::string speaker_role;
    std::string quote;
    std::string date;                      ///< the date of the event, formatted: "1 января 1703"
    bool autopause = true;                 ///< the game pauses while the window is open ("Автопауза" badge)
    std::size_t default_option = 0;        ///< the option that starts highlighted
    std::vector<EventOptionView> options;  ///< up to `EventWindow::kMaxOptions`
};

/// What the window shows. `placeholder` is true while the event does not come from the event engine (E11 is
/// only specified): see make_placeholder_event().
struct EventSnapshot {
    EventView event;
    bool placeholder = false;
};

/// PLACEHOLDER event of the mockup (board 03: "Продолжение рода", three options) with the texts of the board
/// under the keys `ui.event.placeholder.*`. Replaced by the queue of the event engine (E11-T12, E11-T19).
[[nodiscard]] EventSnapshot make_placeholder_event(const loc::Localizer& loc);

/// The window of an important event over the dimmed map (UI-D07, board 03): modal, gold frame, label of the
/// kind, "Автопауза" badge, portrait with a quote on the left, title, subtitle, text, "Как вы поступите?" and
/// the options (number, title, line; the highlighted one is green), the date and the category at the bottom.
///
/// Input: the mouse highlights an option, a click chooses it; the keys 1-3 (and the arrows) highlight and
/// focus an option, Enter on the focused one chooses it; Tab stays inside the window; Esc does nothing. The
/// consequences of an option show as a tooltip while it is hovered or focused: the host passes
/// `explanation()` to the tooltip manager under the keys "event:0", "event:1", ....
class EventWindow {
public:
    static constexpr std::size_t kMaxOptions = 3;  ///< the board holds three options

    /// Creates the data model and loads `document`; `icons` is the directory with the SVG icons
    /// (`assets/ui/patriarch/icons`), the portraits are looked up in the sibling `art` directory. `loc`
    /// supplies the captions and must outlive the window. The window starts hidden. Destroy before the
    /// UiContext.
    [[nodiscard]] static core::Result<std::unique_ptr<EventWindow>>
    create(engine::UiContext& ui,
           const std::filesystem::path& document,
           const std::filesystem::path& icons,
           const loc::Localizer& loc);
    ~EventWindow();
    EventWindow(const EventWindow&) = delete;
    EventWindow& operator=(const EventWindow&) = delete;

    /// Shows `snapshot` as the open event: modal, the first option (or the default one) highlighted, and
    /// `on_event_shown` is called so that the host can pause the game.
    void show_event(const EventSnapshot& snapshot);
    /// Hides the window without choosing anything (the host took the event away).
    void close();
    [[nodiscard]] bool visible() const;
    [[nodiscard]] const EventSnapshot& snapshot() const;

    /// Called each time an event is shown, after the window is up; `event.autopause` says whether the host
    /// should pause the game (the TimeBar).
    void set_event_shown_handler(std::function<void(const EventView&)> handler);
    /// Called with the index of the chosen option after the window has been hidden.
    void set_choice_handler(std::function<void(std::size_t)> handler);

    /// Highlights option `index` (and focuses it when `focus`); ignored for an unavailable or missing option.
    void select(std::size_t index, bool focus = false);
    [[nodiscard]] std::size_t selected() const;
    /// Chooses option `index` as a click does: hides the window, then calls the choice handler. False when
    /// the window is hidden or the option is missing or unavailable.
    bool choose(std::size_t index);

    /// The consequences tooltip behind a tooltip key ("event:0"); null for other keys and options without
    /// one.
    [[nodiscard]] const Explanation* explanation(std::string_view key) const;

    /// Re-reads the captions from the Localizer (after a language change). The event keeps the language it
    /// was built in.
    void relocalize();

    [[nodiscard]] Rml::ElementDocument* document() const;

private:
    struct Impl;
    explicit EventWindow(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

}  // namespace pat::ui
