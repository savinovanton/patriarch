#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>

// Helpers for the sparse World tables that keep their rows in ascending key order (family tables,
// E8-T02; event tables, E11-T02; court tables, E10-T02). Internal to src/sim. `projection` maps a row to its
// key; the key must be totally ordered (an integer or a std::pair of them).

namespace pat::sim::detail {

template <class Rows, class Key, class Projection>
auto row_position(Rows& rows, const Key& key, Projection projection) {
    return std::ranges::lower_bound(rows, key, {}, projection);
}

template <class Rows, class Key, class Projection>
const typename Rows::value_type* find_row(const Rows& rows, const Key& key, Projection projection) {
    const auto position = row_position(rows, key, projection);
    return position != rows.end() && std::invoke(projection, *position) == key ? &*position : nullptr;
}

template <class Rows, class Row, class Projection>
void replace_row(Rows& rows, const Row& row, Projection projection) {
    const auto key = std::invoke(projection, row);
    const auto position = row_position(rows, key, projection);
    if (position != rows.end() && std::invoke(projection, *position) == key) {
        *position = row;
    } else {
        rows.insert(position, row);
    }
}

template <class Rows, class Key, class Projection>
bool erase_row(Rows& rows, const Key& key, Projection projection) {
    const auto position = row_position(rows, key, projection);
    if (position == rows.end() || std::invoke(projection, *position) != key) {
        return false;
    }
    rows.erase(position);
    return true;
}

/// True when the keys strictly ascend (ordered and without repeats).
template <class Rows, class Projection>
bool ordered_unique(const Rows& rows, Projection projection) {
    return std::adjacent_find(rows.begin(), rows.end(), [&](const auto& a, const auto& b) {
               return std::invoke(projection, a) >= std::invoke(projection, b);
           }) == rows.end();
}

/// The last value of a monotone u32 counter; a counter that reached it hands out nothing more.
inline constexpr std::uint32_t kMaxCounter = std::numeric_limits<std::uint32_t>::max();

/// Appends to a ring with a monotone sequence counter (`seq` of the row); the oldest entry leaves a
/// full ring. nullopt (nothing changed) when the counter is exhausted.
template <class Ring, class Row>
std::optional<std::uint32_t> append_ring(Ring& ring, std::size_t capacity, std::uint32_t& next, Row row) {
    if (next == kMaxCounter) {
        return std::nullopt;
    }
    row.seq = next++;
    if (ring.size() >= capacity) {
        ring.erase(ring.begin());
    }
    ring.push_back(row);
    return row.seq;
}

}  // namespace pat::sim::detail
