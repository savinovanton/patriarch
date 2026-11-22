// E11-T03: save, migration and hash of the event tables (docs/dev/event-engine.md section 9): sections
// 48..57, hash parts 24..29, schema 13, the schema-12 fixture, damaged files and events::reconcile().

#include "pat/core/bytes.hpp"
#include "pat/core/fixed.hpp"
#include "pat/sim/events/event_set.hpp"
#include "pat/sim/events/reconcile.hpp"
#include "pat/sim/events/state.hpp"
#include "pat/sim/save.hpp"
#include "pat/sim/simulation.hpp"
#include "pat/sim/world.hpp"

#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <limits>
#include <span>
#include <utility>
#include <vector>

#include "state_sections_fixture.hpp"
#include "test_bytes.hpp"
#include "world_fixture.hpp"

using namespace pat::sim;
using namespace pat::sim::events;
using pat::core::Fixed32;
using pat::core::Fixed64;
using pat::core::StringId;
using pat::test::edit_sections;
using pat::test::expect_rejected;
using pat::test::load_state;
using pat::test::section;
using pat::test::Sections;
using pat::test::sections_of;
using pat::test::state_bytes;

namespace {

Date today() {
    return Date::from_ymd(1703, 5, 20);
}

constexpr EventKey kA = 0x1111;
constexpr EventKey kB = 0x2222;
constexpr EventKey kC = 0x3333;
constexpr EventKey kGroup = kCooldownGroupBit | 0x77;

struct Fixture {
    World world;
    std::array<CharacterId, 3> people;
};

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

World make_plain_world() {
    World world(today(), 323);
    REQUIRE(world.enable_dynasty_mode());
    for (std::uint32_t i = 0; i < 3; ++i) {
        REQUIRE(world.create_dynasty(StringId(100 + i)) == DynastyId(i));
        REQUIRE(world.create_province(StringId(200 + i), DynastyId(i), Fixed32::from_int(100)) ==
                ProvinceId(i));
    }
    return world;
}

CharacterId person(World& world, std::uint32_t house) {
    CharacterInit init;
    init.dynasty = DynastyId(house);
    init.sex = Sex::Male;
    init.birth_date = Date::from_ymd(1670, 1, 1);
    const CharacterId id = world.create_character(init);
    REQUIRE(id.valid());
    return id;
}

/// Every table non-empty: a timer with two saved scopes, a pending, a deferred and a chosen-but-unapplied
/// window, a once-cooldown and a group cooldown, a chain, a modifier, both rings and the meta record.
Fixture make_event_world() {
    Fixture f{make_plain_world(), {}};
    World& w = f.world;
    for (std::uint32_t i = 0; i < f.people.size(); ++i) {
        f.people[i] = person(w, i);
    }
    const CharacterId a = f.people[0];
    const CharacterId b = f.people[1];

    Meta meta;
    meta.started = 1;
    meta.events_hash = 0xFEEDFACE12345678ULL;
    meta.frequency_permille = 2000;
    meta.anchors_enabled = 0;
    meta.player_count = 2;
    meta.players[0] = DynastyId(0);
    meta.players[1] = DynastyId(2);
    meta.modal_month = 1703 * 12 + 4;
    meta.modal_count = 3;
    REQUIRE(w.set_event_meta(meta));

    REQUIRE(w.set_event_flag(character(a), kA));
    REQUIRE(w.set_event_flag(dynasty(1), kB, today() + 40));
    REQUIRE(w.set_event_flag(realm(), kC));
    REQUIRE(w.set_event_var(character(b), kA, Fixed64::from_int(-7)));
    REQUIRE(w.set_event_var(province(2), kB, Fixed64::from_raw(123456789)));

    Timer timer;
    timer.due = today() + 20;
    timer.event_key = kB;
    timer.scope = dynasty(0);
    timer.depth = 3;
    timer.saved_count = 2;
    timer.saved[0] = {7, character(b)};
    timer.saved[1] = {9, province(1)};
    REQUIRE(w.add_event_timer(timer).has_value());
    timer.due = today() + 5;
    timer.event_key = kC;
    timer.scope = character(a);
    timer.depth = 0;
    timer.saved_count = 0;
    timer.saved = {};
    REQUIRE(w.add_event_timer(timer).has_value());

    REQUIRE(w.set_event_cooldown({character(a), kA, today() + 100}));
    REQUIRE(w.set_event_cooldown({dynasty(0), kB, kForever}));
    REQUIRE(w.set_event_cooldown({dynasty(0), kGroup, today() + 10}));
    REQUIRE(w.set_event_chain({dynasty(2), kC, 2, today() - 30, today() - 2}));
    REQUIRE(w.set_event_modifier({province(0), kA, today() - 5, today() + 60}));
    REQUIRE(w.set_event_modifier({realm(), kB, today(), kNoDate}));

    Instance pending;
    pending.event_key = kA;
    pending.recipient = DynastyId(0);
    pending.scope = character(a);
    pending.fired_on = today();
    pending.avail_mask = 3;
    pending.urgency = 2;
    pending.priority = 60;
    pending.flags = kInstanceAutopause | kInstanceCounted;
    pending.saved_count = 1;
    pending.saved[0] = {5, character(b)};
    REQUIRE(w.enqueue_event_instance(pending).has_value());
    Instance deferred = pending;
    deferred.event_key = kB;
    deferred.state = InstanceState::Deferred;
    deferred.defer_until = today() + 14;
    deferred.defer_count = 1;
    deferred.saved_count = 0;
    deferred.saved = {};
    REQUIRE(w.enqueue_event_instance(deferred).has_value());
    Instance chosen = pending;
    chosen.event_key = kC;
    chosen.recipient = DynastyId(2);
    chosen.state = InstanceState::Chosen;
    chosen.option = 1;
    chosen.depth = 4;
    REQUIRE(w.enqueue_event_instance(chosen).has_value());

    HistoryEntry history;
    history.date = today() - 3;
    history.event_key = kA;
    history.scope = character(a);
    history.option = 1;
    history.detail = 9;
    REQUIRE(w.append_event_history(history).has_value());
    history.outcome = HistoryOutcome::Cancelled;
    history.scope = Scope{};
    history.option = kNoOption;
    REQUIRE(w.append_event_history(history).has_value());
    JournalEntry journal;
    journal.date = today() - 1;
    journal.recipient = DynastyId(1);
    journal.kind = JournalKind::Rumor;
    journal.urgency = 1;
    journal.key = kB;
    journal.a = -5;
    journal.b = 100000;
    journal.param = 7;
    journal.flags = 1;
    REQUIRE(w.append_event_journal(journal).has_value());
    pat::test::require_valid(w);
    return f;
}

constexpr std::array<std::uint16_t, 10> kEventSections{48, 49, 50, 51, 52, 53, 54, 55, 56, 57};

bool has_section(const std::vector<std::uint8_t>& bytes, std::uint16_t id) {
    for (const auto& row : sections_of(bytes).rows) {
        if (row.id == id) {
            return true;
        }
    }
    return false;
}

}  // namespace

