#include "pat/ui/settings_file.hpp"

#include "pat/core/log.hpp"
#include "pat/data/pd_parser.hpp"
#include "pat/engine/dpi.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <set>
#include <span>
#include <string_view>
#include <system_error>

namespace pat::ui {

namespace {

constexpr std::array<const char*, kKeyActionCount> kKeyNames = {
    "pause", "camera", "zoom", "select", "back", "map_mode", "quick_save", "quick_load"};
constexpr std::size_t kMaxKeyIdBytes = 32;

std::string quote_text(std::string_view text) {
    std::string out = "\"";
    for (const char c : text) {
        const auto byte = static_cast<unsigned char>(c);
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (c == '\n') {
            out += "\\n";
        } else if (c == '\t') {
            out += "\\t";
        } else if (c == '\r') {
            out += "\\r";
        } else if (byte < 0x20) {
            char buf[16];
            std::snprintf(buf, sizeof buf, "\\u{%X}", static_cast<unsigned>(byte));
            out += buf;
        } else {
            out += c;
        }
    }
    out += '"';
    return out;
}

void line(std::string& out, std::string_view key, int value) {
    out += key;
    out += " = ";
    out += std::to_string(value);
    out += '\n';
}

void line(std::string& out, std::string_view key, bool value) {
    out += key;
    out += value ? " = yes\n" : " = no\n";
}

void line(std::string& out, std::string_view key, std::string_view value) {
    out += key;
    out += " = ";
    out += quote_text(value);
    out += '\n';
}

// Writes an entry back as .pd source (the unknown keys of a newer build or a hand edit). `data::dump_tree` is
// a debug form of the tree, not source, so the settings file has its own small writer.
void write_value(std::string& out, const data::Value& v, int indent);

void write_pad(std::string& out, int indent) {
    out.append(static_cast<std::size_t>(indent) * 2, ' ');
}

void write_entry(std::string& out, const data::Entry& e, int indent) {
    write_pad(out, indent);
    out += e.key;
    if (e.label) {
        out += ' ';
        out += quote_text(*e.label);
        out += ' ';  // a labelled entry is `key "label" { ... }`, with no operator
    } else {
        out += ' ';
        out += data::op_text(e.op);
        out += ' ';
    }
    write_value(out, e.value, indent);
}

void write_value(std::string& out, const data::Value& v, int indent) {
    using data::ValueKind;
    switch (v.kind) {
        case ValueKind::Int:
            out += std::to_string(v.integer);
            break;
        case ValueKind::Bool:
            out += v.integer != 0 ? "yes" : "no";
            break;
        case ValueKind::String:
            out += quote_text(v.text);
            break;
        case ValueKind::Decimal:
        case ValueKind::Date:
        case ValueKind::Ident:
        case ValueKind::Ref:
            out += v.text;
            break;
        case ValueKind::Block:
            out += "{\n";
            for (const data::Entry& e : v.entries) {
                write_entry(out, e, indent + 1);
            }
            write_pad(out, indent);
            out += "}";
            break;
        case ValueKind::List:
            out += "[\n";
            for (const data::Value& item : v.items) {
                write_pad(out, indent + 1);
                write_value(out, item, indent + 1);
                out += '\n';
            }
            write_pad(out, indent);
            out += "]";
            break;
    }
    out += '\n';
}

// The keys of this build; everything else at the top level is kept as an unknown entry.
bool is_known_key(std::string_view key) {
    static const std::set<std::string, std::less<>> known = [] {
        std::set<std::string, std::less<>> s = {"format_version",
                                                "settings_version",
                                                "video.width",
                                                "video.height",
                                                "video.window_mode",
                                                "video.map_quality",
                                                "video.shadows",
                                                "video.frame_limit",
                                                "video.vsync",
                                                "interface.ui_scale",
                                                "interface.language",
                                                "interface.text_size",
                                                "interface.tooltips",
                                                "interface.pause_on_events",
                                                "interface.reduce_motion",
                                                "autosave.interval_min",
                                                "autosave.on_exit",
                                                "audio.master",
                                                "audio.music",
                                                "audio.ambient",
                                                "audio.interface",
                                                "audio.output_device",
                                                "audio.mute_unfocused"};
        for (const char* name : kKeyNames) {
            s.insert(std::string("key.") + name);
        }
        return s;
    }();
    return known.find(key) != known.end();
}

struct Reader {
    [[nodiscard]] const data::Value* find(std::string_view key) const {
        const auto it = values.find(std::string(key));
        return it == values.end() ? nullptr : it->second;
    }

