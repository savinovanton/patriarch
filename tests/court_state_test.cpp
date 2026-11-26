// E10-T02: the court tables of the World (docs/dev/court-model.md sections 4, 12 and 20.4) and the
// influence balances (docs/dev/influence-model.md section 4): storage API, bounds, derived indexes, the
// structural invariants, the rule-dependent check_court_invariants() and a model test of random
// sequences of writes against a simplified model. Saving and hashing are pinned by court_save_test.cpp
// (E10-T03).

#include "pat/core/fixed.hpp"
#include "pat/core/rng.hpp"
#include "pat/sim/court/invariants.hpp"
#include "pat/sim/court/portfolio.hpp"
#include "pat/sim/court/tables.hpp"
#include "pat/sim/court_rules.hpp"
#include "pat/sim/influence.hpp"
#include "pat/sim/world.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "world_fixture.hpp"

using namespace pat::sim;
using namespace pat::sim::court;
using pat::core::Fixed32;
using pat::core::StringId;

namespace {

constexpr std::uint32_t kDynasties = 3;

Date today() {
    return Date::from_ymd(1703, 5, 20);
}

World make_world() {
    World world(today(), 382);
    REQUIRE(world.enable_dynasty_mode());
    for (std::uint32_t i = 0; i < kDynasties; ++i) {
        REQUIRE(world.create_dynasty(StringId(100 + i)) == DynastyId(i));
        REQUIRE(world.create_province(StringId(200 + i), DynastyId(i), Fixed32::from_int(100)) ==
                ProvinceId(i));
    }
    return world;
}

CharacterId person(World& world, std::uint32_t dynasty = 0) {
    CharacterInit init;
    init.dynasty = DynastyId(dynasty);
    init.sex = Sex::Male;
    init.birth_date = Date::from_ymd(1670, 1, 1);
    const CharacterId id = world.create_character(init);
    REQUIRE(id.valid());
    return id;
}

Office office(CharacterId holder, OfficeTypeId type = 1, std::uint32_t locus = 0) {
    Office row;
    row.type = type;
    row.locus = locus;
    row.holder = holder;
    row.since = today();
    return row;
}

Vacancy vacancy(OfficeTypeId type = 4, std::uint32_t locus = 0) {
    Vacancy row;
    row.type = type;
    row.locus = locus;
    row.opened = today();
    row.decide_by = today() + 30;
    return row;
}

Petition petition(VacancyId vacancy_id, CharacterId candidate, std::uint32_t dynasty = 0) {
    Petition row;
    row.vacancy = vacancy_id;
    row.petitioner = DynastyId(dynasty);
    row.candidate = candidate;
    row.submitted = today();
    return row;
}

Servant servant(std::uint32_t dynasty = 0, ServantRole role = ServantRole::Steward) {
    Servant row;
    row.dynasty = DynastyId(dynasty);
    row.role = role;
    row.skill = 10;
    row.loyalty = 60;
    row.wage_kop = 300;
    return row;
}

void require_rule_valid(const World& world, const CourtRules& rules) {
    const auto problems = check_court_invariants(world, rules);
    for (const auto& problem : problems) {
        MESSAGE(problem);
    }
    REQUIRE(problems.empty());
}

}  // namespace

TEST_CASE("court: a world without a court has empty tables and keeps its hash") {
    World world = make_world();
    CHECK(world.court() == State{});
    CHECK(world.influence_state() == InfluenceState{});
    CHECK(world.court_favor_x100(DynastyId(0)) == kDefaultFavorX100);
    CHECK(world.influence_x100(DynastyId(0)) == 0);
    pat::test::require_valid(world);

    const WorldHash before = world.compute_hash();
    const CharacterId a = person(world);
    const WorldHash with_person = world.compute_hash();
    const auto id = world.add_court_office(office(a));
    REQUIRE(id);
    REQUIRE(world.add_court_servant(servant()));
    Favor favor;
    favor.dynasty = DynastyId(1);
    favor.favor_x100 = 7000;
    REQUIRE(world.set_court_favor(favor));
    REQUIRE(world.add_influence(DynastyId(0), 500, InfluenceReason::Start));
    // E10-T03 hashes the tables (parts 32..39 and 40): the total moves away from the world without them.
    const WorldHash with_court = world.compute_hash();
    CHECK_FALSE(with_court.total == with_person.total);
    CHECK(with_court.ext[static_cast<std::size_t>(ExtPart::CourtOffice)] != 0);
    CHECK(with_court.ext[static_cast<std::size_t>(ExtPart::Influence)] != 0);
    CHECK_FALSE(with_person == before);
    pat::test::require_valid(world);
}

