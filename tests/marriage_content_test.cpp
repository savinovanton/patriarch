// Marriage balance and Russian localisation content (E8-T07).
#include "pat/content/marriage_content.hpp"
#include "pat/data/content_source.hpp"
#include "pat/loc/localizer.hpp"
#include "pat/sim/marriage_check.hpp"
#include "pat/sim/marriage_score.hpp"
#include "pat/sim/marriage_text.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

using namespace pat;

namespace {

std::string shipped_marriage() {
    const data::DirectorySource directory(PATRIARCH_DATA_DIR);
    auto body = directory.read_file("balance/marriage.pd");
    REQUIRE(body.has_value());
    std::string text = std::move(body).value();
    text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
    return text;
}

content::MarriageContentResult load_marriage(std::string text) {
    data::MemorySource source("marriage balance");
    source.add("balance/marriage.pd", std::move(text));
    return content::load_marriage_content(source);
}

void replace_once(std::string& text, std::string_view before, std::string_view after) {
    const auto offset = text.find(before);
    REQUIRE(offset != std::string::npos);
    REQUIRE(text.find(before, offset + before.size()) == std::string::npos);
    text.replace(offset, before.size(), after);
}

// Source positions use the parser's one-based line/column convention. The fixture needles are ASCII.
data::SourcePos position_of(const std::string& text, std::string_view needle) {
    const auto offset = text.find(needle);
    REQUIRE(offset != std::string::npos);
    const auto previous_line = text.rfind('\n', offset);
    return {static_cast<std::uint32_t>(std::count(text.begin(), text.begin() + offset, '\n') + 1),
            static_cast<std::uint32_t>(offset - (previous_line == std::string::npos ? 0 : previous_line + 1) +
                                       1)};
}

}  // namespace

TEST_CASE("marriage content: shipped balance equals every fallback field") {
    const auto loaded = load_marriage(shipped_marriage());
    if (!loaded.has_value()) {
        INFO(content::format_errors(loaded.error()));
    }
    REQUIRE(loaded.has_value());
    CHECK(loaded.value().rules == sim::MarriageRules::defaults());
    CHECK(loaded.value().content_hash != 0);
}

TEST_CASE("marriage content: an edited number reaches the existing calculation and content hash") {
    const std::string original = shipped_marriage();
    const auto baseline = load_marriage(original);
    REQUIRE(baseline.has_value());
    std::string edited = original;
    replace_once(edited, "envoy_road_kop = 8000", "envoy_road_kop = 9000");
    const auto changed = load_marriage(edited);
    REQUIRE(changed.has_value());
    CHECK(changed.value().content_hash != baseline.value().content_hash);
    CHECK(sim::envoy_cost(changed.value().rules, false) ==
          sim::envoy_cost(baseline.value().rules, false) + 1000);
    CHECK(sim::envoy_cost(changed.value().rules, true) ==
          sim::envoy_cost(baseline.value().rules, true) + 1000);
    CHECK(load_marriage(original).value().content_hash == baseline.value().content_hash);
}

TEST_CASE("marriage content: invalid fields retain the exact file and source position") {
    std::string text = shipped_marriage();
    std::string_view needle;
    auto expected_code = data::ErrorCode::ContentRule;
    SUBCASE("a typo in a nested field") {
        replace_once(text, "rank_step_penalty", "rank_step_penaty");
        needle = "rank_step_penaty";
        expected_code = data::ErrorCode::UnknownField;
    }
    SUBCASE("a cross-field dowry corridor") {
        replace_once(text, "dowry_min_kop = 5000", "dowry_min_kop = 100001");
        needle = "100001";
    }
    SUBCASE("a rank list with the wrong length") {
        replace_once(text, "[ -1000, 0, 100, 500, 1000 ]", "[ -1000, 0, 100, 500 ]");
        needle = "[ -1000";
    }
    SUBCASE("a rank list with a decreasing step") {
        replace_once(text, "[ -1000, 0, 100, 500, 1000 ]", "[ -1000, 0, 100, 99, 1000 ]");
        needle = "99,";
    }
    SUBCASE("a negative timing value") {
        replace_once(text, "answer_days = 30", "answer_days = -1");
        needle = "-1\n";
    }
    REQUIRE_FALSE(needle.empty());
    const auto result = load_marriage(text);
    REQUIRE_FALSE(result.has_value());
    REQUIRE_FALSE(result.error().empty());
    const auto found = std::find_if(result.error().begin(), result.error().end(), [&](const auto& error) {
        return error.diagnostic.code == expected_code;
    });
    REQUIRE(found != result.error().end());
    const auto& error = *found;
    CAPTURE(error.format());
    CHECK(error.file == "balance/marriage.pd");
    CHECK(error.diagnostic.pos == position_of(text, needle));
}

