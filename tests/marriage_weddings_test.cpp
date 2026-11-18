#include "pat/sim/command_log.hpp"
#include "pat/sim/family_systems.hpp"
#include "pat/sim/marriage_check.hpp"
#include "pat/sim/marriage_text.hpp"
#include "pat/sim/replay.hpp"
#include "pat/sim/save.hpp"

#include <algorithm>
#include <array>
#include <memory>
#include <span>

#include "marriage_test_fixture.hpp"

namespace {

using namespace pat::sim;
using economy::AccountRef;
using economy::LedgerCode;
using BreakReason = family::BetrothalBreakReason;

struct Scene : pat::test::marriage::Scene {
    MarriageRules rules = MarriageRules::defaults();

    explicit Scene(Date today = Date::from_ymd(1703, 5, 20),
                   Date groom_birth = Date::from_ymd(1670, 1, 1),
                   Date bride_birth = Date::from_ymd(1670, 1, 1))
        : pat::test::marriage::Scene(today, groom_birth, bride_birth) {
        REQUIRE(world.set_dynasty_head(groom_house, groom));
        const auto bride_head = person(bride_house, Sex::Male);
        REQUIRE(world.set_dynasty_head(bride_house, bride_head));
    }

    CommandContent content() const { return {nullptr, nullptr, &rules}; }

    void execute(const Command& command) {
        REQUIRE(validate(world, command, content()).has_value());
        apply(world, command, content());
        CHECK(world.check_invariants().empty());
    }

    family::Betrothal engage(std::int64_t dowry = 0) {
        execute(Command{DynastyId(), cmd::Betroth{groom, bride, dowry, bride_house, false}});
        REQUIRE(world.family().betrothals.size() == 1);
        return world.family().betrothals.front();
    }

    void reject(const Command& command) {
        const auto before = world.compute_hash();
        CommandLog log;
        const std::array commands{command};
        const auto stats = process_commands(world, commands, CommandSource::Internal, log, content());
        CHECK(stats.applied == 0);
        CHECK(stats.rejected == 1);
        CHECK(world.compute_hash() == before);
    }

    void advance_to(Date date) {
        while (world.today() < date) {
            (void)world.advance_day();
        }
    }
};

std::size_t notes(const World& world, DynastyId house, std::int32_t code) {
    const auto entries = world.chronicle(house);
    return static_cast<std::size_t>(std::count_if(entries.begin(), entries.end(), [code](const auto& e) {
        return e.kind == ChronicleKind::Note && e.param == code;
    }));
}

}  // namespace

TEST_CASE("marriage weddings: child betrothal waits for both birthdays and applies next tick") {
    Scene scene(Date::from_ymd(1703, 5, 18), Date::from_ymd(1687, 5, 19), Date::from_ymd(1688, 5, 20));
    const auto row = scene.engage();
    CHECK(row.origin == family::BetrothalOrigin::Command);
    CHECK(row.contracted == scene.world.today());
    CHECK(row.wedding_not_before == Date::from_ymd(1703, 5, 20));
    CHECK(row.wedding_deadline == row.wedding_not_before + scene.rules.negotiation.wedding_grace_days);
    CHECK(notes(scene.world, scene.groom_house, 204) == 1);
    scene.reject(Command{DynastyId(), cmd::Wed{row.id}});

    Simulation sim(scene.world);
    install_family_systems(sim, std::make_shared<MarriageRules>(scene.rules));
    (void)sim.tick();
    CHECK(sim.pending_internal().empty());
    (void)sim.tick();
    REQUIRE(sim.pending_internal().size() == 1);
    CHECK(type_of(sim.pending_internal().front()) == CommandType::Wed);
    CHECK(sim.world().family().marriages.empty());
    (void)sim.tick();
    CHECK(sim.world().family().betrothals.empty());
    REQUIRE(sim.world().family().marriages.size() == 1);
    CHECK(sim.world().family().marriages.front().wedding == row.wedding_not_before + 1);
    CHECK(sim.world().characters().spouse[scene.groom.index()] == scene.bride);
    CHECK(sim.world().characters().spouse[scene.bride.index()] == scene.groom);
    CHECK(notes(sim.world(), scene.groom_house, 206) == 1);
    sim.run_days(2);
    CHECK(sim.world().family().marriages.size() == 1);
    CHECK(sim.pending_internal().empty());
}

