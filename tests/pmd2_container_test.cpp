#include "pat/core/hash_stream.hpp"
#include "pat/mapdata/pmd2_container.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>

#include "test_bytes.hpp"

using namespace pat::mapdata;

namespace {

constexpr Pmd2ChunkId kHght = pmd2_chunk_id('H', 'G', 'H', 'T');
constexpr Pmd2ChunkId kMatl = pmd2_chunk_id('M', 'A', 'T', 'L');
constexpr Pmd2ChunkId kRegn = pmd2_chunk_id('R', 'E', 'G', 'N');
constexpr Pmd2ChunkId kPtbl = pmd2_chunk_id('P', 'T', 'B', 'L');
constexpr Pmd2ChunkId kXtra = pmd2_chunk_id('X', 'T', 'R', 'A');

// Offsets inside the file (ADR-0007 section 2.1), spelled out independently of the writer.
constexpr std::size_t kTable = 64;
constexpr std::size_t kEntry = 56;

struct Sample {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint16_t> heights;
    std::vector<std::uint8_t> materials;
    std::vector<std::uint8_t> regions;  // shift 2 raster
    std::vector<std::uint8_t> table;
};

Sample make_sample(std::uint32_t w, std::uint32_t h) {
    Sample s;
    s.width = w;
    s.height = h;
    std::uint32_t rng = 12345;
    const auto next = [&rng] {
        rng = rng * 1664525U + 1013904223U;
        return rng >> 8U;
    };
    s.heights.resize(static_cast<std::size_t>(w) * h);
    s.materials.resize(s.heights.size());
    for (std::uint32_t y = 0; y < h; ++y) {
        for (std::uint32_t x = 0; x < w; ++x) {
            const std::size_t i = static_cast<std::size_t>(y) * w + x;
            std::uint32_t v = (x * 7U + y * 13U) % 5000U + (next() % 16U);
            if (next() % 97U == 0) {
                v = (next() % 2U) != 0 ? 0U : 65535U;  // Forces wrapping deltas.
            }
            s.heights[i] = static_cast<std::uint16_t>(v);
            s.materials[i] = static_cast<std::uint8_t>((x / 20U + y / 20U) % 10U);
        }
    }
    const std::uint32_t rw = (w + 3U) / 4U;
    const std::uint32_t rh = (h + 3U) / 4U;
    s.regions.resize(static_cast<std::size_t>(rw) * rh);
    for (std::size_t i = 0; i < s.regions.size(); ++i) {
        s.regions[i] = static_cast<std::uint8_t>(next() % 7U);
    }
    for (std::uint32_t i = 0; i < 3000; ++i) {
        s.table.push_back(static_cast<std::uint8_t>((i * 31U) % 251U));
    }
    return s;
}

Pmd2WriterOptions options(int level = 3, unsigned threads = 1, Pmd2Codec codec = Pmd2Codec::zstd) {
    Pmd2WriterOptions o;
    o.zstd_level = level;
    o.threads = threads;
    o.codec = codec;
    return o;
}

Pmd2RasterParams raster_params(std::uint8_t tile_log2, std::uint8_t shift = 0, std::uint8_t transform = 1) {
    Pmd2RasterParams p;
    p.tile_log2 = tile_log2;
    p.shift = shift;
    p.transform = transform;
    return p;
}

Pmd2ChunkSpec
spec(Pmd2ChunkId id, bool critical = true, std::uint16_t version = 1, std::uint8_t instance = 0) {
    Pmd2ChunkSpec s;
    s.id = id;
    s.critical = critical;
    s.version = version;
    s.instance = instance;
    return s;
}

std::vector<std::uint8_t> build(const Sample& s, const Pmd2WriterOptions& opt, std::uint8_t tile_log2 = 6) {
    Pmd2Writer w(Pmd2Kind::physical, s.width, s.height, opt);
    w.set_bind_key(0xABCDEF0123456789ULL);
    CHECK(w.add_raster(spec(kHght), raster_params(tile_log2), std::span<const std::uint16_t>(s.heights))
              .has_value());
    CHECK(w.add_raster(spec(kMatl), raster_params(tile_log2), std::span<const std::uint8_t>(s.materials))
              .has_value());
    CHECK(w.add_raster(
               spec(kRegn, false), raster_params(tile_log2, 2), std::span<const std::uint8_t>(s.regions))
              .has_value());
    CHECK(w.add_blob(spec(kPtbl), s.table).has_value());
    auto out = w.finish();
    REQUIRE(out.has_value());
    return out.value();
}

Pmd2OpenOptions known_all() {
    Pmd2OpenOptions o;
    for (const Pmd2ChunkId id : {kHght, kMatl, kRegn, kPtbl}) {
        Pmd2KnownChunk k;
        k.id = id;
        k.max_version = 1;
        o.known.push_back(k);
    }
    return o;
}

int open_code(const std::vector<std::uint8_t>& bytes, const Pmd2OpenOptions& o = known_all()) {
    const auto r = Pmd2Reader::open(bytes, o);
    return r.has_value() ? 0 : r.error().code;
}

/// Recomputes content_id (from the table) and header_hash, so a test can alter a table field and
/// still reach the check it is aimed at.
void refresh_header(std::vector<std::uint8_t>& b, bool recompute_content_id = true) {
    const std::uint32_t count = pat::test::load_le<std::uint32_t>(b, 20);
    if (recompute_content_id) {
        pat::core::HashStream c;
        c.u16(static_cast<std::uint16_t>(b[6] | (b[7] << 8U)));
        for (std::uint32_t i = 0; i < count; ++i) {
            const std::size_t e = kTable + kEntry * i;
            for (std::size_t k = 0; k < 4; ++k) {
                c.u8(b[e + k]);
            }
            c.u16(static_cast<std::uint16_t>(b[e + 4] | (b[e + 5] << 8U)));
            c.u8(b[e + 9]);
            c.u64(pat::test::load_le<std::uint64_t>(b, e + 40));
        }
        pat::test::store_le<std::uint64_t>(b, 32, c.value());
    }
    pat::core::HashStream h;
    h.bytes(std::span<const std::uint8_t>(b).first(56));
    h.bytes(std::span<const std::uint8_t>(b).subspan(kTable, kEntry * count));
    pat::test::store_le<std::uint64_t>(b, 56, h.value());
}

std::size_t entry_of(const std::vector<std::uint8_t>& b, Pmd2ChunkId id) {
    const std::uint32_t count = pat::test::load_le<std::uint32_t>(b, 20);
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::size_t e = kTable + kEntry * i;
        const Pmd2ChunkId got = (static_cast<std::uint32_t>(b[e]) << 24U) |
                                (static_cast<std::uint32_t>(b[e + 1]) << 16U) |
                                (static_cast<std::uint32_t>(b[e + 2]) << 8U) | b[e + 3];
        if (got == id) {
            return e;
        }
    }
    FAIL("chunk not in the table");
    return 0;
}

}  // namespace