TEST_CASE("court: writers refuse outside dynasty mode") {
    World world(today(), 1);
    REQUIRE(world.create_dynasty(StringId(1)) == DynastyId(0));
    CHECK_FALSE(world.add_court_vacancy(vacancy()));
    CHECK_FALSE(world.add_court_servant(servant()));
    Favor favor;
    favor.dynasty = DynastyId(0);
    favor.favor_x100 = 1;
    CHECK_FALSE(world.set_court_favor(favor));
    CHECK_FALSE(world.set_court_market(5, 7));
    CHECK_FALSE(world.add_influence(DynastyId(0), 100, InfluenceReason::Start));
    CHECK(world.court() == State{});
    pat::test::require_valid(world);
}

TEST_CASE("court: offices, their seats and the derived indexes") {
    World world = make_world();
    const CharacterId a = person(world, 0);
    const CharacterId b = person(world, 1);

    const auto first = world.add_court_office(office(a, 1, 0));
    REQUIRE(first == OfficeId{1});
    CHECK(world.find_court_office(*first)->holder == a);
    CHECK(world.court_office_at(1, 0)->id == *first);
    CHECK(world.court_office_at(1, 1) == nullptr);
    CHECK(world.court_offices_of(a) == std::vector<OfficeId>{*first});

    // The seat is unique; an occupied seat cannot open a vacancy.
    CHECK_FALSE(world.add_court_office(office(b, 1, 0)));
    CHECK_FALSE(world.add_court_vacancy(vacancy(1, 0)));

    // Bad rows change nothing.
    CHECK_FALSE(world.add_court_office(office(b, 0, 0)));   // type 0
    CHECK_FALSE(world.add_court_office(office(b, 64, 0)));  // type above 63
    Office flags = office(b, 2, 0);
    flags.flags = 0x80;
    CHECK_FALSE(world.add_court_office(flags));
    Office future = office(b, 2, 0);
    future.since = today() + 1;
    CHECK_FALSE(world.add_court_office(future));
    Office nowhere = office(b, 5, 99);
    nowhere.flags = kOfficeProvinceSeat;
    CHECK_FALSE(world.add_court_office(nowhere));
    CHECK(world.court().offices.size() == 1);

    // A regional seat is indexed by province.
    Office regional = office(b, 5, 2);
    regional.flags = kOfficeProvinceSeat;
    const auto voivode = world.add_court_office(regional);
    REQUIRE(voivode == OfficeId{2});
    CHECK(world.court_offices_in(ProvinceId(2)) == std::vector<OfficeId>{*voivode});
    CHECK(world.court_offices_in(ProvinceId(0)).empty());

    // Several seats of one person up to the structural ceiling.
    for (std::uint32_t locus = 0; locus + 1 < kMaxOfficesPerHolder; ++locus) {
        REQUIRE(world.add_court_office(office(a, 12, locus)));
    }
    CHECK(world.court_offices_of(a).size() == kMaxOfficesPerHolder);
    CHECK_FALSE(world.add_court_office(office(a, 9, 0)));
    pat::test::require_valid(world);

    // Ids are never reused.
    REQUIRE(world.remove_court_office(*first));
    CHECK_FALSE(world.remove_court_office(*first));
    CHECK(world.court_office_at(1, 0) == nullptr);
    const auto again = world.add_court_office(office(b, 1, 0));
    REQUIRE(again);
    CHECK(*again > *voivode);
    CHECK(world.court().meta.next_office_id == *again + 1);
    pat::test::require_valid(world);
}

