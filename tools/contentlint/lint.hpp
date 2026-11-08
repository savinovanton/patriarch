#pragma once

#include "pat/data/content_source.hpp"

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// contentlint (E4-T08, docs/dev/contentlint.md): one command that checks the whole game content and
// the localisation tables and reports EVERYTHING it finds, sorted, in a form a human or CI can read.
//
// The library part (this header) works on `ContentSource` objects, so tests feed it in-memory
// corpora; `main.cpp` only turns the command line into directory sources. Nothing here uses floating
// point, and the output never depends on directory iteration order.

namespace pat::contentlint {

enum class Severity : std::uint8_t { Warning, Error };

/// One problem. `file` is the display path (root name + relative path); empty for global findings.
struct Finding {
    Severity severity = Severity::Error;
    std::string file;
    std::uint32_t line = 0;  // 0 = no position
    std::uint32_t column = 0;
    std::string code;  // PD0xx / PD1xx (data), LC0xx (localisation), CL0xx (this tool)
    std::string message;

    /// `file:line:column: error[CODE]: message` (position and file omitted when unknown).
    [[nodiscard]] std::string format_text() const;
    /// GitHub Actions workflow command: `::error file=F,line=L,col=C,title=CODE::message`.
    [[nodiscard]] std::string format_github() const;
};

/// Coverage of one language against the default one.
struct CoverageLine {
    std::string language;
    bool has_table = false;
    std::size_t reference_keys = 0;  // keys of the default language
    std::size_t keys = 0;            // keys of this language
    std::size_t missing = 0;
    std::size_t extra = 0;
};

/// One key listed by the author in a `--used-keys` file.
struct UsedKey {
    std::string key;
    std::string file;  // display path of the list
    std::uint32_t line = 0;
};

struct Options {
    /// The language the strings are authored in; the reference for coverage.
    std::string default_language = "ru";
    /// Further languages that are expected to have tables (the fallback chain after the default one).
    std::vector<std::string> fallback;
    /// Warnings become errors.
    bool strict = false;
    /// Run cross-object validation. Off for a mod linted alone.
    bool run_rules = true;
    /// Report localisation keys nobody references.
    bool check_unused = true;
    /// Keys that start with one of these prefixes are never reported as unused.
    std::vector<std::string> ignore_unused_prefixes;
    /// Keys the author lists explicitly; they count as used, and one the default language does not
    /// define is an error (CL005).
    std::vector<UsedKey> used_keys;
};

struct Report {
    std::vector<Finding> findings;       // sorted, severity already adjusted for --strict
    std::vector<CoverageLine> coverage;  // sorted by language
    std::vector<std::string> notes;      // informational lines (what was checked, what was skipped)
    std::size_t content_objects = 0;     // effective objects in the registry
    std::size_t errors = 0;
    std::size_t warnings = 0;

    /// 0 clean, 1 at least one error.
    [[nodiscard]] int exit_code() const noexcept { return errors == 0 ? 0 : 1; }
};

/// Checks `roots` (roots[0] is the base game, later ones are mods) and returns the report.
/// `scan_sources` are extra file trees searched for key references (source code, UI markup); the
/// content roots are searched too (their `.pd` files outside `loc/`). Never throws on bad input.
[[nodiscard]] Report lint(std::span<const data::ContentSource* const> roots,
                          std::span<const data::ContentSource* const> scan_sources,
                          const Options& options);

enum class Format : std::uint8_t { Text, Github };

/// Writes findings, coverage notes and the summary line.
void render(const Report& report, Format format, std::ostream& out);

// ---- key reference scan (exposed for tests) --------------------------------------------------

/// What a scan of text found: exact key tokens and prefixes of keys built at run time.
struct KeyScan {
    std::vector<std::string> keys;      // tokens that are exactly a known key (sorted, unique)
    std::vector<std::string> prefixes;  // tokens like "ui.month." (sorted, unique)
};

/// Scans `text` for tokens `[A-Za-z0-9_.]+` that equal a key of `known_keys` (sorted), or that end with
/// a dot and contain at least two dots (a prefix such as "ui.month."). Heuristic by design: a key
/// mentioned in a comment counts as used.
void scan_text_for_keys(std::string_view text, std::span<const std::string> known_keys, KeyScan& scan);

// ---- command line ----------------------------------------------------------------------------

/// Runs the tool: `args` are the arguments after the program name. Writes the report to `out`,
/// usage and I/O problems to `err`. Returns 0 clean, 1 errors (or warnings under --strict), 2 usage
/// or I/O error.
int run_cli(std::span<const std::string_view> args, std::ostream& out, std::ostream& err);

}  // namespace pat::contentlint
