#pragma once

#include "pat/data/pd_parser.hpp"

#include <doctest/doctest.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>

// Reads the exact geometry of the mockup boards (data/ui/layout/*.pd, docs/dev/ui-design.md section 12) so
// that the window tests compare the laid-out documents with the numbers of the design, not with copies of
// them.

namespace pat::test {

struct Layer {
    bool found = false;
    double x = 0;
    double y = 0;
    double w = 0;
    double h = 0;
};

inline double layout_number(const pat::data::Value& v) {
    if (v.kind == pat::data::ValueKind::Decimal) {
        return static_cast<double>(v.integer) / std::pow(10.0, v.scale);
    }
    return static_cast<double>(v.integer);
}

/// The first layer called `name` in the layout file `file` (the n key; text layers are named by their own
/// text only when they have no n, so pass `by_text` for those). `board` limits the search to the board of
/// that name (files with many boards, like menu_boards.pd, repeat layer names); `occurrence` skips that many
/// earlier layers of the same name (0: the first one), for names that a board repeats.
inline Layer find_layer(const char* file,
                        const std::string& name,
                        bool by_text = false,
                        const std::string& board_name = std::string(),
                        int occurrence = 0) {
    std::ifstream in(std::filesystem::path(PATRIARCH_DATA_DIR) / "ui" / "layout" / file, std::ios::binary);
    REQUIRE_MESSAGE(in.good(), "cannot open layout " << file);
    std::ostringstream buffer;
    buffer << in.rdbuf();
    const pat::data::ParseResult parsed = pat::data::parse_pd(buffer.str());
    REQUIRE(parsed.ok());
    for (const pat::data::Entry& board : parsed.file.entries) {
        if (!board_name.empty() && board.label.value_or(std::string()) != board_name) {
            continue;
        }
        for (const pat::data::Entry& field : board.value.entries) {
            if (field.key != "el") {
                continue;
            }
            for (const pat::data::Value& item : field.value.items) {
                Layer out;
                bool named = false;
                for (const pat::data::Entry& e : item.entries) {
                    if (e.key == (by_text ? "txt" : "n") && e.value.text == name) {
                        named = true;
                    } else if (e.key == "x") {
                        out.x = layout_number(e.value);
                    } else if (e.key == "y") {
                        out.y = layout_number(e.value);
                    } else if (e.key == "w") {
                        out.w = layout_number(e.value);
                    } else if (e.key == "h") {
                        out.h = layout_number(e.value);
                    }
                }
                if (named) {
                    if (occurrence-- > 0) {
                        continue;
                    }
                    out.found = true;
                    return out;
                }
            }
        }
    }
    return {};
}

}  // namespace pat::test
