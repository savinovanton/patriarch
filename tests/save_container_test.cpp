// Save container version 2 (E13-T02, docs/dev/save-format.md section 2): the summary between the header
// and the zstd frame, the reader of the legacy version 1, and the behaviour of an older build.

#include "pat/core/build_info.hpp"
#include "pat/core/bytes.hpp"
#include "pat/core/hash_stream.hpp"
#include "pat/sim/demo_scenario.hpp"
#include "pat/sim/save.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <utility>
#include <vector>

using namespace pat::sim;
using pat::core::ByteReader;
using pat::core::ByteWriter;

namespace {

// Offsets inside the version 2 header.
constexpr std::size_t kOffVersion = 4;
constexpr std::size_t kOffFlags = 6;
constexpr std::size_t kOffSummarySize = 8;
constexpr std::size_t kOffSummaryHash = 12;
constexpr std::size_t kOffCompressedSize = 36;

std::filesystem::path fixture(const char* name) {
    return std::filesystem::path(PATRIARCH_TEST_DATA_DIR) / "saves" / name;
}

std::filesystem::path temp_path(const char* name) {
    return std::filesystem::temp_directory_path() / name;
}

std::uint64_t fnv(std::span<const std::uint8_t> bytes) {
    pat::core::HashStream h;
    h.bytes(bytes);
    return h.value();
}

std::uint32_t read_u32(const std::vector<std::uint8_t>& b, std::size_t off) {
    ByteReader r(std::span<const std::uint8_t>(b).subspan(off));
    return r.u32();
}

void put_u32(std::vector<std::uint8_t>& b, std::size_t off, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        b[off + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>((v >> (8 * i)) & 0xFFU);
    }
}

void put_u64(std::vector<std::uint8_t>& b, std::size_t off, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        b[off + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>((v >> (8 * i)) & 0xFFU);
    }
}

/// A world a few months into the demo scenario, with pending internal commands.
Simulation demo_sim() {
    pat::sim::demo::Config config;
    config.seed = 321;
    Simulation sim = demo::make_simulation(config);
    sim.run_days(90);
    return sim;
}

SaveSummary full_summary() {
    SaveSummary s;
    s.game_version = "9.8.7-test";
    s.scenario = "m1";
    s.content_hash = 0x1122334455667788ULL;
    s.player_dynasty = 3;
    s.dynasty_key = "golitsyn";
    s.head_name = "Иван Голицын";
    s.head_age = 41;
    s.date = Date::from_ymd(1708, 3, 4).days();
    s.treasury = -125000;
    s.goal_key = "goal.rise";
    s.goal_progress_ppm = 250000;
    s.play_time_s = 3 * 3600 + 25;
    s.kind = SaveKind::AutoExit;
    s.name = "Перед заговором 1708";
    s.saved_at = 1'800'000'000;
    s.world_hash = 0xDEADBEEFCAFEF00DULL;
    s.tick = 1824;
    s.ironman = 1;
    s.preview = {1, 2, 3, 4, 5};
    return s;
}

struct Field {
    std::uint16_t key;
    std::vector<std::uint8_t> value;
};

Field u32_field(std::uint16_t key, std::uint32_t v) {
    ByteWriter w;
    w.u32(v);
    return {key, w.take()};
}
Field u64_field(std::uint16_t key, std::uint64_t v) {
    ByteWriter w;
    w.u64(v);
    return {key, w.take()};
}
Field u8_field(std::uint16_t key, std::uint8_t v) {
    return {key, {v}};
}
Field text_field(std::uint16_t key, const std::string& s) {
    return {key, std::vector<std::uint8_t>(s.begin(), s.end())};
}

/// The required fields of a summary, ascending by key (a base for crafting damaged summaries).
std::vector<Field> required_fields() {
    return {text_field(1, "t"),
            u32_field(2, World::kStateSchema),
            text_field(3, "m1"),
            u64_field(4, 0),
            u32_field(5, kNoPlayerDynasty),
            u32_field(9, 0),
            u64_field(13, 0),
            u8_field(14, 0),
            u64_field(16, 0),
            u64_field(17, 0),
            u64_field(18, 0)};
}

std::vector<std::uint8_t> encode_fields(const std::vector<Field>& fields) {
    ByteWriter w;
    for (const Field& f : fields) {
        w.u16(f.key);
        w.u16(static_cast<std::uint16_t>(f.value.size()));
        w.bytes(f.value);
    }
    return w.take();
}

/// Replaces the summary of a version 2 file by `summary` (a valid hash and sizes are written for it).
std::vector<std::uint8_t> with_summary(const std::vector<std::uint8_t>& good,
                                       const std::vector<std::uint8_t>& summary,
                                       bool fix_hash = true) {
    const std::size_t old_size = read_u32(good, kOffSummarySize);
    std::vector<std::uint8_t> out(good.begin(), good.begin() + static_cast<std::ptrdiff_t>(kSaveHeaderSize));
    put_u32(out, kOffSummarySize, static_cast<std::uint32_t>(summary.size()));
    put_u64(out, kOffSummaryHash, fix_hash ? fnv(summary) : 0);
    out.insert(out.end(), summary.begin(), summary.end());
    out.insert(out.end(), good.begin() + static_cast<std::ptrdiff_t>(kSaveHeaderSize + old_size), good.end());
    return out;
}

int error_code(const pat::core::Result<SaveInfo, pat::core::Error>& r) {
    REQUIRE_FALSE(r.has_value());
    return r.error().code;
}

/// A byte-exact model of the header gate of the build before this task (container version 1), which is
/// all an older build does with a newer file before it stops. Returns the error code or 0 if the
/// build would go on to decompress.
int old_build_header_gate(const std::vector<std::uint8_t>& bytes) {
    constexpr std::size_t kOldHeaderSize = 4 + 2 + 2 + 8 + 8 + 8;
    constexpr std::uint16_t kOldVersion = 1;
    if (bytes.size() < kOldHeaderSize) {
        return 20;
    }
    ByteReader header(bytes);
    const auto magic = header.bytes(4);
    if (!std::equal(magic.begin(), magic.end(), "PSV1")) {
        return 20;
    }
    const std::uint16_t version = header.u16();
    if (header.u16() != 0) {
        return 20;
    }
    (void)header.u64();
    (void)header.u64();
    const std::uint64_t compressed_size = header.u64();
    if (version == 0 || version > kOldVersion) {
        return 22;
    }
    if (compressed_size != header.remaining()) {
        return 20;
    }
    return 0;
}

}  // namespace

