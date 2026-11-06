#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>

// Typed identifiers (code-style.md section 5). Bare integers must not be used as entity IDs.

namespace pat::core {

/// Dense identifier of an entity of kind `Tag`. A default-constructed id is invalid.
/// Different tags are different types: a ProvinceId cannot be passed where a DynastyId is expected.
template <class Tag, class T = std::uint32_t>
class StrongId {
public:
    using value_type = T;
    static constexpr T kInvalid = std::numeric_limits<T>::max();

    constexpr StrongId() noexcept = default;
    constexpr explicit StrongId(T value) noexcept : value_(value) {}

    [[nodiscard]] static constexpr StrongId invalid() noexcept { return StrongId(); }
    [[nodiscard]] constexpr T value() const noexcept { return value_; }
    [[nodiscard]] constexpr bool valid() const noexcept { return value_ != kInvalid; }
    /// Index into a dense array; the id must be valid.
    [[nodiscard]] constexpr std::size_t index() const noexcept { return static_cast<std::size_t>(value_); }

    [[nodiscard]] friend constexpr auto operator<=>(const StrongId&, const StrongId&) noexcept = default;
    [[nodiscard]] friend constexpr bool operator==(const StrongId&, const StrongId&) noexcept = default;

private:
    T value_ = kInvalid;
};

/// Generational reference to an element of a SlotMap. A stale handle (element erased, slot
/// reused) never matches the current occupant because the generation differs.
template <class Tag>
struct Handle {
    static constexpr std::uint32_t kInvalidIndex = std::numeric_limits<std::uint32_t>::max();

    std::uint32_t index = kInvalidIndex;
    std::uint32_t generation = 0;

    [[nodiscard]] constexpr bool valid() const noexcept { return index != kInvalidIndex; }
    [[nodiscard]] friend constexpr auto operator<=>(const Handle&, const Handle&) noexcept = default;
    [[nodiscard]] friend constexpr bool operator==(const Handle&, const Handle&) noexcept = default;
};

}  // namespace pat::core

// std::hash is provided for convenience in lookup-only hash maps. Never iterate such maps in code
// that influences simulation state (code-style.md section 4).
template <class Tag, class T>
struct std::hash<pat::core::StrongId<Tag, T>> {
    [[nodiscard]] std::size_t operator()(const pat::core::StrongId<Tag, T>& id) const noexcept {
        return std::hash<T>{}(id.value());
    }
};

template <class Tag>
struct std::hash<pat::core::Handle<Tag>> {
    [[nodiscard]] std::size_t operator()(const pat::core::Handle<Tag>& h) const noexcept {
        return std::hash<std::uint64_t>{}((static_cast<std::uint64_t>(h.generation) << 32U) | h.index);
    }
};
