// settings.pd (E13-T09, #374): round trip of the whole SettingsModel, unknown keys survive a rewrite, a
// damaged file gives the defaults (and is kept as .bad), a failed write leaves the old file alone, the saved
// scale reaches the host. Scratch directories only (TempDir); nothing reads real user paths.

#include "pat/ui/settings_file.hpp"

#include <doctest/doctest.h>

#include <filesystem>
#include <string>

#include "test_files.hpp"

using namespace pat;
using ui::KeyAction;
using ui::SettingsFile;
using ui::SettingsLoadStatus;
using ui::SettingsModel;

namespace {

SettingsModel changed_model() {
    SettingsModel m;
    m.width = 2560;
    m.height = 1440;
    m.window_mode = 2;
    m.map_quality = 2;
    m.shadows = 0;
    m.frame_limit = 120;
    m.vsync = false;
    m.ui_scale = 125;
    m.language = "en";
    m.text_size = 2;
    m.tooltips = 2;
    m.pause_on_events = false;
    m.reduce_motion = true;
    m.autosave_interval_min = 20;
    m.autosave_on_exit = false;
    m.master_volume = 10;
    m.music_volume = 20;
    m.ambient_volume = 30;
    m.interface_volume = 40;
    m.output_device = 3;
    m.mute_unfocused = false;
    ui::rebind_key(m, KeyAction::Pause, "p");
    ui::rebind_key(m, KeyAction::QuickSave, "f6");
    return m;
}

}  // namespace

TEST_CASE("settings file: a missing file gives the defaults") {
    test::TempDir dir("settings_missing");
    const auto load = ui::load_settings(dir.path() / "settings.pd");
    CHECK(load.status == SettingsLoadStatus::Missing);
    CHECK(load.file.model == SettingsModel{});
    CHECK(load.warnings.empty());
}

TEST_CASE("settings file: the whole model round-trips, the defaults too") {
    test::TempDir dir("settings_roundtrip");
    const auto path = dir.path() / "nested" / "settings.pd";  // the directory is created
    for (const SettingsModel& model : {SettingsModel{}, changed_model()}) {
        SettingsFile file;
        file.model = model;
        REQUIRE(ui::save_settings(path, file).has_value());
        const auto load = ui::load_settings(path);
        CHECK(load.status == SettingsLoadStatus::Loaded);
        CHECK(load.warnings.empty());
        CHECK(load.file.model == model);
        CHECK(load.file.unknown_entries.empty());
    }
    CHECK_FALSE(std::filesystem::exists(path.string() + ".tmp"));
}

TEST_CASE("settings file: unknown keys survive a rewrite") {
    test::TempDir dir("settings_unknown");
    const auto path = dir.path() / "settings.pd";
    dir.write("settings.pd",
              "format_version = 1\nsettings_version = 1\nvideo.width = 2560\nfuture.option = \"x\"\n"
              "future.block = {\n  a = 1\n  b = [ 2 3.5 yes \"t\\n\" ]\n  when \"lbl\" { d = 1703.10.01 r = "
              "scope:x }\n}\n"
              "video.height = 1440\n");
    auto load = ui::load_settings(path);
    REQUIRE(load.status == SettingsLoadStatus::Loaded);
    CHECK(load.file.model.width == 2560);
    CHECK(load.file.unknown_entries.find("future.option") != std::string::npos);

    load.file.model.height = 1200;  // the player changes something and applies
    REQUIRE(ui::save_settings(path, load.file).has_value());
    const auto again = ui::load_settings(path);
    CHECK(again.file.model.width == 2560);
    CHECK(again.file.model.height == 1200);
    CHECK(again.file.unknown_entries ==
          load.file.unknown_entries);  // the same canonical text, in the same order
    CHECK(again.file.unknown_entries.find("future.block") != std::string::npos);
}

