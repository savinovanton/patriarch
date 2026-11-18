#pragma once

#include "pat/core/result.hpp"
#include "pat/sim/save.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The list of saves on the disk (E13-T04, #369): reads the summary of every `*.psv` in a directory without
// decompressing a body (sim::read_save_summary), tells apart compatible, newer-version, corrupt and
// foreign-content files, sorts newest first, gives saves unique file names, renames and deletes them and
// removes the leftovers of an interrupted write. Platform-free: the host passes the directory (the
// platform path is chosen in the application, Q1 #357). Depends only on core and sim.

namespace pat::game {

/// A path (directory + file name + ".psv.tmp") must stay within this many characters (Windows MAX_PATH
/// with a margin).
inline constexpr std::size_t kMaxSavePathLength = 240;
inline constexpr std::string_view kSaveExtension = ".psv";

enum class SaveClass : std::uint8_t {
    Compatible,      ///< readable by this build (a legacy version-1 file included, without a summary)
    Newer,           ///< written by a newer build (container version or state schema): cannot be loaded
    Corrupt,         ///< header or summary is damaged, or the file is not a save at all
    ForeignContent,  ///< readable, but made with another content set than ScanOptions::expected_content_hash
};

struct SaveEntry {
    std::filesystem::path path;
    std::string file_stem;  ///< file name without ".psv", UTF-8
    std::uint64_t file_size = 0;
    std::int64_t file_mtime = 0;              ///< Unix seconds
    std::uint16_t format_version = 0;         ///< 0 when the header could not be read
    std::optional<sim::SaveSummary> summary;  ///< absent for a legacy file and for a corrupt one
    SaveClass cls = SaveClass::Corrupt;
    int error_code = 0;  ///< core::Error::code of the failed read, 0 if none
    /// What the list orders by: summary.saved_at when it is set, otherwise the file time.
    std::int64_t sort_time = 0;
};

struct ScanOptions {
    /// Content hash of the loaded content set; 0 = do not check. A save whose summary carries a different
    /// non-zero hash is ForeignContent (a save without content, hash 0, is always compatible).
    std::uint64_t expected_content_hash = 0;
};

/// True for the container error codes that mean "written by a newer build" (22 version, 28 schema).
[[nodiscard]] bool is_newer_save_error(int code);

/// Lists every `*.psv` file in `dir` (not recursive), newest first (sort_time descending, ties by file
/// name). A missing directory gives an empty list; a damaged file never stops the scan.
[[nodiscard]] std::vector<SaveEntry> scan_saves(const std::filesystem::path& dir,
                                                const ScanOptions& options = {});

/// Removes `*.psv.tmp` files left by a write that was interrupted (sim::write_file_atomic). Call it once
/// at start-up, not while a background save may be running. Returns how many were removed.
std::size_t remove_orphan_temp_files(const std::filesystem::path& dir);

/// A valid Windows file name stem from free text: forbidden characters and controls become '_', trailing
/// dots and spaces are dropped, reserved device names (CON, NUL, COM1 ...) get a '_' prefix, an empty
/// result becomes "save". Cyrillic and spaces are kept. Not shortened (see unique_save_path).
[[nodiscard]] std::string sanitize_save_name(std::string_view name);

/// A path `dir/<stem>.psv` that does not exist yet: the name is sanitized, shortened (at a character
/// boundary) so that `<path>.tmp` fits kMaxSavePathLength, and " (2)", " (3)" ... is added on a clash.
/// Error 61: the directory itself leaves no room for a name.
[[nodiscard]] core::Result<std::filesystem::path, core::Error>
unique_save_path(const std::filesystem::path& dir, std::string_view name);

/// Renames a save file within its directory to the sanitized `new_name` (unique, see unique_save_path;
/// the file's own name does not count as a clash). Returns the new path. Errors: 62 not an existing
/// `.psv` file, 61 no room, 63 the rename failed. The summary inside the file (its optional display
/// name) is not rewritten: the file name is the name of the save in the list.
[[nodiscard]] core::Result<std::filesystem::path, core::Error> rename_save(const std::filesystem::path& path,
                                                                           std::string_view new_name);

/// Deletes a save file (no recycle bin). Errors: 62 not an existing `.psv` file, 64 removal failed.
[[nodiscard]] core::Result<void, core::Error> delete_save(const std::filesystem::path& path);

}  // namespace pat::game
