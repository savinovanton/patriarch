#pragma once

#include "pat/core/fixed.hpp"
#include "pat/sim/date.hpp"
#include "pat/sim/economy/ledger.hpp"
#include "pat/sim/economy/money.hpp"
#include "pat/sim/economy/types.hpp"
#include "pat/sim/entity_ids.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

// State tables of the economy (E9-T02, docs/dev/economy-model.md section 4).
//
// The tables are structure-of-arrays, owned by pat::sim::World (`World::economy()`), and PARALLEL to
// the province and dynasty tables: row `p` of every province table belongs to ProvinceId `p`, row
// `1 + d` of the account table to DynastyId `d` (row 0 is the crown). They are empty until the world
// enables the economy (`World::enable_economy`); a world without them behaves exactly as before.
//
// Reading: every column is public. Writing: anything that is indexed, derived or part of an invariant
// goes through World (population sizes, owner kind, administrator, the building grid, the ledger);
// plain numeric columns are written through `World::economy_columns_mut()`. check_invariants() catches
// whatever slips through.
//
// The tables are part of the state hash (World::compute_hash, ExtPart::Economy) and of the save file
// (sections 32..39, see src/sim/world_economy_io.cpp); the derived data of section 4.10 is not.

namespace pat::sim {
class World;
}

