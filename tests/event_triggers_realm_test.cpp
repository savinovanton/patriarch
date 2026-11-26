// E11-T08: the triggers and values of the province, the realm, time, the state of events and chance; the
// lists of the iterators with their fan-out bound. The catalog table (codes, signatures), the evaluators on
// a hand-made World boundary by boundary, the host lists (order, cut at the bound) and compiled events run
// through the machine with the host over the World. The tooltip templates of every entry are checked in
// event_triggers_character_test.cpp ("trigger catalog: every entry has Russian tooltip templates").

#include "pat/sim/economy/loans.hpp"
#include "pat/sim/economy/rules.hpp"
#include "pat/sim/economy/tables.hpp"
#include "pat/sim/economy/unrest.hpp"
#include "pat/sim/events/catalog.hpp"
#include "pat/sim/events/catalog_doc.hpp"
#include "pat/sim/events/event_set.hpp"
#include "pat/sim/events/realm_triggers.hpp"
#include "pat/sim/events/state.hpp"
#include "pat/sim/events/vm.hpp"
#include "pat/sim/events/world_host.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "event_compile_fixture.hpp"
#include "world_fixture.hpp"

using namespace pat::sim;
using namespace pat::sim::events;
using pat::core::Fixed32;
using pat::core::Fixed64;
using pat::core::StringId;

namespace {

Fixed32 fx32(int v) {
    return Fixed32::from_int(v);
}

Word whole(std::int64_t v) {
    return word_from_whole(v);
}

Date day(int y, int m, int d) {
    return Date::from_ymd(y, m, d);
}

Scope ch(CharacterId id) {
    return Scope::make(ScopeKind::Character, id.value());
}
Scope dy(DynastyId id) {
    return Scope::make(ScopeKind::Dynasty, id.value());
}
Scope pr(ProvinceId id) {
    return Scope::make(ScopeKind::Province, id.value());
}
Scope realm() {
    return Scope::make(ScopeKind::Realm, 0);
}
Word key(std::string_view name) {
    return static_cast<Word>(event_key(name));
}

constexpr std::uint8_t kMillSlot = 3;

/// Two houses with a few people, a third that died out; three provinces of house A, one of B, one ownerless.
struct Scenario {
    World world{day(1703, 5, 16), 11};
    DynastyId house_a;
    DynastyId house_b;
    DynastyId house_gone;  // exists, nobody alive: extinct
    CharacterId head_a;
    CharacterId head_b;
    std::vector<ProvinceId> provinces;  // 0..2 of A, 3 of B, 4 nobody's
    economy::EconomyRules rules = economy::EconomyRules::defaults();

    CharacterId add(DynastyId house, Date birth, CharacterId father = {}) {
        CharacterInit init;
        init.dynasty = house;
        init.birth_date = birth;
        init.sex = Sex::Male;
        init.father = father;
        const CharacterId id = world.create_character(init);
        REQUIRE(id.valid());
        return id;
    }

