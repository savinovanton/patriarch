#pragma once

#include "pat/core/assert.hpp"

#include <cstddef>
#include <new>
#include <type_traits>
#include <utility>

namespace pat::core {

/// Vector with inline storage and a hard capacity of N elements (no heap allocation, ever).
/// Exceeding the capacity is a programming error (PAT_ASSERT). Intended for bounded lists such as
/// "at most 8 traits per character".
///
/// Note: unlike a classic small-vector it never spills to the heap; a spilling SmallVec is not
/// implemented yet.
template <class T, std::size_t N>
class StaticVec {
    static_assert(N > 0, "StaticVec capacity must be positive");

public:
    using value_type = T;
    using iterator = T*;
    using const_iterator = const T*;

    StaticVec() noexcept = default;
    ~StaticVec() { clear(); }

    StaticVec(const StaticVec& other) {
        for (const T& item : other) {
            emplace_back(item);
        }
    }
    StaticVec(StaticVec&& other) noexcept(std::is_nothrow_move_constructible_v<T>) {
        for (T& item : other) {
            emplace_back(std::move(item));
        }
        other.clear();
    }
    StaticVec& operator=(const StaticVec& other) {
        if (this != &other) {
            clear();
            for (const T& item : other) {
                emplace_back(item);
            }
        }
        return *this;
    }
    StaticVec& operator=(StaticVec&& other) noexcept(std::is_nothrow_move_constructible_v<T>) {
        if (this != &other) {
            clear();
            for (T& item : other) {
                emplace_back(std::move(item));
            }
            other.clear();
        }
        return *this;
    }

    template <class... Args>
    T& emplace_back(Args&&... args) {
        PAT_ASSERT(size_ < N, "StaticVec capacity ({}) exceeded", N);
        T* slot = ::new (static_cast<void*>(data() + size_)) T(std::forward<Args>(args)...);
        ++size_;
        return *slot;
    }
    void push_back(const T& value) { emplace_back(value); }
    void push_back(T&& value) { emplace_back(std::move(value)); }

    void pop_back() {
        PAT_ASSERT(size_ > 0, "StaticVec::pop_back on an empty vector");
        --size_;
        data()[size_].~T();
    }

    /// Removes element `index` by moving the last element into its place (O(1), changes order).
    void erase_unordered(std::size_t index) {
        PAT_ASSERT(index < size_, "StaticVec::erase_unordered out of range");
        if (index + 1 != size_) {
            data()[index] = std::move(data()[size_ - 1]);
        }
        pop_back();
    }

    /// Removes element `index` preserving the order of the others (O(n)).
    void erase(std::size_t index) {
        PAT_ASSERT(index < size_, "StaticVec::erase out of range");
        for (std::size_t i = index; i + 1 < size_; ++i) {
            data()[i] = std::move(data()[i + 1]);
        }
        pop_back();
    }

    void clear() noexcept {
        while (size_ > 0) {
            --size_;
            data()[size_].~T();
        }
    }

    [[nodiscard]] T& operator[](std::size_t i) noexcept {
        PAT_ASSERT(i < size_, "StaticVec index out of range");
        return data()[i];
    }
    [[nodiscard]] const T& operator[](std::size_t i) const noexcept {
        PAT_ASSERT(i < size_, "StaticVec index out of range");
        return data()[i];
    }
    [[nodiscard]] T& front() noexcept { return (*this)[0]; }
    [[nodiscard]] T& back() noexcept { return (*this)[size_ - 1]; }
    [[nodiscard]] const T& front() const noexcept { return (*this)[0]; }
    [[nodiscard]] const T& back() const noexcept { return (*this)[size_ - 1]; }

    [[nodiscard]] T* data() noexcept { return std::launder(reinterpret_cast<T*>(storage_)); }
    [[nodiscard]] const T* data() const noexcept {
        return std::launder(reinterpret_cast<const T*>(storage_));
    }

    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] static constexpr std::size_t capacity() noexcept { return N; }
    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
    [[nodiscard]] bool full() const noexcept { return size_ == N; }

    [[nodiscard]] iterator begin() noexcept { return data(); }
    [[nodiscard]] iterator end() noexcept { return data() + size_; }
    [[nodiscard]] const_iterator begin() const noexcept { return data(); }
    [[nodiscard]] const_iterator end() const noexcept { return data() + size_; }

private:
    alignas(T) unsigned char storage_[N * sizeof(T)];
    std::size_t size_ = 0;
};

}  // namespace pat::core
