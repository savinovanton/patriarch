#include "pat/game/economy_view.hpp"
#include "pat/game/tooltip_view.hpp"
#include "pat/sim/economy/forecast.hpp"
#include "pat/sim/economy/ledger.hpp"
#include "pat/sim/economy/loans.hpp"
#include "pat/sim/economy/money.hpp"
#include "pat/sim/economy/taxes.hpp"

#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "ui_loc_fixture.hpp"

namespace {

using namespace pat;
using namespace sim;
using namespace sim::economy;
using core::Fixed32;
using core::StringId;
using ui::EconomyNumber;
using ui::EconomyUnit;

/// Reads the rendered Russian number, ignoring grouping spaces and its unit suffix.
/// The result is in kopecks for money, tenths for decimals/percent, and whole people/counts.
std::int64_t shown_value(std::string text, EconomyUnit unit) {
    constexpr std::string_view kMinus = "\xE2\x88\x92";
    const auto minus = text.find(kMinus);
    if (minus != std::string::npos) {
        text.replace(minus, kMinus.size(), "-");
    }
    const bool negative = text.find('-') != std::string::npos;
    const int decimals = unit == EconomyUnit::Kopecks                                ? 2
                         : unit == EconomyUnit::People || unit == EconomyUnit::Count ? 0
                                                                                     : 1;
    const auto comma = text.find(',');
    std::int64_t whole = 0;
    std::int64_t fraction = 0;
    int fractional_digits = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c < '0' || c > '9') {
            continue;
        }
        if (comma != std::string::npos && i > comma) {
            if (fractional_digits < decimals) {
                fraction = fraction * 10 + c - '0';
                ++fractional_digits;
            }
        } else {
            whole = whole * 10 + c - '0';
        }
    }
    std::int64_t scale = 1;
    for (int i = 0; i < decimals; ++i) {
        scale *= 10;
    }
    while (fractional_digits++ < decimals) {
        fraction *= 10;
    }
    const auto result = whole * scale + fraction;
    return negative ? -result : result;
}

void check_explanation(const ui::Explanation& explanation, EconomyUnit unit) {
    REQUIRE_FALSE(explanation.value.empty());
    CHECK(explanation.label.find("[[") == std::string::npos);
    if (explanation.terms.empty()) {
        return;
    }
    std::int64_t sum = 0;
    for (const auto& term : explanation.terms) {
        check_explanation(term, unit);
        sum += shown_value(term.value, unit);
    }
    CHECK(sum == shown_value(explanation.value, unit));
}

void check_number(const EconomyNumber& value) {
    const auto& loc = test::ui_localizer();
    CHECK(value.explanation.label.find("[[") == std::string::npos);
    std::string canonical;
    switch (value.unit) {
        case EconomyUnit::Kopecks:
            canonical = game::format_breakdown_value(loc, Unit::Kopecks, value.value, false);
            break;
        case EconomyUnit::People:
            canonical = game::format_breakdown_value(loc, Unit::People, value.value, false);
            break;
        case EconomyUnit::Points:
            canonical = game::format_breakdown_value(loc, Unit::Points, value.value, false);
            break;
        case EconomyUnit::Pieces:
            canonical = game::format_breakdown_value(loc, Unit::Pieces, value.value, false);
            break;
        case EconomyUnit::Permille:
            canonical = game::format_breakdown_value(loc, Unit::Permille, value.value, false);
            break;
        case EconomyUnit::Ppm:
            canonical = game::format_breakdown_value(loc, Unit::Ppm, value.value, false);
            break;
        case EconomyUnit::Count:
            canonical = std::to_string(value.value);
            break;
    }
    CHECK(shown_value(value.explanation.value, value.unit) == shown_value(canonical, value.unit));
    check_explanation(value.explanation, value.unit);
}

World finance_world(DynastyId crown = DynastyId()) {
    World world(Date::from_ymd(1703, 1, 1), 17);
    (void)world.create_dynasty(StringId(1));
    (void)world.create_dynasty(StringId(2));
    REQUIRE(world.enable_economy(crown));
    return world;
}

