// Design data of the UI (issue #252): data/ui/theme.pd (tokens) and data/ui/layout/*.pd (exact
// geometry of the Penpot boards). They are not game content: the content loader must skip them,
// and they must stay valid .pd so the theme loader can read them later.

#include "pat/data/content_loader.hpp"
#include "pat/data/content_schemas.hpp"
#include "pat/data/content_source.hpp"
#include "pat/data/pd_parser.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace pat::data;

namespace {

std::string read_text(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    REQUIRE_MESSAGE(in.good(), "cannot open " << path.string());
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

std::filesystem::path ui_dir() {
    return std::filesystem::path(PATRIARCH_DATA_DIR) / "ui";
}

const Entry* find_entry(const std::vector<Entry>& entries, const std::string& key) {
    for (const Entry& e : entries)
        if (e.key == key)
            return &e;
    return nullptr;
}

bool is_hex_colour(const std::string& s) {
    if (s.size() != 7 || s[0] != '#')
        return false;
    return std::all_of(s.begin() + 1, s.end(), [](unsigned char c) { return std::isxdigit(c) != 0; });
}

}  // namespace

TEST_CASE("ui data: every file below data/ui parses") {
    std::size_t files = 0;
    for (const auto& item : std::filesystem::recursive_directory_iterator(ui_dir())) {
        if (!item.is_regular_file() || item.path().extension() != ".pd")
            continue;
        ++files;
        const ParseResult r = parse_pd(read_text(item.path()));
        INFO(item.path().string());
        CHECK(r.ok());
        CHECK(r.file.format_version == kCurrentFormatVersion);
    }
    CHECK(files >= 2);  // theme.pd and at least one layout file
}

TEST_CASE("ui data: the theme carries the Penpot tokens") {
    const ParseResult r = parse_pd(read_text(ui_dir() / "theme.pd"));
    REQUIRE(r.ok());
    const Entry* theme = find_entry(r.file.entries, "theme");
    REQUIRE(theme != nullptr);
    CHECK(theme->label.value_or("") == "patriarch-concept-01");

    const auto& body = theme->value.entries;
    const Entry* colors = find_entry(body, "color");
    const Entry* spacing = find_entry(body, "spacing");
    const Entry* sizes = find_entry(body, "font_size");
    REQUIRE(colors != nullptr);
    REQUIRE(spacing != nullptr);
    REQUIRE(sizes != nullptr);
    CHECK(colors->value.entries.size() == 17);
    CHECK(spacing->value.entries.size() == 6);
    CHECK(sizes->value.entries.size() == 7);
    for (const Entry& c : colors->value.entries) {
        INFO(c.key);
        CHECK(c.value.kind == ValueKind::String);
        CHECK(is_hex_colour(c.value.text));
    }
    const Entry* ink = find_entry(colors->value.entries, "ink");
    REQUIRE(ink != nullptr);
    CHECK(ink->value.text == "#1B2523");
}

TEST_CASE("ui data: layout boards list their elements") {
    std::size_t boards = 0;
    for (const auto& item : std::filesystem::directory_iterator(ui_dir() / "layout")) {
        if (item.path().extension() != ".pd")
            continue;
        const ParseResult r = parse_pd(read_text(item.path()));
        REQUIRE(r.ok());
        for (const Entry& e : r.file.entries) {
            if (e.key != "board")
                continue;
            ++boards;
            INFO(item.path().string());
            const Entry* el = find_entry(e.value.entries, "el");
            REQUIRE(el != nullptr);
            CHECK(el->value.kind == ValueKind::List);
            CHECK_FALSE(el->value.items.empty());
        }
    }
    CHECK(boards >= 30);  // 36 Penpot boards at the 2026-11-11 snapshot
}

TEST_CASE("ui data: the content loader skips data/ui") {
    MemorySource root("base");
    root.add("ui/theme.pd", "format_version = 1\ntheme \"x\" { color { ink = \"#000000\" } }\n");
    root.add("ui/layout/board.pd", "format_version = 1\nboard \"y\" { w = 1 }\n");
    const LoadResult r = load_content(root, content_schemas());
    CHECK(r.ok());
    CHECK(r.registry.object_count() == 0);
}
