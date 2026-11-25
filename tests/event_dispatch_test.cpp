// E11-T05: the dispatcher of the event engine (docs/dev/event-engine.md section 12): the index, the
// producers of the on-actions, the pulses with their stagger, the roll of mean_time_to_happen, cooldown /
// once / pools / cap, the timers, and the proof that the index gives the launches of a full scan, also
// after a save in the middle of a month. The events are built by hand (the compiler is E11-T06).

#include "pat/core/bytes.hpp"
#include "pat/core/fixed.hpp"
#include "pat/sim/events/character_triggers.hpp"
#include "pat/sim/events/dispatch.hpp"
#include "pat/sim/events/event_set.hpp"
#include "pat/sim/events/event_systems.hpp"
#include "pat/sim/simulation.hpp"
#include "pat/sim/world.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "state_sections_fixture.hpp"

using namespace pat::sim;
using namespace pat::sim::events;
using pat::core::Fixed32;
using pat::core::Fixed64;
using pat::core::StringId;

namespace {

constexpr std::uint16_t kMonthly = 2;
constexpr std::uint16_t kQuarterly = 3;
constexpr std::uint16_t kYearly = 4;
constexpr std::uint16_t kDaily = 5;

Scope character(CharacterId id) {
    return Scope::make(ScopeKind::Character, id.value());
}
Scope dynasty(std::uint32_t id) {
    return Scope::make(ScopeKind::Dynasty, id);
}
Scope province(std::uint32_t id) {
    return Scope::make(ScopeKind::Province, id);
}
Scope realm() {
    return Scope::make(ScopeKind::Realm, 0);
}

struct Setup {
    World world;
    std::vector<CharacterId> people;  // 30 per dynasty, dynasty d has ids d*30 .. d*30+29
};

/// Four dynasties of 30 adults, twelve provinces; the first member of each dynasty is its head.
Setup make_setup(Date start, std::uint64_t seed = 11, bool economy = false) {
    Setup s{World(start, seed), {}};
    World& world = s.world;
    REQUIRE(world.enable_dynasty_mode());
    for (std::uint32_t d = 0; d < 4; ++d) {
        REQUIRE(world.create_dynasty(StringId(100 + d)) == DynastyId(d));
    }
    for (std::uint32_t p = 0; p < 12; ++p) {
        REQUIRE(world.create_province(StringId(200 + p), DynastyId(p % 4), Fixed32::from_int(100)) ==
                ProvinceId(p));
    }
    for (std::uint32_t d = 0; d < 4; ++d) {
        for (int k = 0; k < 30; ++k) {
            CharacterInit init;
            init.dynasty = DynastyId(d);
            init.sex = k % 2 == 0 ? Sex::Male : Sex::Female;
            init.birth_date = Date::from_ymd(1650 + (k * 7) % 30, 1 + k % 12, 1 + k % 27);
            const CharacterId id = world.create_character(init);
            REQUIRE(id.valid());
            s.people.push_back(id);
        }
        REQUIRE(world.set_dynasty_head(DynastyId(d), s.people[d * 30]));
    }
    if (economy) {
        REQUIRE(world.enable_economy());
    }
    return s;
}

OnSub on(std::uint16_t action, std::uint8_t role = 0) {
    return OnSub{action, role};
}

EventDef make_event(const std::string& id, ScopeKind scope, std::vector<OnSub> subs, std::int32_t priority = 50) {
    EventDef def;
    def.id = id;
    def.key = event_key(id);
    def.scope = scope;
    def.on = std::move(subs);
    def.priority = priority;
    return def;
}

/// mean_time_to_happen of `months` on the monthly pulse (section 12.4).
void set_mtth_months(EventDef& def, std::int32_t months) {
    def.mtth_days = months * 30;
    def.mtth_ppm[static_cast<std::size_t>(PulseKind::Monthly)] = mtth_ppm(30, def.mtth_days);
}

Program build(const std::function<void(ProgramBuilder&)>& fn) {
    ProgramBuilder b;
    fn(b);
    auto result = b.finish();
    REQUIRE(result.has_value());
    return std::move(result).value();
}

Program pred_program(std::uint16_t code) {
    return build([&](ProgramBuilder& p) {
        p.emit(Op::kPred, code, 0);
        p.emit(Op::kRet);
    });
}

Program constant_program(std::int32_t value) {
    return build([&](ProgramBuilder& p) {
        p.push_int(value);
        p.emit(Op::kRet);
    });
}

std::shared_ptr<EventSet> make_set(std::vector<EventDef> defs) {
    auto set = std::make_shared<EventSet>();
    set->events = std::move(defs);
    std::ranges::sort(set->events, [](const EventDef& a, const EventDef& b) { return a.key < b.key; });
    return set;
}

/// The Catalog the dispatcher reads: the language entries and the character block.
Catalog test_catalog() {
    Catalog c = Catalog::engine();
    register_character_triggers(c);
    return c;
}

Simulation make_sim(World world) {
    return Simulation(std::move(world));
}

std::size_t history_count(const World& world, const EventDef& def) {
    return static_cast<std::size_t>(std::ranges::count_if(
        world.event_state().history, [&](const HistoryEntry& h) { return h.event_key == def.key; }));
}

}  // namespace

