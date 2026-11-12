#include "pat/content/economy_content.hpp"

#include "pat/core/fixed.hpp"
#include "pat/data/content_schemas.hpp"
#include "pat/data/economy_schemas.hpp"
#include "pat/data/schema.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string_view>
#include <tuple>
#include <utility>

namespace pat::content {

namespace {

using core::Fixed32;
using data::ContentObject;
using data::ContentTable;
using data::ErrorCode;
using data::LoadError;
using data::SourcePos;
using data::TypedBlock;
using data::TypedField;
using data::TypedMap;
using data::TypedValue;
using namespace sim::economy;

// Names of the enumerations in `.pd` files. The numeric values are the stable enum values of
// pat/sim/economy/types.hpp; a test (tests/economy_content_test.cpp) loads the shipped files and
// compares them with EconomyRules::defaults(), so a name that drifts from its enum is caught.
constexpr std::array<std::string_view, kGroupCount> kGroupNames{
    "nobility", "clergy", "townsfolk", "peasants", "cossacks", "natives"};
constexpr std::array<std::string_view, kProvinceKindCount> kProvinceKindNames{
    "rural", "city", "border", "industrial", "port", "steppe", "northern", "simplified"};

/// A string with the lifetime of the process. BuildingDef::key is a std::string_view (the default
/// table points at string literals), so keys read from files need storage that outlives every copy
/// of the rules. Keys are interned in a pool that is only ever appended to; it carries no game state
/// and does not influence any value.
std::string_view stable_key(std::string_view text) {
    static std::mutex mutex;
    static std::set<std::string, std::less<>> pool;
    const std::lock_guard lock(mutex);
    return *pool.emplace(text).first;
}

// ---- typed accessors: schema validation guarantees presence and type; defaults keep us safe ------

std::int64_t int_of(const TypedBlock& block, std::string_view name) {
    const TypedValue* value = block.find(name);
    if (value != nullptr) {
        if (const auto* i = value->get_if<std::int64_t>()) {
            return *i;
        }
    }
    return 0;
}

std::int32_t i32_of(const TypedBlock& block, std::string_view name) {
    return static_cast<std::int32_t>(int_of(block, name));
}

Fixed32 fixed_of(const TypedBlock& block, std::string_view name) {
    const TypedValue* value = block.find(name);
    if (value != nullptr) {
        if (const auto* f = value->get_if<Fixed32>()) {
            return *f;
        }
    }
    return Fixed32::zero();
}

const TypedBlock* block_of(const TypedBlock& block, std::string_view name) {
    const TypedValue* value = block.find(name);
    return value != nullptr ? value->get_if<TypedBlock>() : nullptr;
}

const TypedMap* map_of(const TypedBlock& block, std::string_view name) {
    const TypedValue* value = block.find(name);
    return value != nullptr ? value->get_if<TypedMap>() : nullptr;
}

/// Position of the key of a field (the object itself when the field is missing).
SourcePos pos_of(const ContentObject& object, std::string_view name) {
    for (const TypedField& field : object.object.body.fields) {
        if (field.name == name) {
            return field.pos;
        }
    }
    return object.object.pos;
}

template <class Names>
std::optional<std::size_t> index_in(const Names& names, std::string_view name) {
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (names[i] == name) {
            return i;
        }
    }
    return std::nullopt;
}

template <class Names>
std::string unknown_name_text(std::string_view what, std::string_view name, const Names& names) {
    std::vector<std::string> candidates;
    std::string list;
    for (const std::string_view n : names) {
        candidates.emplace_back(n);
        list += (list.empty() ? "" : ", ");
        list += n;
    }
    std::string text = std::format("unknown {} '{}' (allowed: {})", what, name, list);
    const std::string hint = data::suggest_similar(name, candidates);
    if (!hint.empty()) {
        text += std::format(" (did you mean '{}'?)", hint);
    }
    return text;
}

// ---- the builder ---------------------------------------------------------------------------------

class Builder {
public:
    explicit Builder(const data::ContentRegistry& registry) : registry_(registry) {}

