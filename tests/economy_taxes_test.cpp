#include "pat/core/rng.hpp"
#include "pat/sim/calendar.hpp"
#include "pat/sim/economy/breakdown.hpp"
#include "pat/sim/economy/ledger.hpp"
#include "pat/sim/economy/population.hpp"
#include "pat/sim/economy/production.hpp"
#include "pat/sim/economy/rules.hpp"
#include "pat/sim/economy/systems.hpp"
#include "pat/sim/economy/tables.hpp"
#include "pat/sim/economy/taxes.hpp"
#include "pat/sim/simulation.hpp"
#include "pat/sim/world.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

// E9-T08: taxes of the groups of a province and the posting of the income (docs/dev/economy-model.md
// section 9 formula F6, section 7.3 step 4; GDD 03 sections 3.2, 3.2a and 7.1).

using namespace pat::sim;
using namespace pat::sim::economy;
using pat::core::Fixed32;
using pat::core::Pcg32;
using pat::core::StringId;

namespace {

const EconomyRules& rules() {
    static const EconomyRules value = [] {
        EconomyRules r = EconomyRules::defaults();
        // E9-T10: the administration is frozen here, control and corruption stay as the test sets them
        // (the dynamics of F7 have their own tests in economy_administrator_test.cpp)
        r.control.control_inertia_permille = 0;
        r.control.corruption_inertia_permille = 0;
        // E9-T11: mood, unrest and riots are frozen too (their dynamics have their own tests)
        r.mood.inertia_permille = 0;
        r.unrest.inertia_permille = 0;
        r.unrest.riot_chance_min_permille = 0;
        r.unrest.riot_chance_max_permille = 0;
        return r;
    }();
    return value;
}

void require_valid(const World& world) {
    const auto problems = world.check_invariants();
    for (const auto& problem : problems) {
        MESSAGE(problem);
    }
    REQUIRE(problems.empty());
}

Fixed32 points(std::int64_t whole) {
    return Fixed32::from_int(static_cast<std::int32_t>(whole));
}

/// A province in the peaceful middle: normal wealth, full control, no corruption, the normal policy.
TaxInput normal_input() {
    TaxInput in;
    in.wealth.fill(points(100));
    in.control = points(100);
    in.corruption = Fixed32::zero();
    in.kind = ProvinceKind::Rural;
    in.owner_kind = OwnerKind::Dynasty;
    in.tax_level = 1;  // normal, x1.0
    return in;
}

/// A corruption of `permille` per-mille: the raw value is rounded up so that it reads back exactly.
Fixed32 corruption_of(std::int32_t permille) {
    return Fixed32::from_raw((permille * Fixed32::one_raw + 999) / 1000);
}

constexpr std::size_t kPeasants = index_of(Group::Peasants);
constexpr std::size_t kTownsfolk = index_of(Group::Townsfolk);

GroupTax tax_of(const EconomyRules& r, const TaxInput& in, Group group, Breakdown* b = nullptr) {
    return compute_group_tax(r.tax, r.control, in, group, b);
}

/// Two dynasties and `provinces` provinces owned by dynasty 0; economy on; peasants and townsfolk set.
World make_world(std::uint32_t provinces,
                 std::uint32_t peasants = 1000,
                 std::uint32_t townsfolk = 0,
                 Date start = Date::from_ymd(1702, 12, 31)) {
    World world(start, 5);
    (void)world.create_dynasty(StringId(1));
    (void)world.create_dynasty(StringId(2));
    for (std::uint32_t p = 0; p < provinces; ++p) {
        (void)world.create_province(StringId(100 + p), DynastyId(0), Fixed32::from_int(1));
    }
    REQUIRE(world.enable_economy());
    for (std::uint32_t p = 0; p < provinces; ++p) {
        (void)world.set_group_size(ProvinceId(p), Group::Peasants, peasants);
        (void)world.set_group_size(ProvinceId(p), Group::Townsfolk, townsfolk);
        // a new row starts at policy level 0; the normal level (x1.0) is the default of the rules
        world.economy_columns_mut().tax_policy[p] = static_cast<std::uint8_t>(rules().tax.default_level);
    }
    require_valid(world);
    return world;
}

Kopecks line(const World& world, AccountRef account, LedgerCode code) {
    const auto row = world.resolve_account(account);
    REQUIRE(row.has_value());
    return world.economy().accounts.open_line(*row, code);
}

/// The sum of one ledger line over every account.
Kopecks line_everywhere(const World& world, LedgerCode code) {
    Kopecks sum = 0;
    for (std::size_t row = 0; row < world.economy().accounts.size(); ++row) {
        sum += world.economy().accounts.open_line(row, code);
    }
    return sum;
}

Simulation economy_sim(World world) {
    Simulation sim(std::move(world));
    install_economy_systems(sim, rules());
    return sim;
}

void run_months(Simulation& sim, int months) {
    for (int i = 0; i < months; ++i) {
        const Date next = sim.world().today() + 1;
        sim.run_days(Date::days_in_month(next.year(), next.month()));
    }
}

}  // namespace

// ---- the pieces of the formula ---------------------------------------------------------------------

