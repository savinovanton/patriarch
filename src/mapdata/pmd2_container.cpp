#include "pat/mapdata/pmd2_container.hpp"

#include "pat/core/bytes.hpp"
#include "pat/core/compress.hpp"
#include "pat/core/hash_stream.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <system_error>
#include <thread>
#include <type_traits>
#include <utility>

namespace pat::mapdata {

namespace {

using core::Error;
using core::Result;

constexpr std::array<std::uint8_t, 4> kMagic = {'P', 'M', 'D', '2'};
constexpr std::size_t kRasterPrefixFixed = 16;  // elem, shift, tile_log2, transform, w, h, tile_count
constexpr std::size_t kTileIndexEntry = 12;     // u32 stored_size + u64 stored_hash
constexpr std::uint8_t kMaxShift = 3;
constexpr std::uint16_t kKnownChunkFlags = kPmd2ChunkCritical | kPmd2ChunkTiled;

Error pmd2_err(int code, std::string message) {
    return Error{code, std::move(message)};
}

std::uint64_t fnv(std::span<const std::uint8_t> bytes) {
    core::HashStream h;
    h.bytes(bytes);
    return h.value();
}

bool printable_id(Pmd2ChunkId id) {
    for (unsigned shift = 0; shift < 32; shift += 8) {
        const std::uint32_t c = (id >> shift) & 0xFFU;
        if (c < 0x20U || c > 0x7EU) {
            return false;
        }
    }
    return true;
}

std::uint32_t ceil_shift(std::uint32_t v, unsigned shift) {
    return static_cast<std::uint32_t>((static_cast<std::uint64_t>(v) + ((1ULL << shift) - 1U)) >> shift);
}

std::uint32_t div_ceil(std::uint32_t v, std::uint32_t d) {
    return static_cast<std::uint32_t>((static_cast<std::uint64_t>(v) + d - 1U) / d);
}

unsigned resolve_threads(unsigned requested) {
    if (requested != 0) {
        return requested;
    }
    return std::max(1U, std::thread::hardware_concurrency());
}

/// Runs fn(i) for i in [0, count) on up to `threads` workers (i % workers == worker). The work items
/// must write to disjoint memory; with one worker (or one item) everything runs on the caller.
template <class Fn>
void parallel_for(std::size_t count, unsigned threads, Fn&& fn) {
    const std::size_t workers = std::min<std::size_t>(resolve_threads(threads), count);
    if (workers <= 1) {
        for (std::size_t i = 0; i < count; ++i) {
            fn(i);
        }
        return;
    }
    const auto run = [&](std::size_t first) {
        for (std::size_t i = first; i < count; i += workers) {
            fn(i);
        }
    };
    std::vector<std::thread> pool;
    pool.reserve(workers - 1);
    for (std::size_t w = 1; w < workers; ++w) {
        pool.emplace_back(run, w);
    }
    run(0);
    for (std::thread& t : pool) {
        t.join();
    }
}

// ---- tile transform -------------------------------------------------------------------------------
// Canonical tile bytes: elements row-major, u16 little endian. transform 1 turns every row into
// wrapping deltas (first element of a row is absolute) and splits the 2-byte case into a low-byte
// plane followed by a high-byte plane, exactly like PMD1 but per tile.

std::vector<std::uint8_t> forward_transform(const std::vector<std::uint8_t>& canon,
                                            std::uint32_t tw,
                                            std::uint32_t th,
                                            unsigned elem,
                                            std::uint8_t transform) {
    if (transform == 0) {
        return canon;
    }
    const std::size_t n = static_cast<std::size_t>(tw) * th;
    std::vector<std::uint8_t> out(n * elem);
    std::size_t i = 0;
    for (std::uint32_t y = 0; y < th; ++y) {
        for (std::uint32_t x = 0; x < tw; ++x, ++i) {
            if (elem == 1) {
                out[i] = x == 0 ? canon[i] : static_cast<std::uint8_t>(canon[i] - canon[i - 1]);
            } else {
                const auto cur = static_cast<std::uint16_t>(canon[2 * i] | (canon[2 * i + 1] << 8U));
                std::uint16_t d = cur;
                if (x != 0) {
                    const auto prev = static_cast<std::uint16_t>(canon[2 * i - 2] | (canon[2 * i - 1] << 8U));
                    d = static_cast<std::uint16_t>(cur - prev);
                }
                out[i] = static_cast<std::uint8_t>(d & 0xFFU);
                out[n + i] = static_cast<std::uint8_t>(d >> 8U);
            }
        }
    }
    return out;
}

std::vector<std::uint8_t> inverse_transform(const std::vector<std::uint8_t>& stored,
                                            std::uint32_t tw,
                                            std::uint32_t th,
                                            unsigned elem,
                                            std::uint8_t transform) {
    if (transform == 0) {
        return stored;
    }
    const std::size_t n = static_cast<std::size_t>(tw) * th;
    std::vector<std::uint8_t> out(n * elem);
    std::size_t i = 0;
    for (std::uint32_t y = 0; y < th; ++y) {
        for (std::uint32_t x = 0; x < tw; ++x, ++i) {
            if (elem == 1) {
                out[i] = x == 0 ? stored[i] : static_cast<std::uint8_t>(out[i - 1] + stored[i]);
            } else {
                auto v = static_cast<std::uint16_t>(stored[i] | (stored[n + i] << 8U));
                if (x != 0) {
                    const auto prev = static_cast<std::uint16_t>(out[2 * i - 2] | (out[2 * i - 1] << 8U));
                    v = static_cast<std::uint16_t>(prev + v);
                }
                out[2 * i] = static_cast<std::uint8_t>(v & 0xFFU);
                out[2 * i + 1] = static_cast<std::uint8_t>(v >> 8U);
            }
        }
    }
    return out;
}

template <class T>
void put_element(std::uint8_t* dst, T v) {
    dst[0] = static_cast<std::uint8_t>(v & 0xFFU);
    if constexpr (sizeof(T) == 2) {
        dst[1] = static_cast<std::uint8_t>(v >> 8U);
    }
}

std::uint64_t content_id_of(Pmd2Kind kind, const std::vector<Pmd2ChunkInfo>& chunks) {
    core::HashStream h;
    h.u16(static_cast<std::uint16_t>(kind));
    for (const Pmd2ChunkInfo& c : chunks) {
        h.u8(static_cast<std::uint8_t>(c.id >> 24U));
        h.u8(static_cast<std::uint8_t>(c.id >> 16U));
        h.u8(static_cast<std::uint8_t>(c.id >> 8U));
        h.u8(static_cast<std::uint8_t>(c.id));
        h.u16(c.version);
        h.u8(c.instance);
        h.u64(c.raw_hash);
    }
    return h.value();
}

void write_id(core::ByteWriter& w, Pmd2ChunkId id) {
    w.u8(static_cast<std::uint8_t>(id >> 24U));
    w.u8(static_cast<std::uint8_t>(id >> 16U));
    w.u8(static_cast<std::uint8_t>(id >> 8U));
    w.u8(static_cast<std::uint8_t>(id));
}

Pmd2ChunkId read_id(core::ByteReader& r) {
    const std::uint32_t a = r.u8();
    const std::uint32_t b = r.u8();
    const std::uint32_t c = r.u8();
    const std::uint32_t d = r.u8();
    return (a << 24U) | (b << 16U) | (c << 8U) | d;
}

}  // namespace

std::string pmd2_chunk_id_string(Pmd2ChunkId id) {
    std::string s(4, '?');
    for (std::size_t i = 0; i < 4; ++i) {
        const auto c = static_cast<unsigned char>((id >> (24U - 8U * i)) & 0xFFU);
        if (c >= 0x20U && c <= 0x7EU) {
            s[i] = static_cast<char>(c);
        }
    }
    return s;
}

Pmd2TileRect Pmd2RasterInfo::tile_rect(std::uint32_t index) const noexcept {
    const std::uint32_t size = 1U << tile_log2;
    Pmd2TileRect r;
    r.x = (index % tiles_x) * size;
    r.y = (index / tiles_x) * size;
    r.w = std::min(size, width - r.x);
    r.h = std::min(size, height - r.y);
    return r;
}

// ============================================================================================ writer

Pmd2Writer::Pmd2Writer(Pmd2Kind kind, std::uint32_t width, std::uint32_t height, Pmd2WriterOptions options)
    : kind_(kind), width_(width), height_(height), options_(options) {}

Result<void, Error> Pmd2Writer::check_new_chunk(const Pmd2ChunkSpec& spec) const {
    using R = Result<void, Error>;
    if (width_ == 0 || height_ == 0 || width_ > kPmd2MaxDimension || height_ > kPmd2MaxDimension ||
        static_cast<std::uint64_t>(width_) * height_ > kPmd2MaxPixels) {
        return R::err(pmd2_err(pmd2_error::kBadArgument, "invalid PMD2 dimensions"));
    }
    if (!printable_id(spec.id)) {
        return R::err(pmd2_err(pmd2_error::kBadArgument, "chunk id must be four printable ASCII characters"));
    }
    if (chunks_.size() >= kPmd2MaxChunks) {
        return R::err(pmd2_err(pmd2_error::kBadArgument, "too many chunks (limit 64)"));
    }
    for (const Chunk& c : chunks_) {
        if (c.info.id == spec.id && c.info.instance == spec.instance) {
            return R::err(
                pmd2_err(pmd2_error::kBadArgument, "duplicate chunk " + pmd2_chunk_id_string(spec.id)));
        }
    }
    return R::ok();
}

Result<void, Error> Pmd2Writer::check_raster_params(const Pmd2RasterParams& params) const {
    using R = Result<void, Error>;
    if (params.shift > kMaxShift || params.tile_log2 < kPmd2MinTileLog2 ||
        params.tile_log2 > kPmd2MaxTileLog2 || params.transform > 1) {
        return R::err(pmd2_err(pmd2_error::kBadArgument, "invalid raster parameters"));
    }
    return R::ok();
}

Result<void, Error> Pmd2Writer::add_blob(const Pmd2ChunkSpec& spec, std::span<const std::uint8_t> bytes) {
    using R = Result<void, Error>;
    if (auto ok = check_new_chunk(spec); !ok) {
        return ok;
    }
    if (bytes.size() > kPmd2MaxBlobBytes) {
        return R::err(pmd2_err(pmd2_error::kBadArgument, "blob chunk is too large"));
    }
    Chunk c;
    c.info.id = spec.id;
    c.info.version = spec.version;
    c.info.flags = spec.critical ? kPmd2ChunkCritical : 0U;
    c.info.codec = options_.codec;
    c.info.instance = spec.instance;
    c.info.raw_size = bytes.size();
    c.info.raw_hash = fnv(bytes);
    c.body = options_.codec == Pmd2Codec::zstd ? core::compress(bytes, options_.zstd_level)
                                               : std::vector<std::uint8_t>(bytes.begin(), bytes.end());
    c.info.stored_size = c.body.size();
    c.info.stored_hash = fnv(c.body);
    chunks_.push_back(std::move(c));
    return R::ok();
}

template <class T>
Result<void, Error> Pmd2Writer::add_raster_typed(const Pmd2ChunkSpec& spec,
                                                 const Pmd2RasterParams& params,
                                                 std::span<const T> pixels) {
    static_assert(std::is_same_v<T, std::uint8_t> || std::is_same_v<T, std::uint16_t>);
    using R = Result<void, Error>;
    constexpr unsigned elem = sizeof(T);
    if (auto ok = check_new_chunk(spec); !ok) {
        return ok;
    }
    if (auto ok = check_raster_params(params); !ok) {
        return ok;
    }
    const std::uint32_t w = ceil_shift(width_, params.shift);
    const std::uint32_t h = ceil_shift(height_, params.shift);
    if (pixels.size() != static_cast<std::size_t>(w) * h) {
        return R::err(pmd2_err(pmd2_error::kBadArgument,
                               "raster " + pmd2_chunk_id_string(spec.id) + " must hold " + std::to_string(w) +
                                   " x " + std::to_string(h) + " elements"));
    }
    Pmd2RasterInfo info;
    info.elem_bytes = static_cast<std::uint8_t>(elem);
    info.shift = params.shift;
    info.tile_log2 = params.tile_log2;
    info.transform = params.transform;
    info.width = w;
    info.height = h;
    const std::uint32_t tile = 1U << params.tile_log2;
    info.tiles_x = div_ceil(w, tile);
    info.tiles_y = div_ceil(h, tile);
    const std::uint32_t tile_count = info.tile_count();

    std::vector<std::vector<std::uint8_t>> stored(tile_count);
    const Pmd2WriterOptions& opt = options_;
    parallel_for(tile_count, opt.threads, [&](std::size_t t) {
        const Pmd2TileRect r = info.tile_rect(static_cast<std::uint32_t>(t));
        std::vector<std::uint8_t> canon(static_cast<std::size_t>(r.w) * r.h * elem);
        for (std::uint32_t row = 0; row < r.h; ++row) {
            const T* src = pixels.data() + static_cast<std::size_t>(r.y + row) * w + r.x;
            std::uint8_t* dst = canon.data() + static_cast<std::size_t>(row) * r.w * elem;
            for (std::uint32_t x = 0; x < r.w; ++x) {
                put_element<T>(dst + static_cast<std::size_t>(x) * elem, src[x]);
            }
        }
        std::vector<std::uint8_t> transformed = forward_transform(canon, r.w, r.h, elem, params.transform);
        stored[t] = opt.codec == Pmd2Codec::zstd ? core::compress(transformed, opt.zstd_level)
                                                 : std::move(transformed);
    });

    core::ByteWriter prefix;
    prefix.u8(static_cast<std::uint8_t>(elem));
    prefix.u8(params.shift);
    prefix.u8(params.tile_log2);
    prefix.u8(params.transform);
    prefix.u32(w);
    prefix.u32(h);
    prefix.u32(tile_count);
    std::uint64_t total = prefix.size() + static_cast<std::uint64_t>(tile_count) * kTileIndexEntry;
    for (const auto& s : stored) {
        prefix.u32(static_cast<std::uint32_t>(s.size()));
        prefix.u64(fnv(s));
        total += s.size();
    }

    core::HashStream raw;
    for (const T v : pixels) {
        if constexpr (elem == 1) {
            raw.u8(v);
        } else {
            raw.u16(v);
        }
    }

    Chunk c;
    c.info.id = spec.id;
    c.info.version = spec.version;
    c.info.flags = static_cast<std::uint16_t>((spec.critical ? kPmd2ChunkCritical : 0U) | kPmd2ChunkTiled);
    c.info.codec = opt.codec;
    c.info.instance = spec.instance;
    c.info.raw_size = static_cast<std::uint64_t>(pixels.size()) * elem;
    c.info.raw_hash = raw.value();
    c.info.stored_hash = fnv(prefix.data());
    c.info.stored_size = total;
    c.body = prefix.take();
    c.body.reserve(static_cast<std::size_t>(total));
    for (const auto& s : stored) {
        c.body.insert(c.body.end(), s.begin(), s.end());
    }
    chunks_.push_back(std::move(c));
    return R::ok();
}

Result<void, Error> Pmd2Writer::add_raster(const Pmd2ChunkSpec& spec,
                                           const Pmd2RasterParams& params,
                                           std::span<const std::uint8_t> pixels) {
    return add_raster_typed<std::uint8_t>(spec, params, pixels);
}

Result<void, Error> Pmd2Writer::add_raster(const Pmd2ChunkSpec& spec,
                                           const Pmd2RasterParams& params,
                                           std::span<const std::uint16_t> pixels) {
    return add_raster_typed<std::uint16_t>(spec, params, pixels);
}

Result<void, Error> Pmd2Writer::add_stored_chunk(const Pmd2ChunkInfo& info,
                                                 std::span<const std::uint8_t> stored_bytes) {
    using R = Result<void, Error>;
    Pmd2ChunkSpec spec;
    spec.id = info.id;
    spec.instance = info.instance;
    if (auto ok = check_new_chunk(spec); !ok) {
        return ok;
    }
    if (stored_bytes.size() != info.stored_size) {
        return R::err(pmd2_err(pmd2_error::kBadArgument, "stored chunk size does not match its entry"));
    }
    Chunk c;
    c.info = info;
    c.info.understood = false;
    c.body.assign(stored_bytes.begin(), stored_bytes.end());
    chunks_.push_back(std::move(c));
    return R::ok();
}

Result<std::vector<std::uint8_t>, Error> Pmd2Writer::finish() const {
    using R = Result<std::vector<std::uint8_t>, Error>;
    if (width_ == 0 || height_ == 0 || width_ > kPmd2MaxDimension || height_ > kPmd2MaxDimension ||
        static_cast<std::uint64_t>(width_) * height_ > kPmd2MaxPixels) {
        return R::err(pmd2_err(pmd2_error::kBadArgument, "invalid PMD2 dimensions"));
    }
    std::vector<const Chunk*> order;
    order.reserve(chunks_.size());
    for (const Chunk& c : chunks_) {
        order.push_back(&c);
    }
    std::sort(order.begin(), order.end(), [](const Chunk* a, const Chunk* b) {
        return a->info.id != b->info.id ? a->info.id < b->info.id : a->info.instance < b->info.instance;
    });

    std::uint64_t offset = kPmd2HeaderSize + kPmd2ChunkEntrySize * order.size();
    std::vector<Pmd2ChunkInfo> infos;
    infos.reserve(order.size());
    core::ByteWriter table;
    for (const Chunk* c : order) {
        Pmd2ChunkInfo info = c->info;
        info.offset = offset;
        offset += info.stored_size;
        write_id(table, info.id);
        table.u16(info.version);
        table.u16(info.flags);
        table.u8(static_cast<std::uint8_t>(info.codec));
        table.u8(info.instance);
        for (int i = 0; i < 6; ++i) {
            table.u8(0);
        }
        table.u64(info.offset);
        table.u64(info.stored_size);
        table.u64(info.raw_size);
        table.u64(info.raw_hash);
        table.u64(info.stored_hash);
        infos.push_back(info);
    }

    core::ByteWriter head;
    head.bytes(kMagic);
    head.u16(0);  // minor of the files this writer produces
    head.u16(static_cast<std::uint16_t>(kind_));
    head.u32(0);  // flags
    head.u32(width_);
    head.u32(height_);
    head.u32(static_cast<std::uint32_t>(order.size()));
    head.u64(offset);  // file_size
    head.u64(content_id_of(kind_, infos));
    head.u64(bind_key_);
    head.u64(0);  // reserved
    core::HashStream hh;
    hh.bytes(head.data());
    hh.bytes(table.data());
    head.u64(hh.value());

    core::ByteWriter out;
    out.bytes(head.data());
    out.bytes(table.data());
    for (const Chunk* c : order) {
        out.bytes(c->body);
    }
    return R::ok(out.take());
}

Result<void, Error> Pmd2Writer::write_file(const std::filesystem::path& path) const {
    using R = Result<void, Error>;
    auto bytes = finish();
    if (!bytes) {
        return R::err(bytes.error());
    }
    std::filesystem::path temp = path;
    temp += ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) {
            return R::err(pmd2_err(pmd2_error::kIo, "cannot create " + temp.string()));
        }
        out.write(reinterpret_cast<const char*>(bytes.value().data()),
                  static_cast<std::streamsize>(bytes.value().size()));
        out.flush();
        if (!out) {
            return R::err(pmd2_err(pmd2_error::kIo, "write failed: " + temp.string()));
        }
    }
    std::error_code ec;
    std::filesystem::rename(temp, path, ec);  // Atomic replace on the same volume.
    if (ec) {
        std::filesystem::remove(temp, ec);
        return R::err(pmd2_err(pmd2_error::kIo, "cannot move the file into place: " + path.string()));
    }
    return R::ok();
}

