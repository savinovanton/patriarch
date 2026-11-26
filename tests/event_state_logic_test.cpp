// E11-T11: the logic over the tables of events::State (docs/dev/event-engine.md sections 8.3 "T11", 9.5,
// 11.1, 11.3). Events are compiled from text with the standard catalog and run by the dispatcher in a
// Simulation: `immediate` commits its state operations, a delayed `trigger_event` waits in a timer, an
// immediate one starts in the same pass, the saved scopes travel by name, the dead lose their rows (EVT-A3),
// loops are cut, a save in the middle of a timer continues like an uninterrupted run.

#include "pat/data/content_source.hpp"
#include "pat/loc/localizer.hpp"
#include "pat/sim/command.hpp"
#include "pat/sim/events/catalog.hpp"
#include "pat/sim/events/dispatch.hpp"
#include "pat/sim/events/event_set.hpp"
#include "pat/sim/events/event_systems.hpp"
#include "pat/sim/events/state_effects.hpp"
#include "pat/sim/simulation.hpp"
#include "pat/sim/world.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "court_fixture.hpp"
#include "event_compile_fixture.hpp"
#include "state_sections_fixture.hpp"

using namespace pat::sim;
using namespace pat::sim::events;
using pat::core::Fixed64;
using pat::core::StringId;

namespace {

Scope ch(CharacterId id) {
    return Scope::make(ScopeKind::Character, id.value());
}
constexpr Scope kGlobal{ScopeKind::Realm, 0};

constexpr Date kStart = Date::from_ymd(1703, 1, 1);

struct Setup {
    World world{kStart, 5};
    std::vector<CharacterId> a;  // house 0
    std::vector<CharacterId> b;  // house 1

    Setup() {
        REQUIRE(world.enable_dynasty_mode());
        for (std::uint32_t d = 0; d < 2; ++d) {
            REQUIRE(world.create_dynasty(StringId(10 + d)) == DynastyId(d));
            REQUIRE(world.create_province(
                        StringId(20 + d), DynastyId(d), pat::core::Fixed32::from_int(100)) == ProvinceId(d));
            for (int k = 0; k < 4; ++k) {
                CharacterInit init;
                init.dynasty = DynastyId(d);
                init.sex = k % 2 == 0 ? Sex::Male : Sex::Female;
                init.birth_date = Date::from_ymd(1660 + k, 1 + k, 1);
                const CharacterId id = world.create_character(init);
                REQUIRE(id.valid());
                (d == 0 ? a : b).push_back(id);
            }
            REQUIRE(world.set_dynasty_head(DynastyId(d), (d == 0 ? a : b).front()));
        }
    }
};

constexpr const char* kEvents = R"(
event "t.audit" {
    scope = character
    type = personal
    category = general
    on = monthly_pulse
    trigger = { has_flag = pick }
    immediate = {
        remove_flag = pick
        save_scope_as = auditor
        set_flag = { name = audit_pending  months = 6 }
        trigger_event = { id = t.audit_result  months = 3 }
        start_chain = audit_story
    }
    option "ok" { }
}

event "t.audit_result" {
    scope = character
    type = personal
    category = general
    chain_scopes = { auditor = character }
    trigger = {
        has_flag = audit_pending
        scope.auditor = { has_flag = audit_pending }
    }
    immediate = {
        set_chain_step = { name = audit_story  step = 2 }
        set_global_flag = audit_done
    }
    option "ok" { }
}

event "t.loop" {
    scope = character
    type = personal
    category = general
    on = monthly_pulse
    once = scope
    trigger = { has_flag = loop }
    immediate = { trigger_event = t.loop }
    option "ok" { }
}

event "t.tick" {
    scope = character
    type = personal
    category = general
    on = monthly_pulse
    once = scope
    trigger = { has_flag = tick }
    immediate = { trigger_event = { id = t.tick  days = 1 } }
    option "ok" { }
}

event "t.wrong" {
    scope = character
    type = personal
    category = general
    on = monthly_pulse
    once = scope
    trigger = { has_flag = wrong }
    immediate = { trigger_event = t.house }
    option "ok" { }
}

event "t.house" {
    scope = dynasty
    type = family
    category = general
    option "ok" { }
}

event "t.start" {
    scope = realm
    type = imperial
    category = general
    on = on_game_start
    immediate = {
        set_global_flag = game_begun
        set_variable = { name = counter  value = 5 }
        change_variable = { name = counter  add = 2 }
        change_variable = { name = counter  multiply = 3 }
        change_variable = { name = counter  min = 20 }
    }
    option "ok" { }
}

event "t.mark" {
    scope = character
    type = personal
    category = general
    on = monthly_pulse
    once = scope
    trigger = { has_flag = mark }
    immediate = {
        add_modifier = t.short
        notify = { key = marked  a = root  urgency = important }
    }
    option "ok" { }
}

event_modifier "t.short" {
    scope = character
    duration = { days = 10 }
    stack = refresh
}
)";

