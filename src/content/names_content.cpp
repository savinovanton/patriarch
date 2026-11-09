#include "pat/content/names_content.hpp"

#include "pat/data/content_schemas.hpp"
#include "pat/data/names_schemas.hpp"
#include "pat/data/schema.hpp"

#include <algorithm>
#include <format>
#include <map>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

namespace pat::content {

namespace {

using data::ContentObject;
using data::ContentTable;
using data::ErrorCode;
using data::LoadError;
using data::SourcePos;
using data::TypedBlock;
using data::TypedField;
using data::TypedValue;
using namespace sim;

// ---- typed accessors: schema validation guarantees presence and type; defaults keep us safe ------

std::int32_t i32_of(const ContentObject& object, std::string_view name) {
    const TypedValue* value = object.object.body.find(name);
    if (value != nullptr) {
        if (const auto* i = value->get_if<std::int64_t>()) {
            return static_cast<std::int32_t>(*i);
        }
    }
    return 0;
}

std::string text_of(const ContentObject& object, std::string_view name) {
    const TypedValue* value = object.object.body.find(name);
    if (value != nullptr) {
        if (const auto* s = value->get_if<std::string>()) {
            return *s;
        }
    }
    return {};
}

std::string ref_of(const ContentObject& object, std::string_view name) {
    const TypedValue* value = object.object.body.find(name);
    if (value != nullptr) {
        if (const auto* r = value->get_if<data::RefValue>()) {
            return r->id;
        }
    }
    return {};
}

std::size_t enum_index_of(const ContentObject& object, std::string_view name) {
    const TypedValue* value = object.object.body.find(name);
    if (value != nullptr) {
        if (const auto* e = value->get_if<data::EnumValue>()) {
            return e->index;
        }
    }
    return 0;
}

Sex sex_of(const ContentObject& object) {
    return enum_index_of(object, "sex") == 0 ? Sex::Male : Sex::Female;
}

/// Position of the key of a field (the object itself when the field is absent).
SourcePos field_pos(const ContentObject& object, std::string_view name) {
    for (const TypedField& field : object.object.body.fields) {
        if (field.name == name) {
            return field.pos;
        }
    }
    return object.object.pos;
}

bool has_field(const ContentObject& object, std::string_view name) {
    return object.object.body.find(name) != nullptr;
}

// ---- the builder ---------------------------------------------------------------------------------

class Builder {
public:
    explicit Builder(const data::ContentRegistry& registry) : registry_(registry) {}

    NamePoolsResult build() {
        load_rules();
        load_given_names();
        load_surnames();
        load_portraits();
        if (errors_.empty()) {
            check_assembled_pools();
        }
        if (!errors_.empty()) {
            std::stable_sort(errors_.begin(), errors_.end(), [](const LoadError& a, const LoadError& b) {
                return std::tie(a.root, a.file, a.diagnostic.pos, a.diagnostic.code, a.diagnostic.message) <
                       std::tie(b.root, b.file, b.diagnostic.pos, b.diagnostic.code, b.diagnostic.message);
            });
            return NamePoolsResult::err(std::move(errors_));
        }
        return NamePoolsResult::ok(std::move(pools_));
    }

private:
    const data::ContentRegistry& registry_;
    NamePools pools_;
    std::vector<LoadError> errors_;

    // ---- errors ----

    void fail(const ContentObject& object, SourcePos pos, std::string message) {
        errors_.push_back(LoadError{object.root,
                                    registry_.root_name(object.root),
                                    object.file,
                                    data::Diagnostic{pos, ErrorCode::ContentRule, std::move(message)}});
    }

    /// A problem that belongs to no object (an object is missing, a pool is incomplete).
    void fail_general(std::string message) {
        errors_.push_back(
            LoadError{0,
                      registry_.root_count() > 0 ? registry_.root_name(0) : std::string(),
                      std::string(),
                      data::Diagnostic{SourcePos{0, 0}, ErrorCode::ContentRule, std::move(message)}});
    }

    void require_text(const ContentObject& object, std::string_view field, const std::string& text) {
        if (text.empty() && has_field(object, field)) {
            fail(object, field_pos(object, field), std::format("{} must not be empty", field));
        }
    }

    // ---- loading ----

    void load_rules() {
        const ContentTable* table = registry_.table(data::kNamingRulesKind);
        if (table == nullptr || table->empty()) {
            fail_general(std::format("no {} object found (expected: {} \"main\" {{ ... }})",
                                     data::kNamingRulesKind,
                                     data::kNamingRulesKind));
            return;
        }
        const ContentObject& first = table->objects().front();
        for (std::size_t i = 1; i < table->size(); ++i) {
            const ContentObject& extra = table->objects()[i];
            fail(extra,
                 extra.object.pos,
                 std::format("only one {} object is allowed, '{}' is already defined in {}:{}:{}",
                             data::kNamingRulesKind,
                             first.object.id,
                             first.file,
                             first.object.pos.line,
                             first.object.pos.column));
        }
        pools_.rules.parent_bonus = i32_of(first, "parent_bonus");
        pools_.rules.grandparent_bonus = i32_of(first, "grandparent_bonus");
    }