// ============================================================================================ reader

Result<Pmd2Reader, Error> Pmd2Reader::open(std::span<const std::uint8_t> bytes,
                                           const Pmd2OpenOptions& options) {
    using R = Result<Pmd2Reader, Error>;
    Pmd2Reader reader;
    reader.view_ = bytes;
    if (auto ok = reader.parse(options); !ok) {
        return R::err(ok.error());
    }
    return R::ok(std::move(reader));
}

Result<Pmd2Reader, Error> Pmd2Reader::open_file(const std::filesystem::path& path,
                                                const Pmd2OpenOptions& options) {
    using R = Result<Pmd2Reader, Error>;
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) {
        return R::err(pmd2_err(pmd2_error::kIo, "cannot open " + path.string()));
    }
    const std::streamsize size = in.tellg();
    if (size < 0) {
        return R::err(pmd2_err(pmd2_error::kIo, "cannot determine the size of " + path.string()));
    }
    in.seekg(0);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    in.read(reinterpret_cast<char*>(bytes.data()), size);
    if (!in) {
        return R::err(pmd2_err(pmd2_error::kIo, "read failed: " + path.string()));
    }
    Pmd2Reader reader;
    reader.owned_ = std::move(bytes);
    reader.view_ = reader.owned_;
    if (auto ok = reader.parse(options); !ok) {
        return R::err(ok.error());
    }
    return R::ok(std::move(reader));
}

