#include "pat/core/hash_stream.hpp"
#include "pat/sim/command_log.hpp"

#include <doctest/doctest.h>

#include <string>
#include <vector>

using namespace pat::sim;
using pat::core::ByteReader;
using pat::core::ByteWriter;

namespace {

std::vector<Command> sample_commands() {
    cmd::CreateCharacter child;
    child.dynasty = DynastyId(3);
    child.birth_date = Date::from_ymd(1702, 12, 31);
    child.sex = Sex::Female;
    child.father = CharacterId(7);
    child.mother = CharacterId();
    return {
        Command{DynastyId(1), cmd::SetProvinceOwner{ProvinceId(5), DynastyId(2)}},
        Command{DynastyId(3), child},
        Command{DynastyId(), cmd::KillCharacter{CharacterId(9)}},
        Command{DynastyId(2), cmd::SetDynastyHead{DynastyId(2), CharacterId(11)}},
        Command{DynastyId(), cmd::AdjustHonor{DynastyId(4), -250}},
    };
}

CommandLog sample_log() {
    CommandLog log;
    Date day = Date::from_ymd(1703, 1, 1);
    int i = 0;
    for (const Command& c : sample_commands()) {
        LoggedCommand e;
        e.date = day + i;
        e.source = (i % 2 == 0) ? CommandSource::External : CommandSource::Internal;
        e.outcome = (i == 3) ? Rejection::NotAuthorized : Rejection::None;
        e.command = c;
        log.append(e);
        ++i;
    }
    return log;
}

}  // namespace

TEST_CASE("byte io: round trip of every width") {
    ByteWriter w;
    w.u8(0xAB);
    w.u16(0xBEEF);
    w.u32(0xDEADBEEFU);
    w.u64(0x0123456789ABCDEFULL);
    w.i32(-123456);
    w.i64(-9876543210LL);
    w.boolean(true);
    const std::uint8_t raw[3] = {1, 2, 3};
    w.bytes(raw);

    // Little-endian layout is part of the file formats.
    CHECK(w.data()[1] == 0xEF);
    CHECK(w.data()[2] == 0xBE);

    ByteReader r(w.data());
    CHECK(r.u8() == 0xAB);
    CHECK(r.u16() == 0xBEEF);
    CHECK(r.u32() == 0xDEADBEEFU);
    CHECK(r.u64() == 0x0123456789ABCDEFULL);
    CHECK(r.i32() == -123456);
    CHECK(r.i64() == -9876543210LL);
    CHECK(r.boolean());
    const auto tail = r.bytes(3);
    CHECK(tail.size() == 3);
    CHECK(tail[2] == 3);
    CHECK(r.ok());
    CHECK(r.at_end());
}

TEST_CASE("byte io: reading past the end fails safely") {
    ByteWriter w;
    w.u16(7);
    ByteReader r(w.data());
    CHECK(r.u32() == 0);  // not enough bytes
    CHECK_FALSE(r.ok());
    CHECK(r.u8() == 0);  // stays failed
    CHECK(r.bytes(1).empty());
}

TEST_CASE("command log: single commands round trip") {
    for (const Command& original : sample_commands()) {
        ByteWriter w;
        write_command(w, original);
        ByteReader r(w.data());
        Command decoded;
        REQUIRE(read_command(r, decoded));
        CHECK(r.at_end());
        CHECK(decoded == original);
    }
}

TEST_CASE("command log: unknown tags and truncated commands are rejected") {
    {
        ByteWriter w;
        w.u8(99);  // no such command
        w.u32(0);
        ByteReader r(w.data());
        Command decoded;
        CHECK_FALSE(read_command(r, decoded));
    }
    {
        ByteWriter w;
        write_command(w, sample_commands()[1]);
        std::vector<std::uint8_t> bytes = w.data();
        bytes.pop_back();
        ByteReader r(bytes);
        Command decoded;
        CHECK_FALSE(read_command(r, decoded));
    }
    {
        ByteWriter w;  // invalid sex value
        w.u8(static_cast<std::uint8_t>(CommandType::CreateCharacter));
        w.u32(0);
        w.u32(0);
        w.i32(0);
        w.u8(5);
        w.u32(0);
        w.u32(0);
        ByteReader r(w.data());
        Command decoded;
        CHECK_FALSE(read_command(r, decoded));
    }
}

TEST_CASE("command log: serialization round trip preserves everything") {
    const CommandLog original = sample_log();
    const std::vector<std::uint8_t> bytes = original.serialize();

    const auto decoded = CommandLog::deserialize(bytes);
    REQUIRE(decoded.has_value());
    CHECK(decoded.value() == original);
    CHECK(decoded.value().serialize() == bytes);  // canonical form

    const CommandLog empty;
    const auto empty_decoded = CommandLog::deserialize(empty.serialize());
    REQUIRE(empty_decoded.has_value());
    CHECK(empty_decoded.value().size() == 0);
}

TEST_CASE("command log: damaged input is detected") {
    const std::vector<std::uint8_t> good = sample_log().serialize();

    // Any single flipped bit is caught by the checksum.
    for (std::size_t i = 0; i < good.size(); i += 3) {
        std::vector<std::uint8_t> bad = good;
        bad[i] ^= 0x10;
        CHECK_FALSE(CommandLog::deserialize(bad).has_value());
    }
    // Truncation anywhere is caught.
    for (std::size_t n = 0; n + 1 < good.size(); n += 5) {
        CHECK_FALSE(CommandLog::deserialize(std::span<const std::uint8_t>(good.data(), n)).has_value());
    }
    CHECK_FALSE(CommandLog::deserialize({}).has_value());
}

TEST_CASE("command log: wrong magic or version is reported with a clear error") {
    // Rebuild a stream with a valid checksum but wrong header fields.
    auto with_header = [](std::uint8_t magic0, std::uint16_t version) {
        ByteWriter w;
        w.u8(magic0);
        w.u8('C');
        w.u8('L');
        w.u8('1');
        w.u16(version);
        w.u32(0);
        pat::core::HashStream h;
        h.bytes(w.data());
        w.u64(h.value());
        return w.take();
    };
    const auto bad_magic = CommandLog::deserialize(with_header('X', CommandLog::kFormatVersion));
    REQUIRE_FALSE(bad_magic.has_value());
    CHECK(bad_magic.error().message.find("magic") != std::string::npos);

    const auto bad_version = CommandLog::deserialize(with_header('P', 999));
    REQUIRE_FALSE(bad_version.has_value());
    CHECK(bad_version.error().message.find("version") != std::string::npos);
}
