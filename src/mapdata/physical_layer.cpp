#include "pat/mapdata/physical_layer.hpp"

#include "pat/core/bytes.hpp"
#include "pat/core/hash_stream.hpp"
#include "pat/mapdata/map_data.hpp"

#include <algorithm>
#include <limits>
#include <utility>

#include "layer_codec.hpp"

namespace pat::mapdata {

namespace {

using codec::get_str;
using codec::put_str;
using core::Error;
using core::Result;

constexpr Pmd2ChunkId kGrid = pmd2_chunk_id('G', 'R', 'I', 'D');
constexpr Pmd2ChunkId kGeor = pmd2_chunk_id('G', 'E', 'O', 'R');
constexpr Pmd2ChunkId kHght = pmd2_chunk_id('H', 'G', 'H', 'T');
constexpr Pmd2ChunkId kWbod = pmd2_chunk_id('W', 'B', 'O', 'D');
constexpr Pmd2ChunkId kWtbl = pmd2_chunk_id('W', 'T', 'B', 'L');
constexpr Pmd2ChunkId kMatl = pmd2_chunk_id('M', 'A', 'T', 'L');
constexpr Pmd2ChunkId kProv = pmd2_chunk_id('P', 'R', 'O', 'V');

constexpr std::int32_t kMaxLatUdeg = 90'000'000;
constexpr std::int32_t kMaxLonUdeg = 180'000'000;
constexpr std::int64_t kMaxPlaneMm = 100'000'000'000'000LL;  // 100 000 km: far beyond any planet
constexpr std::int64_t kMaxPixelMm = 1'000'000'000LL;        // 1000 km per pixel

Error err(int code, std::string message) {
    return Error{code, std::move(message)};
}

std::string chunk_text(Pmd2ChunkId id) {
    return pmd2_chunk_id_string(id);
}

// ---- string and blob helpers ---------------------------------------------------------------------

Result<void, Error> finish_blob(const core::ByteReader& r, bool allow_trailing, Pmd2ChunkId id) {
    using R = Result<void, Error>;
    if (!r.ok()) {
        return R::err(err(physical_error::kBadChunk, chunk_text(id) + ": truncated"));
    }
    if (!allow_trailing && !r.at_end()) {
        return R::err(err(physical_error::kBadChunk, chunk_text(id) + ": trailing bytes"));
    }
    return R::ok();
}

bool valid_key(const std::string& key) {
    return codec::valid_ascii_key(key, kMaxWaterKeyLength);
}

bool valid_prov_string(const std::string& s) {
    return s.size() <= kMaxProvStringLength &&
           std::none_of(s.begin(), s.end(), [](char c) { return static_cast<unsigned char>(c) < 0x20U; });
}

std::uint64_t raster_hash(std::span<const std::uint16_t> pixels) {
    core::HashStream h;
    for (const std::uint16_t v : pixels) {
        h.u16(v);
    }
    return h.value();
}

// ---- GRID ----------------------------------------------------------------------------------------

struct Grid {
    std::uint16_t version = 1;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint16_t sea_level = 0;
    std::uint32_t metres_per_unit_milli = 0;
    std::uint64_t seed = 0;
    std::uint16_t material_version = 0;
};

std::vector<std::uint8_t> encode_grid(const PhysicalLayer& l) {
    core::ByteWriter w;
    w.u16(1);
    w.u32(l.width);
    w.u32(l.height);
    w.u16(l.sea_level);
    w.u32(l.metres_per_unit_milli);
    w.u64(l.seed);
    w.u16(l.material_version);
    return w.take();
}

Result<Grid, Error> decode_grid(std::span<const std::uint8_t> bytes, bool allow_trailing) {
    using R = Result<Grid, Error>;
    core::ByteReader r(bytes);
    Grid g;
    g.version = r.u16();
    g.width = r.u32();
    g.height = r.u32();
    g.sea_level = r.u16();
    g.metres_per_unit_milli = r.u32();
    g.seed = r.u64();
    g.material_version = r.u16();
    if (auto ok = finish_blob(r, allow_trailing, kGrid); !ok) {
        return R::err(ok.error());
    }
    if (g.version != 1) {
        return R::err(err(physical_error::kBadGrid, "GRID: unsupported version"));
    }
    return R::ok(g);
}

}  // namespace

// ---- blob codecs ---------------------------------------------------------------------------------

std::vector<std::uint8_t> encode_georef(const GeoRef& g) {
    core::ByteWriter w;
    w.u16(g.version);
    w.u8(static_cast<std::uint8_t>(g.projection));
    w.u8(static_cast<std::uint8_t>(g.datum));
    w.i32(g.lat_origin_udeg);
    w.i32(g.lon_central_udeg);
    w.i32(g.lat_std1_udeg);
    w.i32(g.lat_std2_udeg);
    w.i64(g.false_easting_mm);
    w.i64(g.false_northing_mm);
    w.i64(g.origin_x_mm);
    w.i64(g.origin_y_mm);
    w.i64(g.pixel_size_x_mm);
    w.i64(g.pixel_size_y_mm);
    w.i32(g.bbox_min_lon_udeg);
    w.i32(g.bbox_max_lon_udeg);
    w.i32(g.bbox_min_lat_udeg);
    w.i32(g.bbox_max_lat_udeg);
    w.u32(g.epsg);
    put_str(w, g.proj_text);
    return w.take();
}

Result<GeoRef, Error> decode_georef(std::span<const std::uint8_t> bytes, bool allow_trailing) {
    using R = Result<GeoRef, Error>;
    core::ByteReader r(bytes);
    GeoRef g;
    g.version = r.u16();
    const std::uint8_t projection = r.u8();
    const std::uint8_t datum = r.u8();
    g.lat_origin_udeg = r.i32();
    g.lon_central_udeg = r.i32();
    g.lat_std1_udeg = r.i32();
    g.lat_std2_udeg = r.i32();
    g.false_easting_mm = r.i64();
    g.false_northing_mm = r.i64();
    g.origin_x_mm = r.i64();
    g.origin_y_mm = r.i64();
    g.pixel_size_x_mm = r.i64();
    g.pixel_size_y_mm = r.i64();
    g.bbox_min_lon_udeg = r.i32();
    g.bbox_max_lon_udeg = r.i32();
    g.bbox_min_lat_udeg = r.i32();
    g.bbox_max_lat_udeg = r.i32();
    g.epsg = r.u32();
    g.proj_text = get_str(r);
    if (auto ok = finish_blob(r, allow_trailing, kGeor); !ok) {
        return R::err(ok.error());
    }
    if (g.version != 1) {
        return R::err(err(physical_error::kBadGeoref, "GEOR: unsupported version"));
    }
    if (projection > static_cast<std::uint8_t>(Projection::equirectangular) ||
        datum > static_cast<std::uint8_t>(Datum::wgs84)) {
        return R::err(err(physical_error::kBadGeoref, "GEOR: unknown projection or datum"));
    }
    g.projection = static_cast<Projection>(projection);
    g.datum = static_cast<Datum>(datum);
    return R::ok(std::move(g));
}

std::vector<std::uint8_t> encode_water_table(std::span<const WaterBody> bodies) {
    core::ByteWriter w;
    w.u16(static_cast<std::uint16_t>(bodies.size()));
    for (const WaterBody& b : bodies) {
        w.u16(b.id);
        w.u8(static_cast<std::uint8_t>(b.kind));
        w.u8(b.flags);
        w.u16(b.surface_level);
        w.u32(b.area_px);
        put_str(w, b.key);
    }
    return w.take();
}

Result<std::vector<WaterBody>, Error> decode_water_table(std::span<const std::uint8_t> bytes,
                                                         bool allow_trailing) {
    using R = Result<std::vector<WaterBody>, Error>;
    core::ByteReader r(bytes);
    const std::uint16_t count = r.u16();
    std::vector<WaterBody> out;
    for (std::uint16_t i = 0; i < count && r.ok(); ++i) {
        WaterBody b;
        b.id = r.u16();
        const std::uint8_t kind = r.u8();
        b.flags = r.u8();
        b.surface_level = r.u16();
        b.area_px = r.u32();
        b.key = get_str(r);
        if (kind < static_cast<std::uint8_t>(WaterKind::sea) ||
            kind > static_cast<std::uint8_t>(WaterKind::inland_sea)) {
            return R::err(err(physical_error::kBadWaterTable, "WTBL: unknown water kind"));
        }
        b.kind = static_cast<WaterKind>(kind);
        out.push_back(std::move(b));
    }
    if (auto ok = finish_blob(r, allow_trailing, kWtbl); !ok) {
        return R::err(ok.error());
    }
    return R::ok(std::move(out));
}

std::vector<std::uint8_t> encode_provenance(const Provenance& p) {
    core::ByteWriter w;
    w.u16(p.provenance_version);
    put_str(w, p.builder);
    w.u32(p.manifest_version);
    w.u16(static_cast<std::uint16_t>(p.sources.size()));
    for (const ProvSource& s : p.sources) {
        put_str(w, s.source_id);
        put_str(w, s.source_version);
        w.u16(static_cast<std::uint16_t>(s.files.size()));
        for (const ProvFile& f : s.files) {
            put_str(w, f.path);
            w.bytes(f.sha256);
        }
    }
    w.u16(static_cast<std::uint16_t>(p.steps.size()));
    for (const ProvStep& s : p.steps) {
        put_str(w, s.step_id);
        w.u64(s.params_hash);
    }
    w.u16(static_cast<std::uint16_t>(p.corrections.size()));
    for (const ProvCorrection& c : p.corrections) {
        put_str(w, c.correction_id);
        w.u64(c.content_id);
    }
    return w.take();
}

Result<Provenance, Error> decode_provenance(std::span<const std::uint8_t> bytes, bool allow_trailing) {
    using R = Result<Provenance, Error>;
    core::ByteReader r(bytes);
    Provenance p;
    p.provenance_version = r.u16();
    p.builder = get_str(r);
    p.manifest_version = r.u32();
    const std::uint16_t source_count = r.u16();
    for (std::uint16_t i = 0; i < source_count && r.ok(); ++i) {
        ProvSource s;
        s.source_id = get_str(r);
        s.source_version = get_str(r);
        const std::uint16_t file_count = r.u16();
        for (std::uint16_t j = 0; j < file_count && r.ok(); ++j) {
            ProvFile f;
            f.path = get_str(r);
            const auto hash = r.bytes(f.sha256.size());
            if (hash.size() == f.sha256.size()) {
                std::copy(hash.begin(), hash.end(), f.sha256.begin());
            }
            s.files.push_back(std::move(f));
        }
        p.sources.push_back(std::move(s));
    }
    const std::uint16_t step_count = r.u16();
    for (std::uint16_t i = 0; i < step_count && r.ok(); ++i) {
        ProvStep s;
        s.step_id = get_str(r);
        s.params_hash = r.u64();
        p.steps.push_back(std::move(s));
    }
    const std::uint16_t correction_count = r.u16();
    for (std::uint16_t i = 0; i < correction_count && r.ok(); ++i) {
        ProvCorrection c;
        c.correction_id = get_str(r);
        c.content_id = r.u64();
        p.corrections.push_back(std::move(c));
    }
    if (auto ok = finish_blob(r, allow_trailing, kProv); !ok) {
        return R::err(ok.error());
    }
    if (p.provenance_version != 1) {
        return R::err(err(physical_error::kBadProvenance, "PROV: unsupported version"));
    }
    return R::ok(std::move(p));
}

// ---- coast key -----------------------------------------------------------------------------------

std::uint64_t coast_key(std::uint32_t width,
                        std::uint32_t height,
                        std::uint64_t georef_raw_hash,
                        std::uint64_t water_raw_hash) noexcept {
    core::HashStream h;
    h.u32(width);
    h.u32(height);
    h.u64(georef_raw_hash);
    h.u64(water_raw_hash);
    return h.value();
}

std::uint64_t coast_key(const PhysicalLayer& layer) {
    return coast_key(layer.width,
                     layer.height,
                     core::hash_bytes(encode_georef(layer.georef)),
                     raster_hash(layer.water_body));
}

Result<std::uint64_t, Error> coast_key(const Pmd2Reader& reader) {
    using R = Result<std::uint64_t, Error>;
    const Pmd2ChunkInfo* georef = reader.find(kGeor);
    const Pmd2ChunkInfo* water = reader.find(kWbod);
    if (georef == nullptr || water == nullptr) {
        return R::err(err(physical_error::kMissingChunk, "GEOR and WBOD are needed for the coast key"));
    }
    return R::ok(coast_key(reader.header().width, reader.header().height, georef->raw_hash, water->raw_hash));
}

// ---- validation ----------------------------------------------------------------------------------

Result<void, Error> validate_georef(const GeoRef& g, std::uint32_t width, std::uint32_t height) {
    using R = Result<void, Error>;
    const auto bad = [](std::string m) {
        return R::err(err(physical_error::kBadGeoref, "GEOR: " + std::move(m)));
    };
    if (g.version != 1) {
        return bad("unsupported version");
    }
    if (g.proj_text.size() > kMaxProjTextLength) {
        return bad("proj_text is too long");
    }
    if (g.projection == Projection::none) {
        const bool zero = g.datum == Datum::none && g.lat_origin_udeg == 0 && g.lon_central_udeg == 0 &&
                          g.lat_std1_udeg == 0 && g.lat_std2_udeg == 0 && g.false_easting_mm == 0 &&
                          g.false_northing_mm == 0 && g.origin_x_mm == 0 && g.origin_y_mm == 0 &&
                          g.pixel_size_x_mm == 0 && g.pixel_size_y_mm == 0 && g.bbox_min_lon_udeg == 0 &&
                          g.bbox_max_lon_udeg == 0 && g.bbox_min_lat_udeg == 0 && g.bbox_max_lat_udeg == 0 &&
                          g.epsg == 0 && g.proj_text.empty();
        return zero ? R::ok() : bad("projection none requires every other field to be zero");
    }
    if (g.projection > Projection::equirectangular) {
        return bad("unknown projection");
    }
    if (g.datum != Datum::wgs84) {
        return bad("a real projection needs a datum");
    }
    const auto lat_ok = [](std::int32_t v) { return v >= -kMaxLatUdeg && v <= kMaxLatUdeg; };
    const auto lon_ok = [](std::int32_t v) { return v >= -kMaxLonUdeg && v <= kMaxLonUdeg; };
    if (!lat_ok(g.lat_origin_udeg) || !lat_ok(g.lat_std1_udeg) || !lat_ok(g.lat_std2_udeg) ||
        !lon_ok(g.lon_central_udeg)) {
        return bad("projection angle out of range");
    }
    const auto plane_ok = [](std::int64_t v) { return v >= -kMaxPlaneMm && v <= kMaxPlaneMm; };
    if (!plane_ok(g.false_easting_mm) || !plane_ok(g.false_northing_mm) || !plane_ok(g.origin_x_mm) ||
        !plane_ok(g.origin_y_mm)) {
        return bad("plane coordinate out of range");
    }
    if (g.pixel_size_x_mm <= 0 || g.pixel_size_y_mm <= 0 || g.pixel_size_x_mm > kMaxPixelMm ||
        g.pixel_size_y_mm > kMaxPixelMm) {
        return bad("pixel size must be in 1 mm .. 1000 km");
    }
    // The raster extent in the plane must stay representable (width and height are <= 65535, so the
    // products fit an i64 by the pixel-size ceiling above).
    const std::int64_t extent_x = static_cast<std::int64_t>(width) * g.pixel_size_x_mm;
    const std::int64_t extent_y = static_cast<std::int64_t>(height) * g.pixel_size_y_mm;
    if (g.origin_x_mm + extent_x > kMaxPlaneMm || g.origin_y_mm - extent_y < -kMaxPlaneMm) {
        return bad("raster extent leaves the plane range");
    }
    if (!lon_ok(g.bbox_min_lon_udeg) || !lon_ok(g.bbox_max_lon_udeg) || !lat_ok(g.bbox_min_lat_udeg) ||
        !lat_ok(g.bbox_max_lat_udeg) || g.bbox_min_lon_udeg >= g.bbox_max_lon_udeg ||
        g.bbox_min_lat_udeg >= g.bbox_max_lat_udeg) {
        return bad("bounding box is empty or out of range");
    }
    return R::ok();
}

Result<void, Error> validate_provenance(const Provenance& p) {
    using R = Result<void, Error>;
    const auto bad = [](std::string m) {
        return R::err(err(physical_error::kBadProvenance, "PROV: " + std::move(m)));
    };
    constexpr std::size_t kMaxCount = std::numeric_limits<std::uint16_t>::max();
    if (p.provenance_version != 1) {
        return bad("unsupported version");
    }
    if (!valid_prov_string(p.builder)) {
        return bad("builder string invalid");
    }
    if (p.sources.size() > kMaxCount || p.steps.size() > kMaxCount || p.corrections.size() > kMaxCount) {
        return bad("too many entries");
    }
    for (const ProvSource& s : p.sources) {
        if (s.source_id.empty() || !valid_prov_string(s.source_id) || !valid_prov_string(s.source_version) ||
            s.files.size() > kMaxCount) {
            return bad("source entry invalid");
        }
        for (const ProvFile& f : s.files) {
            if (f.path.empty() || !valid_prov_string(f.path)) {
                return bad("file path invalid");
            }
        }
    }
    for (const ProvStep& s : p.steps) {
        if (s.step_id.empty() || !valid_prov_string(s.step_id)) {
            return bad("step id invalid");
        }
    }
    for (const ProvCorrection& c : p.corrections) {
        if (c.correction_id.empty() || !valid_prov_string(c.correction_id)) {
            return bad("correction id invalid");
        }
    }
    return R::ok();
}

namespace {

/// True if every water body is one 4-connected region; on failure `bad_id` is the first split body.
bool water_connected(const PhysicalLayer& l, std::uint16_t& bad_id) {
    const std::size_t n = l.pixel_count();
    std::vector<std::uint8_t> visited(n, 0);
    std::vector<std::uint8_t> seen(l.water_bodies.size() + 1, 0);
    std::vector<std::uint32_t> stack;
    const std::uint32_t w = l.width;
    for (std::size_t start = 0; start < n; ++start) {
        const std::uint16_t id = l.water_body[start];
        if (id == 0 || visited[start] != 0) {
            continue;
        }
        if (seen[id] != 0) {
            bad_id = id;
            return false;
        }
        seen[id] = 1;
        visited[start] = 1;
        stack.push_back(static_cast<std::uint32_t>(start));
        while (!stack.empty()) {
            const std::uint32_t i = stack.back();
            stack.pop_back();
            const std::uint32_t x = i % w;
            const auto visit = [&](std::uint32_t j) {
                if (visited[j] == 0 && l.water_body[j] == id) {
                    visited[j] = 1;
                    stack.push_back(j);
                }
            };
            if (x > 0) {
                visit(i - 1);
            }
            if (x + 1 < w) {
                visit(i + 1);
            }
            if (i >= w) {
                visit(i - w);
            }
            if (static_cast<std::size_t>(i) + w < n) {
                visit(i + w);
            }
        }
    }
    return true;
}

}  // namespace

std::vector<std::uint32_t> count_water_area(std::span<const std::uint16_t> water_body, std::size_t count) {
    std::vector<std::uint32_t> area(count, 0);
    for (const std::uint16_t id : water_body) {
        if (id != 0 && id <= count) {
            ++area[static_cast<std::size_t>(id) - 1];
        }
    }
    return area;
}

Result<void, Error> validate_physical(const PhysicalLayer& l) {
    using R = Result<void, Error>;
    const auto fail = [](int code, std::string m) { return R::err(err(code, std::move(m))); };

    if (l.width == 0 || l.height == 0 || l.width > kPmd2MaxDimension || l.height > kPmd2MaxDimension ||
        l.pixel_count() > kPmd2MaxPixels) {
        return fail(physical_error::kBadDimensions, "invalid map dimensions");
    }
    const std::size_t n = l.pixel_count();
    if (l.heights.size() != n || l.water_body.size() != n || l.materials.size() != n) {
        return fail(physical_error::kLayerSize, "a raster does not match width x height");
    }
    if (l.metres_per_unit_milli == 0) {
        return fail(physical_error::kBadGrid, "metres_per_unit_milli must be positive");
    }
    if (l.material_version != kMaterialSetVersion) {
        return fail(physical_error::kBadGrid, "unsupported material set version");
    }
    for (const std::uint8_t m : l.materials) {
        if (m >= kMaterialCount) {
            return fail(physical_error::kBadMaterial, "material out of range");
        }
    }

    // Water table: dense ids, known kinds, reserved flags, unique keys, seas at sea level.
    if (l.water_bodies.size() > std::numeric_limits<std::uint16_t>::max()) {
        return fail(physical_error::kBadWaterTable, "too many water bodies");
    }
    std::vector<std::string> keys;
    keys.reserve(l.water_bodies.size());
    for (std::size_t i = 0; i < l.water_bodies.size(); ++i) {
        const WaterBody& b = l.water_bodies[i];
        if (b.id != i + 1) {
            return fail(physical_error::kBadWaterTable, "water body ids must be dense and start at 1");
        }
        if (b.kind < WaterKind::sea || b.kind > WaterKind::inland_sea || b.flags != 0) {
            return fail(physical_error::kBadWaterTable, "water body kind or flags invalid");
        }
        if (!valid_key(b.key)) {
            return fail(physical_error::kBadWaterTable, "water body key invalid");
        }
        if (b.kind == WaterKind::sea && b.surface_level != l.sea_level) {
            return fail(physical_error::kSeaLevel, "a sea must have its surface at sea_level");
        }
        keys.push_back(b.key);
    }
    std::sort(keys.begin(), keys.end());
    if (std::adjacent_find(keys.begin(), keys.end()) != keys.end()) {
        return fail(physical_error::kBadWaterTable, "duplicate water body key");
    }

    // WBOD: ids known, bed not above the surface, areas match.
    std::vector<std::uint32_t> area(l.water_bodies.size(), 0);
    for (std::size_t i = 0; i < n; ++i) {
        const std::uint16_t id = l.water_body[i];
        if (id == 0) {
            continue;
        }
        if (id > l.water_bodies.size()) {
            return fail(physical_error::kBadWaterId, "WBOD id has no table entry");
        }
        if (l.heights[i] > l.water_bodies[static_cast<std::size_t>(id) - 1].surface_level) {
            return fail(physical_error::kBedAboveSurface, "water bed is above the water surface");
        }
        ++area[static_cast<std::size_t>(id) - 1];
    }
    for (std::size_t i = 0; i < area.size(); ++i) {
        if (area[i] == 0 || area[i] != l.water_bodies[i].area_px) {
            return fail(physical_error::kWaterArea, "water body area does not match the raster");
        }
    }
    std::uint16_t split = 0;
    if (!water_connected(l, split)) {
        return fail(physical_error::kWaterSplit, "water body " + std::to_string(split) + " is not connected");
    }

    if (auto ok = validate_georef(l.georef, l.width, l.height); !ok) {
        return ok;
    }
    if (l.provenance) {
        if (auto ok = validate_provenance(*l.provenance); !ok) {
            return ok;
        }
    }
    return R::ok();
}

// ---- write ---------------------------------------------------------------------------------------

namespace {

Result<void, Error> build(Pmd2Writer& w, const PhysicalLayer& l) {
    using R = Result<void, Error>;
    w.set_bind_key(coast_key(l));

    Pmd2ChunkSpec spec;
    spec.critical = true;
    const Pmd2RasterParams params;  // shift 0, tile 1024, delta transform

    spec.id = kGrid;
    if (auto ok = w.add_blob(spec, encode_grid(l)); !ok) {
        return ok;
    }
    spec.id = kGeor;
    if (auto ok = w.add_blob(spec, encode_georef(l.georef)); !ok) {
        return ok;
    }
    spec.id = kHght;
    if (auto ok = w.add_raster(spec, params, std::span<const std::uint16_t>(l.heights)); !ok) {
        return ok;
    }
    spec.id = kWbod;
    if (auto ok = w.add_raster(spec, params, std::span<const std::uint16_t>(l.water_body)); !ok) {
        return ok;
    }
    spec.id = kWtbl;
    if (auto ok = w.add_blob(spec, encode_water_table(l.water_bodies)); !ok) {
        return ok;
    }
    spec.id = kMatl;
    spec.version = l.material_version;
    if (auto ok = w.add_raster(spec, params, std::span<const std::uint8_t>(l.materials)); !ok) {
        return ok;
    }
    if (l.provenance) {
        Pmd2ChunkSpec prov;
        prov.id = kProv;
        prov.critical = false;
        if (auto ok = w.add_blob(prov, encode_provenance(*l.provenance)); !ok) {
            return ok;
        }
    }
    return R::ok();
}

}  // namespace

Result<std::vector<std::uint8_t>, Error> serialize_physical(const PhysicalLayer& layer,
                                                            const Pmd2WriterOptions& options) {
    using R = Result<std::vector<std::uint8_t>, Error>;
    if (auto ok = validate_physical(layer); !ok) {
        return R::err(ok.error());
    }
    Pmd2Writer writer(Pmd2Kind::physical, layer.width, layer.height, options);
    if (auto ok = build(writer, layer); !ok) {
        return R::err(ok.error());
    }
    return writer.finish();
}

Result<void, Error> write_physical_file(const std::filesystem::path& path,
                                        const PhysicalLayer& layer,
                                        const Pmd2WriterOptions& options) {
    using R = Result<void, Error>;
    if (auto ok = validate_physical(layer); !ok) {
        return ok;
    }
    Pmd2Writer writer(Pmd2Kind::physical, layer.width, layer.height, options);
    if (auto ok = build(writer, layer); !ok) {
        return R::err(ok.error());
    }
    return writer.write_file(path);
}

// ---- read ----------------------------------------------------------------------------------------

namespace {

Pmd2OpenOptions open_options(const PhysicalReadOptions& options) {
    Pmd2OpenOptions o;
    o.known = {
        {kGrid, 1}, {kGeor, 1}, {kHght, 1}, {kWbod, 1}, {kWtbl, 1}, {kMatl, kMaterialSetVersion}, {kProv, 1}};
    o.verify_raw = options.verify_raw;
    o.threads = options.threads;
    return o;
}

Result<std::vector<std::uint8_t>, Error> require_blob(const Pmd2Reader& reader, Pmd2ChunkId id) {
    using R = Result<std::vector<std::uint8_t>, Error>;
    if (reader.find(id) == nullptr) {
        return R::err(err(physical_error::kMissingChunk, "missing chunk " + chunk_text(id)));
    }
    return reader.read_blob(id);
}

Result<std::vector<std::uint16_t>, Error>
require_raster16(const Pmd2Reader& reader, Pmd2ChunkId id, unsigned threads) {
    using R = Result<std::vector<std::uint16_t>, Error>;
    if (reader.find(id) == nullptr) {
        return R::err(err(physical_error::kMissingChunk, "missing chunk " + chunk_text(id)));
    }
    auto info = reader.raster_info(id);
    if (!info) {
        return R::err(info.error());
    }
    if (info.value().shift != 0 || info.value().width != reader.header().width ||
        info.value().height != reader.header().height) {
        return R::err(err(physical_error::kLayerSize, chunk_text(id) + ": raster is not full resolution"));
    }
    return reader.read_raster_u16(id, 0, threads);
}

Result<PhysicalLayer, Error> read_from(const Pmd2Reader& reader, const PhysicalReadOptions& options) {
    using R = Result<PhysicalLayer, Error>;
    const Pmd2Header& head = reader.header();
    if (head.kind != Pmd2Kind::physical) {
        return R::err(err(physical_error::kWrongKind, "not a physical PMD2 container"));
    }
    const bool allow_trailing = head.minor > kPmd2KnownMinor;

    PhysicalLayer layer;
    layer.width = head.width;
    layer.height = head.height;

    auto grid_bytes = require_blob(reader, kGrid);
    if (!grid_bytes) {
        return R::err(grid_bytes.error());
    }
    auto grid = decode_grid(grid_bytes.value(), allow_trailing);
    if (!grid) {
        return R::err(grid.error());
    }
    if (grid.value().width != head.width || grid.value().height != head.height) {
        return R::err(err(physical_error::kGridHeader, "GRID size differs from the header"));
    }
    layer.sea_level = grid.value().sea_level;
    layer.metres_per_unit_milli = grid.value().metres_per_unit_milli;
    layer.seed = grid.value().seed;
    layer.material_version = grid.value().material_version;

    auto geor_bytes = require_blob(reader, kGeor);
    if (!geor_bytes) {
        return R::err(geor_bytes.error());
    }
    auto georef = decode_georef(geor_bytes.value(), allow_trailing);
    if (!georef) {
        return R::err(georef.error());
    }
    layer.georef = std::move(georef.value());

    auto table_bytes = require_blob(reader, kWtbl);
    if (!table_bytes) {
        return R::err(table_bytes.error());
    }
    auto table = decode_water_table(table_bytes.value(), allow_trailing);
    if (!table) {
        return R::err(table.error());
    }
    layer.water_bodies = std::move(table.value());

    auto heights = require_raster16(reader, kHght, options.threads);
    if (!heights) {
        return R::err(heights.error());
    }
    layer.heights = std::move(heights.value());
    auto water = require_raster16(reader, kWbod, options.threads);
    if (!water) {
        return R::err(water.error());
    }
    layer.water_body = std::move(water.value());

    const Pmd2ChunkInfo* matl = reader.find(kMatl);
    if (matl == nullptr) {
        return R::err(err(physical_error::kMissingChunk, "missing chunk MATL"));
    }
    if (matl->version != layer.material_version) {
        return R::err(err(physical_error::kBadGrid, "MATL version differs from the GRID material set"));
    }
    auto matl_info = reader.raster_info(kMatl);
    if (!matl_info) {
        return R::err(matl_info.error());
    }
    if (matl_info.value().shift != 0 || matl_info.value().width != head.width ||
        matl_info.value().height != head.height) {
        return R::err(err(physical_error::kLayerSize, "MATL: raster is not full resolution"));
    }
    auto materials = reader.read_raster_u8(kMatl, 0, options.threads);
    if (!materials) {
        return R::err(materials.error());
    }
    layer.materials = std::move(materials.value());

    if (const Pmd2ChunkInfo* prov = reader.find(kProv); prov != nullptr && prov->understood) {
        auto bytes = reader.read_blob(kProv);
        if (!bytes) {
            return R::err(bytes.error());
        }
        auto decoded = decode_provenance(bytes.value(), allow_trailing);
        if (!decoded) {
            return R::err(decoded.error());
        }
        layer.provenance = std::move(decoded.value());
    }

    if (auto ok = validate_physical(layer); !ok) {
        return R::err(ok.error());
    }
    if (head.bind_key != coast_key(layer)) {
        return R::err(err(physical_error::kBindKey, "header bind_key differs from the coast key"));
    }
    return R::ok(std::move(layer));
}

}  // namespace

Result<PhysicalLayer, Error> read_physical(std::span<const std::uint8_t> bytes,
                                           const PhysicalReadOptions& options) {
    using R = Result<PhysicalLayer, Error>;
    auto reader = Pmd2Reader::open(bytes, open_options(options));
    if (!reader) {
        return R::err(reader.error());
    }
    return read_from(reader.value(), options);
}

Result<PhysicalLayer, Error> read_physical_file(const std::filesystem::path& path,
                                                const PhysicalReadOptions& options) {
    using R = Result<PhysicalLayer, Error>;
    auto reader = Pmd2Reader::open_file(path, open_options(options));
    if (!reader) {
        return R::err(reader.error());
    }
    return read_from(reader.value(), options);
}

}  // namespace pat::mapdata
