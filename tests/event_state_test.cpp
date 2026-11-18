// E11-T02: the event engine tables of the World (docs/dev/event-engine.md section 9): storage API,
// bounds, clean-up of a scope, the derived per-scope counters and the invariants. The tables are not saved
// or hashed yet (E11-T03), so these tests also pin that an empty or touched-and-cleared world keeps its hash.

#include "pat/core/fixed.hpp"
#include "pat/sim/events/state.hpp"
#include "pat/sim/world.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <string>

#include "world_fixture.hpp"

using namespace pat::sim;
using namespace pat::sim::events;
using pat::core::Fixed32;
using pat::core::Fixed64;
using pat::core::StringId;

namespace {

Date today() {
    return Date::from_ymd(1703, 5, 20);
}

constexpr EventKey kA = 0x1111;
constexpr EventKey kB = 0x2222;
constexpr EventKey kC = 0x3333;

World make_world() {
    World world(today(), 311);
    REQUIRE(world.enable_dynasty_mode());
    for (std::uint32_t i = 0; i < 3; ++i) {
        REQUIRE(world.create_dynasty(StringId(100 + i)) == DynastyId(i));
        REQUIRE(world.create_province(StringId(200 + i), DynastyId(i), Fixed32::from_int(100)) ==
                ProvinceId(i));
    }
    return world;
}

CharacterId person(World& world, std::uint32_t dynasty = 0) {
    CharacterInit init;
    init.dynasty = DynastyId(dynasty);
    init.sex = Sex::Male;
    init.birth_date = Date::from_ymd(1670, 1, 1);
    const CharacterId id = world.create_character(init);
    REQUIRE(id.valid());
    return id;
}

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

Timer timer(Scope scope, Date due, EventKey key = kA) {
    Timer row;
    row.due = due;
    row.event_key = key;
    row.scope = scope;
    return row;
}

Instance instance(Scope scope, EventKey key = kA, DynastyId recipient = DynastyId(0)) {
    Instance row;
    row.event_key = key;
    row.recipient = recipient;
    row.scope = scope;
    row.fired_on = today();
    row.avail_mask = 3;
    return row;
}

}  // namespace

TEST_CASE("event state: a world without events is empty, valid and keeps its hash") {
    World world = make_world();
    const CharacterId a = person(world);
    CHECK(world.event_state() == State{});
    CHECK(world.event_state().meta.frequency_permille == kDefaultFrequencyPermille);
    pat::test::require_valid(world);
    const WorldHash before = world.compute_hash();

    // Tables are not part of the hash before E11-T03: writing and clearing changes nothing observable.
    REQUIRE(world.set_event_flag(character(a), kA));
    REQUIRE(world.set_event_var(realm(), kB, Fixed64::from_int(3)));
    pat::test::require_valid(world);
    REQUIRE(world.clear_event_flag(character(a), kA));
    REQUIRE(world.clear_event_var(realm(), kB));
    CHECK(world.event_state() == State{});
    CHECK(world.compute_hash() == before);
    CHECK(world.event_rows_of(character(a)) == 0);
}

TEST_CASE("event state: flags expire through their last day and replace in place") {
    World world = make_world();
    const CharacterId a = person(world);
    REQUIRE(world.set_event_flag(character(a), kA));
    REQUIRE(world.set_event_flag(character(a), kB, today() + 2));
    CHECK(world.has_event_flag(character(a), kA));
    CHECK(world.has_event_flag(character(a), kB));
    CHECK_FALSE(world.has_event_flag(character(a), kC));
    CHECK_FALSE(world.has_event_flag(dynasty(0), kA));

    REQUIRE(world.set_event_flag(character(a), kB, today() + 5));  // replaces, one row
    CHECK(world.event_state().flags.size() == 2);
    CHECK(world.find_event_flag(character(a), kB)->expires == today() + 5);
    CHECK(world.event_rows_of(character(a)) == 2);

    // The flag is present through its last day and gone the next day, before any clean-up.
    World later = world;
    for (int i = 0; i < 6; ++i) {
        (void)later.advance_day();
    }
    CHECK(later.has_event_flag(character(a), kA));
    CHECK_FALSE(later.has_event_flag(character(a), kB));
    CHECK(later.find_event_flag(character(a), kB) != nullptr);  // the row stays until the lazy clean-up

    // An end in the past is refused.
    CHECK_FALSE(world.set_event_flag(character(a), kC, today() - 1));
    pat::test::require_valid(world);
}