    void warn(std::string text) { warnings.push_back(std::move(text)); }

    void read_int(std::string_view key, int& target, int lo, int hi) {
        const data::Value* v = find(key);
        if (v == nullptr) {
            return;
        }
        if (v->kind != data::ValueKind::Int || v->integer < lo || v->integer > hi) {
            warn("settings: '" + std::string(key) + "' must be an integer " + std::to_string(lo) + ".." +
                 std::to_string(hi) + ", the default is kept");
            return;
        }
        target = static_cast<int>(v->integer);
    }

    void read_one_of(std::string_view key, int& target, std::span<const int> allowed) {
        const data::Value* v = find(key);
        if (v == nullptr) {
            return;
        }
        if (v->kind == data::ValueKind::Int &&
            std::find(allowed.begin(), allowed.end(), v->integer) != allowed.end()) {
            target = static_cast<int>(v->integer);
            return;
        }
        warn("settings: '" + std::string(key) + "' has an unsupported value, the default is kept");
    }

    void read_bool(std::string_view key, bool& target) {
        const data::Value* v = find(key);
        if (v == nullptr) {
            return;
        }
        if (v->kind != data::ValueKind::Bool) {
            warn("settings: '" + std::string(key) + "' must be yes or no, the default is kept");
            return;
        }
        target = v->integer != 0;
    }

    // A string of 1..max_bytes bytes with no control characters.
    void read_string(std::string_view key, std::string& target, std::size_t max_bytes) {
        const data::Value* v = find(key);
        if (v == nullptr) {
            return;
        }
        const bool ok = v->kind == data::ValueKind::String && !v->text.empty() &&
                        v->text.size() <= max_bytes &&
                        std::none_of(v->text.begin(), v->text.end(), [](char c) {
                            return static_cast<unsigned char>(c) < 0x20;
                        });
        if (!ok) {
            warn("settings: '" + std::string(key) +
                 "' must be a short non-empty string, the default is kept");
            return;
        }
        target = v->text;
    }

