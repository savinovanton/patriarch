// Credits entries (UI-213, docs/dev/credits.md): the `credit` schema, the files data/credits/*.pd with
// their texts in data/loc/ru/credits.pd and the builder that turns loaded content into a sorted list.
// The window and its snapshot are tested in credits_window_test.cpp (engine build only).

#include "pat/content/credits_content.hpp"
#include "pat/data/content_schemas.hpp"
#include "pat/data/content_source.hpp"
#include "pat/data/credits_schemas.hpp"
#include "pat/loc/localizer.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

using namespace pat;
using pat::content::CreditEntry;
using pat::content::CreditSection;
using pat::content::CreditsResult;
using pat::data::DirectorySource;
using pat::data::ErrorCode;
using pat::data::LoadError;
using pat::data::MemorySource;

namespace {

CreditsResult load_text(const std::string& text) {
    MemorySource source("credits");
    source.add("credits/test.pd", "format_version = 1\n" + text);
    return content::load_credits_content(source);
}

LoadError only_error(const CreditsResult& result) {
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().size() == 1);
    return result.error().front();
}

content::CreditsList shipped_credits() {
    const DirectorySource dir{std::string(PATRIARCH_DATA_DIR)};
    const CreditsResult result = content::load_credits_content(dir);
    INFO((result.has_value() ? std::string() : content::format_errors(result.error())));
    REQUIRE(result.has_value());
    return result.value();
}

loc::Localizer shipped_texts() {
    const DirectorySource dir{std::string(PATRIARCH_DATA_DIR)};
    loc::LocLoadResult loaded = loc::load_localization(dir);
    REQUIRE(loaded.ok());
    return std::move(loaded.localizer);
}

const CreditEntry* find_entry(const content::CreditsList& list, std::string_view id) {
    const auto it = std::ranges::find(list.entries, id, &CreditEntry::id);
    return it == list.entries.end() ? nullptr : &*it;
}

}  // namespace

TEST_CASE("credits content: the schema is registered and describes every field") {
    const data::SchemaSet schemas = data::content_schemas();
    const data::ObjectSchema* schema = schemas.find(data::kCreditKind);
    REQUIRE(schema != nullptr);
    for (const char* field : {"section",
                              "order",
                              "name_key",
                              "name",
                              "text_key",
                              "text",
                              "provider",
                              "license",
                              "source",
                              "asset"}) {
        INFO(field);
        CHECK(schema->find_field(field) >= 0);
    }
}

TEST_CASE("credits content: entries are sorted by section, order and id") {
    const CreditsResult result = load_text(R"PD(
credit "b" { section = assets  order = 5   name = "B" }
credit "a" { section = assets  order = 5   name = "A" }
credit "z" { section = authors name = "Z" }
credit "m" { section = data    order = 1   name_key = "k.m"  text = "verbatim" }
credit "n" { section = data    order = 0   name = "N"  text_key = "k.n" }
)PD");
    INFO((result.has_value() ? std::string() : content::format_errors(result.error())));
    REQUIRE(result.has_value());
    const auto& entries = result.value().entries;
    REQUIRE(entries.size() == 5);
    CHECK(entries[0].id == "z");
    CHECK(entries[1].id == "n");
    CHECK(entries[2].id == "m");
    CHECK(entries[3].id == "a");
    CHECK(entries[4].id == "b");
    CHECK(entries[0].order == 100);  // the default
    CHECK(entries[2].name_key == "k.m");
    CHECK(entries[2].text == "verbatim");
    CHECK(result.value().content_hash != 0);
}

