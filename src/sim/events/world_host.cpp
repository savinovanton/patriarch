#include "pat/sim/events/world_host.hpp"

#include "pat/sim/economy/tables.hpp"
#include "pat/sim/family/tables.hpp"
#include "pat/sim/kinship.hpp"

#include <algorithm>
#include <cstddef>
#include <span>
#include <utility>

namespace pat::sim::events {

namespace {

// The codes of the transitions and lists of Catalog::engine() (section 5).
constexpr std::uint16_t kToDynasty = 1;
constexpr std::uint16_t kToFather = 2;
constexpr std::uint16_t kToMother = 3;
constexpr std::uint16_t kToSpouse = 4;
constexpr std::uint16_t kToHead = 5;
constexpr std::uint16_t kToActingHead = 6;
constexpr std::uint16_t kToHeir = 7;
constexpr std::uint16_t kToRegent = 8;
constexpr std::uint16_t kToOwner = 9;
constexpr std::uint16_t kToAdministrator = 10;

constexpr std::uint16_t kListLivingMember = 1;
constexpr std::uint16_t kListChild = 2;
constexpr std::uint16_t kListSibling = 3;
constexpr std::uint16_t kListOwnedProvince = 4;
constexpr std::uint16_t kListRealmDynasty = 5;

[[nodiscard]] Scope character_scope(CharacterId id) noexcept {
    return id.valid() ? Scope::make(ScopeKind::Character, id.value()) : Scope{};
}
[[nodiscard]] Scope dynasty_scope(DynastyId id) noexcept {
    return id.valid() ? Scope::make(ScopeKind::Dynasty, id.value()) : Scope{};
}

/// A living person as a scope, or an empty scope.
[[nodiscard]] Scope living_scope(const World& world, CharacterId id) {
    return id.valid() && world.is_alive(id) ? character_scope(id) : Scope{};
}

/// Appends the `cap` lowest ids of an index of the World (ascending) as scopes; true when the index holds
/// more (the list is cut). O(n log cap): the index is not copied whole.
template <class Id, class ToScope>
bool take_lowest(std::span<const Id> ids, std::size_t cap, std::vector<Scope>& out, ToScope to_scope) {
    std::vector<Id> lowest(std::min(ids.size(), cap));
    std::ranges::partial_sort_copy(ids, lowest);
    for (const Id id : lowest) {
        out.push_back(to_scope(id));
    }
    return ids.size() > cap;
}

}  // namespace

WorldVmHost::WorldVmHost(const World& world, const Catalog& catalog, TriggerEnv env)
    : world_(world), catalog_(catalog), env_(env) {
    env_.world = &world;
}

Scope WorldVmHost::transition(Scope from, std::uint16_t code) {
    if (from.kind == ScopeKind::Character) {
        const CharacterId id{from.id};
        if (world_.is_alive(id)) {
            const CharacterTable& t = world_.characters();
            const std::size_t i = id.index();
            switch (code) {
                case kToDynasty:
                    return t.dynasty[i].valid() ? dynasty_scope(t.dynasty[i]) : Scope{};
                case kToFather:
                    return character_scope(t.father[i]);
                case kToMother:
                    return character_scope(t.mother[i]);
                case kToSpouse:
                    return living_scope(world_, t.spouse[i]);
                default:
                    return Scope{};
            }
        }
        // A deceased root (on_death): the archive keeps the house and the parents.
        if (const std::optional<ArchivedPerson> dead = world_.archived_person(id)) {
            switch (code) {
                case kToDynasty:
                    return dynasty_scope(dead->dynasty);
                case kToFather:
                    return character_scope(dead->father);
                case kToMother:
                    return character_scope(dead->mother);
                default:
                    return Scope{};
            }
        }
        return Scope{};
    }
    if (from.kind == ScopeKind::Dynasty) {
        const DynastyId d{from.id};
        if (!world_.exists(d)) {
            return Scope{};
        }
        switch (code) {
            case kToHead:
                return living_scope(world_, world_.dynasties().head[d.index()]);
            case kToActingHead: {
                const CharacterId head = world_.dynasties().head[d.index()];
                return living_scope(world_, head.valid() ? head : world_.family_caretaker(d));
            }
            case kToHeir: {
                const family::SuccessionState* s = world_.family_succession(d);
                return s != nullptr ? living_scope(world_, s->designated_heir) : Scope{};
            }
            case kToRegent: {
                const family::Regency* r = world_.family_regency(d);
                return r != nullptr ? living_scope(world_, r->regent) : Scope{};
            }
            default:
                return Scope{};
        }
    }
    if (from.kind == ScopeKind::Province) {
        const ProvinceId p{from.id};
        if (!world_.exists(p)) {
            return Scope{};
        }
        switch (code) {
            case kToOwner:
                return dynasty_scope(world_.provinces().owner[p.index()]);
            case kToAdministrator: {
                const auto& admins = world_.economy().provinces.administrator;
                return p.index() < admins.size() ? living_scope(world_, admins[p.index()]) : Scope{};
            }
            default:
                return Scope{};
        }
    }
    return Scope{};  // emperor, emperor_dynasty (E12) and the scopes of later epics: nothing yet
}

Word WorldVmHost::load(Scope scope, std::uint16_t code, std::span<const Word> args) {
    if (const std::optional<TriggerResult> r = eval_character_value(env_, scope, code, args)) {
        return r->value;
    }
    if (const std::optional<TriggerResult> r = eval_realm_value(env_, scope, code, args)) {
        return r->value;
    }
    return 0;
}

Word WorldVmHost::load_var(Scope scope, bool global, std::uint64_t key) {
    const Scope where = global ? Scope::make(ScopeKind::Realm, 0) : scope;
    return word_from_fixed(world_.event_var(where, key));
}

bool WorldVmHost::predicate(Scope scope, std::uint16_t code, std::span<const Word> args) {
    if (const std::optional<TriggerResult> r = eval_character_predicate(env_, scope, code, args)) {
        return r->value != 0;
    }
    if (const std::optional<TriggerResult> r = eval_realm_predicate(env_, scope, code, args)) {
        return r->value != 0;
    }
    return false;
}

bool WorldVmHost::list(Scope scope, std::uint16_t code, std::uint32_t limit, std::vector<Scope>& out) {
    // Fan-out (sections 5.2 and 7.4): at most min(limit, bound of the list) elements, in the order of
    // section 5.2; a longer list is cut and reported. The cut keeps the first elements of that order, so
    // the cost never exceeds what estimate_cost assumed and the same World gives the same elements.
    const std::size_t cap = std::min<std::size_t>(limit, catalog_.list_bound(code));
    bool cut = false;
    switch (code) {
        case kListLivingMember:
            if (scope.kind == ScopeKind::Dynasty && world_.exists(DynastyId{scope.id})) {
                cut = take_lowest(world_.members_of(DynastyId{scope.id}), cap, out, character_scope);
            }
            break;
        case kListChild:
        case kListSibling:
            if (scope.kind == ScopeKind::Character && world_.is_alive(CharacterId{scope.id})) {
                const CharacterId id{scope.id};
                const std::vector<CharacterId> kin =
                    code == kListChild ? kin::children_of(world_, id) : kin::siblings_of(world_, id);
                std::vector<std::pair<Date, CharacterId>> alive;
                for (const CharacterId c : kin) {
                    if (world_.is_alive(c)) {
                        alive.emplace_back(world_.characters().birth_date[c.index()], c);
                    }
                }
                std::ranges::sort(alive);  // (birth_date, id), section 5.2
                cut = alive.size() > cap;
                for (std::size_t i = 0; i < alive.size() && i < cap; ++i) {
                    out.push_back(character_scope(alive[i].second));
                }
            }
            break;
        case kListOwnedProvince:
            if (scope.kind == ScopeKind::Dynasty && world_.exists(DynastyId{scope.id})) {
                cut = take_lowest(world_.provinces_of(DynastyId{scope.id}), cap, out, [](ProvinceId p) {
                    return Scope::make(ScopeKind::Province, p.value());
                });
            }
            break;
        case kListRealmDynasty:
            if (scope.kind == ScopeKind::Realm) {
                // Ascending ids; houses that died out are not part of the realm any more.
                std::size_t taken = 0;
                for (std::size_t i = 0; i < world_.dynasties().size(); ++i) {
                    const DynastyId d{static_cast<std::uint32_t>(i)};
                    if (!world_.exists(d) || world_.dynasty_extinct(d)) {
                        continue;
                    }
                    if (taken == cap) {
                        cut = true;
                        break;
                    }
                    out.push_back(dynasty_scope(d));
                    ++taken;
                }
            }
            break;
        default:  // claimant (E12) and the lists of later blocks: empty
            break;
    }
    return cut;
}

core::Pcg32& WorldVmHost::rng() {
    return rng_ != nullptr ? *rng_ : fallback_;
}

std::uint32_t WorldVmHost::cost(Op op, std::uint16_t code) const {
    return catalog_.cost(op, code);
}

std::uint32_t WorldVmHost::list_bound(std::uint16_t code) const {
    return catalog_.list_bound(code);
}

std::string_view WorldVmHost::name(NameSpace space, std::uint16_t code) const {
    return catalog_.name(space, code);
}

}  // namespace pat::sim::events