TEST_CASE("court: an office holder must be a living member of a dynasty") {
    World world = make_world();
    CharacterInit init;
    init.sex = Sex::Male;
    init.birth_date = Date::from_ymd(1670, 1, 1);
    const CharacterId stranger = world.create_character(init);
    REQUIRE(stranger.valid());
    CHECK_FALSE(world.add_court_office(office(stranger)));
    const CharacterId dead = person(world);
    REQUIRE(world.die(dead, CauseOfDeath::OldAge));
    CHECK_FALSE(world.add_court_office(office(dead)));
    CHECK_FALSE(world.add_court_office(office(CharacterId(9999))));
}

TEST_CASE("court: a dead holder or candidate is allowed for one day only (D10)") {
    World world = make_world();
    const CharacterId holder = person(world, 0);
    const CharacterId candidate = person(world, 1);
    REQUIRE(world.add_court_office(office(holder)));
    const auto open = world.add_court_vacancy(vacancy());
    REQUIRE(open);
    REQUIRE(world.add_court_petition(petition(*open, candidate, 1)));

    REQUIRE(world.die(holder, CauseOfDeath::OldAge));
    REQUIRE(world.die(candidate, CauseOfDeath::Illness));
    pat::test::require_valid(world);  // the death changes nothing in the tables
    CHECK(world.court().offices.size() == 1);
    (void)world.advance_day();
    pat::test::require_valid(world);
    (void)world.advance_day();
    const auto problems = world.check_invariants();
    CHECK(std::ranges::any_of(problems, [](const std::string& p) { return p.starts_with("C-2"); }));
    CHECK(std::ranges::any_of(problems, [](const std::string& p) { return p.starts_with("C-5"); }));
}

TEST_CASE("court: disgraces") {
    World world = make_world();
    const CharacterId a = person(world);
    Disgrace row{a, today() + 100, DisgraceReason::Sovereign};
    REQUIRE(world.set_court_disgrace(row));
    CHECK(world.court_disgrace(a)->until == today() + 100);
    row.until = today() + 200;
    REQUIRE(world.set_court_disgrace(row));  // replaces
    CHECK(world.court().disgraces.size() == 1);
    CHECK_FALSE(world.set_court_disgrace({a, today(), DisgraceReason::Sovereign}));  // not after today
    CHECK_FALSE(world.set_court_disgrace({a, today() + 5, static_cast<DisgraceReason>(4)}));  // reason
    CHECK_FALSE(world.set_court_disgrace({CharacterId(4242), today() + 5, DisgraceReason::Event}));
    pat::test::require_valid(world);
    REQUIRE(world.clear_court_disgrace(a));
    CHECK_FALSE(world.clear_court_disgrace(a));
    CHECK(world.court().disgraces.empty());
}

TEST_CASE("court: vacancies, petitions and the ring of decisions") {
    World world = make_world();
    const CharacterId a = person(world, 0);
    const CharacterId a2 = person(world, 0);
    const CharacterId b = person(world, 1);

    const auto open = world.add_court_vacancy(vacancy(4, 0));
    REQUIRE(open == VacancyId{1});
    CHECK(world.court_vacancy_at(4, 0)->id == *open);
    CHECK_FALSE(world.add_court_vacancy(vacancy(4, 0)));   // one vacancy per seat
    CHECK_FALSE(world.add_court_office(office(a, 4, 0)));  // the seat is waiting for a decision
    Vacancy late = vacancy(3, 0);
    late.decide_by = today() - 1;
    CHECK_FALSE(world.add_court_vacancy(late));

    // Postponement moves decide_by; the seat cannot change.
    Vacancy moved = *world.find_court_vacancy(*open);
    moved.decide_by = today() + 60;
    moved.postponed = 1;
    REQUIRE(world.update_court_vacancy(moved));
    CHECK(world.find_court_vacancy(*open)->postponed == 1);
    moved.locus = 1;
    CHECK_FALSE(world.update_court_vacancy(moved));

    const auto p1 = world.add_court_petition(petition(*open, a, 0));
    REQUIRE(p1 == PetitionId{1});
    CHECK_FALSE(world.add_court_petition(petition(*open, a2, 0)));     // second petition of the dynasty
    CHECK_FALSE(world.add_court_petition(petition(*open, a, 1)));      // not a member of dynasty 1
    CHECK_FALSE(world.add_court_petition(petition(*open + 1, b, 1)));  // unknown vacancy
    Petition gift = petition(*open, b, 1);
    gift.gift_kop = -1;
    CHECK_FALSE(world.add_court_petition(gift));
    gift.gift_kop = 5000;
    gift.flags = kPetitionService | kPetitionParty;
    const auto p2 = world.add_court_petition(gift);
    REQUIRE(p2 == PetitionId{2});
    pat::test::require_valid(world);

    // A vacancy with petitions is not removed.
    CHECK_FALSE(world.remove_court_vacancy(*open));
    REQUIRE(world.remove_court_petition(*p1));
    REQUIRE(world.remove_court_petition(*p2));
    REQUIRE(world.remove_court_vacancy(*open));
    CHECK(world.court().vacancies.empty());

    // The ring keeps the last 32 decisions; the terms explain the score.
    Decision decision;
    decision.vacancy = *open;
    decision.type = 4;
    decision.decided = today();
    decision.n = 1;
    decision.contenders[0] = {a, DynastyId(0), 30, {10, 20, 0, 0, 0, 0, 0, 0}, 0};
    Decision wrong = decision;
    wrong.contenders[0].score = 31;
    CHECK_FALSE(world.append_court_decision(wrong));
    Decision hidden = decision;
    hidden.contenders[1].score = 1;
    CHECK_FALSE(world.append_court_decision(hidden));
    for (std::uint32_t i = 0; i < kDecisionRingSize + 5; ++i) {
        REQUIRE(world.append_court_decision(decision) == DecisionSeq{i + 1});
    }
    CHECK(world.court().decisions.size() == kDecisionRingSize);
    CHECK(world.court().decisions.front().seq == 6);
    pat::test::require_valid(world);
}

