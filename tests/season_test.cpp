// The look of the land through the year (E5-A4, #452): the winter factor and the autumn tint of a Julian
// date.

#include "pat/game/map_overview.hpp"
#include "pat/game/season.hpp"

#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>

namespace {

using namespace pat;

sim::Date d(int year, int month, int day) {
    return sim::Date::from_ymd(year, month, day);
}

}  // namespace

TEST_CASE("season: the game starts in winter") {
    CHECK(game::winter_factor(d(1703, 1, 1)) == doctest::Approx(1.0f));
    CHECK(game::autumn_tint(d(1703, 1, 1)) == doctest::Approx(0.0f));
}

TEST_CASE("season: winter factor by date (table, with the boundaries of the ramps)") {
    struct Row {
        int month;
        int day;
        float winter;
    };
    const Row table[] = {
        {1, 1, 1.0f},
        {2, 28, 1.0f},
        {3, 14, 1.0f},  // deep winter
        {3, 15, 1.0f},  // the thaw starts without a jump
        {5, 1, 0.0f},   // and is over on 1 May
        {5, 2, 0.0f},
        {6, 1, 0.0f},
        {7, 15, 0.0f},
        {8, 31, 0.0f},
        {10, 14, 0.0f},  // summer
        {10, 15, 0.0f},  // the first snow starts
        {12, 1, 1.0f},   // and has settled on 1 December
        {12, 2, 1.0f},
        {12, 31, 1.0f},
    };
    for (const Row& row : table) {
        INFO("date 1703-" << row.month << "-" << row.day);
        CHECK(game::winter_factor(d(1703, row.month, row.day)) ==
              doctest::Approx(row.winter).epsilon(0.0001));
    }
    // The middle of a ramp is in the middle (smoothstep: 0.5 half way).
    CHECK(game::winter_factor(d(1703, 4, 8)) == doctest::Approx(0.5f).epsilon(0.05));   // day 97 of 73..120
    CHECK(game::winter_factor(d(1703, 11, 8)) == doctest::Approx(0.5f).epsilon(0.05));  // day 311 of 287..334
}

TEST_CASE("season: transitions are monotonic and never leave 0..1") {
    // One whole (leap) year, day by day: winter falls through the thaw, rises with the first snow, is flat
    // otherwise.
    float previous = game::winter_factor(d(1704, 1, 1));
    bool thaw_done = false;
    bool snow_started = false;
    for (std::int32_t day = d(1704, 1, 2).days(); day <= d(1704, 12, 31).days(); ++day) {
        const sim::Date date = sim::Date::from_days(day);
        const float now = game::winter_factor(date);
        CHECK(now >= 0.0f);
        CHECK(now <= 1.0f);
        const int month = date.month();
        if (month <= 4 || (month == 5 && date.day() == 1)) {
            CHECK(now <= previous);  // melting: never back up
            thaw_done = thaw_done || now == 0.0f;
        } else if (month <= 9 || (month == 10 && date.day() <= 15)) {
            CHECK(now == 0.0f);
        } else {
            CHECK(now >= previous);  // first snow: never back down
            snow_started = snow_started || now > 0.0f;
        }
        CHECK(std::abs(now - previous) <
              0.06f);  // smooth: no day-to-day jump, whatever the speed of the game
        previous = now;
    }
    CHECK(thaw_done);
    CHECK(snow_started);
    CHECK(previous == doctest::Approx(1.0f));
}

TEST_CASE("season: the year turns over without a jump, leap days included (Julian calendar)") {
    // 1700 is a leap year in the Julian calendar, as are 1704 and 1708.
    for (const int year : {1700, 1703, 1704, 1708}) {
        CHECK(game::winter_factor(d(year, 12, 31)) == game::winter_factor(d(year + 1, 1, 1)));
    }
    CHECK(game::winter_factor(d(1700, 2, 29)) == doctest::Approx(1.0f));
    // The same calendar date gives the same value in a leap and in an ordinary year.
    CHECK(game::winter_factor(d(1704, 4, 20)) == game::winter_factor(d(1703, 4, 20)));
    CHECK(game::winter_factor(d(1704, 11, 20)) == game::winter_factor(d(1703, 11, 20)));
    CHECK(game::winter_factor(d(1704, 7, 1)) == game::winter_factor(d(1703, 7, 1)));
}

TEST_CASE("season: autumn tint is a bump in September-November and zero in winter, spring and summer") {
    CHECK(game::autumn_tint(d(1703, 8, 31)) == 0.0f);
    CHECK(game::autumn_tint(d(1703, 9, 1)) == doctest::Approx(0.0f));
    CHECK(game::autumn_tint(d(1703, 10, 5)) == doctest::Approx(1.0f));
    CHECK(game::autumn_tint(d(1703, 11, 15)) == 0.0f);
    CHECK(game::autumn_tint(d(1703, 1, 15)) == 0.0f);
    CHECK(game::autumn_tint(d(1703, 4, 10)) == 0.0f);  // spring and autumn differ in the picture
    CHECK(game::autumn_tint(d(1703, 7, 15)) == 0.0f);
    float previous = 0.0f;
    for (int day = 1; day <= 30; ++day) {  // September rises monotonically
        const float now = game::autumn_tint(d(1703, 9, day));
        CHECK(now >= previous);
        previous = now;
    }
}

TEST_CASE("season: sample dates are in their season and look the part") {
    using ui::Season;
    for (const Season season : {Season::Winter, Season::Spring, Season::Summer, Season::Autumn}) {
        CHECK(game::season_of(game::season_sample_date(season, 1703)) == season);
    }
    CHECK(game::winter_factor(game::season_sample_date(Season::Winter, 1703)) == doctest::Approx(1.0f));
    CHECK(game::winter_factor(game::season_sample_date(Season::Summer, 1703)) == doctest::Approx(0.0f));
    const float spring = game::winter_factor(game::season_sample_date(Season::Spring, 1703));
    CHECK(spring > 0.2f);
    CHECK(spring < 0.8f);
    const game::SeasonLook autumn = game::season_look(game::season_sample_date(Season::Autumn, 1703));
    CHECK(autumn.winter == doctest::Approx(0.0f));
    CHECK(autumn.autumn == doctest::Approx(1.0f));
}

TEST_CASE("season: look is a pure function of the date") {
    const game::SeasonLook a = game::season_look(d(1703, 4, 20));
    const game::SeasonLook b = game::season_look(d(1703, 4, 20));
    CHECK(a.winter == b.winter);
    CHECK(a.autumn == b.autumn);
}
