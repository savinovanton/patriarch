#pragma once

#include "pat/core/result.hpp"
#include "pat/sim/command.hpp"
#include "pat/sim/world.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <future>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

// Save games (E1-T10, TZ FR-SAVE-1; container version 2 is E13-T02, docs/dev/save-format.md section 2).
//
// File layout, little endian. The magic stays "PSV1" for every container version, so that an older
// build that meets a newer file reports "created by a newer version" (code 22) and not "not a save".
//
// Version 2 (written by this build):
//   "PSV1" | u16 format_version = 2 | u16 flags (0) | u32 summary_size | u64 summary_hash |
//   u64 uncompressed_size | u64 payload_hash | u64 compressed_size | summary | zstd frame
// The summary (SaveSummary) is stored uncompressed between the 44-byte header and the zstd frame, with
// its own hash, so a save list can read it without touching zstd. It is not part of payload_hash.
//
// Version 1 (read forever, never written any more):
//   "PSV1" | u16 format_version = 1 | u16 flags (0) | u64 uncompressed_size | u64 payload_hash |
//   u64 compressed_size | zstd frame
//
// The payload (before compression) is the same in both: World::save_state() followed by the commands
// that systems emitted but that were not applied yet (u32 count + commands), so a loaded game continues
// exactly where the saved one stopped. Systems must not keep persistent state outside the World.
//
// The container version (kSaveFormatVersion) is NOT the layout of the world: that is the schema
// word at the start of World::save_state() (World::kStateSchema, byte layout in world_io.cpp).
// World::load_state() reads every schema from 1 up and migrates it step by step, so the container
// version only changes with the container layout (ADR-0006 D6, ADR-0011); migrate_to_current() in
// save.cpp is reserved for transformations of the raw payload bytes.

namespace pat::sim {

inline constexpr std::uint16_t kSaveFormatVersion = 2;
/// The first container version, without a summary. Files of this version are still read.
inline constexpr std::uint16_t kSaveFormatVersionLegacy = 1;

inline constexpr std::size_t kSaveHeaderSizeV1 = 32;
inline constexpr std::size_t kSaveHeaderSize = 44;     ///< header of the current version, before the summary
inline constexpr std::size_t kMaxSaveSummary = 16384;  ///< summary_size upper bound (and the preview limit)
inline constexpr std::size_t kMaxSummaryFields = 64;

/// player_dynasty of a game without a player ("observer" mode).
inline constexpr std::uint32_t kNoPlayerDynasty = 0xFFFFFFFFU;

/// Why a save was written (summary key 14).
enum class SaveKind : std::uint8_t {
    Manual = 0,
    Auto = 1,
    Quick = 2,
    AutoExit = 3,
};
inline constexpr std::uint8_t kMaxSaveKind = 3;

/// The "face" of a save: everything a save list shows, readable without decompressing the world.
/// Field numbers, limits and meaning: docs/dev/save-format.md section 2.3. Text limits (bytes):
/// game_version / scenario / dynasty_key / goal_key 64, head_name 96, name 256 (1..64 characters).
/// An empty optional text field means "absent" and is not written.
struct SaveSummary {
    std::string game_version;                          ///< 1: display and diagnostics only
    std::uint32_t state_schema = World::kStateSchema;  ///< 2: must equal the schema word of the payload
    std::string scenario;                              ///< 3: "m1", ...
    std::uint64_t content_hash = 0;                    ///< 4: 0 = no content set (demo, dynasty mode)
    std::uint32_t player_dynasty = kNoPlayerDynasty;   ///< 5
    std::string dynasty_key;                           ///< 6 (optional)
    std::string head_name;                             ///< 7 (optional): already formatted text
    std::optional<std::uint8_t> head_age;              ///< 8
    std::int32_t date = 0;                             ///< 9: Date::days()
    std::optional<std::int64_t> treasury;              ///< 10: kopecks; absent = economy is off
    std::string goal_key;                              ///< 11 (optional)
    std::optional<std::uint32_t> goal_progress_ppm;    ///< 12: 0..1000000
    std::uint64_t play_time_s = 0;                     ///< 13: real playing time, seconds
    SaveKind kind = SaveKind::Manual;                  ///< 14
    std::string name;                                  ///< 15 (optional): given by the player
    std::int64_t saved_at = 0;          ///< 16: Unix time (UTC), seconds; never read by the simulation
    std::uint64_t world_hash = 0;       ///< 17: WorldHash::total at the moment of saving
    std::int64_t tick = 0;              ///< 18: calendar().tick_count()
    std::uint8_t ironman = 0;           ///< 19: reserved (M2); written only when non-zero
    std::vector<std::uint8_t> preview;  ///< 20: reserved (M2 screenshot); empty = absent