void check_month(const ui::TreasuryMonthSnapshot& month) {
    REQUIRE(month.available);
    CHECK_FALSE(month.date.empty());
    check_number(month.opening);
    check_number(month.net);
    check_number(month.closing);
    REQUIRE(month.lines.size() == kLedgerLineCount);
    std::int64_t net = 0;
    for (std::size_t i = 0; i < month.lines.size(); ++i) {
        CHECK(month.lines[i].code == static_cast<std::uint16_t>(kLedgerCodes[i]));
        CHECK_FALSE(month.lines[i].name.empty());
        CHECK(month.lines[i].name.find("[[") == std::string::npos);
        check_number(month.lines[i].amount);
        net += month.lines[i].amount.value;
    }
    CHECK(month.net.value == net);
    CHECK(month.opening.value + net == month.closing.value);
}

void check_treasury(const ui::TreasurySnapshot& snapshot) {
    REQUIRE(snapshot.available);
    check_number(snapshot.treasury);
    check_month(snapshot.current);
    CHECK(snapshot.current.closing.value == snapshot.treasury.value);
    for (const auto& month : snapshot.history) {
        if (month.available) {
            check_month(month);
        }
    }
    check_number(snapshot.projected_monthly_net);
    std::int64_t projected_net = 0;
    REQUIRE(snapshot.projected_lines.size() == kLedgerLineCount);
    for (std::size_t i = 0; i < snapshot.projected_lines.size(); ++i) {
        CHECK(snapshot.projected_lines[i].code == static_cast<std::uint16_t>(kLedgerCodes[i]));
        check_number(snapshot.projected_lines[i].amount);
        projected_net += snapshot.projected_lines[i].amount.value;
    }
    CHECK(projected_net == snapshot.projected_monthly_net.value);
    for (const auto& balance : snapshot.projected_balances) {
        check_number(balance);
    }
}

Date next_month(Date current) {
    const auto ymd = current.ymd();
    return Date::from_ymd(ymd.year + (ymd.month == 12 ? 1 : 0), ymd.month == 12 ? 1 : ymd.month + 1, 1);
}

void advance_to(World& world, Date target) {
    while (world.today() < target) {
        (void)world.advance_day();
    }
}

class SmallCourtExpense final : public ExpenseSource {
public:
    void collect(const World&, AccountRef account, Date, std::vector<ExpenseLine>& out) const override {
        if (account == AccountRef::of(DynastyId(0))) {
            out.push_back({LedgerCode::CourtUpkeep, 10});
        }
    }
};

}  // namespace

TEST_CASE("economy snapshots: unknown entities and worlds without economy are unavailable") {
    const auto& loc = test::ui_localizer();
    const auto rules = EconomyRules::defaults();
    World world(Date::from_ymd(1703, 1, 1), 4);
    const auto dynasty = world.create_dynasty(StringId(1));
    const auto province = world.create_province(StringId(2), dynasty, Fixed32::one());
    CHECK_FALSE(game::province_economy_snapshot(loc, world, province, rules).available);
    CHECK_FALSE(game::treasury_snapshot(loc, world, AccountRef::of(dynasty), rules).available);
    REQUIRE(world.enable_economy());
    const auto before = world.compute_hash();
    CHECK_FALSE(game::province_economy_snapshot(loc, world, ProvinceId(900), rules).available);
    CHECK_FALSE(game::treasury_snapshot(loc, world, AccountRef::of(DynastyId(900)), rules).available);
    CHECK(world.compute_hash() == before);
}