TEST_CASE("event state: writers refuse bad scopes and keys and change nothing") {
    World world = make_world();
    const CharacterId a = person(world);
    REQUIRE(world.set_event_flag(dynasty(1), kA));
    const State before = world.event_state();

    CHECK_FALSE(world.set_event_flag(dynasty(1), 0));                          // key 0
    CHECK_FALSE(world.set_event_flag(dynasty(7), kA));                         // no such dynasty
    CHECK_FALSE(world.set_event_flag(province(9), kA));                        // no such province
    CHECK_FALSE(world.set_event_flag(Scope::make(ScopeKind::Realm, 1), kA));   // the realm is id 0
    CHECK_FALSE(world.set_event_flag(Scope::make(ScopeKind::Church, 0), kA));  // reserved kind
    CHECK_FALSE(world.set_event_flag(Scope{}, kA));                            // empty scope
    CHECK_FALSE(world.set_event_flag(character(CharacterId(99)), kA));         // no such character
    CHECK_FALSE(world.set_event_var(Scope::make(ScopeKind::Scheme, 0), kA, Fixed64{}));
    CHECK_FALSE(world.set_event_cooldown({dynasty(9), kA, today() + 5}));
    CHECK_FALSE(world.set_event_cooldown({dynasty(0), kA, Date::from_days(2'000'000)}));  // not sane
    CHECK_FALSE(world.set_event_chain({dynasty(0), kA, 1, today(), today() - 1}));
    CHECK_FALSE(world.set_event_modifier({dynasty(0), kA, today() + 1, kNoDate}));  // added in the future
    CHECK(world.event_state() == before);

    // The dead take no new rows.
    REQUIRE(world.die(a, CauseOfDeath::Illness));
    CHECK_FALSE(world.set_event_flag(character(a), kA));
    CHECK_FALSE(world.add_event_timer(timer(character(a), today() + 3)));
    CHECK(world.event_state() == before);
}

TEST_CASE("event state: variables read as zero when absent and keep Fixed64 values") {
    World world = make_world();
    const Fixed64 half = Fixed64::from_raw(Fixed64::from_int(1).raw() / 2);
    CHECK(world.event_var(realm(), kA) == Fixed64{});
    REQUIRE(world.set_event_var(realm(), kA, half));
    REQUIRE(world.set_event_var(dynasty(2), kA, Fixed64::from_int(-7)));
    CHECK(world.event_var(realm(), kA) == half);
    CHECK(world.event_var(dynasty(2), kA) == Fixed64::from_int(-7));
    CHECK(world.event_state().vars.front().scope ==
          dynasty(2));  // ordered by (kind, id, key): dynasty < realm
    REQUIRE(world.clear_event_var(realm(), kA));
    CHECK_FALSE(world.clear_event_var(realm(), kA));
    CHECK(world.event_var(realm(), kA) == Fixed64{});
    pat::test::require_valid(world);
}

TEST_CASE("event state: cooldowns, once and groups") {
    World world = make_world();
    const EventKey group = kC | kCooldownGroupBit;
    REQUIRE(world.set_event_cooldown({dynasty(0), kA, today() + 10}));
    REQUIRE(world.set_event_cooldown({dynasty(0), kB, kForever}));
    REQUIRE(world.set_event_cooldown({dynasty(0), group, today()}));
    CHECK(world.event_on_cooldown(dynasty(0), kA));
    CHECK(world.event_on_cooldown(dynasty(0), kB));
    CHECK_FALSE(world.event_on_cooldown(dynasty(0), group));  // `until` is the first free day
    CHECK_FALSE(world.event_on_cooldown(dynasty(1), kA));
    CHECK_FALSE(world.event_on_cooldown(dynasty(0), kC));
    pat::test::require_valid(world);
}

