#include "pat/sim/diplomacy/rules.hpp"

#include "pat/core/hash_stream.hpp"
#include "pat/sim/rules_check.hpp"

#include <format>
namespace pat::sim::diplomacy {
DiplomacyRules DiplomacyRules::defaults() {
    DiplomacyRules r;
    r.treaties = {{{24, 60, 120, 25, 8, 0, 15, -40, -40, 48, true, false, true, true},
                   {12, 36, 60, 10, 5, 0, 0, -30, -25, 36, true, false, true, true},
                   {0, 0, 0, 10, 0, 0, 5, -25, -30, 36, true, true, false, false},
                   {12, 36, 60, 15, 0, 10, 0, -40, -40, 48, false, false, true, false}}};
    r.reasons = {{{-5, 12}, {-15, 36}, {-10, 24}, {5, 24}}};
    r.trait_hints = {{{10, 0, 0}, {5, 0, 10}, {-5, 0, 0}, {-5, 0, 0}, {-5, 0, 0}, {0, 500, 0}}};
    return r;
}
std::uint64_t DiplomacyRules::hash() const noexcept {
    core::HashStream h;
#define PAT_DIP_HASH(group, name, def, lo, hi) h.i32(group##_##name);
    PAT_DIPLOMACY_BALANCE_FIELDS(PAT_DIP_HASH)
#undef PAT_DIP_HASH
    h.boolean(ai_may_offer_to_player);
    for (const auto& t : treaties) {
#define PAT_DIP_HASH(name, def, lo, hi) h.i32(t.name);
        PAT_DIPLOMACY_TREATY_FIELDS(PAT_DIP_HASH)
#undef PAT_DIP_HASH
        h.boolean(t.enabled);
        h.boolean(t.system_only);
        h.boolean(t.breakable);
        h.boolean(t.ai_may_offer_to_player);
    }
    for (const auto& reason : reasons) {
        h.i32(reason.value);
        h.i32(reason.months);
    }
    for (const auto& hint : trait_hints) {
        h.i32(hint.dipl_threshold);
        h.i32(hint.dipl_greed);
        h.i32(hint.dipl_arrogance);
    }
    return h.value();
}
core::Result<void, DiplomacyRulesError> validate(const DiplomacyRules& r) {
    using E = DiplomacyRulesError;
    using namespace rules_check;
#define PAT_DIP_CHECK(group, name, def, lo, hi)                                                              \
    PAT_RULES_CHECK(in_range<E>(#group "." #name, r.group##_##name, lo, hi));
    PAT_DIPLOMACY_BALANCE_FIELDS(PAT_DIP_CHECK)
#undef PAT_DIP_CHECK
    if (r.opinion_kin_weight_pm + r.opinion_head_weight_pm != 1000)
        return fail<E>("opinion weights must total 1000", "opinion.head_weight_pm");
    if (r.opinion_feud_enter >= r.opinion_feud_exit)
        return fail<E>("feud exit must exceed entry", "opinion.feud_exit");
    if (r.trust_default > r.trust_treaty_ceiling)
        return fail<E>("treaty ceiling must reach default trust", "trust.treaty_ceiling");
    if (r.gift_min_rub > r.gift_max_rub)
        return fail<E>("minimum exceeds maximum", "gift.max_rub");
    if (r.favor_min_rub > r.favor_max_rub)
        return fail<E>("minimum exceeds maximum", "favor.max_rub");
    if (r.pledge_min_rub > r.pledge_max_rub)
        return fail<E>("minimum exceeds maximum", "pledge.max_rub");
    for (std::size_t i = 0; i < r.treaties.size(); ++i) {
        const auto& t = r.treaties[i];
        const auto prefix = std::format("treaty.{}.", kTreatyNames[i]);
#define PAT_DIP_CHECK(name, def, lo, hi) PAT_RULES_CHECK(in_range<E>(prefix + #name, t.name, lo, hi));
        PAT_DIPLOMACY_TREATY_FIELDS(PAT_DIP_CHECK)
#undef PAT_DIP_CHECK
        const auto kind = kTreatyNames[i];
        const bool marriage_pact = kind == "marriage_pact";
        if (t.system_only != marriage_pact)
            return fail<E>("only the marriage pact is system-only", prefix + "system_only");
        if (t.ai_may_offer_to_player && kind != "alliance" && kind != "pledge")
            return fail<E>("only alliances and pledges can be offered by AI to the player",
                           prefix + "ai_may_offer_to_player");
        if (marriage_pact) {
            if (t.breakable)
                return fail<E>("marriage pact must be unbreakable", prefix + "breakable");
            PAT_RULES_CHECK(in_range<E>(prefix + "min_months", t.min_months, 0, 0));
            PAT_RULES_CHECK(in_range<E>(prefix + "months", t.months, 0, 0));
            PAT_RULES_CHECK(in_range<E>(prefix + "max_months", t.max_months, 0, 0));
        } else {
            PAT_RULES_CHECK(in_range<E>(prefix + "min_months", t.min_months, 1, 120));
            PAT_RULES_CHECK(in_range<E>(prefix + "months", t.months, t.min_months, t.max_months));
        }
    }
    for (std::size_t i = 0; i < r.reasons.size(); ++i) {
        const auto prefix = std::format("reasons.{}.", kReasonNames[i]);
        PAT_RULES_CHECK(
            in_range<E>(prefix + "value", r.reasons[i].value, i == 3 ? 0 : -100, i == 3 ? 100 : 0));
        PAT_RULES_CHECK(in_range<E>(prefix + "months", r.reasons[i].months, i == 1 || i == 2 ? 24 : 6, 60));
    }
    for (std::size_t i = 0; i < r.trait_hints.size(); ++i) {
        const auto prefix = std::format("trait_hints.{}.", kTraitHintNames[i]);
        const auto& hint = r.trait_hints[i];
        PAT_RULES_CHECK(in_range<E>(prefix + "dipl_threshold", hint.dipl_threshold, -100, 100));
        PAT_RULES_CHECK(in_range<E>(prefix + "dipl_greed", hint.dipl_greed, 0, 1000));
        PAT_RULES_CHECK(in_range<E>(prefix + "dipl_arrogance", hint.dipl_arrogance, -100, 100));
    }
    return core::Result<void, E>::ok();
}
}  // namespace pat::sim::diplomacy
