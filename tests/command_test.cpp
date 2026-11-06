#include "pat/sim/command.hpp"
#include "pat/sim/command_log.hpp"

#include <doctest/doctest.h>

#include <vector>

using namespace pat::sim;
using pat::core::Fixed32;
using pat::core::StringId;

namespace {

struct Fixture {
    World world{Date::from_ymd(1703, 1, 1), 7};
    DynastyId golitsyn = world.create_dynasty(StringId(0));
    DynastyId menshikov = world.create_dynasty(StringId(1));
    ProvinceId tula = world.create_province(StringId(10), golitsyn, Fixture::pop());
    ProvinceId kursk = world.create_province(StringId(11), menshikov, Fixture::pop());
    CharacterId prince = make(golitsyn, Sex::Male);
    CharacterId princess = make(golitsyn, Sex::Female);
    CharacterId rival = make(menshikov, Sex::Male);

    static Fixed32 pop() { return Fixed32::from_int(10); }

    CharacterId make(DynastyId d, Sex sex) {
        CharacterInit init;
        init.dynasty = d;
        init.birth_date = Date::from_ymd(1670, 1, 1);
        init.sex = sex;
        return world.create_character(init);
    }
};

Command by(DynastyId actor, CommandPayload payload) {
    return Command{actor, std::move(payload)};
}

Rejection verdict(const World& w, const Command& c) {
    const auto r = validate(w, c);
    return r.has_value() ? Rejection::None : r.error().reason;
}

}  // namespace

TEST_CASE("command: type tags are stable") {
    CHECK(static_cast<int>(CommandType::SetProvinceOwner) == 1);
    CHECK(static_cast<int>(CommandType::CreateCharacter) == 2);
    CHECK(static_cast<int>(CommandType::KillCharacter) == 3);
    CHECK(static_cast<int>(CommandType::SetDynastyHead) == 4);
    CHECK(static_cast<int>(CommandType::AdjustHonor) == 5);
    CHECK(type_of(by(DynastyId(), cmd::AdjustHonor{})) == CommandType::AdjustHonor);
    CHECK(type_of(by(DynastyId(), cmd::KillCharacter{})) == CommandType::KillCharacter);
    CHECK(to_string(CommandType::SetDynastyHead) == "SetDynastyHead");
    CHECK(to_string(Rejection::NotAuthorized) == "not authorized");
}

TEST_CASE_FIXTURE(Fixture, "command: SetProvinceOwner validation") {
    // The owner may transfer its own province.
    CHECK(verdict(world, by(golitsyn, cmd::SetProvinceOwner{tula, menshikov})) == Rejection::None);
    // A foreign dynasty may not.
    CHECK(verdict(world, by(menshikov, cmd::SetProvinceOwner{tula, menshikov})) == Rejection::NotAuthorized);
    // The system may.
    CHECK(verdict(world, by(DynastyId(), cmd::SetProvinceOwner{tula, menshikov})) == Rejection::None);
    // Unknown entities.
    CHECK(verdict(world, by(golitsyn, cmd::SetProvinceOwner{ProvinceId(99), menshikov})) ==
          Rejection::UnknownEntity);
    CHECK(verdict(world, by(golitsyn, cmd::SetProvinceOwner{tula, DynastyId(99)})) ==
          Rejection::UnknownEntity);
    CHECK(verdict(world, by(DynastyId(99), cmd::SetProvinceOwner{tula, menshikov})) ==
          Rejection::UnknownEntity);
    // No-op transfers are rejected.
    CHECK(verdict(world, by(golitsyn, cmd::SetProvinceOwner{tula, golitsyn})) == Rejection::InvalidState);
    // Leaving a province unowned is for the system only.
    CHECK(verdict(world, by(golitsyn, cmd::SetProvinceOwner{tula, DynastyId()})) == Rejection::NotAuthorized);
    CHECK(verdict(world, by(DynastyId(), cmd::SetProvinceOwner{tula, DynastyId()})) == Rejection::None);
}

TEST_CASE_FIXTURE(Fixture, "command: CreateCharacter validation") {
    cmd::CreateCharacter child;
    child.dynasty = golitsyn;
    child.birth_date = Date::from_ymd(1703, 1, 1);
    child.father = prince;
    child.mother = princess;
    CHECK(verdict(world, by(golitsyn, child)) == Rejection::None);

    auto bad = child;
    bad.birth_date = Date::from_ymd(1703, 1, 2);  // tomorrow
    CHECK(verdict(world, by(golitsyn, bad)) == Rejection::InvalidArgument);

    bad = child;
    bad.father = princess;  // a woman cannot be the father
    CHECK(verdict(world, by(golitsyn, bad)) == Rejection::InvalidArgument);

    bad = child;
    bad.mother = prince;
    CHECK(verdict(world, by(golitsyn, bad)) == Rejection::InvalidArgument);

    bad = child;
    bad.mother = CharacterId(99);
    CHECK(verdict(world, by(golitsyn, bad)) == Rejection::UnknownEntity);

    bad = child;
    bad.dynasty = DynastyId(99);
    CHECK(verdict(world, by(golitsyn, bad)) == Rejection::UnknownEntity);

    CHECK(verdict(world, by(menshikov, child)) == Rejection::NotAuthorized);  // not their dynasty
    CHECK(verdict(world, by(DynastyId(), child)) == Rejection::None);
}

