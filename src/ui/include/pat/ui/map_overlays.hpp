#pragma once

#include "pat/core/result.hpp"
#include "pat/engine/ui.hpp"
#include "pat/loc/localizer.hpp"

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace Rml {
class ElementDocument;
}

namespace pat::ui {

enum class Season : unsigned char { Winter, Spring, Summer, Autumn };

/// The part of the map the camera sees, as fractions (0..1) of the whole map: centre and size.
struct MinimapView {
    float center_x = 0.5f;
    float center_y = 0.5f;
    float width = 1.0f;
    float height = 1.0f;
};

/// The pictures of the minimap, two SVG files (the land silhouette and the realm of the player) made from
/// the map data (pat::game::build_overview_svg); `aspect` is the width of the map divided by its height.
struct MinimapOverview {
    std::filesystem::path land;
    std::filesystem::path owned;
    float aspect = 2.0f;
};

/// The things drawn over the map in the design "Rodovaya kantselyariya" (UI-D05, boards 01 and 07): the
/// season badge ("ЗИМА · 1703"), the caption over the realm ("НОВГОРОДСКАЯ ЗЕМЛЯ / ВЛАДЕНИЯ ВАШЕГО РОДА"),
/// the plate of the selected province ("Новгород", gold line) and the minimap "Обзор владений" with the view
/// rectangle, the scale and the buttons − and +. One document; the badge, the caption and the plate ignore
/// the mouse, the minimap takes it (a click moves the camera, the buttons zoom). The caption and the plate
/// are placed by the application over the screen position of a point of the map.
class MapOverlays {
public:
    [[nodiscard]] static core::Result<std::unique_ptr<MapOverlays>>
    create(engine::UiContext& ui,
           const std::filesystem::path& document,
           const std::filesystem::path& icons,
           const loc::Localizer& loc);
    ~MapOverlays();
    MapOverlays(const MapOverlays&) = delete;
    MapOverlays& operator=(const MapOverlays&) = delete;

    /// "ЗИМА · 1703".
    void set_season(Season season, int year);
    [[nodiscard]] static std::string_view season_key(Season season);

    /// The caption over the realm: `name` comes from content ("Новгородская земля"); empty hides it.
    /// `x`, `y` is the screen position (pixels) of the middle of the realm.
    void set_region(std::string_view name);
    void set_region_anchor(float x, float y);
    /// The plate of the selected province; empty hides it. `x`, `y` is the screen position (pixels) of the
    /// province (the marker); the plate hangs below it.
    void set_selection(std::string_view name);
    void set_selection_anchor(float x, float y);

    void set_overview(const MinimapOverview& overview);
    void set_view(const MinimapView& view);
    /// The length of the scale bar in kilometres ("100 км"); 0 hides the label.
    void set_scale_km(int km);

    /// Called with +1 (closer) or -1 (farther) when + or - is pressed.
    void set_zoom_handler(std::function<void(int)> handler);
    /// Called with the point of the map (fractions 0..1) that was clicked on the minimap.
    void set_jump_handler(std::function<void(float, float)> handler);
    void press_zoom(int direction);

    void relocalize();

    [[nodiscard]] Rml::ElementDocument* document() const;
    void show(bool visible);

private:
    struct Impl;
    explicit MapOverlays(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

}  // namespace pat::ui
