#include "pat/core/bytes.hpp"
#include "pat/sim/command.hpp"
#include "pat/sim/save.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>

namespace {

using namespace pat::sim;

CharacterId create_person(World& world, DynastyId dynasty, Date birth) {
    CharacterInit init;
    init.dynasty = dynasty;
    init.birth_date = birth;
    return world.create_character(init);
}

Command note(DynastyId dynasty, std::int32_t text, CharacterId subject = {}) {
    return Command{{}, cmd::RecordChronicle{dynasty, ChronicleKind::Note, subject, {}, text}};
}

void record_note(World& world, DynastyId dynasty, std::int32_t text, CharacterId subject = {}) {
    const Command command = note(dynasty, text, subject);
    REQUIRE(validate(world, command).has_value());
    apply(world, command);
}

std::uint64_t chronicle_hash(const World& world) {
    return world.compute_hash().ext[static_cast<std::size_t>(ExtPart::Chronicle)];
}

}  // namespace

TEST_CASE("chronicle: births, head changes, deaths and extinction survive save and load") {
    const Date start = Date::from_ymd(1703, 1, 1);
    World world(start, 7);
    REQUIRE(world.enable_dynasty_mode());
    const DynastyId dynasty = world.create_dynasty({});
    const DynastyId other = world.create_dynasty({});
    const CharacterId adult = create_person(world, dynasty, Date::from_ymd(1670, 1, 1));
    REQUIRE(adult.valid());
    CHECK(world.chronicle(dynasty).empty());  // A scenario founder is not a birth in play.
    CHECK(chronicle_hash(world) == 0);

    const CharacterId child = create_person(world, dynasty, start);
    REQUIRE(child.valid());
    REQUIRE(world.set_dynasty_head(dynasty, adult));
    REQUIRE(world.set_dynasty_head(dynasty, adult));
    (void)world.advance_day();
    REQUIRE(world.die(adult, CauseOfDeath::Illness));
    REQUIRE(world.set_dynasty_head(dynasty, child));
    (void)world.advance_day();
    REQUIRE(world.die(child, CauseOfDeath::Accident));
    CHECK_FALSE(world.die(child, CauseOfDeath::Accident));

    const auto entries = world.chronicle(dynasty);
    REQUIRE(entries.size() == 6);
    CHECK(entries[0] == ChronicleEntry{start, ChronicleKind::Birth, child, {}, 0});
    CHECK(entries[1] == ChronicleEntry{start, ChronicleKind::HeadChanged, {}, adult, 0});
    CHECK(
        entries[2] ==
        ChronicleEntry{start + 1, ChronicleKind::Death, adult, {}, static_cast<int>(CauseOfDeath::Illness)});
    CHECK(entries[3] == ChronicleEntry{start + 1, ChronicleKind::HeadChanged, {}, child, 0});
    CHECK(
        entries[4] ==
        ChronicleEntry{start + 2, ChronicleKind::Death, child, {}, static_cast<int>(CauseOfDeath::Accident)});
    CHECK(entries[5] == ChronicleEntry{start + 2, ChronicleKind::Extinction, child, {}, 0});
    CHECK(world.chronicle(other).empty());

    const auto day = world.query_chronicle(dynasty, start + 1, start + 1);
    CHECK(std::ranges::equal(day, entries.subspan(2, 2)));
    const auto deaths = world.query_chronicle(dynasty, start, start + 2, ChronicleKind::Death);
    REQUIRE(deaths.size() == 2);
    CHECK(deaths[0] == entries[2]);
    CHECK(deaths[1] == entries[4]);
    CHECK(world.query_chronicle(dynasty, start + 2, start).empty());

    CHECK(chronicle_hash(world) != 0);
    REQUIRE(world.check_invariants().empty());
    auto loaded = deserialize_save(serialize_save(world));
    REQUIRE(loaded.has_value());
    CHECK(std::ranges::equal(loaded.value().world.chronicle(dynasty), entries));
    CHECK(loaded.value().world.compute_hash() == world.compute_hash());
}

