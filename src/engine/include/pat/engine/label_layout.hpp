#pragma once

#include "pat/engine/math.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

// Map label placement (E3-T05): which labels are visible for the current camera, how big they are and
// which of them survive overlap culling. Pure CPU code, independent of the renderer.

namespace pat::engine {

enum class LabelKind : std::uint8_t {
    Region,    ///< a group of provinces; visible when zoomed out
    Province,  ///< one province; visible when zoomed in
};

struct MapLabel {
    std::uint32_t id = 0;
    LabelKind kind = LabelKind::Province;
    std::u32string text;
    Vec3 anchor;                ///< world position the label is attached to (on the terrain)
    float world_extent = 0.0f;  ///< approximate width of the labelled area in world units
    float width_per_px = 0.0f;  ///< text width at a font size of 1 px (precomputed from the font)
};

struct LabelStyle {
    // Province labels fade in when the province is this wide on screen (pixels).
    float province_min_px = 45.0f;
    float province_full_px = 75.0f;
    // Region labels fade in at a smaller size and fade out again when provinces take over.
    float region_min_px = 70.0f;
    float region_full_px = 110.0f;
    float region_fade_out_start_px = 520.0f;
    float region_fade_out_end_px = 820.0f;
    // Font size follows the labelled area: size = clamp(extent_px * size_factor, min, max).
    float size_factor = 0.20f;
    float province_min_size = 11.0f;
    float province_max_size = 28.0f;
    float region_min_size = 18.0f;
    float region_max_size = 46.0f;
    float padding = 4.0f;     ///< extra space kept free around a label (pixels)
    float min_alpha = 0.04f;  ///< labels fainter than this are dropped
};

struct LabelLayoutInput {
    Mat4 view_proj = Mat4::identity();
    Vec3 eye;
    float viewport_w = 1.0f;
    float viewport_h = 1.0f;
    /// Pixels covered by one world unit at distance 1 (viewport_h / (2 tan(fov_y / 2))).
    float pixels_per_unit_at_one = 1.0f;
};

struct PlacedLabel {
    const MapLabel* label = nullptr;
    float x = 0.0f;  ///< screen centre, pixels
    float y = 0.0f;
    float size_px = 0.0f;
    float alpha = 1.0f;
    float width_px = 0.0f;
};

struct LabelLayoutStats {
    std::size_t candidates = 0;
    std::size_t placed = 0;
    std::size_t hidden_offscreen = 0;
    std::size_t hidden_zoom = 0;
    std::size_t hidden_overlap = 0;
};

/// Computes the labels to draw. `out` is cleared first and ordered by descending priority (regions
/// first). Deterministic for the same input.
LabelLayoutStats layout_labels(std::span<const MapLabel> labels,
                               const LabelLayoutInput& input,
                               const LabelStyle& style,
                               std::vector<PlacedLabel>& out);

}  // namespace pat::engine