TEST_CASE("event state: chains and modifiers") {
    World world = make_world();
    REQUIRE(world.set_event_chain({dynasty(0), kA, 2, today() - 30, today()}));
    REQUIRE(world.set_event_chain({dynasty(0), kA, 3, today() - 30, today()}));  // replaces
    CHECK(world.event_state().chains.size() == 1);
    CHECK(world.find_event_chain(dynasty(0), kA)->step == 3);
    REQUIRE(world.set_event_modifier({province(1), kB, today(), today() + 30}));
    CHECK(world.has_event_modifier(province(1), kB));
    CHECK_FALSE(world.has_event_modifier(province(0), kB));
    REQUIRE(world.clear_event_chain(dynasty(0), kA));
    REQUIRE(world.clear_event_modifier(province(1), kB));
    CHECK(world.event_state() == State{});
    pat::test::require_valid(world);
}

TEST_CASE("event state: timers keep (due, seq) order and save their scopes") {
    World world = make_world();
    const CharacterId a = person(world);
    const CharacterId b = person(world);
    Timer late = timer(dynasty(0), today() + 30);
    Timer early = timer(character(a), today() + 2, kB);
    Timer same = timer(province(1), today() + 2, kC);
    same.saved_count = 2;
    same.saved[0] = {7, character(b)};
    same.saved[1] = {9, dynasty(2)};
    const auto s_late = world.add_event_timer(late);
    const auto s_early = world.add_event_timer(early);
    const auto s_same = world.add_event_timer(same);
    REQUIRE((s_late && s_early && s_same));
    CHECK(*s_late == 0);
    CHECK(*s_early == 1);
    CHECK(*s_same == 2);
    const auto& rows = world.event_state().timers;
    REQUIRE(rows.size() == 3);
    CHECK(rows[0].seq == 1);  // due +2, first
    CHECK(rows[1].seq == 2);  // due +2, second
    CHECK(rows[2].seq == 0);  // due +30
    CHECK(world.event_state().meta.next_timer_seq == 3);
    CHECK(world.find_event_timer(2)->saved[1].scope == dynasty(2));

    CHECK_FALSE(world.add_event_timer(timer(dynasty(0), today() - 1)));  // in the past
    Timer dup = timer(dynasty(0), today() + 1);
    dup.saved_count = 2;
    dup.saved[0] = {7, dynasty(0)};
    dup.saved[1] = {7, dynasty(1)};  // names must differ
    CHECK_FALSE(world.add_event_timer(dup));
    CHECK(world.event_state().timers.size() == 3);
    CHECK(world.event_state().meta.next_timer_seq == 3);

    REQUIRE(world.cancel_event_timer(1));
    CHECK_FALSE(world.cancel_event_timer(1));
    CHECK(world.event_rows_of(character(a)) == 0);
    pat::test::require_valid(world);
}