TEST_CASE("economy snapshots: province state and its current-rate projection remain distinct") {
    const auto& loc = test::ui_localizer();
    auto rules = EconomyRules::defaults();
    rules.tax.head_tax_kop_year.fill(0);
    rules.production.base_price_kop.fill(0);
    World world = finance_world();
    REQUIRE(world.enable_dynasty_mode());
    const auto province = world.create_province(StringId(3), DynastyId(0), Fixed32::zero());
    CHECK(world.set_group_size(province, Group::Peasants, 1000) == 1000);
    CHECK(world.set_group_size(province, Group::Townsfolk, 300) == 300);
    world.set_province_development(province, 3);
    auto columns = world.economy_columns_mut();
    columns.slots_total[0] = 3;
    columns.control[0] = Fixed32::from_int(83);
    columns.corruption[0] = Fixed32::from_ratio(1, 4);
    columns.unrest[0] = Fixed32::from_int(44);
    columns.grain_stock[0] = Fixed32::from_ratio(11, 4);
    columns.income_last_month[0] = 12'345;
    columns.mood[index_of(Group::Peasants)] = Fixed32::from_int(-60);
    columns.wealth[index_of(Group::Peasants)] = Fixed32::from_int(71);
    REQUIRE(world.set_slot_building(province, 0, 1));
    CharacterInit init;
    init.dynasty = DynastyId(0);
    init.birth_date = Date::from_ymd(1680, 1, 1);
    const auto administrator = world.create_character(init);
    REQUIRE(world.set_province_administrator(province, administrator));
    game::EconomyViewOptions options;
    options.province_name = [](ProvinceId) { return "Имение из контента"; };
    options.dynasty_name = [](DynastyId) { return "Тестовый владелец"; };
    options.character_name = [](CharacterId) { return "Назначенный управитель"; };
    const auto before = world.compute_hash();
    const auto snapshot = game::province_economy_snapshot(loc, world, province, rules, options);
    REQUIRE(snapshot.available);
    CHECK(snapshot.id == province.value());
    CHECK(snapshot.name == "Имение из контента");
    CHECK(snapshot.owner == "Тестовый владелец");
    REQUIRE(snapshot.has_administrator);
    CHECK(snapshot.administrator_id == administrator.value());
    CHECK(snapshot.administrator == "Назначенный управитель");
    CHECK(snapshot.population.value == 1300);
    CHECK(snapshot.control.value == Fixed32::from_int(83).raw());
    CHECK(snapshot.corruption.unit == EconomyUnit::Permille);
    CHECK(snapshot.corruption.value == 250);
    CHECK(snapshot.unrest.value == Fixed32::from_int(44).raw());
    CHECK(snapshot.grain.value == Fixed32::from_ratio(11, 4).raw());
    CHECK(snapshot.slots.value == 3);
    CHECK(snapshot.empty_slots.value == 2);
    CHECK(snapshot.income_last_month.value == 12'345);
    CHECK(snapshot.projected_income.value == 0);
    CHECK(snapshot.projected_control.value != snapshot.control.value);
    REQUIRE(snapshot.groups.size() == kGroupCount);
    std::int64_t people = 0;
    bool saw_peasants = false;
    for (const auto& group : snapshot.groups) {
        people += group.population.value;
        check_number(group.population);
        check_number(group.mood);
        check_number(group.wealth);
        check_number(group.projected_mood);
        if (group.key == "peasants") {
            saw_peasants = true;
            CHECK(group.population.value == 1000);
            CHECK(group.mood.value == Fixed32::from_int(-60).raw());
            CHECK(group.projected_mood.value != group.mood.value);
            CHECK(group.wealth.value == Fixed32::from_int(71).raw());
        }
    }
    CHECK(saw_peasants);
    CHECK(people == snapshot.population.value);
    REQUIRE(snapshot.buildings.size() == 1);
    const std::array<const EconomyNumber*, 13> numbers{&snapshot.population,
                                                       &snapshot.control,
                                                       &snapshot.corruption,
                                                       &snapshot.unrest,
                                                       &snapshot.grain,
                                                       &snapshot.slots,
                                                       &snapshot.empty_slots,
                                                       &snapshot.income_last_month,
                                                       &snapshot.projected_control,
                                                       &snapshot.projected_corruption,
                                                       &snapshot.projected_unrest,
                                                       &snapshot.projected_income,
                                                       &snapshot.buildings.at(0).upkeep};
    CHECK(snapshot.buildings[0].slot == 0);
    CHECK(snapshot.buildings[0].key == "building.mill");
    CHECK(snapshot.buildings[0].name == loc.format("building.mill"));
    for (const auto* number : numbers) {
        check_number(*number);
    }
    CHECK(world.compute_hash() == before);
}

TEST_CASE("economy snapshots: treasury reconciles signed cents and twelve recorded months") {
    const auto& loc = test::ui_localizer();
    const auto rules = EconomyRules::defaults();
    World world = finance_world();
    const auto account = AccountRef::of(DynastyId(0));
    REQUIRE(world.set_initial_treasury(account, -500));
    for (int month = 0; month < 14; ++month) {
        REQUIRE(world.post_money(account, LedgerCode::GrantsIn, 125 + month) == 125 + month);
        REQUIRE(world.post_money(account, LedgerCode::CourtUpkeep, -200) == -200);
        const auto target = next_month(world.today());
        advance_to(world, target);
        REQUIRE(world.close_ledger_month(target));
    }
    REQUIRE(world.post_money(account, LedgerCode::GrantsIn, 1234) == 1234);
    REQUIRE(world.post_money(account, LedgerCode::CourtUpkeep, -2345) == -2345);
    game::EconomyViewOptions options;
    options.dynasty_name = [](DynastyId) { return "Казна из контента"; };
    const auto before = world.compute_hash();
    const auto snapshot = game::treasury_snapshot(loc, world, account, rules, options);
    REQUIRE(snapshot.available);
    CHECK(snapshot.owner == "Казна из контента");
    CHECK(snapshot.current.net.value == -1111);
    CHECK(snapshot.treasury.value == world.treasury(account));
    CHECK(snapshot.treasury.value < 0);
    const auto row = *world.resolve_account(account);
    for (std::size_t month = 0; month < snapshot.history.size(); ++month) {
        REQUIRE(snapshot.history[month].available);
        CHECK(snapshot.history[month].opening.value == world.economy().accounts.ring_opening(row, month));
        CHECK(snapshot.history[month].closing.value ==
              world.economy().accounts.ring_opening(row, month) +
                  AccountTable::sum(world.economy().accounts.ring_lines(row, month)));
        if (month > 0) {
            CHECK(snapshot.history[month - 1].opening.value == snapshot.history[month].closing.value);
        }
    }
    check_treasury(snapshot);
    CHECK(world.compute_hash() == before);
}

TEST_CASE("economy snapshots: forecast keeps fractional loan carry and the core forecast balances") {
    const auto& loc = test::ui_localizer();
    auto rules = EconomyRules::defaults();
    rules.loans.rate_ppm_year = 100'001;
    World world = finance_world();
    const auto account = AccountRef::of(DynastyId(0));
    REQUIRE(world.set_initial_treasury(account, 20'000));
    REQUIRE(world.set_initial_treasury(AccountRef::crown(), 100'000));
    REQUIRE(world.take_loan(DynastyId(0), 1279, rules.loans).has_value());
    StaticPriceSource prices(rules.production);
    const auto expected = forecast_treasury(world, rules, prices, {}, account);
    const auto before = world.compute_hash();
    const auto snapshot = game::treasury_snapshot(loc, world, account, rules);
    check_treasury(snapshot);
    CHECK(snapshot.projected_monthly_net.value == -10);
    CHECK(snapshot.projected_balances[0].value == world.treasury(account) - 10);
    CHECK(snapshot.projected_balances[1].value == world.treasury(account) - 21);
    CHECK(snapshot.projected_balances.back().value == world.treasury(account) - 127);
    for (std::size_t i = 0; i < snapshot.projected_balances.size(); ++i) {
        CHECK(snapshot.projected_balances[i].value == expected.balance[i]);
    }
    CHECK(snapshot.projected_monthly_net.value == expected.monthly_net);
    for (std::size_t i = 0; i < snapshot.projected_lines.size(); ++i) {
        CHECK(snapshot.projected_lines[i].amount.value == expected.monthly_lines[i]);
    }
    const auto crown = game::treasury_snapshot(loc, world, AccountRef::crown(), rules);
    check_treasury(crown);
    CHECK(crown.projected_monthly_net.value == 10);
    CHECK(crown.projected_balances.back().value == world.treasury(AccountRef::crown()) + 127);
    CHECK(world.compute_hash() == before);
}

TEST_CASE("economy snapshots: one treasury resolves the same account without changing state") {
    const auto& loc = test::ui_localizer();
    const auto rules = EconomyRules::defaults();
    World world = finance_world(DynastyId(0));
    REQUIRE(world.set_initial_treasury(AccountRef::crown(), 12345));
    REQUIRE(world.post_money(AccountRef::crown(), LedgerCode::GrantsIn, -5) == -5);
    const auto before = world.compute_hash();
    const auto crown = game::treasury_snapshot(loc, world, AccountRef::crown(), rules);
    const auto holder = game::treasury_snapshot(loc, world, AccountRef::of(DynastyId(0)), rules);
    check_treasury(crown);
    check_treasury(holder);
    CHECK(crown.treasury.value == 12340);
    CHECK(holder.treasury.value == crown.treasury.value);
    CHECK(holder.current.net.value == crown.current.net.value);
    for (const auto& month : crown.history) {
        CHECK_FALSE(month.available);
    }
    CHECK(world.compute_hash() == before);
}

TEST_CASE("economy snapshots: projected balance clamps keep an additive explanation") {
    const auto& loc = test::ui_localizer();
    const auto rules = EconomyRules::defaults();
    World world = finance_world();
    const auto account = AccountRef::of(DynastyId(0));
    REQUIRE(world.set_initial_treasury(account, -kMaxTreasuryKop + 5));
    const ExpenseSources expenses{std::make_shared<SmallCourtExpense>()};
    game::EconomyViewOptions options;
    options.expenses = &expenses;
    const auto before = world.compute_hash();
    const auto snapshot = game::treasury_snapshot(loc, world, account, rules, options);
    check_treasury(snapshot);
    CHECK(snapshot.projected_monthly_net.value == -10);
    for (const auto& balance : snapshot.projected_balances) {
        CHECK(balance.value == -kMaxTreasuryKop);
    }
    CHECK(world.compute_hash() == before);
}

TEST_CASE("economy snapshots: recorded signed cents and fractional projections have exact displayed sums") {
    const auto& loc = test::ui_localizer();
    const auto money = game::recorded_economy_value(loc, EconomyUnit::Kopecks, -5, "economy.tax.total");
    CHECK(money.value == -5);
    CHECK(money.explanation.value == "\xE2\x88\x92"
                                     "0,05 \xE2\x82\xBD");
    check_number(money);
    struct FractionCase {
        Unit unit;
        EconomyUnit display_unit;
        std::int64_t part;
    };
    const std::array cases{FractionCase{Unit::Points, EconomyUnit::Points, Fixed32::from_ratio(1, 25).raw()},
                           FractionCase{Unit::Pieces, EconomyUnit::Pieces, Fixed32::from_ratio(1, 25).raw()},
                           FractionCase{Unit::Ppm, EconomyUnit::Ppm, 400}};
    for (const auto& sample : cases) {
        Breakdown fractional;
        fractional.begin(term::kControlTarget, sample.unit, sample.part * 2);
        fractional.add_term(kRootNode, term::kControlBase, sample.part);
        fractional.add_term(kRootNode, term::kControlAdministrator, sample.part);
        REQUIRE(fractional.is_consistent());
        const auto value = game::projected_economy_value(loc, fractional, "economy.control.target");
        CHECK(value.value == fractional.node(kRootNode).value);
        CHECK(value.unit == sample.display_unit);
        CHECK(shown_value(value.explanation.value, sample.display_unit) == 1);
        REQUIRE(value.explanation.terms.size() == 3);
        CHECK(value.explanation.terms.back().label == loc.format("ui.explain.rounding"));
        CHECK(shown_value(value.explanation.terms.back().value, sample.display_unit) == 1);
        check_number(value);
    }
}