TEST_CASE("pmd2: header and table layout follow the specification") {
    const Sample s = make_sample(150, 100);
    const auto bytes = build(s, options());
    REQUIRE(bytes.size() > kTable + 4 * kEntry);
    CHECK(std::equal(
        bytes.begin(), bytes.begin() + 4, std::array<std::uint8_t, 4>{'P', 'M', 'D', '2'}.begin()));
    CHECK(pat::test::load_le<std::uint32_t>(bytes, 20) == 4);             // chunk_count
    CHECK(pat::test::load_le<std::uint64_t>(bytes, 24) == bytes.size());  // file_size
    CHECK(pat::test::load_le<std::uint64_t>(bytes, 40) == 0xABCDEF0123456789ULL);
    // Chunks are listed by id: HGHT, MATL, PTBL, REGN.
    CHECK(entry_of(bytes, kHght) == kTable);
    CHECK(entry_of(bytes, kRegn) == kTable + 3 * kEntry);
    CHECK(pat::test::load_le<std::uint64_t>(bytes, kTable + 16) ==
          kTable + 4 * kEntry);  // first body follows the table
    // Flags: critical + tiled for HGHT; optional + tiled for REGN; critical blob for PTBL.
    CHECK(bytes[entry_of(bytes, kHght) + 6] == 3);
    CHECK(bytes[entry_of(bytes, kRegn) + 6] == 2);
    CHECK(bytes[entry_of(bytes, kPtbl) + 6] == 1);
}

