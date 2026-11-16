#include "pat/mapdata/province_layer.hpp"

#include "pat/core/bytes.hpp"

#include <algorithm>
#include <string_view>
#include <utility>

#include "layer_codec.hpp"

namespace pat::mapdata {

namespace {

using codec::get_str;
using codec::put_str;
using core::Error;
using core::Result;

constexpr Pmd2ChunkId kPidx = pmd2_chunk_id('P', 'I', 'D', 'X');
constexpr Pmd2ChunkId kPtbl = pmd2_chunk_id('P', 'T', 'B', 'L');
constexpr Pmd2ChunkId kProv = pmd2_chunk_id('P', 'R', 'O', 'V');

/// Smallest possible encoded record (empty key, no neighbours): used to bound allocations.
constexpr std::size_t kMinRecordBytes = 2 + 2 + 4 + 4 + 8 + 4 + 1 + 2;
constexpr std::size_t kBorderBytes = 6;
constexpr std::uint8_t kFlagCoastal = 1U;

std::uint64_t pack_pair(std::uint16_t a, std::uint16_t b) {
    return (static_cast<std::uint64_t>(std::min(a, b)) << 16U) | std::max(a, b);
}

}  // namespace

bool valid_province_key(const std::string& key) {
    return codec::valid_ascii_key(key, kMaxProvinceKeyLength);
}

// ---- geometry ------------------------------------------------------------------------------------

std::vector<ProvinceEntry> compute_province_geometry(std::uint32_t width,
                                                     std::uint32_t height,
                                                     std::span<const std::uint16_t> province_id,
                                                     std::size_t count,
                                                     std::span<const std::uint8_t> sea) {
    std::vector<ProvinceEntry> out(count);
    std::vector<std::uint64_t> sum_x(count, 0);
    std::vector<std::uint64_t> sum_y(count, 0);
    std::vector<std::uint64_t> edges;  // (low id << 16 | high id) once per shared pixel edge
    for (std::size_t i = 0; i < count; ++i) {
        out[i].id = static_cast<std::uint16_t>(i + 1);
        out[i].min_x = 0xFFFF;
        out[i].min_y = 0xFFFF;
    }
    const std::size_t w = width;
    const bool has_sea = !sea.empty();
    const auto is_sea = [&](std::size_t i) { return has_sea && sea[i] != 0; };
    const auto in_set = [&](std::uint16_t id) { return id != 0 && id <= count; };

    for (std::uint32_t y = 0; y < height; ++y) {
        const std::size_t row = static_cast<std::size_t>(y) * w;
        for (std::uint32_t x = 0; x < width; ++x) {
            const std::size_t i = row + x;
            const std::uint16_t p = province_id[i];
            if (!in_set(p)) {
                continue;
            }
            ProvinceEntry& e = out[static_cast<std::size_t>(p) - 1];
            ++e.area_px;
            sum_x[p - 1] += x;
            sum_y[p - 1] += y;
            e.min_x = std::min<std::uint16_t>(e.min_x, static_cast<std::uint16_t>(x));
            e.min_y = std::min<std::uint16_t>(e.min_y, static_cast<std::uint16_t>(y));
            e.max_x = std::max<std::uint16_t>(e.max_x, static_cast<std::uint16_t>(x));
            e.max_y = std::max<std::uint16_t>(e.max_y, static_cast<std::uint16_t>(y));
            if ((x > 0 && is_sea(i - 1)) || (y > 0 && is_sea(i - w)) || (x + 1 < width && is_sea(i + 1)) ||
                (y + 1 < height && is_sea(i + w))) {
                e.coastal = true;
            }
            if (x + 1 < width) {
                const std::uint16_t q = province_id[i + 1];
                if (in_set(q) && q != p) {
                    edges.push_back(pack_pair(p, q));
                }
            }
            if (y + 1 < height) {
                const std::uint16_t q = province_id[i + w];
                if (in_set(q) && q != p) {
                    edges.push_back(pack_pair(p, q));
                }
            }
        }
    }

    for (std::size_t i = 0; i < count; ++i) {
        ProvinceEntry& e = out[i];
        if (e.area_px == 0) {
            e.min_x = e.min_y = 0;
            continue;
        }
        e.centroid_x = static_cast<std::uint16_t>(sum_x[i] / e.area_px);
        e.centroid_y = static_cast<std::uint16_t>(sum_y[i] / e.area_px);
    }

    std::sort(edges.begin(), edges.end());
    for (std::size_t k = 0; k < edges.size();) {
        std::size_t e = k;
        while (e < edges.size() && edges[e] == edges[k]) {
            ++e;
        }
        const auto a = static_cast<std::uint16_t>(edges[k] >> 16U);
        const auto b = static_cast<std::uint16_t>(edges[k] & 0xFFFFU);
        const auto len = static_cast<std::uint32_t>(e - k);
        out[a - 1].neighbors.push_back({b, len});
        out[b - 1].neighbors.push_back({a, len});
        k = e;
    }
    for (ProvinceEntry& e : out) {
        std::sort(e.neighbors.begin(), e.neighbors.end(), [](const Border& l, const Border& r) {
            return l.neighbor < r.neighbor;
        });
    }
    return out;
}

std::vector<std::uint8_t> sea_mask(const PhysicalLayer& physical) {
    std::vector<std::uint8_t> mask(physical.water_body.size(), 0);
    std::vector<std::uint8_t> is_sea(physical.water_bodies.size() + 1, 0);
    for (const WaterBody& b : physical.water_bodies) {
        if (b.id >= 1 && b.id < is_sea.size() && b.kind == WaterKind::sea) {
            is_sea[b.id] = 1;
        }
    }
    for (std::size_t i = 0; i < mask.size(); ++i) {
        const std::uint16_t id = physical.water_body[i];
        mask[i] = (id < is_sea.size() && is_sea[id] != 0) ? 1 : 0;
    }
    return mask;
}

// ---- PTBL ----------------------------------------------------------------------------------------

std::vector<std::uint8_t> encode_province_table(std::span<const ProvinceEntry> entries) {
    core::ByteWriter w;
    w.u16(static_cast<std::uint16_t>(entries.size()));
    for (const ProvinceEntry& e : entries) {
        w.u16(e.id);
        put_str(w, e.key);
        w.u32(e.color_rgb);
        w.u32(e.area_px);
        w.u16(e.min_x);
        w.u16(e.min_y);
        w.u16(e.max_x);
        w.u16(e.max_y);
        w.u16(e.centroid_x);
        w.u16(e.centroid_y);
        w.u8(e.coastal ? kFlagCoastal : 0U);
        w.u16(static_cast<std::uint16_t>(e.neighbors.size()));
        for (const Border& b : e.neighbors) {
            w.u16(b.neighbor);
            w.u32(b.length_px);
        }
    }
    return w.take();
}

Result<std::vector<ProvinceEntry>, Error> decode_province_table(std::span<const std::uint8_t> bytes,
                                                                bool allow_trailing) {
    using R = Result<std::vector<ProvinceEntry>, Error>;
    const auto truncated = [] {
        return R::err(codec::make_error(province_error::kBadChunk, "PTBL: truncated"));
    };
    core::ByteReader r(bytes);
    const std::uint16_t count = r.u16();
    if (!r.ok() || static_cast<std::size_t>(count) * kMinRecordBytes > r.remaining()) {
        return truncated();
    }
    std::vector<ProvinceEntry> out;
    out.reserve(count);
    for (std::uint16_t i = 0; i < count; ++i) {
        ProvinceEntry e;
        e.id = r.u16();
        e.key = get_str(r);
        e.color_rgb = r.u32();
        e.area_px = r.u32();
        e.min_x = r.u16();
        e.min_y = r.u16();
        e.max_x = r.u16();
        e.max_y = r.u16();
        e.centroid_x = r.u16();
        e.centroid_y = r.u16();
        const std::uint8_t flags = r.u8();
        const std::uint16_t neighbor_count = r.u16();
        if (!r.ok() || static_cast<std::size_t>(neighbor_count) * kBorderBytes > r.remaining()) {
            return truncated();
        }
        if ((flags & ~kFlagCoastal) != 0) {
            return R::err(codec::make_error(province_error::kBadTable, "PTBL: unknown flag bits"));
        }
        e.coastal = (flags & kFlagCoastal) != 0;
        e.neighbors.reserve(neighbor_count);
        for (std::uint16_t k = 0; k < neighbor_count; ++k) {
            Border b;
            b.neighbor = r.u16();
            b.length_px = r.u32();
            e.neighbors.push_back(b);
        }
        out.push_back(std::move(e));
    }
    if (auto ok = codec::finish_blob(r, allow_trailing, kPtbl, province_error::kBadChunk); !ok) {
        return R::err(ok.error());
    }
    return R::ok(std::move(out));
}

Result<void, Error>
validate_province_table(std::span<const ProvinceEntry> entries, std::uint32_t width, std::uint32_t height) {
    using R = Result<void, Error>;
    const std::size_t n = entries.size();
    if (n == 0 || n > kMaxProvinceCount) {
        return R::err(codec::make_error(province_error::kBadTable, "province count must be in 1..65535"));
    }
    const std::uint64_t pixels = static_cast<std::uint64_t>(width) * height;
    for (std::size_t i = 0; i < n; ++i) {
        const ProvinceEntry& e = entries[i];
        const std::string label = "province " + std::to_string(i + 1) + ": ";
        if (e.id != i + 1) {
            return R::err(codec::make_error(province_error::kBadId, label + "ids must be dense 1..N"));
        }
        if (!valid_province_key(e.key)) {
            return R::err(codec::make_error(province_error::kBadKey, label + "invalid key"));
        }
        if (e.area_px == 0 || e.area_px > pixels) {
            return R::err(codec::make_error(province_error::kBadTable, label + "area out of range"));
        }
        if (e.min_x > e.max_x || e.min_y > e.max_y || e.max_x >= width || e.max_y >= height) {
            return R::err(codec::make_error(province_error::kBadTable, label + "bounding box invalid"));
        }
        const std::uint64_t box =
            static_cast<std::uint64_t>(e.max_x - e.min_x + 1U) * (e.max_y - e.min_y + 1U);
        if (e.area_px > box) {
            return R::err(
                codec::make_error(province_error::kBadTable, label + "area exceeds the bounding box"));
        }
        if (e.centroid_x < e.min_x || e.centroid_x > e.max_x || e.centroid_y < e.min_y ||
            e.centroid_y > e.max_y) {
            return R::err(
                codec::make_error(province_error::kBadTable, label + "centroid outside the bounding box"));
        }
        std::uint16_t previous = 0;
        for (const Border& b : e.neighbors) {
            if (b.neighbor == 0 || b.neighbor > n || b.neighbor == e.id || b.neighbor <= previous ||
                b.length_px == 0) {
                return R::err(codec::make_error(province_error::kBadTable, label + "neighbour list invalid"));
            }
            previous = b.neighbor;
        }
    }
    {
        std::vector<std::string_view> keys;
        keys.reserve(n);
        for (const ProvinceEntry& e : entries) {
            keys.emplace_back(e.key);
        }
        std::sort(keys.begin(), keys.end());
        if (std::adjacent_find(keys.begin(), keys.end()) != keys.end()) {
            return R::err(codec::make_error(province_error::kBadKey, "province keys are not unique"));
        }
    }
    {
        std::vector<std::uint32_t> colors;
        colors.reserve(n);
        for (const ProvinceEntry& e : entries) {
            colors.push_back(e.color_rgb);
        }
        std::sort(colors.begin(), colors.end());
        if (std::adjacent_find(colors.begin(), colors.end()) != colors.end()) {
            return R::err(
                codec::make_error(province_error::kDuplicateColor, "province colours are not unique"));
        }
    }
    // Symmetry (same length from both sides).
    for (const ProvinceEntry& e : entries) {
        for (const Border& b : e.neighbors) {
            const auto& other = entries[static_cast<std::size_t>(b.neighbor) - 1].neighbors;
            const auto it =
                std::lower_bound(other.begin(), other.end(), e.id, [](const Border& x, std::uint16_t id) {
                    return x.neighbor < id;
                });
            if (it == other.end() || it->neighbor != e.id || it->length_px != b.length_px) {
                return R::err(
                    codec::make_error(province_error::kBadTable,
                                      "province " + std::to_string(e.id) + ": adjacency is not symmetric"));
            }
        }
    }
    // Connected graph.
    {
        std::vector<std::uint8_t> seen(n + 1, 0);
        std::vector<std::uint16_t> stack = {1};
        seen[1] = 1;
        std::size_t reached = 0;
        while (!stack.empty()) {
            const std::uint16_t id = stack.back();
            stack.pop_back();
            ++reached;
            for (const Border& b : entries[static_cast<std::size_t>(id) - 1].neighbors) {
                if (seen[b.neighbor] == 0) {
                    seen[b.neighbor] = 1;
                    stack.push_back(b.neighbor);
                }
            }
        }
        if (reached != n) {
            return R::err(
                codec::make_error(province_error::kGraphDisconnected, "the province graph is not connected"));
        }
    }
    return R::ok();
}

// ---- layer validation ----------------------------------------------------------------------------

namespace {

/// Each province must be one 4-connected region (and non-empty). Returns the bad id or 0.
std::uint16_t first_bad_region(const ProvinceLayer& l, bool& empty) {
    const std::size_t pixels = l.pixel_count();
    const std::size_t n = l.entries.size();
    const std::size_t w = l.width;
    std::vector<std::uint8_t> visited(pixels, 0);
    std::vector<std::uint8_t> regions(n + 1, 0);
    std::vector<std::uint32_t> stack;
    for (std::size_t start = 0; start < pixels; ++start) {
        const std::uint16_t p = l.province_id[start];
        if (p == 0 || visited[start] != 0) {
            continue;
        }
        if (++regions[p] > 1) {
            empty = false;
            return p;
        }
        visited[start] = 1;
        stack.push_back(static_cast<std::uint32_t>(start));
        while (!stack.empty()) {
            const std::size_t i = stack.back();
            stack.pop_back();
            const std::size_t x = i % w;
            const auto visit = [&](std::size_t j) {
                if (visited[j] == 0 && l.province_id[j] == p) {
                    visited[j] = 1;
                    stack.push_back(static_cast<std::uint32_t>(j));
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
            if (i + w < pixels) {
                visit(i + w);
            }
        }
    }
    for (std::size_t p = 1; p <= n; ++p) {
        if (regions[p] == 0) {
            empty = true;
            return static_cast<std::uint16_t>(p);
        }
    }
    return 0;
}

Result<void, Error> check_layer(const ProvinceLayer& l, const PhysicalLayer* physical) {
    using R = Result<void, Error>;
    if (l.width == 0 || l.height == 0 || l.width > kPmd2MaxDimension || l.height > kPmd2MaxDimension ||
        static_cast<std::uint64_t>(l.width) * l.height > kPmd2MaxPixels) {
        return R::err(codec::make_error(province_error::kLayerSize, "invalid dimensions"));
    }
    if (l.province_id.size() != l.pixel_count()) {
        return R::err(
            codec::make_error(province_error::kLayerSize, "PIDX size does not match width x height"));
    }
    if (auto ok = validate_province_table(l.entries, l.width, l.height); !ok) {
        return ok;
    }
    if (l.provenance) {
        if (auto ok = validate_provenance(*l.provenance); !ok) {
            return ok;
        }
    }
    const std::size_t n = l.entries.size();
    for (std::size_t i = 0; i < l.province_id.size(); ++i) {
        const std::uint16_t p = l.province_id[i];
        if (p > n) {
            return R::err(
                codec::make_error(province_error::kBadId, "PIDX holds an id above the province count"));
        }
        if (physical != nullptr && (p != 0) != (physical->water_body[i] == 0)) {
            return R::err(codec::make_error(province_error::kLandMask,
                                            "province must be non-zero exactly where WBOD is 0 (pixel " +
                                                std::to_string(i) + ")"));
        }
    }
    bool empty = false;
    if (const std::uint16_t bad = first_bad_region(l, empty); bad != 0) {
        return R::err(codec::make_error(province_error::kProvinceShape,
                                        "province " + std::to_string(bad) +
                                            (empty ? " has no pixels" : " is split into several regions")));
    }
    std::vector<std::uint8_t> mask;
    if (physical != nullptr) {
        mask = sea_mask(*physical);
    }
    const std::vector<ProvinceEntry> expected =
        compute_province_geometry(l.width, l.height, l.province_id, n, mask);
    for (std::size_t i = 0; i < n; ++i) {
        const ProvinceEntry& a = expected[i];
        const ProvinceEntry& b = l.entries[i];
        const bool same = a.area_px == b.area_px && a.min_x == b.min_x && a.min_y == b.min_y &&
                          a.max_x == b.max_x && a.max_y == b.max_y && a.centroid_x == b.centroid_x &&
                          a.centroid_y == b.centroid_y && a.neighbors == b.neighbors &&
                          (physical == nullptr || a.coastal == b.coastal);
        if (!same) {
            return R::err(codec::make_error(province_error::kTableMismatch,
                                            "province table entry " + std::to_string(i + 1) +
                                                " does not match the pixels"));
        }
    }
    return R::ok();
}

}  // namespace

Result<void, Error> validate_province_layer(const ProvinceLayer& layer) {
    return check_layer(layer, nullptr);
}

Result<void, Error> validate_provinces(const PhysicalLayer& physical, const ProvinceLayer& provinces) {
    using R = Result<void, Error>;
    if (provinces.width != physical.width || provinces.height != physical.height ||
        physical.water_body.size() != physical.pixel_count()) {
        return R::err(
            codec::make_error(province_error::kLayerSize, "province and physical layers differ in size"));
    }
    if (provinces.bind_key != coast_key(physical)) {
        return R::err(
            codec::make_error(province_error::kBindKey, "province borders were built for another coast"));
    }
    return check_layer(provinces, &physical);
}

// ---- write ---------------------------------------------------------------------------------------

namespace {

Result<void, Error> build(Pmd2Writer& w, const ProvinceLayer& l) {
    using R = Result<void, Error>;
    w.set_bind_key(l.bind_key);
    Pmd2ChunkSpec spec;
    spec.critical = true;
    spec.id = kPidx;
    if (auto ok = w.add_raster(spec, Pmd2RasterParams{}, std::span<const std::uint16_t>(l.province_id));
        !ok) {
        return ok;
    }
    spec.id = kPtbl;
    if (auto ok = w.add_blob(spec, encode_province_table(l.entries)); !ok) {
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

Result<std::vector<std::uint8_t>, Error> serialize_provinces(const ProvinceLayer& layer,
                                                             const Pmd2WriterOptions& options) {
    using R = Result<std::vector<std::uint8_t>, Error>;
    if (auto ok = validate_province_layer(layer); !ok) {
        return R::err(ok.error());
    }
    Pmd2Writer writer(Pmd2Kind::provinces, layer.width, layer.height, options);
    if (auto ok = build(writer, layer); !ok) {
        return R::err(ok.error());
    }
    return writer.finish();
}

Result<void, Error> write_provinces_file(const std::filesystem::path& path,
                                         const ProvinceLayer& layer,
                                         const Pmd2WriterOptions& options) {
    using R = Result<void, Error>;
    if (auto ok = validate_province_layer(layer); !ok) {
        return ok;
    }
    Pmd2Writer writer(Pmd2Kind::provinces, layer.width, layer.height, options);
    if (auto ok = build(writer, layer); !ok) {
        return R::err(ok.error());
    }
    return writer.write_file(path);
}

// ---- read ----------------------------------------------------------------------------------------

namespace {

Pmd2OpenOptions open_options(const ProvinceReadOptions& options) {
    Pmd2OpenOptions o;
    o.known = {{kPidx, 1}, {kPtbl, 1}, {kProv, 1}};
    o.verify_raw = options.verify_raw;
    o.threads = options.threads;
    return o;
}

/// A physical file has critical chunks this reader does not know; report the wrong kind rather than
/// the unknown chunk. `peek` (open in inspect mode) is only called on that failure path.
template <class Open, class Peek>
Result<Pmd2Reader, Error> open_checked(Open&& open, Peek&& peek) {
    auto reader = open();
    if (!reader && (reader.error().code == pmd2_error::kUnknownCritical ||
                    reader.error().code == pmd2_error::kUnsupportedChunk)) {
        auto any = peek();
        if (any && any.value().header().kind != Pmd2Kind::provinces) {
            return Result<Pmd2Reader, Error>::err(
                codec::make_error(province_error::kWrongKind, "not a provinces PMD2 container"));
        }
    }
    return reader;
}

Result<Pmd2Reader, Error> open_bytes(std::span<const std::uint8_t> bytes,
                                     const ProvinceReadOptions& options) {
    return open_checked([&] { return Pmd2Reader::open(bytes, open_options(options)); },
                        [&] {
                            Pmd2OpenOptions o;
                            o.inspect = true;
                            return Pmd2Reader::open(bytes, o);
                        });
}

Result<Pmd2Reader, Error> open_path(const std::filesystem::path& path, const ProvinceReadOptions& options) {
    return open_checked([&] { return Pmd2Reader::open_file(path, open_options(options)); },
                        [&] {
                            Pmd2OpenOptions o;
                            o.inspect = true;
                            return Pmd2Reader::open_file(path, o);
                        });
}

Result<void, Error> check_kind(const Pmd2Reader& reader) {
    using R = Result<void, Error>;
    if (reader.header().kind != Pmd2Kind::provinces) {
        return R::err(codec::make_error(province_error::kWrongKind, "not a provinces PMD2 container"));
    }
    return R::ok();
}

Result<std::vector<ProvinceEntry>, Error> read_table(const Pmd2Reader& reader) {
    using R = Result<std::vector<ProvinceEntry>, Error>;
    if (reader.find(kPtbl) == nullptr) {
        return R::err(codec::make_error(province_error::kMissingChunk, "missing chunk PTBL"));
    }
    auto bytes = reader.read_blob(kPtbl);
    if (!bytes) {
        return R::err(bytes.error());
    }
    return decode_province_table(bytes.value(), reader.header().minor > kPmd2KnownMinor);
}

/// Decodes PIDX and PTBL (and PROV if present) without validating the semantics.
Result<ProvinceLayer, Error> read_layer(const Pmd2Reader& reader, const ProvinceReadOptions& options) {
    using R = Result<ProvinceLayer, Error>;
    const Pmd2Header& head = reader.header();
    ProvinceLayer layer;
    layer.width = head.width;
    layer.height = head.height;
    layer.bind_key = head.bind_key;

    auto table = read_table(reader);
    if (!table) {
        return R::err(table.error());
    }
    layer.entries = std::move(table.value());

    if (reader.find(kPidx) == nullptr) {
        return R::err(codec::make_error(province_error::kMissingChunk, "missing chunk PIDX"));
    }
    auto info = reader.raster_info(kPidx);
    if (!info) {
        return R::err(info.error());
    }
    if (info.value().elem_bytes != 2 || info.value().shift != 0 || info.value().width != head.width ||
        info.value().height != head.height) {
        return R::err(
            codec::make_error(province_error::kLayerSize, "PIDX: raster is not full resolution u16"));
    }
    auto ids = reader.read_raster_u16(kPidx, 0, options.threads);
    if (!ids) {
        return R::err(ids.error());
    }
    layer.province_id = std::move(ids.value());

    if (const Pmd2ChunkInfo* prov = reader.find(kProv); prov != nullptr && prov->understood) {
        auto bytes = reader.read_blob(kProv);
        if (!bytes) {
            return R::err(bytes.error());
        }
        auto decoded = decode_provenance(bytes.value(), head.minor > kPmd2KnownMinor);
        if (!decoded) {
            return R::err(decoded.error());
        }
        layer.provenance = std::move(decoded.value());
    }
    return R::ok(std::move(layer));
}

Result<ProvinceLayer, Error> read_from(const Pmd2Reader& reader, const ProvinceReadOptions& options) {
    using R = Result<ProvinceLayer, Error>;
    if (auto ok = check_kind(reader); !ok) {
        return R::err(ok.error());
    }
    auto layer = read_layer(reader, options);
    if (!layer) {
        return layer;
    }
    if (auto ok = validate_province_layer(layer.value()); !ok) {
        return R::err(ok.error());
    }
    return layer;
}

Result<ProvinceTable, Error> table_from(const Pmd2Reader& reader) {
    using R = Result<ProvinceTable, Error>;
    if (auto ok = check_kind(reader); !ok) {
        return R::err(ok.error());
    }
    if (reader.find(kPidx) == nullptr) {
        return R::err(codec::make_error(province_error::kMissingChunk, "missing chunk PIDX"));
    }
    auto entries = read_table(reader);
    if (!entries) {
        return R::err(entries.error());
    }
    ProvinceTable t;
    t.width = reader.header().width;
    t.height = reader.header().height;
    t.bind_key = reader.header().bind_key;
    t.content_id = reader.header().content_id;
    t.entries = std::move(entries.value());
    if (auto ok = validate_province_table(t.entries, t.width, t.height); !ok) {
        return R::err(ok.error());
    }
    return R::ok(std::move(t));
}

}  // namespace

Result<ProvinceLayer, Error> read_provinces(std::span<const std::uint8_t> bytes,
                                            const ProvinceReadOptions& options) {
    using R = Result<ProvinceLayer, Error>;
    auto reader = open_bytes(bytes, options);
    if (!reader) {
        return R::err(reader.error());
    }
    return read_from(reader.value(), options);
}

Result<ProvinceLayer, Error> read_provinces_file(const std::filesystem::path& path,
                                                 const ProvinceReadOptions& options) {
    using R = Result<ProvinceLayer, Error>;
    auto reader = open_path(path, options);
    if (!reader) {
        return R::err(reader.error());
    }
    return read_from(reader.value(), options);
}

Result<ProvinceTable, Error> load_province_table(std::span<const std::uint8_t> bytes) {
    using R = Result<ProvinceTable, Error>;
    auto reader = open_bytes(bytes, {});
    if (!reader) {
        return R::err(reader.error());
    }
    return table_from(reader.value());
}

Result<ProvinceTable, Error> load_province_table(const std::filesystem::path& path) {
    using R = Result<ProvinceTable, Error>;
    auto reader = open_path(path, {});
    if (!reader) {
        return R::err(reader.error());
    }
    return table_from(reader.value());
}

// ---- MapData -------------------------------------------------------------------------------------

namespace {

/// Moves the (already validated) layers into a MapData and recomputes mean_height.
MapData assemble(PhysicalLayer&& physical, ProvinceLayer&& provinces) {
    MapData map;
    map.width = physical.width;
    map.height = physical.height;
    map.sea_level = physical.sea_level;
    map.metres_per_unit_milli = physical.metres_per_unit_milli;
    map.seed = physical.seed;
    map.heights = std::move(physical.heights);
    map.materials = std::move(physical.materials);
    map.provinces = std::move(provinces.province_id);

    std::vector<std::uint64_t> sum(provinces.entries.size(), 0);
    for (std::size_t i = 0; i < map.provinces.size(); ++i) {
        const std::uint16_t p = map.provinces[i];
        if (p != 0) {
            sum[static_cast<std::size_t>(p) - 1] += map.heights[i];
        }
    }
    map.province_list.reserve(provinces.entries.size());
    for (std::size_t i = 0; i < provinces.entries.size(); ++i) {
        ProvinceEntry& e = provinces.entries[i];
        Province p;
        p.id = e.id;
        p.color_rgb = e.color_rgb;
        p.area_px = e.area_px;
        p.centroid_x = e.centroid_x;
        p.centroid_y = e.centroid_y;
        p.min_x = e.min_x;
        p.min_y = e.min_y;
        p.max_x = e.max_x;
        p.max_y = e.max_y;
        p.mean_height = static_cast<std::uint16_t>(sum[i] / e.area_px);
        p.coastal = e.coastal;
        p.neighbors = std::move(e.neighbors);
        map.province_list.push_back(std::move(p));
    }
    return map;
}

Result<MapData, Error>
load_set(const Pmd2Reader& province_reader, PhysicalLayer&& physical, const ProvinceReadOptions& options) {
    using R = Result<MapData, Error>;
    if (auto ok = check_kind(province_reader); !ok) {
        return R::err(ok.error());
    }
    // Cheap check first: the header bind_key, before the province raster is decoded.
    const Pmd2Header& head = province_reader.header();
    if (head.width != physical.width || head.height != physical.height) {
        return R::err(
            codec::make_error(province_error::kLayerSize, "province and physical layers differ in size"));
    }
    if (head.bind_key != coast_key(physical)) {
        return R::err(
            codec::make_error(province_error::kBindKey, "province borders were built for another coast"));
    }
    auto layer = read_layer(province_reader, options);
    if (!layer) {
        return R::err(layer.error());
    }
    if (auto ok = validate_provinces(physical, layer.value()); !ok) {
        return R::err(ok.error());
    }
    return R::ok(assemble(std::move(physical), std::move(layer.value())));
}

}  // namespace

Result<MapData, Error> build_map_data(const PhysicalLayer& physical, const ProvinceLayer& provinces) {
    using R = Result<MapData, Error>;
    if (auto ok = validate_provinces(physical, provinces); !ok) {
        return R::err(ok.error());
    }
    return R::ok(assemble(PhysicalLayer(physical), ProvinceLayer(provinces)));
}

Result<MapData, Error> load_map_set(std::span<const std::uint8_t> physical_bytes,
                                    std::span<const std::uint8_t> province_bytes,
                                    const ProvinceReadOptions& options) {
    using R = Result<MapData, Error>;
    PhysicalReadOptions po;
    po.verify_raw = options.verify_raw;
    po.threads = options.threads;
    auto physical = read_physical(physical_bytes, po);
    if (!physical) {
        return R::err(physical.error());
    }
    auto reader = open_bytes(province_bytes, options);
    if (!reader) {
        return R::err(reader.error());
    }
    return load_set(reader.value(), std::move(physical.value()), options);
}

Result<MapData, Error> load_map_set(const std::filesystem::path& physical_path,
                                    const std::filesystem::path& provinces_path,
                                    const ProvinceReadOptions& options) {
    using R = Result<MapData, Error>;
    PhysicalReadOptions po;
    po.verify_raw = options.verify_raw;
    po.threads = options.threads;
    auto physical = read_physical_file(physical_path, po);
    if (!physical) {
        return R::err(physical.error());
    }
    auto reader = open_path(provinces_path, options);
    if (!reader) {
        return R::err(reader.error());
    }
    return load_set(reader.value(), std::move(physical.value()), options);
}

}  // namespace pat::mapdata
