#include "pat/content/demography_content.hpp"

#include "pat/data/content_schemas.hpp"
#include "pat/data/demography_schemas.hpp"
#include "pat/data/schema.hpp"

#include <algorithm>
#include <format>
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
using data::TypedList;
using data::TypedValue;
using namespace sim;

// ---- typed accessors: schema validation guarantees presence and type; defaults keep us safe ------

std::int32_t i32_of(const TypedBlock& block, std::string_view name) {
    const TypedValue* value = block.find(name);
    if (value != nullptr) {
        if (const auto* i = value->get_if<std::int64_t>()) {
            return static_cast<std::int32_t>(*i);
        }
    }
    return 0;
}

const TypedBlock* block_of(const TypedBlock& block, std::string_view name) {
    const TypedValue* value = block.find(name);
    return value != nullptr ? value->get_if<TypedBlock>() : nullptr;
}

const TypedList* list_of(const TypedBlock& block, std::string_view name) {
    const TypedValue* value = block.find(name);
    return value != nullptr ? value->get_if<TypedList>() : nullptr;
}

/// Position of the key of a field (`fallback` when the field is missing).
SourcePos key_pos(const TypedBlock& block, std::string_view name, SourcePos fallback) {
    for (const TypedField& field : block.fields) {
        if (field.name == name) {
            return field.pos;
        }
    }
    return fallback;
}

/// One loaded step of an age table, with the position of its `from_age` for error messages.
struct LoadedBucket {
    std::int32_t from_age = 0;
    std::int32_t value = 0;
    SourcePos from_age_pos;
};

/// An age table as written in the file: its steps (all of them, even beyond AgeTable::kMaxBuckets)
/// and where the field is.
struct LoadedTable {
    std::vector<LoadedBucket> buckets;
    SourcePos pos;
};

// ---- the builder ---------------------------------------------------------------------------------

class Builder {
public:
    explicit Builder(const data::ContentRegistry& registry) : registry_(registry) {}

    DemographyRulesResult build() {
        const ContentTable* table = registry_.table(data::kDemographyRulesKind);
        if (table == nullptr || table->empty()) {
            fail_general(std::format("no {} object found (expected: {} \"main\" {{ ... }})",
                                     data::kDemographyRulesKind,
                                     data::kDemographyRulesKind));
        } else {
            object_ = &table->objects().front();
            for (std::size_t i = 1; i < table->size(); ++i) {
                const ContentObject& extra = table->objects()[i];
                fail(extra,
                     extra.object.pos,
                     std::format("only one {} object is allowed, '{}' is already defined in {}:{}:{}",
                                 data::kDemographyRulesKind,
                                 object_->object.id,
                                 object_->file,
                                 object_->object.pos.line,
                                 object_->object.pos.column));
            }
            load(*object_);
        }

        if (errors_.empty()) {
            check_assembled_rules();
        }
        if (!errors_.empty()) {
            sort_errors();
            return DemographyRulesResult::err(std::move(errors_));
        }
        return DemographyRulesResult::ok(std::move(rules_));
    }

private:
    const data::ContentRegistry& registry_;
    const ContentObject* object_ = nullptr;
    DemographyRules rules_{};
    LoadedTable mortality_table_;
    LoadedTable age_factor_table_;
    std::vector<LoadError> errors_;

    // ---- errors ----

    void fail(const ContentObject& object, SourcePos pos, std::string message) {
        errors_.push_back(LoadError{object.root,
                                    registry_.root_name(object.root),
                                    object.file,
                                    data::Diagnostic{pos, ErrorCode::ContentRule, std::move(message)}});
    }

    /// A problem that belongs to no object (the object is missing).
    void fail_general(std::string message) {
        errors_.push_back(
            LoadError{0,
                      registry_.root_count() > 0 ? registry_.root_name(0) : std::string(),
                      std::string(),
                      data::Diagnostic{SourcePos{0, 0}, ErrorCode::ContentRule, std::move(message)}});
    }

    void sort_errors() {
        std::stable_sort(errors_.begin(), errors_.end(), [](const LoadError& a, const LoadError& b) {
            return std::tie(a.root, a.file, a.diagnostic.pos, a.diagnostic.code, a.diagnostic.message) <
                   std::tie(b.root, b.file, b.diagnostic.pos, b.diagnostic.code, b.diagnostic.message);
        });
    }

    // ---- loading ----