std::shared_ptr<const EventSet> compiled_set() {
    static const std::shared_ptr<const EventSet> set = [] {
        auto c = pat::event_fixture::compile_event_files({{"events/t.pd", kEvents}}, {}, Catalog::standard());
        INFO(c.all_errors());
        REQUIRE(c.ok());
        return std::make_shared<const EventSet>(std::move(*c.set));
    }();
    return set;
}

std::size_t outcomes(const World& world, std::string_view id, HistoryOutcome outcome) {
    const EventKey key = event_key(id);
    return static_cast<std::size_t>(
        std::ranges::count_if(world.event_state().history, [&](const HistoryEntry& h) {
            return h.event_key == key && h.outcome == outcome;
        }));
}

struct Run {
    Simulation sim;
    std::shared_ptr<Dispatcher> dispatcher;

    explicit Run(World world) : sim(std::move(world)) {
        dispatcher = install_event_systems(sim, compiled_set(), Catalog::standard());
    }
};

}  // namespace

TEST_CASE("event state: every operation has a Russian template; the codes of section 8.3") {
    pat::data::MemorySource source("tooltips");
    const char* file = "loc/ru/events/tooltip_effects_state.pd";
    source.add(file, pat::court_fixture::shipped_text(file));
    const auto loaded = pat::loc::load_localization(source);
    REQUIRE(loaded.ok());
    Catalog c = Catalog::engine();
    register_state_effects(c);
    std::size_t ops = 0;
    for (const CatalogEntry& e : c.entries()) {
        if (e.space != NameSpace::StateOp) {
            continue;
        }
        ++ops;
        CHECK(e.code > kStSaveScopeAs);
        CHECK(e.code <= kStNotify);
        const std::string key = tooltip_key(e);
        CAPTURE(key);
        REQUIRE(loaded.localizer.contains("ru", key));
        std::set<std::string> allowed;
        for (const ArgSpec& arg : e.args) {
            allowed.insert(arg.name);
        }
        for (const std::string& p : loaded.localizer.parameters("ru", key)) {
            CHECK(allowed.contains(p));
        }
    }
    CHECK(ops == 13);  // 2202 ... 2214; save_scope_as (2201) is a keyword of the compiler
}

TEST_CASE("event state: saved scopes travel by name") {
    EventDef from;
    from.saved_names = {"mother", "auditor", "estate"};
    EventDef to;
    to.saved_names = {"estate", "auditor"};
    SavedSlots slots{};
    slots[1] = Scope::make(ScopeKind::Character, 7);
    slots[2] = Scope::make(ScopeKind::Province, 3);
    SavedScopes rows{};
    REQUIRE(pack_saved(from, slots, rows) == 2);
    CHECK(rows[0].name_key == saved_name_key("auditor"));
    const SavedSlots back = unpack_saved(to, rows, 2);
    CHECK(back[0] == Scope::make(ScopeKind::Province, 3));
    CHECK(back[1] == Scope::make(ScopeKind::Character, 7));
    CHECK(back[2].empty());
    CHECK(saved_name_key("auditor") != 0);
}

