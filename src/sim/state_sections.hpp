#pragma once

#include "pat/core/bytes.hpp"

#include <cstddef>
#include <cstdint>

// Ids of the sections of the extension block of the world state (ADR-0006 D6) and the helpers that
// the readers share. Internal to src/sim: the layout itself is documented at the top of world_io.cpp
// (sections 1..3 and 5..7) and world_economy_io.cpp (sections 32..39).
//
// Ids are stable forever and are only ever added. Blocks: 1..15 E7, 16..31 E8, 32..47 E9.

namespace pat::sim::detail {

inline constexpr std::uint16_t kSectionPerson = 1;
inline constexpr std::uint16_t kSectionDynasty = 2;
inline constexpr std::uint16_t kSectionKin = 3;
inline constexpr std::uint16_t kSectionChronicle = 5;
inline constexpr std::uint16_t kSectionOpinion = 6;
inline constexpr std::uint16_t kSectionStanding = 7;

// ---- E9-T03: the economy (schema >= 5). Written only when the economy is on. ----
inline constexpr std::uint16_t kSectionEconomy = 32;              ///< scalars, rules hash
inline constexpr std::uint16_t kSectionEconomyDynasty = 33;       ///< manpower reserve, bankruptcy
inline constexpr std::uint16_t kSectionEconomyProvince = 34;      ///< policies, control, stock, buildings
inline constexpr std::uint16_t kSectionEconomyPopulation = 35;    ///< the six groups of every province
inline constexpr std::uint16_t kSectionEconomyAccounts = 36;      ///< balances and the ledger rings
inline constexpr std::uint16_t kSectionEconomyConstruction = 37;  ///< construction queue (when not empty)
inline constexpr std::uint16_t kSectionEconomyCredit = 38;        ///< loans, quotas, outcomes (if any)
inline constexpr std::uint16_t kSectionEconomyIncidents = 39;     ///< incident ring (when not empty)
inline constexpr std::uint16_t kSectionEconomyPolicy =
    40;  ///< policy levels, dynasty policies (E9-T12, schema >= 9)

/// True when `count` entries of at least `bytes_each` bytes could still be present in the input;
/// guards allocations against absurd counts in damaged files.
[[nodiscard]] inline bool plausible(const core::ByteReader& in, std::uint32_t count, std::size_t bytes_each) {
    return in.ok() && static_cast<std::uint64_t>(count) * bytes_each <= in.remaining();
}

}  // namespace pat::sim::detail
