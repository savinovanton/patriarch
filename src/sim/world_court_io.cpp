#include "pat/core/hash_stream.hpp"
#include "pat/sim/court/portfolio.hpp"
#include "pat/sim/court/tables.hpp"
#include "pat/sim/influence.hpp"
#include "pat/sim/world.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "state_sections.hpp"

// E10-T03. Version-1 sections 64..71 of the court (docs/dev/court-model.md sections 13.1 and 20.13) and
// section 10 of the influence (docs/dev/influence-model.md section 17), with the extended hash parts
// 32..37, 39 (court) and 40 (influence). A table that is empty is not written and its hash part is 0
// (ADR-0006 D6/D7), so a world without a court saves and hashes exactly as before schema 14.
// The shared emitters define both the little-endian file fields and the FNV byte representation.
// Loading writes raw rows (the public mutators check today's state, whereas a save is a snapshot of
// an earlier day); only the shape of a row, the bounds and the strict order of the keys are checked here.
// The enumerations, references, counters and the derived indexes are checked by World::check_invariants()
// once every section has been read (C-1..C-16, I-1..I-3).
//
// Layout (every table is `u32 rows | rows x row`, keys strictly ascending):
//   64 court_meta       u32 next_office_id, next_vacancy_id, next_petition_id, next_servant_id,
//                       next_decision_seq, next_order_seq | i32 market_month | u64 market_seed
//   65 court_offices    offices {u32 id, u8 type, u32 locus, u32 holder, u32 appointer, i32 since,
//                       u32 patron, u8 flags}; then disgraces {u32 character, i32 until, u8 reason}
//   66 court_vacancies  vacancies {u32 id, u8 type, u32 locus, i32 opened, i32 decide_by, u8 reason,
//                       u32 previous_holder, u8 postponed}; then the decision ring {u32 seq, u32 vacancy,
//                       u8 type, u32 locus, i32 decided, u8 result, u8 n, n x {u32 candidate, u32 dynasty,
//                       i32 score, 8 x i32 terms, u32 petition}}
//   67 court_petitions  {u32 id, u32 vacancy, u32 petitioner, u32 candidate, i32 submitted, i64 gift_kop,
//                       u8 flags, u32 evidence}
//   68 court_servants   {u32 id, u32 dynasty, u32 name, u16 birth_year, u16 hired_month, u8 role, u8 sex,
//                       u8 skill, u8 loyalty, u16 wage_kop, u8 arrears, u8 portrait}
//   69 court_favor      {u32 dynasty, i32 favor_x100, u8 services, u8 party, i32 party_support_on,
//                       i32 gift_month, i32 gift_points_x100, u8 pressure_band}
//   70 court_portfolio  {u8 type, u32 locus, u8 course, i32 course_since, u32 controller, u32 set_by,
//                       u32 target, u8 state, i32 next_step, 4 x i32 order_ready, u8 flags}
//   71 court_orders     ring {u32 seq, u8 type, u32 locus, u8 order, u32 issuer, u32 holder, i32 date,
//                       u32 target, i64 cost_kop, i32 cost_favor_x100, i16 power, i16 execution, i16 noise}
//   10 influence        balances {u32 dynasty, i32 balance_x100}; then the log ring {i32 date,
//                       u32 dynasty, i32 delta_x100, u16 reason, u16 pad = 0}

