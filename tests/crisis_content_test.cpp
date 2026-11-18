// Crisis rules, data files, schema and localisation (E12-T04).
#include "pat/content/crisis_content.hpp"
#include "pat/data/content_loader.hpp"
#include "pat/data/content_schemas.hpp"
#include "pat/data/crisis_schemas.hpp"
#include "pat/loc/localizer.hpp"
#include "pat/sim/crisis_rules.hpp"
#include "pat/sim/crisis_text.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include "court_fixture.hpp"

using namespace pat;
using court_fixture::position_after;
using court_fixture::replace_once;
using court_fixture::shipped_text;

namespace {

constexpr std::string_view kCrisisBalance = "balance/crisis.pd";
constexpr std::string_view kProjects = "crisis/projects.pd";

// The definition files a load needs plus the crisis files; the court files are left out.
data::MemorySource crisis_source(bool with_crisis = true) {
    auto source = court_fixture::shipped_source(false);
    if (with_crisis) {
        source.add(std::string(kCrisisBalance), shipped_text(kCrisisBalance));
        source.add(std::string(kProjects), shipped_text(kProjects));
    }
    return source;
}

content::CrisisContentResult load_with(std::string_view path, const std::string& text) {
    auto source = crisis_source();
    source.add(std::string(path), text);
    return content::load_crisis_content(source);
}

}  // namespace

TEST_CASE("crisis rules: defaults are valid and the weights add up to 1000") {
    const auto rules = sim::crisis::CrisisRules::defaults();
    const auto checked = sim::crisis::validate(rules);
    if (!checked) {
        INFO(checked.error().message);
    }
    CHECK(checked.has_value());
    std::int32_t sum = 0;
    for (const auto weight : rules.component_weights()) {
        sum += weight;
    }
    CHECK(sum == 1000);
    CHECK(rules.projects.size() == sim::crisis::kProjectCount);
    CHECK(rules.find_project(sim::crisis::ProjectKind::Taxes)->level_min == -2);
    CHECK(rules.find_project(sim::crisis::ProjectKind::Purges)->find_effect("punish_tier") != nullptr);
    // 1703 + the protected years is the end of the protective window of the scenario (D17).
    CHECK(rules.balance.protect_years == 5);
}

TEST_CASE("crisis rules: validate rejects each class of inconsistency with a field path") {
    auto rules = sim::crisis::CrisisRules::defaults();
    std::string field;
    SUBCASE("a number outside its corridor") {
        rules.balance.protect_years = 99;
        field = "protect.years";
    }
    SUBCASE("weights that do not add up") {
        rules.balance.components_weight_war = 140;
        field = "components.weight_fiscal";
    }
    SUBCASE("bands out of order") {
        rules.balance.bands_tension_min = 80;
        field = "bands.order_min";
    }
    SUBCASE("a phase exit below its entrance") {
        rules.balance.phases_p3_exit = 30;
        field = "phases.p3_exit";
    }
    SUBCASE("a sigmoid that is not symmetric") {
        rules.balance.sigmoid[3] = 30;
        field = "sigmoid[3]";
    }
    SUBCASE("a sigmoid that decreases") {
        rules.balance.sigmoid[4] = 20;
        field = "sigmoid[4]";
    }
    SUBCASE("blood terms that grow with the degree") {
        rules.claims.blood[2] = 90;
        field = "claim_rules blood[2]";
    }
    SUBCASE("a missing project") {
        rules.projects.pop_back();
        field = "emperor_project";
    }
    SUBCASE("thresholds that do not ascend") {
        rules.projects[0].thresholds = {20, 20, 70};
        field = "emperor_project war.thresholds[1]";
    }
    SUBCASE("a wrong number of thresholds") {
        rules.projects[1].thresholds.pop_back();
        field = "emperor_project taxes.thresholds";
    }
    SUBCASE("a missing effect") {
        rules.projects[2].effects.pop_back();
        field = "emperor_project purges.effects";
    }
    SUBCASE("an effect of another project") {
        rules.projects[1].effects.push_back({"war_fatigue_x100", {0, 0, 0, 0, 0, 0}});
        field = "emperor_project taxes.effects.war_fatigue_x100";
    }
    SUBCASE("an effect with the wrong number of levels") {
        rules.projects[0].effects[0].per_level.push_back(300);
        field = "emperor_project war.effects.war_fatigue_x100";
    }
    const auto checked = sim::crisis::validate(rules);
    REQUIRE_FALSE(checked.has_value());
    CHECK(checked.error().field == field);
    CHECK_FALSE(checked.error().message.empty());
}