    static LoadedTable
    read_table(const TypedBlock& block, std::string_view field, std::string_view value_field) {
        LoadedTable table;
        table.pos = key_pos(block, field, SourcePos{0, 0});
        if (const TypedList* list = list_of(block, field)) {
            for (const TypedValue& item : *list) {
                const TypedBlock* bucket = item.get_if<TypedBlock>();
                if (bucket == nullptr) {
                    continue;
                }
                const TypedValue* from = bucket->find("from_age");
                table.buckets.push_back(LoadedBucket{i32_of(*bucket, "from_age"),
                                                     i32_of(*bucket, value_field),
                                                     from != nullptr ? from->pos : item.pos});
            }
        }
        return table;
    }

    /// Copies what fits into the fixed-size table; `count` keeps the real number of steps so that
    /// validate() rejects a table with too many.
    static void store_table(const LoadedTable& loaded, AgeTable& out) {
        out = AgeTable{};
        const std::size_t kept = std::min(loaded.buckets.size(), AgeTable::kMaxBuckets);
        for (std::size_t i = 0; i < kept; ++i) {
            out.buckets[i] = AgeBucket{loaded.buckets[i].from_age, loaded.buckets[i].value};
        }
        out.count = static_cast<std::int32_t>(loaded.buckets.size());
    }

    void load(const ContentObject& object) {
        const TypedBlock& body = object.object.body;

        if (const TypedBlock* b = block_of(body, "mortality")) {
            mortality_table_ = read_table(*b, "buckets", "permille");
            store_table(mortality_table_, rules_.mortality);
            rules_.noble_child_age_limit = i32_of(*b, "noble_child_age_limit");
            rules_.noble_child_multiplier_permille = i32_of(*b, "noble_child_multiplier_permille");
            rules_.low_health_mortality_permille = i32_of(*b, "low_health_multiplier_permille");
            rules_.high_health_mortality_permille = i32_of(*b, "high_health_multiplier_permille");
        }
        if (const TypedBlock* b = block_of(body, "health")) {
            rules_.low_health_below = i32_of(*b, "low_below");
            rules_.high_health_from = i32_of(*b, "high_from");
        }
        if (const TypedBlock* b = block_of(body, "conception")) {
            rules_.base_conception_permille = i32_of(*b, "base_permille");
            age_factor_table_ = read_table(*b, "age_factor", "factor_permille");
            store_table(age_factor_table_, rules_.age_factor);
            rules_.low_health_fertility_permille = i32_of(*b, "low_health_factor_permille");
            rules_.high_health_fertility_permille = i32_of(*b, "high_health_factor_permille");
            rules_.max_children_per_woman = i32_of(*b, "max_children_per_woman");
            rules_.pregnancy_months = i32_of(*b, "pregnancy_months");
            rules_.crowding_from_living = i32_of(*b, "crowding_from_living");
            rules_.crowding_to_living = i32_of(*b, "crowding_to_living");
            rules_.crowding_min_permille = i32_of(*b, "crowding_min_permille");
            rules_.miscarriage_permille = i32_of(*b, "miscarriage_permille");
        }
        if (const TypedBlock* b = block_of(body, "birth")) {
            rules_.boys_per_100_girls = i32_of(*b, "boys_per_100_girls");
            rules_.childbirth_risk_base_permille = i32_of(*b, "risk_base_permille");
            rules_.childbirth_risk_high_health_delta_permille = i32_of(*b, "risk_high_health_delta_permille");
            rules_.childbirth_risk_low_health_delta_permille = i32_of(*b, "risk_low_health_delta_permille");
            rules_.childbirth_risk_physician_delta_permille = i32_of(*b, "risk_physician_delta_permille");
            rules_.stillbirth_permille = i32_of(*b, "stillbirth_permille");
            rules_.newborn_health = i32_of(*b, "newborn_health");
            rules_.skill_inherit_permille = i32_of(*b, "skill_inherit_permille");
            rules_.skill_spread = i32_of(*b, "skill_spread");
        }
        if (const TypedBlock* b = block_of(body, "health_drift")) {
            rules_.health_drift = {i32_of(*b, "child"),
                                   i32_of(*b, "adolescent"),
                                   i32_of(*b, "prime"),
                                   i32_of(*b, "mature"),
                                   i32_of(*b, "elder")};
            rules_.health_noise = i32_of(*b, "noise");
            rules_.low_health_drift = i32_of(*b, "low_health");
        }
        if (const TypedBlock* b = block_of(body, "stress")) {
            rules_.stress_high_from = i32_of(*b, "high_from");
            rules_.stress_critical_from = i32_of(*b, "critical_from");
            rules_.stress_high_health_drift = i32_of(*b, "high_health_drift");
            rules_.stress_critical_health_drift = i32_of(*b, "critical_health_drift");
            rules_.stress_gain_low_health = i32_of(*b, "gain_low_health");
            rules_.stress_gain_head = i32_of(*b, "gain_head");
            rules_.stress_decay_permille = i32_of(*b, "decay_permille");
        }
        if (const TypedBlock* b = block_of(body, "life_stages")) {
            rules_.adolescent_from = i32_of(*b, "adolescent_from");
            rules_.prime_from = i32_of(*b, "prime_from");
            rules_.mature_from = i32_of(*b, "mature_from");
            rules_.elder_from = i32_of(*b, "elder_from");
            rules_.marriage_age_male = i32_of(*b, "marriage_age_male");
            rules_.marriage_age_female = i32_of(*b, "marriage_age_female");
        }
    }