TEST_CASE("marriage weddings: default simulation leaves an engagement untouched") {
    Scene scene;
    (void)scene.engage();
    Simulation sim(scene.world);
    CHECK(sim.system_count() == 0);
    sim.run_days(3);
    CHECK(sim.world().family().betrothals.size() == 1);
    CHECK(sim.world().family().marriages.empty());
    CHECK(sim.pending_internal().empty());
}

TEST_CASE("marriage weddings: the last grace day permits its queued wedding") {
    Scene scene;
    scene.rules.negotiation.wedding_grace_days = 2;
    const auto row = scene.engage();
    Simulation sim(scene.world);
    install_family_systems(sim, std::make_shared<MarriageRules>(scene.rules));
    bool expired = false;
    SUBCASE("deadline day emits a wedding") {
        scene.advance_to(row.wedding_deadline - 1);
    }
    SUBCASE("past deadline emits a harmless break") {
        expired = true;
        scene.advance_to(row.wedding_deadline);
    }
    const auto honor = scene.world.dynasties().honor[scene.groom_house.index()];
    sim.restore_state(scene.world, {});
    (void)sim.tick();
    REQUIRE(sim.pending_internal().size() == 1);
    if (expired) {
        const auto& broken = std::get<cmd::BreakBetrothal>(sim.pending_internal().front().payload);
        CHECK(broken.betrothal == row.id);
        CHECK(broken.reason == BreakReason::GraceExpired);
    } else {
        CHECK(type_of(sim.pending_internal().front()) == CommandType::Wed);
    }
    (void)sim.tick();
    CHECK(sim.world().family().betrothals.empty());
    CHECK(sim.world().family().marriages.size() == (expired ? 0 : 1));
    CHECK(sim.world().dynasties().honor[scene.groom_house.index()] == honor);
}

TEST_CASE("marriage weddings: authority and voluntary penalties are applied only once") {
    Scene scene;
    scene.reject(
        Command{scene.groom_house, cmd::Betroth{scene.groom, scene.bride, 0, scene.bride_house, false}});
    const auto row = scene.engage();
    scene.reject(Command{scene.groom_house, cmd::Wed{row.id}});
    scene.reject(Command{scene.third_house, cmd::BreakBetrothal{row.id, BreakReason::Voluntary}});
    scene.reject(Command{scene.groom_house, cmd::BreakBetrothal{row.id, BreakReason::PartyDied}});
    const auto honor = scene.world.dynasties().honor[scene.groom_house.index()];
    const auto responder = scene.world.dynasties().head[scene.bride_house.index()];
    const auto opinion = scene.world.get_opinion(responder, scene.groom);
    const Command broken{scene.groom_house, cmd::BreakBetrothal{row.id, BreakReason::Voluntary}};
    scene.execute(broken);
    CHECK(scene.world.family().betrothals.empty());
    CHECK(scene.world.dynasties().honor[scene.groom_house.index()] ==
          honor - scene.rules.consequences.betrothal_broken_honor);
    CHECK(scene.world.get_opinion(responder, scene.groom) ==
          opinion - pat::core::Fixed32::from_int(scene.rules.consequences.betrothal_broken_opinion));
    CHECK(notes(scene.world, scene.groom_house, 205) == 1);
    scene.reject(broken);
}

TEST_CASE("marriage weddings: participant death cancels the engagement synchronously") {
    Scene scene;
    const auto row = scene.engage();
    const auto honor = scene.world.dynasties().honor[scene.groom_house.index()];
    REQUIRE(scene.world.die(scene.bride, CauseOfDeath::Illness));
    CHECK(scene.world.family().betrothals.empty());
    CHECK(scene.world.family_betrothal_of(scene.groom) == nullptr);
    CHECK(notes(scene.world, scene.groom_house, kChronicleBetrothalPartyDied) == 1);
    const auto entries = scene.world.chronicle(scene.groom_house);
    const auto death_note = std::find_if(entries.begin(), entries.end(), [](const auto& entry) {
        return entry.kind == ChronicleKind::Note && entry.param == kChronicleBetrothalPartyDied;
    });
    REQUIRE(death_note != entries.end());
    CHECK(death_note->a == scene.groom);
    CHECK(death_note->b == scene.bride);
    CHECK(scene.world.dynasties().honor[scene.groom_house.index()] == honor);
    scene.reject(Command{DynastyId(), cmd::BreakBetrothal{row.id, BreakReason::PartyDied}});
    CHECK(notes(scene.world, scene.groom_house, kChronicleBetrothalPartyDied) == 1);
}

