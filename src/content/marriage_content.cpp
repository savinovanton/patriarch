#include "pat/content/marriage_content.hpp"

#include "pat/data/content_schemas.hpp"
#include "pat/data/marriage_schemas.hpp"

#include <algorithm>
#include <charconv>
#include <format>
#include <tuple>
#include <utility>

#include "typed_accessors.hpp"

namespace pat::content {
namespace {

// Validation supplies a field path, so locating an error never repeats a simulation rule.
data::SourcePos
field_position(const data::TypedBlock& body, std::string_view path, data::SourcePos fallback) {
    const auto dot = path.find('.');
    const auto* block = detail::block_of(body, path.substr(0, dot));
    if (block == nullptr || dot == std::string_view::npos) {
        return fallback;
    }
    path.remove_prefix(dot + 1);
    const auto bracket = path.find('[');
    const auto* value = block->find(path.substr(0, bracket));
    if (value == nullptr) {
        return fallback;
    }
    if (bracket != std::string_view::npos) {
        const auto* list = value->get_if<data::TypedList>();
        std::size_t index = 0;
        const auto number = path.substr(bracket + 1);
        const auto parsed = std::from_chars(number.data(), number.data() + number.size(), index);
        if (list != nullptr && parsed.ec == std::errc{} && index < list->size()) {
            return (*list)[index].pos;
        }
    }
    return value->pos;
}

sim::MarriageRules read_rules(const data::TypedBlock& body) {
    sim::MarriageRules rules{};
    if (const auto* block = detail::block_of(body, "ranks")) {
        if (const auto* values = detail::list_of(*block, "min_honor")) {
            for (std::size_t i = 0; i < std::min(values->size(), rules.ranks.min_honor.size()); ++i) {
                if (const auto* value = (*values)[i].get_if<std::int64_t>()) {
                    rules.ranks.min_honor[i] = static_cast<std::int32_t>(*value);
                }
            }
        }
    }
    if (const auto* block = detail::block_of(body, "score")) {
        rules.score.honor_weight_permille = detail::i32_of(*block, "honor_weight_permille");
        rules.score.honor_gap_limit = detail::i32_of(*block, "honor_gap_limit");
        rules.score.rank_step_penalty = detail::i32_of(*block, "rank_step_penalty");
        rules.score.dowry_points_per_100rub = detail::i32_of(*block, "dowry_points_per_100rub");
        rules.score.dowry_points_cap = detail::i32_of(*block, "dowry_points_cap");
        rules.score.relation_weight_permille = detail::i32_of(*block, "relation_weight_permille");
        rules.score.opinion_weight_permille = detail::i32_of(*block, "opinion_weight_permille");
        rules.score.health_weight_permille = detail::i32_of(*block, "health_weight_permille");
        rules.score.health_neutral = detail::i32_of(*block, "health_neutral");
        rules.score.fertility_weight_permille = detail::i32_of(*block, "fertility_weight_permille");
        rules.score.fertility_neutral = detail::i32_of(*block, "fertility_neutral");
        rules.score.skills_weight_permille = detail::i32_of(*block, "skills_weight_permille");
        rules.score.skills_neutral = detail::i32_of(*block, "skills_neutral");
        rules.score.age_gap_penalty = detail::i32_of(*block, "age_gap_penalty");
        rules.score.age_gap_free_years = detail::i32_of(*block, "age_gap_free_years");
        rules.score.age_gap_years_cap = detail::i32_of(*block, "age_gap_years_cap");
        rules.score.political_gain_cap = detail::i32_of(*block, "political_gain_cap");
        rules.score.faith_penalty_cap = detail::i32_of(*block, "faith_penalty_cap");
        rules.score.accept_threshold = detail::i32_of(*block, "accept_threshold");
        rules.score.forecast_margin = detail::i32_of(*block, "forecast_margin");
    }
    if (const auto* block = detail::block_of(body, "money")) {
        rules.money.dowry_base_kop = detail::int_of(*block, "dowry_base_kop");
        rules.money.dowry_per_honor_kop = detail::int_of(*block, "dowry_per_honor_kop");
        rules.money.dowry_min_kop = detail::int_of(*block, "dowry_min_kop");
        rules.money.dowry_max_kop = detail::int_of(*block, "dowry_max_kop");
        rules.money.envoy_road_kop = detail::int_of(*block, "envoy_road_kop");
        rules.money.envoy_gifts_kop = detail::int_of(*block, "envoy_gifts_kop");
        rules.money.dispensation_fee_kop = detail::int_of(*block, "dispensation_fee_kop");
    }
    if (const auto* block = detail::block_of(body, "eligibility")) {
        rules.eligibility.min_marriage_age_male = detail::i32_of(*block, "min_marriage_age_male");
        rules.eligibility.min_marriage_age_female = detail::i32_of(*block, "min_marriage_age_female");
        rules.eligibility.min_betrothal_age = detail::i32_of(*block, "min_betrothal_age");
        rules.eligibility.blood_forbidden_degree = detail::i32_of(*block, "blood_forbidden_degree");
        rules.eligibility.blood_absolute_degree = detail::i32_of(*block, "blood_absolute_degree");
        rules.eligibility.affinity_forbidden_degree = detail::i32_of(*block, "affinity_forbidden_degree");
        rules.eligibility.mourning_days_female = detail::i32_of(*block, "mourning_days_female");
        rules.eligibility.mourning_days_male = detail::i32_of(*block, "mourning_days_male");
        if (const auto* value = detail::value_of<bool>(*block, "require_opposite_sex")) {
            rules.eligibility.require_opposite_sex = *value;
        }
        if (const auto* value = detail::value_of<bool>(*block, "forbid_same_house")) {
            rules.eligibility.forbid_same_house = *value;
        }
    }
    if (const auto* block = detail::block_of(body, "relation")) {
        rules.relation.marriage_tie_bonus = detail::i32_of(*block, "marriage_tie_bonus");
        rules.relation.max_marriage_ties = detail::i32_of(*block, "max_marriage_ties");
    }
    if (const auto* block = detail::block_of(body, "negotiation")) {
        rules.negotiation.answer_days = detail::i32_of(*block, "answer_days");
        rules.negotiation.max_parallel_negotiations = detail::i32_of(*block, "max_parallel_negotiations");
        rules.negotiation.refusal_cooldown_days = detail::i32_of(*block, "refusal_cooldown_days");
        rules.negotiation.withdraw_refund_permille = detail::i32_of(*block, "withdraw_refund_permille");
    }
    if (const auto* block = detail::block_of(body, "consequences")) {
        rules.consequences.dispensation_honor = detail::i32_of(*block, "dispensation_honor");
        rules.consequences.marriage_accepted_opinion = detail::i32_of(*block, "marriage_accepted_opinion");
        rules.consequences.marriage_refused_opinion = detail::i32_of(*block, "marriage_refused_opinion");
        rules.consequences.betrothal_broken_honor = detail::i32_of(*block, "betrothal_broken_honor");
        rules.consequences.betrothal_broken_opinion = detail::i32_of(*block, "betrothal_broken_opinion");
        rules.consequences.tonsure_honor = detail::i32_of(*block, "tonsure_honor");
        rules.consequences.tonsure_unity = detail::i32_of(*block, "tonsure_unity");
        rules.consequences.tonsure_opinion = detail::i32_of(*block, "tonsure_opinion");
        rules.consequences.tonsure_refund_permille = detail::i32_of(*block, "tonsure_refund_permille");
        rules.consequences.third_marriage_honor = detail::i32_of(*block, "third_marriage_honor");
        rules.consequences.wedding_rank_honor = detail::i32_of(*block, "wedding_rank_honor");
        rules.consequences.married_into_opinion = detail::i32_of(*block, "married_into_opinion");
        rules.consequences.widow_claim_permille = detail::i32_of(*block, "widow_claim_permille");
        if (const auto* value = detail::value_of<bool>(*block, "bride_joins_groom_house")) {
            rules.consequences.bride_joins_groom_house = *value;
        }
    }
    if (const auto* block = detail::block_of(body, "matchmaker")) {
        rules.matchmaker.matchmaker_min_value = detail::i32_of(*block, "matchmaker_min_value");
        if (const auto* value = detail::value_of<bool>(*block, "ai_may_propose_to_player")) {
            rules.matchmaker.ai_may_propose_to_player = *value;
        }
    }
    return rules;
}

}  // namespace

MarriageRulesResult build_marriage_rules(const data::ContentRegistry& registry) {
    std::vector<data::LoadError> errors;
    const auto* table = registry.table(data::kMarriageRulesKind);
    if (table == nullptr || table->empty()) {
        errors.push_back({0,
                          registry.root_count() > 0 ? registry.root_name(0) : std::string(),
                          {},
                          {{0, 0}, data::ErrorCode::ContentRule, "no marriage_rules object found"}});
        return MarriageRulesResult::err(std::move(errors));
    }
    const auto& object = table->objects().front();
    const auto fail = [&](const data::ContentObject& source, data::SourcePos pos, std::string message) {
        errors.push_back({source.root,
                          registry.root_name(source.root),
                          source.file,
                          {pos, data::ErrorCode::ContentRule, std::move(message)}});
    };
    for (std::size_t i = 1; i < table->size(); ++i) {
        const auto& extra = table->objects()[i];
        fail(extra,
             extra.object.pos,
             std::format("only one marriage_rules object is allowed; '{}' is defined in {}:{}:{}",
                         object.object.id,
                         object.file,
                         object.object.pos.line,
                         object.object.pos.column));
    }
    const auto& body = object.object.body;
    const auto* ranks = detail::block_of(body, "ranks");
    const auto* thresholds = ranks != nullptr ? detail::list_of(*ranks, "min_honor") : nullptr;
    if (thresholds == nullptr || thresholds->size() != sim::kMarriageRankCount) {
        fail(object,
             field_position(body, "ranks.min_honor", object.object.pos),
             std::format("ranks.min_honor must contain exactly {} thresholds", sim::kMarriageRankCount));
    }
    sim::MarriageRules rules = read_rules(body);
    if (const auto checked = sim::validate(rules); !checked) {
        fail(object,
             field_position(body, checked.error().field, object.object.pos),
             std::format("marriage rules are inconsistent: {}", checked.error().message));
    }
    if (!errors.empty()) {
        std::stable_sort(errors.begin(), errors.end(), [](const auto& a, const auto& b) {
            return std::tie(a.root, a.file, a.diagnostic.pos, a.diagnostic.code, a.diagnostic.message) <
                   std::tie(b.root, b.file, b.diagnostic.pos, b.diagnostic.code, b.diagnostic.message);
        });
        return MarriageRulesResult::err(std::move(errors));
    }
    return MarriageRulesResult::ok(std::move(rules));
}

MarriageFaith build_marriage_faith(const data::ContentRegistry& registry) {
    MarriageFaith faith;
    if (const auto* table = registry.table("dynasty")) {
        for (const auto& object : table->objects()) {
            const auto* value = detail::value_of<std::string>(object.object.body, "faith");
            faith.emplace_back(object.object.id, value != nullptr && !value->empty() ? *value : "orthodox");
        }
    }
    return faith;
}

std::vector<std::string_view> bind_marriage_faith(const MarriageFaith& faith,
                                                  std::span<const std::string_view> dynasty_keys_by_id) {
    std::vector<std::string_view> result(dynasty_keys_by_id.size(), "orthodox");
    for (std::size_t i = 0; i < dynasty_keys_by_id.size(); ++i) {
        const auto key = dynasty_keys_by_id[i];
        const auto found =
            std::lower_bound(faith.begin(), faith.end(), key, [](const auto& entry, std::string_view value) {
                return entry.first < value;
            });
        if (found != faith.end() && found->first == key && !found->second.empty()) {
            result[i] = found->second;
        }
    }
    return result;
}

MarriageContentResult load_marriage_content(std::span<const data::ContentSource* const> roots) {
    auto loaded = data::load_content(roots, data::content_schemas());
    if (!loaded.ok()) {
        return MarriageContentResult::err(std::move(loaded.errors));
    }
    auto rules = build_marriage_rules(loaded.registry);
    if (!rules) {
        return MarriageContentResult::err(std::move(rules).error());
    }
    return MarriageContentResult::ok(
        {std::move(rules).value(), build_marriage_faith(loaded.registry), loaded.registry.content_hash()});
}

MarriageContentResult load_marriage_content(const data::ContentSource& root) {
    const data::ContentSource* roots[] = {&root};
    return load_marriage_content(roots);
}

}  // namespace pat::content
