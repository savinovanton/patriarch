#include "pat/content/crisis_content.hpp"

#include "pat/data/content_schemas.hpp"
#include "pat/data/crisis_schemas.hpp"
#include "pat/data/schema.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "typed_accessors.hpp"

namespace pat::content {

namespace {

using data::ContentObject;
using data::TypedBlock;

class Builder {
public:
    explicit Builder(const data::ContentRegistry& registry) : registry_(registry), errors_(registry) {}

    CrisisRulesResult build() {
        load_balance();
        load_claims();
        load_projects();
        if (errors_.empty()) {
            if (const auto checked = sim::crisis::validate(rules_); !checked) {
                report_invalid(checked.error());
            }
        }
        if (!errors_.empty()) {
            return CrisisRulesResult::err(errors_.take());
        }
        return CrisisRulesResult::ok(std::move(rules_));
    }

private:
    const data::ContentRegistry& registry_;
    ErrorSink errors_;
    sim::crisis::CrisisRules rules_;

    /// The numbers of a list field in file order; empty when the field is absent.
    static std::vector<std::int64_t> numbers_of(const TypedBlock& block, std::string_view name) {
        std::vector<std::int64_t> out;
        if (const auto* list = detail::list_of(block, name)) {
            for (const data::TypedValue& item : *list) {
                if (const auto* number = item.get_if<std::int64_t>()) {
                    out.push_back(*number);
                }
            }
        }
        return out;
    }

    void load_balance() {
        const ContentObject* object = errors_.single_object(data::kCrisisBalanceKind);
        if (object == nullptr) {
            return;
        }
        const TypedBlock& body = object->object.body;
        sim::crisis::CrisisBalance& balance = rules_.balance;
        // One list builds the struct, the defaults, the checks and this reader (crisis_rules.hpp).
#define PAT_CRISIS_FIELD_READ(group, type, name, def, lo, hi)                                                \
    if (const auto* block_##group##_##name = detail::block_of(body, #group)) {                               \
        balance.group##_##name = static_cast<type>(detail::int_of(*block_##group##_##name, #name));          \
    }
        PAT_CRISIS_BALANCE_FIELDS(PAT_CRISIS_FIELD_READ)
#undef PAT_CRISIS_FIELD_READ
        detail::read_i32_array(errors_, *object, body, "sigmoid", balance.sigmoid);
    }

    void load_claims() {
        const ContentObject* object = errors_.single_object(data::kClaimRulesKind);
        if (object == nullptr) {
            return;
        }
        const TypedBlock& body = object->object.body;
        sim::crisis::ClaimRules& claims = rules_.claims;
#define PAT_CLAIM_FIELD_READ(group, type, name, def, lo, hi)                                                 \
    if (const auto* block_##group##_##name = detail::block_of(body, #group)) {                               \
        claims.group##_##name = static_cast<type>(detail::int_of(*block_##group##_##name, #name));           \
    }
        PAT_CLAIM_RULE_FIELDS(PAT_CLAIM_FIELD_READ)
#undef PAT_CLAIM_FIELD_READ
        detail::read_i32_array(errors_, *object, body, "blood", claims.blood);
        detail::read_i32_array(errors_, *object, body, "legal", claims.legal);
    }

    void load_projects() {
        const auto* table = registry_.table(data::kEmperorProjectKind);
        if (table == nullptr || table->empty()) {
            errors_.fail_general(std::format("no {} object found (expected: {} \"id\" {{ ... }})",
                                             data::kEmperorProjectKind,
                                             data::kEmperorProjectKind));
            return;
        }
        std::vector<const ContentObject*> sorted;
        for (const ContentObject& object : table->objects()) {
            sorted.push_back(&object);
        }
        std::stable_sort(sorted.begin(), sorted.end(), [](const ContentObject* a, const ContentObject* b) {
            return detail::i32_of(*a, "slot") < detail::i32_of(*b, "slot");
        });
        const ContentObject* previous = nullptr;
        for (const ContentObject* object : sorted) {
            // A repeated slot is reported on the later object and the object is dropped.
            if (previous != nullptr && detail::i32_of(*previous, "slot") == detail::i32_of(*object, "slot")) {
                errors_.fail(*object,
                             detail::field_pos(*object, "slot"),
                             std::format("slot {} is already used by {} '{}'",
                                         detail::i32_of(*object, "slot"),
                                         data::kEmperorProjectKind,
                                         previous->object.id));
                continue;
            }
            previous = object;
            rules_.projects.push_back(read_project(*object));
        }
    }

