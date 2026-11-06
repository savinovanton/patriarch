#pragma once

#include "pat/core/result.hpp"
#include "pat/engine/ui.hpp"
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

/// The heads-up display of the map screen (design "Olive Stone"): realm plaque, resource strip, alert
/// badges, the left menu and the screen frame. Bound to a HudSnapshot through an RmlUi data model; the
/// window only shows what it is told and reports menu presses.
class HudWindow {
public:
    /// Creates the data model and loads `document`. `olive_assets` is the directory with the frame and
    /// the icons (assets/ui/olive); it is written into the document as a path relative to `document`.
    /// Destroy before the UiContext.
    [[nodiscard]] static core::Result<std::unique_ptr<HudWindow>>
    create(engine::UiContext& ui,
           const std::filesystem::path& document,
           const std::filesystem::path& olive_assets);
    ~HudWindow();
    HudWindow(const HudWindow&) = delete;
    HudWindow& operator=(const HudWindow&) = delete;

    void set_snapshot(const HudSnapshot& snapshot);
    [[nodiscard]] const HudSnapshot& snapshot() const;

    /// Keys of the left menu, top to bottom: world, realm, holdings, army, church, economy, diplomacy,
    /// chronicle, system.
    static constexpr int kMenuCount = 9;
    [[nodiscard]] static std::string_view menu_key(int index);
    [[nodiscard]] static std::string_view menu_name(int index);  // Russian display name

    void set_active_menu(std::string_view key);
    [[nodiscard]] std::string_view active_menu() const;

    /// Called with the menu key ("realm", ...) when a menu button is pressed.
    void set_menu_handler(std::function<void(const std::string&)> handler);

    [[nodiscard]] Rml::ElementDocument* document() const;
    void show(bool visible);

private:
    struct Impl;
    explicit HudWindow(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

}  // namespace pat::ui
