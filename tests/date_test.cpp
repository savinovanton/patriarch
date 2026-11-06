#include "pat/sim/date.hpp"

#include <doctest/doctest.h>

#include <format>

using namespace pat::sim;

TEST_CASE("date: the epoch is Monday 1 January 1700 (Julian)") {
    const Date epoch;
    CHECK(epoch.days() == 0);
    CHECK(epoch.ymd() == YearMonthDay{1700, 1, 1});
    CHECK(epoch.weekday() == Weekday::Monday);
    CHECK(epoch.day_of_year() == 0);
}

TEST_CASE("date: known day numbers and weekdays") {
    // Reference values computed independently from the Julian Day Number.
    CHECK(Date::from_ymd(1700, 2, 29).days() == 59);  // 1700 IS a leap year in the Julian calendar
    CHECK(Date::from_ymd(1700, 2, 29).weekday() == Weekday::Thursday);
    CHECK(Date::from_ymd(1703, 1, 1).days() == 1096);  // scenario start
    CHECK(Date::from_ymd(1703, 1, 1).weekday() == Weekday::Friday);
    CHECK(Date::from_ymd(1703, 12, 25).days() == 1454);
    CHECK(Date::from_ymd(1703, 12, 25).weekday() == Weekday::Saturday);
    CHECK(Date::from_ymd(1704, 2, 29).days() == 1520);
    CHECK(Date::from_ymd(1704, 2, 29).weekday() == Weekday::Tuesday);
    CHECK(Date::from_ymd(1709, 6, 27).days() == 3465);  // Poltava
    CHECK(Date::from_ymd(1709, 6, 27).weekday() == Weekday::Monday);
    CHECK(Date::from_ymd(1721, 8, 30).days() == 7912);  // Treaty of Nystad
    CHECK(Date::from_ymd(1721, 8, 30).weekday() == Weekday::Wednesday);
    CHECK(Date::from_ymd(1780, 1, 1).days() == 29220);  // end of the default play window
}

TEST_CASE("date: Julian leap years") {
    CHECK(Date::is_leap_year(1700));  // not a leap year in the Gregorian calendar, but is in the Julian one
    CHECK(Date::is_leap_year(1704));
    CHECK(Date::is_leap_year(1800));
    CHECK_FALSE(Date::is_leap_year(1701));
    CHECK_FALSE(Date::is_leap_year(1703));
    CHECK(Date::days_in_month(1700, 2) == 29);
    CHECK(Date::days_in_month(1703, 2) == 28);
    CHECK(Date::days_in_month(1703, 12) == 31);
    CHECK(Date::days_in_month(1703, 4) == 30);
}

TEST_CASE("date: validation") {
    CHECK(Date::is_valid_ymd(1704, 2, 29));
    CHECK_FALSE(Date::is_valid_ymd(1701, 2, 29));
    CHECK_FALSE(Date::is_valid_ymd(1703, 13, 1));
    CHECK_FALSE(Date::is_valid_ymd(1703, 0, 1));
    CHECK_FALSE(Date::is_valid_ymd(1703, 4, 31));
    CHECK_FALSE(Date::is_valid_ymd(1703, 4, 0));
}

TEST_CASE("date: every day between 1690 and 1800 round-trips and advances by one") {
    Date d = Date::from_ymd(1690, 1, 1);
    const Date end = Date::from_ymd(1801, 1, 1);
    YearMonthDay previous{};
    int count = 0;
    while (d < end) {
        const YearMonthDay v = d.ymd();
        REQUIRE(Date::is_valid_ymd(v.year, v.month, v.day));
        REQUIRE(Date::from_ymd(v.year, v.month, v.day) == d);
        if (count > 0) {
            // exactly one day after the previous one
            const Date before = d - 1;
            REQUIRE(before.ymd() == previous);
        }
        previous = v;
        ++d;
        ++count;
    }
    CHECK(count == 40543);  // 111 years * 365 days + 28 leap days
}

TEST_CASE("date: weekday cycles through seven values") {
    Date d = Date::from_ymd(1703, 1, 1);
    for (int i = 0; i < 70; ++i) {
        const auto expected =
            static_cast<Weekday>((static_cast<int>(Date::from_ymd(1703, 1, 1).weekday()) + i) % 7);
        REQUIRE(d.weekday() == expected);
        ++d;
    }
}

TEST_CASE("date: dates before the epoch work") {
    const Date d = Date::from_ymd(1699, 12, 31);
    CHECK(d.days() == -1);
    CHECK(d.weekday() == Weekday::Sunday);
    CHECK(Date::from_days(-1).ymd() == YearMonthDay{1699, 12, 31});
    CHECK(Date::from_days(-365).ymd() == YearMonthDay{1699, 1, 1});  // 1699 has 365 days
    CHECK(Date::from_days(-366).ymd() == YearMonthDay{1698, 12, 31});
    CHECK(Date::from_days(-1461).ymd() == YearMonthDay{1696, 1, 1});  // a full 4-year Julian cycle back
}

TEST_CASE("date: arithmetic and ordering") {
    const Date a = Date::from_ymd(1703, 1, 1);
    CHECK((a + 365) == Date::from_ymd(1704, 1, 1));  // 1703 has 365 days
    CHECK((Date::from_ymd(1704, 1, 1) + 366) == Date::from_ymd(1705, 1, 1));
    CHECK(Date::from_ymd(1704, 3, 1) - Date::from_ymd(1704, 2, 1) == 29);
    CHECK(a < a + 1);
    CHECK(a + 10 - 10 == a);
    Date b = a;
    b += 31;
    CHECK(b == Date::from_ymd(1703, 2, 1));
}

TEST_CASE("date: first day of the next month") {
    CHECK(Date::from_ymd(1703, 1, 15).first_of_next_month() == Date::from_ymd(1703, 2, 1));
    CHECK(Date::from_ymd(1703, 2, 28).first_of_next_month() == Date::from_ymd(1703, 3, 1));
    CHECK(Date::from_ymd(1704, 2, 10).first_of_next_month() == Date::from_ymd(1704, 3, 1));
    CHECK(Date::from_ymd(1703, 12, 31).first_of_next_month() == Date::from_ymd(1704, 1, 1));
}

TEST_CASE("date: Gregorian equivalents for display") {
    CHECK(Date::from_ymd(1700, 1, 1).gregorian() == YearMonthDay{1700, 1, 11});
    CHECK(Date::from_ymd(1700, 2, 29).gregorian() == YearMonthDay{1700, 3, 11});
    CHECK(Date::from_ymd(1703, 1, 1).gregorian() == YearMonthDay{1703, 1, 12});  // +11 days since March 1700
    CHECK(Date::from_ymd(1703, 12, 25).gregorian() == YearMonthDay{1704, 1, 5});
    CHECK(Date::from_ymd(1709, 6, 27).gregorian() == YearMonthDay{1709, 7, 8});  // Poltava, O.S. -> N.S.
    CHECK(Date::from_ymd(1721, 8, 30).gregorian() == YearMonthDay{1721, 9, 10});
}

TEST_CASE("date: text form") {
    CHECK(to_string(Date::from_ymd(1703, 1, 1)) == "1703-01-01");
    CHECK(to_string(Date::from_ymd(1709, 6, 27)) == "1709-06-27");
    CHECK(std::format("{}", Date::from_ymd(1704, 2, 29)) == "1704-02-29");
}
