// Court rules, data files, schema and localisation (E10-T04).
#include "pat/content/court_content.hpp"
#include "pat/data/content_loader.hpp"
#include "pat/data/content_schemas.hpp"
#include "pat/data/court_schemas.hpp"
#include "pat/loc/localizer.hpp"
#include "pat/sim/court_rules.hpp"
#include "pat/sim/court_text.hpp"
#include "pat/sim/world.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include "court_fixture.hpp"

using namespace pat;
using court_fixture::kBalance;
using court_fixture::kOffices;
using court_fixture::position_after;
using court_fixture::replace_once;
using court_fixture::shipped_source;
using court_fixture::shipped_text;

namespace {

content::CourtContentResult load_with(std::string_view path, const std::string& text) {
    auto source = shipped_source();
    source.add(std::string(path), text);
    return content::load_court_content(source);
}

}  // namespace

TEST_CASE("court rules: defaults are valid and the skill and trait slots match the world") {
    const auto rules = sim::CourtRules::defaults();
    const auto checked = sim::validate(rules);
    if (!checked) {
        INFO(checked.error().field << ": " << checked.error().message);
    }
    REQUIRE(checked);
    CHECK(rules.offices.size() == 13);  // 12 offices of Q2 plus the favourite
    CHECK(rules.servants.size() == 4);
    CHECK(rules.parties.size() == 3);
    CHECK(sim::kCourtSkillCount == sim::kSkillCount);
    CHECK(rules.find_office(6)->id == "governor");
    CHECK(rules.find_office(6)->from_date == sim::Date::from_ymd(1708, 12, 18));
    CHECK(rules.find_office(99) == nullptr);
    CHECK(rules.find_servant_role(2)->id == "spy");
    CHECK(rules.find_party(3)->id == "church");
    // Skill indices of the defaults follow the Skill enumeration.
    CHECK(rules.find_servant_role(1)->skill == static_cast<std::uint8_t>(sim::Skill::Stewardship));
    CHECK(rules.find_servant_role(2)->skill == static_cast<std::uint8_t>(sim::Skill::Intrigue));
    CHECK(rules.find_servant_role(3)->skill == static_cast<std::uint8_t>(sim::Skill::Learning));
    CHECK(rules.find_servant_role(4)->skill == static_cast<std::uint8_t>(sim::Skill::Martial));
    CHECK(rules.find_office(10)->requirements.min_skill[static_cast<std::size_t>(sim::Skill::Diplomacy)] ==
          12);
}

TEST_CASE("court rules: validate rejects each class of inconsistency with a field path") {
    auto rules = sim::CourtRules::defaults();
    const auto field_of = [](const sim::CourtRules& value) {
        const auto checked = sim::validate(value);
        REQUIRE_FALSE(checked);
        return checked.error().field;
    };
    SUBCASE("a number outside its corridor") {
        rules.balance.max_petitions_per_dynasty = 0;
        CHECK(field_of(rules) == "limits.max_petitions_per_dynasty");
    }
    SUBCASE("favor thresholds out of order") {
        rules.balance.favor_low_x100 = rules.balance.favor_high_x100;
        CHECK(field_of(rules) == "favor.favor_low_x100");
    }
    SUBCASE("a fixed seat rule with a maximum") {
        rules.offices[0].seats.max = 3;
        CHECK(field_of(rules) == "office bedchamberman.seats");
    }
    SUBCASE("a regional office with fixed seats") {
        rules.offices[4].seats = {sim::SeatRule::Fixed, 2, 0, 0};
        CHECK(field_of(rules) == "office voivode.seats");
    }
    SUBCASE("a date window that ends before it starts") {
        rules.offices[5].to_date = sim::Date::from_ymd(1700, 1, 1);
        CHECK(field_of(rules) == "office governor.to_date");
    }
    SUBCASE("slots must ascend") {
        std::swap(rules.offices[0], rules.offices[1]);
        CHECK(field_of(rules) == "office bedchamberman.slot");
    }
    SUBCASE("a servant skill corridor reversed") {
        rules.servants[0].skill_min = 16;
        CHECK(field_of(rules) == "servant_role steward.skill_min");
    }
    SUBCASE("two favourites") {
        auto extra = rules.offices.back();
        extra.slot = 14;
        extra.id = "second_favourite";
        rules.offices.push_back(extra);
        CHECK(field_of(rules) == "offices");
    }
}

