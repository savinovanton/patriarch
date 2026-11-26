#include "pat/content/army_content.hpp"

#include "pat/data/army_schemas.hpp"
#include "pat/data/content_schemas.hpp"

#include <algorithm>
#include <format>
#include <map>
#include <numeric>
#include <utility>

#include "typed_accessors.hpp"

namespace pat::content {
namespace {
using data::ContentObject;
using data::TypedBlock;
using sim::army::ArmyRules;
using sim::army::kBranchNames;
using sim::army::kTerrainNames;

template <std::size_t N>
std::size_t index_of(const std::array<std::string_view, N>& names, std::string_view name) {
    const auto it = std::ranges::find(names, name);
    return static_cast<std::size_t>(it - names.begin());
}
std::string reference(const TypedBlock& block, std::string_view field) {
    const auto* ref = detail::value_of<data::RefValue>(block, field);
    return ref == nullptr ? std::string{} : ref->id;
}
std::string text(const TypedBlock& block, std::string_view field) {
    const auto* value = detail::value_of<std::string>(block, field);
    return value == nullptr ? std::string{} : *value;
}
std::size_t enum_index(const TypedBlock& block, std::string_view field) {
    const auto* value = detail::value_of<data::EnumValue>(block, field);
    return value == nullptr ? 0 : value->index;
}
// Labelled/repeated fields need a more precise location than the ordinary dotted block accessor.
data::SourcePos rule_position(const ContentObject& object, std::string_view path) {
    const auto first = path.find('.');
    const auto kind = path.substr(0, first);
    if (first != std::string_view::npos && (kind == "branch" || kind == "terrain" || kind == "season")) {
        path.remove_prefix(first + 1);
        const auto second = path.find('.');
        const auto label = path.substr(0, second);
        for (const auto& field : object.object.body.fields) {
            if (field.name != kind)
                continue;
            const auto* block = field.value.get_if<TypedBlock>();
            const bool matches = kind == "season" ? std::to_string(detail::int_of(*block, "month")) == label
                                                  : field.label == label;
            if (matches)
                return second == std::string_view::npos
                           ? field.pos
                           : detail::key_pos(*block, path.substr(second + 1), field.pos);
        }
        return object.object.pos;
    }
    return detail::path_position(object, path);
}

class RulesBuilder {
public:
    explicit RulesBuilder(const data::ContentRegistry& registry) : errors_(registry) {}
    ArmyRulesResult build() {
        const auto* object = errors_.single_object(data::kArmyRulesKind);
        if (object != nullptr) {
            read(*object);
            if (errors_.empty()) {
                if (const auto checked = sim::army::validate(rules_); !checked)
                    errors_.fail(
                        *object, rule_position(*object, checked.error().field), checked.error().message);
            }
        }
        if (!errors_.empty())
            return ArmyRulesResult::err(errors_.take());
        return ArmyRulesResult::ok(std::move(rules_));
    }

private:
    ErrorSink errors_;
    ArmyRules rules_;
    void read(const ContentObject& object) {
        const auto& body = object.object.body;
#define PAT_ARMY_READ(group, name, def, lo, hi)                                                              \
    if (const auto* b = detail::block_of(body, #group))                                                      \
        rules_.group##_##name = detail::i32_of(*b, #name);
        PAT_ARMY_BALANCE_FIELDS(PAT_ARMY_READ)
#undef PAT_ARMY_READ
        std::array<bool, 4> branches{};
        std::array<bool, 6> terrains{};
        std::array<bool, 12> seasons{};
        for (const auto& field : body.fields) {
            const auto* b = field.value.get_if<TypedBlock>();
            if (b == nullptr)
                continue;
            if (field.name == "branch") {
                const auto i = index_of(kBranchNames, field.label);
                if (i == kBranchNames.size()) {
                    errors_.fail(object, field.pos, "unknown army branch '" + field.label + "'");
                    continue;
                }
                branches[i] = true;
                auto& branch = rules_.branches[i];
                branch = {detail::i32_of(*b, "cv"),
                          detail::i32_of(*b, "speed_kmd"),
                          detail::i32_of(*b, "upkeep_kop"),
                          detail::i32_of(*b, "equip_kop"),
                          detail::i32_of(*b, "vuln"),
                          detail::i32_of(*b, "need"),
                          detail::i32_of(*b, "siege_value"),
                          detail::i32_of(*b, "train_days")};
            } else if (field.name == "terrain") {
                const auto i = index_of(kTerrainNames, field.label);
                if (i == kTerrainNames.size()) {
                    errors_.fail(object, field.pos, "unknown army terrain '" + field.label + "'");
                    continue;
                }
                terrains[i] = true;
                rules_.terrains[i] = {detail::i32_of(*b, "move_pm"),
                                      detail::i32_of(*b, "defence_pm"),
                                      detail::i32_of(*b, "supply_pm"),
                                      static_cast<sim::army::CounterClass>(enum_index(*b, "class"))};
            } else if (field.name == "season") {
                const auto i = static_cast<std::size_t>(detail::int_of(*b, "month") - 1);
                if (seasons[i])
                    errors_.fail(object,
                                 detail::key_pos(*b, "month", field.pos),
                                 std::format("month {} is repeated", i + 1));
                seasons[i] = true;
                rules_.seasons[i] = {detail::i32_of(*b, "move_pm"), detail::i32_of(*b, "supply_pm")};
            }
        }
        for (std::size_t i = 0; i < branches.size(); ++i)
            if (!branches[i])
                errors_.fail(
                    object, object.object.pos, std::format("missing army branch '{}'", kBranchNames[i]));
        for (std::size_t i = 0; i < terrains.size(); ++i)
            if (!terrains[i])
                errors_.fail(
                    object, object.object.pos, std::format("missing army terrain '{}'", kTerrainNames[i]));
        for (std::size_t i = 0; i < seasons.size(); ++i)
            if (!seasons[i])
                errors_.fail(object, object.object.pos, std::format("missing army month {}", i + 1));
        if (const auto* counter = detail::block_of(body, "counter")) {
            for (std::size_t c = 0; c < 2; ++c) {
                const auto* rows = detail::block_of(*counter, c == 0 ? "open" : "rough");
                if (rows == nullptr)
                    continue;
                for (std::size_t i = 0; i < 4; ++i) {
                    const auto* cols = detail::block_of(*rows, kBranchNames[i]);
                    if (cols == nullptr)
                        continue;
                    for (std::size_t j = 0; j < 4; ++j)
                        rules_.counter[c][i][j] = detail::i32_of(*cols, kBranchNames[j]);
                }
            }
        }
        if (const auto* b = detail::block_of(body, "battle"))
            detail::read_i32_array(errors_, object, *b, "sigmoid", rules_.sigmoid, "battle.sigmoid");
        if (const auto* b = detail::block_of(body, "siege"))
            detail::read_i32_array(errors_, object, *b, "defence", rules_.siege_defence, "siege.defence");
        if (const auto* b = detail::block_of(body, "front"))
            detail::read_i32_array(
                errors_, object, *b, "enemy_base", rules_.front_enemy_base, "front.enemy_base");
        if (const auto* b = detail::block_of(body, "upkeep")) {
            detail::read_i32_array(
                errors_, object, *b, "fortress", rules_.fortress_upkeep, "upkeep.fortress");
            const auto* list = detail::list_of(*b, "arrears");
            if (list == nullptr || list->size() != rules_.arrears.size())
                errors_.fail(object,
                             detail::path_position(object, "upkeep.arrears"),
                             "upkeep.arrears needs exactly 4 blocks");
            else
                for (std::size_t i = 0; i < list->size(); ++i) {
                    const auto* row = (*list)[i].get_if<TypedBlock>();
                    if (row != nullptr)
                        rules_.arrears[i] = {detail::i32_of(*row, "morale"), detail::i32_of(*row, "desert")};
                }
        }
    }
};
}  // namespace
ArmyRulesResult build_army_rules(const data::ContentRegistry& registry) {
    return RulesBuilder(registry).build();
}

MilitarySetupResult build_military_setup(const data::ContentRegistry& registry, const ArmyRules& rules) {
    ErrorSink errors(registry);
    if (const auto checked = sim::army::validate(rules); !checked) {
        errors.fail_general("invalid army rules: " + checked.error().message);
        return MilitarySetupResult::err(errors.take());
    }
    const auto* object = errors.single_object(data::kMilitarySetupKind);
    if (object == nullptr)
        return MilitarySetupResult::err(errors.take());
    const auto& body = object->object.body;
    MilitarySetup setup;
    setup.capital = reference(body, "capital");
    setup.crown_manpower = detail::i32_of(body, "crown_manpower");
    std::map<std::string, std::size_t, std::less<>> provinces;
    std::map<std::string, std::int32_t, std::less<>> owner_counts;
    std::int32_t guard_count = 0;
    for (const auto& field : body.fields) {
        const auto* b = field.value.get_if<TypedBlock>();
        if (b == nullptr)
            continue;
        if (field.name == "province") {
            MilitaryProvince p;
            p.province = reference(*b, "id");
            p.terrain = static_cast<sim::army::Terrain>(enum_index(*b, "terrain") + 1);
            if (!provinces.emplace(p.province, setup.provinces.size()).second)
                errors.fail(*object, field.pos, "duplicate military province '" + p.province + "'");
            if (const auto* f = detail::block_of(*b, "fortress")) {
                p.has_fortress = true;
                p.fortress = {detail::i32_of(*f, "level"),
                              detail::i32_of(*f, "garrison"),
                              detail::i32_of(*f, "stores_days"),
                              detail::i32_of(*f, "wall_pm"),
                              enum_index(*f, "holder") == 1};
            }
            setup.provinces.push_back(std::move(p));
        } else if (field.name == "army") {
            MilitaryArmy a;
            a.id = field.label;
            a.name_key = text(*b, "name_key");
            a.dynasty = reference(*b, "dynasty");
            a.province = reference(*b, "province");
            a.owner = static_cast<MilitaryOwner>(enum_index(*b, "owner") + 1);
            a.tag = static_cast<MilitaryTag>(enum_index(*b, "tag") + 1);
            if (const auto* comp = detail::block_of(*b, "comp"))
                for (std::size_t i = 0; i < 4; ++i)
                    a.comp[i] = detail::i32_of(*comp, kBranchNames[i]);
            a.quality = detail::i32_of(*b, "quality");
            a.experience = detail::i32_of(*b, "experience");
            a.morale_x100 = detail::i32_of(*b, "morale_x100");
            a.supply_x100 = detail::i32_of(*b, "supply_x100");
            if (b->find("commander_office") != nullptr) {
                constexpr std::array<std::string_view, 3> offices{
                    "general", "guards_colonel", "fortress_commandant"};
                a.commander_office = offices[enum_index(*b, "commander_office")];
            }
            const auto fail = [&](std::string_view key, std::string message) {
                errors.fail(*object, detail::key_pos(*b, key, field.pos), std::move(message));
            };
            const auto size = std::accumulate(a.comp.begin(), a.comp.end(), std::int64_t{0});
            const auto minimum =
                a.tag == MilitaryTag::Levy
                    ? rules.levy_min_size
                    : (a.owner == MilitaryOwner::Rebels ? rules.limits_min_rebels : rules.limits_min_size);
            if (size < minimum || size > rules.limits_max_size)
                fail("comp", "initial army size is outside the rules limits");
            if ((a.owner == MilitaryOwner::Dynasty) != !a.dynasty.empty())
                fail("dynasty", "dynasty reference is required exactly for a dynasty-owned army");
            if ((a.tag == MilitaryTag::Insurgents) != (a.owner == MilitaryOwner::Rebels))
                fail("tag", "insurgents tag and rebel ownership must coincide");
            if (a.tag == MilitaryTag::Levy && a.owner != MilitaryOwner::Dynasty)
                fail("tag", "levy must belong to a dynasty");
            if (a.tag == MilitaryTag::Guard) {
                if (a.owner != MilitaryOwner::Crown || a.province != setup.capital)
                    fail("tag", "guard must belong to the crown and stand in the capital");
                if (++guard_count > 1)
                    fail("tag", "M1 has at most one guard army");
            }
            if (!a.commander_office.empty() && a.owner != MilitaryOwner::Crown)
                fail("commander_office", "office commander requires crown ownership");
            const auto owner_key = a.owner == MilitaryOwner::Dynasty
                                       ? "dynasty:" + a.dynasty
                                       : (a.owner == MilitaryOwner::Crown ? "crown" : "rebels");
            const auto limit =
                a.owner == MilitaryOwner::Dynasty
                    ? rules.limits_dynasty
                    : (a.owner == MilitaryOwner::Crown ? rules.limits_crown : rules.limits_rebels);
            if (++owner_counts[owner_key] > limit)
                fail("owner", "initial armies exceed their owner's limit");
            if (setup.armies.size() >= static_cast<std::size_t>(rules.limits_world))
                fail("owner", "initial armies exceed the world limit");
            setup.armies.push_back(std::move(a));
        }
    }
    if (const auto* definitions = registry.table("province"))
        for (const auto& p : definitions->objects()) {
            if (!provinces.contains(p.object.id))
                errors.fail(*object,
                            object->object.pos,
                            "missing military terrain for province '" + p.object.id + "'");
        }
    if (!provinces.contains(setup.capital))
        errors.fail(*object, detail::field_pos(*object, "capital"), "capital has no military province entry");
    for (const auto& a : setup.armies)
        if (!provinces.contains(a.province))
            errors.fail(*object, object->object.pos, "army '" + a.id + "' has no military province entry");
    if (const auto* f = detail::block_of(body, "front")) {
        setup.front = {text(*f, "id"),
                       text(*f, "name_key"),
                       static_cast<MilitaryFrontState>(enum_index(*f, "state") + 1),
                       detail::i32_of(*f, "pressure_x100"),
                       detail::i32_of(*f, "enemy_x100"),
                       detail::i32_of(*f, "fatigue_x100")};
        if (setup.front.id != "north")
            errors.fail(
                *object, detail::key_pos(*f, "id", object->object.pos), "M1 requires the single north front");
    }
    if (!errors.empty())
        return MilitarySetupResult::err(errors.take());
    std::ranges::sort(setup.provinces, {}, &MilitaryProvince::province);
    std::ranges::sort(setup.armies, {}, &MilitaryArmy::id);
    return MilitarySetupResult::ok(std::move(setup));
}

ArmyContentResult load_army_content(std::span<const data::ContentSource* const> roots) {
    auto loaded = data::load_content(roots, data::content_schemas());
    if (!loaded.ok())
        return ArmyContentResult::err(std::move(loaded.errors));
    auto rules = build_army_rules(loaded.registry);
    if (!rules)
        return ArmyContentResult::err(std::move(rules).error());
    auto military = build_military_setup(loaded.registry, rules.value());
    if (!military)
        return ArmyContentResult::err(std::move(military).error());
    const auto hash = rules.value().hash();
    return ArmyContentResult::ok({std::move(rules).value(), std::move(military).value(), hash});
}
ArmyContentResult load_army_content(const data::ContentSource& root) {
    const data::ContentSource* roots[] = {&root};
    return load_army_content(roots);
}
}  // namespace pat::content
