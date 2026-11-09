// Name and portrait pools (E7-T09): the schemas, the files data/names/*.pd with their texts in
// data/loc/ru/names.pd, and the builder that turns loaded content into pat::sim::NamePools.

#include "pat/content/names_content.hpp"
#include "pat/core/rng.hpp"
#include "pat/data/content_schemas.hpp"
#include "pat/data/names_schemas.hpp"
#include "pat/loc/localizer.hpp"
#include "pat/sim/names.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace pat;
using pat::content::NamesContentResult;
using pat::data::DirectorySource;
using pat::data::ErrorCode;
using pat::data::LoadError;
using pat::data::MemorySource;
using pat::sim::LifeStage;
using pat::sim::NamePools;
using pat::sim::Sex;

namespace {

NamePools shipped_pools() {
    const DirectorySource dir{std::string(PATRIARCH_DATA_DIR)};
    const NamesContentResult result = content::load_names_content(dir);
    INFO((result.has_value() ? std::string() : content::format_errors(result.error())));
    REQUIRE(result.has_value());
    return result.value().pools;
}

/// The Russian tables of the shipped data.
loc::Localizer shipped_texts() {
    const DirectorySource dir{std::string(PATRIARCH_DATA_DIR)};
    loc::LocLoadResult loaded = loc::load_localization(dir);
    REQUIRE(loaded.ok());
    return std::move(loaded.localizer);
}

std::string text(const loc::Localizer& texts, std::string_view key) {
    std::string out;
    REQUIRE(texts.format_to(out, key).ok());
    return out;
}

/// "Anna Petrovna Menshikova" from the keys of a person, as the UI will build it.
std::string full_name(const loc::Localizer& texts, const sim::PersonNameKeys& keys) {
    std::string out;
    for (const std::string_view key : {keys.given, keys.patronymic, keys.surname}) {
        if (key.empty()) {
            continue;
        }
        if (!out.empty()) {
            out += ' ';
        }
        out += text(texts, key);
    }
    return out;
}

// ---- a small corpus to break ---------------------------------------------------------------------

using Files = std::map<std::string, std::string>;

Files minimal_files() {
    Files f;
    f["dyn.pd"] = R"PD(format_version = 1
dynasty "golitsyn" { name_key = "dynasty.golitsyn.name" }
)PD";
    f["names.pd"] = R"PD(format_version = 1
naming_rules "main" { parent_bonus = 40  grandparent_bonus = 25 }
given_name "ivan" {
    sex = male
    weight = 30
    name_key = "name.given.ivan"
    patronymic_son_key = "name.patronymic.ivan.son"
    patronymic_daughter_key = "name.patronymic.ivan.daughter"
}
given_name "anna" { sex = female  weight = 25  name_key = "name.given.anna" }
surname "golitsyn" {
    dynasty = dynasty:golitsyn
    male_key = "name.surname.golitsyn.male"
    female_key = "name.surname.golitsyn.female"
}
)PD";
    std::string portraits = "format_version = 1\n";
    int id = 1;
    for (const char* sex : {"male", "female"}) {
        for (const char* stage : {"child", "adolescent", "prime", "mature", "elder"}) {
            portraits += std::string("portrait \"") + sex + "_" + stage + "\" { sex = " + sex +
                         "  stage = " + stage + "  portrait_id = " + std::to_string(id++) + " }\n";
        }
    }
    f["portraits.pd"] = portraits;
    return f;
}

NamesContentResult load(const Files& files) {
    MemorySource source("names");
    for (const auto& [path, body] : files) {
        source.add(path, body);
    }
    return content::load_names_content(source);
}

void edit(Files& files, const std::string& file, const std::string& from, const std::string& to) {
    REQUIRE(files.count(file) == 1);
    std::string& body = files[file];
    const std::size_t at = body.find(from);
    INFO("text not found in " << file << ": " << from);
    REQUIRE(at != std::string::npos);
    body.replace(at, from.size(), to);
}