TEST_CASE("event save: a world without events writes nothing and hashes as before") {
    const World world = make_plain_world();
    const auto bytes = state_bytes(world);
    for (const auto id : kEventSections) {
        CHECK_FALSE(has_section(bytes, id));
    }
    const WorldHash hash = world.compute_hash();
    for (std::size_t i = 24; i <= 29; ++i) {
        CHECK(hash.ext[i] == 0);
    }
    // The file of the previous schema is the same bytes with the older schema word.
    auto older = bytes;
    pat::test::store_le<std::uint32_t>(older, 0, 12);
    auto loaded = load_state(older);
    REQUIRE(loaded.has_value());
    CHECK(loaded.value().compute_hash() == hash);
    CHECK(loaded.value().event_state() == State{});
    CHECK(state_bytes(loaded.value()) == bytes);  // saved again: schema 13, nothing else changes
    CHECK(World::kStateSchema == 13);
}

TEST_CASE("event save: every table round-trips with its hash and derived counters") {
    const Fixture f = make_event_world();
    const auto bytes = state_bytes(f.world);
    for (const auto id : kEventSections) {
        CAPTURE(id);
        CHECK(has_section(bytes, id));
    }
    auto loaded = load_state(bytes);
    REQUIRE(loaded.has_value());
    const World& restored = loaded.value();
    CHECK(restored.event_state() == f.world.event_state());
    CHECK(restored.compute_hash() == f.world.compute_hash());
    CHECK(state_bytes(restored) == bytes);
    CHECK(restored.event_rows_of(character(f.people[0])) == f.world.event_rows_of(character(f.people[0])));
    CHECK(restored.event_rows_of(dynasty(0)) == f.world.event_rows_of(dynasty(0)));
    pat::test::require_valid(restored);

    const WorldHash hash = restored.compute_hash();
    for (std::size_t i = 24; i <= 29; ++i) {
        CHECK(hash.ext[i] != 0);
    }
    CHECK(ext_part_name(24) == "event_flags_vars");
    CHECK(ext_part_name(29) == "event_meta");

    // The save of a saved world is stable, through the container as well.
    const auto file = serialize_save(f.world);
    auto from_file = deserialize_save(file);
    REQUIRE(from_file.has_value());
    CHECK(from_file.value().world.compute_hash() == f.world.compute_hash());
    CHECK(from_file.value().summary.events_hash == 0xFEEDFACE12345678ULL);
    CHECK(serialize_save(from_file.value().world) == file);
}

