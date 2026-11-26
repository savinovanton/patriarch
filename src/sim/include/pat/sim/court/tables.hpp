#pragma once

#include "pat/core/string_interner.hpp"
#include "pat/sim/date.hpp"
#include "pat/sim/economy/money.hpp"
#include "pat/sim/entity_ids.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

// World-owned state of the court (E10-T02, docs/dev/court-model.md section 4, ADR-0009 D1-D3, D5, D6):
// the counters and the servant market, the held seats, disgraces, vacancies, the ring of the emperor's
// decisions, petitions, servants (SoA) and favor/services/party per dynasty. The office portfolios of
// section 20.4 live next to them in court/portfolio.hpp. The tables are sparse (empty in a world without
// a court), sorted by key and bounded. World owns every mutation, bound and reference check; these types
// contain no rules. Persistence and hashing are E10-T03; the commands and systems are E10-T05 and later.

namespace pat::sim::court {

using OfficeId = std::uint32_t;
using VacancyId = std::uint32_t;
using PetitionId = std::uint32_t;
using ServantId = std::uint32_t;
using DecisionSeq = std::uint32_t;
/// The stable `slot` of an office type in the data (1...63).
using OfficeTypeId = std::uint8_t;

// ---- bounds (section 4) --------------------------------------------------------------------------------

inline constexpr std::size_t kMaxOffices = 512;
inline constexpr std::size_t kMaxVacancies = 128;
inline constexpr std::size_t kMaxPetitions = 256;
inline constexpr std::size_t kMaxServants = 10000;
inline constexpr std::size_t kMaxDisgraces = 256;
inline constexpr std::size_t kDecisionRingSize = 32;
inline constexpr std::size_t kDecisionContenders = 4;
inline constexpr std::size_t kDecisionTerms = 8;
inline constexpr OfficeTypeId kMaxOfficeTypeSlot = 63;
/// Structural ceilings of numbers the rules bound more tightly (the corridors of CourtBalance):
/// offices per person (max_offices_per_person), petitions per dynasty (max_petitions_per_dynasty) and
/// postponements of one vacancy (max_postponements). check_court_invariants() checks the rules' values.
inline constexpr std::size_t kMaxOfficesPerHolder = 8;
inline constexpr std::size_t kMaxPetitionsPerDynasty = 64;
inline constexpr std::uint8_t kMaxPostponements = 16;

inline constexpr std::int32_t kMinFavorX100 = 0;
inline constexpr std::int32_t kMaxFavorX100 = 10000;
/// The favor of a dynasty without a row (a code constant, not data; section 4.8).
inline constexpr std::int32_t kDefaultFavorX100 = 5000;
inline constexpr std::uint8_t kMaxServantLoyalty = 100;
inline constexpr std::uint8_t kMaxServantSkill = 20;
inline constexpr std::uint8_t kMaxPressureBand = 3;

// ---- enumerations (numeric values are stable: the E10-T03 save stores them) ----------------------------

enum class ServantRole : std::uint8_t { Steward = 1, Spy = 2, Secretary = 3, Guard = 4 };
inline constexpr std::uint8_t kLastServantRole = 4;  ///< 5...15 are reserved (M2)

/// `party` of a favor row; 0 is "outside the parties", 4...7 are reserved.
enum class Party : std::uint8_t { None = 0, OldNobility = 1, NewService = 2, Church = 3 };
inline constexpr std::uint8_t kLastParty = 3;

enum class DisgraceReason : std::uint8_t { Sovereign = 1, Investigation = 2, Event = 3 };
inline constexpr std::uint8_t kLastDisgraceReason = 3;

enum class VacancyReason : std::uint8_t {
    NewSeat = 1,
    HolderDied = 2,
    Resigned = 3,
    Dismissed = 4,
    Tonsured = 5,
    Ineligible = 6,
    Reform = 7,
};
inline constexpr std::uint8_t kLastVacancyReason = 7;

enum class DecisionResult : std::uint8_t { Appointed = 1, Postponed = 2, NoCandidate = 3 };
inline constexpr std::uint8_t kLastDecisionResult = 3;

// ---- flags ---------------------------------------------------------------------------------------------

/// The holder does not meet the soft requirements of the type ("in exceptional order").
inline constexpr std::uint8_t kOfficeExceptional = 1U << 0;
/// Appointed by the scenario or the system (start data); exempt from the requirement check C-3.
inline constexpr std::uint8_t kOfficeBySystem = 1U << 1;
/// `locus` is a ProvinceId (the seat of a regional type); set by the writer from the seat rule of the
/// type, so World can index offices by province without the rules (E10-T02 addition to section 4.2).
inline constexpr std::uint8_t kOfficeProvinceSeat = 1U << 2;
inline constexpr std::uint8_t kOfficeFlagMask = kOfficeExceptional | kOfficeBySystem | kOfficeProvinceSeat;

inline constexpr std::uint8_t kPetitionService = 1U << 0;  ///< a "service" was used
inline constexpr std::uint8_t kPetitionParty = 1U << 1;    ///< the party supports the petition
inline constexpr std::uint8_t kPetitionFlagMask = kPetitionService | kPetitionParty;

// ---- rows ----------------------------------------------------------------------------------------------

/// The counters and the seed of the servant market (section 4.1, plus `next_order_seq` of section 20.4).
/// Counters start at 1 and are never reused.
struct Meta {
    OfficeId next_office_id = 1;
    VacancyId next_vacancy_id = 1;
    PetitionId next_petition_id = 1;
    ServantId next_servant_id = 1;
    DecisionSeq next_decision_seq = 1;
    std::uint32_t next_order_seq = 1;
    /// Month of the current market seed: (year - 1600) * 12 + (month - 1); -1 = no market yet.
    std::int32_t market_month = -1;
    std::uint64_t market_seed = 0;

