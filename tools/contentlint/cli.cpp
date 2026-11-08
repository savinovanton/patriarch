#include "pat/data/content_source.hpp"

#include <algorithm>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <memory>
#include <ostream>

#include "lint.hpp"

namespace pat::contentlint {

namespace {

namespace fs = std::filesystem;

constexpr int kExitOk = 0;
constexpr int kExitUsage = 2;

constexpr const char* kUsage =
    "usage: contentlint [options]\n"
    "  --root DIR           content root (repeatable; the first is the base game, later ones are mods);\n"
    "                       default: data\n"
    "  --lang CODE          language the strings are authored in (coverage reference), default: ru\n"
    "  --fallback CODE[,..] further languages that must have tables (repeatable), e.g. en\n"
    "  --strict             warnings (coverage, unused keys, ...) fail the run like errors\n"
    "  --format text|github plain `file:line:column: error[CODE]: message` lines (default) or GitHub\n"
    "                       Actions annotations\n"
    "  --used-keys FILE     keys referenced by code or data, one per line (repeatable; '#' comments)\n"
    "  --scan DIR           directory searched for key references (repeatable); default: `src` and\n"
    "                       `apps` next to the first root, when they exist\n"
    "  --no-scan            search only the content roots for key references\n"
    "  --ignore-unused PFX  never report keys starting with PFX as unused (repeatable)\n"
    "  --no-unused          skip the unused-key check\n"
    "  --no-rules           skip cross-object rules (lint a mod without the base game)\n"
    "  --help               this text\n"
    "exit codes: 0 clean, 1 errors (or warnings under --strict), 2 usage or I/O error\n";

std::string to_utf8(const fs::path& p) {
    const std::u8string u = p.generic_u8string();
    return std::string(reinterpret_cast<const char*>(u.data()), u.size());
}

fs::path from_utf8(std::string_view s) {
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}

bool is_language_code(std::string_view code) noexcept {
    if (code.empty() || code.size() > 16 || code.front() < 'a' || code.front() > 'z')
        return false;
    return std::ranges::all_of(
        code, [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; });
}

struct Cli {
    std::vector<std::string> roots;
    std::vector<std::string> scan_dirs;
    std::vector<std::string> used_key_files;
    bool no_scan = false;
    Format format = Format::Text;
    Options options;
    bool help = false;
};

/// Parses the arguments; returns an empty string on success, otherwise the problem.
std::string parse_args(std::span<const std::string_view> args, Cli& cli) {
    for (std::size_t i = 0; i < args.size(); ++i) {
        std::string_view arg = args[i];
        if (!arg.starts_with("--"))
            return std::format("unexpected argument '{}'", arg);
        std::string_view name = arg.substr(2);
        std::optional<std::string_view> inline_value;
        if (const std::size_t eq = name.find('='); eq != std::string_view::npos) {
            inline_value = name.substr(eq + 1);
            name = name.substr(0, eq);
        }

        const auto take_value = [&](std::string& out) -> std::string {
            if (inline_value) {
                out = std::string(*inline_value);
            } else if (i + 1 < args.size()) {
                out = std::string(args[++i]);
            } else {
                return std::format("option --{} needs a value", name);
            }
            return out.empty() ? std::format("option --{} needs a non-empty value", name) : std::string();
        };
        const auto flag = [&](bool& target) -> std::string {
            if (inline_value)
                return std::format("option --{} takes no value", name);
            target = true;
            return std::string();
        };

        std::string value;
        std::string problem;
        if (name == "root") {
            if (problem = take_value(value); problem.empty())
                cli.roots.push_back(value);
        } else if (name == "scan") {
            if (problem = take_value(value); problem.empty())
                cli.scan_dirs.push_back(value);
        } else if (name == "used-keys") {
            if (problem = take_value(value); problem.empty())
                cli.used_key_files.push_back(value);
        } else if (name == "ignore-unused") {
            if (problem = take_value(value); problem.empty())
                cli.options.ignore_unused_prefixes.push_back(value);
        } else if (name == "lang") {
            if (problem = take_value(value); problem.empty()) {
                if (!is_language_code(value))
                    problem = std::format("'{}' is not a language code ([a-z][a-z0-9_]*)", value);
                cli.options.default_language = value;
            }
        } else if (name == "fallback") {
            if (problem = take_value(value); problem.empty()) {
                std::size_t start = 0;
                while (start <= value.size() && problem.empty()) {
                    const std::size_t comma = value.find(',', start);
                    const std::string code =
                        value.substr(start, comma == std::string::npos ? comma : comma - start);
                    if (!is_language_code(code))
                        problem = std::format("'{}' is not a language code ([a-z][a-z0-9_]*)", code);
                    else
                        cli.options.fallback.push_back(code);
                    if (comma == std::string::npos)
                        break;
                    start = comma + 1;
                }
            }
        } else if (name == "format") {
            if (problem = take_value(value); problem.empty()) {
                if (value == "text")
                    cli.format = Format::Text;
                else if (value == "github")
                    cli.format = Format::Github;
                else
                    problem = std::format("unknown format '{}' (use text or github)", value);
            }
        } else if (name == "strict") {
            problem = flag(cli.options.strict);
        } else if (name == "no-scan") {
            problem = flag(cli.no_scan);
        } else if (name == "no-unused") {
            bool set = false;
            problem = flag(set);
            cli.options.check_unused = !set;
        } else if (name == "no-rules") {
            bool set = false;
            problem = flag(set);
            cli.options.run_rules = !set;
        } else if (name == "help") {
            cli.help = true;
        } else {
            problem = std::format("unknown option '--{}'", name);
        }
        if (!problem.empty())
            return problem;
    }
    return std::string();
}

bool is_directory(const std::string& path) {
    std::error_code ec;
    return fs::is_directory(from_utf8(path), ec) && !ec;
}

/// Reads a `--used-keys` list: one key per line, blank lines and `#` comments ignored.
std::string read_used_keys(const std::string& path, std::vector<UsedKey>& out) {
    std::ifstream in(from_utf8(path), std::ios::binary);
    if (!in)
        return std::format("cannot read '{}'", path);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (in.bad())
        return std::format("cannot read '{}'", path);
    std::uint32_t line_no = 0;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t end = text.find('\n', start);
        std::string_view line(text.data() + start, (end == std::string::npos ? text.size() : end) - start);
        ++line_no;
        const auto is_space = [](char c) { return c == ' ' || c == '\t' || c == '\r'; };
        while (!line.empty() && is_space(line.front()))
            line.remove_prefix(1);
        while (!line.empty() && is_space(line.back()))
            line.remove_suffix(1);
        if (!line.empty() && line.front() != '#')
            out.push_back(UsedKey{std::string(line), path, line_no});
        if (end == std::string::npos)
            break;
        start = end + 1;
    }
    return std::string();
}

}  // namespace