/// The one error of a failed load, as (code, line, column, message).
LoadError only_error(const NamesContentResult& result) {
    REQUIRE_FALSE(result.has_value());
    INFO(content::format_errors(result.error()));
    REQUIRE(result.error().size() == 1);
    return result.error().front();
}

}  // namespace

// ---- the shipped data ----------------------------------------------------------------------------

TEST_CASE("names content: data/ loads into valid pools of the planned size") {
    const NamePools pools = shipped_pools();
    CHECK(sim::validate(pools).has_value());

    // Epic E7 section "Risks": about 30 names per sex in the minimal pool.
    const auto count_names = [&](Sex sex) {
        return std::count_if(pools.given_names.begin(),
                             pools.given_names.end(),
                             [&](const sim::GivenName& n) { return n.sex == sex; });
    };
    CHECK(count_names(Sex::Male) >= 30);
    CHECK(count_names(Sex::Female) >= 30);
    CHECK(pools.surnames.size() == 4);  // the four houses of data/dynasties/m1.pd
    for (const char* house : {"menshikov", "sheremetev", "golitsyn", "naryshkin"}) {
        CHECK(sim::find_surname(pools, house) != nullptr);
    }
    CHECK(pools.rules.parent_bonus > 0);
    CHECK(pools.rules.grandparent_bonus > 0);
    CHECK(pools.rules.parent_bonus >
          pools.rules.grandparent_bonus);  // the father counts more than the grandfather
}

TEST_CASE("names content: every key of the pools has a Russian text, and only names use them") {
    const NamePools pools = shipped_pools();
    const loc::Localizer texts = shipped_texts();

    std::set<std::string> keys;
    for (const sim::GivenName& n : pools.given_names) {
        keys.insert(n.name_key);
        if (n.sex == Sex::Male) {
            keys.insert(n.patronymic_son_key);
            keys.insert(n.patronymic_daughter_key);
        }
    }
    for (const sim::SurnameForms& s : pools.surnames) {
        keys.insert(s.male_key);
        keys.insert(s.female_key);
    }
    for (const std::string& key : keys) {
        INFO("key " << key);
        std::string out;
        CHECK(texts.format_to(out, key).ok());
        CHECK_FALSE(out.empty());
    }
    // data/loc/ru/names.pd defines exactly these keys (no stray ones, no missing ones).
    std::size_t in_table = 0;
    for (const std::string& key : texts.keys("ru")) {
        if (key.starts_with("name.")) {
            ++in_table;
            CHECK(keys.contains(key));
        }
    }
    CHECK(in_table == keys.size());
}

