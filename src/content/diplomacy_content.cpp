#include "pat/content/diplomacy_content.hpp"

#include "pat/data/content_schemas.hpp"
#include "pat/data/diplomacy_schemas.hpp"

#include <algorithm>
#include <format>
#include <utility>

#include "typed_accessors.hpp"

namespace pat::content {
namespace {

using namespace sim::diplomacy;

void read_balance(const data::ContentRegistry& registry,
                  const data::ContentObject& object,
                  DiplomacyRules& rules,
                  ErrorSink& errors) {
    if (object.object.id != "main")
        errors.fail(object, object.object.pos, "diplomacy_balance must have the ID 'main'");

    // The required blocks and fields have already passed schema validation.
    const auto& body = object.object.body;
#define PAT_DIP_READ(group, name, def, lo, hi)                                                               \
    rules.group##_##name = detail::i32_of(*detail::block_of(body, #group), #name);
    PAT_DIPLOMACY_BALANCE_FIELDS(PAT_DIP_READ)
#undef PAT_DIP_READ
    rules.ai_may_offer_to_player =
        *detail::value_of<bool>(*detail::block_of(body, "offers"), "ai_may_offer_to_player");

    const auto& reasons = *detail::block_of(body, "reasons");
    for (std::size_t i = 0; i < kReasonNames.size(); ++i) {
        const auto& reason = *detail::block_of(reasons, kReasonNames[i]);
        rules.reasons[i] = {detail::i32_of(reason, "value"), detail::i32_of(reason, "months")};
    }

    const auto& hints = *detail::block_of(body, "trait_hints");
    for (std::size_t i = 0; i < kTraitHintNames.size(); ++i) {
        const auto trait = kTraitHintNames[i];
        if (!registry.find("trait", trait))
            errors.fail(object,
                        detail::key_pos(hints, trait, object.object.pos),
                        std::format("unknown diplomacy trait '{}'", trait));
        const auto& hint = *detail::block_of(hints, trait);
        rules.trait_hints[i] = {detail::i32_of(hint, "dipl_threshold"),
                                detail::i32_of(hint, "dipl_greed"),
                                detail::i32_of(hint, "dipl_arrogance")};
    }
}

using TreatyObjects = std::array<const data::ContentObject*, kTreatyNames.size()>;

TreatyObjects read_treaties(const data::ContentRegistry& registry, DiplomacyRules& rules, ErrorSink& errors) {
    TreatyObjects treaties{};
    if (const auto* table = registry.table(data::kTreatyKind)) {
        for (const auto& treaty : table->objects()) {
            const auto it = std::ranges::find(kTreatyNames, treaty.object.id);
            if (it == kTreatyNames.end()) {
                errors.fail(treaty, treaty.object.pos, "unknown M1 treaty kind");
                continue;
            }
            const auto index = static_cast<std::size_t>(it - kTreatyNames.begin());
            treaties[index] = &treaty;
            if (detail::i32_of(treaty, "id") != static_cast<std::int32_t>(index + 1))
                errors.fail(
                    treaty, detail::field_pos(treaty, "id"), "treaty ID does not match its stable kind");

            auto& rule = rules.treaties[index];
#define PAT_DIP_READ(name, def, lo, hi) rule.name = detail::i32_of(treaty, #name);
            PAT_DIPLOMACY_TREATY_FIELDS(PAT_DIP_READ)
#undef PAT_DIP_READ
            const auto& body = treaty.object.body;
            rule.enabled = *detail::value_of<bool>(body, "enabled");
            rule.system_only = *detail::value_of<bool>(body, "system_only");
            rule.breakable = *detail::value_of<bool>(body, "breakable");
            rule.ai_may_offer_to_player = *detail::value_of<bool>(body, "ai_may_offer_to_player");
        }
    }

    for (std::size_t i = 0; i < treaties.size(); ++i) {
        if (!treaties[i])
            errors.fail_general(std::format("missing treaty kind '{}'", kTreatyNames[i]));
    }
    return treaties;
}

}  // namespace

DiplomacyRulesResult build_diplomacy_rules(const data::ContentRegistry& registry) {
    ErrorSink errors(registry);
    DiplomacyRules rules;
    const auto* balance = errors.single_object(data::kDiplomacyRulesKind);
    if (balance)
        read_balance(registry, *balance, rules, errors);

    const auto treaties = read_treaties(registry, rules, errors);
    if (!errors.empty())
        return DiplomacyRulesResult::err(errors.take());

    if (const auto checked = validate(rules); !checked) {
        auto path = std::string_view(checked.error().field);
        const auto* owner = balance;
        constexpr std::string_view treaty_prefix = "treaty.";
        if (path.starts_with(treaty_prefix)) {
            path.remove_prefix(treaty_prefix.size());
            const auto dot = path.find('.');
            const auto it = std::ranges::find(kTreatyNames, path.substr(0, dot));
            owner = treaties[static_cast<std::size_t>(it - kTreatyNames.begin())];
            path.remove_prefix(dot + 1);
        }
        errors.fail(*owner, detail::path_position(*owner, path), checked.error().message);
        return DiplomacyRulesResult::err(errors.take());
    }
    return DiplomacyRulesResult::ok(std::move(rules));
}

DiplomacyContentResult load_diplomacy_content(std::span<const data::ContentSource* const> roots) {
    auto loaded = data::load_content(roots, data::content_schemas());
    if (!loaded.ok())
        return DiplomacyContentResult::err(std::move(loaded.errors));

    auto rules = build_diplomacy_rules(loaded.registry);
    if (!rules)
        return DiplomacyContentResult::err(std::move(rules).error());

    const auto hash = rules.value().hash();
    return DiplomacyContentResult::ok({std::move(rules).value(), hash});
}

DiplomacyContentResult load_diplomacy_content(const data::ContentSource& root) {
    const data::ContentSource* roots[] = {&root};
    return load_diplomacy_content(roots);
}

}  // namespace pat::content