TEST_CASE("container v2: the layout is the one in docs/dev/save-format.md") {
    Simulation sim = demo_sim();
    const auto bytes = serialize_save(sim.world(), sim.pending_internal(), full_summary());
    REQUIRE(bytes.size() > kSaveHeaderSize);

    CHECK(std::string(bytes.begin(), bytes.begin() + 4) == "PSV1");  // the magic never changes
    CHECK(kSaveFormatVersion == 2);
    ByteReader r(bytes);
    (void)r.bytes(4);
    CHECK(r.u16() == 2);
    CHECK(r.u16() == 0);
    const std::uint32_t summary_size = r.u32();
    CHECK(summary_size > 0);
    CHECK(summary_size <= kMaxSaveSummary);
    const std::uint64_t summary_hash = r.u64();
    (void)r.u64();  // uncompressed size
    (void)r.u64();  // payload hash
    const std::uint64_t compressed_size = r.u64();
    CHECK(compressed_size == bytes.size() - kSaveHeaderSize - summary_size);
    CHECK(summary_hash == fnv(std::span<const std::uint8_t>(bytes).subspan(kSaveHeaderSize, summary_size)));
    // The summary is plain: the first field is key 1 (game_version), little endian.
    CHECK(bytes[kSaveHeaderSize] == 1);
    CHECK(bytes[kSaveHeaderSize + 1] == 0);
}