TEST_CASE("event state: tables refuse a new row at their bound and keep replacing old ones") {
    World world = make_world();
    for (std::uint64_t i = 1; i <= kMaxVars; ++i) {
        REQUIRE(world.set_event_var(realm(), i, Fixed64::from_int(1)));
    }
    CHECK_FALSE(world.set_event_var(realm(), kMaxVars + 1, Fixed64::from_int(1)));
    CHECK(world.event_state().vars.size() == kMaxVars);
    CHECK(world.set_event_var(realm(), 5, Fixed64::from_int(9)));  // an existing key still updates
    CHECK(world.event_var(realm(), 5) == Fixed64::from_int(9));

    for (std::size_t i = 0; i < kMaxTimers; ++i) {
        REQUIRE(world.add_event_timer(timer(realm(), today() + 1)));
    }
    CHECK_FALSE(world.add_event_timer(timer(realm(), today() + 1)));
    CHECK(world.event_state().meta.next_timer_seq == kMaxTimers);

    for (std::size_t i = 0; i < kMaxQueue; ++i) {
        REQUIRE(world.enqueue_event_instance(instance(dynasty(0))));
    }
    CHECK_FALSE(world.enqueue_event_instance(instance(dynasty(0))));
    CHECK(world.event_rows_of(realm()) == kMaxVars + kMaxTimers);
    pat::test::require_valid(world);
}

TEST_CASE("event state: the queue assigns ids, validates rows and allows a dead root") {
    World world = make_world();
    const CharacterId a = person(world);
    const auto first = world.enqueue_event_instance(instance(character(a), kA, DynastyId(0)));
    const auto second = world.enqueue_event_instance(instance(dynasty(1), kB, DynastyId(1)));
    REQUIRE((first && second));
    CHECK(*first == 0);
    CHECK(*second == 1);

    Instance bad = instance(dynasty(0));
    bad.state = InstanceState::Chosen;  // Chosen needs an option
    CHECK_FALSE(world.enqueue_event_instance(bad));
    bad = instance(dynasty(0));
    bad.recipient = DynastyId(9);
    CHECK_FALSE(world.enqueue_event_instance(bad));
    bad = instance(dynasty(0));
    bad.avail_mask = 0x10;
    CHECK_FALSE(world.enqueue_event_instance(bad));
    bad = instance(dynasty(0));
    bad.state = InstanceState::Pending;
    bad.defer_until = today() + 3;  // a Pending row does not wait for a date
    CHECK_FALSE(world.enqueue_event_instance(bad));
    CHECK(world.event_state().queue.size() == 2);

    Instance chosen = *world.find_event_instance(*first);
    chosen.state = InstanceState::Chosen;
    chosen.option = 1;
    REQUIRE(world.update_event_instance(chosen));
    CHECK(world.find_event_instance(*first)->option == 1);
    chosen.event_key = kC;  // the event of a row never changes
    CHECK_FALSE(world.update_event_instance(chosen));

    // The root of a queue row may be dead: the on_death event still waits for the player.
    REQUIRE(world.die(a, CauseOfDeath::Murder));
    CHECK(world.find_event_instance(*first) != nullptr);
    pat::test::require_valid(world);
    REQUIRE(world.remove_event_instance(*first));
    CHECK_FALSE(world.remove_event_instance(*first));
    CHECK(world.event_rows_of(character(a)) == 0);
}

TEST_CASE("event state: rings keep the newest entries with ascending seq") {
    World world = make_world();
    for (std::uint32_t i = 0; i < kHistoryRingSize + 20; ++i) {
        HistoryEntry entry;
        entry.date = today();
        entry.event_key = kA;
        entry.detail = static_cast<std::uint16_t>(i);
        REQUIRE(world.append_event_history(entry));
    }
    const auto& history = world.event_state().history;
    REQUIRE(history.size() == kHistoryRingSize);
    CHECK(history.front().seq == 20);
    CHECK(history.back().seq == kHistoryRingSize + 19);
    CHECK(history.front().detail == 20);

    for (std::uint32_t i = 0; i < kJournalRingSize + 5; ++i) {
        JournalEntry entry;
        entry.date = today();
        entry.recipient = DynastyId(1);
        entry.key = kB;
        entry.a = static_cast<std::int32_t>(i);
        REQUIRE(world.append_event_journal(entry));
    }
    CHECK(world.event_state().journal.size() == kJournalRingSize);
    CHECK(world.event_state().journal.front().a == 5);

    HistoryEntry bad;
    bad.date = today();
    bad.outcome = static_cast<HistoryOutcome>(0);
    CHECK_FALSE(world.append_event_history(bad));
    JournalEntry nobody;
    nobody.date = today();
    nobody.recipient = DynastyId(8);
    CHECK_FALSE(world.append_event_journal(nobody));
    pat::test::require_valid(world);
}

