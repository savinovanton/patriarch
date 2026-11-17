// The save list (E13-T04, #369): scan of a directory by summaries, classes, order, names, rename,
// delete, leftovers of an interrupted write, speed. Everything happens in temporary directories.

#include "pat/core/path_utf8.hpp"
#include "pat/game/save_catalog.hpp"
#include "pat/sim/date.hpp"
#include "pat/sim/save.hpp"
#include "pat/sim/world.hpp"

#include <doctest/doctest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "test_files.hpp"

using namespace pat;
using game::SaveClass;
using game::SaveEntry;

namespace {

std::vector<std::uint8_t> make_save_bytes(std::int64_t saved_at,
                                          std::uint64_t content_hash = 0,
                                          const std::string& name = {},
                                          std::uint32_t schema = 0) {
    sim::World world(sim::Date::from_ymd(1703, 1, 1), 5);
    sim::SaveSummary summary = sim::summary_from_world(world);
    summary.saved_at = saved_at;
    summary.content_hash = content_hash;
    summary.name = name;
    if (schema != 0) {
        summary.state_schema = schema;
    }
    return sim::serialize_save(world, {}, summary);
}

void put(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes) {
    REQUIRE(sim::write_file_atomic(path, bytes).has_value());
}

const SaveEntry* find(const std::vector<SaveEntry>& list, const std::string& stem) {
    for (const auto& e : list) {
        if (e.file_stem == stem) {
            return &e;
        }
    }
    return nullptr;
}

}  // namespace

TEST_CASE("scan: newest first by the summary time, ties by name") {
    pat::test::TempDir dir("catalog_order");
    put(dir.path() / "old.psv", make_save_bytes(1000));
    put(dir.path() / "new.psv", make_save_bytes(3000));
    put(dir.path() / "mid.psv", make_save_bytes(2000));
    put(dir.path() / "a_same.psv", make_save_bytes(2000));
    pat::test::write_text(dir.path() / "readme.txt", "not a save");
    std::filesystem::create_directories(dir.path() / "sub.psv");  // a directory with the extension

    const auto list = game::scan_saves(dir.path());
    REQUIRE(list.size() == 4);
    CHECK(list[0].file_stem == "new");
    CHECK(list[1].file_stem == "a_same");
    CHECK(list[2].file_stem == "mid");
    CHECK(list[3].file_stem == "old");
    CHECK(list[0].cls == SaveClass::Compatible);
    REQUIRE(list[0].summary.has_value());
    CHECK(list[0].summary->saved_at == 3000);
    CHECK(list[0].file_size > 0);
}

TEST_CASE("scan: without a summary time the file time orders the entry") {
    pat::test::TempDir dir("catalog_mtime");
    put(dir.path() / "first.psv", make_save_bytes(0));
    put(dir.path() / "second.psv", make_save_bytes(0));
    pat::test::touch_newer(dir.path() / "second.psv");
    const auto list = game::scan_saves(dir.path());
    REQUIRE(list.size() == 2);
    CHECK(list[0].file_stem == "second");
    CHECK(list[0].sort_time == list[0].file_mtime);
}

TEST_CASE("scan: classes") {
    pat::test::TempDir dir("catalog_classes");
    put(dir.path() / "good.psv", make_save_bytes(10));
    put(dir.path() / "nocontent.psv", make_save_bytes(11, 0));
    put(dir.path() / "mine.psv", make_save_bytes(12, 77));
    put(dir.path() / "foreign.psv", make_save_bytes(13, 99));
    put(dir.path() / "newschema.psv", make_save_bytes(14, 0, {}, sim::World::kStateSchema + 1));

    auto newer_version = make_save_bytes(15);
    newer_version[4] = 3;  // container version 3
    put(dir.path() / "newversion.psv", newer_version);

    auto flipped = make_save_bytes(16);
    flipped[50] ^= 0x55;  // inside the summary
    put(dir.path() / "flipped.psv", flipped);
    put(dir.path() / "empty.psv", {});
    pat::test::write_text(dir.path() / "garbage.psv", "this is not a save file at all, just text");

    game::ScanOptions options;
    options.expected_content_hash = 77;
    const auto list = game::scan_saves(dir.path(), options);
    REQUIRE(list.size() == 9);
    CHECK(find(list, "good")->cls == SaveClass::Compatible);
    CHECK(find(list, "nocontent")->cls == SaveClass::Compatible);
    CHECK(find(list, "mine")->cls == SaveClass::Compatible);
    CHECK(find(list, "foreign")->cls == SaveClass::ForeignContent);
    CHECK(find(list, "newschema")->cls == SaveClass::Newer);
    CHECK(find(list, "newversion")->cls == SaveClass::Newer);
    CHECK(find(list, "flipped")->cls == SaveClass::Corrupt);
    CHECK(find(list, "flipped")->error_code != 0);
    CHECK_FALSE(find(list, "flipped")->summary.has_value());
    CHECK(find(list, "empty")->cls == SaveClass::Corrupt);
    CHECK(find(list, "garbage")->cls == SaveClass::Corrupt);

    // Without a content hash to compare to, a foreign set is not told apart.
    CHECK(find(game::scan_saves(dir.path()), "foreign")->cls == SaveClass::Compatible);
}

