#include "lint.hpp"

#include "pat/content/demography_content.hpp"
#include "pat/content/economy_content.hpp"
#include "pat/content/m1_content.hpp"
#include "pat/content/names_content.hpp"
#include "pat/data/content_loader.hpp"
#include "pat/data/content_schemas.hpp"
#include "pat/loc/localizer.hpp"

#include <algorithm>
#include <format>
#include <map>
#include <ostream>
#include <set>
#include <tuple>
#include <utility>

namespace pat::contentlint {

namespace {

constexpr std::size_t kMaxScannedFileBytes = 8u * 1024u * 1024u;

// ---- helpers ---------------------------------------------------------------------------------

/// `root` + "/" + `relative` with separators normalised; a root of "." or "" contributes nothing.
std::string display_path(std::string root, const std::string& relative) {
    std::ranges::replace(root, '\\', '/');
    while (root.starts_with("./"))
        root.erase(0, 2);
    while (root.size() > 1 && root.back() == '/')
        root.pop_back();
    if (root.empty() || root == ".")
        return relative;
    if (relative.empty())
        return root;
    return root + "/" + relative;
}

Finding from_load_error(const data::LoadError& error) {
    Finding f;
    f.severity = Severity::Error;
    f.file = display_path(error.root_name, error.file);
    f.line = error.diagnostic.pos.line;
    f.column = error.diagnostic.pos.column;
    f.code = data::error_code_name(error.diagnostic.code);
    f.message = error.diagnostic.message;
    return f;
}

Finding from_loc_error(const loc::LocError& error) {
    Finding f;
    f.severity = Severity::Error;
    f.file = display_path(error.root_name, error.file);
    f.line = error.pos.line;
    f.column = error.pos.column;
    f.code = error.code;
    f.message = error.message;
    return f;
}

Finding make_finding(
    Severity severity, std::string file, data::SourcePos pos, std::string code, std::string message) {
    Finding f;
    f.severity = severity;
    f.file = std::move(file);
    f.line = pos.line;
    f.column = pos.column;
    f.code = std::move(code);
    f.message = std::move(message);
    return f;
}

/// Where a key is defined, as a display path and position (empty path when unknown).
std::pair<std::string, data::SourcePos> locate(const loc::Localizer& localizer,
                                               std::span<const data::ContentSource* const> roots,
                                               const std::string& language,
                                               const std::string& key) {
    const std::optional<loc::KeyOrigin> origin = localizer.origin(language, key);
    if (!origin || origin->root >= roots.size())
        return {std::string(), data::SourcePos{}};
    return {display_path(roots[origin->root]->name(), origin->file), origin->pos};
}

bool has_scannable_extension(const std::string& path) {
    for (const char* ext : {".pd", ".cpp", ".hpp", ".h", ".cc", ".rml"}) {
        if (path.ends_with(ext))
            return true;
    }
    return false;
}

bool is_token_char(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.';
}

void sort_unique(std::vector<std::string>& values) {
    std::ranges::sort(values);
    values.erase(std::ranges::unique(values).begin(), values.end());
}

/// Scans every readable, scannable file of `source` (paths starting with `skip_prefix` are skipped).
/// Returns the number of files read; a listing failure is added to `notes`.
std::size_t scan_source(const data::ContentSource& source,
                        std::string_view skip_prefix,
                        std::span<const std::string> known,
                        KeyScan& scan,
                        std::vector<std::string>& notes) {
    const auto listed = source.list_files();
    if (!listed) {
        notes.push_back(std::format("could not scan '{}': {}", source.name(), listed.error()));
        return 0;
    }
    std::vector<std::string> paths = listed.value();
    std::ranges::sort(paths);
    std::size_t files = 0;
    for (const std::string& path : paths) {
        if (!skip_prefix.empty() && path.starts_with(skip_prefix))
            continue;
        if (!has_scannable_extension(path))
            continue;
        const auto text = source.read_file(path);
        if (!text || text.value().size() > kMaxScannedFileBytes)
            continue;
        scan_text_for_keys(text.value(), known, scan);
        ++files;
    }
    return files;
}

bool starts_with_any(const std::string& key, const std::vector<std::string>& prefixes) {
    return std::ranges::any_of(prefixes, [&](const std::string& p) { return key.starts_with(p); });
}

}  // namespace

// ---- Finding ---------------------------------------------------------------------------------

std::string Finding::format_text() const {
    std::string out;
    if (!file.empty()) {
        out = file;
        if (line != 0)
            out += std::format(":{}:{}", line, column);
        out += ": ";
    }
    out += std::format("{}[{}]: {}", severity == Severity::Error ? "error" : "warning", code, message);
    return out;
}

namespace {

std::string escape_github(std::string_view text, bool property) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        switch (c) {
            case '%':
                out += "%25";
                break;
            case '\r':
                out += "%0D";
                break;
            case '\n':
                out += "%0A";
                break;
            case ':':
                out += property ? "%3A" : ":";
                break;
            case ',':
                out += property ? "%2C" : ",";
                break;
            default:
                out += c;
                break;
        }
    }
    return out;
}

}  // namespace

