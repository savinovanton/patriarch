#include "pat/core/rng.hpp"
#include "pat/sim/world.hpp"

#include <doctest/doctest.h>

#include <atomic>
#include <thread>
#include <vector>

using namespace pat::sim;
using pat::core::Fixed32;
using pat::core::Pcg32;
using pat::core::StringId;

namespace {

World make_world() {
    return World(Date::from_ymd(1703, 1, 1), 42);
}

CharacterInit adult(DynastyId dynasty, Sex sex = Sex::Male) {
    CharacterInit init;
    init.dynasty = dynasty;
    init.birth_date = Date::from_ymd(1670, 5, 1);
    init.sex = sex;
    return init;
}

void require_valid(const World& world) {
    const auto problems = world.check_invariants();
    for (const auto& p : problems) {
        MESSAGE(p);
    }
    REQUIRE(problems.empty());
}

}  // namespace

TEST_CASE("world: starts at the requested date with no entities") {
    const World world = make_world();
    CHECK(world.today() == Date::from_ymd(1703, 1, 1));
    CHECK(world.dynasties().size() == 0);
    CHECK(world.provinces().size() == 0);
    CHECK(world.characters().ids.alive_count() == 0);
    require_valid(world);
}

TEST_CASE("world: dynasties, provinces and characters can be created and queried") {
    World world = make_world();
    const DynastyId golitsyn = world.create_dynasty(StringId(0));
    const DynastyId menshikov = world.create_dynasty(StringId(1));
    CHECK(golitsyn.value() == 0);
    CHECK(menshikov.value() == 1);

    const ProvinceId tula = world.create_province(StringId(10), golitsyn, Fixed32::from_int(52));
    const ProvinceId kursk = world.create_province(StringId(11), DynastyId(), Fixed32::from_int(30));
    CHECK(world.provinces().owner[tula.index()] == golitsyn);
    CHECK_FALSE(world.provinces().owner[kursk.index()].valid());
    CHECK(world.provinces_of(golitsyn).size() == 1);

    const CharacterId ivan = world.create_character(adult(golitsyn));
    const CharacterId anna = world.create_character(adult(golitsyn, Sex::Female));
    CHECK(world.is_alive(ivan));
    CHECK(world.characters().sex[anna.index()] == Sex::Female);
    CHECK(world.members_of(golitsyn).size() == 2);
    CHECK(world.members_of(menshikov).empty());

    CHECK(world.set_dynasty_head(golitsyn, ivan));
    CHECK(world.dynasties().head[golitsyn.index()] == ivan);
    require_valid(world);
}

TEST_CASE("world: character ids are reused in LIFO order and slots are reset") {
    World world = make_world();
    const DynastyId d = world.create_dynasty(StringId(0));
    const CharacterId a = world.create_character(adult(d));
    const CharacterId b = world.create_character(adult(d));
    const CharacterId c = world.create_character(adult(d));
    CHECK(a.value() == 0);
    CHECK(b.value() == 1);
    CHECK(c.value() == 2);

    CHECK(world.destroy_character(a));
    CHECK(world.destroy_character(c));
    CHECK_FALSE(world.destroy_character(c));  // already dead
    CHECK_FALSE(world.is_alive(a));

    CHECK(world.create_character(adult(d)) == c);  // most recently freed first
    CHECK(world.create_character(adult(d)) == a);
    CHECK(world.create_character(adult(d)).value() == 3);  // then a fresh slot
    require_valid(world);
}

TEST_CASE("world: indexes follow ownership and membership changes") {
    World world = make_world();
    const DynastyId a = world.create_dynasty(StringId(0));
    const DynastyId b = world.create_dynasty(StringId(1));
    const ProvinceId p = world.create_province(StringId(10), a, Fixed32::from_int(10));
    const CharacterId c = world.create_character(adult(a));

    world.set_province_owner(p, b);
    CHECK(world.provinces_of(a).empty());
    CHECK(world.provinces_of(b).size() == 1);

    world.set_province_owner(p, DynastyId());  // becomes unowned
    CHECK(world.provinces_of(b).empty());
    CHECK_FALSE(world.provinces().owner[p.index()].valid());

    world.set_character_dynasty(c, b);
    CHECK(world.members_of(a).empty());
    CHECK(world.members_of(b).size() == 1);
    require_valid(world);
}

