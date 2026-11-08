#include "pat/data/content_loader.hpp"

#include <algorithm>
#include <format>
#include <iterator>
#include <map>
#include <set>
#include <tuple>
#include <utility>

namespace pat::data {

// ---- MigrationRegistry -----------------------------------------------------------------------

MigrationRegistry& MigrationRegistry::add(std::int64_t from, MigrationStep step) {
    steps_[from] = std::move(step);
    return *this;
}

bool MigrationRegistry::has(std::int64_t from) const {
    return steps_.contains(from);
}

const MigrationStep* MigrationRegistry::find(std::int64_t from) const {
    const auto it = steps_.find(from);
    return it == steps_.end() ? nullptr : &it->second;
}

// ---- LoadError -------------------------------------------------------------------------------

std::string LoadError::format() const {
    const std::string prefix = std::format("[root {} '{}'] ", root, root_name);
    if (file.empty())
        return std::format("{}error[{}]: {}", prefix, error_code_name(diagnostic.code), diagnostic.message);
    if (diagnostic.pos.line == 0)
        return std::format(
            "{}{}: error[{}]: {}", prefix, file, error_code_name(diagnostic.code), diagnostic.message);
    return prefix + format_diagnostic(file, diagnostic);
}

namespace {

using Key = std::pair<std::string, std::string>;  // (kind, id)

// Files below the top-level directory `loc/` are localisation tables (`key = "text"`, E4-T06,
// docs/dev/localization.md); they are read by pat::loc, not by the content loader, and are not part
// of the content hash.
bool is_pd_file(const std::string& path) {
    return path.size() > 3 && path.ends_with(".pd") && path[path.size() - 4] != '/' &&
           !path.starts_with("loc/");
}

class Loader {
public:
    Loader(std::span<const ContentSource* const> roots, const SchemaSet& schemas, const LoadOptions& options)
        : roots_(roots), schemas_(schemas), options_(options) {}

    LoadResult run() {
        for (const ContentSource* root : roots_)
            root_names_.push_back(root->name());
        for (std::size_t r = 0; r < roots_.size(); ++r)
            load_root(static_cast<std::uint32_t>(r));

        std::vector<std::string> kinds;
        for (const ObjectSchema& s : schemas_.schemas())
            kinds.push_back(s.kind());
        std::vector<ContentObject> objects;
        objects.reserve(effective_.size());
        for (auto& [key, object] : effective_)
            objects.push_back(std::move(object));

        LoadResult result;
        result.registry = ContentRegistry::build(root_names_, kinds, std::move(objects));
        resolve_references(result.registry);

        result.errors = std::move(errors_);
        std::sort(result.errors.begin(), result.errors.end(), [](const LoadError& a, const LoadError& b) {
            return std::tie(a.root, a.file, a.diagnostic.pos, a.diagnostic.code, a.diagnostic.message) <
                   std::tie(b.root, b.file, b.diagnostic.pos, b.diagnostic.code, b.diagnostic.message);
        });
        return result;
    }

private:
    void
    report(std::uint32_t root, const std::string& file, SourcePos pos, ErrorCode code, std::string text) {
        errors_.push_back(LoadError{root, root_names_[root], file, Diagnostic{pos, code, std::move(text)}});
    }

    void load_root(std::uint32_t root) {
        const ContentSource& source = *roots_[root];
        auto listed = source.list_files();
        if (!listed) {
            report(
                root, {}, {}, ErrorCode::UnreadableSource, "cannot read the content root: " + listed.error());
            return;
        }
        std::vector<std::string> files;
        for (std::string& path : listed.value()) {
            if (is_pd_file(path))
                files.push_back(std::move(path));
        }
        // Bytewise (unsigned) order of the full relative path; independent of how the source lists.
        std::sort(files.begin(), files.end());
        files.erase(std::unique(files.begin(), files.end()), files.end());

        std::map<Key, std::pair<std::string, SourcePos>> seen;  // first definition of each id in this root
        for (const std::string& path : files)
            load_file(root, source, path, seen);
    }