std::string Finding::format_github() const {
    std::string out = severity == Severity::Error ? "::error " : "::warning ";
    if (!file.empty()) {
        out += "file=" + escape_github(file, true);
        if (line != 0)
            out += std::format(",line={},col={}", line, column);
        out += ",";
    }
    out += "title=" + escape_github(code, true) + "::" + escape_github(message, false);
    return out;
}

// ---- key scan --------------------------------------------------------------------------------

void scan_text_for_keys(std::string_view text, std::span<const std::string> known_keys, KeyScan& scan) {
    const auto is_known = [&](std::string_view token) {
        const auto it = std::lower_bound(
            known_keys.begin(), known_keys.end(), token, [](const std::string& a, std::string_view b) {
                return std::string_view(a) < b;
            });
        return it != known_keys.end() && std::string_view(*it) == token;
    };

    const std::size_t n = text.size();
    std::size_t i = 0;
    while (i < n) {
        if (!is_token_char(text[i])) {
            ++i;
            continue;
        }
        std::size_t j = i;
        while (j < n && is_token_char(text[j]))
            ++j;
        std::string_view token = text.substr(i, j - i);
        i = j;

        while (!token.empty() && token.front() == '.')
            token.remove_prefix(1);
        if (token.find('.') == std::string_view::npos)
            continue;
        if (token.back() == '.' && std::ranges::count(token, '.') >= 2)
            scan.prefixes.emplace_back(token);
        while (!token.empty() && token.back() == '.')
            token.remove_suffix(1);
        if (!token.empty() && is_known(token))
            scan.keys.emplace_back(token);
    }
    sort_unique(scan.keys);
    sort_unique(scan.prefixes);
}

// ---- lint ------------------------------------------------------------------------------------

