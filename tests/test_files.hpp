#pragma once

// Shared file helpers for tests: scratch directories, whole-file read and write, modification times.
// Header-only; include from tests only.

#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>

namespace pat::test {

/// Whole file as bytes in a string. Fails the test (REQUIRE) when the file cannot be opened.
inline std::string read_text(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    REQUIRE_MESSAGE(in.good(), "cannot open " << path.string());
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

/// Writes (replaces) a file in binary mode. The parent directory must exist.
inline void write_text(const std::filesystem::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

/// `<system temp>/<name>` (not created).
inline std::filesystem::path temp_path(const char* name) {
    return std::filesystem::temp_directory_path() / name;
}

/// `<system temp>/<name>`, emptied and (re)created.
inline std::filesystem::path fresh_dir(const char* name) {
    const std::filesystem::path dir = temp_path(name);
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}

/// Gives the file a strictly newer modification time than any previous touch_newer() (file times can
/// have coarse granularity, so two quick writes may otherwise carry the same timestamp).
inline void touch_newer(const std::filesystem::path& path) {
    static int generation = 0;
    ++generation;
    std::filesystem::last_write_time(
        path, std::filesystem::file_time_type::clock::now() + std::chrono::seconds(10 * generation));
}

/// A uniquely named scratch directory under the system temp directory, removed on destruction.
class TempDir {
public:
    explicit TempDir(const std::string& tag) {
        const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ =
            std::filesystem::temp_directory_path() / ("patriarch_test_" + tag + "_" + std::to_string(ticks));
        std::filesystem::remove_all(path_);
        std::filesystem::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const { return path_; }

    /// Writes `text` to `<dir>/<relative>`, creating intermediate directories.
    void write(const std::string& relative, const std::string& text) const {
        const std::filesystem::path target = path_ / relative;
        std::filesystem::create_directories(target.parent_path());
        write_text(target, text);
    }

private:
    std::filesystem::path path_;
};

}  // namespace pat::test
