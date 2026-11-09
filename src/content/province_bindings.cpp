#include "pat/content/province_bindings.hpp"

#include <algorithm>
#include <format>
#include <limits>
#include <map>
#include <set>
#include <tuple>
#include <utility>

namespace pat::content {
namespace {

data::LoadError map_error(std::string text) {
    return {0, "map", {}, {{0, 0}, data::ErrorCode::ContentRule, std::move(text)}};
}

void sort_errors(std::vector<data::LoadError>& errors) {
    std::sort(errors.begin(), errors.end(), [](const auto& a, const auto& b) {
        return std::tie(
                   a.root, a.file, a.diagnostic.pos.line, a.diagnostic.pos.column, a.diagnostic.message) <
               std::tie(b.root, b.file, b.diagnostic.pos.line, b.diagnostic.pos.column, b.diagnostic.message);
    });
}

}  // namespace

ProvinceBindingsResult bind_provinces(const data::ContentRegistry& registry,
                                      std::span<const ProvinceMapKey> keys) {
    std::vector<data::LoadError> errors;
    std::vector<ProvinceBinding> bindings;
    std::map<std::string, std::uint16_t, std::less<>> seen_keys;
    std::set<std::uint16_t> seen_ids;
    for (const ProvinceMapKey& entry : keys) {
        if (entry.map_id == 0) {
            errors.push_back(map_error("map province id 0 is reserved for water"));
        }
        if (entry.key.empty()) {
            errors.push_back(map_error("map province key must not be empty"));
        }
        if (!seen_ids.insert(entry.map_id).second) {
            errors.push_back(map_error(std::format("duplicate map province id {}", entry.map_id)));
        }
        if (!seen_keys.emplace(entry.key, entry.map_id).second) {
            errors.push_back(map_error(std::format("duplicate map province key '{}'", entry.key)));
        }
        const auto* definition = registry.find("province", entry.key);
        if (definition == nullptr) {
            errors.push_back(
                map_error(std::format("map province '{}' has no content definition", entry.key)));
        } else {
            bindings.push_back({entry.map_id, definition->id});
        }
    }
    if (const auto* provinces = registry.table("province")) {
        for (const auto& definition : provinces->objects()) {
            if (!seen_keys.contains(definition.object.id)) {
                errors.push_back(
                    {definition.root,
                     registry.root_name(definition.root),
                     definition.file,
                     {definition.object.pos,
                      data::ErrorCode::ContentRule,
                      std::format("province definition '{}' has no map province", definition.object.id)}});
            }
        }
    }
    if (!errors.empty()) {
        sort_errors(errors);
        return ProvinceBindingsResult::err(std::move(errors));
    }
    std::sort(
        bindings.begin(), bindings.end(), [](const auto& a, const auto& b) { return a.map_id < b.map_id; });
    return ProvinceBindingsResult::ok(std::move(bindings));
}

ProvinceBindingsResult bind_pmd1_provinces(const data::ContentRegistry& registry,
                                           const mapdata::MapData& map) {
    if (map.province_list.empty() || map.province_list.size() > std::numeric_limits<std::uint16_t>::max()) {
        return ProvinceBindingsResult::err({map_error("PMD1 province count must be in 1..65535")});
    }
    std::vector<ProvinceMapKey> keys;
    keys.reserve(map.province_list.size());
    for (std::size_t i = 0; i < map.province_list.size(); ++i) {
        const auto id = static_cast<std::uint16_t>(i + 1);
        if (map.province_list[i].id != id) {
            return ProvinceBindingsResult::err(
                {map_error("PMD1 province table must contain ids 1..N in order")});
        }
        keys.push_back({id, std::format("p{:04}", id)});
    }
    return bind_provinces(registry, keys);
}

}  // namespace pat::content