TEST_CASE("marriage weddings: dowry transfers once and preserves the bride origin house") {
    Scene scene;
    bool female_head = false;
    SUBCASE("ordinary bride joins husband") {}
    SUBCASE("female head retains her house") {
        female_head = true;
        REQUIRE(scene.world.set_dynasty_head(scene.bride_house, scene.bride));
    }
    REQUIRE(scene.world.enable_economy());
    const auto source = AccountRef::of(scene.bride_house);
    const auto target = AccountRef::of(scene.groom_house);
    REQUIRE(scene.world.set_initial_treasury(source, 4321));
    REQUIRE(scene.world.set_initial_treasury(target, 100));
    const auto row = scene.engage(4321);
    CHECK(scene.world.treasury(source) == 4321);
    const Command wedding{DynastyId(), cmd::Wed{row.id}};
    scene.execute(wedding);
    CHECK(scene.world.treasury(source) == 0);
    CHECK(scene.world.treasury(target) == 4421);
    const auto& accounts = scene.world.economy().accounts;
    CHECK(accounts.open_line(*scene.world.resolve_account(source), LedgerCode::DowryOut) == -4321);
    CHECK(accounts.open_line(*scene.world.resolve_account(target), LedgerCode::DowryIn) == 4321);
    REQUIRE(scene.world.family().marriages.size() == 1);
    const auto& marriage = scene.world.family().marriages.front();
    CHECK(marriage.state == family::MarriageState::Active);
    CHECK(marriage.bride_house == scene.bride_house);
    CHECK(marriage.groom_house == scene.groom_house);
    CHECK(marriage.dowry_kop == 4321);
    CHECK(scene.world.characters().dynasty[scene.bride.index()] ==
          (female_head ? scene.bride_house : scene.groom_house));
    if (!female_head) {
        CHECK(scene.world.characters().branch[scene.bride.index()] ==
              scene.world.characters().branch[scene.groom.index()]);
    }
    scene.reject(wedding);
}

TEST_CASE("marriage weddings: debt and saturation preserve the amount actually transferred") {
    Scene scene;
    REQUIRE(scene.world.enable_economy());
    const auto source = AccountRef::of(scene.bride_house);
    const auto target = AccountRef::of(scene.groom_house);
    REQUIRE(scene.world.set_initial_treasury(source, 999));
    std::int64_t contracted = 1000;
    std::int64_t delivered = 1000;
    SUBCASE("dowry may create source debt") {}
    SUBCASE("destination saturation reduces delivery") {
        contracted = 999;
        delivered = 400;
        REQUIRE(scene.world.set_initial_treasury(target, economy::kMaxTreasuryKop - delivered));
    }
    const auto target_before = scene.world.treasury(target);
    const auto row = scene.engage(contracted);
    scene.execute(Command{DynastyId(), cmd::Wed{row.id}});
    CHECK(scene.world.treasury(source) == 999 - delivered);
    CHECK(scene.world.treasury(target) == target_before + delivered);
    CHECK(scene.world.family().betrothals.empty());
    REQUIRE(scene.world.family().marriages.size() == 1);
    CHECK(scene.world.family().marriages.front().dowry_kop == delivered);
    const auto& accounts = scene.world.economy().accounts;
    CHECK(accounts.open_line(*scene.world.resolve_account(source), LedgerCode::DowryOut) == -delivered);
    CHECK(accounts.open_line(*scene.world.resolve_account(target), LedgerCode::DowryIn) == delivered);
}