TEST_CASE("court content: shipped files equal the defaults, field by field") {
    auto source = shipped_source();
    const auto loaded = content::load_court_content(source);
    if (!loaded.has_value()) {
        INFO(content::format_errors(loaded.error()));
    }
    REQUIRE(loaded.has_value());
    const auto& rules = loaded.value().rules;
    const auto defaults = sim::CourtRules::defaults();
    CHECK(loaded.value().content_hash != 0);
    REQUIRE(rules.offices.size() == defaults.offices.size());
    for (std::size_t i = 0; i < defaults.offices.size(); ++i) {
        CAPTURE(defaults.offices[i].id);
        CHECK(rules.offices[i] == defaults.offices[i]);
    }
    CHECK(rules.servants == defaults.servants);
    CHECK(rules.parties == defaults.parties);
    CHECK(rules.balance == defaults.balance);
    CHECK(rules == defaults);
}

TEST_CASE("court content: an edited number reaches the rules and the content hash") {
    const auto baseline = [] {
        auto source = shipped_source();
        return content::load_court_content(source);
    }();
    REQUIRE(baseline.has_value());
    SUBCASE("an office salary") {
        std::string text = shipped_text(kOffices);
        replace_once(text,
                     "salary_kop = 3500\n    honor_on_assign = 10\n    honor_monthly = 1\n    "
                     "decision_delay_days = 60\n}\n\noffice \"treasurer\"",
                     "salary_kop = 4000\n    honor_on_assign = 10\n    honor_monthly = 1\n    "
                     "decision_delay_days = 60\n}\n\noffice \"treasurer\"");
        const auto changed = load_with(kOffices, text);
        REQUIRE(changed.has_value());
        CHECK(changed.value().rules.find_office(10)->salary_kop == 4000);
        CHECK(changed.value().rules.find_office(11)->salary_kop == 3500);
        CHECK(changed.value().content_hash != baseline.value().content_hash);
    }
    SUBCASE("a balance number") {
        std::string text = shipped_text(kBalance);
        replace_once(text, "max_petitions_per_dynasty = 3", "max_petitions_per_dynasty = 4");
        const auto changed = load_with(kBalance, text);
        REQUIRE(changed.has_value());
        CHECK(changed.value().rules.balance.max_petitions_per_dynasty == 4);
        CHECK(changed.value().content_hash != baseline.value().content_hash);
    }
    SUBCASE("a loaded value is independent of file order") {
        auto source = shipped_source();
        source.add("court/zz_extra.pd", "format_version = 1\n");
        const auto again = content::load_court_content(source);
        REQUIRE(again.has_value());
        CHECK(again.value().content_hash == baseline.value().content_hash);
        CHECK(again.value().rules == baseline.value().rules);
    }
}

