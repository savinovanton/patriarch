#include "pat/game/province_view.hpp"
#include "pat/game/tooltip_view.hpp"
#include "pat/game/world_view.hpp"

#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <string>

#include "ui_loc_fixture.hpp"

namespace {

using namespace pat;
using namespace sim;
using namespace sim::economy;
using core::Fixed32;
using core::StringId;

struct Holding {
    World world{Date::from_ymd(1703, 1, 1), 23};
    DynastyId dynasty;
    ProvinceId province;
    CharacterId head;

    explicit Holding(bool economy = true) {
        dynasty = world.create_dynasty(StringId(1));
        REQUIRE(world.enable_dynasty_mode());
        if (economy) {
            REQUIRE(world.enable_economy());
        }
        province = world.create_province(StringId(2), dynasty, Fixed32::one());
        CharacterInit init;
        init.dynasty = dynasty;
        init.birth_date = Date::from_ymd(1680, 1, 1);
        head = world.create_character(init);
        REQUIRE(world.set_dynasty_head(dynasty, head));
    }
};

void check_unavailable_actions(const ui::ProvinceSnapshot& snapshot) {
    CHECK(snapshot.garrison == "—");
    REQUIRE(snapshot.garrison_explanation.lines.size() == 1);
    CHECK_FALSE(snapshot.garrison_explanation.lines.front().empty());
    CHECK(snapshot.garrison_explanation.lines.front().find("[[") == std::string::npos);
    CHECK_FALSE(snapshot.manage_available);
    REQUIRE(snapshot.manage_explanation.lines.size() == 1);
    CHECK_FALSE(snapshot.manage_explanation.lines.front().empty());
    CHECK(snapshot.manage_explanation.lines.front().find("[[") == std::string::npos);
    CHECK(snapshot.manage_explanation.tone == ui::TipTone::Warning);
}

}  // namespace