TEST_CASE("pmd2: round trip of rasters (1 and 2 bytes, shift) and a blob") {
    const Sample s = make_sample(150, 100);  // 3 x 2 tiles of 64, ragged edges
    const auto bytes = build(s, options());
    auto r = Pmd2Reader::open(bytes, known_all());
    REQUIRE(r.has_value());
    const Pmd2Reader& reader = r.value();
    CHECK(reader.header().kind == Pmd2Kind::physical);
    CHECK(reader.header().width == 150);
    CHECK(reader.header().bind_key == 0xABCDEF0123456789ULL);
    CHECK(reader.chunks().size() == 4);

    auto heights = reader.read_raster_u16(kHght);
    REQUIRE(heights.has_value());
    CHECK(heights.value() == s.heights);
    auto materials = reader.read_raster_u8(kMatl);
    REQUIRE(materials.has_value());
    CHECK(materials.value() == s.materials);
    auto regions = reader.read_raster_u8(kRegn);
    REQUIRE(regions.has_value());
    CHECK(regions.value() == s.regions);
    auto table = reader.read_blob(kPtbl);
    REQUIRE(table.has_value());
    CHECK(table.value() == s.table);

    const auto info = reader.raster_info(kRegn);
    REQUIRE(info.has_value());
    CHECK(info.value().width == 38);  // ceil(150 / 4)
    CHECK(info.value().height == 25);
    CHECK(info.value().elem_bytes == 1);
    CHECK(reader.raster_info(kHght).value().tile_count() == 6);

    // Strict mode passes on a correct file.
    Pmd2OpenOptions strict = known_all();
    strict.verify_raw = true;
    CHECK(open_code(bytes, strict) == 0);
}

TEST_CASE("pmd2: raw codec and transform 0 round trip") {
    const Sample s = make_sample(90, 70);
    Pmd2Writer w(Pmd2Kind::provinces, s.width, s.height, options(3, 1, Pmd2Codec::raw));
    REQUIRE(w.add_raster(spec(kHght), raster_params(6, 0, 0), std::span<const std::uint16_t>(s.heights))
                .has_value());
    REQUIRE(w.add_raster(spec(kMatl), raster_params(6, 0, 1), std::span<const std::uint8_t>(s.materials))
                .has_value());
    REQUIRE(w.add_blob(spec(kPtbl), s.table).has_value());
    const auto bytes = w.finish().value();
    Pmd2OpenOptions strict = known_all();
    strict.verify_raw = true;
    auto r = Pmd2Reader::open(bytes, strict);
    REQUIRE(r.has_value());
    CHECK(r.value().header().kind == Pmd2Kind::provinces);
    CHECK(r.value().read_raster_u16(kHght).value() == s.heights);
    CHECK(r.value().read_raster_u8(kMatl).value() == s.materials);
    CHECK(r.value().read_blob(kPtbl).value() == s.table);
}

TEST_CASE("pmd2: lazy tile read returns exactly the tile region") {
    const Sample s = make_sample(150, 100);
    const auto bytes = build(s, options());
    auto r = Pmd2Reader::open(bytes, known_all());
    REQUIRE(r.has_value());
    const Pmd2RasterInfo info = r.value().raster_info(kHght).value();
    for (std::uint32_t t = 0; t < info.tile_count(); ++t) {
        CAPTURE(t);
        const Pmd2TileRect rect = info.tile_rect(t);
        auto tile = r.value().read_tile(kHght, t);
        REQUIRE(tile.has_value());
        REQUIRE(tile.value().size() == static_cast<std::size_t>(rect.w) * rect.h * 2);
        bool same = true;
        for (std::uint32_t y = 0; y < rect.h; ++y) {
            for (std::uint32_t x = 0; x < rect.w; ++x) {
                const std::size_t k = (static_cast<std::size_t>(y) * rect.w + x) * 2;
                const auto v = static_cast<std::uint16_t>(tile.value()[k] | (tile.value()[k + 1] << 8U));
                same = same && v == s.heights[static_cast<std::size_t>(rect.y + y) * s.width + rect.x + x];
            }
        }
        CHECK(same);
    }
    CHECK(info.tile_rect(2).w == 22);  // 150 - 128
    CHECK(info.tile_rect(3).h == 36);  // 100 - 64
    const auto bad = r.value().read_tile(kHght, 6);
    REQUIRE_FALSE(bad.has_value());
    CHECK(bad.error().code == pmd2_error::kNotFound);
}

TEST_CASE("pmd2: parallel decode equals sequential decode") {
    const Sample s = make_sample(300, 200);
    const auto bytes = build(s, options(3, 4));
    CHECK(bytes == build(s, options(3, 1)));  // The writer's output does not depend on threads.
    auto r = Pmd2Reader::open(bytes, known_all());
    REQUIRE(r.has_value());
    for (const unsigned threads : {1U, 2U, 3U, 8U, 0U}) {
        CAPTURE(threads);
        CHECK(r.value().read_raster_u16(kHght, 0, threads).value() == s.heights);
    }
    Pmd2OpenOptions strict = known_all();
    strict.verify_raw = true;
    strict.threads = 4;
    CHECK(open_code(bytes, strict) == 0);
}

