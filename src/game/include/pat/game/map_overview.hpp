#pragma once

#include "pat/mapdata/map_data.hpp"
#include "pat/sim/date.hpp"
#include "pat/sim/world.hpp"
#include "pat/ui/map_overlays.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The data behind the overlays of the map screen (UI-D05, #260): the season of a date, the silhouette of the
// map for the minimap, the part of the map the camera sees and the length for the scale label. Pure functions
// over existing map data and World snapshots; nothing here changes the world or defines a new map format.

namespace pat::game {

/// Winter for December-February, spring for March-May, summer for June-August, autumn for September-November
/// (Julian calendar months as in the simulation).
[[nodiscard]] ui::Season season_of(sim::Date date);

/// PLACEHOLDER: the width of the whole map in kilometres until the map data carries a horizontal scale.
inline constexpr int kPlaceholderMapWidthKm = 4000;

/// An SVG of the map at `cols` x `cols * height / width` cells: every cell whose centre is covered by a
/// province for which `include[province id]` is not 0 becomes a filled square (the runs of a row are merged
/// into one rectangle, so a map takes a few thousand of them). `fill` is an SVG colour text. An empty
/// `include` selects every land pixel (province != 0).
[[nodiscard]] std::string build_overview_svg(const mapdata::MapData& map,
                                             std::size_t cols,
                                             std::string_view fill,
                                             std::span<const std::uint8_t> include = {});

/// For every map province id (index 0 unused, size map_province_count + 1) 1 when the province belongs to
/// `dynasty` in the world, else 0. `map_to_world` as in owner_palette.
[[nodiscard]] std::vector<std::uint8_t> owned_provinces(const sim::World& world,
                                                        std::size_t map_province_count,
                                                        std::span<const sim::ProvinceId> map_to_world,
                                                        sim::DynastyId dynasty);

/// The footprint of a camera on the ground, seen from above, as fractions of the map: `focus` in map units,
/// `distance` from the eye to the focus, `fov_y` (radians) and the viewport aspect. The tilt of the camera is
/// ignored (the minimap is a schematic).
[[nodiscard]] ui::MinimapView minimap_view(float focus_x,
                                           float focus_y,
                                           float distance,
                                           float fov_y,
                                           float aspect,
                                           float map_width,
                                           float map_height);

/// The largest "round" length (1, 2 or 5 times a power of ten kilometres) that is not longer than `km`; 0 for
/// `km` below 1.
[[nodiscard]] int round_distance_km(double km);

/// The length of the scale label: a fifth of the visible width of the map, rounded with round_distance_km.
[[nodiscard]] int scale_label_km(const ui::MinimapView& view, int map_width_km);

}  // namespace pat::game
