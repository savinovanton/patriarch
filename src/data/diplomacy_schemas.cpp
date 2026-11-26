#include "pat/data/diplomacy_schemas.hpp"

#include <cstdint>
#include <initializer_list>
#include <limits>
#include <string>
#include <utility>

namespace pat::data {
namespace {

// Semantic ranges belong to sim::diplomacy::validate; the schema guarantees the representation.
Type number() {
    return Type::integer(std::numeric_limits<std::int32_t>::min(), std::numeric_limits<std::int32_t>::max());
}

ObjectSchema numbers(std::initializer_list<std::string_view> keys) {
    ObjectSchema block;
    for (const auto key : keys)
        block.required(std::string(key), number());
    return block;
}

Type numeric_block(std::initializer_list<std::string_view> keys) {
    return Type::block(numbers(keys));
}

}  // namespace

void add_diplomacy_schemas(SchemaSet& set) {
    ObjectSchema balance{std::string(kDiplomacyRulesKind)};
    balance.intern_last().describe_kind(
        "Правила дипломатии M1; отдельная загрузка до E15-T17. Числа предварительные.");
    balance.required("opinion",
                     numeric_block({"kin_weight_pm", "head_weight_pm", "feud_enter", "feud_exit"}));
    balance.required("trust",
                     numeric_block({"default", "treaty_ceiling", "public_breach", "denounced", "honoured"}));
    balance.required("limits",
                     numeric_block({"relations_per_house",
                                    "mods_per_relation",
                                    "treaties_per_house",
                                    "offers_per_pair",
                                    "player_inbox_cap",
                                    "favors_owed_max"}));
    auto offers = numbers({"answer_days",
                           "refusal_cooldown_days",
                           "break_cooldown_days",
                           "ai_offer_cooldown_days",
                           "counter_margin"});
    offers.required("ai_may_offer_to_player", Type::boolean());
    balance.required("offers", Type::block(std::move(offers)));
    balance.required("accept",
                     numeric_block({"opinion_pm",
                                    "trust_pm",
                                    "emperor_pm",
                                    "grudge_pm",
                                    "threshold",
                                    "favor_call",
                                    "value_cap",
                                    "forecast_margin"}));
    balance.required(
        "gift",
        numeric_block({"min_rub", "max_rub", "opinion_per_100rub", "cap", "months", "cooldown_days"}));
    balance.required("favor",
                     numeric_block({"min_rub",
                                    "max_rub",
                                    "granted_opinion",
                                    "ingratitude",
                                    "expiry_months",
                                    "granted_months",
                                    "ingratitude_months"}));
    balance.required("pledge", numeric_block({"min_rub", "max_rub"}));
    balance.required("archetype_threshold", numeric_block({"schemer", "loyalist_with_loyalist"}));
    balance.required("ai", numeric_block({"min_treaty_age_days", "propose_wu", "gift_wu", "favor_wu"}));

    ObjectSchema reasons;
    for (const auto* reason : {"offer_refused", "denounced", "false_denunciation", "honoured"})
        reasons.required(reason, numeric_block({"value", "months"}));
    balance.required("reasons", Type::block(std::move(reasons)));

    ObjectSchema hints;
    for (const auto* trait : {"cautious", "proud", "ambitious", "generous", "diplomat", "greedy"})
        hints.required(trait, numeric_block({"dipl_threshold", "dipl_greed", "dipl_arrogance"}));
    balance.required("trait_hints", Type::block(std::move(hints)))
        .describe("Подсказки ai_hint для дипломатии по идентификаторам черт E7.");
    set.add(std::move(balance));

    ObjectSchema treaty{std::string(kTreatyKind)};
    treaty.intern_last().describe_kind(
        "Четыре публичных договора M1. Коды 1–4 заданы в diplomacy-model.md §3.");
    treaty.required("id", Type::integer(1, 4));
    treaty.required("min_months", number());
    treaty.required("months", number());
    treaty.required("max_months", number());
    treaty.required("opinion_bonus", number());
    treaty.required("base_value", number());
    treaty.required("risk", number());
    treaty.required("political_gain", number());
    treaty.required("breach_trust", number());
    treaty.required("breach_grudge", number());
    treaty.required("breach_months", number());
    treaty.required("enabled", Type::boolean());
    treaty.required("system_only", Type::boolean());
    treaty.required("breakable", Type::boolean());
    treaty.required("ai_may_offer_to_player", Type::boolean())
        .describe("Разрешено только для союза и залога мира.");
    set.add(std::move(treaty));
}

}  // namespace pat::data