TEST_CASE("world: a dynasty head must be alive and a member; death clears the slot") {
    World world = make_world();
    const DynastyId a = world.create_dynasty(StringId(0));
    const DynastyId b = world.create_dynasty(StringId(1));
    const CharacterId mine = world.create_character(adult(a));
    const CharacterId foreign = world.create_character(adult(b));

    CHECK_FALSE(world.set_dynasty_head(a, foreign));
    CHECK(world.set_dynasty_head(a, mine));
    CHECK_FALSE(world.set_dynasty_head(DynastyId(99), mine));

    CHECK(world.destroy_character(mine));
    CHECK_FALSE(world.dynasties().head[a.index()].valid());

    CHECK(world.set_dynasty_head(a, CharacterId()));  // clearing is always allowed
    require_valid(world);
}

TEST_CASE("world: honor is clamped") {
    World world = make_world();
    const DynastyId d = world.create_dynasty(StringId(0));
    world.add_honor(d, 500);
    CHECK(world.dynasties().honor[d.index()] == 500);
    world.add_honor(d, 1'000'000);
    CHECK(world.dynasties().honor[d.index()] == kMaxHonor);
    world.add_honor(d, -2'000'000);
    CHECK(world.dynasties().honor[d.index()] == kMinHonor);
}

TEST_CASE("world: identical operation sequences give identical hashes") {
    auto build = [] {
        World w = make_world();
        const DynastyId d = w.create_dynasty(StringId(0));
        (void)w.create_province(StringId(5), d, Fixed32::from_int(20));
        const CharacterId c1 = w.create_character(adult(d));
        const CharacterId c2 = w.create_character(adult(d));
        (void)w.destroy_character(c1);
        (void)w.set_dynasty_head(d, c2);
        (void)w.rng().stream("events").next_u32();
        return w;
    };
    const World a = build();
    const World b = build();
    CHECK(a.compute_hash() == b.compute_hash());
    CHECK(a.compute_hash().total != 0);
}

TEST_CASE("world: the hash reacts to every kind of change and locates it") {
    World base = make_world();
    const DynastyId d = base.create_dynasty(StringId(0));
    const ProvinceId p = base.create_province(StringId(5), d, Fixed32::from_int(20));
    const CharacterId c = base.create_character(adult(d));
    const WorldHash before = base.compute_hash();

    {
        World w = base;
        w.character_health_mut()[c.index()] = Fixed32::from_int(50);
        const WorldHash h = w.compute_hash();
        CHECK(h.total != before.total);
        CHECK(h.characters != before.characters);
        CHECK(h.provinces == before.provinces);
        CHECK(h.dynasties == before.dynasties);
    }
    {
        World w = base;
        w.province_population_mut()[p.index()] = Fixed32::from_int(21);
        const WorldHash h = w.compute_hash();
        CHECK(h.provinces != before.provinces);
        CHECK(h.characters == before.characters);
    }
    {
        World w = base;
        w.add_honor(d, 1);
        CHECK(w.compute_hash().dynasties != before.dynasties);
    }
    {
        World w = base;
        (void)w.advance_day();
        const WorldHash h = w.compute_hash();
        CHECK(h.calendar != before.calendar);
        CHECK(h.characters == before.characters);
    }
    {
        World w = base;
        (void)w.rng().stream("events").next_u32();
        CHECK(w.compute_hash().rng != before.rng);
    }
}

TEST_CASE("world: the free list is part of the state (destroy order matters)") {
    auto build = [](bool destroy_a_first) {
        World w = make_world();
        const DynastyId d = w.create_dynasty(StringId(0));
        const CharacterId a = w.create_character(adult(d));
        const CharacterId b = w.create_character(adult(d));
        if (destroy_a_first) {
            (void)w.destroy_character(a);
            (void)w.destroy_character(b);
        } else {
            (void)w.destroy_character(b);
            (void)w.destroy_character(a);
        }
        return w;
    };
    const World x = build(true);
    const World y = build(false);
    // Same set of live characters, but the next create_character hands out different ids.
    CHECK(x.compute_hash().characters != y.compute_hash().characters);
}

