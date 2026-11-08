#pragma once

#include "pat/sim/date.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace pat::sim {

/// Queue of delayed work items ordered by (due date, insertion sequence). Items with the same due
/// date are returned in the order they were pushed, so the result never depends on heap internals.
template <class T>
class ScheduledQueue {
public:
    struct Entry {
        Date due;
        std::uint64_t sequence = 0;
        T item;
    };

    void push(Date due, T item) {
        heap_.push_back(Entry{due, next_sequence_++, std::move(item)});
        std::push_heap(heap_.begin(), heap_.end(), later);
    }

    /// Calls handler(due, item) for every entry with due <= now, oldest first. The handler may push
    /// new entries; those that are also due are processed in the same pass. Returns the number of
    /// processed entries.
    template <class Handler>
    std::size_t pop_due(Date now, Handler&& handler) {
        std::size_t processed = 0;
        while (!heap_.empty() && heap_.front().due <= now) {
            std::pop_heap(heap_.begin(), heap_.end(), later);
            Entry entry = std::move(heap_.back());
            heap_.pop_back();
            handler(entry.due, std::move(entry.item));
            ++processed;
        }
        return processed;
    }

    /// Removes every entry for which pred(due, item) is true and returns how many were removed. The
    /// order of the remaining entries (by due date, then insertion sequence) is not affected.
    template <class Pred>
    std::size_t remove_if(Pred&& pred) {
        const std::size_t before = heap_.size();
        heap_.erase(std::remove_if(heap_.begin(),
                                   heap_.end(),
                                   [&](const Entry& entry) { return pred(entry.due, entry.item); }),
                    heap_.end());
        std::make_heap(heap_.begin(), heap_.end(), later);
        return before - heap_.size();
    }

    [[nodiscard]] std::optional<Date> next_due() const {
        return heap_.empty() ? std::nullopt : std::optional<Date>(heap_.front().due);
    }
    [[nodiscard]] std::size_t size() const noexcept { return heap_.size(); }
    [[nodiscard]] bool empty() const noexcept { return heap_.empty(); }
    void clear() { heap_.clear(); }

    /// Entries sorted by (due, sequence) - the canonical form for saving and hashing.
    [[nodiscard]] std::vector<Entry> snapshot() const {
        std::vector<Entry> sorted = heap_;
        std::sort(sorted.begin(), sorted.end(), [](const Entry& a, const Entry& b) { return earlier(a, b); });
        return sorted;
    }
    [[nodiscard]] std::uint64_t next_sequence() const noexcept { return next_sequence_; }

    /// Rebuilds the queue from a snapshot (entries in any order) and the saved sequence counter.
    void restore(std::vector<Entry> entries, std::uint64_t next_sequence) {
        heap_ = std::move(entries);
        std::make_heap(heap_.begin(), heap_.end(), later);
        next_sequence_ = next_sequence;
    }

private:
    [[nodiscard]] static bool earlier(const Entry& a, const Entry& b) noexcept {
        return a.due != b.due ? a.due < b.due : a.sequence < b.sequence;
    }
    // std heap functions build a max-heap; "later" as the comparator puts the earliest entry on top.
    [[nodiscard]] static bool later(const Entry& a, const Entry& b) noexcept { return earlier(b, a); }

    std::vector<Entry> heap_;
    std::uint64_t next_sequence_ = 0;
};

}  // namespace pat::sim
