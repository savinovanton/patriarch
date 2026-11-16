#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace pat::core {

/// Filesystem path -> UTF-8 string with '/' separators (portable text form, e.g. content paths).
[[nodiscard]] inline std::string path_to_utf8(const std::filesystem::path& p) {
    const std::u8string u = p.generic_u8string();
    return std::string(reinterpret_cast<const char*>(u.data()), u.size());
}

/// UTF-8 string -> filesystem path; correct for non-ASCII names on Windows (no ANSI code page).
[[nodiscard]] inline std::filesystem::path path_from_utf8(std::string_view s) {
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}

}  // namespace pat::core