TEST_CASE("court: servants are columns with a role index") {
    World world = make_world();
    const auto s1 = world.add_court_servant(servant(0, ServantRole::Spy));
    const auto s2 = world.add_court_servant(servant(0, ServantRole::Steward));
    const auto s3 = world.add_court_servant(servant(1, ServantRole::Spy));
    const auto s4 = world.add_court_servant(servant(0, ServantRole::Spy));
    REQUIRE((s1 && s2 && s3 && s4));
    CHECK(world.court_servants_of(DynastyId(0), ServantRole::Spy) == std::vector<ServantId>{*s1, *s4});
    CHECK(world.court_servant_count(DynastyId(0)) == 3);
    CHECK(world.court_servant_count(DynastyId(2)) == 0);

    Servant bad = servant();
    bad.skill = 21;
    CHECK_FALSE(world.add_court_servant(bad));
    bad = servant();
    bad.loyalty = 101;
    CHECK_FALSE(world.add_court_servant(bad));
    bad = servant();
    bad.role = static_cast<ServantRole>(5);
    CHECK_FALSE(world.add_court_servant(bad));
    CHECK_FALSE(world.add_court_servant(servant(7)));

    Servant moved = *world.find_court_servant(*s1);
    moved.role = ServantRole::Guard;
    moved.loyalty = 10;
    REQUIRE(world.update_court_servant(moved));
    CHECK(world.find_court_servant(*s1) == moved);
    CHECK(world.court_servants_of(DynastyId(0), ServantRole::Spy) == std::vector<ServantId>{*s4});
    CHECK(world.court_servants_of(DynastyId(0), ServantRole::Guard) == std::vector<ServantId>{*s1});

    REQUIRE(world.remove_court_servant(*s2));
    CHECK_FALSE(world.find_court_servant(*s2));
    CHECK(world.court().servants.size() == 3);
    CHECK(world.court().servants.columns_consistent());
    CHECK(world.add_court_servant(servant()) == ServantId{5});  // not reused
    pat::test::require_valid(world);
}

