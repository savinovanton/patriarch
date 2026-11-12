#include "pat/core/rng.hpp"
#include "pat/sim/calendar.hpp"
#include "pat/sim/command.hpp"
#include "pat/sim/economy/administrator.hpp"
#include "pat/sim/economy/breakdown.hpp"
#include "pat/sim/economy/construction.hpp"
#include "pat/sim/economy/invariants.hpp"
#include "pat/sim/economy/modifiers.hpp"
#include "pat/sim/economy/policy.hpp"
#include "pat/sim/economy/production.hpp"
#include "pat/sim/economy/recruits.hpp"
#include "pat/sim/economy/rules.hpp"
#include "pat/sim/economy/systems.hpp"
#include "pat/sim/economy/taxes.hpp"
#include "pat/sim/replay.hpp"
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

// E9-T17: the effects of buildings and the cache of the province modifiers (docs/dev/economy-model.md
// sections 4.10, 9 F16 and 17.10). What is checked: the set of modifiers of a province (what each of the
// eight types gives, how effects add and clamp, what a construction under way does not give), the
// breakdown of every effect, the dirty flags of every mutator of the World, the property "cache ==
// direct computation" over random sequences of changes, the steps of the slot (output, wealth, control,
// corruption, recruits, taxes) reading the effects, save/load/replay, and the months of 28 to 31 days.

using namespace pat::sim;
using namespace pat::sim::economy;
using pat::core::Fixed32;
using pat::core::Pcg32;
using pat::core::StringId;

namespace {

constexpr std::int64_t kOne = Fixed32::one_raw;

constexpr std::uint8_t kMill = 1;
constexpr std::uint8_t kTract = 4;
constexpr std::uint8_t kWharf = 5;
constexpr std::uint8_t kOstrog = 6;
constexpr std::uint8_t kChurch = 7;
constexpr std::uint8_t kGranary = 8;

const EconomyRules& rules() {
    static const EconomyRules value = EconomyRules::defaults();
    return value;
}

/// Frozen dynamics of mood, unrest and riots: what the steps read does not depend on the history.
EconomyRules calm_rules() {
    EconomyRules r = EconomyRules::defaults();
    r.mood.inertia_permille = 0;
    r.unrest.inertia_permille = 0;
    r.unrest.riot_chance_min_permille = 0;
    r.unrest.riot_chance_max_permille = 0;
    return r;
}

/// The same rules with every type allowed in every kind of province (E9-T16 requirements off), so that a
/// test may put any of the eight types anywhere and still pass the invariants of level R.
EconomyRules open_rules(EconomyRules r = EconomyRules::defaults()) {
    for (BuildingDef& def : r.buildings.defs) {
        def.allowed_kinds = 0;
        def.max_per_province = static_cast<std::int32_t>(kMaxSlots);
    }
    return r;
}

void require_valid(const World& world, const EconomyRules& with = rules()) {
    const auto problems = world.check_invariants();
    for (const auto& problem : problems) {
        MESSAGE(problem);
    }
    REQUIRE(problems.empty());
    const auto broken = check_rules_invariants(world, with);
    for (const auto& problem : broken) {
        MESSAGE(problem);
    }
    REQUIRE(broken.empty());
}

/// Three dynasties; `provinces` provinces of dynasty 0 with `slots` slots, development 4 and 8000
/// peasants (labour 1000), economy on, policy levels bound to the rules.
World make_world(std::uint32_t provinces = 4,
                 std::uint8_t slots = 6,
                 Date start = Date::from_ymd(1702, 12, 31)) {
    World world(start, 5);
    for (std::uint32_t d = 0; d < 3; ++d) {
        (void)world.create_dynasty(StringId(1 + d));
    }
    for (std::uint32_t p = 0; p < provinces; ++p) {
        (void)world.create_province(StringId(100 + p), DynastyId(0), Fixed32::from_int(1));
    }
    REQUIRE(world.enable_economy());
    bind_policy_levels(world, rules());
    EconomyColumnsMut columns = world.economy_columns_mut();
    for (std::uint32_t p = 0; p < provinces; ++p) {
        columns.slots_total[p] = slots;
        columns.labour_base[p] = 8000;
        world.set_province_development(ProvinceId(p), 4);
        (void)world.set_group_size(ProvinceId(p), Group::Peasants, 8000);
    }
    require_valid(world);
    return world;
}

CharacterId make_person(World& world, DynastyId dynasty, std::int32_t stewardship = 10) {
    CharacterInit init;
    init.dynasty = dynasty;
    init.birth_date = Date::from_ymd(1670, 5, 1);
    init.skills[static_cast<std::size_t>(Skill::Stewardship)] = Fixed32::from_int(stewardship);
    return world.create_character(init);
}

/// The profile follows the skills of the character, as E7 will make it (Stewardship 0..20 x5).
class SkillSource final : public AdministratorSource {
public:
    [[nodiscard]] AdministratorProfile profile(const World& world, CharacterId who) const override {
        const Fixed32 skill =
            world.characters().skills[who.index()][static_cast<std::size_t>(Skill::Stewardship)];
        AdministratorProfile out;
        out.stewardship = skill.to_int() * 5;
        out.integrity = 100 - skill.to_int() * 3;
        return out;
    }
};

ProvinceId pid(std::uint32_t p) {
    return ProvinceId(p);
}

ProvinceModifiers modifiers(const World& world, std::uint32_t province, const EconomyRules& with = rules()) {
    return compute_province_modifiers(world, with, nullptr, pid(province));
}

void build(World& world, std::uint32_t province, std::size_t slot, std::uint8_t type) {
    REQUIRE(world.set_slot_building(pid(province), slot, type));
}

Simulation economy_sim(World world, const EconomyRules& with = rules()) {
    Simulation sim(std::move(world));
    install_economy_systems(sim, with);
    return sim;
}

std::size_t clean_provinces(const World& world) {
    std::size_t clean = 0;
    for (std::size_t p = 0; p < world.provinces().size(); ++p) {
        clean += world.modifier_cache().clean(p) ? 1 : 0;
    }
    return clean;
}

/// Every province: the answer the consumers get equals the direct computation, and a clean entry equals
/// it too. Returns the first difference.
std::string cache_problem(const World& world, const EconomyRules& with, const AdministratorSource* source) {
    for (std::size_t p = 0; p < world.provinces().size(); ++p) {
        const ProvinceModifiers direct =
            compute_province_modifiers(world, with, source, pid(static_cast<std::uint32_t>(p)));
        if (province_modifiers(world, with, source, pid(static_cast<std::uint32_t>(p))) != direct) {
            return "province " + std::to_string(p) +
                   ": the answer of the cache differs from the direct computation";
        }
        if (world.modifier_cache().clean(p) && world.modifier_cache().value(p) != direct) {
            return "province " + std::to_string(p) + ": a clean entry differs from the direct computation";
        }
    }
    return {};
}

}  // namespace

// ---- the data -------------------------------------------------------------------------------------------

