#pragma once

#include "pat/core/result.hpp"

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace pat::core {

/// Which step of a file operation failed; callers map it to their own error codes and texts.
enum class FileIoStage : std::uint8_t {
    kOpen,    // cannot open the file (read) or the temporary file (write)
    kSize,    // cannot determine the size of the file being read
    kWrite,   // writing (or flushing) the temporary file failed
    kRename,  // cannot move the temporary file into place
    kRead,    // reading the file failed
};

struct FileIoError {
    FileIoStage stage{};
};

/// Writes `bytes` to `<path>.tmp`, flushes, then renames over `path` (atomic replace on the same
/// volume). If the rename fails the temporary file is removed. Stages: kOpen, kWrite, kRename.
[[nodiscard]] Result<void, FileIoError> write_file_atomic(const std::filesystem::path& path,
                                                          std::span<const std::uint8_t> bytes);

/// Reads a whole file as bytes. Stages: kOpen, kSize (the size cannot be determined), kRead.
[[nodiscard]] Result<std::vector<std::uint8_t>, FileIoError>
read_file_bytes(const std::filesystem::path& path);

/// Reads a whole file as text (bytes as they are, no newline translation). Stages: kOpen, kRead.
[[nodiscard]] Result<std::string, FileIoError> read_file_text(const std::filesystem::path& path);

}  // namespace pat::core
