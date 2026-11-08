#include "pat/loc/localizer.hpp"

#include "pat/core/log.hpp"
#include "pat/data/pd_parser.hpp"

#include <algorithm>
#include <format>
#include <map>
#include <mutex>
#include <set>
#include <tuple>
#include <utility>

namespace pat::loc {

namespace {

constexpr std::string_view kLogCategory = "loc";

}  // namespace

// ---- Localizer state -------------------------------------------------------------------------

struct Localizer::Impl {
    struct Entry {
        std::string key;
        CompiledMessage message;
        std::uint32_t root = 0;
        std::uint32_t file = 0;  // index in `files`
        data::SourcePos pos;
    };
    struct Table {
        LanguageInfo info;
        std::vector<Entry> entries;  // sorted by key (bytewise)
    };

    std::vector<Table> tables;  // sorted by language code
    std::vector<std::string> files;
    std::string default_language;
    std::vector<std::string> chain_names;
    std::vector<std::size_t> chain;  // indices into `tables`

    // Bookkeeping of keys that were looked up and not found; only touched on that cold path.
    mutable std::mutex mutex;
    mutable std::set<std::string, std::less<>> logged;
    mutable std::set<std::string, std::less<>> requested_missing;

    [[nodiscard]] const Table* find_table(std::string_view language) const {
        const auto it =
            std::lower_bound(tables.begin(), tables.end(), language, [](const Table& t, std::string_view l) {
                return t.info.code < l;
            });
        return it != tables.end() && it->info.code == language ? &*it : nullptr;
    }

    [[nodiscard]] static const Entry* find_entry(const Table& table, std::string_view key) {
        const auto it = std::lower_bound(
            table.entries.begin(), table.entries.end(), key, [](const Entry& e, std::string_view k) {
                return std::string_view(e.key) < k;
            });
        return it != table.entries.end() && it->key == key ? &*it : nullptr;
    }

    void note_missing_key(std::string_view key) const {
        const std::scoped_lock lock(mutex);
        if (requested_missing.find(key) == requested_missing.end())
            requested_missing.emplace(key);
        const std::string tag = "key:" + std::string(key);
        if (!logged.insert(tag).second)
            return;
        std::string languages;
        for (const std::string& name : chain_names)
            languages += (languages.empty() ? "" : ", ") + name;
        PAT_LOG_WARN(kLogCategory, "missing localisation key '{}' (language chain: {})", key, languages);
    }