TEST_CASE("event state: EVT-A3, a flag and a timer start the follow-up; a death removes both") {
    const auto days_after_audit = 40;
    Setup s;
    const CharacterId who = s.a[1];
    REQUIRE(s.world.set_event_flag(ch(who), event_key("pick")));
    const EventKey pending = event_key("audit_pending");

    SUBCASE("alive: the follow-up starts three months later, with the saved scope of the chain") {
        Run r(std::move(s.world));
        r.sim.run_days(days_after_audit);
        const World& w = r.sim.world();
        REQUIRE(outcomes(w, "t.audit", HistoryOutcome::Fired) == 1);
        CHECK_FALSE(w.has_event_flag(ch(who), event_key("pick")));
        CHECK(w.has_event_flag(ch(who), pending));
        REQUIRE(w.event_state().timers.size() == 1);
        const Timer& timer = w.event_state().timers.front();
        CHECK(timer.event_key == event_key("t.audit_result"));
        CHECK(timer.depth == 1);
        CHECK(timer.saved_count == 1);
        CHECK(timer.saved[0] == SavedScope{saved_name_key("auditor"), ch(who)});
        const Chain* chain = w.find_event_chain(ch(who), event_key("audit_story"));
        REQUIRE(chain != nullptr);
        CHECK(chain->step == 1);

        r.sim.run_days(120);
        const World& later = r.sim.world();
        CHECK(outcomes(later, "t.audit_result", HistoryOutcome::Fired) == 1);
        CHECK(later.has_event_flag(kGlobal, event_key("audit_done")));
        CHECK(later.find_event_chain(ch(who), event_key("audit_story"))->step == 2);
        CHECK(later.event_state().timers.empty());
    }
    SUBCASE("dead: the flag, the chain and the timer go with the person; the follow-up never starts") {
        Run r(std::move(s.world));
        r.sim.run_days(days_after_audit);
        REQUIRE(r.sim.world().event_state().timers.size() == 1);
        REQUIRE(r.sim.submit(Command{DynastyId(), cmd::Die{who, CauseOfDeath::Illness}}));
        r.sim.run_days(160);
        const World& w = r.sim.world();
        CHECK_FALSE(w.is_alive(who));
        CHECK(w.find_event_flag(ch(who), pending) == nullptr);
        CHECK(w.find_event_chain(ch(who), event_key("audit_story")) == nullptr);
        CHECK(w.event_state().timers.empty());
        CHECK(w.event_rows_of(ch(who)) == 0);
        CHECK(outcomes(w, "t.audit_result", HistoryOutcome::Fired) == 0);
        CHECK_FALSE(w.has_event_flag(kGlobal, event_key("audit_done")));
    }
}

TEST_CASE("event state: a save in the middle of a timer continues like an uninterrupted run") {
    const auto start = [] {
        Setup s;
        REQUIRE(s.world.set_event_flag(ch(s.a[1]), event_key("pick")));
        REQUIRE(s.world.set_event_flag(ch(s.b[2]), event_key("mark")));
        return std::move(s.world);
    };
    Run whole(start());
    whole.sim.run_days(200);
    const std::uint64_t reference = whole.sim.world().compute_hash().total;
    REQUIRE(outcomes(whole.sim.world(), "t.audit_result", HistoryOutcome::Fired) == 1);

    Run first(start());
    first.sim.run_days(50);
    REQUIRE(first.sim.world().event_state().timers.size() == 1);
    auto loaded = pat::test::load_state(pat::test::state_bytes(first.sim.world()));
    REQUIRE(loaded.has_value());
    Run second(Setup{}.world);
    std::vector<Command> pending(first.sim.pending_internal().begin(), first.sim.pending_internal().end());
    second.sim.restore_state(std::move(loaded).value(), std::move(pending));
    second.sim.run_days(150);
    CHECK(second.sim.world().compute_hash().total == reference);
    CHECK(second.sim.world().event_state() == whole.sim.world().event_state());
}

