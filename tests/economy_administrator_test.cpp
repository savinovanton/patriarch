#include "pat/core/rng.hpp"
#include "pat/sim/calendar.hpp"
#include "pat/sim/command.hpp"
#include "pat/sim/command_log.hpp"
#include "pat/sim/economy/administrator.hpp"
#include "pat/sim/economy/breakdown.hpp"
#include "pat/sim/economy/ledger.hpp"
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

// E9-T10: the administrator of a province, control and corruption (docs/dev/economy-model.md section 9
// formula F7, section 7.3 step 1; GDD 03 sections 3.2 and 7.4), the command AssignProvinceAdministrator
// and the entry "Anarchy" of the incident log.

using namespace pat::sim;
using namespace pat::sim::economy;
using pat::core::Fixed32;
using pat::core::Pcg32;
using pat::core::StringId;

namespace {

constexpr std::int64_t kOne = Fixed32::one_raw;

const EconomyRules& rules() {
    static const EconomyRules value = EconomyRules::defaults();
    return value;
}

/// The same rules with the administration frozen: control and corruption stay where the test puts them.
const EconomyRules& frozen_rules() {
    static const EconomyRules value = [] {
        EconomyRules r = EconomyRules::defaults();
        r.control.control_inertia_permille = 0;
        r.control.corruption_inertia_permille = 0;
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

/// A profile table: the test decides who is a good administrator.
class TableSource final : public AdministratorSource {
public:
    void set(CharacterId who, AdministratorProfile profile) { table_.emplace_back(who, profile); }
    [[nodiscard]] AdministratorProfile profile(const World& /*world*/, CharacterId who) const override {
        for (const auto& [id, profile] : table_) {
            if (id == who) {
                return profile;
            }
        }
        return AdministratorProfile{};
    }

private:
    std::vector<std::pair<CharacterId, AdministratorProfile>> table_;
};

AdministrationInput neutral_input() {
    AdministrationInput in;
    in.control = points(70);
    in.corruption = Fixed32::from_raw((150 * kOne + 999) / 1000);
    in.unrest = Fixed32::zero();
    return in;
}

/// Two dynasties and `provinces` provinces owned by dynasty 0; economy on; peasants set.
World make_world(std::uint32_t provinces,
                 std::uint32_t peasants = 1000,
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
        world.economy_columns_mut().tax_policy[p] = static_cast<std::uint8_t>(rules().tax.default_level);
    }
    require_valid(world);
    return world;
}

CharacterId make_person(World& world, DynastyId dynasty) {
    CharacterInit init;
    init.dynasty = dynasty;
    init.birth_date = Date::from_ymd(1670, 5, 1);
    return world.create_character(init);
}

void run_months(Simulation& sim, int months) {
    for (int i = 0; i < months; ++i) {
        const Date next = sim.world().today() + 1;
        sim.run_days(Date::days_in_month(next.year(), next.month()));
    }
}

std::size_t incidents_of(const World& world, IncidentKind kind) {
    std::size_t count = 0;
    for (const std::uint8_t k : world.economy().incidents.kind) {
        count += k == static_cast<std::uint8_t>(kind) ? 1 : 0;
    }
    return count;
}

/// The corruption column that reads back as `permille`.
Fixed32 corruption_of(std::int32_t permille) {
    return Fixed32::from_raw((permille * Fixed32::one_raw + 999) / 1000);
}

}  // namespace

// ---- the thresholds -----------------------------------------------------------------------------

TEST_CASE("control state: 40 and 15 are strict limits, both documented and exact") {
    const ControlRules& c = rules().control;
    REQUIRE(c.low_threshold == 40);
    REQUIRE(c.anarchy_threshold == 15);

    // inclusive below, exclusive at the threshold itself: 40 is still normal, 15 is still low
    CHECK(control_state(c, points(100)) == ControlState::Normal);
    CHECK(control_state(c, points(40)) == ControlState::Normal);
    CHECK(control_state(c, Fixed32::from_raw(40 * kOne - 1)) == ControlState::Low);
    CHECK(control_state(c, points(39)) == ControlState::Low);
    CHECK(control_state(c, points(15)) == ControlState::Low);
    CHECK(control_state(c, Fixed32::from_raw(15 * kOne - 1)) == ControlState::Anarchy);
    CHECK(control_state(c, points(14)) == ControlState::Anarchy);
    CHECK(control_state(c, points(0)) == ControlState::Anarchy);

    ControlRules other = c;  // the thresholds are data, not constants
    other.low_threshold = 50;
    other.anarchy_threshold = 20;
    CHECK(control_state(other, points(45)) == ControlState::Low);
    CHECK(control_state(other, points(20)) == ControlState::Low);
    CHECK(control_state(other, points(19)) == ControlState::Anarchy);
    CHECK(control_state(other, points(50)) == ControlState::Normal);
}

TEST_CASE("control state: the taxes classify control by the same thresholds") {
    TaxInput in;
    in.wealth.fill(points(100));
    in.owner_kind = OwnerKind::Dynasty;
    in.tax_level = 1;
    in.size[index_of(Group::Peasants)] = 12'000;

    in.control = points(40);  // exactly 40: not low
    const GroupTax at_40 = compute_group_tax(rules().tax, rules().control, in, Group::Peasants);
    in.control = Fixed32::from_raw(40 * kOne - 1);  // just below: the low-control multiplier applies
    const GroupTax below_40 = compute_group_tax(rules().tax, rules().control, in, Group::Peasants);
    CHECK(below_40.gross < at_40.gross * 7 / 10 + 5);  // x0.7 on top of a slightly smaller control

    in.control = points(15);  // exactly 15: low, but the owner is paid
    CHECK(compute_group_tax(rules().tax, rules().control, in, Group::Peasants).gross > 0);
    CHECK_FALSE(compute_province_income(rules(), in, 1000).anarchy);
    in.control = Fixed32::from_raw(15 * kOne - 1);  // just below: turmoil, nothing is collected
    CHECK(compute_group_tax(rules().tax, rules().control, in, Group::Peasants).gross == 0);
    CHECK(compute_province_income(rules(), in, 1000).anarchy);
    CHECK(compute_province_income(rules(), in, 1000).holding == 0);
}

// ---- the profile ---------------------------------------------------------------------------------

TEST_CASE("administrator profile: the stub is 50/50 until E7 and tests can substitute it") {
    World world = make_world(1);
    const CharacterId person = make_person(world, DynastyId(0));
    const AdministratorProfile stub = administrator_profile(world, person);
    CHECK(stub.stewardship == 50);
    CHECK(stub.integrity == 50);
    CHECK(StubAdministratorSource().profile(world, person) == stub);

    // the stub changes nothing: no administrator and an average one give the same month
    const ControlRules& c = rules().control;
    AdministrationInput none = neutral_input();
    none.unrest = points(10);
    AdministrationInput average = none;
    average.profile = stub;
    CHECK(compute_administration(c, none) == compute_administration(c, average));

    // a substituted source is read through the input of a province
    REQUIRE(world.set_province_administrator(ProvinceId(0), person));
    TableSource table;
    table.set(person, AdministratorProfile{90, 20});
    const AdministrationInput in = administration_input(world, &table, ProvinceId(0));
    CHECK(in.profile.stewardship == 90);
    CHECK(in.profile.integrity == 20);
    CHECK(administration_input(world, nullptr, ProvinceId(0)).profile == AdministratorProfile{});

    // a profile outside 0..100 is clamped before it is used
    World other = make_world(1);
    const CharacterId wild = make_person(other, DynastyId(0));
    TableSource wild_table;
    wild_table.set(wild, AdministratorProfile{500, -3});
    REQUIRE(other.set_province_administrator(ProvinceId(0), wild));
    const AdministrationInput clamped = administration_input(other, &wild_table, ProvinceId(0));
    CHECK(clamped.profile.stewardship == 100);
    CHECK(clamped.profile.integrity == 0);

    // a world without economy or an unknown province gives the default input
    World bare(Date::from_ymd(1703, 1, 1), 5);
    CHECK(administration_input(bare, nullptr, ProvinceId(0)).control == kDefaultControl);
}

// ---- the formula ---------------------------------------------------------------------------------

TEST_CASE("F7 control target: the terms, their order and the truncation") {
    const ControlRules& c = rules().control;
    REQUIRE(c.control_base == 70);
    REQUIRE(c.steward_gain_permille == 600);
    REQUIRE(c.unrest_penalty_permille == 300);

    AdministrationInput in = neutral_input();
    // the neutral profile and no unrest: the base
    CHECK(compute_control_target(c, in) == points(70));

    in.profile.stewardship = 70;  // +20 above neutral x 0.6 = +12
    CHECK(compute_control_target(c, in) == points(82));
    in.profile.stewardship = 50;
    in.unrest = points(20);  // -20 x 0.3 = -6
    CHECK(compute_control_target(c, in) == points(64));
    in.profile.stewardship = 0;  // -50 x 0.6 = -30
    in.unrest = Fixed32::zero();
    CHECK(compute_control_target(c, in) == points(40));

    // a fraction is truncated toward zero in the raw units: 7 points above neutral x 0.6 = 4.2
    in.profile.stewardship = 57;
    CHECK(compute_control_target(c, in).raw() == 74 * kOne + 13107);  // 4.2 x 65536 = 275251.2 -> 275251
}

TEST_CASE("F7 control target: the breakdown adds up and names every term") {
    const EconomyRules& r = rules();
    AdministrationInput in = neutral_input();
    in.profile.stewardship = 90;      // +40 x 0.6 = +24
    in.unrest = points(40);           // -40 x 0.3 = -12
    in.building_control = points(3);  // the hook of E9-T17
    const Breakdown b = explain_control(r, in);
    REQUIRE(b.is_consistent());
    CHECK(b.root().code == term::kControlTarget);
    CHECK(b.root().unit == Unit::Points);
    CHECK(b.total() == 85 * kOne);  // 70 + 24 + 3 - 12
    CHECK(b.node(b.find(term::kControlBase)).value == 70 * kOne);
    CHECK(b.node(b.find(term::kControlAdministrator)).value == 24 * kOne);
    CHECK(b.node(b.find(term::kControlAdministrator)).factor_permille == 600);
    CHECK(b.node(b.find(term::kControlBuildings)).value == 3 * kOne);
    CHECK(b.node(b.find(term::kControlUnrest)).value == -12 * kOne);
    CHECK(b.find(term::kControlLimit) == kNoNode);
    // the result does not depend on whether a breakdown was asked for
    CHECK(compute_control_target(r.control, in).raw() == b.total());

    // the clamp to 0..100 is a term of its own, so the sum still holds
    in.profile.stewardship = 100;
    in.building_control = points(20);
    const Breakdown over = explain_control(r, in);
    REQUIRE(over.is_consistent());
    CHECK(over.total() == 100 * kOne);
    CHECK(over.node(over.find(term::kControlLimit)).value < 0);

    ControlRules harsh = r.control;
    harsh.control_base = 0;
    in = neutral_input();
    in.profile.stewardship = 0;
    in.unrest = points(100);
    Breakdown under;
    CHECK(compute_control_target(harsh, in, &under) == Fixed32::zero());
    REQUIRE(under.is_consistent());
    CHECK(under.node(under.find(term::kControlLimit)).value > 0);
}

TEST_CASE("F7 corruption target: honest administrators steal less, weak control steals more") {
    const ControlRules& c = rules().control;
    REQUIRE(c.corruption_base_permille == 150);
    REQUIRE(c.integrity_gain_permille == 3);
    REQUIRE(c.low_control_corruption_permille == 5);

    AdministrationInput in = neutral_input();
    CHECK(corruption_permille(compute_corruption_target(c, in)) == 150);
    in.profile.integrity = 100;  // -50 x 3 = -150
    CHECK(corruption_permille(compute_corruption_target(c, in)) == 0);
    in.profile.integrity = 0;  // +150
    CHECK(corruption_permille(compute_corruption_target(c, in)) == 300);

    // below the low threshold every missing point adds 0.5 %: control 30 is 10 points short
    in.profile.integrity = 50;
    in.control = points(30);
    CHECK(corruption_permille(compute_corruption_target(c, in)) == 150 + 50);
    in.control = points(40);  // exactly 40 is not low
    CHECK(corruption_permille(compute_corruption_target(c, in)) == 150);
    in.control = points(0);
    CHECK(corruption_permille(compute_corruption_target(c, in)) == 150 + 200);

    // the breakdown
    in.control = points(30);
    in.profile.integrity = 80;  // -30 x 3 = -90
    in.oversight_permille = 10;
    const Breakdown b = explain_corruption(rules(), in);
    REQUIRE(b.is_consistent());
    CHECK(b.root().code == term::kCorruptionTarget);
    CHECK(b.root().unit == Unit::Permille);
    CHECK(b.total() == 150 - 90 - 10 + 50);
    CHECK(b.node(b.find(term::kCorruptionBase)).value == 150);
    CHECK(b.node(b.find(term::kCorruptionIntegrity)).value == -90);
    CHECK(b.node(b.find(term::kCorruptionOversight)).value == -10);
    CHECK(b.node(b.find(term::kCorruptionLowControl)).value == 50);
    CHECK(corruption_permille(compute_corruption_target(c, in)) == b.total());

    // clamps to 0..1 as a term of their own
    ControlRules thief = c;
    thief.corruption_base_permille = 900;
    in = neutral_input();
    in.profile.integrity = 0;
    in.control = points(0);
    Breakdown top;
    CHECK(compute_corruption_target(thief, in, &top) == Fixed32::one());
    REQUIRE(top.is_consistent());
    CHECK(top.total() == 1000);
    CHECK(top.node(top.find(term::kCorruptionLimit)).value < 0);
    ControlRules saint = c;
    saint.corruption_base_permille = 0;
    in.profile.integrity = 100;
    in.control = points(100);
    Breakdown bottom;
    CHECK(compute_corruption_target(saint, in, &bottom) == Fixed32::zero());
    REQUIRE(bottom.is_consistent());
    CHECK(bottom.node(bottom.find(term::kCorruptionLimit)).value > 0);
}

TEST_CASE("F7: a month closes part of the gap and never jumps over the target") {
    CHECK(approach(points(70), points(100), 0) == points(70));      // frozen
    CHECK(approach(points(70), points(100), 1000) == points(100));  // all at once
    CHECK(approach(points(70), points(100), 150) == Fixed32::from_raw(74 * kOne + kOne / 2));
    CHECK(approach(points(100), points(70), 150) == Fixed32::from_raw(95 * kOne + kOne / 2));
    // a positive inertia closes even a tiny gap by at least one raw unit, without overshooting
    CHECK(approach(Fixed32::from_raw(1000), Fixed32::from_raw(1003), 150) == Fixed32::from_raw(1001));
    CHECK(approach(Fixed32::from_raw(1003), Fixed32::from_raw(1000), 150) == Fixed32::from_raw(1002));
    CHECK(approach(Fixed32::from_raw(1000), Fixed32::from_raw(1001), 1) == Fixed32::from_raw(1001));
    CHECK(approach(Fixed32::from_raw(1001), Fixed32::from_raw(1000), 1) == Fixed32::from_raw(1000));
    CHECK(approach(Fixed32::from_raw(1000), Fixed32::from_raw(1000), 150) == Fixed32::from_raw(1000));
    // an inertia out of its range is clamped
    CHECK(approach(points(10), points(20), 5000) == points(20));
    CHECK(approach(points(10), points(20), -5) == points(10));

    // iterated, the value converges to the target from either side without crossing it
    Fixed32 up = points(0);
    Fixed32 down = points(100);
    for (int month = 0; month < 200; ++month) {
        up = approach(up, points(60), 150);
        down = approach(down, points(60), 150);
        CHECK(up <= points(60));
        CHECK(down >= points(60));
    }
    CHECK(up == points(60));
    CHECK(down == points(60));
}

TEST_CASE("F7: converged control at 40 and 15 has the same state and tax from either side") {
    for (const std::int32_t threshold : {40, 15}) {
        EconomyRules r = rules();
        // The default worst steward targets 40; a data-defined base can put the target at 15.
        r.control.control_base = threshold + 30;
        AdministrationInput settled = neutral_input();
        settled.profile.stewardship = 0;
        settled.control = points(threshold);
        settled.corruption = compute_corruption_target(r.control, settled);
        REQUIRE(compute_control_target(r.control, settled) == points(threshold));

        TaxInput tax;
        tax.wealth.fill(points(100));
        tax.owner_kind = OwnerKind::Dynasty;
        tax.tax_level = 1;
        tax.size[index_of(Group::Peasants)] = 12'000;
        tax.control = settled.control;
        tax.corruption = settled.corruption;
        const ProvinceIncome expected = compute_province_income(r, tax, 1000);
        REQUIRE(expected.group[index_of(Group::Peasants)].gross > 0);
        REQUIRE_FALSE(expected.anarchy);

        for (const std::int32_t start : {threshold - 10, threshold + 20}) {
            AdministrationInput in = settled;
            in.control = points(start);
            in.corruption = Fixed32::zero();
            for (int month = 0; month < 200; ++month) {
                const Administration next = compute_administration(r.control, in);
                in.control = next.control;
                in.corruption = next.corruption;
            }
            INFO("target " << threshold << ", start " << start);
            CHECK(in.control == settled.control);
            CHECK(in.corruption == settled.corruption);
            CHECK(control_state(r.control, in.control) ==
                  (threshold == 40 ? ControlState::Normal : ControlState::Low));
            CHECK(points_permille(in.control) == threshold * 10);
            tax.control = in.control;
            tax.corruption = in.corruption;
            CHECK(compute_province_income(r, tax, 1000) == expected);
        }
    }
}

TEST_CASE("F7: corruption starting at zero reaches the default 150 per-mille exactly") {
    AdministrationInput in = neutral_input();
    in.corruption = Fixed32::zero();
    const Fixed32 target = compute_corruption_target(rules().control, in);
    for (int month = 0; month < 200; ++month) {
        const Administration next = compute_administration(rules().control, in);
        CHECK(next.corruption >= in.corruption);
        CHECK(next.corruption <= target);
        in.control = next.control;
        in.corruption = next.corruption;
    }
    CHECK(in.corruption == target);
    CHECK(corruption_permille(in.corruption) == 150);
}

TEST_CASE("F7: control stays within 0..100 and corruption within 0..1 whatever the input") {
    Pcg32 rng(0xAD3141U, 1);
    for (int round = 0; round < 3000; ++round) {
        EconomyRules r = EconomyRules::defaults();
        r.control.control_base = rng.range(0, 100);
        r.control.steward_gain_permille = rng.range(0, 10'000);
        r.control.unrest_penalty_permille = rng.range(0, 10'000);
        r.control.control_inertia_permille = rng.range(0, 1000);
        r.control.corruption_base_permille = rng.range(0, 1000);
        r.control.integrity_gain_permille = rng.range(0, 10'000);
        r.control.low_control_corruption_permille = rng.range(0, 10'000);
        r.control.corruption_inertia_permille = rng.range(0, 1000);
        REQUIRE(validate(r));

        AdministrationInput in;
        in.control = Fixed32::from_raw(rng.range(0, 100 * Fixed32::one_raw));
        in.corruption = Fixed32::from_raw(rng.range(0, Fixed32::one_raw));
        in.unrest = Fixed32::from_raw(rng.range(0, 100 * Fixed32::one_raw));
        in.profile = {rng.range(-50, 200), rng.range(-50, 200)};
        in.building_control = Fixed32::from_raw(rng.range(0, 30 * Fixed32::one_raw));
        in.oversight_permille = rng.range(-100, 1200);
        const Administration a = compute_administration(r.control, in);
        REQUIRE(a.control.raw() >= 0);
        REQUIRE(a.control.raw() <= 100 * kOne);
        REQUIRE(a.corruption.raw() >= 0);
        REQUIRE(a.corruption.raw() <= kOne);
        // it moves toward the target and not past it
        const auto between = [](Fixed32 from, Fixed32 to, Fixed32 value) {
            return value.raw() >= std::min(from.raw(), to.raw()) &&
                   value.raw() <= std::max(from.raw(), to.raw());
        };
        REQUIRE(between(in.control, a.control_target, a.control));
        REQUIRE(between(in.corruption, a.corruption_target, a.corruption));
    }
}

TEST_CASE("F7: a column written out of its range is pulled back into it") {
    AdministrationInput in = neutral_input();
    in.control = Fixed32::from_raw(150 * kOne);
    in.corruption = Fixed32::from_raw(3 * kOne);
    const Administration a = compute_administration(rules().control, in);
    CHECK(a.control.raw() <= 100 * kOne);
    CHECK(a.corruption.raw() <= kOne);
    in.control = Fixed32::from_raw(-5 * kOne);
    in.corruption = Fixed32::from_raw(-kOne);
    const Administration b = compute_administration(rules().control, in);
    CHECK(b.control.raw() >= 0);
    CHECK(b.corruption.raw() >= 0);
}

TEST_CASE("rules: the new key low_control_corruption_permille is validated") {
    EconomyRules r = EconomyRules::defaults();
    CHECK(validate(r));
    r.control.low_control_corruption_permille = -1;
    const auto negative = validate(r);
    REQUIRE_FALSE(negative);
    CHECK(negative.error().code == EconomyRulesErrorCode::Control);
    r.control.low_control_corruption_permille = kMaxMultiplierPermille + 1;
    CHECK_FALSE(validate(r));
}

// ---- the command ---------------------------------------------------------------------------------

namespace {

/// Province 0 owned by dynasty 0, 1 by dynasty 1, 2 by the crown, 3 by nobody. Persons: 0 and 1 in
/// dynasty 0, 2 in dynasty 1.
struct CommandWorld {
    World world = make_world(4);
    CharacterId a, a2, b;

    CommandWorld() {
        (void)world.set_province_owner(ProvinceId(1), DynastyId(1));
        REQUIRE(world.set_province_owner_kind(ProvinceId(2), OwnerKind::Crown));
        REQUIRE(world.set_province_owner_kind(ProvinceId(3), OwnerKind::Unowned));
        a = make_person(world, DynastyId(0));
        a2 = make_person(world, DynastyId(0));
        b = make_person(world, DynastyId(1));
        require_valid(world);
    }
};

Command assign(DynastyId actor, ProvinceId province, CharacterId who) {
    return Command{actor, cmd::AssignProvinceAdministrator{province, who}};
}

Rejection verdict(const World& world, const Command& command) {
    const auto result = validate(world, command);
    return result.has_value() ? Rejection::None : result.error().reason;
}

}  // namespace

TEST_CASE("AssignProvinceAdministrator: the owner appoints a member of its own house") {
    CommandWorld w;
    const Command ok = assign(DynastyId(0), ProvinceId(0), w.a);
    REQUIRE(validate(w.world, ok).has_value());
    apply(w.world, ok);
    CHECK(w.world.economy().provinces.administrator[0] == w.a);
    require_valid(w.world);

    // a second person replaces the first one; the same one again is an error; nobody dismisses
    apply(w.world, assign(DynastyId(0), ProvinceId(0), w.a2));
    CHECK(w.world.economy().provinces.administrator[0] == w.a2);
    CHECK(verdict(w.world, assign(DynastyId(0), ProvinceId(0), w.a2)) == Rejection::InvalidState);
    REQUIRE(verdict(w.world, assign(DynastyId(0), ProvinceId(0), CharacterId())) == Rejection::None);
    apply(w.world, assign(DynastyId(0), ProvinceId(0), CharacterId()));
    CHECK_FALSE(w.world.economy().provinces.administrator[0].valid());
    CHECK(verdict(w.world, assign(DynastyId(0), ProvinceId(0), CharacterId())) == Rejection::InvalidState);
    require_valid(w.world);
}

TEST_CASE("AssignProvinceAdministrator: every rejection and its reason") {
    CommandWorld w;
    // unknown province or character
    CHECK(verdict(w.world, assign(DynastyId(0), ProvinceId(99), w.a)) == Rejection::UnknownEntity);
    CHECK(verdict(w.world, assign(DynastyId(0), ProvinceId(0), CharacterId(99))) == Rejection::UnknownEntity);
    CHECK(verdict(w.world, assign(DynastyId(), ProvinceId(99), w.a)) == Rejection::UnknownEntity);
    CHECK(verdict(w.world, assign(DynastyId(), ProvinceId(0), CharacterId(99))) == Rejection::UnknownEntity);
    // not the owner: another dynasty's province, the crown's, nobody's
    CHECK(verdict(w.world, assign(DynastyId(0), ProvinceId(1), w.a)) == Rejection::NotAuthorized);
    CHECK(verdict(w.world, assign(DynastyId(0), ProvinceId(2), w.a)) == Rejection::NotAuthorized);
    CHECK(verdict(w.world, assign(DynastyId(0), ProvinceId(3), w.a)) == Rejection::NotAuthorized);
    // a person of another house is not "lawfully" the administrator for a dynasty
    CHECK(verdict(w.world, assign(DynastyId(0), ProvinceId(0), w.b)) == Rejection::NotAuthorized);
    // the actor must exist
    CHECK(verdict(w.world, assign(DynastyId(9), ProvinceId(0), w.a)) == Rejection::UnknownEntity);
    // none of the rejected commands changed anything
    CHECK_FALSE(w.world.economy().provinces.administrator[0].valid());
    CHECK_FALSE(w.world.economy().provinces.administrator[1].valid());

    // a world without economy has no administrators at all
    World bare(Date::from_ymd(1703, 1, 1), 5);
    (void)bare.create_dynasty(StringId(1));
    (void)bare.create_province(StringId(2), DynastyId(0), Fixed32::from_int(1));
    CHECK(verdict(bare, assign(DynastyId(), ProvinceId(0), CharacterId())) == Rejection::InvalidState);
}

TEST_CASE("AssignProvinceAdministrator: the system appoints lawfully, a dead person never") {
    CommandWorld w;
    // the system (an invalid actor) appoints anybody to any province: the crown's, a foreign house's
    REQUIRE(verdict(w.world, assign(DynastyId(), ProvinceId(2), w.b)) == Rejection::None);
    apply(w.world, assign(DynastyId(), ProvinceId(2), w.b));
    CHECK(w.world.economy().provinces.administrator[2] == w.b);
    REQUIRE(verdict(w.world, assign(DynastyId(), ProvinceId(1), w.a)) == Rejection::None);
    REQUIRE(verdict(w.world, assign(DynastyId(), ProvinceId(3), w.a)) == Rejection::None);
    CHECK(verdict(w.world, assign(DynastyId(), ProvinceId(2), w.b)) == Rejection::InvalidState);

    // dynasty mode: the dead stay in the archive and cannot be appointed (InvalidState); the world
    // already dropped the post of the person who died
    REQUIRE(w.world.enable_dynasty_mode());
    REQUIRE(w.world.die(w.b, CauseOfDeath::Illness));
    CHECK_FALSE(w.world.economy().provinces.administrator[2].valid());
    CHECK(verdict(w.world, assign(DynastyId(), ProvinceId(2), w.b)) == Rejection::InvalidState);
    CHECK(verdict(w.world, assign(DynastyId(1), ProvinceId(1), w.b)) == Rejection::InvalidState);
    require_valid(w.world);
}

TEST_CASE("AssignProvinceAdministrator: tag 35, the command log round-trips, a replay gives the same state") {
    CommandWorld w;
    const Command command = assign(DynastyId(0), ProvinceId(0), w.a);
    CHECK(static_cast<std::uint8_t>(type_of(command)) == 35);
    CHECK(to_string(type_of(command)) == "AssignProvinceAdministrator");

    // the serialised log reads back the same command
    CommandLog log;
    LoggedCommand entry;
    entry.date = w.world.today();
    entry.source = CommandSource::External;
    entry.command = command;
    log.append(entry);
    entry.command = assign(DynastyId(), ProvinceId(2), CharacterId());
    entry.outcome = Rejection::InvalidState;
    log.append(entry);
    const auto bytes = log.serialize();
    const auto back = CommandLog::deserialize(bytes);
    REQUIRE(back.has_value());
    REQUIRE(back.value().entries().size() == 2);
    CHECK(back.value().entries()[0].command == command);
    CHECK(back.value().entries()[1].command == entry.command);
    CHECK(back.value().entries()[1].outcome == Rejection::InvalidState);
    CHECK(back.value().serialize() == bytes);

    // submitted from outside, applied at the start of the next tick, the same on every run
    const auto run = [&]() {
        Simulation sim(w.world);
        install_economy_systems(sim, rules());
        REQUIRE(sim.submit(command));
        sim.run_days(40);
        return sim.world().compute_hash();
    };
    const auto first = run();
    CHECK(first == run());
    Simulation sim(w.world);
    install_economy_systems(sim, rules());
    REQUIRE(sim.submit(command));
    sim.run_days(1);
    CHECK(sim.world().economy().provinces.administrator[0] == w.a);
    CHECK(sim.command_log().entries().size() == 1);
    CHECK(sim.command_log().entries()[0].accepted());
}

// ---- the system: ECO-A3 ----------------------------------------------------------------------------

namespace {

/// What the column does in `months` months when it moves `inertia` of the gap each month: written by
/// hand, independently of approach().
std::int64_t iterate(std::int64_t value, std::int64_t target, std::int64_t inertia_permille, int months) {
    for (int i = 0; i < months; ++i) {
        value += (target - value) * inertia_permille / 1000;  // C++ division truncates toward zero
    }
    return value;
}

/// How many months a gap needs to shrink to at most half with this inertia (the N of ECO-A3, from the rules).
int months_to_halve(std::int32_t inertia_permille) {
    std::int64_t gap = 1'000'000;
    int months = 0;
    while (gap * 2 > 1'000'000) {
        gap = gap * (1000 - inertia_permille) / 1000;
        ++months;
        REQUIRE(months < 1000);
    }
    return months;
}

}  // namespace

TEST_CASE("ECO-A3: a skilled administrator raises control and lowers corruption within N months") {
    const EconomyRules& r = rules();
    World world = make_world(3);
    const CharacterId skilled = make_person(world, DynastyId(0));
    const CharacterId clumsy = make_person(world, DynastyId(0));
    // province 0: a skilled and honest administrator, 1: a clumsy and dishonest one, 2: nobody
    REQUIRE(world.set_province_administrator(ProvinceId(0), skilled));
    REQUIRE(world.set_province_administrator(ProvinceId(1), clumsy));
    for (std::size_t p = 0; p < 3; ++p) {  // the same start everywhere: the state of an average province
        world.economy_columns_mut().control[p] = points(70);
        world.economy_columns_mut().corruption[p] = corruption_of(150);
    }
    auto table = std::make_shared<TableSource>();
    table->set(skilled, AdministratorProfile{100, 100});
    table->set(clumsy, AdministratorProfile{0, 0});

    Simulation sim(std::move(world));
    install_economy_systems(sim, r, nullptr, {}, table);
    const int n = months_to_halve(r.control.control_inertia_permille);
    REQUIRE(n == 5);  // 15 % a month: 0.85^5 < 0.5
    run_months(sim, n);

    const auto& columns = sim.world().economy().provinces;
    const Fixed32 skilled_control = columns.control[0];
    const Fixed32 clumsy_control = columns.control[1];
    const Fixed32 nobody_control = columns.control[2];
    CHECK(skilled_control > nobody_control);
    CHECK(nobody_control > clumsy_control);
    CHECK(columns.corruption[0] < columns.corruption[2]);
    CHECK(columns.corruption[2] < columns.corruption[1]);
    // at least half of the way to the target of 100 (+30) after N months
    CHECK(skilled_control.raw() >= 85 * kOne);
    CHECK(clumsy_control.raw() <= 55 * kOne);  // 70 -> 40 and at least half way there
    CHECK(columns.corruption[0].raw() <= corruption_of(75).raw());
    CHECK(columns.corruption[1].raw() >= corruption_of(225).raw());
    // an average administrator (and nobody) changes nothing: the province already stands at its target
    CHECK(nobody_control == points(70));
    CHECK(corruption_permille(columns.corruption[2]) == 150);

    // and they follow the hand-written iteration of the formula exactly
    CHECK(skilled_control.raw() == iterate(70 * kOne, 100 * kOne, 150, n));
    CHECK(clumsy_control.raw() == iterate(70 * kOne, 40 * kOne, 150, n));
    CHECK(columns.corruption[0].raw() == iterate(corruption_of(150).raw(), 0, 150, n));
    CHECK(columns.corruption[1].raw() == iterate(corruption_of(150).raw(), corruption_of(300).raw(), 150, n));

    // the better grip pays: more income for the owner from the same land and the same people
    CHECK(columns.income_last_month[0] > columns.income_last_month[2]);
    CHECK(columns.income_last_month[2] > columns.income_last_month[1]);
    require_valid(sim.world());
}

TEST_CASE("ECO-A3: a province converges to the target of its administrator and stays there") {
    World world = make_world(1);
    const CharacterId person = make_person(world, DynastyId(0));
    REQUIRE(world.set_province_administrator(ProvinceId(0), person));
    world.economy_columns_mut().control[0] = points(100);  // a fresh province starts at 100
    auto table = std::make_shared<TableSource>();
    table->set(person, AdministratorProfile{65, 50});  // +15 x 0.6 = +9: the target is 79
    Simulation sim(std::move(world));
    install_economy_systems(sim, rules(), nullptr, {}, table);
    run_months(sim, 120);
    CHECK(sim.world().economy().provinces.control[0].raw() <= 79 * kOne + 8);
    CHECK(sim.world().economy().provinces.control[0].raw() >= 79 * kOne - 8);
}

TEST_CASE("F7: unrest lowers the control the province tends to") {
    World world = make_world(2);
    world.economy_columns_mut().unrest[1] = points(50);  // -50 x 0.3 = -15
    Simulation sim(std::move(world));
    install_economy_systems(sim, rules());
    run_months(sim, 120);  // 0.85^120 of 45 points is far below one raw unit: only the truncation is left
    const auto& columns = sim.world().economy().provinces;
    // control approaches its target from above and stops a few raw units short of it (truncation)
    CHECK(columns.control[0].raw() >= 70 * kOne);
    CHECK(columns.control[0].raw() <= 70 * kOne + 8);
    CHECK(columns.control[1].raw() >= 55 * kOne);
    CHECK(columns.control[1].raw() <= 55 * kOne + 8);
}

TEST_CASE("F7: month lengths of 28, 29, 30 and 31 days, Julian leap years included: one step a month") {
    // the stagger covers every day 1..28 of the month: 56 provinces, two per slot day, each with its own
    // start, and after every month each one stands exactly where k hand-written steps put it
    for (const int start_year : {1703, 1707, 1799}) {  // 1704, 1708 and 1800 are Julian leap years
        constexpr std::uint32_t kProvinces = 56;
        World world = make_world(kProvinces, 1000, Date::from_ymd(start_year, 12, 31));
        Pcg32 rng(static_cast<std::uint64_t>(start_year), 3);
        std::array<std::int64_t, kProvinces> control{};
        for (std::uint32_t p = 0; p < kProvinces; ++p) {
            control[p] = rng.range(20 * Fixed32::one_raw, 100 * Fixed32::one_raw);
            world.economy_columns_mut().control[p] = Fixed32::from_raw(static_cast<std::int32_t>(control[p]));
            world.economy_columns_mut().unrest[p] = Fixed32::from_raw(rng.range(0, 50 * Fixed32::one_raw));
        }
        const std::vector<Fixed32> unrest(world.economy().provinces.unrest.begin(),
                                          world.economy().provinces.unrest.end());
        Simulation sim(std::move(world));
        // frozen population and prices do not matter for control; unrest is not touched by T10
        install_economy_systems(sim, rules());
        bool saw_february_of_29 = false;
        for (int month = 1; month <= 12; ++month) {
            const Date first = sim.world().today() + 1;
            saw_february_of_29 =
                saw_february_of_29 || (first.month() == 2 && Date::days_in_month(first.year(), 2) == 29);
            sim.run_days(Date::days_in_month(first.year(), first.month()));
            for (std::uint32_t p = 0; p < kProvinces; ++p) {
                const std::int64_t target =
                    std::clamp<std::int64_t>(70 * kOne - unrest[p].raw() * 300 / 1000, 0, 100 * kOne);
                control[p] += (target - control[p]) * 150 / 1000;
                REQUIRE(sim.world().economy().provinces.control[p].raw() == control[p]);
            }
        }
        CHECK(saw_february_of_29);
        require_valid(sim.world());
    }
}

TEST_CASE("F7: a long chaotic run keeps control and corruption in range and the world valid") {
    constexpr std::uint32_t kProvinces = 40;
    World world = make_world(kProvinces);
    Pcg32 rng(77, 5);
    auto table = std::make_shared<TableSource>();
    for (std::uint32_t p = 0; p < kProvinces; ++p) {
        const CharacterId person = make_person(world, DynastyId(0));
        table->set(person, AdministratorProfile{rng.range(0, 100), rng.range(0, 100)});
        if (rng.chance_permille(700)) {
            REQUIRE(world.set_province_administrator(ProvinceId(p), person));
        }
        world.economy_columns_mut().control[p] = Fixed32::from_raw(rng.range(0, 100 * Fixed32::one_raw));
        world.economy_columns_mut().corruption[p] = Fixed32::from_raw(rng.range(0, Fixed32::one_raw));
        world.economy_columns_mut().unrest[p] = Fixed32::from_raw(rng.range(0, 100 * Fixed32::one_raw));
    }
    Simulation sim(std::move(world));
    install_economy_systems(sim, rules(), nullptr, {}, table);
    for (int month = 0; month < 36; ++month) {
        run_months(sim, 1);
        for (std::uint32_t p = 0; p < kProvinces; ++p) {
            REQUIRE(sim.world().economy().provinces.control[p].raw() >= 0);
            REQUIRE(sim.world().economy().provinces.control[p].raw() <= 100 * kOne);
            REQUIRE(sim.world().economy().provinces.corruption[p].raw() >= 0);
            REQUIRE(sim.world().economy().provinces.corruption[p].raw() <= kOne);
        }
        require_valid(sim.world());
    }
}

// ---- turmoil ---------------------------------------------------------------------------------------

TEST_CASE("anarchy: control below 15 pays the owner nothing and writes an entry in the incident log") {
    World world = make_world(3);
    // province 0: just below 15, province 1: exactly 15 (low, not turmoil), province 2: well above
    world.economy_columns_mut().control[0] = Fixed32::from_raw(15 * kOne - 1);
    world.economy_columns_mut().control[1] = points(15);
    world.economy_columns_mut().control[2] = points(80);
    Simulation sim(std::move(world));
    install_economy_systems(sim, frozen_rules());
    run_months(sim, 1);

    const World& w = sim.world();
    const auto& columns = w.economy().provinces;
    CHECK(columns.income_last_month[0] == 0);
    CHECK(columns.income_last_month[1] > 0);
    CHECK(columns.income_last_month[2] > columns.income_last_month[1]);

    // exactly one entry: province 0, the owner dynasty, the control in whole points rounded down
    REQUIRE(incidents_of(w, IncidentKind::Anarchy) == 1);
    const IncidentLog& log = w.economy().incidents;
    REQUIRE(log.size() == 1);
    CHECK(log.kind[0] == static_cast<std::uint8_t>(IncidentKind::Anarchy));
    CHECK(log.province[0] == ProvinceId(0));
    CHECK(log.dynasty[0] == DynastyId(0));
    CHECK(log.param[0] == 14);
    CHECK(log.date[0] == Date::from_ymd(1703, 1, 1));  // the slot day of province 0

    // the owner's account got the income of provinces 1 and 2 only
    const std::size_t row = *w.resolve_account(AccountRef::of(DynastyId(0)));
    CHECK(w.economy().accounts.treasury(row) == columns.income_last_month[1] + columns.income_last_month[2]);
    require_valid(w);

    // it goes on, one entry per month, until the control is back at 15
    run_months(sim, 2);
    CHECK(incidents_of(sim.world(), IncidentKind::Anarchy) == 3);
}

TEST_CASE("anarchy: the crown's province is in turmoil too, an ownerless one is not worth an entry") {
    World world = make_world(3);
    REQUIRE(world.set_province_owner_kind(ProvinceId(0), OwnerKind::Crown));
    REQUIRE(world.set_province_owner_kind(ProvinceId(1), OwnerKind::Unowned));
    REQUIRE(world.set_province_owner_kind(ProvinceId(2), OwnerKind::Free));
    for (std::size_t p = 0; p < 3; ++p) {
        world.economy_columns_mut().control[p] = points(5);
    }
    Simulation sim(std::move(world));
    install_economy_systems(sim, frozen_rules());
    run_months(sim, 1);
    const IncidentLog& log = sim.world().economy().incidents;
    REQUIRE(log.size() == 1);
    CHECK(log.province[0] == ProvinceId(0));
    CHECK_FALSE(log.dynasty[0].valid());
    CHECK(log.param[0] == 5);
    require_valid(sim.world());
}

TEST_CASE("anarchy: a good administrator brings a province out of turmoil and the entries stop") {
    World world = make_world(1);
    const CharacterId person = make_person(world, DynastyId(0));
    REQUIRE(world.set_province_administrator(ProvinceId(0), person));
    world.economy_columns_mut().control[0] = points(0);
    auto table = std::make_shared<TableSource>();
    table->set(person, AdministratorProfile{100, 100});
    Simulation sim(std::move(world));
    install_economy_systems(sim, rules(), nullptr, {}, table);

    // control moves before the taxes of the slot: 0 -> 15 in the very first month (15 % of the 100
    // points to the target), which is not below 15 any more: the owner is paid again at once
    run_months(sim, 1);
    CHECK(sim.world().economy().provinces.control[0].raw() == 15 * kOne);
    CHECK(incidents_of(sim.world(), IncidentKind::Anarchy) == 0);
    CHECK(sim.world().economy().provinces.income_last_month[0] > 0);

    // with a bad one in a rebellious province it stays down: the target of a clumsy administrator is
    // 40, but unrest of 100 takes 30 points off, and control settles at 10, below 15
    World hard = make_world(1);
    const CharacterId clumsy = make_person(hard, DynastyId(0));
    REQUIRE(hard.set_province_administrator(ProvinceId(0), clumsy));
    hard.economy_columns_mut().control[0] = points(30);
    hard.economy_columns_mut().unrest[0] = points(100);
    auto bad = std::make_shared<TableSource>();
    bad->set(clumsy, AdministratorProfile{0, 0});
    Simulation downward(std::move(hard));
    install_economy_systems(downward, rules(), nullptr, {}, bad);
    run_months(downward, 40);
    CHECK(downward.world().economy().provinces.control[0].raw() < 15 * kOne);
    CHECK(downward.world().economy().provinces.control[0].raw() >= 10 * kOne);
    CHECK(incidents_of(downward.world(), IncidentKind::Anarchy) > 0);
    CHECK(downward.world().economy().provinces.income_last_month[0] == 0);
    require_valid(downward.world());
}

TEST_CASE("a province without economy columns to read is left alone") {
    World bare(Date::from_ymd(1703, 1, 1), 5);
    const Administration none = step_province_administration(bare, rules(), nullptr, ProvinceId(0));
    CHECK(none == Administration{});
    CHECK_FALSE(record_anarchy(bare, ProvinceId(0), true));
    World world = make_world(1);
    CHECK_FALSE(record_anarchy(world, ProvinceId(0), false));
    CHECK_FALSE(record_anarchy(world, ProvinceId(5), true));
}
