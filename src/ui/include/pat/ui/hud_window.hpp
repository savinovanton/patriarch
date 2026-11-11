#pragma once

#include "pat/core/result.hpp"
#include "pat/engine/ui.hpp"
#include "pat/loc/localizer.hpp"
#include "pat/ui/hud_snapshot.hpp"

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace Rml {
class ElementDocument;
}

namespace pat::ui {

/// The frame of the map screen in the design "Rodovaya kantselyariya" (UI-D04, board 01): the top bar with
/// the logo, the house and five resources, the left navigation (nine entries and settings) and the bottom bar
/// with the map modes and the mouse hints. Date, pause and speed are the neighbouring window `TimeBar`. Bound
/// to a HudSnapshot through an RmlUi data model; the window only shows what it is told and reports presses.
/// Every visible string comes from the string table (`ui.*` keys of data/loc, docs/dev/localization.md); a
/// missing key shows as [[key]]. Entries without a screen are shown dimmed with the hint "Скоро" and do
/// nothing.
class HudWindow {
public:
    /// Creates the data model and loads `document`. `icons` is the directory with the SVG icons
    /// (assets/ui/patriarch/icons), written into the document as a path relative to it. `loc` supplies the
    /// texts and must outlive the window. Destroy before the UiContext.
    [[nodiscard]] static core::Result<std::unique_ptr<HudWindow>>
    create(engine::UiContext& ui,
           const std::filesystem::path& document,
           const std::filesystem::path& icons,
           const loc::Localizer& loc);
    ~HudWindow();
    HudWindow(const HudWindow&) = delete;
    HudWindow& operator=(const HudWindow&) = delete;

    void set_snapshot(const HudSnapshot& snapshot);
    [[nodiscard]] const HudSnapshot& snapshot() const;

    /// Re-reads every text from the Localizer: call after the language chain or the tables changed.
    void relocalize();

    /// Keys of the left navigation, top to bottom: dynasty, holdings, court, treasury, church, army,
    /// diplomacy, intrigue, chronicle; then `system` (settings, the gear at the bottom).
    static constexpr int kNavCount = 9;
    [[nodiscard]] static std::string_view nav_key(int index);  // 0..kNavCount-1, kNavCount is "system"
    /// Display name of an entry in the current language (ui.nav.<key>); empty for a bad index.
    [[nodiscard]] std::string nav_name(int index) const;

    /// The active entry (raised, gold indicator); empty for none.
    void set_active_nav(std::string_view key);
    [[nodiscard]] std::string_view active_nav() const;
    /// An entry whose screen exists. The others are dimmed, are no tab stops and ignore presses.
    void set_nav_available(std::string_view key, bool available);
    [[nodiscard]] bool nav_available(std::string_view key) const;
    /// Called with the key ("dynasty", ...) when an available entry is pressed.
    void set_nav_handler(std::function<void(const std::string&)> handler);
    /// What a press or a hotkey does for `key`: the handler (when available) and the active entry change.
    void press_nav(std::string_view key);

    /// Map modes of the bottom bar: political, relief, income, faith.
    static constexpr int kModeCount = 4;
    [[nodiscard]] static std::string_view mode_key(int index);
    void set_map_mode(std::string_view key);
    [[nodiscard]] std::string_view map_mode() const;
    void set_mode_available(std::string_view key, bool available);
    [[nodiscard]] bool mode_available(std::string_view key) const;
    /// The next available mode after the current one (the M key); the current one when it is the only one.
    [[nodiscard]] std::string_view next_mode() const;
    /// Called with the mode key when an available mode is pressed.
    void set_mode_handler(std::function<void(const std::string&)> handler);
    void press_mode(std::string_view key);

    [[nodiscard]] Rml::ElementDocument* document() const;
    void show(bool visible);

private:
    struct Impl;
    explicit HudWindow(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

}  // namespace pat::ui