    sim::crisis::ProjectDef read_project(const ContentObject& object) {
        const TypedBlock& body = object.object.body;
        sim::crisis::ProjectDef def;
        def.slot = static_cast<std::uint8_t>(detail::i32_of(object, "slot"));
        def.id = object.object.id;
        def.name_key = detail::text_of(object, "name_key");
        def.desc_key = detail::text_of(object, "desc_key");
        def.level_min = detail::i32_of(object, "level_min");
        def.level_max = detail::i32_of(object, "level_max");
        for (const std::int64_t threshold : numbers_of(body, "thresholds")) {
            def.thresholds.push_back(static_cast<std::int32_t>(threshold));
        }
        def.response_pivot = detail::i32_of(object, "response_pivot");
        def.response_k_pm = detail::i32_of(object, "response_k_pm");
        if (const auto* effects = detail::value_of<data::TypedMap>(body, "effects")) {
            for (const data::TypedField& entry : effects->entries) {
                sim::crisis::ProjectEffect effect;
                effect.name = entry.name;
                if (const auto* list = entry.value.get_if<data::TypedList>()) {
                    for (const data::TypedValue& item : *list) {
                        if (const auto* number = item.get_if<std::int64_t>()) {
                            effect.per_level.push_back(*number);
                        }
                    }
                }
                def.effects.push_back(std::move(effect));
            }
        }
        return def;
    }

    /// Reports the first problem of sim::crisis::validate() at the key it names.
    void report_invalid(const sim::crisis::CrisisRulesError& error) {
        const std::string_view field = error.field;
        const auto message = std::format("crisis rules are inconsistent: {}", error.message);
        const auto in_single = [&](std::string_view kind, std::string_view path) {
            const auto* table = registry_.table(kind);
            if (table == nullptr || table->empty()) {
                return false;
            }
            const ContentObject& object = table->objects().front();
            errors_.fail(object, detail::path_position(object, path), message);
            return true;
        };
        constexpr std::string_view kClaims = "claim_rules ";
        constexpr std::string_view kProject = "emperor_project ";
        bool reported = false;
        if (field.starts_with(kClaims)) {
            reported = in_single(data::kClaimRulesKind, field.substr(kClaims.size()));
        } else if (field.starts_with(kProject)) {
            const auto rest = field.substr(kProject.size());
            const auto dot = rest.find('.');
            if (const auto* object = registry_.find(data::kEmperorProjectKind, rest.substr(0, dot))) {
                errors_.fail(*object,
                             dot == std::string_view::npos
                                 ? object->object.pos
                                 : detail::path_position(*object, rest.substr(dot + 1)),
                             message);
                reported = true;
            }
        } else {
            reported = in_single(data::kCrisisBalanceKind, field);
        }
        if (!reported) {
            errors_.fail_general(message);
        }
    }
};

}  // namespace

CrisisRulesResult build_crisis_rules(const data::ContentRegistry& registry) {
    return Builder(registry).build();
}

CrisisContentResult load_crisis_content(std::span<const data::ContentSource* const> roots) {
    auto loaded = data::load_content(roots, data::content_schemas());
    if (!loaded.ok()) {
        return CrisisContentResult::err(std::move(loaded.errors));
    }
    auto rules = build_crisis_rules(loaded.registry);
    if (!rules) {
        return CrisisContentResult::err(std::move(rules).error());
    }
    return CrisisContentResult::ok({std::move(rules).value(), loaded.registry.content_hash()});
}

CrisisContentResult load_crisis_content(const data::ContentSource& root) {
    const data::ContentSource* roots[] = {&root};
    return load_crisis_content(roots);
}

}  // namespace pat::content
