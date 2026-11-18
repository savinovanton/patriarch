#pragma once

#include "pat/core/result.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <system_error>
#include <vector>

namespace pat::core {

/// Which step of a file operation failed; callers map it to their own error codes and texts.
enum class FileIoStage : std::uint8_t {
    kOpen,    // cannot open the file (read) or the temporary file (write)
    kSize,    // cannot determine the size of the file being read
    kWrite,   // writing (or flushing) the temporary file failed
    kRename,  // cannot move the temporary file into place
    kRead,    // reading the file failed
    kVerify,  // the written temporary file was rejected by the caller's check (write_file_atomic)
};

struct FileIoError {
    FileIoStage stage{};
    /// The operating-system reason when one is known (errno after a failed open or write, the error
    /// code of a failed rename); empty otherwise. Lets callers tell "disk full" from "access denied".
    std::error_code ec{};
};

/// Optional steps of write_file_atomic.
struct AtomicWriteOptions {
    /// Called with the path of the finished temporary file before anything is replaced; false rejects
    /// the write (stage kVerify, the temporary file is removed, the old file is untouched).
    std::function<bool(const std::filesystem::path&)> verify;
    /// Non-empty: an existing `path` is renamed to this file (replacing an older one) right before the
    /// final rename, so the previous version survives as a backup. If the final rename then fails the
    /// backup is moved back.
    std::filesystem::path backup_path;
};

/// Writes `bytes` to `<path>.tmp`, flushes, then renames over `path` (atomic replace on the same
/// volume). If the rename fails the temporary file is removed. Stages: kOpen, kWrite, kRename.
[[nodiscard]] Result<void, FileIoError> write_file_atomic(const std::filesystem::path& path,
                                                          std::span<const std::uint8_t> bytes);
/// Same with a verification step and an optional backup of the previous file (see AtomicWriteOptions).
[[nodiscard]] Result<void, FileIoError> write_file_atomic(const std::filesystem::path& path,
                                                          std::span<const std::uint8_t> bytes,
                                                          const AtomicWriteOptions& options);

/// Reads a whole file as bytes. Stages: kOpen, kSize (the size cannot be determined), kRead.
[[nodiscard]] Result<std::vector<std::uint8_t>, FileIoError>
read_file_bytes(const std::filesystem::path& path);

/// Reads a whole file as text (bytes as they are, no newline translation). Stages: kOpen, kRead.
[[nodiscard]] Result<std::string, FileIoError> read_file_text(const std::filesystem::path& path);

}  // namespace pat::core