TEST_CASE("pmd2: content_id and raw hashes do not depend on tile size, zstd level or codec") {
    const Sample s = make_sample(200, 130);
    const auto reference = build(s, options(3), 6);
    const std::uint64_t id = pat::test::load_le<std::uint64_t>(reference, 32);
    struct Variant {
        int level;
        std::uint8_t tile_log2;
        Pmd2Codec codec;
    };
    for (const Variant v : {Variant{1, 7, Pmd2Codec::zstd},
                            Variant{19, 8, Pmd2Codec::zstd},
                            Variant{3, 12, Pmd2Codec::zstd},
                            Variant{3, 6, Pmd2Codec::raw},
                            Variant{9, 10, Pmd2Codec::zstd}}) {
        CAPTURE(v.level);
        CAPTURE(static_cast<int>(v.tile_log2));
        const auto other = build(s, options(v.level, 1, v.codec), v.tile_log2);
        CHECK(pat::test::load_le<std::uint64_t>(other, 32) == id);
        auto a = Pmd2Reader::open(reference, known_all());
        auto b = Pmd2Reader::open(other, known_all());
        REQUIRE(a.has_value());
        REQUIRE(b.has_value());
        for (std::size_t i = 0; i < 4; ++i) {
            CHECK(a.value().chunks()[i].raw_hash == b.value().chunks()[i].raw_hash);
        }
        CHECK(b.value().read_raster_u16(kHght).value() == s.heights);
    }
    // Different content gives a different id.
    Sample changed = s;
    changed.heights[17] = static_cast<std::uint16_t>(changed.heights[17] + 1U);
    CHECK(pat::test::load_le<std::uint64_t>(build(changed, options()), 32) != id);
}

TEST_CASE("pmd2: every truncation is detected") {
    const Sample s = make_sample(100, 80);
    const auto bytes = build(s, options());
    for (std::size_t len = 0; len < bytes.size(); ++len) {
        CAPTURE(len);
        const std::vector<std::uint8_t> cut(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(len));
        CHECK(open_code(cut) != 0);
    }
    const std::vector<std::uint8_t> short_head(bytes.begin(), bytes.begin() + 30);
    CHECK(open_code(short_head) == pmd2_error::kTruncated);
    const std::vector<std::uint8_t> short_table(bytes.begin(), bytes.begin() + 100);
    CHECK(open_code(short_table) == pmd2_error::kTruncated);
    const std::vector<std::uint8_t> last_byte_missing(bytes.begin(), bytes.end() - 1);
    CHECK(open_code(last_byte_missing) == pmd2_error::kFileSize);
    auto trailing = bytes;
    trailing.push_back(0);
    CHECK(open_code(trailing) == pmd2_error::kFileSize);
}