TEST_CASE("credits content: name or name_key is required, and not both") {
    const LoadError none = only_error(load_text("credit \"x\" { section = data }\n"));
    CHECK(none.diagnostic.code == ErrorCode::ContentRule);
    CHECK(none.diagnostic.message.find("needs name_key or name") != std::string::npos);

    const LoadError both =
        only_error(load_text("credit \"x\" { section = data  name = \"A\"  name_key = \"k\" }\n"));
    CHECK(both.diagnostic.code == ErrorCode::ContentRule);
    CHECK(both.diagnostic.message.find("both name_key and name") != std::string::npos);

    const LoadError text_both = only_error(
        load_text("credit \"x\" { section = data  name = \"A\"  text = \"t\"  text_key = \"k\" }\n"));
    CHECK(text_both.diagnostic.message.find("both text_key and text") != std::string::npos);
}

TEST_CASE("credits content: empty strings and an unknown section are rejected with a position") {
    const LoadError empty = only_error(load_text("credit \"x\" { section = data  name = \"\" }\n"));
    CHECK(empty.diagnostic.code == ErrorCode::ContentRule);
    CHECK(empty.diagnostic.message.find("name must not be empty") != std::string::npos);
    CHECK(empty.diagnostic.pos.line == 2);

    const LoadError section = only_error(load_text("credit \"x\" { section = bonus  name = \"A\" }\n"));
    CHECK(section.diagnostic.code != ErrorCode::ContentRule);  // the schema reports it (an enum value)
}

TEST_CASE("credits content: a mod replaces an entry by repeating its id") {
    MemorySource base("base");
    base.add("credits/a.pd", "format_version = 1\ncredit \"x\" { section = data  name = \"Base\" }\n");
    MemorySource mod("mod");
    mod.add("credits/a.pd", "format_version = 1\ncredit \"x\" { section = data  name = \"Mod\" }\n");
    const data::ContentSource* roots[] = {&base, &mod};
    const CreditsResult result = content::load_credits_content(roots);
    REQUIRE(result.has_value());
    REQUIRE(result.value().entries.size() == 1);
    CHECK(result.value().entries[0].name == "Mod");
}

TEST_CASE("credits content: the shipped data has the National Library of Russia, the manifest sources and "
          "the authors") {
    const content::CreditsList credits = shipped_credits();
    REQUIRE_FALSE(credits.entries.empty());

    // Entries of the three kinds are present: hand-written, from the geodata manifest, and sorted.
    const CreditEntry* rnb = find_entry(credits, "rnb");
    REQUIRE(rnb != nullptr);
    CHECK(rnb->section == CreditSection::Data);
    CHECK(rnb->source == "nlr-historical-maps");
    CHECK(find_entry(credits, "authors") != nullptr);
    const CreditEntry* lakes = find_entry(credits, "source_hydrolakes");
    REQUIRE(lakes != nullptr);
    CHECK(lakes->license == "CC-BY-4.0");
    CHECK(lakes->text.find("Messager") != std::string::npos);
    CHECK(find_entry(credits, "source_copernicus_dem_glo30") != nullptr);

    // The sources the hand-written entries cover are not listed twice.
    CHECK(find_entry(credits, "source_nlr_historical_maps") == nullptr);

    const auto sorted =
        std::ranges::is_sorted(credits.entries, [](const CreditEntry& a, const CreditEntry& b) {
            return std::tie(a.section, a.order, a.id) < std::tie(b.section, b.order, b.id);
        });
    CHECK(sorted);
}

TEST_CASE("credits content: the National Library of Russia is credited with the exact wording") {
    const content::CreditsList credits = shipped_credits();
    const loc::Localizer texts = shipped_texts();
    const CreditEntry* rnb = find_entry(credits, "rnb");
    REQUIRE(rnb != nullptr);
    CHECK(texts.format(rnb->name_key) == "Российская национальная библиотека");
}

TEST_CASE("credits content: every key of the shipped entries is defined in the Russian table") {
    const content::CreditsList credits = shipped_credits();
    const loc::Localizer texts = shipped_texts();
    for (const CreditEntry& entry : credits.entries) {
        INFO(entry.id);
        for (const std::string* key : {&entry.name_key, &entry.text_key}) {
            if (!key->empty()) {
                CHECK(texts.contains("ru", *key));
            }
        }
    }
}