    void load_file(std::uint32_t root,
                   const ContentSource& source,
                   const std::string& path,
                   std::map<Key, std::pair<std::string, SourcePos>>& seen) {
        auto text = source.read_file(path);
        if (!text) {
            report(root, path, {}, ErrorCode::UnreadableSource, "cannot read the file: " + text.error());
            return;
        }
        ParseResult parsed = parse_pd(text.value());
        std::set<SourcePos> tainted = entries_with_syntax_errors(parsed);
        for (Diagnostic& d : parsed.errors)
            report(root, path, d.pos, d.code, std::move(d.message));
        if (parsed.file.format_version == 0)
            return;  // PD020 was reported by the parser; nothing sensible to validate
        if (!check_version(root, path, parsed.file))
            return;
        drop_tainted_entries(parsed.file, tainted);

        FileResult validated = validate_file(schemas_, parsed.file);
        for (Diagnostic& d : validated.errors)
            report(root, path, d.pos, d.code, std::move(d.message));

        // Objects that failed validation still "exist": remember their ids so that references to
        // them do not produce a second, misleading error.
        std::set<SourcePos> valid_positions;
        for (const TypedObject& o : validated.objects)
            valid_positions.insert(o.pos);
        for (const Entry& e : parsed.file.entries) {
            if (e.key_kind == ValueKind::Ident && e.label && schemas_.find(e.key) != nullptr &&
                !valid_positions.contains(e.pos)) {
                poisoned_.insert(Key{e.key, *e.label});
            }
        }

        for (TypedObject& object : validated.objects) {
            Key key{object.kind, object.id};
            const auto [it, inserted] = seen.try_emplace(key, path, object.pos);
            if (!inserted) {
                report(root,
                       path,
                       object.pos,
                       ErrorCode::DuplicateObject,
                       std::format("duplicate {} '{}' in the same root (first defined at {}:{}:{})",
                                   object.kind,
                                   object.id,
                                   it->second.first,
                                   it->second.second.line,
                                   it->second.second.column));
                continue;
            }
            ContentObject entry;
            entry.root = root;
            entry.file = path;
            entry.object = std::move(object);
            effective_.insert_or_assign(std::move(key), std::move(entry));
        }
    }

    /// Top-level entries that contain a syntax error. The parser recovers by dropping the broken inner
    /// entry and keeping the surrounding object, so validating it would only add misleading
    /// "missing field" errors: such objects are left out (and their ids are remembered as broken).
    static std::set<SourcePos> entries_with_syntax_errors(const ParseResult& parsed) {
        std::set<SourcePos> result;
        const std::vector<Entry>& entries = parsed.file.entries;
        for (const Diagnostic& d : parsed.errors) {
            // The owner of an error is the last entry that starts at or before it.
            const auto next = std::upper_bound(
                entries.begin(), entries.end(), d.pos, [](SourcePos p, const Entry& e) { return p < e.pos; });
            if (next != entries.begin())
                result.insert(std::prev(next)->pos);
        }
        return result;
    }

    void drop_tainted_entries(ParsedFile& file, const std::set<SourcePos>& tainted) {
        if (tainted.empty())
            return;
        std::vector<Entry> kept;
        for (Entry& e : file.entries) {
            if (!tainted.contains(e.pos)) {
                kept.push_back(std::move(e));
            } else if (e.key_kind == ValueKind::Ident && e.label && schemas_.find(e.key) != nullptr) {
                poisoned_.insert(Key{e.key, *e.label});
            }
        }
        file.entries = std::move(kept);
    }