    std::map<std::string, const data::Value*> values;
    std::vector<std::string> warnings;
};

constexpr std::array<int, 4> kFrameLimits = {0, 30, 60, 120};
constexpr std::array<int, 4> kAutosaveIntervals = {0, 5, 10, 20};

void read_model(Reader& r, SettingsModel& m) {
    const int min_scale = static_cast<int>(std::lround(engine::kMinUserUiScale * 100.0f));
    const int max_scale = static_cast<int>(std::lround(engine::kMaxUserUiScale * 100.0f));
    r.read_int("video.width", m.width, 640, 16384);
    r.read_int("video.height", m.height, 480, 16384);
    r.read_int("video.window_mode", m.window_mode, 0, 2);
    r.read_int("video.map_quality", m.map_quality, 0, 2);
    r.read_int("video.shadows", m.shadows, 0, 3);
    r.read_one_of("video.frame_limit", m.frame_limit, kFrameLimits);
    r.read_bool("video.vsync", m.vsync);
    r.read_int("interface.ui_scale", m.ui_scale, min_scale, max_scale);
    r.read_string("interface.language", m.language, 16);
    r.read_int("interface.text_size", m.text_size, 0, 2);
    r.read_int("interface.tooltips", m.tooltips, 0, 2);
    r.read_bool("interface.pause_on_events", m.pause_on_events);
    r.read_bool("interface.reduce_motion", m.reduce_motion);
    r.read_one_of("autosave.interval_min", m.autosave_interval_min, kAutosaveIntervals);
    r.read_bool("autosave.on_exit", m.autosave_on_exit);
    r.read_int("audio.master", m.master_volume, 0, 100);
    r.read_int("audio.music", m.music_volume, 0, 100);
    r.read_int("audio.ambient", m.ambient_volume, 0, 100);
    r.read_int("audio.interface", m.interface_volume, 0, 100);
    r.read_int("audio.output_device", m.output_device, 0, 64);
    r.read_bool("audio.mute_unfocused", m.mute_unfocused);

    // Keys: read into a copy, then keep them only when no key serves two actions.
    std::array<std::string, kKeyActionCount> keys = m.keys;
    for (std::size_t i = 0; i < kKeyActionCount; ++i) {
        r.read_string(std::string("key.") + kKeyNames[i], keys[i], kMaxKeyIdBytes);
    }
    const std::set<std::string> distinct(keys.begin(), keys.end());
    if (distinct.size() == keys.size()) {
        m.keys = keys;
    } else {
        r.warn("settings: two actions share a key, the default keys are kept");
    }
}

}  // namespace

float user_ui_scale_of(const SettingsModel& model) {
    return std::clamp(
        static_cast<float>(model.ui_scale) / 100.0f, engine::kMinUserUiScale, engine::kMaxUserUiScale);
}

const char* key_action_name(KeyAction action) {
    return kKeyNames[static_cast<std::size_t>(action)];
}

std::string format_settings(const SettingsFile& file) {
    const SettingsModel& m = file.model;
    std::string out = "format_version = 1\nsettings_version = " + std::to_string(kSettingsVersion) + "\n";
    line(out, "video.width", m.width);
    line(out, "video.height", m.height);
    line(out, "video.window_mode", m.window_mode);
    line(out, "video.map_quality", m.map_quality);
    line(out, "video.shadows", m.shadows);
    line(out, "video.frame_limit", m.frame_limit);
    line(out, "video.vsync", m.vsync);
    line(out, "interface.ui_scale", m.ui_scale);
    line(out, "interface.language", m.language);
    line(out, "interface.text_size", m.text_size);
    line(out, "interface.tooltips", m.tooltips);
    line(out, "interface.pause_on_events", m.pause_on_events);
    line(out, "interface.reduce_motion", m.reduce_motion);
    line(out, "autosave.interval_min", m.autosave_interval_min);
    line(out, "autosave.on_exit", m.autosave_on_exit);
    line(out, "audio.master", m.master_volume);
    line(out, "audio.music", m.music_volume);
    line(out, "audio.ambient", m.ambient_volume);
    line(out, "audio.interface", m.interface_volume);
    line(out, "audio.output_device", m.output_device);
    line(out, "audio.mute_unfocused", m.mute_unfocused);
    for (std::size_t i = 0; i < kKeyActionCount; ++i) {
        line(out, std::string("key.") + kKeyNames[i], m.keys[i]);
    }
    out += file.unknown_entries;
    return out;
}

SettingsLoad load_settings(const std::filesystem::path& path) {
    SettingsLoad result;
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        return result;
    }
    const auto text = core::read_file_text(path);
    std::string problem;
    data::ParseResult parsed;
    if (!text) {
        problem = "the file cannot be read";
    } else {
        parsed = data::parse_pd(text.value());
        if (!parsed.ok()) {
            problem = data::format_diagnostic("settings.pd", parsed.errors.front());
        } else if (parsed.file.format_version != data::kCurrentFormatVersion) {
            problem = "unsupported format_version " + std::to_string(parsed.file.format_version);
        }
    }
    if (!problem.empty()) {
        // Keep the damaged file for the player or a bug report, and start from the defaults.
        std::filesystem::path bad = path;
        bad += ".bad";
        std::filesystem::rename(path, bad, ec);
        PAT_LOG_WARN("settings",
                     "settings file is damaged ({}); defaults are used, the file is kept as {}",
                     problem,
                     bad.filename().string());
        result.status = SettingsLoadStatus::Recovered;
        result.warnings.push_back("settings: damaged file: " + problem);
        return result;
    }

    Reader reader;
    std::string unknown;
    for (const data::Entry& e : parsed.file.entries) {
        if (e.key_kind == data::ValueKind::Ident && !e.label && e.op == data::Op::Eq && is_known_key(e.key)) {
            reader.values[e.key] = &e.value;
        } else {
            write_entry(unknown, e, 0);
        }
    }
    if (const data::Value* v = reader.find("settings_version")) {
        if (v->kind == data::ValueKind::Int && v->integer > kSettingsVersion) {
            reader.warn("settings: written by a newer build (settings_version " + std::to_string(v->integer) +
                        "), read as far as understood");
        }
    }
    read_model(reader, result.file.model);
    result.file.unknown_entries = std::move(unknown);
    result.status = SettingsLoadStatus::Loaded;
    result.warnings = std::move(reader.warnings);
    for (const std::string& w : result.warnings) {
        PAT_LOG_WARN("settings", "{}", w);
    }
    return result;
}

core::Result<void, core::FileIoError> save_settings(const std::filesystem::path& path,
                                                    const SettingsFile& file) {
    std::error_code ec;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), ec);
    }
    const std::string text = format_settings(file);
    core::AtomicWriteOptions options;
    options.verify = [](const std::filesystem::path& tmp) {
        const auto written = core::read_file_text(tmp);
        return written.has_value() && data::parse_pd(written.value()).ok();
    };
    return core::write_file_atomic(
        path,
        std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()),
        options);
}

}  // namespace pat::ui
