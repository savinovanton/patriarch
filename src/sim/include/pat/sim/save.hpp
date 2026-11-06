#pragma once

#include "pat/core/result.hpp"
#include "pat/sim/command.hpp"
#include "pat/sim/world.hpp"

#include <cstdint>
#include <filesystem>
#include <future>
#include <memory>
#include <span>
#include <vector>

// Save games (E1-T10, TZ FR-SAVE-1).
//
// File layout (little endian):
//   "PSV1" | u16 format_version | u16 flags (0) | u64 uncompressed_size | u64 payload_hash |
//   u64 compressed_size | zstd frame
// The payload (before compression) is: World::save_state() followed by the commands that systems
// emitted but that were not applied yet (u32 count + commands), so a loaded game continues exactly
// where the saved one stopped. Systems must not keep persistent state outside the World.

namespace pat::sim {

inline constexpr std::uint16_t kSaveFormatVersion = 1;

struct SaveData {
    World world;
    std::vector<Command> pending_internal;
};

/// Serializes a world (plus pending internal commands) into the compressed save container.
[[nodiscard]] std::vector<std::uint8_t> serialize_save(const World& world,
                                                       std::span<const Command> pending_internal = {});

/// Parses a save. Detects truncation, bit flips, unknown versions and inconsistent worlds.
[[nodiscard]] core::Result<SaveData, core::Error> deserialize_save(std::span<const std::uint8_t> bytes);

/// Writes via a temporary file and renames it, so a crash never leaves a half-written save.
[[nodiscard]] core::Result<void, core::Error> write_file_atomic(const std::filesystem::path& path,
                                                                std::span<const std::uint8_t> bytes);
[[nodiscard]] core::Result<std::vector<std::uint8_t>, core::Error>
read_file(const std::filesystem::path& path);

/// Saves a snapshot on a background thread: the immutable copy keeps the running simulation free
/// to continue. The future yields the outcome of serialization and writing.
[[nodiscard]] std::future<core::Result<void, core::Error>> save_async(std::shared_ptr<const World> snapshot,
                                                                      std::vector<Command> pending_internal,
                                                                      std::filesystem::path path);

}  // namespace pat::sim
