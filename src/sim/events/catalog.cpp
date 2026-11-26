#include "pat/sim/events/catalog.hpp"

#include "pat/core/assert.hpp"

#include <array>
#include <cstddef>
#include <string>
#include <utility>

namespace pat::sim::events {

// ---- scope kinds ------------------------------------------------------------------------------------

namespace {

struct KindName {
    ScopeKind kind;
    std::string_view name;
};

constexpr std::array<KindName, 10> kKindNames{{
    {ScopeKind::Character, "character"},
    {ScopeKind::Dynasty, "dynasty"},
    {ScopeKind::Province, "province"},
    {ScopeKind::Realm, "realm"},
    {ScopeKind::Church, "church"},
    {ScopeKind::Army, "army"},
    {ScopeKind::Scheme, "scheme"},
    {ScopeKind::Plot, "plot"},
    {ScopeKind::Faith, "faith"},
    {ScopeKind::Treaty, "treaty"},
}};

std::string space_key(NameSpace space, std::string_view name) {
    std::string key;
    key += static_cast<char>('A' + static_cast<int>(space));
    key += '/';
    key += name;
    return key;
}

}  // namespace

std::string tooltip_key(const CatalogEntry& entry) {
    const bool is_effect = entry.space == NameSpace::Effect || entry.space == NameSpace::StateOp;
    return std::string(is_effect ? "tooltip.effect." : "tooltip.trigger.") + entry.name;
}

std::string tooltip_not_key(const CatalogEntry& entry) {
    return tooltip_key(entry) + ".not";
}

std::string tooltip_fail_key(const CatalogEntry& entry) {
    return tooltip_key(entry) + ".fail";
}

std::string_view scope_kind_name(ScopeKind kind) noexcept {
    for (const KindName& k : kKindNames) {
        if (k.kind == kind) {
            return k.name;
        }
    }
    return "none";
}

ScopeKind scope_kind_from_name(std::string_view name) noexcept {
    for (const KindName& k : kKindNames) {
        if (k.name == name) {
            return k.kind;
        }
    }
    return ScopeKind::None;
}

// ---- argument builders --------------------------------------------------------------------------------

ArgSpec arg_number(std::string name, bool required, core::Fixed64 def) {
    ArgSpec a;
    a.name = std::move(name);
    a.kind = ArgKind::Number;
    a.required = required;
    a.default_value = def;
    return a;
}

ArgSpec arg_bool(std::string name, bool required, bool def) {
    ArgSpec a;
    a.name = std::move(name);
    a.kind = ArgKind::Bool;
    a.required = required;
    a.default_value = def ? core::Fixed64::one() : core::Fixed64::zero();
    return a;
}

ArgSpec arg_scope(std::string name, ScopeKind kind, bool required) {
    ArgSpec a;
    a.name = std::move(name);
    a.kind = ArgKind::Scope;
    a.required = required;
    a.scope_kind = kind;
    return a;
}

ArgSpec arg_name(std::string name, std::string ref_kind, bool required) {
    ArgSpec a;
    a.name = std::move(name);
    a.kind = ArgKind::Name;
    a.required = required;
    a.ref_kind = std::move(ref_kind);
    return a;
}

ArgSpec arg_ref(std::string name, std::string registry_kind, bool required) {
    ArgSpec a;
    a.name = std::move(name);
    a.kind = ArgKind::Ref;
    a.required = required;
    a.ref_kind = std::move(registry_kind);
    return a;
}

ArgSpec arg_enum(std::string name, std::vector<std::string> values, bool required, std::int32_t def) {
    ArgSpec a;
    a.name = std::move(name);
    a.kind = ArgKind::Enum;
    a.required = required;
    a.enum_values = std::move(values);
    a.default_value = core::Fixed64::from_int(def);
    return a;
}

ArgSpec arg_duration(std::string name, bool required) {
    ArgSpec a;
    a.name = std::move(name);
    a.kind = ArgKind::Duration;
    a.required = required;
    return a;
}

ArgSpec arg_date(std::string name, bool required) {
    ArgSpec a;
    a.name = std::move(name);
    a.kind = ArgKind::Date;
    a.required = required;
    return a;
}

// ---- Catalog ------------------------------------------------------------------------------------------

void Catalog::add(CatalogEntry entry) {
    PAT_ASSERT(
        entry.args.size() <= limits::kMaxEffectArgs, "too many arguments in catalog entry {}", entry.name);
    const std::string name_key = space_key(entry.space, entry.name);
    const std::uint32_t code_key = key_of(entry.space, entry.code);
    PAT_ASSERT(!by_name_.contains(name_key), "duplicate catalog name {}", entry.name);
    PAT_ASSERT(!by_code_.contains(code_key), "duplicate catalog code {} of {}", entry.code, entry.name);
    by_name_.emplace(name_key, entries_.size());
    by_code_.emplace(code_key, entries_.size());
    entries_.push_back(std::move(entry));
}

void Catalog::add_on_action(OnActionEntry entry) {
    PAT_ASSERT(find_on_action(entry.name) == nullptr, "duplicate on-action {}", entry.name);
    PAT_ASSERT(find_on_action_code(entry.code) == nullptr, "duplicate on-action code {}", entry.code);
    on_actions_.push_back(std::move(entry));
}

const CatalogEntry* Catalog::find(NameSpace space, std::string_view name) const {
    const auto it = by_name_.find(space_key(space, name));
    return it == by_name_.end() ? nullptr : &entries_[it->second];
}

const CatalogEntry* Catalog::find_code(NameSpace space, std::uint16_t code) const {
    const auto it = by_code_.find(key_of(space, code));
    return it == by_code_.end() ? nullptr : &entries_[it->second];
}

const OnActionEntry* Catalog::find_on_action(std::string_view name) const {
    for (const OnActionEntry& e : on_actions_) {
        if (e.name == name) {
            return &e;
        }
    }
    return nullptr;
}

const OnActionEntry* Catalog::find_on_action_code(std::uint16_t code) const {
    for (const OnActionEntry& e : on_actions_) {
        if (e.code == code) {
            return &e;
        }
    }
    return nullptr;
}

std::vector<std::string> Catalog::names(NameSpace space) const {
    std::vector<std::string> out;
    for (const CatalogEntry& e : entries_) {
        if (e.space == space) {
            out.push_back(e.name);
        }
    }
    return out;
}

std::vector<std::string> Catalog::on_action_names() const {
    std::vector<std::string> out;
    for (const OnActionEntry& e : on_actions_) {
        out.push_back(e.name);
    }
    return out;
}

std::uint32_t Catalog::cost(Op op, std::uint16_t code) const {
    const NameSpace space = op == Op::kPred ? NameSpace::Predicate : NameSpace::Value;
    if (const CatalogEntry* e = find_code(space, code); e != nullptr && e->cost != 0) {
        return e->cost;
    }
    return op_base_wu(static_cast<std::uint16_t>(op));
}

std::uint32_t Catalog::list_bound(std::uint16_t code) const {
    const CatalogEntry* e = find_code(NameSpace::List, code);
    return e != nullptr ? e->bound : 32;
}

std::string_view Catalog::name(NameSpace space, std::uint16_t code) const {
    const CatalogEntry* e = find_code(space, code);
    return e != nullptr ? std::string_view(e->name) : std::string_view();
}

// ---- the entries of the language -----------------------------------------------------------------------

namespace {

constexpr ScopeKind kC = ScopeKind::Character;
constexpr ScopeKind kD = ScopeKind::Dynasty;
constexpr ScopeKind kP = ScopeKind::Province;
constexpr ScopeKind kR = ScopeKind::Realm;

ScopeKind kind_of_letter(char letter) {
    switch (letter) {
        case 'c':
            return kC;
        case 'd':
            return kD;
        case 'p':
            return kP;
        case 'r':
            return kR;
        default:
            return ScopeKind::None;
    }
}

// "child:c house:d" -> roles / provided scopes; the letter after the colon is the kind.
template <class T>
std::vector<T> parse_named(std::string_view spec) {
    std::vector<T> out;
    std::size_t i = 0;
    while (i < spec.size()) {
        while (i < spec.size() && spec[i] == ' ') {
            ++i;
        }
        const std::size_t colon = spec.find(':', i);
        if (colon == std::string_view::npos) {
            break;
        }
        T item;
        item.name = std::string(spec.substr(i, colon - i));
        item.kind = kind_of_letter(spec[colon + 1]);
        out.push_back(std::move(item));
        i = colon + 2;
    }
    return out;
}

struct OnSpec {
    std::uint16_t code;
    std::string_view name;
    std::string_view roles;
    std::string_view provides;
    bool dormant;
};

// Section 8.1: the producers of the codes 1..25 are alive; the rest waits for its epic.
constexpr OnSpec kOnActions[] = {
    {1, "on_game_start", "realm:r", "", false},
    {6, "on_birth", "child:c house:d", "mother:c father:c", false},
    {7, "on_pregnancy_lost", "mother:c house:d", "father:c", false},
    {8, "on_death", "deceased:c house:d", "", false},
    {9, "on_marriage", "groom:c bride:c groom_house:d bride_house:d", "groom:c bride:c", false},
    {10, "on_marriage_ended", "a:c b:c house_a:d house_b:d", "", false},
    {11, "on_coming_of_age", "character:c house:d", "", false},
    {12, "on_conception", "mother:c house:d", "father:c", false},
    {13, "on_head_changed", "dynasty:d", "predecessor:c successor:c", false},
    {14, "on_trait_gained", "character:c", "", false},
    {15, "on_trait_lost", "character:c", "", false},
    {16, "on_stress_threshold", "character:c house:d", "", false},
    {17, "on_province_owner_changed", "province:p", "old_owner:d new_owner:d", false},
    {18, "on_province_unrest_threshold", "province:p owner:d", "", false},
    {19, "on_riot", "province:p owner:d", "", false},
    {20, "on_uprising", "province:p owner:d", "", false},
    {21, "on_anarchy", "province:p owner:d", "", false},
    {22, "on_bankruptcy", "dynasty:d", "", false},
    {23, "on_loan_overdue", "dynasty:d", "", false},
    {24, "on_quota_short", "dynasty:d", "", false},
    {25, "on_building_completed", "province:p owner:d", "", false},
    {50, "on_marriage_answered", "initiator:d responder:d", "groom:c bride:c", true},
    {51, "on_betrothal", "groom_house:d bride_house:d", "groom:c bride:c", true},
    {52, "on_betrothal_broken", "groom_house:d bride_house:d", "groom:c bride:c", true},
    {53, "on_tonsure", "character:c house:d", "spouse:c tonsuring_house:d", true},
    {54, "on_widowed", "widow:c house:d", "deceased_spouse:c", true},
    {55, "on_succession", "dynasty:d", "predecessor:c successor:c", true},
    {56, "on_succession_dispute", "dynasty:d", "claimant:c", true},
    {57, "on_regent_usurpation", "dynasty:d regent:c", "regent:c ward:c", true},
    {58, "on_regency_started", "dynasty:d", "regent:c ward:c", true},
    {59, "on_regency_ended", "dynasty:d", "ward:c", true},
    {60, "on_heir_designated", "dynasty:d", "heir:c", true},
    {61, "on_disowned", "character:c house:d", "", true},
    {62, "on_law_changed", "dynasty:d", "", true},
    {100, "on_office_assigned", "holder:c house:d", "", true},
    {101, "on_office_removed", "holder:c house:d", "", true},
    {102, "on_petition_decided", "petitioner:d candidate:c", "", true},
    {103, "on_favor_threshold", "dynasty:d", "", true},
    {104, "on_servant_hired", "dynasty:d", "", true},
    {105, "on_servant_left", "dynasty:d", "", true},
    {106, "on_portfolio_course", "holder:c house:d", "", true},
    {107, "on_portfolio_order", "holder:c house:d", "", true},
    {108, "on_portfolio_state", "house:d", "", true},
    {109, "on_portfolio_pressure", "dynasty:d", "", true},
    {110, "on_crisis_phase_changed", "realm:r", "", true},
    {111, "on_stability_threshold", "realm:r", "", true},
    {112, "on_emperor_health", "emperor:c house:d", "", true},
    {113, "on_throne_changed", "emperor:c house:d", "predecessor:c candidate:c", true},
    {114, "on_plot_changed", "organizer:d member:c", "candidate:c", true},
    {115, "on_plot_discovered", "organizer:d", "candidate:c", true},
    {116, "on_coup_resolved", "organizer:d candidate:c", "", true},
    {117, "on_denunciation_resolved", "denouncer:d accused:d", "", true},
    {118, "on_claim_changed", "claimant:c house:d", "", true},
    {119, "on_suspicion_threshold", "dynasty:d", "", true},
};

// Reserved for later epics (section 8.1): subscribing is an error until the epic switches them on.
constexpr std::array<std::pair<std::uint16_t, std::string_view>, 8> kReservedOnActions{{
    {120, "on_scheme_started"},
    {121, "on_scheme_discovered"},
    {122, "on_scheme_completed"},
    {130, "on_battle_resolved"},
    {131, "on_siege_end"},
    {140, "on_treaty_signed"},
    {141, "on_treaty_broken"},
    {150, "on_faith_changed"},
}};

void add_transition(Catalog& c, std::uint16_t code, std::string_view name, ScopeKind from, ScopeKind to) {
    CatalogEntry e;
    e.space = NameSpace::Transition;
    e.code = code;
    e.name = std::string(name);
    e.from = scope_bit(from);
    e.to = to;
    c.add(std::move(e));
}

void add_list(Catalog& c,
              std::uint16_t code,
              std::string_view name,
              ScopeKind from,
              ScopeKind to,
              std::uint32_t bound) {
    CatalogEntry e;
    e.space = NameSpace::List;
    e.code = code;
    e.name = std::string(name);
    e.from = scope_bit(from);
    e.to = to;
    e.bound = bound;
    c.add(std::move(e));
}

}  // namespace

void add_predicate(Catalog& c,
                   std::uint16_t code,
                   std::string name,
                   ScopeMask from,
                   std::string example,
                   std::vector<ArgSpec> args,
                   std::string stub) {
    CatalogEntry e;
    e.space = NameSpace::Predicate;
    e.code = code;
    e.name = std::move(name);
    e.from = from;
    e.result = ValueType::Bool;
    e.args = std::move(args);
    e.example = std::move(example);
    e.stub = std::move(stub);
    c.add(std::move(e));
}

void add_value(Catalog& c,
               std::uint16_t code,
               std::string name,
               ScopeMask from,
               std::string example,
               std::vector<ArgSpec> args,
               std::vector<std::string> suffixes,
               std::string stub) {
    CatalogEntry e;
    e.space = NameSpace::Value;
    e.code = code;
    e.name = std::move(name);
    e.from = from;
    e.result = ValueType::Number;
    e.args = std::move(args);
    e.suffixes = std::move(suffixes);
    e.example = std::move(example);
    e.stub = std::move(stub);
    c.add(std::move(e));
}

Catalog Catalog::engine() {
    Catalog c;
    // Section 5.1: the transitions.
    add_transition(c, 1, "dynasty", kC, kD);
    add_transition(c, 2, "father", kC, kC);
    add_transition(c, 3, "mother", kC, kC);
    add_transition(c, 4, "spouse", kC, kC);
    add_transition(c, 5, "head", kD, kC);
    add_transition(c, 6, "acting_head", kD, kC);
    add_transition(c, 7, "heir", kD, kC);
    add_transition(c, 8, "regent", kD, kC);
    add_transition(c, 9, "owner", kP, kD);
    add_transition(c, 10, "administrator", kP, kC);
    add_transition(c, 11, "emperor", kR, kC);
    add_transition(c, 12, "emperor_dynasty", kR, kD);
    // Section 5.2: the lists (the whole-world lists are deliberately absent, EV107).
    add_list(c, 1, "living_member", kD, kC, 256);
    add_list(c, 2, "child", kC, kC, 32);
    add_list(c, 3, "sibling", kC, kC, 32);
    add_list(c, 4, "owned_province", kD, kP, 128);
    add_list(c, 5, "realm_dynasty", kR, kD, 128);
    add_list(c, 6, "claimant", kD, kC, 4);

    // Section 8.1: the on-actions.
    const auto pulse = [&c](std::uint16_t code, std::string_view name, PulseKind kind, ScopeMask scopes) {
        OnActionEntry e;
        e.code = code;
        e.name = std::string(name);
        e.pulse = kind;
        e.pulse_scopes = scopes;
        c.add_on_action(std::move(e));
    };
    constexpr ScopeMask kFour = scope_mask({kC, kD, kP, kR});
    pulse(2, "monthly_pulse", PulseKind::Monthly, kFour);
    pulse(3, "quarterly_pulse", PulseKind::Quarterly, kFour);
    pulse(4, "yearly_pulse", PulseKind::Yearly, kFour);
    pulse(5, "daily_pulse", PulseKind::Daily, scope_bit(kR));
    for (const OnSpec& spec : kOnActions) {
        OnActionEntry e;
        e.code = spec.code;
        e.name = std::string(spec.name);
        e.roles = parse_named<OnRole>(spec.roles);
        e.provides = parse_named<OnProvided>(spec.provides);
        e.dormant = spec.dormant;
        c.add_on_action(std::move(e));
    }
    for (const auto& [code, name] : kReservedOnActions) {
        OnActionEntry e;
        e.code = code;
        e.name = std::string(name);
        e.reserved = true;
        c.add_on_action(std::move(e));
    }
    return c;
}

}  // namespace pat::sim::events