TEST_CASE("marriage weddings: changed eligibility rejects before moving dowry") {
    Scene scene;
    REQUIRE(scene.world.enable_economy());
    const auto source = AccountRef::of(scene.bride_house);
    REQUIRE(scene.world.set_initial_treasury(source, 1000));
    const auto row = scene.engage(1000);
    family::PersonStatus status;
    status.character = scene.bride;
    status.flags = family::kPersonTonsured;
    status.tonsured_on = scene.world.today();
    REQUIRE(scene.world.set_family_status(status));
    scene.reject(Command{DynastyId(), cmd::Wed{row.id}});
    CHECK(scene.world.treasury(source) == 1000);
    CHECK(scene.world.family().betrothals.size() == 1);
    CHECK(scene.world.family().marriages.empty());
}

TEST_CASE("marriage weddings: the third union pays penance and the fourth is refused") {
    Scene scene;
    for (int previous = 0; previous < 2; ++previous) {
        const auto spouse = scene.person(scene.third_house, Sex::Female);
        REQUIRE(scene.world.marry(scene.groom, spouse));
        REQUIRE(scene.world.end_marriage(scene.groom));
    }
    const auto honor = scene.world.dynasties().honor[scene.groom_house.index()];
    const auto row = scene.engage();
    scene.execute(Command{DynastyId(), cmd::Wed{row.id}});
    CHECK(scene.world.dynasties().honor[scene.groom_house.index()] ==
          honor - scene.rules.consequences.third_marriage_honor);
    REQUIRE(scene.world.die(scene.bride, CauseOfDeath::Illness));
    scene.execute(Command{DynastyId(), cmd::SettleWidowhood{scene.bride}});
    const auto fourth = scene.person(scene.third_house, Sex::Female);
    scene.reject(Command{DynastyId(), cmd::Betroth{scene.groom, fourth, 0, scene.third_house, false}});
    const auto denials = check_marriage(scene.world, scene.rules, scene.groom, fourth);
    CHECK(std::find(denials.begin(), denials.end(), MarriageDenial::MarriageLimit) != denials.end());
}

TEST_CASE("marriage weddings: widowhood sets mourning and a single chronicle fact") {
    Scene scene;
    const auto row = scene.engage();
    scene.execute(Command{DynastyId(), cmd::Wed{row.id}});
    const auto died_on = scene.world.today();
    REQUIRE(scene.world.die(scene.groom, CauseOfDeath::Illness));
    const Command settlement{DynastyId(), cmd::SettleWidowhood{scene.groom}};
    scene.reject(Command{scene.bride_house, cmd::SettleWidowhood{scene.groom}});
    scene.execute(settlement);
    REQUIRE(scene.world.family_status(scene.bride) != nullptr);
    CHECK(scene.world.family_status(scene.bride)->widowed_on == died_on);
    REQUIRE(scene.world.family().marriages.size() == 1);
    CHECK(scene.world.family().marriages.front().state == family::MarriageState::Widowed);
    CHECK(scene.world.family().marriages.front().end_reason == family::MarriageEndReason::Widowhood);
    CHECK(scene.world.family().marriages.front().ended == died_on);
    CHECK(notes(scene.world, scene.groom_house, 208) == 1);
    const auto replacement = scene.person(scene.third_house, Sex::Male);
    const auto denials = check_marriage(scene.world, scene.rules, replacement, scene.bride);
    CHECK(std::find(denials.begin(), denials.end(), MarriageDenial::Mourning) != denials.end());
    scene.reject(settlement);
    CHECK(notes(scene.world, scene.groom_house, 208) == 1);
    scene.advance_to(died_on + scene.rules.eligibility.mourning_days_female);
    const auto expired = check_marriage(scene.world, scene.rules, replacement, scene.bride);
    CHECK(std::find(expired.begin(), expired.end(), MarriageDenial::Mourning) == expired.end());
}

