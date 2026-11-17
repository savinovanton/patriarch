#pragma once

// Shared fixture of the court content tests (E10-T04): the shipped files a court load needs, held in
// memory so a test can edit one of them. Court objects refer to skills, traits and province kinds, so
// those definition files travel with them.

#include "pat/data/content_source.hpp"
#include "pat/data/pd_tree.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pat::court_fixture {

inline constexpr std::string_view kOffices = "court/offices.pd";
inline constexpr std::string_view kBalance = "balance/court.pd";

inline bool needed(std::string_view path, bool with_court) {
    const auto has = [&](std::string_view prefix) { return path.starts_with(prefix); };
    if (has("traits/") || has("skills/") || has("economy/")) {
        return true;
    }
    return with_court && (has("court/") || path == kBalance);
}

inline std::string normalized(std::string text) {
    text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
    return text;
}

/// Shipped definition files plus (optionally) the shipped court files; `edit` may replace one file.
inline data::MemorySource shipped_source(bool with_court = true) {
    const data::DirectorySource directory(PATRIARCH_DATA_DIR);
    const auto files = directory.list_files();
    REQUIRE(files.has_value());
    data::MemorySource source("court fixture");
    for (const std::string& path : files.value()) {
        if (!needed(path, with_court)) {
            continue;
        }
        auto body = directory.read_file(path);
        REQUIRE(body.has_value());
        source.add(path, normalized(std::move(body).value()));
    }
    return source;
}

inline std::string shipped_text(std::string_view path) {
    const data::DirectorySource directory(PATRIARCH_DATA_DIR);
    auto body = directory.read_file(path);
    REQUIRE(body.has_value());
    return normalized(std::move(body).value());
}

/// Replaces `before` by `after` exactly once; both must be present.
inline void replace_once(std::string& text, std::string_view before, std::string_view after) {
    const auto offset = text.find(before);
    REQUIRE(offset != std::string::npos);
    REQUIRE(text.find(before, offset + before.size()) == std::string::npos);
    text.replace(offset, before.size(), after);
}

/// One-based line and column of the first `needle` at or after the first `anchor` in `text`.
inline data::SourcePos
position_after(const std::string& text, std::string_view anchor, std::string_view needle) {
    const auto start = text.find(anchor);
    REQUIRE(start != std::string::npos);
    const auto offset = text.find(needle, start);
    REQUIRE(offset != std::string::npos);
    const auto previous_line = text.rfind('\n', offset);
    return {static_cast<std::uint32_t>(
                std::count(text.begin(), text.begin() + static_cast<std::ptrdiff_t>(offset), '\n') + 1),
            static_cast<std::uint32_t>(offset - (previous_line == std::string::npos ? 0 : previous_line + 1) +
                                       1)};
}

}  // namespace pat::court_fixture
