#pragma once

#include "pat/core/fixed.hpp"
#include "pat/core/ids.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

namespace pat::core {

/// FNV-1a 64 parameters: the single definition shared by HashStream, hash_bytes and fnv1a64.
inline constexpr std::uint64_t kFnv1a64OffsetBasis = 0xCBF29CE484222325ULL;
inline constexpr std::uint64_t kFnv1a64Prime = 0x100000001B3ULL;

/// Incremental 64-bit FNV-1a hash over a canonical byte representation: every integer is fed as
/// little-endian bytes of a fixed width, so the result is identical on all platforms and compilers.
/// Used for state hashes (replay and determinism checks). NOT a cryptographic hash.
class HashStream {
public:
    constexpr void bytes(std::span<const std::uint8_t> data) noexcept {
        for (const std::uint8_t b : data) {
            feed(b);
        }
    }
    constexpr void u8(std::uint8_t v) noexcept { feed(v); }
    constexpr void u16(std::uint16_t v) noexcept { little_endian(v, 2); }
    constexpr void u32(std::uint32_t v) noexcept { little_endian(v, 4); }
    constexpr void u64(std::uint64_t v) noexcept { little_endian(v, 8); }
    constexpr void i32(std::int32_t v) noexcept { u32(static_cast<std::uint32_t>(v)); }
    constexpr void i64(std::int64_t v) noexcept { u64(static_cast<std::uint64_t>(v)); }
    constexpr void boolean(bool v) noexcept { feed(v ? 1U : 0U); }

    template <class Tag, class T>
    constexpr void id(StrongId<Tag, T> value) noexcept {
        u64(static_cast<std::uint64_t>(value.value()));
    }
    template <class Raw, int Frac>
    constexpr void fixed(Fixed<Raw, Frac> value) noexcept {
        i64(static_cast<std::int64_t>(value.raw()));
    }

    [[nodiscard]] constexpr std::uint64_t value() const noexcept { return state_; }

private:
    constexpr void feed(std::uint8_t b) noexcept {
        state_ ^= b;
        state_ *= kFnv1a64Prime;
    }
    constexpr void little_endian(std::uint64_t v, int width) noexcept {
        for (int i = 0; i < width; ++i) {
            feed(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFFU));
        }
    }

    std::uint64_t state_ = kFnv1a64OffsetBasis;
};

/// One-shot FNV-1a 64 of a byte buffer; equal to feeding the same bytes into a fresh HashStream.
[[nodiscard]] constexpr std::uint64_t hash_bytes(std::span<const std::uint8_t> data) noexcept {
    HashStream h;
    h.bytes(data);
    return h.value();
}

/// Same as above for `std::byte` buffers (identical result for identical byte values).
[[nodiscard]] constexpr std::uint64_t hash_bytes(std::span<const std::byte> data) noexcept {
    HashStream h;
    for (const std::byte b : data) {
        h.u8(static_cast<std::uint8_t>(b));
    }
    return h.value();
}

}  // namespace pat::core