TEST_CASE("court: favor rows exist only when not default; the crown has full favor") {
    World world = make_world();
    Favor row;
    row.dynasty = DynastyId(1);
    row.favor_x100 = 6500;
    row.party = Party::Church;
    REQUIRE(world.set_court_favor(row));
    CHECK(world.court_favor_x100(DynastyId(1)) == 6500);
    CHECK(world.court_favor_row(DynastyId(1))->party == Party::Church);

    Favor bad = row;
    bad.favor_x100 = 10001;
    CHECK_FALSE(world.set_court_favor(bad));
    bad = row;
    bad.party = static_cast<Party>(4);
    CHECK_FALSE(world.set_court_favor(bad));
    bad = row;
    bad.gift_month = -2;
    CHECK_FALSE(world.set_court_favor(bad));
    bad = row;
    bad.pressure_band = 4;
    CHECK_FALSE(world.set_court_favor(bad));

    Favor blank;
    blank.dynasty = DynastyId(1);
    REQUIRE(world.set_court_favor(blank));
    CHECK(world.court_favor_row(DynastyId(1)) == nullptr);
    CHECK(world.court_favor_x100(DynastyId(1)) == kDefaultFavorX100);

    REQUIRE(world.enable_economy(DynastyId(2)));
    CHECK(world.court_favor_x100(DynastyId(2)) == kMaxFavorX100);

    REQUIRE(world.set_court_market(7, 0xABCDEF));
    CHECK(world.court().meta.market_month == 7);
    CHECK_FALSE(world.set_court_market(-2, 1));
    pat::test::require_valid(world);
}

TEST_CASE("court: portfolios and the ring of orders") {
    World world = make_world();
    const CharacterId a = person(world);
    Portfolio row;
    row.type = 11;
    row.course = 2;
    row.course_since = today();
    row.controller = DynastyId(0);
    row.set_by = DynastyId(0);
    REQUIRE(world.set_court_portfolio(row));
    CHECK(world.find_court_portfolio(11, 0)->course == 2);

    Portfolio inert = row;
    inert.type = 4;
    inert.state = PortfolioState::Inertia;
    CHECK_FALSE(world.set_court_portfolio(inert));  // a non-active row needs next_step
    inert.next_step = today() + 90;
    REQUIRE(world.set_court_portfolio(inert));
    row.next_step = today() + 1;
    CHECK_FALSE(world.set_court_portfolio(row));  // an active row has none
    row.next_step = kNoDate;
    row.course = kMaxCourses;
    CHECK_FALSE(world.set_court_portfolio(row));
    row.course = 1;
    row.target = DynastyId(77);
    CHECK_FALSE(world.set_court_portfolio(row));
    CHECK(world.court().portfolios.size() == 2);
    CHECK(world.court().portfolios.front().type == 4);  // ordered by (type, locus)

    PortfolioOrder order;
    order.type = 11;
    order.order = 1;
    order.issuer = DynastyId(0);
    order.holder = a;
    order.date = today();
    order.cost_kop = 1000;
    order.execution_permille = 900;
    PortfolioOrder bad = order;
    bad.order = static_cast<std::uint8_t>(kMaxOrdersPerPortfolio);
    CHECK_FALSE(world.append_court_order(bad));
    bad = order;
    bad.execution_permille = 1001;
    CHECK_FALSE(world.append_court_order(bad));
    bad = order;
    bad.cost_kop = -1;
    CHECK_FALSE(world.append_court_order(bad));
    for (std::uint32_t i = 0; i < kOrderRingSize + 3; ++i) {
        REQUIRE(world.append_court_order(order) == i + 1);
    }
    CHECK(world.court().orders.size() == kOrderRingSize);
    CHECK(world.court().orders.front().seq == 4);
    pat::test::require_valid(world);

    REQUIRE(world.remove_court_portfolio(4, 0));
    CHECK_FALSE(world.remove_court_portfolio(4, 0));
}

