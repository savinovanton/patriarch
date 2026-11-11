#pragma once

#include "pat/core/result.hpp"
#include "pat/engine/ui.hpp"
#include "pat/loc/localizer.hpp"

#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace Rml {
class ElementDocument;
}

namespace pat::ui {

/// Date and game-speed controls of the top bar (UI-D04, board 01): the date with the calendar caption, the
/// "НА ПАУЗЕ" badge, the pause button, five speed segments and the hint "Пробел — продолжить". Bound to an
/// RmlUi data model; the bar only shows what it is told and reports presses; it knows nothing about the
/// simulation. While paused the segments keep showing the speed the game resumes at. Texts come from the
/// string table (`ui.timebar.*`, `ui.hud.calendar`); the date text is given ready-made.
class TimeBar {
public:
    /// Creates the data model and loads `document`; `icons` is the directory with the SVG icons
    /// (assets/ui/patriarch/icons). `loc` supplies the texts and must outlive the bar. Destroy before the
    /// UiContext.
    [[nodiscard]] static core::Result<std::unique_ptr<TimeBar>> create(engine::UiContext& ui,
                                                                       const std::filesystem::path& document,
                                                                       const std::filesystem::path& icons,
                                                                       const loc::Localizer& loc);
    ~TimeBar();
    TimeBar(const TimeBar&) = delete;
    TimeBar& operator=(const TimeBar&) = delete;

    /// Re-reads the texts from the Localizer (after a language change). The date is set by the caller.
    void relocalize();

    void set_date(const std::string& text);
    /// 0 = paused, 1..5 = game speed.
    void set_speed(int speed);
    [[nodiscard]] int speed() const;
    /// The speed a press of the pause button or Space resumes at: the last running speed (1 at the start).
    [[nodiscard]] int resume_speed() const;
    [[nodiscard]] const std::string& date() const;

    /// Called with the requested speed (0..5) when a button is pressed.
    void set_speed_handler(std::function<void(int)> handler);
    /// What the pause button and the Space key do: pause when running, resume when paused.
    void toggle_pause();
    /// A press of speed `speed` (0..5) from the keyboard: the same as the button, so the handler decides.
    void press_speed(int speed);

    [[nodiscard]] Rml::ElementDocument* document() const;

private:
    struct Impl;
    explicit TimeBar(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

}  // namespace pat::ui
