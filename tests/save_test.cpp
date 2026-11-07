#include "pat/core/bytes.hpp"
#include "pat/core/compress.hpp"
#include "pat/core/rng.hpp"
#include "pat/sim/demo_scenario.hpp"
#include "pat/sim/save.hpp"

#include <doctest/doctest.h>

#include <filesystem>
#include <vector>

using namespace pat::sim;
using pat::core::ByteReader;
using pat::core::ByteWriter;
using pat::core::Pcg32;

namespace {

std::filesystem::path temp_path(const char* name) {
    return std::filesystem::temp_directory_path() / name;
}

/// A world with a messy history: dead slots, reused ids, swap-removed index entries, RNG streams.
World messy_world() {
    World world(Date::from_ymd(1703, 1, 1), 99);
    Pcg32 rng(5, 5);
    for (int i = 0; i < 5; ++i) {
        (void)world.create_dynasty(pat::core::StringId(static_cast<std::uint32_t>(i)));
    }
    for (int i = 0; i < 30; ++i) {
        (void)world.create_province(
            pat::core::StringId(100), DynastyId(rng.bounded(5)), pat::core::Fixed32::from_int(10 + i));
    }
    std::vector<CharacterId> live;
    for (int step = 0; step < 600; ++step) {
        if (live.empty() || rng.chance_permille(600)) {
            CharacterInit init;
            init.dynasty = DynastyId(rng.bounded(5));
            init.birth_date = Date::from_ymd(1650, 1, 1) + rng.range(0, 10000);
            init.sex = rng.chance_permille(500) ? Sex::Male : Sex::Female;
            init.health = pat::core::Fixed32::from_int(rng.range(1, 100));
            if (!live.empty() && rng.chance_permille(300)) {
                init.father = live[rng.bounded(static_cast<std::uint32_t>(live.size()))];
            }
            live.push_back(world.create_character(init));
        } else if (rng.chance_permille(500)) {
            const std::size_t k = rng.bounded(static_cast<std::uint32_t>(live.size()));
            (void)world.destroy_character(live[k]);
            live[k] = live.back();
            live.pop_back();
        } else {
            world.set_province_owner(ProvinceId(rng.bounded(30)), DynastyId(rng.bounded(5)));
        }
    }
    (void)world.rng().stream("events").next_u32();
    (void)world.rng().stream("ai.dynasty", 3).next_u32();
    for (int i = 0; i < 40; ++i) {
        (void)world.advance_day();
    }
    return world;
}

}  // namespace

TEST_CASE("compress: round trip and corruption handling") {
    std::vector<std::uint8_t> data(100000);
    for (std::size_t i = 0; i < data.size(); ++i) {
        data[i] = static_cast<std::uint8_t>((i * 7) % 251);
    }
    const auto packed = pat::core::compress(data);
    CHECK(packed.size() < data.size() / 4);  // regular data compresses well

    const auto unpacked = pat::core::decompress(packed, data.size());
    REQUIRE(unpacked.has_value());
    CHECK(unpacked.value() == data);

    CHECK_FALSE(pat::core::decompress(packed, data.size() + 1).has_value());  // wrong expected size
    auto damaged = packed;
    damaged[damaged.size() / 2] ^= 0xFF;
    // Either decoding fails or the content differs; it must never crash.
    const auto result = pat::core::decompress(damaged, data.size());
    CHECK((!result.has_value() || result.value() != data));
    CHECK_FALSE(pat::core::decompress({}, 10).has_value());

    const auto empty = pat::core::decompress(pat::core::compress({}), 0);
    REQUIRE(empty.has_value());
    CHECK(empty.value().empty());
}

TEST_CASE("save: world state round trip keeps the hash, the indexes and the future") {
    const World original = messy_world();
    REQUIRE(original.check_invariants().empty());

    ByteWriter out;
    original.save_state(out);
    ByteReader in(out.data());
    auto loaded = World::load_state(in);
    REQUIRE(loaded.has_value());
    CHECK(in.at_end());

    World copy = std::move(loaded).value();
    CHECK(copy.compute_hash() == original.compute_hash());
    CHECK(copy.check_invariants().empty());

    // Behaviour after loading is identical: the same operations produce the same ids and states.
    World a = original;
    for (World* w : {&a, &copy}) {
        for (int i = 0; i < 20; ++i) {
            CharacterInit init;
            init.dynasty = DynastyId(static_cast<std::uint32_t>(i % 5));
            init.birth_date = Date::from_ymd(1700, 1, 1);
            (void)w->create_character(init);
        }
        w->set_province_owner(ProvinceId(3), DynastyId(1));
        (void)w->rng().stream("events").next_u32();
        (void)w->advance_day();
    }
    CHECK(copy.compute_hash() == a.compute_hash());
}

TEST_CASE("save: the container round trips, is deterministic and stable under re-saving") {
    const World world = messy_world();
    const std::vector<Command> pending = {
        Command{DynastyId(), cmd::AdjustHonor{DynastyId(1), 40}},
        Command{DynastyId(2), cmd::SetDynastyHead{DynastyId(2), CharacterId()}},
    };

    const auto bytes = serialize_save(world, pending);
    CHECK(bytes == serialize_save(world, pending));  // deterministic

    auto loaded = deserialize_save(bytes);
    REQUIRE(loaded.has_value());
    CHECK(loaded.value().world.compute_hash() == world.compute_hash());
    CHECK(loaded.value().pending_internal == pending);
    CHECK(serialize_save(loaded.value().world, loaded.value().pending_internal) == bytes);
}

