#include "pat/core/bytes.hpp"
#include "pat/sim/calendar.hpp"
#include "pat/sim/command.hpp"
#include "pat/sim/command_log.hpp"
#include "pat/sim/economy/construction.hpp"
#include "pat/sim/economy/invariants.hpp"
#include "pat/sim/economy/policy.hpp"
#include "pat/sim/economy/rules.hpp"
#include "pat/sim/economy/settlement.hpp"
#include "pat/sim/economy/systems.hpp"
#include "pat/sim/economy/tables.hpp"
#include "pat/sim/replay.hpp"
#include "pat/sim/simulation.hpp"
#include "pat/sim/world.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// E9-T16: buildings. The types of the rules (8 of GDD 03 section 15), the slots of a province, the
// commands StartConstruction (tag 36) and CancelConstruction (tag 37), the queue of the world, the
// system economy.construction and the upkeep of the finished buildings in the statement. ECO-A4: the
// cost is charged, the slot is taken, the building is ready on time; a lack of money, of slots or of
// requirements is a refusal with a reason; the queue survives a save, a replay reproduces it.

using namespace pat::sim;
using namespace pat::sim::economy;
using pat::core::Fixed32;
using pat::core::StringId;

namespace {

constexpr std::uint8_t kMill = 1;
constexpr std::uint8_t kManufactory = 2;
constexpr std::uint8_t kWorks = 3;
constexpr std::uint8_t kTract = 4;
constexpr std::uint8_t kWharf = 5;
constexpr std::uint8_t kOstrog = 6;
constexpr std::uint8_t kChurch = 7;
constexpr std::uint8_t kGranary = 8;

constexpr Kopecks kRich = 100'000'000;  // a million roubles

const EconomyRules& rules() {
    static const EconomyRules value = EconomyRules::defaults();
    return value;
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

/// Three dynasties; `provinces` provinces of dynasty 0, each with `slots` slots, development 4 and kind
/// Rural; dynasty 0 has a million roubles, dynasty 1 ten thousand, dynasty 2 nothing.
World make_world(std::uint32_t provinces = 2,
                 std::uint8_t slots = 3,
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
    for (std::uint32_t p = 0; p < provinces; ++p) {
        world.economy_columns_mut().slots_total[p] = slots;
        world.set_province_development(ProvinceId(p), 4);
        (void)world.set_group_size(ProvinceId(p), Group::Peasants, 8000);
    }
    (void)world.post_money(AccountRef::of(DynastyId(0)), LedgerCode::Adjustment, kRich);
    (void)world.post_money(AccountRef::of(DynastyId(1)), LedgerCode::Adjustment, 1'000'000);
    require_valid(world);
    return world;
}

Simulation economy_sim(World world, const EconomyRules& with = rules()) {
    Simulation sim(std::move(world));
    install_economy_systems(sim, with);
    return sim;
}

/// A simulation that has lived through 1 January 1703: the December of the scenario is closed, the
/// open month is January, and province 0 (slot day 1) has paid its taxes, so that the postings of the
/// next days are exactly the ones of the commands.
Simulation primed_sim(World world, const EconomyRules& with = rules()) {
    Simulation sim = economy_sim(std::move(world), with);
    (void)sim.tick();
    REQUIRE(sim.world().today() == Date::from_ymd(1703, 1, 1));
    return sim;
}

Command start(DynastyId actor, std::uint32_t province, std::uint8_t building) {
    return Command{actor, cmd::StartConstruction{ProvinceId(province), building}};
}
Command cancel(DynastyId actor, std::uint32_t province, std::uint8_t slot) {
    return Command{actor, cmd::CancelConstruction{ProvinceId(province), slot}};
}

struct Verdict {
    Rejection reason = Rejection::None;
    std::string detail;
};

Verdict verdict(const World& world, const Command& command, const EconomyRules* with = &rules()) {
    const auto result = validate(world, command, nullptr, with);
    if (result.has_value()) {
        return {};
    }
    return {result.error().reason, result.error().detail};
}

Kopecks money(const World& world, std::uint32_t dynasty) {
    return world.treasury(AccountRef::of(DynastyId(dynasty)));
}

Kopecks line(const World& world, std::uint32_t dynasty, LedgerCode code) {
    const auto row = world.resolve_account(AccountRef::of(DynastyId(dynasty)));
    REQUIRE(row.has_value());
    return world.economy().accounts.open_line(*row, code);
}

std::uint8_t built(const World& world, std::uint32_t province, std::size_t slot) {
    return world.economy().provinces.slot_building[province * kMaxSlots + slot];
}

std::size_t completed_incidents(const World& world) {
    const IncidentLog& log = world.economy().incidents;
    return static_cast<std::size_t>(std::count(
        log.kind.begin(), log.kind.end(), static_cast<std::uint8_t>(IncidentKind::BuildingCompleted)));
}

const BuildingDef& type(std::uint8_t number) {
    const BuildingDef* def = find_building(rules().buildings, number);
    REQUIRE(def != nullptr);
    return *def;
}

}  // namespace

// ---- the types ---------------------------------------------------------------------------------------

TEST_CASE("the eight buildings of M1 are defined with their requirements") {
    const BuildingRules& b = rules().buildings;
    REQUIRE(b.count == 8);
    const std::array<std::string_view, 8> keys{"building.mill",
                                               "building.manufactory",
                                               "building.works",
                                               "building.tract",
                                               "building.wharf",
                                               "building.ostrog",
                                               "building.church",
                                               "building.granary"};
    for (std::size_t i = 0; i < keys.size(); ++i) {
        CAPTURE(i);
        CHECK(b.defs[i].slot == i + 1);  // the stable numbers of the saves
        CHECK(b.defs[i].key == keys[i]);
        CHECK(b.defs[i].cost_kop > 0);
        CHECK(b.defs[i].build_days >= 1);
        CHECK(b.defs[i].upkeep_kop_month > 0);
        CHECK(b.defs[i].max_per_province >= 1);
    }
    CHECK(b.refund_permille == 800);
    // the example of economy-model 11.1
    CHECK(type(kMill).cost_kop == 400'000);
    CHECK(type(kMill).build_days == 120);
    CHECK(type(kMill).upkeep_kop_month == 3'000);
    // requirements: the kinds are bits of ProvinceKind
    const auto bit = [](ProvinceKind kind) { return 1U << index_of(kind); };
    CHECK(type(kManufactory).allowed_kinds == (bit(ProvinceKind::City) | bit(ProvinceKind::Industrial)));
    CHECK(type(kWorks).allowed_kinds == bit(ProvinceKind::Industrial));
    CHECK(type(kWharf).allowed_kinds == bit(ProvinceKind::Port));
    CHECK(type(kOstrog).allowed_kinds ==
          (bit(ProvinceKind::Border) | bit(ProvinceKind::Steppe) | bit(ProvinceKind::Northern)));
    CHECK(type(kWorks).min_development == 3);
    CHECK(type(kMill).allowed_kinds == 0);
    CHECK(type(kChurch).allowed_kinds == 0);
    CHECK(type(kGranary).allowed_kinds == 0);
    CHECK(type(kTract).allowed_kinds == 0);
    CHECK(economy::validate(rules()).has_value());  // the table is valid
    CHECK(find_building(b, 0) == nullptr);
    CHECK(find_building(b, 9) == nullptr);
}

TEST_CASE("the rules reject nonsense requirements of a building") {
    const auto broken = [](auto change) {
        EconomyRules r = EconomyRules::defaults();
        change(r.buildings.defs[0]);
        const auto result = economy::validate(r);
        REQUIRE_FALSE(result.has_value());
        return result.error().message;
    };
    CHECK(broken([](BuildingDef& d) { d.min_development = 11; }).find("min_development") !=
          std::string::npos);
    CHECK(broken([](BuildingDef& d) { d.min_development = -1; }).find("min_development") !=
          std::string::npos);
    CHECK(broken([](BuildingDef& d) { d.allowed_kinds = 1U << 8; }).find("allowed_kinds") !=
          std::string::npos);
    CHECK(broken([](BuildingDef& d) { d.max_per_province = 0; }).find("max_per_province") !=
          std::string::npos);
    CHECK(broken([](BuildingDef& d) { d.max_per_province = 7; }).find("max_per_province") !=
          std::string::npos);
}

// ---- ECO-A4: the cost, the slot, the time --------------------------------------------------------------

TEST_CASE("ECO-A4: a mill is paid at once in one posting, takes a slot, and stands on the day it is due") {
    Simulation sim = primed_sim(make_world(1));
    const Kopecks before = money(sim.world(), 0);
    REQUIRE(sim.submit(start(DynastyId(0), 0, kMill)));
    (void)sim.tick();  // 2 January 1703: the command is applied before the systems of the day
    REQUIRE(sim.last_report().external_commands.applied == 1);
    const World& world = sim.world();
    const Date started = Date::from_ymd(1703, 1, 2);
    CHECK(world.today() == started);

    // the cost: one posting of the code Construction, the whole price, in the open month
    CHECK(money(world, 0) == before - 400'000);
    CHECK(line(world, 0, LedgerCode::Construction) == -400'000);
    // the slot: reserved by the queue, the lowest one; nothing stands there yet
    const ConstructionQueue& q = world.economy().construction;
    REQUIRE(q.size() == 1);
    CHECK(q.province[0] == ProvinceId(0));
    CHECK(q.slot[0] == 0);
    CHECK(q.building[0] == kMill);
    CHECK(q.payer[0] == DynastyId(0));
    CHECK(q.start_date[0] == started);
    CHECK(q.finish_date[0] == started + 120);  // 2 May 1703
    CHECK(q.finish_date[0] == Date::from_ymd(1703, 5, 2));
    CHECK(q.paid_kop[0] == 400'000);
    CHECK(built(world, 0, 0) == 0);
    CHECK(free_slot(world, ProvinceId(0)) == std::optional<std::uint8_t>(1));
    CHECK(building_count(world, ProvinceId(0), kMill) == 1);  // the construction counts
    require_valid(world);

    // not before the day: the day before it is still a construction site
    while (sim.world().today() < started + 119) {
        (void)sim.tick();
    }
    CHECK(sim.world().today() == Date::from_ymd(1703, 5, 1));
    CHECK(built(sim.world(), 0, 0) == 0);
    CHECK(sim.world().economy().construction.size() == 1);
    CHECK(completed_incidents(sim.world()) == 0);
    // on the day it is due: the building stands, the queue is empty, the log says so
    (void)sim.tick();
    CHECK(sim.world().today() == Date::from_ymd(1703, 5, 2));
    CHECK(built(sim.world(), 0, 0) == kMill);
    CHECK(sim.world().economy().construction.size() == 0);
    CHECK(completed_incidents(sim.world()) == 1);
    const IncidentLog& log = sim.world().economy().incidents;
    const std::size_t last = log.size() - 1;
    CHECK(log.kind[last] == static_cast<std::uint8_t>(IncidentKind::BuildingCompleted));
    CHECK(log.province[last] == ProvinceId(0));
    CHECK(log.dynasty[last] == DynastyId(0));
    CHECK(log.param[last] == kMill);
    CHECK(log.date[last] == Date::from_ymd(1703, 5, 2));
    CHECK(sim.world().economy().construction.size() == 0);
    require_valid(sim.world());
    // the money does not move again: no second charge, and the slot is no longer free
    CHECK(free_slot(sim.world(), ProvinceId(0)) == std::optional<std::uint8_t>(1));
    CHECK(building_count(sim.world(), ProvinceId(0), kMill) == 1);
}

TEST_CASE("a lack of money, slots or requirements is a refusal with its reason") {
    World world = make_world(4, 2);
    // province 1: the second slot is taken by a finished tract, so one slot is left
    REQUIRE(world.set_slot_building(ProvinceId(1), 0, kTract));
    // province 2: no slots at all; province 3: Industrial, development 2
    world.economy_columns_mut().slots_total[2] = 0;
    world.economy_columns_mut().kind[3] = static_cast<std::uint8_t>(ProvinceKind::Industrial);
    world.set_province_development(ProvinceId(3), 2);

    const auto refused = [&](const Command& command, Rejection reason, ConstructionRefusal why) {
        CAPTURE(refusal_text(why));
        const Verdict v = verdict(world, command);
        CHECK(v.reason == reason);
        CHECK(v.detail == refusal_text(why));
    };

    // plenty of money, a free slot, no requirement: accepted
    CHECK(verdict(world, start(DynastyId(0), 0, kMill)).reason == Rejection::None);
    CHECK(verdict(world, start(DynastyId(), 0, kMill)).reason ==
          Rejection::None);  // the system, the holder pays
    CHECK(verdict(world, start(DynastyId(0), 1, kMill)).reason == Rejection::None);

    // money: dynasty 1 has 10 000 roubles; a province of dynasty 1 would cost them all, dynasty 2 has none
    world.set_province_owner(ProvinceId(0), DynastyId(2));
    refused(start(DynastyId(2), 0, kGranary), Rejection::InvalidState, ConstructionRefusal::NoMoney);
    REQUIRE(world.post_money(AccountRef::of(DynastyId(2)), LedgerCode::Adjustment, 299'999) == 299'999);
    refused(start(DynastyId(2), 0, kGranary),
            Rejection::InvalidState,
            ConstructionRefusal::NoMoney);  // 1 kopeck short
    REQUIRE(world.post_money(AccountRef::of(DynastyId(2)), LedgerCode::Adjustment, 1) == 1);
    CHECK(verdict(world, start(DynastyId(2), 0, kGranary)).reason == Rejection::None);  // exactly the price
    // a balance in debt affords nothing
    (void)world.post_money(AccountRef::of(DynastyId(2)), LedgerCode::Adjustment, -400'000);
    refused(start(DynastyId(2), 0, kGranary), Rejection::InvalidState, ConstructionRefusal::NoMoney);

    // slots: none at all; and every slot spoken for by a finished building or a construction
    refused(start(DynastyId(0), 2, kMill), Rejection::InvalidState, ConstructionRefusal::NoFreeSlot);
    REQUIRE(world.begin_construction(ProvinceId(1), kGranary, DynastyId(0), 1, 10).has_value());
    refused(start(DynastyId(0), 1, kMill), Rejection::InvalidState, ConstructionRefusal::NoFreeSlot);

    // requirements: the kind, the development, the number of the type, an unknown type, an unknown province
    refused(start(DynastyId(0), 0 + 3, kWorks),
            Rejection::InvalidState,
            ConstructionRefusal::TooLittleDevelopment);
    refused(start(DynastyId(0), 1, kWharf), Rejection::InvalidState, ConstructionRefusal::WrongKind);
    refused(start(DynastyId(), 1, kManufactory), Rejection::InvalidState, ConstructionRefusal::WrongKind);
    world.set_province_development(ProvinceId(3), 3);
    CHECK(verdict(world, start(DynastyId(0), 3, kWorks)).reason ==
          Rejection::None);  // industrial and developed enough
    refused(start(DynastyId(0), 1, kTract),
            Rejection::InvalidState,
            ConstructionRefusal::TooMany);  // there is a tract
    refused(start(DynastyId(0), 1, 0), Rejection::InvalidArgument, ConstructionRefusal::UnknownBuilding);
    refused(start(DynastyId(0), 1, 9), Rejection::InvalidArgument, ConstructionRefusal::UnknownBuilding);
    refused(start(DynastyId(0), 99, kMill), Rejection::UnknownEntity, ConstructionRefusal::UnknownProvince);
}

TEST_CASE("who may build where: only in one's own provinces (Q4), the system for the holder") {
    World world = make_world(3);
    REQUIRE(world.set_province_owner_kind(ProvinceId(2), OwnerKind::Crown));
    // a house in the province of another house
    const Verdict foreign = verdict(world, start(DynastyId(1), 0, kMill));
    CHECK(foreign.reason == Rejection::NotAuthorized);
    CHECK(foreign.detail == std::string(refusal_text(ConstructionRefusal::NotOwner)));
    // a house in a province of the crown
    CHECK(verdict(world, start(DynastyId(0), 2, kMill)).reason == Rejection::NotAuthorized);
    // the system names the holder as the payer, and has none in a province of the crown
    CHECK(verdict(world, start(DynastyId(), 0, kMill)).reason == Rejection::None);
    const Verdict crown = verdict(world, start(DynastyId(), 2, kMill));
    CHECK(crown.reason == Rejection::InvalidState);
    CHECK(crown.detail == std::string(refusal_text(ConstructionRefusal::NotHeld)));

    // the payer of a system command is the holder
    apply(world, start(DynastyId(), 0, kMill), nullptr, &rules());
    REQUIRE(world.economy().construction.size() == 1);
    CHECK(world.economy().construction.payer[0] == DynastyId(0));
    CHECK(money(world, 0) == kRich - 400'000);
}

TEST_CASE("the rule build_foreign_allowed lets a dynasty build in another's province, and pay for it") {
    EconomyRules open = EconomyRules::defaults();
    open.build_foreign_allowed = true;
    World world = make_world(2);
    REQUIRE(world.set_province_owner_kind(ProvinceId(1), OwnerKind::Crown));
    CHECK(verdict(world, start(DynastyId(1), 0, kGranary), &open).reason == Rejection::None);
    CHECK(verdict(world, start(DynastyId(1), 1, kGranary), &open).reason ==
          Rejection::None);  // even the crown's
    CHECK(verdict(world, start(DynastyId(), 1, kGranary), &open).reason ==
          Rejection::InvalidState);  // no payer
    // the money still has to be there
    CHECK(verdict(world, start(DynastyId(2), 0, kGranary), &open).detail ==
          std::string(refusal_text(ConstructionRefusal::NoMoney)));
    apply(world, start(DynastyId(1), 0, kGranary), nullptr, &open);
    REQUIRE(world.economy().construction.size() == 1);
    CHECK(world.economy().construction.payer[0] == DynastyId(1));  // not the owner
    CHECK(money(world, 1) == 1'000'000 - 300'000);
    CHECK(money(world, 0) == kRich);
    require_valid(world, open);
    // only the payer takes the construction back
    CHECK(verdict(world, cancel(DynastyId(0), 0, 0), &open).reason == Rejection::NotAuthorized);
    CHECK(verdict(world, cancel(DynastyId(1), 0, 0), &open).reason == Rejection::None);
}

TEST_CASE("without an economy or without the rules of the simulation a construction is refused") {
    World plain(Date::from_ymd(1702, 12, 31), 5);
    (void)plain.create_dynasty(StringId(1));
    (void)plain.create_province(StringId(100), DynastyId(0), Fixed32::from_int(1));
    const Verdict no_economy = verdict(plain, start(DynastyId(0), 0, kMill));
    CHECK(no_economy.reason == Rejection::InvalidState);
    CHECK(no_economy.detail == std::string(refusal_text(ConstructionRefusal::NoEconomy)));
    CHECK(verdict(plain, cancel(DynastyId(0), 0, 0)).reason == Rejection::InvalidState);

    const World world = make_world();
    const Verdict no_rules = verdict(world, start(DynastyId(0), 0, kMill), nullptr);
    CHECK(no_rules.reason == Rejection::InvalidState);
    CHECK(no_rules.detail == std::string(refusal_text(ConstructionRefusal::NoRules)));
    // a Simulation that was not given the rules refuses it as well, and nothing changes
    Simulation bare(make_world());
    REQUIRE(bare.submit(start(DynastyId(0), 0, kMill)));
    (void)bare.tick();
    CHECK(bare.last_report().external_commands.rejected == 1);
    CHECK(bare.world().economy().construction.size() == 0);
    CHECK(money(bare.world(), 0) == kRich);
}

TEST_CASE("a stale command does nothing: apply() re-checks the money and the slot") {
    World world = make_world(1, 1);
    apply(world, start(DynastyId(0), 0, kMill), nullptr, &rules());
    apply(world, start(DynastyId(0), 0, kChurch), nullptr, &rules());  // no free slot any more
    CHECK(world.economy().construction.size() == 1);
    CHECK(money(world, 0) == kRich - 400'000);
    apply(world, start(DynastyId(2), 0, kGranary), nullptr, &rules());  // not the owner
    apply(world, start(DynastyId(0), 0, kMill), nullptr, nullptr);      // no rules
    CHECK(world.economy().construction.size() == 1);
    // the World itself will not spend what is not there
    CHECK_FALSE(world.begin_construction(ProvinceId(0), kMill, DynastyId(2), 5, 10).has_value());
    CHECK_FALSE(world.begin_construction(ProvinceId(0), 0, DynastyId(0), 5, 10).has_value());
    CHECK_FALSE(world.begin_construction(ProvinceId(0), kMill, DynastyId(0), -5, 10).has_value());
    CHECK_FALSE(world.begin_construction(ProvinceId(0), kMill, DynastyId(0), 5, 0).has_value());
    CHECK_FALSE(
        world.begin_construction(ProvinceId(0), kMill, DynastyId(0), kMaxPostingKop + 1, 10).has_value());
    CHECK_FALSE(world.begin_construction(ProvinceId(9), kMill, DynastyId(0), 5, 10).has_value());
    CHECK_FALSE(world.begin_construction(ProvinceId(0), kMill, DynastyId(9), 5, 10).has_value());
    require_valid(world);
}

// ---- cancelling --------------------------------------------------------------------------------------

TEST_CASE("cancelling returns the share of the rules, frees the slot, and only the payer may do it") {
    Simulation sim = primed_sim(make_world(1));
    REQUIRE(sim.submit(start(DynastyId(0), 0, kMill)));
    REQUIRE(sim.submit(start(DynastyId(0), 0, kChurch)));
    (void)sim.tick();
    REQUIRE(sim.world().economy().construction.size() == 2);
    const Kopecks after_start = money(sim.world(), 0);
    CHECK(line(sim.world(), 0, LedgerCode::Construction) == -400'000 - 500'000);

    // not the payer, and a slot where nothing is built, and a province that is not there
    CHECK(verdict(sim.world(), cancel(DynastyId(1), 0, 0)).reason == Rejection::NotAuthorized);
    CHECK(verdict(sim.world(), cancel(DynastyId(0), 0, 2)).reason == Rejection::UnknownEntity);
    CHECK(verdict(sim.world(), cancel(DynastyId(0), 99, 0)).reason == Rejection::UnknownEntity);
    CHECK(verdict(sim.world(), cancel(DynastyId(0), 0, 0)).reason == Rejection::None);
    CHECK(verdict(sim.world(), cancel(DynastyId(), 0, 1)).reason == Rejection::None);  // the system may

    REQUIRE(sim.submit(cancel(DynastyId(0), 0, 0)));  // the mill: 80 % of 400 000 comes back
    (void)sim.tick();
    const World& world = sim.world();
    CHECK(world.economy().construction.size() == 1);
    CHECK(world.economy().construction.slot[0] == 1);  // the church stays in its slot
    CHECK(money(world, 0) == after_start + 320'000);
    // the statement: the price and the refund are two lines of the same code that net to the loss
    CHECK(line(world, 0, LedgerCode::Construction) == -400'000 - 500'000 + 320'000);
    CHECK(free_slot(world, ProvinceId(0)) == std::optional<std::uint8_t>(0));  // the lowest slot again
    CHECK(building_count(world, ProvinceId(0), kMill) == 0);
    require_valid(world);

    // a new construction takes the freed slot
    REQUIRE(sim.submit(start(DynastyId(0), 0, kGranary)));
    (void)sim.tick();
    bool in_slot_zero = false;
    for (std::size_t i = 0; i < sim.world().economy().construction.size(); ++i) {
        in_slot_zero = in_slot_zero || (sim.world().economy().construction.slot[i] == 0 &&
                                        sim.world().economy().construction.building[i] == kGranary);
    }
    CHECK(in_slot_zero);

    // a finished building cannot be cancelled: the slot is no construction any more
    sim.run_days(100);
    CHECK(built(sim.world(), 0, 0) == kGranary);
    CHECK(verdict(sim.world(), cancel(DynastyId(0), 0, 0)).reason == Rejection::UnknownEntity);
    require_valid(sim.world());
}

TEST_CASE("the refund share comes from the rules, from none to all") {
    for (const std::int32_t permille : {0, 333, 1000}) {
        CAPTURE(permille);
        EconomyRules changed = EconomyRules::defaults();
        changed.buildings.refund_permille = permille;
        REQUIRE(economy::validate(changed).has_value());
        Simulation sim = primed_sim(make_world(1), changed);
        const Kopecks before = money(sim.world(), 0);
        REQUIRE(sim.submit(start(DynastyId(0), 0, kMill)));
        (void)sim.tick();
        REQUIRE(sim.submit(cancel(DynastyId(0), 0, 0)));
        (void)sim.tick();
        CHECK(money(sim.world(), 0) == before - 400'000 + 400'000 * permille / 1000);
        CHECK(cancel_refund(changed.buildings, 400'000) == 400'000 * permille / 1000);
        require_valid(sim.world(), changed);
    }
    CHECK(cancel_refund(rules().buildings, 1) == 0);  // truncated toward zero
    // the World refuses a share beyond the whole
    World world = make_world();
    REQUIRE(world.begin_construction(ProvinceId(0), kMill, DynastyId(0), 400, 10).has_value());
    CHECK_FALSE(world.abort_construction(ProvinceId(0), 0, 1001).has_value());
    CHECK_FALSE(world.abort_construction(ProvinceId(0), 0, -1).has_value());
    CHECK(world.abort_construction(ProvinceId(0), 0, 800) == std::optional<Kopecks>(320));
    CHECK_FALSE(world.abort_construction(ProvinceId(0), 0, 800).has_value());
}

// ---- the queue ---------------------------------------------------------------------------------------

TEST_CASE("the queue is ordered by the date of readiness and finishes in that order") {
    Simulation sim = economy_sim(make_world(2, 4));
    REQUIRE(sim.submit(start(DynastyId(0), 0, kMill)));     // 120 days
    REQUIRE(sim.submit(start(DynastyId(0), 1, kGranary)));  // 90 days, same day, later in the order
    REQUIRE(sim.submit(start(DynastyId(0), 0, kChurch)));   // 300 days
    (void)sim.tick();
    const ConstructionQueue& q = sim.world().economy().construction;
    REQUIRE(q.size() == 3);
    CHECK(q.building[0] == kGranary);
    CHECK(q.building[1] == kMill);
    CHECK(q.building[2] == kChurch);
    CHECK(q.seq[0] == 1);  // the order of the commands lives on in `seq`
    CHECK(q.seq[1] == 0);
    CHECK(q.seq[2] == 2);
    CHECK(sim.world().economy().meta.next_construction_seq == 3);
    require_valid(sim.world());
    // two that are due on one day: the older command first
    sim.run_days(10);
    REQUIRE(sim.submit(start(DynastyId(0), 1, kGranary)));  // refused: one granary at most
    REQUIRE(sim.submit(start(DynastyId(0), 1, kTract)));    // 180 days from day 12
    (void)sim.tick();
    CHECK(sim.last_report().external_commands.rejected == 1);
    require_valid(sim.world());

    sim.run_days(80 - 1);  // the granary: day 91 of the run, 1 April 1703
    CHECK(built(sim.world(), 1, 0) == kGranary);
    CHECK(sim.world().economy().construction.size() == 3);
    sim.run_days(30);  // the mill: 1 May
    CHECK(built(sim.world(), 0, 0) == kMill);
    CHECK(sim.world().economy().construction.size() == 2);
    sim.run_days(400);
    CHECK(sim.world().economy().construction.size() == 0);
    CHECK(built(sim.world(), 0, 1) == kChurch);
    CHECK(built(sim.world(), 1, 1) == kTract);
    CHECK(completed_incidents(sim.world()) == 4);
    require_valid(sim.world());
}

TEST_CASE("an empty queue costs the system one comparison and changes nothing") {
    World world = make_world();
    const WorldHash before = world.compute_hash();
    CHECK(world.finish_due_constructions() == 0);
    CHECK(world.compute_hash() == before);
    Simulation sim = economy_sim(make_world());
    sim.run_days(40);
    CHECK(sim.world().economy().construction.size() == 0);
    CHECK(completed_incidents(sim.world()) == 0);
}

// ---- upkeep in the statement -----------------------------------------------------------------------

TEST_CASE("the upkeep of a finished building goes into the statement every month, a site pays none") {
    Simulation sim = primed_sim(make_world(1, 3));
    REQUIRE(sim.submit(start(DynastyId(0), 0, kGranary)));  // 90 days: finished on 2 April 1703
    REQUIRE(sim.submit(start(DynastyId(0), 0, kChurch)));   // 300 days: still a site for months
    (void)sim.tick();                                       // 2 January
    const Date church_due = Date::from_ymd(1703, 1, 2) + 300;
    const auto upkeep_of_closed_month = [&](std::size_t month) {
        const auto row = sim.world().resolve_account(AccountRef::of(DynastyId(0)));
        REQUIRE(row.has_value());
        const auto lines = sim.world().economy().accounts.ring_lines(*row, month);
        return lines[*ledger_index(LedgerCode::BuildingUpkeep)];
    };
    const auto row_of_dynasty_0 = *sim.world().resolve_account(AccountRef::of(DynastyId(0)));
    const auto run_to = [&](Date day) {
        while (sim.world().today() < day) {
            (void)sim.tick();
        }
        REQUIRE(sim.world().today() == day);
    };
    // the 1st of April closes March: the granary is not there yet (it is finished the next day)
    run_to(Date::from_ymd(1703, 4, 1));
    CHECK(built(sim.world(), 0, 0) == 0);
    CHECK(upkeep_of_closed_month(0) == 0);
    run_to(Date::from_ymd(1703, 4, 2));
    CHECK(built(sim.world(), 0, 0) == kGranary);
    CHECK(building_upkeep_by_account(sim.world(), rules().buildings)[row_of_dynasty_0] == 2'000);
    // the 1st of May closes April, in which the granary stood: one upkeep, the whole month's, 20 roubles
    run_to(Date::from_ymd(1703, 5, 1));
    CHECK(upkeep_of_closed_month(0) == -2'000);
    CHECK(upkeep_of_closed_month(1) == 0);                 // March
    CHECK(sim.world().economy().meta.closed_months == 5);  // December, January, February, March, April
    // and every month after it, for the granary only: the church is still a site
    run_to(Date::from_ymd(1703, 7, 1));
    CHECK(upkeep_of_closed_month(0) == -2'000);
    CHECK(upkeep_of_closed_month(1) == -2'000);
    require_valid(sim.world());
    // the church stands: both are charged from the next closing on
    run_to(church_due);
    CHECK(built(sim.world(), 0, 1) == kChurch);
    CHECK(building_upkeep_by_account(sim.world(), rules().buildings)[row_of_dynasty_0] == 2'000 + 2'000);
    run_to(Date::from_ymd(1703, 12, 1));  // the closing of November
    CHECK(upkeep_of_closed_month(0) == -4'000);
    // the total of the year: the statement agrees with the number of months each building stood
    require_valid(sim.world());
}

TEST_CASE("months of 28, 29, 30 and 31 days, Julian leap years 1704, 1708 and 1800: due on the day, "
          "one upkeep a month") {
    for (const int year : {1704, 1708, 1800, 1703}) {
        CAPTURE(year);
        std::int64_t charged = 0;
        for (int month = 1; month <= 12; ++month) {
            CAPTURE(month);
            // a mill that takes exactly one month: due on the 1st of the next one
            EconomyRules one_month = EconomyRules::defaults();
            const int length = Date::days_in_month(year, month);
            one_month.buildings.defs[0].build_days = length;
            REQUIRE(economy::validate(one_month).has_value());
            Simulation sim = economy_sim(make_world(1, 2, Date::from_ymd(year, month, 1) - 1), one_month);
            REQUIRE(sim.submit(start(DynastyId(0), 0, kMill)));
            (void)sim.tick();
            REQUIRE(sim.world().today() == Date::from_ymd(year, month, 1));
            REQUIRE(sim.world().economy().construction.size() == 1);
            CHECK(sim.world().economy().construction.finish_date[0] ==
                  Date::from_ymd(year, month, 1) + length);
            sim.run_days(length - 1);
            CHECK(built(sim.world(), 0, 0) == 0);  // the last day of the month: not yet
            (void)sim.tick();                      // the 1st of the next month: the building stands
            CHECK(sim.world().today().day() == 1);
            CHECK(built(sim.world(), 0, 0) == kMill);
            require_valid(sim.world(), one_month);
            // the month that ended is the construction month: the building did not stand in it, no upkeep
            const auto row = sim.world().resolve_account(AccountRef::of(DynastyId(0)));
            REQUIRE(row.has_value());
            charged +=
                sim.world().economy().accounts.ring_lines(*row, 0)[*ledger_index(LedgerCode::BuildingUpkeep)];
            // the next month it is charged once, whatever its length
            const int next_length =
                Date::days_in_month(sim.world().today().year(), sim.world().today().month());
            sim.run_days(next_length);
            charged +=
                sim.world().economy().accounts.ring_lines(*row, 0)[*ledger_index(LedgerCode::BuildingUpkeep)];
        }
        CHECK(charged == -12 * 3'000);
    }
}

// ---- saves, replays -------------------------------------------------------------------------------------

TEST_CASE("the queue is saved and loaded, and the run goes on identically") {
    World world = make_world(3, 4);
    Simulation straight = economy_sim(world);
    Simulation first = economy_sim(world);
    const auto order = [](Simulation& sim) {
        REQUIRE(sim.submit(start(DynastyId(0), 0, kMill)));
        REQUIRE(sim.submit(start(DynastyId(0), 1, kGranary)));
        REQUIRE(sim.submit(start(DynastyId(0), 1, kChurch)));
        REQUIRE(sim.submit(start(DynastyId(0), 2, kTract)));
    };
    order(straight);
    order(first);
    straight.run_days(50);  // in the middle of a month, with four sites open
    first.run_days(50);
    REQUIRE(first.world().economy().construction.size() == 4);
    // the first granary is cancelled and another is ordered: a save holds a queue with a hole in the slots
    for (Simulation* sim : {&straight, &first}) {
        REQUIRE(sim->submit(cancel(DynastyId(0), 1, 0)));
        REQUIRE(sim->submit(start(DynastyId(0), 2, kGranary)));
    }
    straight.run_days(2);
    first.run_days(2);

    pat::core::ByteWriter out;
    first.world().save_state(out);
    pat::core::ByteReader in(out.data());
    auto loaded = World::load_state(in);
    REQUIRE(loaded.has_value());
    CHECK(loaded.value().compute_hash() == first.world().compute_hash());
    CHECK(loaded.value().economy() == first.world().economy());
    CHECK(loaded.value().economy().construction == first.world().economy().construction);
    CHECK(loaded.value().economy().meta.next_construction_seq == 5);  // four orders and the new granary
    require_valid(loaded.value());

    Simulation resumed = economy_sim(make_world(3, 4));
    resumed.restore_state(std::move(loaded).value(), {});
    straight.run_days(400);
    resumed.run_days(400);
    CHECK(resumed.world().compute_hash() == straight.world().compute_hash());
    CHECK(resumed.world().economy() == straight.world().economy());
    CHECK(resumed.world().economy().construction.size() == 0);
    CHECK(built(resumed.world(), 0, 0) == kMill);
    CHECK(completed_incidents(resumed.world()) == completed_incidents(straight.world()));
    require_valid(resumed.world());
}

TEST_CASE("commands 36 and 37: tags, names, the log round-trips, the old tags stay") {
    CHECK(static_cast<std::uint8_t>(type_of(start(DynastyId(), 0, kMill))) == 36);
    CHECK(static_cast<std::uint8_t>(type_of(cancel(DynastyId(), 0, 0))) == 37);
    CHECK(to_string(CommandType::StartConstruction) == "StartConstruction");
    CHECK(to_string(CommandType::CancelConstruction) == "CancelConstruction");
    CHECK(static_cast<std::uint8_t>(CommandType::SetTaxPolicy) == 32);
    CHECK(static_cast<std::uint8_t>(CommandType::ConscriptRecruits) == 34);
    CHECK(static_cast<std::uint8_t>(CommandType::AssignProvinceAdministrator) == 35);

    CommandLog log;
    LoggedCommand entry;
    entry.date = Date::from_ymd(1703, 3, 4);
    entry.source = CommandSource::External;
    entry.command = start(DynastyId(1), 6, kWorks);
    log.append(entry);
    entry.command = cancel(DynastyId(1), 6, 5);
    entry.outcome = Rejection::UnknownEntity;
    log.append(entry);
    entry.command =
        Command{DynastyId(), cmd::AdjustHonor{DynastyId(1), 5}};  // an old command in the same file
    entry.outcome = Rejection::None;
    log.append(entry);
    const auto bytes = log.serialize();
    const auto back = CommandLog::deserialize(bytes);
    REQUIRE(back.has_value());
    REQUIRE(back.value().entries().size() == 3);
    CHECK(back.value().entries()[0].command == start(DynastyId(1), 6, kWorks));
    CHECK(back.value().entries()[1].command == cancel(DynastyId(1), 6, 5));
    CHECK(back.value().entries()[1].outcome == Rejection::UnknownEntity);
    CHECK(back.value().entries()[2].command.payload.index() == CommandPayload(cmd::AdjustHonor{}).index());
    CHECK(back.value().serialize() == bytes);
}

TEST_CASE("a replay (PRP1) with construction commands is read back and reproduces the hashes; "
          "a replay made before them still reads") {
    const auto record = [](Replay* out, bool with_construction) {
        Simulation sim = economy_sim(make_world(4, 3));
        ReplayRecorder recorder(5, 78, sim.world().today(), 10);
        for (int day = 0; day < 260; ++day) {
            if (with_construction) {
                if (day == 2) {
                    REQUIRE(sim.submit(start(DynastyId(0), 0, kMill)));
                    REQUIRE(sim.submit(start(DynastyId(0), 1, kGranary)));
                    REQUIRE(sim.submit(start(DynastyId(1), 2, kGranary)));  // refused: not the owner
                }
                if (day == 20) {
                    REQUIRE(sim.submit(start(DynastyId(), 2, kChurch)));
                    REQUIRE(sim.submit(cancel(DynastyId(0), 1, 0)));
                }
                if (day == 40) {
                    REQUIRE(sim.submit(start(DynastyId(0), 1, kTract)));
                }
            } else if (day == 20) {
                REQUIRE(sim.submit(Command{DynastyId(0), cmd::SetTaxPolicy{DynastyId(0), 2}}));
                REQUIRE(sim.submit(Command{DynastyId(), cmd::ConscriptRecruits{ProvinceId(0), 1}}));
            }
            (void)sim.tick();
            recorder.after_tick(sim);
        }
        *out = recorder.finish(sim);
        return sim.world().compute_hash();
    };
    Replay replay;
    const WorldHash final_hash = record(&replay, true);
    std::size_t starts = 0;
    std::size_t cancels = 0;
    for (const auto& entry : replay.commands) {
        starts += type_of(entry.command) == CommandType::StartConstruction ? 1 : 0;
        cancels += type_of(entry.command) == CommandType::CancelConstruction ? 1 : 0;
    }
    CHECK(starts >= 4);
    CHECK(cancels == 1);

    const std::vector<std::uint8_t> bytes = serialize_replay(replay);
    const auto read = deserialize_replay(bytes);
    REQUIRE(read.has_value());
    CHECK(read.value().commands == replay.commands);
    CHECK(serialize_replay(read.value()) == bytes);

    Simulation player = economy_sim(make_world(4, 3));
    const ReplayResult played = play_replay(player, read.value());
    CHECK(played.error.empty());
    CHECK(played.ok);
    CHECK(player.world().compute_hash() == final_hash);
    CHECK(built(player.world(), 0, 0) == kMill);
    require_valid(player.world());
    Replay again;
    CHECK(record(&again, true) == final_hash);
    CHECK(serialize_replay(again) == bytes);

    // a replay of the time before the commands: only old tags, read and played as before
    Replay old;
    const WorldHash old_hash = record(&old, false);
    const auto old_read = deserialize_replay(serialize_replay(old));
    REQUIRE(old_read.has_value());
    Simulation old_player = economy_sim(make_world(4, 3));
    const ReplayResult old_played = play_replay(old_player, old_read.value());
    CHECK(old_played.ok);
    CHECK(old_player.world().compute_hash() == old_hash);
}

// ---- the rules invariants
// -----------------------------------------------------------------------------------

TEST_CASE("the rules invariant E-R1 checks the queue and the grid against the requirements of the types") {
    Simulation sim = economy_sim(make_world(2, 4));
    REQUIRE(sim.submit(start(DynastyId(0), 0, kMill)));
    REQUIRE(sim.submit(start(DynastyId(0), 0, kGranary)));
    (void)sim.tick();
    sim.run_days(5);
    require_valid(sim.world());

    const auto mentions = [](const std::vector<std::string>& problems, std::string_view what) {
        return std::ranges::any_of(problems,
                                   [&](const std::string& p) { return p.find(what) != std::string::npos; });
    };
    {  // an entry that does not last as long as the rules say
        World world = sim.world();
        ConstructionQueue& q = const_cast<ConstructionQueue&>(world.economy().construction);
        const std::size_t i = 0;
        q.finish_date[i] = q.finish_date[i] + 1;
        CHECK(mentions(check_rules_invariants(world, rules()), "days, the rules give"));
    }
    {  // an entry paid another price
        World world = sim.world();
        ConstructionQueue& q = const_cast<ConstructionQueue&>(world.economy().construction);
        q.paid_kop[1] += 1;
        CHECK(mentions(check_rules_invariants(world, rules()), "kopecks, the rules price"));
    }
    {  // more of a type than the rules allow: a second mill in the grid next to the one being built
        World world = sim.world();
        REQUIRE(world.set_slot_building(ProvinceId(0), 3, kMill));
        CHECK(world.check_invariants().empty());
        CHECK(mentions(check_rules_invariants(world, rules()), "holds 2 of building 1, the rules allow 1"));
    }
    {  // a type in a kind of province that does not allow it
        World world = sim.world();
        REQUIRE(world.set_slot_building(ProvinceId(1), 0, kWharf));  // province 1 is rural
        CHECK(mentions(check_rules_invariants(world, rules()), "do not allow in such a province"));
    }
    {  // the development demanded may fall afterwards: not a violation
        World world = sim.world();
        REQUIRE(world.set_slot_building(ProvinceId(1), 0, kWorks));
        world.economy_columns_mut().kind[1] = static_cast<std::uint8_t>(ProvinceKind::Industrial);
        world.set_province_development(ProvinceId(1), 1);
        CHECK(check_rules_invariants(world, rules()).empty());
    }
}