TEST_CASE("marriage content: missing repeated and malformed objects are rejected") {
    SUBCASE("no rules object") {
        CHECK_FALSE(load_marriage("format_version = 1\n").has_value());
    }
    SUBCASE("two distinct rules objects") {
        std::string text = shipped_marriage();
        std::string second = text.substr(text.find("marriage_rules"));
        replace_once(second, "\"main\"", "\"extra\"");
        CHECK_FALSE(load_marriage(text + "\n" + second).has_value());
    }
    SUBCASE("repeated identity") {
        std::string text = shipped_marriage();
        const std::string second = text.substr(text.find("marriage_rules"));
        CHECK_FALSE(load_marriage(text + "\n" + second).has_value());
    }
    SUBCASE("missing required group") {
        std::string text = shipped_marriage();
        text.erase(text.find("    matchmaker {"));
        text += "}\n";
        CHECK_FALSE(load_marriage(text).has_value());
    }
    SUBCASE("invalid syntax") {
        CHECK_FALSE(load_marriage("format_version = 1\nmarriage_rules \"main\" {\n").has_value());
    }
}

TEST_CASE("marriage content: faith owns strings and binds stable keys in World id order") {
    auto loaded = [] {
        data::MemorySource source("marriage faith");
        source.add("balance/marriage.pd", shipped_marriage());
        source.add("dynasties.pd", R"(format_version = 1
dynasty "a_catholic" { name_key = "a.name" desc_key = "a.desc" faith = "catholic" }
dynasty "b_default" { name_key = "b.name" desc_key = "b.desc" }
dynasty "z_protestant" { name_key = "z.name" desc_key = "z.desc" faith = "protestant" }
)");
        return content::load_marriage_content(source);
    }();
    if (!loaded.has_value()) {
        INFO(content::format_errors(loaded.error()));
    }
    REQUIRE(loaded.has_value());
    // The source is gone and the payload is moved before creating any borrowed views.
    auto moved = std::move(loaded).value();
    REQUIRE(moved.faith_by_dynasty.size() == 3);
    const std::array<std::string_view, 5> keys{"z_protestant", "a_catholic", "b_default", "", "undefined"};
    const auto faith_by_id = content::bind_marriage_faith(moved.faith_by_dynasty, keys);
    REQUIRE(faith_by_id.size() == keys.size());
    const sim::FaithPolicy policy(faith_by_id);
    CHECK(policy.faith_of(sim::DynastyId(0)) == "protestant");
    CHECK(policy.faith_of(sim::DynastyId(1)) == "catholic");
    CHECK(policy.faith_of(sim::DynastyId(2)) == "orthodox");
    CHECK(policy.faith_of(sim::DynastyId(3)) == "orthodox");
    CHECK(policy.faith_of(sim::DynastyId(4)) == "orthodox");
}

