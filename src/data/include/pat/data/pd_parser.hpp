#pragma once

#include "pat/data/pd_tree.hpp"

#include <cstddef>
#include <string_view>
#include <vector>

namespace pat::data {

/// Limits from the specification (docs/dev/data-format.md).
inline constexpr std::size_t kMaxFileBytes = 16u * 1024u * 1024u;
inline constexpr std::size_t kMaxStringBytes = 1024u * 1024u;
inline constexpr std::size_t kMaxIdentBytes = 256;
inline constexpr int kMaxNesting = 64;
inline constexpr std::size_t kMaxErrors = 100;
inline constexpr std::int64_t kCurrentFormatVersion = 1;

struct ParseResult {
    ParsedFile file;
    std::vector<Diagnostic> errors;  // sorted by position; empty on success

    [[nodiscard]] bool ok() const noexcept { return errors.empty(); }
};

/// Parses one .pd file. Never throws and never aborts on any input: all problems are returned as
/// diagnostics (up to kMaxErrors, then PD022). When errors is not empty the tree is partial.
[[nodiscard]] ParseResult parse_pd(std::string_view source);

}  // namespace pat::data
