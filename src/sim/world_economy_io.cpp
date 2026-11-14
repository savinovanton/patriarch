#include "pat/core/hash_stream.hpp"
#include "pat/sim/world.hpp"

#include <algorithm>
#include <array>
#include <span>
#include <utility>
#include <vector>

#include "state_sections.hpp"

// The economy in the save file and in the hash (E9-T03, docs/dev/economy-model.md section 13).
//
// FILE. The economy lives in sections 32..39 of the extension block of the world state (schema >= 5,
// ADR-0006 D6): a world without economy writes none of them, so a world that never switches it on
// saves the same bytes as before. All integers are little endian; every section has version 1 and is
// read by its own reader that must be consumed completely.
//
//   32 economy        u8 present (= 1) | u64 rules_hash | u32 crown_dynasty | i32 open_month_start |
//                     u8 closed_months (<= 12) | u32 next_loan_id | next_quota_id | next_outcome_seq |
//                     next_incident_seq | next_construction_seq                     (always, when on)
//   33 dynasty        u32 rows (= dynasties) | rows x { u32 manpower_reserve | u8 bankrupt }
//   34 province       u32 rows (= provinces) | rows x { u8 kind | u8 owner_kind | u8 tax_policy |
//                     u8 recruit_policy | u8 slots_total | u16 trade_node | u32 administrator |
//                     i32 control | i32 corruption | i32 unrest | u32 labour_base | i32 grain_stock |
//                     u32 recruit_pool | i64 income_last_month | i32 last_slot_date |
//                     kMaxGoods x i32 capacity | kMaxSlots x u8 slot_building }
//   35 population     u32 rows (= 6 x provinces) | rows x { u32 size | i32 mood | i32 wealth |
//                     i32 growth_modifier | i32 growth_carry }
//   36 accounts       u16 lines | lines x u16 ledger code (ascending, issued) | u32 accounts
//                     (= 1 + dynasties) | closed_months x i32 ring_month_start (month 0 = newest) |
//                     accounts x { i64 treasury | i64 opening | lines x i64 open line |
//                     closed_months x { i64 ring_opening | lines x i64 ring line } }
//                     The ledger codes name the lines, so adding a code later does not shift anything.
//   37 construction   u32 rows | rows x { u32 province | u8 slot | u8 building | u32 payer |
//                     i32 start | i32 finish | i64 paid_kop | u32 seq }                (only if any)
//   38 credit         u32 loans | loans x { u32 id | u32 borrower | i64 principal | i32 rate |
//                     i32 issued | i32 due | i32 interest_carry | u8 overdue | u32 collateral } |
//                     u32 quotas | quotas x { u32 id | u32 dynasty | u8 kind | i64 amount |
//                     i32 period_start | i32 deadline } | u32 outcomes (<= 16) | outcomes x { u32 seq |
//                     u32 dynasty | u8 kind | i64 due | i64 delivered | i32 settled_on | u8 flags }
//                                                                                     (only if any)
//   39 incidents      u32 rows (<= 256) | rows x { u32 seq | i32 date | u32 province | u32 dynasty |
//                     u8 kind | i32 param }                                           (only if any)
//   40 policy         u8 tax_levels | u8 recruit_levels | u32 rows (= dynasties) |
//                     rows x { u8 tax_policy | u8 recruit_policy }      (schema >= 9, E9-T12; always,
//                     when the economy is on; 255 = "not chosen", the default of the rules)
//
// Nothing read here is trusted: counts must match the dynasty and province tables or be plausible for
// the bytes that are left, and everything else (enumerations, ranges, references, ordering) is
// checked by World::check_invariants() once the whole state has been read.
//
// HASH. ExtPart::Economy is the FNV hash of nine sub-hashes (economy_hash_parts), each over one table
// in its stored order; 0 (and absent from `total`) without economy. The statement ring is hashed
// from the oldest closed month to the newest, which is the logical order; the unused months are zero
// by invariant and are not hashed. `rules_hash`, derived caches and dirty bits are not part of it.