TEST_CASE("influence: the balance is clamped and every change but the monthly one is logged") {
    World world = make_world();
    CHECK(world.add_influence(DynastyId(0), 3000, InfluenceReason::Start) == 3000);
    CHECK(world.add_influence(DynastyId(0), 500, InfluenceReason::Monthly) == 500);
    CHECK(world.influence_x100(DynastyId(0)) == 3500);
    CHECK(world.add_influence(DynastyId(0), -5000, InfluenceReason::PetitionPush) == -3500);
    CHECK(world.influence_x100(DynastyId(0)) == 0);
    CHECK(world.influence_state().rows.empty());  // a zero balance has no row
    CHECK(world.add_influence(DynastyId(1), 40000, InfluenceReason::Event) == kMaxInfluenceX100);
    CHECK(world.add_influence(DynastyId(1), 1, InfluenceReason::Event) == 0);
    CHECK_FALSE(world.add_influence(DynastyId(9), 1, InfluenceReason::Event));
    CHECK_FALSE(world.add_influence(DynastyId(1), 1, static_cast<InfluenceReason>(77)));

    const auto& log = world.influence_state().log;
    REQUIRE(log.size() == 3);  // Start, PetitionPush, Event; not Monthly, not the zero change
    CHECK(log[1].delta_x100 == -3500);
    CHECK(log[1].reason == InfluenceReason::PetitionPush);
    for (int i = 0; i < 70; ++i) {
        (void)world.add_influence(DynastyId(2), (i % 2 == 0) ? 10 : -10, InfluenceReason::Scenario);
    }
    CHECK(world.influence_state().log.size() == kInfluenceLogSize);
    pat::test::require_valid(world);
}

TEST_CASE("court: the rule-dependent invariants") {
    const CourtRules rules = CourtRules::defaults();
    World world = make_world();
    REQUIRE(world.enable_economy());
    REQUIRE(world.set_province_owner_kind(ProvinceId(1), economy::OwnerKind::Crown));
    const CharacterId a = person(world, 0);
    const CharacterId b = person(world, 1);
    const CharacterId c = person(world, 2);

    Office voivode = office(a, 5, 1);
    voivode.flags = kOfficeProvinceSeat;
    REQUIRE(world.add_court_office(voivode));
    REQUIRE(world.add_court_office(office(b, 1, 1)));
    REQUIRE(world.add_court_servant(servant(0, ServantRole::Spy)));
    pat::test::require_valid(world);
    require_rule_valid(world, rules);

    SUBCASE("a seat number beyond the slots of a fixed type") {
        REQUIRE(world.add_court_office(office(c, 4, 1)));  // court_steward has one seat
        CHECK_FALSE(check_court_invariants(world, rules).empty());
    }
    SUBCASE("a regional seat outside the crown's provinces") {
        Office foreign = office(c, 5, 0);
        foreign.flags = kOfficeProvinceSeat;
        REQUIRE(world.add_court_office(foreign));
        CHECK_FALSE(check_court_invariants(world, rules).empty());
    }
    SUBCASE("a regional seat without the province flag") {
        REQUIRE(world.add_court_office(office(c, 7, 1)));
        CHECK_FALSE(check_court_invariants(world, rules).empty());
    }
    SUBCASE("more seats per person than max_offices_per_person") {
        REQUIRE(world.add_court_office(office(a, 12, 0)));
        CHECK_FALSE(check_court_invariants(world, rules).empty());
    }
    SUBCASE("more servants of a role than its slots") {
        REQUIRE(world.add_court_servant(servant(0, ServantRole::Spy)));
        CHECK_FALSE(check_court_invariants(world, rules).empty());
    }
    SUBCASE("more postponements than max_postponements") {
        Vacancy row = vacancy(4, 0);
        row.postponed = static_cast<std::uint8_t>(rules.balance.max_postponements + 1);
        REQUIRE(world.add_court_vacancy(row));
        CHECK_FALSE(check_court_invariants(world, rules).empty());
    }
    SUBCASE("more services than services_max") {
        Favor row;
        row.dynasty = DynastyId(0);
        row.services = static_cast<std::uint8_t>(rules.balance.services_max + 1);
        REQUIRE(world.set_court_favor(row));
        CHECK_FALSE(check_court_invariants(world, rules).empty());
    }
    SUBCASE("an unknown office type") {
        REQUIRE(world.add_court_office(office(c, 40, 0)));
        CHECK_FALSE(check_court_invariants(world, rules).empty());
    }
    pat::test::require_valid(world);  // all of these are structurally sound
}

// ---- model test ----------------------------------------------------------------------------------------

namespace {

/// A simplified model of the court tables: ordered maps of the rows by id or key.
struct Model {
    std::map<OfficeId, Office> offices;
    std::map<VacancyId, Vacancy> vacancies;
    std::map<PetitionId, Petition> petitions;
    std::map<ServantId, Servant> servants;
    std::map<DynastyId, Favor> favor;
    std::map<DynastyId, std::int32_t> influence;
    OfficeId next_office = 1;
    VacancyId next_vacancy = 1;
    PetitionId next_petition = 1;
    ServantId next_servant = 1;