TEST_CASE("event state: meta is validated and counters never go back") {
    World world = make_world();
    Meta meta = world.event_state().meta;
    meta.started = 1;
    meta.player_count = 2;
    meta.players[0] = DynastyId(1);
    meta.players[1] = DynastyId(2);
    meta.frequency_permille = 2000;
    meta.modal_month = 1703 * 12 + 4;
    meta.modal_count = 3;
    REQUIRE(world.set_event_meta(meta));
    CHECK(world.event_state().meta == meta);

    Meta bad = meta;
    bad.players[1] = DynastyId(1);  // twice
    CHECK_FALSE(world.set_event_meta(bad));
    bad = meta;
    bad.players[1] = DynastyId(9);  // unknown
    CHECK_FALSE(world.set_event_meta(bad));
    bad = meta;
    bad.player_count = 5;
    CHECK_FALSE(world.set_event_meta(bad));
    bad = meta;
    bad.frequency_permille = 0;
    CHECK_FALSE(world.set_event_meta(bad));
    bad = meta;
    bad.incident_cursor = 1;  // no incident log without the economy
    CHECK_FALSE(world.set_event_meta(bad));
    bad = meta;
    bad.players[2] = DynastyId(0);  // a slot beyond the count must stay empty
    CHECK_FALSE(world.set_event_meta(bad));
    CHECK(world.event_state().meta == meta);

    REQUIRE(world.add_event_timer(timer(realm(), today() + 1)));
    bad = meta;  // next_timer_seq 0 would hand out the seq 0 again
    CHECK_FALSE(world.set_event_meta(bad));
    pat::test::require_valid(world);
}

TEST_CASE("event state: clear_event_scope removes the rows of a scope and cancels waiting instances") {
    World world = make_world();
    const CharacterId a = person(world);
    const CharacterId b = person(world);
    const Scope sa = character(a);
    REQUIRE(world.set_event_flag(sa, kA));
    REQUIRE(world.set_event_var(sa, kA, Fixed64::from_int(2)));
    REQUIRE(world.set_event_cooldown({sa, kA, kForever}));
    REQUIRE(world.set_event_chain({sa, kA, 1, today(), today()}));
    REQUIRE(world.set_event_modifier({sa, kA, today(), kNoDate}));
    REQUIRE(world.add_event_timer(timer(sa, today() + 4)));
    REQUIRE(world.add_event_timer(timer(character(b), today() + 4)));
    const auto pending = world.enqueue_event_instance(instance(sa, kA));
    Instance deferred = instance(sa, kB);
    deferred.state = InstanceState::Deferred;
    deferred.defer_until = today() + 14;
    const auto waits = world.enqueue_event_instance(deferred);
    Instance chosen = instance(sa, kC);
    chosen.state = InstanceState::Chosen;
    chosen.option = 0;
    const auto done = world.enqueue_event_instance(chosen);
    REQUIRE((pending && waits && done));
    REQUIRE(world.set_event_flag(character(b), kA));
    CHECK(world.event_rows_of(sa) == 9);

    CHECK(world.clear_event_scope(sa, true) == 8);  // the Chosen row stays: it is applied first
    CHECK(world.event_rows_of(sa) == 1);
    CHECK(world.find_event_instance(*done) != nullptr);
    CHECK(world.find_event_instance(*pending) == nullptr);
    CHECK(world.event_state().timers.size() == 1);
    CHECK(world.event_state().timers.front().scope == character(b));
    CHECK(world.has_event_flag(character(b), kA));  // another scope is untouched
    REQUIRE(world.event_state().history.size() == 2);
    CHECK(world.event_state().history[0].outcome == HistoryOutcome::Cancelled);
    CHECK(world.event_state().history[0].event_key == kA);
    CHECK(world.event_state().history[1].event_key == kB);
    pat::test::require_valid(world);

    // Without cancel_instances a waiting row is kept (an event that survives the death).
    const auto kept = world.enqueue_event_instance(instance(character(b), kA));
    REQUIRE(kept);
    CHECK(world.clear_event_scope(character(b), false) == 2);
    CHECK(world.find_event_instance(*kept) != nullptr);
    CHECK(world.clear_event_scope(realm(), true) == 0);
}

