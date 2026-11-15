#pragma once

#include "pat/core/result.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

// PMD2 container (ADR-0007 section 2, Geo-10). This is the generic part of the format: header, chunk
// table, blobs and tiled rasters, checksums. Which chunks exist (HGHT, WBOD, PTBL, ...) is decided by
// the layer code on top (Geo-11 and later); the container only moves bytes and guards integrity.
//
// All integers are little endian. Layout:
//
//   header (64 bytes)       "PMD2" | u16 minor | u16 kind | u32 flags | u32 width | u32 height |
//                           u32 chunk_count | u64 file_size | u64 content_id | u64 bind_key |
//                           u64 reserved | u64 header_hash
//   chunk table             chunk_count x 56 bytes:
//                           4 id bytes | u16 version | u16 flags | u8 codec | u8 instance |
//                           6 reserved | u64 offset | u64 stored_size | u64 raw_size |
//                           u64 raw_hash | u64 stored_hash
//   chunk bodies            back to back in table order, no padding
//
// Blob chunk: the body is the data, stored raw (codec 0) or as one zstd frame (codec 1).
// Tiled raster chunk (flag bit 1): the body is
//   u8 elem_bytes (1|2) | u8 shift (0..3) | u8 tile_log2 (6..12) | u8 transform (0 none, 1 delta+planes) |
//   u32 w | u32 h | u32 tile_count | tile_count x { u32 stored_size; u64 stored_hash } | tiles...
// where w x h = ceil(width / 2^shift) x ceil(height / 2^shift) and tiles run row by row, left to
// right; every tile is independent: transform(tile) then zstd (codec 1) or raw (codec 0).
//
// Checksums are FNV-1a 64 (core::HashStream):
//   header_hash   header bytes 0..55 and the whole chunk table; checked first, always.
//   stored_hash   blob: the stored bytes; raster: the raster prefix (everything before the first tile,
//                 which holds the stored_hash of every tile); checked when the file is opened. A tile
//                 is checked against its own stored_hash before it is decompressed.
//   raw_hash      the canonical content: blob = raw bytes, raster = elements row by row (u16 as little
//                 endian), independent of tile size, transform and zstd level. Checked only in strict
//                 mode (Pmd2OpenOptions::verify_raw).
//   content_id    FNV-1a over (u16 kind, then per chunk: 4 id bytes, u16 version, u8 instance,
//                 u64 raw_hash) in table order; identical for identical content however it is stored.

namespace pat::mapdata {

inline constexpr std::size_t kPmd2HeaderSize = 64;
inline constexpr std::size_t kPmd2ChunkEntrySize = 56;
inline constexpr std::uint32_t kPmd2MaxChunks = 64;
inline constexpr std::uint16_t kPmd2KnownMinor = 0;  ///< Highest minor this reader understands.
inline constexpr std::uint8_t kPmd2MinTileLog2 = 6;
inline constexpr std::uint8_t kPmd2MaxTileLog2 = 12;
inline constexpr std::uint8_t kPmd2DefaultTileLog2 = 10;
inline constexpr std::uint32_t kPmd2MaxDimension = 65535;
inline constexpr std::uint64_t kPmd2MaxPixels = 1ULL << 30;
inline constexpr std::uint64_t kPmd2MaxBlobBytes = 256ULL << 20;

inline constexpr std::uint16_t kPmd2ChunkCritical = 1U;  ///< The reader must understand the chunk.
inline constexpr std::uint16_t kPmd2ChunkTiled = 2U;     ///< Tiled raster layout (see above).

/// Error codes of the PMD2 container (the PMD1 loader uses 20..45).
namespace pmd2_error {
inline constexpr int kBadMagic = 100;
inline constexpr int kTruncated = 101;         ///< Header or chunk table cut short.
inline constexpr int kHeaderHash = 102;        ///< header_hash mismatch (corrupt header or table).
inline constexpr int kBadHeader = 103;         ///< Kind, flags, reserved, size or chunk_count invalid.
inline constexpr int kFileSize = 104;          ///< Actual size differs from file_size (truncation).
inline constexpr int kChunkRange = 105;        ///< Chunk out of the file, overlapping or leaving a gap.
inline constexpr int kDuplicateChunk = 106;    ///< (id, instance) appears twice.
inline constexpr int kBadChunkEntry = 107;     ///< Reserved fields, flags, codec or sizes invalid.
inline constexpr int kUnknownCritical = 108;   ///< Critical chunk this reader does not know.
inline constexpr int kUnsupportedChunk = 109;  ///< Critical chunk with a newer version or unknown codec.
inline constexpr int kContentId = 110;         ///< content_id does not match the chunk table.
inline constexpr int kStoredHash = 111;        ///< stored_hash mismatch (corrupt chunk body or index).
inline constexpr int kRawHash = 112;           ///< raw_hash mismatch (strict mode).
inline constexpr int kBadRaster = 113;         ///< Tiled raster prefix is inconsistent.
inline constexpr int kTileHash = 114;          ///< A tile does not match its stored_hash.
inline constexpr int kDecode = 115;            ///< Decompression failed or gave the wrong size.
inline constexpr int kNotFound = 116;          ///< No such chunk, tile or wrong chunk kind.
inline constexpr int kIo = 117;                ///< File could not be read or written.
inline constexpr int kBadArgument = 118;       ///< Writer given an invalid chunk or raster.
}  // namespace pmd2_error

/// Four ASCII characters; the first one is the most significant byte, so numeric order equals the
/// byte-wise order of the id in the file.
using Pmd2ChunkId = std::uint32_t;

[[nodiscard]] constexpr Pmd2ChunkId pmd2_chunk_id(char a, char b, char c, char d) noexcept {
    return (static_cast<std::uint32_t>(static_cast<unsigned char>(a)) << 24U) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(b)) << 16U) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(c)) << 8U) |
           static_cast<std::uint32_t>(static_cast<unsigned char>(d));
}