    [[nodiscard]] bool seat_taken(OfficeTypeId type, std::uint32_t locus) const {
        const auto same = [&](const auto& entry) {
            return entry.second.type == type && entry.second.locus == locus;
        };
        return std::ranges::any_of(offices, same) || std::ranges::any_of(vacancies, same);
    }
};

void require_matches(const World& world, const Model& model) {
    const State& c = world.court();
    REQUIRE(c.offices.size() == model.offices.size());
    std::size_t i = 0;
    for (const auto& [id, row] : model.offices) {
        CHECK(c.offices[i++] == row);
        CHECK(world.court_office_at(row.type, row.locus)->id == id);
    }
    REQUIRE(c.vacancies.size() == model.vacancies.size());
    i = 0;
    for (const auto& [id, row] : model.vacancies) {
        CHECK(c.vacancies[i++] == row);
    }
    REQUIRE(c.petitions.size() == model.petitions.size());
    i = 0;
    for (const auto& [id, row] : model.petitions) {
        CHECK(c.petitions[i++] == row);
    }
    REQUIRE(c.servants.size() == model.servants.size());
    i = 0;
    for (const auto& [id, row] : model.servants) {
        CHECK(c.servants.row(i++) == row);
    }
    for (std::uint32_t d = 0; d < kDynasties; ++d) {
        const auto found = model.favor.find(DynastyId(d));
        CHECK(world.court_favor_x100(DynastyId(d)) ==
              (found == model.favor.end() ? kDefaultFavorX100 : found->second.favor_x100));
        const auto balance = model.influence.find(DynastyId(d));
        CHECK(world.influence_x100(DynastyId(d)) == (balance == model.influence.end() ? 0 : balance->second));
        std::size_t servants = 0;
        for (const auto& [id, row] : model.servants) {
            servants += row.dynasty == DynastyId(d) ? 1 : 0;
        }
        CHECK(world.court_servant_count(DynastyId(d)) == servants);
    }
}

}  // namespace

