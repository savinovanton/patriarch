#include "pat/sim/replay.hpp"
#include "pat/sim/save.hpp"
#include "pat/sim/simulation.hpp"
#include "pat/sim/succession.hpp"

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <utility>
#include <variant>

namespace {

using namespace pat::sim;

constexpr Date kStart = Date::from_ymd(1703, 1, 1);
constexpr std::uint64_t kSeed = 31;

struct Family {
    World world{kStart, kSeed};
    DynastyId dynasty;

    Family() {
        REQUIRE(world.enable_dynasty_mode());
        dynasty = world.create_dynasty(pat::core::StringId(1));
    }

    CharacterId person(int age, Sex sex = Sex::Male, CharacterId father = {}, CharacterId mother = {}) {
        CharacterInit init;
        init.dynasty = dynasty;
        init.birth_date = Date::from_ymd(kStart.year() - age, 1, 1);
        init.sex = sex;
        init.father = father;
        init.mother = mother;
        const CharacterId id = world.create_character(init);
        REQUIRE(id.valid());
        return id;
    }

    void retire(CharacterId id) { REQUIRE(world.die(id, CauseOfDeath::Illness)); }

    [[nodiscard]] CharacterId successor(CharacterId predecessor) const {
        return fallback_successor(world, dynasty, predecessor);
    }
};

Command die(CharacterId id) {
    return Command{DynastyId{}, cmd::Die{id, CauseOfDeath::Illness}};
}

std::size_t successor_commands(const Simulation& sim) {
    std::size_t count = 0;
    for (const auto& entry : sim.command_log().entries()) {
        if (entry.accepted() && entry.source == CommandSource::Internal &&
            std::holds_alternative<cmd::SetDynastyHead>(entry.command.payload)) {
            ++count;
        }
    }
    return count;
}

}  // namespace

TEST_CASE("fallback succession: sons, brothers and nephews precede an older unrelated adult") {
    Family f;
    const CharacterId mother = f.person(89, Sex::Female);
    const CharacterId head = f.person(64, Sex::Male, {}, mother);
    const CharacterId brother = f.person(55, Sex::Male, {}, mother);
    const CharacterId nephew = f.person(29, Sex::Male, brother);
    const CharacterId younger = f.person(30, Sex::Male, head);
    const CharacterId first_older = f.person(35, Sex::Male, head);
    const CharacterId second_older = f.person(35, Sex::Male, head);
    const CharacterId daughter = f.person(39, Sex::Female, head);
    const CharacterId elder = f.person(72);
    const CharacterId minor = f.person(15, Sex::Male, head);

    f.retire(head);
    CHECK(f.successor(head) == first_older);
    f.retire(first_older);
    CHECK(f.successor(head) == second_older);
    f.retire(second_older);
    CHECK(f.successor(head) == younger);
    f.retire(younger);
    CHECK(f.successor(head) == brother);
    f.retire(brother);
    CHECK(f.successor(head) == nephew);
    f.retire(nephew);
    CHECK(f.successor(head) == elder);
    f.retire(elder);
    CHECK(f.successor(head) == daughter);
    CHECK(f.world.is_alive(minor));
}

TEST_CASE("fallback succession: daughters and their adult children precede unrelated women") {
    Family f;
    const CharacterId head = f.person(65);
    const CharacterId elder = f.person(72, Sex::Female);
    const CharacterId daughter = f.person(41, Sex::Female, head);
    const CharacterId granddaughter = f.person(19, Sex::Female, {}, daughter);
    const CharacterId minor = f.person(15, Sex::Male, head);

    f.retire(head);
    CHECK(f.successor(head) == daughter);
    f.retire(daughter);
    CHECK(f.successor(head) == granddaughter);
    f.retire(granddaughter);
    CHECK(f.successor(head) == elder);
    f.retire(elder);
    CHECK_FALSE(f.successor(head).valid());
    CHECK(f.world.is_alive(minor));
    CHECK_FALSE(f.world.dynasty_extinct(f.dynasty));
}