TEST_CASE("names content: names and patronymics are right for both sexes on a test set") {
    const NamePools pools = shipped_pools();
    const loc::Localizer texts = shipped_texts();

    struct Case {
        const char* given;
        const char* father;
        const char* house;
        Sex sex;
        const char* expected;
    };
    const Case cases[] = {
        {"ivan", "petr", "menshikov", Sex::Male, "Иван Петрович Меншиков"},
        {"anna", "petr", "menshikov", Sex::Female, "Анна Петровна Меншикова"},
        {"petr", "ivan", "sheremetev", Sex::Male, "Пётр Иванович Шереметев"},
        {"maria", "ivan", "sheremetev", Sex::Female, "Мария Ивановна Шереметева"},
        {"fedor", "mikhail", "golitsyn", Sex::Male, "Фёдор Михайлович Голицын"},
        {"evdokia", "mikhail", "golitsyn", Sex::Female, "Евдокия Михайловна Голицына"},
        {"aleksei", "fedor", "naryshkin", Sex::Male, "Алексей Фёдорович Нарышкин"},
        {"natalya", "fedor", "naryshkin", Sex::Female, "Наталья Фёдоровна Нарышкина"},
        {"dmitry", "nikita", "golitsyn", Sex::Male, "Дмитрий Никитич Голицын"},
        {"praskovya", "nikita", "golitsyn", Sex::Female, "Прасковья Никитична Голицына"},
        {"vasily", "kozma", "menshikov", Sex::Male, "Василий Кузьмич Меншиков"},
        {"varvara", "kozma", "menshikov", Sex::Female, "Варвара Кузьминична Меншикова"},
        {"nikolai", "ilya", "sheremetev", Sex::Male, "Николай Ильич Шереметев"},
        {"sofya", "ilya", "sheremetev", Sex::Female, "Софья Ильинична Шереметева"},
        {"andrei", "yakov", "naryshkin", Sex::Male, "Андрей Яковлевич Нарышкин"},
        {"ekaterina", "yakov", "naryshkin", Sex::Female, "Екатерина Яковлевна Нарышкина"},
        {"semyon", "afanasy", "golitsyn", Sex::Male, "Семён Афанасьевич Голицын"},
        {"matryona", "lev", "menshikov", Sex::Female, "Матрёна Львовна Меншикова"},
    };
    for (const Case& c : cases) {
        INFO(c.expected);
        CHECK(full_name(texts, sim::person_name_keys(pools, c.given, c.father, c.house, c.sex)) ==
              c.expected);
    }
    // Without a known father there is no patronymic.
    CHECK(full_name(texts, sim::person_name_keys(pools, "anna", "", "menshikov", Sex::Female)) ==
          "Анна Меншикова");
}

TEST_CASE("names content: every patronymic pair is a son form and its daughter form") {
    const NamePools pools = shipped_pools();
    const loc::Localizer texts = shipped_texts();

    std::set<std::string> sons;
    std::set<std::string> daughters;
    for (const sim::GivenName& n : pools.given_names) {
        if (n.sex != Sex::Male) {
            continue;
        }
        const std::string son = text(texts, n.patronymic_son_key);
        const std::string daughter = text(texts, n.patronymic_daughter_key);
        INFO(n.id << ": " << son << " / " << daughter);
        // Sons end in -ич, daughters in -на; both start with the same three letters (two bytes each).
        CHECK(son.ends_with("ич"));
        CHECK(daughter.ends_with("на"));
        CHECK(son.substr(0, 6) == daughter.substr(0, 6));
        CHECK(son != daughter);
        sons.insert(son);
        daughters.insert(daughter);
    }
    // No two names share a patronymic.
    CHECK(sons.size() == static_cast<std::size_t>(std::count_if(
                             pools.given_names.begin(), pools.given_names.end(), [](const sim::GivenName& n) {
                                 return n.sex == Sex::Male;
                             })));
    CHECK(daughters.size() == sons.size());
}

TEST_CASE("names content: the same seed gives the same names, portraits and full names") {
    const NamePools pools = shipped_pools();
    const loc::Localizer texts = shipped_texts();

    const auto generate = [&](std::uint64_t seed) {
        core::RngStreams streams(seed);
        core::Pcg32& rng = streams.stream("demography.names");
        std::vector<std::string> out;
        for (int i = 0; i < 100; ++i) {
            const Sex sex = (i % 2 == 0) ? Sex::Male : Sex::Female;
            sim::NamingContext context{sex};
            context.parent_name = sex == Sex::Male ? "ivan" : "anna";
            const std::size_t index = sim::choose_given_name(pools, rng, context);
            REQUIRE(index != sim::kNoName);
            const std::uint16_t portrait = sim::choose_portrait(pools, rng, sex, LifeStage::Child);
            const auto keys =
                sim::person_name_keys(pools, pools.given_names[index].id, "petr", "menshikov", sex);
            out.push_back(full_name(texts, keys) + " #" + std::to_string(portrait));
        }
        return out;
    };
    CHECK(generate(2024) == generate(2024));
    CHECK(generate(2024) != generate(2025));
}

