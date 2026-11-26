#include "pat/content/army_content.hpp"
#include "pat/data/army_schemas.hpp"
#include "pat/data/content_schemas.hpp"
#include "pat/loc/localizer.hpp"
#include "pat/sim/army/text.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <string_view>

#include "army_fixture.hpp"

using namespace pat;
namespace {
using court_fixture::position_after;
using court_fixture::replace_once;
using court_fixture::shipped_text;
content::ArmyContentResult load_with(std::string_view path, const std::string& text) {
    auto source = army_fixture::shipped_source();
    source.add(std::string(path), text);
    return content::load_army_content(source);
}
bool has_error(const std::vector<data::LoadError>& errors, std::string_view text) {
    return std::ranges::any_of(
        errors, [&](const auto& e) { return e.diagnostic.message.find(text) != std::string::npos; });
}
}  // namespace
TEST_CASE("army rules: shipped typed rules equal valid defaults including all four branches and siege") {
    const auto defaults = sim::army::ArmyRules::defaults();
    const auto checked = sim::army::validate(defaults);
    if (!checked)
        INFO(checked.error().field << ": " << checked.error().message);
    REQUIRE(checked.has_value());
    auto source = army_fixture::shipped_source();
    const auto loaded = content::load_army_content(source);
    if (!loaded)
        INFO(content::format_errors(loaded.error()));
    REQUIRE(loaded.has_value());
    CHECK(loaded.value().rules == defaults);
    CHECK(loaded.value().rules_hash == defaults.hash());
    CHECK(defaults.branches[2].siege_value == 4000);
    CHECK(defaults.branches[2].speed_kmd == 12);
    CHECK(defaults.seasons[3].move_pm == 550);
    CHECK(defaults.counter[0][1][0] == 1250);
    CHECK(defaults.counter[1][1][0] == 850);
    CHECK(defaults.siege_breach_wall == 600);
    CHECK(defaults.siege_defence[2] == 900);
}
TEST_CASE("army rules: canonical hash covers every scalar branch terrain season matrix and table") {
    const auto defaults = sim::army::ArmyRules::defaults();
    const auto original = defaults.hash();
#define PAT_ARMY_HASH_TEST(group, name, def, lo, hi)                                                         \
    {                                                                                                        \
        auto changed = defaults;                                                                             \
        ++changed.group##_##name;                                                                            \
        CHECK(changed.hash() != original);                                                                   \
    }
    PAT_ARMY_BALANCE_FIELDS(PAT_ARMY_HASH_TEST)
#undef PAT_ARMY_HASH_TEST
    using B = sim::army::BranchRules;
    for (const auto member : {&B::cv,
                              &B::speed_kmd,
                              &B::upkeep_kop,
                              &B::equip_kop,
                              &B::vuln,
                              &B::need,
                              &B::siege_value,
                              &B::train_days}) {
        for (std::size_t i = 0; i < 4; ++i) {
            auto changed = defaults;
            ++(changed.branches[i].*member);
            CHECK(changed.hash() != original);
        }
    }
    for (std::size_t i = 0; i < 6; ++i) {
        using T = sim::army::TerrainRules;
        for (const auto member : {&T::move_pm, &T::defence_pm, &T::supply_pm}) {
            auto changed = defaults;
            ++(changed.terrains[i].*member);
            CHECK(changed.hash() != original);
        }
        auto changed = defaults;
        changed.terrains[i].counter_class = changed.terrains[i].counter_class == sim::army::CounterClass::Open
                                                ? sim::army::CounterClass::Rough
                                                : sim::army::CounterClass::Open;
        CHECK(changed.hash() != original);
    }
    for (std::size_t i = 0; i < 12; ++i) {
        auto changed = defaults;
        ++changed.seasons[i].move_pm;
        CHECK(changed.hash() != original);
        changed = defaults;
        ++changed.seasons[i].supply_pm;
        CHECK(changed.hash() != original);
    }
    for (std::size_t c = 0; c < 2; ++c)
        for (std::size_t i = 0; i < 4; ++i)
            for (std::size_t j = 0; j < 4; ++j) {
                auto changed = defaults;
                ++changed.counter[c][i][j];
                CHECK(changed.hash() != original);
            }
    for (std::size_t i = 0; i < 11; ++i) {
        auto changed = defaults;
        ++changed.sigmoid[i];
        CHECK(changed.hash() != original);
    }
    for (std::size_t i = 0; i < 6; ++i) {
        auto changed = defaults;
        ++changed.siege_defence[i];
        CHECK(changed.hash() != original);
        changed = defaults;
        ++changed.fortress_upkeep[i];
        CHECK(changed.hash() != original);
    }
    for (std::size_t i = 0; i < 4; ++i) {
        auto changed = defaults;
        ++changed.front_enemy_base[i];
        CHECK(changed.hash() != original);
        changed = defaults;
        ++changed.arrears[i].morale;
        CHECK(changed.hash() != original);
        changed = defaults;
        ++changed.arrears[i].desert;
        CHECK(changed.hash() != original);
    }
}
TEST_CASE("army rules: formatting file order and labelled block order do not change the typed hash") {
    auto text = shipped_text(army_fixture::kBalance);
    const auto start = text.find("    branch \"infantry\"");
    const auto end = text.find('\n', start) + 1;
    REQUIRE(start != std::string::npos);
    const auto line = text.substr(start, end - start);
    text.erase(start, end - start);
    const auto after = text.find('\n', text.find("    branch \"irregular\"")) + 1;
    text.insert(after, line);
    text += "\n# harmless formatting and source positions\n";
    const auto loaded = load_with(army_fixture::kBalance, text);
    if (!loaded)
        INFO(content::format_errors(loaded.error()));
    REQUIRE(loaded.has_value());
    CHECK(loaded.value().rules_hash == sim::army::ArmyRules::defaults().hash());
    auto source = army_fixture::shipped_source();
    const auto files = source.list_files();
    REQUIRE(files.has_value());
    data::MemorySource reversed;
    for (auto it = files.value().rbegin(); it != files.value().rend(); ++it) {
        const auto body = source.read_file(*it);
        REQUIRE(body.has_value());
        reversed.add(*it, body.value());
    }
    const auto reordered = content::load_army_content(reversed);
    REQUIRE(reordered.has_value());
    CHECK(reordered.value().rules_hash == loaded.value().rules_hash);
}
TEST_CASE("army rules: a modded number reaches recruitment cost inputs and changes rules hash") {
    auto base = army_fixture::shipped_source();
    data::MemorySource mod("army mod");
    auto text = shipped_text(army_fixture::kBalance);
    replace_once(text, "equip_kop = 300", "equip_kop = 350");
    mod.add(std::string(army_fixture::kBalance), text);
    const data::ContentSource* roots[] = {&base, &mod};
    const auto loaded = content::load_army_content(roots);
    REQUIRE(loaded.has_value());
    CHECK(loaded.value().rules_hash != sim::army::ArmyRules::defaults().hash());
    // Analytical check of the recruitment input (T06 implements the complete command).
    CHECK(std::int64_t{1500} * loaded.value().rules.branches[0].equip_kop == 525000);
    CHECK(std::int64_t{1500} * sim::army::ArmyRules::defaults().branches[0].equip_kop == 450000);
}
TEST_CASE("army rules: malformed schema and semantic values produce source-located diagnostics") {
    auto text = shipped_text(army_fixture::kBalance);
    std::string expected, anchor, key;
    SUBCASE("unknown field") {
        replace_once(text, "slow_share_pm = 50", "slow_shrae_pm = 50");
        expected = "slow_shrae_pm";
    }
    SUBCASE("representation overflow") {
        replace_once(text, "speed_kmd = 20", "speed_kmd = 2147483648");
        expected = "2147483648";
    }
    SUBCASE("zero speed") {
        replace_once(text, "speed_kmd = 20", "speed_kmd = 0");
        expected = "branch.infantry.speed_kmd";
        anchor = "branch \"infantry\"";
        key = "speed_kmd";
    }
    SUBCASE("unknown branch") {
        replace_once(text, "branch \"artillery\"", "branch \"cannons\"");
        expected = "unknown army branch";
    }
    SUBCASE("missing branch") {
        const auto a = text.find("    branch \"artillery\"");
        REQUIRE(a != std::string::npos);
        text.erase(a, text.find('\n', a) - a + 1);
        expected = "missing army branch 'artillery'";
    }
    SUBCASE("missing terrain") {
        const auto a = text.find("    terrain \"swamp\"");
        REQUIRE(a != std::string::npos);
        text.erase(a, text.find('\n', a) - a + 1);
        expected = "missing army terrain 'swamp'";
    }
    SUBCASE("duplicate month") {
        replace_once(text, "month = 12", "month = 11");
        expected = "month 11 is repeated";
    }
    SUBCASE("short sigmoid") {
        replace_once(text, "998 999 1000]", "998 999]");
        expected = "exactly 11";
        anchor = "battle =";
        key = "sigmoid";
    }
    SUBCASE("reversed sigmoid") {
        replace_once(text, "500 683 823", "500 900 823");
        expected = "sigmoid must not decrease";
        anchor = "battle =";
        key = "sigmoid";
    }
    SUBCASE("asymmetric probability clamp") {
        replace_once(text, "p_max = 970", "p_max = 971");
        expected = "symmetric";
        anchor = "battle =";
        key = "p_max";
    }
    SUBCASE("zero counter") {
        replace_once(text, "infantry = 1250", "infantry = 0");
        expected = "counter.open.cavalry.infantry";
        anchor = "cavalry = { infantry";
        key = "infantry";
    }
    const auto loaded = load_with(army_fixture::kBalance, text);
    REQUIRE_FALSE(loaded.has_value());
    INFO(content::format_errors(loaded.error()));
    CHECK(has_error(loaded.error(), expected));
    for (const auto& error : loaded.error()) {
        CHECK(error.file == army_fixture::kBalance);
        CHECK(error.diagnostic.pos.line > 0);
        CHECK(error.diagnostic.pos.column > 0);
    }
    if (!anchor.empty()) {
        const auto pos = position_after(text, anchor, key);
        CHECK(loaded.error().front().diagnostic.pos == pos);
    }
}
TEST_CASE("army rules: cross-field checks prevent impossible limits probabilities and branch shares") {
    auto rules = sim::army::ArmyRules::defaults();
    SUBCASE("world owner count") {
        rules.limits_world = 2;
    }
    SUBCASE("army size") {
        rules.limits_max_size = 100;
    }
    SUBCASE("overlapping front bands") {
        rules.front_hysteresis = 4000;
    }
    SUBCASE("rebel shares") {
        rules.rebels_share_max = 1;
    }
    SUBCASE("rebel composition") {
        rules.rebels_infantry_pm = 950;
    }
    SUBCASE("rebel quality") {
        rules.rebels_quality_base = 99;
    }
    SUBCASE("levy cooldown") {
        rules.levy_cooldown_days = 1;
    }
    SUBCASE("commander fate") {
        rules.battle_kill_loser_pm = 999;
    }
    SUBCASE("arrears penalties") {
        rules.arrears[2].morale = 0;
    }
    SUBCASE("defence order") {
        rules.siege_defence[3] = 10;
    }
    const auto checked = sim::army::validate(rules);
    REQUIRE_FALSE(checked.has_value());
    CHECK_FALSE(checked.error().field.empty());
    CHECK_FALSE(checked.error().message.empty());
}
TEST_CASE("army military: synthetic setup references existing provinces and resolves no World ids") {
    auto source = army_fixture::shipped_source();
    const auto loaded = content::load_army_content(source);
    REQUIRE(loaded.has_value());
    const auto& setup = loaded.value().military;
    CHECK(setup.capital == "p0001");
    CHECK(setup.provinces.size() == 20);
    CHECK(setup.armies.size() == 2);
    REQUIRE(setup.armies.size() == 2);
    CHECK(setup.armies.front().id == "crown_field");
    CHECK(setup.armies.front().comp[2] == 100);
    CHECK(setup.armies.back().tag == content::MilitaryTag::Guard);
    CHECK(setup.armies.back().province == setup.capital);
    CHECK(setup.armies.back().commander_office == "guards_colonel");
    CHECK(setup.front.id == "north");
    CHECK(setup.front.state == content::MilitaryFrontState::Tension);
    CHECK(std::ranges::count_if(setup.provinces, [](const auto& p) { return p.has_fortress; }) == 3);
}
TEST_CASE("army military: bad references ownership sizes fortress and province coverage are rejected") {
    auto text = shipped_text(army_fixture::kMilitary);
    std::string expected;
    SUBCASE("unknown capital") {
        replace_once(text, "capital = province:p0001", "capital = province:p9999");
        expected = "p9999";
    }
    SUBCASE("duplicate province") {
        replace_once(text, "id = province:p0002", "id = province:p0001");
        expected = "duplicate military province";
    }
    SUBCASE("missing province") {
        const auto a = text.find("    province { id = province:p0002");
        REQUIRE(a != std::string::npos);
        text.erase(a, text.find('\n', a) - a + 1);
        expected = "missing military terrain";
    }
    SUBCASE("missing dynasty") {
        replace_once(text, "owner = crown tag = regular", "owner = dynasty tag = regular");
        expected = "dynasty reference is required";
    }
    SUBCASE("crown with dynasty") {
        replace_once(
            text, "owner = crown tag = regular", "owner = crown dynasty = dynasty:golitsyn tag = regular");
        expected = "dynasty reference is required";
    }
    SUBCASE("guard outside capital") {
        replace_once(text, "tag = guard province = province:p0001", "tag = guard province = province:p0002");
        expected = "guard must belong";
    }
    SUBCASE("rebel tag") {
        replace_once(text, "owner = crown tag = regular", "owner = rebels tag = regular");
        expected = "insurgents tag";
    }
    SUBCASE("too few troops") {
        replace_once(text, "infantry = 1500 cavalry = 100", "infantry = 1 cavalry = 0");
        expected = "initial army size";
    }
    SUBCASE("fortress range") {
        replace_once(text, "level = 2", "level = 6");
        expected = "6";
    }
    SUBCASE("unknown front") {
        replace_once(text, "id = north", "id = south");
        expected = "single north front";
    }
    const auto loaded = load_with(army_fixture::kMilitary, text);
    REQUIRE_FALSE(loaded.has_value());
    INFO(content::format_errors(loaded.error()));
    CHECK(has_error(loaded.error(), expected));
    for (const auto& error : loaded.error()) {
        CHECK(error.file == army_fixture::kMilitary);
        CHECK(error.diagnostic.pos.line > 0);
    }
}
TEST_CASE("army content: optional kinds preserve existing definition ids and compatibility content") {
    auto base = army_fixture::shipped_source(false), with_army = army_fixture::shipped_source();
    const auto plain = data::load_content(base, data::content_schemas());
    REQUIRE(plain.ok());
    const auto loaded = data::load_content(with_army, data::content_schemas());
    REQUIRE(loaded.ok());
    CHECK(loaded.registry.content_hash() != plain.registry.content_hash());
    CHECK(loaded.registry.content_hash_excluding(data::kArmyKinds) == plain.registry.content_hash());
    for (const auto* kind : {"province", "dynasty", "skill", "trait"}) {
        const auto* table = plain.registry.table(kind);
        REQUIRE(table != nullptr);
        for (const auto& object : table->objects()) {
            const auto* current = loaded.registry.find(kind, object.object.id);
            REQUIRE(current != nullptr);
            CHECK(current->id == object.id);
        }
    }
    const auto missing = content::load_army_content(base);
    REQUIRE_FALSE(missing.has_value());
    CHECK(has_error(missing.error(), "no army_rules object"));
}
TEST_CASE("army localization: every reserved denial and presentation key is Russian and nonempty") {
    const data::DirectorySource root{std::string(PATRIARCH_DATA_DIR)};
    const auto localized = loc::load_localization(root);
    REQUIRE(localized.ok());
    for (const auto& denial : sim::army::kDenialTexts) {
        CHECK(sim::army::denial_key(denial.code) == denial.key);
        CHECK(localized.localizer.contains("ru", denial.key));
        std::string rendered;
        CHECK(localized.localizer.format_to(rendered, denial.key).ok());
        CHECK_FALSE(rendered.empty());
    }
    CHECK(sim::army::denial_key(0).empty());
    CHECK(sim::army::denial_key(10).empty());
    CHECK(sim::army::denial_key(41).empty());
    using namespace sim::army;
    const auto check_key = [&](std::string_view key) {
        REQUIRE_FALSE(key.empty());
        std::string rendered;
        CHECK(localized.localizer.contains("ru", key));
        CHECK(localized.localizer.format_to(rendered, key).ok());
        CHECK_FALSE(rendered.empty());
    };
    CHECK(branch_key(Branch::Infantry) == "army.branch.infantry");
    check_key(branch_key(Branch::Infantry));
    CHECK(branch_key(Branch::Cavalry) == "army.branch.cavalry");
    check_key(branch_key(Branch::Cavalry));
    CHECK(branch_key(Branch::Artillery) == "army.branch.artillery");
    check_key(branch_key(Branch::Artillery));
    CHECK(branch_key(Branch::Irregular) == "army.branch.irregular");
    check_key(branch_key(Branch::Irregular));
    CHECK(branch_key(static_cast<Branch>(255)).empty());
    CHECK(terrain_key(Terrain::Plain) == "army.terrain.plain");
    check_key(terrain_key(Terrain::Plain));
    CHECK(terrain_key(Terrain::Steppe) == "army.terrain.steppe");
    check_key(terrain_key(Terrain::Steppe));
    CHECK(terrain_key(Terrain::Forest) == "army.terrain.forest");
    check_key(terrain_key(Terrain::Forest));
    CHECK(terrain_key(Terrain::Swamp) == "army.terrain.swamp");
    check_key(terrain_key(Terrain::Swamp));
    CHECK(terrain_key(Terrain::Hills) == "army.terrain.hills");
    check_key(terrain_key(Terrain::Hills));
    CHECK(terrain_key(Terrain::Mountain) == "army.terrain.mountain");
    check_key(terrain_key(Terrain::Mountain));
    CHECK(terrain_key(static_cast<Terrain>(255)).empty());
    CHECK(tag_key("regular") == "army.tag.regular");
    check_key(tag_key("regular"));
    CHECK(tag_key("levy") == "army.tag.levy");
    check_key(tag_key("levy"));
    CHECK(tag_key("guard") == "army.tag.guard");
    check_key(tag_key("guard"));
    CHECK(tag_key("insurgents") == "army.tag.insurgents");
    check_key(tag_key("insurgents"));
    CHECK(tag_key("unknown").empty());
    CHECK(front_state_key("peace") == "army.front.peace");
    check_key(front_state_key("peace"));
    CHECK(front_state_key("tension") == "army.front.tension");
    check_key(front_state_key("tension"));
    CHECK(front_state_key("war") == "army.front.war");
    check_key(front_state_key("war"));
    CHECK(front_state_key("truce") == "army.front.truce");
    check_key(front_state_key("truce"));
    CHECK(front_state_key("unknown").empty());
    CHECK(siege_phase_key("investing") == "army.siege.investing");
    check_key(siege_phase_key("investing"));
    CHECK(siege_phase_key("breached") == "army.siege.breached");
    check_key(siege_phase_key("breached"));
    CHECK(siege_phase_key("starving") == "army.siege.starving");
    check_key(siege_phase_key("starving"));
    CHECK(siege_phase_key("unknown").empty());
    check_key(kNorthFrontKey);
    check_key(kInitialCrownGuardKey);
    check_key(kInitialCrownFieldKey);
    check_key(kTermCompositionKey);
    check_key(kTermQualityKey);
    check_key(kTermExperienceKey);
    check_key(kTermMoraleKey);
    check_key(kTermSupplyKey);
    check_key(kTermTerrainKey);
    check_key(kTermCommanderKey);
    check_key(kTermCounterKey);
    check_key(kTermFortressKey);
    check_key(kTermSeasonKey);
    check_key(kTermRoadKey);
    check_key(kTermUpkeepKey);
    check_key(kTermLossesKey);
    check_key(kTermPressureKey);
    check_key(kTermWallKey);
    check_key(kTermProgressKey);
    check_key(kTermStoresKey);
    check_key(kUiListKey);
    check_key(kUiDetailsKey);
    check_key(kUiCommanderKey);
    check_key(kUiFrontsKey);
    check_key(kUiBattleReportKey);
    check_key(kUiSiegeKey);
    check_key(kUiAssaultKey);
    check_key(kUiRaiseKey);
    check_key(kUiLevyKey);
    check_key(kUiDisbandKey);
    check_key(kUiSendToFrontKey);
    check_key(kUiRecallKey);
}
