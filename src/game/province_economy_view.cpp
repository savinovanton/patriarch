#include "pat/game/economy_view.hpp"
#include "pat/game/tooltip_view.hpp"
#include "pat/sim/economy/modifiers.hpp"
#include "pat/sim/economy/mood.hpp"
#include "pat/sim/economy/taxes.hpp"
#include "pat/sim/economy/unrest.hpp"

#include <array>
#include <string_view>
#include <utility>

namespace pat::game {

using namespace sim::economy;

namespace {

using loc::Arg;

constexpr std::array<std::string_view, kGroupCount> kGroupNames{
    "nobility", "clergy", "townsfolk", "peasants", "cossacks", "natives"};

std::string group_name(const loc::Localizer& loc, std::size_t group) {
    return loc.format("economy.unrest." + std::string(kGroupNames[group]));
}

std::string province_owner(const loc::Localizer& loc,
                           const sim::World& world,
                           sim::ProvinceId province,
                           const EconomyViewOptions& options) {
    const auto kind = static_cast<OwnerKind>(world.economy().provinces.owner_kind[province.index()]);
    if (kind == OwnerKind::Dynasty) {
        const auto dynasty = world.provinces().owner[province.index()];
        return options.dynasty_name
                   ? options.dynasty_name(dynasty)
                   : loc.format("ui.treasury.owner.dynasty", {Arg("id", dynasty.index() + 1)});
    }
    switch (kind) {
        case OwnerKind::Crown:
            return loc.format("ui.treasury.owner.crown");
        case OwnerKind::Church:
            return loc.format("ui.province.owner.church");
        case OwnerKind::Free:
            return loc.format("ui.province.owner.free");
        default:
            return loc.format("ui.province.owner.none");
    }
}

ui::EconomyNumber projected_owner_income(const loc::Localizer& loc,
                                         const sim::World& world,
                                         sim::ProvinceId province,
                                         const EconomyRules& rules,
                                         const ProductionInput& production_input) {
    const auto production = compute_province_production(rules, production_input);
    const auto input = tax_input(world, rules, province);
    const auto income = compute_province_income(rules, input, production.holding_income_kop);
    auto out = recorded_economy_value(loc, ui::EconomyUnit::Kopecks, 0, "ui.province.projected_income");
    out.explanation.terms.front().label = loc.format("ui.economy.computed");
    if (!income_recipient(world, province)) {
        out.explanation.terms.front().label = loc.format("ui.province.no_income_recipient");
        return out;
    }
    out.value = income.owner_total();
    out.explanation.value = format_money(loc, out.value);
    out.explanation.terms.clear();
    for (std::size_t g = 0; g < kGroupCount; ++g) {
        auto tax = projected_economy_value(loc, explain_group_tax(rules, input, static_cast<Group>(g)), "");
        tax.explanation.label = group_name(loc, g);
        out.explanation.terms.push_back(std::move(tax.explanation));
    }
    auto holding = projected_economy_value(
        loc, explain_holding_income(rules, production_input), "ui.province.holding_income");
    if (holding.value != income.holding) {
        ui::Explanation adjusted;
        adjusted.label = holding.explanation.label;
        adjusted.value = format_money(loc, income.holding);
        adjusted.terms.push_back(std::move(holding.explanation));
        adjusted.terms.push_back({loc.format("ui.province.income_control"),
                                  format_money(loc, income.holding - holding.value, true),
                                  "",
                                  {}});
        out.explanation.terms.push_back(std::move(adjusted));
    } else {
        out.explanation.terms.push_back(std::move(holding.explanation));
    }
    return out;
}

}  // namespace

ui::ProvinceEconomySnapshot province_economy_snapshot(const loc::Localizer& loc,
                                                      const sim::World& world,
                                                      sim::ProvinceId province,
                                                      const sim::economy::EconomyRules& rules,
                                                      const EconomyViewOptions& options) {
    ui::ProvinceEconomySnapshot out;
    if (!world.economy_present() || !world.exists(province)) {
        return out;
    }
    out.available = true;
    out.id = province.value();
    out.name = options.province_name ? options.province_name(province)
                                     : loc.format("ui.province.name", {Arg("id", province.index() + 1)});
    out.owner = province_owner(loc, world, province, options);
    const auto& economy = world.economy();
    const auto& provinces = economy.provinces;
    const std::size_t p = province.index();
    const auto administrator = provinces.administrator[p];
    if (world.is_alive(administrator)) {
        out.has_administrator = true;
        out.administrator_id = administrator.value();
        out.administrator = options.character_name ? options.character_name(administrator)
                                                   : loc.format("ui.province.administrator_name",
                                                                {Arg("id", administrator.index() + 1)});
    } else {
        out.administrator = loc.format("ui.province.administrator_none");
    }
    const auto modifiers = compute_province_modifiers(world, rules, options.administrators, province);
    const auto administration = administration_input(world, options.administrators, province, &modifiers);
    const auto moods = mood_input(world, rules, province);
    out.population = recorded_economy_value(loc,
                                            ui::EconomyUnit::People,
                                            static_cast<std::int64_t>(world.population_persons(province)),
                                            "ui.province.population");
    out.population.explanation.terms.clear();
    out.groups.reserve(kGroupCount);
    for (std::size_t g = 0; g < kGroupCount; ++g) {
        const std::size_t row = p * kGroupCount + g;
        ui::EconomyGroupSnapshot group;
        group.key = kGroupNames[g];
        group.name = group_name(loc, g);
        group.population = recorded_economy_value(
            loc, ui::EconomyUnit::People, economy.groups.size[row], "ui.province.population");
        group.mood = recorded_economy_value(
            loc, ui::EconomyUnit::Points, economy.groups.mood[row].raw(), "ui.province.mood");
        group.wealth = recorded_economy_value(
            loc, ui::EconomyUnit::Points, economy.groups.wealth[row].raw(), "ui.province.wealth");
        group.projected_mood = projected_economy_value(
            loc, explain_mood(rules, moods, static_cast<Group>(g)), "ui.province.projected_mood");
        auto term = group.population.explanation;
        term.label = group.name;
        out.population.explanation.terms.push_back(std::move(term));
        out.groups.push_back(std::move(group));
    }
    out.control = recorded_economy_value(
        loc, ui::EconomyUnit::Points, provinces.control[p].raw(), "ui.province.control");
    out.corruption = recorded_economy_value(loc,
                                            ui::EconomyUnit::Permille,
                                            corruption_permille(provinces.corruption[p]),
                                            "ui.province.corruption");
    out.unrest =
        recorded_economy_value(loc, ui::EconomyUnit::Points, provinces.unrest[p].raw(), "ui.province.unrest");
    out.grain = recorded_economy_value(
        loc, ui::EconomyUnit::Pieces, provinces.grain_stock[p].raw(), "ui.province.grain");
    out.income_last_month = recorded_economy_value(
        loc, ui::EconomyUnit::Kopecks, provinces.income_last_month[p], "ui.province.income_last_month");
    out.projected_control =
        projected_economy_value(loc, explain_control(rules, administration), "ui.province.projected_control");
    out.projected_corruption = projected_economy_value(
        loc, explain_corruption(rules, administration), "ui.province.projected_corruption");
    out.projected_unrest = projected_economy_value(
        loc, explain_unrest(rules, unrest_input(world, province)), "ui.province.projected_unrest");
    const StaticPriceSource prices(rules.production);
    out.projected_income = projected_owner_income(
        loc,
        world,
        province,
        rules,
        production_input(world, options.prices ? *options.prices : prices, province, &modifiers));
    const std::size_t slots = provinces.slots_total[p];
    std::array<bool, kMaxSlots> occupied{};
    const auto upkeep = make_upkeep_table(rules.buildings);
    for (std::size_t slot = 0; slot < slots; ++slot) {
        const std::uint8_t type = provinces.slot_building[p * kMaxSlots + slot];
        if (type == 0) {
            continue;
        }
        occupied[slot] = true;
        ui::EconomyBuildingSnapshot building;
        building.slot = static_cast<std::uint8_t>(slot);
        for (std::size_t i = 0; i < static_cast<std::size_t>(rules.buildings.count); ++i) {
            if (rules.buildings.defs[i].slot == type) {
                building.key = rules.buildings.defs[i].key;
                break;
            }
        }
        building.name = building.key.empty() ? loc.format("ui.province.building_name", {Arg("id", type)})
                                             : loc.format(building.key);
        building.upkeep =
            recorded_economy_value(loc, ui::EconomyUnit::Kopecks, upkeep[type], "ui.province.upkeep");
        out.buildings.push_back(std::move(building));
    }
    for (std::size_t row = 0; row < economy.construction.size(); ++row) {
        if (economy.construction.province[row] == province && economy.construction.slot[row] < slots) {
            occupied[economy.construction.slot[row]] = true;
        }
    }
    std::int64_t free = 0;
    for (std::size_t slot = 0; slot < slots; ++slot) {
        free += occupied[slot] ? 0 : 1;
    }
    out.slots = recorded_economy_value(
        loc, ui::EconomyUnit::Count, static_cast<std::int64_t>(slots), "ui.province.slots");
    out.empty_slots = recorded_economy_value(loc, ui::EconomyUnit::Count, free, "ui.province.empty_slots");
    return out;
}

}  // namespace pat::game
