// PMD2 rivers (RIVR/RVEC) and natural regions (NREG/NRTB), ADR-0007 sections 3.4 and 3.5 (Geo-14).
// The physical base is a committed migrated fixture (tests/data/maps/v2_*), so no floating point is
// involved at test time; rivers and regions are built by hand on top of it.

#include "pat/mapdata/nature_layer.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace pat::mapdata;

namespace {

namespace fs = std::filesystem;
using Bytes = std::vector<std::uint8_t>;

constexpr Pmd2ChunkId kRivr = pmd2_chunk_id('R', 'I', 'V', 'R');
constexpr Pmd2ChunkId kRvec = pmd2_chunk_id('R', 'V', 'E', 'C');
constexpr Pmd2ChunkId kNreg = pmd2_chunk_id('N', 'R', 'E', 'G');
constexpr Pmd2ChunkId kNrtb = pmd2_chunk_id('N', 'R', 'T', 'B');

fs::path map_file(const char* set, const char* name) {
    return fs::path(PATRIARCH_TEST_DATA_DIR) / "maps" / set / name;
}

PhysicalLayer load_physical(const char* set) {
    auto layer = read_physical_file(map_file(set, "physical.pmd"));
    REQUIRE_MESSAGE(layer.has_value(), (layer.has_value() ? "" : layer.error().message));
    return std::move(layer.value());
}

template <class T>
int code_of(const pat::core::Result<T, pat::core::Error>& r) {
    return r.has_value() ? 0 : r.error().code;
}

struct Spot {
    std::uint32_t x = 0;
    std::uint32_t y = 0;
};

/// First run of `len` land pixels in a row, scanning from row `from`.
Spot find_land_run(const PhysicalLayer& l, std::uint32_t from, std::uint32_t len) {
    for (std::uint32_t y = from; y < l.height; ++y) {
        std::uint32_t run = 0;
        for (std::uint32_t x = 0; x < l.width; ++x) {
            run = l.is_land(static_cast<std::size_t>(y) * l.width + x) ? run + 1 : 0;
            if (run == len) {
                return {x + 1 - len, y};
            }
        }
    }
    FAIL("no land run found in the fixture");
    return {};
}

RiverPoint point_at(std::uint32_t px, std::uint32_t py, std::uint8_t width_class) {
    return {static_cast<std::int32_t>(px * 16 + 8), static_cast<std::int32_t>(py * 16 + 8), width_class};
}

/// Two rivers running west to east along land runs; classes grow downstream.
RiverLayer make_rivers(const PhysicalLayer& l) {
    RiverLayer rivers;
    rivers.rank_raster.assign(l.pixel_count(), 0);
    const std::uint8_t classes[] = {1, 1, 2, 2, 3, 3};
    Spot at = find_land_run(l, 0, 6);
    for (std::uint16_t id = 1; id <= 2; ++id) {
        River r;
        r.id = id;
        r.rank = 3;
        r.key = id == 1 ? "test-river" : "test-brook-2";
        for (std::uint32_t i = 0; i < 6; ++i) {
            r.points.push_back(point_at(at.x + i, at.y, classes[i]));
            rivers.rank_raster[static_cast<std::size_t>(at.y) * l.width + at.x + i] = classes[i];
        }
        rivers.rivers.push_back(std::move(r));
        at = find_land_run(l, at.y + 1, 6);
    }
    return rivers;
}

RegionLayer make_regions(const PhysicalLayer& l, std::uint8_t shift) {
    RegionLayer r;
    r.shift = shift;
    const std::uint32_t ex = region_raster_extent(l.width, shift);
    const std::uint32_t ey = region_raster_extent(l.height, shift);
    r.raster.assign(static_cast<std::size_t>(ex) * ey, 0);
    for (std::uint32_t y = 1; y < ey; ++y) {
        for (std::uint32_t x = 0; x < ex; ++x) {
            r.raster[static_cast<std::size_t>(y) * ex + x] = static_cast<std::uint8_t>(1 + x * 3 / ex);
        }
    }
    r.regions.push_back({1, RegionKind::climate_zone, "temperate-zone", "Temperate zone"});
    r.regions.push_back({2, RegionKind::orographic, "central-upland", "Central upland"});
    r.regions.push_back({3, RegionKind::climate_zone, "subarctic-zone", "Subarctic zone"});
    return r;
}

NatureLayer make_nature(const PhysicalLayer& l, std::uint8_t shift = 2) {
    NatureLayer n;
    n.rivers = make_rivers(l);
    n.regions = make_regions(l, shift);
    return n;
}

Bytes with_nature(const PhysicalLayer& l, const NatureLayer& n, const Pmd2WriterOptions& o = {}) {
    auto bytes = serialize_physical_with_nature(l, n, o);
    REQUIRE_MESSAGE(bytes.has_value(), (bytes.has_value() ? "" : bytes.error().message));
    return std::move(bytes.value());
}

/// Copies a physical container without the given chunk (tests of half pairs).
Bytes without_chunk(const Bytes& bytes, Pmd2ChunkId drop) {
    Pmd2OpenOptions open;
    open.inspect = true;
    auto reader = Pmd2Reader::open(bytes, open);
    REQUIRE(reader.has_value());
    Pmd2Writer w(reader.value().header().kind, reader.value().header().width, reader.value().header().height);
    w.set_bind_key(reader.value().header().bind_key);
    for (const Pmd2ChunkInfo& c : reader.value().chunks()) {
        if (c.id != drop) {
            REQUIRE(w.add_stored_chunk(c, reader.value().stored_bytes(c)).has_value());
        }
    }
    auto out = w.finish();
    REQUIRE(out.has_value());
    return std::move(out.value());
}

/// Copies a container giving the nature chunks a layout version this reader does not know.
Bytes with_future_version(const Bytes& bytes) {
    Pmd2OpenOptions open;
    open.inspect = true;
    auto reader = Pmd2Reader::open(bytes, open);
    REQUIRE(reader.has_value());
    Pmd2Writer w(reader.value().header().kind, reader.value().header().width, reader.value().header().height);
    w.set_bind_key(reader.value().header().bind_key);
    for (Pmd2ChunkInfo c : reader.value().chunks()) {
        if (c.id == kRivr || c.id == kRvec || c.id == kNreg || c.id == kNrtb) {
            c.version = 2;
        }
        REQUIRE(w.add_stored_chunk(c, reader.value().stored_bytes(c)).has_value());
    }
    auto out = w.finish();
    REQUIRE(out.has_value());
    return std::move(out.value());
}

}  // namespace