TEST_CASE("taxes: wealth and corruption become per-mille, truncated and clamped") {
    CHECK(points_permille(points(100)) == 1000);
    CHECK(points_permille(points(0)) == 0);
    CHECK(points_permille(points(300)) == 3000);
    CHECK(points_permille(points(150)) == 1500);
    CHECK(points_permille(Fixed32::from_raw(Fixed32::one_raw / 10 * 9)) ==
          8);  // 0.9 points = 9 per-mille, 8 after the raw truncation
    CHECK(points_permille(points(-5)) == 0);
    CHECK(points_permille(points(1000)) == 3000);
    CHECK(corruption_permille(Fixed32::zero()) == 0);
    CHECK(corruption_permille(points(1)) == 1000);
    CHECK(corruption_permille(corruption_of(250)) == 250);
    CHECK(corruption_permille(Fixed32::from_raw(Fixed32::one_raw * 2 / 3)) == 666);
    CHECK(corruption_permille(Fixed32::from_raw(-1)) == 0);
    CHECK(corruption_permille(points(5)) == 1000);
}

TEST_CASE("taxes: the base is the head tax of a year over twelve, truncated, for any size") {
    CHECK(tax_base(1000, 50) == 4166);  // 50 000 / 12 = 4166.67
    CHECK(tax_base(12, 50) == 50);
    CHECK(tax_base(11, 50) == 45);  // 550 / 12 = 45.83
    CHECK(tax_base(0, 50) == 0);
    CHECK(tax_base(1000, 0) == 0);
    CHECK(tax_base(1000, -5) == 0);
    CHECK(tax_base(1, 11) == 0);  // less than a kopeck a month: nothing (documented truncation)
    CHECK(tax_base(1, 12) == 1);
    // the product of the largest values needs 128 bits and ends capped at a posting
    CHECK(tax_base(0xFFFFFFFFU, kMaxPostingKop) == kMaxPostingKop);
    CHECK(tax_base(32'000'000, 150'000) == 400'000'000'000);
}

// ---- the tax of a group ----------------------------------------------------------------------------

TEST_CASE("taxes: the default rules tax the groups by the rates of the GDD, nobility and clergy nothing") {
    TaxInput in = normal_input();
    in.size = {{500, 300, 2000, 12'000, 600, 40}};
    // rates 0, 0, 150, 50, 20, 40 kopecks a head a year
    CHECK(tax_of(rules(), in, Group::Nobility).gross == 0);
    CHECK(tax_of(rules(), in, Group::Clergy).gross == 0);
    CHECK(tax_of(rules(), in, Group::Townsfolk).gross == 2000 * 150 / 12);
    CHECK(tax_of(rules(), in, Group::Peasants).gross == 12'000 * 50 / 12);
    CHECK(tax_of(rules(), in, Group::Cossacks).gross == 600 * 20 / 12);
    CHECK(tax_of(rules(), in, Group::Natives).gross == 40 * 40 / 12);
    // no corruption, a rural province: everything goes to the owner
    const GroupTax peasants = tax_of(rules(), in, Group::Peasants);
    CHECK(peasants.leak == 0);
    CHECK(peasants.crown == 0);
    CHECK(peasants.owner() == peasants.gross);
}

TEST_CASE("taxes: the multipliers act in the order of the spec and each one truncates") {
    EconomyRules r = rules();
    r.tax.law_permille = 1100;
    TaxInput in = normal_input();
    in.size[kPeasants] = 1000;
    in.tax_level = 2;  // high, x1.3
    in.wealth[kPeasants] = points(150);
    in.control = points(80);  // above the low threshold of 40: no low-control factor

    Kopecks expected = 1000 * 50 / 12;  // 4166
    expected = expected * 1300 / 1000;  // 5415 (5415.8)
    expected = expected * 1500 / 1000;  // 8122 (8122.5)
    expected = expected * 800 / 1000;   // 6497 (6497.6)
    expected = expected * 1100 / 1000;  // 7146 (7146.7)
    CHECK(expected == 7146);
    CHECK(tax_of(r, in, Group::Peasants).gross == expected);

    // the order is visible on small numbers: x0.5 then x1.5 is not x1.5 then x0.5
    EconomyRules small = rules();
    small.tax.levels[1].tax_permille = 500;
    TaxInput t = normal_input();
    t.size[kPeasants] = 12;            // base 50
    t.wealth[kPeasants] = points(35);  // x0.35
    // 50 x 0.5 = 25, then 25 x 0.35 = 8.75 -> 8
    CHECK(tax_of(small, t, Group::Peasants).gross == 8);
}

TEST_CASE("taxes: a higher rate or a higher policy level raises the income (ECO-A1)") {
    TaxInput in = normal_input();
    in.size = {{0, 0, 3000, 40'000, 2000, 0}};
    Kopecks previous = -1;
    for (std::uint8_t level = 0; level < rules().tax.level_count; ++level) {
        in.tax_level = level;
        Kopecks total = 0;
        for (std::size_t g = 0; g < kGroupCount; ++g) {
            total += tax_of(rules(), in, static_cast<Group>(g)).gross;
        }
        INFO("level " << static_cast<int>(level));
        CHECK(total > previous);
        previous = total;
    }
    // the same for the rate of a group
    Kopecks last = -1;
    for (Kopecks rate = 10; rate <= 200; rate += 10) {
        EconomyRules r = rules();
        r.tax.head_tax_kop_year[kPeasants] = rate;
        in.tax_level = 1;
        const Kopecks now = tax_of(r, in, Group::Peasants).gross;
        INFO("rate " << rate);
        CHECK(now > last);
        last = now;
    }
    // and for wealth and control, which are part of the efficiency
    last = -1;
    for (int wealth = 20; wealth <= 300; wealth += 20) {
        in.wealth[kPeasants] = points(wealth);
        const Kopecks now = tax_of(rules(), in, Group::Peasants).gross;
        CHECK(now > last);
        last = now;
    }
}

TEST_CASE("taxes: a level index past the table is the last level, an empty table is x1.0") {
    TaxInput in = normal_input();
    in.size[kPeasants] = 12'000;
    in.tax_level = 200;
    const Kopecks last = tax_of(rules(), in, Group::Peasants).gross;
    in.tax_level = static_cast<std::uint8_t>(rules().tax.level_count - 1);
    CHECK(last == tax_of(rules(), in, Group::Peasants).gross);
    EconomyRules none = rules();
    none.tax.level_count = 0;
    in.tax_level = 3;
    CHECK(tax_of(none, in, Group::Peasants).gross == 12'000 * 50 / 12);
}

TEST_CASE("taxes: low control collects less, turmoil collects nothing, at the exact thresholds") {
    TaxInput in = normal_input();
    in.size[kPeasants] = 12'000;  // base 50 000
    const auto gross_at = [&](Fixed32 control) {
        in.control = control;
        return tax_of(rules(), in, Group::Peasants).gross;
    };
    // control 40 is not "below 40": only the control factor itself acts (x0.4)
    CHECK(gross_at(points(40)) == 50'000 * 400 / 1000);
    // just below 40: the low-control factor x0.7 joins
    const Fixed32 just_below_40 = Fixed32::from_raw(points(40).raw() - 1);
    CHECK(gross_at(just_below_40) == 50'000 * 399 / 1000 * 700 / 1000);
    CHECK(gross_at(points(30)) == 50'000 * 300 / 1000 * 700 / 1000);
    // control 15 is not "below 15": the province still pays
    CHECK(gross_at(points(15)) == 50'000 * 150 / 1000 * 700 / 1000);
    CHECK(gross_at(Fixed32::from_raw(points(15).raw() - 1)) == 0);
    CHECK(gross_at(points(0)) == 0);
    CHECK(gross_at(points(14)) == 0);
    // the thresholds are rules
    EconomyRules strict = rules();
    strict.control.anarchy_threshold = 50;
    in.control = points(45);
    CHECK(tax_of(strict, in, Group::Peasants).gross == 0);
    EconomyRules lax = rules();
    lax.control.low_threshold = 10;
    in.control = points(30);
    CHECK(tax_of(lax, in, Group::Peasants).gross == 50'000 * 300 / 1000);
}

TEST_CASE("taxes: corruption is stolen from the gross tax, the rest is the owner's") {
    TaxInput in = normal_input();
    in.size[kPeasants] = 12'000;         // gross 50 000
    in.corruption = corruption_of(250);  // 25 %
    const GroupTax tax = tax_of(rules(), in, Group::Peasants);
    CHECK(tax.gross == 50'000);
    CHECK(tax.leak == 12'500);
    CHECK(tax.crown == 0);
    CHECK(tax.owner() == 37'500);
    // the leak is truncated: the odd kopeck stays with the owner
    in.size[kPeasants] = 1000;  // gross 4166, 25 % = 1041.5
    const GroupTax odd = tax_of(rules(), in, Group::Peasants);
    CHECK(odd.gross == 4166);
    CHECK(odd.leak == 1041);
    CHECK(odd.owner() == 3125);
    CHECK(odd.leak + odd.owner() == odd.gross);
    // total corruption leaves nothing
    in.corruption = points(1);
    CHECK(tax_of(rules(), in, Group::Peasants).owner() == 0);
}

TEST_CASE("taxes: only the townsfolk of a city held by a dynasty give a share to the crown") {
    TaxInput in = normal_input();
    in.size = {{0, 0, 2000, 12'000, 0, 0}};  // townsfolk gross 25 000, peasants 50 000
    in.kind = ProvinceKind::City;
    in.corruption = corruption_of(100);  // 10 %: 2 500 leak
    const GroupTax townsfolk = tax_of(rules(), in, Group::Townsfolk);
    CHECK(townsfolk.gross == 25'000);
    CHECK(townsfolk.leak == 2500);
    // 30 % of what is left after the leak: 22 500 x 0.3 = 6 750
    CHECK(townsfolk.crown == 6750);
    CHECK(townsfolk.owner() == 25'000 - 2500 - 6750);
    // the peasants of the same city pay the owner alone
    CHECK(tax_of(rules(), in, Group::Peasants).crown == 0);
    // another kind of province, or the crown as the owner: no split
    in.kind = ProvinceKind::Rural;
    CHECK(tax_of(rules(), in, Group::Townsfolk).crown == 0);
    in.kind = ProvinceKind::City;
    in.owner_kind = OwnerKind::Crown;
    CHECK(tax_of(rules(), in, Group::Townsfolk).crown == 0);
    // the share is a rule
    in.owner_kind = OwnerKind::Dynasty;
    EconomyRules none = rules();
    none.tax.city_crown_share_permille = 0;
    CHECK(tax_of(none, in, Group::Townsfolk).crown == 0);
    EconomyRules all = rules();
    all.tax.city_crown_share_permille = 1000;
    const GroupTax whole = tax_of(all, in, Group::Townsfolk);
    CHECK(whole.crown == 22'500);
    CHECK(whole.owner() == 0);  // the owner keeps nothing, never less than nothing
}

TEST_CASE("taxes: the breakdown names the steps, adds up to what the owner keeps and changes nothing") {
    EconomyRules r = rules();
    TaxInput in = normal_input();
    in.size[kTownsfolk] = 2000;
    in.kind = ProvinceKind::City;
    in.tax_level = 3;  // x1.6
    in.wealth[kTownsfolk] = points(80);
    in.control = points(30);             // low control: x0.7
    in.corruption = corruption_of(200);  // 20 %

    Breakdown b;
    const GroupTax with = tax_of(r, in, Group::Townsfolk, &b);
    CHECK(with == tax_of(r, in, Group::Townsfolk));  // the result does not depend on the breakdown
    CHECK(b.is_consistent());
    CHECK(b.root().code == term::kTaxTotal);
    CHECK(b.root().unit == Unit::Kopecks);
    CHECK(b.total() == with.owner());
    CHECK(b.node(b.find(term::kTaxBase)).value == tax_base(2000, 150));
    const auto factor = [&](std::uint16_t code) { return b.node(b.find(code)).factor_permille; };
    CHECK(factor(term::kTaxPolicy) == 1600);
    CHECK(factor(term::kTaxWealth) == 800);
    CHECK(factor(term::kTaxControl) == 300);
    CHECK(factor(term::kTaxLowControl) == 700);
    CHECK(b.find(term::kTaxLaw) == kNoNode);  // x1.0 is left out
    CHECK(b.find(term::kTaxAnarchy) == kNoNode);
    CHECK(b.node(b.find(term::kTaxLeak)).value == -with.leak);
    CHECK(b.node(b.find(term::kTaxLeak)).factor_permille == 200);
    CHECK(b.node(b.find(term::kTaxCrownShare)).value == -with.crown);
    CHECK(b.node(b.find(term::kTaxCrownShare)).factor_permille == 300);
    CHECK(b.child_count(kRootNode) == b.size() - 1);  // every term hangs under the total

    // the same through the explanation of a province
    CHECK(explain_group_tax(r, in, Group::Townsfolk) == b);

    // turmoil: a step to zero that closes the sum
    in.control = points(10);
    Breakdown turmoil;
    CHECK(tax_of(r, in, Group::Townsfolk, &turmoil).owner() == 0);
    CHECK(turmoil.is_consistent());
    CHECK(turmoil.total() == 0);
    CHECK(turmoil.find(term::kTaxAnarchy) != kNoNode);
    CHECK(turmoil.node(turmoil.find(term::kTaxAnarchy)).value < 0);
    CHECK(turmoil.find(term::kTaxLeak) == kNoNode);

    // an unknown group is an empty tax with an empty breakdown
    Breakdown none;
    CHECK(tax_of(r, in, static_cast<Group>(9), &none) == GroupTax{});
    CHECK(none.total() == 0);
    CHECK(none.size() == 1);
}

TEST_CASE("taxes: random provinces keep every invariant, and the explanation tells the same number") {
    Pcg32 rng(2025, 11);
    for (int round = 0; round < 4000; ++round) {
        TaxInput in;
        for (std::size_t g = 0; g < kGroupCount; ++g) {
            in.size[g] = rng.chance_permille(300) ? 0 : rng.bounded(30'000'000);
            in.wealth[g] = Fixed32::from_raw(rng.range(0, 300 * Fixed32::one_raw));
        }
        in.control = Fixed32::from_raw(rng.range(0, 100 * Fixed32::one_raw));
        in.corruption = Fixed32::from_raw(rng.range(0, Fixed32::one_raw));
        in.kind = static_cast<ProvinceKind>(rng.bounded(kProvinceKindCount));
        in.owner_kind = static_cast<OwnerKind>(rng.bounded(kOwnerKindCount));
        in.tax_level = static_cast<std::uint8_t>(rng.bounded(6));
        EconomyRules r = rules();
        r.tax.city_crown_share_permille = rng.range(0, 1000);
        r.tax.law_permille = rng.range(0, 3000);
        const Kopecks holding = rng.range(0, 1'000'000'000);

        const ProvinceIncome income = compute_province_income(r, in, holding);
        REQUIRE(income == compute_province_income(r, in, holding));
        for (std::size_t g = 0; g < kGroupCount; ++g) {
            const GroupTax& tax = income.group[g];
            REQUIRE(tax.gross >= 0);
            REQUIRE(tax.gross <= kMaxPostingKop);
            REQUIRE(tax.leak >= 0);
            REQUIRE(tax.crown >= 0);
            REQUIRE(tax.owner() >= 0);
            REQUIRE(tax.leak + tax.crown + tax.owner() == tax.gross);
            REQUIRE(tax.gross == tax_of(r, in, static_cast<Group>(g)).gross);
            Breakdown b;
            REQUIRE(tax_of(r, in, static_cast<Group>(g), &b) == tax);
            REQUIRE(b.is_consistent());
            REQUIRE(b.total() == tax.owner());
        }
        REQUIRE(income.owner_total() + income.leak_total() + income.crown_total() ==
                income.gross_total() + income.holding);
        REQUIRE(income.net_total() == income.gross_total() - income.leak_total());
        if (income.anarchy) {
            REQUIRE(income.gross_total() == 0);
            REQUIRE(income.holding == 0);
            REQUIRE(income.owner_total() == 0);
        }
    }
}

// ---- the province: input, recipient, postings ----------------------------------------------------------

TEST_CASE("taxes: the input is read from the world and the recipient follows the owner kind") {
    World world = make_world(4, 1000, 300);
    auto columns = world.economy_columns_mut();
    columns.control[1] = points(33);
    columns.corruption[1] = corruption_of(500);
    columns.kind[1] = static_cast<std::uint8_t>(ProvinceKind::City);
    columns.tax_policy[1] = 3;
    columns.wealth[group_row(1, Group::Peasants)] = points(77);

    const TaxInput in = tax_input(world, ProvinceId(1));
    CHECK(in.size[kPeasants] == 1000);
    CHECK(in.size[kTownsfolk] == 300);
    CHECK(in.control == points(33));
    CHECK(in.corruption == corruption_of(500));
    CHECK(in.kind == ProvinceKind::City);
    CHECK(in.owner_kind == OwnerKind::Dynasty);
    CHECK(in.tax_level == 3);
    CHECK(in.wealth[kPeasants] == points(77));
    CHECK(in.wealth[kTownsfolk] == points(100));

    CHECK(income_recipient(world, ProvinceId(0)) == AccountRef::of(DynastyId(0)));
    REQUIRE(world.set_province_owner_kind(ProvinceId(1), OwnerKind::Crown));
    CHECK(income_recipient(world, ProvinceId(1)) == AccountRef::crown());
    REQUIRE(world.set_province_owner_kind(ProvinceId(2), OwnerKind::Church));
    CHECK_FALSE(income_recipient(world, ProvinceId(2)).has_value());
    REQUIRE(world.set_province_owner_kind(ProvinceId(3), OwnerKind::Unowned));
    CHECK_FALSE(income_recipient(world, ProvinceId(3)).has_value());
    CHECK(tax_input(world, ProvinceId(1)).owner_kind == OwnerKind::Crown);

    // an unknown province and a world without economy: defaults and no recipient
    CHECK_FALSE(income_recipient(world, ProvinceId(99)).has_value());
    CHECK(tax_input(world, ProvinceId(99)).size == std::array<std::uint32_t, kGroupCount>{});
    World bare(Date::from_ymd(1703, 1, 1), 1);
    (void)bare.create_province(StringId(1), DynastyId(), Fixed32::from_int(1));
    CHECK_FALSE(income_recipient(bare, ProvinceId(0)).has_value());
    CHECK(step_province_taxes(bare, rules(), ProvinceId(0), 500) == ProvinceIncome{});
    CHECK(step_province_taxes(world, rules(), ProvinceId(99), 500) == ProvinceIncome{});
}

TEST_CASE("taxes: a month of a province posts the taxes and the holding income to the dynasty") {
    World world = make_world(1, 12'000, 0);  // peasants 12 000: gross 50 000
    const AccountRef owner = AccountRef::of(DynastyId(0));
    const Kopecks before = world.treasury(owner);
    const ProvinceIncome income = step_province_taxes(world, rules(), ProvinceId(0), 7777);

    CHECK(income.group[kPeasants].gross == 50'000);
    CHECK(income.holding == 7777);
    CHECK(line(world, owner, LedgerCode::TaxPeasants) == 50'000);
    CHECK(line(world, owner, LedgerCode::HoldingIncome) == 7777);
    CHECK(line(world, owner, LedgerCode::Embezzlement) == 0);
    CHECK(line(world, owner, LedgerCode::TaxTownsfolk) == 0);
    CHECK(world.treasury(owner) == before + 50'000 + 7777);
    CHECK(world.economy().provinces.income_last_month[0] == 57'777);
    // the crown and the other dynasty got nothing
    CHECK(world.treasury(AccountRef::crown()) == 0);
    CHECK(world.treasury(AccountRef::of(DynastyId(1))) == 0);
    require_valid(world);
}

TEST_CASE("taxes: the leak is a negative line next to the tax and the income is the net") {
    World world = make_world(1, 12'000, 0);
    world.economy_columns_mut().corruption[0] = corruption_of(250);
    const AccountRef owner = AccountRef::of(DynastyId(0));
    (void)step_province_taxes(world, rules(), ProvinceId(0), 1000);
    CHECK(line(world, owner, LedgerCode::TaxPeasants) == 50'000);
    CHECK(line(world, owner, LedgerCode::Embezzlement) == -12'500);
    CHECK(world.treasury(owner) == 50'000 - 12'500 + 1000);
    CHECK(world.economy().provinces.income_last_month[0] == 37'500 + 1000);
    require_valid(world);
}

TEST_CASE("taxes: a city gives the crown its share of the townsfolk tax and the owner the rest") {
    World world = make_world(1, 0, 2000);  // townsfolk 2000: gross 25 000
    world.economy_columns_mut().kind[0] = static_cast<std::uint8_t>(ProvinceKind::City);
    world.economy_columns_mut().corruption[0] = corruption_of(100);
    const AccountRef owner = AccountRef::of(DynastyId(0));
    const AccountRef crown = AccountRef::crown();
    (void)step_province_taxes(world, rules(), ProvinceId(0), 0);

    // leak 2500, crown 30 % of 22 500 = 6750, the owner 15 750
    CHECK(line(world, owner, LedgerCode::TaxTownsfolk) == 25'000 - 6750);
    CHECK(line(world, owner, LedgerCode::Embezzlement) == -2500);
    CHECK(line(world, crown, LedgerCode::TaxTownsfolk) == 6750);
    CHECK(world.treasury(owner) == 15'750);
    CHECK(world.treasury(crown) == 6750);
    CHECK(world.economy().provinces.income_last_month[0] == 15'750);
    // the postings add up to the net tax of the province (gross - leak)
    CHECK(line_everywhere(world, LedgerCode::TaxTownsfolk) +
              line_everywhere(world, LedgerCode::Embezzlement) ==
          22'500);
    require_valid(world);
}

TEST_CASE("taxes: a crown province pays the crown account, a province of nobody pays nobody") {
    World world = make_world(3, 12'000, 2000);
    world.economy_columns_mut().kind[0] = static_cast<std::uint8_t>(ProvinceKind::City);
    REQUIRE(world.set_province_owner_kind(ProvinceId(0), OwnerKind::Crown));
    REQUIRE(world.set_province_owner_kind(ProvinceId(1), OwnerKind::Church));
    REQUIRE(world.set_province_owner_kind(ProvinceId(2), OwnerKind::Unowned));
    for (std::uint32_t p = 0; p < 3; ++p) {
        (void)step_province_taxes(world, rules(), ProvinceId(p), 5000);
    }
    // the crown owns the city: it gets the whole tax of the townsfolk, no split
    CHECK(world.treasury(AccountRef::crown()) == 50'000 + 25'000 + 5000);
    CHECK(line(world, AccountRef::crown(), LedgerCode::TaxTownsfolk) == 25'000);
    CHECK(world.treasury(AccountRef::of(DynastyId(0))) == 0);
    CHECK(world.economy().provinces.income_last_month[0] == 80'000);
    // Church and nobody: computed but not paid
    CHECK(world.economy().provinces.income_last_month[1] == 0);
    CHECK(world.economy().provinces.income_last_month[2] == 0);
    CHECK(line_everywhere(world, LedgerCode::TaxPeasants) == 50'000);
    CHECK(line_everywhere(world, LedgerCode::HoldingIncome) == 5000);
    require_valid(world);
}

TEST_CASE("taxes: in turmoil the owner gets nothing, not even the holding income, and the crown no share") {
    World world = make_world(2, 12'000, 2000);
    world.economy_columns_mut().kind[0] = static_cast<std::uint8_t>(ProvinceKind::City);
    world.economy_columns_mut().control[0] = points(10);
    world.economy_columns_mut().income_last_month[0] = 999;  // from an earlier month
    const ProvinceIncome income = step_province_taxes(world, rules(), ProvinceId(0), 5000);
    CHECK(income.anarchy);
    CHECK(income.owner_total() == 0);
    CHECK(income.crown_total() == 0);
    CHECK(world.treasury(AccountRef::of(DynastyId(0))) == 0);
    CHECK(world.treasury(AccountRef::crown()) == 0);
    CHECK(world.economy().provinces.income_last_month[0] == 0);  // overwritten, not left over
    for (const LedgerCode code : kLedgerCodes) {
        CHECK(line_everywhere(world, code) == 0);
    }
    // the next province, at control 100, pays as usual
    (void)step_province_taxes(world, rules(), ProvinceId(1), 5000);
    CHECK(world.treasury(AccountRef::of(DynastyId(0))) > 0);
    require_valid(world);
}

TEST_CASE("taxes: an account at its limit takes less and the income is what was really applied") {
    World world = make_world(1, 12'000, 0);
    const AccountRef owner = AccountRef::of(DynastyId(0));
    // fill the account of the dynasty up to kMaxTreasuryKop - 10 000 through the service line
    const Kopecks target = kMaxTreasuryKop - 10'000;
    while (world.treasury(owner) + kMaxPostingKop <= target) {
        (void)world.post_money(owner, LedgerCode::Adjustment, kMaxPostingKop);
    }
    (void)world.post_money(owner, LedgerCode::Adjustment, target - world.treasury(owner));
    REQUIRE(world.treasury(owner) == target);

    const ProvinceIncome income = step_province_taxes(world, rules(), ProvinceId(0), 7000);
    CHECK(income.group[kPeasants].gross == 50'000);   // what was computed
    CHECK(world.treasury(owner) == kMaxTreasuryKop);  // what fitted
    CHECK(world.economy().provinces.income_last_month[0] == 10'000);
    // the saturation is counted, not silent: what the account refused is requested (tax + holding) - applied
    CHECK(income.refused_kop == 50'000 + 7000 - 10'000);
    require_valid(world);  // the ledger still closes: balance == opening + lines

    // and an account with room refuses nothing
    World roomy = make_world(1, 12'000, 0);
    CHECK(step_province_taxes(roomy, rules(), ProvinceId(0), 7000).refused_kop == 0);
}

TEST_CASE("taxes: postings and the column are a function of the world, run twice the same") {
    World a = make_world(3, 5000, 700);
    World b = a;
    for (std::uint32_t p = 0; p < 3; ++p) {
        a.economy_columns_mut().corruption[p] =
            Fixed32::from_raw(Fixed32::one_raw / 7 * static_cast<std::int32_t>(p));
        b.economy_columns_mut().corruption[p] = a.economy().provinces.corruption[p];
        (void)step_province_taxes(a, rules(), ProvinceId(p), 100 * p);
        (void)step_province_taxes(b, rules(), ProvinceId(p), 100 * p);
    }
    CHECK(a.compute_hash() == b.compute_hash());
    CHECK(a.economy() == b.economy());
    // and the postings really changed the state
    CHECK(a.economy().accounts.treasury(1) > 0);
}

// ---- the system -------------------------------------------------------------------------------------

TEST_CASE("economy.provinces: taxes are posted once per province per month, in months of any length") {
    constexpr std::uint32_t kProvinces = 61;  // not a multiple of 28: the slots are not even
    // Julian leap years 1704, 1708 and the century year 1800: months of 28, 29, 30 and 31 days
    for (const int first_year : {1703, 1799}) {
        World start = make_world(kProvinces, 3000, 500, Date::from_ymd(first_year - 1, 12, 31));
        for (std::uint32_t p = 0; p < kProvinces; ++p) {
            auto columns = start.economy_columns_mut();
            columns.corruption[p] = Fixed32::from_raw(static_cast<std::int32_t>(p) * 700);
            columns.control[p] = points(10 + static_cast<std::int64_t>(p) * 3 % 91);  // 10..100: turmoil too
            columns.tax_policy[p] = static_cast<std::uint8_t>(p % 4);
            if (p % 5 == 0) {
                columns.kind[p] = static_cast<std::uint8_t>(ProvinceKind::City);
            }
            if (p % 7 == 0) {
                (void)start.set_province_owner_kind(ProvinceId(p), OwnerKind::Crown);
            }
            if (p % 11 == 0) {
                (void)start.set_province_owner_kind(ProvinceId(p), OwnerKind::Unowned);
            }
        }
        require_valid(start);

        // the model: for every month, for every province, the steps 2, 3 and 4 exactly once
        // (only the provinces system runs: the month is never closed, so the open lines of the statement
        // accumulate the postings of the whole run and the model below can be compared with them)
        World model = start;
        Simulation sim(start);
        sim.add_system(make_provinces_system(rules()));
        const StaticPriceSource prices(rules().production);
        const int last_year = first_year == 1703 ? 1712 : 1800;
        std::array<int, 32> lengths{};
        for (int year = first_year; year <= last_year; ++year) {
            for (int month = 1; month <= 12; ++month) {
                const int days = Date::days_in_month(year, month);
                ++lengths[static_cast<std::size_t>(days)];
                run_months(sim, 1);
                REQUIRE(sim.world().today() == Date::from_ymd(year, month, days));
                for (std::uint32_t p = 0; p < kProvinces; ++p) {
                    (void)step_province_population(model, rules().population, ProvinceId(p));
                    const ProvinceProduction production =
                        step_province_production(model, rules(), prices, ProvinceId(p));
                    (void)step_province_taxes(model, rules(), ProvinceId(p), production.holding_income_kop);
                }
                INFO("year " << year << " month " << month << " (" << days << " days)");
                REQUIRE(sim.world().economy().accounts == model.economy().accounts);
                REQUIRE(sim.world().economy().provinces.income_last_month ==
                        model.economy().provinces.income_last_month);
                REQUIRE(sim.world().economy().groups == model.economy().groups);
            }
        }
        CHECK(lengths[28] > 0);
        CHECK(lengths[29] > 0);
        CHECK(lengths[30] > 0);
        CHECK(lengths[31] > 0);
        require_valid(sim.world());
    }
}

TEST_CASE("economy.provinces: the postings of a month are the sum of the taxes of the provinces") {
    constexpr std::uint32_t kProvinces = 56;                                       // two full rounds of slots
    World start = make_world(kProvinces, 4000, 800, Date::from_ymd(1704, 1, 31));  // February of a leap year
    Pcg32 rng(77, 3);
    for (std::uint32_t p = 0; p < kProvinces; ++p) {
        auto columns = start.economy_columns_mut();
        columns.corruption[p] = Fixed32::from_raw(rng.range(0, Fixed32::one_raw / 2));
        columns.control[p] = Fixed32::from_raw(rng.range(0, 100 * Fixed32::one_raw));
        columns.tax_policy[p] = static_cast<std::uint8_t>(rng.bounded(4));
        columns.wealth[group_row(p, Group::Peasants)] =
            Fixed32::from_raw(rng.range(0, 200 * Fixed32::one_raw));
        if (p % 3 == 0) {
            columns.kind[p] = static_cast<std::uint8_t>(ProvinceKind::City);
        }
        if (p % 4 == 1) {
            (void)start.set_province_owner_kind(ProvinceId(p), OwnerKind::Crown);
        }
        if (p % 9 == 2) {
            (void)start.set_province_owner_kind(ProvinceId(p), OwnerKind::Church);
        }
    }
    // no growth or hunger noise: the wealth and sizes the taxes see are those of the model below
    Simulation sim = economy_sim(start);
    World model = start;
    const StaticPriceSource prices(rules().production);

    Kopecks expected_net = 0;
    Kopecks expected_holding = 0;
    std::vector<Kopecks> expected_income(kProvinces, 0);
    for (std::uint32_t p = 0; p < kProvinces; ++p) {
        (void)step_province_population(model, rules().population, ProvinceId(p));
        const ProvinceProduction production = step_province_production(model, rules(), prices, ProvinceId(p));
        const ProvinceIncome income =
            compute_province_income(rules(), tax_input(model, ProvinceId(p)), production.holding_income_kop);
        if (income_recipient(model, ProvinceId(p))) {
            expected_net += income.net_total();
            expected_holding += income.holding;
            expected_income[p] = income.owner_total();
        }
    }
    run_months(sim, 1);  // February 1704, 29 days
    REQUIRE(sim.world().today() == Date::from_ymd(1704, 2, 29));

    Kopecks taxes = 0;
    for (const LedgerCode code : {LedgerCode::TaxNobility,
                                  LedgerCode::TaxClergy,
                                  LedgerCode::TaxTownsfolk,
                                  LedgerCode::TaxPeasants,
                                  LedgerCode::TaxCossacks,
                                  LedgerCode::TaxNatives}) {
        taxes += line_everywhere(sim.world(), code);
    }
    CHECK(expected_net > 0);
    CHECK(taxes + line_everywhere(sim.world(), LedgerCode::Embezzlement) == expected_net);
    CHECK(line_everywhere(sim.world(), LedgerCode::HoldingIncome) == expected_holding);
    // every kopeck on the accounts is on a line: the money came from nowhere else
    Kopecks treasuries = 0;
    for (std::size_t row = 0; row < sim.world().economy().accounts.size(); ++row) {
        treasuries += sim.world().economy().accounts.treasury(row);
    }
    CHECK(treasuries == expected_net + expected_holding);
    for (std::uint32_t p = 0; p < kProvinces; ++p) {
        INFO("province " << p);
        CHECK(sim.world().economy().provinces.income_last_month[p] == expected_income[p]);
    }
    require_valid(sim.world());
}

TEST_CASE("economy.provinces: raising the tax policy of a province raises what its owner gets") {
    Kopecks previous = -1;
    for (std::uint8_t level = 0; level < rules().tax.level_count; ++level) {
        World world = make_world(1, 8000, 1500, Date::from_ymd(1702, 12, 31));
        world.economy_columns_mut().tax_policy[0] = level;
        Simulation sim = economy_sim(std::move(world));
        run_months(sim, 1);
        const Kopecks owner = sim.world().treasury(AccountRef::of(DynastyId(0)));
        INFO("level " << static_cast<int>(level));
        CHECK(owner > previous);
        CHECK(sim.world().economy().provinces.income_last_month[0] == owner);
        previous = owner;
    }
}

TEST_CASE("economy.provinces: the same world with taxes runs to the same hash and survives a save") {
    World world = make_world(40, 2000, 300);
    for (std::uint32_t p = 0; p < 40; ++p) {
        world.economy_columns_mut().corruption[p] = Fixed32::from_raw(static_cast<std::int32_t>(p) * 900);
        if (p % 6 == 0) {
            world.economy_columns_mut().kind[p] = static_cast<std::uint8_t>(ProvinceKind::City);
        }
    }
    Simulation a = economy_sim(world);
    Simulation b = economy_sim(world);
    a.run_days(400);
    b.run_days(400);
    CHECK(a.world().compute_hash() == b.world().compute_hash());
    CHECK(a.world().economy() == b.world().economy());
    CHECK(a.world().economy().accounts.treasury(1) > 0);
    CHECK(a.world().economy().accounts.treasury(0) > 0);  // the cities paid the crown
    require_valid(a.world());
}
