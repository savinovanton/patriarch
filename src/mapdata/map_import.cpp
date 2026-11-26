#include "pat/mapdata/map_import.hpp"

#include "pat/core/file_io.hpp"
#include "pat/core/hash_stream.hpp"
#include "pat/core/json_reader.hpp"
#include "pat/mapdata/geo_projection.hpp"
#include "pat/mapdata/map_gen.hpp"
#include "pat/mapdata/province_layer.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <set>
#include <utility>

#include "layer_codec.hpp"
#include "parallel.hpp"
#include "raster_label.hpp"

namespace pat::mapdata {

namespace {

using core::Error;
using core::JsonValue;
using core::Result;

// ---- tunables of the stage-2 rules [предл.] (docs/dev/geodata-pipeline.md section 11) -------------
constexpr std::uint32_t kRulesVersion = 1;
constexpr std::int32_t kSeaDepthPerPixel = 48;  ///< Synthetic sea bed: units per pixel from the coast.
constexpr std::int32_t kDeepWaterUnits = 2400;  ///< Deeper than this is DeepWater (as in generate_map).
constexpr std::int32_t kNoDataLandMetres = 1;   ///< Land with no DEM anywhere on its landmass.
constexpr const char* kImportStepId = "import:stage2-v1";
constexpr const char* kProvinceStepId = "provinces:placeholder-v1";
constexpr const char* kBuilder = "mapgen import";

Error descriptor_error(const std::string& what) {
    return codec::make_error(import_error::kBadDescriptor, "stage1.json: " + what);
}

template <class T>
bool get_int(const JsonValue& object, std::string_view key, T& out) {
    const JsonValue* v = object.find(key);
    if (v == nullptr) {
        return false;
    }
    const auto i = v->as_i64();
    if (!i || !std::in_range<T>(*i)) {
        return false;
    }
    out = static_cast<T>(*i);
    return true;
}

/// A string member; null is accepted (as "") when `nullable`.
bool get_string(const JsonValue& object, std::string_view key, std::string& out, bool nullable = false) {
    const JsonValue* v = object.find(key);
    if (v == nullptr) {
        return false;
    }
    if (v->is_null() && nullable) {
        out.clear();
        return true;
    }
    if (!v->is_string()) {
        return false;
    }
    out = v->text;
    return true;
}

int hex_digit(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

bool parse_sha256(std::string_view hex, std::array<std::uint8_t, 32>& out) {
    if (hex.size() != 64) {
        return false;
    }
    for (std::size_t i = 0; i < 32; ++i) {
        const int hi = hex_digit(hex[2 * i]);
        const int lo = hex_digit(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        out[i] = static_cast<std::uint8_t>(hi * 16 + lo);
    }
    return true;
}

/// "0x" + 16 hex digits (stage 1 writes the hash as a string to keep its 64 bits exact in JSON).
bool parse_hash64(std::string_view text, std::uint64_t& out) {
    if (text.size() != 18 || text[0] != '0' || (text[1] != 'x' && text[1] != 'X')) {
        return false;
    }
    out = 0;
    for (std::size_t i = 2; i < text.size(); ++i) {
        const int d = hex_digit(text[i]);
        if (d < 0) {
            return false;
        }
        out = (out << 4U) | static_cast<std::uint64_t>(d);
    }
    return true;
}

Result<GeoRef, Error> parse_georef(const JsonValue& j) {
    using R = Result<GeoRef, Error>;
    GeoRef g;
    std::uint8_t projection = 0;
    std::uint8_t datum = 0;
    const bool ok =
        j.is_object() && get_int(j, "version", g.version) && get_int(j, "projection", projection) &&
        get_int(j, "datum", datum) && get_int(j, "lat_origin_udeg", g.lat_origin_udeg) &&
        get_int(j, "lon_central_udeg", g.lon_central_udeg) && get_int(j, "lat_std1_udeg", g.lat_std1_udeg) &&
        get_int(j, "lat_std2_udeg", g.lat_std2_udeg) && get_int(j, "false_easting_mm", g.false_easting_mm) &&
        get_int(j, "false_northing_mm", g.false_northing_mm) && get_int(j, "origin_x_mm", g.origin_x_mm) &&
        get_int(j, "origin_y_mm", g.origin_y_mm) && get_int(j, "pixel_size_x_mm", g.pixel_size_x_mm) &&
        get_int(j, "pixel_size_y_mm", g.pixel_size_y_mm) &&
        get_int(j, "bbox_min_lon_udeg", g.bbox_min_lon_udeg) &&
        get_int(j, "bbox_max_lon_udeg", g.bbox_max_lon_udeg) &&
        get_int(j, "bbox_min_lat_udeg", g.bbox_min_lat_udeg) &&
        get_int(j, "bbox_max_lat_udeg", g.bbox_max_lat_udeg) && get_int(j, "epsg", g.epsg) &&
        get_string(j, "proj_text", g.proj_text);
    if (!ok) {
        return R::err(descriptor_error("georef: a field is missing or out of range"));
    }
    g.projection = static_cast<Projection>(projection);
    g.datum = static_cast<Datum>(datum);
    return R::ok(std::move(g));
}

Result<Provenance, Error> parse_provenance(const JsonValue& j, std::size_t& unverified) {
    using R = Result<Provenance, Error>;
    Provenance p;
    unverified = 0;
    const JsonValue* sources = j.find("sources");
    const JsonValue* steps = j.find("steps");
    if (!j.is_object() || !get_int(j, "manifest_version", p.manifest_version) || sources == nullptr ||
        !sources->is_array() || steps == nullptr || !steps->is_array()) {
        return R::err(descriptor_error("provenance: manifest_version, sources or steps missing"));
    }
    for (const JsonValue& s : sources->items) {
        ProvSource src;
        const JsonValue* files = s.find("files");
        if (!get_string(s, "id", src.source_id) || !get_string(s, "version", src.source_version, true) ||
            files == nullptr || !files->is_array()) {
            return R::err(descriptor_error("provenance: a source needs id, version and files"));
        }
        for (const JsonValue& f : files->items) {
            ProvFile file;
            const JsonValue* sha = f.find("sha256");
            if (!get_string(f, "path", file.path) || sha == nullptr) {
                return R::err(descriptor_error("provenance: a file needs path and sha256"));
            }
            if (sha->is_null()) {
                ++unverified;  // listed without a hash: stays all zeros (= unknown)
            } else if (!sha->is_string() || !parse_sha256(sha->text, file.sha256)) {
                return R::err(descriptor_error("provenance: bad sha256 of " + file.path));
            }
            src.files.push_back(std::move(file));
        }
        p.sources.push_back(std::move(src));
    }
    for (const JsonValue& s : steps->items) {
        ProvStep step;
        std::string hash;
        if (!get_string(s, "id", step.step_id) || !get_string(s, "params_hash", hash) ||
            !parse_hash64(hash, step.params_hash)) {
            return R::err(descriptor_error("provenance: a step needs id and params_hash \"0x\" + 16 hex"));
        }
        p.steps.push_back(std::move(step));
    }
    return R::ok(std::move(p));
}

/// A file name from the descriptor: a plain name inside the stage-1 directory.
bool plain_file_name(const std::string& name) {
    return !name.empty() && name != "." && name != ".." && name.find_first_of("/\\:") == std::string::npos;
}

Result<std::vector<std::uint8_t>, Error>
read_raster(const std::filesystem::path& dir, const std::string& name, std::size_t expected) {
    using R = Result<std::vector<std::uint8_t>, Error>;
    auto bytes = core::read_file_bytes(dir / name);
    if (!bytes) {
        return R::err(codec::make_error(import_error::kIo, "cannot read " + (dir / name).string()));
    }
    if (bytes.value().size() != expected) {
        return R::err(codec::make_error(import_error::kRasterSize,
                                        name + ": " + std::to_string(bytes.value().size()) +
                                            " bytes, expected " + std::to_string(expected)));
    }
    return R::ok(std::move(bytes).value());
}

std::string numbered_key(const char* base, std::size_t k) {
    return k == 1 ? std::string(base) : std::string(base) + "-" + std::to_string(k);
}

/// Metres to height units relative to sea_level, rounded to nearest (ties away from zero).
std::int64_t metres_to_units(std::int64_t metres) {
    const std::int64_t scaled = metres * 1000;
    const std::int64_t unit = kImportMetresPerUnitMilli;
    return scaled >= 0 ? (scaled + unit / 2) / unit : -((-scaled + unit / 2) / unit);
}

std::uint16_t clamp_u16(std::int64_t v, std::size_t& clamped) {
    if (v < 0 || v > 65535) {
        ++clamped;
        return static_cast<std::uint16_t>(std::clamp<std::int64_t>(v, 0, 65535));
    }
    return static_cast<std::uint16_t>(v);
}

template <class Fn>
void for_neighbours(std::size_t i, std::size_t w, std::size_t n, Fn&& fn) {
    const std::size_t x = i % w;
    if (x > 0) {
        fn(i - 1);
    }
    if (x + 1 < w) {
        fn(i + 1);
    }
    if (i >= w) {
        fn(i - w);
    }
    if (i + w < n) {
        fn(i + w);
    }
}

Material land_material(double elev_m, double slope, double lat, bool next_to_sea) {
    // Rules [предл.] (docs/dev/geodata-pipeline.md section 11): permanent snow above a snow line that
    // falls towards the pole, bare rock on steep slopes and just below it, tundra in the Arctic and
    // above the tree line, hills on moderate slopes and high plateaus, beaches on low sea shores,
    // then latitude belts: steppe south of 52 N, mixed plains to 56.5 N, forest north of that.
    const double snow_line = 4800.0 - 55.0 * (lat - 28.0);
    if (elev_m > snow_line) {
        return Material::Snow;
    }
    if (slope > 0.20 || elev_m > snow_line - 1000.0) {
        return Material::Rock;
    }
    if (lat + elev_m / 300.0 > 67.0) {
        return Material::Tundra;
    }
    if (slope > 0.05 || elev_m > 1500.0) {
        return Material::Hills;
    }
    if (next_to_sea && elev_m < 5.0) {
        return Material::Beach;
    }
    if (lat < 52.0) {
        return Material::Steppe;
    }
    if (lat < 56.5) {
        return Material::Plains;
    }
    return Material::Forest;
}

}  // namespace

// ---- stage-1 input --------------------------------------------------------------------------------

Result<Stage1Data, Error> parse_stage1_descriptor(std::string_view json) {
    using R = Result<Stage1Data, Error>;
    auto parsed = core::parse_json(json);
    if (!parsed) {
        return R::err(descriptor_error(parsed.error().message));
    }
    const JsonValue& root = parsed.value();
    std::string format;
    int version = 0;
    if (!root.is_object() || !get_string(root, "format", format) || format != "patriarch-geo-stage1") {
        return R::err(descriptor_error("not a patriarch-geo-stage1 descriptor"));
    }
    if (!get_int(root, "format_version", version) || version != kStage1FormatVersion) {
        return R::err(descriptor_error("unsupported format_version (expected 1)"));
    }
    Stage1Data d;
    if (!get_int(root, "width", d.width) || !get_int(root, "height", d.height) || d.width == 0 ||
        d.height == 0 || d.width > kPmd2MaxDimension || d.height > kPmd2MaxDimension ||
        !get_string(root, "projection_id", d.projection_id)) {
        return R::err(descriptor_error("width, height or projection_id invalid"));
    }
    const JsonValue* georef = root.find("georef");
    const JsonValue* provenance = root.find("provenance");
    if (georef == nullptr || provenance == nullptr) {
        return R::err(descriptor_error("georef or provenance missing"));
    }
    auto g = parse_georef(*georef);
    if (!g) {
        return R::err(g.error());
    }
    d.georef = std::move(g).value();
    auto p = parse_provenance(*provenance, d.unverified_files);
    if (!p) {
        return R::err(p.error());
    }
    d.provenance = std::move(p).value();

    const JsonValue* files = root.find("files");
    const JsonValue* height = files != nullptr ? files->find("height") : nullptr;
    const JsonValue* water = files != nullptr ? files->find("water") : nullptr;
    std::string height_type;
    std::string water_type;
    if (height == nullptr || water == nullptr || !get_string(*height, "path", d.height_file) ||
        !get_string(*water, "path", d.water_file) || !get_string(*height, "type", height_type) ||
        !get_string(*water, "type", water_type) || height_type != "int16le" || water_type != "uint8" ||
        !plain_file_name(d.height_file) || !plain_file_name(d.water_file)) {
        return R::err(
            descriptor_error("files.height (int16le) and files.water (uint8) with plain names needed"));
    }
    return R::ok(std::move(d));
}

Result<Stage1Data, Error> read_stage1(const std::filesystem::path& dir) {
    using R = Result<Stage1Data, Error>;
    auto text = core::read_file_text(dir / kStage1DescriptorName);
    if (!text) {
        return R::err(
            codec::make_error(import_error::kIo, "cannot read " + (dir / kStage1DescriptorName).string()));
    }
    auto parsed = parse_stage1_descriptor(text.value());
    if (!parsed) {
        return parsed;
    }
    Stage1Data d = std::move(parsed).value();
    const std::size_t n = static_cast<std::size_t>(d.width) * d.height;
    auto hb = read_raster(dir, d.height_file, n * 2);
    if (!hb) {
        return R::err(hb.error());
    }
    auto wb = read_raster(dir, d.water_file, n);
    if (!wb) {
        return R::err(wb.error());
    }
    d.height_m.resize(n);
    const std::vector<std::uint8_t>& raw = hb.value();
    for (std::size_t i = 0; i < n; ++i) {
        d.height_m[i] =
            static_cast<std::int16_t>(static_cast<std::uint16_t>(raw[2 * i] | (raw[2 * i + 1] << 8U)));
    }
    d.water = std::move(wb).value();
    return R::ok(std::move(d));
}

// ---- stage 2 --------------------------------------------------------------------------------------

Result<ImportedMap, Error> import_stage1(const Stage1Data& in, const ImportParams& params) {
    using R = Result<ImportedMap, Error>;
    const unsigned threads = parallel::resolve_threads(params.threads);
    const std::uint32_t w = in.width;
    const std::uint32_t h = in.height;
    const std::size_t n = static_cast<std::size_t>(w) * h;
    if (w == 0 || h == 0 || in.height_m.size() != n || in.water.size() != n) {
        return R::err(codec::make_error(import_error::kRasterSize, "rasters do not match width x height"));
    }
    for (std::size_t i = 0; i < n; ++i) {
        if (in.water[i] > static_cast<std::uint8_t>(Stage1Water::lake)) {
            return R::err(codec::make_error(import_error::kBadWaterCode,
                                            "water.u8: code " + std::to_string(in.water[i]) + " at pixel " +
                                                std::to_string(i)));
        }
    }
    if (auto ok = validate_georef(in.georef, w, h); !ok) {
        return R::err(ok.error());
    }
    auto projection = GeoProjection::create(in.georef);
    if (!projection) {
        return R::err(projection.error());
    }
    const GeoProjection& proj = projection.value();

    ImportedMap out;
    ImportReport& rep = out.report;
    PhysicalLayer& l = out.layers.physical;
    l.width = w;
    l.height = h;
    l.sea_level = kImportSeaLevel;
    l.metres_per_unit_milli = kImportMetresPerUnitMilli;
    l.seed = 0;
    l.georef = in.georef;
    const std::int64_t sea = kImportSeaLevel;
    const auto is_land = [&](std::size_t i) { return in.water[i] == 0; };
    const auto is_sea = [&](std::size_t i) {
        return in.water[i] == static_cast<std::uint8_t>(Stage1Water::sea);
    };

    // Water bodies: 4-connected components of each code, numbered in raster order.
    bool overflow = false;
    const std::size_t bodies = raster::label_components(w, in.water, l.water_body, overflow);
    if (overflow) {
        return R::err(codec::make_error(import_error::kTooManyWaters, "more than 65535 water bodies"));
    }
    std::vector<std::uint8_t> body_code(bodies + 1, 0);
    for (std::size_t i = 0; i < n; ++i) {
        rep.land_px += is_land(i) ? 1U : 0U;
        if (l.water_body[i] != 0) {
            body_code[l.water_body[i]] = in.water[i];
        }
    }
    if (rep.land_px == 0) {
        return R::err(codec::make_error(import_error::kNoLand, "the water mask has no land"));
    }

    // Land heights: DEM in units, land at or above 0 m kept above the waterline, nodata filled from the
    // nearest known land pixel (breadth-first in raster order, so deterministic).
    std::vector<std::int32_t> land(n, 0);
    std::vector<std::uint8_t> known(n, 0);
    std::vector<std::uint32_t> queue;
    queue.reserve(rep.land_px);
    for (std::size_t i = 0; i < n; ++i) {
        if (is_land(i) && in.height_m[i] != kStage1NoData) {
            std::int64_t units = metres_to_units(in.height_m[i]);
            if (in.height_m[i] >= 0) {
                units = std::max<std::int64_t>(units, 1);
            }
            land[i] = static_cast<std::int32_t>(units);
            known[i] = 1;
            queue.push_back(static_cast<std::uint32_t>(i));
        }
    }
    for (std::size_t head = 0; head < queue.size(); ++head) {
        const std::size_t i = queue[head];
        for_neighbours(i, w, n, [&](std::size_t j) {
            if (known[j] == 0 && is_land(j)) {
                known[j] = 1;
                land[j] = land[i];
                ++rep.nodata_filled_px;
                queue.push_back(static_cast<std::uint32_t>(j));
            }
        });
    }
    const auto fallback = static_cast<std::int32_t>(metres_to_units(kNoDataLandMetres));
    for (std::size_t i = 0; i < n; ++i) {
        if (is_land(i) && known[i] == 0) {
            land[i] = fallback;
            ++rep.nodata_filled_px;
        }
    }

    // Lake surfaces: the lowest land pixel on the shore (the outlet); a lake with no land shore gets
    // the median of its own DEM samples, or 1 m.
    std::vector<std::int32_t> surface(bodies + 1, std::numeric_limits<std::int32_t>::max());
    for (std::size_t i = 0; i < n; ++i) {
        const std::uint16_t b = l.water_body[i];
        if (b == 0 || body_code[b] != static_cast<std::uint8_t>(Stage1Water::lake)) {
            continue;
        }
        for_neighbours(i, w, n, [&](std::size_t j) {
            if (is_land(j)) {
                surface[b] = std::min(surface[b], land[j]);
            }
        });
    }
    {
        std::vector<std::vector<std::int32_t>> samples;
        for (std::size_t b = 1; b <= bodies; ++b) {
            if (body_code[b] == static_cast<std::uint8_t>(Stage1Water::lake) &&
                surface[b] == std::numeric_limits<std::int32_t>::max()) {
                samples.resize(bodies + 1);
                break;
            }
        }
        if (!samples.empty()) {
            for (std::size_t i = 0; i < n; ++i) {
                const std::uint16_t b = l.water_body[i];
                if (b != 0 && surface[b] == std::numeric_limits<std::int32_t>::max() &&
                    in.height_m[i] != kStage1NoData) {
                    samples[b].push_back(static_cast<std::int32_t>(metres_to_units(in.height_m[i])));
                }
            }
        }
        for (std::size_t b = 1; b <= bodies; ++b) {
            if (body_code[b] != static_cast<std::uint8_t>(Stage1Water::lake) ||
                surface[b] != std::numeric_limits<std::int32_t>::max()) {
                continue;
            }
            std::vector<std::int32_t>& s = samples[b];
            if (s.empty()) {
                surface[b] = fallback;
            } else {
                std::nth_element(s.begin(), s.begin() + static_cast<std::ptrdiff_t>(s.size() / 2), s.end());
                surface[b] = s[s.size() / 2];
            }
        }
    }

    // Sea bed: synthetic depth growing with the distance (4-neighbour steps) from the nearest non-sea pixel.
    std::vector<std::uint32_t> dist(n, std::numeric_limits<std::uint32_t>::max());
    queue.clear();
    for (std::size_t i = 0; i < n; ++i) {
        if (!is_sea(i)) {
            dist[i] = 0;
            queue.push_back(static_cast<std::uint32_t>(i));
        }
    }
    for (std::size_t head = 0; head < queue.size(); ++head) {
        const std::size_t i = queue[head];
        for_neighbours(i, w, n, [&](std::size_t j) {
            if (dist[j] == std::numeric_limits<std::uint32_t>::max()) {
                dist[j] = dist[i] + 1;
                queue.push_back(static_cast<std::uint32_t>(j));
            }
        });
    }
    std::vector<std::uint32_t>().swap(queue);

    l.heights.assign(n, 0);
    for (std::size_t i = 0; i < n; ++i) {
        if (is_land(i)) {
            l.heights[i] = clamp_u16(sea + land[i], rep.clamped_px);
        } else if (is_sea(i)) {
            const std::int64_t depth =
                std::min<std::int64_t>(static_cast<std::int64_t>(dist[i]) * kSeaDepthPerPixel, sea - 1);
            l.heights[i] = static_cast<std::uint16_t>(sea - depth);
        } else {
            l.heights[i] = clamp_u16(sea + surface[l.water_body[i]], rep.clamped_px);
        }
    }
    std::vector<std::uint32_t>().swap(dist);
    std::vector<std::int32_t>().swap(land);
    std::vector<std::uint8_t>().swap(known);

    // Water table.
    const std::vector<std::uint32_t> area = count_water_area(l.water_body, bodies);
    for (std::size_t b = 1; b <= bodies; ++b) {
        WaterBody body;
        body.id = static_cast<std::uint16_t>(b);
        if (body_code[b] == static_cast<std::uint8_t>(Stage1Water::sea)) {
            body.kind = WaterKind::sea;
            body.surface_level = kImportSeaLevel;
            body.key = numbered_key("sea", ++rep.seas);
        } else {
            body.kind = WaterKind::lake;
            std::size_t ignored = 0;
            body.surface_level = clamp_u16(sea + surface[b], ignored);
            body.key = numbered_key("lake", ++rep.lakes);
        }
        body.area_px = area[b - 1];
        l.water_bodies.push_back(std::move(body));
    }

    // Materials by depth, elevation, slope (of the visible surface: water counts at its level) and
    // latitude. Every pixel is independent, so the result does not depend on the thread count.
    l.materials.assign(n, 0);
    const double metres_per_unit = static_cast<double>(kImportMetresPerUnitMilli) / 1000.0;
    const double pixel_m_x = static_cast<double>(in.georef.pixel_size_x_mm) / 1000.0;
    const double pixel_m_y = static_cast<double>(in.georef.pixel_size_y_mm) / 1000.0;
    const auto visible = [&](std::size_t i) -> double {
        const std::uint16_t b = l.water_body[i];
        return b == 0 ? static_cast<double>(l.heights[i])
                      : static_cast<double>(l.water_bodies[static_cast<std::size_t>(b) - 1].surface_level);
    };
    parallel::parallel_rows(h, threads, [&](std::uint32_t y0, std::uint32_t y1) {
        for (std::uint32_t y = y0; y < y1; ++y) {
            for (std::uint32_t x = 0; x < w; ++x) {
                const std::size_t i = static_cast<std::size_t>(y) * w + x;
                Material m = Material::ShallowWater;
                if (is_sea(i)) {
                    m = sea - l.heights[i] > kDeepWaterUnits ? Material::DeepWater : Material::ShallowWater;
                } else if (is_land(i)) {
                    const std::uint32_t xm = x > 0 ? x - 1 : x;
                    const std::uint32_t xp = x + 1 < w ? x + 1 : x;
                    const std::uint32_t ym = y > 0 ? y - 1 : y;
                    const std::uint32_t yp = y + 1 < h ? y + 1 : y;
                    const std::size_t row = static_cast<std::size_t>(y) * w;
                    const double gx = (visible(row + xp) - visible(row + xm)) * metres_per_unit /
                                      (static_cast<double>(xp - xm) * pixel_m_x);
                    const double gy = (visible(static_cast<std::size_t>(yp) * w + x) -
                                       visible(static_cast<std::size_t>(ym) * w + x)) *
                                      metres_per_unit / (static_cast<double>(yp - ym) * pixel_m_y);
                    const double slope = std::sqrt(gx * gx + gy * gy);
                    const double elev =
                        (static_cast<double>(l.heights[i]) - static_cast<double>(sea)) * metres_per_unit;
                    bool next_to_sea = false;
                    for_neighbours(i, w, n, [&](std::size_t j) { next_to_sea = next_to_sea || is_sea(j); });
                    m = land_material(elev, slope, proj.pixel_lonlat(x, y).lat_deg, next_to_sea);
                }
                l.materials[i] = static_cast<std::uint8_t>(m);
            }
        }
    });

    // Provenance: stage 1 plus this step.
    {
        Provenance prov = in.provenance;
        prov.builder = kBuilder;
        core::HashStream hs;
        hs.u32(kRulesVersion);
        hs.u16(kImportSeaLevel);
        hs.u32(kImportMetresPerUnitMilli);
        hs.i32(kSeaDepthPerPixel);
        hs.i32(kDeepWaterUnits);
        prov.steps.push_back({kImportStepId, hs.value()});
        l.provenance = std::move(prov);
    }
    if (auto ok = validate_physical(l); !ok) {
        return R::err(ok.error());
    }

    // Placeholder provinces: the generate_map split of the land, then one province per landmass the
    // split did not reach (islands without a seed).
    ProvinceLayer& p = out.layers.provinces;
    p.width = w;
    p.height = h;
    p.bind_key = coast_key(l);
    {
        MapData mask;
        mask.width = w;
        mask.height = h;
        mask.sea_level = 0;
        mask.heights.resize(n);
        for (std::size_t i = 0; i < n; ++i) {
            mask.heights[i] = l.water_body[i] == 0 ? 1 : 0;
        }
        mask.provinces.assign(n, 0);
        const std::uint32_t count =
            partition_provinces(mask, rep.land_px, params.province_count, params.seed, threads);
        std::vector<std::uint8_t> leftover(n, 0);
        for (std::size_t i = 0; i < n; ++i) {
            leftover[i] = mask.provinces[i] == kUnassignedProvince ? 1 : 0;
        }
        std::vector<std::uint16_t> island;
        bool island_overflow = false;
        const std::size_t islands = raster::label_components(w, leftover, island, island_overflow);
        if (island_overflow || static_cast<std::size_t>(count) + islands > kMaxProvinceCount) {
            return R::err(codec::make_error(import_error::kTooManyProvinces,
                                            "more than 65535 provinces (islands included)"));
        }
        rep.placeholder_provinces = count;
        rep.island_provinces = islands;
        p.province_id = std::move(mask.provinces);
        for (std::size_t i = 0; i < n; ++i) {
            if (island[i] != 0) {
                p.province_id[i] = static_cast<std::uint16_t>(count + island[i]);
            }
        }
    }
    const std::size_t total = rep.placeholder_provinces + rep.island_provinces;
    p.entries = compute_province_geometry(w, h, p.province_id, total, sea_mask(l));
    std::set<std::uint32_t> colors;
    for (ProvinceEntry& e : p.entries) {
        e.key = generated_province_key(e.id);
        std::uint32_t c = generated_province_color(e.id) & 0xFFFFFFU;
        while (!colors.insert(c).second) {
            c = (c * 2654435761U + 0x9E3779B9U) & 0xFFFFFFU;  // deterministic re-roll of a repeated colour
        }
        e.color_rgb = c;
    }
    {
        Provenance prov;
        prov.builder = kBuilder;
        core::HashStream hs;
        hs.u32(params.province_count);
        hs.u64(params.seed);
        prov.steps.push_back({kProvinceStepId, hs.value()});
        p.provenance = std::move(prov);
    }
    if (auto ok = validate_provinces(l, p); !ok) {
        return R::err(ok.error());
    }
    return R::ok(std::move(out));
}

}  // namespace pat::mapdata
