// The economy -> top bar bridge (E9-T20, US-9.9): the numbers of game::make_hud equal the tables of the
// World.

#include "pat/game/hud_view.hpp"
#include "pat/game/world_view.hpp"
#include "pat/sim/economy/ledger.hpp"
#include "pat/sim/economy/production.hpp"
#include "pat/sim/economy/rules.hpp"
#include "pat/sim/economy/systems.hpp"
#include "pat/sim/economy/tables.hpp"
#include "pat/sim/simulation.hpp"
#include "pat/sim/world.hpp"

#include <doctest/doctest.h>

#include <cstdint>
#include <string>

namespace {

using namespace pat;
using sim::Date;
using sim::DynastyId;
using sim::ProvinceId;
using sim::World;
using sim::economy::AccountRef;
using sim::economy::Group;
using sim::economy::Kopecks;
using sim::economy::LedgerCode;

const sim::economy::EconomyRules& rules() {
    static const sim::economy::EconomyRules value = sim::economy::EconomyRules::defaults();
    return value;
}

/// Three dynasties and `provinces` provinces (owner of province `p` is dynasty `p % 3`, population
/// `(p + 1) * 7.5` thousand at the start), economy on, the world starts on 31 December 1702.
World make_world(std::uint32_t provinces) {
    World world(Date::from_ymd(1702, 12, 31), 5);
    for (std::uint32_t d = 0; d < 3; ++d) {
        (void)world.create_dynasty(core::StringId(1 + d));
    }
    for (std::uint32_t p = 0; p < provinces; ++p) {
        (void)world.create_province(core::StringId(100 + p), DynastyId(p % 3), core::Fixed32::from_int(1));
    }
    REQUIRE(world.enable_economy());
    for (std::uint32_t p = 0; p < provinces; ++p) {
        (void)world.set_group_size(ProvinceId(p), Group::Peasants, 7'000 * (p + 1));
        (void)world.set_group_size(ProvinceId(p), Group::Townsfolk, 500 * (p + 1));
        world.economy_columns_mut().tax_policy[p] = static_cast<std::uint8_t>(rules().tax.default_level);
    }
    return world;
}

}  // namespace

TEST_CASE("hud bridge: kopecks become whole roubles, truncated toward zero") {
    CHECK(game::kopecks_to_roubles(0) == 0);
    CHECK(game::kopecks_to_roubles(1'248'099) == 12'480);
    CHECK(game::kopecks_to_roubles(99) == 0);
    CHECK(game::kopecks_to_roubles(-50) == 0);
    CHECK(game::kopecks_to_roubles(-1'250) == -12);
    CHECK(game::kopecks_to_roubles(sim::economy::kMaxTreasuryKop) == 10'000'000'000'000);
}

TEST_CASE("hud bridge: the house in the genitive") {
    CHECK(game::clan_genitive("Шереметевы") == "Шереметевых");
    CHECK(game::clan_genitive("Голицыны") == "Голицыных");
    CHECK(game::clan_genitive("Одоевские") == "Одоевских");
    CHECK(game::clan_genitive("Стрешневы 2") == "Стрешневых 2");
    CHECK(game::clan_genitive("Без рода") == "Без рода");
}

TEST_CASE("hud bridge: a world without the economy keeps the placeholder HUD") {
    World plain(Date::from_ymd(1702, 12, 31), 5);
    const DynastyId d = plain.create_dynasty(core::StringId(1));
    (void)plain.create_province(core::StringId(100), d, core::Fixed32::from_int(94));
    REQUIRE_FALSE(plain.economy_present());
    const ui::HudSnapshot expected = ui::make_test_hud();
    const ui::HudSnapshot hud = game::make_hud(plain, d);
    CHECK(hud.house == expected.house);
    REQUIRE(hud.resources.size() == expected.resources.size());
    for (std::size_t i = 0; i < hud.resources.size(); ++i) {
        CHECK(hud.resources[i].key == expected.resources[i].key);
        CHECK(hud.resources[i].value == expected.resources[i].value);
        CHECK(hud.resources[i].delta == expected.resources[i].delta);
    }
    CHECK(hud.holdings.empty());

    // an economy world with an invalid or unknown dynasty is the placeholder as well
    const World world = make_world(3);
    CHECK(game::make_hud(world, DynastyId()).house == expected.house);
    CHECK(game::make_hud(world, DynastyId(99)).house == expected.house);
}

TEST_CASE("hud bridge: treasury is the balance in roubles, the delta is the last closed month") {
    World world = make_world(6);
    const AccountRef d1 = AccountRef::of(DynastyId(1));
    const AccountRef d2 = AccountRef::of(DynastyId(2));

    // before the first close there is no month to compare: the delta is 0
    (void)world.post_money(d1, LedgerCode::Adjustment, 1'000'000);
    ui::HudSnapshot hud = game::make_hud(world, DynastyId(1));
    CHECK(hud.resources[0].key == "treasury");
    CHECK(hud.resources[0].value == 10'000);
    CHECK(hud.resources[0].delta == 0);

    // a month with +12 480.99 net (the deposit included) closes; a posting of the open month changes the
    // balance only
    (void)world.post_money(d1, LedgerCode::TaxPeasants, 300'099);
    (void)world.post_money(d1, LedgerCode::BuildingUpkeep, -52'000);
    (void)world.post_money(d2, LedgerCode::Adjustment, 5'000);
    (void)world.post_money(d2, LedgerCode::CourtUpkeep, -85'000);  // debt
    REQUIRE(world.close_ledger_month(Date::from_ymd(1703, 1, 1)));
    (void)world.post_money(d1, LedgerCode::HoldingIncome, 4'321);

    hud = game::make_hud(world, DynastyId(1));
    const sim::economy::AccountTable& accounts = world.economy().accounts;
    const std::size_t row1 = *world.resolve_account(d1);
    CHECK(hud.resources[0].value == accounts.treasury(row1) / 100);
    CHECK(hud.resources[0].value == 12'524);  // 12 480.99 + 43.21 = 12 524.20
    CHECK(hud.resources[0].delta == sim::economy::AccountTable::sum(accounts.ring_lines(row1, 0)) / 100);
    CHECK(hud.resources[0].delta == 12'480);

    // a negative balance and a negative month
    hud = game::make_hud(world, DynastyId(2));
    const std::size_t row2 = *world.resolve_account(d2);
    CHECK(hud.resources[0].value == accounts.treasury(row2) / 100);
    CHECK(hud.resources[0].value == -800);
    CHECK(hud.resources[0].delta == -800);
}

TEST_CASE("hud bridge: holdings are the provinces of the player with the population of the table") {
    World world = make_world(7);
    const DynastyId player(1);
    game::HudOptions options;
    options.province_name = [](ProvinceId province) { return "P" + std::to_string(province.index()); };
    const ui::HudSnapshot hud = game::make_hud(world, player, options);
    CHECK(hud.house == game::clan_genitive(game::dynasty_name(player)));

    const auto owned = world.provinces_of(player);
    REQUIRE(hud.holdings.size() == owned.size());
    REQUIRE(owned.size() == 2);  // provinces 1 and 4
    for (std::size_t i = 0; i < owned.size(); ++i) {
        CHECK(hud.holdings[i].id == owned[i].index());
        CHECK(hud.holdings[i].name == "P" + std::to_string(owned[i].index()));
        CHECK(hud.holdings[i].population == world.provinces().population[owned[i].index()].to_int());
    }
    // 7 000 + 500 people per step: province 1 has 15 000, province 4 has 37 500 -> 15 and 37 thousand
    CHECK(hud.holdings[0].population == 15);
    CHECK(hud.holdings[1].population == 37);

    // no names: empty, the window falls back to the number
    CHECK(game::make_hud(world, player).holdings[0].name.empty());
}

TEST_CASE("hud bridge: grain is the sum of the granaries of the player, the delta follows the rules") {
    World world = make_world(6);
    const DynastyId player(0);  // provinces 0 and 3
    auto columns = world.economy_columns_mut();
    columns.grain_stock[0] = core::Fixed32::from_int(120);
    columns.grain_stock[3] =
        core::Fixed32::from_int(30) + core::Fixed32::from_raw(core::Fixed32::one_raw / 2);
    columns.grain_stock[1] = core::Fixed32::from_int(999);  // somebody else's granary

    ui::HudSnapshot hud = game::make_hud(world, player);
    CHECK(hud.grain.stock == 150);  // 150.5 units, truncated
    CHECK(hud.grain.delta == 0);    // no rules: no delta

    // With the rules the delta is what one month of the simulation functions does to the granaries.
    game::HudOptions options;
    options.rules = &rules();
    hud = game::make_hud(world, player, options);
    World month = world;
    const sim::economy::StaticPriceSource prices(rules().production);
    std::int64_t before = 0;
    std::int64_t after = 0;
    for (const ProvinceId province : world.provinces_of(player)) {
        before += world.economy().provinces.grain_stock[province.index()].raw();
        (void)sim::economy::step_province_production(month, rules(), prices, province);
        after += month.economy().provinces.grain_stock[province.index()].raw();
    }
    CHECK(after != before);  // the scenario really moves the granaries
    CHECK(hud.grain.delta == (after - before) / sim::economy::kQuantityOne);
    CHECK(hud.grain.stock == 150);

    // somebody else's granary does not count
    CHECK(game::make_hud(world, DynastyId(1), options).grain.stock == 999);
}

TEST_CASE("hud bridge: militia is the reserve of recruits out of the reserve plus the pools") {
    World world = make_world(6);
    auto columns = world.economy_columns_mut();
    columns.manpower_reserve[2] = 1'240;
    columns.recruit_pool[2] = 300;  // province 2 belongs to dynasty 2
    columns.recruit_pool[5] = 60;   // so does province 5
    columns.recruit_pool[0] = 999;  // dynasty 0

    const ui::HudSnapshot hud = game::make_hud(world, DynastyId(2));
    const ui::HudResource& militia = hud.resources[4];
    CHECK(militia.key == "militia");
    CHECK(militia.value == 1'240);
    CHECK(militia.capacity == 1'600);
    CHECK(militia.note == ui::HudNote::Capacity);
    // the stubs of E7/E31 stay
    const ui::HudSnapshot stub = ui::make_test_hud();
    for (const std::size_t i : {1U, 2U, 3U}) {
        CHECK(hud.resources[i].key == stub.resources[i].key);
        CHECK(hud.resources[i].value == stub.resources[i].value);
    }
}

TEST_CASE("hud bridge: after months of the simulation the numbers still equal the tables and the world is "
          "untouched") {
    sim::Simulation simulation(make_world(9));
    sim::economy::install_economy_systems(simulation, rules());
    simulation.run_days(75);
    const World& world = simulation.world();
    REQUIRE(world.economy().meta.closed_months >= 2);
    const std::uint64_t hash = world.compute_hash().total;

    game::HudOptions options;
    options.rules = &rules();
    for (std::uint32_t d = 0; d < 3; ++d) {
        const AccountRef account = AccountRef::of(DynastyId(d));
        const std::size_t row = *world.resolve_account(account);
        const ui::HudSnapshot hud = game::make_hud(world, DynastyId(d), options);
        CHECK(hud.resources[0].value == world.treasury(account) / 100);
        CHECK(hud.resources[0].delta ==
              sim::economy::AccountTable::sum(world.economy().accounts.ring_lines(row, 0)) / 100);
        std::int64_t grain_raw = 0;
        for (const ProvinceId province : world.provinces_of(DynastyId(d))) {
            grain_raw += world.economy().provinces.grain_stock[province.index()].raw();
        }
        CHECK(hud.grain.stock == grain_raw / sim::economy::kQuantityOne);
        CHECK(hud.holdings.size() == world.provinces_of(DynastyId(d)).size());
        CHECK(hud.resources[4].value == world.economy().dynasties.manpower_reserve[d]);
    }
    CHECK(world.compute_hash().total == hash);
}
