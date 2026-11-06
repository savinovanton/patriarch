#include "pat/sim/calendar.hpp"
#include "pat/sim/scheduled_queue.hpp"

#include <doctest/doctest.h>

#include <string>
#include <vector>

using namespace pat::sim;

TEST_CASE("calendar: tick advance reports crossed boundaries") {
    TickCalendar cal(Date::from_ymd(1703, 12, 30));
    CHECK(cal.tick_count() == 0);

    const TickEvents d31 = cal.advance();  // 31 December 1703 (Sunday)
    CHECK(cal.today() == Date::from_ymd(1703, 12, 31));
    CHECK_FALSE(d31.new_month);
    CHECK_FALSE(d31.new_year);

    const TickEvents jan1 = cal.advance();  // 1 January 1704 (a Saturday: 25 December 1703 was one too)
    CHECK(cal.today() == Date::from_ymd(1704, 1, 1));
    CHECK(jan1.new_year);
    CHECK(jan1.new_quarter);
    CHECK(jan1.new_month);
    CHECK_FALSE(jan1.new_week);
    CHECK(cal.tick_count() == 2);

    // Monday 3 January 1704 starts a week without starting a month.
    (void)cal.advance();  // Sunday 2 January
    const TickEvents monday = cal.advance();
    CHECK(cal.today().weekday() == Weekday::Monday);
    CHECK(monday.new_week);
    CHECK_FALSE(monday.new_month);
}

TEST_CASE("calendar: boundary counts over four years") {
    Date start = Date::from_ymd(1703, 1, 1);
    TickCalendar cal(start);
    int weeks = 0;
    int months = 0;
    int quarters = 0;
    int years = 0;
    const int days = 365 + 366 + 365 + 365;  // 1703..1706 (1704 is a leap year)
    for (int i = 0; i < days; ++i) {
        const TickEvents e = cal.advance();
        weeks += e.new_week ? 1 : 0;
        months += e.new_month ? 1 : 0;
        quarters += e.new_quarter ? 1 : 0;
        years += e.new_year ? 1 : 0;
    }
    // Advanced from 1703-01-01 to 1707-01-01: crossed 48 month starts, 16 quarter starts, 4 year starts.
    CHECK(cal.today() == Date::from_ymd(1707, 1, 1));
    CHECK(months == 48);
    CHECK(quarters == 16);
    CHECK(years == 4);
    CHECK(weeks == 209);  // 1461 days contain 208 or 209 Mondays; 1703-01-02 is a Saturday -> 209
}

TEST_CASE("calendar: events_for is a pure function of the date") {
    CHECK(events_for(Date::from_ymd(1703, 4, 1)).new_quarter);
    CHECK_FALSE(events_for(Date::from_ymd(1703, 5, 1)).new_quarter);
    CHECK(events_for(Date::from_ymd(1703, 5, 1)).new_month);
    const TickEvents mid_month = events_for(Date::from_ymd(1703, 5, 2));
    CHECK_FALSE(mid_month.new_month);
    CHECK_FALSE(mid_month.new_quarter);
    CHECK_FALSE(mid_month.new_year);
}

namespace {

// Counts how often each id in [0, ids) is selected over [start, start + days).
template <class Predicate>
std::vector<int> selection_counts(std::uint32_t ids, Date start, int days, Predicate selected) {
    std::vector<int> counts(ids, 0);
    for (int i = 0; i < days; ++i) {
        const Date d = start + i;
        for (std::uint32_t id = 0; id < ids; ++id) {
            if (selected(id, d)) {
                ++counts[id];
            }
        }
    }
    return counts;
}

}  // namespace

TEST_CASE("stagger: monthly slots select every id exactly once per calendar month") {
    // 28-, 29-, 30- and 31-day months, including February of a leap year.
    for (const Date month_start : {Date::from_ymd(1703, 2, 1),
                                   Date::from_ymd(1704, 2, 1),
                                   Date::from_ymd(1703, 4, 1),
                                   Date::from_ymd(1703, 12, 1)}) {
        const int length = month_start.first_of_next_month() - month_start;
        const auto counts = selection_counts(1000, month_start, length, is_monthly_slot);
        for (const int c : counts) {
            REQUIRE(c == 1);
        }
    }
    // Days 29..31 select nobody.
    for (std::uint32_t id = 0; id < 100; ++id) {
        CHECK_FALSE(is_monthly_slot(id, Date::from_ymd(1703, 12, 31)));
        CHECK_FALSE(is_monthly_slot(id, Date::from_ymd(1703, 12, 29)));
    }
}