TEST_CASE("event hash: each table moves only its own part and the total") {
    Fixture f = make_event_world();
    const WorldHash base = f.world.compute_hash();
    const CharacterId a = f.people[0];

    const auto changed_parts = [&](const World& world) {
        std::vector<std::size_t> parts;
        const WorldHash now = world.compute_hash();
        for (std::size_t i = 0; i < kExtPartCount; ++i) {
            if (now.ext[i] != base.ext[i]) {
                parts.push_back(i);
            }
        }
        CHECK(now.total != base.total);
        return parts;
    };
    using Parts = std::vector<std::size_t>;
    {
        World world = f.world;
        REQUIRE(world.set_event_flag(character(a), 0x99));
        CHECK(changed_parts(world) == Parts{24});
    }
    {
        World world = f.world;
        REQUIRE(world.set_event_var(realm(), 0x99, Fixed64::from_int(1)));
        CHECK(changed_parts(world) == Parts{24});
    }
    {
        World world = f.world;
        REQUIRE(world.cancel_event_timer(0));
        CHECK(changed_parts(world) == Parts{25});
    }
    {
        World world = f.world;
        REQUIRE(world.clear_event_cooldown(dynasty(0), kB));
        CHECK(changed_parts(world) == Parts{25});
    }
    {
        World world = f.world;
        REQUIRE(world.clear_event_chain(dynasty(2), kC));
        CHECK(changed_parts(world) == Parts{26});
    }
    {
        World world = f.world;
        REQUIRE(world.clear_event_modifier(realm(), kB));
        CHECK(changed_parts(world) == Parts{26});
    }
    {
        World world = f.world;
        Instance row = *world.find_event_instance(0);
        row.urgency = 3;
        REQUIRE(world.update_event_instance(row));
        CHECK(changed_parts(world) == Parts{27});
    }
    {
        World world = f.world;
        JournalEntry entry;
        entry.date = today();
        entry.recipient = DynastyId(0);
        entry.key = kC;
        REQUIRE(world.append_event_journal(entry).has_value());
        // The journal counter lives in the meta record: the part of the rings and the meta change.
        CHECK(changed_parts(world) == Parts{28, 29});
    }
    {
        World world = f.world;
        Meta meta = world.event_state().meta;
        meta.modal_count = 4;
        REQUIRE(world.set_event_meta(meta));
        CHECK(changed_parts(world) == Parts{29});
    }
    {
        // `events_hash` says which set made the world; it is not state.
        World world = f.world;
        Meta meta = world.event_state().meta;
        meta.events_hash ^= 1;
        REQUIRE(world.set_event_meta(meta));
        CHECK(world.compute_hash() == base);
        CHECK(state_bytes(world) != state_bytes(f.world));  // but it is saved
    }
    {
        // A lone `events_hash` is not state either: no part, no change of the total.
        World plain = make_plain_world();
        const WorldHash before = plain.compute_hash();
        Meta meta;
        meta.events_hash = 5;
        REQUIRE(plain.set_event_meta(meta));
        CHECK(plain.compute_hash() == before);
        auto loaded = load_state(state_bytes(plain));
        REQUIRE(loaded.has_value());
        CHECK(loaded.value().event_state().meta.events_hash == 5);
    }
    CHECK(static_cast<std::size_t>(ExtPart::EventMeta) == 29);
}