TEST_CASE("crisis content: shipped files equal the defaults, field by field") {
    auto source = crisis_source();
    const auto loaded = content::load_crisis_content(source);
    if (!loaded.has_value()) {
        INFO(content::format_errors(loaded.error()));
    }
    REQUIRE(loaded.has_value());
    const auto& rules = loaded.value().rules;
    const auto defaults = sim::crisis::CrisisRules::defaults();
    CHECK(loaded.value().content_hash != 0);
    CHECK(rules.balance == defaults.balance);
    CHECK(rules.claims == defaults.claims);
    REQUIRE(rules.projects.size() == defaults.projects.size());
    for (std::size_t i = 0; i < defaults.projects.size(); ++i) {
        CAPTURE(defaults.projects[i].id);
        CHECK(rules.projects[i] == defaults.projects[i]);
    }
    CHECK(rules == defaults);
}

TEST_CASE("crisis content: an edited number reaches the rules and the content hash") {
    const auto baseline = [] {
        auto source = crisis_source();
        return content::load_crisis_content(source);
    }();
    REQUIRE(baseline.has_value());
    SUBCASE("a balance number") {
        std::string text = shipped_text(kCrisisBalance);
        replace_once(text, "years = 5", "years = 6");
        const auto changed = load_with(kCrisisBalance, text);
        REQUIRE(changed.has_value());
        CHECK(changed.value().rules.balance.protect_years == 6);
        CHECK(changed.value().content_hash != baseline.value().content_hash);
    }
    SUBCASE("moving weight between components changes the rules, not the sum") {
        std::string text = shipped_text(kCrisisBalance);
        replace_once(text, "weight_fiscal = 200", "weight_fiscal = 250");
        replace_once(text, "weight_war = 150", "weight_war = 100");
        const auto changed = load_with(kCrisisBalance, text);
        REQUIRE(changed.has_value());
        CHECK(changed.value().rules.component_weights()[0] == 250);
        CHECK(changed.value().rules.component_weights()[1] == 100);
    }
    SUBCASE("a project level threshold") {
        std::string text = shipped_text(kProjects);
        replace_once(text, "thresholds = [ 20, 45, 70 ]", "thresholds = [ 25, 45, 70 ]");
        const auto changed = load_with(kProjects, text);
        REQUIRE(changed.has_value());
        CHECK(changed.value().rules.find_project(sim::crisis::ProjectKind::War)->thresholds[0] == 25);
        CHECK(changed.value().content_hash != baseline.value().content_hash);
    }
    SUBCASE("a loaded value is independent of file order") {
        auto source = crisis_source();
        source.add("crisis/zz_extra.pd", "format_version = 1\n");
        const auto again = content::load_crisis_content(source);
        REQUIRE(again.has_value());
        CHECK(again.value().content_hash == baseline.value().content_hash);
        CHECK(again.value().rules == baseline.value().rules);
    }
}