namespace pat::sim {

namespace {

using core::Fixed32;
using detail::plausible;

constexpr std::size_t kLines = economy::kLedgerLineCount;
constexpr std::size_t kRing = economy::kLedgerRingMonths;

// Freeze the pre-E8 hash layout by CODE, not by dense index: later codes can be inserted anywhere.
constexpr std::array kLegacyLedgerCodes = {
    economy::LedgerCode::TaxNobility,    economy::LedgerCode::TaxClergy,
    economy::LedgerCode::TaxTownsfolk,   economy::LedgerCode::TaxPeasants,
    economy::LedgerCode::TaxCossacks,    economy::LedgerCode::TaxNatives,
    economy::LedgerCode::HoldingIncome,  economy::LedgerCode::Duties,
    economy::LedgerCode::SalariesIn,     economy::LedgerCode::GrantsIn,
    economy::LedgerCode::Tribute,        economy::LedgerCode::FarmingIncome,
    economy::LedgerCode::Embezzlement,   economy::LedgerCode::BuildingUpkeep,
    economy::LedgerCode::Construction,   economy::LedgerCode::CourtUpkeep,
    economy::LedgerCode::SalariesOut,    economy::LedgerCode::ArmyUpkeep,
    economy::LedgerCode::GiftsBribes,    economy::LedgerCode::ChurchDonations,
    economy::LedgerCode::QuotaMoney,     economy::LedgerCode::Interest,
    economy::LedgerCode::RaidLoss,       economy::LedgerCode::DowryOut,
    economy::LedgerCode::LoanTaken,      economy::LedgerCode::LoanRepaid,
    economy::LedgerCode::LoanIssued,     economy::LedgerCode::LoanCollected,
    economy::LedgerCode::QuotaCollected, economy::LedgerCode::InterestCollected,
    economy::LedgerCode::Adjustment,
};
static_assert(kLegacyLedgerCodes.size() == 31);
static_assert(std::ranges::all_of(kLegacyLedgerCodes, economy::is_issued));

void hash_new_ledger_lines(core::HashStream& hash,
                           std::size_t account,
                           std::uint8_t ordinal,
                           std::span<const economy::Kopecks> lines) {
    for (std::size_t index = 0; index < kLines; ++index) {
        const auto code = economy::ledger_code_at(index);
        if (lines[index] == 0 || std::ranges::find(kLegacyLedgerCodes, code) != kLegacyLedgerCodes.end()) {
            continue;
        }
        hash.u8(0xE8);  // Extension record; no records means exactly the pre-E8 byte stream.
        hash.u64(account);
        hash.u8(ordinal);  // 0 = open; 1..closed = closed months in oldest-first order.
        hash.u16(static_cast<std::uint16_t>(code));
        hash.i64(lines[index]);
    }
}

constexpr std::size_t kDynastyRowBytes = 4 + 1;
constexpr std::size_t kPolicyRowBytes = 2;
constexpr std::size_t kProvinceRowBytes =
    5 + 2 + 4 + 3 * 4 + 4 + 4 + 4 + 8 + 4 + 4 * economy::kMaxGoods + economy::kMaxSlots;
constexpr std::size_t kGroupRowBytes = 5 * 4;
constexpr std::size_t kConstructionRowBytes = 4 + 1 + 1 + 4 + 4 + 4 + 8 + 4;
constexpr std::size_t kLoanRowBytes = 4 + 4 + 8 + 4 + 4 + 4 + 4 + 1 + 4;
constexpr std::size_t kQuotaRowBytes = 4 + 4 + 1 + 8 + 4 + 4;
constexpr std::size_t kOutcomeRowBytes = 4 + 4 + 1 + 8 + 8 + 4 + 1;
constexpr std::size_t kIncidentRowBytes = 4 + 4 + 4 + 4 + 1 + 4;

}  // namespace

const char* economy::economy_hash_part_name(std::size_t part) noexcept {
    switch (static_cast<EconomyHashPart>(part)) {
        case EconomyHashPart::Meta:
            return "meta";
        case EconomyHashPart::Dynasties:
            return "dynasties";
        case EconomyHashPart::Provinces:
            return "provinces";
        case EconomyHashPart::Groups:
            return "groups";
        case EconomyHashPart::Construction:
            return "construction";
        case EconomyHashPart::Loans:
            return "loans";
        case EconomyHashPart::Quotas:
            return "quotas";
        case EconomyHashPart::Incidents:
            return "incidents";
        case EconomyHashPart::Accounts:
            return "accounts";
    }
    return "?";
}

// ---- hash ---------------------------------------------------------------------------------------

std::array<std::uint64_t, economy::kEconomyHashPartCount> World::economy_hash_parts() const {
    std::array<std::uint64_t, economy::kEconomyHashPartCount> out{};
    const economy::EconomyState& e = economy_;
    if (e.meta.present == 0) {
        return out;
    }
    const auto at = [](economy::EconomyHashPart part) { return static_cast<std::size_t>(part); };

    {
        core::HashStream h;
        h.u8(e.meta.present);
        h.id(e.meta.crown_dynasty);
        h.i32(e.meta.open_month_start.days());
        h.u8(e.meta.closed_months);
        h.u32(e.meta.next_loan_id);
        h.u32(e.meta.next_quota_id);
        h.u32(e.meta.next_outcome_seq);
        h.u32(e.meta.next_incident_seq);
        h.u32(e.meta.next_construction_seq);
        out[at(economy::EconomyHashPart::Meta)] = h.value();
    }
    {
        core::HashStream h;
        h.u64(e.dynasties.size());
        for (std::size_t d = 0; d < e.dynasties.size(); ++d) {
            h.u32(e.dynasties.manpower_reserve[d]);
            h.u8(e.dynasties.bankrupt[d]);
            h.u8(e.dynasties.tax_policy[d]);
            h.u8(e.dynasties.recruit_policy[d]);
        }
        out[at(economy::EconomyHashPart::Dynasties)] = h.value();
    }
    {
        const economy::ProvinceEconomy& p = e.provinces;
        core::HashStream h;
        h.u64(p.size());
        for (std::size_t i = 0; i < p.size(); ++i) {
            h.u8(p.kind[i]);
            h.u8(p.owner_kind[i]);
            h.u8(p.tax_policy[i]);
            h.u8(p.recruit_policy[i]);
            h.u8(p.slots_total[i]);
            h.u16(p.trade_node[i]);
            h.id(p.administrator[i]);
            h.fixed(p.control[i]);
            h.fixed(p.corruption[i]);
            h.fixed(p.unrest[i]);
            h.u32(p.labour_base[i]);
            h.fixed(p.grain_stock[i]);
            h.u32(p.recruit_pool[i]);
            h.i64(p.income_last_month[i]);
            h.i32(p.last_slot_date[i].days());
            for (std::size_t g = 0; g < economy::kMaxGoods; ++g) {
                h.fixed(p.capacity[i * economy::kMaxGoods + g]);
            }
            for (std::size_t s = 0; s < economy::kMaxSlots; ++s) {
                h.u8(p.slot_building[i * economy::kMaxSlots + s]);
            }
        }
        out[at(economy::EconomyHashPart::Provinces)] = h.value();
    }
    {
        const economy::PopulationGroups& g = e.groups;
        core::HashStream h;
        h.u64(g.rows());
        for (std::size_t i = 0; i < g.rows(); ++i) {
            h.u32(g.size[i]);
            h.fixed(g.mood[i]);
            h.fixed(g.wealth[i]);
            h.i32(g.growth_modifier[i]);
            h.i32(g.growth_carry[i]);
        }
        out[at(economy::EconomyHashPart::Groups)] = h.value();
    }
    {
        const economy::ConstructionQueue& q = e.construction;
        core::HashStream h;
        h.u64(q.size());
        for (std::size_t i = 0; i < q.size(); ++i) {
            h.id(q.province[i]);
            h.u8(q.slot[i]);
            h.u8(q.building[i]);
            h.id(q.payer[i]);
            h.i32(q.start_date[i].days());
            h.i32(q.finish_date[i].days());
            h.i64(q.paid_kop[i]);
            h.u32(q.seq[i]);
        }
        out[at(economy::EconomyHashPart::Construction)] = h.value();
    }
    {
        const economy::LoanTable& l = e.loans;
        core::HashStream h;
        h.u64(l.size());
        for (std::size_t i = 0; i < l.size(); ++i) {
            h.u32(l.loan_id[i]);
            h.id(l.borrower[i]);
            h.i64(l.principal[i]);
            h.i32(l.rate_ppm_year[i]);
            h.i32(l.issued[i].days());
            h.i32(l.due[i].days());
            h.i32(l.interest_carry[i]);
            h.u8(l.overdue[i]);
            h.id(l.collateral[i]);
        }
        out[at(economy::EconomyHashPart::Loans)] = h.value();
    }
    {
        const economy::QuotaTable& q = e.quotas;
        const economy::QuotaOutcomeRing& o = e.outcomes;
        core::HashStream h;
        h.u64(q.size());
        for (std::size_t i = 0; i < q.size(); ++i) {
            h.u32(q.quota_id[i]);
            h.id(q.dynasty[i]);
            h.u8(q.kind[i]);
            h.i64(q.amount[i]);
            h.i32(q.period_start[i].days());
            h.i32(q.deadline[i].days());
        }
        h.u64(o.size());
        for (std::size_t i = 0; i < o.size(); ++i) {
            h.u32(o.seq[i]);
            h.id(o.dynasty[i]);
            h.u8(o.kind[i]);
            h.i64(o.due[i]);
            h.i64(o.delivered[i]);
            h.i32(o.settled_on[i].days());
            h.u8(o.flags[i]);
        }
        out[at(economy::EconomyHashPart::Quotas)] = h.value();
    }
    {
        const economy::IncidentLog& l = e.incidents;
        core::HashStream h;
        h.u64(l.size());
        for (std::size_t i = 0; i < l.size(); ++i) {
            h.u32(l.seq[i]);
            h.i32(l.date[i].days());
            h.id(l.province[i]);
            h.id(l.dynasty[i]);
            h.u8(l.kind[i]);
            h.i32(l.param[i]);
        }
        out[at(economy::EconomyHashPart::Incidents)] = h.value();
    }
    {
        const economy::AccountTable& a = e.accounts;
        const std::size_t closed = std::min<std::size_t>(e.meta.closed_months, kRing);
        core::HashStream h;
        h.u64(a.size());
        h.u64(kLegacyLedgerCodes.size());
        for (std::size_t m = closed; m-- > 0;) {  // oldest first
            h.i32(a.ring_month_start(m).days());
        }
        for (std::size_t row = 0; row < a.size(); ++row) {
            h.i64(a.treasury(row));
            h.i64(a.opening(row));
            for (const auto code : kLegacyLedgerCodes) {
                h.i64(a.open_line(row, code));
            }
            for (std::size_t m = closed; m-- > 0;) {
                h.i64(a.ring_opening(row, m));
                const auto lines = a.ring_lines(row, m);
                for (const auto code : kLegacyLedgerCodes) {
                    h.i64(lines[*economy::ledger_index(code)]);
                }
            }
        }
        for (std::size_t row = 0; row < a.size(); ++row) {
            hash_new_ledger_lines(h, row, 0, a.open_lines(row));
            for (std::size_t m = closed; m-- > 0;) {
                hash_new_ledger_lines(h, row, static_cast<std::uint8_t>(closed - m), a.ring_lines(row, m));
            }
        }
        out[at(economy::EconomyHashPart::Accounts)] = h.value();
    }

    // Parts that happen to be 0 stay 0: the part is "empty" only as a whole (present == 0). A sub-hash
    // is never replaced by 1 here, so the array is a faithful picture of the tables.
    return out;
}

// ---- writing ------------------------------------------------------------------------------------

void World::economy_write_sections(std::vector<StateSection>& sections) const {
    const economy::EconomyState& e = economy_;
    if (e.meta.present == 0) {
        return;
    }

    {  // 32: scalars
        StateSection s{detail::kSectionEconomy, 1, {}};
        core::ByteWriter& w = s.body;
        w.u8(e.meta.present);
        w.u64(e.meta.rules_hash);
        w.u32(e.meta.crown_dynasty.value());
        w.i32(e.meta.open_month_start.days());
        w.u8(e.meta.closed_months);
        w.u32(e.meta.next_loan_id);
        w.u32(e.meta.next_quota_id);
        w.u32(e.meta.next_outcome_seq);
        w.u32(e.meta.next_incident_seq);
        w.u32(e.meta.next_construction_seq);
        sections.push_back(std::move(s));
    }
    if (e.dynasties.size() != 0) {  // 33
        StateSection s{detail::kSectionEconomyDynasty, 1, {}};
        core::ByteWriter& w = s.body;
        w.u32(static_cast<std::uint32_t>(e.dynasties.size()));
        for (std::size_t d = 0; d < e.dynasties.size(); ++d) {
            w.u32(e.dynasties.manpower_reserve[d]);
            w.u8(e.dynasties.bankrupt[d]);
        }
        sections.push_back(std::move(s));
    }
    if (e.provinces.size() != 0) {  // 34 and 35
        const economy::ProvinceEconomy& p = e.provinces;
        StateSection s{detail::kSectionEconomyProvince, 1, {}};
        core::ByteWriter& w = s.body;
        w.u32(static_cast<std::uint32_t>(p.size()));
        for (std::size_t i = 0; i < p.size(); ++i) {
            w.u8(p.kind[i]);
            w.u8(p.owner_kind[i]);
            w.u8(p.tax_policy[i]);
            w.u8(p.recruit_policy[i]);
            w.u8(p.slots_total[i]);
            w.u16(p.trade_node[i]);
            w.u32(p.administrator[i].value());
            w.i32(p.control[i].raw());
            w.i32(p.corruption[i].raw());
            w.i32(p.unrest[i].raw());
            w.u32(p.labour_base[i]);
            w.i32(p.grain_stock[i].raw());
            w.u32(p.recruit_pool[i]);
            w.i64(p.income_last_month[i]);
            w.i32(p.last_slot_date[i].days());
            for (std::size_t g = 0; g < economy::kMaxGoods; ++g) {
                w.i32(p.capacity[i * economy::kMaxGoods + g].raw());
            }
            for (std::size_t slot = 0; slot < economy::kMaxSlots; ++slot) {
                w.u8(p.slot_building[i * economy::kMaxSlots + slot]);
            }
        }
        sections.push_back(std::move(s));

        StateSection pop{detail::kSectionEconomyPopulation, 1, {}};
        core::ByteWriter& pw = pop.body;
        const economy::PopulationGroups& g = e.groups;
        pw.u32(static_cast<std::uint32_t>(g.rows()));
        for (std::size_t i = 0; i < g.rows(); ++i) {
            pw.u32(g.size[i]);
            pw.i32(g.mood[i].raw());
            pw.i32(g.wealth[i].raw());
            pw.i32(g.growth_modifier[i]);
            pw.i32(g.growth_carry[i]);
        }
        sections.push_back(std::move(pop));
    }
    {  // 36: accounts and the statement
        const economy::AccountTable& a = e.accounts;
        const std::size_t closed = std::min<std::size_t>(e.meta.closed_months, kRing);
        StateSection s{detail::kSectionEconomyAccounts, 1, {}};
        core::ByteWriter& w = s.body;
        w.u16(static_cast<std::uint16_t>(kLines));
        for (const economy::LedgerCode code : economy::kLedgerCodes) {
            w.u16(static_cast<std::uint16_t>(code));
        }
        w.u32(static_cast<std::uint32_t>(a.size()));
        for (std::size_t m = 0; m < closed; ++m) {
            w.i32(a.ring_month_start(m).days());
        }
        for (std::size_t row = 0; row < a.size(); ++row) {
            w.i64(a.treasury(row));
            w.i64(a.opening(row));
            for (const economy::Kopecks line : a.open_lines(row)) {
                w.i64(line);
            }
            for (std::size_t m = 0; m < closed; ++m) {
                w.i64(a.ring_opening(row, m));
                for (const economy::Kopecks line : a.ring_lines(row, m)) {
                    w.i64(line);
                }
            }
        }
        sections.push_back(std::move(s));
    }
    if (e.construction.size() != 0) {  // 37
        const economy::ConstructionQueue& q = e.construction;
        StateSection s{detail::kSectionEconomyConstruction, 1, {}};
        core::ByteWriter& w = s.body;
        w.u32(static_cast<std::uint32_t>(q.size()));
        for (std::size_t i = 0; i < q.size(); ++i) {
            w.u32(q.province[i].value());
            w.u8(q.slot[i]);
            w.u8(q.building[i]);
            w.u32(q.payer[i].value());
            w.i32(q.start_date[i].days());
            w.i32(q.finish_date[i].days());
            w.i64(q.paid_kop[i]);
            w.u32(q.seq[i]);
        }
        sections.push_back(std::move(s));
    }
    if (e.loans.size() != 0 || e.quotas.size() != 0 || e.outcomes.size() != 0) {  // 38
        const economy::LoanTable& l = e.loans;
        const economy::QuotaTable& q = e.quotas;
        const economy::QuotaOutcomeRing& o = e.outcomes;
        StateSection s{detail::kSectionEconomyCredit, 1, {}};
        core::ByteWriter& w = s.body;
        w.u32(static_cast<std::uint32_t>(l.size()));
        for (std::size_t i = 0; i < l.size(); ++i) {
            w.u32(l.loan_id[i]);
            w.u32(l.borrower[i].value());
            w.i64(l.principal[i]);
            w.i32(l.rate_ppm_year[i]);
            w.i32(l.issued[i].days());
            w.i32(l.due[i].days());
            w.i32(l.interest_carry[i]);
            w.u8(l.overdue[i]);
            w.u32(l.collateral[i].value());
        }
        w.u32(static_cast<std::uint32_t>(q.size()));
        for (std::size_t i = 0; i < q.size(); ++i) {
            w.u32(q.quota_id[i]);
            w.u32(q.dynasty[i].value());
            w.u8(q.kind[i]);
            w.i64(q.amount[i]);
            w.i32(q.period_start[i].days());
            w.i32(q.deadline[i].days());
        }
        w.u32(static_cast<std::uint32_t>(o.size()));
        for (std::size_t i = 0; i < o.size(); ++i) {
            w.u32(o.seq[i]);
            w.u32(o.dynasty[i].value());
            w.u8(o.kind[i]);
            w.i64(o.due[i]);
            w.i64(o.delivered[i]);
            w.i32(o.settled_on[i].days());
            w.u8(o.flags[i]);
        }
        sections.push_back(std::move(s));
    }
    if (e.incidents.size() != 0) {  // 39
        const economy::IncidentLog& l = e.incidents;
        StateSection s{detail::kSectionEconomyIncidents, 1, {}};
        core::ByteWriter& w = s.body;
        w.u32(static_cast<std::uint32_t>(l.size()));
        for (std::size_t i = 0; i < l.size(); ++i) {
            w.u32(l.seq[i]);
            w.i32(l.date[i].days());
            w.u32(l.province[i].value());
            w.u32(l.dynasty[i].value());
            w.u8(l.kind[i]);
            w.i32(l.param[i]);
        }
        sections.push_back(std::move(s));
    }
    {  // 40
        StateSection s{detail::kSectionEconomyPolicy, 1, {}};
        core::ByteWriter& w = s.body;
        w.u8(e.meta.tax_levels);
        w.u8(e.meta.recruit_levels);
        w.u32(static_cast<std::uint32_t>(e.dynasties.size()));
        for (std::size_t d = 0; d < e.dynasties.size(); ++d) {
            w.u8(e.dynasties.tax_policy[d]);
            w.u8(e.dynasties.recruit_policy[d]);
        }
        sections.push_back(std::move(s));
    }
}

// ---- reading ------------------------------------------------------------------------------------

const char* World::economy_read_section(std::uint16_t id, core::ByteReader& in) {
    economy::EconomyState& e = economy_;
    if (id != detail::kSectionEconomy && e.meta.present == 0) {
        return "an economy section comes without the economy header";
    }
    const std::size_t province_count = provinces_.size();
    const std::size_t dynasty_count = dynasties_.size();

    switch (id) {
        case detail::kSectionEconomy: {
            if (in.u8() != 1) {
                return "the economy header does not say that the economy is on";
            }
            e.meta.present = 1;
            e.meta.rules_hash = in.u64();
            e.meta.crown_dynasty = DynastyId(in.u32());
            e.meta.open_month_start = Date::from_days(in.i32());
            e.meta.closed_months = in.u8();
            e.meta.next_loan_id = in.u32();
            e.meta.next_quota_id = in.u32();
            e.meta.next_outcome_seq = in.u32();
            e.meta.next_incident_seq = in.u32();
            e.meta.next_construction_seq = in.u32();
            if (e.meta.closed_months > kRing) {
                return "the economy header has more closed months than the ring holds";
            }
            break;
        }
        case detail::kSectionEconomyDynasty: {
            const std::uint32_t rows = in.u32();
            if (rows != dynasty_count || !plausible(in, rows, kDynastyRowBytes)) {
                return "the economy dynasty rows do not match the dynasties";
            }
            e.dynasties.manpower_reserve.resize(rows);
            e.dynasties.bankrupt.resize(rows);
            for (std::uint32_t d = 0; d < rows; ++d) {
                e.dynasties.manpower_reserve[d] = in.u32();
                e.dynasties.bankrupt[d] = in.u8();
            }
            break;
        }
        case detail::kSectionEconomyProvince: {
            const std::uint32_t rows = in.u32();
            if (rows != province_count || !plausible(in, rows, kProvinceRowBytes)) {
                return "the economy province rows do not match the provinces";
            }
            economy::ProvinceEconomy& p = e.provinces;
            p.kind.resize(rows);
            p.owner_kind.resize(rows);
            p.tax_policy.resize(rows);
            p.recruit_policy.resize(rows);
            p.slots_total.resize(rows);
            p.trade_node.resize(rows);
            p.administrator.resize(rows);
            p.control.resize(rows);
            p.corruption.resize(rows);
            p.unrest.resize(rows);
            p.labour_base.resize(rows);
            p.grain_stock.resize(rows);
            p.recruit_pool.resize(rows);
            p.income_last_month.resize(rows);
            p.last_slot_date.resize(rows);
            p.capacity.resize(static_cast<std::size_t>(rows) * economy::kMaxGoods);
            p.slot_building.resize(static_cast<std::size_t>(rows) * economy::kMaxSlots);
            for (std::size_t i = 0; i < rows; ++i) {
                p.kind[i] = in.u8();
                p.owner_kind[i] = in.u8();
                p.tax_policy[i] = in.u8();
                p.recruit_policy[i] = in.u8();
                p.slots_total[i] = in.u8();
                p.trade_node[i] = in.u16();
                p.administrator[i] = CharacterId(in.u32());
                p.control[i] = Fixed32::from_raw(in.i32());
                p.corruption[i] = Fixed32::from_raw(in.i32());
                p.unrest[i] = Fixed32::from_raw(in.i32());
                p.labour_base[i] = in.u32();
                p.grain_stock[i] = Fixed32::from_raw(in.i32());
                p.recruit_pool[i] = in.u32();
                p.income_last_month[i] = in.i64();
                p.last_slot_date[i] = Date::from_days(in.i32());
                for (std::size_t g = 0; g < economy::kMaxGoods; ++g) {
                    p.capacity[i * economy::kMaxGoods + g] = Fixed32::from_raw(in.i32());
                }
                for (std::size_t s = 0; s < economy::kMaxSlots; ++s) {
                    p.slot_building[i * economy::kMaxSlots + s] = in.u8();
                }
            }
            break;
        }
        case detail::kSectionEconomyPopulation: {
            const std::uint32_t rows = in.u32();
            if (rows != province_count * economy::kGroupCount || !plausible(in, rows, kGroupRowBytes)) {
                return "the economy population rows do not match the provinces";
            }
            economy::PopulationGroups& g = e.groups;
            g.size.resize(rows);
            g.mood.resize(rows);
            g.wealth.resize(rows);
            g.growth_modifier.resize(rows);
            g.growth_carry.resize(rows);
            for (std::size_t i = 0; i < rows; ++i) {
                g.size[i] = in.u32();
                g.mood[i] = Fixed32::from_raw(in.i32());
                g.wealth[i] = Fixed32::from_raw(in.i32());
                g.growth_modifier[i] = in.i32();
                g.growth_carry[i] = in.i32();
            }
            break;
        }
        case detail::kSectionEconomyAccounts: {
            const std::uint16_t line_count = in.u16();
            if (line_count > kLines || !plausible(in, line_count, 2)) {
                return "the economy statement has an impossible number of lines";
            }
            // The codes tell where each stored line goes; an unissued or unordered code is damage.
            std::array<std::size_t, kLines> dense{};
            std::uint32_t previous = 0;
            for (std::uint16_t i = 0; i < line_count; ++i) {
                const std::uint16_t code = in.u16();
                const auto index = economy::ledger_index(static_cast<economy::LedgerCode>(code));
                if (!in.ok() || !index || (i > 0 && code <= previous)) {
                    return "the economy statement names an unknown or unordered ledger code";
                }
                dense[i] = *index;
                previous = code;
            }
            const std::uint32_t accounts = in.u32();
            const std::size_t closed = e.meta.closed_months;
            const std::size_t account_bytes = 16 + 8 * static_cast<std::size_t>(line_count) +
                                              closed * (8 + 8 * static_cast<std::size_t>(line_count));
            const std::uint64_t needed =
                static_cast<std::uint64_t>(closed) * 4 + static_cast<std::uint64_t>(accounts) * account_bytes;
            if (accounts != 1 + dynasty_count || !in.ok() || needed > in.remaining()) {
                return "the economy accounts do not match the dynasties";
            }
            economy::AccountTable& a = e.accounts;
            a.treasury_.assign(accounts, 0);
            a.opening_.assign(accounts, 0);
            a.open_lines_.assign(static_cast<std::size_t>(accounts) * kLines, 0);
            a.ring_opening_.assign(static_cast<std::size_t>(accounts) * kRing, 0);
            a.ring_lines_.assign(static_cast<std::size_t>(accounts) * kRing * kLines, 0);
            a.ring_month_start_ = {};
            for (std::size_t m = 0; m < closed; ++m) {
                a.ring_month_start_[m] = Date::from_days(in.i32());
            }
            for (std::size_t row = 0; row < accounts; ++row) {
                a.treasury_[row] = in.i64();
                a.opening_[row] = in.i64();
                for (std::uint16_t i = 0; i < line_count; ++i) {
                    a.open_lines_[row * kLines + dense[i]] = in.i64();
                }
                for (std::size_t m = 0; m < closed; ++m) {
                    a.ring_opening_[row * kRing + m] = in.i64();
                    for (std::uint16_t i = 0; i < line_count; ++i) {
                        a.ring_lines_[(row * kRing + m) * kLines + dense[i]] = in.i64();
                    }
                }
            }
            break;
        }
        case detail::kSectionEconomyConstruction: {
            const std::uint32_t rows = in.u32();
            if (!plausible(in, rows, kConstructionRowBytes)) {
                return "the construction queue has an impossible length";
            }
            economy::ConstructionQueue& q = e.construction;
            for (std::uint32_t i = 0; i < rows; ++i) {
                q.province.emplace_back(in.u32());
                q.slot.push_back(in.u8());
                q.building.push_back(in.u8());
                q.payer.emplace_back(in.u32());
                q.start_date.push_back(Date::from_days(in.i32()));
                q.finish_date.push_back(Date::from_days(in.i32()));
                q.paid_kop.push_back(in.i64());
                q.seq.push_back(in.u32());
            }
            break;
        }
        case detail::kSectionEconomyCredit: {
            const std::uint32_t loans = in.u32();
            if (!plausible(in, loans, kLoanRowBytes)) {
                return "the loan table has an impossible length";
            }
            economy::LoanTable& l = e.loans;
            for (std::uint32_t i = 0; i < loans; ++i) {
                l.loan_id.push_back(in.u32());
                l.borrower.emplace_back(in.u32());
                l.principal.push_back(in.i64());
                l.rate_ppm_year.push_back(in.i32());
                l.issued.push_back(Date::from_days(in.i32()));
                l.due.push_back(Date::from_days(in.i32()));
                l.interest_carry.push_back(in.i32());
                l.overdue.push_back(in.u8());
                l.collateral.emplace_back(in.u32());
            }
            const std::uint32_t quotas = in.u32();
            if (!plausible(in, quotas, kQuotaRowBytes)) {
                return "the quota table has an impossible length";
            }
            economy::QuotaTable& q = e.quotas;
            for (std::uint32_t i = 0; i < quotas; ++i) {
                q.quota_id.push_back(in.u32());
                q.dynasty.emplace_back(in.u32());
                q.kind.push_back(in.u8());
                q.amount.push_back(in.i64());
                q.period_start.push_back(Date::from_days(in.i32()));
                q.deadline.push_back(Date::from_days(in.i32()));
            }
            const std::uint32_t outcomes = in.u32();
            if (outcomes > economy::kQuotaOutcomeRingSize || !plausible(in, outcomes, kOutcomeRowBytes)) {
                return "the ring of quota outcomes has an impossible length";
            }
            economy::QuotaOutcomeRing& o = e.outcomes;
            for (std::uint32_t i = 0; i < outcomes; ++i) {
                o.seq.push_back(in.u32());
                o.dynasty.emplace_back(in.u32());
                o.kind.push_back(in.u8());
                o.due.push_back(in.i64());
                o.delivered.push_back(in.i64());
                o.settled_on.push_back(Date::from_days(in.i32()));
                o.flags.push_back(in.u8());
            }
            break;
        }
        case detail::kSectionEconomyIncidents: {
            const std::uint32_t rows = in.u32();
            if (rows > economy::kIncidentLogSize || !plausible(in, rows, kIncidentRowBytes)) {
                return "the incident log has an impossible length";
            }
            economy::IncidentLog& l = e.incidents;
            for (std::uint32_t i = 0; i < rows; ++i) {
                l.seq.push_back(in.u32());
                l.date.push_back(Date::from_days(in.i32()));
                l.province.emplace_back(in.u32());
                l.dynasty.emplace_back(in.u32());
                l.kind.push_back(in.u8());
                l.param.push_back(in.i32());
            }
            break;
        }
        case detail::kSectionEconomyPolicy: {
            e.meta.tax_levels = in.u8();
            e.meta.recruit_levels = in.u8();
            const std::uint32_t rows = in.u32();
            if (rows != dynasty_count || !plausible(in, rows, kPolicyRowBytes)) {
                return "the economy policy rows do not match the dynasties";
            }
            e.dynasties.tax_policy.resize(rows);
            e.dynasties.recruit_policy.resize(rows);
            for (std::uint32_t d = 0; d < rows; ++d) {
                e.dynasties.tax_policy[d] = in.u8();
                e.dynasties.recruit_policy[d] = in.u8();
            }
            break;
        }
        default:
            return "unknown economy section";
    }
    return in.ok() ? nullptr : "an economy section is truncated";
}

}  // namespace pat::sim
