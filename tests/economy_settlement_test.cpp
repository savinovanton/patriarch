#include "pat/sim/calendar.hpp"
#include "pat/sim/economy/forecast.hpp"
#include "pat/sim/economy/ledger.hpp"
#include "pat/sim/economy/population.hpp"
#include "pat/sim/economy/production.hpp"
#include "pat/sim/economy/rules.hpp"
#include "pat/sim/economy/settlement.hpp"
#include "pat/sim/economy/systems.hpp"
#include "pat/sim/economy/tables.hpp"
#include "pat/sim/economy/taxes.hpp"
#include "pat/sim/save.hpp"
#include "pat/sim/simulation.hpp"
#include "pat/sim/world.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

// E9-T09: closing of the month, flat expenses (upkeep of the buildings, ExpenseSource), the order of
// the first of the month, the books over fifty years, the forecast and the affordability rule
// (docs/dev/economy-model.md sections 6.3, 7.1, 12 and D12).

using namespace pat::sim;
using namespace pat::sim::economy;
using pat::core::Fixed32;
using pat::core::StringId;

namespace {

constexpr std::uint8_t kMill = 1;  // the building of the default rules: 30 roubles a month

const EconomyRules& rules() {
    static const EconomyRules value = EconomyRules::defaults();
    return value;
}

void require_valid(const World& world) {
    const auto problems = world.check_invariants();
    for (const auto& problem : problems) {
        MESSAGE(problem);
    }
    REQUIRE(problems.empty());
}

Date first_of(int year, int month) {
    return Date::from_ymd(year, month, 1);
}

/// `provinces` provinces; the owner of province `p` is dynasty `p % 3` (three dynasties); economy on,
/// the world starts on 31 December 1702, so the first tick is 1 January 1703. `crown_holder` is the
/// dynasty of the "one treasury" mode of Q7 (invalid: a separate account for the crown).
World make_world(std::uint32_t provinces,
                 Date start = Date::from_ymd(1702, 12, 31),
                 DynastyId crown_holder = DynastyId()) {
    World world(start, 5);
    for (std::uint32_t d = 0; d < 3; ++d) {
        (void)world.create_dynasty(StringId(1 + d));
    }
    for (std::uint32_t p = 0; p < provinces; ++p) {
        (void)world.create_province(StringId(100 + p), DynastyId(p % 3), Fixed32::from_int(1));
    }
    REQUIRE(world.enable_economy(crown_holder));
    for (std::uint32_t p = 0; p < provinces; ++p) {
        (void)world.set_group_size(ProvinceId(p), Group::Peasants, 1000);
        (void)world.set_group_size(ProvinceId(p), Group::Townsfolk, 200);
        world.economy_columns_mut().tax_policy[p] = static_cast<std::uint8_t>(rules().tax.default_level);
    }
    require_valid(world);
    return world;
}

/// Gives province `p` `slots` slots with the mill in each.
void build_mills(World& world, std::uint32_t p, std::size_t slots) {
    world.economy_columns_mut().slots_total[p] = static_cast<std::uint8_t>(slots);
    for (std::size_t s = 0; s < slots; ++s) {
        REQUIRE(world.set_slot_building(ProvinceId(p), s, kMill));
    }
}

std::size_t row_of(const World& world, AccountRef account) {
    const auto row = world.resolve_account(account);
    REQUIRE(row.has_value());
    return *row;
}

Kopecks open_line(const World& world, AccountRef account, LedgerCode code) {
    return world.economy().accounts.open_line(row_of(world, account), code);
}

Kopecks closed_line(const World& world, AccountRef account, LedgerCode code, std::size_t month = 0) {
    const auto index = ledger_index(code);
    REQUIRE(index.has_value());
    return world.economy().accounts.ring_lines(row_of(world, account), month)[*index];
}

/// A source with fixed amounts that remembers whom it was asked about.
class FixedExpenses final : public ExpenseSource {
public:
    explicit FixedExpenses(std::vector<std::pair<std::size_t, ExpenseLine>> lines = {})
        : lines_(std::move(lines)) {}

    void collect(const World& world,
                 AccountRef account,
                 Date month_start,
                 std::vector<ExpenseLine>& out) const override {
        asked.push_back({account, month_start});
        const std::size_t row = account.kind == AccountKind::Crown ? 0 : 1 + account.dynasty.index();
        for (const auto& [line_row, line] : lines_) {
            if (line_row == row && world.economy_present()) {
                out.push_back(line);
            }
        }
    }

    mutable std::vector<std::pair<AccountRef, Date>> asked;

private:
    std::vector<std::pair<std::size_t, ExpenseLine>> lines_;
};

}  // namespace

// ---- the upkeep of the buildings ---------------------------------------------------------------------