TEST_CASE("marriage weddings: daily widowhood settles either spouse after death") {
    Scene scene;
    bool husband_died = true;
    SUBCASE("widow") {}
    SUBCASE("widower") {
        husband_died = false;
    }
    const auto row = scene.engage();
    scene.execute(Command{DynastyId(), cmd::Wed{row.id}});
    const auto deceased = husband_died ? scene.groom : scene.bride;
    const auto survivor = husband_died ? scene.bride : scene.groom;
    const auto death_date = scene.world.today();
    REQUIRE(scene.world.die(deceased, CauseOfDeath::Illness));
    Simulation sim(scene.world);
    install_family_systems(sim, std::make_shared<MarriageRules>(scene.rules));
    (void)sim.tick();
    REQUIRE(sim.pending_internal().size() == 1);
    REQUIRE(std::holds_alternative<cmd::SettleWidowhood>(sim.pending_internal().front().payload));
    CHECK(std::get<cmd::SettleWidowhood>(sim.pending_internal().front().payload).deceased == deceased);
    (void)sim.tick();
    REQUIRE(sim.world().family_status(survivor) != nullptr);
    CHECK(sim.world().family_status(survivor)->widowed_on == death_date);
    REQUIRE(sim.world().family().marriages.size() == 1);
    CHECK(sim.world().family().marriages.front().state ==
          (husband_died ? family::MarriageState::Widowed : family::MarriageState::Closed));
    CHECK(sim.pending_internal().empty());
    CHECK(sim.world().check_invariants().empty());
}

TEST_CASE("marriage weddings: saves during engagement and queued wedding replay the same result") {
    Scene scene;
    scene.rules.negotiation.wedding_delay_days = 3;
    auto rules = std::make_shared<MarriageRules>(scene.rules);
    const World initial = scene.world;
    Simulation sim(initial);
    install_family_systems(sim, rules);
    ReplayRecorder recorder(299, 809, initial.today(), 1);
    REQUIRE(sim.submit(
        Command{DynastyId(), cmd::Betroth{scene.groom, scene.bride, 0, scene.bride_house, false}}));
    (void)sim.tick();
    recorder.after_tick(sim);
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
    REQUIRE(sim.pending_internal().size() == 1);
    CHECK(type_of(sim.pending_internal().front()) == CommandType::Wed);
    auto queued = deserialize_save(serialize_save(sim.world(), sim.pending_internal()));
    REQUIRE(queued.has_value());
    REQUIRE(queued.value().pending_internal.size() == 1);
    CHECK(queued.value().pending_internal.front() == sim.pending_internal().front());
    Simulation from_queue(initial);
    install_family_systems(from_queue, rules);
    from_queue.restore_state(std::move(queued.value().world), std::move(queued.value().pending_internal));
    (void)sim.tick();
    (void)resumed.tick();
    (void)from_queue.tick();
    recorder.after_tick(sim);
    REQUIRE(sim.world().family().marriages.size() == 1);
    CHECK(resumed.world().compute_hash() == sim.world().compute_hash());
    CHECK(from_queue.world().compute_hash() == sim.world().compute_hash());
    auto replay = deserialize_replay(serialize_replay(recorder.finish(sim)));
    REQUIRE(replay.has_value());
    Simulation playback(initial);
    install_family_systems(playback, rules);
    const auto result = play_replay(playback, replay.value());
    CHECK(result.ok);
    CHECK(result.error.empty());
    CHECK(playback.world().compute_hash() == sim.world().compute_hash());
}

TEST_CASE("marriage weddings: stable command tags round trip and reject truncation") {
    const std::array commands{
        Command{DynastyId(), cmd::Betroth{CharacterId(7), CharacterId(8), 12345, DynastyId(2), true}},
        Command{DynastyId(2), cmd::BreakBetrothal{42, BreakReason::Voluntary}},
        Command{DynastyId(), cmd::Wed{42}},
        Command{DynastyId(), cmd::SettleWidowhood{CharacterId(7)}}};
    constexpr std::array<std::uint8_t, 4> tags{67, 68, 78, 81};
    for (std::size_t index = 0; index < commands.size(); ++index) {
        pat::core::ByteWriter writer;
        write_command(writer, commands[index]);
        CHECK(writer.data().front() == tags[index]);
        pat::core::ByteReader reader(writer.data());
        Command decoded;
        REQUIRE(read_command(reader, decoded));
        CHECK(decoded == commands[index]);
        CHECK(reader.at_end());
        for (std::size_t size = 0; size < writer.data().size(); ++size) {
            pat::core::ByteReader truncated(std::span<const std::uint8_t>(writer.data().data(), size));
            CHECK_FALSE(read_command(truncated, decoded));
        }
    }
}