TEST_CASE("nature: a valid layer passes validation, on both fixtures") {
    for (const char* set : {"v2_migrated", "v2_lake"}) {
        const PhysicalLayer l = load_physical(set);
        const NatureLayer n = make_nature(l);
        CHECK(validate_nature(n, l).has_value());
        CHECK(validate_nature(NatureLayer{}, l).has_value());
    }
}

TEST_CASE("nature: round trip keeps every field and read_physical still works") {
    const PhysicalLayer l = load_physical("v2_migrated");
    const NatureLayer n = make_nature(l);
    const Bytes bytes = with_nature(l, n);

    auto physical = read_physical(bytes);
    REQUIRE(physical.has_value());
    CHECK(physical.value() == l);  // the nature chunks are optional and skipped

    auto back = read_nature(bytes, physical.value());
    REQUIRE(back.has_value());
    CHECK(back.value() == n);
    CHECK(back.value().regions->shift == 2);

    PhysicalReadOptions strict;
    strict.verify_raw = true;
    strict.threads = 3;
    CHECK(read_nature(bytes, physical.value(), strict).has_value());

    auto none = read_nature(serialize_physical(l).value(), l);
    REQUIRE(none.has_value());
    CHECK(none.value() == NatureLayer{});
}

TEST_CASE("nature: only one of the two parts, every shift") {
    const PhysicalLayer l = load_physical("v2_migrated");
    NatureLayer only_rivers;
    only_rivers.rivers = make_rivers(l);
    auto a = read_nature(with_nature(l, only_rivers), l);
    REQUIRE(a.has_value());
    CHECK(a.value() == only_rivers);

    for (std::uint8_t shift = 0; shift <= 3; ++shift) {
        NatureLayer only_regions;
        only_regions.regions = make_regions(l, shift);
        auto b = read_nature(with_nature(l, only_regions), l);
        REQUIRE(b.has_value());
        CHECK(b.value() == only_regions);
    }
}

