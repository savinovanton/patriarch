#pragma once

#include "pat/loc/localizer.hpp"
#include "pat/sim/economy/breakdown.hpp"
#include "pat/ui/explanation.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

// Read-only adapters from simulation numbers to tooltip content (UI-D03, #258, docs/dev/economy-model.md
// section 10): a Breakdown tree becomes the rows of an ui::Explanation; the unit of a node decides the number
// format, the code of a node decides the label (term_key -> string table). Nothing here changes the world.

namespace pat::game {

/// "1 248 ₽", "12,50 ₽" from kopecks; `with_sign` adds "+" for positive values, a negative value always
/// carries the true minus sign U+2212 ("−26 ₽").
[[nodiscard]] std::string
format_money(const loc::Localizer& loc, std::int64_t kopecks, bool with_sign = false);

/// A value of a Breakdown node in its unit: kopecks as money, people as "N чел.", points and pieces
/// (Fixed32 raw) with one decimal, per-mille and ppm as percent. `with_sign` as in format_money.
[[nodiscard]] std::string format_breakdown_value(const loc::Localizer& loc,
                                                 sim::economy::Unit unit,
                                                 std::int64_t value,
                                                 bool with_sign);

/// "×1,20" from a per-mille multiplier (1200).
[[nodiscard]] std::string format_factor(const loc::Localizer& loc, std::int32_t factor_permille);

struct BreakdownTooltip {
    /// Key of the label of the sum row ("Итого"); empty: the total stands next to the title.
    std::string_view total_label_key = "ui.explain.total";
    /// Show "+" on the total (a delta such as income per month); the terms are always signed.
    bool signed_total = false;
    bool wide = false;
};

/// The tooltip of a Breakdown: the title is the label of the root node, the rows are its terms (a term's
/// own terms become the nested level of the tooltip), a multiplier step carries "×1,20" in `note`, the
/// total is the root value. A truncated tree gets a footer saying so.
[[nodiscard]] ui::Explanation breakdown_explanation(const loc::Localizer& loc,
                                                    const sim::economy::Breakdown& breakdown,
                                                    const BreakdownTooltip& options = {});

struct CostPart {
    std::string label;  ///< "Дары и письмо"
    std::int64_t kopecks = 0;
};

/// "Не хватает 40 ₽": the reason an action is unavailable because of money, with the split of its cost
/// (board 09 of the mockup). `available_kopecks` is what the treasury holds.
[[nodiscard]] ui::Explanation shortage_explanation(const loc::Localizer& loc,
                                                   std::span<const CostPart> parts,
                                                   std::int64_t available_kopecks);

}  // namespace pat::game