TEST_CASE("pmd2: corrupt bytes are caught in every area") {
    const Sample s = make_sample(150, 100);
    const auto good = build(s, options());
    REQUIRE(open_code(good) == 0);

    SUBCASE("magic") {
        auto b = good;
        b[0] = 'X';
        CHECK(open_code(b) == pmd2_error::kBadMagic);
    }
    SUBCASE("every header byte after the magic") {
        for (std::size_t i = 4; i < kTable; ++i) {
            CAPTURE(i);
            auto b = good;
            b[i] = static_cast<std::uint8_t>(b[i] ^ 0x40U);
            CHECK(open_code(b) != 0);
        }
        auto b = good;
        b[12] = static_cast<std::uint8_t>(b[12] ^ 1U);  // width
        CHECK(open_code(b) == pmd2_error::kHeaderHash);
    }
    SUBCASE("every byte of the chunk table") {
        for (std::size_t i = kTable; i < kTable + 4 * kEntry; ++i) {
            CAPTURE(i);
            auto b = good;
            b[i] = static_cast<std::uint8_t>(b[i] ^ 0x01U);
            CHECK(open_code(b) == pmd2_error::kHeaderHash);
        }
    }
    SUBCASE("blob body") {
        auto b = good;
        const std::size_t at =
            static_cast<std::size_t>(pat::test::load_le<std::uint64_t>(b, entry_of(b, kPtbl) + 16)) + 3;
        b[at] = static_cast<std::uint8_t>(b[at] ^ 0xFFU);
        CHECK(open_code(b) == pmd2_error::kStoredHash);
    }
    SUBCASE("raster prefix and tile index") {
        for (const std::size_t delta : {std::size_t{0}, std::size_t{4}, std::size_t{17}, std::size_t{22}}) {
            CAPTURE(delta);
            auto b = good;
            const std::size_t at =
                static_cast<std::size_t>(pat::test::load_le<std::uint64_t>(b, entry_of(b, kHght) + 16)) +
                delta;
            b[at] = static_cast<std::uint8_t>(b[at] ^ 0x10U);
            CHECK(open_code(b) == pmd2_error::kStoredHash);
        }
    }
    SUBCASE("tile bytes: found when the tile is read, other tiles stay readable") {
        auto b = good;
        const std::size_t base =
            static_cast<std::size_t>(pat::test::load_le<std::uint64_t>(b, entry_of(b, kHght) + 16));
        const std::size_t prefix = 16 + 12 * 6;
        b[base + prefix + 5] = static_cast<std::uint8_t>(b[base + prefix + 5] ^ 0x08U);  // inside tile 0
        auto r = Pmd2Reader::open(b, known_all());
        REQUIRE(r.has_value());
        const auto t0 = r.value().read_tile(kHght, 0);
        REQUIRE_FALSE(t0.has_value());
        CHECK(t0.error().code == pmd2_error::kTileHash);
        CHECK(r.value().read_tile(kHght, 1).has_value());
        CHECK(r.value().read_raster_u16(kHght, 0, 4).error().code == pmd2_error::kTileHash);
        Pmd2OpenOptions strict = known_all();
        strict.verify_raw = true;
        CHECK(open_code(b, strict) == pmd2_error::kTileHash);
    }
    SUBCASE("raw_hash is checked only in strict mode") {
        auto b = good;
        const std::size_t e = entry_of(b, kMatl);
        pat::test::store_le<std::uint64_t>(b, e + 40, pat::test::load_le<std::uint64_t>(b, e + 40) ^ 1U);
        refresh_header(b);
        CHECK(open_code(b) == 0);
        Pmd2OpenOptions strict = known_all();
        strict.verify_raw = true;
        CHECK(open_code(b, strict) == pmd2_error::kRawHash);
    }
    SUBCASE("content_id that disagrees with the table") {
        auto b = good;
        pat::test::store_le<std::uint64_t>(b, 32, pat::test::load_le<std::uint64_t>(b, 32) ^ 1U);
        refresh_header(b, false);
        CHECK(open_code(b) == pmd2_error::kContentId);
    }
    SUBCASE("corrupt zstd data with consistent hashes is a decode error") {
        auto b = good;
        const std::size_t e = entry_of(b, kPtbl);
        const auto at = static_cast<std::size_t>(pat::test::load_le<std::uint64_t>(b, e + 16));
        const auto size = static_cast<std::size_t>(pat::test::load_le<std::uint64_t>(b, e + 24));
        for (std::size_t i = 0; i < 8; ++i) {
            b[at + i] = 0xEE;  // destroys the frame header
        }
        pat::test::store_le<std::uint64_t>(
            b, e + 48, pat::core::hash_bytes(std::span<const std::uint8_t>(b).subspan(at, size)));
        refresh_header(b);
        auto r = Pmd2Reader::open(b, known_all());
        REQUIRE(r.has_value());
        const auto blob = r.value().read_blob(kPtbl);
        REQUIRE_FALSE(blob.has_value());
        CHECK(blob.error().code == pmd2_error::kDecode);
    }
}