Report lint(std::span<const data::ContentSource* const> roots,
            std::span<const data::ContentSource* const> scan_sources,
            const Options& options) {
    Report report;
    std::vector<Finding>& findings = report.findings;

    // 1. Content: load once, then run the cross-object builders on the same registry.
    const data::SchemaSet schemas = data::content_schemas();
    data::LoadResult loaded = data::load_content(roots, schemas);
    for (const data::LoadError& error : loaded.errors)
        findings.push_back(from_load_error(error));
    report.content_objects = loaded.registry.object_count();
    if (options.run_rules) {
        if (loaded.ok()) {
            const content::EconomyRulesResult economy = content::build_economy_rules(loaded.registry);
            if (!economy) {
                for (const data::LoadError& error : economy.error())
                    findings.push_back(from_load_error(error));
            }
            const content::DemographyRulesResult demography =
                content::build_demography_rules(loaded.registry);
            if (!demography) {
                for (const data::LoadError& error : demography.error())
                    findings.push_back(from_load_error(error));
            }
            const content::NamePoolsResult names = content::build_name_pools(loaded.registry);
            if (!names) {
                for (const data::LoadError& error : names.error())
                    findings.push_back(from_load_error(error));
            }
            for (const data::LoadError& error : content::validate_m1_definitions(loaded.registry))
                findings.push_back(from_load_error(error));
        } else {
            report.notes.emplace_back("cross-object rules were not checked: fix the load errors first");
        }
    }

    // 2. Localisation tables.
    loc::LoadOptions loc_options;
    loc_options.default_language = options.default_language;
    loc_options.chain.clear();
    loc_options.chain.push_back(options.default_language);
    for (const std::string& language : options.fallback) {
        if (std::ranges::find(loc_options.chain, language) == loc_options.chain.end())
            loc_options.chain.push_back(language);
    }
    loc::LocLoadResult localized = loc::load_localization(roots, loc_options);
    for (const loc::LocError& error : localized.errors)
        findings.push_back(from_loc_error(error));
    const loc::Localizer& localizer = localized.localizer;
    const std::string& def = options.default_language;

    const std::vector<std::string> present = localizer.languages();
    std::set<std::string> language_set(present.begin(), present.end());
    std::set<std::string> expected;
    if (!present.empty())
        expected.insert(def);
    expected.insert(options.fallback.begin(), options.fallback.end());
    for (const std::string& language : expected) {
        if (!localizer.has_language(language)) {
            findings.push_back(make_finding(
                Severity::Warning,
                std::string(),
                data::SourcePos{},
                "CL006",
                std::format(
                    "language '{}' has no string tables (expected loc/{}/*.pd)", language, language)));
        }
    }
    language_set.insert(expected.begin(), expected.end());

    const bool default_present = localizer.has_language(def);
    const std::size_t reference_keys = localizer.key_count(def);
    if (default_present) {
        for (const loc::LanguageCoverage& cov : localizer.coverage()) {
            for (const std::string& key : cov.missing) {
                auto [file, pos] = locate(localizer, roots, def, key);
                findings.push_back(
                    make_finding(Severity::Warning,
                                 std::move(file),
                                 pos,
                                 "CL001",
                                 std::format("key '{}' is missing in language '{}' (it is defined in '{}')",
                                             key,
                                             cov.language,
                                             def)));
            }
            for (const std::string& key : cov.extra) {
                auto [file, pos] = locate(localizer, roots, cov.language, key);
                findings.push_back(make_finding(
                    Severity::Warning,
                    std::move(file),
                    pos,
                    "CL002",
                    std::format("key '{}' exists in language '{}' but not in the default language '{}'",
                                key,
                                cov.language,
                                def)));
            }
            for (const std::string& key : localizer.keys(cov.language)) {
                if (!localizer.contains(def, key))
                    continue;
                const std::vector<std::string> want = localizer.parameters(def, key);
                const std::vector<std::string> have = localizer.parameters(cov.language, key);
                if (want == have)
                    continue;
                const auto join = [](const std::vector<std::string>& names) {
                    std::string text;
                    for (const std::string& name : names)
                        text += (text.empty() ? "" : ", ") + name;
                    return text.empty() ? std::string("none") : text;
                };
                auto [file, pos] = locate(localizer, roots, cov.language, key);
                findings.push_back(
                    make_finding(Severity::Warning,
                                 std::move(file),
                                 pos,
                                 "CL003",
                                 std::format("message '{}' uses parameters ({}) in '{}' but ({}) in '{}'",
                                             key,
                                             join(have),
                                             cov.language,
                                             join(want),
                                             def)));
            }
        }
    }
    for (const std::string& language : language_set) {
        CoverageLine line;
        line.language = language;
        line.has_table = localizer.has_language(language);
        line.reference_keys = reference_keys;
        line.keys = localizer.key_count(language);
        if (language != def && default_present) {
            line.missing = localizer.missing_keys(language, def).size();
            line.extra = localizer.missing_keys(def, language).size();
        } else if (language != def) {
            line.missing = 0;
        }
        report.coverage.push_back(std::move(line));
    }

    // 3. Key references: unused keys (heuristic scan) and explicitly listed keys that do not exist.
    if (default_present && options.check_unused) {
        const std::vector<std::string> known = localizer.keys(def);
        KeyScan scan;
        std::size_t files = 0;
        for (const data::ContentSource* root : roots)
            files += scan_source(*root, "loc/", known, scan, report.notes);
        for (const data::ContentSource* source : scan_sources)
            files += scan_source(*source, std::string_view(), known, scan, report.notes);
        report.notes.push_back(
            std::format("scanned {} file(s) for references to {} localisation key(s)", files, known.size()));

        std::set<std::string> used(scan.keys.begin(), scan.keys.end());
        for (const UsedKey& entry : options.used_keys)
            used.insert(entry.key);
        const std::vector<std::string_view> used_views(used.begin(), used.end());
        for (const std::string& key : localizer.unused_keys(def, used_views)) {
            if (starts_with_any(key, scan.prefixes) || starts_with_any(key, options.ignore_unused_prefixes))
                continue;
            auto [file, pos] = locate(localizer, roots, def, key);
            findings.push_back(
                make_finding(Severity::Warning,
                             std::move(file),
                             pos,
                             "CL004",
                             std::format("key '{}' is not referenced by any scanned file", key)));
        }
    }
    for (const UsedKey& entry : options.used_keys) {
        if (localizer.resolves(entry.key))
            continue;
        findings.push_back(
            make_finding(Severity::Error,
                         entry.file,
                         data::SourcePos{entry.line, 1},
                         "CL005",
                         std::format("key '{}' is used but defined in no language of the chain", entry.key)));
    }

    // 4. Sort, promote warnings under --strict, count.
    std::ranges::sort(findings, [](const Finding& a, const Finding& b) {
        return std::tie(a.file, a.line, a.column, a.code, a.message, a.severity) <
               std::tie(b.file, b.line, b.column, b.code, b.message, b.severity);
    });
    for (Finding& f : findings) {
        if (options.strict && f.severity == Severity::Warning)
            f.severity = Severity::Error;
        if (f.severity == Severity::Error)
            ++report.errors;
        else
            ++report.warnings;
    }
    return report;
}

void render(const Report& report, Format format, std::ostream& out) {
    for (const Finding& f : report.findings)
        out << (format == Format::Github ? f.format_github() : f.format_text()) << '\n';
    for (const std::string& note : report.notes)
        out << "note: " << note << '\n';
    for (const CoverageLine& line : report.coverage) {
        if (!line.has_table) {
            out << "note: coverage: " << line.language << ": no string tables\n";
            continue;
        }
        const std::size_t covered = line.keys >= line.extra ? line.keys - line.extra : 0;
        const std::size_t percent = line.reference_keys == 0 ? 100 : covered * 100 / line.reference_keys;
        out << std::format("note: coverage: {}: {} key(s), {} of {} reference key(s) translated ({}%), {} "
                           "missing, {} extra\n",
                           line.language,
                           line.keys,
                           covered,
                           line.reference_keys,
                           percent,
                           line.missing,
                           line.extra);
    }
    out << std::format("contentlint: {} object(s) checked, {} error(s), {} warning(s)\n",
                       report.content_objects,
                       report.errors,
                       report.warnings);
}

}  // namespace pat::contentlint
