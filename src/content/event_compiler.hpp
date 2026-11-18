#pragma once

#include "pat/data/content_registry.hpp"
#include "pat/data/pd_tree.hpp"
#include "pat/sim/events/catalog.hpp"
#include "pat/sim/events/event_set.hpp"
#include "pat/sim/events/program.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

// The compiler of the rule blocks of event files (E11-T06, docs/dev/event-engine.md sections 4 .. 7, 16):
// a block of `key operator value` entries (the Type::rules() value of the schema) becomes a
// pat::sim::events::Program. The compiler resolves every identifier through the Catalog and the
// content registry, types the scopes statically (a trigger of a province in a character scope is
// EV102), and reports ALL problems with `line:column` positions instead of stopping at the first.
//
// Internal to pat::content: the public entry is build_event_set() (event_content.hpp).

namespace pat::content::events {

/// One problem found while compiling (position in the file, an EV code and the text).
struct Diag {
    data::SourcePos pos;
    data::ErrorCode code = data::ErrorCode::EventBadField;
    std::string message;
};

/// The 64-bit keys of the names used by the rules: flags, variables, pools, groups, events, modifiers.
/// Two different names with one key are a collision (EV105).
class NameTable {
public:
    using KeyFn = std::function<std::uint64_t(std::string_view)>;

    explicit NameTable(KeyFn fn = nullptr);

    /// Records `name` and returns its key. When another name already owns the key, `clash` is set to it.
    [[nodiscard]] std::uint64_t add(std::string_view name, std::string* clash = nullptr);
    [[nodiscard]] std::uint64_t key_of(std::string_view name) const;
    [[nodiscard]] const std::map<std::uint64_t, std::string>& names() const noexcept { return names_; }

private:
    KeyFn fn_;
    std::map<std::uint64_t, std::string> names_;
};

/// A saved scope of the event (`scope:name`): the slot is its index.
struct SavedScope {
    std::string name;
    sim::events::ScopeKind kind = sim::events::ScopeKind::None;
};
struct SavedScopes {
    std::vector<SavedScope> slots;
};

/// What every program of the build shares.
struct CompileShared {
    const data::ContentRegistry* registry = nullptr;
    const sim::events::Catalog* catalog = nullptr;
    const sim::events::EventSettings* settings = nullptr;
    NameTable* names = nullptr;
    const std::set<std::string, std::less<>>* event_ids = nullptr;
    const std::set<std::string, std::less<>>* modifier_ids = nullptr;
    const std::set<std::string, std::less<>>* pools = nullptr;
    /// Output: the ids of the events named by `trigger_event`-like arguments (chain targets).
    std::set<std::string, std::less<>>* event_refs = nullptr;
};

/// Compiles the blocks of ONE event (a new instance per program). `root` is the scope kind of the
/// event; `saved` the saved scopes known when the program starts (the ones the on-action provides plus
/// those `immediate` saved). The compiler adds the names the program saves itself; saved() returns all.
class RulesCompiler {
public:
    RulesCompiler(const CompileShared& shared,
                  std::vector<Diag>& diags,
                  std::uint32_t file,
                  sim::events::ScopeKind root,
                  SavedScopes saved);
    ~RulesCompiler();
    RulesCompiler(const RulesCompiler&) = delete;
    RulesCompiler& operator=(const RulesCompiler&) = delete;

    /// A trigger block (AND of its entries) -> a program that leaves a boolean.
    [[nodiscard]] std::optional<sim::events::Program> trigger(const data::Value& block);
    /// A number or an accumulator block -> a program that leaves a Fixed64. `step_base` is the family
    /// of Breakdown codes (docs/dev/event-engine.md section 8.5): the n-th step is step_base + n.
    [[nodiscard]] std::optional<sim::events::Program> accumulator(const data::Value& value,
                                                                  std::uint16_t step_base);
    /// An effect block -> a program that emits commands and state operations.
    [[nodiscard]] std::optional<sim::events::Program> effect(const data::Value& block);
    /// A path to a scope (`scope:name`, `root.owner`) -> a program that leaves the scope word
    /// (the `speaker` of an event); an empty transition leaves 0.
    [[nodiscard]] std::optional<sim::events::Program> scope_value(const data::Value& value);

    [[nodiscard]] const SavedScopes& saved() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

/// Parses a duration block `{ days = 3  months = 2  years = 1 }` (or a plain number of days) into days
/// with the month and year lengths of `settings`. Returns nullopt after reporting to `diags`.
[[nodiscard]] std::optional<std::int64_t> parse_duration(const data::Value& value,
                                                         const sim::events::EventSettings& settings,
                                                         std::vector<Diag>& diags);

/// The Fixed64 of an Int or Decimal value; nullopt when it is another kind or out of range (not reported).
[[nodiscard]] std::optional<core::Fixed64> fixed_of(const data::Value& value);

}  // namespace pat::content::events
