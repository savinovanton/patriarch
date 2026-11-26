#pragma once

#include "pat/sim/court/tables.hpp"
#include "pat/sim/date.hpp"
#include "pat/sim/economy/money.hpp"
#include "pat/sim/entity_ids.hpp"

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

// Office portfolios (E10-T02 part of docs/dev/court-model.md section 20.4, ADR-0009 D18, D19): the row
// of a seat that carries a course and the ring of issued orders, and the aggregate State of the court
// tables World owns. No rules here: courses, orders and their effects are E10-T23 and later; persistence
// is E10-T03 (sections 70 and 71, hash part 39).

namespace pat::sim::court {

inline constexpr std::size_t kMaxPortfolios = 32;
inline constexpr std::size_t kMaxOrdersPerPortfolio = 4;
inline constexpr std::size_t kOrderRingSize = 64;
inline constexpr std::uint8_t kMaxCourses = 8;  ///< `course` is 0...7
/// Upper bound of `execution_permille` of an order (a share of one).
inline constexpr std::int16_t kMaxExecutionPermille = 1000;

/// Numeric values are stable (the save stores them).
enum class PortfolioState : std::uint8_t { Active = 1, Inertia = 2, Rollback = 3 };
inline constexpr std::uint8_t kLastPortfolioState = 3;

inline constexpr std::uint8_t kPortfolioHostile = 1U << 0;   ///< derived, kept for the event hash
inline constexpr std::uint8_t kPortfolioBySystem = 1U << 1;  ///< the course was set by the system/an event
inline constexpr std::uint8_t kPortfolioFlagMask = kPortfolioHostile | kPortfolioBySystem;

/// The course of a seat (section 20.4); keyed by (type, locus).
struct Portfolio {
    OfficeTypeId type = 0;
    std::uint32_t locus = 0;
    std::uint8_t course = 0;
    Date course_since = kNoDate;
    DynastyId controller;  ///< dynasty of the current or the last holder
    DynastyId set_by;      ///< dynasty that chose the course; invalid: the system
    DynastyId target;      ///< target dynasty of a course "with a target"; invalid: none
    PortfolioState state = PortfolioState::Active;
    Date next_step = kNoDate;  ///< kNoDate exactly in the Active state
    std::array<Date, kMaxOrdersPerPortfolio> order_ready{kNoDate, kNoDate, kNoDate, kNoDate};
    std::uint8_t flags = 0;

    [[nodiscard]] bool operator==(const Portfolio&) const = default;
};

/// One issued order in the ring of the world (section 20.4); the oldest leaves a full ring.
struct PortfolioOrder {
    std::uint32_t seq = 0;
    OfficeTypeId type = 0;
    std::uint32_t locus = 0;
    std::uint8_t order = 0;  ///< 0...kMaxOrdersPerPortfolio-1
    DynastyId issuer;
    CharacterId holder;  ///< may be archived
    Date date = kNoDate;
    std::uint32_t target = 0;  ///< a dynasty, province or character by the target kind of the order
    economy::Kopecks cost_kop = 0;
    std::int32_t cost_favor_x100 = 0;
    std::int16_t power_permille = 0;
    std::int16_t execution_permille = 0;
    std::int16_t noise_x100 = 0;

    [[nodiscard]] bool operator==(const PortfolioOrder&) const = default;
};

/// Primary court state; the indexes are derived by World (court::Indexes).
struct State {
    Meta meta;
    std::vector<Office> offices;         ///< by id
    std::vector<Disgrace> disgraces;     ///< by character
    std::vector<Vacancy> vacancies;      ///< by id
    std::vector<Decision> decisions;     ///< ring, ascending seq
    std::vector<Petition> petitions;     ///< by id
    ServantTable servants;               ///< by id
    std::vector<Favor> favor;            ///< by dynasty
    std::vector<Portfolio> portfolios;   ///< by (type, locus)
    std::vector<PortfolioOrder> orders;  ///< ring, ascending seq

    [[nodiscard]] bool operator==(const State&) const = default;
};

/// A servant of a dynasty and role in the derived index (ordered by dynasty, role, id).
struct ServantKey {
    DynastyId dynasty;
    ServantRole role = ServantRole::Steward;
    ServantId id = 0;

    [[nodiscard]] auto operator<=>(const ServantKey&) const = default;
};

/// Derived indexes over State (section 4.2, 4.7). Never saved, hashed or compared; World rebuilds them
/// after a load (World::rebuild_court_indexes) and keeps them in step with every mutation.
struct Indexes {
    std::vector<std::pair<CharacterId, OfficeId>> office_by_holder;                           ///< sorted
    std::vector<std::pair<std::pair<OfficeTypeId, std::uint32_t>, OfficeId>> office_by_seat;  ///< sorted
    std::vector<std::pair<ProvinceId, OfficeId>> offices_by_province;  ///< sorted; kOfficeProvinceSeat rows
    std::vector<ServantKey> servant_by_role;                           ///< sorted

    [[nodiscard]] bool operator==(const Indexes&) const = default;
};

}  // namespace pat::sim::court
