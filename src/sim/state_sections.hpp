#pragma once

#include "pat/core/bytes.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <type_traits>
#include <utility>

// Ids of the sections of the extension block of the world state (ADR-0006 D6) and the helpers that
// the readers share. Internal to src/sim: the layout itself is documented at the top of world_io.cpp
// (sections 1..7), world_family_io.cpp (sections 16..23), world_economy_io.cpp (sections 32..40),
// world_events_io.cpp (sections 48..57) and world_court_io.cpp (sections 10 and 64..71).
//
// Ids are stable forever and are only ever added. Blocks: 1..15 E7 (10: influence, E10-T03), 16..31 E8
// (E8 uses 16..23), 32..47 E9, 48..63 E11 (event tables, 48..57 used by E11-T03), 64..71 E10 (court).

namespace pat::sim::detail {

inline constexpr std::uint16_t kSectionPerson = 1;
inline constexpr std::uint16_t kSectionDynasty = 2;
inline constexpr std::uint16_t kSectionKin = 3;
inline constexpr std::uint16_t kSectionPregnancy = 4;
inline constexpr std::uint16_t kSectionChronicle = 5;
inline constexpr std::uint16_t kSectionOpinion = 6;
inline constexpr std::uint16_t kSectionStanding = 7;
/// E10-T03 (schema >= 14): influence balances and log (influence-model.md section 17); omitted when empty.
inline constexpr std::uint16_t kSectionInfluence = 10;

// ---- E8-T03: the family (schema >= 12). Default sections are omitted. ----
inline constexpr std::uint16_t kSectionFamilyCounters = 16;
inline constexpr std::uint16_t kSectionNegotiations = 17;
inline constexpr std::uint16_t kSectionBetrothals = 18;
inline constexpr std::uint16_t kSectionMarriages = 19;
inline constexpr std::uint16_t kSectionPersonStatus = 20;
inline constexpr std::uint16_t kSectionSuccession = 21;
inline constexpr std::uint16_t kSectionRegency = 22;
inline constexpr std::uint16_t kSectionDispute = 23;

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

// ---- E11-T03: the event tables (schema >= 13). Default (empty) sections are omitted. ----
inline constexpr std::uint16_t kSectionEventsMeta = 48;
inline constexpr std::uint16_t kSectionEventFlags = 49;
inline constexpr std::uint16_t kSectionEventVars = 50;
inline constexpr std::uint16_t kSectionEventTimers = 51;
inline constexpr std::uint16_t kSectionEventCooldowns = 52;
inline constexpr std::uint16_t kSectionEventChains = 53;
inline constexpr std::uint16_t kSectionEventModifiers = 54;
inline constexpr std::uint16_t kSectionEventQueue = 55;
inline constexpr std::uint16_t kSectionEventHistory = 56;
inline constexpr std::uint16_t kSectionEventJournal = 57;

// ---- E10-T03: the court tables (schema >= 14). Default (empty) sections are omitted. ----
inline constexpr std::uint16_t kSectionCourtMeta = 64;
inline constexpr std::uint16_t kSectionCourtOffices = 65;    ///< offices, then disgraces
inline constexpr std::uint16_t kSectionCourtVacancies = 66;  ///< vacancies, then the decision ring
inline constexpr std::uint16_t kSectionCourtPetitions = 67;
inline constexpr std::uint16_t kSectionCourtServants = 68;
inline constexpr std::uint16_t kSectionCourtFavor = 69;
inline constexpr std::uint16_t kSectionCourtPortfolio = 70;
inline constexpr std::uint16_t kSectionCourtOrders = 71;  ///< the ring of portfolio orders

/// A part of the hash that has content is never 0 (0 means "absent" and stays out of `total`).
[[nodiscard]] constexpr std::uint64_t nonzero(std::uint64_t hash) noexcept {
    return hash == 0 ? 1 : hash;
}

/// Reads `u32 count | count rows` into `rows`: the count is checked against `limit` and against what
/// the rest of the section can hold (`min_row_bytes` per row) before any memory is reserved, and the keys
/// of `projection` must strictly ascend. `read_row(in, row)` fills one row and returns false when the row is
/// malformed. Shared by the sections of the family (E8-T03), the events (E11-T03) and the court (E10-T03).
template <class Rows, class Projection, class ReadRow>
bool read_ordered_rows(core::ByteReader& in,
                       Rows& rows,
                       std::size_t limit,
                       std::size_t min_row_bytes,
                       Projection projection,
                       ReadRow read_row) {
    const std::uint32_t count = in.u32();
    if (!in.ok() || count > limit || !in.can_hold(count, min_row_bytes)) {
        return false;
    }
    using Row = typename Rows::value_type;
    using Key = std::remove_cvref_t<std::invoke_result_t<Projection, Row>>;
    std::optional<Key> previous;
    rows.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        Row row;
        if (!read_row(in, row)) {
            return false;
        }
        const Key key = std::invoke(projection, row);
        if (previous && key <= *previous) {
            return false;
        }
        previous = key;
        rows.push_back(std::move(row));
    }
    return true;
}

}  // namespace pat::sim::detail
