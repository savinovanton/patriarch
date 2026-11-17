#include "pat/game/save_catalog.hpp"

#include "pat/core/path_utf8.hpp"
#include "pat/core/utf8.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <system_error>

namespace pat::game {

namespace {

using core::Error;

constexpr int kErrNoRoom = 61;
constexpr int kErrNotSave = 62;
constexpr int kErrRenameFailed = 63;
constexpr int kErrDeleteFailed = 64;

// Container error codes that mean "written by a newer build" (docs/dev/save-format.md section 2.5).
constexpr int kCodeVersionNewer = 22;
constexpr int kCodeSchemaNewer = 28;

constexpr std::string_view kTempSuffix = ".tmp";

bool has_save_extension(const std::filesystem::path& p) {
    return core::path_to_utf8(p.extension()) == kSaveExtension;
}

bool is_temp_file(const std::filesystem::path& p) {
    const std::string name = core::path_to_utf8(p.filename());
    const std::string suffix = std::string(kSaveExtension) + std::string(kTempSuffix);
    return name.size() > suffix.size() &&
           name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::int64_t to_unix_seconds(std::filesystem::file_time_type t) {
    const auto sys = std::chrono::clock_cast<std::chrono::system_clock>(t);
    return std::chrono::duration_cast<std::chrono::seconds>(sys.time_since_epoch()).count();
}

SaveEntry read_entry(const std::filesystem::directory_entry& de, const ScanOptions& options) {
    SaveEntry e;
    e.path = de.path();
    e.file_stem = core::path_to_utf8(de.path().stem());
    std::error_code ec;
    e.file_size = de.file_size(ec);
    if (ec) {
        e.file_size = 0;
    }
    const auto mtime = de.last_write_time(ec);
    e.file_mtime = ec ? 0 : to_unix_seconds(mtime);
    e.sort_time = e.file_mtime;

    auto info = sim::read_save_summary(de.path());
    if (!info.has_value()) {
        e.error_code = info.error().code;
        e.cls = (e.error_code == kCodeVersionNewer || e.error_code == kCodeSchemaNewer) ? SaveClass::Newer
                                                                                        : SaveClass::Corrupt;
        return e;
    }
    e.format_version = info.value().format_version;
    e.summary = std::move(info.value().summary);
    e.cls = SaveClass::Compatible;
    if (e.summary) {
        if (e.summary->state_schema > sim::World::kStateSchema) {
            e.cls = SaveClass::Newer;
        } else if (options.expected_content_hash != 0 && e.summary->content_hash != 0 &&
                   e.summary->content_hash != options.expected_content_hash) {
            e.cls = SaveClass::ForeignContent;
        }
        if (e.summary->saved_at > 0) {
            e.sort_time = e.summary->saved_at;
        }
    }
    return e;
}

bool is_reserved_device_name(std::string_view stem) {
    // The name before the first dot decides on Windows ("CON.txt" is still the console).
    const std::size_t dot = stem.find('.');
    std::string up(stem.substr(0, dot));
    while (!up.empty() && up.back() == ' ') {
        up.pop_back();
    }
    std::transform(up.begin(), up.end(), up.begin(), [](char c) {
        return static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    });
    static constexpr std::array<std::string_view, 4> kPlain = {"CON", "PRN", "AUX", "NUL"};
    if (std::find(kPlain.begin(), kPlain.end(), up) != kPlain.end()) {
        return true;
    }
    return up.size() == 4 && (up.compare(0, 3, "COM") == 0 || up.compare(0, 3, "LPT") == 0) && up[3] >= '1' &&
           up[3] <= '9';
}

/// Longest prefix of `stem` (whole UTF-8 characters) for which `dir/<prefix><suffix>.psv.tmp` fits.
std::string fit_stem(const std::filesystem::path& dir, std::string stem, std::string_view suffix) {
    const auto fits = [&](const std::string& st) {
        const auto p = dir / core::path_from_utf8(st + std::string(suffix) + std::string(kSaveExtension) +
                                                  std::string(kTempSuffix));
        return p.native().size() <= kMaxSavePathLength;
    };
    while (!stem.empty() && !fits(stem)) {
        std::size_t cut = stem.size() - 1;  // start of the last character
        while (cut > 0 && (static_cast<unsigned char>(stem[cut]) & 0xC0U) == 0x80U) {
            --cut;
        }
        stem.resize(cut);
        while (!stem.empty() && (stem.back() == ' ' || stem.back() == '.')) {
            stem.pop_back();
        }
    }
    return stem;
}

core::Result<std::filesystem::path, Error>
pick_path(const std::filesystem::path& dir, std::string_view name, const std::filesystem::path& own) {
    using R = core::Result<std::filesystem::path, Error>;
    const std::string clean = sanitize_save_name(name);
    for (int n = 1; n < 100000; ++n) {
        const std::string suffix = n == 1 ? std::string() : " (" + std::to_string(n) + ")";
        const std::string stem = fit_stem(dir, clean, suffix);
        if (stem.empty()) {
            return R::err(Error{kErrNoRoom, "the directory path leaves no room for a file name"});
        }
        const auto candidate = dir / core::path_from_utf8(stem + suffix + std::string(kSaveExtension));
        std::error_code ec;
        if (!std::filesystem::exists(candidate, ec) || (!own.empty() && candidate == own)) {
            return R::ok(candidate);
        }
    }
    return R::err(Error{kErrNoRoom, "no free file name"});
}

}  // namespace

std::vector<SaveEntry> scan_saves(const std::filesystem::path& dir, const ScanOptions& options) {
    std::vector<SaveEntry> out;
    std::error_code ec;
    std::filesystem::directory_iterator it(dir, ec);
    if (ec) {
        return out;
    }
    for (const std::filesystem::directory_iterator end; it != end; it.increment(ec)) {
        if (ec) {
            break;
        }
        std::error_code ec2;
        if (!it->is_regular_file(ec2) || ec2 || !has_save_extension(it->path())) {
            continue;
        }
        out.push_back(read_entry(*it, options));
    }
    std::sort(out.begin(), out.end(), [](const SaveEntry& a, const SaveEntry& b) {
        if (a.sort_time != b.sort_time) {
            return a.sort_time > b.sort_time;
        }
        return a.file_stem < b.file_stem;
    });
    return out;
}

std::size_t remove_orphan_temp_files(const std::filesystem::path& dir) {
    std::error_code ec;
    std::filesystem::directory_iterator it(dir, ec);
    if (ec) {
        return 0;
    }
    std::vector<std::filesystem::path> found;
    for (const std::filesystem::directory_iterator end; it != end; it.increment(ec)) {
        if (ec) {
            break;
        }
        std::error_code ec2;
        if (it->is_regular_file(ec2) && !ec2 && is_temp_file(it->path())) {
            found.push_back(it->path());
        }
    }
    std::size_t removed = 0;
    for (const auto& p : found) {
        std::error_code rc;
        if (std::filesystem::remove(p, rc) && !rc) {
            ++removed;
        }
    }
    return removed;
}

std::string sanitize_save_name(std::string_view name) {
    // Stray bytes that are not valid UTF-8 would make the path conversion fail: replace them too.
    std::string out;
    out.reserve(name.size());
    for (std::size_t i = 0; i < name.size();) {
        const std::size_t len = core::utf8_sequence_length(name.substr(i));
        if (len == 0) {
            out.push_back('_');
            ++i;
            continue;
        }
        if (len == 1) {
            const char ch = name[i];
            const auto c = static_cast<unsigned char>(ch);
            const bool forbidden = c < 0x20 || c == 0x7F || ch == '<' || ch == '>' || ch == ':' ||
                                   ch == '"' || ch == '/' || ch == '\\' || ch == '|' || ch == '?' ||
                                   ch == '*';
            out.push_back(forbidden ? '_' : ch);
        } else {
            out.append(name.substr(i, len));
        }
        i += len;
    }
    while (!out.empty() && (out.back() == ' ' || out.back() == '.')) {
        out.pop_back();
    }
    const std::size_t lead = out.find_first_not_of(' ');
    out.erase(0, lead == std::string::npos ? out.size() : lead);
    if (out.empty()) {
        return "save";
    }
    if (is_reserved_device_name(out)) {
        out.insert(out.begin(), '_');
    }
    return out;
}

core::Result<std::filesystem::path, core::Error> unique_save_path(const std::filesystem::path& dir,
                                                                  std::string_view name) {
    return pick_path(dir, name, {});
}

core::Result<std::filesystem::path, core::Error> rename_save(const std::filesystem::path& path,
                                                             std::string_view new_name) {
    using R = core::Result<std::filesystem::path, Error>;
    std::error_code ec;
    if (!has_save_extension(path) || !std::filesystem::is_regular_file(path, ec)) {
        return R::err(Error{kErrNotSave, "not an existing save file"});
    }
    auto target = pick_path(path.parent_path(), new_name, path);
    if (!target.has_value() || target.value() == path) {
        return target;
    }
    std::filesystem::rename(path, target.value(), ec);
    if (ec) {
        return R::err(Error{kErrRenameFailed, "cannot rename the save file"});
    }
    return target;
}

core::Result<void, core::Error> delete_save(const std::filesystem::path& path) {
    using R = core::Result<void, Error>;
    std::error_code ec;
    if (!has_save_extension(path) || !std::filesystem::is_regular_file(path, ec)) {
        return R::err(Error{kErrNotSave, "not an existing save file"});
    }
    if (!std::filesystem::remove(path, ec) || ec) {
        return R::err(Error{kErrDeleteFailed, "cannot delete the save file"});
    }
    return R::ok();
}

}  // namespace pat::game
