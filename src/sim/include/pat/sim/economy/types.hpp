#pragma once

#include <cstddef>
#include <cstdint>

// Small shared vocabulary of the economy (E9, docs/dev/economy-model.md sections 3-4): enumerations
// whose numeric values are stable (they end up in saves) and the fixed sizes of the economy tables.
// Kept in its own header so that rules, tables and formulas can share it without depending on each
// other.

namespace pat::sim::economy {

/// Population groups (GDD 03 section 4.1). Numeric values are stable.
enum class Group : std::uint8_t {
    Nobility = 0,
    Clergy = 1,
    Townsfolk = 2,
    Peasants = 3,
    Cossacks = 4,
    Natives = 5,  ///< reserve slot: size is 0 in M1
};
inline constexpr std::size_t kGroupCount = 6;

/// Province kinds (GDD 03 section 2). Numeric values are stable.
enum class ProvinceKind : std::uint8_t {
    Rural = 0,
    City = 1,
    Border = 2,
    Industrial = 3,
    Port = 4,
    Steppe = 5,
    Northern = 6,
    Simplified = 7,  ///< Siberia / Far East (GDD 03 section 2)
};
inline constexpr std::size_t kProvinceKindCount = 8;

/// How the crown's money is held (open question Q7, #142; economy-model section 5.3).
enum class CrownAccountMode : std::uint8_t {
    Separate = 0,        ///< the crown has its own account (accepted for now)
    EmperorDynasty = 1,  ///< the crown's account is the emperor dynasty's account
};

/// Number of goods slots (GDD 03 section 5.1): 0 grain, 1 salt, 2 fur, 3 timber, 4 hemp/flax,
/// 5 iron, 6 copper/silver, 7 cloth, 8 fish, 9 arms.
inline constexpr std::size_t kMaxGoods = 10;
/// Building slots per province (GDD 03 section 8.1: 2...6).
inline constexpr std::size_t kMaxSlots = 6;
/// Province development range (GDD 03 section 8.3).
inline constexpr std::int32_t kMaxDevelopment = 10;
/// Monthly processing is spread over days 1...28 (ADR-0002 P2; AGENTS.md section 7).
inline constexpr std::int32_t kStaggerDays = 28;

[[nodiscard]] constexpr std::size_t index_of(Group g) noexcept {
    return static_cast<std::size_t>(g);
}
[[nodiscard]] constexpr std::size_t index_of(ProvinceKind k) noexcept {
    return static_cast<std::size_t>(k);
}

}  // namespace pat::sim::economy