TEST_CASE("container v2: every summary field round trips, in memory and through a file") {
    Simulation sim = demo_sim();
    const SaveSummary summary = full_summary();
    const auto bytes = serialize_save(sim.world(), sim.pending_internal(), summary);

    auto loaded = deserialize_save(bytes);
    REQUIRE(loaded.has_value());
    CHECK(loaded.value().format_version == 2);
    CHECK(loaded.value().summary == summary);
    CHECK(loaded.value().world.compute_hash() == sim.world().compute_hash());
    CHECK(loaded.value().pending_internal ==
          std::vector<Command>(sim.pending_internal().begin(), sim.pending_internal().end()));

    const auto info = read_save_summary(bytes);
    REQUIRE(info.has_value());
    CHECK(info.value().format_version == 2);
    REQUIRE(info.value().summary.has_value());
    CHECK(*info.value().summary == summary);
    CHECK(info.value().uncompressed_size > 0);
    CHECK(info.value().compressed_size > 0);

    const auto path = temp_path("patriarch_save_container_v2.psv");
    REQUIRE(write_file_atomic(path, bytes).has_value());
    const auto from_file = read_save_summary(path);
    REQUIRE(from_file.has_value());
    REQUIRE(from_file.value().summary.has_value());
    CHECK(*from_file.value().summary == summary);
    std::filesystem::remove(path);

    // Re-saving a loaded game with the same summary gives the same bytes.
    CHECK(serialize_save(loaded.value().world, loaded.value().pending_internal, loaded.value().summary) ==
          bytes);
}

TEST_CASE(
    "container v2: optional fields are absent when empty, and the minimum summary comes from the world") {
    Simulation sim = demo_sim();
    const auto bytes = serialize_save(sim.world(), sim.pending_internal());
    CHECK(bytes == serialize_save(sim.world(), sim.pending_internal()));  // no clock is read

    const auto info = read_save_summary(bytes);
    REQUIRE(info.has_value());
    REQUIRE(info.value().summary.has_value());
    const SaveSummary& s = *info.value().summary;
    CHECK(s == summary_from_world(sim.world()));
    CHECK(s.state_schema == World::kStateSchema);
    CHECK(s.date == sim.world().today().days());
    CHECK(s.tick == sim.world().calendar().tick_count());
    CHECK(s.world_hash == sim.world().compute_hash().total);
    CHECK(s.player_dynasty == kNoPlayerDynasty);
    CHECK(s.content_hash == 0);
    CHECK(s.game_version == std::string(pat::core::version()));
    CHECK_FALSE(s.treasury.has_value());
    CHECK_FALSE(s.head_age.has_value());
    CHECK(s.name.empty());
    CHECK(s.preview.empty());
    CHECK(s.ironman == 0);
}

TEST_CASE("container v2: the summary is read without touching the zstd frame") {
    Simulation sim = demo_sim();
    const SaveSummary summary = full_summary();
    auto bytes = serialize_save(sim.world(), sim.pending_internal(), summary);
    const std::size_t body = kSaveHeaderSize + read_u32(bytes, kOffSummarySize);

    // Destroy the whole compressed frame: the summary is still readable, the world is not.
    auto broken = bytes;
    for (std::size_t i = body; i < broken.size(); ++i) {
        broken[i] = static_cast<std::uint8_t>(0xA5U ^ i);
    }
    const auto info = read_save_summary(broken);
    REQUIRE(info.has_value());
    CHECK(*info.value().summary == summary);
    const auto full = deserialize_save(broken);
    REQUIRE_FALSE(full.has_value());
    CHECK((full.error().code == 1 || full.error().code == 2 ||
           full.error().code == 23));  // zstd (core 1, 2) or the payload hash

    // Only the header and the summary are enough: a prefix plus the size of the whole file.
    const std::span<const std::uint8_t> prefix(bytes.data(), body);
    const auto from_prefix = read_save_summary(prefix, bytes.size());
    REQUIRE(from_prefix.has_value());
    CHECK(*from_prefix.value().summary == summary);
    // ... but a prefix of a file whose size does not fit the header is refused.
    CHECK(error_code(read_save_summary(prefix, bytes.size() + 1)) == 20);

    // The same from a file on disk whose frame is garbage.
    const auto path = temp_path("patriarch_save_container_noframe.psv");
    REQUIRE(write_file_atomic(path, broken).has_value());
    const auto from_file = read_save_summary(path);
    REQUIRE(from_file.has_value());
    CHECK(*from_file.value().summary == summary);
    std::filesystem::remove(path);

    CHECK(error_code(read_save_summary(temp_path("patriarch_save_container_missing.psv"))) == 33);
}

