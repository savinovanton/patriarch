#include "pat/mapdata/nature_layer.hpp"

#include "pat/core/bytes.hpp"

#include <algorithm>
#include <array>
#include <set>
#include <utility>

#include "layer_codec.hpp"

namespace pat::mapdata {

namespace {

using codec::get_str;
using codec::put_str;
using core::Error;
using core::Result;

constexpr Pmd2ChunkId kRivr = pmd2_chunk_id('R', 'I', 'V', 'R');
constexpr Pmd2ChunkId kRvec = pmd2_chunk_id('R', 'V', 'E', 'C');
constexpr Pmd2ChunkId kNreg = pmd2_chunk_id('N', 'R', 'E', 'G');
constexpr Pmd2ChunkId kNrtb = pmd2_chunk_id('N', 'R', 'T', 'B');

constexpr std::size_t kPointBytes = 9;  // i32 x, i32 y, u8 width class
constexpr std::uint8_t kMaxShift = 3;

Error err(int code, std::string message) {
    return Error{code, std::move(message)};
}

std::string chunk_text(Pmd2ChunkId id) {
    return pmd2_chunk_id_string(id);
}

Result<void, Error> finish_blob(const core::ByteReader& r, bool allow_trailing, Pmd2ChunkId id) {
    using R = Result<void, Error>;
    if (!r.ok()) {
        return R::err(err(nature_error::kBadChunk, chunk_text(id) + ": truncated"));
    }
    if (!allow_trailing && !r.at_end()) {
        return R::err(err(nature_error::kBadChunk, chunk_text(id) + ": trailing bytes"));
    }
    return R::ok();
}

bool printable_name(const std::string& s) {
    return !s.empty() && s.size() <= kMaxRegionNameLength &&
           std::all_of(s.begin(), s.end(), [](char c) { return c >= 0x20 && c <= 0x7e; });
}

/// 1 for every water id that is a sea or an inland sea (index 0 = land is 0).
std::vector<std::uint8_t> salt_water_ids(const PhysicalLayer& physical) {
    std::vector<std::uint8_t> salt(physical.water_bodies.size() + 1, 0);
    for (const WaterBody& b : physical.water_bodies) {
        if (b.id < salt.size() && b.kind != WaterKind::lake) {
            salt[b.id] = 1;
        }
    }
    return salt;
}

bool in_salt_water(const PhysicalLayer& physical, const std::vector<std::uint8_t>& salt, std::size_t pixel) {
    const std::uint16_t id = physical.water_body[pixel];
    return id != 0 && id < salt.size() && salt[id] != 0;
}

}  // namespace

std::uint32_t region_raster_extent(std::uint32_t size, std::uint8_t shift) noexcept {
    const std::uint32_t step = 1U << shift;
    return (size + step - 1U) / step;
}

// ---- validation ----------------------------------------------------------------------------------

Result<void, Error> validate_rivers(const RiverLayer& rivers, const PhysicalLayer& physical) {
    using R = Result<void, Error>;
    if (rivers.rank_raster.size() != physical.pixel_count()) {
        return R::err(err(nature_error::kBadRiverRaster, "RIVR does not match the grid size"));
    }
    const std::vector<std::uint8_t> salt = salt_water_ids(physical);
    for (std::size_t i = 0; i < rivers.rank_raster.size(); ++i) {
        if (rivers.rank_raster[i] != 0 && in_salt_water(physical, salt, i)) {
            return R::err(err(nature_error::kRiverInSea, "RIVR: a river pixel lies in the sea"));
        }
    }
    if (rivers.rivers.size() > 65535U) {
        return R::err(err(nature_error::kBadRiverTable, "RVEC: too many rivers"));
    }
    const std::int64_t limit_x = static_cast<std::int64_t>(physical.width) * kRiverSubpixel;
    const std::int64_t limit_y = static_cast<std::int64_t>(physical.height) * kRiverSubpixel;
    std::set<std::string> keys;
    std::size_t vertices = 0;
    for (std::size_t i = 0; i < rivers.rivers.size(); ++i) {
        const River& river = rivers.rivers[i];
        const std::string who = "RVEC river " + std::to_string(i + 1) + ": ";
        if (river.id != i + 1) {
            return R::err(err(nature_error::kBadRiverTable, who + "ids must be dense 1..N"));
        }
        if (river.rank == 0) {
            return R::err(err(nature_error::kBadRiverTable, who + "rank must be 1..255"));
        }
        if (!codec::valid_ascii_key(river.key, kMaxRiverKeyLength) || !keys.insert(river.key).second) {
            return R::err(err(nature_error::kBadRiverTable, who + "invalid or duplicate key"));
        }
        if (river.points.size() < 2 || river.points.size() > kMaxRiverPoints) {
            return R::err(err(nature_error::kBadRiverTable,
                              who + "needs 2.." + std::to_string(kMaxRiverPoints) + " points"));
        }
        vertices += river.points.size();
        if (vertices > kMaxRiverVertices) {
            return R::err(err(nature_error::kBadRiverTable, "RVEC: too many vertices"));
        }
        for (std::size_t k = 0; k < river.points.size(); ++k) {
            const RiverPoint& p = river.points[k];
            if (p.x < 0 || p.y < 0 || p.x >= limit_x || p.y >= limit_y) {
                return R::err(err(nature_error::kBadRiverGeometry, who + "vertex outside the grid"));
            }
            if (p.width_class == 0) {
                return R::err(err(nature_error::kBadRiverTable, who + "width class must be 1..255"));
            }
            if (k > 0) {
                const RiverPoint& q = river.points[k - 1];
                if (p.x == q.x && p.y == q.y) {
                    return R::err(err(nature_error::kBadRiverGeometry, who + "repeated vertex"));
                }
                if (p.width_class < q.width_class) {
                    return R::err(
                        err(nature_error::kRankNotMonotonic, who + "width class decreases downstream"));
                }
            }
            const std::size_t pixel = static_cast<std::size_t>(p.y / kRiverSubpixel) * physical.width +
                                      static_cast<std::size_t>(p.x / kRiverSubpixel);
            if (in_salt_water(physical, salt, pixel)) {
                return R::err(err(nature_error::kRiverInSea, who + "vertex lies in the sea"));
            }
        }
        if (river.points.back().width_class != river.rank) {
            return R::err(
                err(nature_error::kRankNotMonotonic, who + "rank differs from the mouth width class"));
        }
    }
    return R::ok();
}

Result<void, Error> validate_regions(const RegionLayer& regions, const PhysicalLayer& physical) {
    using R = Result<void, Error>;
    if (regions.shift > kMaxShift) {
        return R::err(err(nature_error::kBadRegionRaster, "NREG: shift above 3"));
    }
    const std::size_t expected =
        static_cast<std::size_t>(region_raster_extent(physical.width, regions.shift)) *
        region_raster_extent(physical.height, regions.shift);
    if (regions.raster.size() != expected) {
        return R::err(err(nature_error::kBadRegionRaster, "NREG does not match the grid size for its shift"));
    }
    if (regions.regions.size() > 255U) {
        return R::err(err(nature_error::kBadRegionTable, "NRTB: too many regions"));
    }
    std::set<std::string> keys;
    for (std::size_t i = 0; i < regions.regions.size(); ++i) {
        const NaturalRegion& r = regions.regions[i];
        const std::string who = "NRTB region " + std::to_string(i + 1) + ": ";
        if (r.id != i + 1) {
            return R::err(err(nature_error::kBadRegionTable, who + "ids must be dense 1..N"));
        }
        if (r.kind != RegionKind::climate_zone && r.kind != RegionKind::orographic) {
            return R::err(err(nature_error::kBadRegionTable, who + "unknown kind"));
        }
        if (!codec::valid_ascii_key(r.key, kMaxRegionKeyLength) || !keys.insert(r.key).second) {
            return R::err(err(nature_error::kBadRegionTable, who + "invalid or duplicate key"));
        }
        if (!printable_name(r.name)) {
            return R::err(err(nature_error::kBadRegionTable, who + "invalid name"));
        }
    }
    std::array<bool, 256> used{};
    for (const std::uint8_t id : regions.raster) {
        if (id > regions.regions.size()) {
            return R::err(err(nature_error::kBadRegionId, "NREG: id without a table entry"));
        }
        used[id] = true;
    }
    for (std::size_t i = 0; i < regions.regions.size(); ++i) {
        if (!used[i + 1]) {
            return R::err(
                err(nature_error::kBadRegionId, "NRTB region " + std::to_string(i + 1) + " has no pixels"));
        }
    }
    return R::ok();
}

Result<void, Error> validate_nature(const NatureLayer& nature, const PhysicalLayer& physical) {
    if (nature.rivers) {
        if (auto ok = validate_rivers(*nature.rivers, physical); !ok) {
            return ok;
        }
    }
    if (nature.regions) {
        if (auto ok = validate_regions(*nature.regions, physical); !ok) {
            return ok;
        }
    }
    return Result<void, Error>::ok();
}

// ---- blob codecs ---------------------------------------------------------------------------------

std::vector<std::uint8_t> encode_river_vectors(std::span<const River> rivers) {
    core::ByteWriter w;
    w.u32(static_cast<std::uint32_t>(rivers.size()));
    for (const River& r : rivers) {
        w.u16(r.id);
        w.u8(r.rank);
        w.u32(static_cast<std::uint32_t>(r.points.size()));
        for (const RiverPoint& p : r.points) {
            w.i32(p.x);
            w.i32(p.y);
            w.u8(p.width_class);
        }
        put_str(w, r.key);
    }
    return w.take();
}

Result<std::vector<River>, Error> decode_river_vectors(std::span<const std::uint8_t> bytes,
                                                       bool allow_trailing) {
    using R = Result<std::vector<River>, Error>;
    core::ByteReader r(bytes);
    const std::uint32_t count = r.u32();
    if (r.ok() && count > 65535U) {
        return R::err(err(nature_error::kBadRiverTable, "RVEC: too many rivers"));
    }
    std::vector<River> rivers;
    std::size_t vertices = 0;
    for (std::uint32_t i = 0; i < count && r.ok(); ++i) {
        River river;
        river.id = r.u16();
        river.rank = r.u8();
        const std::uint32_t n = r.u32();
        if (!r.ok()) {
            break;
        }
        vertices += n;
        if (n > kMaxRiverPoints || vertices > kMaxRiverVertices) {
            return R::err(err(nature_error::kBadRiverTable, "RVEC: too many points"));
        }
        if (static_cast<std::uint64_t>(n) * kPointBytes > r.remaining()) {
            r.fail();
            break;
        }
        river.points.resize(n);
        for (RiverPoint& p : river.points) {
            p.x = r.i32();
            p.y = r.i32();
            p.width_class = r.u8();
        }
        river.key = get_str(r);
        rivers.push_back(std::move(river));
    }
    if (auto ok = finish_blob(r, allow_trailing, kRvec); !ok) {
        return R::err(ok.error());
    }
    return R::ok(std::move(rivers));
}

std::vector<std::uint8_t> encode_region_table(std::span<const NaturalRegion> regions) {
    core::ByteWriter w;
    w.u16(static_cast<std::uint16_t>(regions.size()));
    for (const NaturalRegion& n : regions) {
        w.u8(n.id);
        w.u8(static_cast<std::uint8_t>(n.kind));
        put_str(w, n.key);
        put_str(w, n.name);
    }
    return w.take();
}

Result<std::vector<NaturalRegion>, Error> decode_region_table(std::span<const std::uint8_t> bytes,
                                                              bool allow_trailing) {
    using R = Result<std::vector<NaturalRegion>, Error>;
    core::ByteReader r(bytes);
    const std::uint16_t count = r.u16();
    std::vector<NaturalRegion> regions;
    for (std::uint16_t i = 0; i < count && r.ok(); ++i) {
        NaturalRegion n;
        n.id = r.u8();
        n.kind = static_cast<RegionKind>(r.u8());
        n.key = get_str(r);
        n.name = get_str(r);
        regions.push_back(std::move(n));
    }
    if (auto ok = finish_blob(r, allow_trailing, kNrtb); !ok) {
        return R::err(ok.error());
    }
    return R::ok(std::move(regions));
}

// ---- write ---------------------------------------------------------------------------------------

namespace {

bool is_nature_chunk(Pmd2ChunkId id, const NatureLayer& nature) {
    return (nature.rivers && (id == kRivr || id == kRvec)) ||
           (nature.regions && (id == kNreg || id == kNrtb));
}

Result<Pmd2Writer, Error> build_attached(std::span<const std::uint8_t> physical_bytes,
                                         const NatureLayer& nature,
                                         const Pmd2WriterOptions& options) {
    using R = Result<Pmd2Writer, Error>;
    Pmd2OpenOptions open;
    open.inspect = true;
    auto reader = Pmd2Reader::open(physical_bytes, open);
    if (!reader) {
        return R::err(reader.error());
    }
    const Pmd2Header& head = reader.value().header();
    if (head.kind != Pmd2Kind::physical) {
        return R::err(err(nature_error::kWrongKind, "not a physical PMD2 container"));
    }
    auto physical = read_physical(physical_bytes);
    if (!physical) {
        return R::err(physical.error());
    }
    if (auto ok = validate_nature(nature, physical.value()); !ok) {
        return R::err(ok.error());
    }

    Pmd2Writer writer(Pmd2Kind::physical, head.width, head.height, options);
    writer.set_bind_key(head.bind_key);
    for (const Pmd2ChunkInfo& info : reader.value().chunks()) {
        if (is_nature_chunk(info.id, nature)) {
            continue;
        }
        if (auto ok = writer.add_stored_chunk(info, reader.value().stored_bytes(info)); !ok) {
            return R::err(ok.error());
        }
    }
    Pmd2ChunkSpec spec;
    spec.critical = false;
    const Pmd2RasterParams full;
    if (nature.rivers) {
        spec.id = kRivr;
        if (auto ok =
                writer.add_raster(spec, full, std::span<const std::uint8_t>(nature.rivers->rank_raster));
            !ok) {
            return R::err(ok.error());
        }
        spec.id = kRvec;
        if (auto ok = writer.add_blob(spec, encode_river_vectors(nature.rivers->rivers)); !ok) {
            return R::err(ok.error());
        }
    }
    if (nature.regions) {
        Pmd2RasterParams coarse;
        coarse.shift = nature.regions->shift;
        spec.id = kNreg;
        if (auto ok = writer.add_raster(spec, coarse, std::span<const std::uint8_t>(nature.regions->raster));
            !ok) {
            return R::err(ok.error());
        }
        spec.id = kNrtb;
        if (auto ok = writer.add_blob(spec, encode_region_table(nature.regions->regions)); !ok) {
            return R::err(ok.error());
        }
    }
    return R::ok(std::move(writer));
}

}  // namespace

Result<std::vector<std::uint8_t>, Error> attach_nature(std::span<const std::uint8_t> physical_bytes,
                                                       const NatureLayer& nature,
                                                       const Pmd2WriterOptions& options) {
    using R = Result<std::vector<std::uint8_t>, Error>;
    auto writer = build_attached(physical_bytes, nature, options);
    if (!writer) {
        return R::err(writer.error());
    }
    return writer.value().finish();
}

Result<std::vector<std::uint8_t>, Error> serialize_physical_with_nature(const PhysicalLayer& layer,
                                                                        const NatureLayer& nature,
                                                                        const Pmd2WriterOptions& options) {
    using R = Result<std::vector<std::uint8_t>, Error>;
    auto bytes = serialize_physical(layer, options);
    if (!bytes) {
        return R::err(bytes.error());
    }
    return attach_nature(bytes.value(), nature, options);
}

Result<void, Error> write_physical_with_nature_file(const std::filesystem::path& path,
                                                    const PhysicalLayer& layer,
                                                    const NatureLayer& nature,
                                                    const Pmd2WriterOptions& options) {
    using R = Result<void, Error>;
    auto bytes = serialize_physical(layer, options);
    if (!bytes) {
        return R::err(bytes.error());
    }
    auto writer = build_attached(bytes.value(), nature, options);
    if (!writer) {
        return R::err(writer.error());
    }
    return writer.value().write_file(path);
}

// ---- read ----------------------------------------------------------------------------------------

namespace {

/// An optional chunk of a version this reader does not know is skipped (ADR-0007 section 2.4).
const Pmd2ChunkInfo* usable(const Pmd2Reader& reader, Pmd2ChunkId id) {
    const Pmd2ChunkInfo* info = reader.find(id);
    return (info != nullptr && info->version <= 1) ? info : nullptr;
}

Result<NatureLayer, Error>
read_from(const Pmd2Reader& reader, const PhysicalLayer& physical, const PhysicalReadOptions& options) {
    using R = Result<NatureLayer, Error>;
    const Pmd2Header& head = reader.header();
    if (head.kind != Pmd2Kind::physical) {
        return R::err(err(nature_error::kWrongKind, "not a physical PMD2 container"));
    }
    const bool allow_trailing = head.minor > kPmd2KnownMinor;
    NatureLayer nature;

    const Pmd2ChunkInfo* rivr = usable(reader, kRivr);
    const Pmd2ChunkInfo* rvec = usable(reader, kRvec);
    if ((rivr == nullptr) != (rvec == nullptr)) {
        return R::err(err(nature_error::kMissingChunk, "RIVR and RVEC must come together"));
    }
    if (rivr != nullptr) {
        auto info = reader.raster_info(kRivr);
        if (!info) {
            return R::err(info.error());
        }
        if (info.value().elem_bytes != 1 || info.value().shift != 0 || info.value().width != head.width ||
            info.value().height != head.height) {
            return R::err(err(nature_error::kBadRiverRaster, "RIVR: not a full-resolution u8 raster"));
        }
        auto raster = reader.read_raster_u8(kRivr, 0, options.threads);
        if (!raster) {
            return R::err(raster.error());
        }
        auto blob = reader.read_blob(kRvec);
        if (!blob) {
            return R::err(blob.error());
        }
        auto vectors = decode_river_vectors(blob.value(), allow_trailing);
        if (!vectors) {
            return R::err(vectors.error());
        }
        RiverLayer rivers;
        rivers.rank_raster = std::move(raster.value());
        rivers.rivers = std::move(vectors.value());
        nature.rivers = std::move(rivers);
    }

    const Pmd2ChunkInfo* nreg = usable(reader, kNreg);
    const Pmd2ChunkInfo* nrtb = usable(reader, kNrtb);
    if ((nreg == nullptr) != (nrtb == nullptr)) {
        return R::err(err(nature_error::kMissingChunk, "NREG and NRTB must come together"));
    }
    if (nreg != nullptr) {
        auto info = reader.raster_info(kNreg);
        if (!info) {
            return R::err(info.error());
        }
        if (info.value().elem_bytes != 1) {
            return R::err(err(nature_error::kBadRegionRaster, "NREG: not a u8 raster"));
        }
        auto raster = reader.read_raster_u8(kNreg, 0, options.threads);
        if (!raster) {
            return R::err(raster.error());
        }
        auto blob = reader.read_blob(kNrtb);
        if (!blob) {
            return R::err(blob.error());
        }
        auto table = decode_region_table(blob.value(), allow_trailing);
        if (!table) {
            return R::err(table.error());
        }
        RegionLayer regions;
        regions.shift = info.value().shift;
        regions.raster = std::move(raster.value());
        regions.regions = std::move(table.value());
        nature.regions = std::move(regions);
    }

    if (auto ok = validate_nature(nature, physical); !ok) {
        return R::err(ok.error());
    }
    return R::ok(std::move(nature));
}

Pmd2OpenOptions open_options(const PhysicalReadOptions& options) {
    Pmd2OpenOptions o;
    o.inspect = true;  // the physical chunks are the business of read_physical; here they are only skipped
    o.verify_raw = options.verify_raw;
    o.threads = options.threads;
    return o;
}

}  // namespace

Result<NatureLayer, Error> read_nature(std::span<const std::uint8_t> physical_bytes,
                                       const PhysicalLayer& physical,
                                       const PhysicalReadOptions& options) {
    using R = Result<NatureLayer, Error>;
    auto reader = Pmd2Reader::open(physical_bytes, open_options(options));
    if (!reader) {
        return R::err(reader.error());
    }
    return read_from(reader.value(), physical, options);
}

Result<NatureLayer, Error> read_nature_file(const std::filesystem::path& path,
                                            const PhysicalLayer& physical,
                                            const PhysicalReadOptions& options) {
    using R = Result<NatureLayer, Error>;
    auto reader = Pmd2Reader::open_file(path, open_options(options));
    if (!reader) {
        return R::err(reader.error());
    }
    return read_from(reader.value(), physical, options);
}

}  // namespace pat::mapdata
