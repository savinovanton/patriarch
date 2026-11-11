#include "pat/content/m1_content.hpp"

#include "pat/data/m1_schemas.hpp"
#include "pat/sim/world.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <map>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

namespace pat::content {

namespace {

using data::ContentObject;
using data::LoadError;
using data::SourcePos;
using data::TypedField;
using data::TypedValue;

struct SkillSlot {
    std::string_view id;
    sim::Skill slot;
};

constexpr std::array<SkillSlot, sim::kSkillCount> kSkills{{
    {"stewardship", sim::Skill::Stewardship},
    {"diplomacy", sim::Skill::Diplomacy},
    {"martial", sim::Skill::Martial},
    {"intrigue", sim::Skill::Intrigue},
    {"learning", sim::Skill::Learning},
}};

SourcePos field_pos(const ContentObject& object, std::string_view name) {
    for (const TypedField& field : object.object.body.fields) {
        if (field.name == name) {
            return field.pos;
        }
    }
    return object.object.pos;
}

template <class T>
const T* field_value(const ContentObject& object, std::string_view name) {
    const TypedValue* value = object.object.body.find(name);
    return value != nullptr ? value->get_if<T>() : nullptr;
}

class Validator {
public:
    explicit Validator(const data::ContentRegistry& registry) : registry_(registry) {}

    std::vector<LoadError> validate() {
        check_keys();
        check_skills();
        check_traits();
        check_honor_ranks();
        std::stable_sort(errors_.begin(), errors_.end(), [](const LoadError& a, const LoadError& b) {
            return std::tie(a.root, a.file, a.diagnostic.pos, a.diagnostic.code, a.diagnostic.message) <
                   std::tie(b.root, b.file, b.diagnostic.pos, b.diagnostic.code, b.diagnostic.message);
        });
        return std::move(errors_);
    }

private:
    const data::ContentRegistry& registry_;
    std::vector<LoadError> errors_;

    void fail(const ContentObject& object, SourcePos pos, std::string message) {
        errors_.push_back(
            LoadError{object.root,
                      object.root < registry_.root_count() ? registry_.root_name(object.root) : std::string(),
                      object.file,
                      data::Diagnostic{pos, data::ErrorCode::ContentRule, std::move(message)}});
    }

    void check_keys() {
        constexpr std::array<std::string_view, 5> kinds{
            "province", "dynasty", "trait", "skill", data::kHonorRankKind};
        for (const std::string_view kind : kinds) {
            const data::ContentTable* table = registry_.table(kind);
            if (table == nullptr) {
                continue;
            }
            for (const ContentObject& object : table->objects()) {
                for (const std::string_view key : {"name_key", "desc_key"}) {
                    const auto* text = field_value<std::string>(object, key);
                    if (text != nullptr && text->empty()) {
                        fail(object, field_pos(object, key), std::format("{} must not be empty", key));
                    }
                }
            }
        }
    }

    void check_skills() {
        const data::ContentTable* table = registry_.table("skill");
        if (table == nullptr || table->empty()) {
            return;
        }
        const ContentObject& first = table->objects().front();
        for (const SkillSlot& expected : kSkills) {
            const ContentObject* object = table->find(expected.id);
            if (object == nullptr) {
                fail(first,
                     first.object.pos,
                     std::format("missing canonical skill '{}' (expected all five skills)", expected.id));
            }
        }
        for (const ContentObject& object : table->objects()) {
            const auto expected =
                std::find_if(kSkills.begin(), kSkills.end(), [&](const SkillSlot& candidate) {
                    return candidate.id == object.object.id;
                });
            if (expected == kSkills.end()) {
                fail(object, object.object.pos, std::format("unknown skill '{}'", object.object.id));
                continue;
            }
            const auto* slot = field_value<std::int64_t>(object, "slot");
            if (slot == nullptr || *slot != static_cast<std::int64_t>(expected->slot)) {
                fail(object,
                     field_pos(object, "slot"),
                     std::format("skill '{}' must use slot {}",
                                 object.object.id,
                                 static_cast<std::int64_t>(expected->slot)));
            }
        }
    }