TEST_CASE("modifiers: the eight types carry the effects of GDD 03 section 8.1") {
    const BuildingRules& b = rules().buildings;
    const auto def = [&](std::uint8_t type) -> const BuildingDef& {
        const BuildingDef* found = find_building(b, type);
        REQUIRE(found != nullptr);
        return *found;
    };
    // economic buildings: output; infrastructure and military: control; the military: recruits;
    // religious: oversight; social (the granary): protection from famine
    CHECK(def(kMill).output_permille[0] == 1150);
    CHECK(def(kTract).control_points == 3);
    CHECK(def(kOstrog).control_points == 5);
    CHECK(def(kOstrog).recruit_permille == 1250);
    CHECK(def(kChurch).control_points == 2);
    CHECK(def(kChurch).oversight_permille == 30);
    CHECK(def(kGranary).famine_protect_permille == 300);
    // the others are neutral in every effect they do not name
    CHECK(def(kMill).control_points == 0);
    CHECK(def(kMill).recruit_permille == 1000);
    CHECK(def(kMill).famine_protect_permille == 0);
    CHECK(def(kGranary).recruit_permille == 1000);
    CHECK(def(kTract).oversight_permille == 0);
}

TEST_CASE("modifiers: the rules reject effects out of their range") {
    const auto broken = [](auto&& change) {
        EconomyRules r = EconomyRules::defaults();
        change(r.buildings.defs[0]);
        return validate(r);
    };
    CHECK(broken([](BuildingDef& d) { d.control_points = 50; }).has_value());
    CHECK_FALSE(broken([](BuildingDef& d) { d.control_points = 51; }).has_value());
    CHECK_FALSE(broken([](BuildingDef& d) { d.control_points = -51; }).has_value());
    CHECK(broken([](BuildingDef& d) { d.oversight_permille = 1000; }).has_value());
    CHECK_FALSE(broken([](BuildingDef& d) { d.oversight_permille = 1001; }).has_value());
    CHECK_FALSE(broken([](BuildingDef& d) { d.oversight_permille = -1; }).has_value());
    CHECK(broken([](BuildingDef& d) { d.recruit_permille = kMaxMultiplierPermille; }).has_value());
    CHECK_FALSE(broken([](BuildingDef& d) { d.recruit_permille = kMaxMultiplierPermille + 1; }).has_value());
    CHECK(broken([](BuildingDef& d) { d.famine_protect_permille = 1000; }).has_value());
    CHECK_FALSE(broken([](BuildingDef& d) { d.famine_protect_permille = 1001; }).has_value());
    const auto error = broken([](BuildingDef& d) { d.control_points = 99; });
    REQUIRE_FALSE(error.has_value());
    CHECK(error.error().code == EconomyRulesErrorCode::Buildings);
}

TEST_CASE("modifiers: the codes of terms of buildings are one range, outside the table of keys") {
    CHECK(term::building_term(1) == 4101);
    CHECK(term::building_term(255) == 4355);
    CHECK(term::is_building_term(term::building_term(8)));
    CHECK_FALSE(term::is_building_term(term::kOutputBuildingsLimit));
    CHECK_FALSE(term::is_building_term(term::kBuildingTermFirst));
    CHECK_FALSE(term::is_building_term(4356));
    CHECK(term::building_of_term(term::building_term(7)) == 7);
    CHECK(term::building_of_term(term::kOutputBuildingsTotal) == 0);
    // the code of a building is data: it has no row in the table of localisation keys
    CHECK(term_key(term::building_term(1)).empty());
    for (const std::uint16_t code : {term::kOutputBuildingsTotal,
                                     term::kOutputBuildingsBase,
                                     term::kOutputBuildingsLimit,
                                     term::kWealthFamineTotal,
                                     term::kWealthFamineLimit,
                                     term::kControlBuildingsTotal,
                                     term::kCorruptionOversightTotal,
                                     term::kCorruptionOversightLimit,
                                     term::kRecruitBuildings,
                                     term::kRecruitBuildingsTotal,
                                     term::kRecruitBuildingsBase,
                                     term::kRecruitBuildingsLimit}) {
        CAPTURE(code);
        CHECK_FALSE(term_key(code).empty());
        CHECK_FALSE(term::is_building_term(code));
    }
}

// ---- the set of a province --------------------------------------------------------------------------------

TEST_CASE("modifiers: a province without buildings, policy or administrator is neutral") {
    World world = make_world();
    const ProvinceModifiers m = modifiers(world, 0);
    ProvinceModifiers neutral;
    neutral.tax_level = static_cast<std::uint8_t>(rules().tax.default_level);
    neutral.recruit_level = static_cast<std::uint8_t>(rules().recruit.default_level);
    CHECK(m == neutral);
    for (const std::int16_t output : m.output_permille) {
        CHECK(output == 1000);
    }
    // a world without economy and an unknown province: the neutral set too
    const World bare(Date::from_ymd(1703, 1, 1), 1);
    CHECK(compute_province_modifiers(bare, rules(), nullptr, pid(0)).control_points == 0);
    CHECK(compute_province_modifiers(world, rules(), nullptr, pid(99)).recruit_permille == 1000);
    World without_economy(Date::from_ymd(1703, 1, 1), 1);
    CHECK(refresh_modifier_cache(without_economy, rules(), nullptr) == 0);
}

TEST_CASE("modifiers: every type gives what its definition says, and only a ready building gives it") {
    World world = make_world();
    build(world, 0, 0, kMill);
    ProvinceModifiers m = modifiers(world, 0);
    CHECK(m.output_permille[0] == 1150);
    CHECK(m.output_permille[7] == 1000);
    CHECK(m.control_points == 0);

    build(world, 0, 1, kOstrog);
    build(world, 0, 2, kChurch);
    build(world, 0, 3, kGranary);
    build(world, 0, 4, kTract);
    build(world, 0, 5, kWharf);
    m = modifiers(world, 0);
    CHECK(m.output_permille[0] == 1150);
    CHECK(m.output_permille[8] == 1100);                       // the wharf: fish
    CHECK(m.control_points == 5 + 2 + 3);                      // ostrog, church, tract
    CHECK(m.oversight_permille == 30);                         // the church
    CHECK(m.recruit_permille == 1250);                         // the ostrog
    CHECK(m.famine_protect_permille == 300);                   // the granary
    CHECK(modifiers(world, 1) == modifiers(make_world(), 1));  // another province: untouched

    // a construction under way gives nothing until it is ready (like it costs no upkeep)
    World building = make_world(1, 3);
    (void)building.post_money(AccountRef::of(DynastyId(0)), LedgerCode::Adjustment, 100'000'000);
    REQUIRE(start_construction(building, rules(), DynastyId(0), pid(0), kTract).has_value());
    CHECK(modifiers(building, 0).control_points == 0);
    CHECK(modifiers(building, 0) == modifiers(make_world(), 0));
}