TEST_CASE("names content: the portrait pool gives every sex and life stage a portrait") {
    const NamePools pools = shipped_pools();
    core::RngStreams streams(9);
    core::Pcg32& rng = streams.stream("demography.names");
    for (const Sex sex : {Sex::Male, Sex::Female}) {
        for (const LifeStage stage : {LifeStage::Child,
                                      LifeStage::Adolescent,
                                      LifeStage::Prime,
                                      LifeStage::Mature,
                                      LifeStage::Elder}) {
            for (int i = 0; i < 20; ++i) {
                const std::uint16_t id = sim::choose_portrait(pools, rng, sex, stage);
                CHECK(id != 0);
                const auto it = std::find_if(pools.portraits.begin(),
                                             pools.portraits.end(),
                                             [&](const sim::PortraitEntry& p) { return p.id == id; });
                REQUIRE(it != pools.portraits.end());
                CHECK(it->sex == sex);
                CHECK(it->stage == stage);
            }
        }
    }
}

TEST_CASE("names content: the schema corridors equal the limits of pat::sim") {
    // The data module repeats the limits of sim (it must not depend on it); a value at the edge loads and
    // one beyond it is rejected by the schema.
    Files f = minimal_files();
    edit(f, "names.pd", "weight = 30", "weight = 1000");
    CHECK(load(f).has_value());
    edit(f, "names.pd", "weight = 1000", "weight = 1001");
    CHECK(only_error(load(f)).diagnostic.code == ErrorCode::ValueOutOfRange);

    f = minimal_files();
    edit(f, "names.pd", "parent_bonus = 40", "parent_bonus = 100000");
    CHECK(load(f).has_value());
    edit(f, "names.pd", "parent_bonus = 100000", "parent_bonus = 100001");
    CHECK(only_error(load(f)).diagnostic.code == ErrorCode::ValueOutOfRange);

    f = minimal_files();
    edit(f, "portraits.pd", "portrait_id = 1 ", "portrait_id = 65535 ");
    CHECK(load(f).has_value());
    edit(f, "portraits.pd", "portrait_id = 65535 ", "portrait_id = 65536 ");
    CHECK(only_error(load(f)).diagnostic.code == ErrorCode::ValueOutOfRange);
    edit(f, "portraits.pd", "portrait_id = 65536 ", "portrait_id = 0 ");
    CHECK(only_error(load(f)).diagnostic.code == ErrorCode::ValueOutOfRange);
}

// ---- the builder on small corpora ----------------------------------------------------------------

TEST_CASE("names content: the minimal corpus builds the expected pools") {
    const NamesContentResult result = load(minimal_files());
    INFO((result.has_value() ? std::string() : content::format_errors(result.error())));
    REQUIRE(result.has_value());
    const NamePools& pools = result.value().pools;
    REQUIRE(pools.given_names.size() == 2);
    CHECK(pools.given_names[0].id == "anna");  // in the order of the ids
    CHECK(pools.given_names[0].sex == Sex::Female);
    CHECK(pools.given_names[0].patronymic_son_key.empty());
    CHECK(pools.given_names[1].id == "ivan");
    CHECK(pools.given_names[1].weight == 30);
    CHECK(pools.given_names[1].patronymic_daughter_key == "name.patronymic.ivan.daughter");
    REQUIRE(pools.surnames.size() == 1);
    CHECK(pools.surnames[0].dynasty_id == "golitsyn");
    CHECK(pools.portraits.size() == 10);
    CHECK(
        std::is_sorted(pools.portraits.begin(),
                       pools.portraits.end(),
                       [](const sim::PortraitEntry& a, const sim::PortraitEntry& b) { return a.id < b.id; }));
    CHECK(result.value().content_hash != 0);
}

TEST_CASE("names content: loading twice gives identical pools and hash; a changed value changes the hash") {
    const NamesContentResult a = load(minimal_files());
    const NamesContentResult b = load(minimal_files());
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());
    CHECK(a.value().pools == b.value().pools);
    CHECK(a.value().content_hash == b.value().content_hash);

    Files changed = minimal_files();
    edit(changed, "names.pd", "weight = 25", "weight = 26");
    const NamesContentResult c = load(changed);
    REQUIRE(c.has_value());
    CHECK(c.value().content_hash != a.value().content_hash);
    CHECK(c.value().pools != a.value().pools);
}