TEST_CASE("nature: output does not depend on threads, content does not depend on the codec") {
    const PhysicalLayer l = load_physical("v2_migrated");
    const NatureLayer n = make_nature(l);
    Pmd2WriterOptions one;
    one.threads = 1;
    Pmd2WriterOptions many;
    many.threads = 4;
    CHECK(with_nature(l, n, one) == with_nature(l, n, many));

    Pmd2WriterOptions raw;
    raw.codec = Pmd2Codec::raw;
    const Bytes zstd_bytes = with_nature(l, n, one);
    const Bytes raw_bytes = with_nature(l, n, raw);
    CHECK(zstd_bytes != raw_bytes);
    Pmd2OpenOptions open;
    open.inspect = true;
    auto a = Pmd2Reader::open(zstd_bytes, open);
    auto b = Pmd2Reader::open(raw_bytes, open);
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());
    CHECK(a.value().header().content_id == b.value().header().content_id);
    CHECK(read_nature(raw_bytes, l).value() == n);
}

TEST_CASE("nature: attach keeps the physical chunks and the bind key and replaces a part") {
    const PhysicalLayer l = load_physical("v2_migrated");
    const Bytes plain = serialize_physical(l).value();
    const NatureLayer n = make_nature(l);
    auto attached = attach_nature(plain, n);
    REQUIRE(attached.has_value());

    Pmd2OpenOptions open;
    open.inspect = true;
    auto before = Pmd2Reader::open(plain, open);
    auto after = Pmd2Reader::open(attached.value(), open);
    REQUIRE(before.has_value());
    REQUIRE(after.has_value());
    CHECK(after.value().header().bind_key == before.value().header().bind_key);
    for (const Pmd2ChunkInfo& c : before.value().chunks()) {
        const Pmd2ChunkInfo* same = after.value().find(c.id);
        REQUIRE(same != nullptr);
        CHECK(same->raw_hash == c.raw_hash);
    }
    CHECK(after.value().chunks().size() == before.value().chunks().size() + 4);

    // Replacing the regions leaves the rivers alone.
    NatureLayer other;
    other.regions = make_regions(l, 1);
    auto replaced = attach_nature(attached.value(), other);
    REQUIRE(replaced.has_value());
    auto back = read_nature(replaced.value(), l);
    REQUIRE(back.has_value());
    CHECK(back.value().rivers == n.rivers);
    CHECK(back.value().regions == other.regions);
}

TEST_CASE("nature: file round trip") {
    const PhysicalLayer l = load_physical("v2_lake");
    const NatureLayer n = make_nature(l, 3);
    const fs::path file = fs::temp_directory_path() / "patriarch_nature_roundtrip.pmd";
    REQUIRE(write_physical_with_nature_file(file, l, n).has_value());
    auto physical = read_physical_file(file);
    REQUIRE(physical.has_value());
    auto back = read_nature_file(file, physical.value());
    REQUIRE(back.has_value());
    CHECK(back.value() == n);
    std::error_code ec;
    fs::remove(file, ec);
}

TEST_CASE("validate_rivers: detects broken rivers") {
    const PhysicalLayer l = load_physical("v2_migrated");
    const RiverLayer good = make_rivers(l);
    REQUIRE(validate_rivers(good, l).has_value());

    auto expect = [&](int code, const auto& mutate) {
        RiverLayer r = good;
        mutate(r);
        CHECK(code_of(validate_rivers(r, l)) == code);
    };
    expect(nature_error::kBadRiverRaster, [](RiverLayer& r) { r.rank_raster.pop_back(); });
    expect(nature_error::kBadRiverTable, [](RiverLayer& r) { r.rivers[1].id = 7; });
    expect(nature_error::kBadRiverTable, [](RiverLayer& r) { r.rivers[0].rank = 0; });
    expect(nature_error::kBadRiverTable, [](RiverLayer& r) { r.rivers[0].key = "Bad Key"; });
    expect(nature_error::kBadRiverTable, [](RiverLayer& r) { r.rivers[1].key = r.rivers[0].key; });
    expect(nature_error::kBadRiverTable, [](RiverLayer& r) { r.rivers[0].points.resize(1); });
    expect(nature_error::kBadRiverTable, [](RiverLayer& r) { r.rivers[0].points[2].width_class = 0; });
    expect(nature_error::kBadRiverGeometry, [&](RiverLayer& r) { r.rivers[0].points[1].x = -1; });
    expect(nature_error::kBadRiverGeometry,
           [&](RiverLayer& r) { r.rivers[0].points[1].y = static_cast<std::int32_t>(l.height * 16); });
    expect(nature_error::kBadRiverGeometry,
           [](RiverLayer& r) { r.rivers[0].points[3] = r.rivers[0].points[2]; });
    expect(nature_error::kRankNotMonotonic, [](RiverLayer& r) { r.rivers[0].points[4].width_class = 1; });
    expect(nature_error::kRankNotMonotonic, [](RiverLayer& r) { r.rivers[0].rank = 2; });

    // A sea pixel in RIVR and a sea vertex in RVEC.
    std::size_t sea_pixel = 0;
    while (l.is_land(sea_pixel)) {
        ++sea_pixel;
    }
    expect(nature_error::kRiverInSea, [&](RiverLayer& r) { r.rank_raster[sea_pixel] = 1; });
    expect(nature_error::kRiverInSea, [&](RiverLayer& r) {
        r.rivers[0].points[0] = point_at(static_cast<std::uint32_t>(sea_pixel % l.width),
                                         static_cast<std::uint32_t>(sea_pixel / l.width),
                                         1);
    });
}