TEST_CASE("event state: an immediate loop is cut at max_chain_depth, a timer loop at max_timer_chain") {
    Setup s;
    REQUIRE(s.world.set_event_flag(ch(s.a[2]), event_key("loop")));
    REQUIRE(s.world.set_event_flag(ch(s.b[1]), event_key("tick")));
    Run r(std::move(s.world));
    r.sim.run_days(80);
    const World& w = r.sim.world();
    const EventSettings& settings = compiled_set()->settings;
    // the pulse start and max_chain_depth immediate starts, then one cut
    CHECK(outcomes(w, "t.loop", HistoryOutcome::Fired) ==
          static_cast<std::size_t>(settings.max_chain_depth) + 1);
    CHECK(outcomes(w, "t.loop", HistoryOutcome::ChainDepthExceeded) == 1);
    // the pulse start and max_timer_chain timer starts, a day apart, then one cut
    CHECK(outcomes(w, "t.tick", HistoryOutcome::Fired) ==
          static_cast<std::size_t>(settings.max_timer_chain) + 1);
    CHECK(outcomes(w, "t.tick", HistoryOutcome::ChainDepthExceeded) == 1);
    CHECK(w.event_state().timers.empty());
}

TEST_CASE("event state: an immediate start on a scope of the wrong kind is recorded and skipped") {
    Setup s;
    REQUIRE(s.world.set_event_flag(ch(s.a[3]), event_key("wrong")));
    Run r(std::move(s.world));
    r.sim.run_days(30);
    const World& w = r.sim.world();
    CHECK(outcomes(w, "t.wrong", HistoryOutcome::Fired) == 1);
    CHECK(outcomes(w, "t.house", HistoryOutcome::Fired) == 0);
    REQUIRE(outcomes(w, "t.house", HistoryOutcome::Aborted) == 1);
    const auto it = std::ranges::find_if(
        w.event_state().history, [](const HistoryEntry& h) { return h.event_key == event_key("t.house"); });
    CHECK(it->detail == kAbortScopeMismatch);
}

TEST_CASE("event state: on_game_start sets global flags and variables; modifiers expire and are pruned") {
    Setup s;
    const CharacterId marked = s.a[0];
    const CharacterId other = s.b[0];
    REQUIRE(s.world.set_event_flag(ch(marked), event_key("mark")));
    REQUIRE(s.world.set_event_flag(ch(other), event_key("mark")));
    Meta meta = s.world.event_state().meta;
    meta.player_count = 1;
    meta.players[0] = DynastyId(0);
    REQUIRE(s.world.set_event_meta(meta));
    Run r(std::move(s.world));

    r.sim.run_days(1);
    CHECK(r.sim.world().has_event_flag(kGlobal, event_key("game_begun")));
    CHECK(r.sim.world().event_var(kGlobal, event_key("counter")) ==
          Fixed64::from_int(20));  // min((5+2)*3, 20)

    r.sim.run_days(30);  // both have had their monthly slot
    const World& w = r.sim.world();
    CHECK(outcomes(w, "t.mark", HistoryOutcome::Fired) == 2);
    // the journal is the player's: one entry for house 0, none for house 1
    REQUIRE(w.event_state().journal.size() == 1);
    const JournalEntry& note = w.event_state().journal.front();
    CHECK(note.recipient == DynastyId(0));
    CHECK(note.key == event_key("marked"));
    CHECK(note.a == static_cast<std::int32_t>(marked.value()));
    CHECK(note.urgency == 1);

    // ten days after the start the modifier reads as absent; on the next slot of its scope the row goes
    r.sim.run_days(60);
    CHECK_FALSE(r.sim.world().has_event_modifier(ch(marked), event_key("t.short")));
    CHECK(r.sim.world().find_event_modifier(ch(marked), event_key("t.short")) == nullptr);
    CHECK(r.sim.world().find_event_modifier(ch(other), event_key("t.short")) == nullptr);
}
