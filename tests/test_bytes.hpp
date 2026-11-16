#pragma once

// Little-endian load and patch helpers for tests that inspect or corrupt serialized bytes at a known
// offset. Header-only; include from tests only.

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <span>

namespace pat::test {

/// Reads a `T` (unsigned integer) stored little-endian at `at`. Fails the test when out of range.
template <typename T>
[[nodiscard]] T load_le(std::span<const std::uint8_t> bytes, std::size_t at) {
    REQUIRE(at + sizeof(T) <= bytes.size());
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < sizeof(T); ++i) {
        value |= static_cast<std::uint64_t>(bytes[at + i]) << (8U * i);
    }
    return static_cast<T>(value);
}

/// Overwrites `width` bytes at `at` with the low `width` bytes of `value`, little-endian.
inline void
store_le_bytes(std::span<std::uint8_t> bytes, std::size_t at, std::uint64_t value, std::size_t width) {
    REQUIRE(at + width <= bytes.size());
    for (std::size_t i = 0; i < width; ++i) {
        bytes[at + i] = static_cast<std::uint8_t>((value >> (8U * i)) & 0xFFU);
    }
}

/// Overwrites a `T` (unsigned integer) at `at`, little-endian.
template <typename T>
void store_le(std::span<std::uint8_t> bytes, std::size_t at, T value) {
    store_le_bytes(bytes, at, static_cast<std::uint64_t>(value), sizeof(T));
}

}  // namespace pat::test