Result<void, Error> Pmd2Reader::parse(const Pmd2OpenOptions& options) {
    using R = Result<void, Error>;
    const std::span<const std::uint8_t> bytes = view_;
    if (bytes.size() < kMagic.size()) {
        return R::err(pmd2_err(pmd2_error::kTruncated, "PMD2 file is truncated (header)"));
    }
    if (std::memcmp(bytes.data(), kMagic.data(), kMagic.size()) != 0) {
        return R::err(pmd2_err(pmd2_error::kBadMagic, "not a PMD2 file (bad magic)"));
    }
    if (bytes.size() < kPmd2HeaderSize) {
        return R::err(pmd2_err(pmd2_error::kTruncated, "PMD2 file is truncated (header)"));
    }

    core::ByteReader in(bytes);
    (void)in.bytes(kMagic.size());
    header_.minor = in.u16();
    const std::uint16_t kind = in.u16();
    header_.flags = in.u32();
    header_.width = in.u32();
    header_.height = in.u32();
    header_.chunk_count = in.u32();
    header_.file_size = in.u64();
    header_.content_id = in.u64();
    header_.bind_key = in.u64();
    const std::uint64_t reserved = in.u64();
    const std::uint64_t header_hash = in.u64();

    if (header_.chunk_count > kPmd2MaxChunks) {
        return R::err(pmd2_err(pmd2_error::kBadHeader, "too many chunks (limit 64)"));
    }
    const std::size_t table_end = kPmd2HeaderSize + kPmd2ChunkEntrySize * header_.chunk_count;
    if (bytes.size() < table_end) {
        return R::err(pmd2_err(pmd2_error::kTruncated, "PMD2 file is truncated (chunk table)"));
    }
    {
        core::HashStream h;
        h.bytes(bytes.first(kPmd2HeaderSize - 8));
        h.bytes(bytes.subspan(kPmd2HeaderSize, table_end - kPmd2HeaderSize));
        if (h.value() != header_hash) {
            return R::err(pmd2_err(pmd2_error::kHeaderHash, "PMD2 header is corrupted (hash mismatch)"));
        }
    }

    if (kind < 1 || kind > 3) {
        return R::err(pmd2_err(pmd2_error::kBadHeader, "unknown PMD2 kind " + std::to_string(kind)));
    }
    header_.kind = static_cast<Pmd2Kind>(kind);
    const bool strict_fields = header_.minor <= kPmd2KnownMinor;
    if (strict_fields && (header_.flags != 0 || reserved != 0)) {
        return R::err(pmd2_err(pmd2_error::kBadHeader, "unsupported PMD2 flags or reserved fields"));
    }
    if (header_.width == 0 || header_.height == 0 || header_.width > kPmd2MaxDimension ||
        header_.height > kPmd2MaxDimension ||
        static_cast<std::uint64_t>(header_.width) * header_.height > kPmd2MaxPixels) {
        return R::err(pmd2_err(pmd2_error::kBadHeader, "invalid PMD2 dimensions"));
    }
    if (header_.file_size != bytes.size()) {
        return R::err(pmd2_err(pmd2_error::kFileSize,
                               header_.file_size > bytes.size()
                                   ? "PMD2 file is truncated (shorter than file_size)"
                                   : "trailing bytes after the last PMD2 chunk"));
    }

    // Chunk table.
    chunks_.clear();
    chunks_.reserve(header_.chunk_count);
    for (std::uint32_t i = 0; i < header_.chunk_count; ++i) {
        Pmd2ChunkInfo c;
        c.id = read_id(in);
        c.version = in.u16();
        c.flags = in.u16();
        const std::uint8_t codec = in.u8();
        c.instance = in.u8();
        std::uint64_t pad = 0;
        for (int k = 0; k < 6; ++k) {
            pad |= in.u8();
        }
        c.offset = in.u64();
        c.stored_size = in.u64();
        c.raw_size = in.u64();
        c.raw_hash = in.u64();
        c.stored_hash = in.u64();
        c.codec = static_cast<Pmd2Codec>(codec);
        if (!in.ok()) {
            return R::err(pmd2_err(pmd2_error::kTruncated, "PMD2 file is truncated (chunk table)"));
        }
        const std::string name = pmd2_chunk_id_string(c.id);
        if (!printable_id(c.id)) {
            return R::err(pmd2_err(pmd2_error::kBadChunkEntry, "chunk id is not ASCII"));
        }
        if (strict_fields && (pad != 0 || (c.flags & ~kKnownChunkFlags) != 0)) {
            return R::err(pmd2_err(pmd2_error::kBadChunkEntry, "chunk " + name + " has reserved bits set"));
        }
        const bool codec_known = codec <= static_cast<std::uint8_t>(Pmd2Codec::zstd);
        if (!codec_known && strict_fields) {
            return R::err(pmd2_err(pmd2_error::kBadChunkEntry, "chunk " + name + " has an unknown codec"));
        }
        // Ranges: inside the file, after the table.
        if (c.offset < table_end || c.offset > header_.file_size ||
            c.stored_size > header_.file_size - c.offset) {
            return R::err(pmd2_err(pmd2_error::kChunkRange, "chunk " + name + " is outside the file"));
        }
        for (const Pmd2ChunkInfo& o : chunks_) {
            if (o.id == c.id && o.instance == c.instance) {
                return R::err(pmd2_err(pmd2_error::kDuplicateChunk, "duplicate chunk " + name));
            }
            if (o.stored_size != 0 && c.stored_size != 0 && c.offset < o.offset + o.stored_size &&
                o.offset < c.offset + c.stored_size) {
                return R::err(pmd2_err(pmd2_error::kChunkRange, "chunk " + name + " overlaps another chunk"));
            }
        }

        // Is the chunk understood by the caller?
        bool understood = codec_known;
        if (understood && !options.inspect) {
            understood = false;
            for (const Pmd2KnownChunk& k : options.known) {
                if (k.id == c.id) {
                    understood = c.version <= k.max_version;
                    if (!understood && c.critical()) {
                        return R::err(pmd2_err(pmd2_error::kUnsupportedChunk,
                                               "chunk " + name + " has an unsupported version " +
                                                   std::to_string(c.version)));
                    }
                    break;
                }
            }
            if (!understood && c.critical() &&
                std::none_of(options.known.begin(), options.known.end(), [&](const Pmd2KnownChunk& k) {
                    return k.id == c.id;
                })) {
                return R::err(pmd2_err(pmd2_error::kUnknownCritical, "unknown critical chunk " + name));
            }
        } else if (!understood && c.critical()) {
            return R::err(
                pmd2_err(pmd2_error::kUnsupportedChunk, "chunk " + name + " uses an unknown codec"));
        }
        c.understood = understood;

        // Sizes that do not depend on the content.
        if (!c.tiled()) {
            if (c.raw_size > kPmd2MaxBlobBytes) {
                return R::err(pmd2_err(pmd2_error::kBadChunkEntry, "chunk " + name + " is too large"));
            }
            if (c.codec == Pmd2Codec::raw && codec_known && c.stored_size != c.raw_size) {
                return R::err(
                    pmd2_err(pmd2_error::kBadChunkEntry, "chunk " + name + " has inconsistent sizes"));
            }
        }
        chunks_.push_back(c);
    }
    {
        std::uint64_t sum = 0;
        for (const Pmd2ChunkInfo& c : chunks_) {
            sum += c.stored_size;  // Each is bounded by file_size and ranges do not overlap: no overflow.
        }
        if (table_end + sum != header_.file_size) {
            return R::err(pmd2_err(pmd2_error::kChunkRange, "chunk sizes do not add up to the file size"));
        }
    }
    if (content_id_of(header_.kind, chunks_) != header_.content_id) {
        return R::err(pmd2_err(pmd2_error::kContentId, "content_id does not match the chunk table"));
    }

    // stored_hash of every chunk (raster: the prefix; tiles are checked when read).
    layouts_.assign(chunks_.size(), std::nullopt);
    for (std::size_t i = 0; i < chunks_.size(); ++i) {
        const Pmd2ChunkInfo& c = chunks_[i];
        const std::string name = pmd2_chunk_id_string(c.id);
        if (!c.tiled()) {
            if (fnv(stored_bytes(c)) != c.stored_hash) {
                return R::err(
                    pmd2_err(pmd2_error::kStoredHash, "chunk " + name + " is corrupted (hash mismatch)"));
            }
            continue;
        }
        if (static_cast<std::uint8_t>(c.codec) > static_cast<std::uint8_t>(Pmd2Codec::zstd)) {
            continue;  // Unknown codec, optional chunk (a critical one was rejected above).
        }
        auto layout = parse_raster(c);
        if (!layout) {
            return R::err(layout.error());
        }
        layouts_[i] = std::move(layout).value();
    }

    if (options.verify_raw) {
        return verify_raw(options.threads);
    }
    return R::ok();
}

