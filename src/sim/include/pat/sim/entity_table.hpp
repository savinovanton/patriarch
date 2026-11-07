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
///
/// Slot states (one byte per slot): 0 = free, 1 = alive, 2 = retired. A retired slot keeps its id
/// forever (ADR-0006: ids of the dead are never reused in dynasty mode); it is not in the free list
/// and not counted as alive.
template <class Id>
class EntityTable {
public:
    /// Returns the most recently freed index, or appends a new one.
    [[nodiscard]] Id create() {
        std::uint32_t index;
        if (!free_.empty()) {
            index = free_.back();
            free_.pop_back();
            alive_[index] = kAlive;
        } else {
            index = static_cast<std::uint32_t>(alive_.size());
            alive_.push_back(kAlive);
            generation_.push_back(0);
        }
        ++alive_count_;
        return Id(index);
    }

    /// Moves an alive slot to the retired state; the slot is never handed out again. Returns false
    /// when the id is invalid or not alive.
    bool retire(Id id) {
        if (!is_alive(id)) {
            return false;
        }
        alive_[id.index()] = kRetired;
        --alive_count_;
        ++retired_count_;
        return true;
    }

    /// Returns false when the id is invalid or already destroyed.
    bool destroy(Id id) {
        if (!is_alive(id)) {
            return false;
        }
        const std::size_t index = id.index();
        alive_[index] = kFree;
        ++generation_[index];
        free_.push_back(static_cast<std::uint32_t>(index));
        --alive_count_;
        return true;
    }

    [[nodiscard]] bool is_alive(Id id) const noexcept {
        return id.valid() && id.index() < alive_.size() && alive_[id.index()] == kAlive;
    }

    /// Number of slots ever allocated (alive + free + retired). Columns must have at least this many entries.
    [[nodiscard]] std::size_t capacity() const noexcept { return alive_.size(); }
    [[nodiscard]] std::size_t alive_count() const noexcept { return alive_count_; }
    [[nodiscard]] std::size_t retired_count() const noexcept { return retired_count_; }
    /// Incremented every time a slot is destroyed; lets callers detect reuse.
    [[nodiscard]] std::uint32_t generation(Id id) const noexcept {
        return id.index() < generation_.size() ? generation_[id.index()] : 0;
    }
    [[nodiscard]] std::span<const std::uint32_t> free_list() const noexcept { return free_; }
    [[nodiscard]] std::span<const std::uint8_t> alive_flags() const noexcept { return alive_; }
    [[nodiscard]] std::span<const std::uint32_t> generations() const noexcept { return generation_; }

    /// Rebuilds the table from saved parts (loading a save). Returns false and leaves the table
    /// untouched when the parts are inconsistent: different lengths, an unknown slot state, a
    /// free-list entry that is out of range, duplicated or not free, or a free list that does not
    /// cover exactly the free slots.
    [[nodiscard]] bool restore(std::vector<std::uint8_t> alive,
                               std::vector<std::uint32_t> generation,
                               std::vector<std::uint32_t> free_list) {
        if (alive.size() != generation.size()) {
            return false;
        }
        std::size_t dead = 0;
        std::size_t retired = 0;
        for (const std::uint8_t flag : alive) {
            if (flag > kRetired) {
                return false;
            }
            dead += flag == kFree ? 1 : 0;
            retired += flag == kRetired ? 1 : 0;
        }
        if (free_list.size() != dead) {
            return false;
        }
        std::vector<std::uint8_t> seen(alive.size(), 0);
        for (const std::uint32_t slot : free_list) {
            if (slot >= alive.size() || alive[slot] != kFree || seen[slot] != 0) {
                return false;
            }
            seen[slot] = 1;
        }
        alive_ = std::move(alive);
        generation_ = std::move(generation);
        free_ = std::move(free_list);
        retired_count_ = retired;
        alive_count_ = alive_.size() - dead - retired;
        return true;
    }

    /// Visits live ids in ascending order.
    template <class F>
    void for_each_alive(F&& visit) const {
        for (std::size_t i = 0; i < alive_.size(); ++i) {
            if (alive_[i] == kAlive) {
                visit(Id(static_cast<std::uint32_t>(i)));
            }
        }
    }

    [[nodiscard]] bool operator==(const EntityTable&) const = default;

private:
    static constexpr std::uint8_t kFree = 0;
    static constexpr std::uint8_t kAlive = 1;
    static constexpr std::uint8_t kRetired = 2;

    std::vector<std::uint8_t> alive_;
    std::vector<std::uint32_t> generation_;
    std::vector<std::uint32_t> free_;
    std::size_t alive_count_ = 0;
    std::size_t retired_count_ = 0;
};

}  // namespace pat::sim
