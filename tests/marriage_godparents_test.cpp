#include "pat/sim/command_log.hpp"
#include "pat/sim/family_systems.hpp"
#include "pat/sim/kinship.hpp"
#include "pat/sim/marriage_check.hpp"
#include "pat/sim/marriage_godparents.hpp"
#include "pat/sim/replay.hpp"
#include "pat/sim/save.hpp"

#include <algorithm>
#include <array>
#include <memory>
#include <span>

#include "marriage_test_fixture.hpp"

namespace {

using namespace pat::sim;
using pat::core::Fixed32;
using Godparents = std::array<CharacterId, kGodparentSlots>;

struct Scene : pat::test::marriage::Scene {
    CharacterId child(Sex sex = Sex::Female) { return person(groom_house, sex, world.today(), groom, bride); }

    CharacterId adult(DynastyId house, Sex sex, int age = 35) {
        return person(house, sex, Date::from_ymd(world.today().year() - age, 5, 20));
    }

    void execute(const Command& command) {
        REQUIRE(validate(world, command).has_value());
        apply(world, command);
        CHECK(world.check_invariants().empty());
    }

    void reject(const Command& command, Rejection reason) {
        const auto result = validate(world, command);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().reason == reason);
        const auto before = world.compute_hash();
        CommandLog log;
        const std::array commands{command};
        const auto stats = process_commands(world, commands, CommandSource::Internal, log);
        CHECK(stats.applied == 0);
        CHECK(stats.rejected == 1);
        CHECK(world.compute_hash() == before);
    }
};

std::vector<Command> emit(World& world) {
    pat::core::Arena scratch;
    WorkBudget budget;
    SerialExecutor executor;
    std::vector<Command> commands;
    TickContext context{world.today(),
                        events_for(world.today()),
                        world.calendar().tick_count(),
                        scratch,
                        budget,
                        executor,
                        commands};
    make_marriage_godparents_system()->run(world, context);
    return commands;
}

std::int32_t score(const std::vector<GodparentCandidate>& candidates, CharacterId person) {
    const auto row = std::ranges::find(candidates, person, &GodparentCandidate::character);
    REQUIRE(row != candidates.end());
    return row->score;
}

bool spiritual_denial(const World& world, CharacterId groom, CharacterId bride) {
    const auto denials = check_marriage(
        world, MarriageRules::defaults(), groom, bride, {.as_betrothal = true, .dispensation = true});
    return std::ranges::find(denials, MarriageDenial::SpiritualKinship) != denials.end();
}

}  // namespace

TEST_CASE("marriage godparents: ranked adults exclude parents, siblings, children and outsiders") {
    Scene s;
    const auto man = s.adult(s.groom_house, Sex::Male);
    const auto second_man = s.adult(s.groom_house, Sex::Male);
    const auto woman = s.adult(s.bride_house, Sex::Female);
    (void)s.adult(s.third_house, Sex::Male);
    (void)s.adult(s.groom_house, Sex::Female, 15);
    const auto deceased = s.adult(s.groom_house, Sex::Male);
    REQUIRE(s.world.die(deceased, CauseOfDeath::Illness));
    (void)s.person(s.groom_house, Sex::Male, Date::from_ymd(1687, 5, 20), s.groom, s.bride);
    (void)s.person(s.bride_house, Sex::Female, Date::from_ymd(1687, 5, 20), {}, s.bride);
    const auto child = s.child();
    const auto rng_before = s.world.compute_hash();
    const auto candidates = godparent_candidates(s.world, child);
    REQUIRE(candidates.size() == 3);
    CHECK(candidates[0] == GodparentCandidate{man, 20});
    CHECK(candidates[1] == GodparentCandidate{second_man, 20});
    CHECK(candidates[2] == GodparentCandidate{woman, 10});
    CHECK(choose_godparents(s.world, child) == Godparents{man, woman});
    CHECK(s.world.compute_hash() == rng_before);
}

