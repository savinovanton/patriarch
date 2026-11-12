#include "pat/content/m1_scenario.hpp"

#include "pat/content/demography_content.hpp"
#include "pat/content/economy_content.hpp"
#include "pat/content/load_errors.hpp"
#include "pat/content/m1_content.hpp"
#include "pat/content/m1_economy_content.hpp"
#include "pat/content/province_bindings.hpp"
#include "pat/content/traits_content.hpp"
#include "pat/core/hash_stream.hpp"
#include "pat/data/content_schemas.hpp"
#include "pat/data/demography_schemas.hpp"
#include "pat/data/m1_schemas.hpp"
#include "pat/loc/localizer.hpp"
#include "pat/mapdata/map_validate.hpp"
#include "pat/sim/economy/invariants.hpp"

#include <array>
#include <format>
#include <map>
#include <string_view>
#include <utility>

namespace pat::content {

namespace {

using ScenarioResult = core::Result<M1Scenario>;
using DefinitionsResult = core::Result<M1Definitions>;

ScenarioResult fail(std::string message) {
    return ScenarioResult::err({1, std::move(message)});
}

std::uint64_t replay_id(std::uint64_t content_hash) {
    core::HashStream hash;
    hash.u64(0x31762e316dULL);  // m1.v1
    hash.u64(content_hash);
    return hash.value();
}

}  // namespace

mapdata::GenParams m1_map_params() {
    return {.width = 256, .height = 128, .seed = 7, .province_count = 20, .threads = 1};
}

DefinitionsResult load_m1_definitions(const data::ContentSource& source, const mapdata::MapData& map) {
    const auto fail_definition = [](std::string message) {
        return DefinitionsResult::err({1, std::move(message)});
    };
    auto loaded = data::load_content(source, data::content_schemas());
    if (!loaded.ok()) {
        return fail_definition(format_errors(loaded.errors));
    }
    const auto errors = validate_m1_definitions(loaded.registry);
    if (!errors.empty()) {
        return fail_definition(format_errors(errors));
    }
    for (const std::string_view kind : {"dynasty", "skill", "trait"}) {
        if (const auto* table = loaded.registry.table(kind); table == nullptr || table->empty()) {
            return fail_definition(std::format("M1 requires at least one '{}' definition", kind));
        }
    }
    auto traits = build_trait_catalog(loaded.registry);
    if (!traits) {
        return fail_definition(format_errors(traits.error()));
    }
    auto bindings = bind_pmd1_provinces(loaded.registry, map);
    if (!bindings) {
        return fail_definition(format_errors(bindings.error()));
    }
    auto economy_rules = build_economy_rules(loaded.registry);
    if (!economy_rules) {
        return fail_definition(format_errors(economy_rules.error()));
    }
    if (economy_rules.value().crown_account_mode != sim::economy::CrownAccountMode::Separate) {
        return fail_definition(
            "M1 needs a separate crown account until the scenario defines an emperor dynasty");
    }
    auto economy = build_m1_economy_content(loaded.registry, economy_rules.value());
    if (!economy) {
        return fail_definition(format_errors(economy.error()));
    }
    sim::DemographyRules demography = sim::kDefaultDemography;
    if (const auto* table = loaded.registry.table(data::kDemographyRulesKind); table && !table->empty()) {
        auto rules = build_demography_rules(loaded.registry);
        if (!rules) {
            return fail_definition(format_errors(rules.error()));
        }
        demography = std::move(rules).value();
    }
    auto localized = loc::load_localization(source);
    if (!localized.ok()) {
        std::string messages;
        for (const auto& error : localized.errors) {
            messages += error.format() + '\n';
        }
        return fail_definition(std::move(messages));
    }
    constexpr std::array<std::string_view, 6> localized_kinds{
        "province", "dynasty", "skill", "trait", data::kHonorRankKind, data::kTradeNodeKind};
    for (const std::string_view kind : localized_kinds) {
        const auto* table = loaded.registry.table(kind);
        if (table == nullptr) {
            continue;
        }
        for (const auto& object : table->objects()) {
            for (const std::string_view field : {"name_key", "desc_key"}) {
                if (const auto* value = object.object.body.find(field)) {
                    const auto& key = *value->get_if<std::string>();
                    if (!localized.localizer.contains("ru", key)) {
                        return fail_definition(std::format("{}:{}:{}: missing Russian localisation key '{}'",
                                                           object.file,
                                                           value->pos.line,
                                                           value->pos.column,
                                                           key));
                    }
                    if (kind == data::kHonorRankKind || kind == data::kTradeNodeKind) {
                        std::string name;
                        if (!localized.localizer.format_to(name, key).ok() || name.empty()) {
                            return fail_definition(
                                std::format("{} name '{}' must be a nonempty text without arguments",
                                            kind == data::kHonorRankKind ? "honor rank" : "trade node",
                                            key));
                        }
                    }
                }
            }
        }
    }

    const auto* provinces = loaded.registry.table("province");
    std::vector<std::string> province_names(map.province_list.size() + 1);
    for (const ProvinceBinding& binding : bindings.value()) {
        const auto& definition = *provinces->find(binding.definition_id, loaded.registry.ids());
        const auto& name_key = *definition.object.body.find("name_key")->get_if<std::string>();
        auto& name = province_names[binding.map_id];
        if (!localized.localizer.format_to(name, name_key).ok() || name.empty()) {
            return fail_definition(
                std::format("province name '{}' must be a nonempty text without arguments", name_key));
        }
    }
    auto honor_ranks = build_honor_ranks(loaded.registry);
    return DefinitionsResult::ok(
        {std::move(loaded.registry),
         std::move(bindings).value(),
         std::move(province_names),
         std::move(localized.localizer),
         demography,
         std::move(honor_ranks),
         std::make_shared<const sim::TraitCatalog>(std::move(traits).value()),
         std::make_shared<const sim::economy::EconomyRules>(std::move(economy_rules).value()),
         std::make_shared<const M1EconomyContent>(std::move(economy).value())});
}

core::Result<M1Scenario>
load_m1_scenario(const std::filesystem::path& root, const mapdata::MapData& map, std::uint64_t seed) {
    const data::DirectorySource source(root);
    auto loaded = load_m1_definitions(source, map);
    if (!loaded) {
        return ScenarioResult::err(loaded.error());
    }
    if (const auto valid = mapdata::validate_map(map); !valid) {
        return fail(std::format("invalid M1 map: {}", valid.error().message));
    }
    auto& definitions = loaded.value();

    sim::World world(sim::Date::from_ymd(1703, 1, 1), seed);
    if (!world.enable_dynasty_mode() || !world.enable_economy()) {
        return fail("cannot initialize M1 world modes");
    }
    const std::uint64_t content_hash = definitions.registry.content_hash_excluding(data::kHonorRankKind);
    world.set_economy_rules_hash(content_hash);

    std::map<std::string_view, sim::DynastyId, std::less<>> dynasties;
    for (const auto& definition : definitions.registry.table("dynasty")->objects()) {
        dynasties.emplace(definition.object.id, world.create_dynasty(definition.id));
    }
    const auto* provinces = definitions.registry.table("province");
    std::vector<sim::ProvinceId> map_to_world(map.province_list.size() + 1);
    std::map<std::string_view, sim::ProvinceId, std::less<>> province_ids;
    for (const ProvinceBinding& binding : definitions.bindings) {
        const auto& definition = *provinces->find(binding.definition_id, definitions.registry.ids());
        const auto& fields = definition.object.body;
        sim::DynastyId owner;
        if (const auto* value = fields.find("owner")) {
            const auto found = dynasties.find(value->get_if<data::RefValue>()->id);
            if (found == dynasties.end()) {
                return fail(std::format("unknown owner for province '{}'", definition.object.id));
            }
            owner = found->second;
        }
        const auto& kind = fields.find("kind")->get_if<data::RefValue>()->id;
        const auto province_kind = province_kind_from_key(kind);
        if (!province_kind) {
            return fail(std::format("unknown province kind '{}'", kind));
        }
        const sim::ProvinceId id = world.create_province(definition.id, owner, core::Fixed32::zero());
        world.economy_columns_mut().kind[id.index()] = static_cast<std::uint8_t>(*province_kind);
        map_to_world[binding.map_id] = id;
        province_ids.emplace(definition.object.id, id);
    }

    const sim::economy::EconomyRules& rules = *definitions.economy_rules;
    if (!world.set_economy_policy_levels(static_cast<std::uint8_t>(rules.tax.level_count),
                                         static_cast<std::uint8_t>(rules.recruit.level_count))) {
        return fail("cannot initialize M1 policy levels");
    }
    for (const auto& [key, dynasty] : dynasties) {
        (void)key;
        if (!world.set_dynasty_tax_policy(dynasty, static_cast<std::uint8_t>(rules.tax.default_level)) ||
            !world.set_dynasty_recruit_policy(dynasty,
                                              static_cast<std::uint8_t>(rules.recruit.default_level))) {
            return fail("cannot initialize M1 dynasty policies");
        }
    }

    for (const HoldingDef& holding : definitions.economy->holdings) {
        const auto found = province_ids.find(holding.province);
        if (found == province_ids.end()) {
            return fail(std::format("unknown holding province '{}'", holding.province));
        }
        const sim::ProvinceId id = found->second;
        const std::size_t p = id.index();
        if (holding.owner_kind == sim::economy::OwnerKind::Dynasty) {
            const auto owner = dynasties.find(holding.owner);
            if (owner == dynasties.end()) {
                return fail(std::format("unknown holding owner '{}'", holding.owner));
            }
            world.set_province_owner(id, owner->second);
        } else if (!world.set_province_owner_kind(id, holding.owner_kind)) {
            return fail(std::format("cannot initialize owner of holding '{}'", holding.province));
        }
        world.set_province_development(id, holding.development);
        auto columns = world.economy_columns_mut();
        columns.kind[p] = static_cast<std::uint8_t>(holding.kind);
        columns.slots_total[p] = holding.slots_total;
        columns.trade_node[p] = holding.trade_node;
        columns.control[p] = holding.control;
        columns.corruption[p] = holding.corruption;
        columns.unrest[p] = holding.unrest;
        columns.grain_stock[p] = holding.grain_stock;
        columns.labour_base[p] = holding.population[sim::economy::index_of(sim::economy::Group::Peasants)] +
                                 holding.population[sim::economy::index_of(sim::economy::Group::Townsfolk)];
        for (std::size_t g = 0; g < sim::economy::kGroupCount; ++g) {
            world.set_group_size(id, static_cast<sim::economy::Group>(g), holding.population[g]);
            columns.mood[p * sim::economy::kGroupCount + g] = holding.mood[g];
            columns.wealth[p * sim::economy::kGroupCount + g] = holding.wealth[g];
        }
        for (std::size_t good = 0; good < sim::economy::kMaxGoods; ++good) {
            columns.capacity[p * sim::economy::kMaxGoods + good] = holding.capacity[good];
        }
        for (std::size_t slot = 0; slot < holding.buildings.size(); ++slot) {
            if (!world.set_slot_building(id, slot, holding.buildings[slot])) {
                return fail(std::format("cannot initialize building slot of holding '{}'", holding.province));
            }
        }
        world.invalidate_province_modifiers(id);
    }

    // Every owner kind starts at the loaded policy defaults. Dynasty setters above also establish
    // what newly acquired provinces will inherit, but crown, Church and free holdings have no house.
    auto policy_columns = world.economy_columns_mut();
    for (std::size_t p = 0; p < world.provinces().size(); ++p) {
        policy_columns.tax_policy[p] = static_cast<std::uint8_t>(rules.tax.default_level);
        policy_columns.recruit_policy[p] = static_cast<std::uint8_t>(rules.recruit.default_level);
    }
    world.invalidate_all_province_modifiers();

    if (!world.set_initial_treasury(sim::economy::AccountRef::crown(),
                                    definitions.economy->crown_treasury_kop)) {
        return fail("cannot initialize M1 crown treasury");
    }
    for (const auto& [key, amount] : definitions.economy->dynasty_treasuries) {
        const auto dynasty = dynasties.find(key);
        if (dynasty == dynasties.end() ||
            !world.set_initial_treasury(sim::economy::AccountRef::of(dynasty->second), amount)) {
            return fail(std::format("cannot initialize treasury of dynasty '{}'", key));
        }
    }

    auto problems = world.check_invariants();
    const auto rules_problems = sim::economy::check_rules_invariants(world, rules);
    problems.insert(problems.end(), rules_problems.begin(), rules_problems.end());
    if (!problems.empty()) {
        std::string message = "invalid M1 initial economy:";
        for (const auto& problem : problems) {
            message += '\n' + problem;
        }
        return fail(std::move(message));
    }
    return ScenarioResult::ok({std::move(world),
                               replay_id(content_hash),
                               content_hash,
                               std::move(map_to_world),
                               std::move(definitions.province_names),
                               std::move(definitions.registry),
                               definitions.demography,
                               std::move(definitions.honor_ranks),
                               std::move(definitions.traits),
                               std::move(definitions.economy_rules)});
}

}  // namespace pat::content
