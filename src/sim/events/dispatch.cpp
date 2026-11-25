#include "pat/sim/events/dispatch.hpp"

#include "pat/core/assert.hpp"
#include "pat/sim/calendar.hpp"
#include "pat/sim/economy/tables.hpp"
#include "pat/sim/events/world_host.hpp"
#include "pat/sim/family/tables.hpp"
#include "pat/sim/health.hpp"
#include "pat/sim/kinship.hpp"
#include "pat/sim/world.hpp"

#include <algorithm>
#include <limits>
#include <numeric>
#include <variant>

namespace pat::sim::events {

namespace {

template <class... Ts>
struct Overloaded : Ts... {
    using Ts::operator()...;
};
template <class... Ts>
Overloaded(Ts...) -> Overloaded<Ts...>;

constexpr std::array<PulseKind, 4> kPulseOrder{
    PulseKind::Daily, PulseKind::Monthly, PulseKind::Quarterly, PulseKind::Yearly};
constexpr std::array<ScopeKind, 4> kKindOrder{
    ScopeKind::Character, ScopeKind::Dynasty, ScopeKind::Province, ScopeKind::Realm};

constexpr std::uint32_t kPpmOne = 1'000'000;
constexpr Scope kRealm{ScopeKind::Realm, 0};

[[nodiscard]] Scope character_scope(CharacterId id) noexcept {
    return id.valid() ? Scope::make(ScopeKind::Character, id.value()) : Scope{};
}
[[nodiscard]] Scope dynasty_scope(DynastyId id) noexcept {
    return id.valid() ? Scope::make(ScopeKind::Dynasty, id.value()) : Scope{};
}
[[nodiscard]] Scope province_scope(ProvinceId id) noexcept {
    return id.valid() ? Scope::make(ScopeKind::Province, id.value()) : Scope{};
}

/// The house of a person, alive or archived (the dead are not in the member lists).
[[nodiscard]] DynastyId house_of(const World& world, CharacterId id) {
    if (!id.valid()) {
        return {};
    }
    if (world.is_alive(id)) {
        return world.characters().dynasty[id.index()];
    }
    const std::optional<ArchivedPerson> dead = world.archived_person(id);
    return dead ? dead->dynasty : DynastyId{};
}

/// A scope the gates may start an event on: an existing row (a dead person only when the event allows it).
[[nodiscard]] bool scope_usable(const World& world, Scope scope, bool allow_dead) {
    switch (scope.kind) {
        case ScopeKind::Character: {
            const CharacterId id{scope.id};
            return allow_dead ? world.exists(id) : world.is_alive(id);
        }
        case ScopeKind::Dynasty:
            return world.exists(DynastyId{scope.id});
        case ScopeKind::Province:
            return world.exists(ProvinceId{scope.id});
        case ScopeKind::Realm:
            return scope.id == 0;
        default:
            return false;
    }
}

[[nodiscard]] EventKey group_key(std::uint64_t group) noexcept {
    return group | kCooldownGroupBit;
}

/// Section 11.2: is the event paused on this scope (own cooldown, group, once)?
[[nodiscard]] bool paused(const World& world, const EventDef& def, Scope scope) {
    const State& state = world.event_state();
    if (state.cooldowns.empty()) {
        return false;
    }
    if (world.event_on_cooldown(scope, def.key)) {
        return true;
    }
    if (def.cooldown_group != 0 && world.event_on_cooldown(scope, group_key(def.cooldown_group))) {
        return true;
    }
    return def.once == OnceMode::Global && world.event_on_cooldown(kRealm, def.key);
}

[[nodiscard]] bool in_window(const EventDef& def, Date today, bool anchors_enabled) noexcept {
    if (!def.has_window) {
        return true;
    }
    if (def.anchor && !anchors_enabled) {
        return false;
    }
    return today.days() >= def.window_from && today.days() <= def.window_to;
}

[[nodiscard]] std::uint32_t clamp_ppm(std::int64_t p) noexcept {
    return static_cast<std::uint32_t>(std::clamp<std::int64_t>(p, 1, kPpmOne));
}

/// Section 12.4: probability per pulse in ppm.
[[nodiscard]] std::uint32_t chance_ppm(core::Fixed64 percent) noexcept {
    const std::int64_t one_percent = std::int64_t{1} << core::Fixed64::frac_bits;
    const std::int64_t raw = std::clamp<std::int64_t>(percent.raw(), 0, 100 * one_percent);
    // percent x 10^4 ppm; raw < 2^39, so the product fits
    return static_cast<std::uint32_t>((raw * 10'000) >> core::Fixed64::frac_bits);
}

}  // namespace

// ---- index ----------------------------------------------------------------------------------------------

DispatchIndex::DispatchIndex(const EventSet& set, const Catalog& catalog) {
    const std::size_t n = set.events.size();
    ranked_.resize(n);
    std::iota(ranked_.begin(), ranked_.end(), std::uint32_t{0});
    std::ranges::sort(ranked_, [&](std::uint32_t a, std::uint32_t b) {
        const EventDef& x = set.events[a];
        const EventDef& y = set.events[b];
        return x.priority != y.priority ? x.priority > y.priority : x.key < y.key;
    });
    rank_.assign(n, 0);
    for (std::size_t r = 0; r < n; ++r) {
        rank_[ranked_[r]] = static_cast<std::uint32_t>(r);
    }
    for (const std::uint32_t ev : ranked_) {
        const EventDef& def = set.events[ev];
        const auto kind = static_cast<std::size_t>(def.scope);
        if (kind == 0 || kind >= kScopeKinds) {
            continue;
        }
        for (const OnSub& sub : def.on) {
            const OnActionEntry* entry = catalog.find_on_action_code(sub.action);
            if (entry == nullptr || entry->reserved) {
                continue;
            }
            if (entry->pulse != PulseKind::None) {
                auto& list = pulses_[kind][static_cast<std::size_t>(entry->pulse)];
                if (list.empty() || list.back() != ev) {
                    list.push_back(ev);
                    ++subscriptions_;
                }
            } else if (sub.action < kActionCodes) {
                by_action_[kind][sub.action].push_back(Subscriber{ev, sub.role});
                ++subscriptions_;
            }
        }
    }
}

std::span<const Subscriber> DispatchIndex::subscribers(ScopeKind kind, std::uint16_t action) const noexcept {
    const auto k = static_cast<std::size_t>(kind);
    if (k >= kScopeKinds || action >= kActionCodes) {
        return {};
    }
    return by_action_[k][action];
}

std::span<const std::uint32_t> DispatchIndex::pulse(ScopeKind kind, PulseKind pulse) const noexcept {
    const auto k = static_cast<std::size_t>(kind);
    const auto p = static_cast<std::size_t>(pulse);
    if (k >= kScopeKinds || p >= 5) {
        return {};
    }
    return pulses_[k][p];
}

// ---- dispatcher -----------------------------------------------------------------------------------------

Dispatcher::Dispatcher(std::shared_ptr<const EventSet> set, Catalog catalog, DispatchConfig config)
    : set_(std::move(set)), catalog_(std::move(catalog)), config_(std::move(config)) {
    PAT_ASSERT(set_ != nullptr, "Dispatcher without a set of events");
    index_ = DispatchIndex(*set_, catalog_);
    for (const OnActionEntry& entry : catalog_.on_actions()) {
        if (entry.code < kActionCodes) {
            actions_[entry.code] = &entry;
        }
    }
    multi_.assign(set_->events.size(), 0);
    for (std::size_t i = 0; i < set_->events.size(); ++i) {
        multi_[i] = set_->events[i].on.size() > 1 ? 1 : 0;
    }
}

void Dispatcher::rebaseline(const World& world) {
    prev_owner_ = world.provinces().owner;
    baselined_ = true;
}

void Dispatcher::run(World& world, std::span<const LoggedCommand> applied, WorkBudget* budget) {
    begin(world);
    fire_timers(world);
    collect_occurrences(world, applied);
    collect_candidates(world);
    gate(world);
    fire(world);
    finish(world);
    if (budget != nullptr) {
        budget->spend(stats_.wu + stats_.action_candidates + stats_.pulse_candidates);
    }
}

void Dispatcher::begin(World& world) {
    stats_ = DispatchStats{};
    occurrences_.clear();
    candidates_.clear();
    survivors_.clear();
    launches_.clear();
    state_full_ = false;
    if (!baselined_) {
        rebaseline(world);
    }
}

// ---- stage 1: the occurrences ---------------------------------------------------------------------------

namespace {

/// Appends an occurrence; empty roots stay empty (the candidates skip them).
void add_occurrence(std::vector<Occurrence>& out,
                    std::uint16_t action,
                    std::int32_t param,
                    std::initializer_list<Scope> roots,
                    std::initializer_list<Scope> provided = {}) {
    Occurrence occ;
    occ.action = action;
    occ.param = param;
    for (const Scope& s : roots) {
        occ.roots[occ.root_count++] = s;
    }
    std::size_t i = 0;
    for (const Scope& s : provided) {
        occ.provided[i++] = s;
    }
    out.push_back(occ);
}

}  // namespace

void Dispatcher::collect_occurrences(World& world, std::span<const LoggedCommand> applied) {
    const Date today = world.today();
    const State& state = world.event_state();
    const CharacterTable& chars = world.characters();

    // on_game_start: the first pass of a new game.
    if (state.meta.started == 0) {
        add_occurrence(occurrences_, action::kGameStart, 0, {kRealm});
    }

    // 1. the commands the tick applied, in the order of the log.
    std::vector<const family::Marriage*> weddings;  // marriages made today, built when a Wed is met
    bool weddings_built = false;
    std::size_t wedding_next = 0;
    const auto visitor = Overloaded{
        [&](const cmd::Die& c) {
            const DynastyId house = house_of(world, c.character);
            add_occurrence(occurrences_,
                           action::kDeath,
                           static_cast<std::int32_t>(c.cause),
                           {character_scope(c.character), dynasty_scope(house)});
        },
        [&](const cmd::Birth& c) {
            const bool live = c.outcome == BirthOutcome::Live;
            if (live) {
                // the newborn is the youngest child of the mother (children come eldest first)
                CharacterId child;
                const std::vector<CharacterId> kids = kin::children_of(world, c.mother);
                for (auto it = kids.rbegin(); it != kids.rend(); ++it) {
                    if (world.is_alive(*it) && chars.birth_date[it->index()] == today) {
                        child = *it;
                        break;
                    }
                }
                if (child.valid()) {
                    const std::size_t i = child.index();
                    add_occurrence(occurrences_,
                                   action::kBirth,
                                   0,
                                   {character_scope(child), dynasty_scope(chars.dynasty[i])},
                                   {character_scope(chars.mother[i]), character_scope(chars.father[i])});
                }
            } else {
                add_occurrence(occurrences_,
                               action::kPregnancyLost,
                               static_cast<std::int32_t>(c.outcome),
                               {character_scope(c.mother), dynasty_scope(house_of(world, c.mother))});
            }
        },
        [&](const cmd::Conceive& c) {
            add_occurrence(occurrences_,
                           action::kConception,
                           0,
                           {character_scope(c.mother), dynasty_scope(house_of(world, c.mother))},
                           {character_scope(c.father)});
        },
        [&](const cmd::Marry& c) {
            const bool a_groom = !world.is_alive(c.b) || chars.sex[c.a.index()] == Sex::Male;
            const CharacterId groom = a_groom ? c.a : c.b;
            const CharacterId bride = a_groom ? c.b : c.a;
            add_occurrence(occurrences_,
                           action::kMarriage,
                           0,
                           {character_scope(groom),
                            character_scope(bride),
                            dynasty_scope(house_of(world, groom)),
                            dynasty_scope(house_of(world, bride))},
                           {character_scope(groom), character_scope(bride)});
        },
        [&](const cmd::Wed&) {
            if (!weddings_built) {
                weddings_built = true;
                const family::MarriageTable& table = world.family().marriages;
                for (auto it = table.rbegin(); it != table.rend() && it->wedding >= today; ++it) {
                    if (it->wedding == today) {
                        weddings.push_back(&*it);
                    }
                }
                std::ranges::reverse(weddings);
            }
            if (wedding_next < weddings.size()) {
                const family::Marriage& m = *weddings[wedding_next++];
                add_occurrence(
                    occurrences_,
                    action::kMarriage,
                    0,
                    {character_scope(m.groom), character_scope(m.bride), dynasty_scope(m.groom_house),
                     dynasty_scope(m.bride_house)},
                    {character_scope(m.groom), character_scope(m.bride)});
            }
        },
        [&](const cmd::EndMarriage& c) {
            CharacterId other;
            if (c.a.valid() && world.is_alive(c.a)) {
                for (const CharacterId ex : chars.ex_spouses[c.a.index()]) {
                    if (ex.valid()) {
                        other = ex;
                    }
                }
            }
            add_occurrence(occurrences_,
                           action::kMarriageEnded,
                           static_cast<std::int32_t>(c.reason),
                           {character_scope(c.a),
                            character_scope(other),
                            dynasty_scope(house_of(world, c.a)),
                            dynasty_scope(house_of(world, other))});
        },
        [&](const cmd::ComeOfAge& c) {
            add_occurrence(occurrences_,
                           action::kComingOfAge,
                           0,
                           {character_scope(c.character), dynasty_scope(house_of(world, c.character))});
        },
        [&](const cmd::GrantTrait& c) {
            add_occurrence(occurrences_, action::kTraitGained, c.trait, {character_scope(c.character)});
        },
        [&](const cmd::RevokeTrait& c) {
            add_occurrence(occurrences_, action::kTraitLost, c.trait, {character_scope(c.character)});
        },
        [&](const cmd::SetProvinceOwner& c) {
            if (!c.province.valid() || c.province.index() >= prev_owner_.size()) {
                return;
            }
            const DynastyId old = prev_owner_[c.province.index()];
            prev_owner_[c.province.index()] = c.new_owner;  // two changes in one tick chain correctly
            add_occurrence(occurrences_,
                           action::kProvinceOwnerChanged,
                           0,
                           {province_scope(c.province)},
                           {dynasty_scope(old), dynasty_scope(c.new_owner)});
        },
        [](const auto&) {}};
    for (const LoggedCommand& entry : applied) {
        if (entry.accepted()) {
            std::visit(visitor, entry.command.payload);
        }
    }

    // 2. the incident log of the economy, from the cursor.
    const economy::IncidentLog& incidents = world.economy().incidents;
    if (!incidents.seq.empty() && incidents.seq.back() >= state.meta.incident_cursor) {
        std::size_t first = incidents.size();
        while (first > 0 && incidents.seq[first - 1] >= state.meta.incident_cursor) {
            --first;
        }
        for (std::size_t i = first; i < incidents.size(); ++i) {
            const ProvinceId province = incidents.province[i];
            DynastyId owner = incidents.dynasty[i];
            if (!owner.valid() && province.valid() && world.exists(province)) {
                owner = world.provinces().owner[province.index()];
            }
            const auto param = incidents.param[i];
            switch (static_cast<economy::IncidentKind>(incidents.kind[i])) {
                case economy::IncidentKind::Riot:
                    add_occurrence(occurrences_,
                                   action::kRiot,
                                   param,
                                   {province_scope(province), dynasty_scope(owner)});
                    break;
                case economy::IncidentKind::UnrestStateChanged:
                    add_occurrence(occurrences_,
                                   action::kUnrestThreshold,
                                   param,
                                   {province_scope(province), dynasty_scope(owner)});
                    break;
                case economy::IncidentKind::Anarchy:
                    add_occurrence(
                        occurrences_, action::kAnarchy, 0, {province_scope(province), dynasty_scope(owner)});
                    break;
                case economy::IncidentKind::Uprising:
                    add_occurrence(occurrences_,
                                   action::kUprising,
                                   param,
                                   {province_scope(province), dynasty_scope(owner)});
                    break;
                case economy::IncidentKind::Bankruptcy:
                    add_occurrence(occurrences_, action::kBankruptcy, 0, {dynasty_scope(owner)});
                    break;
                case economy::IncidentKind::LoanOverdue:
                    add_occurrence(occurrences_, action::kLoanOverdue, 0, {dynasty_scope(owner)});
                    break;
                case economy::IncidentKind::QuotaShort:
                    add_occurrence(occurrences_, action::kQuotaShort, 0, {dynasty_scope(owner)});
                    break;
                case economy::IncidentKind::BuildingCompleted:
                    add_occurrence(occurrences_,
                                   action::kBuildingCompleted,
                                   param,
                                   {province_scope(province), dynasty_scope(owner)});
                    break;
                default:
                    break;
            }
        }
    }

    // 3. the tail of the chronicle of today: a change of head, the stress notes.
    for (std::size_t d = 0; d < world.dynasties().size(); ++d) {
        const DynastyId dynasty{static_cast<std::uint32_t>(d)};
        const std::span<const ChronicleEntry> chronicle = world.chronicle(dynasty);
        if (chronicle.empty() || chronicle.back().date != today) {
            continue;
        }
        std::size_t first = chronicle.size();
        while (first > 0 && chronicle[first - 1].date == today) {
            --first;
        }
        for (std::size_t i = first; i < chronicle.size(); ++i) {
            const ChronicleEntry& e = chronicle[i];
            if (e.kind == ChronicleKind::HeadChanged) {
                add_occurrence(occurrences_,
                               action::kHeadChanged,
                               0,
                               {dynasty_scope(dynasty)},
                               {character_scope(e.a), character_scope(e.b)});
            } else if (e.kind == ChronicleKind::Note &&
                       (e.param == kNoteStressStrained || e.param == kNoteStressBreaking)) {
                add_occurrence(occurrences_,
                               action::kStressThreshold,
                               e.param == kNoteStressStrained ? 1 : 2,
                               {character_scope(e.a), dynasty_scope(dynasty)});
            }
        }
    }
    stats_.occurrences = static_cast<std::uint32_t>(occurrences_.size());
}

// ---- stage 2: the candidates ----------------------------------------------------------------------------

namespace {

/// The ids of the entities of a kind that have their pulse today (stagger, section 12.3), ascending.
void slot_entities(const World& world,
                   ScopeKind kind,
                   PulseKind pulse,
                   Date today,
                   int month,
                   std::vector<std::uint32_t>& out) {
    out.clear();
    if (pulse == PulseKind::Daily) {
        if (kind == ScopeKind::Realm) {
            out.push_back(0);
        }
        return;
    }
    if (pulse == PulseKind::Quarterly && (month - 1) % 3 != 0) {
        return;
    }
    const auto due = [&](std::uint32_t id) {
        return pulse == PulseKind::Yearly ? is_yearly_slot(id, today) : is_monthly_slot(id, today);
    };
    switch (kind) {
        case ScopeKind::Character:
            for (const CharacterId c : world.living()) {
                if (due(c.value())) {
                    out.push_back(c.value());
                }
            }
            break;
        case ScopeKind::Dynasty:
            for (std::uint32_t i = 0; i < world.dynasties().size(); ++i) {
                if (due(i) && !world.dynasty_extinct(DynastyId{i})) {
                    out.push_back(i);
                }
            }
            break;
        case ScopeKind::Province:
            for (std::uint32_t i = 0; i < world.provinces().size(); ++i) {
                if (due(i)) {
                    out.push_back(i);
                }
            }
            break;
        case ScopeKind::Realm:
            if (due(0)) {
                out.push_back(0);
            }
            break;
        default:
            break;
    }
}

}  // namespace

void Dispatcher::collect_candidates(const World& world) {
    candidates_.clear();
    const bool indexed = mode_ == DispatchMode::Indexed;
    const EventSet& set = *set_;

    // actions: per occurrence, per root, the events in the order (priority descending, key ascending)
    for (std::size_t i = 0; i < occurrences_.size(); ++i) {
        const Occurrence& occ = occurrences_[i];
        for (std::size_t r = 0; r < occ.root_count; ++r) {
            const Scope root = occ.roots[r];
            if (root.empty()) {
                continue;
            }
            if (indexed) {
                for (const Subscriber& sub : index_.subscribers(root.kind, occ.action)) {
                    if (sub.role == r) {
                        candidates_.push_back(
                            Candidate{sub.event, root, PulseKind::None, static_cast<std::int32_t>(i)});
                    }
                }
            } else {
                for (const std::uint32_t ev : index_.ranked()) {
                    const EventDef& def = set.events[ev];
                    if (def.scope != root.kind) {
                        continue;
                    }
                    for (const OnSub& sub : def.on) {
                        if (sub.action == occ.action && sub.role == r) {
                            candidates_.push_back(
                                Candidate{ev, root, PulseKind::None, static_cast<std::int32_t>(i)});
                        }
                    }
                }
            }
        }
    }
    stats_.action_candidates = static_cast<std::uint32_t>(candidates_.size());

    // pulses: per pulse kind and scope kind, the events over the entities of today's slot
    const Date today = world.today();
    const int month = today.month();
    for (const PulseKind pulse : kPulseOrder) {
        for (const ScopeKind kind : kKindOrder) {
            if (indexed && index_.pulse(kind, pulse).empty()) {
                continue;
            }
            slot_entities(world, kind, pulse, today, month, slot_ids_);
            if (slot_ids_.empty()) {
                continue;
            }
            const auto emit = [&](std::uint32_t ev) {
                for (const std::uint32_t id : slot_ids_) {
                    candidates_.push_back(Candidate{ev, Scope::make(kind, id), pulse, -1});
                }
            };
            if (indexed) {
                for (const std::uint32_t ev : index_.pulse(kind, pulse)) {
                    emit(ev);
                }
            } else {
                for (const std::uint32_t ev : index_.ranked()) {
                    const EventDef& def = set.events[ev];
                    if (def.scope != kind) {
                        continue;
                    }
                    const bool subscribed = std::ranges::any_of(def.on, [&](const OnSub& sub) {
                        return sub.action < kActionCodes && actions_[sub.action] != nullptr &&
                               actions_[sub.action]->pulse == pulse;
                    });
                    if (subscribed) {
                        emit(ev);
                    }
                }
            }
        }
    }
    stats_.pulse_candidates = static_cast<std::uint32_t>(candidates_.size()) - stats_.action_candidates;

    // a candidate appears once per tick: the first one in the generation order stays (sorted vector)
    keys_.clear();
    for (std::size_t i = 0; i < candidates_.size(); ++i) {
        const Candidate& c = candidates_[i];
        if (c.occurrence >= 0 || multi_[c.event] != 0) {
            keys_.push_back(SortKey{c.event, c.scope.word(), static_cast<std::uint32_t>(i)});
        }
    }
    std::sort(keys_.begin(), keys_.end());
    drop_.assign(candidates_.size(), 0);
    std::size_t dropped = 0;
    for (std::size_t i = 1; i < keys_.size(); ++i) {
        if (keys_[i].a == keys_[i - 1].a && keys_[i].b == keys_[i - 1].b) {
            drop_[keys_[i].position] = 1;
            ++dropped;
        }
    }
    if (dropped != 0) {
        std::size_t out = 0;
        for (std::size_t i = 0; i < candidates_.size(); ++i) {
            if (drop_[i] == 0) {
                candidates_[out++] = candidates_[i];
            }
        }
        candidates_.resize(out);
    }
    stats_.duplicates = static_cast<std::uint32_t>(dropped);
}

// ---- stage 3: the gates ---------------------------------------------------------------------------------

namespace {

/// The saved scopes of a start: the slots the action provides (`scope:mother`), by name.
[[nodiscard]] std::array<Scope, limits::kMaxSavedScopes>
saved_of(const EventDef& def, const Occurrence* occ, const OnActionEntry* action) {
    std::array<Scope, limits::kMaxSavedScopes> saved{};
    if (occ == nullptr || action == nullptr || def.provided_saved == 0) {
        return saved;
    }
    const std::size_t slots = std::min<std::size_t>(def.provided_saved, def.saved_names.size());
    for (std::size_t slot = 0; slot < slots && slot < saved.size(); ++slot) {
        for (std::size_t p = 0; p < action->provides.size() && p < occ->provided.size(); ++p) {
            if (action->provides[p].name == def.saved_names[slot]) {
                saved[slot] = occ->provided[p];
            }
        }
    }
    return saved;
}

[[nodiscard]] DynastyId recipient_of(const World& world, Scope scope) {
    switch (scope.kind) {
        case ScopeKind::Character:
            return world.is_alive(CharacterId{scope.id}) ? world.characters().dynasty[scope.id] : DynastyId{};
        case ScopeKind::Dynasty:
            return DynastyId{scope.id};
        case ScopeKind::Province:
            return world.exists(ProvinceId{scope.id}) ? world.provinces().owner[scope.id] : DynastyId{};
        default:
            return {};
    }
}

[[nodiscard]] bool is_player(const Meta& meta, DynastyId who) noexcept {
    for (std::size_t i = 0; i < meta.player_count && i < meta.players.size(); ++i) {
        if (meta.players[i] == who) {
            return true;
        }
    }
    return false;
}

}  // namespace

void Dispatcher::gate(World& world) {
    survivors_.clear();
    const EventSet& set = *set_;
    const EventSettings& settings = set.settings;
    const Date today = world.today();
    const Meta meta = world.event_state().meta;
    const bool anchors = meta.anchors_enabled != 0;
    const bool scaled = meta.frequency_permille != kDefaultFrequencyPermille;
    TriggerEnv env;
    env.world = &world;
    env.traits = config_.traits.get();
    env.trait_slots = config_.trait_slots;
    WorldVmHost host(world, catalog_, env);

    std::uint32_t stream_event = std::numeric_limits<std::uint32_t>::max();
    core::Pcg32* roll_rng = nullptr;

    const auto run_program = [&](const Program& program,
                                 Kind kind,
                                 const EventDef& def,
                                 Scope scope,
                                 const std::array<Scope, limits::kMaxSavedScopes>& saved) {
        host.set_rng(&world.rng().stream("events.run", def.key));
        RunOptions options;
        options.mode = Mode::Run;
        options.kind = kind;
        options.root = scope;
        options.saved = saved;
        options.max_instr = settings.max_trigger_instr;
        options.max_commands = settings.max_commands;
        options.max_state_ops = settings.max_state_ops;
        RunResult result = vm_.run(program, host, options);
        ++stats_.vm_runs;
        stats_.wu += result.wu;
        if (!result.ok) {
            ++stats_.vm_aborts;
        }
        return result;
    };

    for (const Candidate& c : candidates_) {
        const EventDef& def = set.events[c.event];
        const Occurrence* occ = c.occurrence >= 0 ? &occurrences_[static_cast<std::size_t>(c.occurrence)] : nullptr;

        // a scope that exists (the pulse entities are alive by construction)
        if (occ != nullptr) {
            const bool allow_dead = def.survives_death || occ->action == action::kDeath;
            if (!scope_usable(world, c.scope, allow_dead)) {
                ++stats_.scope_rejected;
                continue;
            }
        }
        if (paused(world, def, c.scope)) {
            ++stats_.cooldown_rejected;
            continue;
        }
        if (!in_window(def, today, anchors)) {
            ++stats_.window_rejected;
            continue;
        }

        const std::array<Scope, limits::kMaxSavedScopes> saved =
            saved_of(def, occ, occ != nullptr ? actions_[occ->action] : nullptr);

        // the roll: chance (an action or a pulse) or mean_time_to_happen (a pulse)
        bool rolled = false;
        std::uint32_t ppm = kPpmOne;
        if (def.chance) {
            const RunResult r = run_program(*def.chance, Kind::Value, def, c.scope, saved);
            ppm = r.ok && r.has_value ? chance_ppm(r.fixed()) : 0;
            rolled = true;
        } else if (def.mtth_days > 0 && c.pulse != PulseKind::None) {
            std::uint32_t base = def.mtth_ppm[static_cast<std::size_t>(c.pulse)];
            if (base == 0) {
                base = mtth_ppm(settings.pulse_days(c.pulse), def.mtth_days);
            }
            std::int64_t p = base;
            for (const MtthModifier& modifier : def.mtth_modifiers) {
                if (run_program(modifier.trigger, Kind::Trigger, def, c.scope, saved).truthy()) {
                    p = clamp_ppm(p * 1000 / std::max<std::int32_t>(1, modifier.factor_permille));
                }
            }
            if (scaled && (settings.frequency_applies_to_all || is_player(meta, recipient_of(world, c.scope)))) {
                p = clamp_ppm(p * meta.frequency_permille / 1000);
            }
            ppm = clamp_ppm(p);
            rolled = true;
        }
        if (rolled) {
            if (stream_event != c.event) {
                stream_event = c.event;
                roll_rng = &world.rng().stream("events.mtth", def.key);
            }
            ++stats_.rolls;
            if (roll_rng->bounded(kPpmOne) >= ppm) {
                ++stats_.roll_rejected;
                continue;
            }
        }

        if (def.trigger && !run_program(*def.trigger, Kind::Trigger, def, c.scope, saved).truthy()) {
            ++stats_.trigger_rejected;
            continue;
        }
        survivors_.push_back(c);
    }
    stats_.survivors = static_cast<std::uint32_t>(survivors_.size());
}

// ---- stage 0, 4, 5: timers, pools, cap, launches -------------------------------------------------------

void Dispatcher::launch_now(World& world, const EventDef& def, const Launch& launch) {
    const Date today = world.today();
    HistoryEntry history;
    history.date = today;
    history.event_key = def.key;
    history.outcome = HistoryOutcome::Fired;
    history.scope = launch.scope;
    if (!world.append_event_history(history)) {
        state_full_ = true;
    }
    if (!launch.from_timer) {
        // the pause starts with the launch (section 11.2); `once` is written last so that it wins
        const auto put = [&](Scope scope, EventKey key, Date until) {
            if (!world.set_event_cooldown(Cooldown{scope, key, until})) {
                state_full_ = true;
            }
        };
        if (def.cooldown_days > 0) {
            const Date until = today + def.cooldown_days;
            put(launch.scope, def.key, until);
            if (def.cooldown_group != 0) {
                put(launch.scope, group_key(def.cooldown_group), until);
            }
        }
        if (def.once == OnceMode::Scope) {
            put(launch.scope, def.key, kForever);
        } else if (def.once == OnceMode::Global) {
            put(kRealm, def.key, kForever);
        }
    }
    launches_.push_back(launch);
    ++stats_.launches;
}

void Dispatcher::fire_timers(World& world) {
    const Date today = world.today();
    const EventSettings& settings = set_->settings;
    TriggerEnv env;
    env.world = &world;
    env.traits = config_.traits.get();
    env.trait_slots = config_.trait_slots;
    while (!world.event_state().timers.empty() && world.event_state().timers.front().due <= today) {
        const Timer timer = world.event_state().timers.front();
        if (!world.cancel_event_timer(timer.seq)) {
            break;  // cannot happen; never loop forever
        }
        ++stats_.timers_due;
        const EventDef* def = set_->find(timer.event_key);
        if (def == nullptr) {
            continue;  // reconcile() removes these on load; a hot reload may race
        }
        if (timer.depth > settings.max_timer_chain) {
            HistoryEntry history;
            history.date = today;
            history.event_key = def->key;
            history.outcome = HistoryOutcome::ChainDepthExceeded;
            history.scope = timer.scope;
            (void)world.append_event_history(history);
            continue;
        }
        if (!scope_usable(world, timer.scope, def->survives_death)) {
            continue;
        }
        Launch launch;
        launch.key = def->key;
        launch.event = static_cast<std::uint32_t>(def - set_->events.data());
        launch.scope = timer.scope;
        launch.depth = timer.depth;
        launch.from_timer = true;
        for (std::size_t i = 0; i < timer.saved_count && i < launch.saved.size(); ++i) {
            launch.saved[i] = timer.saved[i].scope;
        }
        if (def->trigger) {
            WorldVmHost host(world, catalog_, env);
            host.set_rng(&world.rng().stream("events.run", def->key));
            RunOptions options;
            options.kind = Kind::Trigger;
            options.root = timer.scope;
            options.saved = launch.saved;
            options.max_instr = settings.max_trigger_instr;
            const RunResult r = vm_.run(*def->trigger, host, options);
            ++stats_.vm_runs;
            stats_.wu += r.wu;
            if (!r.truthy()) {
                ++stats_.trigger_rejected;
                continue;
            }
        }
        launch_now(world, *def, launch);
    }
}

void Dispatcher::fire(World& world) {
    const EventSet& set = *set_;
    const EventSettings& settings = set.settings;
    const Date today = world.today();

    // pools: the survivors of one pool on one scope compete, one weighted pick (section 12.5)
    if (!survivors_.empty()) {
        keys_.clear();
        for (std::size_t i = 0; i < survivors_.size(); ++i) {
            const EventDef& def = set.events[survivors_[i].event];
            if (def.pool != 0) {
                keys_.push_back(SortKey{def.pool, survivors_[i].scope.word(), static_cast<std::uint32_t>(i)});
            }
        }
        std::sort(keys_.begin(), keys_.end());
        drop_.assign(survivors_.size(), 0);
        TriggerEnv env;
        env.world = &world;
        env.traits = config_.traits.get();
        env.trait_slots = config_.trait_slots;
        WorldVmHost host(world, catalog_, env);
        std::size_t dropped = 0;
        for (std::size_t begin = 0; begin < keys_.size();) {
            std::size_t end = begin + 1;
            while (end < keys_.size() && keys_[end].a == keys_[begin].a && keys_[end].b == keys_[begin].b) {
                ++end;
            }
            if (end - begin >= 2) {
                std::uint64_t total = 0;
                weights_.clear();
                for (std::size_t k = begin; k < end; ++k) {
                    const Candidate& c = survivors_[keys_[k].position];
                    const EventDef& def = set.events[c.event];
                    std::uint32_t w = 1;
                    if (def.weight) {
                        host.set_rng(&world.rng().stream("events.run", def.key));
                        RunOptions options;
                        options.kind = Kind::Value;
                        options.root = c.scope;
                        options.max_instr = settings.max_trigger_instr;
                        const RunResult r = vm_.run(*def.weight, host, options);
                        ++stats_.vm_runs;
                        stats_.wu += r.wu;
                        const std::int64_t raw = r.ok && r.has_value ? r.fixed().raw() : 0;
                        w = static_cast<std::uint32_t>(std::clamp<std::int64_t>(raw >> 16, 0, 1 << 20));
                    }
                    weights_.push_back(w);
                    total += w;
                }
                std::size_t winner = 0;  // all weights 0: the first in the order wins, without a draw
                if (total > 0) {
                    auto pick = world.rng().stream("events.pool", keys_[begin].a).bounded(static_cast<std::uint32_t>(total));
                    for (std::size_t k = 0; k < weights_.size(); ++k) {
                        if (pick < weights_[k]) {
                            winner = k;
                            break;
                        }
                        pick -= weights_[k];
                    }
                }
                for (std::size_t k = 0; k < end - begin; ++k) {
                    if (k != winner) {
                        drop_[keys_[begin + k].position] = 1;
                        ++dropped;
                    }
                }
            }
            begin = end;
        }
        stats_.pool_losers = static_cast<std::uint32_t>(dropped);
        if (dropped != 0) {
            std::size_t out = 0;
            for (std::size_t i = 0; i < survivors_.size(); ++i) {
                if (drop_[i] == 0) {
                    survivors_[out++] = survivors_[i];
                }
            }
            survivors_.resize(out);
        }

        // the cap: at most `fire_cap_per_pulse` pulse events per scope and pass, the first in the order
        keys_.clear();
        for (std::size_t i = 0; i < survivors_.size(); ++i) {
            if (survivors_[i].pulse != PulseKind::None) {
                keys_.push_back(SortKey{0, survivors_[i].scope.word(), static_cast<std::uint32_t>(i)});
            }
        }
        std::sort(keys_.begin(), keys_.end());
        drop_.assign(survivors_.size(), 0);
        std::size_t capped = 0;
        const auto cap = static_cast<std::size_t>(std::max<std::int32_t>(1, settings.fire_cap_per_pulse));
        std::size_t run_length = 0;
        for (std::size_t i = 0; i < keys_.size(); ++i) {
            run_length = i > 0 && keys_[i].b == keys_[i - 1].b ? run_length + 1 : 1;
            if (run_length > cap) {
                drop_[keys_[i].position] = 1;
                ++capped;
            }
        }
        stats_.capped = static_cast<std::uint32_t>(capped);
        if (capped != 0) {
            std::size_t out = 0;
            for (std::size_t i = 0; i < survivors_.size(); ++i) {
                if (drop_[i] == 0) {
                    survivors_[out++] = survivors_[i];
                }
            }
            survivors_.resize(out);
        }
    }

    // the launches, in the order of the survivors
    for (const Candidate& c : survivors_) {
        const EventDef& def = set.events[c.event];
        const Occurrence* occ = c.occurrence >= 0 ? &occurrences_[static_cast<std::size_t>(c.occurrence)] : nullptr;
        Launch launch;
        launch.key = def.key;
        launch.event = c.event;
        launch.scope = c.scope;
        launch.saved = saved_of(def, occ, occ != nullptr ? actions_[occ->action] : nullptr);
        launch.param = occ != nullptr ? occ->param : 0;
        launch_now(world, def, launch);
    }
    if (state_full_) {
        HistoryEntry history;
        history.date = today;
        history.outcome = HistoryOutcome::StateFull;
        (void)world.append_event_history(history);
        state_full_ = false;
    }
}

void Dispatcher::finish(World& world) {
    Meta meta = world.event_state().meta;
    bool changed = false;
    if (meta.started == 0) {
        meta.started = 1;
        changed = true;
    }
    const std::uint32_t next = world.economy().meta.next_incident_seq;
    if (meta.incident_cursor != next) {
        meta.incident_cursor = next;
        changed = true;
    }
    if (changed) {
        (void)world.set_event_meta(meta);
    }
    prev_owner_ = world.provinces().owner;
}

}  // namespace pat::sim::events
