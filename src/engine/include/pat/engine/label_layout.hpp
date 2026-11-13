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

    // Letter-spacing (tracking) as a fraction of the font size: the region names of the art direction are
    // set wide (board 01: 5 px on 27 px). Zero keeps the glyph advances of the font.
    float region_spacing_em = 0.0f;
    float province_spacing_em = 0.0f;
    // City marker (an outlined diamond, board 01): the anchor is the marker centre and the name hangs below
    // it. Province labels get one when `province_marker` is set.
    bool province_marker = false;
    float marker_em = 1.04f;     ///< marker size relative to the label's font size (24 px on 23 px type)
    float marker_dy_em = 1.35f;  ///< distance from the marker centre to the centre of the name line
    // Room kept free around the UI plate of the selected province (design pixels of a 1080-pixel-high
    // screen, scaled with the screen height): the plate hangs below the marker, see ui::MapOverlays.
    float plate_half_width = 103.5f;
    float plate_top = -12.0f;
    float plate_bottom = 72.0f;
};

struct LabelLayoutInput {
    Mat4 view_proj = Mat4::identity();
    Vec3 eye;
    float viewport_w = 1.0f;
    float viewport_h = 1.0f;
    /// Pixels covered by one world unit at distance 1 (viewport_h / (2 tan(fov_y / 2))).
    float pixels_per_unit_at_one = 1.0f;
    /// The province whose holding plate is shown by the UI (`ui::MapOverlays`): its label is not drawn (the
    /// plate and marker replace it) and the plate's area is kept free of other labels. 0 = none.
    std::uint32_t selected_id = 0;
};

struct PlacedLabel {
    const MapLabel* label = nullptr;
    float x = 0.0f;  ///< screen centre, pixels
    float y = 0.0f;
    float size_px = 0.0f;
    float alpha = 1.0f;
    float width_px = 0.0f;
    float text_y = 0.0f;      ///< screen centre of the text line, pixels (below `y` when there is a marker)
    float marker_px = 0.0f;   ///< size of the city marker centred on (x, y); 0 = none
    float spacing_em = 0.0f;  ///< letter-spacing used for `width_px`, as a fraction of `size_px`
};

struct LabelLayoutStats {
    std::size_t candidates = 0;
    std::size_t placed = 0;
    std::size_t hidden_offscreen = 0;
    std::size_t hidden_zoom = 0;
    std::size_t hidden_overlap = 0;
    std::size_t hidden_selected = 0;  ///< the label of the selected province (replaced by the UI plate)
};

/// Computes the labels to draw. `out` is cleared first and ordered by descending priority (regions
/// first). Deterministic for the same input.
LabelLayoutStats layout_labels(std::span<const MapLabel> labels,
                               const LabelLayoutInput& input,
                               const LabelStyle& style,
                               std::vector<PlacedLabel>& out);

}  // namespace pat::engine