TEST_CASE("validate_rivers: a lake is not the sea") {
    const PhysicalLayer l = load_physical("v2_lake");
    std::size_t lake_pixel = l.pixel_count();
    for (std::size_t i = 0; i < l.pixel_count(); ++i) {
        const WaterBody* b = l.water(l.water_body[i]);
        if (b != nullptr && b->kind == WaterKind::lake) {
            lake_pixel = i;
            break;
        }
    }
    REQUIRE(lake_pixel < l.pixel_count());
    RiverLayer r = make_rivers(l);
    r.rivers[0].points[0] = point_at(static_cast<std::uint32_t>(lake_pixel % l.width),
                                     static_cast<std::uint32_t>(lake_pixel / l.width),
                                     1);
    r.rank_raster[lake_pixel] = 1;
    CHECK(validate_rivers(r, l).has_value());
}

TEST_CASE("validate_regions: detects broken regions") {
    const PhysicalLayer l = load_physical("v2_migrated");
    const RegionLayer good = make_regions(l, 2);
    REQUIRE(validate_regions(good, l).has_value());

    auto expect = [&](int code, const auto& mutate) {
        RegionLayer r = good;
        mutate(r);
        CHECK(code_of(validate_regions(r, l)) == code);
    };
    expect(nature_error::kBadRegionRaster, [](RegionLayer& r) { r.shift = 4; });
    expect(nature_error::kBadRegionRaster, [](RegionLayer& r) { r.shift = 1; });
    expect(nature_error::kBadRegionRaster, [](RegionLayer& r) { r.raster.pop_back(); });
    expect(nature_error::kBadRegionTable, [](RegionLayer& r) { r.regions[1].id = 9; });
    expect(nature_error::kBadRegionTable,
           [](RegionLayer& r) { r.regions[0].kind = static_cast<RegionKind>(0); });
    expect(nature_error::kBadRegionTable, [](RegionLayer& r) { r.regions[0].key = "Upper-Case"; });
    expect(nature_error::kBadRegionTable, [](RegionLayer& r) { r.regions[2].key = r.regions[0].key; });
    expect(nature_error::kBadRegionTable, [](RegionLayer& r) { r.regions[0].name.clear(); });
    expect(nature_error::kBadRegionTable, [](RegionLayer& r) { r.regions[0].name = "tab\there"; });
    expect(nature_error::kBadRegionId, [](RegionLayer& r) { r.raster.back() = 4; });
    expect(nature_error::kBadRegionId, [](RegionLayer& r) {
        r.regions.push_back({4, RegionKind::orographic, "unused-region", "Unused"});
    });
}

