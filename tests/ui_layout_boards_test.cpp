// The layout files of the boards 14-81 of the Penpot file (UI-D11, tools/penpot_export.py): each one parses
// without errors, holds the expected boards, and every board is well formed (root at the origin with the
// board's own size, every element with a type and a rectangle). The geometry itself is the design; the
// window tests of the later UI tasks compare their documents with it.

#include "pat/data/pd_parser.hpp"

#include <doctest/doctest.h>

#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "ui_layout_fixture.hpp"

using namespace pat::data;

namespace {

struct LayoutFile {
    const char* name;
    std::vector<int> boards;  // the board numbers of the Penpot file, in file order
};

const std::vector<LayoutFile>& layout_files() {
    static const std::vector<LayoutFile> files = {
        {"save_menu.pd", {14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 78, 72}},
        {"game_summary.pd", {24, 25, 26, 27, 73, 74}},
        {"empire_state.pd", {28, 29}},
        {"court.pd", {30, 31, 32, 75, 76}},
        {"throne.pd", {33, 34, 35, 79}},
        {"plots_coup.pd", {36, 37, 38, 39, 40, 41, 42, 43, 44}},
        {"goals.pd", {45, 46, 47, 48}},
        {"dynasty_start.pd", {49, 50, 51, 52, 53, 54, 55}},
        {"servants.pd", {56, 57, 58, 59, 70, 77}},
        {"house_council.pd", {60, 61, 62, 63, 64, 71, 80}},
        {"chronicle_journal.pd", {65, 66, 67, 68, 69}},
        {"ui_guide.pd", {81}},
    };
    return files;
}

std::string read_layout(const char* file) {
    std::ifstream in(std::filesystem::path(PATRIARCH_DATA_DIR) / "ui" / "layout" / file, std::ios::binary);
    REQUIRE_MESSAGE(in.good(), "cannot open layout " << file);
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

const Entry* find_key(const std::vector<Entry>& entries, const std::string& key) {
    for (const Entry& e : entries) {
        if (e.key == key)
            return &e;
    }
    return nullptr;
}

/// The number before " / " in a board name ("15 / Сохранить игру"), or -1.
int board_number(const std::string& name) {
    std::size_t i = 0;
    int n = 0;
    while (i < name.size() && name[i] >= '0' && name[i] <= '9') {
        n = n * 10 + (name[i] - '0');
        ++i;
    }
    if (i == 0 || name.compare(i, 3, " / ") != 0)
        return -1;
    return n;
}

bool known_type(const std::string& t) {
    static const std::set<std::string> types = {
        "board", "rectangle", "ellipse", "text", "path", "group", "image"};
    return types.count(t) != 0;
}

}  // namespace

TEST_CASE("ui layout boards 14-81: every file parses and lists its boards") {
    std::set<int> seen;
    std::size_t boards = 0;
    for (const LayoutFile& file : layout_files()) {
        INFO(file.name);
        const ParseResult parsed = parse_pd(read_layout(file.name));
        REQUIRE(parsed.ok());
        CHECK(parsed.file.format_version == kCurrentFormatVersion);

        std::vector<int> numbers;
        for (const Entry& board : parsed.file.entries) {
            if (board.key != "board")
                continue;
            const std::string name = board.label.value_or(std::string());
            INFO(name);
            const int number = board_number(name);
            REQUIRE(number >= 14);
            numbers.push_back(number);
            CHECK(seen.insert(number).second);  // a board lives in exactly one file
            ++boards;

            // Every board of this set is a full 1920x1080 screen or dialog frame.
            const Entry* w = find_key(board.value.entries, "w");
            const Entry* h = find_key(board.value.entries, "h");
            REQUIRE(w != nullptr);
            REQUIRE(h != nullptr);
            CHECK(pat::test::layout_number(w->value) == doctest::Approx(1920.0));
            CHECK(pat::test::layout_number(h->value) == doctest::Approx(1080.0));

            const Entry* el = find_key(board.value.entries, "el");
            REQUIRE(el != nullptr);
            REQUIRE(el->value.kind == ValueKind::List);
            REQUIRE_FALSE(el->value.items.empty());

            std::size_t index = 0;
            for (const Value& item : el->value.items) {
                const Entry* t = find_key(item.entries, "t");
                REQUIRE(t != nullptr);
                CHECK(known_type(t->value.text));
                for (const char* key : {"x", "y", "w", "h"}) {
                    const Entry* e = find_key(item.entries, key);
                    REQUIRE(e != nullptr);
                    CHECK(std::isfinite(pat::test::layout_number(e->value)));
                }
                if (t->value.text == "text") {
                    const Entry* txt = find_key(item.entries, "txt");
                    REQUIRE(txt != nullptr);
                    CHECK(txt->value.kind == ValueKind::String);
                }
                if (index++ == 0) {
                    // The root: the board itself, at the origin, with the board's own size.
                    CHECK(t->value.text == "board");
                    CHECK(pat::test::layout_number(find_key(item.entries, "x")->value) ==
                          doctest::Approx(0.0));
                    CHECK(pat::test::layout_number(find_key(item.entries, "y")->value) ==
                          doctest::Approx(0.0));
                    CHECK(pat::test::layout_number(find_key(item.entries, "w")->value) ==
                          doctest::Approx(1920.0));
                    CHECK(pat::test::layout_number(find_key(item.entries, "h")->value) ==
                          doctest::Approx(1080.0));
                }
            }
            CHECK(index > 1);
        }
        CHECK(numbers == file.boards);
    }
    CHECK(boards == 68);  // 14..81
    for (int n = 14; n <= 81; ++n) {
        INFO("board " << n);
        CHECK(seen.count(n) == 1);
    }
}

TEST_CASE("ui layout boards 14-81: files carry the snapshot date and no secrets") {
    for (const LayoutFile& file : layout_files()) {
        INFO(file.name);
        const std::string text = read_layout(file.name);
        CHECK(text.find("snapshot 2026-11-13") != std::string::npos);
        CHECK(text.find("Generated by tools/penpot_export.py") != std::string::npos);
        CHECK(text.find("userToken") == std::string::npos);
        CHECK(text.find("penpot.app") == std::string::npos);
    }
}

TEST_CASE("ui layout boards 14-81: find_layer reads the new files") {
    const pat::test::Layer guide = pat::test::find_layer("ui_guide.pd", "Divider");
    REQUIRE(guide.found);
    CHECK(guide.x == doctest::Approx(40.0));
    CHECK(guide.w == doctest::Approx(1840.0));
    const pat::test::Layer text = pat::test::find_layer(
        "ui_guide.pd", "Patriarch · Художественный интерфейс", true, "81 / Путеводитель по интерфейсам");
    REQUIRE(text.found);
    CHECK(text.y == doctest::Approx(40.0));
}
