#include "pat/engine/renderer.hpp"
#include "pat/engine/sky.hpp"

#include <doctest/doctest.h>

#include <cmath>

using namespace pat::engine;

TEST_CASE("Sky: the noon sun is south and as high as the latitude and season allow") {
    // Equinox (day 80): noon elevation = 90 - latitude, due south.
    const SunPosition equinox = solar_position(12.0f, 80);
    CHECK(equinox.elevation_deg == doctest::Approx(32.0f).epsilon(0.03));
    CHECK(equinox.azimuth_deg == doctest::Approx(180.0f).epsilon(0.01));
    // Summer solstice (about day 172): 90 - 58 + 23.44 = 55.4; winter (about day 355): 8.6.
    CHECK(solar_position(12.0f, 172).elevation_deg == doctest::Approx(55.4f).epsilon(0.03));
    CHECK(solar_position(12.0f, 355).elevation_deg == doctest::Approx(8.6f).epsilon(0.1));
    // At 58 degrees north the summer sun barely sets and the winter day is short.
    CHECK(solar_position(0.0f, 172).elevation_deg > -10.0f);
    CHECK(solar_position(0.0f, 355).elevation_deg < -40.0f);
    CHECK(solar_position(12.0f, 355).elevation_deg < solar_position(12.0f, 172).elevation_deg);
}

TEST_CASE("Sky: the sun rises in the east and sets in the west") {
    const SunPosition morning = solar_position(8.0f, 80);
    const SunPosition evening = solar_position(16.0f, 80);
    CHECK(morning.azimuth_deg > 90.0f);
    CHECK(morning.azimuth_deg < 180.0f);
    CHECK(evening.azimuth_deg > 180.0f);
    CHECK(evening.azimuth_deg < 270.0f);
    CHECK(morning.elevation_deg > 0.0f);
    // Symmetric around noon.
    CHECK(morning.elevation_deg == doctest::Approx(evening.elevation_deg).epsilon(0.001));
    // Always within range, azimuth in [0, 360).
    for (int day = 1; day <= 366; day += 13) {
        for (float hour = 0.0f; hour <= 24.0f; hour += 0.5f) {
            const SunPosition p = solar_position(hour, day);
            CHECK(p.elevation_deg >= -90.0f);
            CHECK(p.elevation_deg <= 90.0f);
            CHECK(p.azimuth_deg >= 0.0f);
            CHECK(p.azimuth_deg < 360.0f);
        }
    }
}

TEST_CASE("Sky: colours - no sunlight and a dark blue ambient at night, warm light at low sun") {
    const SkyColors night = sky_colors(-20.0f);
    CHECK(night.sun[0] == 0.0f);
    CHECK(night.sun[1] == 0.0f);
    CHECK(night.sun[2] == 0.0f);
    CHECK(night.ambient_mul[2] > night.ambient_mul[0]);  // bluish
    CHECK(night.ambient_mul[2] < 0.35f);

    const SkyColors noon = sky_colors(55.0f);
    CHECK(noon.ambient_mul[0] == doctest::Approx(1.0f).epsilon(0.001));
    CHECK(noon.sun[0] > 0.95f);

    const SkyColors sunset = sky_colors(4.0f);
    CHECK(sunset.sun[0] > sunset.sun[2] * 1.5f);  // red-orange
    CHECK(sunset.sun[0] > 0.0f);

    // The ambient light grows with the elevation.
    float previous = 0.0f;
    for (float el = -30.0f; el <= 60.0f; el += 5.0f) {
        const float value = sky_colors(el).ambient_mul[1];
        CHECK(value >= previous - 1.0e-6f);
        previous = value;
    }
}

TEST_CASE("Sky: MapLighting defaults keep the sun above the horizon") {
    const MapLighting light;
    CHECK(light.sun_elevation_deg > 0.0f);
}