TEST_CASE("settlement: the upkeep table knows the buildings of the rules and nothing else") {
    const UpkeepTable table = make_upkeep_table(rules().buildings);
    CHECK(table[0] == 0);  // "no building"
    CHECK(table[kMill] == rules().buildings.defs[0].upkeep_kop_month);
    CHECK(table[kMill] > 0);
    CHECK(table[200] == 0);  // an id the rules do not know
    BuildingRules empty;
    CHECK(make_upkeep_table(empty) == UpkeepTable{});
    // negative or absurd upkeep of broken data is clamped, not trusted
    BuildingRules broken;
    broken.count = 2;
    broken.defs[0].slot = 7;
    broken.defs[0].upkeep_kop_month = -5;
    broken.defs[1].slot = 9;
    broken.defs[1].upkeep_kop_month = kMaxPostingKop * 4;
    const UpkeepTable clamped = make_upkeep_table(broken);
    CHECK(clamped[7] == 0);
    CHECK(clamped[9] == kMaxPostingKop);
}

TEST_CASE("settlement: a province pays for the buildings in its open slots only") {
    World world = make_world(2);
    const UpkeepTable table = make_upkeep_table(rules().buildings);
    CHECK(province_building_upkeep(world, table, ProvinceId(0)) == 0);
    build_mills(world, 0, 3);
    CHECK(province_building_upkeep(world, table, ProvinceId(0)) == 3 * table[kMill]);
    // a building left in a slot that does not exist any more is not paid for
    world.economy_columns_mut().slots_total[0] = 2;
    CHECK(province_building_upkeep(world, table, ProvinceId(0)) == 2 * table[kMill]);
    CHECK(province_building_upkeep(world, table, ProvinceId(77)) == 0);  // unknown province
    const World plain(Date::from_ymd(1703, 1, 1), 1);
    CHECK(province_building_upkeep(plain, table, ProvinceId(0)) == 0);
}

TEST_CASE("settlement: the buildings are paid by whoever gets the income of the province") {
    World world = make_world(6);
    for (std::uint32_t p = 0; p < 6; ++p) {
        build_mills(world, p, 1);
    }
    REQUIRE(world.set_province_owner_kind(ProvinceId(3), OwnerKind::Crown));
    REQUIRE(world.set_province_owner_kind(ProvinceId(4), OwnerKind::Church));
    REQUIRE(world.set_province_owner_kind(ProvinceId(5), OwnerKind::Unowned));
    const Kopecks mill = make_upkeep_table(rules().buildings)[kMill];
    const std::vector<Kopecks> by_account = building_upkeep_by_account(world, rules().buildings);
    REQUIRE(by_account.size() == 4);
    CHECK(by_account[0] == mill);  // the crown: province 3
    CHECK(by_account[1] == mill);  // dynasty 0: province 0
    CHECK(by_account[2] == mill);  // dynasty 1: province 1
    CHECK(by_account[3] == mill);  // dynasty 2: province 2
    // Church, free and unowned provinces pay nobody in M1

    const World plain(Date::from_ymd(1703, 1, 1), 1);
    CHECK(building_upkeep_by_account(plain, rules().buildings).empty());
}

TEST_CASE("settlement: in the one-treasury mode the crown's buildings are paid from the holder's row") {
    World world = make_world(3, Date::from_ymd(1702, 12, 31), DynastyId(1));
    build_mills(world, 0, 2);
    REQUIRE(world.set_province_owner_kind(ProvinceId(0), OwnerKind::Crown));
    const Kopecks mill = make_upkeep_table(rules().buildings)[kMill];
    const std::vector<Kopecks> by_account = building_upkeep_by_account(world, rules().buildings);
    CHECK(by_account[0] == 0);  // row 0 is dead
    CHECK(by_account[2] == 2 * mill);
}

// ---- the closing of the month -----------------------------------------------------------------------

