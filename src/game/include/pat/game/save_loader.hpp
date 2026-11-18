#pragma once

#include "pat/content/session_assembly.hpp"
#include "pat/game/save_catalog.hpp"
#include "pat/sim/save.hpp"
#include "pat/sim/simulation.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>

// Loading a game (E13-T07, #372, docs/dev/save-format.md 2.4 and 8): reads a save file, decides its
// compatibility, builds the Simulation with the same assembly as a new game (content::assemble_session)
// and tells the "Restoring the save" screen where it is. The caller's current session is never touched:
// a failed or cancelled load only returns a result, the host swaps sessions on Ok. Platform-free; runs on
// any thread (the host usually uses a worker and reads the progress callback's data from the UI).

namespace pat::game {

enum class LoadOutcome : std::uint8_t {
    Ok,
    Incompatible,    ///< readable, but not for this build's content (see LoadReason)
    Corrupt,         ///< damaged, truncated, unreadable; `backup_available` tells about a `.bak`
    NeedsNewerGame,  ///< written by a newer build (container version or state schema)
    Cancelled,       ///< the caller cancelled between stages; nothing was changed
    NothingToLoad,   ///< continue_game(): no compatible save in the directory
};

/// Why a load did not succeed: picks the player-facing text (`newer_format`, `different_content`,
/// `corrupted`, `unreadable_disk`, `unknown` of docs/dev/save-format.md 8.2). Details go to the log.
enum class LoadReason : std::uint8_t {
    None,
    NewerFormat,       ///< NeedsNewerGame
    DifferentContent,  ///< the content hash differs, or the world does not fit the loaded content
    NoMigration,       ///< a legacy file with no migration path to this build
    Damaged,           ///< hashes, zstd, invariants, truncation
    UnreadableDisk,    ///< the file could not be opened or read
    Unknown,
};

/// The stages of the restore screen: texts come from the localization tables by these ids.
enum class LoadStage : std::uint8_t {
    ReadingHistory,     ///< read the file and the world ("Reading the history")
    BuildingWorld,      ///< check the content, assemble the Simulation ("Building the world")
    CheckingIntegrity,  ///< compare the world hash with the summary ("Checking integrity")
};

struct LoadProgress {
    LoadStage stage = LoadStage::ReadingHistory;
    int percent = 0;  ///< 0..100 over the whole load, never decreases
};

struct LoadOptions {
    /// `--dev` (E4-T07): the content hash is not compared; format and schema are still checked.
    bool ignore_content_hash = false;
    /// Called when a stage starts and when it ends, from the loading thread.
    std::function<void(const LoadProgress&)> on_progress;
    /// Polled between stages; true cancels the load (the result is Cancelled, nothing else happens).
    std::function<bool()> cancelled;
};

struct LoadResult {
    LoadOutcome outcome = LoadOutcome::Corrupt;
    LoadReason reason = LoadReason::Unknown;
    std::string message;            ///< English diagnostic for the log, never shown to the player
    int error_code = 0;             ///< core::Error::code behind it, 0 if none
    bool backup_available = false;  ///< Corrupt only: `<file>.bak` exists (docs/dev/save-format.md 9.5)
    std::filesystem::path path;
    std::optional<sim::SaveSummary> summary;    ///< when the header could be read
    std::optional<sim::Simulation> simulation;  ///< only for Ok
};

/// Loads one save with the given content set. Steps and their order: docs/dev/save-format.md 2.4.
[[nodiscard]] LoadResult load_game(const std::filesystem::path& path,
                                   const content::SessionContent& content,
                                   const LoadOptions& options = {});

/// "Continue": the newest save of the list that the catalog calls Compatible (any kind), or nullopt.
/// `entries` come from scan_saves() (newest first).
[[nodiscard]] std::optional<std::filesystem::path> pick_continue_save(std::span<const SaveEntry> entries);

/// Scans `dir` (the content hash of `content` as the expected one) and loads the newest compatible save.
/// Nothing to continue gives NothingToLoad. A corrupt newest save is reported as Corrupt, not skipped:
/// the restore screen offers the next one (docs/dev/save-format.md 9.5).
[[nodiscard]] LoadResult continue_game(const std::filesystem::path& dir,
                                       const content::SessionContent& content,
                                       const LoadOptions& options = {});

}  // namespace pat::game