TEST_CASE_FIXTURE(Fixture, "command: KillCharacter and SetDynastyHead validation") {
    CHECK(verdict(world, by(golitsyn, cmd::KillCharacter{prince})) == Rejection::None);
    CHECK(verdict(world, by(menshikov, cmd::KillCharacter{prince})) == Rejection::NotAuthorized);
    CHECK(verdict(world, by(DynastyId(), cmd::KillCharacter{prince})) == Rejection::None);
    CHECK(verdict(world, by(golitsyn, cmd::KillCharacter{CharacterId(99)})) == Rejection::UnknownEntity);

    CHECK(verdict(world, by(golitsyn, cmd::SetDynastyHead{golitsyn, prince})) == Rejection::None);
    CHECK(verdict(world, by(golitsyn, cmd::SetDynastyHead{golitsyn, rival})) == Rejection::InvalidState);
    CHECK(verdict(world, by(menshikov, cmd::SetDynastyHead{golitsyn, prince})) == Rejection::NotAuthorized);
    CHECK(verdict(world, by(golitsyn, cmd::SetDynastyHead{golitsyn, CharacterId(99)})) ==
          Rejection::UnknownEntity);
    CHECK(verdict(world, by(golitsyn, cmd::SetDynastyHead{golitsyn, CharacterId()})) == Rejection::None);
}

TEST_CASE_FIXTURE(Fixture, "command: AdjustHonor is for the system only") {
    CHECK(verdict(world, by(DynastyId(), cmd::AdjustHonor{golitsyn, 50})) == Rejection::None);
    CHECK(verdict(world, by(golitsyn, cmd::AdjustHonor{golitsyn, 50})) == Rejection::NotAuthorized);
    CHECK(verdict(world, by(DynastyId(), cmd::AdjustHonor{DynastyId(99), 50})) == Rejection::UnknownEntity);
}

TEST_CASE_FIXTURE(Fixture, "command: validation never changes the world") {
    const WorldHash before = world.compute_hash();
    (void)validate(world, by(golitsyn, cmd::SetProvinceOwner{tula, menshikov}));
    (void)validate(world, by(golitsyn, cmd::KillCharacter{prince}));
    (void)validate(world, by(DynastyId(), cmd::AdjustHonor{golitsyn, 10}));
    CHECK(world.compute_hash() == before);
}

TEST_CASE_FIXTURE(Fixture, "command: apply changes the state through the world API") {
    apply(world, by(golitsyn, cmd::SetProvinceOwner{tula, menshikov}));
    CHECK(world.provinces().owner[tula.index()] == menshikov);
    CHECK(world.provinces_of(menshikov).size() == 2);

    apply(world, by(golitsyn, cmd::SetDynastyHead{golitsyn, prince}));
    CHECK(world.dynasties().head[golitsyn.index()] == prince);

    const std::size_t members = world.members_of(golitsyn).size();
    cmd::CreateCharacter child;
    child.dynasty = golitsyn;
    child.birth_date = Date::from_ymd(1702, 12, 31);
    child.father = prince;
    child.mother = princess;
    apply(world, by(golitsyn, child));
    CHECK(world.members_of(golitsyn).size() == members + 1);

    apply(world, by(golitsyn, cmd::KillCharacter{prince}));
    CHECK_FALSE(world.is_alive(prince));
    CHECK_FALSE(world.dynasties().head[golitsyn.index()].valid());

    apply(world, by(DynastyId(), cmd::AdjustHonor{golitsyn, 120}));
    CHECK(world.dynasties().honor[golitsyn.index()] == 120);
    CHECK(world.check_invariants().empty());
}

TEST_CASE_FIXTURE(Fixture, "command: process_commands applies valid ones, rejects the rest and logs both") {
    CommandLog log;
    const std::vector<Command> batch = {
        by(golitsyn, cmd::SetProvinceOwner{tula, menshikov}),  // ok
        by(golitsyn, cmd::SetProvinceOwner{tula, golitsyn}),   // rejected: no longer the owner
        by(DynastyId(), cmd::AdjustHonor{menshikov, 10}),      // ok
        by(menshikov, cmd::KillCharacter{prince}),             // rejected: foreign character
    };

    const ApplyStats stats = process_commands(world, batch, CommandSource::External, log);
    CHECK(stats.applied == 2);
    CHECK(stats.rejected == 2);
    CHECK(world.provinces().owner[tula.index()] == menshikov);
    CHECK(world.is_alive(prince));  // the rejected kill did nothing

    REQUIRE(log.size() == 4);
    CHECK(log.entries()[0].accepted());
    CHECK(log.entries()[1].outcome == Rejection::NotAuthorized);
    CHECK(log.entries()[2].accepted());
    CHECK(log.entries()[3].outcome == Rejection::NotAuthorized);
    CHECK(log.entries()[0].date == Date::from_ymd(1703, 1, 1));
    CHECK(log.entries()[0].source == CommandSource::External);

    // Only accepted external commands feed a replay.
    CHECK(log.external_commands_on(Date::from_ymd(1703, 1, 1)).size() == 2);
    CHECK(log.external_commands_on(Date::from_ymd(1703, 1, 2)).empty());
}

TEST_CASE_FIXTURE(Fixture, "command: the same batch gives the same result (deterministic application)") {
    const std::vector<Command> batch = {
        by(golitsyn, cmd::SetProvinceOwner{tula, menshikov}),
        by(DynastyId(), cmd::AdjustHonor{menshikov, 33}),
        by(golitsyn, cmd::KillCharacter{princess}),
    };
    World copy = world;
    CommandLog log_a;
    CommandLog log_b;
    (void)process_commands(world, batch, CommandSource::External, log_a);
    (void)process_commands(copy, batch, CommandSource::External, log_b);
    CHECK(world.compute_hash() == copy.compute_hash());
    CHECK(log_a == log_b);
}