TEST_CASE("court content: invalid data is reported with the file and the position of the key") {
    std::string file{kOffices};
    std::string text;
    std::string anchor;
    std::string needle;
    auto code = data::ErrorCode::ContentRule;
    SUBCASE("an unknown skill in the requirements") {
        text = shipped_text(kOffices);
        replace_once(text, "skills = { martial = 6 }", "skills = { marital = 6 }");
        anchor = "office \"orderly\"";
        needle = "marital";
    }
    SUBCASE("an unknown trait reference") {
        text = shipped_text(kOffices);
        text.replace(text.find("dimwitted", text.find("office \"tsarevich_tutor\"")), 9, "dimwited");
        anchor = "office \"tsarevich_tutor\"";
        needle = "dimwited";
        code = data::ErrorCode::UnresolvedReference;
    }
    SUBCASE("two offices with the same slot") {
        text = shipped_text(kOffices);
        replace_once(text, "slot = 2\n    group = court", "slot = 1\n    group = court");
        anchor = "office \"orderly\"";
        needle = "slot = 1";
    }
    SUBCASE("seat rule that mixes fixed and maximum") {
        text = shipped_text(kOffices);
        replace_once(text,
                     "seats = { rule = fixed slots = 2 }\n    requirements = { min_age = 16  sex_allowed = "
                     "male  min_honor = 100 }",
                     "seats = { rule = fixed slots = 2 max = 3 }\n    requirements = { min_age = 16  "
                     "sex_allowed = male  min_honor = 100 }");
        anchor = "office \"bedchamberman\"";
        needle = "seats";
    }
    SUBCASE("a window that ends before it starts") {
        text = shipped_text(kOffices);
        replace_once(text, "from_date = 1708.12.18", "from_date = 1708.12.18\n    to_date = 1700.01.01");
        anchor = "office \"governor\"";
        needle = "to_date";
    }
    SUBCASE("a balance number outside the corridor") {
        file = std::string(kBalance);
        text = shipped_text(kBalance);
        replace_once(text, "max_petitions_per_dynasty = 3", "max_petitions_per_dynasty = 0");
        anchor = "limits";
        needle = "max_petitions_per_dynasty";
    }
    SUBCASE("favor thresholds out of order") {
        file = std::string(kBalance);
        text = shipped_text(kBalance);
        replace_once(text, "favor_low_x100 = 2000", "favor_low_x100 = 9000");
        anchor = "favor = {";
        needle = "favor_low_x100";
    }
    SUBCASE("a typo in a balance key") {
        file = std::string(kBalance);
        text = shipped_text(kBalance);
        replace_once(text, "retire_age", "retire_agee");
        anchor = "limits";
        needle = "retire_agee";
        code = data::ErrorCode::UnknownField;
    }
    const auto result = load_with(file, text);
    REQUIRE_FALSE(result.has_value());
    CAPTURE(content::format_errors(result.error()));
    const auto found =
        std::find_if(result.error().begin(), result.error().end(), [&](const data::LoadError& error) {
            return error.diagnostic.code == code;
        });
    REQUIRE(found != result.error().end());
    CHECK(found->file == file);
    CHECK(found->diagnostic.pos == position_after(text, anchor, needle));
}

TEST_CASE("court content: missing and repeated objects are rejected") {
    SUBCASE("no court files at all") {
        auto source = shipped_source(false);
        const auto result = content::load_court_content(source);
        REQUIRE_FALSE(result.has_value());
        CHECK(content::format_errors(result.error()).find("no office object found") != std::string::npos);
        CHECK(content::format_errors(result.error()).find("no court_balance object found") !=
              std::string::npos);
    }
    SUBCASE("a second balance object") {
        std::string text = shipped_text(kBalance);
        std::string second = text.substr(text.find("court_balance \"main\""));
        replace_once(second, "\"main\"", "\"extra\"");
        const auto result = load_with(kBalance, text + "\n" + second);
        REQUIRE_FALSE(result.has_value());
        INFO(content::format_errors(result.error()));
        CHECK(content::format_errors(result.error()).find("only one court_balance") != std::string::npos);
    }
}

TEST_CASE("court content: the four kinds stay out of the compatibility hash") {
    const auto with_court = [] {
        auto source = shipped_source();
        return data::load_content(source, data::content_schemas());
    }();
    const auto without_court = [] {
        auto source = shipped_source(false);
        return data::load_content(source, data::content_schemas());
    }();
    REQUIRE(with_court.ok());
    REQUIRE(without_court.ok());
    CHECK(with_court.registry.content_hash() != without_court.registry.content_hash());
    CHECK(with_court.registry.content_hash_excluding(data::kCourtKinds) ==
          without_court.registry.content_hash());
    // The court ids are interned last, so no definition id (it reaches the world hash) shifts.
    const auto* trait_with = with_court.registry.find("trait", "ambitious");
    const auto* trait_without = without_court.registry.find("trait", "ambitious");
    REQUIRE(trait_with != nullptr);
    REQUIRE(trait_without != nullptr);
    CHECK(trait_with->id == trait_without->id);
    // Editing a court file leaves the excluded hash unchanged.
    std::string text = shipped_text(kBalance);
    replace_once(text, "retire_age = 65", "retire_age = 66");
    auto source = shipped_source();
    source.add(std::string(kBalance), text);
    const auto edited = data::load_content(source, data::content_schemas());
    REQUIRE(edited.ok());
    CHECK(edited.registry.content_hash() != with_court.registry.content_hash());
    CHECK(edited.registry.content_hash_excluding(data::kCourtKinds) ==
          with_court.registry.content_hash_excluding(data::kCourtKinds));
}