TEST_CASE("court: random sequences of writes agree with a simplified model") {
    for (std::uint64_t seed = 1; seed <= 6; ++seed) {
        CAPTURE(seed);
        World world = make_world();
        Model model;
        pat::core::Pcg32 rng(seed, 382);
        const auto pick = [&rng](std::uint32_t n) { return rng.next_u32() % n; };
        std::vector<CharacterId> people;
        for (std::uint32_t i = 0; i < 12; ++i) {
            people.push_back(person(world, i % kDynasties));
        }
        const auto dynasty_of = [&](CharacterId id) { return world.characters().dynasty[id.index()]; };

        for (int step = 0; step < 400; ++step) {
            switch (pick(10)) {
                case 0: {  // add an office
                    Office row = office(people[pick(12)], static_cast<OfficeTypeId>(1 + pick(4)), pick(3));
                    const bool ok = world.is_alive(row.holder) && !model.seat_taken(row.type, row.locus) &&
                                    world.court_offices_of(row.holder).size() < kMaxOfficesPerHolder;
                    const auto id = world.add_court_office(row);
                    REQUIRE(id.has_value() == ok);
                    if (ok) {
                        row.id = model.next_office++;
                        CHECK(*id == row.id);
                        model.offices[row.id] = row;
                    }
                    break;
                }
                case 1: {  // remove an office (maybe a missing id)
                    const OfficeId id = 1 + pick(model.next_office);
                    CHECK(world.remove_court_office(id) == (model.offices.erase(id) == 1));
                    break;
                }
                case 2: {  // open a vacancy
                    Vacancy row = vacancy(static_cast<OfficeTypeId>(1 + pick(4)), pick(3));
                    const bool ok = !model.seat_taken(row.type, row.locus);
                    const auto id = world.add_court_vacancy(row);
                    REQUIRE(id.has_value() == ok);
                    if (ok) {
                        row.id = model.next_vacancy++;
                        model.vacancies[row.id] = row;
                    }
                    break;
                }
                case 3: {  // remove a vacancy
                    const VacancyId id = 1 + pick(model.next_vacancy);
                    const bool named = std::ranges::any_of(
                        model.petitions, [id](const auto& entry) { return entry.second.vacancy == id; });
                    const bool ok = model.vacancies.contains(id) && !named;
                    CHECK(world.remove_court_vacancy(id) == ok);
                    if (ok) {
                        model.vacancies.erase(id);
                    }
                    break;
                }
                case 4: {  // submit a petition
                    const CharacterId candidate = people[pick(12)];
                    const DynastyId petitioner =
                        world.is_alive(candidate) ? dynasty_of(candidate) : DynastyId(0);
                    Petition row = petition(1 + pick(model.next_vacancy), candidate, petitioner.value());
                    const bool dup = std::ranges::any_of(model.petitions, [&](const auto& entry) {
                        return entry.second.vacancy == row.vacancy &&
                               (entry.second.petitioner == row.petitioner ||
                                entry.second.candidate == candidate);
                    });
                    const bool ok =
                        model.vacancies.contains(row.vacancy) && world.is_alive(candidate) && !dup;
                    const auto id = world.add_court_petition(row);
                    REQUIRE(id.has_value() == ok);
                    if (ok) {
                        row.id = model.next_petition++;
                        model.petitions[row.id] = row;
                    }
                    break;
                }
                case 5: {  // withdraw a petition
                    const PetitionId id = 1 + pick(model.next_petition);
                    CHECK(world.remove_court_petition(id) == (model.petitions.erase(id) == 1));
                    break;
                }
                case 6: {  // hire, update or fire a servant
                    const std::uint32_t action = pick(3);
                    if (action == 0 || model.servants.empty()) {
                        Servant row = servant(pick(kDynasties), static_cast<ServantRole>(1 + pick(4)));
                        row.skill = static_cast<std::uint8_t>(pick(21));
                        const auto id = world.add_court_servant(row);
                        REQUIRE(id);
                        row.id = model.next_servant++;
                        model.servants[row.id] = row;
                    } else {
                        auto entry = model.servants.begin();
                        std::advance(entry, pick(static_cast<std::uint32_t>(model.servants.size())));
                        if (action == 1) {
                            Servant row = entry->second;
                            row.loyalty = static_cast<std::uint8_t>(pick(101));
                            row.role = static_cast<ServantRole>(1 + pick(4));
                            REQUIRE(world.update_court_servant(row));
                            entry->second = row;
                        } else {
                            REQUIRE(world.remove_court_servant(entry->first));
                            model.servants.erase(entry);
                        }
                    }
                    break;
                }
                case 7: {  // favor
                    Favor row;
                    row.dynasty = DynastyId(pick(kDynasties));
                    row.favor_x100 =
                        pick(3) == 0 ? kDefaultFavorX100 : static_cast<std::int32_t>(pick(10001));
                    REQUIRE(world.set_court_favor(row));
                    if (is_default(row)) {
                        model.favor.erase(row.dynasty);
                    } else {
                        model.favor[row.dynasty] = row;
                    }
                    break;
                }
                case 8: {  // influence
                    const DynastyId dynasty(pick(kDynasties));
                    const auto delta = static_cast<std::int32_t>(pick(20001)) - 10000;
                    const std::int32_t before =
                        model.influence.contains(dynasty) ? model.influence[dynasty] : 0;
                    const std::int32_t after = std::clamp(before + delta, 0, kMaxInfluenceX100);
                    CHECK(world.add_influence(dynasty, delta, InfluenceReason::Scenario) == after - before);
                    if (after == 0) {
                        model.influence.erase(dynasty);
                    } else {
                        model.influence[dynasty] = after;
                    }
                    break;
                }
                default: {  // a death: the tables do not change (D10)
                    const CharacterId victim = people[pick(12)];
                    if (world.is_alive(victim)) {
                        REQUIRE(world.die(victim, CauseOfDeath::Illness));
                    }
                    break;
                }
            }
            require_matches(world, model);
            pat::test::require_valid(world);
        }
        // Indexes rebuilt from scratch equal the incrementally kept ones (C-12 in the invariants).
        World copy = world;
        copy.rebuild_court_indexes();
        pat::test::require_valid(copy);
    }
}