TEST_CASE("marriage godparents: scores use mean clamped opinion, age decades and parent siblings") {
    Scene s;
    const auto liked = s.adult(s.bride_house, Sex::Female);
    const auto old = s.adult(s.groom_house, Sex::Male, 45);
    const auto young = s.adult(s.groom_house, Sex::Male, 25);
    const auto just_adult = s.adult(s.groom_house, Sex::Male, 16);
    REQUIRE(s.world.add_opinion(s.groom, liked, Fixed32::from_int(100), 1));
    REQUIRE(s.world.add_opinion(s.bride, liked, Fixed32::from_int(100), 1));
    const auto child = s.child();
    const auto candidates = godparent_candidates(s.world, child);
    CHECK(score(candidates, liked) == 25);
    CHECK(score(candidates, old) == 15);
    CHECK(score(candidates, young) == 15);
    CHECK(score(candidates, just_adult) == 15);
    CHECK(choose_godparents(s.world, child)[0] == liked);

    const auto grandfather = s.person(s.groom_house, Sex::Male, Date::from_ymd(1640, 1, 1));
    const auto father = s.person(s.groom_house, Sex::Male, Date::from_ymd(1670, 1, 1), grandfather);
    const auto uncle = s.person(s.groom_house, Sex::Male, Date::from_ymd(1668, 5, 20), grandfather);
    const auto cousin = s.person(s.groom_house, Sex::Female, s.world.today(), father, s.bride);
    CHECK(score(godparent_candidates(s.world, cousin), uncle) == 30);
    REQUIRE(s.world.add_opinion(s.groom, old, Fixed32::from_int(-100), 1));
    CHECK(score(godparent_candidates(s.world, child), old) == 0);  // (-100 + 0) / 2, weighted 300 permille.
}

TEST_CASE("marriage godparents: same-house parents do not duplicate candidates and fewer are accepted") {
    Scene s;
    s.world.set_character_dynasty(s.bride, s.groom_house);
    const auto child = s.child();
    CHECK(choose_godparents(s.world, child) == Godparents{});
    const auto first = s.adult(s.groom_house, Sex::Female);
    CHECK(choose_godparents(s.world, child) == Godparents{first, {}});
    const auto second = s.adult(s.groom_house, Sex::Female);
    REQUIRE(godparent_candidates(s.world, child).size() == 2);
    CHECK(choose_godparents(s.world, child) == Godparents{first, second});
    s.execute(Command{{}, cmd::AssignGodparents{child, {first, second}}});
    CHECK(s.world.get_opinion(first, s.groom) == Fixed32::from_int(5));
    CHECK(s.world.get_opinion(s.groom, first) == Fixed32::from_int(5));
    CHECK(s.world.get_opinion(first, s.bride) == Fixed32::from_int(5));
    const auto sources = s.world.opinion_sources(first, s.groom);
    REQUIRE(sources.size() == 1);
    CHECK(sources.front().reason == kOpinionReasonGodparent);
}

TEST_CASE("marriage godparents: commands reject unauthorized and invalid assignments without effects") {
    Scene s;
    const auto man = s.adult(s.groom_house, Sex::Male);
    const auto outsider = s.adult(s.third_house, Sex::Female);
    const auto child = s.child();
    const auto sibling = s.person(s.groom_house, Sex::Male, Date::from_ymd(1687, 5, 20), s.groom);
    const auto juvenile = s.adult(s.groom_house, Sex::Male, 15);
    const auto dead = s.adult(s.groom_house, Sex::Male);
    REQUIRE(s.world.die(dead, CauseOfDeath::Illness));
    const Command valid{{}, cmd::AssignGodparents{child, {man, {}}}};
    s.reject(Command{s.groom_house, valid.payload}, Rejection::NotAuthorized);
    s.reject(Command{{}, cmd::AssignGodparents{CharacterId(9999), {}}}, Rejection::UnknownEntity);
    for (const auto candidate : {child, s.groom, s.bride, sibling, outsider, juvenile, dead}) {
        s.reject(Command{{}, cmd::AssignGodparents{child, {candidate, {}}}}, Rejection::InvalidArgument);
    }
    s.reject(Command{{}, cmd::AssignGodparents{child, {man, man}}}, Rejection::InvalidArgument);
    s.reject(Command{{}, cmd::AssignGodparents{child, {CharacterId(), man}}}, Rejection::InvalidArgument);
    s.reject(Command{{}, cmd::AssignGodparents{s.groom, {man, {}}}}, Rejection::InvalidState);
    s.execute(valid);
    const auto after = s.world.compute_hash();
    s.reject(valid, Rejection::InvalidState);
    CHECK(s.world.compute_hash() == after);
}

TEST_CASE("marriage godparents: ordered commands acknowledge old, dead and already assigned rows") {
    Scene s;
    const auto man = s.adult(s.groom_house, Sex::Male);
    const auto first = s.child();
    const auto second = s.child();
    const auto historical = s.adult(s.groom_house, Sex::Female, 1);
    REQUIRE(s.world.set_character_godparents(historical, {man, {}}));
    const auto last = s.adult(s.groom_house, Sex::Male);
    REQUIRE(s.world.die(last, CauseOfDeath::Illness));
    const auto before = s.world.compute_hash();
    const auto commands = emit(s.world);
    CHECK(s.world.compute_hash() == before);
    REQUIRE(commands.size() == 3);
    CHECK(std::get<cmd::AssignGodparents>(commands[0].payload).child == first);
    CHECK(std::get<cmd::AssignGodparents>(commands[1].payload).child == second);
    CHECK(std::get<cmd::AssignGodparents>(commands[2].payload) == cmd::AssignGodparents{last, {}});
    for (const auto& command : commands) {
        s.execute(command);
    }
    CHECK(s.world.family().godparent_cursor == s.world.characters().ids.capacity());
    CHECK(s.world.characters().godparent[historical.index()] == Godparents{man, {}});
    CHECK(emit(s.world).empty());
    s.reject(commands.back(), Rejection::InvalidState);
}

