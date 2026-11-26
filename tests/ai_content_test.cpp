#include "pat/content/ai_content.hpp"
#include "pat/data/ai_schemas.hpp"
#include "pat/data/content_schemas.hpp"
#include "pat/loc/localizer.hpp"
#include "pat/sim/ai/text.hpp"

#include <doctest/doctest.h>

#include <array>
#include <format>
#include <string>
#include <string_view>

#include "court_fixture.hpp"
using namespace pat;
namespace {
constexpr std::string_view kBalance = "balance/ai.pd";
data::MemorySource ai_source(std::string balance = court_fixture::shipped_text(kBalance)) {
    data::MemorySource source("AI fixture");
    source.add(std::string(kBalance), std::move(balance));
    source.add("dynasties/m1.pd", court_fixture::shipped_text("dynasties/m1.pd"));
    source.add("scenarios/m1/ai_agents.pd", court_fixture::shipped_text("scenarios/m1/ai_agents.pd"));
    return source;
}
void check_failure(std::string text, std::string_view anchor, std::string_view key) {
    const auto loaded = content::load_ai_content(ai_source(text));
    REQUIRE_FALSE(loaded.has_value());
    REQUIRE(loaded.error().size() == 1);
    const auto& error = loaded.error().front();
    CHECK(error.file == kBalance);
    CHECK(error.diagnostic.pos == court_fixture::position_after(text, anchor, key));
    CHECK_FALSE(error.diagnostic.message.empty());
}
}  // namespace
TEST_CASE("ai rules: defaults match all shipped values and stable M1 table sizes") {
    const auto loaded = content::load_ai_content(ai_source());
    if (!loaded)
        INFO(content::format_errors(loaded.error()));
    REQUIRE(loaded.has_value());
    const auto defaults = sim::ai::AiRules::defaults();
    CHECK(sim::ai::validate(defaults).has_value());
    CHECK(loaded.value().rules == defaults);
    CHECK(loaded.value().rules.hash() == defaults.hash());
    CHECK(loaded.value().agents.empty());  // E30-T08 owns the final roster.
    CHECK(defaults.archetypes.size() == 8);
    CHECK(defaults.archetypes.front().weights.size() == 7);
    CHECK(defaults.difficulty.size() == 3);
    CHECK(static_cast<std::uint8_t>(sim::ai::GoalKind::KeepOrder) == 7);
    CHECK(static_cast<std::uint8_t>(sim::ai::Archetype::Reformer) == 8);
    CHECK(defaults.archetypes[3].weights[5] == 1500);
    CHECK(defaults.difficulty[2].ap_mult_pm == 1200);
}
TEST_CASE("ai rules: every scalar participates in validation and the canonical hash") {
    const auto defaults = sim::ai::AiRules::defaults();
#define PAT_AI_TEST(name, def, lo, hi)                                                                       \
    {                                                                                                        \
        auto rules = defaults;                                                                               \
        ++rules.budget.name;                                                                                 \
        CHECK(rules.hash() != defaults.hash());                                                              \
        rules.budget.name = (lo) - 1;                                                                        \
        const auto invalid = sim::ai::validate(rules);                                                       \
        REQUIRE_FALSE(invalid.has_value());                                                                  \
        CHECK(invalid.error().field == "budget." #name);                                                     \
    }
    PAT_AI_BUDGET_FIELDS(PAT_AI_TEST)
#undef PAT_AI_TEST
#define PAT_AI_TEST(name, def, lo, hi)                                                                       \
    {                                                                                                        \
        auto rules = defaults;                                                                               \
        ++rules.schedule.name;                                                                               \
        CHECK(rules.hash() != defaults.hash());                                                              \
        rules.schedule.name = (lo) - 1;                                                                      \
        const auto invalid = sim::ai::validate(rules);                                                       \
        REQUIRE_FALSE(invalid.has_value());                                                                  \
        CHECK(invalid.error().field == "schedule." #name);                                                   \
    }
    PAT_AI_SCHEDULE_FIELDS(PAT_AI_TEST)
#undef PAT_AI_TEST
#define PAT_AI_TEST(name, def, lo, hi)                                                                       \
    {                                                                                                        \
        auto rules = defaults;                                                                               \
        ++rules.attention.name;                                                                              \
        CHECK(rules.hash() != defaults.hash());                                                              \
        rules.attention.name = (lo) - 1;                                                                     \
        const auto invalid = sim::ai::validate(rules);                                                       \
        REQUIRE_FALSE(invalid.has_value());                                                                  \
        CHECK(invalid.error().field == "attention." #name);                                                  \
    }
    PAT_AI_ATTENTION_FIELDS(PAT_AI_TEST)
#undef PAT_AI_TEST
#define PAT_AI_TEST(name, def, lo, hi)                                                                       \
    {                                                                                                        \
        auto rules = defaults;                                                                               \
        ++rules.goals.name;                                                                                  \
        CHECK(rules.hash() != defaults.hash());                                                              \
        rules.goals.name = (lo) - 1;                                                                         \
        const auto invalid = sim::ai::validate(rules);                                                       \
        REQUIRE_FALSE(invalid.has_value());                                                                  \
        CHECK(invalid.error().field == "goals." #name);                                                      \
    }
    PAT_AI_GOALS_FIELDS(PAT_AI_TEST)
#undef PAT_AI_TEST
#define PAT_AI_TEST(name, def, lo, hi)                                                                       \
    {                                                                                                        \
        auto rules = defaults;                                                                               \
        ++rules.hysteresis.name;                                                                             \
        CHECK(rules.hash() != defaults.hash());                                                              \
        rules.hysteresis.name = (lo) - 1;                                                                    \
        const auto invalid = sim::ai::validate(rules);                                                       \
        REQUIRE_FALSE(invalid.has_value());                                                                  \
        CHECK(invalid.error().field == "hysteresis." #name);                                                 \
    }
    PAT_AI_HYSTERESIS_FIELDS(PAT_AI_TEST)
#undef PAT_AI_TEST
#define PAT_AI_TEST(name, def, lo, hi)                                                                       \
    {                                                                                                        \
        auto rules = defaults;                                                                               \
        ++rules.commitment.name;                                                                             \
        CHECK(rules.hash() != defaults.hash());                                                              \
        rules.commitment.name = (lo) - 1;                                                                    \
        const auto invalid = sim::ai::validate(rules);                                                       \
        REQUIRE_FALSE(invalid.has_value());                                                                  \
        CHECK(invalid.error().field == "commitment." #name);                                                 \
    }
    PAT_AI_COMMITMENT_FIELDS(PAT_AI_TEST)
#undef PAT_AI_TEST
#define PAT_AI_TEST(name, def, lo, hi)                                                                       \
    {                                                                                                        \
        auto rules = defaults;                                                                               \
        ++rules.cooldowns.name;                                                                              \
        CHECK(rules.hash() != defaults.hash());                                                              \
        rules.cooldowns.name = (lo) - 1;                                                                     \
        const auto invalid = sim::ai::validate(rules);                                                       \
        REQUIRE_FALSE(invalid.has_value());                                                                  \
        CHECK(invalid.error().field == "cooldowns." #name);                                                  \
    }
    PAT_AI_COOLDOWNS_FIELDS(PAT_AI_TEST)
#undef PAT_AI_TEST
#define PAT_AI_TEST(name, def, lo, hi)                                                                       \
    {                                                                                                        \
        auto rules = defaults;                                                                               \
        ++rules.archetype_change.name;                                                                       \
        CHECK(rules.hash() != defaults.hash());                                                              \
        rules.archetype_change.name = (lo) - 1;                                                              \
        const auto invalid = sim::ai::validate(rules);                                                       \
        REQUIRE_FALSE(invalid.has_value());                                                                  \
        CHECK(invalid.error().field == "archetype_change." #name);                                           \
    }
    PAT_AI_ARCHETYPE_CHANGE_FIELDS(PAT_AI_TEST)
#undef PAT_AI_TEST
#define PAT_AI_TEST(name, def, lo, hi)                                                                       \
    {                                                                                                        \
        auto rules = defaults;                                                                               \
        ++rules.command_costs.name;                                                                          \
        CHECK(rules.hash() != defaults.hash());                                                              \
        rules.command_costs.name = (lo) - 1;                                                                 \
        const auto invalid = sim::ai::validate(rules);                                                       \
        REQUIRE_FALSE(invalid.has_value());                                                                  \
        CHECK(invalid.error().field == "command_costs." #name);                                              \
    }
    PAT_AI_COMMAND_COSTS_FIELDS(PAT_AI_TEST)
#undef PAT_AI_TEST
    for (std::size_t a = 0; a < defaults.archetypes.size(); ++a) {
        for (std::size_t g = 0; g < defaults.archetypes[a].weights.size(); ++g) {
            auto rules = defaults;
            ++rules.archetypes[a].weights[g];
            CHECK(rules.hash() != defaults.hash());
            rules.archetypes[a].weights[g] = 3001;
            CHECK_FALSE(sim::ai::validate(rules).has_value());
        }
    }
    for (std::size_t i = 0; i < defaults.difficulty.size(); ++i) {
        auto rules = defaults;
        ++rules.difficulty[i].noise_pm;
        CHECK(rules.hash() != defaults.hash());
        rules.difficulty[i].noise_pm = 1001;
        CHECK_FALSE(sim::ai::validate(rules).has_value());
    }
    auto rules = defaults;
    rules.wealth_bands[2] = rules.wealth_bands[1];
    CHECK_FALSE(sim::ai::validate(rules).has_value());
    rules = defaults;
    rules.archetype_follows_head = false;
    CHECK(rules.hash() != defaults.hash());
}
TEST_CASE("ai content: an edited balance number reaches callers and changes the AI hash") {
    auto text = court_fixture::shipped_text(kBalance);
    court_fixture::replace_once(text, "quota_wu = 12000", "quota_wu = 13000");
    const auto loaded = content::load_ai_content(ai_source(text));
    REQUIRE(loaded.has_value());
    CHECK(loaded.value().rules.budget.quota_wu == 13000);
    CHECK(loaded.value().rules.hash() != sim::ai::AiRules::defaults().hash());
    auto weight = court_fixture::shipped_text(kBalance);
    court_fixture::replace_once(weight, "gain_office = 1400", "gain_office = 1450");
    const auto changed = content::load_ai_content(ai_source(weight));
    REQUIRE(changed.has_value());
    CHECK(changed.value().rules.archetypes[0].weights[0] == 1450);
    CHECK(changed.value().rules.hash() != sim::ai::AiRules::defaults().hash());
}
TEST_CASE("ai content: diagnostics identify bad values and cross-field bounds") {
    auto text = court_fixture::shipped_text(kBalance);
    SUBCASE("out-of-range weight") {
        court_fixture::replace_once(text, "gain_office = 1400", "gain_office = 3001");
        check_failure(text, "ambitious =", "gain_office");
    }
    SUBCASE("invalid divisor") {
        court_fixture::replace_once(text, "court_divisor = 4", "court_divisor = 0");
        check_failure(text, "attention =", "court_divisor");
    }
    SUBCASE("unordered tiers") {
        court_fixture::replace_once(text, "tier2_days = 30", "tier2_days = 1");
        check_failure(text, "schedule =", "tier2_days");
    }
    SUBCASE("unknown goal") {
        court_fixture::replace_once(text, "gain_office = 1400", "invented_goal = 1400");
        const auto loaded = content::load_ai_content(ai_source(text));
        REQUIRE_FALSE(loaded.has_value());
        CHECK_FALSE(loaded.error().empty());
    }
    SUBCASE("missing archetype") {
        court_fixture::replace_once(text, "reformer = {", "invented_archetype = {");
        const auto loaded = content::load_ai_content(ai_source(text));
        REQUIRE_FALSE(loaded.has_value());
    }
    SUBCASE("extra difficulty") {
        court_fixture::replace_once(text, "ruthless = {", "invented_difficulty = {");
        const auto loaded = content::load_ai_content(ai_source(text));
        REQUIRE_FALSE(loaded.has_value());
    }
}
TEST_CASE("ai content: effect signatures use the supplied E11 catalog and reject unknown identifiers") {
    auto text = court_fixture::shipped_text(kBalance);
    court_fixture::replace_once(
        text,
        "event_goal_signatures = []",
        R"(event_goal_signatures = [ { effect = test_honor goal = gain_office weight_pm = 200 } ])");
    sim::events::Catalog catalog = sim::events::Catalog::engine();
    sim::events::CatalogEntry effect;
    effect.space = sim::events::NameSpace::Effect;
    effect.code = 2300;
    effect.name = "test_honor";
    catalog.add(std::move(effect));
    const auto source = ai_source(text);
    const auto loaded = content::load_ai_content(source, {}, &catalog);
    REQUIRE(loaded.has_value());
    REQUIRE(loaded.value().rules.event_goal_signatures.size() == 1);
    CHECK(loaded.value().rules.event_goal_signatures.front().goal == sim::ai::GoalKind::GainOffice);
    CHECK(loaded.value().rules.hash() != sim::ai::AiRules::defaults().hash());
    const auto unknown = content::load_ai_content(source);
    REQUIRE_FALSE(unknown.has_value());
    REQUIRE(unknown.error().size() == 1);
    CHECK(unknown.error()[0].diagnostic.pos ==
          court_fixture::position_after(text, "event_goal_signatures", "effect"));
    court_fixture::replace_once(text, "goal = gain_office", "goal = invented_goal");
    CHECK_FALSE(content::load_ai_content(ai_source(text), {}, &catalog).has_value());
}
TEST_CASE("ai content: scenario agents resolve existing houses and reject duplicates or player houses") {
    auto source = ai_source();
    source.add("scenarios/m1/ai_agents.pd", R"(format_version = 1
ai_agent "fixture" { dynasty = dynasty:golitsyn archetype = steward mode = agent force_tier = 1 }
)");
    const auto loaded = content::load_ai_content(source);
    REQUIRE(loaded.has_value());
    REQUIRE(loaded.value().agents.size() == 1);
    CHECK(
        loaded.value().agents.front() ==
        content::AiAgentDefinition{"golitsyn", sim::ai::Archetype::Steward, content::AiAgentMode::Agent, 1});
    const std::array<std::string_view, 1> players{"golitsyn"};
    const auto player = content::load_ai_content(source, players);
    REQUIRE_FALSE(player.has_value());
    REQUIRE(player.error().size() == 1);
    CHECK(player.error()[0].diagnostic.pos == data::SourcePos{2, 22});
    source.add("scenarios/m1/duplicate.pd", R"(format_version = 1
ai_agent "duplicate" { dynasty = dynasty:golitsyn archetype = pious mode = background }
)");
    CHECK_FALSE(content::load_ai_content(source).has_value());
}
TEST_CASE("ai content: disabled AI leaves existing definition ids and compatibility hash unchanged") {
    data::MemorySource before("before");
    before.add("dynasties/m1.pd", court_fixture::shipped_text("dynasties/m1.pd"));
    const auto baseline = data::load_content(before, data::content_schemas());
    const auto with_ai = data::load_content(ai_source(), data::content_schemas());
    REQUIRE(baseline.ok());
    REQUIRE(with_ai.ok());
    CHECK(with_ai.registry.content_hash_excluding(data::kAiKinds) == baseline.registry.content_hash());
    for (const auto& object : baseline.registry.table("dynasty")->objects()) {
        CHECK(with_ai.registry.ids().find(object.object.id) ==
              baseline.registry.ids().find(object.object.id));
    }
}
TEST_CASE("ai localisation: all seven goals eight archetypes three levels and common reasons exist") {
    data::MemorySource source("AI loc");
    source.add("loc/ru/ai.pd", court_fixture::shipped_text("loc/ru/ai.pd"));
    const auto loaded = loc::load_localization(source);
    REQUIRE(loaded.ok());
    for (std::uint8_t i = 1; i <= sim::ai::kGoalKeys.size(); ++i)
        CHECK(loaded.localizer.contains("ru", sim::ai::goal_key(static_cast<sim::ai::GoalKind>(i))));
    for (std::uint8_t i = 1; i <= sim::ai::kArchetypeKeys.size(); ++i)
        CHECK(loaded.localizer.contains("ru", sim::ai::archetype_key(static_cast<sim::ai::Archetype>(i))));
    for (std::uint8_t i = 1; i <= sim::ai::kDifficultyKeys.size(); ++i)
        CHECK(loaded.localizer.contains("ru", sim::ai::difficulty_key(static_cast<sim::ai::Difficulty>(i))));
    for (std::uint16_t i = 1; i <= 16; ++i)
        CHECK(loaded.localizer.contains("ru", sim::ai::reason_key(i)));
    for (auto key : {sim::ai::kTitleKey,
                     sim::ai::kDifficultyKey,
                     sim::ai::kApKey,
                     sim::ai::kWuKey,
                     sim::ai::kQueueKey,
                     sim::ai::kFrameKey,
                     sim::ai::kLogKey,
                     sim::ai::kGoalKey,
                     sim::ai::kArchetypeKey,
                     sim::ai::kTierKey,
                     sim::ai::kDueKey,
                     sim::ai::kCommitmentKey,
                     sim::ai::kCooldownKey,
                     sim::ai::kDevOnlyKey,
                     sim::ai::kFactorsKey})
        CHECK(loaded.localizer.contains("ru", key));
    CHECK(sim::ai::goal_key(static_cast<sim::ai::GoalKind>(0)).empty());
    CHECK(sim::ai::archetype_key(static_cast<sim::ai::Archetype>(255)).empty());
    CHECK(sim::ai::difficulty_key(static_cast<sim::ai::Difficulty>(0)).empty());
    CHECK(sim::ai::reason_key(0).empty());
    CHECK(sim::ai::reason_key(17).empty());
}