[[nodiscard]] std::string pmd2_chunk_id_string(Pmd2ChunkId id);

enum class Pmd2Kind : std::uint16_t { physical = 1, provinces = 2, patches = 3 };
enum class Pmd2Codec : std::uint8_t { raw = 0, zstd = 1 };

struct Pmd2Header {
    std::uint16_t minor = 0;
    Pmd2Kind kind = Pmd2Kind::physical;
    std::uint32_t flags = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t chunk_count = 0;
    std::uint64_t file_size = 0;
    std::uint64_t content_id = 0;
    std::uint64_t bind_key = 0;
};

struct Pmd2ChunkInfo {
    Pmd2ChunkId id = 0;
    std::uint16_t version = 0;
    std::uint16_t flags = 0;
    Pmd2Codec codec = Pmd2Codec::zstd;
    std::uint8_t instance = 0;
    std::uint64_t offset = 0;
    std::uint64_t stored_size = 0;
    std::uint64_t raw_size = 0;
    std::uint64_t raw_hash = 0;
    std::uint64_t stored_hash = 0;
    /// Reader only: true if the chunk is known (and its version supported) according to
    /// Pmd2OpenOptions. Unknown optional chunks are listed but must not be interpreted.
    bool understood = false;

    [[nodiscard]] bool critical() const noexcept { return (flags & kPmd2ChunkCritical) != 0; }
    [[nodiscard]] bool tiled() const noexcept { return (flags & kPmd2ChunkTiled) != 0; }
};

struct Pmd2TileRect {
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    std::uint32_t w = 0;
    std::uint32_t h = 0;
};

/// Geometry of a tiled raster chunk (raster size is already divided by 2^shift).
struct Pmd2RasterInfo {
    std::uint8_t elem_bytes = 1;
    std::uint8_t shift = 0;
    std::uint8_t tile_log2 = kPmd2DefaultTileLog2;
    std::uint8_t transform = 1;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t tiles_x = 0;
    std::uint32_t tiles_y = 0;

    [[nodiscard]] std::uint32_t tile_count() const noexcept { return tiles_x * tiles_y; }
    [[nodiscard]] Pmd2TileRect tile_rect(std::uint32_t index) const noexcept;
};

/// A chunk this reader knows how to interpret and the newest layout version it supports.
struct Pmd2KnownChunk {
    Pmd2ChunkId id = 0;
    std::uint16_t max_version = 1;
};

