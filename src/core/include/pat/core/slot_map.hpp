#pragma once

#include "pat/core/assert.hpp"
#include "pat/core/ids.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

namespace pat::core {

/// Stable-index container with generational handles.
///
///  * insert/emplace/erase/get are O(1).
///  * Slots are reused in LIFO order, so the layout depends only on the sequence of operations.
///  * Iteration visits occupied slots in ascending slot index: deterministic (code-style.md 4).
///  * References returned by get() stay valid until the element is erased or the map grows
///    (growth may move elements - hold handles, not pointers).
template <class T, class Tag = T>
class SlotMap {
public:
    using handle_type = Handle<Tag>;
    using value_type = T;

    template <bool IsConst>
    class Iterator;

    handle_type insert(T value) { return emplace(std::move(value)); }

    template <class... Args>
    handle_type emplace(Args&&... args) {
        std::uint32_t index;
        if (!free_.empty()) {
            index = free_.back();
            free_.pop_back();
        } else {
            index = static_cast<std::uint32_t>(slots_.size());
            slots_.emplace_back();
        }
        Slot& slot = slots_[index];
        slot.value.emplace(std::forward<Args>(args)...);
        ++size_;
        return handle_type{index, slot.generation};
    }

    /// Removes the element; returns false if the handle is stale or invalid.
    bool erase(handle_type handle) {
        if (!contains(handle)) {
            return false;
        }
        Slot& slot = slots_[handle.index];
        slot.value.reset();
        ++slot.generation;
        if (slot.generation == 0) {
            slot.generation = 1;  // 0 is reserved for default-constructed (invalid) handles
        }
        free_.push_back(handle.index);
        --size_;
        return true;
    }

    [[nodiscard]] bool contains(handle_type handle) const noexcept {
        return handle.valid() && handle.index < slots_.size() &&
               slots_[handle.index].generation == handle.generation && slots_[handle.index].value.has_value();
    }

    /// Returns nullptr for stale or invalid handles.
    [[nodiscard]] T* get(handle_type handle) noexcept {
        return contains(handle) ? &*slots_[handle.index].value : nullptr;
    }
    [[nodiscard]] const T* get(handle_type handle) const noexcept {
        return contains(handle) ? &*slots_[handle.index].value : nullptr;
    }

    [[nodiscard]] T& at(handle_type handle) {
        PAT_ASSERT(contains(handle), "SlotMap::at with a stale handle");
        return *slots_[handle.index].value;
    }
    [[nodiscard]] const T& at(handle_type handle) const {
        PAT_ASSERT(contains(handle), "SlotMap::at with a stale handle");
        return *slots_[handle.index].value;
    }

    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
    /// Number of slots ever allocated (occupied + free).
    [[nodiscard]] std::size_t slot_count() const noexcept { return slots_.size(); }
    void reserve(std::size_t n) { slots_.reserve(n); }

    /// Erases everything; all previously issued handles become stale.
    void clear() {
        for (std::uint32_t i = 0; i < slots_.size(); ++i) {
            Slot& slot = slots_[i];
            if (slot.value.has_value()) {
                slot.value.reset();
                ++slot.generation;
                if (slot.generation == 0) {
                    slot.generation = 1;
                }
            }
        }
        free_.clear();
        for (std::uint32_t i = static_cast<std::uint32_t>(slots_.size()); i > 0; --i) {
            free_.push_back(i - 1);  // lowest index is reused first
        }
        size_ = 0;
    }

    /// Entry yielded by iteration.
    template <bool IsConst>
    struct Entry {
        handle_type handle;
        std::conditional_t<IsConst, const T&, T&> value;
    };

    template <bool IsConst>
    class Iterator {
    public:
        using map_type = std::conditional_t<IsConst, const SlotMap, SlotMap>;
        using value_type = Entry<IsConst>;

        Iterator(map_type* map, std::uint32_t index) : map_(map), index_(index) { skip_empty(); }

        [[nodiscard]] Entry<IsConst> operator*() const {
            auto& slot = map_->slots_[index_];
            return Entry<IsConst>{handle_type{index_, slot.generation}, *slot.value};
        }
        Iterator& operator++() {
            ++index_;
            skip_empty();
            return *this;
        }
        [[nodiscard]] bool operator==(const Iterator& other) const noexcept { return index_ == other.index_; }
        [[nodiscard]] bool operator!=(const Iterator& other) const noexcept { return index_ != other.index_; }

    private:
        void skip_empty() {
            while (index_ < map_->slots_.size() && !map_->slots_[index_].value.has_value()) {
                ++index_;
            }
        }
        map_type* map_;
        std::uint32_t index_;
    };

    [[nodiscard]] Iterator<false> begin() { return Iterator<false>(this, 0); }
    [[nodiscard]] Iterator<false> end() {
        return Iterator<false>(this, static_cast<std::uint32_t>(slots_.size()));
    }
    [[nodiscard]] Iterator<true> begin() const { return Iterator<true>(this, 0); }
    [[nodiscard]] Iterator<true> end() const {
        return Iterator<true>(this, static_cast<std::uint32_t>(slots_.size()));
    }

private:
    struct Slot {
        std::optional<T> value;
        std::uint32_t generation = 1;
    };

    std::vector<Slot> slots_;
    std::vector<std::uint32_t> free_;
    std::size_t size_ = 0;
};

}  // namespace pat::core
