#include "pat/content/diplomacy_content.hpp"
#include "pat/data/content_schemas.hpp"
#include "pat/data/diplomacy_schemas.hpp"
#include "pat/loc/localizer.hpp"
#include "pat/sim/diplomacy/text.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <span>
#include <string>
#include <string_view>

#include "court_fixture.hpp"

using namespace pat;
namespace {

using court_fixture::position_after;
using court_fixture::replace_once;
using court_fixture::shipped_text;

constexpr std::string_view kBalance = "balance/diplomacy.pd";
constexpr std::string_view kTraits = "traits/m1.pd";

data::MemorySource shipped_source() {
    auto source = court_fixture::shipped_source(false);
    source.add(std::string(kBalance), shipped_text(kBalance));
    return source;
}

}  // namespace

TEST_CASE("diplomacy content: four shipped treaty templates and trait hints equal validated defaults") {
    const auto defaults = sim::diplomacy::DiplomacyRules::defaults();
    const auto checked = sim::diplomacy::validate(defaults);
    if (!checked)
        INFO(checked.error().field << ": " << checked.error().message);
    REQUIRE(checked.has_value());

    const auto loaded = content::load_diplomacy_content(shipped_source());
    if (!loaded)
        INFO(content::format_errors(loaded.error()));
    REQUIRE(loaded.has_value());
    const auto& rules = loaded.value().rules;
    CHECK(rules == defaults);
    CHECK(loaded.value().rules_hash == defaults.hash());
    CHECK(rules.limits_treaties_per_house == 8);
    CHECK(rules.limits_player_inbox_cap == 2);
    CHECK(rules.offers_answer_days == 30);
    CHECK(rules.ai_may_offer_to_player);
    for (std::size_t i = 0; i < sim::diplomacy::kTreatyNames.size(); ++i) {
        const auto kind = sim::diplomacy::kTreatyNames[i];
        CHECK(rules.treaties[i].enabled == (kind != "claim_support"));
        CHECK(rules.treaties[i].ai_may_offer_to_player == (kind == "alliance" || kind == "pledge"));
    }
}

TEST_CASE("diplomacy content: opt-in kinds preserve the existing content hash and trait IDs") {
    const auto base = data::load_content(court_fixture::shipped_source(false), data::content_schemas());
    REQUIRE(base.ok());
    const auto loaded = data::load_content(shipped_source(), data::content_schemas());
    REQUIRE(loaded.ok());
    CHECK(loaded.registry.content_hash() != base.registry.content_hash());
    CHECK(loaded.registry.content_hash_excluding(data::kDiplomacyKinds) == base.registry.content_hash());
    const auto* traits = base.registry.table("trait");
    REQUIRE(traits != nullptr);
    for (const auto& trait : traits->objects()) {
        const auto* retained = loaded.registry.find("trait", trait.object.id);
        REQUIRE(retained != nullptr);
        CHECK(retained->id == trait.id);
    }
}

TEST_CASE("diplomacy content: balance edits reach typed rules and alter their hash") {
    const auto source = shipped_source();
    const auto baseline = content::load_diplomacy_content(source);
    REQUIRE(baseline.has_value());

    auto edited = shipped_text(kBalance);
    replace_once(edited, "dipl_greed = 500", "dipl_greed = 600");
    data::MemorySource mod("diplomacy mod");
    mod.add(std::string(kBalance), edited);
    const data::ContentSource* roots[] = {&source, &mod};
    const auto loaded = content::load_diplomacy_content(roots);
    REQUIRE(loaded.has_value());
    CHECK(loaded.value().rules_hash != baseline.value().rules_hash);
    auto expected = baseline.value().rules;
    const auto& names = sim::diplomacy::kTraitHintNames;
    const auto greedy = std::ranges::find(names, "greedy");
    REQUIRE(greedy != names.end());
    expected.trait_hints[static_cast<std::size_t>(greedy - names.begin())].dipl_greed = 600;
    CHECK(loaded.value().rules == expected);
}

TEST_CASE("diplomacy content: canonical hash is independent of treaty declaration order") {
    auto edited = shipped_text(kBalance);
    const auto first = edited.find("treaty_kind \"alliance\"");
    const auto second = edited.find("treaty_kind \"pledge\"");
    REQUIRE(first != std::string::npos);
    REQUIRE(second != std::string::npos);
    const auto alliance = edited.substr(first, second - first);
    edited.erase(first, second - first);
    edited += "\n# The same alliance declared last\n" + alliance;
    auto source = shipped_source();
    source.add(std::string(kBalance), edited);
    const auto loaded = content::load_diplomacy_content(source);
    REQUIRE(loaded.has_value());
    CHECK(loaded.value().rules_hash == sim::diplomacy::DiplomacyRules::defaults().hash());
}

