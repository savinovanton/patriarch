#pragma once

#include "pat/core/result.hpp"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

// Where content files come from (E4-T04, docs/dev/data-loader.md). The loader works against this
// small interface so the whole pipeline can be tested from in-memory text; the directory
// implementation is the only place in the data module that touches the file system.

namespace pat::data {

/// One content root (the base game or a mod): a set of files addressed by relative paths with '/'
/// separators, e.g. "traits/core.pd". Implementations may return paths in any order; the loader sorts
/// them itself, so correctness never depends on directory iteration order.
class ContentSource {
public:
    virtual ~ContentSource() = default;

    /// Human-readable name used in diagnostics (a directory path, a mod name, ...).
    [[nodiscard]] virtual std::string name() const = 0;

    /// Relative paths of ALL files below the root (recursively), in any order. An error is a
    /// human-readable reason (missing directory, access denied, ...).
    [[nodiscard]] virtual core::Result<std::vector<std::string>, std::string> list_files() const = 0;

    /// Contents of one file previously returned by list_files(). An error is a human-readable reason.
    [[nodiscard]] virtual core::Result<std::string, std::string> read_file(std::string_view path) const = 0;
};

/// A root held in memory; used by tests and tools. Files are listed in insertion order.
class MemorySource final : public ContentSource {
public:
    explicit MemorySource(std::string name = "memory") : name_(std::move(name)) {}

    /// Adds a file; adding the same path again replaces its text.
    MemorySource& add(std::string path, std::string text);
    /// Adds a file that is listed but cannot be read (simulates an I/O error).
    MemorySource& add_unreadable(std::string path, std::string reason = "access denied");
    /// Makes list_files() fail with this reason (simulates a missing or unreadable root).
    MemorySource& fail_listing(std::string reason);

    [[nodiscard]] std::string name() const override { return name_; }
    [[nodiscard]] core::Result<std::vector<std::string>, std::string> list_files() const override;
    [[nodiscard]] core::Result<std::string, std::string> read_file(std::string_view path) const override;

private:
    struct File {
        std::string path;
        std::string text;
        bool readable = true;
        std::string reason;
    };

    std::string name_;
    std::vector<File> files_;
    bool listing_fails_ = false;
    std::string listing_reason_;
};

/// A directory on disk. Subdirectories are traversed recursively (symbolic links to directories are
/// not followed); only regular files are listed; paths are relative to the directory and use '/'.
class DirectorySource final : public ContentSource {
public:
    explicit DirectorySource(std::filesystem::path directory) : directory_(std::move(directory)) {}

    [[nodiscard]] std::string name() const override;
    [[nodiscard]] core::Result<std::vector<std::string>, std::string> list_files() const override;
    [[nodiscard]] core::Result<std::string, std::string> read_file(std::string_view path) const override;

private:
    std::filesystem::path directory_;
};

}  // namespace pat::data