TEST_CASE("names content: a man's name without a patronymic form is rejected at the object") {
    Files f = minimal_files();
    edit(f, "names.pd", "    patronymic_daughter_key = \"name.patronymic.ivan.daughter\"\n", "");
    const LoadError& e = only_error(load(f));
    CHECK(e.diagnostic.code == ErrorCode::ContentRule);
    CHECK(e.file == "names.pd");
    CHECK(e.diagnostic.pos.line == 3);
    CHECK(e.diagnostic.message.find("patronymic_daughter_key") != std::string::npos);
}

TEST_CASE("names content: a woman's name with a patronymic form is rejected at the field") {
    Files f = minimal_files();
    edit(f,
         "names.pd",
         "given_name \"anna\" { sex = female  weight = 25  name_key = \"name.given.anna\" }",
         "given_name \"anna\" {\n    sex = female\n    weight = 25\n    name_key = \"name.given.anna\"\n"
         "    patronymic_son_key = \"name.patronymic.anna.son\"\n}");
    const LoadError& e = only_error(load(f));
    CHECK(e.diagnostic.code == ErrorCode::ContentRule);
    CHECK(e.diagnostic.pos.line == 14);  // the line of the field
    CHECK(e.diagnostic.message.find("woman's name 'anna'") != std::string::npos);
}

TEST_CASE("names content: empty keys are rejected") {
    Files f = minimal_files();
    edit(f, "names.pd", "name_key = \"name.given.ivan\"", "name_key = \"\"");
    const LoadError& e = only_error(load(f));
    CHECK(e.diagnostic.code == ErrorCode::ContentRule);
    CHECK(e.diagnostic.message.find("name_key must not be empty") != std::string::npos);
}

TEST_CASE("names content: a repeated portrait id is rejected and points at the first use") {
    Files f = minimal_files();
    edit(f, "portraits.pd", "portrait_id = 2 ", "portrait_id = 1 ");
    const LoadError& e = only_error(load(f));
    CHECK(e.diagnostic.code == ErrorCode::ContentRule);
    CHECK(e.file == "portraits.pd");
    CHECK(e.diagnostic.message.find("portrait id 1 is already used by") != std::string::npos);
}

TEST_CASE("names content: a pool without a portrait for some sex and stage is rejected") {
    Files f = minimal_files();
    edit(f,
         "portraits.pd",
         "portrait \"female_elder\" { sex = female  stage = elder  portrait_id = 10 }\n",
         "");
    const LoadError& e = only_error(load(f));
    CHECK(e.diagnostic.code == ErrorCode::ContentRule);
    CHECK(e.diagnostic.message.find("no portrait for sex female and stage elder") != std::string::npos);
}

TEST_CASE("names content: a pool without a name for a sex is rejected") {
    Files f = minimal_files();
    edit(f,
         "names.pd",
         "given_name \"anna\" { sex = female  weight = 25  name_key = \"name.given.anna\" }\n",
         "");
    const LoadError& e = only_error(load(f));
    CHECK(e.diagnostic.code == ErrorCode::ContentRule);
    CHECK(e.diagnostic.message.find("no female given name") != std::string::npos);
}

