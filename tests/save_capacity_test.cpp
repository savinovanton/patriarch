// Capacity of the file formats (E13-T03, docs/dev/save-format.md sections 5.1 and 6.1): the limit of
// 96 sections in the extension block of the world state and the varint command tag (u16 in memory,
// one byte for tags 1..127 and two bytes for 128..16383 on the wire).

#include "pat/core/bytes.hpp"
#include "pat/sim/command_log.hpp"
#include "pat/sim/save.hpp"
#include "pat/sim/state_test_hooks.hpp"
#include "pat/sim/world.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

using namespace pat::sim;
using pat::core::ByteReader;
using pat::core::ByteWriter;

namespace {

constexpr std::uint16_t kFirstTestSection = 41;  // the real sections end at 40

std::vector<std::uint8_t> tag_bytes(std::uint16_t tag) {
    ByteWriter w;
    write_command_tag(w, tag);
    return w.take();
}

bool read_tag(const std::vector<std::uint8_t>& bytes, std::uint16_t& tag) {
    ByteReader r(bytes);
    return read_command_tag(r, tag) && r.at_end();
}

World small_world() {
    World world(Date::from_ymd(1703, 1, 1), 11);
    for (int i = 0; i < 5; ++i) {
        (void)world.advance_day();
    }
    return world;
}

std::vector<std::uint8_t> state_bytes(const World& world) {
    ByteWriter out;
    world.save_state(out);
    return out.take();
}

pat::core::Result<World, pat::core::Error> load(std::span<const std::uint8_t> bytes) {
    ByteReader in(bytes);
    return World::load_state(in);
}

/// The section count written at the end of the state of a world without real sections whose test
/// sections are `count` entries of 12 bytes each (id, version, length, one u32).
std::uint16_t trailing_section_count(const std::vector<std::uint8_t>& bytes, std::size_t count) {
    const std::size_t at = bytes.size() - count * 12 - 2;
    return static_cast<std::uint16_t>(bytes[at] | (bytes[at + 1] << 8));
}

}  // namespace

TEST_CASE("command tag: ids below 128 take one byte, the same byte as the old u8 tag") {
    for (const std::uint16_t tag : std::vector<std::uint16_t>{1, 2, 39, 64, 96, 127}) {
        const std::vector<std::uint8_t> bytes = tag_bytes(tag);
        REQUIRE(bytes.size() == 1);
        CHECK(bytes[0] == tag);
        std::uint16_t decoded = 0;
        CHECK(read_tag(bytes, decoded));
        CHECK(decoded == tag);
    }
}

TEST_CASE("command tag: ids from 128 take two bytes (LEB128), boundaries 127 / 128 / 16383") {
    CHECK(tag_bytes(127) == std::vector<std::uint8_t>{0x7F});
    CHECK(tag_bytes(128) == std::vector<std::uint8_t>{0x80, 0x01});
    CHECK(tag_bytes(129) == std::vector<std::uint8_t>{0x81, 0x01});
    CHECK(tag_bytes(255) == std::vector<std::uint8_t>{0xFF, 0x01});
    CHECK(tag_bytes(256) == std::vector<std::uint8_t>{0x80, 0x02});
    CHECK(tag_bytes(kMaxCommandTag) == std::vector<std::uint8_t>{0xFF, 0x7F});
    CHECK(kMaxCommandTag == 16383);

    // every tag the format can carry: canonical length, and encode -> decode -> the same tag
    for (std::uint16_t tag = 1; tag <= kMaxCommandTag; ++tag) {
        const std::vector<std::uint8_t> bytes = tag_bytes(tag);
        REQUIRE(bytes.size() == (tag < 128 ? 1U : 2U));
        std::uint16_t decoded = 0;
        REQUIRE(read_tag(bytes, decoded));
        REQUIRE(decoded == tag);
    }
}

TEST_CASE("command tag: non-canonical, empty, truncated and too large spellings are rejected") {
    std::uint16_t tag = 0;
    CHECK_FALSE(read_tag({}, tag));                        // nothing there
    CHECK_FALSE(read_tag({0x00}, tag));                    // tag 0 is not a tag
    CHECK_FALSE(read_tag({0x80}, tag));                    // cut after the first byte
    CHECK_FALSE(read_tag({0x81, 0x00}, tag));              // overlong spelling of 1
    CHECK_FALSE(read_tag({0x80, 0x00}, tag));              // overlong spelling of 0
    CHECK_FALSE(read_tag({0xFF, 0x00}, tag));              // overlong spelling of 127
    CHECK_FALSE(read_tag({0x80, 0x80, 0x01}, tag));        // 16384: a third byte (overflow)
    CHECK_FALSE(read_tag({0xFF, 0xFF, 0x7F}, tag));        // far above the limit
    CHECK_FALSE(read_tag({0xFF, 0xFF, 0xFF, 0xFF}, tag));  // endless continuation
    CHECK(read_tag({0xFF, 0x7F}, tag));                    // the largest tag is fine
    CHECK(tag == 16383);
}

