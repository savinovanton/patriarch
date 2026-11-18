#pragma once

#include "pat/core/file_io.hpp"
#include "pat/ui/menu.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace pat::ui {

/// `settings.pd` (E13-T09, docs/dev/save-format.md section 9.7): the whole `SettingsModel` as a `.pd` file
/// with flat dotted keys (`video.width`, `interface.ui_scale`, `autosave.interval_min`, `key.pause`, ...).
/// Versioned by its own key `settings_version` (the `.pd` header `format_version` stays 1).
inline constexpr int kSettingsVersion = 1;

/// The model plus the top-level entries this build does not know (a newer build, a hand edit): they are
/// kept as canonical `.pd` text and written back unchanged, so saving never loses them.
struct SettingsFile {
    SettingsModel model;
    std::string unknown_entries;
};

enum class SettingsLoadStatus {
    Missing,    ///< no file: the defaults
    Loaded,     ///< read (values that were out of range fell back to the defaults, see `warnings`)
    Recovered,  ///< the file was damaged: the defaults, the file was moved to `settings.pd.bad`
};

struct SettingsLoad {
    SettingsFile file;
    SettingsLoadStatus status = SettingsLoadStatus::Missing;
    std::vector<std::string> warnings;  ///< English, one problem each; also written to the log
};

/// The text of the file for `file` (known keys in a fixed order, then the unknown entries).
[[nodiscard]] std::string format_settings(const SettingsFile& file);

/// Reads `path`. Never fails and never throws: a missing file gives the defaults; a damaged one (it does not
/// parse, or it has a wrong header) gives the defaults and is renamed to `<path>.bad`; a bad single value
/// falls back to its default with a warning. A file written by a newer build (`settings_version` above
/// `kSettingsVersion`) is read as far as it is understood and left alone.
[[nodiscard]] SettingsLoad load_settings(const std::filesystem::path& path);

/// Writes `file` atomically (`core::write_file_atomic`, the temporary file is parsed back before it
/// replaces the old one); the directory is created when missing. The old file survives any failure.
[[nodiscard]] core::Result<void, core::FileIoError> save_settings(const std::filesystem::path& path,
                                                                  const SettingsFile& file);

/// The multiplier of the OS display scale that the saved `ui_scale` (percent) stands for (clamped to the
/// limits of `engine::design_ui_scale`); the host applies it at start, before the window opens.
[[nodiscard]] float user_ui_scale_of(const SettingsModel& model);

/// The key name of `action` in the file ("pause", "camera", ...).
[[nodiscard]] const char* key_action_name(KeyAction action);

}  // namespace pat::ui