    void note_bad_argument(std::string_view key) const {
        const std::scoped_lock lock(mutex);
        const std::string tag = "arg:" + std::string(key);
        if (!logged.insert(tag).second)
            return;
        PAT_LOG_WARN(kLogCategory, "localisation message '{}' got a missing or mistyped argument", key);
    }
};

Localizer::Localizer() : impl_(std::make_unique<Impl>()) {}
Localizer::~Localizer() = default;
Localizer::Localizer(Localizer&&) noexcept = default;
Localizer& Localizer::operator=(Localizer&&) noexcept = default;

// ---- formatting ------------------------------------------------------------------------------

namespace {

FormatResult
format_impl(const Localizer::Impl& impl, Output& out, std::string_view key, std::span<const Arg> args) {
    FormatResult result;
    for (const std::size_t index : impl.chain) {
        const Localizer::Impl::Entry* entry = Localizer::Impl::find_entry(impl.tables[index], key);
        if (entry == nullptr)
            continue;
        entry->message.format(out, args, result);
        if (result.bad_argument)
            impl.note_bad_argument(key);
        result.size = out.size();
        result.truncated = out.truncated();
        return result;
    }
    result.missing_key = true;
    out.append("[[");
    out.append(key);
    out.append("]]");
    impl.note_missing_key(key);
    result.size = out.size();
    result.truncated = out.truncated();
    return result;
}

}  // namespace

FormatResult Localizer::format_to(std::string& out, std::string_view key, std::span<const Arg> args) const {
    Output output(out);
    return format_impl(*impl_, output, key, args);
}

FormatResult
Localizer::format_to(std::span<char> out, std::string_view key, std::span<const Arg> args) const {
    Output output(out);
    return format_impl(*impl_, output, key, args);
}

std::string Localizer::format(std::string_view key, std::span<const Arg> args) const {
    std::string out;
    (void)format_to(out, key, args);
    return out;
}

// ---- chain and queries -----------------------------------------------------------------------

std::size_t Localizer::set_chain(std::span<const std::string_view> languages) {
    impl_->chain.clear();
    impl_->chain_names.clear();
    for (const std::string_view language : languages) {
        impl_->chain_names.emplace_back(language);
        const auto it = std::find_if(impl_->tables.begin(),
                                     impl_->tables.end(),
                                     [language](const Impl::Table& t) { return t.info.code == language; });
        if (it != impl_->tables.end())
            impl_->chain.push_back(static_cast<std::size_t>(it - impl_->tables.begin()));
    }
    return impl_->chain.size();
}

std::vector<std::string> Localizer::chain() const {
    return impl_->chain_names;
}

const std::string& Localizer::default_language() const noexcept {
    return impl_->default_language;
}

std::vector<std::string> Localizer::languages() const {
    std::vector<std::string> out;
    for (const Impl::Table& table : impl_->tables)
        out.push_back(table.info.code);
    return out;
}

bool Localizer::has_language(std::string_view language) const {
    return impl_->find_table(language) != nullptr;
}

std::vector<std::string> Localizer::keys(std::string_view language) const {
    std::vector<std::string> out;
    if (const Impl::Table* table = impl_->find_table(language)) {
        out.reserve(table->entries.size());
        for (const Impl::Entry& entry : table->entries)
            out.push_back(entry.key);
    }
    return out;
}

std::size_t Localizer::key_count(std::string_view language) const {
    const Impl::Table* table = impl_->find_table(language);
    return table != nullptr ? table->entries.size() : 0;
}

bool Localizer::contains(std::string_view language, std::string_view key) const {
    const Impl::Table* table = impl_->find_table(language);
    return table != nullptr && Impl::find_entry(*table, key) != nullptr;
}

bool Localizer::resolves(std::string_view key) const {
    return std::any_of(impl_->chain.begin(), impl_->chain.end(), [this, key](std::size_t index) {
        return Impl::find_entry(impl_->tables[index], key) != nullptr;
    });
}

std::optional<KeyOrigin> Localizer::origin(std::string_view language, std::string_view key) const {
    const Impl::Table* table = impl_->find_table(language);
    if (table == nullptr)
        return std::nullopt;
    const Impl::Entry* entry = Impl::find_entry(*table, key);
    if (entry == nullptr)
        return std::nullopt;
    return KeyOrigin{entry->root, impl_->files[entry->file], entry->pos};
}

std::vector<std::string> Localizer::parameters(std::string_view language, std::string_view key) const {
    const Impl::Table* table = impl_->find_table(language);
    if (table == nullptr)
        return {};
    const Impl::Entry* entry = Impl::find_entry(*table, key);
    return entry != nullptr ? entry->message.parameter_names() : std::vector<std::string>{};
}

// ---- missing keys ----------------------------------------------------------------------------

std::vector<std::string> Localizer::missing_keys(std::string_view language,
                                                 std::string_view reference) const {
    std::vector<std::string> out;
    const Impl::Table* ref = impl_->find_table(reference);
    if (ref == nullptr)
        return out;
    const Impl::Table* table = impl_->find_table(language);
    for (const Impl::Entry& entry : ref->entries) {
        if (table == nullptr || Impl::find_entry(*table, entry.key) == nullptr)
            out.push_back(entry.key);
    }
    return out;
}

std::vector<LanguageCoverage> Localizer::coverage() const {
    std::vector<LanguageCoverage> out;
    for (const Impl::Table& table : impl_->tables) {
        if (table.info.code == impl_->default_language)
            continue;
        LanguageCoverage cov;
        cov.language = table.info.code;
        cov.total = table.entries.size();
        cov.missing = missing_keys(table.info.code, impl_->default_language);
        cov.extra = missing_keys(impl_->default_language, table.info.code);
        out.push_back(std::move(cov));
    }
    return out;
}

std::vector<std::string> Localizer::absent_keys(std::string_view language,
                                                std::span<const std::string_view> used) const {
    std::set<std::string, std::less<>> absent;
    const Impl::Table* table = impl_->find_table(language);
    for (const std::string_view key : used) {
        if (table == nullptr || Impl::find_entry(*table, key) == nullptr)
            absent.emplace(key);
    }
    return {absent.begin(), absent.end()};
}

std::vector<std::string> Localizer::unused_keys(std::string_view language,
                                                std::span<const std::string_view> used) const {
    std::vector<std::string> out;
    const Impl::Table* table = impl_->find_table(language);
    if (table == nullptr)
        return out;
    const std::set<std::string_view> used_set(used.begin(), used.end());
    for (const Impl::Entry& entry : table->entries) {
        if (!used_set.contains(entry.key))
            out.push_back(entry.key);
    }
    return out;
}

std::vector<std::string> Localizer::requested_missing_keys() const {
    const std::scoped_lock lock(impl_->mutex);
    return {impl_->requested_missing.begin(), impl_->requested_missing.end()};
}

void Localizer::reset_missing_log() {
    const std::scoped_lock lock(impl_->mutex);
    impl_->logged.clear();
    impl_->requested_missing.clear();
}

// ---- LocError --------------------------------------------------------------------------------

std::string LocError::format() const {
    const std::string prefix = std::format("[root {} '{}'] ", root, root_name);
    if (file.empty())
        return std::format("{}error[{}]: {}", prefix, code, message);
    if (pos.line == 0)
        return std::format("{}{}: error[{}]: {}", prefix, file, code, message);
    return std::format("{}{}:{}:{}: error[{}]: {}", prefix, file, pos.line, pos.column, code, message);
}

// ---- loading ---------------------------------------------------------------------------------

namespace {

/// Maps a byte offset of a decoded string value back to a position in the .pd source: the position
/// of the character (or escape sequence) that produced that byte. `value_pos` is the position of the
/// opening quote. Only called when a message has an error, so clarity wins over speed.
data::SourcePos
locate_in_source(std::string_view source, data::SourcePos value_pos, std::size_t decoded_offset) {
    // Find the byte offset of value_pos (columns count code points; a BOM is not counted).
    std::size_t i = source.starts_with("\xEF\xBB\xBF") ? 3 : 0;
    data::SourcePos pos{1, 1};
    const auto step = [&source, &pos](std::size_t& at) {
        const char c = source[at++];
        if (c == '\n') {
            ++pos.line;
            pos.column = 1;
        } else if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) {
            ++pos.column;
        }
    };
    while (i < source.size() &&
           (pos.line < value_pos.line || (pos.line == value_pos.line && pos.column < value_pos.column)))
        step(i);
    if (i >= source.size())
        return value_pos;