TEST_CASE("modifiers: effects add as deltas around 1000 in any order, and are clamped to their range") {
    EconomyRules r = EconomyRules::defaults();
    // three made-up types of the same good and the same effects
    for (std::size_t i = 0; i < 3; ++i) {
        BuildingDef& d = r.buildings.defs[i];
        d.output_permille.fill(1000);
        d.output_permille[0] = 1500;
        d.control_points = 20;
        d.oversight_permille = 400;
        d.recruit_permille = 1600;
        d.famine_protect_permille = 450;
        d.max_per_province = 6;
    }
    REQUIRE(validate(r).has_value());
    World a = make_world();
    World b = make_world();
    build(a, 0, 0, 1);
    build(a, 0, 1, 2);
    build(a, 0, 2, 3);
    build(b, 0, 0, 3);
    build(b, 0, 1, 1);
    build(b, 0, 2, 2);
    const ProvinceModifiers sum = modifiers(a, 0, r);
    CHECK(sum == modifiers(b, 0, r));  // the order of the slots does not matter
    CHECK(sum.output_permille[0] == 1000 + 3 * 500);
    CHECK(sum.control_points == 60);
    CHECK(sum.oversight_permille == 1000);  // 1200 clamped
    CHECK(sum.recruit_permille == 1000 + 3 * 600);
    CHECK(sum.famine_protect_permille == 1000);  // 1350 clamped

    // the multiplier cannot go below zero or above the limit of the rules
    EconomyRules low = EconomyRules::defaults();
    for (std::size_t i = 0; i < 3; ++i) {
        low.buildings.defs[i].output_permille.fill(1000);
        low.buildings.defs[i].output_permille[0] = 200;
        low.buildings.defs[i].recruit_permille = 9000;
        low.buildings.defs[i].max_per_province = 6;
    }
    REQUIRE(validate(low).has_value());
    const ProvinceModifiers clamped = modifiers(a, 0, low);
    CHECK(clamped.output_permille[0] == 0);  // 1000 - 3 x 800
    CHECK(clamped.recruit_permille == kMaxMultiplierPermille);

    // a type the rules do not define is ignored (a world is checked against the rules by E-R1)
    World stray = make_world();
    build(stray, 0, 0, 200);
    CHECK(modifiers(stray, 0) == modifiers(make_world(), 0));
}

TEST_CASE("modifiers: the policy of the owner and the administrator are part of the set") {
    World world = make_world();
    const std::uint8_t default_tax = static_cast<std::uint8_t>(rules().tax.default_level);
    CHECK(modifiers(world, 0).tax_level == default_tax);
    REQUIRE(world.set_dynasty_tax_policy(DynastyId(0), 3));
    REQUIRE(world.set_dynasty_recruit_policy(DynastyId(0), 2));
    CHECK(modifiers(world, 0).tax_level == 3);
    CHECK(modifiers(world, 0).recruit_level == 2);
    // a decree of the province itself wins over the owner
    world.economy_columns_mut().tax_policy[1] = 0;
    CHECK(modifiers(world, 1).tax_level == 0);
    // a province that changes hands follows its new owner (dynasty 1 has not chosen: the default)
    world.set_province_owner(pid(2), DynastyId(1));
    CHECK(modifiers(world, 2).tax_level == default_tax);
    REQUIRE(world.set_dynasty_tax_policy(DynastyId(1), 1));
    CHECK(modifiers(world, 2).tax_level == 1);

    // the profile of the administrator: neutral for nobody, from the source for somebody
    const SkillSource skills;
    const CharacterId boss = make_person(world, DynastyId(0), 12);
    CHECK(compute_province_modifiers(world, rules(), &skills, pid(0)).stewardship == 50);
    REQUIRE(world.set_province_administrator(pid(0), boss));
    const ProvinceModifiers governed = compute_province_modifiers(world, rules(), &skills, pid(0));
    CHECK(governed.stewardship == 60);
    CHECK(governed.integrity == 64);
    CHECK(governed.profile() == AdministratorProfile{.stewardship = 60, .integrity = 64});
    // the stub (no source) gives everybody the neutral profile
    CHECK(compute_province_modifiers(world, rules(), nullptr, pid(0)).profile() == AdministratorProfile{});
    // a dead administrator is removed, and the set goes back to the neutral profile
    REQUIRE(world.destroy_character(boss));
    CHECK(compute_province_modifiers(world, rules(), &skills, pid(0)).stewardship == 50);
}

// ---- breakdown --------------------------------------------------------------------------------------------

TEST_CASE("modifiers: every effect explains itself, building by building") {
    World world = make_world();
    build(world, 0, 0, kMill);
    build(world, 0, 1, kOstrog);
    build(world, 0, 2, kChurch);
    build(world, 0, 3, kGranary);
    build(world, 0, 4, kTract);
    const ProvinceModifiers m = modifiers(world, 0);

    Breakdown output = explain_modifier(world, rules(), pid(0), ModifierKind::Output, 0);
    CHECK(output.is_consistent());
    CHECK(output.root().code == term::kOutputBuildingsTotal);
    CHECK(output.total() == m.output_permille[0]);
    CHECK(output.node(output.find(term::kOutputBuildingsBase)).value == 1000);
    const NodeIndex mill = output.find(term::building_term(kMill));
    REQUIRE(mill != kNoNode);
    CHECK(output.node(mill).value == 150);
    CHECK(output.node(mill).factor_permille == 1150);
    CHECK(output.find(term::building_term(kOstrog)) == kNoNode);  // no effect on grain: no term
    CHECK(explain_modifier(world, rules(), pid(0), ModifierKind::Output, 8).total() == 1000);

    Breakdown control = explain_modifier(world, rules(), pid(0), ModifierKind::Control);
    CHECK(control.is_consistent());
    CHECK(control.root().code == term::kControlBuildingsTotal);
    CHECK(control.root().unit == Unit::Points);
    CHECK(control.total() == m.control_points * kOne);
    CHECK(control.node(control.find(term::building_term(kOstrog))).value == 5 * kOne);
    CHECK(control.node(control.find(term::building_term(kTract))).value == 3 * kOne);
    CHECK(control.node(control.find(term::building_term(kChurch))).value == 2 * kOne);
    CHECK(control.child_count(kRootNode) == 3);

    Breakdown oversight = explain_modifier(world, rules(), pid(0), ModifierKind::Oversight);
    CHECK(oversight.is_consistent());
    CHECK(oversight.root().code == term::kCorruptionOversightTotal);
    CHECK(oversight.total() == m.oversight_permille);
    CHECK(oversight.node(oversight.find(term::building_term(kChurch))).value == 30);

    Breakdown recruits = explain_modifier(world, rules(), pid(0), ModifierKind::Recruits);
    CHECK(recruits.is_consistent());
    CHECK(recruits.root().code == term::kRecruitBuildingsTotal);
    CHECK(recruits.total() == m.recruit_permille);
    CHECK(recruits.node(recruits.find(term::building_term(kOstrog))).factor_permille == 1250);

    Breakdown famine = explain_modifier(world, rules(), pid(0), ModifierKind::FamineProtection);
    CHECK(famine.is_consistent());
    CHECK(famine.root().code == term::kWealthFamineTotal);
    CHECK(famine.total() == m.famine_protect_permille);
    CHECK(famine.child_count(kRootNode) == 1);

    // nothing to explain: an unknown province, a good out of range, a world without economy
    CHECK(explain_modifier(world, rules(), pid(77), ModifierKind::Control).empty());
    CHECK(explain_modifier(world, rules(), pid(0), ModifierKind::Output, kMaxGoods).empty());
    CHECK(explain_modifier(World(Date::from_ymd(1703, 1, 1), 1), rules(), pid(0), ModifierKind::Control)
              .empty());
}