namespace pat::sim {
namespace {

constexpr std::size_t kOfficeRowBytes = 4 + 1 + 4 + 4 + 4 + 4 + 4 + 1;
constexpr std::size_t kDisgraceRowBytes = 4 + 4 + 1;
constexpr std::size_t kVacancyRowBytes = 4 + 1 + 4 + 4 + 4 + 1 + 4 + 1;
constexpr std::size_t kContenderBytes = 4 + 4 + 4 + 4 * court::kDecisionTerms + 4;
constexpr std::size_t kDecisionRowBytes = 4 + 4 + 1 + 4 + 4 + 1 + 1;  // without contenders
constexpr std::size_t kPetitionRowBytes = 4 + 4 + 4 + 4 + 4 + 8 + 1 + 4;
constexpr std::size_t kServantRowBytes = 4 + 4 + 4 + 2 + 2 + 1 + 1 + 1 + 1 + 2 + 1 + 1;
constexpr std::size_t kFavorRowBytes = 4 + 4 + 1 + 1 + 4 + 4 + 4 + 1;
constexpr std::size_t kPortfolioRowBytes =
    1 + 4 + 1 + 4 + 4 + 4 + 4 + 1 + 4 + 4 * court::kMaxOrdersPerPortfolio + 1;
constexpr std::size_t kOrderRowBytes = 4 + 1 + 4 + 1 + 4 + 4 + 4 + 4 + 8 + 4 + 2 + 2 + 2;
constexpr std::size_t kInfluenceRowBytes = 4 + 4;
constexpr std::size_t kInfluenceLogBytes = 4 + 4 + 4 + 2 + 2;

static_assert(kServantRowBytes == 24, "court-model.md section 4.7: 24 bytes per servant");
static_assert(kPortfolioRowBytes == 44, "court-model.md section 20.13: 44 bytes per portfolio");

// ---- writers (file and hash) --------------------------------------------------------------------------

template <class Sink>
void write_i16(Sink& out, std::int16_t value) {
    out.u16(static_cast<std::uint16_t>(value));
}

template <class Sink>
void write_row(Sink& out, const court::Office& row) {
    out.u32(row.id);
    out.u8(row.type);
    out.u32(row.locus);
    out.u32(row.holder.value());
    out.u32(row.appointer.value());
    out.i32(row.since.days());
    out.u32(row.patron.value());
    out.u8(row.flags);
}

template <class Sink>
void write_row(Sink& out, const court::Disgrace& row) {
    out.u32(row.character.value());
    out.i32(row.until.days());
    out.u8(static_cast<std::uint8_t>(row.reason));
}

template <class Sink>
void write_row(Sink& out, const court::Vacancy& row) {
    out.u32(row.id);
    out.u8(row.type);
    out.u32(row.locus);
    out.i32(row.opened.days());
    out.i32(row.decide_by.days());
    out.u8(static_cast<std::uint8_t>(row.reason));
    out.u32(row.previous_holder.value());
    out.u8(row.postponed);
}

template <class Sink>
void write_row(Sink& out, const court::Decision& row) {
    out.u32(row.seq);
    out.u32(row.vacancy);
    out.u8(row.type);
    out.u32(row.locus);
    out.i32(row.decided.days());
    out.u8(static_cast<std::uint8_t>(row.result));
    out.u8(row.n);
    for (std::size_t i = 0; i < row.n && i < row.contenders.size(); ++i) {
        const court::Contender& contender = row.contenders[i];
        out.u32(contender.candidate.value());
        out.u32(contender.dynasty.value());
        out.i32(contender.score);
        for (const std::int32_t term : contender.terms) {
            out.i32(term);
        }
        out.u32(contender.petition);
    }
}

template <class Sink>
void write_row(Sink& out, const court::Petition& row) {
    out.u32(row.id);
    out.u32(row.vacancy);
    out.u32(row.petitioner.value());
    out.u32(row.candidate.value());
    out.i32(row.submitted.days());
    out.i64(row.gift_kop);
    out.u8(row.flags);
    out.u32(row.evidence);
}

template <class Sink>
void write_row(Sink& out, const court::Servant& row) {
    out.u32(row.id);
    out.u32(row.dynasty.value());
    out.u32(row.name.value());
    out.u16(row.birth_year);
    out.u16(row.hired_month);
    out.u8(static_cast<std::uint8_t>(row.role));
    out.u8(row.sex);
    out.u8(row.skill);
    out.u8(row.loyalty);
    out.u16(row.wage_kop);
    out.u8(row.arrears);
    out.u8(row.portrait);
}

template <class Sink>
void write_row(Sink& out, const court::Favor& row) {
    out.u32(row.dynasty.value());
    out.i32(row.favor_x100);
    out.u8(row.services);
    out.u8(static_cast<std::uint8_t>(row.party));
    out.i32(row.party_support_on.days());
    out.i32(row.gift_month);
    out.i32(row.gift_points_x100);
    out.u8(row.pressure_band);
}

template <class Sink>
void write_row(Sink& out, const court::Portfolio& row) {
    out.u8(row.type);
    out.u32(row.locus);
    out.u8(row.course);
    out.i32(row.course_since.days());
    out.u32(row.controller.value());
    out.u32(row.set_by.value());
    out.u32(row.target.value());
    out.u8(static_cast<std::uint8_t>(row.state));
    out.i32(row.next_step.days());
    for (const Date ready : row.order_ready) {
        out.i32(ready.days());
    }
    out.u8(row.flags);
}

template <class Sink>
void write_row(Sink& out, const court::PortfolioOrder& row) {
    out.u32(row.seq);
    out.u8(row.type);
    out.u32(row.locus);
    out.u8(row.order);
    out.u32(row.issuer.value());
    out.u32(row.holder.value());
    out.i32(row.date.days());
    out.u32(row.target);
    out.i64(row.cost_kop);
    out.i32(row.cost_favor_x100);
    write_i16(out, row.power_permille);
    write_i16(out, row.execution_permille);
    write_i16(out, row.noise_x100);
}

template <class Sink>
void write_row(Sink& out, const InfluenceRow& row) {
    out.u32(row.dynasty.value());
    out.i32(row.balance_x100);
}

template <class Sink>
void write_row(Sink& out, const InfluenceLogEntry& row) {
    out.i32(row.date.days());
    out.u32(row.dynasty.value());
    out.i32(row.delta_x100);
    out.u16(static_cast<std::uint16_t>(row.reason));
    out.u16(0);  // pad
}

template <class Sink, class Rows>
void write_rows(Sink& out, const Rows& rows) {
    out.u32(static_cast<std::uint32_t>(rows.size()));
    for (const auto& row : rows) {
        write_row(out, row);
    }
}

template <class Sink>
void write_servants(Sink& out, const court::ServantTable& servants) {
    out.u32(static_cast<std::uint32_t>(servants.size()));
    for (std::size_t i = 0; i < servants.size(); ++i) {
        write_row(out, servants.row(i));
    }
}

template <class Sink>
void write_meta(Sink& out, const court::Meta& meta) {
    out.u32(meta.next_office_id);
    out.u32(meta.next_vacancy_id);
    out.u32(meta.next_petition_id);
    out.u32(meta.next_servant_id);
    out.u32(meta.next_decision_seq);
    out.u32(meta.next_order_seq);
    out.i32(meta.market_month);
    out.u64(meta.market_seed);
}

template <class Sink>
void write_body(Sink& out, std::uint16_t id, const court::State& state) {
    switch (id) {
        case detail::kSectionCourtMeta:
            write_meta(out, state.meta);
            break;
        case detail::kSectionCourtOffices:
            write_rows(out, state.offices);
            write_rows(out, state.disgraces);
            break;
        case detail::kSectionCourtVacancies:
            write_rows(out, state.vacancies);
            write_rows(out, state.decisions);
            break;
        case detail::kSectionCourtPetitions:
            write_rows(out, state.petitions);
            break;
        case detail::kSectionCourtServants:
            write_servants(out, state.servants);
            break;
        case detail::kSectionCourtFavor:
            write_rows(out, state.favor);
            break;
        case detail::kSectionCourtPortfolio:
            write_rows(out, state.portfolios);
            break;
        case detail::kSectionCourtOrders:
            write_rows(out, state.orders);
            break;
        default:
            break;
    }
}

template <class Sink>
void write_influence(Sink& out, const InfluenceState& state) {
    write_rows(out, state.rows);
    write_rows(out, state.log);
}

// ---- readers ------------------------------------------------------------------------------------------

Date read_date(core::ByteReader& in) {
    return Date::from_days(in.i32());
}

std::int16_t read_i16(core::ByteReader& in) {
    return static_cast<std::int16_t>(in.u16());
}

bool read_row(core::ByteReader& in, court::Office& row) {
    row.id = in.u32();
    row.type = in.u8();
    row.locus = in.u32();
    row.holder = CharacterId(in.u32());
    row.appointer = DynastyId(in.u32());
    row.since = read_date(in);
    row.patron = CharacterId(in.u32());
    row.flags = in.u8();
    return in.ok();
}

bool read_row(core::ByteReader& in, court::Disgrace& row) {
    row.character = CharacterId(in.u32());
    row.until = read_date(in);
    row.reason = static_cast<court::DisgraceReason>(in.u8());
    return in.ok();
}

bool read_row(core::ByteReader& in, court::Vacancy& row) {
    row.id = in.u32();
    row.type = in.u8();
    row.locus = in.u32();
    row.opened = read_date(in);
    row.decide_by = read_date(in);
    row.reason = static_cast<court::VacancyReason>(in.u8());
    row.previous_holder = CharacterId(in.u32());
    row.postponed = in.u8();
    return in.ok();
}

bool read_row(core::ByteReader& in, court::Decision& row) {
    row.seq = in.u32();
    row.vacancy = in.u32();
    row.type = in.u8();
    row.locus = in.u32();
    row.decided = read_date(in);
    row.result = static_cast<court::DecisionResult>(in.u8());
    row.n = in.u8();
    if (!in.ok() || row.n > court::kDecisionContenders || !in.can_hold(row.n, kContenderBytes)) {
        return false;
    }
    for (std::size_t i = 0; i < row.n; ++i) {
        court::Contender& contender = row.contenders[i];
        contender.candidate = CharacterId(in.u32());
        contender.dynasty = DynastyId(in.u32());
        contender.score = in.i32();
        for (std::int32_t& term : contender.terms) {
            term = in.i32();
        }
        contender.petition = in.u32();
    }
    return in.ok();
}

bool read_row(core::ByteReader& in, court::Petition& row) {
    row.id = in.u32();
    row.vacancy = in.u32();
    row.petitioner = DynastyId(in.u32());
    row.candidate = CharacterId(in.u32());
    row.submitted = read_date(in);
    row.gift_kop = in.i64();
    row.flags = in.u8();
    row.evidence = in.u32();
    return in.ok();
}

bool read_row(core::ByteReader& in, court::Servant& row) {
    row.id = in.u32();
    row.dynasty = DynastyId(in.u32());
    row.name = core::StringId(in.u32());
    row.birth_year = in.u16();
    row.hired_month = in.u16();
    row.role = static_cast<court::ServantRole>(in.u8());
    row.sex = in.u8();
    row.skill = in.u8();
    row.loyalty = in.u8();
    row.wage_kop = in.u16();
    row.arrears = in.u8();
    row.portrait = in.u8();
    return in.ok();
}

bool read_row(core::ByteReader& in, court::Favor& row) {
    row.dynasty = DynastyId(in.u32());
    row.favor_x100 = in.i32();
    row.services = in.u8();
    row.party = static_cast<court::Party>(in.u8());
    row.party_support_on = read_date(in);
    row.gift_month = in.i32();
    row.gift_points_x100 = in.i32();
    row.pressure_band = in.u8();
    return in.ok();
}

bool read_row(core::ByteReader& in, court::Portfolio& row) {
    row.type = in.u8();
    row.locus = in.u32();
    row.course = in.u8();
    row.course_since = read_date(in);
    row.controller = DynastyId(in.u32());
    row.set_by = DynastyId(in.u32());
    row.target = DynastyId(in.u32());
    row.state = static_cast<court::PortfolioState>(in.u8());
    row.next_step = read_date(in);
    for (Date& ready : row.order_ready) {
        ready = read_date(in);
    }
    row.flags = in.u8();
    return in.ok();
}

bool read_row(core::ByteReader& in, court::PortfolioOrder& row) {
    row.seq = in.u32();
    row.type = in.u8();
    row.locus = in.u32();
    row.order = in.u8();
    row.issuer = DynastyId(in.u32());
    row.holder = CharacterId(in.u32());
    row.date = read_date(in);
    row.target = in.u32();
    row.cost_kop = in.i64();
    row.cost_favor_x100 = in.i32();
    row.power_permille = read_i16(in);
    row.execution_permille = read_i16(in);
    row.noise_x100 = read_i16(in);
    return in.ok();
}

bool read_row(core::ByteReader& in, InfluenceRow& row) {
    row.dynasty = DynastyId(in.u32());
    row.balance_x100 = in.i32();
    return in.ok();
}

bool read_row(core::ByteReader& in, InfluenceLogEntry& row) {
    row.date = read_date(in);
    row.dynasty = DynastyId(in.u32());
    row.delta_x100 = in.i32();
    row.reason = static_cast<InfluenceReason>(in.u16());
    const std::uint16_t pad = in.u16();
    return in.ok() && pad == 0;
}

template <class Rows, class Projection>
bool read_rows(
    core::ByteReader& in, Rows& rows, std::size_t limit, std::size_t row_bytes, Projection projection) {
    return detail::read_ordered_rows(
        in, rows, limit, row_bytes, projection, [](core::ByteReader& reader, auto& row) {
            return read_row(reader, row);
        });
}

/// The log of the influence is a ring ordered by date, which may repeat: no key order to check here
/// (I-3 checks that the dates do not decrease).
bool read_influence_log(core::ByteReader& in, std::vector<InfluenceLogEntry>& rows) {
    const std::uint32_t count = in.u32();
    if (!in.ok() || count > kInfluenceLogSize || !in.can_hold(count, kInfluenceLogBytes)) {
        return false;
    }
    rows.resize(count);
    for (InfluenceLogEntry& row : rows) {
        if (!read_row(in, row)) {
            return false;
        }
    }
    return true;
}

std::pair<court::OfficeTypeId, std::uint32_t> portfolio_key(const court::Portfolio& row) {
    return {row.type, row.locus};
}

bool read_meta(core::ByteReader& in, court::Meta& meta) {
    meta.next_office_id = in.u32();
    meta.next_vacancy_id = in.u32();
    meta.next_petition_id = in.u32();
    meta.next_servant_id = in.u32();
    meta.next_decision_seq = in.u32();
    meta.next_order_seq = in.u32();
    meta.market_month = in.i32();
    meta.market_seed = in.u64();
    return in.ok();
}

void store_servants(const std::vector<court::Servant>& rows, court::ServantTable& table) {
    table = court::ServantTable{};
    for (const court::Servant& row : rows) {
        table.id.push_back(row.id);
        table.dynasty.push_back(row.dynasty);
        table.name.push_back(row.name);
        table.birth_year.push_back(row.birth_year);
        table.hired_month.push_back(row.hired_month);
        table.role.push_back(row.role);
        table.sex.push_back(row.sex);
        table.skill.push_back(row.skill);
        table.loyalty.push_back(row.loyalty);
        table.wage_kop.push_back(row.wage_kop);
        table.arrears.push_back(row.arrears);
        table.portrait.push_back(row.portrait);
    }
}

}  // namespace

bool World::court_section_present(std::uint16_t id) const {
    const court::State& c = court_;
    switch (id) {
        case detail::kSectionCourtMeta:
            return !(c.meta == court::Meta{});
        case detail::kSectionCourtOffices:
            return !c.offices.empty() || !c.disgraces.empty();
        case detail::kSectionCourtVacancies:
            return !c.vacancies.empty() || !c.decisions.empty();
        case detail::kSectionCourtPetitions:
            return !c.petitions.empty();
        case detail::kSectionCourtServants:
            return !c.servants.empty();
        case detail::kSectionCourtFavor:
            return !c.favor.empty();
        case detail::kSectionCourtPortfolio:
            return !c.portfolios.empty();
        case detail::kSectionCourtOrders:
            return !c.orders.empty();
        default:
            return false;
    }
}

void World::court_write_sections(std::vector<StateSection>& sections) const {
    for (std::uint16_t id = detail::kSectionCourtMeta; id <= detail::kSectionCourtOrders; ++id) {
        if (court_section_present(id)) {
            StateSection section{id, 1, {}};
            write_body(section.body, id, court_);
            sections.push_back(std::move(section));
        }
    }
}

const char* World::court_read_section(std::uint16_t id, core::ByteReader& in) {
    court::State& c = court_;
    switch (id) {
        case detail::kSectionCourtMeta: {
            court::Meta meta;
            if (!read_meta(in, meta)) {
                return "invalid court meta section";
            }
            c.meta = meta;
            break;
        }
        case detail::kSectionCourtOffices: {
            std::vector<court::Office> offices;
            std::vector<court::Disgrace> disgraces;
            if (!read_rows(in, offices, court::kMaxOffices, kOfficeRowBytes, &court::Office::id) ||
                !read_rows(
                    in, disgraces, court::kMaxDisgraces, kDisgraceRowBytes, &court::Disgrace::character)) {
                return "invalid court offices section";
            }
            c.offices = std::move(offices);
            c.disgraces = std::move(disgraces);
            break;
        }
        case detail::kSectionCourtVacancies: {
            std::vector<court::Vacancy> vacancies;
            std::vector<court::Decision> decisions;
            if (!read_rows(in, vacancies, court::kMaxVacancies, kVacancyRowBytes, &court::Vacancy::id) ||
                !read_rows(
                    in, decisions, court::kDecisionRingSize, kDecisionRowBytes, &court::Decision::seq)) {
                return "invalid court vacancies section";
            }
            c.vacancies = std::move(vacancies);
            c.decisions = std::move(decisions);
            break;
        }
        case detail::kSectionCourtPetitions: {
            std::vector<court::Petition> rows;
            if (!read_rows(in, rows, court::kMaxPetitions, kPetitionRowBytes, &court::Petition::id)) {
                return "invalid court petitions section";
            }
            c.petitions = std::move(rows);
            break;
        }
        case detail::kSectionCourtServants: {
            std::vector<court::Servant> rows;
            if (!read_rows(in, rows, court::kMaxServants, kServantRowBytes, &court::Servant::id)) {
                return "invalid court servants section";
            }
            store_servants(rows, c.servants);
            break;
        }
        case detail::kSectionCourtFavor: {
            std::vector<court::Favor> rows;
            if (!read_rows(in, rows, dynasties_.size(), kFavorRowBytes, &court::Favor::dynasty)) {
                return "invalid court favor section";
            }
            c.favor = std::move(rows);
            break;
        }
        case detail::kSectionCourtPortfolio: {
            std::vector<court::Portfolio> rows;
            if (!read_rows(in, rows, court::kMaxPortfolios, kPortfolioRowBytes, portfolio_key)) {
                return "invalid court portfolio section";
            }
            c.portfolios = std::move(rows);
            break;
        }
        case detail::kSectionCourtOrders: {
            std::vector<court::PortfolioOrder> rows;
            if (!read_rows(in, rows, court::kOrderRingSize, kOrderRowBytes, &court::PortfolioOrder::seq)) {
                return "invalid court orders section";
            }
            c.orders = std::move(rows);
            break;
        }
        default:
            return "unknown court section";
    }
    return in.ok() && in.at_end() ? nullptr : "a court section is truncated or has extra bytes";
}

std::array<std::uint64_t, 8> World::court_hash_parts() const {
    // Parts 32..39 (court-model.md sections 13.2, 20.13): one section each, 70 and 71 together in part 39;
    // part 38 belongs to E7-T25 (Feud) and stays 0 here. A part without content is 0 (outside `total`).
    struct Group {
        std::uint16_t first;
        std::uint16_t second;  // 0: the group has one section
    };
    constexpr std::array<Group, 8> groups = {{{detail::kSectionCourtMeta, 0},
                                              {detail::kSectionCourtOffices, 0},
                                              {detail::kSectionCourtVacancies, 0},
                                              {detail::kSectionCourtPetitions, 0},
                                              {detail::kSectionCourtServants, 0},
                                              {detail::kSectionCourtFavor, 0},
                                              {0, 0},
                                              {detail::kSectionCourtPortfolio, detail::kSectionCourtOrders}}};
    std::array<std::uint64_t, 8> parts{};
    for (std::size_t i = 0; i < groups.size(); ++i) {
        const Group group = groups[i];
        if (group.first == 0 || !(court_section_present(group.first) ||
                                  (group.second != 0 && court_section_present(group.second)))) {
            continue;
        }
        core::HashStream hash;
        write_body(hash, group.first, court_);
        if (group.second != 0) {
            write_body(hash, group.second, court_);
        }
        parts[i] = detail::nonzero(hash.value());
    }
    return parts;
}

void World::influence_write_section(std::vector<StateSection>& sections) const {
    if (influence_.rows.empty() && influence_.log.empty()) {
        return;
    }
    StateSection section{detail::kSectionInfluence, 1, {}};
    write_influence(section.body, influence_);
    sections.push_back(std::move(section));
}

const char* World::influence_read_section(core::ByteReader& in) {
    InfluenceState state;
    if (!read_rows(in, state.rows, dynasties_.size(), kInfluenceRowBytes, &InfluenceRow::dynasty) ||
        !read_influence_log(in, state.log)) {
        return "invalid influence section";
    }
    influence_ = std::move(state);
    return in.ok() && in.at_end() ? nullptr : "the influence section is truncated or has extra bytes";
}

std::uint64_t World::influence_hash_part() const {
    if (influence_.rows.empty() && influence_.log.empty()) {
        return 0;
    }
    core::HashStream hash;
    write_influence(hash, influence_);
    return detail::nonzero(hash.value());
}

}  // namespace pat::sim