TEST_CASE("event dispatch: the index lists events by priority and key, per scope and action") {
    std::vector<EventDef> defs;
    defs.push_back(make_event("a.low", ScopeKind::Character, {on(8)}, 10));
    defs.push_back(make_event("b.high", ScopeKind::Character, {on(8), on(kMonthly)}, 90));
    defs.push_back(make_event("c.house", ScopeKind::Dynasty, {on(8, 1)}));
    defs.push_back(make_event("d.tie", ScopeKind::Character, {on(8)}, 90));
    const auto set = make_set(std::move(defs));
    const DispatchIndex index(*set, test_catalog());

    const auto at = [&](const char* id) { return static_cast<std::uint32_t>(set->find(id) - set->events.data()); };
    const std::span<const Subscriber> death = index.subscribers(ScopeKind::Character, 8);
    REQUIRE(death.size() == 3);
    // priority 90 first (b.high and d.tie by ascending key), then the low one
    const std::uint64_t kb = event_key("b.high");
    const std::uint64_t kd = event_key("d.tie");
    CHECK(set->events[death[0].event].key == std::min(kb, kd));
    CHECK(set->events[death[1].event].key == std::max(kb, kd));
    CHECK(death[2].event == at("a.low"));
    const std::span<const Subscriber> house = index.subscribers(ScopeKind::Dynasty, 8);
    REQUIRE(house.size() == 1);
    CHECK(house[0].role == 1);
    CHECK(index.pulse(ScopeKind::Character, PulseKind::Monthly).size() == 1);
    CHECK(index.pulse(ScopeKind::Character, PulseKind::Yearly).empty());
    CHECK(index.subscribers(ScopeKind::Province, 8).empty());
    CHECK(index.subscriptions() == 5);
    CHECK(index.rank_of(at("a.low")) == 3);
}

TEST_CASE("event dispatch: a simulation without the system is untouched; the system only adds its cursors") {
    auto a = make_setup(Date::from_ymd(1703, 1, 31));
    auto b = make_setup(Date::from_ymd(1703, 1, 31));
    Simulation plain = make_sim(std::move(a.world));
    Simulation with = make_sim(std::move(b.world));
    (void)install_event_systems(with, make_set({}), test_catalog());
    plain.run_days(10);
    with.run_days(10);
    // the only difference is the meta record of the events (the first pass marks the game as started)
    CHECK(with.world().event_state().meta.started == 1);
    CHECK(plain.world().event_state().meta.started == 0);
    CHECK(with.world().event_state().history.empty());
}

