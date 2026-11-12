#include "pat/sim/command.hpp"
#include "pat/sim/command_log.hpp"
#include "pat/sim/economy/breakdown.hpp"
#include "pat/sim/economy/forecast.hpp"
#include "pat/sim/economy/invariants.hpp"
#include "pat/sim/economy/ledger.hpp"
#include "pat/sim/economy/loans.hpp"
#include "pat/sim/economy/rules.hpp"
#include "pat/sim/economy/settlement.hpp"
#include "pat/sim/economy/systems.hpp"
#include "pat/sim/replay.hpp"
#include "pat/sim/save.hpp"
#include "pat/sim/simulation.hpp"
#include "pat/sim/world.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

// E9-T15: crown loans, paired money postings, exact monthly interest, overdue and bankruptcy.

using namespace pat::sim;
using namespace pat::sim::economy;
using pat::core::StringId;

namespace {

EconomyRules loan_rules() {
    EconomyRules rules = EconomyRules::defaults();
    rules.loans.base_limit_kop = 100'000;
    rules.loans.per_honor_kop = 100;
    rules.loans.rate_ppm_year = 120'000;
    rules.loans.term_months = 12;
    rules.loans.bankruptcy_threshold_kop = 500'000;
    rules.loans.bankruptcy_exit_kop = 250'000;
    rules.loans.max_loans_per_dynasty = 3;
    return rules;
}

World make_world(DynastyId crown_holder = DynastyId()) {
    World world(Date::from_ymd(1703, 1, 1), 7);
    for (std::uint32_t i = 0; i < 3; ++i) {
        (void)world.create_dynasty(StringId(1 + i));
    }
    REQUIRE(world.enable_economy(crown_holder));
    return world;
}

void require_valid(const World& world, const EconomyRules& rules) {
    const auto problems = world.check_invariants();
    for (const auto& problem : problems) {
        MESSAGE(problem);
    }
    REQUIRE(problems.empty());
    const auto rules_problems = check_rules_invariants(world, rules);
    for (const auto& problem : rules_problems) {
        MESSAGE(problem);
    }
    REQUIRE(rules_problems.empty());
}

Date next_month(Date date) {
    const auto ymd = date.ymd();
    return Date::from_ymd(ymd.year + (ymd.month == 12 ? 1 : 0), ymd.month == 12 ? 1 : ymd.month + 1, 1);
}

void close_next_month(World& world, const EconomyRules& rules) {
    const Date target = next_month(world.today());
    while (world.today() < target) {
        (void)world.advance_day();
    }
    REQUIRE(settle_month(world, rules, {}, target).closed);
}

Kopecks open_line(const World& world, AccountRef account, LedgerCode code) {
    const auto row = world.resolve_account(account);
    REQUIRE(row.has_value());
    return world.economy().accounts.open_line(*row, code);
}

Kopecks closed_line(const World& world, AccountRef account, LedgerCode code) {
    const auto row = world.resolve_account(account);
    const auto line = ledger_index(code);
    REQUIRE(row.has_value());
    REQUIRE(line.has_value());
    return world.economy().accounts.ring_lines(*row, 0)[*line];
}

std::size_t incidents(const World& world, IncidentKind kind) {
    const auto& kinds = world.economy().incidents.kind;
    return static_cast<std::size_t>(std::count(kinds.begin(), kinds.end(), static_cast<std::uint8_t>(kind)));
}

Command take(DynastyId borrower, Kopecks amount) {
    return Command{borrower, cmd::TakeLoan{amount}};
}

Command repay(DynastyId actor, std::uint32_t loan, Kopecks amount) {
    return Command{actor, cmd::RepayLoan{loan, amount}};
}

Rejection verdict(const World& world, const Command& command, const EconomyRules* rules) {
    const auto result = validate(world, command, nullptr, rules);
    return result.has_value() ? Rejection::None : result.error().reason;
}

Simulation make_simulation(World world, const EconomyRules& rules) {
    Simulation simulation(std::move(world));
    install_economy_systems(simulation, rules);
    return simulation;
}

}  // namespace

