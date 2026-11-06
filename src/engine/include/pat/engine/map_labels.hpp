#pragma once

#include "pat/core/result.hpp"
#include "pat/engine/label_layout.hpp"
#include "pat/engine/renderer.hpp"
#include "pat/engine/sdf_font.hpp"
#include "pat/engine/terrain.hpp"
#include "pat/mapdata/map_data.hpp"

#include <memory>
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

/// Draws the labels of a map: placement per frame, glyph layout and SDF text rendering.
class MapLabelRenderer {
public:
    [[nodiscard]] static core::Result<std::unique_ptr<MapLabelRenderer>>
    create(IRenderer& renderer, SdfAtlas atlas, MapLabelSet labels);

    /// Lays out the labels for this camera and draws them (between begin_frame and end_frame).
    LabelLayoutStats draw(const LabelLayoutInput& input, const LabelStyle& style = {});

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
