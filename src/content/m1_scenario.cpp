#include "pat/content/m1_scenario.hpp"

#include "pat/content/economy_content.hpp"
#include "pat/content/load_errors.hpp"
#include "pat/content/m1_content.hpp"
#include "pat/content/province_bindings.hpp"
#include "pat/core/hash_stream.hpp"
#include "pat/data/content_schemas.hpp"
#include "pat/loc/localizer.hpp"
#include "pat/mapdata/map_validate.hpp"

#include <format>
#include <map>
#include <string_view>
#include <utility>

namespace pat::content {

namespace {

using ScenarioResult = core::Result<M1Scenario>;

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

core::Result<M1Scenario>
load_m1_scenario(const std::filesystem::path& root, const mapdata::MapData& map, std::uint64_t seed) {
    const data::DirectorySource source(root);
    auto loaded = data::load_content(source, data::content_schemas());
    if (!loaded.ok()) {
        return fail(format_errors(loaded.errors));
    }
    const auto errors = validate_m1_definitions(loaded.registry);
    if (!errors.empty()) {
        return fail(format_errors(errors));
    }
    for (const std::string_view kind : {"dynasty", "skill", "trait"}) {
        if (const auto* table = loaded.registry.table(kind); table == nullptr || table->empty()) {
            return fail(std::format("M1 requires at least one '{}' definition", kind));
        }
    }
    const auto bindings = bind_pmd1_provinces(loaded.registry, map);
    if (!bindings) {
        return fail(format_errors(bindings.error()));
    }
    if (const auto valid = mapdata::validate_map(map); !valid) {
        return fail(std::format("invalid M1 map: {}", valid.error().message));
    }
    const auto economy = build_economy_rules(loaded.registry);
    if (!economy) {
        return fail(format_errors(economy.error()));
    }
    if (economy.value().crown_account_mode != sim::economy::CrownAccountMode::Separate) {
        return fail("M1 needs a separate crown account until the scenario defines an emperor dynasty");
    }
    const auto localized = loc::load_localization(source);
    if (!localized.ok()) {
        std::string messages;
        for (const auto& error : localized.errors) {
            messages += error.format() + '\n';
        }
        return fail(std::move(messages));
    }
    for (const std::string_view kind : {"province", "dynasty", "skill", "trait"}) {
        for (const auto& object : loaded.registry.table(kind)->objects()) {
            for (const std::string_view field : {"name_key", "desc_key"}) {
                if (const auto* value = object.object.body.find(field)) {
                    const auto& key = *value->get_if<std::string>();
                    if (!localized.localizer.contains("ru", key)) {
                        return fail(std::format("{}:{}:{}: missing Russian localisation key '{}'",
                                                object.file,
                                                value->pos.line,
                                                value->pos.column,
                                                key));
                    }
                }
            }
        }
    }

    sim::World world(sim::Date::from_ymd(1703, 1, 1), seed);
    if (!world.enable_dynasty_mode() || !world.enable_economy()) {
        return fail("cannot initialize M1 world modes");
    }
    const std::uint64_t content_hash = loaded.registry.content_hash();
    world.set_economy_rules_hash(content_hash);

    std::map<std::string_view, sim::DynastyId, std::less<>> dynasties;
    for (const auto& definition : loaded.registry.table("dynasty")->objects()) {
        dynasties.emplace(definition.object.id, world.create_dynasty(definition.id));
    }
    const auto* provinces = loaded.registry.table("province");
    std::vector<sim::ProvinceId> map_to_world(map.province_list.size() + 1);
    std::vector<std::string> province_names(map_to_world.size());
    for (const ProvinceBinding& binding : bindings.value()) {
        const auto& definition = *provinces->find(binding.definition_id, loaded.registry.ids());
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
        const auto& name_key = *fields.find("name_key")->get_if<std::string>();
        auto& name = province_names[binding.map_id];
        if (!localized.localizer.format_to(name, name_key).ok() || name.empty()) {
            return fail(
                std::format("province name '{}' must be a nonempty text without arguments", name_key));
        }
        const sim::ProvinceId id = world.create_province(definition.id, owner, core::Fixed32::zero());
        world.economy_columns_mut().kind[id.index()] = static_cast<std::uint8_t>(*province_kind);
        map_to_world[binding.map_id] = id;
    }
    return ScenarioResult::ok({std::move(world),
                               replay_id(content_hash),
                               content_hash,
                               std::move(map_to_world),
                               std::move(province_names),
                               std::move(loaded.registry)});
}

}  // namespace pat::content