TEST_CASE("pmd2: table inconsistencies are rejected") {
    const Sample s = make_sample(150, 100);
    const auto good = build(s, options());

    SUBCASE("too many chunks") {
        auto b = good;
        pat::test::store_le<std::uint32_t>(b, 20, 65);
        CHECK(open_code(b) == pmd2_error::kBadHeader);
    }
    SUBCASE("unknown kind, flags, reserved") {
        auto b = good;
        pat::test::store_le<std::uint16_t>(b, 6, 9);
        refresh_header(b);
        CHECK(open_code(b) == pmd2_error::kBadHeader);
        b = good;
        pat::test::store_le<std::uint32_t>(b, 8, 1);
        refresh_header(b);
        CHECK(open_code(b) == pmd2_error::kBadHeader);
        b = good;
        pat::test::store_le<std::uint64_t>(b, 48, 1);
        refresh_header(b);
        CHECK(open_code(b) == pmd2_error::kBadHeader);
    }
    SUBCASE("zero or huge dimensions") {
        auto b = good;
        pat::test::store_le<std::uint32_t>(b, 12, 0);
        refresh_header(b);
        CHECK(open_code(b) == pmd2_error::kBadHeader);
        b = good;
        pat::test::store_le<std::uint32_t>(b, 16, 70000);
        refresh_header(b);
        CHECK(open_code(b) == pmd2_error::kBadHeader);
    }
    SUBCASE("duplicate (id, instance)") {
        auto b = good;
        for (std::size_t i = 0; i < 4; ++i) {
            b[kTable + kEntry + i] = b[kTable + i];  // second entry gets the first id
        }
        refresh_header(b);
        CHECK(open_code(b) == pmd2_error::kDuplicateChunk);
    }
    SUBCASE("overlapping ranges") {
        auto b = good;
        pat::test::store_le<std::uint64_t>(
            b, kTable + kEntry + 16, pat::test::load_le<std::uint64_t>(b, kTable + 16) + 1);
        refresh_header(b);
        CHECK(open_code(b) == pmd2_error::kChunkRange);
    }
    SUBCASE("range outside the file") {
        auto b = good;
        pat::test::store_le<std::uint64_t>(b, kTable + 24, 1ULL << 40);
        refresh_header(b);
        CHECK(open_code(b) == pmd2_error::kChunkRange);
    }
    SUBCASE("gap before the first chunk") {
        auto b = good;
        pat::test::store_le<std::uint64_t>(
            b, kTable + 16, pat::test::load_le<std::uint64_t>(b, kTable + 16) - 1);
        refresh_header(b);
        CHECK(open_code(b) == pmd2_error::kChunkRange);
    }
    SUBCASE("reserved chunk fields, flags and codec") {
        auto b = good;
        b[kTable + 10] = 1;
        refresh_header(b);
        CHECK(open_code(b) == pmd2_error::kBadChunkEntry);
        b = good;
        b[kTable + 7] = 0x80;  // flag bit 15
        refresh_header(b);
        CHECK(open_code(b) == pmd2_error::kBadChunkEntry);
        b = good;
        b[kTable + 8] = 7;  // codec
        refresh_header(b);
        CHECK(open_code(b) == pmd2_error::kBadChunkEntry);
    }
    SUBCASE("non-ASCII chunk id") {
        auto b = good;
        b[kTable] = 0x01;
        refresh_header(b);
        CHECK(open_code(b) == pmd2_error::kBadChunkEntry);
    }
    SUBCASE("raster geometry that disagrees with the header") {
        // Change the raster's w field and fix the prefix hash so only the geometry is wrong.
        auto b = good;
        const std::size_t e = entry_of(b, kHght);
        const std::size_t at = static_cast<std::size_t>(pat::test::load_le<std::uint64_t>(b, e + 16));
        pat::test::store_le<std::uint32_t>(b, at + 4, 149);
        const std::size_t prefix = 16 + 12 * 6;
        pat::test::store_le<std::uint64_t>(
            b, e + 48, pat::core::hash_bytes(std::span<const std::uint8_t>(b).subspan(at, prefix)));
        refresh_header(b);
        CHECK(open_code(b) == pmd2_error::kBadRaster);
    }
}