TEST_CASE("event dispatch: on_game_start runs once, also across a save") {
    auto s = make_setup(Date::from_ymd(1703, 1, 31));
    EventDef start = make_event("t.start", ScopeKind::Realm, {on(1)});
    const auto set = make_set({start});
    Simulation sim = make_sim(std::move(s.world));
    const auto dispatcher = install_event_systems(sim, set, test_catalog());
    (void)sim.tick();
    REQUIRE(dispatcher->launches().size() == 1);
    CHECK(dispatcher->launches()[0].scope == realm());
    CHECK(dispatcher->launches()[0].key == start.key);
    (void)sim.tick();
    CHECK(dispatcher->launches().empty());

    auto loaded = pat::test::load_state(pat::test::state_bytes(sim.world()));
    REQUIRE(loaded.has_value());
    Simulation second = make_sim(make_setup(Date::from_ymd(1703, 1, 31)).world);
    const auto again = install_event_systems(second, set, test_catalog());
    second.restore_state(std::move(loaded).value(), {});
    second.run_days(3);
    CHECK(history_count(second.world(), *set->find(start.key)) == 1);
    CHECK(again->launches().empty());
}

TEST_CASE("event dispatch: EVT-A1, a pulse event with a trigger starts only where it fits") {
    // February 1703 has 28 days: every monthly slot is visited exactly once
    auto s = make_setup(Date::from_ymd(1703, 1, 31));
    EventDef heads = make_event("t.heads", ScopeKind::Character, {on(kMonthly)});
    heads.trigger = pred_program(kTrigIsHeadOfDynasty);
    Simulation sim = make_sim(std::move(s.world));
    const auto dispatcher = install_event_systems(sim, make_set({heads}), test_catalog());
    std::vector<Scope> started;
    for (int day = 0; day < 28; ++day) {
        (void)sim.tick();
        for (const Launch& l : dispatcher->launches()) {
            started.push_back(l.scope);
        }
    }
    std::ranges::sort(started, [](Scope a, Scope b) { return a.word() < b.word(); });
    std::vector<Scope> expected;
    for (std::uint32_t d = 0; d < 4; ++d) {
        expected.push_back(character(s.people[d * 30]));
    }
    std::ranges::sort(expected, [](Scope a, Scope b) { return a.word() < b.word(); });
    CHECK(started == expected);
    CHECK(history_count(sim.world(), heads) == 4);
}

