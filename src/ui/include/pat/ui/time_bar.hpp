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

/// Date and game-speed controls (pause, 1-5) bound to an RmlUi data model (E3-T12). The bar only shows
/// what it is told and reports button presses; it knows nothing about the simulation. The caption of the
/// pause button comes from the string table (ui.timebar.pause); the date text is given ready-made.
class TimeBar {
public:
    /// Creates the data model and loads `document`. `loc` supplies the texts and must outlive the bar.
    /// Destroy before the UiContext.
    [[nodiscard]] static core::Result<std::unique_ptr<TimeBar>>
    create(engine::UiContext& ui, const std::filesystem::path& document, const loc::Localizer& loc);
    ~TimeBar();
    TimeBar(const TimeBar&) = delete;
    TimeBar& operator=(const TimeBar&) = delete;

    /// Re-reads the texts from the Localizer (after a language change). The date is set by the caller.
    void relocalize();

    void set_date(const std::string& text);
    /// 0 = paused, 1..5 = game speed.
    void set_speed(int speed);
    [[nodiscard]] int speed() const;
    [[nodiscard]] const std::string& date() const;

    /// Called with the requested speed (0..5) when a button is pressed.
    void set_speed_handler(std::function<void(int)> handler);

    [[nodiscard]] Rml::ElementDocument* document() const;

private:
    struct Impl;
    explicit TimeBar(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

}  // namespace pat::ui