    [[nodiscard]] bool operator==(const SaveSummary&) const = default;
};

/// The part of the summary that the world itself knows: schema, date, tick, content hash, world hash,
/// build version. Everything else (scenario, player, head, treasury, kind, name, play time, saved_at)
/// is for the caller. Deterministic: no clock is read.
[[nodiscard]] SaveSummary summary_from_world(const World& world);

/// What a reader of the container header learns without decompressing anything.
struct SaveInfo {
    std::uint16_t format_version = 0;
    std::uint64_t uncompressed_size = 0;
    std::uint64_t compressed_size = 0;
    /// Absent for the legacy version 1 (no summary): a save list then shows the file name and mtime.
    std::optional<SaveSummary> summary;
};

struct SaveData {
    World world;
    std::vector<Command> pending_internal;
    /// The summary of the file; for a legacy file (version 1) the minimum taken from the world
    /// (summary_from_world), with `format_version == 1`.
    SaveSummary summary;
    std::uint16_t format_version = kSaveFormatVersion;
};

/// Serializes a world (plus pending internal commands) into the compressed save container of the
/// current version. Text fields of the summary are cut to their limits (at a character boundary, NUL
/// removed) and a preview that would not fit in kMaxSaveSummary is dropped, so the writer always
/// produces a file the reader accepts.
[[nodiscard]] std::vector<std::uint8_t>
serialize_save(const World& world, std::span<const Command> pending_internal, const SaveSummary& summary);

/// Same, with the minimum summary taken from the world (summary_from_world). Deterministic.
[[nodiscard]] std::vector<std::uint8_t> serialize_save(const World& world,
                                                       std::span<const Command> pending_internal = {});

/// Parses a save (version 1 or 2). Detects truncation, bit flips, unknown versions and inconsistent
/// worlds. Error codes: docs/dev/save-format.md section 2.5.
[[nodiscard]] core::Result<SaveData, core::Error> deserialize_save(std::span<const std::uint8_t> bytes);

/// Reads the header and the summary and nothing else: zstd is never touched. `bytes` may be a prefix of
/// the file (the first 44 + summary_size bytes are enough); `file_size` is the size of the whole file,
/// used to check compressed_size (0 = `bytes` is the whole file).
[[nodiscard]] core::Result<SaveInfo, core::Error> read_save_summary(std::span<const std::uint8_t> bytes,
                                                                    std::uint64_t file_size = 0);

/// Same for a file: reads only the header and the summary from the disk (at most 44 + 16 KiB).
[[nodiscard]] core::Result<SaveInfo, core::Error> read_save_summary(const std::filesystem::path& path);

/// Writes via a temporary file and renames it, so a crash never leaves a half-written save.
[[nodiscard]] core::Result<void, core::Error> write_file_atomic(const std::filesystem::path& path,
                                                                std::span<const std::uint8_t> bytes);
[[nodiscard]] core::Result<std::vector<std::uint8_t>, core::Error>
read_file(const std::filesystem::path& path);

/// Saves a snapshot on a background thread: the immutable copy keeps the running simulation free
/// to continue. The future yields the outcome of serialization and writing. With a summary the file
/// carries it; without, the minimum from the world.
[[nodiscard]] std::future<core::Result<void, core::Error>> save_async(std::shared_ptr<const World> snapshot,
                                                                      std::vector<Command> pending_internal,
                                                                      std::filesystem::path path);
[[nodiscard]] std::future<core::Result<void, core::Error>> save_async(std::shared_ptr<const World> snapshot,
                                                                      std::vector<Command> pending_internal,
                                                                      std::filesystem::path path,
                                                                      SaveSummary summary);

}  // namespace pat::sim