    EconomyRulesResult build() {
        const ContentTable* rules_table = registry_.table(data::kEconomyRulesKind);
        if (rules_table == nullptr || rules_table->empty()) {
            fail_general(std::format("no {} object found (expected: {} \"main\" {{ ... }})",
                                     data::kEconomyRulesKind,
                                     data::kEconomyRulesKind));
        } else {
            rules_object_ = &rules_table->objects().front();
            for (std::size_t i = 1; i < rules_table->size(); ++i) {
                const ContentObject& extra = rules_table->objects()[i];
                fail(extra,
                     extra.object.pos,
                     std::format("only one {} object is allowed, '{}' is already defined in {}:{}:{}",
                                 data::kEconomyRulesKind,
                                 rules_object_->object.id,
                                 rules_object_->file,
                                 rules_object_->object.pos.line,
                                 rules_object_->object.pos.column));
            }
        }

        load_goods();
        load_groups();
        load_province_kinds();
        load_tax_levels();
        load_recruit_levels();
        load_buildings();
        if (rules_object_ != nullptr) {
            load_scalars(*rules_object_);
        }

        if (errors_.empty()) {
            check_assembled_rules();
        }
        if (!errors_.empty()) {
            sort_errors();
            return EconomyRulesResult::err(std::move(errors_));
        }
        return EconomyRulesResult::ok(std::move(rules_));
    }

private:
    const data::ContentRegistry& registry_;
    const ContentObject* rules_object_ = nullptr;
    EconomyRules rules_{};
    std::vector<LoadError> errors_;
    std::map<std::string, std::size_t, std::less<>> good_slots_;  // good id -> slot
    std::vector<const ContentObject*> tax_levels_;                // ordered by index
    std::vector<const ContentObject*> recruit_levels_;

    // ---- errors ----

    void fail(const ContentObject& object, SourcePos pos, std::string message) {
        errors_.push_back(LoadError{object.root,
                                    registry_.root_name(object.root),
                                    object.file,
                                    data::Diagnostic{pos, ErrorCode::ContentRule, std::move(message)}});
    }

