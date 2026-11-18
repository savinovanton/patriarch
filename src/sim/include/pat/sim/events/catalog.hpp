#pragma once

#include "pat/core/fixed.hpp"
#include "pat/sim/events/program.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <map>
#include <string>
#include <string_view>
#include <vector>

// Catalogs of the event engine (docs/dev/event-engine.md sections 5 and 8; E11-T06): the names the
// rule blocks of event files may use, with stable numeric codes, the scope kinds they apply to, and
// the signature of their arguments. The compiler (pat::content, E11-T06) resolves every identifier
// of a block through a Catalog and turns it into LOAD / PRED / EMIT / STATE / SCOPE_ENTER / ITER_BEGIN
// instructions; the host of the machine (E11-T05) gives the codes their meaning at run time.
//
// The engine owns the entries that belong to the language itself (the scope transitions of section
// 5.1, the lists of section 5.2 and the on-actions of section 8.1: Catalog::engine()). The entries of
// the trigger, value, effect and state catalogs are registered by their tasks (E11-T07 .. T11);
// tests register small ones. Codes never change and are never reused (section 8).
//
// Nothing here uses floating point; every listing (names(), entries()) is in registration order.

namespace pat::sim::events {

/// Bit mask of scope kinds (bit k = ScopeKind k). 0 means "any scope".
using ScopeMask = std::uint32_t;

[[nodiscard]] constexpr ScopeMask scope_bit(ScopeKind kind) noexcept {
    return ScopeMask{1} << static_cast<unsigned>(kind);
}
[[nodiscard]] constexpr ScopeMask scope_mask(std::initializer_list<ScopeKind> kinds) noexcept {
    ScopeMask mask = 0;
    for (const ScopeKind k : kinds) {
        mask |= scope_bit(k);
    }
    return mask;
}
/// True when a catalog entry with this `from` mask may be used in a scope of `kind`.
[[nodiscard]] constexpr bool scope_allowed(ScopeMask from, ScopeKind kind) noexcept {
    return from == 0 || (from & scope_bit(kind)) != 0;
}

/// "character", "dynasty", "province", "realm", ... (the words of event files and messages).
[[nodiscard]] std::string_view scope_kind_name(ScopeKind kind) noexcept;
/// Inverse of scope_kind_name; ScopeKind::None when the word is not a kind.
[[nodiscard]] ScopeKind scope_kind_from_name(std::string_view name) noexcept;

/// The static type of an expression: what the machine leaves on the stack.
enum class ValueType : std::uint8_t {
    Bool,    // 0 / 1
    Number,  // Fixed64 raw
    Scope,   // a packed Scope (PUSH_SCOPE)
};

/// How an argument of an entry is written in an event file and what the compiler pushes for it.
enum class ArgKind : std::uint8_t {
    Number,  // an expression: a literal, a path to a catalog value, or an accumulator block
    Bool,    // `yes` / `no`, pushed as 0 / 1
    Scope,   // a scope: `scope:name`, `root`, or a path of transitions; pushed with PUSH_SCOPE
    Name,    // a literal name that becomes a 64-bit key (flag, variable, chain); see `ref_kind`
    Ref,   // an object of a registry kind (`ref_kind`: `trait`, `building`, `office`), pushed as its interned
           // id
    Enum,  // an identifier of `enum_values`, pushed as its index
    Duration,  // `{ days = … months = … years = … }` or a plain number of days; pushed as days
    Date,      // `1703.05.16`; pushed as the day number of pat::sim::Date
};

struct ArgSpec {
    std::string name;
    ArgKind kind = ArgKind::Number;
    bool required = true;
    /// The value pushed when an optional argument is absent (a Fixed64; Enum: the index).
    core::Fixed64 default_value;
    /// Scope: the kind the argument must have (None: any).
    ScopeKind scope_kind = ScopeKind::None;
    /// Name: "" (a plain name), "event", "event_modifier" or "pool" (the name must exist, EV104).
    /// Ref: the registry kind.
    std::string ref_kind;
    /// Enum: the allowed identifiers.
    std::vector<std::string> enum_values;
};

/// Builders of the common argument shapes (keep the catalog tables short).
[[nodiscard]] ArgSpec arg_number(std::string name, bool required = true, core::Fixed64 def = {});
[[nodiscard]] ArgSpec arg_bool(std::string name, bool required = false, bool def = false);
[[nodiscard]] ArgSpec arg_scope(std::string name, ScopeKind kind = ScopeKind::None, bool required = true);
[[nodiscard]] ArgSpec arg_name(std::string name, std::string ref_kind = {}, bool required = true);
[[nodiscard]] ArgSpec arg_ref(std::string name, std::string registry_kind, bool required = true);
[[nodiscard]] ArgSpec
arg_enum(std::string name, std::vector<std::string> values, bool required = true, std::int32_t def = 0);
[[nodiscard]] ArgSpec arg_duration(std::string name, bool required = true);
[[nodiscard]] ArgSpec arg_date(std::string name, bool required = true);

struct CatalogEntry {
    NameSpace space = NameSpace::Predicate;
    std::uint16_t code = 0;
    std::string name;
    /// The scope kinds where the entry may be used (0: any). Transition and List: the kinds they start from.
    ScopeMask from = 0;
    /// Value and Predicate: what it yields. Predicates are Bool; a value may be a Number or a Scope.
    ValueType result = ValueType::Bool;
    /// Transition: the scope it leads to. List: the kind of the elements. Value with result Scope: its kind.
    ScopeKind to = ScopeKind::None;
    /// The arguments in the order the machine pops them. The first one is the "main" argument
    /// (`has_trait = greedy`, `change_honor = -5`).
    std::vector<ArgSpec> args;
    /// Value: a static suffix of the path, `skill.martial`: the index of the suffix is pushed first.
    std::vector<std::string> suffixes;
    /// List: an upper bound of the number of elements (the cost estimate and section 5.2).
    std::uint32_t bound = 32;
    /// Cost of a LOAD or PRED in work units (0: the default of the instruction).
    std::uint32_t cost = 0;
    /// A retired entry (EV114): the text of the replacement, shown in the error.
    std::string retired_hint;
    bool retired = false;
    /// Non-empty: the system behind the entry is not built yet ("E10"); the evaluator answers "no data"
    /// (TriggerResult::has_data == false) and the reference marks the entry.
    std::string stub;
    /// One line of event-file syntax for the generated reference (`age >= 16`).
    std::string example;
};

/// Keys of the auto-tooltip templates of a trigger or value entry (docs/dev/event-engine.md section 15.1):
/// `tooltip.trigger.<name>` (the condition), `.not` (a predicate required to be `no`) and `.fail`
/// (why a variant is unavailable). Effects and state operations use `tooltip.effect.<name>`.
/// Shared keys: `tooltip.cmp.<ge|gt|le|lt|eq|ne>` (the comparison phrase of `{need}`, parameter `{value}`)
/// and `tooltip.fail.required` (the wrapper of the `.not` text when a predicate is required to be `no`).
inline constexpr std::string_view kTooltipComparePrefix = "tooltip.cmp.";
inline constexpr std::string_view kTooltipRequiredKey = "tooltip.fail.required";
[[nodiscard]] std::string tooltip_key(const CatalogEntry& entry);
[[nodiscard]] std::string tooltip_not_key(const CatalogEntry& entry);
[[nodiscard]] std::string tooltip_fail_key(const CatalogEntry& entry);

// ---- on-actions (section 8.1) ----------------------------------------------------------------------

/// How often a pulse fires (the length of the interval of mean_time_to_happen).
enum class PulseKind : std::uint8_t { None = 0, Daily = 1, Monthly = 2, Quarterly = 3, Yearly = 4 };
inline constexpr std::size_t kPulseKindCount = 5;

struct OnRole {
    std::string name;
    ScopeKind kind = ScopeKind::None;
};

struct OnProvided {
    std::string name;  // `scope.<name>` inside the event
    ScopeKind kind = ScopeKind::None;
};

struct OnActionEntry {
    std::uint16_t code = 0;
    std::string name;
    /// The roots the action offers; the first role is the default (`on = on_death` means `deceased`).
    std::vector<OnRole> roles;
    /// For pulses: the kinds the pulse runs over (the event's `scope` must be one of them).
    ScopeMask pulse_scopes = 0;
    PulseKind pulse = PulseKind::None;
    /// Scopes the action puts into the saved scopes of the event (`scope:mother`).
    std::vector<OnProvided> provides;
    /// True for a code that is reserved for a later epic: subscribing is an error (EV108).
    bool reserved = false;
    /// Informational: the producer is not switched on yet (contentlint warns, EL201).
    bool dormant = false;
};

// ---- the catalog ------------------------------------------------------------------------------------

/// Registry of the names of the event language. It is also a CatalogInfo, so the disassembler and the
/// cost estimate can use it directly.
class Catalog : public CatalogInfo {
public:
    /// Adds an entry. A duplicate name or code in the same namespace is a programming error (asserted).
    void add(CatalogEntry entry);
    void add_on_action(OnActionEntry entry);

