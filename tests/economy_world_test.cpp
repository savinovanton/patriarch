#include "pat/core/assert.hpp"
#include "pat/core/log.hpp"
#include "pat/core/rng.hpp"
#include "pat/sim/demo_scenario.hpp"
#include "pat/sim/economy/ledger.hpp"
#include "pat/sim/economy/money.hpp"
#include "pat/sim/economy/tables.hpp"
#include "pat/sim/world.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <vector>

// E9-T02: the economy tables of the World, the single money function with its double entry, and the
// invariants (docs/dev/economy-model.md sections 4, 5, 6, 12).

namespace pat::sim::economy {

/// The tests damage the private columns of the account table on purpose to prove that
/// check_invariants() notices (declared a friend in tables.hpp).
struct AccountTableTestAccess {
    static std::vector<Kopecks>& treasury(AccountTable& t) { return t.treasury_; }
    static std::vector<Kopecks>& opening(AccountTable& t) { return t.opening_; }
    static std::vector<Kopecks>& open_lines(AccountTable& t) { return t.open_lines_; }
    static std::vector<Kopecks>& ring_opening(AccountTable& t) { return t.ring_opening_; }
    static std::vector<Kopecks>& ring_lines(AccountTable& t) { return t.ring_lines_; }
    static std::array<Date, kLedgerRingMonths>& ring_month_start(AccountTable& t) {
        return t.ring_month_start_;
    }
};

}  // namespace pat::sim::economy

using namespace pat::sim;
using namespace pat::sim::economy;
using pat::core::Fixed32;
using pat::core::Pcg32;
using pat::core::StringId;

namespace {

using Access = AccountTableTestAccess;

int g_assert_calls = 0;
void counting_handler(const pat::core::AssertInfo&) {
    ++g_assert_calls;
}

/// Counts failed assertions instead of aborting, and keeps the console quiet.
struct AssertCounter {
    AssertCounter() { g_assert_calls = 0; }
    pat::core::ScopedLogCapture capture;
    pat::core::ScopedAssertHandler handler{&counting_handler};
};

World make_plain_world() {
    return World(Date::from_ymd(1703, 1, 1), 42);
}

/// A world with `dynasties` dynasties and `provinces` provinces (province p belongs to dynasty
/// p % dynasties; populations are whole thousands) that has the economy switched on.
World make_economy_world(std::uint32_t dynasties = 3, std::uint32_t provinces = 4) {
    World world = make_plain_world();
    for (std::uint32_t d = 0; d < dynasties; ++d) {
        (void)world.create_dynasty(StringId(d));
    }
    for (std::uint32_t p = 0; p < provinces; ++p) {
        (void)world.create_province(
            StringId(100 + p), DynastyId(p % dynasties), Fixed32::from_int(10 * (p + 1)));
    }
    REQUIRE(world.enable_economy());
    return world;
}

EconomyState& state(World& world) {
    return const_cast<EconomyState&>(world.economy());
}

AccountTable& accounts(World& world) {
    return state(world).accounts;
}

void require_valid(const World& world) {
    const auto problems = world.check_invariants();
    for (const auto& problem : problems) {
        MESSAGE(problem);
    }
    REQUIRE(problems.empty());
}

bool mentions(const std::vector<std::string>& problems, const std::string& fragment) {
    return std::any_of(problems.begin(), problems.end(), [&](const std::string& problem) {
        return problem.find(fragment) != std::string::npos;
    });
}

/// Builds a fresh economy world, damages it and expects check_invariants() to complain with a message
/// that contains `fragment`.
void expect_violation(const std::function<void(World&)>& damage, const std::string& fragment) {
    World world = make_economy_world();
    // One closed month, so that the ring has content to damage.
    (void)world.post_money(AccountRef::of(DynastyId(0)), LedgerCode::TaxPeasants, 5000);
    (void)world.post_money(AccountRef::crown(), LedgerCode::Adjustment, 700);
    REQUIRE(world.close_ledger_month(Date::from_ymd(1703, 2, 1)));
    (void)world.post_money(AccountRef::of(DynastyId(1)), LedgerCode::HoldingIncome, 300);
    require_valid(world);
    damage(world);
    const auto problems = world.check_invariants();
    INFO("expected a violation mentioning: " << fragment);
    for (const auto& problem : problems) {
        MESSAGE("reported: " << problem);
    }
    CHECK_FALSE(problems.empty());
    CHECK(mentions(problems, fragment));
}

constexpr std::size_t line_of(LedgerCode code) {
    return *ledger_index(code);
}

}  // namespace

// ---- creation and access --------------------------------------------------------------------------

TEST_CASE("economy world: a world has no economy by default and stays valid") {
    World world = make_plain_world();
    const DynastyId d = world.create_dynasty(StringId(1));
    (void)world.create_province(StringId(2), d, Fixed32::from_int(10));
    CHECK_FALSE(world.economy_present());
    CHECK(world.economy() == EconomyState{});
    CHECK_FALSE(world.resolve_account(AccountRef::crown()).has_value());
    CHECK(world.treasury(AccountRef::of(d)) == 0);
    CHECK(world.population_persons(ProvinceId(0)) == 0);
    CHECK(world.set_group_size(ProvinceId(0), Group::Peasants, 100) == 0);
    CHECK_FALSE(world.set_province_administrator(ProvinceId(0), CharacterId()));
    CHECK_FALSE(world.set_province_owner_kind(ProvinceId(0), OwnerKind::Crown));
    CHECK_FALSE(world.close_ledger_month(Date::from_ymd(1703, 2, 1)));
    CHECK(world.economy_columns_mut().control.empty());
    require_valid(world);
}

TEST_CASE("economy world: enabling builds a default row for every dynasty and province") {
    World world = make_economy_world(3, 4);
    const EconomyState& e = world.economy();
    CHECK(world.economy_present());
    CHECK(e.meta.present == 1);
    CHECK_FALSE(e.meta.crown_dynasty.valid());
    CHECK(e.meta.open_month_start == Date::from_ymd(1703, 1, 1));
    CHECK(e.meta.closed_months == 0);
    CHECK(e.accounts.size() == 4);  // the crown + 3 dynasties
    CHECK(e.dynasties.size() == 3);
    CHECK(e.provinces.size() == 4);
    CHECK(e.groups.rows() == 4 * kGroupCount);
    CHECK(e.provinces.capacity.size() == 4 * kMaxGoods);
    CHECK(e.provinces.slot_building.size() == 4 * kMaxSlots);

    for (std::size_t p = 0; p < 4; ++p) {
        CHECK(e.provinces.kind[p] == static_cast<std::uint8_t>(ProvinceKind::Rural));
        CHECK(e.provinces.owner_kind[p] == static_cast<std::uint8_t>(OwnerKind::Dynasty));
        CHECK(e.provinces.control[p] == kDefaultControl);
        CHECK(e.provinces.unrest[p] == Fixed32::zero());
        CHECK(e.provinces.corruption[p] == Fixed32::zero());
        CHECK_FALSE(e.provinces.administrator[p].valid());
        CHECK(e.provinces.last_slot_date[p] == kNoDate);
        for (std::size_t g = 0; g < kGroupCount; ++g) {
            const std::size_t row = group_row(p, static_cast<Group>(g));
            CHECK(e.groups.wealth[row] == kDefaultWealth);
            CHECK(e.groups.mood[row] == Fixed32::zero());
            CHECK(e.groups.growth_modifier[row] == 0);
            CHECK(e.groups.growth_carry[row] == 0);
        }
    }
    require_valid(world);
}

TEST_CASE("economy world: the people already counted in the population cache become peasants") {
    World world = make_economy_world(3, 4);
    for (std::uint32_t p = 0; p < 4; ++p) {
        const ProvinceId province(p);
        const std::uint32_t persons = 10'000 * (p + 1);
        CHECK(world.population_persons(province) == persons);
        CHECK(world.economy().groups.size[group_row(p, Group::Peasants)] == persons);
        CHECK(world.economy().groups.size[group_row(p, Group::Nobility)] == 0);
        CHECK(world.provinces().population[p] == Fixed32::from_int(10 * (p + 1)));
    }
}