TEST_CASE("scan: a legacy version 1 file is compatible, shown by name and file time") {
    const auto legacy = std::filesystem::path(PATRIARCH_TEST_DATA_DIR) / "saves" / "e1_demo_v1.psv";
    pat::test::TempDir dir("catalog_legacy");
    std::filesystem::copy_file(legacy, dir.path() / "legacy.psv");
    const auto list = game::scan_saves(dir.path());
    REQUIRE(list.size() == 1);
    CHECK(list[0].cls == SaveClass::Compatible);
    CHECK(list[0].format_version == 1);
}

TEST_CASE("scan: a missing directory is an empty list") {
    CHECK(game::scan_saves(pat::test::temp_path("patriarch_catalog_no_such_dir")).empty());
}

TEST_CASE("scan: a Cyrillic name with spaces") {
    pat::test::TempDir dir("catalog_cyr");
    const auto path = dir.path() / core::path_from_utf8("Быстрое сохранение — Рюриковичи.psv");
    put(path, make_save_bytes(5));
    const auto list = game::scan_saves(dir.path());
    REQUIRE(list.size() == 1);
    CHECK(list[0].file_stem == "Быстрое сохранение — Рюриковичи");
}

TEST_CASE("names: sanitizing") {
    CHECK(game::sanitize_save_name("Мой сейв") == "Мой сейв");
    CHECK(game::sanitize_save_name("a<b>c:d\"e/f\\g|h?i*j") == "a_b_c_d_e_f_g_h_i_j");
    CHECK(game::sanitize_save_name("  name. . ") == "name");
    CHECK(game::sanitize_save_name("") == "save");
    CHECK(game::sanitize_save_name("...") == "save");
    CHECK(game::sanitize_save_name("CON") == "_CON");
    CHECK(game::sanitize_save_name("nul.txt") == "_nul.txt");
    CHECK(game::sanitize_save_name("Com3") == "_Com3");
    CHECK(game::sanitize_save_name("COM0") == "COM0");
    CHECK(game::sanitize_save_name("tab\tname") == "tab_name");
    CHECK(game::sanitize_save_name("bad\xFF\xFEname") == "bad__name");
}

TEST_CASE("names: unique, shortened to the path limit") {
    pat::test::TempDir dir("catalog_unique");
    auto first = game::unique_save_path(dir.path(), "Партия 1");
    REQUIRE(first.has_value());
    CHECK(first.value().filename() == core::path_from_utf8("Партия 1.psv"));
    put(first.value(), make_save_bytes(1));

    auto second = game::unique_save_path(dir.path(), "Партия 1");
    REQUIRE(second.has_value());
    CHECK(second.value().filename() == core::path_from_utf8("Партия 1 (2).psv"));
    put(second.value(), make_save_bytes(1));
    auto third = game::unique_save_path(dir.path(), "Партия 1");
    CHECK(third.value().filename() == core::path_from_utf8("Партия 1 (3).psv"));

    std::string long_name;
    for (int i = 0; i < 200; ++i) {
        long_name += "Я";  // two bytes per character
    }
    auto shortened = game::unique_save_path(dir.path(), long_name);
    REQUIRE(shortened.has_value());
    CHECK((shortened.value().native() +
           std::filesystem::path::string_type(std::filesystem::path(".tmp").native()))
              .size() <= game::kMaxSavePathLength);
    CHECK(core::path_to_utf8(shortened.value().stem()).size() % 2 == 0);  // no half a character left
    put(shortened.value(), make_save_bytes(1));  // the shortened path is really usable
    auto again = game::unique_save_path(dir.path(), long_name);
    REQUIRE(again.has_value());
    CHECK(again.value() != shortened.value());
    CHECK(again.value().native().size() + 4 <= game::kMaxSavePathLength);

    // A directory that leaves no room.
    std::filesystem::path deep = dir.path();
    while (deep.native().size() < game::kMaxSavePathLength) {
        deep /= "dddddddddd";
    }
    CHECK_FALSE(game::unique_save_path(deep, "x").has_value());
}