    explicit Scenario(bool economy = true) {
        REQUIRE(world.enable_dynasty_mode());
        house_a = world.create_dynasty(StringId(0));
        house_b = world.create_dynasty(StringId(1));
        house_gone = world.create_dynasty(StringId(2));
        head_a = add(house_a, day(1670, 1, 1));
        head_b = add(house_b, day(1668, 1, 1));
        REQUIRE(world.set_dynasty_head(house_a, head_a));
        REQUIRE(world.set_dynasty_head(house_b, head_b));
        for (int i = 0; i < 5; ++i) {
            const DynastyId owner = i < 3 ? house_a : (i == 3 ? house_b : DynastyId{});
            provinces.push_back(world.create_province(StringId(10 + i), owner, fx32(12)));
        }
        world.set_province_development(provinces[0], 4);
        world.set_province_development(provinces[1], 2);
        if (economy) {
            REQUIRE(world.enable_economy());
            economy::EconomyColumnsMut cols = world.economy_columns_mut();
            cols.unrest[0] = fx32(45);
            cols.unrest[1] = fx32(10);
            cols.control[0] = fx32(25);
            cols.corruption[0] = Fixed32::from_ratio(1, 4);
            cols.recruit_pool[0] = 640;
            cols.slots_total[0] = 2;
            cols.mood[economy::group_row(0, economy::Group::Nobility)] = fx32(-30);
            REQUIRE(world.set_slot_building(provinces[0], 1, kMillSlot));
            REQUIRE(world.set_group_size(provinces[0], economy::Group::Nobility, 1'500) == 1'500);
        }
        pat::test::require_valid(world);
    }

    [[nodiscard]] TriggerEnv env(bool with_rules = true) const {
        TriggerEnv e;
        e.world = &world;
        e.economy = with_rules ? &rules : nullptr;
        return e;
    }
};

TriggerResult pred_r(const TriggerEnv& env, Scope scope, std::uint16_t code, std::vector<Word> args = {}) {
    const auto r = eval_realm_predicate(env, scope, code, args);
    REQUIRE(r.has_value());
    return *r;
}
bool pred(const TriggerEnv& env, Scope scope, std::uint16_t code, std::vector<Word> args = {}) {
    const TriggerResult r = pred_r(env, scope, code, std::move(args));
    CHECK(r.has_data);
    return r.value == 1;
}
TriggerResult val_r(const TriggerEnv& env, Scope scope, std::uint16_t code, std::vector<Word> args = {}) {
    const auto r = eval_realm_value(env, scope, code, args);
    REQUIRE(r.has_value());
    return *r;
}
Word val(const TriggerEnv& env, Scope scope, std::uint16_t code, std::vector<Word> args = {}) {
    const TriggerResult r = val_r(env, scope, code, std::move(args));
    CHECK(r.has_data);
    return r.value;
}

}  // namespace

// ---- the table ------------------------------------------------------------------------------------------

TEST_CASE("realm triggers: the table of section 8.2 is registered with stable codes") {
    Catalog c = Catalog::engine();
    register_realm_triggers(c);
    const std::map<std::uint16_t, std::string_view> predicates{
        {1101, "has_flag"},
        {1102, "has_global_flag"},
        {1103, "has_modifier"},
        {1104, "has_fired"},
        {1108, "random_chance"},
        {1117, "has_building"},
        {1120, "is_player_controlled"},
        {1121, "has_cooldown"},
        {1125, "has_quota"},
    };
    const std::map<std::uint16_t, std::string_view> values{
        {1105, "chain_step"},
        {1106, "variable"},
        {1107, "global_variable"},
        {1109, "current_year"},
        {1110, "current_month"},
        {1111, "date"},
        {1112, "unrest"},
        {1113, "control"},
        {1114, "corruption"},
        {1115, "population"},
        {1116, "recruit_pool"},
        {1118, "unrest_state"},
        {1119, "development"},
        {1122, "group_size"},
        {1123, "group_mood"},
        {1124, "debt"},
    };
    for (const auto& [code, name] : predicates) {
        CAPTURE(name);
        const CatalogEntry* e = c.find(NameSpace::Predicate, name);
        REQUIRE(e != nullptr);
        CHECK(e->code == code);
        CHECK(e->result == ValueType::Bool);
        CHECK_FALSE(e->example.empty());
        CHECK(e->stub.empty());
    }
    for (const auto& [code, name] : values) {
        CAPTURE(name);
        const CatalogEntry* e = c.find(NameSpace::Value, name);
        REQUIRE(e != nullptr);
        CHECK(e->code == code);
        CHECK(e->result == ValueType::Number);
        CHECK_FALSE(e->example.empty());
        CHECK(e->stub.empty());
    }
    std::size_t registered = 0;
    for (const CatalogEntry& e : c.entries()) {
        if (e.space == NameSpace::Predicate || e.space == NameSpace::Value) {
            ++registered;
            CHECK((e.code >= 1101 && e.code <= 1199));  // the block of T08; 1200.. belong to the epics
        }
    }
    CHECK(registered == predicates.size() + values.size());

    // Scopes: the event state and time anywhere, the economy of a province, the house in the realm.
    CHECK(c.find(NameSpace::Predicate, "has_flag")->from == 0);
    CHECK(c.find(NameSpace::Value, "date")->from == 0);
    CHECK(c.find(NameSpace::Value, "unrest")->from == scope_bit(ScopeKind::Province));
    CHECK(c.find(NameSpace::Predicate, "is_player_controlled")->from ==
          scope_mask({ScopeKind::Character, ScopeKind::Dynasty, ScopeKind::Province}));
    CHECK(c.find(NameSpace::Value, "debt")->from == scope_bit(ScopeKind::Dynasty));
    CHECK(c.find(NameSpace::Predicate, "has_modifier")->args[0].ref_kind == "event_modifier");
    CHECK(c.find(NameSpace::Predicate, "has_building")->args[0].kind == ArgKind::Ref);
    CHECK(c.find(NameSpace::Value, "group_size")->suffixes.size() == economy::kGroupCount);

    // The standard catalog holds both blocks and the generated reference lists them and the iterators.
    const Catalog standard = Catalog::standard();
    CHECK(standard.find(NameSpace::Predicate, "has_flag") != nullptr);
    CHECK(standard.find(NameSpace::Predicate, "is_adult") != nullptr);
    const std::string reference = render_catalog_reference(standard);
    CHECK(reference.find("`has_global_flag`") != std::string::npos);
    CHECK(reference.find("## Списки итераторов (6)") != std::string::npos);
    CHECK(reference.find("| 4 | `owned_province` | dynasty | province | 128 |") != std::string::npos);
}

TEST_CASE("realm triggers: codes of other blocks are not answered") {
    Scenario s;
    const TriggerEnv env = s.env();
    CHECK_FALSE(eval_realm_predicate(env, ch(s.head_a), 1001, {}).has_value());  // T07: is_adult
    CHECK_FALSE(eval_realm_value(env, ch(s.head_a), 1021, {}).has_value());      // T07: age
    CHECK_FALSE(eval_realm_predicate(env, ch(s.head_a), 1200, {}).has_value());  // E10: has_office
    CHECK_FALSE(eval_realm_predicate(env, pr(s.provinces[0]), kValUnrest, {}).has_value());
    CHECK_FALSE(eval_realm_value(env, pr(s.provinces[0]), kTrigHasFlag, {}).has_value());
    CHECK_FALSE(eval_character_predicate(env, ch(s.head_a), kTrigHasFlag, {}).has_value());
}

// ---- the state of events --------------------------------------------------------------------------------

TEST_CASE("realm triggers: flags of a scope and global flags, with their last day") {
    Scenario s;
    const TriggerEnv env = s.env();
    const Scope head = ch(s.head_a);
    CHECK_FALSE(pred(env, head, kTrigHasFlag, {key("hushed_theft")}));
    REQUIRE(s.world.set_event_flag(head, event_key("hushed_theft"), day(1703, 5, 16)));
    CHECK(pred(env, head, kTrigHasFlag, {key("hushed_theft")}));
    CHECK_FALSE(pred(env, head, kTrigHasFlag, {key("other")}));
    CHECK_FALSE(pred(env, ch(s.head_b), kTrigHasFlag, {key("hushed_theft")}));  // another scope
    CHECK_FALSE(pred(env, head, kTrigHasGlobalFlag, {key("hushed_theft")}));    // not global

    REQUIRE(s.world.set_event_flag(realm(), event_key("poltava_won")));
    for (const Scope where : {head, dy(s.house_b), pr(s.provinces[4]), realm()}) {
        CHECK(pred(env, where, kTrigHasGlobalFlag, {key("poltava_won")}));
    }
    CHECK(pred(env, realm(), kTrigHasFlag, {key("poltava_won")}));
    CHECK_FALSE(pred(env, head, kTrigHasFlag, {key("poltava_won")}));

    // The last day of a flag is the day it expires: on the next day it is gone.
    s.world.advance_day();
    CHECK_FALSE(pred(env, head, kTrigHasFlag, {key("hushed_theft")}));
    CHECK(pred(env, realm(), kTrigHasGlobalFlag, {key("poltava_won")}));
}

TEST_CASE("realm triggers: modifiers, cooldowns, has_fired, chain steps and variables") {
    Scenario s;
    const TriggerEnv env = s.env();
    const Scope p0 = pr(s.provinces[0]);
    const Date today = s.world.today();

    CHECK_FALSE(pred(env, p0, kTrigHasModifier, {key("estate_audit")}));
    REQUIRE(s.world.set_event_modifier(Modifier{p0, event_key("estate_audit"), today, today + 30}));
    CHECK(pred(env, p0, kTrigHasModifier, {key("estate_audit")}));
    CHECK_FALSE(pred(env, pr(s.provinces[1]), kTrigHasModifier, {key("estate_audit")}));

    // has_cooldown: an active pause of the event or of its cooldown group; has_fired: a row is kept.
    CHECK_FALSE(pred(env, p0, kTrigHasCooldown, {key("theft_found")}));
    CHECK_FALSE(pred(env, p0, kTrigHasFired, {key("theft_found")}));
    REQUIRE(s.world.set_event_cooldown(Cooldown{p0, event_key("theft_found"), today + 1}));
    CHECK(pred(env, p0, kTrigHasCooldown, {key("theft_found")}));
    CHECK(pred(env, p0, kTrigHasFired, {key("theft_found")}));
    REQUIRE(s.world.set_event_cooldown(Cooldown{p0, event_key("estate") | kCooldownGroupBit, kForever}));
    CHECK(pred(env, p0, kTrigHasCooldown, {key("estate")}));  // the group
    CHECK_FALSE(pred(env, p0, kTrigHasCooldown, {Word{0}}));  // no name
    s.world.advance_day();
    CHECK_FALSE(pred(env, p0, kTrigHasCooldown, {key("theft_found")}));  // `until` is the first free day
    CHECK(pred(env, p0, kTrigHasFired, {key("theft_found")}));           // the row is still there
    CHECK(pred(env, p0, kTrigHasCooldown, {key("estate")}));             // `once` holds forever

    // A running chain counts as fired; its step is a value (0: no chain).
    CHECK(val(env, p0, kValChainStep, {key("theft_story")}) == 0);
    REQUIRE(s.world.set_event_chain(Chain{p0, event_key("theft_story"), 2, today, today}));
    CHECK(val(env, p0, kValChainStep, {key("theft_story")}) == whole(2));
    CHECK(pred(env, p0, kTrigHasFired, {key("theft_story")}));
    CHECK(val(env, pr(s.provinces[1]), kValChainStep, {key("theft_story")}) == 0);

    // Variables: 0 when absent; the global one lives on the realm.
    CHECK(val(env, p0, kValVariable, {key("debt_count")}) == 0);
    REQUIRE(s.world.set_event_var(p0, event_key("debt_count"), Fixed64::from_ratio(7, 2)));
    CHECK(val(env, p0, kValVariable, {key("debt_count")}) == word_from_fixed(Fixed64::from_ratio(7, 2)));
    REQUIRE(s.world.set_event_var(realm(), event_key("levy_year"), Fixed64::from_int(-4)));
    CHECK(val(env, ch(s.head_a), kValGlobalVariable, {key("levy_year")}) == whole(-4));
    CHECK(val(env, ch(s.head_a), kValVariable, {key("levy_year")}) == 0);
}

// ---- chance and time ------------------------------------------------------------------------------------

TEST_CASE("realm triggers: random_chance is a draw of the machine, the evaluator has no data") {
    Scenario s;
    const TriggerResult r = pred_r(s.env(), ch(s.head_a), kTrigRandomChance, {whole(50)});
    CHECK(r.value == 0);
    CHECK_FALSE(r.has_data);
}

TEST_CASE("realm triggers: current year, month and date at the turn of the year") {
    World world{day(1703, 12, 31), 1};
    TriggerEnv env;
    env.world = &world;
    const Scope r = realm();
    CHECK(val(env, r, kValCurrentYear) == whole(1703));
    CHECK(val(env, r, kValCurrentMonth) == whole(12));
    CHECK(val(env, r, kValDate) == whole(day(1703, 12, 31).days()));
    world.advance_day();
    CHECK(val(env, r, kValCurrentYear) == whole(1704));
    CHECK(val(env, r, kValCurrentMonth) == whole(1));
    CHECK(val(env, r, kValDate) == whole(day(1704, 1, 1).days()));
    CHECK(val(env, r, kValDate) - val(env, r, kValDate) == 0);
}

// ---- the province -----------------------------------------------------------------------------------------

TEST_CASE("realm triggers: unrest, its state, control, corruption, recruits, development") {
    Scenario s;
    const TriggerEnv env = s.env();
    const Scope p0 = pr(s.provinces[0]);
    const Scope p1 = pr(s.provinces[1]);
    CHECK(val(env, p0, kValUnrest) == word_from_fixed32(fx32(45)));
    CHECK(val(env, p1, kValUnrest) == word_from_fixed32(fx32(10)));
    CHECK(val(env, p0, kValControl) == word_from_fixed32(fx32(25)));
    CHECK(val(env, p0, kValCorruption) == word_from_fixed32(Fixed32::from_ratio(1, 4)));
    CHECK(val(env, p0, kValRecruitPool) == whole(640));
    CHECK(val(env, p0, kValDevelopment) == whole(4));
    CHECK(val(env, p1, kValDevelopment) == whole(2));
    CHECK(val(env, ch(s.head_a), kValUnrest) == 0);  // not a province

    // The state follows the thresholds of the rules: calm at 0, uprising at 100.
    const auto state_at = [&](int unrest) {
        s.world.economy_columns_mut().unrest[1] = fx32(unrest);
        return val(env, p1, kValUnrestState);
    };
    CHECK(state_at(0) == whole(static_cast<int>(economy::UnrestState::Calm)));
    CHECK(state_at(100) == whole(static_cast<int>(economy::UnrestState::Uprising)));
    for (int u = 0; u <= 100; u += 5) {
        CAPTURE(u);
        CHECK(state_at(u) == whole(static_cast<int>(economy::unrest_state(s.rules.unrest, fx32(u)))));
    }
    // Without the rules the state has no data.
    CHECK_FALSE(val_r(s.env(false), p1, kValUnrestState).has_data);
}

TEST_CASE("realm triggers: population, groups and buildings with and without the economy") {
    Scenario s;
    const TriggerEnv env = s.env();
    const Scope p0 = pr(s.provinces[0]);
    CHECK(val(env, p0, kValPopulation) ==
          whole(static_cast<std::int64_t>(s.world.population_persons(s.provinces[0]))));
    CHECK(val(env, pr(s.provinces[2]), kValPopulation) == whole(12'000));
    const Word nobility = word_from_int(static_cast<int>(economy::Group::Nobility));
    const Word peasants = word_from_int(static_cast<int>(economy::Group::Peasants));
    CHECK(val(env, p0, kValGroupSize, {nobility}) == whole(1'500));
    CHECK(val(env, p0, kValGroupSize, {peasants}) == whole(12'000));
    CHECK(val(env, p0, kValGroupMood, {nobility}) == word_from_fixed32(fx32(-30)));
    CHECK(val(env, p0, kValGroupSize, {word_from_int(6)}) == 0);  // no such group

    // has_building: the registry index -> slot table; the finished building in slot 1 counts.
    CHECK(pred(env, p0, kTrigHasBuilding, {word_from_int(kMillSlot)}));  // empty table: index == slot
    CHECK_FALSE(pred(env, p0, kTrigHasBuilding, {word_from_int(kMillSlot + 1)}));
    CHECK_FALSE(pred(env, p0, kTrigHasBuilding, {word_from_int(0)}));
    CHECK_FALSE(pred(env, pr(s.provinces[1]), kTrigHasBuilding, {word_from_int(kMillSlot)}));
    const std::vector<std::uint8_t> slots{0, 7, kMillSlot};  // registry index 2 is the mill
    TriggerEnv mapped = env;
    mapped.building_slots = slots;
    CHECK(pred(mapped, p0, kTrigHasBuilding, {word_from_int(2)}));
    CHECK_FALSE(pred(mapped, p0, kTrigHasBuilding, {word_from_int(1)}));
    CHECK_FALSE(pred(mapped, p0, kTrigHasBuilding, {word_from_int(9)}));  // outside the table

    // Without the economy: population from the cache in thousands; the rest has no data.
    Scenario bare(false);
    const TriggerEnv none = bare.env();
    const Scope b0 = pr(bare.provinces[0]);
    CHECK(val(none, b0, kValPopulation) == whole(12'000));
    CHECK(val(none, b0, kValDevelopment) == whole(4));
    for (const std::uint16_t code :
         {kValUnrest, kValControl, kValCorruption, kValRecruitPool, kValUnrestState, kValGroupSize}) {
        CAPTURE(code);
        const TriggerResult r = val_r(none, b0, code, {nobility});
        CHECK(r.value == 0);
        CHECK_FALSE(r.has_data);
    }
    CHECK_FALSE(pred_r(none, b0, kTrigHasBuilding, {word_from_int(kMillSlot)}).has_data);
}

// ---- the house in the realm -------------------------------------------------------------------------------

TEST_CASE("realm triggers: player control of a person, a house and a province") {
    Scenario s;
    const TriggerEnv env = s.env();
    const auto controlled = [&](Scope scope) { return pred(env, scope, kTrigIsPlayerControlled); };
    CHECK_FALSE(controlled(dy(s.house_a)));  // nobody plays yet
    Meta meta = s.world.event_state().meta;
    meta.player_count = 1;
    meta.players[0] = s.house_a;
    REQUIRE(s.world.set_event_meta(meta));
    CHECK(controlled(dy(s.house_a)));
    CHECK(controlled(ch(s.head_a)));
    CHECK(controlled(pr(s.provinces[0])));
    CHECK_FALSE(controlled(dy(s.house_b)));
    CHECK_FALSE(controlled(ch(s.head_b)));
    CHECK_FALSE(controlled(pr(s.provinces[3])));
    CHECK_FALSE(controlled(pr(s.provinces[4])));  // nobody's
    CHECK_FALSE(controlled(realm()));
}

TEST_CASE("realm triggers: debt and crown quotas of a house") {
    Scenario s;
    const TriggerEnv env = s.env();
    CHECK(val(env, dy(s.house_a), kValDebt) == 0);
    REQUIRE(s.world.take_loan(s.house_a, 250'050, s.rules.loans).has_value());  // 2500.50 roubles
    CHECK(val(env, dy(s.house_a), kValDebt) == word_from_fixed(Fixed64::from_ratio(250'050, 100)));
    CHECK(val(env, dy(s.house_a), kValDebt) ==
          word_from_fixed(Fixed64::from_ratio(economy::dynasty_debt(s.world, s.house_a), 100)));
    CHECK(val(env, dy(s.house_b), kValDebt) == 0);
    CHECK(val(env, ch(s.head_a), kValDebt) == 0);  // a person is not a house

    const Word any = word_from_int(0);
    const Word money = word_from_int(1);
    const Word recruits = word_from_int(2);
    CHECK_FALSE(pred(env, dy(s.house_a), kTrigHasQuota, {any}));
    CHECK_FALSE(pred(env, dy(s.house_a), kTrigHasQuota, {}));
    // Quotas have no writer in the World yet (E9-T14 assigns them): the row is put in place like the
    // stress generator does (tools/stressgen/economy_generator.cpp).
    auto& quotas = const_cast<economy::EconomyState&>(s.world.economy()).quotas;
    quotas.quota_id.push_back(0);
    quotas.dynasty.push_back(s.house_a);
    quotas.kind.push_back(static_cast<std::uint8_t>(economy::QuotaKind::Recruits));
    quotas.amount.push_back(40);
    quotas.period_start.push_back(day(1703, 1, 1));
    quotas.deadline.push_back(day(1704, 1, 1));
    CHECK(pred(env, dy(s.house_a), kTrigHasQuota, {any}));
    CHECK(pred(env, dy(s.house_a), kTrigHasQuota, {recruits}));
    CHECK_FALSE(pred(env, dy(s.house_a), kTrigHasQuota, {money}));
    CHECK_FALSE(pred(env, dy(s.house_b), kTrigHasQuota, {any}));

    Scenario bare(false);
    CHECK_FALSE(val_r(bare.env(), dy(bare.house_a), kValDebt).has_data);
    CHECK_FALSE(pred_r(bare.env(), dy(bare.house_a), kTrigHasQuota, {any}).has_data);
}

// ---- the lists of the iterators: order and fan-out --------------------------------------------------------

namespace {

std::vector<Scope> listing(const World& world, Scope scope, std::uint16_t code, bool* cut = nullptr) {
    const Catalog catalog = Catalog::standard();
    TriggerEnv env;
    WorldVmHost host(world, catalog, env);
    std::vector<Scope> out;
    const bool c = host.list(scope, code, static_cast<std::uint32_t>(limits::kMaxFanout), out);
    if (cut != nullptr) {
        *cut = c;
    }
    return out;
}

constexpr std::uint16_t kListLivingMember = 1;
constexpr std::uint16_t kListChild = 2;
constexpr std::uint16_t kListSibling = 3;
constexpr std::uint16_t kListOwnedProvince = 4;
constexpr std::uint16_t kListRealmDynasty = 5;

}  // namespace

TEST_CASE(
    "realm iterators: owned provinces are cut at the bound, lowest ids first, in any acquisition order") {
    Scenario s(false);
    // 130 more provinces of house B, acquired in a shuffled order (the index of the World keeps its own).
    std::vector<ProvinceId> extra;
    for (int i = 0; i < 130; ++i) {
        extra.push_back(s.world.create_province(StringId(100 + i), DynastyId{}, fx32(1)));
    }
    for (std::size_t i = 0; i < extra.size(); ++i) {
        s.world.set_province_owner(extra[(i * 37) % extra.size()], s.house_b);
    }
    s.world.set_province_owner(s.provinces[3], DynastyId{});
    s.world.set_province_owner(s.provinces[3], s.house_b);  // moves to the end of the index

    const std::uint32_t bound = Catalog::standard().list_bound(kListOwnedProvince);
    REQUIRE(bound == 128);
    bool cut = false;
    const std::vector<Scope> got = listing(s.world, dy(s.house_b), kListOwnedProvince, &cut);
    CHECK(cut);
    REQUIRE(got.size() == bound);
    std::vector<std::uint32_t> expected{s.provinces[3].value()};
    for (const ProvinceId p : extra) {
        expected.push_back(p.value());
    }
    std::ranges::sort(expected);
    for (std::size_t i = 0; i < got.size(); ++i) {
        CHECK(got[i].kind == ScopeKind::Province);
        CHECK(got[i].id == expected[i]);
    }

    // Under the bound: every province, ascending, not cut.
    const std::vector<Scope> small = listing(s.world, dy(s.house_a), kListOwnedProvince, &cut);
    CHECK_FALSE(cut);
    REQUIRE(small.size() == 3);
    CHECK(small[0] == pr(s.provinces[0]));
    CHECK(small[2] == pr(s.provinces[2]));
    CHECK(listing(s.world, pr(s.provinces[0]), kListOwnedProvince).empty());  // not a dynasty
}

TEST_CASE("realm iterators: living members stop at 256, children and siblings by birth date") {
    Scenario s(false);
    for (int i = 0; i < 260; ++i) {
        (void)s.add(s.house_b, day(1680, 1, 1) + i);
    }
    bool cut = false;
    const std::vector<Scope> members = listing(s.world, dy(s.house_b), kListLivingMember, &cut);
    CHECK(cut);
    REQUIRE(members.size() == 256);
    CHECK(std::ranges::is_sorted(members, {}, &Scope::id));
    CHECK(members.front() == ch(s.head_b));  // the lowest id

    // Children: a younger child with a lower id comes after the elder one (section 5.2: (birth_date, id)).
    const CharacterId younger = s.add(s.house_a, day(1700, 1, 1), s.head_a);
    const CharacterId elder = s.add(s.house_a, day(1695, 1, 1), s.head_a);
    const CharacterId twin = s.add(s.house_a, day(1695, 1, 1), s.head_a);
    const std::vector<Scope> kids = listing(s.world, ch(s.head_a), kListChild, &cut);
    CHECK_FALSE(cut);
    REQUIRE(kids.size() == 3);
    CHECK(kids[0] == ch(elder));
    CHECK(kids[1] == ch(twin));
    CHECK(kids[2] == ch(younger));
    const std::vector<Scope> sibs = listing(s.world, ch(younger), kListSibling);
    REQUIRE(sibs.size() == 2);
    CHECK(sibs[0] == ch(elder));
    CHECK(sibs[1] == ch(twin));
    REQUIRE(s.world.die(elder, CauseOfDeath::OldAge));
    CHECK(listing(s.world, ch(s.head_a), kListChild).size() == 2);  // the living only
}

TEST_CASE("realm iterators: the houses of the realm skip the houses that died out") {
    Scenario s(false);
    const std::vector<Scope> houses = listing(s.world, realm(), kListRealmDynasty);
    REQUIRE(houses.size() == 2);
    CHECK(houses[0] == dy(s.house_a));
    CHECK(houses[1] == dy(s.house_b));
    CHECK(listing(s.world, dy(s.house_a), kListRealmDynasty).empty());
}

// ---- compiled events through the machine and the host over the World -------------------------------------

namespace {

std::string province_event(std::string_view trigger, std::string_view id = "t.prov") {
    return "event \"" + std::string(id) + R"(" {
    scope = province
    type = provincial
    category = economy
    on = monthly_pulse
    )" + std::string(trigger) +
           R"(
    option "ok" { }
}
)";
}

struct Run {
    bool ok = false;
    bool truthy = false;
    std::uint32_t truncated = 0;
};

Run run_trigger(const World& world, const TriggerEnv& env, const Program& program, Scope root) {
    const Catalog catalog = Catalog::standard();
    WorldVmHost host(world, catalog, env);
    pat::core::Pcg32 rng(5, 9);
    host.set_rng(&rng);
    Vm vm;
    RunOptions options;
    options.kind = Kind::Trigger;
    options.root = root;
    const RunResult r = vm.run(program, host, options);
    return Run{r.ok, r.truthy(), r.counters.truncated};
}

}  // namespace

TEST_CASE("realm triggers: a compiled province trigger runs against the World") {
    Scenario s;
    Meta meta = s.world.event_state().meta;
    meta.player_count = 1;
    meta.players[0] = s.house_b;
    REQUIRE(s.world.set_event_meta(meta));
    REQUIRE(s.world.set_event_flag(pr(s.provinces[0]), event_key("hushed_theft")));
    REQUIRE(s.world.set_event_var(pr(s.provinces[0]), event_key("thefts"), Fixed64::from_int(2)));
    REQUIRE(s.world.set_event_chain(
        Chain{pr(s.provinces[0]), event_key("theft_story"), 1, s.world.today(), s.world.today()}));

    const auto compiled = pat::event_fixture::compile_event_files({{"events/t.pd", province_event(R"(trigger {
        unrest >= 40
        control < 30
        corruption > 0.2
        has_flag = hushed_theft
        variable.thefts >= 2
        chain_step = { chain = theft_story  value >= 1 }
        not = { has_global_flag = poltava_won }
        nor = { development < 3  population < 1000 }
        or = { current_month = 5  current_year > 1710 }
        date >= 1703.05.16
        group_mood.nobility < -20
        owner = {
            is_player_controlled = no
            debt < 1
            any_owned_province = { count >= 2  development >= 2 }
            all_owned_province = { population >= 1000 }
        }
        random_chance = 100
    })")}},
                                                                  {},
                                                                  Catalog::standard());
    INFO(compiled.all_errors());
    REQUIRE(compiled.ok());
    const Program& trigger = *compiled.set->events.front().trigger;
    const TriggerEnv env = s.env();

    CHECK(run_trigger(s.world, env, trigger, pr(s.provinces[0])).truthy);
    CHECK_FALSE(run_trigger(s.world, env, trigger, pr(s.provinces[1])).truthy);  // calm, no flag
    // Each part matters.
    s.world.economy_columns_mut().unrest[0] = fx32(39);
    CHECK_FALSE(run_trigger(s.world, env, trigger, pr(s.provinces[0])).truthy);
    s.world.economy_columns_mut().unrest[0] = fx32(40);
    CHECK(run_trigger(s.world, env, trigger, pr(s.provinces[0])).truthy);
    REQUIRE(s.world.set_event_flag(realm(), event_key("poltava_won")));
    CHECK_FALSE(run_trigger(s.world, env, trigger, pr(s.provinces[0])).truthy);
    REQUIRE(s.world.clear_event_flag(realm(), event_key("poltava_won")));
    meta.players[0] = s.house_a;
    REQUIRE(s.world.set_event_meta(meta));
    CHECK_FALSE(run_trigger(s.world, env, trigger, pr(s.provinces[0])).truthy);  // the player's own
    meta.player_count = 0;
    meta.players[0] = DynastyId{};
    REQUIRE(s.world.set_event_meta(meta));
    CHECK(run_trigger(s.world, env, trigger, pr(s.provinces[0])).truthy);
    s.world.set_province_development(s.provinces[1], 1);  // all_ / any_ over the owned provinces
    CHECK_FALSE(run_trigger(s.world, env, trigger, pr(s.provinces[0])).truthy);
}

TEST_CASE("realm iterators: a count beyond the fan-out bound is cut and reported, deterministically") {
    Scenario s(false);
    for (int i = 0; i < 140; ++i) {
        (void)s.world.create_province(StringId(200 + i), s.house_b, fx32(1));
    }
    const auto compiled = pat::event_fixture::compile_event_files({{"events/t.pd", province_event(R"(trigger {
        owner = { any_owned_province = { count >= 128 } }
    })")},
                                                                   {"events/u.pd",
                                                                    province_event(R"(trigger {
        owner = { any_owned_province = { count >= 129 } }
    })",
                                                                                   "t.prov2")}},
                                                                  {},
                                                                  Catalog::standard());
    INFO(compiled.all_errors());
    REQUIRE(compiled.ok());
    REQUIRE(compiled.set->events.size() == 2);
    const TriggerEnv env = s.env();
    const Scope root = pr(s.provinces[3]);
    const auto& events = compiled.set->events;
    for (const EventDef& e : events) {
        const Run first = run_trigger(s.world, env, *e.trigger, root);
        const Run again = run_trigger(s.world, env, *e.trigger, root);
        CHECK(first.ok);
        CHECK(first.truncated == 1);  // 141 provinces, the bound is 128
        CHECK(first.truthy == again.truthy);
        CHECK(first.truncated == again.truncated);
    }
    // 128 elements reach the iterator: "at least 128" holds, "at least 129" cannot.
    std::vector<bool> results;
    for (const EventDef& e : events) {
        results.push_back(run_trigger(s.world, env, *e.trigger, root).truthy);
    }
    CHECK(std::ranges::count(results, true) == 1);
}