TEST_CASE("loans: the available limit includes honor and subtracts every outstanding principal") {
    EconomyRules rules = loan_rules();
    rules.loans.base_limit_kop = 1000;
    World world = make_world();
    const DynastyId borrower(0);
    world.add_honor(borrower, 5);

    Breakdown breakdown;
    CHECK(loan_limit(world, rules.loans, borrower, &breakdown) == 1500);
    CHECK(breakdown.total() == 1500);
    CHECK(breakdown.is_consistent());
    REQUIRE(world.take_loan(borrower, 700, rules.loans).has_value());
    CHECK(dynasty_debt(world, borrower) == 700);
    CHECK(loan_limit(world, rules.loans, borrower, &breakdown) == 800);
    CHECK(breakdown.total() == 800);
    CHECK(breakdown.is_consistent());

    world.add_honor(borrower, -20);
    CHECK(loan_limit(world, rules.loans, borrower, &breakdown) == 300);
    CHECK(breakdown.is_consistent());
    CHECK(loan_limit(world, rules.loans, DynastyId(99)) == 0);
    const World plain(Date::from_ymd(1703, 1, 1), 7);
    CHECK(loan_limit(plain, rules.loans, borrower) == 0);
    require_valid(world, rules);
}

TEST_CASE("loans: issuing and repaying use paired postings and preserve ordered stable ids") {
    const EconomyRules rules = loan_rules();
    World world = make_world();
    const DynastyId borrower(0);
    const auto first = world.take_loan(borrower, 1200, rules.loans);
    const auto second = world.take_loan(DynastyId(1), 300, rules.loans);
    const auto third = world.take_loan(borrower, 400, rules.loans);
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    REQUIRE(third.has_value());
    CHECK(*first < *second);
    CHECK(*second < *third);
    CHECK(world.treasury(AccountRef::of(borrower)) == 1600);
    CHECK(world.treasury(AccountRef::crown()) == -1900);
    CHECK(open_line(world, AccountRef::of(borrower), LedgerCode::LoanTaken) == 1600);
    CHECK(open_line(world, AccountRef::crown(), LedgerCode::LoanIssued) == -1900);

    REQUIRE(world.repay_loan(*first, 200, rules.loans).has_value());
    CHECK(dynasty_debt(world, borrower) == 1400);
    CHECK(open_line(world, AccountRef::of(borrower), LedgerCode::LoanRepaid) == -200);
    CHECK(open_line(world, AccountRef::crown(), LedgerCode::LoanCollected) == 200);
    REQUIRE(world.repay_loan(*first, 1000, rules.loans).has_value());
    CHECK_FALSE(find_loan(world, *first).has_value());
    REQUIRE(world.economy().loans.size() == 2);
    CHECK(world.economy().loans.loan_id[0] == *second);
    CHECK(world.economy().loans.loan_id[1] == *third);
    const auto fourth = world.take_loan(borrower, 50, rules.loans);
    REQUIRE(fourth.has_value());
    CHECK(*fourth > *third);
    CHECK(world.treasury(AccountRef::of(borrower)) + world.treasury(AccountRef::of(DynastyId(1))) +
              world.treasury(AccountRef::crown()) ==
          0);
    require_valid(world, rules);
}

TEST_CASE("loan commands: invalid amounts, authority, configuration and overpayment refuse atomically") {
    const EconomyRules rules = loan_rules();
    World world = make_world();
    const DynastyId borrower(0);
    const auto loan = world.take_loan(borrower, 1200, rules.loans);
    REQUIRE(loan.has_value());
    const auto before = world.compute_hash();
    const std::array commands = {take(borrower, 0),
                                 take(borrower, -1),
                                 take(borrower, rules.loans.base_limit_kop + 1),
                                 take(DynastyId(), 1),
                                 take(DynastyId(99), 1),
                                 repay(borrower, *loan, 0),
                                 repay(borrower, *loan, 1201),
                                 repay(DynastyId(1), *loan, 1),
                                 repay(borrower, std::numeric_limits<std::uint32_t>::max(), 1)};
    for (const Command& command : commands) {
        CHECK(verdict(world, command, &rules) != Rejection::None);
        CHECK(world.compute_hash() == before);
    }
    CHECK(verdict(world, take(borrower, 1), nullptr) == Rejection::InvalidState);
    CHECK(verdict(world, repay(borrower, *loan, 1), nullptr) == Rejection::InvalidState);
    CHECK(verdict(world, repay(DynastyId(1), *loan, 1), &rules) == Rejection::NotAuthorized);
    CHECK(verdict(world, repay(borrower, *loan, 1201), &rules) == Rejection::InvalidArgument);

    REQUIRE(world.post_money(AccountRef::of(borrower), LedgerCode::Adjustment, -1200) == -1200);
    const auto empty_hash = world.compute_hash();
    CHECK(verdict(world, repay(borrower, *loan, 1), &rules) == Rejection::InvalidState);
    CHECK_FALSE(world.repay_loan(*loan, 1, rules.loans).has_value());
    CHECK(world.compute_hash() == empty_hash);
    require_valid(world, rules);
}

