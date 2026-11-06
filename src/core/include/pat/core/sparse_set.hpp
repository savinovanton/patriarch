#pragma once

#include "pat/core/assert.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>
#include <vector>

namespace pat::core {

/// Set of small dense integer keys (plain integers or StrongId) with O(1) insert/erase/contains
/// and contiguous iteration.
///
/// Erase uses swap-remove, so the dense order is "insertion order perturbed by erasures" - fully
/// determined by the sequence of operations, hence deterministic. Memory is proportional to the
/// largest key, which is appropriate for dense entity ids.
template <class Key = std::uint32_t>
class SparseSet {
public:
    bool insert(Key key) {
        const std::size_t index = index_of(key);
        if (index >= sparse_.size()) {
            sparse_.resize(index + 1, kNone);
        }
        if (sparse_[index] != kNone) {
            return false;
        }
        sparse_[index] = static_cast<std::uint32_t>(dense_.size());
        dense_.push_back(key);
        return true;
    }

    bool erase(Key key) {
        if (!contains(key)) {
            return false;
        }
        const std::size_t index = index_of(key);
        const std::uint32_t position = sparse_[index];
        const Key last = dense_.back();
        dense_[position] = last;
        sparse_[index_of(last)] = position;
        dense_.pop_back();
        sparse_[index] = kNone;
        return true;
    }

    [[nodiscard]] bool contains(Key key) const noexcept {
        const std::size_t index = index_of(key);
        return index < sparse_.size() && sparse_[index] != kNone;
    }

    [[nodiscard]] std::size_t size() const noexcept { return dense_.size(); }
    [[nodiscard]] bool empty() const noexcept { return dense_.empty(); }
    [[nodiscard]] std::span<const Key> values() const noexcept { return dense_; }
    [[nodiscard]] auto begin() const noexcept { return dense_.begin(); }
    [[nodiscard]] auto end() const noexcept { return dense_.end(); }

    void clear() {
        for (const Key key : dense_) {
            sparse_[index_of(key)] = kNone;
        }
        dense_.clear();
    }

private:
    static constexpr std::uint32_t kNone = 0xFFFFFFFFU;

    [[nodiscard]] static constexpr std::size_t index_of(Key key) noexcept {
        if constexpr (std::is_integral_v<Key>) {
            return static_cast<std::size_t>(key);
        } else {
            return key.index();
        }
    }

    std::vector<Key> dense_;
    std::vector<std::uint32_t> sparse_;
};

}  // namespace pat::core