TEST_CASE("marriage content: Russian table covers all issued denial and breakdown keys") {
    const data::DirectorySource directory(PATRIARCH_DATA_DIR);
    const auto body = directory.read_file("loc/ru/marriage.pd");
    REQUIRE(body.has_value());
    data::MemorySource source("marriage localisation");
    source.add("loc/ru/marriage.pd", body.value());
    const auto loaded = loc::load_localization(source);
    std::string report;
    for (const auto& error : loaded.errors) {
        report += error.format() + "\n";
    }
    INFO(report);
    REQUIRE(loaded.ok());
    const auto& strings = loaded.localizer;
    for (std::uint16_t value = 1; value <= 82; ++value) {
        const auto key = sim::marriage_denial_key(static_cast<sim::MarriageDenial>(value));
        if (!key.empty()) {
            CAPTURE(key);
            CHECK(strings.contains("ru", key));
            CHECK_FALSE(strings.format(key).empty());
        }
    }
    for (std::uint16_t code = 10000; code <= 10204; ++code) {
        const auto key = sim::marriage_term_key(code);
        if (!key.empty()) {
            CAPTURE(key);
            CHECK(strings.contains("ru", key));
            CHECK_FALSE(strings.format(key).empty());
        }
    }
    const std::array state_keys{
        sim::kMarriageNegotiationPendingKey,
        sim::kMarriageBetrothalActiveKey,
        sim::kMarriageBetrothalBrokenKey,
        sim::kMarriageBetrothalReadyKey,
        sim::marriage_negotiation_result_key(sim::family::NegotiationResult::Accepted),
        sim::marriage_negotiation_result_key(sim::family::NegotiationResult::Refused),
        sim::marriage_negotiation_result_key(sim::family::NegotiationResult::Withdrawn),
        sim::marriage_negotiation_result_key(sim::family::NegotiationResult::Void),
        sim::marriage_betrothal_origin_key(sim::family::BetrothalOrigin::Negotiation),
        sim::marriage_betrothal_origin_key(sim::family::BetrothalOrigin::Command),
        sim::marriage_state_key(sim::family::MarriageState::Active),
        sim::marriage_state_key(sim::family::MarriageState::Widowed),
        sim::marriage_state_key(sim::family::MarriageState::Closed),
        sim::marriage_end_reason_key(sim::family::MarriageEndReason::None),
        sim::marriage_end_reason_key(sim::family::MarriageEndReason::Widowhood),
        sim::marriage_end_reason_key(sim::family::MarriageEndReason::Tonsure),
        sim::marriage_end_reason_key(sim::family::MarriageEndReason::Annulment),
        sim::marriage_end_reason_key(sim::family::MarriageEndReason::BothDead),
    };
    for (const std::string_view key : state_keys) {
        CAPTURE(key);
        CHECK_FALSE(key.empty());
        CHECK(strings.contains("ru", key));
        CHECK_FALSE(strings.format(key).empty());
    }
    for (std::uint16_t param = 201; param <= 208; ++param) {
        const auto key = sim::marriage_chronicle_key(param);
        CAPTURE(key);
        CHECK_FALSE(key.empty());
        CHECK(strings.contains("ru", key));
        std::string text;
        const auto result = strings.format_to(
            text,
            key,
            {loc::Arg("groom", "Иван"), loc::Arg("bride", "Анна"), loc::Arg("character", "Алексей")});
        CHECK(result.ok());
        CHECK_FALSE(text.empty());
        CHECK(text.find('{') == std::string::npos);
    }
    CHECK(sim::marriage_negotiation_result_key(static_cast<sim::family::NegotiationResult>(0)).empty());
    CHECK(sim::marriage_betrothal_origin_key(static_cast<sim::family::BetrothalOrigin>(0)).empty());
    CHECK(sim::marriage_state_key(static_cast<sim::family::MarriageState>(0)).empty());
    CHECK(sim::marriage_end_reason_key(static_cast<sim::family::MarriageEndReason>(255)).empty());
    CHECK(sim::marriage_chronicle_key(200).empty());
    CHECK(sim::marriage_chronicle_key(209).empty());
}