TEST_CASE("container v2: an unknown summary key is skipped, the summary may grow without a new version") {
    Simulation sim = demo_sim();
    const auto good = serialize_save(sim.world(), sim.pending_internal());

    auto fields = required_fields();
    fields.push_back({25, {1, 2, 3}});
    fields.push_back({63, std::vector<std::uint8_t>(500, 0x7F)});
    fields.push_back({4000, {}});
    const auto bytes = with_summary(good, encode_fields(fields));

    const auto info = read_save_summary(bytes);
    REQUIRE(info.has_value());
    REQUIRE(info.value().summary.has_value());
    CHECK(info.value().summary->scenario == "m1");
    CHECK(deserialize_save(bytes).has_value());

    // A summary of exactly the maximum size is fine.
    auto big = required_fields();
    const std::size_t used = encode_fields(big).size();
    big.push_back({40, std::vector<std::uint8_t>(kMaxSaveSummary - used - 4, 1)});
    const auto biggest = with_summary(good, encode_fields(big));
    CHECK(read_u32(biggest, kOffSummarySize) == kMaxSaveSummary);
    CHECK(deserialize_save(biggest).has_value());
}

TEST_CASE("container v2: a damaged summary is an error and never a crash") {
    Simulation sim = demo_sim();
    const auto good = serialize_save(sim.world(), sim.pending_internal(), full_summary());
    const std::size_t summary_size = read_u32(good, kOffSummarySize);

    SUBCASE("every flipped byte of the summary is caught by its hash (code 25)") {
        for (std::size_t i = 0; i < summary_size; ++i) {
            auto bad = good;
            bad[kSaveHeaderSize + i] ^= 0x40;
            CAPTURE(i);
            CHECK(error_code(read_save_summary(bad)) == 25);
            CHECK_FALSE(deserialize_save(bad).has_value());
        }
    }
    SUBCASE("a wrong summary hash (code 25)") {
        auto bad = good;
        bad[kOffSummaryHash] ^= 1;
        CHECK(error_code(read_save_summary(bad)) == 25);
        CHECK_FALSE(deserialize_save(bad).has_value());
    }
    SUBCASE("a summary larger than 16 KiB is refused from the header alone (code 27)") {
        auto bad = good;
        put_u32(bad, kOffSummarySize, static_cast<std::uint32_t>(kMaxSaveSummary + 1));
        CHECK(error_code(read_save_summary(bad)) == 27);
        CHECK_FALSE(deserialize_save(bad).has_value());
        put_u32(bad, kOffSummarySize, 0xFFFFFFFFU);
        CHECK(error_code(read_save_summary(bad)) == 27);
        // ... also with a well-formed oversized summary body behind a correct hash
        auto fields = required_fields();
        fields.push_back({40, std::vector<std::uint8_t>(kMaxSaveSummary, 1)});
        CHECK(error_code(read_save_summary(with_summary(good, encode_fields(fields)))) == 27);
    }
    SUBCASE("an empty summary (code 25)") {
        CHECK(error_code(read_save_summary(with_summary(good, {}))) == 25);
    }
    SUBCASE("a missing required field (code 26)") {
        for (std::size_t skip = 0; skip < required_fields().size(); ++skip) {
            auto fields = required_fields();
            fields.erase(fields.begin() + static_cast<std::ptrdiff_t>(skip));
            CAPTURE(skip);
            const auto bytes = with_summary(good, encode_fields(fields));
            CHECK(error_code(read_save_summary(bytes)) == 26);
            CHECK_FALSE(deserialize_save(bytes).has_value());
        }
    }
    SUBCASE("keys out of order, repeated or zero (code 25)") {
        auto fields = required_fields();
        std::swap(fields[3], fields[4]);
        CHECK(error_code(read_save_summary(with_summary(good, encode_fields(fields)))) == 25);
        fields = required_fields();
        fields.insert(fields.begin() + 1, text_field(1, "again"));
        CHECK(error_code(read_save_summary(with_summary(good, encode_fields(fields)))) == 25);
        fields = required_fields();
        fields.insert(fields.begin(), Field{0, {1}});
        CHECK(error_code(read_save_summary(with_summary(good, encode_fields(fields)))) == 25);
    }
    SUBCASE("a field longer than the summary (code 25)") {
        auto summary = encode_fields(required_fields());
        summary.push_back(40);
        summary.push_back(0);
        summary.push_back(0xFF);  // claims 255 bytes, none follow
        summary.push_back(0);
        CHECK(error_code(read_save_summary(with_summary(good, summary))) == 25);
        summary.resize(summary.size() - 1);  // an incomplete field header
        CHECK(error_code(read_save_summary(with_summary(good, summary))) == 25);
    }
    SUBCASE("too many fields (code 25)") {
        auto fields = required_fields();
        for (std::uint16_t key = 21; fields.size() <= kMaxSummaryFields; ++key) {
            fields.push_back({key, {0}});
        }
        CHECK(fields.size() == kMaxSummaryFields + 1);
        CHECK(error_code(read_save_summary(with_summary(good, encode_fields(fields)))) == 25);
        fields.pop_back();
        CHECK(read_save_summary(with_summary(good, encode_fields(fields))).has_value());
    }
    SUBCASE("a known field with a wrong length or value (code 25)") {
        const auto damage = [&](std::uint16_t key, Field replacement) {
            auto fields = required_fields();
            for (Field& f : fields) {
                if (f.key == key) {
                    f = std::move(replacement);
                }
            }
            return error_code(read_save_summary(with_summary(good, encode_fields(fields))));
        };
        CHECK(damage(2, {2, {1, 2, 3}}) == 25);                       // u32 with 3 bytes
        CHECK(damage(4, {4, {1, 2, 3, 4}}) == 25);                    // u64 with 4 bytes
        CHECK(damage(14, u8_field(14, 4)) == 25);                     // kind beyond the known ones
        CHECK(damage(1, text_field(1, std::string(65, 'x'))) == 25);  // text beyond its limit
        CHECK(damage(3, {3, {'a', 0, 'b'}}) == 25);                   // a NUL byte
        CHECK(damage(3, {3, {0xC3}}) == 25);                          // truncated UTF-8
        CHECK(damage(3, {3, {0xC0, 0x80}}) == 25);                    // overlong UTF-8
        CHECK(damage(3, {3, {0xED, 0xA0, 0x80}}) == 25);              // a surrogate
    }
    SUBCASE("optional fields with bad values (code 25)") {
        const auto with = [&](Field extra) {
            auto fields = required_fields();
            fields.push_back(std::move(extra));
            std::sort(
                fields.begin(), fields.end(), [](const Field& a, const Field& b) { return a.key < b.key; });
            return error_code(read_save_summary(with_summary(good, encode_fields(fields))));
        };
        CHECK(with(u32_field(12, 1000001)) == 25);                // progress beyond 100%
        CHECK(with(text_field(15, "")) == 25);                    // an empty name
        CHECK(with(text_field(15, std::string(65, 'x'))) == 25);  // more than 64 characters
        CHECK(with(text_field(7, std::string(97, 'x'))) == 25);   // head name beyond its limit
        CHECK(with({8, {1, 2}}) == 25);                           // head_age has one byte
    }
}

