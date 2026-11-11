#pragma once

#include "pat/core/result.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The UI theme "Rodovaya kantselyariya" (issue #256, docs/dev/ui-design.md): design tokens read from
// data/ui/theme.pd, the RCSS generated from them and the rules that let window styles refer to tokens
// instead of literals. data/ui/theme.pd is the single source of truth: there are no hex colours in the
// window stylesheets (apps/spike/ui/*.rcss use `$name` placeholders), and the font families, sizes,
// spacing and shape come from the same file.

namespace pat::engine {
class UiContext;
}

namespace pat::ui {

struct ThemeColor {
    std::string name;  // token name from theme.pd: ink, panel, gold, green_hover ...
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
};

/// A numeric token in density-independent pixels, kept as thousandths (1.5 dp = 1500): no floating point.
struct ThemeNumber {
    std::string name;  // space_8, type_14, radius, stroke_icon ...
    std::int32_t milli = 0;
};

struct ThemeFont {
    std::string key;     // serif, sans
    std::string family;  // the family name stored in the font files ("PT Serif")
};

struct Theme {
    std::string id;  // label of the theme block, "patriarch-concept-01"
    int reference_width = 1920;
    int reference_height = 1080;
    std::vector<ThemeColor> colors;
    std::vector<ThemeNumber> numbers;  // spacing, font sizes (main and extra scale) and shape metrics
    std::vector<ThemeFont> fonts;
    std::int32_t line_height_milli = 1350;  // the line height multiplier

    [[nodiscard]] const ThemeColor* find_color(std::string_view name) const noexcept;
    [[nodiscard]] const ThemeNumber* find_number(std::string_view name) const noexcept;
    [[nodiscard]] const ThemeFont* find_font(std::string_view key) const noexcept;
};

/// "#1B2523".
[[nodiscard]] std::string color_hex(const ThemeColor& color);
/// "#1B2523F2": the colour with an alpha of `percent` (0..100).
[[nodiscard]] std::string color_hex_alpha(const ThemeColor& color, int percent);

/// Reads the theme block of a theme.pd text. Fails (with the position in the message) when the file does
/// not parse or lacks a token the generated stylesheet needs (the 17 colours, type_14, radius,
/// stroke_hairline, the serif and sans families).
[[nodiscard]] core::Result<Theme> parse_theme(std::string_view pd_text);
[[nodiscard]] core::Result<Theme> load_theme(const std::filesystem::path& file);

struct ExpandResult {
    std::string text;
    std::vector<std::string> unknown;  // tokens that are not in the theme (left in the text as written)
};

/// Replaces `$token` in a stylesheet: a colour (`$gold` -> `#D2B77D`, `$panel_a95` -> the colour with 95 %
/// alpha), a number (`$space_8`, `$type_14`, `$radius`, `$stroke_icon` -> `8dp`, `14dp`, `2dp`, `1.5dp`),
/// a font family (`$serif` -> `"PT Serif"`) or `$line_height` (`1.35`).
[[nodiscard]] ExpandResult expand_tokens(const Theme& theme, std::string_view rcss);

/// The stylesheet generated from the tokens (what a document gets for `<link href="theme.rcss"/>`): block
/// defaults, the base look (body, .panel, .heading, .muted) and utility classes `.fg-<colour>`,
/// `.bg-<colour>`, `.bd-<colour>`, `.type-<size>`, `.serif`, `.sans`.
[[nodiscard]] std::string generate_theme_rcss(const Theme& theme);

/// Repaints a monochrome SVG icon: every `stroke="#rrggbb"` and `fill="#rrggbb"` becomes `color`.
[[nodiscard]] std::string recolor_svg(std::string_view svg, const ThemeColor& color);

/// Teaches the UI context the theme: `theme.rcss` is generated, every other .rcss gets its `$tokens`
/// expanded, and `<icon>@<colour>.svg` is the icon file `<icon>.svg` repainted in the colour token
/// (for example `chevron@gold.svg`).
void install_theme(engine::UiContext& ui, std::shared_ptr<const Theme> theme);

}  // namespace pat::ui
