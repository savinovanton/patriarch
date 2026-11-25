#include "pat/sim/events/world_host.hpp"

#include "pat/sim/economy/tables.hpp"
#include "pat/sim/family/tables.hpp"
#include "pat/sim/kinship.hpp"

#include <algorithm>
#include <cstddef>

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
    return false;
}

bool WorldVmHost::list(Scope scope, std::uint16_t code, std::uint32_t limit, std::vector<Scope>& out) {
    const std::size_t before = out.size();
    const auto room = [&] { return out.size() - before < limit; };
    bool cut = false;
    const auto add = [&](Scope s) {
        if (!room()) {
            cut = true;
            return false;
        }
        out.push_back(s);
        return true;
    };
    switch (code) {
        case kListLivingMember:
            if (scope.kind == ScopeKind::Dynasty && world_.exists(DynastyId{scope.id})) {
                std::vector<CharacterId> members(world_.members_of(DynastyId{scope.id}).begin(),
                                                 world_.members_of(DynastyId{scope.id}).end());
                std::ranges::sort(members);
                for (const CharacterId m : members) {
                    if (!add(character_scope(m))) {
                        break;
                    }
                }
            }
            break;
        case kListChild:
        case kListSibling:
            if (scope.kind == ScopeKind::Character && world_.is_alive(CharacterId{scope.id})) {
                const CharacterId id{scope.id};
                const std::vector<CharacterId> kin =
                    code == kListChild ? kin::children_of(world_, id) : kin::siblings_of(world_, id);
                std::vector<CharacterId> alive;
                for (const CharacterId c : kin) {
                    if (world_.is_alive(c)) {
                        alive.push_back(c);
                    }
                }
                std::ranges::sort(alive);
                for (const CharacterId c : alive) {
                    if (!add(character_scope(c))) {
                        break;
                    }
                }
            }
            break;
        case kListOwnedProvince:
            if (scope.kind == ScopeKind::Dynasty) {
                const DynastyId d{scope.id};
                const std::vector<DynastyId>& owner = world_.provinces().owner;
                for (std::size_t i = 0; i < owner.size(); ++i) {
                    if (owner[i] == d &&
                        !add(Scope::make(ScopeKind::Province, static_cast<std::uint32_t>(i)))) {
                        break;
                    }
                }
            }
            break;
        case kListRealmDynasty:
            if (scope.kind == ScopeKind::Realm) {
                for (std::size_t i = 0; i < world_.dynasties().size(); ++i) {
                    if (!add(Scope::make(ScopeKind::Dynasty, static_cast<std::uint32_t>(i)))) {
                        break;
                    }
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
