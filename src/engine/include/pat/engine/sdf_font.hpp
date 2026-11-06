#pragma once

#include "pat/core/result.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Signed-distance-field font atlas and text layout (E3-T05). Map labels use SDF text so that one small
// atlas stays sharp from tiny to large sizes and gets an outline for free. Pure CPU code: the atlas is
// built with FreeType's SDF renderer and uploaded to the GPU by the renderer backend.

namespace pat::engine {

/// One glyph in the atlas. All sizes are in atlas pixels (at SdfAtlas::font_px) and include the SDF spread
/// padding around the outline, so a quad of (width, height) at (bearing_x, -bearing_y) from the pen
/// position shows exactly the stored bitmap.
struct SdfGlyph {
    char32_t codepoint = 0;
    float u0 = 0.0f;
    float v0 = 0.0f;
    float u1 = 0.0f;
    float v1 = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
    float bearing_x = 0.0f;  ///< left edge relative to the pen position
    float bearing_y = 0.0f;  ///< top edge above the baseline
    float advance = 0.0f;
};

struct SdfAtlas {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> pixels;  ///< R8; 128 = on the glyph outline, larger = inside
    std::vector<SdfGlyph> glyphs;      ///< sorted by code point
    float font_px = 0.0f;              ///< the pixel size glyphs were rendered at
    float spread = 0.0f;  ///< distance (in atlas pixels) mapped to the full 0..255 range on each side
    float ascent = 0.0f;
    float descent = 0.0f;
    float line_height = 0.0f;

    [[nodiscard]] const SdfGlyph* find(char32_t codepoint) const noexcept;
};

struct SdfAtlasOptions {
    int font_px = 40;
    int spread = 8;
    int max_width = 1024;
    /// Inclusive code point ranges to include. The default covers Basic Latin, Cyrillic (incl. Ё/ё) and
    /// the punctuation used in names and numbers.
    std::vector<std::pair<char32_t, char32_t>> ranges = {{0x20, 0x7E},
                                                         {0x0401, 0x0401},
                                                         {0x0410, 0x044F},
                                                         {0x0451, 0x0451},
                                                         {0x00AB, 0x00AB},
                                                         {0x00BB, 0x00BB},
                                                         {0x2013, 0x2014},
                                                         {0x2026, 0x2026},
                                                         {0x2116, 0x2116},
                                                         {0x00B7, 0x00B7}};
};

/// Renders the glyphs of a TrueType/OpenType file into an SDF atlas.
[[nodiscard]] core::Result<SdfAtlas> build_sdf_atlas(const std::filesystem::path& font_file,
                                                     const SdfAtlasOptions& options = {});

/// One glyph rectangle in screen pixels with its atlas coordinates.
struct TextQuadGeometry {
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;
    float u0 = 0.0f;
    float v0 = 0.0f;
    float u1 = 0.0f;
    float v1 = 0.0f;
};

[[nodiscard]] std::u32string utf8_to_utf32(std::string_view text);

/// Width of the text in pixels when the font is drawn at `size_px` (the em size, i.e. atlas font_px
/// scales to size_px). Glyphs missing from the atlas are skipped.
[[nodiscard]] float measure_text(const SdfAtlas& atlas, std::u32string_view text, float size_px);

/// Appends the glyph quads of a single line of text. `center_x` is the horizontal centre of the line,
/// `baseline_y` the baseline in screen pixels (y grows downwards).
void layout_text(const SdfAtlas& atlas,
                 std::u32string_view text,
                 float size_px,
                 float center_x,
                 float baseline_y,
                 std::vector<TextQuadGeometry>& out);

}  // namespace pat::engine