TEST_CASE("event save: the schema-12 fixture still loads with the same hash and empty events") {
    const auto path = std::filesystem::path(PATRIARCH_TEST_DATA_DIR) / "saves" / "dynasty_v12_e8.psv";
    const auto file = read_file(path);
    REQUIRE(file.has_value());
    auto loaded = deserialize_save(file.value());
    REQUIRE(loaded.has_value());
    CHECK(loaded.value().summary.state_schema == 12);
    CHECK(loaded.value().world.compute_hash().total == 11932784967711559582ULL);  // E8-T03, schema 12
    CHECK(loaded.value().world.event_state() == State{});
    for (std::size_t i = 24; i <= 29; ++i) {
        CHECK(loaded.value().world.compute_hash().ext[i] == 0);
    }
    pat::test::require_valid(loaded.value().world);
    // Saved again it is a schema-13 file with the same state.
    const auto again = deserialize_save(serialize_save(loaded.value().world));
    REQUIRE(again.has_value());
    CHECK(again.value().summary.state_schema == 13);
    CHECK(again.value().world.compute_hash() == loaded.value().world.compute_hash());
}

TEST_CASE("event save: a file of the previous schema cannot hold event sections") {
    const Fixture f = make_event_world();
    auto bytes = state_bytes(f.world);
    pat::test::store_le<std::uint32_t>(bytes, 0, 12);
    expect_rejected(bytes);
    pat::test::store_le<std::uint32_t>(bytes, 0, World::kStateSchema + 1);
    expect_rejected(bytes);
}

TEST_CASE("event sections: versions, lengths, counts and row order are checked before use") {
    const auto bytes = state_bytes(make_event_world().world);
    for (std::uint16_t id = 48; id <= 57; ++id) {
        CAPTURE(id);
        expect_rejected(edit_sections(bytes, [&](Sections& s) { section(s, id).version = 2; }));
        for (int cut = 0; cut < 3; ++cut) {
            expect_rejected(edit_sections(bytes, [&](Sections& s) {
                auto& body = section(s, id).body;
                body.resize(cut == 0 ? 0 : cut == 1 ? body.size() / 2 : body.size() - 1);
            }));
        }
        expect_rejected(edit_sections(bytes, [&](Sections& s) { section(s, id).body.push_back(0); }));
        if (id == 48) {
            continue;
        }
        expect_rejected(edit_sections(bytes, [&](Sections& s) {
            pat::test::store_le<std::uint32_t>(
                section(s, id).body, 0, std::numeric_limits<std::uint32_t>::max());
        }));
        // A row repeated at the front breaks the strict order.
        expect_rejected(edit_sections(bytes, [&](Sections& s) {
            auto& body = section(s, id).body;
            const auto n = pat::test::load_le<std::uint32_t>(body, 0);
            REQUIRE(n >= 1);
            // The row sizes are fixed except for timers and the queue, whose first row is the shortest
            // one at the end of the list (no saved scopes): a duplicate of the last row is enough.
            const std::size_t fixed[] = {0, 0, 17, 21, 0, 17, 22, 21, 0, 25, 35};
            std::size_t row_size = fixed[id - 47];
            if (row_size == 0) {  // timers, queue: duplicate the whole body after the count
                std::vector<std::uint8_t> rows(body.begin() + 4, body.end());
                body.insert(body.end(), rows.begin(), rows.end());
                pat::test::store_le<std::uint32_t>(body, 0, n * 2);
                return;
            }
            const std::vector<std::uint8_t> first(body.begin() + 4, body.begin() + 4 + row_size);
            body.insert(body.begin() + 4, first.begin(), first.end());
            pat::test::store_le<std::uint32_t>(body, 0, n + 1);
        }));
    }
    expect_rejected(edit_sections(bytes, [](Sections& s) { s.rows.push_back(section(s, 49)); }));
}

