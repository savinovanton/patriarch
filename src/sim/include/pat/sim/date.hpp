#pragma once

#include "pat/core/assert.hpp"

#include <compare>
#include <cstdint>
#include <format>
#include <limits>
#include <string>

// Game calendar (E1-T04, TZ FR-TIME). Russia used the Julian calendar ("old style") until 1918, so
// the simulation counts days in the Julian calendar; the Gregorian equivalent is available for display.
//
//   * Day 0 is 1 January 1700 (Julian), a Monday. The year starts on 1 January (Peter I's reform, 1700).
//   * Leap years: every year divisible by 4 (Julian rule; 1700 and 1800 ARE leap years).
//   * One tick of the simulation is one day.

namespace pat::sim {

struct YearMonthDay {
    int year = 0;
    int month = 1;  // 1..12
    int day = 1;    // 1..31

    [[nodiscard]] bool operator==(const YearMonthDay&) const = default;
};

/// Monday is 0 so that the value can be used directly as a weekly stagger slot.
enum class Weekday : std::uint8_t { Monday, Tuesday, Wednesday, Thursday, Friday, Saturday, Sunday };

class Date {
public:
    static constexpr int kEpochYear = 1700;
    /// Julian Day Number of the epoch (1 January 1700, Julian calendar).
    static constexpr std::int32_t kEpochJdn = 2341983;

    constexpr Date() noexcept = default;

    /// Days since 1 January 1700 (Julian).
    [[nodiscard]] static constexpr Date from_days(std::int32_t days) noexcept {
        Date d;
        d.days_ = days;
        return d;
    }

    [[nodiscard]] static constexpr Date from_ymd(int year, int month, int day) noexcept {
        PAT_ASSERT(is_valid_ymd(year, month, day), "invalid date {}-{}-{}", year, month, day);
        return from_days(days_from_ymd(year, month, day));
    }

    [[nodiscard]] static constexpr bool is_leap_year(int year) noexcept { return year % 4 == 0; }

