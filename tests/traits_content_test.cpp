#include "pat/content/load_errors.hpp"
#include "pat/content/m1_hot_reload.hpp"
#include "pat/content/traits_content.hpp"
#include "pat/data/content_schemas.hpp"
#include "pat/mapdata/map_gen.hpp"

#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace {

using pat::core::Fixed32;

const char* kCustomTraits = R"(format_version = 1
skill "custom_skill" { slot = 4 name_key = "skill.name" desc_key = "skill.desc" }
trait "z_first" {
    slot = 3 category = congenital name_key = "first.name" desc_key = "first.desc"
    opposite = trait:a_second
    skills = { custom_skill = 3 }
    modifiers = { health = 1 fertility = 2 stress = 3 piety = 4 ambition = 5
                  loyalty_head = 6 loyalty_emperor = 7 }
    health_multiplier_permille = 1100
    fertility_multiplier_permille = 800
    mortality_multiplier_permille = 1500
    inherit_chance = 0.5
    ai_hint = { z_last = -7 a_first = 9 }
}
trait "a_second" {
    slot = 600 category = religious name_key = "second.name" desc_key = "second.desc"
    opposite = trait:z_first
})";

}  // namespace

TEST_CASE("trait content: all shipped definitions retain stable slots, effects and AI hints") {
    const pat::data::DirectorySource source{std::string(PATRIARCH_DATA_DIR)};
    const auto loaded = pat::data::load_content(source, pat::data::content_schemas());
    INFO(pat::content::format_errors(loaded.errors));
    REQUIRE(loaded.ok());
    const auto result = pat::content::build_trait_catalog(loaded.registry);
    if (!result) {
        INFO(pat::content::format_errors(result.error()));
        REQUIRE(result.has_value());
    }
    const auto& catalog = result.value();
    REQUIRE(catalog.definitions.size() == 25);
    CHECK(pat::sim::validate(catalog));
    for (std::size_t i = 0; i < catalog.definitions.size(); ++i) {
        const auto& def = catalog.definitions[i];
        CHECK(def.id == i + 1);
        const auto* object = loaded.registry.find("trait", def.key);
        REQUIRE(object != nullptr);
        CHECK(*object->object.body.find("slot")->get_if<std::int64_t>() == def.id);
        CHECK(object->object.body.find("health_multiplier_permille") == nullptr);
        CHECK(object->object.body.find("fertility_multiplier_permille") == nullptr);
        CHECK(object->object.body.find("mortality_multiplier_permille") == nullptr);
        CHECK(def.health_multiplier_permille == 1000);
        CHECK(def.fertility_multiplier_permille == 1000);
        CHECK(def.mortality_multiplier_permille == 1000);
    }
    const auto& ambitious = *catalog.find(1);
    CHECK(ambitious.key == "ambitious");
    CHECK(ambitious.name_key == "trait.ambitious.name");
    CHECK(ambitious.desc_key == "trait.ambitious.desc");
    CHECK(ambitious.category == pat::sim::TraitCategory::Personality);
    CHECK(ambitious.skill_modifiers[1] == Fixed32::from_int(1));
    CHECK(ambitious.skill_modifiers[3] == Fixed32::from_int(2));
    CHECK(ambitious.attribute_modifiers[4] == Fixed32::from_int(30));
    const std::vector<std::pair<std::string, std::int32_t>> hints{{"joins_plots", 1}, {"seeks_promotion", 2}};
    CHECK(ambitious.ai_hint == hints);
    CHECK(catalog.find(3)->opposite == 4);
    CHECK(catalog.find(4)->opposite == 3);
    CHECK(catalog.find(18)->key == "robust");
    CHECK(catalog.find(18)->inherit_chance == Fixed32::from_ratio(4, 10));
    CHECK(catalog.find(25)->key == "widowed");
}