struct Pmd2OpenOptions {
    /// Chunks the caller understands. A critical chunk outside this list (or newer than listed) is an
    /// error; an optional one is skipped (listed with understood == false).
    std::vector<Pmd2KnownChunk> known;
    /// Tools (mapgen --info): accept every chunk with a supported codec as understood.
    bool inspect = false;
    /// Strict mode (CI, tools): decode every understood chunk and compare raw_hash.
    bool verify_raw = false;
    /// Worker threads for strict verification (0 = hardware concurrency, 1 = sequential).
    unsigned threads = 1;
};

struct Pmd2ChunkSpec {
    Pmd2ChunkId id = 0;
    std::uint16_t version = 1;
    bool critical = true;
    std::uint8_t instance = 0;
};

struct Pmd2RasterParams {
    std::uint8_t shift = 0;  ///< Raster is ceil(width / 2^shift) x ceil(height / 2^shift).
    std::uint8_t tile_log2 = kPmd2DefaultTileLog2;
    std::uint8_t transform = 1;  ///< 0 none, 1 row delta + byte planes.
};

struct Pmd2WriterOptions {
    int zstd_level = 3;
    Pmd2Codec codec = Pmd2Codec::zstd;
    /// Worker threads for tile compression (0 = hardware concurrency). Output does not depend on it.
    unsigned threads = 1;
};

/// Builds a PMD2 file in memory. Chunks may be added in any order; the file lists them by
/// (id, instance), so the output is deterministic.
class Pmd2Writer {
public:
    Pmd2Writer(Pmd2Kind kind, std::uint32_t width, std::uint32_t height, Pmd2WriterOptions options = {});

    void set_bind_key(std::uint64_t key) noexcept { bind_key_ = key; }

    /// A blob chunk (tables, provenance, ...). `bytes` are the raw content.
    [[nodiscard]] core::Result<void, core::Error> add_blob(const Pmd2ChunkSpec& spec,
                                                           std::span<const std::uint8_t> bytes);

    /// A tiled raster with 1 or 2 bytes per pixel, row-major, size = raster size for `params.shift`.
    [[nodiscard]] core::Result<void, core::Error> add_raster(const Pmd2ChunkSpec& spec,
                                                             const Pmd2RasterParams& params,
                                                             std::span<const std::uint8_t> pixels);
    [[nodiscard]] core::Result<void, core::Error> add_raster(const Pmd2ChunkSpec& spec,
                                                             const Pmd2RasterParams& params,
                                                             std::span<const std::uint16_t> pixels);

    /// Copies a chunk byte for byte (editing tools keep unknown optional chunks this way). The offset
    /// is recomputed; everything else, including the hashes, is taken from `info`.
    [[nodiscard]] core::Result<void, core::Error>
    add_stored_chunk(const Pmd2ChunkInfo& info, std::span<const std::uint8_t> stored_bytes);

    [[nodiscard]] core::Result<std::vector<std::uint8_t>, core::Error> finish() const;
    [[nodiscard]] core::Result<void, core::Error> write_file(const std::filesystem::path& path) const;

private:
    struct Chunk {
        Pmd2ChunkInfo info;
        std::vector<std::uint8_t> body;
    };

    [[nodiscard]] core::Result<void, core::Error> check_new_chunk(const Pmd2ChunkSpec& spec) const;
    [[nodiscard]] core::Result<void, core::Error> check_raster_params(const Pmd2RasterParams& params) const;
    template <class T>
    [[nodiscard]] core::Result<void, core::Error>
    add_raster_typed(const Pmd2ChunkSpec& spec, const Pmd2RasterParams& params, std::span<const T> pixels);

    Pmd2Kind kind_;
    std::uint32_t width_;
    std::uint32_t height_;
    Pmd2WriterOptions options_;
    std::uint64_t bind_key_ = 0;
    std::vector<Chunk> chunks_;
};

/// Reads a PMD2 file. open() validates the header, the chunk table and every chunk's stored_hash up
/// front (cheap: one pass over the compressed bytes); decompression happens on demand, tile by tile.
/// Any inconsistency is an error with a code from pmd2_error; there is no partial reading.
class Pmd2Reader {
public:
    Pmd2Reader(const Pmd2Reader&) = delete;
    Pmd2Reader& operator=(const Pmd2Reader&) = delete;
    Pmd2Reader(Pmd2Reader&&) noexcept = default;
    Pmd2Reader& operator=(Pmd2Reader&&) noexcept = default;
    ~Pmd2Reader() = default;