TEST_CASE("modifiers: a clamped sum shows the limit as a term and still adds up") {
    EconomyRules r = EconomyRules::defaults();
    for (std::size_t i = 0; i < 3; ++i) {
        r.buildings.defs[i].oversight_permille = 600;
        r.buildings.defs[i].famine_protect_permille = 600;
        r.buildings.defs[i].recruit_permille = 9000;
        r.buildings.defs[i].output_permille.fill(1000);
        r.buildings.defs[i].output_permille[0] = 200;
        r.buildings.defs[i].max_per_province = 6;
    }
    REQUIRE(validate(r).has_value());
    World world = make_world();
    build(world, 0, 0, 1);
    build(world, 0, 1, 2);
    build(world, 0, 2, 3);
    const Breakdown oversight = explain_modifier(world, r, pid(0), ModifierKind::Oversight);
    CHECK(oversight.is_consistent());
    CHECK(oversight.total() == 1000);
    CHECK(oversight.node(oversight.find(term::kCorruptionOversightLimit)).value == -800);
    const Breakdown famine = explain_modifier(world, r, pid(0), ModifierKind::FamineProtection);
    CHECK(famine.is_consistent());
    CHECK(famine.node(famine.find(term::kWealthFamineLimit)).value == -800);
    const Breakdown recruits = explain_modifier(world, r, pid(0), ModifierKind::Recruits);
    CHECK(recruits.is_consistent());
    CHECK(recruits.total() == kMaxMultiplierPermille);
    CHECK(recruits.node(recruits.find(term::kRecruitBuildingsLimit)).value ==
          kMaxMultiplierPermille - (1000 + 3 * 8000));
    const Breakdown output = explain_modifier(world, r, pid(0), ModifierKind::Output, 0);
    CHECK(output.is_consistent());
    CHECK(output.total() == 0);
    CHECK(output.node(output.find(term::kOutputBuildingsLimit)).value == 1400);
    // the totals are the fields of the set, under the same rules
    const ProvinceModifiers m = modifiers(world, 0, r);
    CHECK(m.oversight_permille == 1000);
    CHECK(m.output_permille[0] == 0);
    CHECK(m.recruit_permille == kMaxMultiplierPermille);
}

// ---- the dirty flags --------------------------------------------------------------------------------------

TEST_CASE("cache: a new world is cold, refresh fills it, nothing is saved or hashed") {
    World world = make_world(6);
    CHECK(world.modifier_cache().clean_count() == 0);
    CHECK(clean_provinces(world) == 0);
    const WorldHash before = world.compute_hash();
    CHECK(refresh_modifier_cache(world, rules(), nullptr) == 6);
    CHECK(clean_provinces(world) == 6);
    CHECK(refresh_modifier_cache(world, rules(), nullptr) == 0);  // nothing dirty: nothing done
    CHECK(world.compute_hash() == before);                        // derived data is not in the hash
    CHECK(cache_problem(world, rules(), nullptr).empty());
    CHECK(rebuild_modifier_cache(world, rules(), nullptr) == 6);
    CHECK(world.modifier_cache().stamp() == modifier_rules_stamp(rules()));
    CHECK(world.compute_hash() == before);
    // a copy of the world carries the cache (it is part of the object, not of the state)
    const World copy = world;
    CHECK(copy.modifier_cache().clean_count() == 6);
    CHECK(copy.compute_hash() == before);
}

TEST_CASE("cache: every mutator marks exactly the provinces whose inputs it changes") {
    const auto fresh = [] {
        World world = make_world(5, 6);
        world.set_province_owner(pid(4), DynastyId(1));
        (void)refresh_modifier_cache(world, rules(), nullptr);
        REQUIRE(clean_provinces(world) == 5);
        return world;
    };
    const auto dirty_set = [](const World& world) {
        std::vector<std::size_t> dirty;
        for (std::size_t p = 0; p < world.provinces().size(); ++p) {
            if (!world.modifier_cache().clean(p)) {
                dirty.push_back(p);
            }
        }
        return dirty;
    };
    using Dirty = std::vector<std::size_t>;

    {  // a building appears or goes
        World world = fresh();
        REQUIRE(world.set_slot_building(pid(2), 1, kMill));
        CHECK(dirty_set(world) == Dirty{2});
        REQUIRE(refresh_province_modifiers(world, rules(), nullptr, pid(2)));
        CHECK_FALSE(refresh_province_modifiers(world, rules(), nullptr, pid(2)));  // clean already
        CHECK(world.set_slot_building(pid(2), 1, 0));
        CHECK(dirty_set(world) == Dirty{2});
        // a refused change changes nothing
        (void)refresh_modifier_cache(world, rules(), nullptr);
        CHECK_FALSE(world.set_slot_building(pid(2), kMaxSlots, kMill));
        CHECK(dirty_set(world).empty());
    }
    {  // a construction that finishes
        World world = make_world(2, 3, Date::from_ymd(1703, 1, 1));
        (void)world.post_money(AccountRef::of(DynastyId(0)), LedgerCode::Adjustment, 100'000'000);
        REQUIRE(start_construction(world, rules(), DynastyId(0), pid(1), kGranary).has_value());
        (void)refresh_modifier_cache(world, rules(), nullptr);
        Simulation sim = economy_sim(std::move(world));
        sim.run_days(rules().buildings.defs[7].build_days + 1);
        CHECK(sim.world().economy().provinces.slot_building[1 * kMaxSlots] == kGranary);
        CHECK(sim.world().modifier_cache().clean(1));
        CHECK(sim.world().modifier_cache().value(1).famine_protect_permille == 300);
    }
    {  // the policy of a dynasty: every province of that dynasty and no other
        World world = fresh();
        REQUIRE(world.set_dynasty_tax_policy(DynastyId(0), 3));
        CHECK(dirty_set(world) == Dirty{0, 1, 2, 3});
        (void)refresh_modifier_cache(world, rules(), nullptr);
        REQUIRE(world.set_dynasty_recruit_policy(DynastyId(1), 2));
        CHECK(dirty_set(world) == Dirty{4});
        (void)refresh_modifier_cache(world, rules(), nullptr);
        CHECK_FALSE(world.set_dynasty_tax_policy(DynastyId(0), 9));  // refused: nothing marked
        CHECK(dirty_set(world).empty());
    }
    {  // the owner
        World world = fresh();
        world.set_province_owner(pid(1), DynastyId(2));
        CHECK(dirty_set(world) == Dirty{1});
        (void)refresh_modifier_cache(world, rules(), nullptr);
        REQUIRE(world.set_province_owner_kind(pid(3), OwnerKind::Crown));
        CHECK(dirty_set(world) == Dirty{3});
    }
    {  // the administrator
        World world = fresh();
        const CharacterId boss = make_person(world, DynastyId(0));
        const CharacterId other = make_person(world, DynastyId(0));
        REQUIRE(world.set_province_administrator(pid(0), boss));
        REQUIRE(world.set_province_administrator(pid(2), boss));
        CHECK(dirty_set(world) == Dirty{0, 2});
        (void)refresh_modifier_cache(world, rules(), nullptr);
        world.set_character_skill(other, Skill::Stewardship, Fixed32::from_int(3));  // administers nothing
        CHECK(dirty_set(world).empty());
        world.set_character_skill(boss, Skill::Stewardship, Fixed32::from_int(18));
        CHECK(dirty_set(world) == Dirty{0, 2});
        (void)refresh_modifier_cache(world, rules(), nullptr);
        const std::array<std::uint16_t, 1> traits{3};
        REQUIRE(world.set_character_traits(boss, traits));
        CHECK(dirty_set(world) == Dirty{0, 2});
        (void)refresh_modifier_cache(world, rules(), nullptr);
        // handing a province to a house that is not the administrator's drops him: the province is marked
        world.set_province_owner(pid(0), DynastyId(1));
        CHECK(world.economy().provinces.administrator[0] == CharacterId());
        CHECK(dirty_set(world) == Dirty{0});
        (void)refresh_modifier_cache(world, rules(), nullptr);
        // the administrator dies: every province he governed is marked
        REQUIRE(world.destroy_character(boss));
        CHECK(world.economy().provinces.administrator[2] == CharacterId());
        CHECK(dirty_set(world) == Dirty{2});
    }
    {  // a new province, the explicit invalidations
        World world = fresh();
        (void)world.create_province(StringId(500), DynastyId(0), Fixed32::from_int(1));
        CHECK(dirty_set(world) == Dirty{5});
        (void)refresh_modifier_cache(world, rules(), nullptr);
        world.economy_columns_mut().tax_policy[1] = 2;  // a writer the world cannot see ...
        CHECK(dirty_set(world).empty());
        CHECK_FALSE(cache_problem(world, rules(), nullptr).empty());  // the clean entry is now wrong
        world.invalidate_province_modifiers(pid(1));                  // ... says so itself
        CHECK(dirty_set(world) == Dirty{1});
        CHECK(cache_problem(world, rules(), nullptr).empty());
        world.invalidate_province_modifiers(pid(99));  // unknown: nothing
        world.invalidate_all_province_modifiers();
        CHECK(dirty_set(world).size() == 6);
    }
}

