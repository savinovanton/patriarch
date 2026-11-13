#pragma once

#include "pat/core/result.hpp"
#include "pat/engine/label_layout.hpp"
#include "pat/engine/renderer.hpp"
#include "pat/engine/sdf_font.hpp"
#include "pat/engine/terrain.hpp"
#include "pat/mapdata/map_data.hpp"

#include <memory>
#include <span>
#include <string>
#include <vector>

namespace pat::engine {

struct MapLabelSet {
    std::vector<MapLabel> labels;
    std::size_t province_labels = 0;
    std::size_t region_labels = 0;
};

/// Builds labels for a map: one per province (placed on a pixel that really belongs to the province,
/// closest to its centroid) and one per region (the provinces are clustered into about sqrt(N) regions).
/// The names are **placeholder test names** (a pool of Russian toponyms, repeated with numerals) - the real
/// names come with the historical data (ADR-0004).
[[nodiscard]] MapLabelSet make_test_map_labels(const mapdata::MapData& map,
                                               const TerrainGeometry& geometry,
                                               const HeightfieldPicker& picker,
                                               const SdfAtlas& atlas);

/// Builds province labels from UTF-8 names indexed by map province id, with index 0 reserved for water.
/// Missing names are skipped. Uses the same placement anchors and text measurement as the test labels.
[[nodiscard]] MapLabelSet make_province_labels(const mapdata::MapData& map,
                                               const HeightfieldPicker& picker,
                                               const SdfAtlas& atlas,
                                               std::span<const std::string> names);

/// Colours of the labels (E5-A3, docs/dev/map-art-direction.md): the ink of the names, and the halo drawn
/// around them. The halo is a contrasting underlay for dark ink, or a dark outline for light letters.
struct LabelTheme {
    Color region_fill{1.0f, 0.93f, 0.68f, 1.0f};
    Color region_halo{0.06f, 0.045f, 0.03f, 0.92f};
    Color province_fill{0.97f, 0.95f, 0.88f, 1.0f};
    Color province_halo{0.06f, 0.045f, 0.03f, 0.92f};
    TextHalo halo_shape;  ///< default: a crisp outline
};

/// The original look: cream letters with a dark outline (readable on any terrain; the default, also used
/// by the unstyled map).
[[nodiscard]] LabelTheme cream_label_theme();

/// The look of board 01 (winter map): region names and cities in the warm ink `#242e29` over a soft
/// light underlay instead of an outline.
[[nodiscard]] LabelTheme ink_label_theme();

/// The layout that goes with `ink_label_theme()`: region names tracked wide (5 px on 27 px type), a diamond
/// marker above each province (city) name.
[[nodiscard]] LabelStyle ink_label_style();

/// Draws the labels of a map: placement per frame, glyph layout and SDF text rendering.
class MapLabelRenderer {
public:
    [[nodiscard]] static core::Result<std::unique_ptr<MapLabelRenderer>>
    create(IRenderer& renderer, SdfAtlas atlas, MapLabelSet labels);

    /// Updates the label data between frames, reusing the existing atlas and text renderer.
    void replace_labels(MapLabelSet labels);

    /// Lays out the labels for this camera and draws them (between begin_frame and end_frame).
    LabelLayoutStats draw(const LabelLayoutInput& input,
                          const LabelStyle& style = {},
                          const LabelTheme& theme = cream_label_theme());

    [[nodiscard]] const std::vector<PlacedLabel>& last_placed() const noexcept { return placed_; }
    [[nodiscard]] const MapLabelSet& labels() const noexcept { return labels_; }
    [[nodiscard]] const SdfAtlas& atlas() const noexcept { return atlas_; }

private:
    MapLabelRenderer() = default;

    SdfAtlas atlas_;
    MapLabelSet labels_;
    std::unique_ptr<ITextRenderer> text_;
    std::vector<PlacedLabel> placed_;
    std::vector<TextQuadGeometry> geometry_;
    std::vector<TextQuad> quads_;
};

}  // namespace pat::engine
