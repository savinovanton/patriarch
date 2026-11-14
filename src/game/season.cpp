#include "pat/game/season.hpp"

#include <algorithm>
#include <array>

namespace pat::game {

namespace {

// Zero-based day of a 365-day year (1 January = 0); the leap day shares the number of 28 February.
constexpr std::array<int, 12> kMonthStart = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};

constexpr int day_index(int month, int day) noexcept {
    return kMonthStart[static_cast<std::size_t>(month - 1)] + day - 1;
}

constexpr int kThawStart = day_index(3, 15);
constexpr int kThawEnd = day_index(5, 1);
constexpr int kSnowStart = day_index(10, 15);
constexpr int kSnowEnd = day_index(12, 1);
constexpr int kTintStart = day_index(9, 1);
constexpr int kTintPeak = day_index(10, 5);
constexpr int kTintEnd = day_index(11, 15);

int day_of_season_year(sim::Date date) noexcept {
    const sim::YearMonthDay v = date.ymd();
    return day_index(v.month, v.month == 2 ? std::min(v.day, 28) : v.day);
}

// 0 at `from`, 1 at `to`, smooth at both ends.
float ramp(int day, int from, int to) noexcept {
    const float t = std::clamp(static_cast<float>(day - from) / static_cast<float>(to - from), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

}  // namespace

float winter_factor(sim::Date date) {
    const int day = day_of_season_year(date);
    if (day < kThawStart) {
        return 1.0f;
    }
    if (day < kThawEnd) {
        return 1.0f - ramp(day, kThawStart, kThawEnd);
    }
    if (day < kSnowStart) {
        return 0.0f;
    }
    return ramp(day, kSnowStart, kSnowEnd);
}

float autumn_tint(sim::Date date) {
    const int day = day_of_season_year(date);
    if (day < kTintStart || day >= kTintEnd) {
        return 0.0f;
    }
    return day < kTintPeak ? ramp(day, kTintStart, kTintPeak) : 1.0f - ramp(day, kTintPeak, kTintEnd);
}

SeasonLook season_look(sim::Date date) {
    return SeasonLook{winter_factor(date), autumn_tint(date)};
}

sim::Date season_sample_date(ui::Season season, int year) {
    switch (season) {
        case ui::Season::Winter:
            return sim::Date::from_ymd(year, 1, 15);
        case ui::Season::Spring:
            return sim::Date::from_ymd(year, 4, 10);
        case ui::Season::Summer:
            return sim::Date::from_ymd(year, 7, 15);
        case ui::Season::Autumn:
            break;
    }
    return sim::Date::from_ymd(year, 10, 5);
}

}  // namespace pat::game