TEST_CASE("marriage godparents: no candidates and all historical rows still advance the cursor") {
    Scene s;
    const auto child = s.child();
    const auto commands = emit(s.world);
    REQUIRE(commands.size() == 1);
    CHECK(std::get<cmd::AssignGodparents>(commands.front().payload) == cmd::AssignGodparents{child, {}});
    s.execute(commands.front());
    CHECK(emit(s.world).empty());
    (void)s.adult(s.groom_house, Sex::Male);
    const auto progress = emit(s.world);
    REQUIRE(progress.size() == 1);
    s.execute(progress.front());
    CHECK(s.world.characters().godparent[child.index()] == Godparents{});
    CHECK(s.world.opinions_from(s.groom).empty());
    CHECK(emit(s.world).empty());
}

TEST_CASE("marriage godparents: a rejected queued assignment cannot be skipped by later progress") {
    Scene s;
    const auto man = s.adult(s.groom_house, Sex::Male);
    const auto woman = s.adult(s.bride_house, Sex::Female);
    const auto first = s.child();
    const auto second = s.child();
    (void)s.adult(s.third_house, Sex::Male);  // Trailing cursor-only marker.
    const auto pending = emit(s.world);
    REQUIRE(pending.size() == 3);
    REQUIRE(s.world.die(man, CauseOfDeath::Illness));
    (void)s.world.advance_day();
    s.reject(pending.front(), Rejection::InvalidArgument);
    s.reject(pending[1], Rejection::InvalidState);
    s.reject(pending[2], Rejection::InvalidState);
    CHECK(s.world.family().godparent_cursor == 0);
    const auto recovered = emit(s.world);
    REQUIRE(recovered.size() == 3);
    (void)s.world.advance_day();
    for (const auto& command : recovered) {
        s.execute(command);
    }
    CHECK(s.world.characters().godparent[first.index()] == Godparents{woman, {}});
    CHECK(s.world.characters().godparent[second.index()] == Godparents{woman, {}});
    CHECK(s.world.family().godparent_cursor == s.world.characters().ids.capacity());
}

TEST_CASE("marriage godparents: narrow birth window excludes historical infants") {
    Scene s;
    const auto man = s.adult(s.groom_house, Sex::Male);
    const auto child = s.child();
    (void)s.world.advance_day();
    REQUIRE(std::get<cmd::AssignGodparents>(emit(s.world).front().payload).godparents[0] == man);
    (void)s.world.advance_day();
    s.execute(Command{{}, cmd::AssignGodparents{child, {man, {}}}});  // Queued yesterday, applied today.
    Scene stale;
    const auto other = stale.adult(stale.groom_house, Sex::Male);
    const auto infant = stale.child();
    for (int day = 0; day < 3; ++day) {
        (void)stale.world.advance_day();
    }
    stale.reject(Command{{}, cmd::AssignGodparents{infant, {other, {}}}}, Rejection::InvalidState);
    const auto skipped = emit(stale.world);
    REQUIRE(skipped.size() == 1);
    CHECK(std::get<cmd::AssignGodparents>(skipped.front().payload).godparents == Godparents{});
}