TEST_CASE("pmd2: unknown optional chunks are skipped, unknown critical chunks are errors") {
    const Sample s = make_sample(100, 80);
    const std::vector<std::uint8_t> extra = {1, 2, 3, 4, 5, 6, 7, 8, 9};
    const auto build_with = [&](bool critical, std::uint16_t version) {
        Pmd2Writer w(Pmd2Kind::provinces, s.width, s.height, options());
        REQUIRE(w.add_raster(spec(kMatl), raster_params(6), std::span<const std::uint8_t>(s.materials))
                    .has_value());
        REQUIRE(w.add_blob(spec(kXtra, critical, version), extra).has_value());
        return w.finish().value();
    };
    Pmd2OpenOptions known;
    Pmd2KnownChunk m;
    m.id = kMatl;
    m.max_version = 1;
    known.known.push_back(m);

    SUBCASE("optional unknown chunk") {
        const auto bytes = build_with(false, 1);
        auto r = Pmd2Reader::open(bytes, known);
        REQUIRE(r.has_value());
        CHECK(r.value().chunks().size() == 2);
        CHECK(r.value().read_raster_u8(kMatl).value() == s.materials);
        const Pmd2ChunkInfo* x = r.value().find(kXtra);
        REQUIRE(x != nullptr);
        CHECK_FALSE(x->understood);
        CHECK(r.value().find(kMatl)->understood);
        Pmd2OpenOptions strict = known;
        strict.verify_raw = true;  // unknown chunks are not interpreted
        CHECK(open_code(bytes, strict) == 0);

        // An editing tool copies it byte for byte: the result is identical to the original.
        Pmd2Writer copy(Pmd2Kind::provinces, s.width, s.height, options());
        for (const Pmd2ChunkInfo& c : r.value().chunks()) {
            REQUIRE(copy.add_stored_chunk(c, r.value().stored_bytes(c)).has_value());
        }
        CHECK(copy.finish().value() == bytes);
    }
    SUBCASE("unknown critical chunk") {
        const auto bytes = build_with(true, 1);
        CHECK(open_code(bytes, known) == pmd2_error::kUnknownCritical);
        Pmd2OpenOptions inspect;
        inspect.inspect = true;
        CHECK(open_code(bytes, inspect) == 0);
        Pmd2OpenOptions all = known;
        Pmd2KnownChunk k;
        k.id = kXtra;
        all.known.push_back(k);
        CHECK(open_code(bytes, all) == 0);
    }
    SUBCASE("newer chunk version") {
        Pmd2OpenOptions with_x = known;
        Pmd2KnownChunk k;
        k.id = kXtra;
        k.max_version = 1;
        with_x.known.push_back(k);
        CHECK(open_code(build_with(true, 2), with_x) == pmd2_error::kUnsupportedChunk);
        auto r = Pmd2Reader::open(build_with(false, 2), with_x);  // optional: skipped
        REQUIRE(r.has_value());
        CHECK_FALSE(r.value().find(kXtra)->understood);
    }
}

TEST_CASE("pmd2: a larger minor version is readable") {
    const Sample s = make_sample(100, 80);
    const auto good = build(s, options());

    auto b = good;
    pat::test::store_le<std::uint16_t>(b, 4, 3);     // minor 3
    pat::test::store_le<std::uint32_t>(b, 8, 0xF0);  // flags the 2.0 reader does not know
    pat::test::store_le<std::uint64_t>(b, 48, 77);   // reserved field used by a later minor
    b[kTable + 10] = 9;                              // reserved bytes of a chunk entry
    refresh_header(b);
    auto r = Pmd2Reader::open(b, known_all());
    REQUIRE(r.has_value());
    CHECK(r.value().header().minor == 3);
    CHECK(r.value().read_raster_u16(kHght).value() == s.heights);

    // The same fields are errors while the minor is the one we know.
    auto strict_minor = b;
    pat::test::store_le<std::uint16_t>(strict_minor, 4, 0);
    refresh_header(strict_minor);
    CHECK(open_code(strict_minor) == pmd2_error::kBadHeader);
}

TEST_CASE("pmd2: wrapping row deltas and extreme values survive") {
    Pmd2Writer w(Pmd2Kind::physical, 64, 64, options());
    std::vector<std::uint16_t> v(64 * 64);
    for (std::size_t i = 0; i < v.size(); ++i) {
        v[i] = (i % 2 == 0) ? 0 : 65535;
    }
    REQUIRE(w.add_raster(spec(kHght), raster_params(6), std::span<const std::uint16_t>(v)).has_value());
    std::vector<std::uint8_t> u(64 * 64);
    for (std::size_t i = 0; i < u.size(); ++i) {
        u[i] = static_cast<std::uint8_t>((i * 37U) ^ (i >> 3U));
    }
    REQUIRE(w.add_raster(spec(kMatl), raster_params(6), std::span<const std::uint8_t>(u)).has_value());
    const auto bytes = w.finish().value();
    Pmd2OpenOptions strict = known_all();
    strict.verify_raw = true;
    auto r = Pmd2Reader::open(bytes, strict);
    REQUIRE(r.has_value());
    CHECK(r.value().read_raster_u16(kHght).value() == v);
    CHECK(r.value().read_raster_u8(kMatl).value() == u);
}