TEST_CASE("crisis content: invalid data is reported with the file and the position of the key") {
    std::string file{kCrisisBalance};
    std::string text;
    std::string anchor;
    std::string needle;
    auto code = data::ErrorCode::ContentRule;
    SUBCASE("a number outside the corridor") {
        text = shipped_text(kCrisisBalance);
        replace_once(text, "years = 5", "years = 77");
        anchor = "protect";
        needle = "years";
    }
    SUBCASE("weights that do not add up") {
        text = shipped_text(kCrisisBalance);
        replace_once(text, "weight_church = 80", "weight_church = 90");
        anchor = "components";
        needle = "weight_fiscal";
    }
    SUBCASE("a phase exit below its entrance") {
        text = shipped_text(kCrisisBalance);
        replace_once(text, "p3_exit = 42", "p3_exit = 20");
        anchor = "phases";
        needle = "p3_exit";
    }
    SUBCASE("a sigmoid of the wrong length") {
        text = shipped_text(kCrisisBalance);
        replace_once(text, "7, 11, 18,", "7, 11,");
        anchor = "crisis_balance";
        needle = "sigmoid = [";
    }
    SUBCASE("a typo in a balance key") {
        text = shipped_text(kCrisisBalance);
        replace_once(text, "legacy_decay_pm", "legacy_decay_pmm");
        anchor = "stability";
        needle = "legacy_decay_pmm";
        code = data::ErrorCode::UnknownField;
    }
    SUBCASE("a claim number outside the corridor") {
        text = shipped_text(kCrisisBalance);
        replace_once(text, "outside_line = -30", "outside_line = 30");
        anchor = "penalty";
        needle = "outside_line";
    }
    SUBCASE("blood terms that grow with the degree") {
        text = shipped_text(kCrisisBalance);
        replace_once(text, "blood = [ 40, 32, 24, 16, 8 ]", "blood = [ 40, 32, 50, 16, 8 ]");
        anchor = "claim_rules";
        needle = "blood";
    }
    SUBCASE("thresholds that do not ascend") {
        file = std::string(kProjects);
        text = shipped_text(kProjects);
        replace_once(text, "thresholds = [ 20, 45, 70 ]", "thresholds = [ 20, 45, 45 ]");
        anchor = "emperor_project \"war\"";
        needle = "thresholds";
    }
    SUBCASE("an effect of another project") {
        file = std::string(kProjects);
        text = shipped_text(kProjects);
        replace_once(text, "per_weight_kop = [", "per_weight_kopp = [");
        anchor = "emperor_project \"taxes\"";
        needle = "effects";
    }
    SUBCASE("two projects with the same slot") {
        file = std::string(kProjects);
        text = shipped_text(kProjects);
        replace_once(text, "slot = 2", "slot = 1");
        anchor = "emperor_project \"war\"";  // the later of the two objects in id order is reported
        needle = "slot = 1";
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

TEST_CASE("crisis content: missing and repeated objects are rejected") {
    SUBCASE("no crisis files at all") {
        auto source = crisis_source(false);
        const auto result = content::load_crisis_content(source);
        REQUIRE_FALSE(result.has_value());
        const auto report = content::format_errors(result.error());
        CHECK(report.find("no crisis_balance object found") != std::string::npos);
        CHECK(report.find("no claim_rules object found") != std::string::npos);
        CHECK(report.find("no emperor_project object found") != std::string::npos);
    }
    SUBCASE("a second balance object") {
        std::string text = shipped_text(kCrisisBalance);
        std::string second = text.substr(text.find("crisis_balance \"main\""));
        second = second.substr(0, second.find("# Претензии"));
        replace_once(second, "\"main\"", "\"extra\"");
        const auto result = load_with(kCrisisBalance, text + "\n" + second);
        REQUIRE_FALSE(result.has_value());
        INFO(content::format_errors(result.error()));
        CHECK(content::format_errors(result.error()).find("only one crisis_balance") != std::string::npos);
    }
}

TEST_CASE("crisis content: the three kinds stay out of the compatibility hash") {
    const auto with_crisis = [] {
        auto source = crisis_source();
        return data::load_content(source, data::content_schemas());
    }();
    const auto without_crisis = [] {
        auto source = crisis_source(false);
        return data::load_content(source, data::content_schemas());
    }();
    REQUIRE(with_crisis.ok());
    REQUIRE(without_crisis.ok());
    CHECK(with_crisis.registry.content_hash() != without_crisis.registry.content_hash());
    CHECK(with_crisis.registry.content_hash_excluding(data::kCrisisKinds) ==
          without_crisis.registry.content_hash());
    // The crisis ids are interned last, so no definition id (it reaches the world hash) shifts.
    const auto* trait_with = with_crisis.registry.find("trait", "ambitious");
    const auto* trait_without = without_crisis.registry.find("trait", "ambitious");
    REQUIRE(trait_with != nullptr);
    REQUIRE(trait_without != nullptr);
    CHECK(trait_with->id == trait_without->id);
    // Editing a crisis file leaves the excluded hash unchanged.
    std::string text = shipped_text(kCrisisBalance);
    replace_once(text, "years = 5", "years = 6");
    auto source = crisis_source();
    source.add(std::string(kCrisisBalance), text);
    const auto edited = data::load_content(source, data::content_schemas());
    REQUIRE(edited.ok());
    CHECK(edited.registry.content_hash() != with_crisis.registry.content_hash());
    CHECK(edited.registry.content_hash_excluding(data::kCrisisKinds) ==
          with_crisis.registry.content_hash_excluding(data::kCrisisKinds));
}

TEST_CASE("crisis schema: the balance tables equal the balance fields of the rules") {
    const auto check = [](const std::vector<std::tuple<std::string_view, std::string_view, bool>>& expected,
                          const std::vector<data::CrisisBalanceField>& table) {
        REQUIRE(table.size() == expected.size());
        for (std::size_t i = 0; i < expected.size(); ++i) {
            CAPTURE(std::get<1>(expected[i]));
            CHECK(table[i].group == std::get<0>(expected[i]));
            CHECK(table[i].name == std::get<1>(expected[i]));
            CHECK(table[i].wide == std::get<2>(expected[i]));
            CHECK_FALSE(table[i].description.empty());
        }
    };
    std::vector<std::tuple<std::string_view, std::string_view, bool>> balance;
#define PAT_TEST_BALANCE(group, type, name, def, lo, hi)                                                     \
    balance.emplace_back(#group, #name, sizeof(type) == 8);
    PAT_CRISIS_BALANCE_FIELDS(PAT_TEST_BALANCE)
#undef PAT_TEST_BALANCE
    CHECK(balance.size() == sim::crisis::kCrisisBalanceFieldCount);
    check(balance, data::crisis_balance_fields());
    std::vector<std::tuple<std::string_view, std::string_view, bool>> claims;
#define PAT_TEST_CLAIM(group, type, name, def, lo, hi) claims.emplace_back(#group, #name, sizeof(type) == 8);
    PAT_CLAIM_RULE_FIELDS(PAT_TEST_CLAIM)
#undef PAT_TEST_CLAIM
    CHECK(claims.size() == sim::crisis::kClaimRuleFieldCount);
    check(claims, data::claim_rule_fields());
    const auto schemas = data::content_schemas();
    for (const std::string_view kind : data::kCrisisKinds) {
        CHECK(schemas.find(kind) != nullptr);
    }
}

TEST_CASE("crisis localisation: every key the rules and the code name exists in the Russian table") {
    data::MemorySource source("crisis loc");
    source.add("loc/ru/crisis.pd", shipped_text("loc/ru/crisis.pd"));
    const auto loaded = loc::load_localization(source);
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
    for (std::size_t i = 0; i < sim::crisis::kComponentCount; ++i) {
        require_key(sim::crisis::component_key(static_cast<sim::crisis::Component>(i)));
    }
    CHECK(sim::crisis::component_key(static_cast<sim::crisis::Component>(sim::crisis::kComponentCount))
              .empty());
    require_key(sim::crisis::legacy_component_key());
    CHECK(sim::crisis::stability_reason_key(sim::crisis::StabilityReason::None).empty());
    for (std::uint8_t code = 1; code <= sim::crisis::kStabilityReasonCount; ++code) {
        require_key(sim::crisis::stability_reason_key(static_cast<sim::crisis::StabilityReason>(code)));
    }
    CHECK(sim::crisis::stability_reason_key(
              static_cast<sim::crisis::StabilityReason>(sim::crisis::kStabilityReasonCount + 1))
              .empty());
    for (std::uint8_t phase = 0; phase < sim::crisis::kPhaseCount; ++phase) {
        require_key(sim::crisis::phase_name_key(phase));
        require_key(sim::crisis::phase_hint_key(phase));
    }
    CHECK(sim::crisis::phase_name_key(sim::crisis::kPhaseCount).empty());
    for (std::uint8_t band = 0; band < 4; ++band) {
        require_key(sim::crisis::band_key(band));
    }
    CHECK(sim::crisis::band_key(4).empty());
    for (const auto& project : sim::crisis::CrisisRules::defaults().projects) {
        require_key(project.name_key);
        require_key(project.desc_key);
    }
}
