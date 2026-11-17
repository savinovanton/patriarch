#include "pat/content/court_content.hpp"

#include "pat/content/economy_content.hpp"
#include "pat/data/content_schemas.hpp"
#include "pat/data/court_schemas.hpp"
#include "pat/data/schema.hpp"
#include "pat/sim/date.hpp"

#include <algorithm>
#include <cstddef>
#include <format>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "typed_accessors.hpp"

namespace pat::content {

namespace {

using data::ContentObject;
using data::SourcePos;
using data::TypedBlock;

/// Id -> numeric slot of the objects of a kind (`skill`: the Skill index, `trait`: the u16 slot).
using SlotMap = std::map<std::string, std::int32_t, std::less<>>;

SlotMap slots_of(const data::ContentRegistry& registry, std::string_view kind) {
    SlotMap slots;
    if (const auto* table = registry.table(kind)) {
        for (const ContentObject& object : table->objects()) {
            slots.emplace(object.object.id, detail::i32_of(object, "slot"));
        }
    }
    return slots;
}

std::string unknown_text(std::string_view what, std::string_view id, const SlotMap& known) {
    std::vector<std::string> candidates;
    for (const auto& [name, slot] : known) {
        (void)slot;
        candidates.push_back(name);
    }
    std::string text = std::format("there is no {} '{}'", what, id);
    if (const std::string hint = data::suggest_similar(id, candidates); !hint.empty()) {
        text += std::format(" (did you mean '{}'?)", hint);
    }
    return text;
}

/// Position of a dotted path such as `seats.max` or `requirements.skills[2]` inside an object: the key
/// of the deepest field that exists, so an error never points outside the object.
SourcePos path_position(const ContentObject& object, std::string_view path) {
    SourcePos pos = object.object.pos;
    const TypedBlock* block = &object.object.body;
    while (block != nullptr && !path.empty()) {
        const auto dot = path.find('.');
        std::string_view name = path.substr(0, dot);
        name = name.substr(0, name.find('['));
        pos = detail::key_pos(*block, name, pos);
        block = dot == std::string_view::npos ? nullptr : detail::block_of(*block, name);
        path = dot == std::string_view::npos ? std::string_view{} : path.substr(dot + 1);
    }
    return pos;
}

class Builder {
public:
    explicit Builder(const data::ContentRegistry& registry)
        : registry_(registry), errors_(registry), skills_(slots_of(registry, "skill")),
          traits_(slots_of(registry, "trait")) {}

    CourtRulesResult build() {
        load_offices();
        load_servants();
        load_parties();
        load_balance();
        if (errors_.empty()) {
            if (const auto checked = sim::validate(rules_); !checked) {
                report_invalid(checked.error());
            }
        }
        if (!errors_.empty()) {
            return CourtRulesResult::err(errors_.take());
        }
        return CourtRulesResult::ok(std::move(rules_));
    }

private:
    const data::ContentRegistry& registry_;
    ErrorSink errors_;
    SlotMap skills_;
    SlotMap traits_;
    sim::CourtRules rules_;

    /// The objects of a kind ordered by their stable slot (ties keep id order from the registry).
    std::vector<const ContentObject*> by_slot(std::string_view kind, std::string_view label) {
        std::vector<const ContentObject*> sorted;
        const auto* table = registry_.table(kind);
        if (table == nullptr || table->empty()) {
            errors_.fail_general(
                std::format("no {} object found (expected: {} \"id\" {{ ... }})", label, label));
            return sorted;
        }
        for (const ContentObject& object : table->objects()) {
            sorted.push_back(&object);
        }
        std::stable_sort(sorted.begin(), sorted.end(), [](const ContentObject* a, const ContentObject* b) {
            return detail::i32_of(*a, "slot") < detail::i32_of(*b, "slot");
        });
        // A repeated slot is reported on the later object and the object is dropped.
        std::vector<const ContentObject*> unique;
        for (const ContentObject* object : sorted) {
            if (!unique.empty() &&
                detail::i32_of(*unique.back(), "slot") == detail::i32_of(*object, "slot")) {
                errors_.fail(*object,
                             detail::field_pos(*object, "slot"),
                             std::format("slot {} is already used by {} '{}'",
                                         detail::i32_of(*object, "slot"),
                                         label,
                                         unique.back()->object.id));
                continue;
            }
            unique.push_back(object);
        }
        return unique;
    }

    /// Slot of the `skill` or `trait` an id names; reports an unknown id at `pos`.
    std::int32_t resolve(const SlotMap& known,
                         std::string_view what,
                         std::string_view id,
                         const ContentObject& object,
                         SourcePos pos) {
        const auto found = known.find(id);
        if (found == known.end()) {
            errors_.fail(object, pos, unknown_text(what, id, known));
            return 0;
        }
        return found->second;
    }