TEST_CASE("cache: a forgotten flag is caught by the invariant E-R3 and by the answer of a clean entry") {
    World world = make_world(3);
    (void)refresh_modifier_cache(world, rules(), nullptr);
    require_valid(world);
    world.economy_columns_mut().tax_policy[1] = 3;  // a province decree, no invalidation
    const auto broken = check_rules_invariants(world, rules());
    REQUIRE_FALSE(broken.empty());
    CHECK(broken[0].find("province 1") != std::string::npos);
    CHECK(broken[0].find("modifier cache") != std::string::npos);
    world.invalidate_province_modifiers(pid(1));
    require_valid(world);
}

TEST_CASE("cache: the system notices other rules and recomputes under them") {
    EconomyRules strong = EconomyRules::defaults();
    strong.buildings.defs[0].output_permille[0] = 1300;
    REQUIRE(validate(strong).has_value());
    CHECK(modifier_rules_stamp(strong) != modifier_rules_stamp(rules()));
    CHECK(modifier_rules_stamp(strong) == modifier_rules_stamp(strong));
    EconomyRules other_default = EconomyRules::defaults();
    other_default.tax.default_level = 2;
    CHECK(modifier_rules_stamp(other_default) != modifier_rules_stamp(rules()));

    World world = make_world(2);
    build(world, 0, 0, kMill);
    (void)rebuild_modifier_cache(world, rules(), nullptr);  // entries made under the old rules
    CHECK(world.modifier_cache().value(0).output_permille[0] == 1150);
    // a hot reload: new rules, the same world; the first tick drops the old entries
    Simulation second(std::move(world));
    install_economy_systems(second, strong);
    (void)second.tick();
    CHECK(second.world().modifier_cache().value(0).output_permille[0] == 1300);
    CHECK(cache_problem(second.world(), strong, nullptr).empty());
}

// ---- the property: the cache equals the direct computation
// ---------------------------------------------------

TEST_CASE("cache: over random changes of buildings, policies, administrators and owners it never differs "
          "from the direct computation") {
    const SkillSource skills;
    const EconomyRules open = open_rules();
    std::size_t refreshed = 0;
    std::size_t checked = 0;
    for (std::uint64_t seed = 1; seed <= 40; ++seed) {
        CAPTURE(seed);
        Pcg32 rng(seed, 0x17);
        World world = make_world(12, 6);
        std::vector<CharacterId> people;
        for (std::uint32_t i = 0; i < 6; ++i) {
            people.push_back(
                make_person(world, DynastyId(i % 3), static_cast<std::int32_t>(rng.bounded(21))));
        }
        for (int step = 0; step < 150; ++step) {
            CAPTURE(step);
            const ProvinceId province = pid(rng.bounded(12));
            const std::uint32_t roll = rng.bounded(100);
            if (roll < 30) {  // build or demolish
                (void)world.set_slot_building(
                    province, rng.bounded(7), static_cast<std::uint8_t>(rng.bounded(9)));
            } else if (roll < 40) {
                (void)world.set_dynasty_tax_policy(DynastyId(rng.bounded(3)),
                                                   static_cast<std::uint8_t>(rng.bounded(4)));
            } else if (roll < 50) {
                (void)world.set_dynasty_recruit_policy(DynastyId(rng.bounded(3)),
                                                       static_cast<std::uint8_t>(rng.bounded(3)));
            } else if (roll < 58) {
                world.set_province_owner(province,
                                         rng.chance_permille(150) ? DynastyId() : DynastyId(rng.bounded(3)));
            } else if (roll < 63) {
                (void)world.set_province_owner_kind(province, static_cast<OwnerKind>(rng.bounded(5)));
            } else if (roll < 75) {
                const CharacterId who = rng.chance_permille(200) ? CharacterId() : people[rng.bounded(6)];
                (void)world.set_province_administrator(province, who);
            } else if (roll < 82) {
                world.set_character_skill(people[rng.bounded(6)],
                                          Skill::Stewardship,
                                          Fixed32::from_int(static_cast<std::int32_t>(rng.bounded(21))));
            } else if (roll < 85) {
                world.economy_columns_mut().tax_policy[province.index()] =
                    rng.chance_permille(500) ? kPolicyDefault : static_cast<std::uint8_t>(rng.bounded(4));
                world.economy_columns_mut().recruit_policy[province.index()] =
                    rng.chance_permille(500) ? kPolicyDefault : static_cast<std::uint8_t>(rng.bounded(3));
                world.invalidate_province_modifiers(
                    province);  // the protocol of a writer the world cannot see
            } else if (roll < 90) {
                refreshed += refresh_province_modifiers(world, open, &skills, province) ? 1 : 0;
            } else if (roll < 96) {
                refreshed += refresh_modifier_cache(world, open, &skills);
            } else if (roll < 98) {
                refreshed += rebuild_modifier_cache(world, open, &skills);
            } else if (rng.chance_permille(300) && !people.empty()) {
                const std::size_t i = rng.bounded(static_cast<std::uint32_t>(people.size()));
                (void)world.destroy_character(people[i]);  // gone from every province he governed
                people[i] = make_person(world, DynastyId(i % 3), static_cast<std::int32_t>(rng.bounded(21)));
            }
            const std::string problem = cache_problem(world, open, &skills);
            REQUIRE_MESSAGE(problem.empty(), problem);
            checked += world.provinces().size();
        }
        // the invariant of level R agrees (it leaves the profile out, so it holds for any source)
        const auto broken = check_rules_invariants(world, open);
        for (const auto& line : broken) {
            MESSAGE(line);
        }
        CHECK(broken.empty());
        // and a full rebuild at the end gives the same sets as the entries the run kept
        std::vector<ProvinceModifiers> kept;
        for (std::size_t p = 0; p < 12; ++p) {
            kept.push_back(province_modifiers(world, open, &skills, pid(static_cast<std::uint32_t>(p))));
        }
        (void)rebuild_modifier_cache(world, open, &skills);
        for (std::size_t p = 0; p < 12; ++p) {
            CHECK(world.modifier_cache().clean(p));
            CHECK(world.modifier_cache().value(p) == kept[p]);
        }
    }
    CHECK(refreshed > 500);  // the sequences do refill the cache, they do not just leave it cold
    CHECK(checked == 40U * 150U * 12U);
}