TEST_CASE("command: tags of existing commands keep their one-byte wire form; bad tags fail cleanly") {
    const std::vector<Command> commands = {
        Command{DynastyId(1), cmd::SetProvinceOwner{ProvinceId(5), DynastyId(2)}},
        Command{DynastyId(), cmd::AdjustHonor{DynastyId(4), -250}},
        Command{DynastyId(2), cmd::TakeLoan{}},
    };
    for (const Command& command : commands) {
        ByteWriter w;
        write_command(w, command);
        REQUIRE(w.size() > 1);
        CHECK(w.data()[0] == static_cast<std::uint8_t>(type_of(command)));  // the byte of the old format
        ByteReader r(w.data());
        Command decoded;
        REQUIRE(read_command(r, decoded));
        CHECK(r.at_end());
        CHECK(decoded == command);
    }

    // AdjustHonor (tag 5): the exact bytes the u8 format wrote (tag, actor, dynasty, delta)
    ByteWriter honor;
    write_command(honor, commands[1]);
    CHECK(honor.data() == std::vector<std::uint8_t>{
                              0x05, 0xFF, 0xFF, 0xFF, 0xFF, 0x04, 0x00, 0x00, 0x00, 0x06, 0xFF, 0xFF, 0xFF});

    // no command lives at these tags yet: a two-byte tag is "unknown", a non-canonical one is
    // malformed; neither crashes or reads on
    for (const std::vector<std::uint8_t>& head : {std::vector<std::uint8_t>{0x80, 0x01},
                                                  std::vector<std::uint8_t>{0xFF, 0x7F},
                                                  std::vector<std::uint8_t>{0x81, 0x00},
                                                  std::vector<std::uint8_t>{0x80, 0x80, 0x01}}) {
        ByteWriter w;
        w.bytes(head);
        for (int i = 0; i < 32; ++i) {
            w.u8(0);
        }
        ByteReader r(w.data());
        Command decoded;
        CHECK_FALSE(read_command(r, decoded));
    }
}

TEST_CASE("sections: a world with 40 extension sections saves and loads") {
    const World world = small_world();
    const std::vector<std::uint8_t> plain = state_bytes(world);
    std::vector<std::uint8_t> bytes;
    {
        const testing::ScopedTestSections sections({kFirstTestSection, 40});
        bytes = state_bytes(world);
        CHECK(trailing_section_count(bytes, 40) == 40);
        CHECK(bytes.size() == plain.size() + 40 * 12);

        auto loaded = load(bytes);
        REQUIRE(loaded.has_value());
        CHECK(loaded.value().compute_hash() == world.compute_hash());
        CHECK(state_bytes(loaded.value()) == bytes);  // 40 sections out again, byte for byte
        CHECK(loaded.value().check_invariants().empty());

        // damaged content of a test section
        std::vector<std::uint8_t> damaged = bytes;
        damaged[damaged.size() - 1] ^= 0x01U;
        CHECK_FALSE(load(damaged).has_value());
    }
    // the hook is off again: nothing is added, and a build without these sections refuses the file
    CHECK(state_bytes(world) == plain);
    const auto without_hook = load(bytes);
    REQUIRE_FALSE(without_hook.has_value());
    CHECK(without_hook.error().message.find("section") != std::string::npos);
}

TEST_CASE("sections: 96 are the most a file may hold, 97 are an error and not a crash") {
    const World world = small_world();
    {
        const testing::ScopedTestSections sections({kFirstTestSection, 96});
        const std::vector<std::uint8_t> bytes = state_bytes(world);
        CHECK(trailing_section_count(bytes, 96) == 96);
        auto loaded = load(bytes);
        REQUIRE(loaded.has_value());
        CHECK(loaded.value().compute_hash() == world.compute_hash());
    }
    {
        const testing::ScopedTestSections sections({kFirstTestSection, 97});
        const std::vector<std::uint8_t> bytes = state_bytes(world);
        CHECK(trailing_section_count(bytes, 97) == 97);
        const auto loaded = load(bytes);
        REQUIRE_FALSE(loaded.has_value());
        CHECK(loaded.error().message.find("invalid section count") != std::string::npos);
    }
    {
        // a count the rest of the file cannot hold (8 bytes of header per section) is damage too
        std::vector<std::uint8_t> bytes = state_bytes(world);
        bytes[bytes.size() - 2] = 96;
        bytes[bytes.size() - 1] = 0;
        const auto loaded = load(bytes);
        REQUIRE_FALSE(loaded.has_value());
        CHECK(loaded.error().message.find("invalid section count") != std::string::npos);
    }
}

TEST_CASE("fixtures: every saved world of an earlier build still reads") {
    const std::filesystem::path dir = std::filesystem::path(PATRIARCH_TEST_DATA_DIR) / "saves";
    int checked = 0;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        if (entry.path().extension() != ".psv") {
            continue;
        }
        const auto file = read_file(entry.path());
        INFO(entry.path().filename().string());
        REQUIRE(file.has_value());
        const auto loaded = deserialize_save(file.value());
        REQUIRE(loaded.has_value());
        CHECK(loaded.value().world.check_invariants().empty());
        ++checked;
    }
    CHECK(checked >= 7);
}