TEST_CASE("province view: recorded state and current-rate income are read-only") {
    const auto& loc = test::ui_localizer();
    const auto rules = EconomyRules::defaults();
    Holding holding;
    auto columns = holding.world.economy_columns_mut();
    columns.control[holding.province.index()] = Fixed32::from_ratio(125, 2);
    columns.corruption[holding.province.index()] = Fixed32::from_ratio(1, 4);
    columns.income_last_month[holding.province.index()] = 12'345;
    const auto before = holding.world.compute_hash();
    const auto snapshot = game::province_snapshot(loc, holding.world, holding.province, rules);
    REQUIRE(snapshot.economy.available);
    CHECK(snapshot.economy.id == holding.province.value());
    CHECK(snapshot.economy.population.value == 1000);
    CHECK(snapshot.order == "62,5 / 100");
    CHECK(snapshot.order_percent == 63);
    CHECK(snapshot.state == loc.format("ui.province_window.view.state.no_administrator"));
    CHECK(snapshot.state_detail == "Коррупция: 25,0 %");
    CHECK(snapshot.state_explanation == snapshot.economy.corruption.explanation);
    CHECK(snapshot.income_change.value == snapshot.economy.projected_income.value - 12'345);
    CHECK(snapshot.subtitle == loc.format("ui.province_window.view.subtitle.dynasty",
                                          {loc::Arg("name", game::dynasty_name(holding.dynasty))}));
    check_unavailable_actions(snapshot);
    CHECK(holding.world.compute_hash() == before);
}

TEST_CASE("province view: order track stays within display bounds") {
    const auto& loc = test::ui_localizer();
    const auto rules = EconomyRules::defaults();
    Holding holding;
    for (const auto points : std::array<int, 4>{-3, 0, 100, 105}) {
        holding.world.economy_columns_mut().control[holding.province.index()] = Fixed32::from_int(points);
        const auto snapshot = game::province_snapshot(loc, holding.world, holding.province, rules);
        CHECK(snapshot.order_percent == (points <= 0 ? 0 : 100));
    }
}

TEST_CASE("province view: income difference has exact additive signed terms") {
    const auto& loc = test::ui_localizer();
    auto rules = EconomyRules::defaults();
    rules.tax.head_tax_kop_year.fill(0);
    rules.tax.head_tax_kop_year[index_of(Group::Peasants)] = 1200;
    rules.production.base_price_kop.fill(0);
    Holding holding;
    const auto projected =
        game::province_economy_snapshot(loc, holding.world, holding.province, rules).projected_income.value;
    REQUIRE(projected >= 125);
    for (const std::int64_t difference : std::array<std::int64_t, 3>{-125, 0, 125}) {
        const auto recorded = projected - difference;
        holding.world.economy_columns_mut().income_last_month[holding.province.index()] = recorded;
        const auto before = holding.world.compute_hash();
        const auto snapshot = game::province_snapshot(loc, holding.world, holding.province, rules);
        CHECK(snapshot.income_change.value == difference);
        CHECK(snapshot.income_change.unit == ui::EconomyUnit::Kopecks);
        const auto& explanation = snapshot.income_change.explanation;
        CHECK(explanation.label == loc.format("ui.province_window.view.income_change"));
        CHECK(explanation.note == loc.format("ui.province_window.view.income_change.note"));
        CHECK(explanation.value == game::format_money(loc, difference, true));
        REQUIRE(explanation.terms.size() == 2);
        CHECK(explanation.terms[0].value == game::format_money(loc, projected, true));
        CHECK(explanation.terms[0].terms == snapshot.economy.projected_income.explanation.terms);
        CHECK(explanation.terms[1].label ==
              loc.format("ui.province_window.view.income_change.subtract_recorded"));
        CHECK(explanation.terms[1].value == game::format_money(loc, -recorded, true));
        CHECK(explanation.terms[1].terms.empty());
        CHECK(holding.world.compute_hash() == before);
    }
}

TEST_CASE("province view: content names and an assigned administrator are preserved") {
    const auto& loc = test::ui_localizer();
    const auto rules = EconomyRules::defaults();
    Holding holding;
    REQUIRE(holding.world.set_province_administrator(holding.province, holding.head));
    game::EconomyViewOptions options;
    options.province_name = [](ProvinceId) { return "Вотчина из контента"; };
    options.dynasty_name = [](DynastyId) { return "Тестовы"; };
    options.character_name = [](CharacterId) { return "Иван Иванович Тестов"; };
    const auto before = holding.world.compute_hash();
    const auto snapshot = game::province_snapshot(loc, holding.world, holding.province, rules, options);
    CHECK(snapshot.economy.name == "Вотчина из контента");
    CHECK(snapshot.economy.owner == "Тестовы");
    CHECK(snapshot.subtitle == "Род: Тестовы");
    CHECK(snapshot.owner_name == "Иван Иванович Тестов");
    CHECK(snapshot.owner_detail == "Род Тестовы · 23 года");
    CHECK(snapshot.owner_portrait);
    REQUIRE(snapshot.economy.has_administrator);
    CHECK(snapshot.economy.administrator_id == holding.head.value());
    CHECK(snapshot.state == "Управитель: Иван Иванович Тестов");
    CHECK(snapshot.state_explanation == snapshot.economy.corruption.explanation);
    CHECK(holding.world.compute_hash() == before);
}

TEST_CASE("province view: empty callbacks use existing fallback names and dead heads have no portrait") {
    const auto& loc = test::ui_localizer();
    const auto rules = EconomyRules::defaults();
    Holding holding;
    REQUIRE(holding.world.set_province_administrator(holding.province, holding.head));
    game::EconomyViewOptions options;
    options.province_name = [](ProvinceId) { return std::string(); };
    options.dynasty_name = [](DynastyId) { return std::string(); };
    options.character_name = [](CharacterId) { return std::string(); };
    const auto snapshot = game::province_snapshot(loc, holding.world, holding.province, rules, options);
    CHECK(snapshot.economy.name == "Провинция 1");
    CHECK(snapshot.economy.owner == game::dynasty_name(holding.dynasty));
    CHECK(snapshot.owner_name == game::character_snapshot(loc, holding.world, holding.head).full_name());
    CHECK(snapshot.economy.administrator == snapshot.owner_name);
    CHECK(snapshot.owner_portrait);

    REQUIRE(holding.world.die(holding.head, CauseOfDeath::Illness));
    int character_name_calls = 0;
    options.character_name = [&character_name_calls](CharacterId) {
        ++character_name_calls;
        return "Dead character must not be queried";
    };
    const auto before = holding.world.compute_hash();
    const auto no_head = game::province_snapshot(loc, holding.world, holding.province, rules, options);
    CHECK_FALSE(no_head.owner_portrait);
    CHECK(no_head.owner_name == game::dynasty_name(holding.dynasty));
    CHECK(no_head.owner_detail == loc.format("ui.province_window.view.owner.no_head"));
    CHECK_FALSE(no_head.economy.has_administrator);
    CHECK(no_head.state == loc.format("ui.province_window.view.state.no_administrator"));
    CHECK(character_name_calls == 0);
    CHECK(holding.world.compute_hash() == before);
}

TEST_CASE("province view: valid provinces keep identity without economy and invalid ids stay empty") {
    const auto& loc = test::ui_localizer();
    const auto rules = EconomyRules::defaults();
    Holding holding(false);
    game::EconomyViewOptions options;
    options.province_name = [](ProvinceId) { return "Владение без экономики"; };
    const auto before = holding.world.compute_hash();
    const auto snapshot = game::province_snapshot(loc, holding.world, holding.province, rules, options);
    CHECK_FALSE(snapshot.economy.available);
    CHECK(snapshot.economy.id == holding.province.value());
    CHECK(snapshot.economy.name == "Владение без экономики");
    CHECK(snapshot.economy.owner == game::dynasty_name(holding.dynasty));
    CHECK(snapshot.owner_name == game::character_snapshot(loc, holding.world, holding.head).full_name());
    CHECK(snapshot.owner_portrait);
    CHECK(snapshot.order == "—");
    CHECK(snapshot.order_percent == 0);
    CHECK(snapshot.state == loc.format("ui.province_window.view.state.unavailable"));
    REQUIRE(snapshot.state_explanation.lines.size() == 1);
    CHECK(snapshot.state_explanation.lines[0] == loc.format("ui.province_window.view.economy.unavailable"));
    CHECK(snapshot.income_change.explanation.value == "—");
    check_unavailable_actions(snapshot);

    for (const auto id : std::array<ProvinceId, 2>{ProvinceId(), ProvinceId(900)}) {
        const auto invalid = game::province_snapshot(loc, holding.world, id, rules, options);
        CHECK_FALSE(invalid.economy.available);
        CHECK(invalid.economy.name.empty());
        CHECK(invalid.economy.owner.empty());
        CHECK(invalid.subtitle.empty());
        CHECK(invalid.owner_name.empty());
        CHECK(invalid.state.empty());
        CHECK(invalid.manage_explanation.lines.empty());
    }
    CHECK(holding.world.compute_hash() == before);
}

TEST_CASE("province view: ownership kind supplies factual subtitle and no dynasty portrait") {
    const auto& loc = test::ui_localizer();
    const auto rules = EconomyRules::defaults();
    Holding holding;
    struct OwnerCase {
        OwnerKind kind;
        const char* key;
    };
    for (const auto& entry : std::array<OwnerCase, 4>{{{OwnerKind::Crown, "crown"},
                                                       {OwnerKind::Church, "church"},
                                                       {OwnerKind::Free, "free"},
                                                       {OwnerKind::Unowned, "none"}}}) {
        REQUIRE(holding.world.set_province_owner_kind(holding.province, entry.kind));
        const auto before = holding.world.compute_hash();
        const auto snapshot = game::province_snapshot(loc, holding.world, holding.province, rules);
        CHECK(snapshot.subtitle == loc.format("ui.province_window.view.subtitle." + std::string(entry.key)));
        CHECK(snapshot.owner_name == loc.format("ui.province_window.view.owner." + std::string(entry.key)));
        CHECK_FALSE(snapshot.owner_portrait);
        CHECK_FALSE(snapshot.economy.has_administrator);
        CHECK(snapshot.state == loc.format("ui.province_window.view.state.no_administrator"));
        CHECK(holding.world.compute_hash() == before);
    }
}