    void check_traits() {
        const data::ContentTable* table = registry_.table("trait");
        if (table == nullptr) {
            return;
        }
        std::map<std::int64_t, const ContentObject*> slots;
        for (const ContentObject& object : table->objects()) {
            if (const auto* slot = field_value<std::int64_t>(object, "slot")) {
                const auto [previous, inserted] = slots.emplace(*slot, &object);
                if (!inserted) {
                    const ContentObject& other = *previous->second;
                    const SourcePos other_pos = field_pos(other, "slot");
                    fail(object,
                         field_pos(object, "slot"),
                         std::format("trait slot {} is already used by '{}' in {}:{}:{}",
                                     *slot,
                                     other.object.id,
                                     other.file,
                                     other_pos.line,
                                     other_pos.column));
                }
            }
            if (const auto* skills = field_value<data::TypedMap>(object, "skills")) {
                for (const TypedField& skill : skills->entries) {
                    if (registry_.find("skill", skill.name) == nullptr) {
                        fail(
                            object, skill.pos, std::format("unknown skill '{}' in trait skills", skill.name));
                    }
                }
            }
            const auto* opposite = field_value<data::RefValue>(object, "opposite");
            if (opposite == nullptr) {
                continue;
            }
            if (opposite->id == object.object.id) {
                fail(object, field_pos(object, "opposite"), "a trait cannot be its own opposite");
                continue;
            }
            const ContentObject* other = table->find(opposite->id);
            if (other == nullptr) {
                continue;  // Dangling references are reported by load_content(), before this stage.
            }
            const auto* reverse = field_value<data::RefValue>(*other, "opposite");
            if (reverse == nullptr || reverse->id != object.object.id) {
                fail(object,
                     field_pos(object, "opposite"),
                     std::format("opposite relation '{}' -> '{}' must be reciprocal",
                                 object.object.id,
                                 opposite->id));
            }
        }
    }

    void check_honor_ranks() {
        const data::ContentTable* table = registry_.table(data::kHonorRankKind);
        if (table == nullptr || table->empty()) {
            return;
        }
        std::map<std::int64_t, const ContentObject*> thresholds;
        for (const ContentObject& object : table->objects()) {
            const auto threshold = *field_value<std::int64_t>(object, "min_honor");
            const auto [previous, inserted] = thresholds.emplace(threshold, &object);
            if (!inserted) {
                fail(object,
                     field_pos(object, "min_honor"),
                     std::format("honor rank threshold {} is already used by '{}'",
                                 threshold,
                                 previous->second->object.id));
            }
        }
        const auto& [minimum, object] = *thresholds.begin();
        if (minimum != sim::kMinHonor) {
            fail(*object,
                 field_pos(*object, "min_honor"),
                 std::format("the lowest honor rank threshold must be {}", sim::kMinHonor));
        }
    }
};

}  // namespace

std::vector<data::LoadError> validate_m1_definitions(const data::ContentRegistry& registry) {
    return Validator(registry).validate();
}

std::vector<HonorRank> build_honor_ranks(const data::ContentRegistry& registry) {
    const auto* table = registry.table(data::kHonorRankKind);
    if (table == nullptr) {
        return {};
    }
    std::vector<HonorRank> ranks;
    ranks.reserve(table->size());
    for (const auto& object : table->objects()) {
        ranks.push_back({object.object.id,
                         static_cast<std::int32_t>(*field_value<std::int64_t>(object, "min_honor")),
                         *field_value<std::string>(object, "name_key")});
    }
    std::ranges::sort(ranks, {}, &HonorRank::min_honor);
    return ranks;
}

const HonorRank* honor_rank(std::span<const HonorRank> ranks, std::int32_t honor) {
    const auto next = std::ranges::upper_bound(ranks, honor, {}, &HonorRank::min_honor);
    return next == ranks.begin() ? nullptr : &*(next - 1);
}

}  // namespace pat::content