TEST_CASE("diplomacy content: invalid and inconsistent edits have precise source positions") {
    auto source = shipped_source();
    auto balance = shipped_text(kBalance);
    std::string_view anchor;
    std::string_view field;
    std::string_view message;
    SUBCASE("unknown hint key") {
        replace_once(balance, "dipl_greed = 500", "dipl_greed_typo = 500");
        anchor = "greedy =";
        field = "dipl_greed_typo";
        message = "dipl_greed_typo";
    }
    SUBCASE("missing referenced trait") {
        auto traits = shipped_text(kTraits);
        replace_once(traits, "trait \"greedy\"", "trait \"modded_greedy\"");
        replace_once(traits, "opposite = trait:greedy", "opposite = trait:modded_greedy");
        source.add(std::string(kTraits), traits);
        anchor = "trait_hints";
        field = "greedy =";
        message = "unknown diplomacy trait 'greedy'";
    }
    SUBCASE("negative greed") {
        replace_once(balance, "dipl_greed = 500", "dipl_greed = -1");
        anchor = "greedy =";
        field = "dipl_greed";
        message = "trait_hints.greedy.dipl_greed";
    }
    SUBCASE("inconsistent opinion weights") {
        replace_once(balance, "head_weight_pm = 300", "head_weight_pm = 400");
        anchor = "opinion =";
        field = "head_weight_pm";
        message = "weights must total 1000";
    }
    SUBCASE("wrong stable treaty ID") {
        replace_once(balance, "id = 4", "id = 3");
        anchor = "treaty_kind \"claim_support\"";
        field = "id =";
        message = "treaty ID does not match";
    }
    SUBCASE("claim offers to player are disallowed") {
        const auto treaty = balance.find("treaty_kind \"claim_support\"");
        REQUIRE(treaty != std::string::npos);
        const auto flag = balance.find("ai_may_offer_to_player = no", treaty);
        REQUIRE(flag != std::string::npos);
        balance.replace(
            flag, std::string_view("ai_may_offer_to_player = no").size(), "ai_may_offer_to_player = yes");
        anchor = "treaty_kind \"claim_support\"";
        field = "ai_may_offer_to_player";
        message = "only alliances and pledges";
    }
    source.add(std::string(kBalance), balance);
    const auto loaded = content::load_diplomacy_content(source);
    REQUIRE_FALSE(loaded.has_value());
    INFO(content::format_errors(loaded.error()));
    const auto matching = std::ranges::find_if(loaded.error(), [&](const auto& error) {
        return error.diagnostic.message.find(message) != std::string::npos;
    });
    REQUIRE(matching != loaded.error().end());
    CHECK(matching->file == kBalance);
    CHECK(matching->diagnostic.pos == position_after(balance, anchor, field));
}

TEST_CASE("diplomacy localization: treaty, reason, denial, breakdown and UI keys resolve in Russian") {
    using namespace sim::diplomacy;
    const data::DirectorySource root{std::string(PATRIARCH_DATA_DIR)};
    const auto localized = loc::load_localization(root);
    REQUIRE(localized.ok());
    const auto check_key = [&](std::string_view key) {
        INFO(key);
        CHECK(localized.localizer.contains("ru", key));
        std::string rendered;
        REQUIRE(localized.localizer.format_to(rendered, key).ok());
        CHECK_FALSE(rendered.empty());
    };
    for (const auto group : {std::span<const TextKey>(kTreatyTexts),
                             std::span<const TextKey>(kRelationReasonTexts),
                             std::span<const TextKey>(kReplyReasonTexts),
                             std::span<const TextKey>(kDenialTexts),
                             std::span<const TextKey>(kBreakdownTexts),
                             std::span<const TextKey>(kAiReasonTexts)}) {
        for (const auto& entry : group) {
            CHECK(text_key(group, entry.code) == entry.key);
            check_key(entry.key);
        }
    }
    CHECK(text_key(kBreakdownTexts, 20000) == "dipl.term.code_20000");
    CHECK(text_key(kAiReasonTexts, 450) == "ai.reason.code_450");
    CHECK(text_key(kDenialTexts, 0).empty());
    CHECK(text_key(kDenialTexts, 29).empty());
    for (const auto key : {kUiDiplomacyKey,
                           kUiRelationsKey,
                           kUiTreatiesKey,
                           kUiOffersKey,
                           kUiFavorsKey,
                           kUiProposeKey,
                           kUiAcceptKey,
                           kUiRefuseKey,
                           kUiCounterKey,
                           kUiWithdrawKey,
                           kUiBreakKey,
                           kUiGiftKey,
                           kUiAskFavorKey})
        check_key(key);
}