int run_cli(std::span<const std::string_view> args, std::ostream& out, std::ostream& err) {
    Cli cli;
    if (const std::string problem = parse_args(args, cli); !problem.empty()) {
        err << "contentlint: " << problem << '\n' << kUsage;
        return kExitUsage;
    }
    if (cli.help) {
        out << kUsage;
        return kExitOk;
    }
    if (cli.roots.empty())
        cli.roots.emplace_back("data");

    for (const std::string& root : cli.roots) {
        if (!is_directory(root)) {
            err << std::format("contentlint: cannot read root '{}': not a directory\n", root);
            return kExitUsage;
        }
    }
    for (const std::string& file : cli.used_key_files) {
        if (const std::string problem = read_used_keys(file, cli.options.used_keys); !problem.empty()) {
            err << "contentlint: " << problem << '\n';
            return kExitUsage;
        }
    }

    std::vector<std::string> scan_dirs = cli.scan_dirs;
    for (const std::string& dir : scan_dirs) {
        if (!is_directory(dir)) {
            err << std::format("contentlint: cannot scan '{}': not a directory\n", dir);
            return kExitUsage;
        }
    }
    if (scan_dirs.empty() && !cli.no_scan) {
        // Default: `src` and `apps` next to the first root (the repository root for `--root data`).
        std::error_code ec;
        fs::path first = fs::absolute(from_utf8(cli.roots.front()), ec).lexically_normal();
        if (!ec) {
            if (!first.has_filename())
                first = first.parent_path();
            const fs::path base = first.parent_path();
            for (const char* name : {"src", "apps"}) {
                const std::string candidate = to_utf8(base / name);
                if (is_directory(candidate))
                    scan_dirs.push_back(candidate);
            }
        }
    }

    std::vector<std::unique_ptr<data::DirectorySource>> owned;
    std::vector<const data::ContentSource*> roots;
    std::vector<const data::ContentSource*> scans;
    for (const std::string& root : cli.roots) {
        owned.push_back(std::make_unique<data::DirectorySource>(from_utf8(root)));
        roots.push_back(owned.back().get());
    }
    for (const std::string& dir : scan_dirs) {
        owned.push_back(std::make_unique<data::DirectorySource>(from_utf8(dir)));
        scans.push_back(owned.back().get());
    }

    const Report report = lint(roots, scans, cli.options);
    render(report, cli.format, out);
    return report.exit_code();
}

}  // namespace pat::contentlint