TEST_CASE("chronicle: capacity retains important records and discards the oldest equal priority") {
    const Date start = Date::from_ymd(1703, 1, 1);
    World world(start, 7);
    REQUIRE(world.enable_dynasty_mode());
    const DynastyId mixed = world.create_dynasty({});
    const DynastyId important = world.create_dynasty({});

    record_note(world, mixed, 0);
    const CharacterId child = create_person(world, mixed, start);
    REQUIRE(child.valid());
    for (std::size_t i = 1; i < kMaxChronicleEntries; ++i) {
        record_note(world, mixed, static_cast<std::int32_t>(i));
    }
    const auto mixed_entries = world.chronicle(mixed);
    REQUIRE(mixed_entries.size() == kMaxChronicleEntries);
    CHECK(mixed_entries.front().kind == ChronicleKind::Birth);
    CHECK(mixed_entries[1].param == 1);
    CHECK(mixed_entries.back().param == static_cast<std::int32_t>(kMaxChronicleEntries - 1));

    const CharacterId adult = create_person(world, important, Date::from_ymd(1670, 1, 1));
    const CharacterId peer = create_person(world, important, Date::from_ymd(1669, 1, 1));
    REQUIRE(adult.valid());
    REQUIRE(peer.valid());
    for (std::size_t i = 0; i < kMaxChronicleEntries; ++i) {
        REQUIRE(world.set_dynasty_head(important, i % 2 == 0 ? adult : peer));
    }
    const auto full_hash = chronicle_hash(world);
    record_note(world, important, 500);
    const auto important_entries = world.chronicle(important);
    REQUIRE(important_entries.size() == kMaxChronicleEntries);
    CHECK(important_entries.front().b == adult);
    CHECK(important_entries.back().b == peer);
    CHECK(chronicle_hash(world) == full_hash);
}

TEST_CASE("chronicle: editorial commands validate participants and preserve the pending payload") {
    const Date start = Date::from_ymd(1703, 1, 1);
    World world(start, 7);
    REQUIRE(world.enable_dynasty_mode());
    const DynastyId dynasty = world.create_dynasty({});
    const CharacterId adult = create_person(world, dynasty, Date::from_ymd(1670, 1, 1));
    REQUIRE(adult.valid());
    REQUIRE(world.die(adult, CauseOfDeath::Illness));

    const Command command = note(dynasty, -31, adult);
    CHECK(static_cast<std::uint8_t>(type_of(command)) == 18);
    REQUIRE(validate(world, command).has_value());  // Archived references are stable.
    CHECK_FALSE(validate(world, Command{dynasty, command.payload}).has_value());
    CHECK_FALSE(validate(world, note(dynasty, 1, CharacterId(100))).has_value());
    CHECK_FALSE(
        validate(world, Command{{}, cmd::RecordChronicle{dynasty, ChronicleKind::Death, adult, {}, 0}})
            .has_value());

    auto saved = deserialize_save(serialize_save(world, std::span(&command, 1)));
    REQUIRE(saved.has_value());
    REQUIRE(saved.value().pending_internal.size() == 1);
    const Command& pending = saved.value().pending_internal.front();
    CHECK(pending == command);
    REQUIRE(validate(saved.value().world, pending).has_value());
    apply(saved.value().world, pending);
    REQUIRE(!saved.value().world.chronicle(dynasty).empty());
    CHECK(saved.value().world.chronicle(dynasty).back() ==
          ChronicleEntry{start, ChronicleKind::Note, adult, {}, -31});
    CHECK(chronicle_hash(saved.value().world) != chronicle_hash(world));
}

TEST_CASE("chronicle: a schema-5 world with no chronicle section loads with empty history") {
    const Date start = Date::from_ymd(1703, 1, 1);
    World world(start, 7);
    REQUIRE(world.enable_dynasty_mode());
    const DynastyId dynasty = world.create_dynasty({});
    REQUIRE(create_person(world, dynasty, Date::from_ymd(1670, 1, 1)).valid());

    pat::core::ByteWriter out;
    world.save_state(out);
    auto payload = out.take();
    REQUIRE(payload.size() >= 4);
    payload[0] = 5;  // Without section 5, this is also the schema-5 layout.
    pat::core::ByteReader in(payload);
    auto loaded = World::load_state(in);
    REQUIRE(loaded.has_value());
    CHECK(in.at_end());
    CHECK(loaded.value().chronicle(dynasty).empty());
    CHECK(loaded.value().compute_hash() == world.compute_hash());
}