TEST_CASE("pmd2: writer rejects invalid input") {
    const Sample s = make_sample(100, 80);
    Pmd2Writer w(Pmd2Kind::physical, s.width, s.height, options());
    const std::span<const std::uint8_t> mats(s.materials);
    CHECK(w.add_raster(spec(kMatl), raster_params(6), mats).has_value());
    CHECK(w.add_raster(spec(kMatl), raster_params(6), mats).error().code ==
          pmd2_error::kBadArgument);  // duplicate
    CHECK(w.add_raster(spec(kMatl, true, 1, 1), raster_params(6), mats.first(10)).error().code ==
          pmd2_error::kBadArgument);  // wrong size
    CHECK(w.add_raster(spec(kHght), raster_params(5), mats).error().code == pmd2_error::kBadArgument);
    CHECK(w.add_raster(spec(kHght), raster_params(13), mats).error().code == pmd2_error::kBadArgument);
    CHECK(w.add_raster(spec(kHght), raster_params(6, 4), mats).error().code == pmd2_error::kBadArgument);
    CHECK(w.add_raster(spec(kHght), raster_params(6, 0, 2), mats).error().code == pmd2_error::kBadArgument);
    CHECK(w.add_blob(spec(pmd2_chunk_id('A', 'B', 'C', '\x01')), s.table).error().code ==
          pmd2_error::kBadArgument);
    // The same chunk id with a different instance is allowed (regional patches).
    CHECK(w.add_raster(spec(kMatl, true, 1, 1), raster_params(6), mats).has_value());
    for (std::uint32_t i = 0; i < 62; ++i) {
        const Pmd2ChunkId id =
            pmd2_chunk_id('B', 'L', static_cast<char>('A' + i / 26), static_cast<char>('a' + i % 26));
        CHECK(w.add_blob(spec(id), s.table).has_value());
    }
    CHECK(w.add_blob(spec(kPtbl), s.table).error().code == pmd2_error::kBadArgument);  // 65th chunk
    Pmd2Writer bad(Pmd2Kind::physical, 0, 10, options());
    CHECK(bad.finish().error().code == pmd2_error::kBadArgument);
}

TEST_CASE("pmd2: lookup errors and instances") {
    const Sample s = make_sample(100, 80);
    Pmd2Writer w(Pmd2Kind::physical, s.width, s.height, options());
    REQUIRE(
        w.add_raster(spec(kMatl), raster_params(6), std::span<const std::uint8_t>(s.materials)).has_value());
    REQUIRE(
        w.add_raster(spec(kMatl, true, 1, 1), raster_params(6), std::span<const std::uint8_t>(s.materials))
            .has_value());
    REQUIRE(w.add_blob(spec(kPtbl), s.table).has_value());
    const auto bytes = w.finish().value();
    auto r = Pmd2Reader::open(bytes, known_all());
    REQUIRE(r.has_value());
    CHECK(r.value().find(kMatl, 1) != nullptr);
    CHECK(r.value().find(kMatl, 2) == nullptr);
    CHECK(r.value().read_blob(kHght).error().code == pmd2_error::kNotFound);
    CHECK(r.value().read_blob(kMatl).error().code == pmd2_error::kNotFound);        // a raster, not a blob
    CHECK(r.value().read_raster(kPtbl).error().code == pmd2_error::kNotFound);      // a blob, not a raster
    CHECK(r.value().read_raster_u16(kMatl).error().code == pmd2_error::kNotFound);  // wrong element width
    CHECK(r.value().raster_info(kHght).error().code == pmd2_error::kNotFound);
    CHECK(pmd2_chunk_id_string(kHght) == "HGHT");
}

TEST_CASE("pmd2: files are written atomically and read back") {
    const Sample s = make_sample(100, 80);
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "patriarch_pmd2_test.pmd";
    Pmd2Writer w(Pmd2Kind::patches, s.width, s.height, options());
    REQUIRE(
        w.add_raster(spec(kMatl), raster_params(6), std::span<const std::uint8_t>(s.materials)).has_value());
    REQUIRE(w.write_file(path).has_value());
    CHECK_FALSE(std::filesystem::exists(path.string() + ".tmp"));
    {
        auto r = Pmd2Reader::open_file(path, known_all());
        REQUIRE(r.has_value());
        CHECK(r.value().header().kind == Pmd2Kind::patches);
        // The reader owns the bytes: moving it keeps the data valid.
        Pmd2Reader moved = std::move(r).value();
        CHECK(moved.read_raster_u8(kMatl).value() == s.materials);
    }
    std::error_code ec;
    std::filesystem::remove(path, ec);
    CHECK(Pmd2Reader::open_file(path, known_all()).error().code == pmd2_error::kIo);
}
