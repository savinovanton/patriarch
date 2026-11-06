#pragma once

#include "pat/core/assert.hpp"

#include <atomic>
#include <cstddef>
#include <vector>

namespace pat::core {

/// Bounded lock-free queue for exactly ONE producer thread and ONE consumer thread
/// (e.g. UI thread -> simulation thread). Elements are delivered in push order.
///
/// try_push() may only be called from the producer thread and try_pop() only from the consumer
/// thread. T must be default-constructible and copy-assignable.
template <class T>
class SpscQueue {
public:
    /// Capacity is rounded up to a power of two (at least 2).
    explicit SpscQueue(std::size_t min_capacity) {
        std::size_t capacity = 2;
        while (capacity < min_capacity) {
            capacity <<= 1U;
        }
        buffer_.resize(capacity);
        mask_ = capacity - 1;
    }

    SpscQueue(const SpscQueue&) = delete;
    SpscQueue& operator=(const SpscQueue&) = delete;

    /// Producer side. Returns false if the queue is full.
    [[nodiscard]] bool try_push(const T& value) {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        if (tail - head_.load(std::memory_order_acquire) >= buffer_.size()) {
            return false;
        }
        buffer_[tail & mask_] = value;
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    /// Consumer side. Returns false if the queue is empty.
    [[nodiscard]] bool try_pop(T& out) {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        if (head == tail_.load(std::memory_order_acquire)) {
            return false;
        }
        out = buffer_[head & mask_];
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

    [[nodiscard]] std::size_t capacity() const noexcept { return buffer_.size(); }
    /// Approximate number of queued elements (exact when called from either end while idle).
    [[nodiscard]] std::size_t size_approx() const noexcept {
        return tail_.load(std::memory_order_acquire) - head_.load(std::memory_order_acquire);
    }

private:
    static constexpr std::size_t kCacheLine = 64;

    std::vector<T> buffer_;
    std::size_t mask_ = 0;
    // Producer and consumer indices live on separate cache lines (manual padding keeps MSVC quiet
    // about alignas padding warnings).
    std::atomic<std::size_t> head_{0};
    char pad_head_[kCacheLine - sizeof(std::atomic<std::size_t>)] = {};
    std::atomic<std::size_t> tail_{0};
    char pad_tail_[kCacheLine - sizeof(std::atomic<std::size_t>)] = {};
};

}  // namespace pat::core