TEST_CASE("cache: a tick refreshes what was marked, so the steps of the slot read a clean entry") {
    World world = make_world(30, 6, Date::from_ymd(1702, 12, 31));
    for (std::uint32_t p = 0; p < 30; ++p) {
        build(world, p, 0, static_cast<std::uint8_t>(1 + p % 8));
    }
    Simulation sim = economy_sim(std::move(world));
    CHECK(sim.world().modifier_cache().clean_count() == 0);  // cold, like after a load
    (void)sim.tick();
    CHECK(clean_provinces(sim.world()) == 30);  // the system rebuilt everything on its first day
    CHECK(cache_problem(sim.world(), rules(), nullptr).empty());
}

// ---- the steps read the effects
// ---------------------------------------------------------------------------------

TEST_CASE("steps: F3, the output of a province, reads the multiplier of the buildings") {
    const EconomyRules r = calm_rules();
    World with = make_world(1);
    World without = make_world(1);
    for (World* w : {&with, &without}) {
        w->economy_columns_mut().capacity[0] = Fixed32::from_int(100);
        w->economy_columns_mut().grain_stock[0] = Fixed32::from_int(500);
    }
    build(with, 0, 0, kMill);
    const StaticPriceSource prices(r.production);
    const ProvinceProduction plain = step_province_production(without, r, prices, pid(0));
    const ProvinceProduction mill = step_province_production(with, r, prices, pid(0));
    CHECK(mill.output[kGrainGood] > plain.output[kGrainGood]);
    // exactly the formula with 1150 in the place of the buildings (control is the default 70 points)
    const ProvinceModifiers m = modifiers(with, 0, r);
    ProductionInput in = production_input(without, prices, pid(0), &m);
    CHECK(in.buildings_permille[kGrainGood] == 1150);
    CHECK(in.buildings_permille[1] == 1000);
    const ProvinceProduction expected = compute_province_production(r, in);
    CHECK(expected.output[kGrainGood] == mill.output[kGrainGood]);
    CHECK(expected.holding_income_kop == mill.holding_income_kop);
    // the breakdown shows the step "buildings" with the multiplier
    const Breakdown explained = explain_output(r, in, kGrainGood);
    const NodeIndex step = explained.find(term::kOutputBuildings);
    REQUIRE(step != kNoNode);
    CHECK(explained.node(step).factor_permille == 1150);
    CHECK(explained.node(step).value > 0);
    // without the set the buildings have no effect (the old way to build the input)
    CHECK(production_input(with, prices, pid(0)).buildings_permille[kGrainGood] == 1000);
}

TEST_CASE("steps: F5, a granary takes a share of a grain shortage off the wealth of the people") {
    const EconomyRules r = calm_rules();
    World with = make_world(1);
    World without = make_world(1);
    build(with, 0, 0, kGranary);
    const StaticPriceSource prices(r.production);  // no grain made, no stock: the people go hungry
    const ProvinceModifiers m = modifiers(with, 0, r);
    ProductionInput in = production_input(with, prices, pid(0), &m);
    CHECK(in.famine_protect_permille == 300);
    const ProvinceProduction hungry =
        compute_province_production(r, production_input(without, prices, pid(0)));
    const ProvinceProduction shielded = compute_province_production(r, in);
    CHECK(hungry.shortage_permille[kGrainGood] == 1000);
    CHECK(shielded.shortage_permille[kGrainGood] == 1000);  // the shortage itself is the same
    for (std::size_t g = 0; g < kGroupCount; ++g) {
        CHECK(shielded.new_wealth[g] > hungry.new_wealth[g]);
    }
    // target: base 100 - 60 points of grain shortage = 40; with 30 % protection: 100 - 42 = 58
    const WealthResult plain =
        compute_wealth(r.consumption, Group::Peasants, Fixed32::from_int(100), hungry.shortage_permille);
    const WealthResult kept =
        compute_wealth(r.consumption, Group::Peasants, Fixed32::from_int(100), hungry.shortage_permille, 300);
    CHECK(plain.target == Fixed32::from_int(40));
    CHECK(kept.target == Fixed32::from_int(58));
    Breakdown b;
    (void)compute_wealth(
        r.consumption, Group::Peasants, Fixed32::from_int(100), hungry.shortage_permille, 300, &b);
    CHECK(b.is_consistent());
    CHECK(b.total() == 58 * kOne);
    CHECK(b.node(b.find(term::kWealthShortage)).value == -60 * kOne);
    CHECK(b.node(b.find(term::kWealthBuildings)).value == 18 * kOne);
    // without protection the tree is the one of E9-T07
    Breakdown old;
    (void)compute_wealth(
        r.consumption, Group::Peasants, Fixed32::from_int(100), hungry.shortage_permille, 0, &old);
    CHECK(old.find(term::kWealthBuildings) == kNoNode);
    // and the explanation of a province uses the protection too
    CHECK(explain_wealth(r, in, Group::Peasants)
              .node(explain_wealth(r, in, Group::Peasants).find(term::kWealthBuildings))
              .value == 18 * kOne);
    // an out-of-range protection is clamped, the other goods are not shielded
    std::array<std::int32_t, kMaxGoods> shortage{};
    shortage[kGrainGood] = 1000;
    shortage[3] = 1000;
    const EconomyRules points = [&] {
        EconomyRules p = r;
        p.consumption.shortage_wealth_points[3] = 10;
        return p;
    }();
    CHECK(
        compute_wealth(points.consumption, Group::Peasants, Fixed32::from_int(100), shortage, 5000).target ==
        Fixed32::from_int(100 - 10));  // all grain protected (clamped to 1000), the timber is not
}