    /// `bytes` must outlive the reader.
    [[nodiscard]] static core::Result<Pmd2Reader, core::Error> open(std::span<const std::uint8_t> bytes,
                                                                    const Pmd2OpenOptions& options = {});
    /// Loads the file into memory (owned by the reader) and opens it.
    [[nodiscard]] static core::Result<Pmd2Reader, core::Error> open_file(const std::filesystem::path& path,
                                                                         const Pmd2OpenOptions& options = {});

    [[nodiscard]] const Pmd2Header& header() const noexcept { return header_; }
    [[nodiscard]] std::span<const Pmd2ChunkInfo> chunks() const noexcept { return chunks_; }
    [[nodiscard]] const Pmd2ChunkInfo* find(Pmd2ChunkId id, std::uint8_t instance = 0) const noexcept;

    /// The stored (compressed) bytes of a chunk, valid while the reader lives.
    [[nodiscard]] std::span<const std::uint8_t> stored_bytes(const Pmd2ChunkInfo& info) const noexcept;

    /// Decompressed content of a blob chunk.
    [[nodiscard]] core::Result<std::vector<std::uint8_t>, core::Error>
    read_blob(Pmd2ChunkId id, std::uint8_t instance = 0) const;

    [[nodiscard]] core::Result<Pmd2RasterInfo, core::Error> raster_info(Pmd2ChunkId id,
                                                                        std::uint8_t instance = 0) const;
    /// One tile, canonical bytes: tile_rect(index).w x .h elements, row-major, u16 little endian.
    [[nodiscard]] core::Result<std::vector<std::uint8_t>, core::Error>
    read_tile(Pmd2ChunkId id, std::uint32_t tile_index, std::uint8_t instance = 0) const;
    /// The whole raster, canonical bytes (see read_tile). `threads`: 0 = hardware concurrency.
    [[nodiscard]] core::Result<std::vector<std::uint8_t>, core::Error>
    read_raster(Pmd2ChunkId id, std::uint8_t instance = 0, unsigned threads = 1) const;
    [[nodiscard]] core::Result<std::vector<std::uint8_t>, core::Error>
    read_raster_u8(Pmd2ChunkId id, std::uint8_t instance = 0, unsigned threads = 1) const;
    [[nodiscard]] core::Result<std::vector<std::uint16_t>, core::Error>
    read_raster_u16(Pmd2ChunkId id, std::uint8_t instance = 0, unsigned threads = 1) const;

    /// Strict check: decodes every understood chunk and compares raw_hash.
    [[nodiscard]] core::Result<void, core::Error> verify_raw(unsigned threads = 1) const;

private:
    Pmd2Reader() = default;

    struct RasterLayout {
        Pmd2RasterInfo info;
        std::uint64_t prefix_size = 0;
        std::vector<std::uint64_t> tile_offset;  ///< Absolute file offsets.
        std::vector<std::uint32_t> tile_size;
        std::vector<std::uint64_t> tile_hash;
    };

    [[nodiscard]] core::Result<void, core::Error> parse(const Pmd2OpenOptions& options);
    [[nodiscard]] core::Result<RasterLayout, core::Error> parse_raster(const Pmd2ChunkInfo& info) const;
    [[nodiscard]] std::optional<std::size_t> find_index(Pmd2ChunkId id, std::uint8_t instance) const noexcept;
    [[nodiscard]] core::Result<std::vector<std::uint8_t>, core::Error> decode_tile(std::size_t chunk_index,
                                                                                   std::uint32_t tile) const;
    [[nodiscard]] core::Result<std::vector<std::uint8_t>, core::Error> read_raster_at(std::size_t chunk_index,
                                                                                      unsigned threads) const;
    [[nodiscard]] core::Result<void, core::Error> verify_chunk_raw(std::size_t chunk_index,
                                                                   unsigned threads) const;

    std::vector<std::uint8_t> owned_;
    std::span<const std::uint8_t> view_;
    Pmd2Header header_;
    std::vector<Pmd2ChunkInfo> chunks_;
    std::vector<std::optional<RasterLayout>> layouts_;  ///< Parallel to chunks_; set for tiled chunks.
};

}  // namespace pat::mapdata