TEST_CASE("event dispatch: mean_time_to_happen gives the expected frequency over ten years") {
    auto s = make_setup(Date::from_ymd(1703, 1, 31));
    EventDef rare = make_event("t.rare", ScopeKind::Character, {on(kMonthly)});
    set_mtth_months(rare, 24);
    CHECK(rare.mtth_ppm[static_cast<std::size_t>(PulseKind::Monthly)] == 41'667);
    Simulation sim = make_sim(std::move(s.world));
    const auto dispatcher = install_event_systems(sim, make_set({rare}), test_catalog());
    std::size_t launches = 0;
    std::size_t rolls = 0;
    for (int day = 0; day < 365 * 10; ++day) {
        (void)sim.tick();
        launches += dispatcher->launches().size();
        rolls += dispatcher->stats().rolls;
    }
    // 120 characters x 120 monthly pulses / 24 months = 600 launches expected
    CHECK(rolls >= 14300);
    CHECK(rolls <= 14500);
    CHECK(launches > 510);
    CHECK(launches < 690);
}

TEST_CASE("event dispatch: the player frequency scales the probability of the players only") {
    const auto run = [](std::uint16_t permille) {
        auto s = make_setup(Date::from_ymd(1703, 1, 31));
        Meta meta = s.world.event_state().meta;
        meta.frequency_permille = permille;
        meta.player_count = 1;
        meta.players[0] = DynastyId(0);
        REQUIRE(s.world.set_event_meta(meta));
        EventDef def = make_event("t.freq", ScopeKind::Character, {on(kMonthly)});
        set_mtth_months(def, 12);
        Simulation sim = make_sim(std::move(s.world));
        const auto dispatcher = install_event_systems(sim, make_set({def}), test_catalog());
        std::array<std::size_t, 4> per_house{};
        for (int day = 0; day < 365 * 10; ++day) {
            (void)sim.tick();
            for (const Launch& l : dispatcher->launches()) {
                ++per_house[sim.world().characters().dynasty[l.scope.id].value()];
            }
        }
        return per_house;
    };
    const auto normal = run(1000);
    const auto halved = run(500);
    // the player's house (0) halves; the others do not change
    CHECK(halved[0] < normal[0] * 6 / 10);
    CHECK(halved[0] > normal[0] * 4 / 10);
    CHECK(halved[1] == normal[1]);
    CHECK(halved[2] == normal[2]);
    CHECK(halved[3] == normal[3]);
}

TEST_CASE("event dispatch: once, cooldown and the group pause an event on a scope") {
    SUBCASE("once = scope: each character starts it once in a year") {
        auto s = make_setup(Date::from_ymd(1703, 1, 31));
        EventDef def = make_event("t.once", ScopeKind::Character, {on(kMonthly)});
        def.once = OnceMode::Scope;
        Simulation sim = make_sim(std::move(s.world));
        (void)install_event_systems(sim, make_set({def}), test_catalog());
        sim.run_days(365);
        CHECK(history_count(sim.world(), def) == 120);
    }
    SUBCASE("once = global: one start in the whole realm") {
        auto s = make_setup(Date::from_ymd(1703, 1, 31));
        EventDef def = make_event("t.global", ScopeKind::Realm, {on(kMonthly)});
        def.once = OnceMode::Global;
        Simulation sim = make_sim(std::move(s.world));
        (void)install_event_systems(sim, make_set({def}), test_catalog());
        sim.run_days(365);
        CHECK(history_count(sim.world(), def) == 1);
    }
    SUBCASE("cooldown: a pause of ninety days lets a monthly pulse through every fourth month") {
        auto s = make_setup(Date::from_ymd(1703, 1, 31));
        EventDef def = make_event("t.cool", ScopeKind::Realm, {on(kMonthly)});
        def.cooldown_days = 90;
        Simulation sim = make_sim(std::move(s.world));
        (void)install_event_systems(sim, make_set({def}), test_catalog());
        sim.run_days(365);
        // Feb 1 starts it, the pause lasts to May 2: the next start is the pulse of June 1 or so
        const std::size_t count = history_count(sim.world(), def);
        CHECK(count >= 3);
        CHECK(count <= 4);
    }
    SUBCASE("a cooldown group pauses the other members") {
        auto s = make_setup(Date::from_ymd(1703, 1, 31));
        EventDef a = make_event("t.group_a", ScopeKind::Realm, {on(kMonthly)}, 80);
        a.cooldown_days = 400;
        a.cooldown_group = 0x77;
        EventDef b = make_event("t.group_b", ScopeKind::Realm, {on(kQuarterly)}, 60);
        b.cooldown_group = 0x77;
        EventDef c = make_event("t.group_c", ScopeKind::Realm, {on(kQuarterly)}, 40);  // no group: starts
        Simulation sim = make_sim(std::move(s.world));
        (void)install_event_systems(sim, make_set({a, b, c}), test_catalog());
        sim.run_days(120);  // February 1 .. May 31: the quarterly pulse is on April 1
        CHECK(history_count(sim.world(), a) == 1);
        CHECK(history_count(sim.world(), b) == 0);
        CHECK(history_count(sim.world(), c) == 1);
    }
}

TEST_CASE("event dispatch: pools pick one event per scope, the cap lets one pulse event through") {
    SUBCASE("pool") {
        auto s = make_setup(Date::from_ymd(1703, 1, 31));
        EventDef a = make_event("t.pool_a", ScopeKind::Character, {on(kMonthly)});
        a.pool = 0x5151;
        a.weight = constant_program(1);
        EventDef b = make_event("t.pool_b", ScopeKind::Character, {on(kMonthly)});
        b.pool = 0x5151;
        b.weight = constant_program(3);
        Simulation sim = make_sim(std::move(s.world));
        const auto dispatcher = install_event_systems(sim, make_set({a, b}), test_catalog());
        std::size_t launches = 0;
        std::size_t losers = 0;
        for (int day = 0; day < 28; ++day) {
            (void)sim.tick();
            launches += dispatcher->launches().size();
            losers += dispatcher->stats().pool_losers;
        }
        // one winner per character; the other lost the draw (and then the cap does not matter)
        CHECK(launches == 120);
        CHECK(losers == 120);
        CHECK(history_count(sim.world(), b) > history_count(sim.world(), a));  // weight 3 against 1
    }
    SUBCASE("cap") {
        auto s = make_setup(Date::from_ymd(1703, 1, 31));
        EventDef high = make_event("t.cap_high", ScopeKind::Character, {on(kMonthly)}, 70);
        EventDef low = make_event("t.cap_low", ScopeKind::Character, {on(kMonthly)}, 30);
        Simulation sim = make_sim(std::move(s.world));
        const auto dispatcher = install_event_systems(sim, make_set({low, high}), test_catalog());
        std::size_t capped = 0;
        for (int day = 0; day < 28; ++day) {
            (void)sim.tick();
            capped += dispatcher->stats().capped;
        }
        CHECK(history_count(sim.world(), high) == 120);
        CHECK(history_count(sim.world(), low) == 0);
        CHECK(capped == 120);
    }
}

TEST_CASE("event dispatch: the commands of the tick are the on-actions") {
    auto s = make_setup(Date::from_ymd(1703, 1, 31));
    EventDef death = make_event("t.death", ScopeKind::Character, {on(8)});
    EventDef house = make_event("t.death_house", ScopeKind::Dynasty, {on(8, 1)});
    EventDef owner = make_event("t.owner", ScopeKind::Province, {on(17)});
    owner.saved_names = {"old_owner", "new_owner"};
    owner.provided_saved = 2;
    EventDef wed = make_event("t.wed", ScopeKind::Character, {on(9), on(9, 1)});
    const auto set = make_set({death, house, owner, wed});
    Simulation sim = make_sim(std::move(s.world));
    const auto dispatcher = install_event_systems(sim, set, test_catalog());
    (void)sim.tick();  // the first pass (nothing happens)

    const CharacterId dead = s.people[31];       // dynasty 1, not a head
    const CharacterId groom = s.people[60 + 2];  // dynasty 2, male (k = 2)
    const CharacterId bride = s.people[60 + 3];  // dynasty 2, female (k = 3)
    REQUIRE(sim.submit(Command{{}, cmd::Die{dead, CauseOfDeath::Illness}}));
    REQUIRE(sim.submit(Command{{}, cmd::Marry{bride, groom}}));
    REQUIRE(sim.submit(Command{{}, cmd::SetProvinceOwner{ProvinceId(0), DynastyId(3)}}));
    REQUIRE(sim.submit(Command{{}, cmd::SetProvinceOwner{ProvinceId(0), DynastyId(2)}}));
    REQUIRE(sim.submit(Command{{}, cmd::Die{dead, CauseOfDeath::Illness}}));  // already dead: refused
    (void)sim.tick();

    const auto& occurrences = dispatcher->occurrences();
    REQUIRE(occurrences.size() == 4);
    CHECK(occurrences[0].action == 8);
    CHECK(occurrences[0].roots[0] == character(dead));
    CHECK(occurrences[0].roots[1] == dynasty(1));
    CHECK(occurrences[0].param == static_cast<std::int32_t>(CauseOfDeath::Illness));
    // the groom is the male one, whatever the order of the command
    CHECK(occurrences[1].action == 9);
    CHECK(occurrences[1].roots[0] == character(groom));
    CHECK(occurrences[1].roots[1] == character(bride));

    // the launches: the person, the house, the province twice in a row but once (the same candidate),
    // the wedding on both spouses
    std::vector<std::string> names;
    for (const Launch& l : dispatcher->launches()) {
        names.push_back(set->events[l.event].id);
    }
    CHECK(std::ranges::count(names, "t.death") == 1);
    CHECK(std::ranges::count(names, "t.death_house") == 1);
    CHECK(std::ranges::count(names, "t.owner") == 1);
    CHECK(std::ranges::count(names, "t.wed") == 2);
    CHECK(dispatcher->stats().duplicates == 1);
    for (const Launch& l : dispatcher->launches()) {
        if (set->events[l.event].id == "t.owner") {
            // the first change counts: from the old owner 0 to 3 (the second one, 3 to 2, is the duplicate)
            CHECK(l.scope == province(0));
            CHECK(l.saved[0] == dynasty(0));
            CHECK(l.saved[1] == dynasty(3));
        }
    }
    // a dead character is a legal scope of on_death only
    CHECK(!sim.world().is_alive(dead));
}

TEST_CASE("event dispatch: the incident log and the chronicle are producers") {
    auto s = make_setup(Date::from_ymd(1703, 1, 31), 11, true);
    EventDef riot = make_event("t.riot", ScopeKind::Province, {on(19)});
    EventDef riot_house = make_event("t.riot_house", ScopeKind::Dynasty, {on(19, 1)});
    EventDef bankrupt = make_event("t.bankrupt", ScopeKind::Dynasty, {on(22)});
    EventDef head = make_event("t.head", ScopeKind::Dynasty, {on(13)});
    head.saved_names = {"predecessor", "successor"};
    head.provided_saved = 2;
    const auto set = make_set({riot, riot_house, bankrupt, head});
    REQUIRE(s.world.log_incident(economy::IncidentKind::Riot, ProvinceId(5), DynastyId{}, 40));
    REQUIRE(s.world.log_incident(economy::IncidentKind::Bankruptcy, ProvinceId{}, DynastyId(2), 0));
    Simulation sim = make_sim(std::move(s.world));
    const auto dispatcher = install_event_systems(sim, set, test_catalog());
    const CharacterId old_head = s.people[0];
    const CharacterId new_head = s.people[2];
    REQUIRE(sim.submit(Command{{}, cmd::SetDynastyHead{DynastyId(0), new_head}}));
    (void)sim.tick();
    std::vector<std::string> names;
    for (const Launch& l : dispatcher->launches()) {
        names.push_back(set->events[l.event].id);
        if (set->events[l.event].id == "t.riot") {
            CHECK(l.scope == province(5));
            CHECK(l.param == 40);
        }
        if (set->events[l.event].id == "t.head") {
            CHECK(l.scope == dynasty(0));
            CHECK(l.saved[0] == character(old_head));
            CHECK(l.saved[1] == character(new_head));
        }
    }
    CHECK(std::ranges::count(names, "t.riot") == 1);
    CHECK(std::ranges::count(names, "t.riot_house") == 1);  // province 5 belongs to house 1
    CHECK(std::ranges::count(names, "t.bankrupt") == 1);
    CHECK(std::ranges::count(names, "t.head") == 1);
    // the cursor moved: the next tick sees nothing new
    (void)sim.tick();
    CHECK(dispatcher->launches().empty());
    CHECK(sim.world().event_state().meta.incident_cursor == sim.world().economy().meta.next_incident_seq);
}

TEST_CASE("event dispatch: a due timer starts its event with the saved depth, a dead scope drops it") {
    auto s = make_setup(Date::from_ymd(1703, 1, 31));
    EventDef follow = make_event("t.follow", ScopeKind::Character, {});
    const auto set = make_set({follow});
    Timer a;
    a.due = Date::from_ymd(1703, 2, 2);
    a.event_key = follow.key;
    a.scope = character(s.people[5]);
    a.depth = 3;
    Timer b = a;
    b.scope = character(s.people[6]);
    b.due = Date::from_ymd(1703, 2, 3);
    REQUIRE(s.world.add_event_timer(a));
    REQUIRE(s.world.add_event_timer(b));
    Simulation sim = make_sim(std::move(s.world));
    const auto dispatcher = install_event_systems(sim, set, test_catalog());
    (void)sim.tick();  // February 1
    CHECK(dispatcher->launches().empty());
    (void)sim.tick();  // February 2
    REQUIRE(dispatcher->launches().size() == 1);
    CHECK(dispatcher->launches()[0].from_timer);
    CHECK(dispatcher->launches()[0].depth == 3);
    CHECK(dispatcher->launches()[0].scope == character(s.people[5]));
    CHECK(sim.world().event_state().timers.size() == 1);
    // the second one is due tomorrow, its owner dies tonight: the timer is dropped without a start
    REQUIRE(sim.submit(Command{{}, cmd::Die{s.people[6], CauseOfDeath::Illness}}));
    (void)sim.tick();  // February 3
    CHECK(dispatcher->launches().empty());
    CHECK(sim.world().event_state().timers.empty());
}

namespace {

/// A mixed set of events for the equivalence and save tests: pulses of every period, actions, triggers,
/// chance, once, cooldown, pools.
std::shared_ptr<EventSet> mixed_set() {
    std::vector<EventDef> defs;
    for (int i = 0; i < 24; ++i) {
        EventDef def = make_event("m.pulse" + std::to_string(i), ScopeKind::Character, {on(kMonthly)}, 10 + i % 5);
        set_mtth_months(def, 6 + i);
        if (i % 3 == 0) {
            def.trigger = pred_program(kTrigIsAdult);
        }
        if (i % 4 == 0) {
            def.cooldown_days = 200;
        }
        if (i % 5 == 0) {
            def.once = OnceMode::Scope;
        }
        if (i % 6 == 0) {
            def.pool = 0x900 + static_cast<std::uint64_t>(i % 2);
            def.weight = constant_program(1 + i % 3);
        }
        defs.push_back(def);
    }
    for (int i = 0; i < 6; ++i) {
        EventDef def = make_event("m.house" + std::to_string(i), ScopeKind::Dynasty, {on(kMonthly), on(8, 1)});
        set_mtth_months(def, 4 + i);
        defs.push_back(def);
    }
    EventDef quarter = make_event("m.quarter", ScopeKind::Character, {on(kQuarterly)});
    quarter.mtth_days = 91 * 3;
    quarter.mtth_ppm[static_cast<std::size_t>(PulseKind::Quarterly)] = mtth_ppm(91, quarter.mtth_days);
    defs.push_back(quarter);
    EventDef yearly = make_event("m.yearly", ScopeKind::Province, {on(kYearly)});
    defs.push_back(yearly);
    EventDef daily = make_event("m.daily", ScopeKind::Realm, {on(kDaily)});
    daily.has_window = true;
    daily.window_from = Date::from_ymd(1703, 3, 1).days();
    daily.window_to = Date::from_ymd(1703, 3, 3).days();
    defs.push_back(daily);
    for (int i = 0; i < 6; ++i) {
        EventDef def = make_event("m.death" + std::to_string(i), ScopeKind::Character, {on(8)}, 20 + i);
        if (i % 2 == 0) {
            def.chance = constant_program(50);
        }
        defs.push_back(def);
    }
    EventDef wedding = make_event("m.wed", ScopeKind::Character, {on(9), on(9, 1)});
    defs.push_back(wedding);
    EventDef owner = make_event("m.owner", ScopeKind::Province, {on(17)});
    defs.push_back(owner);
    return make_set(std::move(defs));
}

/// The same commands for every run of the same scenario: deaths, weddings and a change of owner.
void feed(Simulation& sim, const Setup& s) {
    const Date today = sim.world().today();
    const int day = today.days() - Date::from_ymd(1703, 1, 31).days();
    if (day % 17 == 5) {
        const auto target = s.people[static_cast<std::size_t>(day * 7) % s.people.size()];
        (void)sim.submit(Command{{}, cmd::Die{target, CauseOfDeath::Illness}});
    }
    if (day % 23 == 9) {
        const auto a = s.people[static_cast<std::size_t>(day * 5) % s.people.size()];
        const auto b = s.people[static_cast<std::size_t>(day * 5 + 1) % s.people.size()];
        (void)sim.submit(Command{{}, cmd::Marry{a, b}});
    }
    if (day % 31 == 11) {
        (void)sim.submit(Command{{},
                                 cmd::SetProvinceOwner{ProvinceId(static_cast<std::uint32_t>(day) % 12),
                                                       DynastyId(static_cast<std::uint32_t>(day / 3) % 4)}});
    }
}

}  // namespace

TEST_CASE("event dispatch: the index and the full scan give the same candidates and launches every day") {
    const auto set = mixed_set();
    auto a = make_setup(Date::from_ymd(1703, 1, 31));
    auto b = make_setup(Date::from_ymd(1703, 1, 31));
    Simulation fast = make_sim(std::move(a.world));
    Simulation slow = make_sim(std::move(b.world));
    const auto indexed = install_event_systems(fast, set, test_catalog());
    const auto scan = install_event_systems(slow, set, test_catalog());
    scan->set_mode(DispatchMode::FullScan);
    const Setup shape = make_setup(Date::from_ymd(1703, 1, 31));

    std::size_t launches = 0;
    std::size_t candidates = 0;
    for (int day = 0; day < 400; ++day) {
        feed(fast, shape);
        feed(slow, shape);
        (void)fast.tick();
        (void)slow.tick();
        REQUIRE(indexed->candidates() == scan->candidates());
        REQUIRE(indexed->launches() == scan->launches());
        REQUIRE(indexed->stats() == scan->stats());
        launches += indexed->launches().size();
        candidates += indexed->candidates().size();
    }
    CHECK(fast.world().compute_hash() == slow.world().compute_hash());
    // the scenario actually exercises the dispatcher
    CHECK(launches > 100);
    CHECK(candidates > 2000);
}

TEST_CASE("event dispatch: a save in the middle of a month and the replay reproduce the launches") {
    const auto set = mixed_set();
    const Setup shape = make_setup(Date::from_ymd(1703, 1, 31));
    // the reference: 200 uninterrupted days
    auto whole = make_setup(Date::from_ymd(1703, 1, 31));
    Simulation reference = make_sim(std::move(whole.world));
    const auto ref_dispatcher = install_event_systems(reference, set, test_catalog());
    std::vector<std::vector<Launch>> ref_launches;
    std::vector<std::uint64_t> ref_hash;
    for (int day = 0; day < 200; ++day) {
        feed(reference, shape);
        (void)reference.tick();
        ref_launches.push_back(ref_dispatcher->launches());
    }
    ref_hash.push_back(reference.world().compute_hash().total);

    // the interrupted run: 83 days (the 24th of April: mid-month), save, load into a new simulation
    auto first = make_setup(Date::from_ymd(1703, 1, 31));
    Simulation before = make_sim(std::move(first.world));
    const auto before_dispatcher = install_event_systems(before, set, test_catalog());
    for (int day = 0; day < 83; ++day) {
        feed(before, shape);
        (void)before.tick();
        CHECK(before_dispatcher->launches() == ref_launches[static_cast<std::size_t>(day)]);
    }
    const std::vector<std::uint8_t> bytes = pat::test::state_bytes(before.world());
    {
        auto probe = pat::test::load_state(bytes);
        INFO((probe.has_value() ? std::string("ok") : probe.error().message));
        REQUIRE(probe.has_value());
    }

    SUBCASE("a new simulation loaded from the save") {
        auto loaded = pat::test::load_state(bytes);
        REQUIRE(loaded.has_value());
        // systems are registered on a fresh simulation, the save comes in through restore_state
        Simulation after = make_sim(make_setup(Date::from_ymd(1703, 1, 31)).world);
        const auto after_dispatcher = install_event_systems(after, set, test_catalog());
        after.restore_state(std::move(loaded).value(), {});
        for (int day = 83; day < 200; ++day) {
            feed(after, shape);
            (void)after.tick();
            REQUIRE(after_dispatcher->launches() == ref_launches[static_cast<std::size_t>(day)]);
        }
        CHECK(after.world().compute_hash().total == ref_hash[0]);
    }
    SUBCASE("the same simulation restored from the save (on_state_restored)") {
        // run on for a while, then go back to the saved state: the dispatcher forgets the days in between
        for (int day = 83; day < 120; ++day) {
            feed(before, shape);
            (void)before.tick();
        }
        auto loaded = pat::test::load_state(bytes);
        REQUIRE(loaded.has_value());
        before.restore_state(std::move(loaded).value(), {});
        for (int day = 83; day < 200; ++day) {
            feed(before, shape);
            (void)before.tick();
            REQUIRE(before_dispatcher->launches() == ref_launches[static_cast<std::size_t>(day)]);
        }
        CHECK(before.world().compute_hash().total == ref_hash[0]);
    }
}