TEST_CASE("container v2: truncated files are rejected at every length") {
    Simulation sim = demo_sim();
    const auto good = serialize_save(sim.world(), sim.pending_internal(), full_summary());
    const std::size_t summary_size = read_u32(good, kOffSummarySize);

    // A truncated header, then a truncated summary: both are errors of the file layout (code 20).
    for (std::size_t n = 0; n < kSaveHeaderSize + summary_size; ++n) {
        const std::span<const std::uint8_t> cut(good.data(), n);
        CAPTURE(n);
        CHECK(error_code(read_save_summary(cut)) == 20);
        CHECK_FALSE(deserialize_save(cut).has_value());
    }
    // Missing bytes of the frame: the size in the header no longer matches the file.
    CHECK(error_code(read_save_summary(std::span<const std::uint8_t>(good.data(), good.size() - 1))) == 20);
    CHECK_FALSE(deserialize_save(std::span<const std::uint8_t>(good.data(), good.size() - 1)).has_value());
    // Trailing bytes too.
    auto longer = good;
    longer.push_back(0);
    CHECK(error_code(read_save_summary(longer)) == 20);

    // Flags are reserved: any bit is an error.
    auto flagged = good;
    flagged[kOffFlags] = 1;
    CHECK(error_code(read_save_summary(flagged)) == 20);
    // Bad magic.
    auto foreign = good;
    foreign[0] = 'X';
    CHECK(error_code(read_save_summary(foreign)) == 20);
    // Header and summary of a path that does not exist or is empty.
    const auto empty = temp_path("patriarch_save_container_empty.psv");
    REQUIRE(write_file_atomic(empty, {}).has_value());
    CHECK(error_code(read_save_summary(empty)) == 20);
    std::filesystem::remove(empty);
}

