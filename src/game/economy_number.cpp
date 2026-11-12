#include "pat/game/economy_view.hpp"
#include "pat/game/tooltip_view.hpp"

#include <string>

namespace pat::game {

namespace {

ui::EconomyUnit snapshot_unit(sim::economy::Unit unit) {
    using sim::economy::Unit;
    switch (unit) {
        case Unit::Kopecks:
            return ui::EconomyUnit::Kopecks;
        case Unit::People:
            return ui::EconomyUnit::People;
        case Unit::Points:
            return ui::EconomyUnit::Points;
        case Unit::Pieces:
            return ui::EconomyUnit::Pieces;
        case Unit::Permille:
            return ui::EconomyUnit::Permille;
        case Unit::Ppm:
            return ui::EconomyUnit::Ppm;
    }
    return ui::EconomyUnit::Count;
}

std::string value_text(const loc::Localizer& loc, ui::EconomyUnit unit, std::int64_t value, bool with_sign) {
    using sim::economy::Unit;
    switch (unit) {
        case ui::EconomyUnit::Kopecks:
            return format_breakdown_value(loc, Unit::Kopecks, value, with_sign);
        case ui::EconomyUnit::People:
            return format_breakdown_value(loc, Unit::People, value, with_sign);
        case ui::EconomyUnit::Points:
            return format_breakdown_value(loc, Unit::Points, value, with_sign);
        case ui::EconomyUnit::Pieces:
            return format_breakdown_value(loc, Unit::Pieces, value, with_sign);
        case ui::EconomyUnit::Permille:
            return format_breakdown_value(loc, Unit::Permille, value, with_sign);
        case ui::EconomyUnit::Ppm:
            return format_breakdown_value(loc, Unit::Ppm, value, with_sign);
        case ui::EconomyUnit::Count:
            return (with_sign && value > 0 ? "+" : "") + std::to_string(value);
    }
    return {};
}

}  // namespace

ui::EconomyNumber recorded_economy_value(const loc::Localizer& loc,
                                         ui::EconomyUnit unit,
                                         std::int64_t value,
                                         std::string_view label_key) {
    ui::EconomyNumber out;
    out.value = value;
    out.unit = unit;
    out.explanation.label = loc.format(label_key);
    out.explanation.value = value_text(loc, unit, value, false);
    out.explanation.total_label = loc.format("ui.explain.total");
    out.explanation.terms.push_back(
        {loc.format("ui.economy.recorded"), value_text(loc, unit, value, true), "", {}});
    return out;
}

ui::EconomyNumber projected_economy_value(const loc::Localizer& loc,
                                          const sim::economy::Breakdown& breakdown,
                                          std::string_view label_key) {
    if (breakdown.empty()) {
        return {};
    }
    const ui::EconomyUnit unit = snapshot_unit(breakdown.root().unit);
    if (!breakdown.is_consistent()) {
        // An incomplete tree cannot explain its total additively. Keep the authoritative computed
        // value and explicitly mark the absent detail instead of showing a misleading partial sum.
        auto out = recorded_economy_value(loc, unit, breakdown.total(), label_key);
        out.explanation.terms.front().label = loc.format("ui.economy.computed");
        out.explanation.footer = loc.format("ui.explain.truncated");
        return out;
    }
    ui::EconomyNumber out;
    out.value = breakdown.total();
    out.unit = unit;
    out.explanation = breakdown_explanation(loc, breakdown);
    if (!label_key.empty()) {
        out.explanation.label = loc.format(label_key);
    }
    return out;
}

}  // namespace pat::game
