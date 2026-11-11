#include "pat/content/load_errors.hpp"
#include "pat/content/m1_content.hpp"
#include "pat/data/content_loader.hpp"
#include "pat/data/content_schemas.hpp"
#include "pat/data/m1_schemas.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace {

using pat::data::LoadError;

constexpr std::string_view kCanonicalSkills = R"PD(
skill "stewardship" { name_key = "skill.stewardship" desc_key = "skill.stewardship.desc" slot = 0 }
skill "diplomacy" { name_key = "skill.diplomacy" desc_key = "skill.diplomacy.desc" slot = 1 }
skill "martial" { name_key = "skill.martial" desc_key = "skill.martial.desc" slot = 2 }
skill "intrigue" { name_key = "skill.intrigue" desc_key = "skill.intrigue.desc" slot = 3 }
skill "learning" { name_key = "skill.learning" desc_key = "skill.learning.desc" slot = 4 }
)PD";

pat::data::LoadResult load_definitions(const std::string& definitions) {
    pat::data::MemorySource source("M1 base");
    source.add("defs/m1.pd", "format_version = 1\n" + definitions);
    auto loaded = pat::data::load_content(source, pat::data::content_schemas());
    INFO(pat::content::format_errors(loaded.errors));
    REQUIRE(loaded.ok());
    return loaded;
}

bool has_message(const std::vector<LoadError>& errors, std::string_view text) {
    return std::any_of(errors.begin(), errors.end(), [&](const LoadError& error) {
        return error.diagnostic.message.find(text) != std::string::npos;
    });
}

}  // namespace

TEST_CASE("m1_content: minimal and canonical definitions pass semantic validation") {
    const auto empty = load_definitions("");
    CHECK(pat::content::validate_m1_definitions(empty.registry).empty());

    const auto loaded = load_definitions(std::string(kCanonicalSkills) + R"PD(
dynasty "golitsyn" { name_key = "dynasty.golitsyn" }
trait "cautious" {
    name_key = "trait.cautious"
    desc_key = "trait.cautious.desc"
    slot = 1
    category = personality
    opposite = trait:reckless
    skills = { stewardship = 1  intrigue = -1 }
}


trait "reckless" {
    name_key = "trait.reckless"
    desc_key = "trait.reckless.desc"
    slot = 2
    category = personality
    opposite = trait:cautious
}
)PD");
    const auto errors = pat::content::validate_m1_definitions(loaded.registry);
    INFO(pat::content::format_errors(errors));
    CHECK(errors.empty());
}

TEST_CASE("m1_content: unknown trait skill reports its source key") {
    const auto loaded = load_definitions(R"PD(trait "cautious" {
    name_key = "trait.cautious"
    desc_key = "trait.cautious.desc"
    slot = 1
    category = personality
    skills = {
        sorcery = 1
    }
}
)PD");
    const auto errors = pat::content::validate_m1_definitions(loaded.registry);
    REQUIRE(errors.size() == 1);
    CHECK(errors.front().diagnostic.code == pat::data::ErrorCode::ContentRule);
    CHECK(errors.front().root == 0);
    CHECK(errors.front().root_name == "M1 base");
    CHECK(errors.front().file == "defs/m1.pd");
    CHECK(errors.front().diagnostic.pos.line == 8);
    CHECK(errors.front().diagnostic.pos.column == 9);
    CHECK(errors.front().format().find("defs/m1.pd:8:9: error[PD115]") != std::string::npos);
    CHECK(has_message(errors, "unknown skill 'sorcery'"));
}

TEST_CASE("m1_content: trait slots cannot be reused") {
    const auto loaded =
        load_definitions(R"PD(trait "a" { name_key = "a" desc_key = "a.desc" slot = 1 category = life }
trait "b" { name_key = "b" desc_key = "b.desc" slot = 1 category = life }
)PD");
    const auto errors = pat::content::validate_m1_definitions(loaded.registry);
    REQUIRE(errors.size() == 1);
    CHECK(errors.front().diagnostic.pos.line == 3);
    CHECK(has_message(errors, "trait slot 1 is already used by 'a'"));
}

TEST_CASE("m1_content: opposite traits are distinct and reciprocal") {
    SUBCASE("self reference") {
        const auto loaded = load_definitions(R"PD(
trait "a" { name_key = "a" desc_key = "a.desc" slot = 1 category = life opposite = trait:a }
)PD");
        const auto errors = pat::content::validate_m1_definitions(loaded.registry);
        REQUIRE(errors.size() == 1);
        CHECK(has_message(errors, "cannot be its own opposite"));
    }
    SUBCASE("one-way reference") {
        const auto loaded = load_definitions(R"PD(
trait "a" { name_key = "a" desc_key = "a.desc" slot = 1 category = life opposite = trait:b }
trait "b" { name_key = "b" desc_key = "b.desc" slot = 2 category = life }
)PD");
        const auto errors = pat::content::validate_m1_definitions(loaded.registry);
        REQUIRE(errors.size() == 1);
        CHECK(has_message(errors, "opposite relation 'a' -> 'b' must be reciprocal"));
    }
}

