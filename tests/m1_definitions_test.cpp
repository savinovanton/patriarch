#include "pat/content/load_errors.hpp"
#include "pat/content/m1_content.hpp"
#include "pat/content/province_bindings.hpp"
#include "pat/data/content_schemas.hpp"
#include "pat/loc/localizer.hpp"
#include "pat/mapdata/map_format.hpp"
#include "pat/mapdata/map_gen.hpp"
#include "pat/mapdata/map_validate.hpp"

#include <doctest/doctest.h>

#include <string>

TEST_CASE("M1 definitions: shipped data is complete, localized and matches a procedural PMD1 map") {
    const pat::data::DirectorySource root{std::string(PATRIARCH_DATA_DIR)};
    const auto loaded = pat::data::load_content(root, pat::data::content_schemas());
    INFO(pat::content::format_errors(loaded.errors));
    REQUIRE(loaded.ok());
    const auto errors = pat::content::validate_m1_definitions(loaded.registry);
    INFO(pat::content::format_errors(errors));
    CHECK(errors.empty());
    for (const auto& [kind, count] : {std::pair{"province", 20U},
                                      std::pair{"dynasty", 4U},
                                      std::pair{"skill", 5U},
                                      std::pair{"trait", 25U}}) {
        const auto* table = loaded.registry.table(kind);
        REQUIRE(table != nullptr);
        CHECK(table->size() == count);
    }
    auto localized = pat::loc::load_localization(root);
    REQUIRE(localized.ok());
    for (const std::string_view kind : {"province", "dynasty", "skill", "trait"}) {
        for (const auto& object : loaded.registry.table(kind)->objects()) {
            for (const std::string_view name : {"name_key", "desc_key"}) {
                if (const auto* value = object.object.body.find(name)) {
                    const auto* key = value->get_if<std::string>();
                    REQUIRE(key != nullptr);
                    INFO(object.object.kind, ":", object.object.id, " ", *key);
                    CHECK(localized.localizer.contains("ru", *key));
                }
            }
        }
    }
    pat::mapdata::GenParams params;
    params.width = 256;
    params.height = 128;
    params.province_count = 20;
    params.seed = 7;
    params.threads = 1;
    const auto map = pat::mapdata::generate_map(params);
    REQUIRE(pat::mapdata::validate_map(map));
    const auto decoded = pat::mapdata::deserialize_map(pat::mapdata::serialize_map(map));
    REQUIRE(decoded.has_value());
    auto bindings = pat::content::bind_pmd1_provinces(loaded.registry, decoded.value());
    if (!bindings) {
        INFO(pat::content::format_errors(bindings.error()));
        REQUIRE(bindings.has_value());
    }
    CHECK(bindings.value().size() == 20);
    const auto again = pat::data::load_content(root, pat::data::content_schemas());
    REQUIRE(again.ok());
    CHECK(loaded.registry.content_hash() == again.registry.content_hash());
}