Result<Pmd2Reader::RasterLayout, Error> Pmd2Reader::parse_raster(const Pmd2ChunkInfo& info) const {
    using R = Result<RasterLayout, Error>;
    const std::string name = pmd2_chunk_id_string(info.id);
    const std::span<const std::uint8_t> body = stored_bytes(info);
    if (body.size() < kRasterPrefixFixed) {
        return R::err(pmd2_err(pmd2_error::kBadRaster, "raster " + name + " is truncated"));
    }
    core::ByteReader in(body);
    RasterLayout layout;
    Pmd2RasterInfo& r = layout.info;
    r.elem_bytes = in.u8();
    r.shift = in.u8();
    r.tile_log2 = in.u8();
    r.transform = in.u8();
    r.width = in.u32();
    r.height = in.u32();
    const std::uint32_t tile_count = in.u32();
    layout.prefix_size = kRasterPrefixFixed + static_cast<std::uint64_t>(tile_count) * kTileIndexEntry;
    if (layout.prefix_size > body.size()) {
        return R::err(pmd2_err(pmd2_error::kBadRaster, "raster " + name + " has a truncated tile index"));
    }
    if (fnv(body.first(static_cast<std::size_t>(layout.prefix_size))) != info.stored_hash) {
        return R::err(
            pmd2_err(pmd2_error::kStoredHash, "raster " + name + " is corrupted (index hash mismatch)"));
    }

    if ((r.elem_bytes != 1 && r.elem_bytes != 2) || r.shift > kMaxShift || r.tile_log2 < kPmd2MinTileLog2 ||
        r.tile_log2 > kPmd2MaxTileLog2 || r.transform > 1) {
        return R::err(pmd2_err(pmd2_error::kBadRaster, "raster " + name + " has unsupported parameters"));
    }
    if (r.width != ceil_shift(header_.width, r.shift) || r.height != ceil_shift(header_.height, r.shift)) {
        return R::err(pmd2_err(pmd2_error::kBadRaster, "raster " + name + " does not match the grid size"));
    }
    const std::uint32_t size = 1U << r.tile_log2;
    r.tiles_x = div_ceil(r.width, size);
    r.tiles_y = div_ceil(r.height, size);
    if (static_cast<std::uint64_t>(r.tiles_x) * r.tiles_y != tile_count) {
        return R::err(pmd2_err(pmd2_error::kBadRaster, "raster " + name + " has a wrong tile count"));
    }
    if (info.raw_size != static_cast<std::uint64_t>(r.width) * r.height * r.elem_bytes) {
        return R::err(pmd2_err(pmd2_error::kBadRaster, "raster " + name + " has a wrong raw size"));
    }

    layout.tile_offset.reserve(tile_count);
    layout.tile_size.reserve(tile_count);
    layout.tile_hash.reserve(tile_count);
    std::uint64_t cursor = info.offset + layout.prefix_size;
    std::uint64_t sum = layout.prefix_size;
    for (std::uint32_t t = 0; t < tile_count; ++t) {
        const std::uint32_t stored = in.u32();
        const std::uint64_t hash = in.u64();
        if (info.codec == Pmd2Codec::raw) {
            const Pmd2TileRect rect = r.tile_rect(t);
            if (stored != static_cast<std::uint64_t>(rect.w) * rect.h * r.elem_bytes) {
                return R::err(
                    pmd2_err(pmd2_error::kBadRaster, "raster " + name + " has a wrong raw tile size"));
            }
        }
        layout.tile_offset.push_back(cursor);
        layout.tile_size.push_back(stored);
        layout.tile_hash.push_back(hash);
        cursor += stored;
        sum += stored;
        if (sum > info.stored_size) {
            return R::err(pmd2_err(pmd2_error::kBadRaster, "raster " + name + " tiles exceed the chunk"));
        }
    }
    if (!in.ok() || sum != info.stored_size) {
        return R::err(pmd2_err(pmd2_error::kBadRaster, "raster " + name + " tile sizes do not add up"));
    }
    return R::ok(std::move(layout));
}