TEST_CASE("court schema: the balance table equals the balance fields of the rules") {
    std::vector<std::tuple<std::string_view, std::string_view, bool>> expected;
#define PAT_TEST_FIELD(group, type, name, def, lo, hi)                                                       \
    expected.emplace_back(#group, #name, sizeof(type) == 8);
    PAT_COURT_BALANCE_FIELDS(PAT_TEST_FIELD)
#undef PAT_TEST_FIELD
    CHECK(expected.size() == sim::kCourtBalanceFieldCount);
    const auto& table = data::court_balance_fields();
    REQUIRE(table.size() == expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        CAPTURE(std::get<1>(expected[i]));
        CHECK(table[i].group == std::get<0>(expected[i]));
        CHECK(table[i].name == std::get<1>(expected[i]));
        CHECK(table[i].wide == std::get<2>(expected[i]));
        CHECK_FALSE(table[i].description.empty());
    }
    const auto schemas = data::content_schemas();
    const auto* schema = schemas.find(data::kCourtBalanceKind);
    REQUIRE(schema != nullptr);
    CHECK(schema->fields().size() == 10);  // one required block per group
    for (const std::string_view kind : data::kCourtKinds) {
        CHECK(schemas.find(kind) != nullptr);
    }
}

namespace {

loc::LocLoadResult load_court_loc() {
    data::MemorySource source("court loc");
    source.add("loc/ru/court.pd", shipped_text("loc/ru/court.pd"));
    return loc::load_localization(source);
}

}  // namespace

TEST_CASE("court localisation: every key the rules and the code name exists in the Russian table") {
    const auto loaded = load_court_loc();
    std::string report;
    for (const auto& error : loaded.errors) {
        report += error.format() + "\n";
    }
    INFO(report);
    REQUIRE(loaded.ok());
    const auto& strings = loaded.localizer;
    const auto require_key = [&](std::string_view key) {
        CAPTURE(key);
        REQUIRE_FALSE(key.empty());
        CHECK(strings.contains("ru", std::string(key)));
    };
    const auto rules = sim::CourtRules::defaults();
    for (const auto& office : rules.offices) {
        require_key(office.name_key);
        require_key(office.desc_key);
    }
    for (const auto& role : rules.servants) {
        require_key(role.name_key);
        require_key(role.effect_key);
    }
    for (const auto& party : rules.parties) {
        require_key(party.name_key);
        require_key(party.agenda_key);
    }
    std::size_t denials = 0;
    for (std::uint16_t code = 1; code < 100; ++code) {
        const auto key = sim::court_denial_key(static_cast<sim::CourtDenial>(code));
        if (!key.empty()) {
            ++denials;
            require_key(key);
        }
    }
    CHECK(denials == 42);  // codes of court-model section 7.3
    CHECK(sim::court_denial_key(sim::CourtDenial::None).empty());
    std::size_t terms = 0;
    for (std::uint16_t code = 14000; code <= 14602; ++code) {
        const auto key = sim::court_term_key(code);
        if (!key.empty()) {
            ++terms;
            require_key(key);
        }
    }
    CHECK(terms == 37);  // families 14000...14600 of court-model section 9
    const std::pair<sim::CourtReasonKind, std::uint8_t> reason_kinds[] = {{sim::CourtReasonKind::Vacancy, 7},
                                                                          {sim::CourtReasonKind::Resign, 5},
                                                                          {sim::CourtReasonKind::Dismiss, 4},
                                                                          {sim::CourtReasonKind::Withdraw, 3},
                                                                          {sim::CourtReasonKind::Fire, 5}};
    for (const auto& [kind, count] : reason_kinds) {
        CHECK(sim::court_reason_key(kind, 0).empty());
        for (std::uint8_t reason = 1; reason <= count; ++reason) {
            require_key(sim::court_reason_key(kind, reason));
        }
        CHECK(sim::court_reason_key(kind, static_cast<std::uint8_t>(count + 1)).empty());
    }
    for (std::uint16_t kind = 1; kind <= 10; ++kind) {
        require_key(sim::court_chronicle_key(static_cast<std::uint16_t>(3000 + 100 * kind + 5)));
    }
    CHECK(sim::court_chronicle_key(3005).empty());
    CHECK(sim::court_chronicle_key(4105).empty());
}
