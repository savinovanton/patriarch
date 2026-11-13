#include "pat/game/province_view.hpp"

#include "pat/game/tooltip_view.hpp"
#include "pat/game/world_view.hpp"

#include <algorithm>
#include <string>
#include <utility>

namespace pat::game {

namespace {

using loc::Arg;
using sim::economy::OwnerKind;

EconomyViewOptions
resolved_names(const loc::Localizer& loc, const sim::World& world, const EconomyViewOptions& options) {
    EconomyViewOptions out = options;
    out.province_name = [&loc, callback = options.province_name](sim::ProvinceId id) {
        const std::string name = callback ? callback(id) : std::string();
        return name.empty() ? loc.format("ui.province_window.view.name", {Arg("id", id.index() + 1)}) : name;
    };
    out.dynasty_name = [callback = options.dynasty_name](sim::DynastyId id) {
        const std::string name = callback ? callback(id) : std::string();
        return name.empty() ? dynasty_name(id) : name;
    };
    out.character_name = [&loc, &world, callback = options.character_name](sim::CharacterId id) {
        const std::string name = callback ? callback(id) : std::string();
        return name.empty() ? character_snapshot(loc, world, id).full_name() : name;
    };
    return out;
}

void describe_owner(const loc::Localizer& loc,
                    const sim::World& world,
                    sim::ProvinceId province,
                    const EconomyViewOptions& names,
                    ui::ProvinceSnapshot& out) {
    const auto dynasty = world.provinces().owner[province.index()];
    const auto kind = world.economy_present()
                          ? static_cast<OwnerKind>(world.economy().provinces.owner_kind[province.index()])
                      : world.exists(dynasty) ? OwnerKind::Dynasty
                                              : OwnerKind::Unowned;
    if (kind == OwnerKind::Dynasty && world.exists(dynasty)) {
        const std::string name = names.dynasty_name(dynasty);
        out.economy.owner = name;
        out.subtitle = loc.format("ui.province_window.view.subtitle.dynasty", {Arg("name", name)});
        out.owner_name = name;
        const auto head = world.dynasties().head[dynasty.index()];
        if (world.is_alive(head)) {
            out.owner_name = names.character_name(head);
            out.owner_detail = loc.format(
                "ui.province_window.view.owner.head",
                {Arg("dynasty", name),
                 Arg("age", age_in_years(world.characters().birth_date[head.index()], world.today()))});
            out.owner_portrait = true;
        } else {
            out.owner_detail = loc.format("ui.province_window.view.owner.no_head");
        }
        return;
    }
    const char* owner_key = "ui.province_window.view.owner.none";
    const char* subtitle_key = "ui.province_window.view.subtitle.none";
    switch (kind) {
        case OwnerKind::Crown:
            owner_key = "ui.province_window.view.owner.crown";
            subtitle_key = "ui.province_window.view.subtitle.crown";
            break;
        case OwnerKind::Church:
            owner_key = "ui.province_window.view.owner.church";
            subtitle_key = "ui.province_window.view.subtitle.church";
            break;
        case OwnerKind::Free:
            owner_key = "ui.province_window.view.owner.free";
            subtitle_key = "ui.province_window.view.subtitle.free";
            break;
        default:
            break;
    }
    out.economy.owner = loc.format(owner_key);
    out.owner_name = out.economy.owner;
    out.subtitle = loc.format(subtitle_key);
}

ui::Explanation unavailable(const loc::Localizer& loc, const char* label, const char* reason) {
    ui::Explanation out;
    out.label = loc.format(label);
    out.lines.push_back(loc.format(reason));
    return out;
}

}  // namespace

ui::ProvinceSnapshot province_snapshot(const loc::Localizer& loc,
                                       const sim::World& world,
                                       sim::ProvinceId province,
                                       const sim::economy::EconomyRules& rules,
                                       const EconomyViewOptions& options) {
    ui::ProvinceSnapshot out;
    if (!world.exists(province)) {
        return out;
    }
    const auto names = resolved_names(loc, world, options);
    out.economy = province_economy_snapshot(loc, world, province, rules, names);
    out.economy.id = province.value();
    if (!out.economy.available) {
        out.economy.name = names.province_name(province);
    }
    describe_owner(loc, world, province, names, out);
    out.garrison = loc.format("ui.province_window.view.unavailable");
    out.garrison_explanation =
        unavailable(loc, "ui.province_window.view.garrison", "ui.province_window.view.garrison.unavailable");
    out.manage_explanation =
        unavailable(loc, "ui.province_window.view.manage", "ui.province_window.view.manage.unavailable");
    out.manage_explanation.tone = ui::TipTone::Warning;
    if (!out.economy.available) {
        out.order = loc.format("ui.province_window.view.unavailable");
        out.state = loc.format("ui.province_window.view.state.unavailable");
        out.state_detail = loc.format("ui.province_window.view.economy.unavailable");
        out.state_explanation = unavailable(
            loc, "ui.province_window.view.state.unavailable", "ui.province_window.view.economy.unavailable");
        out.income_change.unit = ui::EconomyUnit::Kopecks;
        out.income_change.explanation = unavailable(
            loc, "ui.province_window.view.income_change", "ui.province_window.view.economy.unavailable");
        out.income_change.explanation.value = loc.format("ui.province_window.view.unavailable");
        return out;
    }

    out.order =
        loc.format("ui.province_window.view.order", {Arg("value", out.economy.control.explanation.value)});
    out.order_percent = std::clamp(world.economy().provinces.control[province.index()].round_int(), 0, 100);
    out.state = out.economy.has_administrator ? loc.format("ui.province_window.view.state.administrator",
                                                           {Arg("name", out.economy.administrator)})
                                              : loc.format("ui.province_window.view.state.no_administrator");
    out.state_detail = loc.format("ui.province_window.view.state.corruption",
                                  {Arg("value", out.economy.corruption.explanation.value)});
    out.state_explanation = out.economy.corruption.explanation;

    out.income_change.value = out.economy.projected_income.value - out.economy.income_last_month.value;
    out.income_change.unit = ui::EconomyUnit::Kopecks;
    auto& explanation = out.income_change.explanation;
    explanation.label = loc.format("ui.province_window.view.income_change");
    explanation.note = loc.format("ui.province_window.view.income_change.note");
    explanation.value = format_money(loc, out.income_change.value, true);
    explanation.total_label = loc.format("ui.explain.total");
    auto projected = out.economy.projected_income.explanation;
    projected.value = format_money(loc, out.economy.projected_income.value, true);
    explanation.terms.push_back(std::move(projected));
    explanation.terms.push_back({loc.format("ui.province_window.view.income_change.subtract_recorded"),
                                 format_money(loc, -out.economy.income_last_month.value, true),
                                 "",
                                 {}});
    return out;
}

}  // namespace pat::game