    const auto utf8_length = [](std::uint32_t cp) -> std::size_t {
        return cp < 0x80 ? 1 : cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4;
    };
    std::size_t decoded = 0;
    const auto sequence_length = [&source](std::size_t at) -> std::size_t {
        std::size_t n = 1;
        while (at + n < source.size() && (static_cast<unsigned char>(source[at + n]) & 0xC0) == 0x80)
            ++n;
        return n;
    };

    if (source.substr(i, 3) == "\"\"\"") {
        step(i);
        step(i);
        step(i);
        if (source.substr(i, 2) == "\r\n") {
            step(i);
            step(i);
        } else if (i < source.size() && source[i] == '\n') {
            step(i);
        }
        while (i < source.size()) {
            if (source.substr(i, 3) == "\"\"\"")
                return pos;  // the closing quotes: the offset is at the end of the text
            if (source.substr(i, 2) == "\r\n") {
                step(i);  // CRLF is decoded as a single LF, attributed to the line break
                continue;
            }
            const std::size_t length = sequence_length(i);
            if (decoded_offset < decoded + length)
                return pos;
            decoded += length;
            for (std::size_t k = 0; k < length; ++k)
                step(i);
        }
        return pos;
    }

    step(i);  // opening quote
    while (i < source.size() && source[i] != '"' && source[i] != '\n') {
        const data::SourcePos start = pos;
        std::size_t length = 1;     // decoded bytes produced
        std::size_t raw_bytes = 1;  // raw bytes consumed
        if (source[i] == '\\' && i + 1 < source.size()) {
            const char e = source[i + 1];
            if (e == 'u' && i + 2 < source.size() && source[i + 2] == '{') {
                std::size_t k = i + 3;
                std::uint32_t cp = 0;
                while (k < source.size() && source[k] != '}') {
                    const char h = source[k];
                    cp = cp * 16 + static_cast<std::uint32_t>(h >= 'a'   ? h - 'a' + 10
                                                              : h >= 'A' ? h - 'A' + 10
                                                                         : h - '0');
                    ++k;
                }
                raw_bytes = k - i + 1;
                length = utf8_length(cp);
            } else {
                raw_bytes = 2;
            }
        } else {
            length = sequence_length(i);
            raw_bytes = length;
        }
        if (decoded_offset < decoded + length)
            return start;
        decoded += length;
        for (std::size_t k = 0; k < raw_bytes && i < source.size(); ++k)
            step(i);
    }
    return pos;
}

struct Pending {
    CompiledMessage message;
    std::uint32_t root = 0;
    std::uint32_t file = 0;
    data::SourcePos pos;
};

using KeyMap = std::map<std::string, Pending, std::less<>>;

}  // namespace

