// The adapters from simulation numbers to tooltip content (UI-D03, #258): money and Breakdown units,
// a Breakdown tree as tooltip rows, the string of every issued term code, the shortage reason.

#include "pat/game/tooltip_view.hpp"
#include "pat/sim/economy/breakdown.hpp"

#include <doctest/doctest.h>

#include <string>

#include "ui_loc_fixture.hpp"

namespace {

using namespace pat::game;
namespace economy = pat::sim::economy;

const std::string kMinus = "\xE2\x88\x92";

std::string rouble(std::int64_t n) {
    return pat::test::ui_localizer().format("ui.fmt.rouble", {pat::loc::Arg("n", n)});
}

}  // namespace

TEST_CASE("tooltip view: money has groups, the true minus sign and an optional plus") {
    const auto& loc = pat::test::ui_localizer();
    CHECK(format_money(loc, 124800) == rouble(1248));
    CHECK(format_money(loc, 6800, true) == "+" + rouble(68));
    CHECK(format_money(loc, 6800) == rouble(68));
    CHECK(format_money(loc, -2600, true) == kMinus + rouble(26));
    CHECK(format_money(loc, -2600) == kMinus + rouble(26));
    CHECK(format_money(loc, 0, true) == rouble(0));
    CHECK(format_money(loc, 1250) == "12,50 \xE2\x82\xBD");
    CHECK(format_money(loc, -5, true) == kMinus + "0,05 \xE2\x82\xBD");
}

TEST_CASE("tooltip view: every unit of a Breakdown node has a format") {
    const auto& loc = pat::test::ui_localizer();
    using economy::Unit;
    CHECK(format_breakdown_value(loc, Unit::People, 12000, false).find("чел.") != std::string::npos);
    CHECK(format_breakdown_value(loc, Unit::Points, 65536 * 7 / 2, true) == "+3,5");
    CHECK(format_breakdown_value(loc, Unit::Points, -65536, true) == kMinus + "1,0");
    CHECK(format_breakdown_value(loc, Unit::Permille, 350, false) == "35,0 %");
    CHECK(format_breakdown_value(loc, Unit::Permille, -125, true) == kMinus + "12,5 %");
    CHECK(format_breakdown_value(loc, Unit::Ppm, 12000, true) == "+1,2 % в год");
    CHECK(format_breakdown_value(loc, Unit::Pieces, 65536 * 2, false) == "2,0 шт.");
    CHECK(format_factor(loc, 1200) == "×1,20");
    CHECK(format_factor(loc, 875) == "×0,88");  // rounded to two decimals
}

TEST_CASE("tooltip view: a Breakdown becomes the rows of a tooltip, a multiplier step keeps its factor") {
    const auto& loc = pat::test::ui_localizer();
    economy::Breakdown b;
    b.begin(economy::term::kTaxTotal, economy::Unit::Kopecks, 10000);
    b.add_term(economy::kRootNode, economy::term::kTaxBase, 8000);
    const economy::NodeIndex policy =
        b.add_step(economy::kRootNode, economy::term::kTaxPolicy, 8000, 9600, 1200);
    b.add_term(policy, economy::term::kTaxWealth, 1000);
    b.add_term(policy, economy::term::kTaxControl, 600);
    b.add_term(economy::kRootNode, economy::term::kTaxLaw, 400);
    REQUIRE(b.is_consistent());

    const pat::ui::Explanation e = breakdown_explanation(loc, b);
    CHECK(e.label == loc.format("economy.tax.total"));
    CHECK(e.value == rouble(100));
    CHECK(e.total_label == loc.format("ui.explain.total"));
    CHECK(e.footer.empty());
    REQUIRE(e.terms.size() == 3);
    CHECK(e.terms[0].label == loc.format("economy.tax.base"));
    CHECK(e.terms[0].value == "+" + rouble(80));
    CHECK(e.terms[0].terms.empty());
    CHECK(e.terms[1].label == loc.format("economy.tax.policy"));
    CHECK(e.terms[1].note == "×1,20");
    CHECK(e.terms[1].value == "+" + rouble(16));
    REQUIRE(e.terms[1].terms.size() == 2);  // the next level of the tooltip
    CHECK(e.terms[1].terms[0].label == loc.format("economy.tax.wealth"));
    CHECK(e.terms[1].terms[1].value == "+" + rouble(6));

    // A total with a sign (income per month) and no sum row.
    economy::Breakdown delta;
    delta.begin(economy::term::kOutputIncome, economy::Unit::Kopecks, 4200);
    delta.add_term(economy::kRootNode, economy::term::kOutputOwnerShare, 6800);
    delta.add_term(economy::kRootNode, economy::term::kTaxLeak, -2600);
    BreakdownTooltip options;
    options.signed_total = true;
    options.total_label_key = {};
    const pat::ui::Explanation income = breakdown_explanation(loc, delta, options);
    CHECK(income.value == "+" + rouble(42));
    CHECK(income.total_label.empty());
    CHECK(income.terms[1].value == kMinus + rouble(26));

    // A truncated tree says so.
    economy::Breakdown full;
    full.begin(economy::term::kTaxTotal, economy::Unit::Kopecks, 0);
    for (std::size_t i = 0; i < economy::kBreakdownCapacity + 2; ++i) {
        full.add_term(economy::kRootNode, economy::term::kTaxBase, 0);
    }
    REQUIRE(full.truncated());
    CHECK_FALSE(breakdown_explanation(loc, full).footer.empty());

    CHECK(breakdown_explanation(loc, economy::Breakdown{}) == pat::ui::Explanation{});
}

TEST_CASE("tooltip view: every issued term code has a string in the table") {
    const auto& loc = pat::test::ui_localizer();
    for (std::size_t i = 0; i < economy::term_code_count(); ++i) {
        const std::uint16_t code = economy::term_code_at(i);
        const std::string_view key = economy::term_key(code);
        CAPTURE(code);
        REQUIRE_FALSE(key.empty());
        const std::string text = loc.format(key);
        CHECK(text.rfind("[[", 0) != 0);  // a missing key shows as [[key]]
    }
}

TEST_CASE("tooltip view: an unavailable action shows what is missing and the split of its cost") {
    const auto& loc = pat::test::ui_localizer();
    const CostPart parts[] = {{"Дары и письмо", 8000}, {"Дорога посредника", 4000}};
    const pat::ui::Explanation e = shortage_explanation(loc, parts, 8000);
    CHECK(e.label == "Не хватает " + rouble(40));
    CHECK(e.tone == pat::ui::TipTone::Warning);
    REQUIRE(e.lines.size() == 1);
    CHECK(e.lines[0] == "В казне " + rouble(80) + ", требуется " + rouble(120) + ".");
    REQUIRE(e.terms.size() == 2);
    CHECK(e.terms[0].value == rouble(80));
    CHECK(e.terms[1].value == rouble(40));
    CHECK(e.total_label == "Списать сейчас");
    CHECK(e.value == rouble(120));
    CHECK_FALSE(e.footer.empty());
}