    void load_given_names() {
        const ContentTable* table = registry_.table(data::kGivenNameKind);
        if (table == nullptr) {
            return;
        }
        for (const ContentObject& object : table->objects()) {
            GivenName name;
            name.id = object.object.id;
            name.sex = sex_of(object);
            name.weight = i32_of(object, "weight");
            name.name_key = text_of(object, "name_key");
            name.patronymic_son_key = text_of(object, "patronymic_son_key");
            name.patronymic_daughter_key = text_of(object, "patronymic_daughter_key");

            require_text(object, "name_key", name.name_key);
            require_text(object, "patronymic_son_key", name.patronymic_son_key);
            require_text(object, "patronymic_daughter_key", name.patronymic_daughter_key);
            if (name.sex == Sex::Male) {
                if (!has_field(object, "patronymic_son_key")) {
                    fail(object,
                         object.object.pos,
                         std::format("man's name '{}' needs patronymic_son_key (the form for a son)",
                                     name.id));
                }
                if (!has_field(object, "patronymic_daughter_key")) {
                    fail(
                        object,
                        object.object.pos,
                        std::format("man's name '{}' needs patronymic_daughter_key (the form for a daughter)",
                                    name.id));
                }
            } else {
                for (const std::string_view field : {"patronymic_son_key", "patronymic_daughter_key"}) {
                    if (has_field(object, field)) {
                        fail(object,
                             field_pos(object, field),
                             std::format("woman's name '{}' must not have {}: only men's names have "
                                         "patronymic forms",
                                         name.id,
                                         field));
                    }
                }
            }
            pools_.given_names.push_back(std::move(name));
        }
    }

    void load_surnames() {
        std::map<std::string, const ContentObject*> by_dynasty;
        if (const ContentTable* table = registry_.table(data::kSurnameKind)) {
            for (const ContentObject& object : table->objects()) {
                SurnameForms surname;
                surname.dynasty_id = ref_of(object, "dynasty");
                surname.male_key = text_of(object, "male_key");
                surname.female_key = text_of(object, "female_key");
                require_text(object, "male_key", surname.male_key);
                require_text(object, "female_key", surname.female_key);

                const auto [it, inserted] = by_dynasty.emplace(surname.dynasty_id, &object);
                if (!inserted) {
                    fail(object,
                         field_pos(object, "dynasty"),
                         std::format("dynasty '{}' already has the surname '{}' (defined in {}:{}:{})",
                                     surname.dynasty_id,
                                     it->second->object.id,
                                     it->second->file,
                                     it->second->object.pos.line,
                                     it->second->object.pos.column));
                }
                pools_.surnames.push_back(std::move(surname));
            }
        }
        // Every house needs the forms of its surname, or its people could not be named.
        if (const ContentTable* dynasties = registry_.table("dynasty")) {
            for (const ContentObject& dynasty : dynasties->objects()) {
                if (!by_dynasty.contains(dynasty.object.id)) {
                    fail(dynasty,
                         dynasty.object.pos,
                         std::format("dynasty '{}' has no surname object (expected: surname \"{}\" {{ "
                                     "dynasty = dynasty:{} ... }})",
                                     dynasty.object.id,
                                     dynasty.object.id,
                                     dynasty.object.id));
                }
            }
        }
    }

    void load_portraits() {
        const ContentTable* table = registry_.table(data::kPortraitKind);
        if (table == nullptr) {
            return;
        }
        std::map<std::int32_t, const ContentObject*> by_id;
        for (const ContentObject& object : table->objects()) {
            PortraitEntry portrait;
            portrait.id = static_cast<std::uint16_t>(i32_of(object, "portrait_id"));
            portrait.sex = sex_of(object);
            portrait.stage = static_cast<LifeStage>(enum_index_of(object, "stage"));

            const auto [it, inserted] = by_id.emplace(portrait.id, &object);
            if (!inserted) {
                fail(object,
                     field_pos(object, "portrait_id"),
                     std::format("portrait id {} is already used by '{}' ({}:{}:{})",
                                 portrait.id,
                                 it->second->object.id,
                                 it->second->file,
                                 it->second->object.pos.line,
                                 it->second->object.pos.column));
            }
            pools_.portraits.push_back(portrait);
        }
        std::stable_sort(pools_.portraits.begin(),
                         pools_.portraits.end(),
                         [](const PortraitEntry& a, const PortraitEntry& b) { return a.id < b.id; });
    }

    // ---- the final check ----

    /// What the per-object checks cannot see: the pool as a whole (a name for each sex, a portrait for
    /// each sex and life stage). sim::validate() decides; the problem belongs to no single object.
    void check_assembled_pools() {
        const auto result = validate(pools_);
        if (!result) {
            fail_general(std::format("name pools are inconsistent: {}", result.error().message));
        }
    }
};

}  // namespace

NamePoolsResult build_name_pools(const data::ContentRegistry& registry) {
    return Builder(registry).build();
}

NamesContentResult load_names_content(std::span<const data::ContentSource* const> roots) {
    const data::SchemaSet schemas = data::content_schemas();
    data::LoadResult loaded = data::load_content(roots, schemas);
    if (!loaded.ok()) {
        return NamesContentResult::err(std::move(loaded.errors));
    }
    NamePoolsResult pools = build_name_pools(loaded.registry);
    if (!pools) {
        return NamesContentResult::err(pools.error());
    }
    return NamesContentResult::ok(NamesContent{std::move(pools).value(), loaded.registry.content_hash()});
}

NamesContentResult load_names_content(const data::ContentSource& root) {
    const data::ContentSource* roots[] = {&root};
    return load_names_content(roots);
}

}  // namespace pat::content