TEST_CASE("steps: F7, control and corruption read the buildings") {
    const EconomyRules r = calm_rules();
    World with = make_world(1);
    World without = make_world(1);
    build(with, 0, 0, kTract);
    build(with, 0, 1, kOstrog);
    build(with, 0, 2, kChurch);
    const Administration plain = step_province_administration(without, r, nullptr, pid(0));
    const Administration built = step_province_administration(with, r, nullptr, pid(0));
    CHECK(built.control_target == plain.control_target + Fixed32::from_int(10));  // 3 + 5 + 2
    CHECK(built.corruption_target.raw() < plain.corruption_target.raw());
    // 30 per-mille less, read back in per-mille
    const auto permille = [](Fixed32 v) { return v.raw() * 1000 / kOne; };
    CHECK(permille(plain.corruption_target) - permille(built.corruption_target) == 30);

    const ProvinceModifiers m = modifiers(with, 0, r);
    const AdministrationInput in = administration_input(with, nullptr, pid(0), &m);
    CHECK(in.building_control == Fixed32::from_int(10));
    CHECK(in.oversight_permille == 30);
    const Breakdown control = explain_control(r, in);
    CHECK(control.node(control.find(term::kControlBuildings)).value == 10 * kOne);
    const Breakdown corruption = explain_corruption(r, in);
    CHECK(corruption.node(corruption.find(term::kCorruptionOversight)).value == -30);
    // the input without the set: no effect of buildings, the profile from the source
    const AdministrationInput bare = administration_input(with, nullptr, pid(0));
    CHECK(bare.building_control == Fixed32::zero());
    CHECK(bare.oversight_permille == 0);
    // the profile of the administrator comes through the set
    const SkillSource skills;
    const CharacterId boss = make_person(with, DynastyId(0), 20);
    REQUIRE(with.set_province_administrator(pid(0), boss));
    const Administration governed = step_province_administration(with, r, &skills, pid(0));
    CHECK(governed.control_target > built.control_target);
    const ProvinceModifiers with_boss = compute_province_modifiers(with, r, &skills, pid(0));
    CHECK(administration_input(with, &skills, pid(0), &with_boss).profile ==
          AdministratorProfile{.stewardship = 100, .integrity = 40});
}

TEST_CASE("steps: F11, an ostrog raises the recruit potential, and the ceiling stays above it") {
    const EconomyRules r = open_rules(calm_rules());
    World with = make_world(1);
    World without = make_world(1);
    for (World* w : {&with, &without}) {
        (void)w->set_group_size(pid(0), Group::Peasants, 20000);
        w->economy_columns_mut().control[0] = Fixed32::from_int(100);
    }
    build(with, 0, 0, kOstrog);
    const RecruitInput in_with = recruit_input(with, r, pid(0));
    const RecruitInput in_without = recruit_input(without, r, pid(0));
    CHECK(in_with.buildings_permille == 1250);
    CHECK(in_without.buildings_permille == 1000);
    const std::uint32_t cap_with = compute_recruit_cap(r, in_with);
    const std::uint32_t cap_without = compute_recruit_cap(r, in_without);
    CHECK(cap_with > cap_without);
    CHECK(cap_with <= recruit_ceiling(r, in_with.size));
    Breakdown b;
    CHECK(compute_recruit_cap(r, in_with, &b) == cap_with);
    CHECK(b.is_consistent());
    const NodeIndex step = b.find(term::kRecruitBuildings);
    REQUIRE(step != kNoNode);
    CHECK(b.node(step).factor_permille == 1250);
    CHECK(explain_recruit_cap(with, r, pid(0)) == b);
    Breakdown plain;
    (void)compute_recruit_cap(r, in_without, &plain);
    CHECK(plain.find(term::kRecruitBuildings) == kNoNode);  // 1000 is left out, like the other neutral steps
    // the bound of the invariant counts the best legal set of buildings, at least 1000
    CHECK(max_building_recruit_permille(r) == 1000 + 6 * 250);  // open rules: six ostrogs
    EconomyRules none = r;
    for (std::size_t i = 0; i < 8; ++i) {
        none.buildings.defs[i].recruit_permille = 1000;
    }
    CHECK(max_building_recruit_permille(none) == 1000);
    EconomyRules many = r;
    many.buildings.defs[0].recruit_permille = 1100;
    many.buildings.defs[0].max_per_province = 3;
    many.buildings.defs[5].max_per_province = 2;
    CHECK(max_building_recruit_permille(many) ==
          1000 + 2 * 250 + 3 * 100);  // two of the best, three of the next
    // after a month the pool never passes the ceiling, with the ostrog standing
    Simulation sim = economy_sim(std::move(with), r);
    Simulation plain_sim = economy_sim(std::move(without), r);
    sim.run_days(120);
    plain_sim.run_days(120);
    require_valid(sim.world(), r);
    CHECK(sim.world().economy().provinces.recruit_pool[0] >
          plain_sim.world().economy().provinces.recruit_pool[0]);
}

TEST_CASE("steps: F6 and F8 take the policy level from the cache, and a command changes it at once") {
    const EconomyRules r = calm_rules();
    World world = make_world(2);
    (void)refresh_modifier_cache(world, r, nullptr);
    REQUIRE(world.modifier_cache().clean(0));
    CHECK(tax_input(world, r, pid(0)).tax_level == r.tax.default_level);
    REQUIRE(world.set_dynasty_tax_policy(DynastyId(0), 3));
    REQUIRE(world.set_dynasty_recruit_policy(DynastyId(0), 2));
    CHECK(tax_input(world, r, pid(0)).tax_level == 3);
    CHECK(recruit_input(world, r, pid(0)).level == 2);
    // inside a tick the system refreshes the entry; the level is still the right one
    Simulation sim = economy_sim(std::move(world), r);
    (void)sim.tick();
    CHECK(sim.world().modifier_cache().value(0).tax_level == 3);
    CHECK(tax_input(sim.world(), r, pid(0)).tax_level == 3);
}

// ---- save, load, replay, months
// ---------------------------------------------------------------------------------

