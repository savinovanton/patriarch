#pragma once

// The game fonts (assets/fonts, PT Serif and PT Sans Caption, OFL-1.1) for the UI and map-label tests, so the
// tests no longer depend on fonts installed on the machine.

#include <filesystem>
#include <vector>

#ifndef PATRIARCH_ASSETS_DIR
#define PATRIARCH_ASSETS_DIR ""
#endif

namespace pat::test {

/// All four faces: the first one also becomes the fallback face of RmlUi.
inline std::vector<std::filesystem::path> game_font_files() {
    const std::filesystem::path dir = std::filesystem::path(PATRIARCH_ASSETS_DIR) / "fonts";
    return {dir / "pt-serif" / "PT_Serif-Web-Regular.ttf",
            dir / "pt-serif" / "PT_Serif-Web-Bold.ttf",
            dir / "pt-sans-caption" / "PT_Sans-Caption-Web-Regular.ttf",
            dir / "pt-sans-caption" / "PT_Sans-Caption-Web-Bold.ttf"};
}

inline std::filesystem::path game_label_font() {
    return std::filesystem::path(PATRIARCH_ASSETS_DIR) / "fonts" / "pt-serif" / "PT_Serif-Web-Bold.ttf";
}

inline bool game_fonts_present() {
    for (const auto& file : game_font_files()) {
        if (!std::filesystem::exists(file)) {
            return false;
        }
    }
    return true;
}

}  // namespace pat::test