TEST_CASE("marriage godparents: full saves preserve pending choices, cursor, opinions and replay") {
    Scene s;
    const auto man = s.adult(s.groom_house, Sex::Male);
    const auto woman = s.adult(s.bride_house, Sex::Female);
    const auto child = s.child();
    const World initial = s.world;
    auto rules = std::make_shared<MarriageRules>(MarriageRules::defaults());
    Simulation sim(initial);
    install_family_systems(sim, rules);
    CHECK(sim.system_count() == 3);
    ReplayRecorder recorder(299, 308, initial.today(), 1);
    (void)sim.tick();
    recorder.after_tick(sim);
    REQUIRE(sim.pending_internal().size() == 1);
    const auto choice = std::get<cmd::AssignGodparents>(sim.pending_internal().front().payload);
    CHECK(choice == cmd::AssignGodparents{child, {man, woman}});
    auto saved = deserialize_save(serialize_save(sim.world(), sim.pending_internal()));
    REQUIRE(saved.has_value());
    Simulation resumed(initial);
    install_family_systems(resumed, rules);
    resumed.restore_state(std::move(saved.value().world), std::move(saved.value().pending_internal));
    for (int day = 0; day < 3; ++day) {
        (void)sim.tick();
        (void)resumed.tick();
        recorder.after_tick(sim);
        CHECK(resumed.world().compute_hash() == sim.world().compute_hash());
    }
    CHECK(sim.world().characters().godparent[child.index()] == Godparents{man, woman});
    CHECK(sim.world().get_opinion(s.groom, man) == Fixed32::from_int(5));
    CHECK(sim.pending_internal().empty());
    auto replay = deserialize_replay(serialize_replay(recorder.finish(sim)));
    REQUIRE(replay.has_value());
    Simulation playback(initial);
    install_family_systems(playback, rules);
    CHECK(play_replay(playback, replay.value()).ok);
    CHECK(playback.world().compute_hash() == sim.world().compute_hash());
}

TEST_CASE("marriage godparents: a birth-day World-only save regenerates its lost command next day") {
    Scene s;
    const auto man = s.adult(s.groom_house, Sex::Male);
    const auto child = s.child();
    const auto pending = emit(s.world);
    REQUIRE(pending.size() == 1);
    auto saved = deserialize_save(serialize_save(s.world));
    REQUIRE(saved.has_value());
    Simulation resumed(saved.value().world);
    install_family_systems(resumed, {});
    (void)resumed.tick();
    REQUIRE(resumed.pending_internal().size() == 1);
    CHECK(resumed.pending_internal().front() == pending.front());
    (void)resumed.tick();
    CHECK(resumed.world().characters().godparent[child.index()] == Godparents{man, {}});
    CHECK(resumed.world().family().godparent_cursor == s.world.characters().ids.capacity());
}

TEST_CASE("marriage godparents: spiritual prohibitions and genealogy survive archived participants") {
    Scene s;
    const auto man = s.adult(s.groom_house, Sex::Male);
    const auto child = s.child();
    s.execute(Command{{}, cmd::AssignGodparents{child, {man, {}}}});
    CHECK(spiritual_denial(s.world, man, child));
    CHECK(spiritual_denial(s.world, man, s.bride));
    const auto second = s.child(Sex::Male);
    s.execute(Command{{}, cmd::AssignGodparents{second, {man, {}}}});
    CHECK(spiritual_denial(s.world, second, child));
    REQUIRE(s.world.die(man, CauseOfDeath::Illness));
    CHECK(kin::spiritually_related(s.world, man, child));
    CHECK(spiritual_denial(s.world, second, child));
    REQUIRE(s.world.die(child, CauseOfDeath::Illness));
    CHECK(kin::spiritually_related(s.world, man, s.bride));
    CHECK(s.world.characters().father[child.index()] == s.groom);
    CHECK(s.world.characters().mother[child.index()] == s.bride);
    CHECK(s.world.characters().godparent[child.index()] == Godparents{man, {}});
    auto saved = deserialize_save(serialize_save(s.world));
    REQUIRE(saved.has_value());
    CHECK(saved.value().world.compute_hash() == s.world.compute_hash());
    CHECK(kin::spiritually_related(saved.value().world, man, s.bride));
}

TEST_CASE("marriage godparents: default simulation leaves slots and cursor untouched") {
    Scene s;
    (void)s.adult(s.groom_house, Sex::Male);
    const auto child = s.child();
    Simulation sim(s.world);
    sim.run_days(3);
    CHECK(sim.system_count() == 0);
    CHECK(sim.pending_internal().empty());
    CHECK(sim.world().family().godparent_cursor == 0);
    CHECK(sim.world().characters().godparent[child.index()] == Godparents{});
}

TEST_CASE("marriage godparents: tag 77 retains its fixed body and rejects every truncated payload") {
    for (const auto pair : {Godparents{CharacterId(3), CharacterId(4)}, Godparents{}}) {
        const Command command{{}, cmd::AssignGodparents{CharacterId(9), pair}};
        pat::core::ByteWriter writer;
        write_command(writer, command);
        CHECK(writer.data().front() == 77);
        pat::core::ByteReader reader(writer.data());
        Command decoded;
        REQUIRE(read_command(reader, decoded));
        CHECK(decoded == command);
        CHECK(reader.at_end());
        for (std::size_t size = 0; size < writer.data().size(); ++size) {
            pat::core::ByteReader truncated(std::span<const std::uint8_t>(writer.data().data(), size));
            CHECK_FALSE(read_command(truncated, decoded));
        }
    }
}