TEST_CASE("cache: a loaded world starts cold, rebuilds on its first tick and goes on like the original") {
    World start = make_world(40, 6, Date::from_ymd(1702, 12, 31));
    (void)start.post_money(AccountRef::of(DynastyId(0)), LedgerCode::Adjustment, 1'000'000'000);
    for (std::uint32_t p = 0; p < 40; ++p) {
        build(start, p, 0, static_cast<std::uint8_t>(1 + p % 8));
        build(start, p, 1, static_cast<std::uint8_t>(1 + (p * 3) % 8));
    }
    require_valid(start, open_rules());
    const EconomyRules open = open_rules();
    const World template_world = start;  // a fresh simulation takes it before the saved state is restored
    Simulation sim = economy_sim(std::move(start), open);
    sim.run_days(45);
    REQUIRE(sim.world().modifier_cache().clean_count() == 40);
    const WorldHash hash = sim.world().compute_hash();

    const std::vector<std::uint8_t> bytes = serialize_save(sim.world(), sim.pending_internal());
    auto loaded = deserialize_save(bytes);
    REQUIRE(loaded.has_value());
    World& world = loaded.value().world;
    CHECK(world.compute_hash() == hash);               // the results and the hash are the ones of the save
    CHECK(world.modifier_cache().clean_count() == 0);  // the cache is not in the save
    for (std::size_t p = 0; p < 40; ++p) {
        CHECK(province_modifiers(world, open, nullptr, pid(static_cast<std::uint32_t>(p))) ==
              sim.world().modifier_cache().value(p));  // the answer is the same, computed directly
    }
    // a save of the loaded world is the same file: nothing of the cache leaks into it
    CHECK(serialize_save(world, loaded.value().pending_internal) == bytes);

    Simulation restored = economy_sim(World(template_world), open);
    restored.restore_state(std::move(loaded.value().world), std::move(loaded.value().pending_internal));
    REQUIRE(restored.world().modifier_cache().clean_count() == 0);
    for (int day = 0; day < 70; ++day) {
        (void)sim.tick();
        (void)restored.tick();
        REQUIRE(restored.world().compute_hash() == sim.world().compute_hash());
    }
    CHECK(restored.world().modifier_cache().clean_count() == 40);  // rebuilt by the system
    CHECK(cache_problem(restored.world(), open, nullptr).empty());
    require_valid(restored.world(), open);
}

TEST_CASE("cache: months of 28, 29, 30 and 31 days, Julian leap years 1704, 1708 and 1800") {
    struct Month {
        int year;
        int month;
        int days;
    };
    const std::array<Month, 7> months{{{1703, 1, 31},
                                       {1703, 2, 28},
                                       {1703, 4, 30},
                                       {1704, 2, 29},
                                       {1708, 2, 29},
                                       {1800, 2, 29},
                                       {1800, 3, 31}}};
    for (const Month& m : months) {
        CAPTURE(m.year);
        CAPTURE(m.month);
        REQUIRE(Date::days_in_month(m.year, m.month) == m.days);
        // a mill that is finished on the 1st of the next month, in a world of 60 provinces (each slot day
        // of the stagger has two), the rest of them with buildings from the start
        EconomyRules r = open_rules();
        r.buildings.defs[0].build_days = m.days;
        REQUIRE(validate(r).has_value());
        World world = make_world(60, 4, Date::from_ymd(m.year, m.month, 1) - 1);
        (void)world.post_money(AccountRef::of(DynastyId(0)), LedgerCode::Adjustment, 1'000'000'000);
        for (std::uint32_t p = 1; p < 60; ++p) {
            build(world, p, 0, static_cast<std::uint8_t>(1 + p % 8));
        }
        Simulation sim = economy_sim(std::move(world), r);
        REQUIRE(sim.submit(Command{DynastyId(0), cmd::StartConstruction{pid(0), kMill}}));
        (void)sim.tick();  // the 1st: the construction starts
        REQUIRE(sim.world().economy().construction.size() == 1);
        for (int day = 2; day <= m.days; ++day) {
            (void)sim.tick();
            REQUIRE(cache_problem(sim.world(), r, nullptr).empty());
            CHECK(sim.world().modifier_cache().value(0).output_permille[0] == 1000);  // not yet standing
        }
        (void)sim.tick();  // the 1st of the next month: the mill stands, the cache says so on the same day
        CHECK(sim.world().today().day() == 1);
        CHECK(sim.world().economy().provinces.slot_building[0] == kMill);
        CHECK(sim.world().modifier_cache().clean(0));
        CHECK(sim.world().modifier_cache().value(0).output_permille[0] == 1150);
        // every province was processed exactly once in the month, whatever its length
        for (std::size_t p = 0; p < 60; ++p) {
            CHECK(sim.world().economy().provinces.last_slot_date[p] != kNoDate);
            CHECK(sim.world().economy().provinces.last_slot_date[p] >= Date::from_ymd(m.year, m.month, 1));
        }
        require_valid(sim.world(), r);
    }
}

TEST_CASE("replay: a run with buildings, policies and conscriptions is reproduced by the hash, "
          "also when it is interrupted by a save") {
    const auto make = [] {
        World world = make_world(8, 5, Date::from_ymd(1702, 12, 31));
        (void)world.post_money(AccountRef::of(DynastyId(0)), LedgerCode::Adjustment, 1'000'000'000);
        for (std::uint32_t p = 0; p < 8; ++p) {
            (void)world.set_group_size(pid(p), Group::Peasants, 20000);
        }
        return world;
    };
    const auto script = [](Simulation& sim, int day) {
        if (day == 1) {
            for (std::uint32_t p = 0; p < 8; ++p) {
                REQUIRE(sim.submit(Command{
                    DynastyId(0), cmd::StartConstruction{pid(p), static_cast<std::uint8_t>(1 + p % 8)}}));
            }
        }
        if (day == 100) {
            REQUIRE(sim.submit(Command{DynastyId(0), cmd::SetRecruitPolicy{DynastyId(0), 2}}));
            REQUIRE(sim.submit(Command{DynastyId(0), cmd::SetTaxPolicy{DynastyId(0), 0}}));
        }
        if (day == 150) {
            REQUIRE(sim.submit(Command{DynastyId(0), cmd::StartConstruction{pid(3), kOstrog}}));
        }
        if (day == 380) {
            REQUIRE(sim.submit(Command{DynastyId(), cmd::ConscriptRecruits{pid(3), 5}}));
        }
    };
    const EconomyRules open = open_rules();
    Simulation straight = economy_sim(make(), open);
    ReplayRecorder recorder(5, 78, straight.world().today(), 10);
    std::vector<WorldHash> hashes;
    for (int day = 0; day < 420; ++day) {
        script(straight, day);
        (void)straight.tick();
        recorder.after_tick(straight);
        hashes.push_back(straight.world().compute_hash());
    }
    const Replay replay = recorder.finish(straight);
    // the buildings did get built, and the cache agrees with the direct computation at the end
    std::size_t standing = 0;
    for (const std::uint8_t b : straight.world().economy().provinces.slot_building) {
        standing += b != 0 ? 1 : 0;
    }
    CHECK(standing >= 8);
    CHECK(cache_problem(straight.world(), open, nullptr).empty());
    require_valid(straight.world(), open);

    Simulation player = economy_sim(make(), open);
    const ReplayResult played = play_replay(player, replay);
    CHECK(played.error.empty());
    CHECK(played.ok);
    CHECK(player.world().compute_hash() == straight.world().compute_hash());

    // the same run, interrupted by a save and a load every 60 days, stays on the hashes of the first one
    Simulation interrupted = economy_sim(make(), open);
    for (int day = 0; day < 420; ++day) {
        script(interrupted, day);
        (void)interrupted.tick();
        REQUIRE(interrupted.world().compute_hash() == hashes[static_cast<std::size_t>(day)]);
        if (day % 60 == 59) {
            auto loaded =
                deserialize_save(serialize_save(interrupted.world(), interrupted.pending_internal()));
            REQUIRE(loaded.has_value());
            interrupted.restore_state(std::move(loaded.value().world),
                                      std::move(loaded.value().pending_internal));
        }
    }
}

TEST_CASE("cache: a full rebuild of 400 provinces gives 400 clean entries equal to the direct computation") {
    World world = make_world(400, 6);
    Pcg32 rng(9, 0x18);
    for (std::uint32_t p = 0; p < 400; ++p) {
        for (std::size_t s = 0; s < 6; ++s) {
            if (rng.chance_permille(600)) {
                build(world, p, s, static_cast<std::uint8_t>(1 + rng.bounded(8)));
            }
        }
    }
    CHECK(rebuild_modifier_cache(world, rules(), nullptr) == 400);
    CHECK(world.modifier_cache().clean_count() == 400);
    CHECK(cache_problem(world, rules(), nullptr).empty());
    // the time is measured in Release by stressgen (docs/benchmarks/E9-T17-building-effects.md); a test
    // on a shared CI machine does not gate on it
}