TEST_CASE("names content: a house needs exactly one surname object") {
    {
        Files f = minimal_files();
        edit(f,
             "dyn.pd",
             "dynasty \"golitsyn\" { name_key = \"dynasty.golitsyn.name\" }\n",
             "dynasty \"golitsyn\" { name_key = \"dynasty.golitsyn.name\" }\n"
             "dynasty \"naryshkin\" { name_key = \"dynasty.naryshkin.name\" }\n");
        const LoadError& e = only_error(load(f));
        CHECK(e.diagnostic.code == ErrorCode::ContentRule);
        CHECK(e.file == "dyn.pd");
        CHECK(e.diagnostic.message.find("dynasty 'naryshkin' has no surname") != std::string::npos);
    }
    {
        Files f = minimal_files();
        f["names.pd"] += "surname \"golitsyn2\" {\n    dynasty = dynasty:golitsyn\n    male_key = \"a\"\n"
                         "    female_key = \"b\"\n}\n";
        const LoadError& e = only_error(load(f));
        CHECK(e.diagnostic.code == ErrorCode::ContentRule);
        CHECK(e.diagnostic.message.find("dynasty 'golitsyn' already has the surname 'golitsyn'") !=
              std::string::npos);
    }
    {
        Files f = minimal_files();
        edit(f, "names.pd", "dynasty = dynasty:golitsyn", "dynasty = dynasty:nobody");
        const auto result = load(f);
        REQUIRE_FALSE(result.has_value());
        CHECK(std::any_of(result.error().begin(), result.error().end(), [](const LoadError& e) {
            return e.diagnostic.code == ErrorCode::UnresolvedReference;
        }));
    }
}

TEST_CASE("names content: naming_rules must exist exactly once") {
    {
        Files f = minimal_files();
        edit(f, "names.pd", "naming_rules \"main\" { parent_bonus = 40  grandparent_bonus = 25 }\n", "");
        const LoadError& e = only_error(load(f));
        CHECK(e.diagnostic.code == ErrorCode::ContentRule);
        CHECK(e.diagnostic.message.find("no naming_rules object") != std::string::npos);
    }
    {
        Files f = minimal_files();
        f["names.pd"] += "naming_rules \"second\" { parent_bonus = 1  grandparent_bonus = 1 }\n";
        const LoadError& e = only_error(load(f));
        CHECK(e.diagnostic.code == ErrorCode::ContentRule);
        CHECK(e.diagnostic.message.find("only one naming_rules object is allowed") != std::string::npos);
    }
}

TEST_CASE("names content: schema mistakes are reported by the loader with a position") {
    Files f = minimal_files();
    edit(f, "names.pd", "sex = male", "sex = mail");
    const LoadError& e = only_error(load(f));
    CHECK(e.diagnostic.code == ErrorCode::UnknownEnumValue);
    CHECK(e.diagnostic.pos.line == 4);

    f = minimal_files();
    edit(f, "portraits.pd", "stage = child", "stage = toddler");
    CHECK(only_error(load(f)).diagnostic.code == ErrorCode::UnknownEnumValue);
}

TEST_CASE("names content: a mod replaces a name whole and adds one") {
    MemorySource base("base");
    for (const auto& [path, body] : minimal_files()) {
        base.add(path, body);
    }
    MemorySource mod("mod");
    mod.add("names.pd", R"PD(format_version = 1
given_name "ivan" {
    sex = male
    weight = 99
    name_key = "name.given.ivan"
    patronymic_son_key = "mod.son"
    patronymic_daughter_key = "mod.daughter"
}
given_name "yaroslav" {
    sex = male
    weight = 3
    name_key = "name.given.yaroslav"
    patronymic_son_key = "name.patronymic.yaroslav.son"
    patronymic_daughter_key = "name.patronymic.yaroslav.daughter"
}
)PD");
    const data::ContentSource* roots[] = {&base, &mod};
    const NamesContentResult result = content::load_names_content(roots);
    INFO((result.has_value() ? std::string() : content::format_errors(result.error())));
    REQUIRE(result.has_value());
    const NamePools& pools = result.value().pools;
    REQUIRE(pools.given_names.size() == 3);
    const sim::GivenName* ivan = sim::find_given_name(pools, "ivan");
    REQUIRE(ivan != nullptr);
    CHECK(ivan->weight == 99);
    CHECK(ivan->patronymic_son_key == "mod.son");
    CHECK(sim::find_given_name(pools, "yaroslav") != nullptr);
}