    [[nodiscard]] static constexpr int days_in_month(int year, int month) noexcept {
        constexpr int kLengths[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
        return (month == 2 && is_leap_year(year)) ? 29 : kLengths[month - 1];
    }

    [[nodiscard]] static constexpr bool is_valid_ymd(int year, int month, int day) noexcept {
        return month >= 1 && month <= 12 && day >= 1 && day <= days_in_month(year, month);
    }

    [[nodiscard]] constexpr std::int32_t days() const noexcept { return days_; }

    [[nodiscard]] constexpr YearMonthDay ymd() const noexcept {
        constexpr std::int32_t kCycle = 1461;  // 4 Julian years, the first one is a leap year
        const std::int32_t block = floor_div(days_, kCycle);
        const std::int32_t rem = days_ - block * kCycle;
        std::int32_t year_in_block = 0;
        std::int32_t day_of_year = rem;
        if (rem >= 366) {
            year_in_block = 1 + (rem - 366) / 365;
            day_of_year = (rem - 366) % 365;
        }
        const int year = kEpochYear + 4 * block + year_in_block;

        int month = 1;
        int remaining = day_of_year;
        while (remaining >= days_in_month(year, month)) {
            remaining -= days_in_month(year, month);
            ++month;
        }
        return YearMonthDay{year, month, remaining + 1};
    }

    [[nodiscard]] constexpr int year() const noexcept { return ymd().year; }
    [[nodiscard]] constexpr int month() const noexcept { return ymd().month; }
    [[nodiscard]] constexpr int day() const noexcept { return ymd().day; }

    /// Zero-based day of the year (0 = 1 January).
    [[nodiscard]] constexpr int day_of_year() const noexcept {
        const YearMonthDay v = ymd();
        return static_cast<int>(days_ - days_from_ymd(v.year, 1, 1));
    }

    [[nodiscard]] constexpr Weekday weekday() const noexcept {
        const std::int32_t m = ((days_ % 7) + 7) % 7;  // 1 January 1700 was a Monday
        return static_cast<Weekday>(m);
    }

    /// Julian Day Number (astronomical day count), handy for conversions.
    [[nodiscard]] constexpr std::int32_t julian_day_number() const noexcept { return kEpochJdn + days_; }

    /// The same day in the Gregorian calendar (for display only; the simulation never uses it).
    [[nodiscard]] constexpr YearMonthDay gregorian() const noexcept {
        const std::int64_t a = static_cast<std::int64_t>(julian_day_number()) + 32044;
        const std::int64_t b = (4 * a + 3) / 146097;
        const std::int64_t c = a - 146097 * b / 4;
        const std::int64_t d = (4 * c + 3) / 1461;
        const std::int64_t e = c - 1461 * d / 4;
        const std::int64_t m = (5 * e + 2) / 153;
        return YearMonthDay{static_cast<int>(100 * b + d - 4800 + m / 10),
                            static_cast<int>(m + 3 - 12 * (m / 10)),
                            static_cast<int>(e - (153 * m + 2) / 5 + 1)};
    }

    /// First day of the following month.
    [[nodiscard]] constexpr Date first_of_next_month() const noexcept {
        const YearMonthDay v = ymd();
        return from_days(days_ - (v.day - 1) + days_in_month(v.year, v.month));
    }

    [[nodiscard]] constexpr Date operator+(std::int32_t delta) const noexcept {
        return from_days(days_ + delta);
    }
    [[nodiscard]] constexpr Date operator-(std::int32_t delta) const noexcept {
        return from_days(days_ - delta);
    }
    constexpr Date& operator+=(std::int32_t delta) noexcept {
        days_ += delta;
        return *this;
    }
    constexpr Date& operator++() noexcept {
        ++days_;
        return *this;
    }
    /// Number of days between two dates (this - other).
    [[nodiscard]] constexpr std::int32_t operator-(Date other) const noexcept { return days_ - other.days_; }

    [[nodiscard]] friend constexpr auto operator<=>(const Date&, const Date&) noexcept = default;
    [[nodiscard]] friend constexpr bool operator==(const Date&, const Date&) noexcept = default;

private:
    [[nodiscard]] static constexpr std::int32_t floor_div(std::int32_t a, std::int32_t b) noexcept {
        const std::int32_t q = a / b;
        return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
    }

    [[nodiscard]] static constexpr std::int32_t days_from_ymd(int year, int month, int day) noexcept {
        constexpr int kBeforeMonth[12] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
        const std::int32_t leap_days = floor_div(year - 1, 4) - floor_div(kEpochYear - 1, 4);
        std::int32_t days = (year - kEpochYear) * 365 + leap_days;
        days += kBeforeMonth[month - 1];
        if (month > 2 && is_leap_year(year)) {
            days += 1;
        }
        return days + (day - 1);
    }

    std::int32_t days_ = 0;
};

// ---- anniversaries (E7-T02, ADR-0006 D4) --------------------------------------------------------
// Ages and the life-event schedule use these functions only, so that "how old is X" and "when is X's
// next birthday" can never disagree. A birthday on 29 February is celebrated on 28 February in
// common years.

/// The day on which `birth` is celebrated in `year`.
[[nodiscard]] constexpr Date anniversary_in_year(Date birth, int year) noexcept {
    const YearMonthDay v = birth.ymd();
    const int day = (v.month == 2 && v.day == 29 && !Date::is_leap_year(year)) ? 28 : v.day;
    return Date::from_ymd(year, v.month, day);
}

/// Whole years lived on `on`: the number of anniversaries that are <= `on`. Zero for dates up to
/// and including the birth date (and before it).
[[nodiscard]] constexpr int age_years(Date birth, Date on) noexcept {
    if (on <= birth) {
        return 0;
    }
    const int years = on.year() - birth.year();
    return years - (on < anniversary_in_year(birth, on.year()) ? 1 : 0);
}

/// First anniversary (1 year or more after the birth) that is strictly later than `after`.
[[nodiscard]] constexpr Date next_anniversary_after(Date birth, Date after) noexcept {
    const int first_year = birth.year() + 1;
    const int year = after.year() > first_year ? after.year() : first_year;
    Date candidate = anniversary_in_year(birth, year);
    if (candidate <= after) {
        candidate = anniversary_in_year(birth, year + 1);
    }
    return candidate;
}

/// First anniversary (1 year or more after the birth) that is `on` or later.
[[nodiscard]] constexpr Date next_anniversary_on_or_after(Date birth, Date on) noexcept {
    const int first_year = birth.year() + 1;
    const int year = on.year() > first_year ? on.year() : first_year;
    Date candidate = anniversary_in_year(birth, year);
    if (candidate < on) {
        candidate = anniversary_in_year(birth, year + 1);
    }
    return candidate;
}

/// "No date" (no death date, no planned death, a province slot that was never processed). Never pass
/// it to calendar functions.
inline constexpr Date kNoDate = Date::from_days(std::numeric_limits<std::int32_t>::min());

/// "1703-01-01" (Julian calendar, ISO-like layout) for logs and tools. Localized text (month names)
/// belongs to the UI layer.
[[nodiscard]] std::string to_string(Date date);

}  // namespace pat::sim

template <>
struct std::formatter<pat::sim::Date> : std::formatter<std::string> {
    auto format(pat::sim::Date d, auto& ctx) const {
        return std::formatter<std::string>::format(pat::sim::to_string(d), ctx);
    }
};
