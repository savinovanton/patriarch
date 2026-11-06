#pragma once

#include <algorithm>
#include <cmath>

namespace pat::engine {

/// Smallest and largest UI scale factors the interface supports (100% = 1.0).
inline constexpr float kMinUiScale = 0.5f;
inline constexpr float kMaxUiScale = 4.0f;

/// Pixel dimensions of a surface.
struct PixelSize {
    int w = 0;
    int h = 0;
    [[nodiscard]] constexpr bool operator==(const PixelSize&) const = default;
};

/// Effective UI scale: OS display scale (per-monitor DPI, 1.0 = 100%) times the user's own
/// setting, clamped to the supported range and snapped to 1/8 steps so glyph sizes stay stable.
[[nodiscard]] inline float effective_ui_scale(float display_scale, float user_scale = 1.0f) noexcept {
    const float raw = std::clamp(display_scale * user_scale, kMinUiScale, kMaxUiScale);
    return std::round(raw * 8.0f) / 8.0f;
}

/// Size of a surface in UI ("logical") units at the given scale. Always at least 1x1.
[[nodiscard]] inline PixelSize logical_size(PixelSize pixels, float scale) noexcept {
    const float s = std::max(scale, kMinUiScale);
    return {std::max(1, static_cast<int>(std::lround(static_cast<float>(pixels.w) / s))),
            std::max(1, static_cast<int>(std::lround(static_cast<float>(pixels.h) / s)))};
}

/// Size in pixels that corresponds to a logical size at the given scale. Always at least 1x1.
[[nodiscard]] inline PixelSize physical_size(PixelSize logical, float scale) noexcept {
    const float s = std::max(scale, kMinUiScale);
    return {std::max(1, static_cast<int>(std::lround(static_cast<float>(logical.w) * s))),
            std::max(1, static_cast<int>(std::lround(static_cast<float>(logical.h) * s)))};
}

}  // namespace pat::engine
