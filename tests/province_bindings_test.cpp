#include "pat/content/province_bindings.hpp"
#include "pat/data/content_source.hpp"

#include <doctest/doctest.h>

#include <string>

namespace {

pat::data::LoadResult provinces(std::string body) {
    pat::data::ObjectSchema schema("province");
    schema.required("name_key", pat::data::Type::string());
    pat::data::SchemaSet schemas;
    schemas.add(std::move(schema));
    pat::data::MemorySource root("test");
    root.add("provinces.pd", "format_version = 1\n" + body);
    return pat::data::load_content(root, schemas);
}

}  // namespace

TEST_CASE("province bindings: exact key matching is independent of map and definition order") {
    auto loaded = provinces(R"(province "b" { name_key = "b" }
province "a" { name_key = "a" })");
    REQUIRE(loaded.ok());
    const pat::content::ProvinceMapKey keys[] = {{2, "a"}, {1, "b"}};
    auto bound = pat::content::bind_provinces(loaded.registry, keys);
    REQUIRE(bound.has_value());
    REQUIRE(bound.value().size() == 2);
    CHECK(bound.value()[0].map_id == 1);
    CHECK(bound.value()[0].definition_id == loaded.registry.find("province", "b")->id);
    CHECK(bound.value()[1].definition_id == loaded.registry.find("province", "a")->id);
}

TEST_CASE("province bindings: missing and extra definitions are both reported") {
    auto loaded = provinces("province \"extra\" { name_key = \"extra\" }");
    REQUIRE(loaded.ok());
    const pat::content::ProvinceMapKey keys[] = {{1, "missing"}};
    const auto bound = pat::content::bind_provinces(loaded.registry, keys);
    REQUIRE_FALSE(bound.has_value());
    REQUIRE(bound.error().size() == 2);
    const auto text = bound.error()[0].format() + bound.error()[1].format();
    CHECK(text.find("missing") != std::string::npos);
    CHECK(text.find("extra") != std::string::npos);
    CHECK(text.find("provinces.pd:2:") != std::string::npos);
}

TEST_CASE("province bindings: duplicate ids, duplicate keys, water and blank keys are rejected") {
    auto loaded = provinces("province \"a\" { name_key = \"a\" }");
    REQUIRE(loaded.ok());
    SUBCASE("duplicate key") {
        const pat::content::ProvinceMapKey keys[] = {{1, "a"}, {2, "a"}};
        const auto result = pat::content::bind_provinces(loaded.registry, keys);
        REQUIRE_FALSE(result.has_value());
        REQUIRE(result.error().size() == 1);
        CHECK(result.error()[0].diagnostic.message == "duplicate map province key 'a'");
    }
    SUBCASE("duplicate id") {
        auto both = provinces(R"(province "a" { name_key = "a" }
province "b" { name_key = "b" })");
        REQUIRE(both.ok());
        const pat::content::ProvinceMapKey keys[] = {{1, "a"}, {1, "b"}};
        const auto result = pat::content::bind_provinces(both.registry, keys);
        REQUIRE_FALSE(result.has_value());
        REQUIRE(result.error().size() == 1);
        CHECK(result.error()[0].diagnostic.message == "duplicate map province id 1");
    }
    SUBCASE("water") {
        const pat::content::ProvinceMapKey keys[] = {{0, "a"}};
        const auto result = pat::content::bind_provinces(loaded.registry, keys);
        REQUIRE_FALSE(result.has_value());
        REQUIRE(result.error().size() == 1);
        CHECK(result.error()[0].diagnostic.message == "map province id 0 is reserved for water");
    }
    SUBCASE("blank") {
        const pat::content::ProvinceMapKey keys[] = {{1, ""}};
        const auto result = pat::content::bind_provinces(loaded.registry, keys);
        REQUIRE_FALSE(result.has_value());
        std::string errors;
        for (const auto& error : result.error()) {
            errors += error.format();
        }
        CHECK(errors.find("map province key must not be empty") != std::string::npos);
    }
}

TEST_CASE("province bindings: PMD1 uses one-based table ids and p0001 keys") {
    auto loaded = provinces(R"(province "p0001" { name_key = "a" }
province "p0002" { name_key = "b" })");
    REQUIRE(loaded.ok());
    pat::mapdata::MapData map;
    map.province_list.resize(2);
    map.province_list[0].id = 1;
    map.province_list[1].id = 2;
    REQUIRE(pat::content::bind_pmd1_provinces(loaded.registry, map));
    map.province_list[1].id = 1;
    CHECK_FALSE(pat::content::bind_pmd1_provinces(loaded.registry, map));
    map.province_list.clear();
    CHECK_FALSE(pat::content::bind_pmd1_provinces(loaded.registry, map));
}

TEST_CASE("province bindings: noncanonical PMD1 definition keys are not silently renumbered") {
    auto loaded = provinces("province \"p1\" { name_key = \"a\" }");
    REQUIRE(loaded.ok());
    pat::mapdata::MapData map;
    map.province_list.resize(1);
    map.province_list[0].id = 1;
    const auto result = pat::content::bind_pmd1_provinces(loaded.registry, map);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().size() == 2);
}