TEST_CASE("world: a snapshot is an immutable copy") {
    World world = make_world();
    const DynastyId d = world.create_dynasty(StringId(0));
    const CharacterId c = world.create_character(adult(d));
    const auto snapshot = world.snapshot();
    const WorldHash frozen = snapshot->compute_hash();

    // Mutate the live world heavily.
    for (int i = 0; i < 100; ++i) {
        (void)world.create_character(adult(d));
    }
    (void)world.destroy_character(c);
    world.add_honor(d, 77);
    (void)world.advance_day();

    CHECK(snapshot->compute_hash() == frozen);
    CHECK(snapshot->is_alive(c));
    CHECK(snapshot->today() == Date::from_ymd(1703, 1, 1));
    CHECK(world.compute_hash() != frozen);
    CHECK(snapshot->check_invariants().empty());
}

TEST_CASE("world: snapshots can be read from another thread while the world changes") {
    World world = make_world();
    const DynastyId d = world.create_dynasty(StringId(0));
    for (int i = 0; i < 50; ++i) {
        (void)world.create_province(StringId(1), d, Fixed32::from_int(10));
    }
    const auto snapshot = world.snapshot();

    std::atomic<bool> stop{false};
    std::atomic<int> torn_reads{0};
    std::thread reader([&] {
        while (!stop.load()) {
            std::int64_t sum = 0;
            for (const Fixed32 population : snapshot->provinces().population) {
                sum += population.to_int();
            }
            if (sum != 500) {
                torn_reads.fetch_add(1);
            }
        }
    });

    for (int i = 0; i < 2000; ++i) {
        for (Fixed32& population : world.province_population_mut()) {
            population = Fixed32::from_int(i % 90 + 1);
        }
        (void)world.create_character(adult(d));
    }
    stop.store(true);
    reader.join();
    CHECK(torn_reads.load() == 0);
}

TEST_CASE("world: invariants hold through thousands of random operations") {
    World world = make_world();
    Pcg32 rng(2026, 11);
    for (int i = 0; i < 6; ++i) {
        (void)world.create_dynasty(StringId(static_cast<std::uint32_t>(i)));
    }
    // One of 7 outcomes is "no dynasty" (unowned province / character without a dynasty).
    const auto pick_dynasty = [&rng] {
        const std::uint32_t v = rng.bounded(7);
        return v == 6 ? DynastyId() : DynastyId(v);
    };
    for (int i = 0; i < 40; ++i) {
        (void)world.create_province(StringId(100), pick_dynasty(), Fixed32::from_int(10 + i));
    }

    std::vector<CharacterId> live;
    for (int step = 0; step < 6000; ++step) {
        const std::uint32_t op = rng.bounded(100);
        if (op < 35 || live.empty()) {
            live.push_back(world.create_character(adult(pick_dynasty())));
        } else if (op < 55) {
            const std::size_t k = rng.bounded(static_cast<std::uint32_t>(live.size()));
            REQUIRE(world.destroy_character(live[k]));
            live[k] = live.back();
            live.pop_back();
        } else if (op < 70) {
            world.set_character_dynasty(live[rng.bounded(static_cast<std::uint32_t>(live.size()))],
                                        pick_dynasty());
        } else if (op < 85) {
            world.set_province_owner(ProvinceId(rng.bounded(40)), pick_dynasty());
        } else if (op < 95) {
            const DynastyId d(rng.bounded(6));
            const auto members = world.members_of(d);
            if (!members.empty()) {
                REQUIRE(world.set_dynasty_head(
                    d, members[rng.bounded(static_cast<std::uint32_t>(members.size()))]));
            }
        } else {
            world.add_honor(DynastyId(rng.bounded(6)), static_cast<std::int32_t>(rng.range(-3000, 3000)));
        }
        if (step % 50 == 0) {
            require_valid(world);
        }
    }
    require_valid(world);
    CHECK(world.characters().ids.alive_count() == live.size());
}