    /// Checks the header version and migrates the tree to the current version.
    bool check_version(std::uint32_t root, const std::string& path, ParsedFile& file) {
        const SourcePos where = file.entries.empty() ? SourcePos{1, 1} : file.entries.front().value.pos;
        const std::int64_t current = options_.current_version;
        if (file.format_version > current) {
            report(root,
                   path,
                   where,
                   ErrorCode::UnsupportedVersion,
                   std::format("unsupported format_version {} (this build reads versions up to {})",
                               file.format_version,
                               current));
            return false;
        }
        while (file.format_version < current) {
            const MigrationStep* step = options_.migrations.find(file.format_version);
            if (step == nullptr) {
                report(
                    root,
                    path,
                    where,
                    ErrorCode::UnsupportedVersion,
                    std::format(
                        "unsupported format_version {}: no migration from version {} to {} (current is {})",
                        file.format_version,
                        file.format_version,
                        file.format_version + 1,
                        current));
                return false;
            }
            if (std::optional<std::string> failure = (*step)(file)) {
                report(root,
                       path,
                       where,
                       ErrorCode::MigrationFailed,
                       std::format(
                           "migration from format_version {} failed: {}", file.format_version, *failure));
                return false;
            }
            ++file.format_version;
        }
        return true;
    }

    // ---- references ----------------------------------------------------------------------------

    void check_value(const ContentObject& owner, const ContentRegistry& registry, const TypedValue& value) {
        if (const auto* ref = value.get_if<RefValue>()) {
            check_reference(owner, registry, *ref, value.pos);
        } else if (const auto* list = value.get_if<TypedList>()) {
            for (const TypedValue& item : *list)
                check_value(owner, registry, item);
        } else if (const auto* block = value.get_if<TypedBlock>()) {
            for (const TypedField& f : block->fields)
                check_value(owner, registry, f.value);
        } else if (const auto* map = value.get_if<TypedMap>()) {
            for (const TypedField& f : map->entries)
                check_value(owner, registry, f.value);
        }
    }

    void check_reference(const ContentObject& owner,
                         const ContentRegistry& registry,
                         const RefValue& ref,
                         SourcePos pos) {
        if (registry.find(ref.kind, ref.id) != nullptr)
            return;
        if (poisoned_.contains(Key{ref.kind, ref.id}))
            return;
        const ContentTable* table = registry.table(ref.kind);
        std::string text;
        if (table == nullptr || table->empty()) {
            text = std::format("unresolved reference: there is no {} '{}' (no '{}' objects are defined)",
                               ref.kind,
                               ref.id,
                               ref.kind);
        } else {
            std::vector<std::string> ids;
            ids.reserve(table->size());
            for (const ContentObject& o : table->objects())
                ids.push_back(o.object.id);
            const std::string hint = suggest_similar(ref.id, ids);
            text = std::format("unresolved reference: there is no {} '{}'", ref.kind, ref.id);
            if (!hint.empty())
                text += std::format(" (did you mean '{}'?)", hint);
        }
        report(owner.root, owner.file, pos, ErrorCode::UnresolvedReference, std::move(text));
    }

    void resolve_references(const ContentRegistry& registry) {
        for (const ContentTable& table : registry.tables()) {
            for (const ContentObject& object : table.objects()) {
                for (const TypedField& f : object.object.body.fields)
                    check_value(object, registry, f.value);
            }
        }
    }

    std::span<const ContentSource* const> roots_;
    const SchemaSet& schemas_;
    const LoadOptions& options_;
    std::map<Key, ContentObject> effective_;
    std::set<Key> poisoned_;
    std::vector<std::string> root_names_;
    std::vector<LoadError> errors_;
};

}  // namespace

LoadResult load_content(std::span<const ContentSource* const> roots,
                        const SchemaSet& schemas,
                        const LoadOptions& options) {
    return Loader(roots, schemas, options).run();
}

LoadResult load_content(const ContentSource& root, const SchemaSet& schemas, const LoadOptions& options) {
    const ContentSource* roots[] = {&root};
    return load_content(std::span<const ContentSource* const>(roots), schemas, options);
}

}  // namespace pat::data