    [[nodiscard]] bool operator==(const Meta&) const = default;
};

/// A held seat (section 4.2). The seat (type, locus) is unique; a seat without a holder is a vacancy.
struct Office {
    OfficeId id = 0;
    OfficeTypeId type = 0;
    std::uint32_t locus = 0;  ///< seat number 0...slots-1, or the raw ProvinceId (kOfficeProvinceSeat)
    CharacterId holder;
    DynastyId appointer;  ///< invalid: appointed by the system
    Date since = kNoDate;
    CharacterId patron;  ///< invalid: nobody put him forward
    std::uint8_t flags = 0;

    [[nodiscard]] bool operator==(const Office&) const = default;
};

/// A character in disgrace until `until` (section 4.3); keyed by character.
struct Disgrace {
    CharacterId character;
    Date until = kNoDate;
    DisgraceReason reason = DisgraceReason::Sovereign;

    [[nodiscard]] bool operator==(const Disgrace&) const = default;
};

/// An open seat waiting for the emperor's decision (section 4.4).
struct Vacancy {
    VacancyId id = 0;
    OfficeTypeId type = 0;
    std::uint32_t locus = 0;
    Date opened = kNoDate;
    Date decide_by = kNoDate;
    VacancyReason reason = VacancyReason::NewSeat;
    CharacterId previous_holder;  ///< may be archived
    std::uint8_t postponed = 0;

    [[nodiscard]] bool operator==(const Vacancy&) const = default;
};

/// One contender of a decision: the utility and its terms in the order of section 8.2.
struct Contender {
    CharacterId candidate;
    DynastyId dynasty;
    std::int32_t score = 0;
    std::array<std::int32_t, kDecisionTerms> terms{};
    PetitionId petition = 0;  ///< 0: without a petition

    [[nodiscard]] bool operator==(const Contender&) const = default;
};

/// An outcome of the emperor's decision (section 4.5); the ring keeps the last kDecisionRingSize.
struct Decision {
    DecisionSeq seq = 0;
    VacancyId vacancy = 0;  ///< historical id
    OfficeTypeId type = 0;
    std::uint32_t locus = 0;
    Date decided = kNoDate;
    DecisionResult result = DecisionResult::Appointed;
    std::uint8_t n = 0;  ///< contenders used, the winner first; the rest are default
    std::array<Contender, kDecisionContenders> contenders{};