TEST_CASE("event sections: counters, references and kinds that a file may not carry are refused") {
    const Fixture f = make_event_world();
    const auto bytes = state_bytes(f.world);
    const std::size_t kScopeKind = 0;  // first byte of the row of flags, variables, modifiers
    const std::array<std::function<void(Sections&)>, 14> damage{
        // unknown and reserved scope kinds, a realm that is not 0, a stale character
        [&](Sections& s) { section(s, 49).body[4 + kScopeKind] = 7; },
        [&](Sections& s) { section(s, 50).body[4 + kScopeKind] = 0; },
        [&](Sections& s) {
            section(s, 49).body[4 + 0] = 4; /* realm id must be 0 */
        },
        [&](Sections& s) { pat::test::store_le<std::uint32_t>(section(s, 50).body, 4 + 1, 9999); },
        [&](Sections& s) { pat::test::store_le<std::uint32_t>(section(s, 49).body, 4 + 1, 9999); },
        // zero key
        [&](Sections& s) { pat::test::store_le<std::uint64_t>(section(s, 49).body, 4 + 5, 0); },
        // counters that do not cover the rows
        [&](Sections& s) { pat::test::store_le<std::uint32_t>(section(s, 48).body, 1 + 8 + 4, 1); },
        [&](Sections& s) { pat::test::store_le<std::uint32_t>(section(s, 48).body, 1 + 8, 1); },
        [&](Sections& s) { pat::test::store_le<std::uint32_t>(section(s, 48).body, 1 + 8 + 8, 0); },
        // settings and players
        [&](Sections& s) { section(s, 48).body[0] = 2; },
        [&](Sections& s) { pat::test::store_le<std::uint16_t>(section(s, 48).body, 1 + 8 + 20, 0); },
        [&](Sections& s) { section(s, 48).body[1 + 8 + 20 + 3] = 5; },
        [&](Sections& s) { pat::test::store_le<std::uint32_t>(section(s, 48).body, 1 + 8 + 20 + 4, 77); },
        // the cursor of the incident log beyond the (absent) economy
        [&](Sections& s) { pat::test::store_le<std::uint32_t>(section(s, 48).body, 1 + 8 + 16, 9); },
    };
    for (std::size_t i = 0; i < damage.size(); ++i) {
        CAPTURE(i);
        expect_rejected(edit_sections(bytes, damage[i]));
    }
    // too many saved scopes in a timer
    expect_rejected(edit_sections(bytes, [](Sections& s) {
        auto& body = section(s, 51).body;
        body[4 + 4 + 4 + 8 + 1 + 4 + 1] = 9;
    }));
}

TEST_CASE("event sections: no prefix and no flipped byte of the event sections crashes the loader") {
    const Fixture f = make_event_world();
    const auto bytes = state_bytes(f.world);
    for (std::size_t length = 0; length < bytes.size(); ++length) {
        const auto result = load_state(std::span<const std::uint8_t>(bytes).first(length));
        CHECK_FALSE(result.has_value());
    }
    const Sections sections = sections_of(bytes);
    std::size_t checked = 0;
    for (const auto& row : sections.rows) {
        if (row.id < 48 || row.id > 57) {
            continue;
        }
        for (std::size_t at = 0; at < row.body.size(); ++at) {
            for (const std::uint8_t mask : {std::uint8_t{0x01}, std::uint8_t{0x80}, std::uint8_t{0xFF}}) {
                const auto damaged =
                    edit_sections(bytes, [&](Sections& s) { section(s, row.id).body[at] ^= mask; });
                auto result = load_state(damaged);
                if (result.has_value()) {
                    CHECK(result.value().check_invariants().empty());  // accepted only if still a valid world
                }
                ++checked;
            }
        }
    }
    CHECK(checked > 500);
}