    void load_offices() {
        for (const ContentObject* object : by_slot(data::kOfficeKind, data::kOfficeKind)) {
            rules_.offices.push_back(read_office(*object));
        }
    }

    sim::OfficeDef read_office(const ContentObject& object) {
        const TypedBlock& body = object.object.body;
        sim::OfficeDef def;
        def.slot = static_cast<std::uint8_t>(detail::i32_of(object, "slot"));
        def.id = object.object.id;
        def.name_key = detail::text_of(object, "name_key");
        def.desc_key = detail::text_of(object, "desc_key");
        def.group = static_cast<sim::OfficeGroup>(detail::enum_index_of(object, "group"));
        def.appointer = static_cast<sim::OfficeAppointer>(detail::enum_index_of(object, "appointer"));
        if (const auto* seats = detail::block_of(body, "seats")) {
            const auto* rule = detail::value_of<data::EnumValue>(*seats, "rule");
            def.seats.rule = static_cast<sim::SeatRule>(rule != nullptr ? rule->index : 0);
            def.seats.slots = detail::i32_of(*seats, "slots");
            def.seats.max = detail::i32_of(*seats, "max");
            if (const auto* kinds = detail::list_of(*seats, "kinds")) {
                for (const data::TypedValue& item : *kinds) {
                    const auto* ref = item.get_if<data::RefValue>();
                    const auto kind = ref != nullptr ? province_kind_from_key(ref->id) : std::nullopt;
                    if (!kind) {
                        errors_.fail(
                            object,
                            item.pos,
                            std::format("unknown province kind '{}'", ref != nullptr ? ref->id : ""));
                        continue;
                    }
                    def.seats.kinds |= 1U << static_cast<unsigned>(*kind);
                }
            }
        }
        if (const auto* requirements = detail::block_of(body, "requirements")) {
            auto& r = def.requirements;
            r.min_age = detail::i32_of(*requirements, "min_age");
            const auto* sex = detail::value_of<data::EnumValue>(*requirements, "sex_allowed");
            r.sex_allowed = static_cast<sim::SexRule>(sex != nullptr ? sex->index : 0);
            r.min_honor = detail::i32_of(*requirements, "min_honor");
            if (const auto* skills = detail::value_of<data::TypedMap>(*requirements, "skills")) {
                for (const data::TypedField& entry : skills->entries) {
                    const auto skill = resolve(skills_, "skill", entry.name, object, entry.pos);
                    if (const auto* level = entry.value.get_if<std::int64_t>();
                        level != nullptr && skill >= 0 &&
                        static_cast<std::size_t>(skill) < r.min_skill.size()) {
                        r.min_skill[static_cast<std::size_t>(skill)] = static_cast<std::uint8_t>(*level);
                    }
                }
            }
            if (const auto* list = detail::list_of(*requirements, "not_traits")) {
                for (const data::TypedValue& item : *list) {
                    if (const auto* ref = item.get_if<data::RefValue>()) {
                        r.not_traits.push_back(
                            static_cast<std::uint16_t>(resolve(traits_, "trait", ref->id, object, item.pos)));
                    }
                }
            }
        }
        if (const auto* weights = detail::value_of<data::TypedMap>(body, "skill_weights")) {
            for (const data::TypedField& entry : weights->entries) {
                const auto skill = resolve(skills_, "skill", entry.name, object, entry.pos);
                if (const auto* points = entry.value.get_if<std::int64_t>();
                    points != nullptr && skill >= 0 &&
                    static_cast<std::size_t>(skill) < def.skill_weights.size()) {
                    def.skill_weights[static_cast<std::size_t>(skill)] = static_cast<std::int32_t>(*points);
                }
            }
        }
        def.salary_kop = detail::int_of(body, "salary_kop");
        def.honor_on_assign = detail::i32_of(object, "honor_on_assign");
        def.honor_monthly = detail::i32_of(object, "honor_monthly");
        def.decision_delay_days = detail::i32_of(object, "decision_delay_days");
        def.from_date = date_of(body, "from_date");
        def.to_date = date_of(body, "to_date");
        def.administrator_priority = detail::i32_of(object, "administrator_priority");
        if (const auto* ref = detail::value_of<data::RefValue>(body, "grants_trait")) {
            def.grants_trait = static_cast<std::uint16_t>(
                resolve(traits_, "trait", ref->id, object, detail::field_pos(object, "grants_trait")));
        }
        def.council_vote_weight = detail::i32_of(object, "council_vote_weight");
        return def;
    }

    static sim::Date date_of(const TypedBlock& body, std::string_view name) {
        const auto* value = detail::value_of<data::DateValue>(body, name);
        return value != nullptr ? sim::Date::from_ymd(value->year, value->month, value->day) : sim::kNoDate;
    }