TEST_CASE("save: damaged files are rejected with an error, never a crash") {
    const World original = messy_world();
    const auto good = serialize_save(original);

    for (std::size_t i = 0; i < good.size(); i += 7) {
        auto bad = good;
        bad[i] ^= 0x5A;
        CAPTURE(i);
        // A flipped bit inside the zstd stream that does not change the decoded bytes (an unused
        // bit of the entropy coder) is harmless: the payload hash still matches. Anything else
        // must be rejected.
        const auto result = deserialize_save(bad);
        if (result.has_value()) {
            CHECK(result.value().world.compute_hash() == original.compute_hash());
        }
    }
    for (std::size_t n = 0; n < good.size(); n += 11) {
        CHECK_FALSE(deserialize_save(std::span<const std::uint8_t>(good.data(), n)).has_value());
    }
    CHECK_FALSE(deserialize_save({}).has_value());
}

TEST_CASE("save: unknown versions and foreign files get clear messages") {
    auto bytes = serialize_save(messy_world());

    auto newer = bytes;
    newer[4] = static_cast<std::uint8_t>(kSaveFormatVersion + 1);  // version field, little endian
    const auto newer_result = deserialize_save(newer);
    REQUIRE_FALSE(newer_result.has_value());
    CHECK(newer_result.error().message.find("newer") != std::string::npos);

    auto foreign = bytes;
    foreign[0] = 'X';
    const auto foreign_result = deserialize_save(foreign);
    REQUIRE_FALSE(foreign_result.has_value());
    CHECK(foreign_result.error().message.find("magic") != std::string::npos);
}

TEST_CASE("save: random corruption of the raw world payload is handled safely") {
    // Bypass the container checksum and attack World::load_state directly.
    ByteWriter out;
    messy_world().save_state(out);
    const std::vector<std::uint8_t> payload = out.data();

    Pcg32 rng(77, 7);
    int rejected = 0;
    for (int trial = 0; trial < 1500; ++trial) {
        auto mutated = payload;
        const int flips = 1 + static_cast<int>(rng.bounded(3));
        for (int f = 0; f < flips; ++f) {
            mutated[rng.bounded(static_cast<std::uint32_t>(mutated.size()))] =
                static_cast<std::uint8_t>(rng.next_u32());
        }
        ByteReader in(mutated);
        auto result = World::load_state(in);
        if (result.has_value()) {
            // A surviving mutation must still describe a consistent world.
            CHECK(result.value().check_invariants().empty());
        } else {
            ++rejected;
        }
    }
    CHECK(rejected > 500);  // most random damage is detected

    // Truncations at every length.
    for (std::size_t n = 0; n < payload.size(); n += 13) {
        ByteReader in(std::span<const std::uint8_t>(payload.data(), n));
        CHECK_FALSE(World::load_state(in).has_value());
    }
}

TEST_CASE("save: a loaded simulation continues exactly like the original") {
    pat::sim::demo::Config config;
    config.seed = 321;

    Simulation original = demo::make_simulation(config);
    original.run_days(90);  // 1 April 1703: the quarterly rewards have just been emitted
    REQUIRE_FALSE(original.pending_internal().empty());

    const auto bytes = serialize_save(original.world(), original.pending_internal());

    Simulation resumed = demo::make_simulation(config);  // same systems
    auto loaded = deserialize_save(bytes);
    REQUIRE(loaded.has_value());
    resumed.restore_state(std::move(loaded.value().world), std::move(loaded.value().pending_internal));

    CHECK(resumed.world().compute_hash() == original.world().compute_hash());
    original.run_days(365);
    resumed.run_days(365);
    CHECK(resumed.world().compute_hash() == original.world().compute_hash());
    CHECK(resumed.world().today() == original.world().today());
}

TEST_CASE("save: files are written atomically and read back") {
    const auto path = temp_path("patriarch_save_atomic.bin");
    std::filesystem::remove(path);

    const std::vector<std::uint8_t> first = {1, 2, 3, 4};
    REQUIRE(write_file_atomic(path, first).has_value());
    CHECK(read_file(path).value() == first);

    const std::vector<std::uint8_t> second(5000, 7);
    REQUIRE(write_file_atomic(path, second).has_value());  // replaces the existing file
    CHECK(read_file(path).value() == second);

    auto temp = path;
    temp += ".tmp";
    CHECK_FALSE(std::filesystem::exists(temp));  // no leftovers

    CHECK_FALSE(read_file(temp_path("patriarch_does_not_exist.bin")).has_value());
    CHECK_FALSE(write_file_atomic(temp_path("no_such_dir_xyz/file.bin"), first).has_value());

    std::filesystem::remove(path);
}

TEST_CASE("save: saving in the background does not disturb or depend on the running simulation") {
    pat::sim::demo::Config config;
    config.seed = 11;
    Simulation sim = demo::make_simulation(config);
    sim.run_days(120);

    const auto snapshot = sim.snapshot();
    const WorldHash at_save_time = snapshot->compute_hash();
    const auto path = temp_path("patriarch_save_async.bin");
    std::filesystem::remove(path);

    auto pending = std::vector<Command>(sim.pending_internal().begin(), sim.pending_internal().end());
    auto outcome = save_async(snapshot, std::move(pending), path);
    sim.run_days(200);  // the simulation moves on while the file is being written

    REQUIRE(outcome.get().has_value());
    auto bytes = read_file(path);
    REQUIRE(bytes.has_value());
    auto loaded = deserialize_save(bytes.value());
    REQUIRE(loaded.has_value());
    CHECK(loaded.value().world.compute_hash() == at_save_time);
    CHECK(sim.world().compute_hash() != at_save_time);

    std::filesystem::remove(path);
}