TEST_CASE("container v1: the previous format is still read, with the minimum summary from the world") {
    const auto file = read_file(fixture("e1_demo_v1.psv"));
    REQUIRE(file.has_value());
    CHECK(file.value()[kOffVersion] == 1);

    const auto info = read_save_summary(file.value());
    REQUIRE(info.has_value());
    CHECK(info.value().format_version == 1);
    CHECK_FALSE(info.value().summary.has_value());
    CHECK(info.value().compressed_size + kSaveHeaderSizeV1 == file.value().size());

    auto loaded = deserialize_save(file.value());
    REQUIRE(loaded.has_value());
    CHECK(loaded.value().format_version == 1);
    CHECK(loaded.value().world.compute_hash().total == 0xe9822290b7ce3cc3ULL);  // tests/golden_test.cpp
    SaveSummary expected = summary_from_world(loaded.value().world);
    expected.game_version.clear();
    CHECK(loaded.value().summary == expected);
    CHECK(loaded.value().summary.date == Date::from_ymd(1707, 1, 1).days());

    // Saving it again writes the current version and keeps the world.
    const auto resaved = serialize_save(loaded.value().world, loaded.value().pending_internal);
    CHECK(resaved[kOffVersion] == 2);
    auto reloaded = deserialize_save(resaved);
    REQUIRE(reloaded.has_value());
    CHECK(reloaded.value().world.compute_hash() == loaded.value().world.compute_hash());

    // The same from a path (header only) and a version 1 file with damage.
    CHECK(read_save_summary(fixture("e1_demo_v1.psv")).value().format_version == 1);
    auto cut = file.value();
    cut.pop_back();
    CHECK(error_code(read_save_summary(cut)) == 20);
    CHECK_FALSE(deserialize_save(cut).has_value());
    for (std::size_t n = 0; n < kSaveHeaderSizeV1; ++n) {
        CHECK_FALSE(read_save_summary(std::span<const std::uint8_t>(file.value().data(), n)).has_value());
    }
    auto flipped = file.value();
    flipped[flipped.size() / 2] ^= 0x10;
    const auto flipped_result = deserialize_save(flipped);
    if (flipped_result.has_value()) {
        CHECK(flipped_result.value().world.compute_hash() == loaded.value().world.compute_hash());
    }
}

