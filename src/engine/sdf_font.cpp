#include "pat/engine/sdf_font.hpp"

#include "pat/core/log.hpp"

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_MODULE_H

#include <algorithm>
#include <fstream>
#include <iterator>

namespace pat::engine {

const SdfGlyph* SdfAtlas::find(char32_t codepoint) const noexcept {
    const auto it =
        std::lower_bound(glyphs.begin(), glyphs.end(), codepoint, [](const SdfGlyph& g, char32_t cp) {
            return g.codepoint < cp;
        });
    return (it != glyphs.end() && it->codepoint == codepoint) ? &*it : nullptr;
}

namespace {

core::Error ft_error(int code, const char* what, FT_Error err) {
    return core::Error{code, std::string(what) + " (FreeType error " + std::to_string(err) + ")"};
}

struct Library {
    FT_Library lib = nullptr;
    ~Library() {
        if (lib != nullptr) {
            FT_Done_FreeType(lib);
        }
    }
};

struct Face {
    FT_Face face = nullptr;
    ~Face() {
        if (face != nullptr) {
            FT_Done_Face(face);
        }
    }
};

struct PendingGlyph {
    SdfGlyph glyph;
    int bitmap_w = 0;
    int bitmap_h = 0;
    std::vector<std::uint8_t> bitmap;
    int x = 0;
    int y = 0;
};

int next_pow2(int v) {
    int p = 1;
    while (p < v) {
        p <<= 1;
    }
    return p;
}

}  // namespace

core::Result<SdfAtlas> build_sdf_atlas(const std::filesystem::path& font_file,
                                       const SdfAtlasOptions& options) {
    using R = core::Result<SdfAtlas>;

    std::ifstream in(font_file, std::ios::binary);
    if (!in) {
        return R::err(core::Error{100, "cannot open font " + font_file.generic_string()});
    }
    const std::vector<unsigned char> font_data((std::istreambuf_iterator<char>(in)),
                                               std::istreambuf_iterator<char>());

    Library library;
    if (const FT_Error e = FT_Init_FreeType(&library.lib); e != 0) {
        return R::err(ft_error(101, "FreeType initialisation failed", e));
    }
    FT_Int spread = options.spread;
    FT_Property_Set(library.lib, "sdf", "spread", &spread);  // distance range of the field, in pixels

    Face face;
    if (const FT_Error e = FT_New_Memory_Face(
            library.lib, font_data.data(), static_cast<FT_Long>(font_data.size()), 0, &face.face);
        e != 0) {
        return R::err(ft_error(102, "cannot parse the font", e));
    }
    if (const FT_Error e = FT_Set_Pixel_Sizes(face.face, 0, static_cast<FT_UInt>(options.font_px)); e != 0) {
        return R::err(ft_error(103, "cannot set the font size", e));
    }

    std::vector<PendingGlyph> pending;
    for (const auto& [first, last] : options.ranges) {
        for (char32_t cp = first; cp <= last; ++cp) {
            const FT_UInt index = FT_Get_Char_Index(face.face, cp);
            if (index == 0) {
                continue;  // the font has no such glyph
            }
            if (FT_Load_Glyph(face.face, index, FT_LOAD_DEFAULT | FT_LOAD_NO_HINTING) != 0) {
                continue;
            }
            FT_GlyphSlot slot = face.face->glyph;
            PendingGlyph pg;
            pg.glyph.codepoint = cp;
            pg.glyph.advance = static_cast<float>(slot->advance.x) / 64.0f;
            if (slot->format == FT_GLYPH_FORMAT_OUTLINE && FT_Render_Glyph(slot, FT_RENDER_MODE_SDF) == 0 &&
                slot->bitmap.width > 0 && slot->bitmap.rows > 0) {
                pg.bitmap_w = static_cast<int>(slot->bitmap.width);
                pg.bitmap_h = static_cast<int>(slot->bitmap.rows);
                pg.bitmap.resize(static_cast<std::size_t>(pg.bitmap_w) *
                                 static_cast<std::size_t>(pg.bitmap_h));
                for (int row = 0; row < pg.bitmap_h; ++row) {
                    const unsigned char* src =
                        slot->bitmap.buffer + static_cast<std::ptrdiff_t>(row) * slot->bitmap.pitch;
                    std::copy(src,
                              src + pg.bitmap_w,
                              pg.bitmap.begin() + static_cast<std::ptrdiff_t>(row) * pg.bitmap_w);
                }
                pg.glyph.width = static_cast<float>(pg.bitmap_w);
                pg.glyph.height = static_cast<float>(pg.bitmap_h);
                pg.glyph.bearing_x = static_cast<float>(slot->bitmap_left);
                pg.glyph.bearing_y = static_cast<float>(slot->bitmap_top);
            }
            pending.push_back(std::move(pg));
        }
    }
    if (pending.empty()) {
        return R::err(core::Error{104, "the font contains none of the requested glyphs"});
    }

    // Shelf packing, tallest glyphs first so shelves are used well.
    std::vector<std::size_t> order(pending.size());
    for (std::size_t i = 0; i < order.size(); ++i) {
        order[i] = i;
    }
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        return pending[a].bitmap_h > pending[b].bitmap_h;
    });
    constexpr int kPad = 1;
    const int atlas_w = options.max_width;
    int cursor_x = kPad;
    int cursor_y = kPad;
    int row_h = 0;
    for (const std::size_t i : order) {
        PendingGlyph& pg = pending[i];
        if (pg.bitmap_w == 0) {
            continue;
        }
        if (cursor_x + pg.bitmap_w + kPad > atlas_w) {
            cursor_x = kPad;
            cursor_y += row_h + kPad;
            row_h = 0;
        }
        pg.x = cursor_x;
        pg.y = cursor_y;
        cursor_x += pg.bitmap_w + kPad;
        row_h = std::max(row_h, pg.bitmap_h);
    }
    const int atlas_h = next_pow2(cursor_y + row_h + kPad);

    SdfAtlas atlas;
    atlas.width = atlas_w;
    atlas.height = atlas_h;
    atlas.pixels.assign(static_cast<std::size_t>(atlas_w) * static_cast<std::size_t>(atlas_h), 0);
    atlas.font_px = static_cast<float>(options.font_px);
    atlas.spread = static_cast<float>(options.spread);
    atlas.ascent = static_cast<float>(face.face->size->metrics.ascender) / 64.0f;
    atlas.descent = static_cast<float>(-face.face->size->metrics.descender) / 64.0f;
    atlas.line_height = static_cast<float>(face.face->size->metrics.height) / 64.0f;

    for (PendingGlyph& pg : pending) {
        for (int row = 0; row < pg.bitmap_h; ++row) {
            std::copy(pg.bitmap.begin() + static_cast<std::ptrdiff_t>(row) * pg.bitmap_w,
                      pg.bitmap.begin() + static_cast<std::ptrdiff_t>(row + 1) * pg.bitmap_w,
                      atlas.pixels.begin() + static_cast<std::ptrdiff_t>(pg.y + row) * atlas_w + pg.x);
        }
        if (pg.bitmap_w > 0) {
            pg.glyph.u0 = static_cast<float>(pg.x) / static_cast<float>(atlas_w);
            pg.glyph.v0 = static_cast<float>(pg.y) / static_cast<float>(atlas_h);
            pg.glyph.u1 = static_cast<float>(pg.x + pg.bitmap_w) / static_cast<float>(atlas_w);
            pg.glyph.v1 = static_cast<float>(pg.y + pg.bitmap_h) / static_cast<float>(atlas_h);
        }
        atlas.glyphs.push_back(pg.glyph);
    }
    std::sort(atlas.glyphs.begin(), atlas.glyphs.end(), [](const SdfGlyph& a, const SdfGlyph& b) {
        return a.codepoint < b.codepoint;
    });
    PAT_LOG_INFO("text",
                 "SDF atlas built: {} glyphs, {}x{} px, font size {} px",
                 atlas.glyphs.size(),
                 atlas.width,
                 atlas.height,
                 options.font_px);
    return R::ok(std::move(atlas));
}