TEST_CASE("settings file: a damaged file gives the defaults and is kept as .bad") {
    test::TempDir dir("settings_damaged");
    const auto path = dir.path() / "settings.pd";
    for (const char* text :
         {"video.width = = =\n", "settings_version = 1\n", "format_version = 99\n", "{{{{"}) {
        dir.write("settings.pd", text);
        const auto load = ui::load_settings(path);
        CHECK(load.status == SettingsLoadStatus::Recovered);
        CHECK(load.file.model == SettingsModel{});
        CHECK_FALSE(load.warnings.empty());
        CHECK_FALSE(std::filesystem::exists(path));
        CHECK(std::filesystem::exists(dir.path() / "settings.pd.bad"));
    }
    // the next start works again from the defaults and writes a good file
    SettingsFile file;
    REQUIRE(ui::save_settings(path, file).has_value());
    CHECK(ui::load_settings(path).status == SettingsLoadStatus::Loaded);
}

TEST_CASE("settings file: a bad value falls back to its default, the rest is read") {
    test::TempDir dir("settings_badvalue");
    dir.write("settings.pd",
              "format_version = 1\nvideo.width = 2560\nvideo.shadows = 99\ninterface.ui_scale = \"big\"\n"
              "audio.master = 7\nvideo.frame_limit = 45\nkey.pause = \"m\"\n");  // "m" is also the map mode
    const auto load = ui::load_settings(dir.path() / "settings.pd");
    CHECK(load.status == SettingsLoadStatus::Loaded);
    CHECK(load.file.model.width == 2560);
    CHECK(load.file.model.master_volume == 7);
    CHECK(load.file.model.shadows == SettingsModel{}.shadows);
    CHECK(load.file.model.ui_scale == SettingsModel{}.ui_scale);
    CHECK(load.file.model.frame_limit == SettingsModel{}.frame_limit);
    CHECK(load.file.model.keys == SettingsModel{}.keys);  // a key serving two actions: the defaults
    CHECK(load.warnings.size() == 4);
}

TEST_CASE("settings file: a failed write leaves the previous file intact") {
    test::TempDir dir("settings_partial");
    const auto path = dir.path() / "settings.pd";
    SettingsFile file;
    file.model = changed_model();
    REQUIRE(ui::save_settings(path, file).has_value());
    const std::string before = test::read_text(path);

    // The temporary file cannot be created (a directory sits on its name): nothing is replaced.
    std::filesystem::create_directory(path.string() + ".tmp");
    file.model.width = 800;
    CHECK_FALSE(ui::save_settings(path, file).has_value());
    CHECK(test::read_text(path) == before);
    CHECK(ui::load_settings(path).file.model == changed_model());
}

TEST_CASE("settings file: a stale temporary file does not disturb loading or saving") {
    test::TempDir dir("settings_stale_tmp");
    const auto path = dir.path() / "settings.pd";
    SettingsFile file;
    file.model = changed_model();
    REQUIRE(ui::save_settings(path, file).has_value());
    dir.write("settings.pd.tmp", "format_version = 1\nvideo.wid");  // a torn write of an earlier run
    CHECK(ui::load_settings(path).file.model == changed_model());
    file.model.width = 3000;
    REQUIRE(ui::save_settings(path, file).has_value());
    CHECK(ui::load_settings(path).file.model.width == 3000);
}

TEST_CASE("settings file: a newer settings_version is read as far as understood and left alone") {
    test::TempDir dir("settings_newer");
    dir.write("settings.pd", "format_version = 1\nsettings_version = 7\nvideo.width = 2560\n");
    const auto load = ui::load_settings(dir.path() / "settings.pd");
    CHECK(load.status == SettingsLoadStatus::Loaded);
    CHECK(load.file.model.width == 2560);
    CHECK(load.warnings.size() == 1);
    CHECK(std::filesystem::exists(dir.path() / "settings.pd"));
}

TEST_CASE("settings file: the saved interface scale reaches the host at start") {
    test::TempDir dir("settings_scale");
    SettingsFile file;
    file.model.ui_scale = 125;
    REQUIRE(ui::save_settings(dir.path() / "settings.pd", file).has_value());
    const auto load = ui::load_settings(dir.path() / "settings.pd");
    CHECK(ui::user_ui_scale_of(load.file.model) == doctest::Approx(1.25f));
    SettingsModel wild;
    wild.ui_scale = 400;  // outside the file's range: clamped for the host
    CHECK(ui::user_ui_scale_of(wild) == doctest::Approx(1.5f));
}
