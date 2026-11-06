#pragma once

#include "pat/sim/date.hpp"

#include <cstdint>

// Tick calendar and staggering helpers (E1-T04, ADR-0002 P2).
//
// Periodic systems run on calendar pulses (new week / month / quarter / year). To avoid one huge
// spike at the start of a month, entity-by-entity work is spread over the days of the period: each
// entity (identified by a dense integer id) belongs to a slot and is processed exactly once per period.

namespace pat::sim {

/// Which calendar boundaries the day that has just started crosses.
struct TickEvents {
    bool new_week = false;     // Monday
    bool new_month = false;    // 1st day of a month
    bool new_quarter = false;  // 1 January / April / July / October
    bool new_year = false;     // 1 January

    [[nodiscard]] constexpr bool any() const noexcept {
        return new_week || new_month || new_quarter || new_year;
    }
    [[nodiscard]] bool operator==(const TickEvents&) const = default;
};

/// Classifies a date (independent from any calendar state).
[[nodiscard]] constexpr TickEvents events_for(Date date) noexcept {
    const YearMonthDay v = date.ymd();
    TickEvents e;
    e.new_week = date.weekday() == Weekday::Monday;
    e.new_month = v.day == 1;
    e.new_quarter = v.day == 1 && (v.month == 1 || v.month == 4 || v.month == 7 || v.month == 10);
    e.new_year = v.day == 1 && v.month == 1;
    return e;
}

/// The simulation clock: one tick = one day.
class TickCalendar {
public:
    explicit constexpr TickCalendar(Date start) noexcept : today_(start) {}

    /// Rebuilds a clock from saved values (loading a save).
    [[nodiscard]] static constexpr TickCalendar restore(Date today, std::int64_t ticks) noexcept {
        TickCalendar c(today);
        c.ticks_ = ticks;
        return c;
    }

    [[nodiscard]] constexpr Date today() const noexcept { return today_; }
    /// Number of ticks executed since construction.
    [[nodiscard]] constexpr std::int64_t tick_count() const noexcept { return ticks_; }

    /// Moves to the next day and returns the boundaries crossed by the NEW date.
    constexpr TickEvents advance() noexcept {
        ++today_;
        ++ticks_;
        return events_for(today_);
    }

private:
    Date today_;
    std::int64_t ticks_ = 0;
};

// ---- staggering -----------------------------------------------------------------------------------
// All predicates answer: "should entity `id` be processed on `date`?". Every entity is selected
// exactly once per period, whatever the id, and the choice depends only on (id, date).

/// Calendar-aligned monthly pass: entities are spread over days 1..28 (id % 28 == day - 1). Days
/// 29..31 select nobody, so no entity is processed twice in a 30/31-day month.
[[nodiscard]] constexpr bool is_monthly_slot(std::uint32_t id, Date date) noexcept {
    const int day = date.day();
    return day <= 28 && id % 28U == static_cast<std::uint32_t>(day - 1);
}

/// Calendar-aligned weekly pass: id % 7 == weekday (Monday = 0).
[[nodiscard]] constexpr bool is_weekly_slot(std::uint32_t id, Date date) noexcept {
    return id % 7U == static_cast<std::uint32_t>(date.weekday());
}

/// Calendar-aligned yearly pass: id % 365 == day_of_year; the 366th day of a leap year selects nobody.
[[nodiscard]] constexpr bool is_yearly_slot(std::uint32_t id, Date date) noexcept {
    const int doy = date.day_of_year();
    return doy < 365 && id % 365U == static_cast<std::uint32_t>(doy);
}

/// Interval pass independent from the calendar: exactly once in every `period_days` consecutive days.
[[nodiscard]] constexpr bool is_due_every(std::uint32_t period_days, std::uint32_t id, Date date) noexcept {
    PAT_ASSERT(period_days > 0, "is_due_every with a zero period");
    if (period_days == 0) {
        return false;
    }
    const auto days =
        static_cast<std::uint64_t>(static_cast<std::int64_t>(date.days()) + (std::int64_t{1} << 40));
    return (days + id) % period_days == 0;
}

}  // namespace pat::sim
