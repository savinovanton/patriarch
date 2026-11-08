#pragma once

#include "pat/data/content_source.hpp"
#include "pat/data/pd_tree.hpp"
#include "pat/loc/message.hpp"
#include "pat/loc/plural.hpp"

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Localisation (E4-T06, docs/dev/localization.md): string tables in `.pd` files under
// `loc/<language>/`, named parameters, plural forms, a fallback chain of languages and detection of
// missing keys. The module depends on `core` and `data` (it reads tables with the `.pd` parser and
// the ContentSource interface); `data` itself does not know about it.

namespace pat::loc {

/// What loading may be configured with.
struct LoadOptions {
    /// Languages the loader accepts (the directory name under `loc/` must be one of them) with their
    /// plural rules. Add entries here to support another language.
    std::vector<LanguageInfo> languages = builtin_languages();
    /// The language strings are authored in; the reference for coverage reports.
    std::string default_language = "ru";
    /// Initial fallback chain: the first language that has the key wins. Languages without a table
    /// are skipped.
    std::vector<std::string> chain = {"ru", "en"};
};

/// A problem found while loading a table, with the root and file it belongs to.
struct LocError {
    std::uint32_t root = 0;  // index in the list of roots
    std::string root_name;   // ContentSource::name()
    std::string file;        // path relative to the root; empty for root-level problems
    data::SourcePos pos;     // {0, 0} when there is no position
    std::string code;        // "PD0xx" (syntax), "PD113", "LC001"... (see docs/dev/localization.md)
    std::string message;

    /// `[root 0 'data'] loc/ru/ui.pd:3:7: error[LC010]: message`
    [[nodiscard]] std::string format() const;
};

/// Where a key's effective text comes from.
struct KeyOrigin {
    std::uint32_t root = 0;
    std::string file;
    data::SourcePos pos;
};

/// Coverage of one language against the default one (the input of the future `contentlint`).
struct LanguageCoverage {
    std::string language;
    std::size_t total = 0;             // keys in this language
    std::vector<std::string> missing;  // in the default language but not here (sorted)
    std::vector<std::string> extra;    // here but not in the default language (sorted)
};

/// The loaded string tables of all languages plus the fallback chain.
///
/// Thread safety: formatting and the read-only queries are const and may run concurrently (the log
/// bookkeeping for missing keys is protected by a mutex); set_chain() must not run concurrently with
/// them. The Localizer is movable, not copyable.
class Localizer {
public:
    Localizer();
    ~Localizer();
    Localizer(Localizer&&) noexcept;
    Localizer& operator=(Localizer&&) noexcept;
    Localizer(const Localizer&) = delete;
    Localizer& operator=(const Localizer&) = delete;

    // ---- formatting ----------------------------------------------------------------------

    /// Formats the message `key` (the first language of the chain that has it) and appends it to
    /// `out`; no heap allocation happens while `out` has capacity for the text. A key found nowhere
    /// writes the visible placeholder `[[key]]`, sets FormatResult::missing_key and logs a warning
    /// once per key.
    FormatResult format_to(std::string& out, std::string_view key, std::span<const Arg> args = {}) const;

    /// Same into a fixed buffer: never allocates, stops at a code point boundary when full
    /// (FormatResult::truncated). The text is not NUL terminated.
    FormatResult format_to(std::span<char> out, std::string_view key, std::span<const Arg> args = {}) const;

    /// Convenience: returns the formatted text in a new string.
    [[nodiscard]] std::string format(std::string_view key, std::span<const Arg> args = {}) const;

    /// Braced-list forms: `loc.format("ui.age", {Arg("n", 21)})`.
    FormatResult format_to(std::string& out, std::string_view key, std::initializer_list<Arg> args) const {
        return format_to(out, key, std::span<const Arg>(args.begin(), args.size()));
    }
    FormatResult format_to(std::span<char> out, std::string_view key, std::initializer_list<Arg> args) const {
        return format_to(out, key, std::span<const Arg>(args.begin(), args.size()));
    }
    [[nodiscard]] std::string format(std::string_view key, std::initializer_list<Arg> args) const {
        return format(key, std::span<const Arg>(args.begin(), args.size()));
    }

    // ---- chain ---------------------------------------------------------------------------

    /// Replaces the fallback chain; returns how many of the given languages have a table. Unknown
    /// languages are skipped silently (the chain "ru, en" works before an English table exists).
    std::size_t set_chain(std::span<const std::string_view> languages);
    [[nodiscard]] std::vector<std::string> chain() const;
    [[nodiscard]] const std::string& default_language() const noexcept;

    // ---- queries -------------------------------------------------------------------------

    /// Languages that have a table, sorted by code.
    [[nodiscard]] std::vector<std::string> languages() const;
    [[nodiscard]] bool has_language(std::string_view language) const;
    /// All keys of one language, sorted (empty for an unknown language).
    [[nodiscard]] std::vector<std::string> keys(std::string_view language) const;
    [[nodiscard]] std::size_t key_count(std::string_view language) const;
    /// True when the language itself (without the chain) has the key.
    [[nodiscard]] bool contains(std::string_view language, std::string_view key) const;
    /// True when some language of the chain has the key.
    [[nodiscard]] bool resolves(std::string_view key) const;
    [[nodiscard]] std::optional<KeyOrigin> origin(std::string_view language, std::string_view key) const;
    /// Names of the parameters the message uses (sorted); empty for an unknown key.
    [[nodiscard]] std::vector<std::string> parameters(std::string_view language, std::string_view key) const;

    // ---- missing keys (data for `contentlint`) ---------------------------------------------

    /// Keys present in `reference` but absent in `language` (sorted).
    [[nodiscard]] std::vector<std::string> missing_keys(std::string_view language,
                                                        std::string_view reference) const;
    /// Coverage of every language except the default one against the default one, sorted by code.
    [[nodiscard]] std::vector<LanguageCoverage> coverage() const;
    /// Of `used` (keys referenced by code or data) those that `language` does not define (sorted,
    /// without duplicates).
    [[nodiscard]] std::vector<std::string> absent_keys(std::string_view language,
                                                       std::span<const std::string_view> used) const;
    /// Keys `language` defines that are not in `used` (sorted).
    [[nodiscard]] std::vector<std::string> unused_keys(std::string_view language,
                                                       std::span<const std::string_view> used) const;
    /// Keys that were requested at run time and found in no language of the chain (sorted).
    [[nodiscard]] std::vector<std::string> requested_missing_keys() const;
    /// Forgets which missing keys were already logged and recorded.
    void reset_missing_log();

    /// Implementation detail (opaque outside localizer.cpp).
    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
    friend class LocalizerBuilder;
};

/// The outcome of loading: the Localizer with everything that was valid, plus ALL problems found.
struct LocLoadResult {
    Localizer localizer;
    std::vector<LocError> errors;  // sorted by (root, file, position, code, message)

    [[nodiscard]] bool ok() const noexcept { return errors.empty(); }
};

/// Loads the tables of an ordered list of roots (roots[0] is the base game, later roots are mods).
/// Per root, every file `loc/<language>/**/*.pd` is a table of that language. Within a root a key may
/// be defined once per language (LC003); a later root replaces single keys of an earlier one. Files
/// are visited in bytewise path order, so the result never depends on the order a source lists them.
[[nodiscard]] LocLoadResult load_localization(std::span<const data::ContentSource* const> roots,
                                              const LoadOptions& options = {});

/// Convenience for a single root.
[[nodiscard]] LocLoadResult load_localization(const data::ContentSource& root,
                                              const LoadOptions& options = {});

}  // namespace pat::loc
