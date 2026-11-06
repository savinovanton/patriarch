#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace pat::sim {

/// Allocator of dense entity indices with a LIFO free list, used by tables that store their data
/// in parallel arrays (structure of arrays). The table only tracks liveness; the owner keeps the
/// columns in sync (resize when capacity() grows, reset on reuse).
///
/// Everything here is a pure function of the sequence of create/destroy calls, so ids handed out
/// are deterministic. The free list is part of the simulation state and is included in state hashes.
template <class Id>
class EntityTable {
public:
    /// Returns the most recently freed index, or appends a new one.
    [[nodiscard]] Id create() {
        std::uint32_t index;
        if (!free_.empty()) {
            index = free_.back();
            free_.pop_back();
            alive_[index] = 1;
        } else {
            index = static_cast<std::uint32_t>(alive_.size());
            alive_.push_back(1);
            generation_.push_back(0);
        }
        ++alive_count_;
        return Id(index);
    }

    /// Returns false when the id is invalid or already destroyed.
    bool destroy(Id id) {
        if (!is_alive(id)) {
            return false;
        }
        const std::size_t index = id.index();
        alive_[index] = 0;
        ++generation_[index];
        free_.push_back(static_cast<std::uint32_t>(index));
        --alive_count_;
        return true;
    }

    [[nodiscard]] bool is_alive(Id id) const noexcept {
        return id.valid() && id.index() < alive_.size() && alive_[id.index()] != 0;
    }

    /// Number of slots ever allocated (alive + free). Columns must have at least this many entries.
    [[nodiscard]] std::size_t capacity() const noexcept { return alive_.size(); }
    [[nodiscard]] std::size_t alive_count() const noexcept { return alive_count_; }
    /// Incremented every time a slot is destroyed; lets callers detect reuse.
    [[nodiscard]] std::uint32_t generation(Id id) const noexcept {
        return id.index() < generation_.size() ? generation_[id.index()] : 0;
    }
    [[nodiscard]] std::span<const std::uint32_t> free_list() const noexcept { return free_; }
    [[nodiscard]] std::span<const std::uint8_t> alive_flags() const noexcept { return alive_; }

    /// Visits live ids in ascending order.
    template <class F>
    void for_each_alive(F&& visit) const {
        for (std::size_t i = 0; i < alive_.size(); ++i) {
            if (alive_[i] != 0) {
                visit(Id(static_cast<std::uint32_t>(i)));
            }
        }
    }

    [[nodiscard]] bool operator==(const EntityTable&) const = default;

private:
    std::vector<std::uint8_t> alive_;
    std::vector<std::uint32_t> generation_;
    std::vector<std::uint32_t> free_;
    std::size_t alive_count_ = 0;
};

}  // namespace pat::sim
