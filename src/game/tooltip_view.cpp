#include "pat/game/tooltip_view.hpp"

#include <string>
#include <vector>

namespace pat::game {

namespace {

using loc::Arg;
using sim::economy::Breakdown;
using sim::economy::BreakdownNode;
using sim::economy::NodeIndex;
using sim::economy::Unit;

constexpr const char* kMinus = "\xE2\x88\x92";  // U+2212, as in the mockup

/// Divides rounding half away from zero.
std::int64_t round_div(std::int64_t value, std::int64_t divisor) {
    const std::int64_t half = divisor / 2;
    return value >= 0 ? (value + half) / divisor : -((-value + half) / divisor);
}

std::string with_sign_prefix(const std::string& text, std::int64_t value, bool with_sign) {
    if (value < 0) {
        return kMinus + text;
    }
    return with_sign && value > 0 ? "+" + text : text;
}

/// "3,5": `scaled` is the number times 10^decimals (decimals is 1 or 2), without sign.
std::string decimal_text(const loc::Localizer& loc, std::int64_t scaled, int decimals) {
    std::int64_t unit = 1;
    for (int i = 0; i < decimals; ++i) {
        unit *= 10;
    }
    std::string frac = std::to_string(scaled % unit);
    while (static_cast<int>(frac.size()) < decimals) {
        frac.insert(frac.begin(), '0');
    }
    return loc.format("ui.fmt.decimal", {Arg("whole", scaled / unit), Arg("frac", frac)});
}

std::string money_text(const loc::Localizer& loc, std::int64_t kopecks) {
    const std::int64_t abs_k = kopecks < 0 ? -kopecks : kopecks;
    if (abs_k % 100 == 0) {
        return loc.format("ui.fmt.rouble", {Arg("n", abs_k / 100)});
    }
    std::string kop = std::to_string(abs_k % 100);
    if (kop.size() < 2) {
        kop.insert(kop.begin(), '0');
    }
    return loc.format("ui.fmt.rouble_kop", {Arg("n", abs_k / 100), Arg("kop", kop)});
}

}  // namespace

std::string format_money(const loc::Localizer& loc, std::int64_t kopecks, bool with_sign) {
    return with_sign_prefix(money_text(loc, kopecks), kopecks, with_sign);
}

std::string format_breakdown_value(const loc::Localizer& loc,
                                   sim::economy::Unit unit,
                                   std::int64_t value,
                                   bool with_sign) {
    const std::int64_t abs_v = value < 0 ? -value : value;
    std::string text;
    switch (unit) {
        case Unit::Kopecks:
            return format_money(loc, value, with_sign);
        case Unit::People:
            text = loc.format("ui.fmt.people", {Arg("n", abs_v)});
            break;
        case Unit::Points:  // Fixed32 raw, Q16.16
            text = decimal_text(loc, round_div(abs_v * 10, 65536), 1);
            break;
        case Unit::Pieces:
            text = loc.format("ui.fmt.pieces",
                              {Arg("value", decimal_text(loc, round_div(abs_v * 10, 65536), 1))});
            break;
        case Unit::Permille:  // 1 per-mille = 0.1 %
            text = loc.format("ui.fmt.percent", {Arg("value", decimal_text(loc, abs_v, 1))});
            break;
        case Unit::Ppm:  // 10000 ppm = 1 %; one decimal
            text = loc.format("ui.fmt.percent_year",
                              {Arg("value", decimal_text(loc, round_div(abs_v, 1000), 1))});
            break;
    }
    return with_sign_prefix(text, value, with_sign);
}

std::string format_factor(const loc::Localizer& loc, std::int32_t factor_permille) {
    return loc.format("ui.explain.factor",
                      {Arg("factor", decimal_text(loc, round_div(factor_permille, 10), 2))});
}

namespace {

std::int64_t displayed_units(Unit unit, std::int64_t value) {
    return unit == Unit::Ppm ? round_div(value, 1000) : round_div(value * 10, 65536);
}

std::int64_t display_adjustment_raw(Unit unit, std::int64_t value) {
    return unit == Unit::Ppm ? value * 1000 : value * 65536 / 10;
}

ui::Explanation
node_explanation(const loc::Localizer& loc, const Breakdown& b, NodeIndex index, bool signed_value) {
    const BreakdownNode& node = b.node(index);
    ui::Explanation e;
    const std::string_view key = sim::economy::term_key(node.code);
    e.label = loc.format(key.empty() ? std::string_view("ui.explain.unknown_term") : key);
    e.value = format_breakdown_value(loc, node.unit, node.value, signed_value);
    if (node.factor_permille != 0) {
        e.note = format_factor(loc, node.factor_permille);
    }
    for (std::size_t i = 1; i < b.size(); ++i) {
        if (b.node(static_cast<NodeIndex>(i)).parent == index) {
            e.terms.push_back(node_explanation(loc, b, static_cast<NodeIndex>(i), true));
        }
    }
    if (!e.terms.empty() &&
        (node.unit == Unit::Points || node.unit == Unit::Pieces || node.unit == Unit::Ppm) &&
        b.children_sum(index) == node.value) {
        std::int64_t displayed_sum = 0;
        for (std::size_t i = 1; i < b.size(); ++i) {
            const auto child = static_cast<NodeIndex>(i);
            if (b.node(child).parent == index) {
                displayed_sum += displayed_units(node.unit, b.node(child).value);
            }
        }
        const std::int64_t correction = displayed_units(node.unit, node.value) - displayed_sum;
        if (correction != 0) {
            e.terms.push_back(
                {loc.format("ui.explain.rounding"),
                 format_breakdown_value(loc, node.unit, display_adjustment_raw(node.unit, correction), true),
                 "",
                 {}});
        }
    }
    return e;
}

}  // namespace

ui::Explanation breakdown_explanation(const loc::Localizer& loc,
                                      const sim::economy::Breakdown& breakdown,
                                      const BreakdownTooltip& options) {
    if (breakdown.empty()) {
        return {};
    }
    ui::Explanation root = node_explanation(loc, breakdown, sim::economy::kRootNode, options.signed_total);
    root.note.clear();  // the root has no multiplier of its own
    if (!options.total_label_key.empty()) {
        root.total_label = loc.format(options.total_label_key);
    }
    if (breakdown.truncated()) {
        root.footer = loc.format("ui.explain.truncated");
    }
    root.wide = options.wide;
    return root;
}

ui::Explanation shortage_explanation(const loc::Localizer& loc,
                                     std::span<const CostPart> parts,
                                     std::int64_t available_kopecks) {
    std::int64_t needed = 0;
    ui::Explanation e;
    for (const CostPart& part : parts) {
        needed += part.kopecks;
        e.terms.push_back({part.label, format_money(loc, part.kopecks), "", {}});
    }
    const std::int64_t missing = needed > available_kopecks ? needed - available_kopecks : 0;
    e.label = loc.format("ui.tip.shortage.title", {Arg("amount", format_money(loc, missing))});
    e.tone = ui::TipTone::Warning;
    e.lines.push_back(loc.format(
        "ui.tip.shortage.detail",
        {Arg("have", format_money(loc, available_kopecks)), Arg("need", format_money(loc, needed))}));
    e.total_label = loc.format("ui.tip.shortage.total");
    e.value = format_money(loc, needed);
    e.footer = loc.format("ui.tip.shortage.footer");
    return e;
}

}  // namespace pat::game