    // ---- the final check ----

    /// Where an age table breaks the rules of sim::validate(): the offending step, or the table
    /// itself (empty table). validate() has already decided that the table is bad; this only finds the
    /// place, following the same order of checks.
    static SourcePos table_problem_pos(const LoadedTable& table) {
        const auto& buckets = table.buckets;
        if (buckets.size() > AgeTable::kMaxBuckets) {
            return buckets[AgeTable::kMaxBuckets].from_age_pos;
        }
        if (!buckets.empty() && buckets.front().from_age != 0) {
            return buckets.front().from_age_pos;
        }
        for (std::size_t i = 1; i < buckets.size(); ++i) {
            if (buckets[i].from_age <= buckets[i - 1].from_age) {
                return buckets[i].from_age_pos;
            }
        }
        return table.pos;
    }

    /// The block of the demography_rules object that a validate() error belongs to.
    static std::string_view block_name(RulesErrorCode code) {
        switch (code) {
            case RulesErrorCode::MortalityTable:
            case RulesErrorCode::Multiplier:
                return "mortality";
            case RulesErrorCode::AgeFactorTable:
            case RulesErrorCode::Conception:
                return "conception";
            case RulesErrorCode::HealthThreshold:
                return "health";
            case RulesErrorCode::Birth:
                return "birth";
            case RulesErrorCode::LifeStages:
                return "life_stages";
            case RulesErrorCode::HealthDrift:
                return "health_drift";
            case RulesErrorCode::Stress:
                return "stress";
        }
        return {};
    }

    /// sim::validate() knows the field but not the file: the error is placed at the offending step of
    /// an age table, or else at the matching block of the object (or at the object).
    void check_assembled_rules() {
        const auto result = validate(rules_);
        if (result || object_ == nullptr) {
            return;
        }
        const RulesError& error = result.error();
        SourcePos pos = object_->object.pos;
        switch (error.code) {
            case RulesErrorCode::MortalityTable:
                pos = table_problem_pos(mortality_table_);
                break;
            case RulesErrorCode::AgeFactorTable:
                pos = table_problem_pos(age_factor_table_);
                break;
            default:
                pos = key_pos(object_->object.body, block_name(error.code), pos);
                break;
        }
        fail(*object_, pos, std::format("demography rules are inconsistent: {}", error.message));
    }
};

}  // namespace

DemographyRulesResult build_demography_rules(const data::ContentRegistry& registry) {
    return Builder(registry).build();
}

DemographyContentResult load_demography_content(std::span<const data::ContentSource* const> roots) {
    const data::SchemaSet schemas = data::content_schemas();
    data::LoadResult loaded = data::load_content(roots, schemas);
    if (!loaded.ok()) {
        return DemographyContentResult::err(std::move(loaded.errors));
    }
    DemographyRulesResult rules = build_demography_rules(loaded.registry);
    if (!rules) {
        return DemographyContentResult::err(rules.error());
    }
    return DemographyContentResult::ok(
        DemographyContent{std::move(rules).value(), loaded.registry.content_hash()});
}

DemographyContentResult load_demography_content(const data::ContentSource& root) {
    const data::ContentSource* roots[] = {&root};
    return load_demography_content(roots);
}

}  // namespace pat::content
