#pragma once

#include "pat/core/string_interner.hpp"
#include "pat/data/schema.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Typed tables of loaded content (E4-T04, docs/dev/data-loader.md).
//
// The registry holds the EFFECTIVE content: for every (kind, id) the definition from the last root
// that provides it. Determinism: tables are ordered by kind name, objects inside a table by id; both
// orders are bytewise (unsigned UTF-8) comparisons, independent of file names, file order and load
// order. Ids are interned after all overrides are decided, in (kind, id) order, so StringId values
// depend only on the effective content.

namespace pat::data {

/// One effective definition.
struct ContentObject {
    core::StringId id;       // interned object id (valid once the object is in a registry)
    std::uint32_t root = 0;  // index of the root that provided this definition
    std::string file;        // path relative to that root, '/' separators (diagnostics only)
    TypedObject object;      // kind, id string, source position and typed body
};

/// All effective objects of one kind, sorted by id.
class ContentTable {
public:
    ContentTable() = default;
    explicit ContentTable(std::string kind) : kind_(std::move(kind)) {}

    [[nodiscard]] const std::string& kind() const noexcept { return kind_; }
    [[nodiscard]] std::span<const ContentObject> objects() const noexcept { return objects_; }
    [[nodiscard]] std::size_t size() const noexcept { return objects_.size(); }
    [[nodiscard]] bool empty() const noexcept { return objects_.empty(); }

    /// The object with this id, or nullptr (binary search).
    [[nodiscard]] const ContentObject* find(std::string_view id) const noexcept;
    /// The object with this interned id (from ContentRegistry::ids()), or nullptr.
    [[nodiscard]] const ContentObject* find(core::StringId id,
                                            const core::StringInterner& ids) const noexcept;

private:
    friend class ContentRegistry;

    std::string kind_;
    std::vector<ContentObject> objects_;  // sorted by object.id
};

/// The result of a successful (or partially successful) load. Move-only.
class ContentRegistry {
public:
    ContentRegistry() = default;

    /// Builds a registry from effective objects given in ANY order (each (kind, id) at most once).
    /// `kinds` lists kinds that must have a table even when empty. Tables are sorted by kind, objects
    /// by id, ids are interned in that order and the content hash is computed.
    [[nodiscard]] static ContentRegistry build(std::vector<std::string> root_names,
                                               const std::vector<std::string>& kinds,
                                               std::vector<ContentObject> objects);

    [[nodiscard]] std::span<const ContentTable> tables() const noexcept { return tables_; }
    /// The table of a kind, or nullptr when the kind is unknown. O(log n).
    [[nodiscard]] const ContentTable* table(std::string_view kind) const noexcept;
    /// Shortcut for table(kind)->find(id).
    [[nodiscard]] const ContentObject* find(std::string_view kind, std::string_view id) const noexcept;

    /// Total number of effective objects.
    [[nodiscard]] std::size_t object_count() const noexcept;

    [[nodiscard]] const core::StringInterner& ids() const noexcept { return ids_; }

    [[nodiscard]] std::size_t root_count() const noexcept { return root_names_.size(); }
    [[nodiscard]] const std::string& root_name(std::size_t index) const { return root_names_.at(index); }

    /// 64-bit hash of the effective content (docs/dev/data-loader.md section 5): it depends only on
    /// the kinds, ids and typed values of the effective objects, not on file names, source positions,
    /// comments, formatting or on which root provided an object. Meant for save files (a save made
    /// with other content is detected by comparing it). Computed also when loading reported errors,
    /// but only meaningful for an error-free load.
    [[nodiscard]] std::uint64_t content_hash() const noexcept { return content_hash_; }

private:
    std::vector<std::string> root_names_;
    std::vector<ContentTable> tables_;  // sorted by kind
    core::StringInterner ids_;
    std::uint64_t content_hash_ = 0;
};

}  // namespace pat::data