namespace pat::sim::economy {

// ---- sizes of the rings and limits ---------------------------------------------------------------
/// Closed months kept in the statement of an account (economy-model section 4.2).
inline constexpr std::size_t kLedgerRingMonths = 12;
/// Entries of the ring of quota outcomes (section 4.8).
inline constexpr std::size_t kQuotaOutcomeRingSize = 16;
/// Entries of the incident log (section 4.9).
inline constexpr std::size_t kIncidentLogSize = 256;
/// Largest absolute value of a single statement line: balance (+-kMaxTreasuryKop) minus opening
/// balance (+-kMaxTreasuryKop) bounds the sum of the lines; one line may reach the same bound.
inline constexpr Kopecks kMaxLedgerLineKop = 2 * kMaxTreasuryKop;
/// Largest population of a province in persons. The cache `ProvinceTable::population` holds it in
/// thousands as a Q16.16 number, which ends at 32 768 thousand; the cap keeps it exactly
/// representable. (The spec of section 3 allows 4e9 per group; real provinces stay below 2e6.)
inline constexpr std::uint32_t kMaxProvincePersons = 32'000'000;
/// Bounds of the growth columns (section 4.5).
inline constexpr std::int32_t kMaxGrowthModifierPpm = 1'000'000;
inline constexpr std::int32_t kMaxGrowthCarry = 999'999;
/// Exact monthly denominator for annual ppm interest, and the bound of its remainder (section 4.7).
inline constexpr std::int32_t kInterestCarryDenominator = 12'000'000;
inline constexpr std::int32_t kMaxInterestCarry = kInterestCarryDenominator - 1;

// ---- bounds of the Fixed32 columns ---------------------------------------------------------------
inline constexpr core::Fixed32 kMoodMax = core::Fixed32::from_int(100);  // mood: -100..100
inline constexpr core::Fixed32 kMoodMin = core::Fixed32::from_int(-100);
inline constexpr core::Fixed32 kPointsMax = core::Fixed32::from_int(100);    // unrest, control: 0..100
inline constexpr core::Fixed32 kWealthMax = core::Fixed32::from_int(300);    // wealth: 0..300, 100 = norm
inline constexpr core::Fixed32 kCorruptionMax = core::Fixed32::from_int(1);  // corruption: 0..1
inline constexpr core::Fixed32 kStockMax = core::Fixed32::from_int(32'767);  // goods: 0..32767

// Defaults of a freshly created row. A province starts fully controlled, quiet and at the normal
// wealth; scenarios overwrite them.
inline constexpr core::Fixed32 kDefaultWealth = core::Fixed32::from_int(100);
inline constexpr core::Fixed32 kDefaultControl = core::Fixed32::from_int(100);

/// The value of a policy column that says "not chosen": a dynasty row means the `default_level` of the
/// rules, a province row means "follow the owner" (E9-T12). Never a valid level (the rules have at most
/// kMaxPolicyLevels = 8 of them), and it is what a new row starts with, so that the default needs no
/// knowledge of the rules where the row is made.
inline constexpr std::uint8_t kPolicyDefault = 0xFF;

// ---- enumerations (values are stable: they go into saves) ----------------------------------------

/// Who holds a province (economy-model section 4).
enum class OwnerKind : std::uint8_t {
    Unowned = 0,
    Dynasty = 1,  ///< `ProvinceTable::owner` is valid; the only kind that has an owner
    Crown = 2,
    Church = 3,  ///< reserved for E31
    Free = 4,    ///< reserved
};
inline constexpr std::uint8_t kOwnerKindCount = 5;

enum class AccountKind : std::uint8_t {
    Crown = 0,
    Dynasty = 1,
};

/// Reference to an account: the crown, or the account of a dynasty.
struct AccountRef {
    AccountKind kind = AccountKind::Crown;
    DynastyId dynasty;  ///< valid only for AccountKind::Dynasty

    [[nodiscard]] static constexpr AccountRef crown() noexcept { return AccountRef{AccountKind::Crown, {}}; }
    [[nodiscard]] static constexpr AccountRef of(DynastyId dynasty) noexcept {
        return AccountRef{AccountKind::Dynasty, dynasty};
    }
    [[nodiscard]] constexpr bool operator==(const AccountRef&) const noexcept = default;
};

enum class QuotaKind : std::uint8_t { Money = 0, Recruits = 1 };
inline constexpr std::uint8_t kQuotaKindCount = 2;
/// Bits of `QuotaOutcomeRing::flags`.
inline constexpr std::uint8_t kQuotaOutcomeShort = 1U << 0;    ///< delivered less than due
inline constexpr std::uint8_t kQuotaOutcomePenalty = 1U << 1;  ///< the penalty was applied
inline constexpr std::uint8_t kQuotaOutcomeKnownFlags = kQuotaOutcomeShort | kQuotaOutcomePenalty;

/// Kinds of the incident log (section 4.9). 9..15 are reserved (M2: famine, epidemics).
enum class IncidentKind : std::uint8_t {
    Riot = 1,
    UnrestStateChanged = 2,
    Anarchy = 3,   ///< control below 15
    Uprising = 4,  ///< unrest at 75 or above
    Bankruptcy = 5,
    LoanOverdue = 6,
    QuotaShort = 7,
    BuildingCompleted = 8,
};
inline constexpr std::uint8_t kMaxIncidentKind = 15;

// ---- scalars -------------------------------------------------------------------------------------

/// Scalars of the economy (section 4.1).
struct EconomyMeta {
    /// 0 = the world has no economy: every table below is empty.
    std::uint8_t present = 0;
    /// Hash of the rules the world was built with. Informational; not part of the state hash (E9-T03).
    std::uint64_t rules_hash = 0;
    /// The dynasty that holds the crown's money in the "one treasury" mode of open question Q7;
    /// invalid in the default mode, where the crown has its own account (row 0).
    DynastyId crown_dynasty;
    /// First day of the month the open statement lines belong to.
    Date open_month_start;
    /// Closed months in the ring of the statement, 0..kLedgerRingMonths.
    std::uint8_t closed_months = 0;
    // Monotonic counters for stable references and cursors.
    std::uint32_t next_loan_id = 0;
    std::uint32_t next_quota_id = 0;
    std::uint32_t next_outcome_seq = 0;
    std::uint32_t next_incident_seq = 0;
    std::uint32_t next_construction_seq = 0;
    /// How many levels the tax and the recruit policy have in the rules the world was built with
    /// (E9-T12): the range that SetTaxPolicy / SetRecruitPolicy accept. 0 = not set, then only the
    /// structural limit kMaxPolicyLevels holds. Informational like `rules_hash`: saved, not hashed
    /// (the rules decide it), checked against the rules by the rules invariant E-R1.
    std::uint8_t tax_levels = 0;
    std::uint8_t recruit_levels = 0;

    [[nodiscard]] bool operator==(const EconomyMeta&) const = default;
};

// ---- accounts and the statement ------------------------------------------------------------------

/// Accounts and their statement (section 4.2): row 0 is the crown, row `1 + d` is dynasty `d`. The
/// During play the balance and the lines change only through World::post_money / World::transfer,
/// and the ring only
/// through World::close_ledger_month, so that `treasury == opening + sum(open lines)` always holds
/// (invariant E-M1). The data is private; read through the accessors.
/// A fresh scenario may seed both balance and opening through World::set_initial_treasury before
/// the first tick, with no postings or closed months.
///
/// The ring holds the last kLedgerRingMonths closed months, index 0 being the most recent; an entry
/// beyond `EconomyMeta::closed_months` is all zero. Lines are indexed by the dense index of the
/// ledger code (`ledger_index`), see ledger.hpp.
class AccountTable {
public:
    [[nodiscard]] std::size_t size() const noexcept { return treasury_.size(); }
    [[nodiscard]] Kopecks treasury(std::size_t account) const noexcept { return treasury_[account]; }
    /// Balance at the start of the open month.
    [[nodiscard]] Kopecks opening(std::size_t account) const noexcept { return opening_[account]; }
    /// The lines of the open month, one per ledger code (kLedgerLineCount).
    [[nodiscard]] std::span<const Kopecks> open_lines(std::size_t account) const noexcept {
        return std::span<const Kopecks>(open_lines_).subspan(account * kLedgerLineCount, kLedgerLineCount);
    }
    /// One line of the open month; 0 for a number that was never issued.
    [[nodiscard]] Kopecks open_line(std::size_t account, LedgerCode code) const noexcept {
        const auto index = ledger_index(code);
        return index ? open_lines(account)[*index] : 0;
    }
    /// Opening balance of closed month `month` (0 = most recent).
    [[nodiscard]] Kopecks ring_opening(std::size_t account, std::size_t month) const noexcept {
        return ring_opening_[account * kLedgerRingMonths + month];
    }
    /// The lines of closed month `month` (0 = most recent).
    [[nodiscard]] std::span<const Kopecks> ring_lines(std::size_t account, std::size_t month) const noexcept {
        return std::span<const Kopecks>(ring_lines_)
            .subspan((account * kLedgerRingMonths + month) * kLedgerLineCount, kLedgerLineCount);
    }
    /// First day of closed month `month` (the same for all accounts).
    [[nodiscard]] Date ring_month_start(std::size_t month) const noexcept { return ring_month_start_[month]; }

    /// Sum of statement lines, saturating (a damaged state must not overflow the checker).
    [[nodiscard]] static Kopecks sum(std::span<const Kopecks> lines) noexcept {
        Kopecks total = 0;
        for (const Kopecks line : lines) {
            total = saturating_add(total, line);
        }
        return total;
    }

    [[nodiscard]] bool operator==(const AccountTable&) const = default;

private:
    friend class pat::sim::World;
    friend struct AccountTableTestAccess;  // defined by tests that deliberately damage a table

    std::vector<Kopecks> treasury_;
    std::vector<Kopecks> opening_;
    std::vector<Kopecks> open_lines_;    // [account * kLedgerLineCount + line]
    std::vector<Kopecks> ring_opening_;  // [account * kLedgerRingMonths + month]
    std::vector<Kopecks> ring_lines_;    // [(account * kLedgerRingMonths + month) * kLedgerLineCount + line]
    std::array<Date, kLedgerRingMonths> ring_month_start_{};
};

// ---- per-dynasty, per-province and per-group columns ---------------------------------------------

/// Per dynasty (section 4.3), parallel to DynastyTable.
struct DynastyEconomy {
    std::vector<std::uint32_t> manpower_reserve;  ///< people
    std::vector<std::uint8_t> bankrupt;           ///< 0 / 1
    /// The policy of the dynasty for all its provinces (E9-T12, GDD 03 section 9): a level index or
    /// kPolicyDefault ("not chosen": the `default_level` of the rules). Set by SetTaxPolicy.
    std::vector<std::uint8_t> tax_policy;
    /// Same for the recruit policy; set by SetRecruitPolicy.
    std::vector<std::uint8_t> recruit_policy;

    [[nodiscard]] std::size_t size() const noexcept { return manpower_reserve.size(); }
    [[nodiscard]] bool operator==(const DynastyEconomy&) const = default;
};

/// Per province (section 4.4), parallel to ProvinceTable. `development` stays in ProvinceTable.
struct ProvinceEconomy {
    std::vector<std::uint8_t> kind;        ///< ProvinceKind
    std::vector<std::uint8_t> owner_kind;  ///< OwnerKind; Dynasty <=> ProvinceTable::owner is valid
    /// A decree for this one province: a level index, or kPolicyDefault = "follow the owner" (the
    /// policy of the owner dynasty, see effective_tax_level). A new row follows the owner; no command
    /// writes this column (E9-T12 made the policy a matter of the dynasty), it is for scenarios.
    std::vector<std::uint8_t> tax_policy;
    std::vector<std::uint8_t> recruit_policy;  ///< same
    std::vector<std::uint8_t> slots_total;     ///< 0..kMaxSlots
    std::vector<std::uint16_t> trade_node;     ///< 0 = none
    std::vector<CharacterId> administrator;    ///< invalid = none; otherwise alive
    std::vector<core::Fixed32> control;        ///< 0..100
    std::vector<core::Fixed32> corruption;     ///< 0..1
    std::vector<core::Fixed32> unrest;         ///< 0..100
    std::vector<std::uint32_t> labour_base;    ///< people at the start; the base of the labour factor
    std::vector<core::Fixed32> grain_stock;    ///< >= 0
    std::vector<std::uint32_t> recruit_pool;   ///< people
    std::vector<Kopecks> income_last_month;    ///< net income of the owner in the last processed slot
    std::vector<Date> last_slot_date;          ///< kNoDate = never processed
    /// Output potential per goods slot, units per month: [province * kMaxGoods + good].
    std::vector<core::Fixed32> capacity;
    /// Buildings in the slots, 0 = empty: [province * kMaxSlots + slot].
    std::vector<std::uint8_t> slot_building;

    [[nodiscard]] std::size_t size() const noexcept { return kind.size(); }
    [[nodiscard]] bool operator==(const ProvinceEconomy&) const = default;
};

/// Population groups (section 4.5): `kGroupCount` rows per province, flat index
/// `province * kGroupCount + group`.
struct PopulationGroups {
    std::vector<std::uint32_t> size;            ///< people (change through World::set_group_size)
    std::vector<core::Fixed32> mood;            ///< -100..100
    std::vector<core::Fixed32> wealth;          ///< 0..300, 100 = norm
    std::vector<std::int32_t> growth_modifier;  ///< ppm per year, -1e6..1e6
    std::vector<std::int32_t> growth_carry;     ///< micro-people, (-1e6, 1e6)

    [[nodiscard]] std::size_t rows() const noexcept { return size.size(); }
    [[nodiscard]] bool operator==(const PopulationGroups&) const = default;
};

[[nodiscard]] constexpr std::size_t group_row(std::size_t province_index, Group group) noexcept {
    return province_index * kGroupCount + index_of(group);
}

// ---- buildings, loans, quotas, incidents (empty until their tasks) -------------------------------

/// Construction queue (section 4.6), ordered by `(finish_date, seq)`.
struct ConstructionQueue {
    std::vector<ProvinceId> province;
    std::vector<std::uint8_t> slot;
    std::vector<std::uint8_t> building;
    std::vector<DynastyId> payer;
    std::vector<Date> start_date;
    std::vector<Date> finish_date;
    std::vector<Kopecks> paid_kop;
    std::vector<std::uint32_t> seq;

    [[nodiscard]] std::size_t size() const noexcept { return province.size(); }
    [[nodiscard]] bool operator==(const ConstructionQueue&) const = default;
};

/// Loans (section 4.7), ordered by ascending `loan_id`. The creditor is always the crown.
struct LoanTable {
    std::vector<std::uint32_t> loan_id;
    std::vector<DynastyId> borrower;
    std::vector<Kopecks> principal;  ///< >= 0
    std::vector<std::int32_t> rate_ppm_year;
    std::vector<Date> issued;
    std::vector<Date> due;
    std::vector<std::int32_t> interest_carry;  ///< 0..kMaxInterestCarry, annual ppm * kopecks
    std::vector<std::uint8_t> overdue;         ///< 0 / 1
    std::vector<ProvinceId> collateral;        ///< reserved (Q3), invalid in M1

    [[nodiscard]] std::size_t size() const noexcept { return loan_id.size(); }
    [[nodiscard]] bool operator==(const LoanTable&) const = default;
};

/// Quotas (section 4.8), ordered by ascending `quota_id`.
struct QuotaTable {
    std::vector<std::uint32_t> quota_id;
    std::vector<DynastyId> dynasty;
    std::vector<std::uint8_t> kind;    ///< QuotaKind
    std::vector<std::int64_t> amount;  ///< kopecks or people, > 0
    std::vector<Date> period_start;
    std::vector<Date> deadline;  ///< > period_start

    [[nodiscard]] std::size_t size() const noexcept { return quota_id.size(); }
    [[nodiscard]] bool operator==(const QuotaTable&) const = default;
};

/// Ring of the last kQuotaOutcomeRingSize settled quotas, oldest first, ascending `seq`.
struct QuotaOutcomeRing {
    std::vector<std::uint32_t> seq;
    std::vector<DynastyId> dynasty;
    std::vector<std::uint8_t> kind;  ///< QuotaKind
    std::vector<std::int64_t> due;
    std::vector<std::int64_t> delivered;
    std::vector<Date> settled_on;
    std::vector<std::uint8_t> flags;  ///< kQuotaOutcome* bits

    [[nodiscard]] std::size_t size() const noexcept { return seq.size(); }
    [[nodiscard]] bool operator==(const QuotaOutcomeRing&) const = default;
};

/// Ring of the last kIncidentLogSize incidents, oldest first, ascending `seq`. The only writer is
/// World::log_incident; event triggers (E11) read entries whose `seq` is above their cursor.
struct IncidentLog {
    std::vector<std::uint32_t> seq;
    std::vector<Date> date;
    std::vector<ProvinceId> province;  ///< invalid when the incident is not provincial
    std::vector<DynastyId> dynasty;    ///< invalid when no dynasty is involved
    std::vector<std::uint8_t> kind;    ///< IncidentKind
    std::vector<std::int32_t> param;

    [[nodiscard]] std::size_t size() const noexcept { return seq.size(); }
    [[nodiscard]] bool operator==(const IncidentLog&) const = default;
};

/// All the state of the economy. A copy of the World copies it; comparison covers every table.
struct EconomyState {
    EconomyMeta meta;
    AccountTable accounts;
    DynastyEconomy dynasties;
    ProvinceEconomy provinces;
    PopulationGroups groups;
    ConstructionQueue construction;
    LoanTable loans;
    QuotaTable quotas;
    QuotaOutcomeRing outcomes;
    IncidentLog incidents;

    [[nodiscard]] bool operator==(const EconomyState&) const = default;
};

/// The sub-hashes the economy hash is built from (economy-model section 13.1), in the order they are
/// mixed. They let a test or a tool find out WHICH table diverged; the game compares only the sum
/// (`WorldHash::ext[ExtPart::Economy]`). All are 0 in a world without economy.
enum class EconomyHashPart : std::uint8_t {
    Meta = 0,      ///< the scalars (not `rules_hash`)
    Dynasties,     ///< DynastyEconomy
    Provinces,     ///< ProvinceEconomy, the building grid included
    Groups,        ///< PopulationGroups
    Construction,  ///< ConstructionQueue
    Loans,         ///< LoanTable
    Quotas,        ///< QuotaTable and QuotaOutcomeRing
    Incidents,     ///< IncidentLog
    Accounts,      ///< balances and statement, ring in the order old -> new
};
inline constexpr std::size_t kEconomyHashPartCount = 9;

/// Name of a sub-hash for diagnostics.
[[nodiscard]] const char* economy_hash_part_name(std::size_t part) noexcept;

/// Writable views of the plain numeric columns (World::economy_columns_mut). The spans have the
/// size of the tables and cannot grow them. Values are NOT clamped: check_invariants() reports a
/// value out of its range, so a system must clamp what it writes.
struct EconomyColumnsMut {
    // per population group (flat index `province * kGroupCount + group`)
    std::span<core::Fixed32> mood;
    std::span<core::Fixed32> wealth;
    std::span<std::int32_t> growth_modifier;
    std::span<std::int32_t> growth_carry;
    // per province
    std::span<std::uint8_t> kind;
    std::span<std::uint8_t> tax_policy;
    std::span<std::uint8_t> recruit_policy;
    std::span<std::uint8_t> slots_total;
    std::span<std::uint16_t> trade_node;
    std::span<core::Fixed32> control;
    std::span<core::Fixed32> corruption;
    std::span<core::Fixed32> unrest;
    std::span<std::uint32_t> labour_base;
    std::span<core::Fixed32> grain_stock;
    std::span<std::uint32_t> recruit_pool;
    std::span<Kopecks> income_last_month;
    std::span<Date> last_slot_date;
    std::span<core::Fixed32> capacity;  ///< [province * kMaxGoods + good]
    // per dynasty
    std::span<std::uint32_t> manpower_reserve;
    std::span<std::uint8_t> bankrupt;
};

}  // namespace pat::sim::economy