TEST_CASE("event save: a load in the middle of a timer, a window and a chain continues like the original") {
    Fixture f = make_event_world();
    const std::vector<Command> none;
    Simulation continuous(f.world);
    continuous.restore_state(f.world, none);
    auto loaded = deserialize_save(serialize_save(f.world, none));
    REQUIRE(loaded.has_value());
    Simulation resumed(std::move(loaded.value().world));
    resumed.restore_state(resumed.world(), loaded.value().pending_internal);
    for (int day = 0; day < 40; ++day) {
        (void)continuous.tick();
        (void)resumed.tick();
        REQUIRE(continuous.world().compute_hash() == resumed.world().compute_hash());
    }
    CHECK(state_bytes(continuous.world()) == state_bytes(resumed.world()));
    CHECK(resumed.world().event_state() == continuous.world().event_state());
    pat::test::require_valid(resumed.world());
}

TEST_CASE("event save: the summary key 21 carries the hash of the event set and is absent without one") {
    const World plain = make_plain_world();
    CHECK(summary_from_world(plain).events_hash == 0);
    const auto plain_file = deserialize_save(serialize_save(plain));
    REQUIRE(plain_file.has_value());
    CHECK(plain_file.value().summary.events_hash == 0);

    const Fixture f = make_event_world();
    CHECK(summary_from_world(f.world).events_hash == 0xFEEDFACE12345678ULL);
    SaveSummary summary = summary_from_world(f.world);
    summary.preview.assign(100, 7);  // the preview goes before key 21 in the file
    const auto file = serialize_save(f.world, {}, summary);
    const auto info = read_save_summary(file, file.size());
    REQUIRE(info.has_value());
    REQUIRE(info.value().summary.has_value());
    CHECK(info.value().summary->events_hash == 0xFEEDFACE12345678ULL);
    CHECK(info.value().summary->preview.size() == 100);
}

namespace {

EventDef def(EventKey key, std::size_t options) {
    EventDef event;
    event.key = key;
    event.options.resize(options);
    return event;
}

}  // namespace

TEST_CASE("event reconcile: rows of events that are gone are dropped and recorded, the rest stays") {
    Fixture f = make_event_world();
    World& world = f.world;

    EventSet complete;  // kA with 2 options, kB with 2, kC with 2
    complete.events = {def(kA, 2), def(kB, 2), def(kC, 2)};
    {
        World same = world;
        const ReconcileResult none = reconcile(same, complete);
        CHECK(none.total() == 0);
        CHECK(same.event_state() == world.event_state());
    }

    EventSet reduced;  // kA is gone, kB lost an option (the deferred window offered 2), kC unchanged
    reduced.events = {def(kB, 1), def(kC, 2)};
    const ReconcileResult result = reconcile(world, reduced);
    CHECK(result.timers == 0);
    CHECK(result.cooldowns == 1);  // the cooldown of kA on the character; the group cooldown stays
    CHECK(result.instances == 2);  // kA pending, kB with an option that no longer exists
    CHECK(world.event_state().queue.size() == 1);
    CHECK(world.event_state().queue[0].event_key == kC);
    CHECK(world.event_state().history.size() == 4);
    CHECK(world.event_state().history[2].outcome == HistoryOutcome::Cancelled);
    CHECK(world.find_event_cooldown(dynasty(0), kGroup) != nullptr);
    pat::test::require_valid(world);

    EventSet only_c;
    only_c.events = {def(kC, 2)};
    const ReconcileResult second = reconcile(world, only_c);
    CHECK(second.timers == 1);  // the timer of kB
    CHECK(world.find_event_timer(1) != nullptr);
    CHECK(world.find_event_timer(0) == nullptr);
    CHECK(world.event_state().history.back().outcome == HistoryOutcome::Disabled);
    pat::test::require_valid(world);
    // reconciled state saves and loads like any other
    auto loaded = load_state(state_bytes(world));
    REQUIRE(loaded.has_value());
    CHECK(loaded.value().compute_hash() == world.compute_hash());
}