TEST_CASE("container v1: a version 1 file made from the current payload loads like a version 2 one") {
    // The payload is the same in both versions, so a version 1 file can be assembled from a version 2 one.
    Simulation sim = demo_sim();
    REQUIRE_FALSE(sim.pending_internal().empty());
    const auto v2 = serialize_save(sim.world(), sim.pending_internal());
    const std::size_t summary_size = read_u32(v2, kOffSummarySize);

    ByteWriter w;
    w.bytes(std::span<const std::uint8_t>(v2).subspan(0, 4));
    w.u16(1);
    w.u16(0);
    w.bytes(std::span<const std::uint8_t>(v2).subspan(
        20, 24));  // uncompressed size, payload hash, compressed size
    w.bytes(std::span<const std::uint8_t>(v2).subspan(kSaveHeaderSize + summary_size));
    const auto v1 = w.take();

    auto loaded = deserialize_save(v1);
    REQUIRE(loaded.has_value());
    CHECK(loaded.value().format_version == 1);
    CHECK(loaded.value().world.compute_hash() == sim.world().compute_hash());
    CHECK(loaded.value().pending_internal ==
          std::vector<Command>(sim.pending_internal().begin(), sim.pending_internal().end()));
}

TEST_CASE("container v2: the version 2 fixture written by this build is readable") {
    // tests/data/saves/e1_demo_container_v2.psv: made by the code of E13-T02 with
    //   simcli --seed 42 --days 1461 --save e1_demo_container_v2.psv
    // (the demo scenario at 1 January 1707, the state of tests/golden_test.cpp; world schema 11).
    const auto file = read_file(fixture("e1_demo_container_v2.psv"));
    REQUIRE(file.has_value());
    CHECK(file.value()[kOffVersion] == 2);

    const auto info = read_save_summary(fixture("e1_demo_container_v2.psv"));
    REQUIRE(info.has_value());
    REQUIRE(info.value().summary.has_value());
    const SaveSummary& s = *info.value().summary;
    CHECK(s.state_schema == 11);
    CHECK(s.date == Date::from_ymd(1707, 1, 1).days());
    CHECK(s.world_hash == 0xe9822290b7ce3cc3ULL);
    CHECK(s.content_hash == 0);
    CHECK(s.player_dynasty == kNoPlayerDynasty);
    CHECK_FALSE(s.game_version.empty());

    auto loaded = deserialize_save(file.value());
    REQUIRE(loaded.has_value());
    CHECK(loaded.value().format_version == 2);
    CHECK(loaded.value().world.compute_hash().total == 0xe9822290b7ce3cc3ULL);
    CHECK(loaded.value().summary == s);
}

TEST_CASE("container v2: an older build stops at the header with \"created by a newer version\"") {
    Simulation sim = demo_sim();
    const auto v2 = serialize_save(sim.world(), sim.pending_internal(), full_summary());

    // What the build before E13-T02 does with this very file: it knows the magic, sees version 2 > 1
    // and answers code 22, never reading the rest (the layout after offset 8 is different).
    CHECK(old_build_header_gate(v2) == 22);
    // A version 1 file passes that gate (the formats are compatible in that direction).
    const auto v1 = read_file(fixture("e1_demo_v1.psv"));
    REQUIRE(v1.has_value());
    CHECK(old_build_header_gate(v1.value()) == 0);

    // The same rule for this build and the version after it: 3 is "newer", with the message the UI shows.
    for (const std::uint16_t newer : {std::uint16_t(3), std::uint16_t(4), std::uint16_t(0xFFFF)}) {
        auto bytes = v2;
        bytes[kOffVersion] = static_cast<std::uint8_t>(newer & 0xFFU);
        bytes[kOffVersion + 1] = static_cast<std::uint8_t>(newer >> 8);
        const auto info = read_save_summary(bytes);
        REQUIRE_FALSE(info.has_value());
        CHECK(info.error().code == 22);
        CHECK(info.error().message.find("newer") != std::string::npos);
        const auto full = deserialize_save(bytes);
        REQUIRE_FALSE(full.has_value());
        CHECK(full.error().code == 22);
        // even if the newer layout set flags and changed sizes
        bytes[kOffFlags] = 7;
        CHECK(error_code(read_save_summary(bytes)) == 22);
        CHECK(error_code(read_save_summary(std::span<const std::uint8_t>(bytes.data(), 8))) == 22);
    }
    auto zero = v2;
    zero[kOffVersion] = 0;
    CHECK(error_code(read_save_summary(zero)) == 22);
}