const Pmd2ChunkInfo* Pmd2Reader::find(Pmd2ChunkId id, std::uint8_t instance) const noexcept {
    const auto index = find_index(id, instance);
    return index ? &chunks_[*index] : nullptr;
}

std::optional<std::size_t> Pmd2Reader::find_index(Pmd2ChunkId id, std::uint8_t instance) const noexcept {
    for (std::size_t i = 0; i < chunks_.size(); ++i) {
        if (chunks_[i].id == id && chunks_[i].instance == instance) {
            return i;
        }
    }
    return std::nullopt;
}

std::span<const std::uint8_t> Pmd2Reader::stored_bytes(const Pmd2ChunkInfo& info) const noexcept {
    return view_.subspan(static_cast<std::size_t>(info.offset), static_cast<std::size_t>(info.stored_size));
}

Result<std::vector<std::uint8_t>, Error> Pmd2Reader::read_blob(Pmd2ChunkId id, std::uint8_t instance) const {
    using R = Result<std::vector<std::uint8_t>, Error>;
    const auto index = find_index(id, instance);
    if (!index) {
        return R::err(pmd2_err(pmd2_error::kNotFound, "no chunk " + pmd2_chunk_id_string(id)));
    }
    const Pmd2ChunkInfo& c = chunks_[*index];
    if (c.tiled() || static_cast<std::uint8_t>(c.codec) > static_cast<std::uint8_t>(Pmd2Codec::zstd)) {
        return R::err(
            pmd2_err(pmd2_error::kNotFound, "chunk " + pmd2_chunk_id_string(id) + " is not a blob"));
    }
    const std::span<const std::uint8_t> stored = stored_bytes(c);
    if (c.codec == Pmd2Codec::raw) {
        return R::ok(std::vector<std::uint8_t>(stored.begin(), stored.end()));
    }
    auto raw = core::decompress(stored, static_cast<std::size_t>(c.raw_size));
    if (!raw) {
        return R::err(
            pmd2_err(pmd2_error::kDecode,
                     "chunk " + pmd2_chunk_id_string(id) + " is corrupted: " + raw.error().message));
    }
    return R::ok(std::move(raw).value());
}