TEST_CASE("loans: the count and free limit bound subsequent loans without partial transfers") {
    EconomyRules rules = loan_rules();
    rules.loans.base_limit_kop = 1000;
    rules.loans.max_loans_per_dynasty = 2;
    World world = make_world();
    const DynastyId borrower(0);
    const auto first = world.take_loan(borrower, 300, rules.loans);
    REQUIRE(first.has_value());
    const auto before = world.compute_hash();
    CHECK(check_take_loan(world, rules.loans, borrower, 701) == LoanStatus::LimitExceeded);
    CHECK_FALSE(world.take_loan(borrower, 701, rules.loans).has_value());
    CHECK(world.compute_hash() == before);
    REQUIRE(world.take_loan(borrower, 700, rules.loans).has_value());
    const auto full_hash = world.compute_hash();
    CHECK_FALSE(world.take_loan(borrower, 1, rules.loans).has_value());
    CHECK(world.compute_hash() == full_hash);
    REQUIRE(world.repay_loan(*first, 100, rules.loans).has_value());
    CHECK(loan_limit(world, rules.loans, borrower) == 100);
    CHECK(check_take_loan(world, rules.loans, borrower, 1) == LoanStatus::TooManyLoans);
    require_valid(world, rules);
}

TEST_CASE("loan interest: fractional annual rates retain twelfths and wide products stay exact") {
    const auto fraction = compute_loan_interest(1201, 100'000, 0);
    CHECK(fraction.amount_kop == 10);
    CHECK(fraction.carry == 100'000);
    const auto carried = compute_loan_interest(1201, 100'000, 11'900'000);
    CHECK(carried.amount_kop == 11);
    CHECK(carried.carry == 0);
    const auto wide = compute_loan_interest(kMaxPostingKop, 1'000'000, 11'999'999);
    CHECK(wide.amount_kop == 833'333'333'334);
    CHECK(wide.carry == 3'999'999);
}

TEST_CASE("loans: settlement posts interest in the closing month and penalizes overdue only once") {
    EconomyRules rules = loan_rules();
    rules.loans.term_months = 1;
    World world = make_world();
    const DynastyId borrower(0);
    world.add_honor(borrower, 100);
    const auto loan = world.take_loan(borrower, 1201, rules.loans);
    REQUIRE(loan.has_value());
    const auto row = find_loan(world, *loan);
    REQUIRE(row.has_value());
    CHECK(world.economy().loans.due[*row] == Date::from_ymd(1703, 2, 1));

    close_next_month(world, rules);
    CHECK(closed_line(world, AccountRef::of(borrower), LedgerCode::Interest) == -12);
    CHECK(closed_line(world, AccountRef::crown(), LedgerCode::InterestCollected) == 12);
    CHECK(open_line(world, AccountRef::of(borrower), LedgerCode::Interest) == 0);
    CHECK(world.economy().loans.interest_carry[*row] == 120'000);
    CHECK(world.economy().loans.overdue[*row] == 1);
    CHECK(world.dynasties().honor[borrower.index()] == 100 - rules.loans.overdue_penalty_honor);
    CHECK(incidents(world, IncidentKind::LoanOverdue) == 1);
    const auto settled_hash = world.compute_hash();
    CHECK_FALSE(settle_month(world, rules, {}, world.today()).closed);
    CHECK(world.compute_hash() == settled_hash);

    close_next_month(world, rules);
    CHECK(world.dynasties().honor[borrower.index()] == 100 - rules.loans.overdue_penalty_honor);
    CHECK(incidents(world, IncidentKind::LoanOverdue) == 1);
    CHECK(closed_line(world, AccountRef::of(borrower), LedgerCode::Interest) == -12);
    require_valid(world, rules);
}

TEST_CASE("loans: bankruptcy has hysteresis, one penalty per entry and never ends the simulation") {
    EconomyRules rules = loan_rules();
    rules.loans.bankruptcy_threshold_kop = 5000;
    rules.loans.bankruptcy_exit_kop = 2500;
    rules.loans.rate_ppm_year = 0;
    World world = make_world();
    const DynastyId borrower(0);
    world.add_honor(borrower, 1000);
    const auto loan = world.take_loan(borrower, 5000, rules.loans);
    REQUIRE(loan.has_value());
    CHECK(world.economy().dynasties.bankrupt[borrower.index()] == 1);
    CHECK(world.dynasties().honor[borrower.index()] == 1000 - rules.loans.bankruptcy_penalty_honor);
    CHECK(incidents(world, IncidentKind::Bankruptcy) == 1);
    CHECK(check_take_loan(world, rules.loans, borrower, 1) == LoanStatus::Bankrupt);

    close_next_month(world, rules);
    CHECK(incidents(world, IncidentKind::Bankruptcy) == 1);
    REQUIRE(world.repay_loan(*loan, 2500, rules.loans).has_value());
    CHECK(world.economy().dynasties.bankrupt[borrower.index()] == 1);
    REQUIRE(world.repay_loan(*loan, 1, rules.loans).has_value());
    CHECK(world.economy().dynasties.bankrupt[borrower.index()] == 0);
    REQUIRE(world.take_loan(borrower, 2501, rules.loans).has_value());
    CHECK(incidents(world, IncidentKind::Bankruptcy) == 2);
    CHECK(world.dynasties().honor[borrower.index()] == 1000 - 2 * rules.loans.bankruptcy_penalty_honor);

    Simulation simulation = make_simulation(make_world(), rules);
    simulation.restore_state(std::move(world), {});
    const Date before = simulation.world().today();
    simulation.run_days(3);
    CHECK(simulation.world().today() == before + 3);
    CHECK(simulation.world().economy().dynasties.bankrupt[borrower.index()] == 1);
    require_valid(simulation.world(), rules);
}

TEST_CASE("loans: one treasury cannot borrow from itself and saturation refuses the whole loan") {
    const EconomyRules rules = loan_rules();
    World combined = make_world(DynastyId(0));
    const auto before = combined.compute_hash();
    CHECK_FALSE(combined.take_loan(DynastyId(0), 1, rules.loans).has_value());
    CHECK(combined.compute_hash() == before);
    REQUIRE(combined.take_loan(DynastyId(1), 100, rules.loans).has_value());
    CHECK(combined.treasury(AccountRef::crown()) == -100);
    CHECK(combined.treasury(AccountRef::of(DynastyId(1))) == 100);
    require_valid(combined, rules);

    World full = make_world();
    const AccountRef account = AccountRef::of(DynastyId(0));
    for (int i = 0; i < 100; ++i) {
        REQUIRE(full.post_money(account, LedgerCode::Adjustment, kMaxPostingKop) == kMaxPostingKop);
    }
    REQUIRE(full.post_money(account, LedgerCode::Adjustment, -5) == -5);
    REQUIRE(full.treasury(account) == kMaxTreasuryKop - 5);
    const auto full_hash = full.compute_hash();
    CHECK_FALSE(full.take_loan(DynastyId(0), 10, rules.loans).has_value());
    CHECK(full.compute_hash() == full_hash);
    CHECK(full.economy().loans.size() == 0);
    require_valid(full, rules);
}

TEST_CASE("loans: fifty years preserve every kopeck for rates not divisible by twelve") {
    for (const auto& [principal, rate] :
         std::array<std::pair<Kopecks, std::int32_t>, 3>{{{1201, 100'000}, {13, 99'991}, {1, 1}}}) {
        EconomyRules rules = loan_rules();
        rules.loans.rate_ppm_year = rate;
        rules.loans.term_months = 600;
        World world = make_world();
        const DynastyId borrower(0);
        const auto loan = world.take_loan(borrower, principal, rules.loans);
        REQUIRE(loan.has_value());
        Kopecks interest = 0;
        for (int month = 0; month < 600; ++month) {
            close_next_month(world, rules);
            const Kopecks charged = -closed_line(world, AccountRef::of(borrower), LedgerCode::Interest);
            CHECK(charged == closed_line(world, AccountRef::crown(), LedgerCode::InterestCollected));
            interest += charged;
        }
        const Kopecks numerator = principal * rate * 600;
        CHECK(interest == numerator / 12'000'000);
        const auto row = find_loan(world, *loan);
        REQUIRE(row.has_value());
        CHECK(world.economy().loans.interest_carry[*row] == numerator % 12'000'000);
        CHECK(world.treasury(AccountRef::of(borrower)) == principal - interest);
        CHECK(world.treasury(AccountRef::of(borrower)) + world.treasury(AccountRef::crown()) == 0);
        require_valid(world, rules);
    }
}

TEST_CASE("loans: the treasury forecast includes fractional interest without changing the world") {
    EconomyRules rules = loan_rules();
    rules.loans.rate_ppm_year = 100'000;
    World world = make_world();
    const DynastyId borrower(0);
    REQUIRE(world.take_loan(borrower, 100, rules.loans).has_value());
    close_next_month(world, rules);  // 5/6 kopeck is carried into the forecast.
    const auto hash = world.compute_hash();
    const StaticPriceSource prices(rules.production);
    const auto debt = forecast_treasury(world, rules, prices, {}, AccountRef::of(borrower));
    const auto crown = forecast_treasury(world, rules, prices, {}, AccountRef::crown());
    CHECK(debt.monthly_lines[*ledger_index(LedgerCode::Interest)] == -1);
    CHECK(crown.monthly_lines[*ledger_index(LedgerCode::InterestCollected)] == 1);
    CHECK(debt.monthly_net == -1);
    CHECK(crown.monthly_net == 1);
    CHECK(debt.balance.back() == 90);  // Ten kopecks in a year; twelve copies of next month lose two.
    CHECK(crown.balance.back() == crown.treasury + 10);
    CHECK(world.compute_hash() == hash);
    for (std::size_t month = 0; month < kForecastMonths; ++month) {
        close_next_month(world, rules);
        CHECK(debt.balance[month] == world.treasury(AccountRef::of(borrower)));
        CHECK(crown.balance[month] == world.treasury(AccountRef::crown()));
    }
}

TEST_CASE("loans: saves retain fractional interest and both pending command layouts") {
    const EconomyRules rules = loan_rules();
    World straight = make_world();
    const auto loan = straight.take_loan(DynastyId(0), 1201, rules.loans);
    REQUIRE(loan.has_value());
    close_next_month(straight, rules);
    const std::vector<Command> pending = {take(DynastyId(1), 77), repay(DynastyId(0), *loan, 1)};
    const auto bytes = serialize_save(straight, pending);
    auto loaded = deserialize_save(bytes);
    REQUIRE(loaded.has_value());
    CHECK(loaded.value().pending_internal == pending);
    CHECK(loaded.value().world.compute_hash() == straight.compute_hash());
    CHECK(loaded.value().world.economy().loans == straight.economy().loans);
    REQUIRE(straight.economy().loans.interest_carry[0] > 0);
    World resumed = std::move(loaded.value().world);
    for (int month = 0; month < 24; ++month) {
        close_next_month(straight, rules);
        close_next_month(resumed, rules);
    }
    CHECK(resumed.compute_hash() == straight.compute_hash());
    CHECK(resumed.economy() == straight.economy());
    require_valid(resumed, rules);
}

TEST_CASE("loan commands: reserved tags, command logs and recorded replay preserve the outcome") {
    const EconomyRules rules = loan_rules();
    CHECK(static_cast<std::uint8_t>(type_of(take(DynastyId(0), 1))) == 38);
    CHECK(static_cast<std::uint8_t>(type_of(repay(DynastyId(0), 0, 1))) == 39);
    CHECK(to_string(CommandType::TakeLoan) == "TakeLoan");
    CHECK(to_string(CommandType::RepayLoan) == "RepayLoan");
    Simulation recorded = make_simulation(make_world(), rules);
    ReplayRecorder recorder(7, 158, recorded.world().today(), 10);
    for (int day = 0; day < 75; ++day) {
        if (day == 0) {
            REQUIRE(recorded.submit(take(DynastyId(0), 1201)));
        }
        if (day == 7) {
            REQUIRE(recorded.world().economy().loans.size() == 1);
            const auto id = recorded.world().economy().loans.loan_id[0];
            REQUIRE(recorded.submit(repay(DynastyId(0), id, 201)));
            REQUIRE(recorded.submit(repay(DynastyId(1), id, 1)));  // wrong borrower
        }
        (void)recorded.tick();
        recorder.after_tick(recorded);
    }
    const Replay replay = recorder.finish(recorded);
    REQUIRE(replay.commands.size() == 2);
    CHECK(type_of(replay.commands[0].command) == CommandType::TakeLoan);
    CHECK(type_of(replay.commands[1].command) == CommandType::RepayLoan);
    const auto log_bytes = recorded.command_log().serialize();
    const auto log = CommandLog::deserialize(log_bytes);
    REQUIRE(log.has_value());
    CHECK(log.value().serialize() == log_bytes);
    const auto replay_bytes = serialize_replay(replay);
    const auto parsed = deserialize_replay(replay_bytes);
    REQUIRE(parsed.has_value());
    CHECK(parsed.value().commands == replay.commands);
    CHECK(serialize_replay(parsed.value()) == replay_bytes);
    Simulation played = make_simulation(make_world(), rules);
    const ReplayResult result = play_replay(played, parsed.value());
    CHECK(result.error.empty());
    CHECK(result.ok);
    CHECK(played.world().compute_hash() == recorded.world().compute_hash());
    require_valid(played.world(), rules);
}