TEST_CASE("nature: blob codecs round trip and reject truncation and trailing bytes") {
    const PhysicalLayer l = load_physical("v2_migrated");
    const RiverLayer rivers = make_rivers(l);
    const RegionLayer regions = make_regions(l, 2);

    const Bytes rvec = encode_river_vectors(rivers.rivers);
    auto d1 = decode_river_vectors(rvec);
    REQUIRE(d1.has_value());
    CHECK(d1.value() == rivers.rivers);
    for (std::size_t n = 0; n < rvec.size(); ++n) {
        CHECK_FALSE(decode_river_vectors(std::span<const std::uint8_t>(rvec.data(), n)).has_value());
    }
    Bytes trailing = rvec;
    trailing.push_back(0);
    CHECK(code_of(decode_river_vectors(trailing)) == nature_error::kBadChunk);
    CHECK(decode_river_vectors(trailing, true).has_value());

    const Bytes nrtb = encode_region_table(regions.regions);
    auto d2 = decode_region_table(nrtb);
    REQUIRE(d2.has_value());
    CHECK(d2.value() == regions.regions);
    for (std::size_t n = 0; n < nrtb.size(); ++n) {
        CHECK_FALSE(decode_region_table(std::span<const std::uint8_t>(nrtb.data(), n)).has_value());
    }
    trailing = nrtb;
    trailing.push_back(0);
    CHECK(code_of(decode_region_table(trailing)) == nature_error::kBadChunk);

    // A point count that promises more than the blob holds is rejected before any allocation.
    Bytes huge = {1, 0, 0, 0, 1, 0, 1, 0xff, 0xff, 0xff, 0x7f};
    CHECK_FALSE(decode_river_vectors(huge).has_value());
    huge = {1, 0, 0, 0, 1, 0, 1, 0x10, 0x00, 0x00, 0x00};
    CHECK(code_of(decode_river_vectors(huge)) == nature_error::kBadChunk);
}

TEST_CASE("nature reader: corruption, truncation and structure") {
    const PhysicalLayer l = load_physical("v2_migrated");
    const NatureLayer n = make_nature(l);
    const Bytes bytes = with_nature(l, n);

    Pmd2OpenOptions open;
    open.inspect = true;
    auto reader = Pmd2Reader::open(bytes, open);
    REQUIRE(reader.has_value());

    // A flipped byte inside every nature chunk is found when the file is opened.
    for (const Pmd2ChunkId id : {kRivr, kRvec, kNreg, kNrtb}) {
        const Pmd2ChunkInfo* info = reader.value().find(id);
        REQUIRE(info != nullptr);
        Bytes bad = bytes;
        bad[static_cast<std::size_t>(info->offset + info->stored_size / 2)] ^= 0x5A;
        const auto r = read_nature(bad, l);
        CHECK_FALSE(r.has_value());
    }
    // Truncation anywhere is caught by file_size / header checks.
    for (const std::size_t cut : {std::size_t{10}, std::size_t{70}, bytes.size() / 2, bytes.size() - 1}) {
        const auto r = read_nature(std::span<const std::uint8_t>(bytes.data(), cut), l);
        CHECK_FALSE(r.has_value());
    }
    // Half a pair is an error, not silently half the data.
    CHECK(code_of(read_nature(without_chunk(bytes, kRvec), l)) == nature_error::kMissingChunk);
    CHECK(code_of(read_nature(without_chunk(bytes, kRivr), l)) == nature_error::kMissingChunk);
    CHECK(code_of(read_nature(without_chunk(bytes, kNreg), l)) == nature_error::kMissingChunk);
    CHECK(code_of(read_nature(without_chunk(bytes, kNrtb), l)) == nature_error::kMissingChunk);
    // Chunks of a newer layout version are skipped like any unknown optional chunk.
    auto future = read_nature(with_future_version(bytes), l);
    REQUIRE(future.has_value());
    CHECK(future.value() == NatureLayer{});
}

TEST_CASE("nature reader: rejects a layer that does not fit the physical layer") {
    const PhysicalLayer l = load_physical("v2_migrated");
    const PhysicalLayer lake = load_physical("v2_lake");
    const Bytes bytes = with_nature(l, make_nature(l));
    if (lake.width != l.width || lake.height != l.height) {
        CHECK_FALSE(read_nature(bytes, lake).has_value());
    }
    NatureLayer bad = make_nature(l);
    bad.rivers->rivers[0].rank = 9;
    CHECK(code_of(attach_nature(serialize_physical(l).value(), bad)) == nature_error::kRankNotMonotonic);
}

TEST_CASE("nature: a provinces container is not a physical one") {
    const PhysicalLayer l = load_physical("v2_migrated");
    CHECK(code_of(read_nature_file(map_file("v2_migrated", "provinces.pmd"), l)) == nature_error::kWrongKind);
}
