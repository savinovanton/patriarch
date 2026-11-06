#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

namespace pat::core {

/// Bump allocator for short-lived data (per-tick scratch space). reset() rewinds in O(blocks) and
/// keeps the memory for reuse. Only trivially destructible types may be created: the arena never
/// runs destructors.
class Arena {
public:
    explicit Arena(std::size_t block_size = 64 * 1024);

    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;
    Arena(Arena&&) noexcept = default;
    Arena& operator=(Arena&&) noexcept = default;

    /// Returns `size` bytes aligned to `alignment` (a power of two).
    [[nodiscard]] void* allocate(std::size_t size, std::size_t alignment = alignof(std::max_align_t));

    template <class T, class... Args>
    [[nodiscard]] T* create(Args&&... args) {
        static_assert(std::is_trivially_destructible_v<T>, "Arena never runs destructors");
        return ::new (allocate(sizeof(T), alignof(T))) T(std::forward<Args>(args)...);
    }

    /// Array of `count` value-initialized elements.
    template <class T>
    [[nodiscard]] std::span<T> allocate_array(std::size_t count) {
        static_assert(std::is_trivially_destructible_v<T>, "Arena never runs destructors");
        T* items = static_cast<T*>(allocate(sizeof(T) * count, alignof(T)));
        for (std::size_t i = 0; i < count; ++i) {
            ::new (static_cast<void*>(items + i)) T();
        }
        return {items, count};
    }

    /// Invalidates everything handed out so far; memory is retained.
    void reset() noexcept;

    /// Bytes handed out since the last reset (excluding alignment padding).
    [[nodiscard]] std::size_t bytes_used() const noexcept { return used_; }
    /// Total bytes obtained from the system allocator.
    [[nodiscard]] std::size_t bytes_reserved() const noexcept { return reserved_; }
    [[nodiscard]] std::size_t block_count() const noexcept { return blocks_.size(); }

private:
    struct Block {
        std::unique_ptr<std::byte[]> data;
        std::size_t size = 0;
    };

    std::size_t block_size_;
    std::vector<Block> blocks_;
    std::size_t current_ = 0;
    std::size_t offset_ = 0;
    std::size_t used_ = 0;
    std::size_t reserved_ = 0;
};

}  // namespace pat::core