    /// A problem that belongs to no single object (something is missing): reported at the
    /// economy_rules object when there is one.
    void fail_general(std::string message) {
        if (rules_object_ != nullptr) {
            fail(*rules_object_, rules_object_->object.pos, std::move(message));
            return;
        }
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

    // ---- tables of objects ----

    /// Calls `store(group index, value)` for every `group = value` entry of a map; a key that is not a
    /// group name is an error at the key.
    template <class Store>
    void for_each_group_entry(const ContentObject& object, const TypedMap* map, Store&& store) {
        if (map == nullptr) {
            return;
        }
        for (const TypedField& entry : map->entries) {
            const std::optional<std::size_t> group = index_in(kGroupNames, entry.name);
            if (!group) {
                fail(object, entry.pos, unknown_name_text("population group", entry.name, kGroupNames));
                continue;
            }
            store(*group, entry.value);
        }
    }

    void load_goods() {
        const ContentTable* table = registry_.table(data::kGoodKind);
        if (table == nullptr) {
            return;
        }
        std::array<const ContentObject*, kMaxGoods> by_slot{};
        for (const ContentObject& object : table->objects()) {
            const TypedBlock& body = object.object.body;
            const std::int64_t slot = int_of(body, "slot");
            if (slot < 0 || slot >= static_cast<std::int64_t>(kMaxGoods)) {
                fail(object,
                     pos_of(object, "slot"),
                     std::format("good slot {} is outside 0...{}", slot, kMaxGoods - 1));
                continue;
            }
            const auto s = static_cast<std::size_t>(slot);
            if (by_slot[s] != nullptr) {
                fail(object,
                     pos_of(object, "slot"),
                     std::format("slot {} is already used by good '{}'", slot, by_slot[s]->object.id));
                continue;
            }
            by_slot[s] = &object;
            good_slots_.emplace(object.object.id, s);

            rules_.production.base_price_kop[s] = int_of(body, "base_price_kop");
            rules_.consumption.shortage_wealth_points[s] = i32_of(body, "shortage_wealth_points");
            for_each_group_entry(
                object, map_of(body, "need_per_head_milli"), [&](std::size_t group, const TypedValue& value) {
                    if (const auto* v = value.get_if<std::int64_t>()) {
                        rules_.consumption.need_per_head_milli[group][s] = static_cast<std::int32_t>(*v);
                    }
                });
        }
    }

    void load_groups() {
        const ContentTable* table = registry_.table(data::kPopulationGroupKind);
        std::array<const ContentObject*, kGroupCount> seen{};
        if (table != nullptr) {
            for (const ContentObject& object : table->objects()) {
                const std::optional<std::size_t> group = index_in(kGroupNames, object.object.id);
                if (!group) {
                    fail(object,
                         object.object.pos,
                         unknown_name_text("population group", object.object.id, kGroupNames));
                    continue;
                }
                const std::size_t g = *group;
                seen[g] = &object;
                const TypedBlock& body = object.object.body;
                rules_.population.birth_ppm_year[g] = i32_of(body, "birth_ppm_year");
                rules_.population.death_ppm_year[g] = i32_of(body, "death_ppm_year");
                rules_.tax.head_tax_kop_year[g] = int_of(body, "head_tax_kop_year");
                rules_.consumption.wealth_base[g] = i32_of(body, "wealth_base");
                rules_.mood.base_mood[g] = fixed_of(body, "base_mood");
                rules_.unrest.influence_permille[g] = i32_of(body, "unrest_influence_permille");
                rules_.recruit.share_permille[g] = i32_of(body, "recruit_share_permille");
            }
        }
        for (std::size_t g = 0; g < kGroupCount; ++g) {
            if (seen[g] == nullptr) {
                fail_general(
                    std::format("{} '{}' is not defined", data::kPopulationGroupKind, kGroupNames[g]));
            }
        }
    }

    void load_province_kinds() {
        const ContentTable* table = registry_.table(data::kProvinceKindKind);
        std::array<const ContentObject*, kProvinceKindCount> seen{};
        if (table != nullptr) {
            for (const ContentObject& object : table->objects()) {
                const std::optional<std::size_t> kind = index_in(kProvinceKindNames, object.object.id);
                if (!kind) {
                    fail(object,
                         object.object.pos,
                         unknown_name_text("province kind", object.object.id, kProvinceKindNames));
                    continue;
                }
                seen[*kind] = &object;
                rules_.production.owner_share_permille[*kind] =
                    i32_of(object.object.body, "owner_share_permille");
            }
        }
        for (std::size_t k = 0; k < kProvinceKindCount; ++k) {
            if (seen[k] == nullptr) {
                fail_general(
                    std::format("{} '{}' is not defined", data::kProvinceKindKind, kProvinceKindNames[k]));
            }
        }
    }

    /// The level objects of a kind ordered by their `index`; indices must be 0...n-1 without gaps
    /// or repeats. Empty (with errors) otherwise.
    std::vector<const ContentObject*> ordered_levels(std::string_view kind) {
        std::array<const ContentObject*, kMaxPolicyLevels> by_index{};
        std::size_t count = 0;
        if (const ContentTable* table = registry_.table(kind)) {
            for (const ContentObject& object : table->objects()) {
                const std::int64_t index = int_of(object.object.body, "index");
                if (index < 0 || index >= static_cast<std::int64_t>(kMaxPolicyLevels)) {
                    fail(object,
                         pos_of(object, "index"),
                         std::format("level index {} is outside 0...{}", index, kMaxPolicyLevels - 1));
                    continue;
                }
                const auto i = static_cast<std::size_t>(index);
                if (by_index[i] != nullptr) {
                    fail(object,
                         pos_of(object, "index"),
                         std::format(
                             "index {} is already used by {} '{}'", index, kind, by_index[i]->object.id));
                    continue;
                }
                by_index[i] = &object;
                ++count;
            }
        }
        std::vector<const ContentObject*> ordered;
        for (std::size_t i = 0; i < count; ++i) {
            if (by_index[i] == nullptr) {
                fail_general(std::format(
                    "{} indices must be 0...{} without gaps: index {} is missing", kind, count - 1, i));
                return {};
            }
            ordered.push_back(by_index[i]);
        }
        if (count == 0) {
            fail_general(std::format("at least one {} is required", kind));
        }
        return ordered;
    }

    void fill_mood_pressure(const ContentObject& object, std::array<Fixed32, kGroupCount>& out) {
        for_each_group_entry(object,
                             map_of(object.object.body, "mood_pressure"),
                             [&](std::size_t group, const TypedValue& value) {
                                 if (const auto* v = value.get_if<Fixed32>()) {
                                     out[group] = *v;
                                 }
                             });
    }

    void load_tax_levels() {
        const auto& levels = tax_levels_ = ordered_levels(data::kTaxLevelKind);
        rules_.tax.level_count = static_cast<std::int32_t>(levels.size());
        for (std::size_t i = 0; i < levels.size(); ++i) {
            rules_.tax.levels[i].tax_permille = i32_of(levels[i]->object.body, "tax_permille");
            fill_mood_pressure(*levels[i], rules_.tax.levels[i].mood_pressure);
        }
    }

    void load_recruit_levels() {
        const auto& levels = recruit_levels_ = ordered_levels(data::kRecruitLevelKind);
        rules_.recruit.level_count = static_cast<std::int32_t>(levels.size());
        for (std::size_t i = 0; i < levels.size(); ++i) {
            rules_.recruit.levels[i].recruit_permille = i32_of(levels[i]->object.body, "recruit_permille");
            fill_mood_pressure(*levels[i], rules_.recruit.levels[i].mood_pressure);
        }
    }

    void load_buildings() {
        const ContentTable* table = registry_.table(data::kBuildingKind);
        if (table == nullptr) {
            return;
        }
        // By slot, so the order of the definitions does not depend on ids or files.
        std::vector<const ContentObject*> sorted;
        for (const ContentObject& object : table->objects()) {
            sorted.push_back(&object);
        }
        std::stable_sort(sorted.begin(), sorted.end(), [](const ContentObject* a, const ContentObject* b) {
            return int_of(a->object.body, "slot") < int_of(b->object.body, "slot");
        });

        std::int32_t count = 0;
        const ContentObject* previous = nullptr;
        for (const ContentObject* object : sorted) {
            const TypedBlock& body = object->object.body;
            const std::int64_t slot = int_of(body, "slot");
            if (previous != nullptr && int_of(previous->object.body, "slot") == slot) {
                fail(*object,
                     pos_of(*object, "slot"),
                     std::format("slot {} is already used by building '{}'", slot, previous->object.id));
                continue;
            }
            previous = object;
            if (count >= static_cast<std::int32_t>(kMaxBuildingTypes)) {
                fail(*object,
                     object->object.pos,
                     std::format("at most {} buildings are supported", kMaxBuildingTypes));
                continue;
            }
            BuildingDef def;
            def.slot = static_cast<std::uint8_t>(slot);
            if (const TypedValue* key = body.find("name_key")) {
                if (const auto* text = key->get_if<std::string>()) {
                    def.key = stable_key(*text);
                }
            }
            if (const TypedValue* key = body.find("desc_key")) {
                if (const auto* text = key->get_if<std::string>()) {
                    def.desc_key = stable_key(*text);
                }
            }
            def.cost_kop = int_of(body, "cost_kop");
            def.build_days = i32_of(body, "build_days");
            def.upkeep_kop_month = int_of(body, "upkeep_kop_month");
            def.output_permille.fill(kPermilleOne);
            if (const TypedMap* map = map_of(body, "output_permille")) {
                for (const TypedField& entry : map->entries) {
                    const auto good = good_slots_.find(entry.name);
                    if (good == good_slots_.end()) {
                        fail(*object, entry.pos, unknown_good_text(entry.name));
                        continue;
                    }
                    if (const auto* v = entry.value.get_if<std::int64_t>()) {
                        def.output_permille[good->second] = static_cast<std::int32_t>(*v);
                    }
                }
            }
            // effects of E9-T17: a missing key means "no effect"
            def.control_points = i32_of(body, "control_points");
            def.oversight_permille = i32_of(body, "oversight_permille");
            def.recruit_permille =
                body.find("recruit_permille") != nullptr ? i32_of(body, "recruit_permille") : kPermilleOne;
            def.famine_protect_permille = i32_of(body, "famine_protect_permille");
            def.min_development = i32_of(body, "min_development");
            def.max_per_province =
                body.find("max_per_province") != nullptr ? i32_of(body, "max_per_province") : 1;
            if (const TypedValue* kinds = body.find("allowed_kinds")) {
                if (const auto* list = kinds->get_if<data::TypedList>()) {
                    for (const TypedValue& item : *list) {
                        const auto* ref = item.get_if<data::RefValue>();
                        const auto kind =
                            ref != nullptr ? index_in(kProvinceKindNames, ref->id) : std::nullopt;
                        if (!kind) {
                            fail(*object,
                                 item.pos,
                                 unknown_name_text(
                                     "province kind", ref != nullptr ? ref->id : "", kProvinceKindNames));
                            continue;
                        }
                        def.allowed_kinds |= 1U << *kind;
                    }
                }
            }
            rules_.buildings.defs[static_cast<std::size_t>(count)] = def;
            ++count;
        }
        rules_.buildings.count = count;
    }

    std::string unknown_good_text(std::string_view name) const {
        std::vector<std::string> ids;
        for (const auto& [id, slot] : good_slots_) {
            (void)slot;
            ids.push_back(id);
        }
        std::string text = std::format("there is no good '{}'", name);
        const std::string hint = data::suggest_similar(name, ids);
        if (!hint.empty()) {
            text += std::format(" (did you mean '{}'?)", hint);
        }
        return text;
    }

    // ---- the economy_rules object ----

    template <class Fn>
    void with_block(const ContentObject& object, std::string_view name, Fn&& fn) {
        if (const TypedBlock* block = block_of(object.object.body, name)) {
            fn(*block);
        }
    }

    /// The index of the level a `ref` field points at, or nullopt (the dangling reference itself was
    /// reported by the loader).
    static std::optional<std::int32_t> level_index(const TypedBlock& body,
                                                   std::string_view field,
                                                   const std::vector<const ContentObject*>& levels) {
        const TypedValue* value = body.find(field);
        const auto* ref = value != nullptr ? value->get_if<data::RefValue>() : nullptr;
        if (ref == nullptr) {
            return std::nullopt;
        }
        for (std::size_t i = 0; i < levels.size(); ++i) {
            if (levels[i]->object.id == ref->id) {
                return static_cast<std::int32_t>(i);
            }
        }
        return std::nullopt;
    }

    void load_scalars(const ContentObject& object) {
        const TypedBlock& body = object.object.body;

        if (const TypedValue* mode = body.find("crown_account_mode")) {
            if (const auto* e = mode->get_if<data::EnumValue>()) {
                rules_.crown_account_mode = e->name == "emperor_dynasty" ? CrownAccountMode::EmperorDynasty
                                                                         : CrownAccountMode::Separate;
            }
        }
        if (const TypedValue* flag = body.find("build_foreign_allowed")) {
            if (const auto* b = flag->get_if<bool>()) {
                rules_.build_foreign_allowed = *b;
            }
        }
        if (const auto index = level_index(body, "default_tax_level", tax_levels_)) {
            rules_.tax.default_level = *index;
        }
        if (const auto index = level_index(body, "default_recruit_level", recruit_levels_)) {
            rules_.recruit.default_level = *index;
        }

        with_block(object, "population", [&](const TypedBlock& b) {
            rules_.population.wealth_growth_ppm_per_point = i32_of(b, "wealth_growth_ppm_per_point");
            rules_.population.capacity_per_development = i32_of(b, "capacity_per_development");
        });
        with_block(object, "production", [&](const TypedBlock& b) {
            rules_.production.labour_min_permille = i32_of(b, "labour_min_permille");
            rules_.production.labour_max_permille = i32_of(b, "labour_max_permille");
            rules_.production.labour_mood_slope_permille = i32_of(b, "labour_mood_slope_permille");
            rules_.production.losses_permille = i32_of(b, "losses_permille");
        });
        with_block(object, "consumption", [&](const TypedBlock& b) {
            rules_.consumption.stock_cap_months = i32_of(b, "stock_cap_months");
            rules_.consumption.wealth_inertia_permille = i32_of(b, "wealth_inertia_permille");
        });
        with_block(object, "tax", [&](const TypedBlock& b) {
            rules_.tax.law_permille = i32_of(b, "law_permille");
            rules_.tax.low_control_tax_permille = i32_of(b, "low_control_tax_permille");
            rules_.tax.city_crown_share_permille = i32_of(b, "city_crown_share_permille");
        });
        with_block(object, "control", [&](const TypedBlock& b) {
            ControlRules& c = rules_.control;
            c.low_threshold = i32_of(b, "low_threshold");
            c.anarchy_threshold = i32_of(b, "anarchy_threshold");
            c.administrator_neutral = i32_of(b, "administrator_neutral");
            c.control_base = i32_of(b, "control_base");
            c.steward_gain_permille = i32_of(b, "steward_gain_permille");
            c.unrest_penalty_permille = i32_of(b, "unrest_penalty_permille");
            c.control_inertia_permille = i32_of(b, "control_inertia_permille");
            c.corruption_base_permille = i32_of(b, "corruption_base_permille");
            c.integrity_gain_permille = i32_of(b, "integrity_gain_permille");
            c.low_control_corruption_permille = i32_of(b, "low_control_corruption_permille");
            c.corruption_inertia_permille = i32_of(b, "corruption_inertia_permille");
        });
        with_block(object, "mood", [&](const TypedBlock& b) {
            rules_.mood.wealth_slope_permille = i32_of(b, "wealth_slope_permille");
            rules_.mood.admin_slope_permille = i32_of(b, "admin_slope_permille");
            rules_.mood.inertia_permille = i32_of(b, "inertia_permille");
        });
        with_block(object, "unrest", [&](const TypedBlock& b) {
            UnrestRules& u = rules_.unrest;
            u.slope_permille = i32_of(b, "slope_permille");
            u.control_slope_permille = i32_of(b, "control_slope_permille");
            u.inertia_permille = i32_of(b, "inertia_permille");
            u.calm_below = i32_of(b, "calm_below");
            u.riot_from = i32_of(b, "riot_from");
            u.uprising_from = i32_of(b, "uprising_from");
            u.riot_chance_min_permille = i32_of(b, "riot_chance_min_permille");
            u.riot_chance_max_permille = i32_of(b, "riot_chance_max_permille");
            u.riot_loot_permille = i32_of(b, "riot_loot_permille");
            u.riot_control_loss = i32_of(b, "riot_control_loss");
        });
        with_block(object, "recruit", [&](const TypedBlock& b) {
            rules_.recruit.regen_permille = i32_of(b, "regen_permille");
            rules_.recruit.quota_penalty_honor = i32_of(b, "quota_penalty_honor");
        });
        with_block(object, "loans", [&](const TypedBlock& b) {
            LoanRules& l = rules_.loans;
            l.rate_ppm_year = i32_of(b, "rate_ppm_year");
            l.term_months = i32_of(b, "term_months");
            l.base_limit_kop = int_of(b, "base_limit_kop");
            l.per_honor_kop = int_of(b, "per_honor_kop");
            l.bankruptcy_threshold_kop = int_of(b, "bankruptcy_threshold_kop");
            l.bankruptcy_exit_kop = int_of(b, "bankruptcy_exit_kop");
            l.max_loans_per_dynasty = i32_of(b, "max_loans_per_dynasty");
        });
        with_block(object, "buildings", [&](const TypedBlock& b) {
            rules_.buildings.refund_permille = i32_of(b, "refund_permille");
        });
    }

    // ---- the final check ----

    static std::string_view block_name(EconomyRulesErrorCode code) {
        switch (code) {
            case EconomyRulesErrorCode::Population:
                return "population";
            case EconomyRulesErrorCode::Production:
                return "production";
            case EconomyRulesErrorCode::Consumption:
                return "consumption";
            case EconomyRulesErrorCode::Tax:
                return "tax";
            case EconomyRulesErrorCode::Control:
                return "control";
            case EconomyRulesErrorCode::Mood:
                return "mood";
            case EconomyRulesErrorCode::Unrest:
                return "unrest";
            case EconomyRulesErrorCode::Recruit:
                return "recruit";
            case EconomyRulesErrorCode::Loans:
                return "loans";
            case EconomyRulesErrorCode::Buildings:
                return "buildings";
            case EconomyRulesErrorCode::Flags:
                break;
        }
        return {};
    }

    /// sim::economy::validate() knows the field but not the file: the error is placed at the
    /// matching block of the economy_rules object (or at the object).
    void check_assembled_rules() {
        const auto result = validate(rules_);
        if (result || rules_object_ == nullptr) {
            return;
        }
        const EconomyRulesError& error = result.error();
        const std::string_view block = block_name(error.code);
        fail(*rules_object_,
             block.empty() ? rules_object_->object.pos : pos_of(*rules_object_, block),
             std::format("economy rules are inconsistent: {}", error.message));
    }
};

}  // namespace

EconomyRulesResult build_economy_rules(const data::ContentRegistry& registry) {
    return Builder(registry).build();
}

std::optional<sim::economy::ProvinceKind> province_kind_from_key(std::string_view key) {
    if (const auto index = index_in(kProvinceKindNames, key)) {
        return static_cast<sim::economy::ProvinceKind>(*index);
    }
    return std::nullopt;
}

EconomyContentResult load_economy_content(std::span<const data::ContentSource* const> roots) {
    const data::SchemaSet schemas = data::content_schemas();
    data::LoadResult loaded = data::load_content(roots, schemas);
    if (!loaded.ok()) {
        return EconomyContentResult::err(std::move(loaded.errors));
    }
    EconomyRulesResult rules = build_economy_rules(loaded.registry);
    if (!rules) {
        return EconomyContentResult::err(rules.error());
    }
    return EconomyContentResult::ok(EconomyContent{std::move(rules).value(), loaded.registry.content_hash()});
}

EconomyContentResult load_economy_content(const data::ContentSource& root) {
    const data::ContentSource* roots[] = {&root};
    return load_economy_content(roots);
}

}  // namespace pat::content