TEST_CASE("stagger: weekly slots select every id exactly once per week") {
    const auto counts = selection_counts(500, Date::from_ymd(1703, 1, 1), 7, is_weekly_slot);
    for (const int c : counts) {
        CHECK(c == 1);
    }
}

TEST_CASE("stagger: yearly slots select every id exactly once per year") {
    for (const Date year_start :
         {Date::from_ymd(1703, 1, 1), Date::from_ymd(1704, 1, 1)}) {  // normal and leap year
        const int length = (year_start.year() % 4 == 0) ? 366 : 365;
        const auto counts = selection_counts(800, year_start, length, is_yearly_slot);
        for (const int c : counts) {
            REQUIRE(c == 1);
        }
    }
}

TEST_CASE("stagger: interval slots select every id exactly once per period") {
    const std::uint32_t period = 30;
    for (const int offset : {0, 7, 1000}) {
        const Date start = Date::from_ymd(1703, 1, 1) + offset;
        const auto counts =
            selection_counts(200, start, static_cast<int>(period), [&](std::uint32_t id, Date d) {
                return is_due_every(period, id, d);
            });
        for (const int c : counts) {
            CHECK(c == 1);
        }
    }
}

TEST_CASE("scheduled queue: due order, insertion order for ties, partial pops") {
    ScheduledQueue<std::string> queue;
    const Date d1 = Date::from_ymd(1703, 3, 1);
    queue.push(d1 + 5, "e");
    queue.push(d1, "a");
    queue.push(d1 + 5, "f");
    queue.push(d1 + 2, "c");
    queue.push(d1, "b");
    queue.push(d1 + 2, "d");
    CHECK(queue.size() == 6);
    CHECK(queue.next_due() == d1);

    std::vector<std::string> order;
    const auto handler = [&](Date, std::string item) { order.push_back(std::move(item)); };

    CHECK(queue.pop_due(d1 - 1, handler) == 0);  // nothing due yet
    CHECK(queue.pop_due(d1 + 2, handler) == 4);
    CHECK(order == std::vector<std::string>{"a", "b", "c", "d"});
    CHECK(queue.next_due() == d1 + 5);

    CHECK(queue.pop_due(d1 + 100, handler) == 2);
    CHECK(order == std::vector<std::string>{"a", "b", "c", "d", "e", "f"});
    CHECK(queue.empty());
    CHECK_FALSE(queue.next_due().has_value());
}

TEST_CASE("scheduled queue: handlers may schedule more work, including work that is already due") {
    ScheduledQueue<int> queue;
    const Date now = Date::from_ymd(1703, 6, 1);
    queue.push(now, 1);

    std::vector<int> seen;
    queue.pop_due(now, [&](Date, int value) {
        seen.push_back(value);
        if (value < 3) {
            queue.push(now, value + 1);         // due immediately: processed in this pass
            queue.push(now + 10, value + 100);  // future: stays queued
        }
    });
    CHECK(seen == std::vector<int>{1, 2, 3});
    CHECK(queue.size() == 2);
}

TEST_CASE("scheduled queue: snapshot is canonical and restore reproduces the same behaviour") {
    ScheduledQueue<int> original;
    const Date base = Date::from_ymd(1710, 1, 1);
    for (int i = 0; i < 20; ++i) {
        original.push(base + (i * 7) % 5, i);
    }

    const auto snap = original.snapshot();
    for (std::size_t i = 1; i < snap.size(); ++i) {
        const bool ordered = snap[i - 1].due < snap[i].due ||
                             (snap[i - 1].due == snap[i].due && snap[i - 1].sequence < snap[i].sequence);
        REQUIRE(ordered);
    }

    ScheduledQueue<int> restored;
    restored.restore(snap, original.next_sequence());

    std::vector<int> a;
    std::vector<int> b;
    original.pop_due(base + 100, [&](Date, int v) { a.push_back(v); });
    restored.pop_due(base + 100, [&](Date, int v) { b.push_back(v); });
    CHECK(a == b);
    CHECK(a.size() == 20);

    // New pushes after a restore continue the sequence, preserving tie-breaking.
    restored.push(base, 500);
    restored.push(base, 501);
    std::vector<int> c;
    restored.pop_due(base, [&](Date, int v) { c.push_back(v); });
    CHECK(c == std::vector<int>{500, 501});
}
