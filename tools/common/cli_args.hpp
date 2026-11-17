#pragma once

// Shared command-line argument parsing for the tools (header-only, no project dependencies).

#include <charconv>
#include <cstdint>
#include <optional>
#include <string_view>
#include <system_error>

namespace pat::tools {

/// Strict unsigned integer: decimal digits only, the whole text must be consumed, the value must fit
/// in 64 bits. Rejects the empty string, signs, spaces and trailing garbage ("12abc").
[[nodiscard]] inline std::optional<std::uint64_t> parse_u64(std::string_view text) noexcept {
    std::uint64_t value = 0;
    const char* const first = text.data();
    const char* const last = first + text.size();
    const auto [end, error] = std::from_chars(first, last, value, 10);
    if (error != std::errc{} || end != last || text.empty()) {
        return std::nullopt;
    }
    return value;
}

}  // namespace pat::tools