std::u32string utf8_to_utf32(std::string_view text) {
    std::u32string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size();) {
        const auto c = static_cast<unsigned char>(text[i]);
        char32_t cp = 0xFFFD;
        std::size_t len = 1;
        if (c < 0x80) {
            cp = c;
        } else if ((c & 0xE0U) == 0xC0U && i + 1 < text.size()) {
            cp = ((c & 0x1FU) << 6U) | (static_cast<unsigned char>(text[i + 1]) & 0x3FU);
            len = 2;
        } else if ((c & 0xF0U) == 0xE0U && i + 2 < text.size()) {
            cp = ((c & 0x0FU) << 12U) | ((static_cast<unsigned char>(text[i + 1]) & 0x3FU) << 6U) |
                 (static_cast<unsigned char>(text[i + 2]) & 0x3FU);
            len = 3;
        } else if ((c & 0xF8U) == 0xF0U && i + 3 < text.size()) {
            cp = ((c & 0x07U) << 18U) | ((static_cast<unsigned char>(text[i + 1]) & 0x3FU) << 12U) |
                 ((static_cast<unsigned char>(text[i + 2]) & 0x3FU) << 6U) |
                 (static_cast<unsigned char>(text[i + 3]) & 0x3FU);
            len = 4;
        }
        out.push_back(cp);
        i += len;
    }
    return out;
}

float measure_text(const SdfAtlas& atlas, std::u32string_view text, float size_px) {
    const float scale = size_px / atlas.font_px;
    float width = 0.0f;
    for (const char32_t cp : text) {
        if (const SdfGlyph* g = atlas.find(cp)) {
            width += g->advance * scale;
        }
    }
    return width;
}

void layout_text(const SdfAtlas& atlas,
                 std::u32string_view text,
                 float size_px,
                 float center_x,
                 float baseline_y,
                 std::vector<TextQuadGeometry>& out) {
    const float scale = size_px / atlas.font_px;
    float pen = center_x - measure_text(atlas, text, size_px) * 0.5f;
    for (const char32_t cp : text) {
        const SdfGlyph* g = atlas.find(cp);
        if (g == nullptr) {
            continue;
        }
        if (g->width > 0.0f) {
            TextQuadGeometry q;
            q.x = pen + g->bearing_x * scale;
            q.y = baseline_y - g->bearing_y * scale;
            q.w = g->width * scale;
            q.h = g->height * scale;
            q.u0 = g->u0;
            q.v0 = g->v0;
            q.u1 = g->u1;
            q.v1 = g->v1;
            out.push_back(q);
        }
        pen += g->advance * scale;
    }
}

}  // namespace pat::engine
