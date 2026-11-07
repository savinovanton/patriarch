#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

// Sun position and sky colours for the map (ADR-0003 phase P5): pure math, no rendering. The sun follows the
// real astronomy of the setting (Russia, about 58 degrees north) so that the length of the day and the
// height of the noon sun change with the season; the colours turn warm near the horizon and dark blue at
// night. Floating point is fine here: this is presentation only and never feeds the simulation.

namespace pat::engine {

inline constexpr float kDefaultLatitudeDeg = 58.0f;

struct SunPosition {
    float azimuth_deg = 180.0f;  ///< Clockwise from north (90 = east, 180 = south, 270 = west).
    float elevation_deg = 0.0f;  ///< Above the horizon; negative at night.
};

/// Sun position for a local solar `hour` (0..24, 12 = noon), `day_of_year` (1..366) and latitude.
[[nodiscard]] inline SunPosition
solar_position(float hour, int day_of_year, float latitude_deg = kDefaultLatitudeDeg) noexcept {
    constexpr float kPi = 3.14159265358979f;
    constexpr float kRad = kPi / 180.0f;
    const float declination =
        23.44f * kRad * std::sin(2.0f * kPi * (static_cast<float>(day_of_year) - 80.0f) / 365.0f);
    const float lat = latitude_deg * kRad;
    const float hour_angle = (hour - 12.0f) * 15.0f * kRad;
    const float sin_el =
        std::sin(lat) * std::sin(declination) + std::cos(lat) * std::cos(declination) * std::cos(hour_angle);
    SunPosition sun;
    sun.elevation_deg = std::asin(std::clamp(sin_el, -1.0f, 1.0f)) / kRad;
    float az = std::atan2(std::sin(hour_angle),
                          std::cos(hour_angle) * std::sin(lat) - std::tan(declination) * std::cos(lat)) /
                   kRad +
               180.0f;
    az = std::fmod(az, 360.0f);
    sun.azimuth_deg = az < 0.0f ? az + 360.0f : az;
    return sun;
}

struct SkyColors {
    std::array<float, 3> sun{};          ///< Sunlight colour times intensity (0 when the sun is down).
    std::array<float, 3> ambient_mul{};  ///< Multiplier of the day ambient light (dark blue at night).
    std::array<float, 3> haze{};         ///< Colour of the distance haze and the horizon.
};

[[nodiscard]] inline float smoothstep(float edge0, float edge1, float x) noexcept {
    const float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

/// Colours for a sun at `elevation_deg` above the horizon.
[[nodiscard]] inline SkyColors sky_colors(float elevation_deg) noexcept {
    const float warm = smoothstep(0.0f, 25.0f, elevation_deg);   // 0 at the horizon, 1 when the sun is high
    const float lit = smoothstep(-2.0f, 6.0f, elevation_deg);    // the sun fades in over the horizon
    const float day = smoothstep(-10.0f, 14.0f, elevation_deg);  // ambient follows twilight
    SkyColors c;
    const std::array<float, 3> low{1.00f, 0.52f, 0.24f};
    const std::array<float, 3> high{1.00f, 0.92f, 0.78f};
    const std::array<float, 3> night_ambient{0.15f, 0.19f, 0.33f};
    const std::array<float, 3> night_haze{0.05f, 0.07f, 0.13f};
    const std::array<float, 3> day_haze{0.63f, 0.71f, 0.80f};
    const std::array<float, 3> dusk_haze{0.78f, 0.55f, 0.45f};
    // Haze takes a pink tint when the sun is close to the horizon.
    const float dusk = (1.0f - warm) * lit;
    for (std::size_t i = 0; i < 3; ++i) {
        c.sun[i] = (low[i] + (high[i] - low[i]) * warm) * lit;
        c.ambient_mul[i] = night_ambient[i] + (1.0f - night_ambient[i]) * day;
        const float daylight = day_haze[i] + (dusk_haze[i] - day_haze[i]) * dusk;
        c.haze[i] = night_haze[i] + (daylight - night_haze[i]) * day;
    }
    return c;
}

}  // namespace pat::engine