Result<Pmd2RasterInfo, Error> Pmd2Reader::raster_info(Pmd2ChunkId id, std::uint8_t instance) const {
    using R = Result<Pmd2RasterInfo, Error>;
    const auto index = find_index(id, instance);
    if (!index || !layouts_[*index]) {
        return R::err(pmd2_err(pmd2_error::kNotFound, "no raster chunk " + pmd2_chunk_id_string(id)));
    }
    return R::ok(layouts_[*index]->info);
}

Result<std::vector<std::uint8_t>, Error> Pmd2Reader::decode_tile(std::size_t chunk_index,
                                                                 std::uint32_t tile) const {
    using R = Result<std::vector<std::uint8_t>, Error>;
    const Pmd2ChunkInfo& c = chunks_[chunk_index];
    const RasterLayout& layout = *layouts_[chunk_index];
    const std::string name = pmd2_chunk_id_string(c.id) + " tile " + std::to_string(tile);
    const std::span<const std::uint8_t> stored =
        view_.subspan(static_cast<std::size_t>(layout.tile_offset[tile]), layout.tile_size[tile]);
    if (fnv(stored) != layout.tile_hash[tile]) {
        return R::err(pmd2_err(pmd2_error::kTileHash, name + " is corrupted (hash mismatch)"));
    }
    const Pmd2TileRect rect = layout.info.tile_rect(tile);
    const std::size_t raw_size = static_cast<std::size_t>(rect.w) * rect.h * layout.info.elem_bytes;
    std::vector<std::uint8_t> payload;
    if (c.codec == Pmd2Codec::zstd) {
        auto raw = core::decompress(stored, raw_size);
        if (!raw) {
            return R::err(pmd2_err(pmd2_error::kDecode, name + " is corrupted: " + raw.error().message));
        }
        payload = std::move(raw).value();
    } else {
        payload.assign(stored.begin(), stored.end());
    }
    return R::ok(inverse_transform(payload, rect.w, rect.h, layout.info.elem_bytes, layout.info.transform));
}

