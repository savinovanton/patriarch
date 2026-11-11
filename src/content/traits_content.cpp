#include "pat/content/traits_content.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <map>
#include <optional>
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

constexpr std::array<std::string_view, 6> kCategories{
    "personality", "education", "health", "life", "congenital", "religious"};
constexpr std::array<std::string_view, 7> kAttributes{
    "health", "fertility", "stress", "piety", "ambition", "loyalty_head", "loyalty_emperor"};

template <class T>
const T* field_value(const ContentObject& object, std::string_view name) {
    const auto* value = object.object.body.find(name);
    return value != nullptr ? value->get_if<T>() : nullptr;
}

SourcePos field_pos(const ContentObject& object, std::string_view name) {
    for (const auto& field : object.object.body.fields) {
        if (field.name == name) {
            return field.pos;
        }
    }
    return object.object.pos;
}

template <std::size_t N>
std::optional<std::size_t> index_in(const std::array<std::string_view, N>& names, std::string_view key) {
    const auto found = std::ranges::find(names, key);
    return found == names.end() ? std::nullopt
                                : std::optional(static_cast<std::size_t>(found - names.begin()));
}

class Builder {
public:
    explicit Builder(const data::ContentRegistry& registry) : registry_(registry) {}

    TraitCatalogResult build() {
        const auto* table = registry_.table("trait");
        if (table == nullptr || table->empty()) {
            return TraitCatalogResult::ok({});
        }
        catalog_.definitions.reserve(table->size());
        std::map<std::uint16_t, const ContentObject*> slots;
        for (const auto& object : table->objects()) {
            sim::TraitDef def;
            def.id = static_cast<std::uint16_t>(*field_value<std::int64_t>(object, "slot"));
            const auto [previous, inserted] = slots.emplace(def.id, &object);
            if (!inserted) {
                fail(object,
                     field_pos(object, "slot"),
                     std::format(
                         "trait slot {} is already used by '{}'", def.id, previous->second->object.id));
            }
            def.key = object.object.id;
            def.name_key = *field_value<std::string>(object, "name_key");
            def.desc_key = *field_value<std::string>(object, "desc_key");
            const auto& category = field_value<data::EnumValue>(object, "category")->name;
            def.category = static_cast<sim::TraitCategory>(*index_in(kCategories, category));
            if (const auto* opposite = field_value<data::RefValue>(object, "opposite")) {
                if (const auto* other = table->find(opposite->id)) {
                    def.opposite = static_cast<std::uint16_t>(*field_value<std::int64_t>(*other, "slot"));
                    const auto* reverse = field_value<data::RefValue>(*other, "opposite");
                    if (other == &object || reverse == nullptr || reverse->id != object.object.id) {
                        fail(object,
                             field_pos(object, "opposite"),
                             "trait opposites must be distinct and reciprocal");
                    }
                } else {
                    fail(object,
                         field_pos(object, "opposite"),
                         std::format("unknown opposite trait '{}'", opposite->id));
                }
            }
            load_skills(object, def);
            load_attributes(object, def);
            load_multiplier(object, "health_multiplier_permille", def.health_multiplier_permille);
            load_multiplier(object, "fertility_multiplier_permille", def.fertility_multiplier_permille);
            load_multiplier(object, "mortality_multiplier_permille", def.mortality_multiplier_permille);
            if (const auto* chance = field_value<core::Fixed32>(object, "inherit_chance")) {
                def.inherit_chance = *chance;
            }
            if (const auto* hints = field_value<data::TypedMap>(object, "ai_hint")) {
                for (const auto& hint : hints->entries) {
                    def.ai_hint.emplace_back(hint.name,
                                             static_cast<std::int32_t>(*hint.value.get_if<std::int64_t>()));
                }
                std::ranges::sort(def.ai_hint);
            }
            catalog_.definitions.push_back(std::move(def));
        }
        std::ranges::sort(catalog_.definitions, {}, &sim::TraitDef::id);
        if (errors_.empty()) {
            const auto valid = sim::validate(catalog_);
            if (!valid) {
                const auto& first = table->objects().front();
                fail(first, first.object.pos, valid.error().message);
            }
        }
        if (!errors_.empty()) {
            std::ranges::sort(errors_, [](const LoadError& a, const LoadError& b) {
                return std::tie(a.root, a.file, a.diagnostic.pos, a.diagnostic.code, a.diagnostic.message) <
                       std::tie(b.root, b.file, b.diagnostic.pos, b.diagnostic.code, b.diagnostic.message);
            });
            return TraitCatalogResult::err(std::move(errors_));
        }
        return TraitCatalogResult::ok(std::move(catalog_));
    }

private:
    const data::ContentRegistry& registry_;
    sim::TraitCatalog catalog_;
    std::vector<LoadError> errors_;

    void fail(const ContentObject& object, SourcePos pos, std::string message) {
        errors_.push_back(
            {object.root,
             object.root < registry_.root_count() ? registry_.root_name(object.root) : std::string(),
             object.file,
             {pos, data::ErrorCode::ContentRule, std::move(message)}});
    }

    void load_skills(const ContentObject& object, sim::TraitDef& def) {
        const auto* skills = field_value<data::TypedMap>(object, "skills");
        if (skills == nullptr) {
            return;
        }
        for (const TypedField& entry : skills->entries) {
            const auto* skill = registry_.find("skill", entry.name);
            if (skill == nullptr) {
                fail(object, entry.pos, std::format("unknown skill '{}' in trait skills", entry.name));
                continue;
            }
            const auto slot = *field_value<std::int64_t>(*skill, "slot");
            def.skill_modifiers[static_cast<std::size_t>(slot)] =
                core::Fixed32::from_int(static_cast<std::int32_t>(*entry.value.get_if<std::int64_t>()));
        }
    }

    void load_attributes(const ContentObject& object, sim::TraitDef& def) {
        const auto* modifiers = field_value<data::TypedMap>(object, "modifiers");
        if (modifiers == nullptr) {
            return;
        }
        for (const TypedField& entry : modifiers->entries) {
            const auto index = index_in(kAttributes, entry.name);
            if (!index) {
                fail(object, entry.pos, std::format("unknown trait modifier '{}'", entry.name));
                continue;
            }
            def.attribute_modifiers[*index] = *entry.value.get_if<core::Fixed32>();
        }
    }

    static void load_multiplier(const ContentObject& object, std::string_view name, std::int32_t& out) {
        if (const auto* value = field_value<std::int64_t>(object, name)) {
            out = static_cast<std::int32_t>(*value);
        }
    }
};

}  // namespace

TraitCatalogResult build_trait_catalog(const data::ContentRegistry& registry) {
    return Builder(registry).build();
}

}  // namespace pat::content