    [[nodiscard]] bool operator==(const Decision&) const = default;
};

/// A petition of a dynasty for a vacancy (section 4.6); deleted once decided.
struct Petition {
    PetitionId id = 0;
    VacancyId vacancy = 0;
    DynastyId petitioner;
    CharacterId candidate;  ///< a member of `petitioner` when submitted
    Date submitted = kNoDate;
    economy::Kopecks gift_kop = 0;  ///< already paid, >= 0
    std::uint8_t flags = 0;
    std::uint32_t evidence = 0;  ///< reserved for E14 (a secret id); 0 in M1

    [[nodiscard]] bool operator==(const Petition&) const = default;
};

/// One servant as a value (the API of World); stored column-wise in ServantTable.
struct Servant {
    ServantId id = 0;
    DynastyId dynasty;
    core::StringId name;
    std::uint16_t birth_year = 0;
    std::uint16_t hired_month = 0;  ///< as Meta::market_month
    ServantRole role = ServantRole::Steward;
    std::uint8_t sex = 0;      ///< the value of pat::sim::Sex (0 male, 1 female)
    std::uint8_t skill = 0;    ///< 0...20
    std::uint8_t loyalty = 0;  ///< 0...100
    std::uint16_t wage_kop = 0;
    std::uint8_t arrears = 0;  ///< consecutive months of unpaid wages
    std::uint8_t portrait = 0;

    [[nodiscard]] bool operator==(const Servant&) const = default;
};

/// Servants, structure of arrays (24 bytes per row, section 4.7), ascending `id`. Columns have equal
/// length; World is the only writer.
struct ServantTable {
    std::vector<ServantId> id;
    std::vector<DynastyId> dynasty;
    std::vector<core::StringId> name;
    std::vector<std::uint16_t> birth_year;
    std::vector<std::uint16_t> hired_month;
    std::vector<ServantRole> role;
    std::vector<std::uint8_t> sex;
    std::vector<std::uint8_t> skill;
    std::vector<std::uint8_t> loyalty;
    std::vector<std::uint16_t> wage_kop;
    std::vector<std::uint8_t> arrears;
    std::vector<std::uint8_t> portrait;

    [[nodiscard]] std::size_t size() const noexcept { return id.size(); }
    [[nodiscard]] bool empty() const noexcept { return id.empty(); }
    [[nodiscard]] Servant row(std::size_t i) const {
        return {id[i],
                dynasty[i],
                name[i],
                birth_year[i],
                hired_month[i],
                role[i],
                sex[i],
                skill[i],
                loyalty[i],
                wage_kop[i],
                arrears[i],
                portrait[i]};
    }
    [[nodiscard]] bool columns_consistent() const noexcept {
        const std::size_t n = id.size();
        return dynasty.size() == n && name.size() == n && birth_year.size() == n && hired_month.size() == n &&
               role.size() == n && sex.size() == n && skill.size() == n && loyalty.size() == n &&
               wage_kop.size() == n && arrears.size() == n && portrait.size() == n;
    }
    [[nodiscard]] bool operator==(const ServantTable&) const = default;
};

/// Favor, services and party of a dynasty (section 4.8, `pressure_band` of section 20.4). A row exists
/// only while one field differs from its default.
struct Favor {
    DynastyId dynasty;
    std::int32_t favor_x100 = kDefaultFavorX100;
    std::uint8_t services = 0;
    Party party = Party::None;
    Date party_support_on = kNoDate;
    std::int32_t gift_month = -1;
    std::int32_t gift_points_x100 = 0;
    std::uint8_t pressure_band = 0;

    [[nodiscard]] bool operator==(const Favor&) const = default;
};

/// True when every field except the key has its default (such a row is not stored).
[[nodiscard]] inline bool is_default(const Favor& row) {
    Favor blank;
    blank.dynasty = row.dynasty;
    return row == blank;
}

}  // namespace pat::sim::court