    [[nodiscard]] const CatalogEntry* find(NameSpace space, std::string_view name) const;
    [[nodiscard]] const CatalogEntry* find_code(NameSpace space, std::uint16_t code) const;
    [[nodiscard]] const OnActionEntry* find_on_action(std::string_view name) const;
    [[nodiscard]] const OnActionEntry* find_on_action_code(std::uint16_t code) const;

    /// The names of a namespace in registration order (candidates of "did you mean").
    [[nodiscard]] std::vector<std::string> names(NameSpace space) const;
    [[nodiscard]] std::vector<std::string> on_action_names() const;
    [[nodiscard]] const std::vector<CatalogEntry>& entries() const noexcept { return entries_; }
    [[nodiscard]] const std::vector<OnActionEntry>& on_actions() const noexcept { return on_actions_; }

    /// The entries that belong to the language: transitions (5.1), lists (5.2) and on-actions (8.1).
    [[nodiscard]] static Catalog engine();
    /// The language plus every entry the game registers (E11-T07 .. T11): what the compiler of the game
    /// and the generated reference use.
    [[nodiscard]] static Catalog standard();

    // CatalogInfo
    [[nodiscard]] std::uint32_t cost(Op op, std::uint16_t code) const override;
    [[nodiscard]] std::uint32_t list_bound(std::uint16_t code) const override;
    [[nodiscard]] std::string_view name(NameSpace space, std::uint16_t code) const override;

private:
    [[nodiscard]] static std::uint32_t key_of(NameSpace space, std::uint16_t code) noexcept {
        return (static_cast<std::uint32_t>(space) << 16) | code;
    }

    std::vector<CatalogEntry> entries_;
    std::vector<OnActionEntry> on_actions_;
    std::map<std::string, std::size_t, std::less<>> by_name_;  // "<space>/<name>" -> index
    std::map<std::uint32_t, std::size_t> by_code_;             // key_of(space, code) -> index
};

}  // namespace pat::sim::events