TEST_CASE("event state: a dead character keeps rows for a day, then the invariants complain") {
    World world = make_world();
    const CharacterId a = person(world);
    REQUIRE(world.set_event_flag(character(a), kA));
    REQUIRE(world.set_event_var(character(a), kA, Fixed64::from_int(1)));
    REQUIRE(world.die(a, CauseOfDeath::OldAge));
    // die() leaves the clean-up to the dispatcher: the on_death events read the flags of the dead.
    CHECK(world.has_event_flag(character(a), kA));
    pat::test::require_valid(world);
    (void)world.advance_day();
    pat::test::require_valid(world);
    (void)world.advance_day();
    const auto problems = world.check_invariants();
    REQUIRE_FALSE(problems.empty());
    CHECK(problems.front().find("E-2") != std::string::npos);

    CHECK(world.clear_event_scope(character(a), true) == 2);
    pat::test::require_valid(world);
}

TEST_CASE("event state: a destroyed character leaves no rows behind (outside dynasty mode)") {
    World world(today(), 5);
    CharacterInit init;
    init.birth_date = Date::from_ymd(1670, 1, 1);
    const CharacterId a = world.create_character(init);
    REQUIRE(world.set_event_flag(character(a), kA));
    REQUIRE(world.add_event_timer(timer(character(a), today() + 1)));
    REQUIRE(world.destroy_character(a));
    CHECK(world.event_state().flags.empty());
    CHECK(world.event_state().timers.empty());
    CHECK(world.event_rows_of(character(a)) == 0);
    pat::test::require_valid(world);
}

TEST_CASE("event state: a copy is independent and keeps its derived counters") {
    World world = make_world();
    REQUIRE(world.set_event_flag(dynasty(0), kA));
    REQUIRE(world.enqueue_event_instance(instance(dynasty(0))));
    World copy = world;
    REQUIRE(copy.set_event_flag(dynasty(0), kB));
    REQUIRE(copy.clear_event_flag(dynasty(0), kA));
    CHECK(world.event_rows_of(dynasty(0)) == 2);
    CHECK(copy.event_rows_of(dynasty(0)) == 2);
    CHECK(world.has_event_flag(dynasty(0), kA));
    CHECK_FALSE(copy.has_event_flag(dynasty(0), kA));
    CHECK(world.event_state() != copy.event_state());
    pat::test::require_valid(world);
    pat::test::require_valid(copy);
    copy.rebuild_event_indexes();
    CHECK(copy.event_rows_of(dynasty(0)) == 2);
}

TEST_CASE("event state: the order of the first access to random streams does not change the hash") {
    World one = make_world();
    World two = make_world();
    for (const char* name : {"events.run", "events.mtth", "events.ai"}) {
        (void)one.rng().stream(name).next_u32();
    }
    for (const char* name : {"events.ai", "events.run", "events.mtth"}) {
        (void)two.rng().stream(name).next_u32();
    }
    // Indexed families (one stream per event key) behave the same way.
    (void)one.rng().stream("events.mtth", 7).next_u32();
    (void)one.rng().stream("events.mtth", 3).next_u32();
    (void)two.rng().stream("events.mtth", 3).next_u32();
    (void)two.rng().stream("events.mtth", 7).next_u32();
    CHECK(one.compute_hash() == two.compute_hash());
}