TEST_CASE("m1_content: nonempty skill definitions must match the simulation columns") {
    SUBCASE("incomplete set") {
        const auto loaded = load_definitions(R"PD(
skill "stewardship" { name_key = "s" desc_key = "s.desc" slot = 0 }
)PD");
        const auto errors = pat::content::validate_m1_definitions(loaded.registry);
        CHECK(errors.size() == 4);
        CHECK(has_message(errors, "missing canonical skill 'learning'"));
    }
    SUBCASE("wrong slot") {
        std::string definitions(kCanonicalSkills);
        const auto at = definitions.find("slot = 4");
        REQUIRE(at != std::string::npos);
        definitions.replace(at, std::string_view("slot = 4").size(), "slot = 3");
        const auto loaded = load_definitions(definitions);
        const auto errors = pat::content::validate_m1_definitions(loaded.registry);
        REQUIRE(errors.size() == 1);
        CHECK(has_message(errors, "skill 'learning' must use slot 4"));
    }
    SUBCASE("unknown extra skill") {
        const auto loaded = load_definitions(std::string(kCanonicalSkills) + R"PD(
skill "sorcery" { name_key = "s" desc_key = "s.desc" slot = 0 }
)PD");
        const auto errors = pat::content::validate_m1_definitions(loaded.registry);
        REQUIRE(errors.size() == 1);
        CHECK(has_message(errors, "unknown skill 'sorcery'"));
    }
}

TEST_CASE("m1_content: empty localization keys are source-located and sorted") {
    const auto loaded = load_definitions(R"PD(trait "a" {
    name_key = ""
    desc_key = ""
    slot = 1
    category = life
}
dynasty "b" { name_key = "" desc_key = "" }
)PD");
    const auto errors = pat::content::validate_m1_definitions(loaded.registry);
    REQUIRE(errors.size() == 4);
    CHECK(errors.front().diagnostic.pos.line == 3);
    CHECK(errors.front().diagnostic.pos.column == 5);
    CHECK(has_message(errors, "name_key must not be empty"));
    CHECK(has_message(errors, "desc_key must not be empty"));
    CHECK(std::is_sorted(errors.begin(), errors.end(), [](const LoadError& a, const LoadError& b) {
        return std::tie(a.root, a.file, a.diagnostic.pos, a.diagnostic.code, a.diagnostic.message) <
               std::tie(b.root, b.file, b.diagnostic.pos, b.diagnostic.code, b.diagnostic.message);
    }));
}

TEST_CASE("m1_content: honor ranks are inclusive and leave simulation compatibility intact") {
    const std::string base = R"PD(dynasty "golitsyn" { name_key = "dynasty.golitsyn" })PD";
    const auto plain = load_definitions(base);
    const auto ranked = load_definitions(base + R"PD(
ui_honor_rank "first" { min_honor = 100 name_key = "rank.first" }
ui_honor_rank "last" { min_honor = -1000 name_key = "rank.last" }
)PD");
    REQUIRE(pat::content::validate_m1_definitions(ranked.registry).empty());
    const auto ranks = pat::content::build_honor_ranks(ranked.registry);
    REQUIRE(ranks.size() == 2);
    CHECK(ranks.front().min_honor == -1000);
    CHECK(pat::content::honor_rank(ranks, -1001) == nullptr);
    CHECK(pat::content::honor_rank(ranks, -1000) == &ranks.front());
    CHECK(pat::content::honor_rank(ranks, 99) == &ranks.front());
    CHECK(pat::content::honor_rank(ranks, 100) == &ranks.back());
    CHECK(pat::content::honor_rank(ranks, 10000) == &ranks.back());
    CHECK(pat::content::honor_rank({}, 0) == nullptr);
    CHECK(ranked.registry.content_hash() != plain.registry.content_hash());
    CHECK(ranked.registry.content_hash_excluding(pat::data::kHonorRankKind) == plain.registry.content_hash());
    const auto* plain_dynasty = plain.registry.find("dynasty", "golitsyn");
    const auto* ranked_dynasty = ranked.registry.find("dynasty", "golitsyn");
    REQUIRE(plain_dynasty != nullptr);
    REQUIRE(ranked_dynasty != nullptr);
    CHECK(ranked_dynasty->id == plain_dynasty->id);

    const auto invalid = load_definitions(R"PD(
ui_honor_rank "first" { min_honor = 0 name_key = "rank.first" }
ui_honor_rank "last" { min_honor = 0 name_key = "rank.last" }
)PD");
    const auto errors = pat::content::validate_m1_definitions(invalid.registry);
    CHECK(has_message(errors, "threshold 0 is already used"));
    CHECK(has_message(errors, "lowest honor rank threshold must be -1000"));
}