class LocalizerBuilder {
public:
    static LocLoadResult run(std::span<const data::ContentSource* const> roots, const LoadOptions& options);
};

namespace {

class Loader {
public:
    Loader(std::span<const data::ContentSource* const> roots, const LoadOptions& options)
        : roots_(roots), options_(options) {}

    void load_all() {
        for (std::size_t r = 0; r < roots_.size(); ++r)
            load_root(static_cast<std::uint32_t>(r));
    }

    std::vector<LocError> errors;
    std::map<std::string, KeyMap> merged;  // language -> key -> effective message
    std::vector<std::string> files;        // every file that supplied a message (index = Pending::file)

private:
    std::span<const data::ContentSource* const> roots_;
    const LoadOptions& options_;
    std::string root_name_;
    std::set<std::string> warned_languages_;

    void report(std::uint32_t root,
                const std::string& file,
                data::SourcePos pos,
                std::string code,
                std::string message) {
        errors.push_back(LocError{root, root_name_, file, pos, std::move(code), std::move(message)});
    }

    [[nodiscard]] const LanguageInfo* find_language(std::string_view code) const {
        for (const LanguageInfo& info : options_.languages) {
            if (info.code == code)
                return &info;
        }
        return nullptr;
    }

    void load_root(std::uint32_t root) {
        const data::ContentSource& source = *roots_[root];
        root_name_ = source.name();
        auto listed = source.list_files();
        if (!listed.has_value()) {
            report(root, "", {}, "PD113", "cannot list the root: " + listed.error());
            return;
        }
        std::vector<std::string> paths = std::move(listed).value();
        std::sort(paths.begin(), paths.end());

        std::map<std::string, std::map<std::string, std::pair<std::string, data::SourcePos>, std::less<>>>
            defined;  // language -> key -> first definition
        std::map<std::string, KeyMap> root_keys;

        for (const std::string& path : paths) {
            if (!path.starts_with("loc/") || !path.ends_with(".pd") || path.size() <= 7)
                continue;
            const std::size_t slash = path.find('/', 4);
            if (slash == std::string::npos) {
                report(root,
                       path,
                       {},
                       "LC001",
                       "a table file must lie in a language directory: loc/<language>/...");
                continue;
            }
            const std::string language = path.substr(4, slash - 4);
            if (slash + 1 >= path.size() - 3)
                continue;  // ".pd" directly after the slash: a hidden file name, not a table
            const LanguageInfo* info = is_valid_language_code(language) ? find_language(language) : nullptr;
            if (info == nullptr) {
                if (warned_languages_.insert(std::to_string(root) + "/" + language).second)
                    report(root,
                           path,
                           {},
                           "LC002",
                           std::format(
                               "unknown language '{}': add it to LoadOptions::languages with its plural rule",
                               language));
                continue;
            }
            load_file(root, source, path, *info, defined[language], root_keys[language]);
        }

        // Later roots replace single keys of earlier ones.
        for (auto& [language, keys] : root_keys) {
            KeyMap& target = merged[language];
            for (auto& [key, pending] : keys)
                target[key] = std::move(pending);
        }
    }