TEST_CASE("rename and delete") {
    pat::test::TempDir dir("catalog_rename");
    put(dir.path() / "a.psv", make_save_bytes(1));
    put(dir.path() / "b.psv", make_save_bytes(2));

    auto renamed = game::rename_save(dir.path() / "a.psv", "Новое: имя?");
    REQUIRE(renamed.has_value());
    CHECK(renamed.value().filename() == core::path_from_utf8("Новое_ имя_.psv"));
    CHECK_FALSE(std::filesystem::exists(dir.path() / "a.psv"));
    CHECK(std::filesystem::exists(renamed.value()));
    CHECK(sim::read_save_summary(renamed.value()).has_value());

    // A clash gets a number; the file's own name is not a clash.
    auto clash = game::rename_save(dir.path() / "b.psv", "Новое_ имя_");
    REQUIRE(clash.has_value());
    CHECK(clash.value().filename() == core::path_from_utf8("Новое_ имя_ (2).psv"));
    auto same = game::rename_save(clash.value(), "Новое_ имя_ (2)");
    REQUIRE(same.has_value());
    CHECK(same.value() == clash.value());

    CHECK(game::rename_save(dir.path() / "missing.psv", "x").error().code == 62);
    pat::test::write_text(dir.path() / "note.txt", "x");
    CHECK(game::rename_save(dir.path() / "note.txt", "x").error().code == 62);

    CHECK(game::delete_save(renamed.value()).has_value());
    CHECK_FALSE(std::filesystem::exists(renamed.value()));
    CHECK(game::delete_save(renamed.value()).error().code == 62);
    CHECK(game::delete_save(dir.path() / "note.txt").error().code == 62);
    CHECK(std::filesystem::exists(dir.path() / "note.txt"));
    CHECK(game::scan_saves(dir.path()).size() == 1);
}

TEST_CASE("orphan temporary files are removed, saves are not") {
    pat::test::TempDir dir("catalog_tmp");
    put(dir.path() / "keep.psv", make_save_bytes(1));
    pat::test::write_text(dir.path() / "half.psv.tmp", "partial");
    pat::test::write_text(dir.path() / "other.psv.tmp", "partial");
    pat::test::write_text(dir.path() / "notes.tmp", "not ours");
    CHECK(game::remove_orphan_temp_files(dir.path()) == 2);
    CHECK_FALSE(std::filesystem::exists(dir.path() / "half.psv.tmp"));
    CHECK(std::filesystem::exists(dir.path() / "keep.psv"));
    CHECK(std::filesystem::exists(dir.path() / "notes.tmp"));
    CHECK(game::remove_orphan_temp_files(dir.path()) == 0);
    CHECK(game::remove_orphan_temp_files(pat::test::temp_path("patriarch_catalog_no_such_dir")) == 0);
    CHECK(game::scan_saves(dir.path()).size() == 1);  // a leftover .tmp never shows in the list
}

TEST_CASE("scan: 1000 saves within 100 ms") {
    pat::test::TempDir dir("catalog_perf");
    const auto bytes = make_save_bytes(100, 7, "perf");
    for (int i = 0; i < 1000; ++i) {
        put(dir.path() / ("save_" + std::to_string(i) + ".psv"), bytes);
    }
    // One warm-up scan (the files were just written, the file system cache is hot either way); then the
    // best of a few runs, so a single scheduling hiccup of a busy CI machine does not fail the test.
    CHECK(game::scan_saves(dir.path()).size() == 1000);
    std::chrono::milliseconds best{1000000};
    for (int run = 0; run < 3; ++run) {
        const auto start = std::chrono::steady_clock::now();
        const auto list = game::scan_saves(dir.path());
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
        CHECK(list.size() == 1000);
        best = std::min(best, elapsed);
    }
    MESSAGE("scan of 1000 saves: " << best.count() << " ms");
    // The criterion (100 ms) is for an optimized build; Debug and ASan builds are instrumented and slower.
#if defined(NDEBUG) && !defined(__SANITIZE_ADDRESS__)
    constexpr std::int64_t kLimitMs = 100;
#else
    constexpr std::int64_t kLimitMs = 400;
#endif
    CHECK(best.count() <= kLimitMs);
}