TEST_CASE("settlement: the flat expenses belong to the month that closes and the ring rotates") {
    World world = make_world(3);
    build_mills(world, 0, 2);  // dynasty 0
    build_mills(world, 1, 1);  // dynasty 1
    const Kopecks mill = make_upkeep_table(rules().buildings)[kMill];
    const AccountRef d0 = AccountRef::of(DynastyId(0));
    const AccountRef d1 = AccountRef::of(DynastyId(1));
    (void)world.post_money(d0, LedgerCode::Adjustment, 1'000'000);
    (void)world.post_money(d1, LedgerCode::Adjustment, 1'000);  // less than the upkeep it must pay

    const SettlementReport report = settle_month(world, rules(), {}, first_of(1703, 1));
    REQUIRE(report.closed);
    CHECK(report.closed_month_start == first_of(1702, 12));
    CHECK(report.building_upkeep == 3 * mill);
    CHECK(report.other_expenses == 0);

    // the closed month holds the opening deposit and the upkeep; the new month is clean
    CHECK(world.economy().meta.open_month_start == first_of(1703, 1));
    CHECK(world.economy().meta.closed_months == 1);
    CHECK(world.economy().accounts.ring_month_start(0) == first_of(1702, 12));
    CHECK(closed_line(world, d0, LedgerCode::BuildingUpkeep) == -2 * mill);
    CHECK(closed_line(world, d0, LedgerCode::Adjustment) == 1'000'000);
    CHECK(open_line(world, d0, LedgerCode::BuildingUpkeep) == 0);
    CHECK(world.treasury(d0) == 1'000'000 - 2 * mill);
    CHECK(world.economy().accounts.opening(row_of(world, d0)) == world.treasury(d0));
    // an expense is never refused: the treasury of dynasty 1 goes into debt
    CHECK(world.treasury(d1) == 1'000 - mill);
    CHECK(world.treasury(d1) < 0);
    CHECK(closed_line(world, d1, LedgerCode::BuildingUpkeep) == -mill);
    // the crown has no buildings and pays nothing
    CHECK(closed_line(world, AccountRef::crown(), LedgerCode::BuildingUpkeep) == 0);
    require_valid(world);
}

TEST_CASE("settlement: a month is closed once, and only on the first day of a later month") {
    World world = make_world(1);
    build_mills(world, 0, 1);
    CHECK_FALSE(settle_month(world, rules(), {}, Date::from_ymd(1703, 1, 2)).closed);  // not the first day
    CHECK_FALSE(settle_month(world, rules(), {}, first_of(1702, 12)).closed);  // the open month itself
    CHECK_FALSE(settle_month(world, rules(), {}, first_of(1702, 11)).closed);  // earlier
    CHECK(world.economy().meta.closed_months == 0);
    CHECK(world.treasury(AccountRef::of(DynastyId(0))) == 0);  // nothing was charged by the refusals

    REQUIRE(settle_month(world, rules(), {}, first_of(1703, 1)).closed);
    const Kopecks after_first = world.treasury(AccountRef::of(DynastyId(0)));
    CHECK_FALSE(settle_month(world, rules(), {}, first_of(1703, 1)).closed);  // again: nothing to do
    CHECK(world.treasury(AccountRef::of(DynastyId(0))) == after_first);
    CHECK(world.economy().meta.closed_months == 1);

    // a skipped month is skipped without back-charging: one month of expenses per call
    REQUIRE(settle_month(world, rules(), {}, first_of(1703, 4)).closed);
    CHECK(world.treasury(AccountRef::of(DynastyId(0))) ==
          after_first - make_upkeep_table(rules().buildings)[kMill]);

    World plain(Date::from_ymd(1703, 1, 1), 1);
    CHECK_FALSE(settle_month(plain, rules(), {}, first_of(1703, 2)).closed);  // no economy
}

TEST_CASE("settlement: the expense sources add their lines to the accounts they are asked about") {
    World world = make_world(3);
    const AccountRef d0 = AccountRef::of(DynastyId(0));
    const AccountRef d2 = AccountRef::of(DynastyId(2));
    // rows: 0 crown, 1 + d dynasty d. Lines in the "wrong" order, a zero and a negative amount.
    auto source = std::make_shared<FixedExpenses>(std::vector<std::pair<std::size_t, ExpenseLine>>{
        {1, {LedgerCode::ArmyUpkeep, 700}},
        {1, {LedgerCode::CourtUpkeep, 300}},
        {1, {LedgerCode::CourtUpkeep, 50}},
        {1, {LedgerCode::SalariesOut, 0}},
        {1, {LedgerCode::GiftsBribes, -9}},
        {3, {LedgerCode::ChurchDonations, 1'200}},
        {0, {LedgerCode::ArmyUpkeep, 40'000}},
    });
    const ExpenseSources sources{source};
    const SettlementReport report = settle_month(world, rules(), sources, first_of(1703, 1));
    REQUIRE(report.closed);
    CHECK(report.other_expenses == 700 + 300 + 50 + 1'200 + 40'000);
    CHECK(closed_line(world, d0, LedgerCode::ArmyUpkeep) == -700);
    CHECK(closed_line(world, d0, LedgerCode::CourtUpkeep) == -350);
    CHECK(closed_line(world, d0, LedgerCode::SalariesOut) == 0);
    CHECK(closed_line(world, d0, LedgerCode::GiftsBribes) == 0);
    CHECK(closed_line(world, d2, LedgerCode::ChurchDonations) == -1'200);
    CHECK(closed_line(world, AccountRef::crown(), LedgerCode::ArmyUpkeep) == -40'000);
    CHECK(world.treasury(AccountRef::crown()) == -40'000);

    // every account is asked once about the closing month, ascending by row
    REQUIRE(source->asked.size() == 4);
    CHECK(source->asked[0].first == AccountRef::crown());
    CHECK(source->asked[1].first == d0);
    CHECK(source->asked[3].first == d2);
    for (const auto& [who, month] : source->asked) {
        CHECK(month == first_of(1702, 12));
    }
    require_valid(world);
}

TEST_CASE("settlement: in the one-treasury mode the holder is asked as itself and as the crown") {
    World world = make_world(3, Date::from_ymd(1702, 12, 31), DynastyId(1));
    auto source = std::make_shared<FixedExpenses>(std::vector<std::pair<std::size_t, ExpenseLine>>{
        {2, {LedgerCode::CourtUpkeep, 500}},
    });
    REQUIRE(settle_month(world, rules(), ExpenseSources{source}, first_of(1703, 1)).closed);
    // row 0 is dead and not asked; the holder (row 2) is asked as dynasty 1 and then as the crown
    REQUIRE(source->asked.size() == 4);
    CHECK(source->asked[0].first == AccountRef::of(DynastyId(0)));
    CHECK(source->asked[1].first == AccountRef::of(DynastyId(1)));
    CHECK(source->asked[2].first == AccountRef::crown());
    CHECK(source->asked[3].first == AccountRef::of(DynastyId(2)));
    // both asks reached the holder's row (the fixed source answers for row 2 only when asked as d1)
    CHECK(world.treasury(AccountRef::crown()) == -500);
    CHECK(world.economy().accounts.treasury(0) == 0);
    require_valid(world);
}

// ---- the system and the first day of the month --------------------------------------------------------

TEST_CASE("settlement: the system is registered in front of the provinces and runs on the 1st") {
    Simulation sim(make_world(3));
    install_economy_systems(sim, rules());
    REQUIRE(sim.system_count() == 2);
    (void)sim.tick();  // 1 January 1703
    REQUIRE(sim.last_report().systems.size() == 2);
    CHECK(sim.last_report().systems[0].name == kSettlementSystemName);
    CHECK(sim.last_report().systems[0].cadence == Cadence::Monthly);
    CHECK(sim.last_report().systems[0].ran);
    CHECK(sim.last_report().systems[1].name == kProvincesSystemName);
    CHECK(sim.world().economy().meta.closed_months == 1);
    (void)sim.tick();  // 2 January: the monthly system does not run
    CHECK_FALSE(sim.last_report().systems[0].ran);
    CHECK(sim.world().economy().meta.closed_months == 1);

    // without the economy the system does nothing, however long it runs
    World plain(Date::from_ymd(1702, 12, 31), 5);
    (void)plain.create_dynasty(StringId(1));
    Simulation off(std::move(plain));
    install_economy_systems(off, rules());
    off.run_days(100);
    CHECK_FALSE(off.world().economy_present());
}

TEST_CASE(
    "settlement: on the 1st the month is closed first, then the slot of the day posts to the new month") {
    // 28 provinces of one dynasty: province 0 is the one of the slot of the 1st of the month
    for (const bool wrong_order : {false, true}) {
        INFO(std::string(wrong_order ? "provinces BEFORE settlement"
                                     : "settlement before provinces (the spec)"));
        World start = make_world(28, Date::from_ymd(1703, 1, 31));
        for (std::uint32_t p = 0; p < 28; ++p) {
            REQUIRE(start.set_province_owner_kind(ProvinceId(p), OwnerKind::Crown));
        }
        Simulation sim(start);
        if (wrong_order) {
            sim.add_system(make_provinces_system(rules()));
            sim.add_system(make_settlement_system(rules()));
        } else {
            install_economy_systems(sim, rules());
        }
        (void)sim.tick();  // 1 February 1703
        REQUIRE(sim.world().today() == first_of(1703, 2));
        const AccountRef crown = AccountRef::crown();
        const Kopecks closed = closed_line(sim.world(), crown, LedgerCode::TaxPeasants);
        const Kopecks open = open_line(sim.world(), crown, LedgerCode::TaxPeasants);
        // the tax of exactly one province, the one whose slot is the 1st
        World model = start;
        const StaticPriceSource prices(rules().production);
        (void)step_province_population(model, rules().population, ProvinceId(0));
        const ProvinceProduction production = step_province_production(model, rules(), prices, ProvinceId(0));
        const auto income = step_province_taxes(model, rules(), ProvinceId(0), production.holding_income_kop);
        const Kopecks one_province = income.group[index_of(Group::Peasants)].gross;
        REQUIRE(one_province > 0);
        if (wrong_order) {
            // the slot was posted into the month that the settlement then closed: the order matters
            CHECK(closed == one_province);
            CHECK(open == 0);
        } else {
            CHECK(closed == 0);
            CHECK(open == one_province);
        }
        require_valid(sim.world());
    }
}

TEST_CASE("settlement: every month of every length is closed exactly once, Julian leap years included") {
    // 1704 and 1708 are leap years, so is the century year 1800 (Julian calendar)
    for (const int first_year : {1703, 1799}) {
        const int last_year = first_year == 1703 ? 1712 : 1801;
        World start = make_world(5, Date::from_ymd(first_year - 1, 12, 31));
        build_mills(start, 0, 2);
        Simulation sim(start);
        install_economy_systems(sim, rules());
        std::array<int, 32> lengths{};
        int months = 0;
        for (int year = first_year; year <= last_year; ++year) {
            for (int month = 1; month <= 12; ++month) {
                INFO("year " << year << " month " << month);
                const int days = Date::days_in_month(year, month);
                ++lengths[static_cast<std::size_t>(days)];
                sim.run_days(1);  // the 1st of the month
                REQUIRE(sim.world().today() == first_of(year, month));
                REQUIRE(sim.world().economy().meta.open_month_start == first_of(year, month));
                ++months;
                REQUIRE(sim.world().economy().meta.closed_months == std::min(months, 12));
                const Date previous = month == 1 ? first_of(year - 1, 12) : first_of(year, month - 1);
                REQUIRE(sim.world().economy().accounts.ring_month_start(0) == previous);
                if (months >= 2) {
                    // the ring is a chain of consecutive months
                    const Date before = month >= 3   ? first_of(year, month - 2)
                                        : month == 2 ? first_of(year - 1, 12)
                                                     : first_of(year - 1, 11);
                    REQUIRE(sim.world().economy().accounts.ring_month_start(1) == before);
                }
                sim.run_days(days - 1);  // the last day of the month
                REQUIRE(sim.world().today() == Date::from_ymd(year, month, days));
                REQUIRE(sim.world().economy().meta.open_month_start == first_of(year, month));
            }
        }
        CHECK(lengths[28] > 0);
        CHECK(lengths[29] > 0);
        CHECK(lengths[30] > 0);
        CHECK(lengths[31] > 0);
        require_valid(sim.world());
    }
}

// ---- the books over fifty years ---------------------------------------------------------------------

namespace {

/// Court of the dynasties and the army of the crown: fixed sums, the same every month.
std::shared_ptr<FixedExpenses> standard_court() {
    return std::make_shared<FixedExpenses>(std::vector<std::pair<std::size_t, ExpenseLine>>{
        {0, {LedgerCode::ArmyUpkeep, 3'000}},
        {1, {LedgerCode::CourtUpkeep, 1'000}},
        {2, {LedgerCode::CourtUpkeep, 2'000}},
        {3, {LedgerCode::CourtUpkeep, 3'000}},
        {3, {LedgerCode::SalariesOut, 120}},
    });
}

}  // namespace

TEST_CASE("settlement: the treasury moves by exactly the lines of the statement, fifty years running") {
    constexpr std::uint32_t kProvinces = 61;
    World start = make_world(kProvinces);
    for (std::uint32_t p = 0; p < kProvinces; ++p) {
        if (p % 3 != 2) {
            build_mills(start, p, 1 + p % 4);
        }
        if (p % 9 == 0) {
            start.economy_columns_mut().kind[p] = static_cast<std::uint8_t>(ProvinceKind::City);
        }
        if (p % 13 == 0) {
            (void)start.set_province_owner_kind(ProvinceId(p), OwnerKind::Crown);
        }
    }
    for (std::size_t row = 0; row < start.economy().accounts.size(); ++row) {
        (void)start.post_money(row == 0 ? AccountRef::crown()
                                        : AccountRef::of(DynastyId(static_cast<std::uint32_t>(row - 1))),
                               LedgerCode::Adjustment,
                               10'000);
    }
    require_valid(start);
    const std::size_t accounts = start.economy().accounts.size();
    std::vector<Kopecks> start_treasury(accounts);
    for (std::size_t a = 0; a < accounts; ++a) {
        start_treasury[a] = start.economy().accounts.treasury(a);
    }

    const auto court = standard_court();
    const ExpenseSources sources{court};
    Simulation sim(start);
    install_economy_systems(sim, rules(), nullptr, sources);
    const std::vector<Kopecks> upkeep = building_upkeep_by_account(start, rules().buildings);
    const std::vector<Kopecks> source_cost{3'000, 1'000, 2'000, 3'120};

    std::vector<Kopecks> sum_of_closed_lines(accounts, 0);
    constexpr int kYears = 50;
    for (int month = 0; month < kYears * 12; ++month) {
        // the day before the first: remember what every account holds
        const Date today = sim.world().today();
        const Date next_first = today.first_of_next_month();
        sim.run_days(next_first - today - 1);
        std::vector<Kopecks> last_day(accounts);
        for (std::size_t a = 0; a < accounts; ++a) {
            last_day[a] = sim.world().economy().accounts.treasury(a);
        }
        sim.run_days(1);  // the 1st: the settlement closes the month, then the slot of the day posts
        REQUIRE(sim.world().today() == next_first);
        const AccountTable& t = sim.world().economy().accounts;
        for (std::size_t a = 0; a < accounts; ++a) {
            INFO("account " << a << ", month " << month);
            // the books of the closed month: closing == opening + sum of the lines (E-M2) ...
            const Kopecks closing = t.ring_opening(a, 0) + AccountTable::sum(t.ring_lines(a, 0));
            // ... and an independent oracle: what it held on the last day, minus the flat expenses
            REQUIRE(closing == last_day[a] - upkeep[a] - source_cost[a]);
            // the new month starts from the closing balance, with the slot of the 1st already posted
            REQUIRE(t.opening(a) == closing);
            REQUIRE(t.treasury(a) == t.opening(a) + AccountTable::sum(t.open_lines(a)));
            sum_of_closed_lines[a] += AccountTable::sum(t.ring_lines(a, 0));
        }
        require_valid(sim.world());  // E-M1 .. E-M4 every month
    }
    // the same thing over the whole run: the change of the treasury is the sum of all the lines
    const AccountTable& t = sim.world().economy().accounts;
    for (std::size_t a = 0; a < accounts; ++a) {
        INFO("account " << a);
        CHECK(t.treasury(a) - start_treasury[a] ==
              sum_of_closed_lines[a] - 10'000 /* the deposit sits in the first closed month */ +
                  AccountTable::sum(t.open_lines(a)));
        CHECK(t.treasury(a) != start_treasury[a]);
    }
    CHECK(sim.world().economy().meta.closed_months == 12);
    // the court and the buildings really cost something, and the taxes really came in
    CHECK(closed_line(sim.world(), AccountRef::of(DynastyId(2)), LedgerCode::CourtUpkeep) == -3'000);
    CHECK(closed_line(sim.world(), AccountRef::of(DynastyId(0)), LedgerCode::BuildingUpkeep) < 0);
    CHECK(closed_line(sim.world(), AccountRef::of(DynastyId(0)), LedgerCode::TaxPeasants) > 0);
}

TEST_CASE("settlement: the same world runs to the same hash and a save in the middle of a month resumes") {
    World start = make_world(40);
    for (std::uint32_t p = 0; p < 40; ++p) {
        build_mills(start, p, 1 + p % 3);
    }
    const ExpenseSources sources{standard_court()};
    Simulation a(start);
    install_economy_systems(a, rules(), nullptr, sources);
    Simulation b(start);
    install_economy_systems(b, rules(), nullptr, sources);
    a.run_days(400);
    b.run_days(400);
    CHECK(a.world().compute_hash() == b.world().compute_hash());
    CHECK(a.world().economy() == b.world().economy());

    // a save and a load in the middle of a month: the settlement keeps no state of its own
    Simulation c(start);
    install_economy_systems(c, rules(), nullptr, sources);
    c.run_days(200);
    auto loaded = deserialize_save(serialize_save(c.world(), c.pending_internal()));
    REQUIRE(loaded);
    Simulation d(start);
    install_economy_systems(d, rules(), nullptr, sources);
    d.restore_state(std::move(loaded.value().world), std::move(loaded.value().pending_internal));
    REQUIRE(d.world().compute_hash() == c.world().compute_hash());
    d.run_days(200);
    c.run_days(200);
    CHECK(d.world().compute_hash() == c.world().compute_hash());
    CHECK(d.world().economy() == c.world().economy());
}

// ---- affordability ------------------------------------------------------------------------------------

TEST_CASE("settlement: can_afford compares the treasury with the cost and never allows debt to spend") {
    World world = make_world(3);
    const AccountRef d0 = AccountRef::of(DynastyId(0));
    (void)world.post_money(d0, LedgerCode::Adjustment, 1'000);
    CHECK(can_afford(world, d0, 1'000));  // exactly enough
    CHECK(can_afford(world, d0, 999));
    CHECK_FALSE(can_afford(world, d0, 1'001));
    CHECK(can_afford(world, d0, 0));
    CHECK(can_afford(world, d0, -50));  // a refund is always fine
    const AccountRef d1 = AccountRef::of(DynastyId(1));
    CHECK(can_afford(world, d1, 0));
    CHECK_FALSE(can_afford(world, d1, 1));                     // nothing in the treasury
    (void)world.post_money(d1, LedgerCode::Adjustment, -300);  // debt
    CHECK_FALSE(can_afford(world, d1, 1));
    CHECK(can_afford(world, d1, 0));                                  // free things stay free
    CHECK_FALSE(can_afford(world, AccountRef::of(DynastyId(9)), 0));  // unknown account
    CHECK_FALSE(can_afford(world, AccountRef::of(DynastyId()), 0));
    const World plain(Date::from_ymd(1703, 1, 1), 1);
    CHECK_FALSE(can_afford(plain, AccountRef::crown(), 0));
    // in the one-treasury mode the crown is the holder's account
    World single = make_world(1, Date::from_ymd(1702, 12, 31), DynastyId(1));
    (void)single.post_money(AccountRef::crown(), LedgerCode::Adjustment, 70);
    CHECK(can_afford(single, AccountRef::crown(), 70));
    CHECK(can_afford(single, AccountRef::of(DynastyId(1)), 70));
    CHECK_FALSE(can_afford(single, AccountRef::of(DynastyId(0)), 1));
}

// ---- the forecast ---------------------------------------------------------------------------------------

namespace {

/// Rules under which the economy of a province stands still: nobody is born or dies, wealth does not
/// follow the harvest, so the month of the forecast is the month the simulation really gives.
EconomyRules steady_rules() {
    EconomyRules r = EconomyRules::defaults();
    r.population.birth_ppm_year.fill(0);
    r.population.death_ppm_year.fill(0);
    r.population.wealth_growth_ppm_per_point = 0;
    return r;
}

/// A world on which nothing changes between months under steady_rules(): grain for everybody, a city
/// among the provinces (the crown's share), crown, free and unowned provinces, buildings.
World steady_world(std::uint32_t provinces) {
    World world = make_world(provinces);
    for (std::uint32_t p = 0; p < provinces; ++p) {
        world.economy_columns_mut().capacity[p * kMaxGoods] = Fixed32::from_int(30'000);  // grain
        world.economy_columns_mut().capacity[p * kMaxGoods + 2] = Fixed32::from_int(40);  // fur
        world.economy_columns_mut().capacity[p * kMaxGoods + 7] = Fixed32::from_int(25);  // cloth
        world.economy_columns_mut().corruption[p] = Fixed32::from_raw(static_cast<std::int32_t>(p) * 900);
        world.economy_columns_mut().tax_policy[p] = static_cast<std::uint8_t>(p % 3);
        if (p % 4 == 0) {
            world.economy_columns_mut().kind[p] = static_cast<std::uint8_t>(ProvinceKind::City);
        }
        if (p % 5 == 0) {
            build_mills(world, p, 1 + p % 3);
        }
        if (p % 7 == 3) {
            (void)world.set_province_owner_kind(ProvinceId(p), OwnerKind::Crown);
        }
        if (p % 11 == 5) {
            (void)world.set_province_owner_kind(ProvinceId(p), OwnerKind::Church);
        }
    }
    require_valid(world);
    return world;
}

}  // namespace

TEST_CASE("forecast: one month of the forecast is the month the simulation gives, for every account") {
    const EconomyRules steady = steady_rules();
    World world = steady_world(56);
    const auto court = standard_court();
    const ExpenseSources sources{court};
    const StaticPriceSource prices(steady.production);

    std::vector<TreasuryForecast> forecasts;
    for (std::size_t row = 0; row < world.economy().accounts.size(); ++row) {
        forecasts.push_back(forecast_treasury(
            world,
            steady,
            prices,
            sources,
            row == 0 ? AccountRef::crown() : AccountRef::of(DynastyId(static_cast<std::uint32_t>(row - 1)))));
    }
    const World untouched = world;
    Simulation sim(std::move(world));
    install_economy_systems(sim, steady, nullptr, sources);
    sim.run_days(32);  // 31 December -> 1 February: January is closed
    REQUIRE(sim.world().today() == first_of(1703, 2));
    REQUIRE(sim.world().economy().accounts.ring_month_start(0) == first_of(1703, 1));
    // the wealth did not move (that is what makes the world steady)
    REQUIRE(sim.world().economy().groups.wealth == untouched.economy().groups.wealth);

    bool something_forecast = false;
    for (std::size_t row = 0; row < forecasts.size(); ++row) {
        const TreasuryForecast& f = forecasts[row];
        INFO("account " << row);
        for (std::size_t i = 0; i < kLedgerLineCount; ++i) {
            INFO("line " << static_cast<unsigned>(ledger_code_at(i)));
            // February's postings of the 1st are not in January: compare with the closed month
            CHECK(f.monthly_lines[i] == sim.world().economy().accounts.ring_lines(row, 0)[i]);
            something_forecast = something_forecast || f.monthly_lines[i] != 0;
        }
        CHECK(f.monthly_net == AccountTable::sum(sim.world().economy().accounts.ring_lines(row, 0)));
    }
    CHECK(something_forecast);
    // the crown gets the city share and pays the army; a dynasty pays its court and its mills
    CHECK(forecasts[0].monthly_lines[*ledger_index(LedgerCode::TaxTownsfolk)] > 0);
    CHECK(forecasts[0].monthly_lines[*ledger_index(LedgerCode::ArmyUpkeep)] == -3'000);
    CHECK(forecasts[1].monthly_lines[*ledger_index(LedgerCode::CourtUpkeep)] == -1'000);
    CHECK(forecasts[1].monthly_lines[*ledger_index(LedgerCode::BuildingUpkeep)] < 0);
    CHECK(forecasts[1].monthly_lines[*ledger_index(LedgerCode::Embezzlement)] < 0);
    CHECK(forecasts[1].monthly_lines[*ledger_index(LedgerCode::HoldingIncome)] > 0);
}

TEST_CASE("forecast: twelve balances that grow by the net of one month, within the bounds of an account") {
    const EconomyRules steady = steady_rules();
    World world = steady_world(30);
    const StaticPriceSource prices(steady.production);
    const AccountRef d0 = AccountRef::of(DynastyId(0));
    (void)world.post_money(d0, LedgerCode::Adjustment, 123'456);
    const TreasuryForecast f = forecast_treasury(world, steady, prices, {}, d0);
    CHECK(f.treasury == 123'456);
    CHECK(f.monthly_net != 0);
    for (std::size_t month = 0; month < kForecastMonths; ++month) {
        CHECK(f.balance[month] == 123'456 + static_cast<Kopecks>(month + 1) * f.monthly_net);
    }
    CHECK(kForecastMonths == 12);

    // a balance at the very limit does not overflow: it stays at the bound of an account
    World rich = steady_world(30);
    for (int i = 0; i < 100 && rich.treasury(d0) < kMaxTreasuryKop - kMaxPostingKop; ++i) {
        (void)rich.post_money(d0, LedgerCode::Adjustment, kMaxPostingKop);
    }
    const TreasuryForecast capped = forecast_treasury(rich, steady, prices, {}, d0);
    for (const Kopecks balance : capped.balance) {
        CHECK(balance <= kMaxTreasuryKop);
        CHECK(balance >= -kMaxTreasuryKop);
    }

    // an unknown account and a world without economy forecast nothing
    const TreasuryForecast unknown =
        forecast_treasury(world, steady, prices, {}, AccountRef::of(DynastyId(9)));
    CHECK(unknown == TreasuryForecast{});
    const World plain(Date::from_ymd(1703, 1, 1), 1);
    CHECK(forecast_treasury(plain, steady, prices, {}, AccountRef::crown()) == TreasuryForecast{});
}

TEST_CASE("forecast: asking changes nothing, and what is saved and hashed does not depend on it") {
    const EconomyRules steady = steady_rules();
    World world = steady_world(40);
    const StaticPriceSource prices(steady.production);
    const WorldHash before = world.compute_hash();
    const EconomyState state_before = world.economy();
    const World& view = world;  // the forecast takes a const World: it cannot write
    for (int i = 0; i < 3; ++i) {
        (void)forecast_treasury(view, steady, prices, {}, AccountRef::of(DynastyId(1)));
        (void)forecast_treasury(view, steady, prices, {}, AccountRef::crown());
    }
    CHECK(world.compute_hash() == before);
    CHECK(world.economy() == state_before);

    // the save of the world is the same bytes whether or not a forecast was ever computed
    const auto bytes_a = serialize_save(world);
    const World other = steady_world(40);
    const auto bytes_b = serialize_save(other);
    CHECK(bytes_a == bytes_b);
    // two equal worlds have equal forecasts, and the forecast of a loaded world is the same
    auto loaded = deserialize_save(bytes_a);
    REQUIRE(loaded);
    for (const AccountRef who :
         {AccountRef::crown(), AccountRef::of(DynastyId(0)), AccountRef::of(DynastyId(2))}) {
        CHECK(forecast_treasury(loaded.value().world, steady, prices, {}, who) ==
              forecast_treasury(world, steady, prices, {}, who));
    }
}

TEST_CASE("forecast: rates that rise move the forecast the same way the month would") {
    const EconomyRules steady = steady_rules();
    World world = steady_world(30);
    const StaticPriceSource prices(steady.production);
    // provinces of dynasty 0 are those with p % 3 == 0 that are not crown or church
    const AccountRef d0 = AccountRef::of(DynastyId(0));
    const Kopecks normal = forecast_treasury(world, steady, prices, {}, d0).monthly_net;
    for (std::uint32_t p = 0; p < 30; ++p) {
        world.economy_columns_mut().tax_policy[p] = static_cast<std::uint8_t>(steady.tax.level_count - 1);
    }
    const Kopecks heavy = forecast_treasury(world, steady, prices, {}, d0).monthly_net;
    CHECK(heavy > normal);
}