    void load_file(std::uint32_t root,
                   const data::ContentSource& source,
                   const std::string& path,
                   const LanguageInfo& info,
                   std::map<std::string, std::pair<std::string, data::SourcePos>, std::less<>>& defined,
                   KeyMap& keys) {
        auto text = source.read_file(path);
        if (!text.has_value()) {
            report(root, path, {}, "PD113", "cannot read the file: " + text.error());
            return;
        }
        const std::string content = std::move(text).value();
        data::ParseResult parsed = data::parse_pd(content);
        if (!parsed.ok()) {
            for (const data::Diagnostic& d : parsed.errors)
                report(root, path, d.pos, data::error_code_name(d.code), d.message);
            return;
        }
        if (parsed.file.format_version != data::kCurrentFormatVersion) {
            const data::Entry& header = parsed.file.entries.front();
            report(root,
                   path,
                   header.value.pos,
                   "PD021",
                   std::format("unsupported format_version {} (this build reads {})",
                               parsed.file.format_version,
                               data::kCurrentFormatVersion));
            return;
        }

        files.push_back(path);
        const auto file_index = static_cast<std::uint32_t>(files.size() - 1);

        for (std::size_t e = 1; e < parsed.file.entries.size(); ++e) {
            const data::Entry& entry = parsed.file.entries[e];
            if (entry.key_kind != data::ValueKind::Ident || entry.key == "format_version" ||
                entry.label.has_value() || entry.op != data::Op::Eq) {
                report(root,
                       path,
                       entry.pos,
                       "LC004",
                       std::format("expected `key = \"text\"` with a dotted identifier key, got key '{}'",
                                   entry.key));
                continue;
            }
            if (entry.value.kind != data::ValueKind::String) {
                report(root,
                       path,
                       entry.value.pos,
                       "LC004",
                       std::format("the value of '{}' must be a string, got {}",
                                   entry.key,
                                   data::value_kind_name(entry.value.kind)));
                continue;
            }
            const auto first = defined.find(entry.key);
            if (first != defined.end()) {
                report(root,
                       path,
                       entry.pos,
                       "LC003",
                       std::format("duplicate key '{}' (first defined at {}:{}:{})",
                                   entry.key,
                                   first->second.first,
                                   first->second.second.line,
                                   first->second.second.column));
                continue;
            }
            defined.emplace(entry.key, std::make_pair(path, entry.pos));

            auto compiled = CompiledMessage::compile(entry.value.text, info.rule);
            if (!compiled.has_value()) {
                const MessageError& err = compiled.error();
                const data::SourcePos at = locate_in_source(content, entry.value.pos, err.offset);
                report(root,
                       path,
                       at,
                       message_error_code_name(err.code),
                       std::format("message '{}': {}", entry.key, err.message));
                continue;
            }
            keys[entry.key] = Pending{std::move(compiled).value(), root, file_index, entry.pos};
        }
    }
};

}  // namespace

LocLoadResult LocalizerBuilder::run(std::span<const data::ContentSource* const> roots,
                                    const LoadOptions& options) {
    // Global file indices: every root's files are appended, Pending::file refers to the running list.
    // Loader::files is filled in load order, so indices are stable within one Loader.
    Loader loader(roots, options);
    loader.load_all();

    LocLoadResult result;
    Localizer::Impl& impl = *result.localizer.impl_;
    impl.files = std::move(loader.files);
    impl.default_language = options.default_language;
    for (auto& [language, keys] : loader.merged) {
        const auto info = std::find_if(options.languages.begin(),
                                       options.languages.end(),
                                       [&language](const LanguageInfo& l) { return l.code == language; });
        Localizer::Impl::Table table;
        table.info = *info;
        table.entries.reserve(keys.size());
        for (auto& [key, pending] : keys) {
            Localizer::Impl::Entry entry;
            entry.key = key;
            entry.message = std::move(pending.message);
            entry.root = pending.root;
            entry.file = pending.file;
            entry.pos = pending.pos;
            table.entries.push_back(std::move(entry));
        }
        impl.tables.push_back(std::move(table));
    }

    std::vector<std::string_view> chain(options.chain.begin(), options.chain.end());
    (void)result.localizer.set_chain(chain);

    result.errors = std::move(loader.errors);
    std::sort(result.errors.begin(), result.errors.end(), [](const LocError& a, const LocError& b) {
        return std::tie(a.root, a.file, a.pos, a.code, a.message) <
               std::tie(b.root, b.file, b.pos, b.code, b.message);
    });
    return result;
}

LocLoadResult load_localization(std::span<const data::ContentSource* const> roots,
                                const LoadOptions& options) {
    return LocalizerBuilder::run(roots, options);
}

LocLoadResult load_localization(const data::ContentSource& root, const LoadOptions& options) {
    const data::ContentSource* roots[] = {&root};
    return LocalizerBuilder::run(roots, options);
}

}  // namespace pat::loc