TEST_CASE("trait content: custom skill slots, sparse trait ids and explicit multipliers are retained") {
    pat::data::MemorySource source;
    source.add("traits.pd", kCustomTraits);
    const auto loaded = pat::data::load_content(source, pat::data::content_schemas());
    INFO(pat::content::format_errors(loaded.errors));
    REQUIRE(loaded.ok());
    const auto result = pat::content::build_trait_catalog(loaded.registry);
    REQUIRE(result.has_value());
    const auto& catalog = result.value();
    REQUIRE(catalog.definitions.size() == 2);
    CHECK(catalog.definitions[0].id == 3);
    CHECK(catalog.definitions[1].id == 600);
    const auto& first = *catalog.find(3);
    CHECK(first.category == pat::sim::TraitCategory::Congenital);
    CHECK(first.opposite == 600);
    CHECK(first.skill_modifiers[4] == Fixed32::from_int(3));
    for (std::size_t i = 0; i < 4; ++i) {
        CHECK(first.skill_modifiers[i] == Fixed32::zero());
    }
    for (std::size_t i = 0; i < first.attribute_modifiers.size(); ++i) {
        CHECK(first.attribute_modifiers[i] == Fixed32::from_int(static_cast<std::int32_t>(i + 1)));
    }
    CHECK(first.health_multiplier_permille == 1100);
    CHECK(first.fertility_multiplier_permille == 800);
    CHECK(first.mortality_multiplier_permille == 1500);
    CHECK(first.inherit_chance == Fixed32::from_ratio(1, 2));
    const std::vector<std::pair<std::string, std::int32_t>> hints{{"a_first", 9}, {"z_last", -7}};
    CHECK(first.ai_hint == hints);
    CHECK(catalog.find(600)->category == pat::sim::TraitCategory::Religious);
    CHECK(catalog.find(600)->health_multiplier_permille == 1000);
}

TEST_CASE("trait content: unknown modifiers report the source key and suppress the catalogue") {
    pat::data::MemorySource source("trait-mod");
    source.add("traits/custom.pd", R"(format_version = 1
trait "bad" {
    slot = 1 category = personality name_key = "bad.name" desc_key = "bad.desc"
    modifiers = {
        healt = 2
    }
})");
    const auto loaded = pat::data::load_content(source, pat::data::content_schemas());
    REQUIRE(loaded.ok());
    const auto result = pat::content::build_trait_catalog(loaded.registry);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().size() == 1);
    const auto& error = result.error().front();
    CHECK(error.root_name == "trait-mod");
    CHECK(error.file == "traits/custom.pd");
    CHECK(error.diagnostic.pos.line == 5);
    CHECK(error.diagnostic.pos.column == 9);
    CHECK(error.diagnostic.code == pat::data::ErrorCode::ContentRule);
    CHECK(error.diagnostic.message.find("healt") != std::string::npos);
}

TEST_CASE("trait content: duplicate stable slots and asymmetric opposites are rejected") {
    pat::data::MemorySource source;
    source.add("traits.pd", R"(format_version = 1
trait "a" { slot = 1 category = personality name_key = "a" desc_key = "a" opposite = trait:b }
trait "b" { slot = 1 category = personality name_key = "b" desc_key = "b" }
)");
    const auto loaded = pat::data::load_content(source, pat::data::content_schemas());
    REQUIRE(loaded.ok());
    const auto result = pat::content::build_trait_catalog(loaded.registry);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().size() == 2);
    CHECK(result.error()[0].diagnostic.message.find("reciprocal") != std::string::npos);
    CHECK(result.error()[1].diagnostic.message.find("already used") != std::string::npos);
}

TEST_CASE("trait content: a registry without traits has a valid empty catalogue") {
    const auto result = pat::content::build_trait_catalog(pat::data::ContentRegistry{});
    REQUIRE(result.has_value());
    CHECK(result.value().definitions.empty());
    CHECK(pat::sim::validate(result.value()));
}

TEST_CASE("trait content: M1 reload publishes validated catalogues and retains the last valid one") {
    const pat::data::DirectorySource shipped{std::string(PATRIARCH_DATA_DIR)};
    pat::data::MemorySource source("traits-hot-reload");
    const auto files = shipped.list_files();
    REQUIRE(files.has_value());
    for (const auto& path : files.value()) {
        const auto text = shipped.read_file(path);
        REQUIRE(text.has_value());
        source.add(path, text.value());
    }
    const auto map = pat::mapdata::generate_map(pat::content::m1_map_params());
    pat::content::M1HotReload reload(source, map);
    const auto initial = reload.poll();
    if (!initial) {
        INFO(initial.error().message);
        REQUIRE(initial.has_value());
    }
    REQUIRE(initial.value());
    const auto snapshot = reload.current();
    REQUIRE(snapshot->traits != nullptr);
    CHECK(snapshot->traits->definitions.size() == 25);

    auto traits_text = source.read_file("traits/m1.pd").value();
    const auto key = traits_text.find("ambition = 30");
    REQUIRE(key != std::string::npos);
    traits_text.replace(key, std::string("ambition = 30").size(), "unknown_attribute = 30");
    source.add("traits/m1.pd", std::move(traits_text));
    const auto invalid = reload.poll();
    REQUIRE_FALSE(invalid.has_value());
    CHECK(invalid.error().message.find("unknown_attribute") != std::string::npos);
    CHECK(reload.current() == snapshot);
    CHECK(snapshot->traits->find(1)->attribute_modifiers[4] == Fixed32::from_int(30));
}