TEST_CASE("container v2: the schema of the summary is checked against this build and against the payload") {
    Simulation sim = demo_sim();
    const auto good = serialize_save(sim.world(), sim.pending_internal());

    // The summary says the world is of a newer schema: refused before anything is decompressed (code 28).
    auto fields = required_fields();
    fields[1] = u32_field(2, World::kStateSchema + 1);
    const auto newer = with_summary(good, encode_fields(fields));
    CHECK(read_save_summary(newer).has_value());  // a list can still show it
    const auto newer_result = deserialize_save(newer);
    REQUIRE_FALSE(newer_result.has_value());
    CHECK(newer_result.error().code == 28);
    CHECK(newer_result.error().message.find("newer") != std::string::npos);

    // The summary and the payload disagree (code 29).
    fields[1] = u32_field(2, World::kStateSchema - 1);
    const auto mismatch = deserialize_save(with_summary(good, encode_fields(fields)));
    REQUIRE_FALSE(mismatch.has_value());
    CHECK(mismatch.error().code == 29);
}

TEST_CASE("container v2: the writer cuts text to its limits and never writes an unreadable file") {
    Simulation sim = demo_sim();
    SaveSummary s = full_summary();
    s.game_version = std::string(200, 'v');
    s.scenario = "m\xD0\x96" + std::string(100, 'z');
    s.head_name = std::string(500, 'h');
    s.name = std::string(90, 'n');  // more than 64 characters
    s.goal_key = std::string("bad\0nul", 7) + "\xFF\xFE" + "tail";
    s.goal_progress_ppm = 5'000'000;
    s.preview = std::vector<std::uint8_t>(kMaxSaveSummary, 9);  // does not fit: dropped
    s.kind = static_cast<SaveKind>(77);

    const auto bytes = serialize_save(sim.world(), sim.pending_internal(), s);
    const auto info = read_save_summary(bytes);
    REQUIRE(info.has_value());
    const SaveSummary& got = *info.value().summary;
    CHECK(got.game_version.size() == 64);
    CHECK(got.scenario.size() <= 64);
    CHECK(got.scenario.starts_with("m\xD0\x96"));
    CHECK(got.head_name.size() == 96);
    CHECK(got.name.size() == 64);
    CHECK(got.goal_key == "badnultail");  // NUL and malformed bytes dropped
    CHECK(*got.goal_progress_ppm == 1000000);
    CHECK(got.preview.empty());
    CHECK(got.kind == SaveKind::Manual);
    CHECK(deserialize_save(bytes).has_value());

    // A multi-byte name is cut at a character boundary: 64 two-byte letters = 128 bytes, kept whole.
    SaveSummary cyr = full_summary();
    cyr.name.clear();
    for (int i = 0; i < 70; ++i) {
        cyr.name += "\xD0\xAF";  // U+042F
    }
    const auto cyr_info = read_save_summary(serialize_save(sim.world(), sim.pending_internal(), cyr));
    REQUIRE(cyr_info.has_value());
    CHECK(cyr_info.value().summary->name.size() == 128);
}