Result<std::vector<std::uint8_t>, Error>
Pmd2Reader::read_tile(Pmd2ChunkId id, std::uint32_t tile_index, std::uint8_t instance) const {
    using R = Result<std::vector<std::uint8_t>, Error>;
    const auto index = find_index(id, instance);
    if (!index || !layouts_[*index]) {
        return R::err(pmd2_err(pmd2_error::kNotFound, "no raster chunk " + pmd2_chunk_id_string(id)));
    }
    if (tile_index >= layouts_[*index]->info.tile_count()) {
        return R::err(pmd2_err(pmd2_error::kNotFound, "tile index out of range"));
    }
    return decode_tile(*index, tile_index);
}

Result<std::vector<std::uint8_t>, Error> Pmd2Reader::read_raster_at(std::size_t chunk_index,
                                                                    unsigned threads) const {
    using R = Result<std::vector<std::uint8_t>, Error>;
    const Pmd2ChunkInfo& c = chunks_[chunk_index];
    const Pmd2RasterInfo& info = layouts_[chunk_index]->info;
    std::vector<std::uint8_t> out(static_cast<std::size_t>(c.raw_size));
    const std::uint32_t tile_count = info.tile_count();
    std::vector<std::optional<Error>> errors(tile_count);
    parallel_for(tile_count, threads, [&](std::size_t t) {
        auto tile = decode_tile(chunk_index, static_cast<std::uint32_t>(t));
        if (!tile) {
            errors[t] = tile.error();
            return;
        }
        const Pmd2TileRect r = info.tile_rect(static_cast<std::uint32_t>(t));
        const std::size_t row_bytes = static_cast<std::size_t>(r.w) * info.elem_bytes;
        for (std::uint32_t row = 0; row < r.h; ++row) {
            std::memcpy(out.data() +
                            ((static_cast<std::size_t>(r.y + row) * info.width) + r.x) * info.elem_bytes,
                        tile.value().data() + row * row_bytes,
                        row_bytes);
        }
    });
    for (const auto& e : errors) {  // The lowest failing tile wins: the same error on every run.
        if (e) {
            return R::err(*e);
        }
    }
    return R::ok(std::move(out));
}