TEST_CASE("economy world: dynasties and provinces created later get their rows") {
    World world = make_economy_world(2, 2);
    const DynastyId d = world.create_dynasty(StringId(7));
    const ProvinceId owned = world.create_province(StringId(8), d, Fixed32::from_int(52));
    const ProvinceId free = world.create_province(StringId(9), DynastyId(), Fixed32::from_ratio(3, 2));
    const EconomyState& e = world.economy();
    CHECK(e.accounts.size() == 4);
    CHECK(e.dynasties.size() == 3);
    CHECK(e.provinces.size() == 4);
    CHECK(e.provinces.owner_kind[owned.index()] == static_cast<std::uint8_t>(OwnerKind::Dynasty));
    CHECK(e.provinces.owner_kind[free.index()] == static_cast<std::uint8_t>(OwnerKind::Unowned));
    CHECK(world.population_persons(owned) == 52'000);
    CHECK(world.population_persons(free) == 1'500);
    CHECK(world.provinces().population[free.index()] == Fixed32::from_ratio(3, 2));
    require_valid(world);
}

TEST_CASE("economy world: a population that is not whole persons is truncated into the groups") {
    World world = make_plain_world();
    (void)world.create_province(StringId(1), DynastyId(), Fixed32::from_raw(1));  // 1/65536 thousand people
    (void)world.create_province(StringId(2), DynastyId(), Fixed32::from_ratio(2001, 1000));
    REQUIRE(world.enable_economy());
    CHECK(world.population_persons(ProvinceId(0)) == 0);
    // 2.001 thousand is 2000.9 persons in Q16.16: truncated to 2000, and the cache says so too.
    CHECK(world.population_persons(ProvinceId(1)) == 2000);
    CHECK(world.provinces().population[1] == Fixed32::from_int(2));
    require_valid(world);
}

TEST_CASE("economy world: enable_economy is idempotent and checks the crown dynasty") {
    World world = make_plain_world();
    const DynastyId d = world.create_dynasty(StringId(1));
    CHECK_FALSE(world.enable_economy(DynastyId(5)));  // unknown dynasty
    CHECK_FALSE(world.economy_present());
    CHECK(world.enable_economy());
    CHECK(world.enable_economy());                // same argument: fine
    CHECK_FALSE(world.enable_economy(d));         // other mode once it is on
    CHECK(world.economy().accounts.size() == 2);  // not built twice
    require_valid(world);
}

TEST_CASE("economy world: accounts resolve to the crown row or, in the one-treasury mode, to a dynasty") {
    World separate = make_economy_world(3, 1);
    CHECK(separate.resolve_account(AccountRef::crown()).value() == 0U);
    CHECK(separate.resolve_account(AccountRef::of(DynastyId(0))).value() == 1U);
    CHECK(separate.resolve_account(AccountRef::of(DynastyId(2))).value() == 3U);
    CHECK_FALSE(separate.resolve_account(AccountRef::of(DynastyId(3))).has_value());
    CHECK_FALSE(separate.resolve_account(AccountRef::of(DynastyId())).has_value());

    World single = make_plain_world();
    for (std::uint32_t d = 0; d < 3; ++d) {
        (void)single.create_dynasty(StringId(d));
    }
    REQUIRE(single.enable_economy(DynastyId(1)));
    CHECK(single.economy().meta.crown_dynasty == DynastyId(1));
    CHECK(single.resolve_account(AccountRef::crown()).value() == 2U);
    (void)single.post_money(AccountRef::crown(), LedgerCode::Adjustment, 900);
    CHECK(single.treasury(AccountRef::of(DynastyId(1))) == 900);
    CHECK(single.economy().accounts.treasury(0) == 0);  // the crown's own row stays empty
    require_valid(single);
}

TEST_CASE("economy world: group sizes keep the population cache and the cap") {
    World world = make_economy_world(1, 1);
    const ProvinceId p(0);
    CHECK(world.set_group_size(p, Group::Townsfolk, 4'000) == 4'000);
    CHECK(world.set_group_size(p, Group::Nobility, 2'500) == 2'500);
    CHECK(world.population_persons(p) == 10'000 + 4'000 + 2'500);
    CHECK(world.provinces().population[0] == Fixed32::from_ratio(16'500, 1000));
    require_valid(world);

    // The total is capped; the group that is set takes what is left.
    CHECK(world.set_group_size(p, Group::Cossacks, 4'000'000'000U) == kMaxProvincePersons - 16'500);
    CHECK(world.population_persons(p) == kMaxProvincePersons);
    require_valid(world);
    CHECK(world.set_group_size(p, Group::Clergy, 5) == 0);  // nothing left
    CHECK(world.set_group_size(p, Group::Cossacks, 0) == 0);
    CHECK(world.population_persons(p) == 16'500);
    CHECK(world.set_group_size(ProvinceId(9), Group::Clergy, 5) == 0);  // unknown province
    require_valid(world);
}

TEST_CASE("economy world: the owner and the owner kind change together") {
    World world = make_economy_world(2, 2);  // province 0 -> dynasty 0, province 1 -> dynasty 1
    const ProvinceId p(0);
    const auto kind = [&] { return world.economy().provinces.owner_kind[0]; };
    CHECK(kind() == static_cast<std::uint8_t>(OwnerKind::Dynasty));

    CHECK(world.set_province_owner_kind(p, OwnerKind::Crown));
    CHECK(kind() == static_cast<std::uint8_t>(OwnerKind::Crown));
    CHECK_FALSE(world.provinces().owner[0].valid());
    CHECK(world.provinces_of(DynastyId(0)).empty());
    require_valid(world);

    world.set_province_owner(p, DynastyId(1));
    CHECK(kind() == static_cast<std::uint8_t>(OwnerKind::Dynasty));
    CHECK(world.provinces_of(DynastyId(1)).size() == 2);
    world.set_province_owner(p, DynastyId());
    CHECK(kind() == static_cast<std::uint8_t>(OwnerKind::Unowned));
    require_valid(world);

    CHECK(world.set_province_owner_kind(p, OwnerKind::Church));
    world.set_province_owner(p, DynastyId());  // unowned again: a Church province stays Church
    CHECK(kind() == static_cast<std::uint8_t>(OwnerKind::Church));
    CHECK_FALSE(world.set_province_owner_kind(p, OwnerKind::Dynasty));
    CHECK_FALSE(world.set_province_owner_kind(p, static_cast<OwnerKind>(9)));
    CHECK_FALSE(world.set_province_owner_kind(ProvinceId(7), OwnerKind::Crown));
    require_valid(world);
}

TEST_CASE("economy world: the administrator must be alive and is dropped when the person dies") {
    World world = make_economy_world(1, 2);
    CharacterInit init;
    init.dynasty = DynastyId(0);
    init.birth_date = Date::from_ymd(1670, 5, 1);
    const CharacterId a = world.create_character(init);
    const ProvinceId p0(0);
    const ProvinceId p1(1);

    CHECK(world.set_province_administrator(p0, a));
    CHECK(world.set_province_administrator(p1, a));
    CHECK_FALSE(world.set_province_administrator(p0, CharacterId(99)));  // nobody
    CHECK(world.economy().provinces.administrator[0] == a);
    require_valid(world);

    // Without dynasty mode a destroyed character frees its slot: the references go first.
    REQUIRE(world.destroy_character(a));
    CHECK_FALSE(world.economy().provinces.administrator[0].valid());
    CHECK_FALSE(world.economy().provinces.administrator[1].valid());
    require_valid(world);

    // In dynasty mode nobody is destroyed: the dead stay in the archive and administer nothing.
    World mortal = make_economy_world(1, 2);
    const CharacterId c = mortal.create_character(init);
    REQUIRE(mortal.enable_dynasty_mode());
    CHECK(mortal.set_province_administrator(p0, c));
    CHECK(mortal.set_province_administrator(p1, c));
    require_valid(mortal);
    REQUIRE(mortal.die(c, CauseOfDeath::Illness));
    CHECK_FALSE(mortal.economy().provinces.administrator[0].valid());
    CHECK_FALSE(mortal.economy().provinces.administrator[1].valid());
    CHECK_FALSE(mortal.set_province_administrator(p0, c));  // an archived person administers nothing
    CHECK(mortal.set_province_administrator(p0, CharacterId()));
    require_valid(mortal);
}

TEST_CASE("economy world: slots, development and the writable columns") {
    World world = make_economy_world(1, 2);
    const ProvinceId p(1);
    CHECK_FALSE(world.set_slot_building(p, 0, 3));  // the province has no slots yet
    world.economy_columns_mut().slots_total[1] = 3;
    CHECK(world.set_slot_building(p, 2, 7));
    CHECK_FALSE(world.set_slot_building(p, 3, 7));
    CHECK_FALSE(world.set_slot_building(p, kMaxSlots, 7));
    CHECK_FALSE(world.set_slot_building(ProvinceId(5), 0, 7));
    CHECK(world.economy().provinces.slot_building[1 * kMaxSlots + 2] == 7);

    world.set_province_development(p, 0);
    CHECK(world.provinces().development[1] == 1);
    world.set_province_development(p, 99);
    CHECK(world.provinces().development[1] == kMaxDevelopment);
    world.set_province_development(p, 6);
    CHECK(world.provinces().development[1] == 6);

    EconomyColumnsMut c = world.economy_columns_mut();
    CHECK(c.control.size() == 2);
    CHECK(c.mood.size() == 2 * kGroupCount);
    CHECK(c.capacity.size() == 2 * kMaxGoods);
    CHECK(c.manpower_reserve.size() == 1);
    c.control[1] = Fixed32::from_int(40);
    c.mood[group_row(1, Group::Peasants)] = Fixed32::from_int(-30);
    c.capacity[1 * kMaxGoods + 5] = Fixed32::from_int(60);
    c.manpower_reserve[0] = 25;
    c.last_slot_date[0] = world.today();
    CHECK(world.economy().provinces.control[1] == Fixed32::from_int(40));
    CHECK(world.economy().groups.mood[group_row(1, Group::Peasants)] == Fixed32::from_int(-30));
    CHECK(world.economy().dynasties.manpower_reserve[0] == 25);
    require_valid(world);
}

// ---- postings -------------------------------------------------------------------------------------

TEST_CASE("economy world: ledger codes are stable, ascending and mapped to dense lines") {
    CHECK(kLedgerLineCount == kLedgerCodes.size());
    CHECK(*ledger_index(LedgerCode::TaxNobility) == 0);
    CHECK(*ledger_index(LedgerCode::Adjustment) == kLedgerLineCount - 1);
    for (std::size_t i = 0; i < kLedgerLineCount; ++i) {
        CHECK(*ledger_index(ledger_code_at(i)) == i);
        CHECK(is_issued(ledger_code_at(i)));
    }
    CHECK_FALSE(ledger_index(static_cast<LedgerCode>(0)).has_value());
    CHECK_FALSE(ledger_index(static_cast<LedgerCode>(7)).has_value());
    CHECK_FALSE(ledger_index(static_cast<LedgerCode>(301)).has_value());
    CHECK_FALSE(ledger_index(static_cast<LedgerCode>(65535)).has_value());
    // The issued numbers never move (they are in saves).
    CHECK(static_cast<int>(tax_code(Group::Peasants)) == 4);
    CHECK(static_cast<int>(LedgerCode::HoldingIncome) == 10);
    CHECK(static_cast<int>(LedgerCode::Embezzlement) == 100);
    CHECK(static_cast<int>(LedgerCode::InterestCollected) == 205);
    CHECK(static_cast<int>(LedgerCode::Adjustment) == 300);
    CHECK(is_ledger_pair(LedgerCode::LoanIssued, LedgerCode::LoanTaken));
    CHECK_FALSE(is_ledger_pair(LedgerCode::LoanTaken, LedgerCode::LoanIssued));
}

TEST_CASE("economy world: post_money changes the balance and the line by the same amount") {
    World world = make_economy_world();
    const AccountRef d0 = AccountRef::of(DynastyId(0));
    CHECK(world.post_money(d0, LedgerCode::TaxPeasants, 12'345) == 12'345);
    CHECK(world.post_money(d0, LedgerCode::BuildingUpkeep, -2'000) == -2'000);
    CHECK(world.post_money(d0, LedgerCode::TaxPeasants, 55) == 55);
    CHECK(world.post_money(d0, LedgerCode::Construction, 0) == 0);  // no-op

    const AccountTable& t = world.economy().accounts;
    CHECK(world.treasury(d0) == 10'400);
    CHECK(t.treasury(1) == 10'400);
    CHECK(t.opening(1) == 0);
    CHECK(t.open_line(1, LedgerCode::TaxPeasants) == 12'400);
    CHECK(t.open_line(1, LedgerCode::BuildingUpkeep) == -2'000);
    CHECK(t.open_line(1, LedgerCode::Construction) == 0);
    CHECK(AccountTable::sum(t.open_lines(1)) == 10'400);
    // Other accounts are untouched.
    CHECK(t.treasury(0) == 0);
    CHECK(t.treasury(2) == 0);
    require_valid(world);

    // The treasury may go negative.
    CHECK(world.post_money(d0, LedgerCode::CourtUpkeep, -50'000) == -50'000);
    CHECK(world.treasury(d0) == -39'600);
    require_valid(world);
}

TEST_CASE("economy world: bad postings change nothing") {
    AssertCounter guard;
    World world = make_economy_world();
    const EconomyState before = world.economy();
    CHECK(world.post_money(AccountRef::of(DynastyId(9)), LedgerCode::Adjustment, 100) == 0);
    CHECK(world.post_money(AccountRef::of(DynastyId()), LedgerCode::Adjustment, 100) == 0);
    CHECK(world.post_money(AccountRef::crown(), static_cast<LedgerCode>(7), 100) == 0);
    CHECK(world.post_money(AccountRef::crown(), static_cast<LedgerCode>(0), 100) == 0);
    // The codes of a transfer pair are posted by transfer() only: alone they would unbalance the pair.
    CHECK(world.post_money(AccountRef::crown(), LedgerCode::LoanIssued, 100) == 0);
    CHECK(world.post_money(AccountRef::of(DynastyId(0)), LedgerCode::InterestCollected, -100) == 0);
#if PAT_ASSERTS_ENABLED
    CHECK(g_assert_calls == 6);
#endif
    CHECK(world.economy() == before);

    World plain = make_plain_world();
    (void)plain.create_dynasty(StringId(1));
    CHECK(plain.post_money(AccountRef::of(DynastyId(0)), LedgerCode::Adjustment, 100) == 0);
    CHECK(plain.economy() == EconomyState{});
    CHECK_FALSE(plain.log_incident(IncidentKind::Riot, ProvinceId(), DynastyId(), 0).has_value());
    require_valid(plain);
}

TEST_CASE("economy world: a single posting is capped at kMaxPostingKop") {
    World world = make_economy_world();
    const AccountRef d0 = AccountRef::of(DynastyId(0));
    CHECK(world.post_money(d0, LedgerCode::Adjustment, kMaxPostingKop * 5) == kMaxPostingKop);
    CHECK(world.post_money(d0, LedgerCode::Adjustment, std::numeric_limits<Kopecks>::max()) ==
          kMaxPostingKop);
    CHECK(world.post_money(d0, LedgerCode::Adjustment, std::numeric_limits<Kopecks>::min()) ==
          -kMaxPostingKop);
    CHECK(world.treasury(d0) == kMaxPostingKop);
    require_valid(world);
}

TEST_CASE("economy world: the balance saturates at the treasury bound and the ledger follows") {
    World world = make_economy_world();
    const AccountRef d0 = AccountRef::of(DynastyId(0));
    // 100 postings of 1e13 reach exactly 1e15.
    for (int i = 0; i < 100; ++i) {
        CHECK(world.post_money(d0, LedgerCode::Adjustment, kMaxPostingKop) == kMaxPostingKop);
    }
    CHECK(world.treasury(d0) == kMaxTreasuryKop);
    CHECK(world.post_money(d0, LedgerCode::Adjustment, 10) == 0);  // nothing fits
    CHECK(world.post_money(d0, LedgerCode::HoldingIncome, kMaxPostingKop) == 0);
    require_valid(world);

    // Half of a posting fits: 5e12 of 1e13.
    CHECK(world.post_money(d0, LedgerCode::Construction, -kMaxPostingKop / 2) == -kMaxPostingKop / 2);
    CHECK(world.post_money(d0, LedgerCode::HoldingIncome, kMaxPostingKop) == kMaxPostingKop / 2);
    CHECK(world.treasury(d0) == kMaxTreasuryKop);
    CHECK(world.economy().accounts.open_line(1, LedgerCode::HoldingIncome) == kMaxPostingKop / 2);
    require_valid(world);

    // The same on the negative side.
    const AccountRef d1 = AccountRef::of(DynastyId(1));
    for (int i = 0; i < 100; ++i) {
        (void)world.post_money(d1, LedgerCode::CourtUpkeep, -kMaxPostingKop);
    }
    CHECK(world.treasury(d1) == -kMaxTreasuryKop);
    CHECK(world.post_money(d1, LedgerCode::CourtUpkeep, -1) == 0);
    CHECK(world.post_money(d1, LedgerCode::TaxPeasants, 3) == 3);
    require_valid(world);
}

TEST_CASE("economy world: a line saturates at its own bound while the balance is far from its limit") {
    World world = make_economy_world();
    const AccountRef d0 = AccountRef::of(DynastyId(0));
    // Fill the Adjustment line to +2e15 by going up to the balance cap twice and spending in between.
    for (int round = 0; round < 2; ++round) {
        for (int i = 0; i < 100; ++i) {
            CHECK(world.post_money(d0, LedgerCode::Adjustment, kMaxPostingKop) == kMaxPostingKop);
        }
        for (int i = 0; i < 100; ++i) {
            CHECK(world.post_money(d0, LedgerCode::Construction, -kMaxPostingKop) == -kMaxPostingKop);
        }
    }
    CHECK(world.treasury(d0) == 0);
    CHECK(world.economy().accounts.open_line(1, LedgerCode::Adjustment) == kMaxLedgerLineKop);
    CHECK(world.post_money(d0, LedgerCode::Adjustment, 1'000) == 0);  // the line is full, the balance is not
    CHECK(world.treasury(d0) == 0);
    CHECK(world.post_money(d0, LedgerCode::Adjustment, -1'000) == -1'000);  // the other way is open
    require_valid(world);
}

TEST_CASE("economy world: rounding never loses or creates a kopeck when taxes are split") {
    World world = make_economy_world(2, 2);
    const AccountRef owner = AccountRef::of(DynastyId(0));
    const AccountRef crown = AccountRef::crown();
    Kopecks gross_total = 0;
    Kopecks leak_total = 0;
    for (Kopecks gross = 1; gross <= 3'000; gross += 7) {
        for (std::int32_t leak_permille = 0; leak_permille <= 1000; leak_permille += 37) {
            // The townsfolk tax of a city: the crown's share goes to the crown, the rest to the owner
            // (split by subtraction), and the owner's share loses its leak (F6: the tax line in full,
            // the leak as a negative line next to it).
            const Split city = split_permille(gross, 300);
            const Split leak = split_permille(city.rest, leak_permille);
            CHECK(city.part + city.rest == gross);
            CHECK(leak.part + leak.rest == city.rest);
            (void)world.post_money(owner, LedgerCode::TaxTownsfolk, city.rest);
            (void)world.post_money(owner, LedgerCode::Embezzlement, -leak.part);
            (void)world.post_money(crown, LedgerCode::TaxTownsfolk, city.part);
            gross_total += gross;
            leak_total += leak.part;
        }
    }
    const AccountTable& t = world.economy().accounts;
    // Not a kopeck is lost or created: what was taxed, less what leaked, sits in the two accounts.
    CHECK(world.treasury(owner) + world.treasury(crown) == gross_total - leak_total);
    CHECK(t.open_line(0, LedgerCode::TaxTownsfolk) + t.open_line(1, LedgerCode::TaxTownsfolk) == gross_total);
    CHECK(t.open_line(1, LedgerCode::Embezzlement) == -leak_total);
    require_valid(world);
}

TEST_CASE("economy world: a sum split by weights over several accounts is conserved") {
    World world = make_economy_world(4, 1);
    const std::array<std::uint32_t, 4> weights = {5, 0, 3, 11};
    Kopecks total = 0;
    for (Kopecks amount = 0; amount < 400; amount += 3) {
        std::array<Kopecks, 4> parts{};
        REQUIRE(split_by_weights(amount, weights, parts));
        for (std::size_t i = 0; i < parts.size(); ++i) {
            (void)world.post_money(
                AccountRef::of(DynastyId(static_cast<std::uint32_t>(i))), LedgerCode::GrantsIn, parts[i]);
        }
        total += amount;
    }
    Kopecks held = 0;
    for (std::uint32_t d = 0; d < 4; ++d) {
        held += world.treasury(AccountRef::of(DynastyId(d)));
    }
    CHECK(held == total);
    CHECK(world.treasury(AccountRef::of(DynastyId(1))) == 0);  // weight 0 gets nothing
    require_valid(world);
}

// ---- transfers ------------------------------------------------------------------------------------

TEST_CASE("economy world: a transfer is a double entry that cancels out over the accounts") {
    World world = make_economy_world();
    const AccountRef crown = AccountRef::crown();
    const AccountRef d0 = AccountRef::of(DynastyId(0));
    // The crown lends 5000 to the dynasty, the dynasty pays 1200 back.
    CHECK(world.transfer(crown, d0, LedgerCode::LoanIssued, LedgerCode::LoanTaken, 5'000) == 5'000);
    CHECK(world.transfer(d0, crown, LedgerCode::LoanRepaid, LedgerCode::LoanCollected, 1'200) == 1'200);
    const AccountTable& t = world.economy().accounts;
    CHECK(world.treasury(crown) == -5'000 + 1'200);
    CHECK(world.treasury(d0) == 5'000 - 1'200);
    CHECK(t.open_line(0, LedgerCode::LoanIssued) == -5'000);
    CHECK(t.open_line(1, LedgerCode::LoanTaken) == 5'000);
    CHECK(t.open_line(1, LedgerCode::LoanRepaid) == -1'200);
    CHECK(t.open_line(0, LedgerCode::LoanCollected) == 1'200);
    CHECK(world.treasury(crown) + world.treasury(d0) == 0);
    require_valid(world);  // includes E-M3
}

TEST_CASE("economy world: a transfer moves the smaller of what both sides can take") {
    World world = make_economy_world();
    const AccountRef crown = AccountRef::crown();
    const AccountRef d0 = AccountRef::of(DynastyId(0));
    for (int i = 0; i < 100; ++i) {
        (void)world.post_money(crown, LedgerCode::Adjustment, kMaxPostingKop);
    }
    (void)world.post_money(crown, LedgerCode::Construction, -5);  // room for 5 kopecks
    (void)world.post_money(d0, LedgerCode::Adjustment, 100);
    // d0 -> crown, 50 requested, the crown can take 5.
    CHECK(world.transfer(d0, crown, LedgerCode::QuotaMoney, LedgerCode::QuotaCollected, 50) == 5);
    CHECK(world.treasury(d0) == 95);
    CHECK(world.treasury(crown) == kMaxTreasuryKop);
    // Now there is no room at all.
    CHECK(world.transfer(d0, crown, LedgerCode::QuotaMoney, LedgerCode::QuotaCollected, 50) == 0);
    CHECK(world.treasury(d0) == 95);
    require_valid(world);

    // The sender may be negative: the debt grows, as the spec allows (commands check the funds).
    World debt = make_economy_world();
    CHECK(debt.transfer(AccountRef::of(DynastyId(1)),
                        AccountRef::crown(),
                        LedgerCode::Interest,
                        LedgerCode::InterestCollected,
                        700) == 700);
    CHECK(debt.treasury(AccountRef::of(DynastyId(1))) == -700);
    require_valid(debt);
}

TEST_CASE("economy world: transfers reject wrong pairs, bad amounts and unknown accounts") {
    AssertCounter guard;
    World world = make_economy_world();
    const EconomyState before = world.economy();
    const AccountRef crown = AccountRef::crown();
    const AccountRef d0 = AccountRef::of(DynastyId(0));
    CHECK(world.transfer(crown, d0, LedgerCode::LoanTaken, LedgerCode::LoanIssued, 10) == 0);  // reversed
    CHECK(world.transfer(crown, d0, LedgerCode::TaxPeasants, LedgerCode::Adjustment, 10) == 0);
    CHECK(world.transfer(crown, d0, LedgerCode::LoanIssued, LedgerCode::LoanCollected, 10) == 0);  // mixed
#if PAT_ASSERTS_ENABLED
    CHECK(g_assert_calls == 3);
#endif
    CHECK(world.transfer(crown, d0, LedgerCode::LoanIssued, LedgerCode::LoanTaken, 0) == 0);
    CHECK(world.transfer(crown, d0, LedgerCode::LoanIssued, LedgerCode::LoanTaken, -10) == 0);
    CHECK(world.transfer(
              crown, AccountRef::of(DynastyId(8)), LedgerCode::LoanIssued, LedgerCode::LoanTaken, 10) == 0);
    CHECK(world.economy() == before);
}

TEST_CASE(
    "economy world: in the one-treasury mode a transfer between the crown and its holder cancels itself") {
    World world = make_plain_world();
    for (std::uint32_t d = 0; d < 2; ++d) {
        (void)world.create_dynasty(StringId(d));
    }
    REQUIRE(world.enable_economy(DynastyId(0)));
    const AccountRef holder = AccountRef::of(DynastyId(0));
    (void)world.post_money(holder, LedgerCode::Adjustment, 1'000);
    CHECK(world.transfer(
              holder, AccountRef::crown(), LedgerCode::QuotaMoney, LedgerCode::QuotaCollected, 400) == 400);
    CHECK(world.treasury(holder) == 1'000);  // out of one pocket into the same pocket
    CHECK(world.economy().accounts.open_line(1, LedgerCode::QuotaMoney) == -400);
    CHECK(world.economy().accounts.open_line(1, LedgerCode::QuotaCollected) == 400);
    // A loan from the crown to a third dynasty comes out of the holder's money.
    CHECK(world.transfer(AccountRef::crown(),
                         AccountRef::of(DynastyId(1)),
                         LedgerCode::LoanIssued,
                         LedgerCode::LoanTaken,
                         600) == 600);
    CHECK(world.treasury(holder) == 400);
    CHECK(world.treasury(AccountRef::of(DynastyId(1))) == 600);
    require_valid(world);
}

// ---- closing a month ------------------------------------------------------------------------------

TEST_CASE("economy world: closing a month moves the open lines into the ring") {
    World world = make_economy_world();
    const AccountRef d0 = AccountRef::of(DynastyId(0));
    const AccountRef crown = AccountRef::crown();
    (void)world.post_money(d0, LedgerCode::TaxPeasants, 1'000);
    (void)world.post_money(d0, LedgerCode::BuildingUpkeep, -300);
    (void)world.transfer(crown, d0, LedgerCode::LoanIssued, LedgerCode::LoanTaken, 50);

    REQUIRE(world.close_ledger_month(Date::from_ymd(1703, 2, 1)));
    const EconomyState& e = world.economy();
    const AccountTable& t = e.accounts;
    CHECK(e.meta.open_month_start == Date::from_ymd(1703, 2, 1));
    CHECK(e.meta.closed_months == 1);
    CHECK(t.ring_month_start(0) == Date::from_ymd(1703, 1, 1));
    CHECK(t.treasury(1) == 750);
    CHECK(t.opening(1) == 750);
    CHECK(AccountTable::sum(t.open_lines(1)) == 0);
    CHECK(t.ring_opening(1, 0) == 0);
    CHECK(t.ring_lines(1, 0)[line_of(LedgerCode::TaxPeasants)] == 1'000);
    CHECK(t.ring_lines(1, 0)[line_of(LedgerCode::BuildingUpkeep)] == -300);
    CHECK(t.ring_lines(1, 0)[line_of(LedgerCode::LoanTaken)] == 50);
    CHECK(t.ring_lines(0, 0)[line_of(LedgerCode::LoanIssued)] == -50);
    require_valid(world);

    // Month two: the opening balance of the closed month three starts from 750.
    (void)world.post_money(d0, LedgerCode::HoldingIncome, 25);
    REQUIRE(world.close_ledger_month(Date::from_ymd(1703, 3, 1)));
    CHECK(t.ring_opening(1, 0) == 750);
    CHECK(t.ring_opening(1, 1) == 0);
    CHECK(t.ring_month_start(0) == Date::from_ymd(1703, 2, 1));
    CHECK(t.ring_month_start(1) == Date::from_ymd(1703, 1, 1));
    CHECK(t.ring_lines(1, 1)[line_of(LedgerCode::TaxPeasants)] == 1'000);
    CHECK(t.ring_lines(1, 0)[line_of(LedgerCode::HoldingIncome)] == 25);
    require_valid(world);
}

TEST_CASE("economy world: the ring keeps twelve closed months and forgets the oldest") {
    World world = make_economy_world(2, 1);
    const AccountRef d0 = AccountRef::of(DynastyId(0));
    Date month = Date::from_ymd(1703, 1, 1);
    for (int i = 1; i <= 15; ++i) {
        (void)world.post_money(d0, LedgerCode::TaxPeasants, 100 * i);  // month i earns 100 * i
        month = month.first_of_next_month();
        REQUIRE(world.close_ledger_month(month));
        require_valid(world);
    }
    const AccountTable& t = world.economy().accounts;
    CHECK(world.economy().meta.closed_months == kLedgerRingMonths);
    // Month 15 is the newest, month 4 the oldest of the twelve.
    for (std::size_t m = 0; m < kLedgerRingMonths; ++m) {
        const auto expected = static_cast<Kopecks>(100 * (15 - m));
        CHECK(t.ring_lines(1, m)[line_of(LedgerCode::TaxPeasants)] == expected);
        CHECK(t.ring_opening(1, m) == static_cast<Kopecks>(100 * ((15 - m) * (15 - m - 1) / 2)));
    }
    // Closed month j (1 = January 1703) starts on the first day of the (j-1)-th month after January 1703.
    for (int m = 0; m < static_cast<int>(kLedgerRingMonths); ++m) {
        const int k = 14 - m;
        CHECK(t.ring_month_start(static_cast<std::size_t>(m)) ==
              Date::from_ymd(1703 + k / 12, 1 + k % 12, 1));
    }
    CHECK(world.treasury(d0) == 100 * (15 * 16 / 2));
}

TEST_CASE("economy world: closing a month needs a later first day of a month") {
    World world = make_economy_world();
    const EconomyState before = world.economy();
    CHECK_FALSE(world.close_ledger_month(Date::from_ymd(1703, 1, 1)));   // not later
    CHECK_FALSE(world.close_ledger_month(Date::from_ymd(1702, 12, 1)));  // earlier
    CHECK_FALSE(world.close_ledger_month(Date::from_ymd(1703, 2, 2)));   // not the first day
    CHECK_FALSE(world.close_ledger_month(kNoDate));
    CHECK(world.economy() == before);
    CHECK(world.close_ledger_month(Date::from_ymd(1703, 4, 1)));  // skipping months is allowed
}

// ---- incidents ------------------------------------------------------------------------------------

TEST_CASE("economy world: the incident log numbers its entries and forgets the oldest") {
    World world = make_economy_world();
    const auto first = world.log_incident(IncidentKind::Riot, ProvinceId(2), DynastyId(1), -5);
    REQUIRE(first.has_value());
    CHECK(*first == 0);
    // Unknown references are dropped, the rest is kept.
    const auto second = world.log_incident(IncidentKind::Bankruptcy, ProvinceId(99), DynastyId(), 7);
    REQUIRE(second.has_value());
    CHECK(*second == 1);
    const IncidentLog& log = world.economy().incidents;
    CHECK(log.size() == 2);
    CHECK(log.province[0] == ProvinceId(2));
    CHECK(log.dynasty[0] == DynastyId(1));
    CHECK(log.param[0] == -5);
    CHECK(log.kind[1] == static_cast<std::uint8_t>(IncidentKind::Bankruptcy));
    CHECK_FALSE(log.province[1].valid());
    CHECK(log.date[1] == Date::from_ymd(1703, 1, 1));

    CHECK_FALSE(world.log_incident(static_cast<IncidentKind>(0), ProvinceId(), DynastyId(), 0).has_value());
    CHECK_FALSE(world.log_incident(static_cast<IncidentKind>(16), ProvinceId(), DynastyId(), 0).has_value());
    CHECK(world.economy().incidents.size() == 2);

    for (int i = 0; i < 300; ++i) {
        (void)world.log_incident(IncidentKind::UnrestStateChanged, ProvinceId(1), DynastyId(), i);
    }
    CHECK(log.size() == kIncidentLogSize);
    CHECK(log.seq.back() == 301);
    CHECK(log.seq.front() == 302 - kIncidentLogSize);
    CHECK(log.param.back() == 299);
    CHECK(world.economy().meta.next_incident_seq == 302);
    require_valid(world);
}

// ---- one test per invariant -----------------------------------------------------------------------

TEST_CASE("economy invariants E-S1: lengths and the absence of economy") {
    {
        World world = make_plain_world();
        (void)world.create_dynasty(StringId(1));
        state(world).meta.closed_months = 1;
        CHECK(mentions(world.check_invariants(), "has no economy but holds economy data"));
    }
    {
        World world = make_plain_world();
        state(world).dynasties.bankrupt.push_back(0);
        CHECK(mentions(world.check_invariants(), "has no economy but holds economy data"));
    }
    expect_violation([](World& w) { state(w).provinces.control.pop_back(); },
                     "province control has 3 rows, expected 4");
    expect_violation([](World& w) { state(w).groups.mood.push_back(Fixed32::zero()); },
                     "group mood has 25 rows");
    expect_violation([](World& w) { state(w).dynasties.manpower_reserve.pop_back(); },
                     "dynasty manpower_reserve has 2 rows");
    expect_violation([](World& w) { Access::treasury(accounts(w)).pop_back(); },
                     "account treasury has 3 rows");
    expect_violation([](World& w) { Access::open_lines(accounts(w)).pop_back(); }, "account open lines");
    expect_violation([](World& w) { Access::ring_lines(accounts(w)).push_back(0); }, "account ring lines");
    expect_violation([](World& w) { state(w).provinces.capacity.pop_back(); }, "province capacity");
    expect_violation([](World& w) { state(w).provinces.slot_building.pop_back(); }, "province slot_building");
    expect_violation([](World& w) { state(w).incidents.kind.push_back(1); }, "incident kind");
    expect_violation([](World& w) { state(w).loans.principal.push_back(1); }, "loan principal");
    // The creation functions keep the lengths in step with the entities.
    World world = make_economy_world();
    (void)world.create_dynasty(StringId(50));
    (void)world.create_province(StringId(51), DynastyId(), Fixed32::zero());
    require_valid(world);
}

TEST_CASE("economy invariants E-S2: enumerations, ownership and the scalars") {
    expect_violation([](World& w) { state(w).provinces.kind[1] = 8; }, "unknown kind");
    expect_violation([](World& w) { state(w).provinces.owner_kind[0] = 5; }, "unknown owner kind");
    expect_violation([](World& w) { state(w).provinces.owner_kind[0] = 2; },
                     "must go with a dynasty owner");  // owned, not Dynasty
    expect_violation(
        [](World& w) {
            state(w).provinces.owner_kind[0] = 1;
            w.set_province_owner(ProvinceId(0), DynastyId());
            state(w).provinces.owner_kind[0] = 1;
        },
        "must go with a dynasty owner");  // Dynasty without an owner
    expect_violation([](World& w) { state(w).provinces.slots_total[0] = 7; }, "slots, at most");
    expect_violation([](World& w) { state(w).dynasties.bankrupt[2] = 2; }, "bankrupt flag");
    expect_violation([](World& w) { state(w).meta.present = 2; }, "present flag");
    expect_violation([](World& w) { state(w).meta.closed_months = 13; }, "closed months");
    expect_violation([](World& w) { state(w).meta.crown_dynasty = DynastyId(40); },
                     "crown dynasty does not exist");
    expect_violation([](World& w) { state(w).meta.open_month_start = Date::from_ymd(1703, 2, 3); },
                     "first day of a month");
    expect_violation([](World& w) { state(w).provinces.income_last_month[0] = kMaxTreasuryKop + 1; },
                     "last income");
    expect_violation([](World& w) { Access::ring_month_start(accounts(w))[5] = Date::from_ymd(1700, 1, 2); },
                     "unused ring month 5 has a date");
    // Development leaves 1..10 (checked once the economy is on).
    expect_violation([](World& w) { const_cast<ProvinceTable&>(w.provinces()).development[1] = 11; },
                     "development 11");
    expect_violation([](World& w) { const_cast<ProvinceTable&>(w.provinces()).development[1] = 0; },
                     "development 0");
}

TEST_CASE("economy invariants E-M1: the balance equals the opening balance plus the open lines") {
    expect_violation([](World& w) { Access::treasury(accounts(w))[1] += 1; }, "account 1 treasury");
    expect_violation([](World& w) { Access::opening(accounts(w))[2] += 100; }, "account 2 treasury");
    expect_violation([](World& w) { Access::open_lines(accounts(w))[2 * kLedgerLineCount + 4] -= 1; },
                     "account 2 treasury");
    // Going around post_money, even by a posting that looks right on one side only, is caught.
    expect_violation([](World& w) { Access::treasury(accounts(w))[0] += 500; }, "account 0 treasury");
}

TEST_CASE("economy invariants E-M2: closed months chain from the opening balance to the next month") {
    expect_violation([](World& w) { Access::ring_opening(accounts(w))[1 * kLedgerRingMonths + 0] += 5; },
                     "closed month 0 ends at");
    expect_violation(
        [](World& w) {
            Access::ring_lines(accounts(w))[(1 * kLedgerRingMonths + 0) * kLedgerLineCount + 3] -= 5;
        },
        "closed month 0 ends at");
    // Data in a ring month that was never closed.
    expect_violation([](World& w) { Access::ring_opening(accounts(w))[1 * kLedgerRingMonths + 3] = 9; },
                     "unused ring month 3");
    expect_violation(
        [](World& w) { Access::ring_lines(accounts(w))[(1 * kLedgerRingMonths + 4) * kLedgerLineCount] = 9; },
        "unused ring month 4");
    // The dates of the closed months must run backwards from the open month.
    expect_violation([](World& w) { Access::ring_month_start(accounts(w))[0] = Date::from_ymd(1703, 2, 1); },
                     "closed month 0 is dated out of order");
    // A chain of several months: break the middle one.
    World world = make_economy_world();
    (void)world.post_money(AccountRef::of(DynastyId(0)), LedgerCode::TaxPeasants, 10);
    REQUIRE(world.close_ledger_month(Date::from_ymd(1703, 2, 1)));
    (void)world.post_money(AccountRef::of(DynastyId(0)), LedgerCode::TaxPeasants, 20);
    REQUIRE(world.close_ledger_month(Date::from_ymd(1703, 3, 1)));
    require_valid(world);
    Access::ring_opening(accounts(world))[1 * kLedgerRingMonths + 1] = 3;
    CHECK(mentions(world.check_invariants(), "closed month 1 ends at"));
}

TEST_CASE("economy invariants E-M3: transfer pairs cancel out in the open month and in every closed month") {
    const auto unbalanced_open = [](World& w) {
        // +7 on one side only, kept consistent with E-M1 so that only E-M3 fails.
        Access::open_lines(accounts(w))[1 * kLedgerLineCount + line_of(LedgerCode::LoanTaken)] += 7;
        Access::treasury(accounts(w))[1] += 7;
    };
    expect_violation(unbalanced_open, "does not cancel out in the open month");
    expect_violation(
        [](World& w) {
            // The closed month 0: change the line and keep the chain by moving the opening balance.
            Access::ring_lines(accounts(
                w))[(1 * kLedgerRingMonths + 0) * kLedgerLineCount + line_of(LedgerCode::QuotaMoney)] -= 4;
            Access::ring_opening(accounts(w))[1 * kLedgerRingMonths + 0] += 4;
            // opening of the chain's start is a free parameter, only the sums and the chain matter
        },
        "does not cancel out in closed month 0");
    // A pair line on the sender only: the other side is missing.
    expect_violation(
        [](World& w) {
            Access::open_lines(accounts(w))[3 * kLedgerLineCount + line_of(LedgerCode::InterestCollected)] +=
                1;
            Access::treasury(accounts(w))[3] += 1;
        },
        "transfer pair 109/205");
}

TEST_CASE("economy invariants E-M4: balances and lines stay within their bounds") {
    expect_violation([](World& w) { Access::treasury(accounts(w))[1] = kMaxTreasuryKop + 1; },
                     "beyond the treasury bound");
    expect_violation([](World& w) { Access::treasury(accounts(w))[1] = -kMaxTreasuryKop - 1; },
                     "beyond the treasury bound");
    expect_violation([](World& w) { Access::opening(accounts(w))[1] = kMaxTreasuryKop + 5; },
                     "opening balance");
    expect_violation(
        [](World& w) { Access::open_lines(accounts(w))[kLedgerLineCount + 2] = kMaxLedgerLineKop + 1; },
        "open line");
    expect_violation([](World& w) { Access::ring_lines(accounts(w))[5] = -kMaxLedgerLineKop - 1; },
                     "closed line");
    expect_violation([](World& w) { Access::ring_opening(accounts(w))[2] = kMaxTreasuryKop + 1; },
                     "ring opening balance");
}

TEST_CASE("economy invariants E-P1: the groups add up to the population") {
    expect_violation([](World& w) { state(w).groups.size[group_row(2, Group::Clergy)] += 1; },
                     "population cache");
    expect_violation(
        [](World& w) { const_cast<ProvinceTable&>(w.provinces()).population[1] = Fixed32::from_int(1); },
        "population cache");
    expect_violation(
        [](World& w) { state(w).groups.size[group_row(0, Group::Natives)] = kMaxProvincePersons; },
        "over the cap");
    // set_group_size keeps the cache in step.
    World world = make_economy_world();
    (void)world.set_group_size(ProvinceId(2), Group::Cossacks, 777);
    require_valid(world);
}

TEST_CASE("economy invariants E-P2: the ranges of the province and group columns") {
    expect_violation([](World& w) { state(w).groups.mood[3] = Fixed32::from_int(101); }, "mood");
    expect_violation([](World& w) { state(w).groups.mood[3] = Fixed32::from_int(-101); }, "mood");
    expect_violation([](World& w) { state(w).groups.wealth[4] = Fixed32::from_int(301); }, "wealth");
    expect_violation([](World& w) { state(w).groups.wealth[4] = Fixed32::from_int(-1); }, "wealth");
    expect_violation([](World& w) { state(w).groups.growth_modifier[5] = 1'000'001; }, "growth modifier");
    expect_violation([](World& w) { state(w).groups.growth_carry[5] = 1'000'000; }, "growth carry");
    expect_violation([](World& w) { state(w).groups.growth_carry[5] = -1'000'000; }, "growth carry");
    expect_violation([](World& w) { state(w).provinces.control[1] = Fixed32::from_int(101); }, "control");
    expect_violation([](World& w) { state(w).provinces.control[1] = Fixed32::from_int(-1); }, "control");
    expect_violation([](World& w) { state(w).provinces.unrest[2] = Fixed32::from_int(101); }, "unrest");
    expect_violation([](World& w) { state(w).provinces.unrest[2] = Fixed32::from_raw(-1); }, "unrest");
    expect_violation([](World& w) { state(w).provinces.corruption[0] = Fixed32::from_ratio(11, 10); },
                     "corruption");
    expect_violation([](World& w) { state(w).provinces.corruption[0] = Fixed32::from_raw(-1); },
                     "corruption");
    // The edges themselves are fine.
    World world = make_economy_world();
    EconomyColumnsMut c = world.economy_columns_mut();
    c.mood[0] = kMoodMax;
    c.mood[1] = kMoodMin;
    c.wealth[0] = kWealthMax;
    c.control[0] = kPointsMax;
    c.unrest[0] = kPointsMax;
    c.corruption[0] = kCorruptionMax;
    c.growth_modifier[0] = kMaxGrowthModifierPpm;
    c.growth_carry[0] = -kMaxGrowthCarry;
    require_valid(world);
}

TEST_CASE("economy invariants E-P3: stocks and potentials are not negative") {
    expect_violation([](World& w) { state(w).provinces.grain_stock[1] = Fixed32::from_raw(-1); },
                     "grain stock");
    expect_violation([](World& w) { state(w).provinces.grain_stock[1] = Fixed32::max(); }, "grain stock");
    expect_violation([](World& w) { state(w).provinces.capacity[2 * kMaxGoods + 7] = Fixed32::from_raw(-1); },
                     "capacity of good 7");
    expect_violation([](World& w) { state(w).provinces.capacity[2 * kMaxGoods + 7] = Fixed32::max(); },
                     "capacity of good 7");
}

TEST_CASE("economy invariants E-P4: an administrator is alive") {
    World world = make_economy_world(1, 1);
    CharacterInit init;
    init.dynasty = DynastyId(0);
    init.birth_date = Date::from_ymd(1670, 5, 1);
    const CharacterId person = world.create_character(init);
    REQUIRE(world.set_province_administrator(ProvinceId(0), person));
    require_valid(world);
    // A stale reference written around the API is reported.
    state(world).provinces.administrator[0] = CharacterId(12);
    CHECK(mentions(world.check_invariants(), "administrator 12 is not alive"));
}

TEST_CASE("economy invariants E-B1: the building grid and the construction queue") {
    const auto enqueue = [](World& w, std::uint32_t seq = 0) {
        EconomyState& e = state(w);
        e.provinces.slots_total[1] = 3;
        e.meta.next_construction_seq = std::max(e.meta.next_construction_seq, seq + 1);
        ConstructionQueue& q = e.construction;
        q.province.push_back(ProvinceId(1));
        q.slot.push_back(static_cast<std::uint8_t>(seq));
        q.building.push_back(4);
        q.payer.push_back(DynastyId(1));
        q.start_date.push_back(Date::from_ymd(1703, 1, 1));
        q.finish_date.push_back(Date::from_ymd(1703, 6, 1) + static_cast<std::int32_t>(seq));
        q.paid_kop.push_back(4'000);
        q.seq.push_back(seq);
    };
    // A valid queue passes.
    {
        World world = make_economy_world();
        enqueue(world, 0);
        enqueue(world, 1);
        require_valid(world);
    }
    expect_violation([](World& w) { state(w).provinces.slot_building[0 * kMaxSlots + 4] = 2; },
                     "building in slot 4");  // 0 slots
    expect_violation(
        [&](World& w) {
            enqueue(w);
            state(w).construction.province[0] = ProvinceId(77);
        },
        "unknown province");
    expect_violation(
        [&](World& w) {
            enqueue(w);
            state(w).construction.slot[0] = 3;
        },
        "beyond the slots");
    expect_violation(
        [&](World& w) {
            enqueue(w);
            state(w).provinces.slot_building[1 * kMaxSlots + 0] = 9;
        },
        "occupied slot");
    expect_violation(
        [&](World& w) {
            enqueue(w, 0);
            enqueue(w, 1);
            state(w).construction.slot[1] = 0;
        },
        "share a slot");
    expect_violation(
        [&](World& w) {
            enqueue(w);
            state(w).construction.finish_date[0] = Date::from_ymd(1703, 1, 1);
        },
        "does not finish after it starts");
    expect_violation(
        [&](World& w) {
            enqueue(w, 0);
            enqueue(w, 1);
            state(w).construction.finish_date[0] = Date::from_ymd(1704, 1, 1);
        },
        "out of (finish date, seq) order");
    expect_violation(
        [&](World& w) {
            enqueue(w);
            state(w).construction.building[0] = 0;
        },
        "builds nothing");
    expect_violation(
        [&](World& w) {
            enqueue(w);
            state(w).construction.payer[0] = DynastyId();
        },
        "unknown payer");
    expect_violation(
        [&](World& w) {
            enqueue(w);
            state(w).construction.paid_kop[0] = -1;
        },
        "impossible payment");
    expect_violation(
        [&](World& w) {
            enqueue(w);
            state(w).meta.next_construction_seq = 0;
        },
        "sequence number from the future");
}

TEST_CASE("economy invariants E-L1: loans") {
    const auto lend = [](World& w, std::uint32_t id, std::uint32_t borrower = 1) {
        EconomyState& e = state(w);
        e.meta.next_loan_id = std::max(e.meta.next_loan_id, id + 1);
        LoanTable& l = e.loans;
        l.loan_id.push_back(id);
        l.borrower.push_back(DynastyId(borrower));
        l.principal.push_back(100'000);
        l.rate_ppm_year.push_back(80'000);
        l.issued.push_back(Date::from_ymd(1703, 1, 1));
        l.due.push_back(Date::from_ymd(1704, 1, 1));
        l.interest_carry.push_back(0);
        l.overdue.push_back(0);
        l.collateral.emplace_back();
    };
    {
        World world = make_economy_world();
        lend(world, 0);
        lend(world, 5);
        require_valid(world);
    }
    expect_violation(
        [&](World& w) {
            lend(w, 0);
            state(w).loans.principal[0] = -1;
        },
        "impossible principal");
    expect_violation(
        [&](World& w) {
            lend(w, 0);
            state(w).loans.borrower[0] = DynastyId(9);
        },
        "unknown borrower");
    expect_violation(
        [&](World& w) {
            lend(w, 3);
            lend(w, 3);
        },
        "ascending order of ids");
    expect_violation(
        [&](World& w) {
            lend(w, 3);
            state(w).meta.next_loan_id = 3;
        },
        "ascending order of ids");
    expect_violation(
        [&](World& w) {
            lend(w, 0);
            state(w).loans.interest_carry[0] = 1'000'000;
        },
        "interest carry");
    expect_violation(
        [&](World& w) {
            lend(w, 0);
            state(w).loans.interest_carry[0] = -1;
        },
        "interest carry");
    expect_violation(
        [&](World& w) {
            lend(w, 0);
            state(w).loans.rate_ppm_year[0] = 1'000'001;
        },
        "rate");
    expect_violation(
        [&](World& w) {
            lend(w, 0);
            state(w).loans.overdue[0] = 2;
        },
        "overdue flag");
    expect_violation(
        [&](World& w) {
            lend(w, 0);
            state(w).loans.collateral[0] = ProvinceId(40);
        },
        "unknown collateral");
}

TEST_CASE("economy invariants E-Q1: quotas, outcomes and the incident log") {
    const auto add_quota = [](World& w, std::uint32_t id) {
        EconomyState& e = state(w);
        e.meta.next_quota_id = std::max(e.meta.next_quota_id, id + 1);
        QuotaTable& q = e.quotas;
        q.quota_id.push_back(id);
        q.dynasty.push_back(DynastyId(0));
        q.kind.push_back(static_cast<std::uint8_t>(QuotaKind::Money));
        q.amount.push_back(500'000);
        q.period_start.push_back(Date::from_ymd(1703, 1, 1));
        q.deadline.push_back(Date::from_ymd(1704, 1, 1));
    };
    const auto add_outcome = [](World& w, std::uint32_t seq) {
        EconomyState& e = state(w);
        e.meta.next_outcome_seq = std::max(e.meta.next_outcome_seq, seq + 1);
        QuotaOutcomeRing& o = e.outcomes;
        o.seq.push_back(seq);
        o.dynasty.push_back(DynastyId(0));
        o.kind.push_back(static_cast<std::uint8_t>(QuotaKind::Recruits));
        o.due.push_back(20);
        o.delivered.push_back(15);
        o.settled_on.push_back(Date::from_ymd(1704, 1, 1));
        o.flags.push_back(kQuotaOutcomeShort | kQuotaOutcomePenalty);
    };
    {
        World world = make_economy_world();
        add_quota(world, 0);
        add_quota(world, 1);
        add_outcome(world, 0);
        add_outcome(world, 1);
        (void)world.log_incident(IncidentKind::QuotaShort, ProvinceId(), DynastyId(0), 5);
        require_valid(world);
    }
    expect_violation(
        [&](World& w) {
            add_quota(w, 2);
            add_quota(w, 2);
        },
        "quota 1 breaks the ascending order");
    expect_violation(
        [&](World& w) {
            add_quota(w, 0);
            state(w).quotas.dynasty[0] = DynastyId(8);
        },
        "quota 0 has an unknown dynasty");
    expect_violation(
        [&](World& w) {
            add_quota(w, 0);
            state(w).quotas.kind[0] = 2;
        },
        "unknown kind");
    expect_violation(
        [&](World& w) {
            add_quota(w, 0);
            state(w).quotas.amount[0] = 0;
        },
        "asks for nothing");
    expect_violation(
        [&](World& w) {
            add_quota(w, 0);
            state(w).quotas.deadline[0] = Date::from_ymd(1703, 1, 1);
        },
        "ends before its period starts");
    expect_violation(
        [&](World& w) {
            add_outcome(w, 3);
            add_outcome(w, 2);
        },
        "quota outcome 1 breaks the ascending order");
    expect_violation(
        [&](World& w) {
            add_outcome(w, 0);
            state(w).outcomes.flags[0] = 4;
        },
        "quota outcome 0 is malformed");
    expect_violation(
        [&](World& w) {
            add_outcome(w, 0);
            state(w).outcomes.delivered[0] = -1;
        },
        "quota outcome 0 is malformed");
    expect_violation(
        [&](World& w) {
            add_outcome(w, 0);
            state(w).meta.next_outcome_seq = 0;
        },
        "quota outcome 0 breaks the ascending order");
    expect_violation(
        [&](World& w) {
            for (std::size_t i = 0; i <= kQuotaOutcomeRingSize; ++i) {
                add_outcome(w, static_cast<std::uint32_t>(i));
            }
        },
        "outcome ring is over capacity");
    expect_violation(
        [](World& w) {
            (void)w.log_incident(IncidentKind::Riot, ProvinceId(0), DynastyId(), 1);
            (void)w.log_incident(IncidentKind::Riot, ProvinceId(0), DynastyId(), 2);
            std::swap(state(w).incidents.seq[0], state(w).incidents.seq[1]);
        },
        "incident 1 breaks the ascending order");
    expect_violation(
        [](World& w) {
            (void)w.log_incident(IncidentKind::Riot, ProvinceId(0), DynastyId(), 1);
            state(w).incidents.kind[0] = 0;
        },
        "incident 0 is malformed");
    expect_violation(
        [](World& w) {
            (void)w.log_incident(IncidentKind::Riot, ProvinceId(0), DynastyId(), 1);
            state(w).incidents.province[0] = ProvinceId(90);
        },
        "incident 0 is malformed");
    expect_violation(
        [](World& w) {
            for (std::size_t i = 0; i <= kIncidentLogSize; ++i) {
                IncidentLog& log = state(w).incidents;
                log.seq.push_back(static_cast<std::uint32_t>(i));
                log.date.push_back(Date::from_ymd(1703, 1, 1));
                log.province.emplace_back();
                log.dynasty.emplace_back();
                log.kind.push_back(1);
                log.param.push_back(0);
            }
            state(w).meta.next_incident_seq = 1000;
        },
        "incident log is over capacity");
}

TEST_CASE("economy invariants E-T1: a province cannot have been processed in the future") {
    expect_violation([](World& w) { state(w).provinces.last_slot_date[2] = Date::from_ymd(1703, 1, 2); },
                     "impossible date");
    expect_violation([](World& w) { state(w).provinces.last_slot_date[2] = Date::from_days(5'000'000); },
                     "impossible date");
    World world = make_economy_world();
    world.economy_columns_mut().last_slot_date[1] = world.today();
    require_valid(world);
}

// ---- snapshots and comparison ---------------------------------------------------------------------

TEST_CASE("economy world: a copy of the world carries the economy and is independent of it") {
    World world = make_economy_world();
    (void)world.post_money(AccountRef::of(DynastyId(2)), LedgerCode::TaxCossacks, 4'200);
    (void)world.log_incident(IncidentKind::Riot, ProvinceId(1), DynastyId(), 3);
    REQUIRE(world.close_ledger_month(Date::from_ymd(1703, 2, 1)));
    (void)world.post_money(AccountRef::crown(), LedgerCode::Tribute, 99);

    const std::shared_ptr<const World> snapshot = world.snapshot();
    CHECK(snapshot->economy() == world.economy());
    CHECK(snapshot->economy_present());
    CHECK(snapshot->treasury(AccountRef::of(DynastyId(2))) == 4'200);
    CHECK(snapshot->economy().incidents.size() == 1);
    CHECK(snapshot->economy().meta.closed_months == 1);
    CHECK(snapshot->check_invariants().empty());

    World copy = world;
    CHECK(copy.economy() == world.economy());
    (void)world.post_money(AccountRef::crown(), LedgerCode::Tribute, 1);
    CHECK_FALSE(copy.economy() == world.economy());
    CHECK(snapshot->treasury(AccountRef::crown()) == 99);  // the snapshot did not move
    CHECK(copy.treasury(AccountRef::crown()) == 99);
    require_valid(copy);
}

TEST_CASE("economy world: comparison sees every table and column") {
    World base = make_economy_world();
    (void)base.post_money(AccountRef::of(DynastyId(0)), LedgerCode::TaxPeasants, 10);
    REQUIRE(base.close_ledger_month(Date::from_ymd(1703, 2, 1)));
    (void)base.log_incident(IncidentKind::Riot, ProvinceId(0), DynastyId(0), 1);

    // Each entry changes one column of a copy; the copy must then differ from the original.
    const std::vector<std::function<void(EconomyState&)>> changes = {
        [](EconomyState& e) { e.meta.rules_hash = 1; },
        [](EconomyState& e) { e.meta.crown_dynasty = DynastyId(1); },
        [](EconomyState& e) { e.meta.open_month_start = Date::from_ymd(1703, 3, 1); },
        [](EconomyState& e) { e.meta.closed_months = 5; },
        [](EconomyState& e) { e.meta.next_loan_id = 1; },
        [](EconomyState& e) { e.meta.next_quota_id = 1; },
        [](EconomyState& e) { e.meta.next_outcome_seq = 1; },
        [](EconomyState& e) { e.meta.next_construction_seq = 1; },
        [](EconomyState& e) { Access::treasury(e.accounts)[2] = 1; },
        [](EconomyState& e) { Access::opening(e.accounts)[2] = 1; },
        [](EconomyState& e) { Access::open_lines(e.accounts)[7] = 1; },
        [](EconomyState& e) { Access::ring_opening(e.accounts)[3] = 1; },
        [](EconomyState& e) { Access::ring_lines(e.accounts)[9] = 1; },
        [](EconomyState& e) { Access::ring_month_start(e.accounts)[4] = Date::from_ymd(1700, 1, 2); },
        [](EconomyState& e) { e.dynasties.manpower_reserve[1] = 4; },
        [](EconomyState& e) { e.dynasties.bankrupt[1] = 1; },
        [](EconomyState& e) { e.provinces.kind[1] = 3; },
        [](EconomyState& e) { e.provinces.owner_kind[1] = 3; },
        [](EconomyState& e) { e.provinces.tax_policy[1] = 3; },
        [](EconomyState& e) { e.provinces.recruit_policy[1] = 2; },
        [](EconomyState& e) { e.provinces.slots_total[1] = 2; },
        [](EconomyState& e) { e.provinces.trade_node[1] = 2; },
        [](EconomyState& e) { e.provinces.administrator[1] = CharacterId(0); },
        [](EconomyState& e) { e.provinces.control[1] = Fixed32::zero(); },
        [](EconomyState& e) { e.provinces.corruption[1] = Fixed32::one(); },
        [](EconomyState& e) { e.provinces.unrest[1] = Fixed32::one(); },
        [](EconomyState& e) { e.provinces.labour_base[1] = 5; },
        [](EconomyState& e) { e.provinces.grain_stock[1] = Fixed32::one(); },
        [](EconomyState& e) { e.provinces.recruit_pool[1] = 5; },
        [](EconomyState& e) { e.provinces.income_last_month[1] = 5; },
        [](EconomyState& e) { e.provinces.last_slot_date[1] = Date::from_ymd(1703, 1, 1); },
        [](EconomyState& e) { e.provinces.capacity[11] = Fixed32::one(); },
        [](EconomyState& e) { e.provinces.slot_building[8] = 1; },
        [](EconomyState& e) { e.groups.size[7] += 1; },
        [](EconomyState& e) { e.groups.mood[7] = Fixed32::one(); },
        [](EconomyState& e) { e.groups.wealth[7] = Fixed32::one(); },
        [](EconomyState& e) { e.groups.growth_modifier[7] = 1; },
        [](EconomyState& e) { e.groups.growth_carry[7] = 1; },
        [](EconomyState& e) { e.construction.province.emplace_back(); },
        [](EconomyState& e) { e.loans.loan_id.push_back(0); },
        [](EconomyState& e) { e.quotas.quota_id.push_back(0); },
        [](EconomyState& e) { e.outcomes.seq.push_back(0); },
        [](EconomyState& e) { e.incidents.param[0] = 99; },
    };
    for (std::size_t i = 0; i < changes.size(); ++i) {
        World copy = base;
        REQUIRE(copy.economy() == base.economy());
        changes[i](state(copy));
        INFO("change number " << i);
        CHECK_FALSE(copy.economy() == base.economy());
    }
}

// ---- a world without economy behaves as before ---------------------------------------------------

TEST_CASE("economy world: the economy is not in the hash or the save file yet") {
    // Whole-thousand populations survive the conversion to persons and back exactly.
    World plain = make_plain_world();
    for (std::uint32_t d = 0; d < 3; ++d) {
        (void)plain.create_dynasty(StringId(d));
    }
    for (std::uint32_t p = 0; p < 4; ++p) {
        (void)plain.create_province(StringId(100 + p), DynastyId(p % 3), Fixed32::from_int(10 * (p + 1)));
    }
    const WorldHash before = plain.compute_hash();
    pat::core::ByteWriter before_bytes;
    plain.save_state(before_bytes);

    World world = plain;
    REQUIRE(world.enable_economy());
    (void)world.post_money(AccountRef::of(DynastyId(1)), LedgerCode::TaxPeasants, 1'234);
    (void)world.set_group_size(ProvinceId(0), Group::Clergy, 321);
    (void)world.set_group_size(ProvinceId(0), Group::Clergy, 0);
    (void)world.log_incident(IncidentKind::Riot, ProvinceId(0), DynastyId(0), 1);
    REQUIRE(world.close_ledger_month(Date::from_ymd(1703, 2, 1)));
    world.economy_columns_mut().unrest[0] = Fixed32::from_int(60);

    CHECK(world.compute_hash() == before);
    pat::core::ByteWriter after_bytes;
    world.save_state(after_bytes);
    CHECK(after_bytes.data() == before_bytes.data());
    require_valid(world);
}

TEST_CASE("economy world: the demo scenario is untouched and can host the economy") {
    demo::Config config;
    config.seed = 7;
    World demo_world = demo::make_world(config);
    CHECK_FALSE(demo_world.economy_present());
    CHECK(demo_world.economy() == EconomyState{});
    require_valid(demo_world);

    // Switching the economy on over existing content keeps every invariant.
    REQUIRE(demo_world.enable_economy());
    CHECK(demo_world.economy().provinces.size() == demo_world.provinces().size());
    CHECK(demo_world.economy().accounts.size() == 1 + demo_world.dynasties().size());
    for (std::size_t p = 0; p < demo_world.provinces().size(); ++p) {
        CHECK(demo_world.economy().provinces.owner_kind[p] ==
              static_cast<std::uint8_t>(demo_world.provinces().owner[p].valid() ? OwnerKind::Dynasty
                                                                                : OwnerKind::Unowned));
    }
    require_valid(demo_world);
}

// ---- determinism and the property test ------------------------------------------------------------

namespace {

/// A random script of postings, transfers and month closings against a world; deterministic for a
/// seed. Returns the list of applied amounts (what the world answered).
struct Step {
    int kind = 0;  // 0 post, 1 transfer, 2 close
    std::size_t from = 0;
    std::size_t to = 0;
    std::size_t line = 0;  // dense line, or index into kLedgerPairs for a transfer
    Kopecks amount = 0;
};

std::vector<Step> make_script(std::uint64_t seed, std::size_t steps, std::size_t account_count) {
    Pcg32 rng(seed, 77);
    std::vector<Step> script;
    for (std::size_t i = 0; i < steps; ++i) {
        Step s;
        const std::uint32_t pick = rng.bounded(100);
        s.kind = pick < 80 ? 0 : (pick < 97 ? 1 : 2);
        s.from = rng.bounded(static_cast<std::uint32_t>(account_count));
        s.to = rng.bounded(static_cast<std::uint32_t>(account_count));
        if (s.kind == 1) {
            s.line = rng.bounded(static_cast<std::uint32_t>(kLedgerPairs.size()));
        } else {
            do {  // a single posting never uses a code of a transfer pair
                s.line = rng.bounded(static_cast<std::uint32_t>(kLedgerLineCount));
            } while (is_transfer_code(ledger_code_at(s.line)));
        }
        const std::uint32_t magnitude = rng.bounded(100);
        Kopecks amount = 0;
        if (magnitude < 60) {
            amount = static_cast<Kopecks>(rng.bounded(100'000));
        } else if (magnitude < 85) {
            amount = static_cast<Kopecks>(rng.next_u64() % static_cast<std::uint64_t>(kMaxPostingKop));
        } else if (magnitude < 98) {
            amount = kMaxPostingKop;  // pushes accounts and lines towards their bounds
        } else {
            amount = static_cast<Kopecks>(rng.next_u64() >> 1);  // far beyond a single posting
        }
        // Mostly income, so that the bounds are really reached.
        const bool negative = s.kind == 0 ? rng.bounded(100) < (magnitude < 85 ? 40U : 15U) : false;
        s.amount = negative ? -amount : amount;
        script.push_back(s);
    }
    return script;
}

AccountRef account_at(std::size_t row) {
    return row == 0 ? AccountRef::crown() : AccountRef::of(DynastyId(static_cast<std::uint32_t>(row - 1)));
}

/// Plays a script. `on_step` sees (step, answer) for every step; returns the number of steps.
template <class OnStep>
void play(World& world, const std::vector<Step>& script, OnStep&& on_step) {
    Date month = world.economy().meta.open_month_start;
    for (const Step& s : script) {
        Kopecks answer = 0;
        switch (s.kind) {
            case 0:
                answer = world.post_money(account_at(s.from), ledger_code_at(s.line), s.amount);
                break;
            case 1:
                answer = world.transfer(account_at(s.from),
                                        account_at(s.to),
                                        kLedgerPairs[s.line].out,
                                        kLedgerPairs[s.line].in,
                                        s.amount);
                break;
            default:
                month = month.first_of_next_month();
                answer = world.close_ledger_month(month) ? 1 : 0;
                break;
        }
        on_step(s, answer);
    }
}

}  // namespace

TEST_CASE("economy world: the same script gives the same economy, bit for bit") {
    const std::vector<Step> script = make_script(2024, 4'000, 4);
    World a = make_economy_world(3, 2);
    World b = make_economy_world(3, 2);
    play(a, script, [](const Step&, Kopecks) {});
    play(b, script, [](const Step&, Kopecks) {});
    CHECK(a.economy() == b.economy());
    require_valid(a);

    // A different script differs, so the comparison above means something.
    World c = make_economy_world(3, 2);
    play(c, make_script(2025, 4'000, 4), [](const Step&, Kopecks) {});
    CHECK_FALSE(a.economy() == c.economy());
}

TEST_CASE("economy world: random postings obey the model of double entry") {
    for (const std::uint64_t seed : {1ULL, 2ULL, 3ULL, 4ULL, 5ULL, 6ULL}) {
        INFO("seed " << seed);
        constexpr std::size_t kAccounts = 4;  // the crown and three dynasties
        World world = make_economy_world(3, 2);
        const std::vector<Step> script = make_script(seed, 12'000, kAccounts);

        // The independent model: the amounts the world reports as applied, summed per month. It never
        // looks at the tables, only at the answers.
        using Lines = std::array<Kopecks, kLedgerLineCount>;
        std::vector<std::vector<Lines>> lines_by_month(1, std::vector<Lines>(kAccounts));
        std::vector<std::vector<Kopecks>> opening_by_month(1, std::vector<Kopecks>(kAccounts, 0));
        std::vector<Kopecks> balance(kAccounts, 0);
        Kopecks moved_out = 0;
        Kopecks moved_in = 0;
        bool saw_bound = false;

        play(world, script, [&](const Step& s, Kopecks answer) {
            if (s.kind == 0) {
                const Kopecks wanted = std::clamp(s.amount, -kMaxPostingKop, kMaxPostingKop);
                // Never more than asked, same direction.
                REQUIRE(answer >= std::min<Kopecks>(0, wanted));
                REQUIRE(answer <= std::max<Kopecks>(0, wanted));
                const std::size_t line = s.line;
                lines_by_month.back()[s.from][line] += answer;
                balance[s.from] += answer;
                if (answer != wanted) {
                    // Less was applied than asked: a bound must have been hit.
                    saw_bound = true;
                    const Kopecks line_now = lines_by_month.back()[s.from][line];
                    const bool at_balance_bound =
                        balance[s.from] == kMaxTreasuryKop || balance[s.from] == -kMaxTreasuryKop;
                    const bool at_line_bound =
                        line_now == kMaxLedgerLineKop || line_now == -kMaxLedgerLineKop;
                    REQUIRE((at_balance_bound || at_line_bound));
                }
            } else if (s.kind == 1) {
                REQUIRE(answer >= 0);
                REQUIRE(answer <= s.amount);
                const LedgerPair pair = kLedgerPairs[s.line];
                const std::size_t out_line = *ledger_index(pair.out);
                const std::size_t in_line = *ledger_index(pair.in);
                lines_by_month.back()[s.from][out_line] -= answer;
                lines_by_month.back()[s.to][in_line] += answer;
                balance[s.from] -= answer;
                balance[s.to] += answer;
                moved_out += answer;
                moved_in += answer;
            } else {
                REQUIRE(answer == 1);
                opening_by_month.push_back(balance);
                lines_by_month.emplace_back(kAccounts);
            }
        });

        CHECK(moved_out == moved_in);
        // The model and the tables agree on everything.
        const AccountTable& t = world.economy().accounts;
        // The crown and the dynasty accounts are not the same row, but the model's rows are the world's.
        const std::size_t months = lines_by_month.size();
        for (std::size_t a = 0; a < kAccounts; ++a) {
            CHECK(t.treasury(a) == balance[a]);
            CHECK(t.opening(a) == opening_by_month.back()[a]);
            for (std::size_t l = 0; l < kLedgerLineCount; ++l) {
                CHECK(t.open_lines(a)[l] == lines_by_month.back()[a][l]);
            }
            const std::size_t closed = std::min<std::size_t>(months - 1, kLedgerRingMonths);
            for (std::size_t m = 0; m < closed; ++m) {
                const std::size_t month_index = months - 2 - m;
                CHECK(t.ring_opening(a, m) == opening_by_month[month_index][a]);
                for (std::size_t l = 0; l < kLedgerLineCount; ++l) {
                    CHECK(t.ring_lines(a, m)[l] == lines_by_month[month_index][a][l]);
                }
            }
        }
        // Money is only created by the lines of the outside world: the sum of all balances equals the
        // sum of every line of every account (transfers cancel out).
        Kopecks all_balances = 0;
        for (const Kopecks b : balance) {
            all_balances += b;
        }
        Kopecks all_lines = 0;
        for (const auto& month : lines_by_month) {
            for (const Lines& account_lines : month) {
                for (const Kopecks line : account_lines) {
                    all_lines += line;
                }
            }
        }
        CHECK(all_balances == all_lines);
        require_valid(world);
        MESSAGE("seed " << seed << ": reached a bound = " << saw_bound);
    }
}

TEST_CASE("economy world: the property test really reaches the bounds") {
    // Guards the property test above: at least one seed saturates, otherwise it proves too little.
    bool any = false;
    for (const std::uint64_t seed : {1ULL, 2ULL, 3ULL, 4ULL, 5ULL, 6ULL}) {
        World world = make_economy_world(3, 2);
        play(world, make_script(seed, 12'000, 4), [&](const Step& s, Kopecks answer) {
            if (s.kind == 0 && answer != std::clamp(s.amount, -kMaxPostingKop, kMaxPostingKop)) {
                any = true;
            }
        });
    }
    CHECK(any);
}