TEST_CASE("succession: a head death appoints a survivor after the whole batch and replays the appointment") {
    Family f;
    const CharacterId head = f.person(67);
    const CharacterId first = f.person(38, Sex::Male, head);
    const CharacterId survivor = f.person(29, Sex::Male, head);
    REQUIRE(f.world.set_dynasty_head(f.dynasty, head));

    Simulation sim(f.world);
    ReplayRecorder recorder(kSeed, 0x5155ULL, kStart, 1);
    REQUIRE(sim.submit(die(head)));
    REQUIRE(sim.submit(die(first)));
    (void)sim.tick();
    recorder.after_tick(sim);
    REQUIRE(sim.world().dynasties().head[f.dynasty.index()] == survivor);
    REQUIRE(successor_commands(sim) == 1);
    CHECK(sim.world().is_archived(first));
    CHECK(sim.world().check_invariants().empty());

    const auto entries = sim.world().query_chronicle(
        f.dynasty, sim.world().today(), sim.world().today(), ChronicleKind::HeadChanged);
    REQUIRE(entries.size() == 1);
    CHECK(entries.front().b == survivor);

    auto parsed = deserialize_replay(serialize_replay(recorder.finish(sim)));
    REQUIRE(parsed.has_value());
    CHECK(parsed.value().commands.size() == 2);
    Simulation replayed(f.world);
    CHECK(play_replay(replayed, parsed.value()).ok);
    CHECK(replayed.world().compute_hash() == sim.world().compute_hash());
    CHECK(successor_commands(replayed) == 1);

    (void)sim.tick();
    CHECK(successor_commands(sim) == 1);
}

TEST_CASE("succession: a saved internal death appoints the successor and the last death marks extinction") {
    Family f;
    const CharacterId head = f.person(61);
    const CharacterId child = f.person(30, Sex::Male, head);
    REQUIRE(f.world.set_dynasty_head(f.dynasty, head));
    CHECK_FALSE(f.world.dynasty_extinct(f.dynasty));
    CHECK_FALSE(f.world.dynasty_extinct(DynastyId{}));

    Simulation original(f.world);
    original.restore_state(f.world, {die(head)});
    auto loaded = deserialize_save(serialize_save(original.world(), original.pending_internal()));
    REQUIRE(loaded.has_value());
    Simulation resumed(f.world);
    resumed.restore_state(std::move(loaded.value().world), std::move(loaded.value().pending_internal));

    (void)original.tick();
    (void)resumed.tick();
    CHECK(original.world().compute_hash() == resumed.world().compute_hash());
    CHECK(resumed.world().dynasties().head[f.dynasty.index()] == child);
    CHECK(successor_commands(resumed) == 1);

    REQUIRE(resumed.submit(die(child)));
    (void)resumed.tick();
    CHECK(resumed.world().dynasty_extinct(f.dynasty));
    CHECK_FALSE(resumed.world().dynasties().head[f.dynasty.index()].valid());
    CHECK(successor_commands(resumed) == 1);
    const auto entries = resumed.world().query_chronicle(
        f.dynasty, kStart, resumed.world().today(), ChronicleKind::Extinction);
    REQUIRE(entries.size() == 1);
    CHECK(entries.front().a == child);
}

TEST_CASE("succession: unrelated and rejected deaths leave the head, an explicit appointment prevails") {
    Family f;
    const CharacterId head = f.person(65);
    const CharacterId relative = f.person(32, Sex::Male, head);
    const CharacterId appointment = f.person(55);
    const CharacterId other = f.person(50);
    REQUIRE(f.world.set_dynasty_head(f.dynasty, head));

    Simulation sim(std::move(f.world));
    REQUIRE(sim.submit(Command{DynastyId{}, cmd::Die{head, CauseOfDeath::None}}));
    REQUIRE(sim.submit(die(other)));
    (void)sim.tick();
    CHECK(sim.world().dynasties().head[f.dynasty.index()] == head);
    CHECK(successor_commands(sim) == 0);

    REQUIRE(sim.submit(die(head)));
    REQUIRE(sim.submit(Command{DynastyId{}, cmd::SetDynastyHead{f.dynasty, appointment}}));
    (void)sim.tick();
    CHECK(sim.world().dynasties().head[f.dynasty.index()] == appointment);
    CHECK(sim.world().is_alive(relative));
    CHECK(successor_commands(sim) == 0);

    Family headless;
    const CharacterId member = headless.person(51);
    const CharacterId other_member = headless.person(24, Sex::Male, member);
    Simulation without_head(std::move(headless.world));
    REQUIRE(without_head.submit(die(member)));
    (void)without_head.tick();
    CHECK(without_head.world().is_alive(other_member));
    CHECK_FALSE(without_head.world().dynasties().head[headless.dynasty.index()].valid());
    CHECK(successor_commands(without_head) == 0);
}