Result<std::vector<std::uint8_t>, Error>
Pmd2Reader::read_raster(Pmd2ChunkId id, std::uint8_t instance, unsigned threads) const {
    using R = Result<std::vector<std::uint8_t>, Error>;
    const auto index = find_index(id, instance);
    if (!index || !layouts_[*index]) {
        return R::err(pmd2_err(pmd2_error::kNotFound, "no raster chunk " + pmd2_chunk_id_string(id)));
    }
    return read_raster_at(*index, threads);
}

Result<std::vector<std::uint8_t>, Error>
Pmd2Reader::read_raster_u8(Pmd2ChunkId id, std::uint8_t instance, unsigned threads) const {
    using R = Result<std::vector<std::uint8_t>, Error>;
    const auto index = find_index(id, instance);
    if (!index || !layouts_[*index] || layouts_[*index]->info.elem_bytes != 1) {
        return R::err(pmd2_err(pmd2_error::kNotFound, "no 8-bit raster chunk " + pmd2_chunk_id_string(id)));
    }
    return read_raster_at(*index, threads);
}

Result<std::vector<std::uint16_t>, Error>
Pmd2Reader::read_raster_u16(Pmd2ChunkId id, std::uint8_t instance, unsigned threads) const {
    using R = Result<std::vector<std::uint16_t>, Error>;
    const auto index = find_index(id, instance);
    if (!index || !layouts_[*index] || layouts_[*index]->info.elem_bytes != 2) {
        return R::err(pmd2_err(pmd2_error::kNotFound, "no 16-bit raster chunk " + pmd2_chunk_id_string(id)));
    }
    auto bytes = read_raster_at(*index, threads);
    if (!bytes) {
        return R::err(bytes.error());
    }
    std::vector<std::uint16_t> out(bytes.value().size() / 2);
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = static_cast<std::uint16_t>(bytes.value()[2 * i] | (bytes.value()[2 * i + 1] << 8U));
    }
    return R::ok(std::move(out));
}

Result<void, Error> Pmd2Reader::verify_chunk_raw(std::size_t chunk_index, unsigned threads) const {
    using R = Result<void, Error>;
    const Pmd2ChunkInfo& c = chunks_[chunk_index];
    std::uint64_t hash = 0;
    if (c.tiled()) {
        auto raster = read_raster_at(chunk_index, threads);
        if (!raster) {
            return R::err(raster.error());
        }
        hash = fnv(raster.value());
    } else {
        auto blob = read_blob(c.id, c.instance);
        if (!blob) {
            return R::err(blob.error());
        }
        hash = fnv(blob.value());
    }
    if (hash != c.raw_hash) {
        return R::err(pmd2_err(pmd2_error::kRawHash,
                               "chunk " + pmd2_chunk_id_string(c.id) + " content does not match raw_hash"));
    }
    return R::ok();
}

Result<void, Error> Pmd2Reader::verify_raw(unsigned threads) const {
    for (std::size_t i = 0; i < chunks_.size(); ++i) {
        if (!chunks_[i].understood) {
            continue;
        }
        if (auto ok = verify_chunk_raw(i, threads); !ok) {
            return ok;
        }
    }
    return Result<void, Error>::ok();
}

}  // namespace pat::mapdata