    void load_servants() {
        for (const ContentObject* object : by_slot(data::kServantRoleKind, data::kServantRoleKind)) {
            sim::ServantRoleDef def;
            def.slot = static_cast<std::uint8_t>(detail::i32_of(*object, "slot"));
            def.id = object->object.id;
            def.name_key = detail::text_of(*object, "name_key");
            def.effect_key = detail::text_of(*object, "effect_key");
            if (const auto* ref = detail::value_of<data::RefValue>(object->object.body, "skill")) {
                def.skill = static_cast<std::uint8_t>(
                    resolve(skills_, "skill", ref->id, *object, detail::field_pos(*object, "skill")));
            }
            def.slots = detail::i32_of(*object, "slots");
            def.skill_min = detail::i32_of(*object, "skill_min");
            def.skill_max = detail::i32_of(*object, "skill_max");
            def.wage_base_kop = detail::int_of(object->object.body, "wage_base_kop");
            def.wage_per_skill_kop = detail::int_of(object->object.body, "wage_per_skill_kop");
            def.fee_months = detail::i32_of(*object, "fee_months");
            rules_.servants.push_back(std::move(def));
        }
    }

    void load_parties() {
        for (const ContentObject* object : by_slot(data::kCourtPartyKind, data::kCourtPartyKind)) {
            sim::CourtPartyDef def;
            def.slot = static_cast<std::uint8_t>(detail::i32_of(*object, "slot"));
            def.id = object->object.id;
            def.name_key = detail::text_of(*object, "name_key");
            def.agenda_key = detail::text_of(*object, "agenda_key");
            rules_.parties.push_back(std::move(def));
        }
    }

    void load_balance() {
        const auto* table = registry_.table(data::kCourtBalanceKind);
        if (table == nullptr || table->empty()) {
            errors_.fail_general(std::format("no {} object found (expected: {} \"main\" {{ ... }})",
                                             data::kCourtBalanceKind,
                                             data::kCourtBalanceKind));
            return;
        }
        const ContentObject& object = table->objects().front();
        for (std::size_t i = 1; i < table->size(); ++i) {
            const ContentObject& extra = table->objects()[i];
            errors_.fail(extra,
                         extra.object.pos,
                         std::format("only one {} object is allowed, '{}' is already defined in {}:{}:{}",
                                     data::kCourtBalanceKind,
                                     object.object.id,
                                     object.file,
                                     object.object.pos.line,
                                     object.object.pos.column));
        }
        const TypedBlock& body = object.object.body;
        sim::CourtBalance& balance = rules_.balance;
        // One list builds the struct, the defaults, the checks and this reader (court_rules.hpp).
#define PAT_COURT_FIELD_READ(group, type, name, def, lo, hi)                                                 \
    if (const auto* block_##name = detail::block_of(body, #group)) {                                         \
        balance.name = static_cast<type>(detail::int_of(*block_##name, #name));                              \
    }
        PAT_COURT_BALANCE_FIELDS(PAT_COURT_FIELD_READ)
#undef PAT_COURT_FIELD_READ
    }

    /// Reports the first problem of sim::validate() at the key it names.
    void report_invalid(const sim::CourtRulesError& error) {
        const std::string_view field = error.field;
        const auto message = std::format("court rules are inconsistent: {}", error.message);
        for (const std::string_view kind :
             {data::kOfficeKind, data::kServantRoleKind, data::kCourtPartyKind}) {
            if (field.starts_with(kind) && field.size() > kind.size() && field[kind.size()] == ' ') {
                const auto rest = field.substr(kind.size() + 1);
                const auto dot = rest.find('.');
                const auto* object = registry_.find(kind, rest.substr(0, dot));
                if (object != nullptr) {
                    errors_.fail(*object,
                                 dot == std::string_view::npos ? object->object.pos
                                                               : path_position(*object, rest.substr(dot + 1)),
                                 message);
                    return;
                }
            }
        }
        if (const auto* table = registry_.table(data::kCourtBalanceKind);
            table != nullptr && !table->empty()) {
            const ContentObject& object = table->objects().front();
            errors_.fail(object, path_position(object, field), message);
            return;
        }
        errors_.fail_general(message);
    }
};

}  // namespace

CourtRulesResult build_court_rules(const data::ContentRegistry& registry) {
    return Builder(registry).build();
}

CourtContentResult load_court_content(std::span<const data::ContentSource* const> roots) {
    auto loaded = data::load_content(roots, data::content_schemas());
    if (!loaded.ok()) {
        return CourtContentResult::err(std::move(loaded.errors));
    }
    auto rules = build_court_rules(loaded.registry);
    if (!rules) {
        return CourtContentResult::err(std::move(rules).error());
    }
    return CourtContentResult::ok({std::move(rules).value(), loaded.registry.content_hash()});
}

CourtContentResult load_court_content(const data::ContentSource& root) {
    const data::ContentSource* roots[] = {&root};
    return load_court_content(roots);
}

}  // namespace pat::content
