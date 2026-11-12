#include "pat/core/rng.hpp"
#include "pat/ui/character_snapshot.hpp"
#include "pat/ui/explanation.hpp"
#include "pat/ui/tooltip_state.hpp"

#include <doctest/doctest.h>

#include <string>

#include "ui_loc_fixture.hpp"

using namespace pat::ui;

namespace {

HoverInfo hover(std::string path, int level, RectF anchor = {100, 100, 80, 20}) {
    return {std::move(path), level, anchor};
}

// Runs the state for `seconds` with a constant hover, in 50 ms steps.
void run(TooltipState& s, const HoverInfo& h, float seconds) {
    s.set_hover(h);
    for (float t = 0.0f; t < seconds - 1e-4f; t += 0.05f) {
        s.tick(0.05f);
    }
}

int sum_children(const Explanation& e) {
    int sum = 0;
    for (const Explanation& t : e.terms) {
        sum += std::stoi(t.value);
    }
    return sum;
}

void check_consistent(const Explanation& e) {
    if (!e.terms.empty()) {
        CHECK(sum_children(e) == std::stoi(e.value));
        for (const Explanation& t : e.terms) {
            check_consistent(t);
        }
    }
}

int depth(const Explanation& e) {
    int d = 0;
    for (const Explanation& t : e.terms) {
        d = std::max(d, depth(t));
    }
    return d + (e.terms.empty() ? 0 : 1);
}

}  // namespace

// ---------------------------------------------------------------- explanation trees

TEST_CASE("explanation: signed values, paths and escaping") {
    CHECK(signed_value(3) == "+3");
    CHECK(signed_value(0) == "0");
    CHECK(signed_value(-2) == "-2");

    Explanation root{"Хозяйство", "9", "", {}};
    root.terms.push_back({"Базовое", "+4", "", {}});
    root.terms.push_back(
        {"Образование",
         "+3",
         "",
         {{"Наставник", "+2", "", {{"Навык", "+2", "", {}}}}, {"За границей", "+1", "", {}}}});
    CHECK(find_term(root, "") == &root);
    CHECK(find_term(root, "0")->label == "Базовое");
    CHECK(find_term(root, "1/0")->label == "Наставник");
    CHECK(find_term(root, "1/0/0")->label == "Навык");
    CHECK(find_term(root, "1/1")->label == "За границей");
    CHECK(find_term(root, "2") == nullptr);
    CHECK(find_term(root, "1/5") == nullptr);
    CHECK(find_term(root, "x") == nullptr);
    CHECK(find_term(root, "1/") == nullptr);
    CHECK(find_term(root, "-1") == nullptr);

    const SplitPath sp = split_tooltip_path("skill:2/1/0");
    CHECK(sp.key == "skill:2");
    CHECK(sp.terms == "1/0");
    CHECK(split_tooltip_path("skill:2").terms.empty());

    CHECK(escape_markup("a<b>&\"c\"") == "a&lt;b&gt;&amp;&quot;c&quot;");
}

TEST_CASE("explanation: generated markup has a row per term and data-tip only on nested ones") {
    Explanation root{"Навык <опасный>", "9", "Заметка & пояснение", {}};
    root.terms.push_back({"Базовое", "+4", "", {}});
    root.terms.push_back({"Штраф", "-2", "", {}});
    root.terms.push_back({"Образование", "+7", "", {{"Наставник", "+7", "", {}}}});
    const std::string rml = render_tooltip_rml(root, "skill:0", 0);

    const auto count = [&](const std::string& needle) {
        std::size_t n = 0;
        for (std::size_t pos = rml.find(needle); pos != std::string::npos; pos = rml.find(needle, pos + 1)) {
            ++n;
        }
        return n;
    };
    CHECK(count("class=\"tip-row") == 3);
    CHECK(count("data-tip=") == 1);  // only the term with sub-terms
    CHECK(rml.find("data-tip=\"skill:0/2\"") != std::string::npos);
    CHECK(rml.find("data-tip-level=\"0\"") != std::string::npos);
    CHECK(rml.find("tip-value pos") != std::string::npos);
    CHECK(rml.find("tip-value neg") != std::string::npos);
    CHECK(rml.find("&lt;опасный&gt;") != std::string::npos);  // escaped title
    CHECK(rml.find("Заметка &amp; пояснение") != std::string::npos);
    CHECK(rml.find("<опасный>") == std::string::npos);
}

TEST_CASE("test characters: explanation trees add up and go three levels deep") {
    int deepest = 0;
    for (std::uint32_t seed = 1; seed <= 40; ++seed) {
        const CharacterSnapshot c = make_test_character(pat::test::ui_localizer(), seed, 5);
        for (std::size_t i = 0; i < kSkillCount; ++i) {
            const Explanation& e = c.skill_explanations[i];
            CHECK(e.label == pat::test::ui_localizer().format(kSkillKeys[i]));
            CHECK(std::stoi(e.value) == c.skills[i]);
            check_consistent(e);
            deepest = std::max(deepest, depth(e));
        }
    }
    CHECK(deepest == 3);
}

// ---------------------------------------------------------------- placement

TEST_CASE("place_tooltip: prefers below-right of the cursor and flips at the screen edges") {
    PlaceRequest r;
    r.size = {200, 100};
    r.viewport = {1000, 700};
    r.anchor = {300, 300, 100, 20};
    r.cursor_x = 320;
    r.cursor_y = 310;
    RectF t = place_tooltip(r);
    CHECK(t.x > r.cursor_x);
    CHECK(t.y > r.cursor_y);

    r.cursor_x = 950;  // near the right edge: goes to the left of the cursor
    r.anchor = {930, 300, 60, 20};
    t = place_tooltip(r);
    CHECK(t.right() < r.cursor_x);
    CHECK(t.right() <= 1000 - r.margin);

    r.cursor_x = 300;
    r.cursor_y = 660;  // near the bottom: goes above
    r.anchor = {290, 650, 60, 20};
    t = place_tooltip(r);
    CHECK(t.bottom() < r.cursor_y);
}

TEST_CASE("place_tooltip: always inside the screen and off the cursor (fuzz)") {
    pat::core::Pcg32 rng(11, 5);
    int checked = 0;
    for (int i = 0; i < 3000; ++i) {
        PlaceRequest r;
        // Big enough that a tooltip, its nested tooltip and the cursor can always be kept apart.
        r.viewport = {static_cast<float>(rng.range(1000, 2400)), static_cast<float>(rng.range(700, 1400))};
        r.size = {static_cast<float>(rng.range(80, 280)), static_cast<float>(rng.range(40, 240))};
        r.cursor_x = static_cast<float>(rng.range(0, static_cast<int>(r.viewport.w)));
        r.cursor_y = static_cast<float>(rng.range(0, static_cast<int>(r.viewport.h)));
        r.anchor = {r.cursor_x - 10, r.cursor_y - 8, 60, 16};
        RectF parent{0, 0, 0, 0};
        if (i % 2 == 1) {  // nested: a parent tooltip next to the cursor
            parent = place_tooltip({r.size, r.viewport, r.cursor_x, r.cursor_y, r.anchor, nullptr, 8, 6, {}});
            r.parent = &parent;
            r.anchor = {parent.x + 4, parent.y + 30, parent.w - 8, 20};
            // the cursor is on the parent's row
            r.cursor_x = r.anchor.x + 10;
            r.cursor_y = r.anchor.y + 10;
        }
        const RectF t = place_tooltip(r);
        // Inside the screen whenever it can fit at all.
        CHECK(t.x >= r.margin - 0.01f);
        CHECK(t.y >= r.margin - 0.01f);
        CHECK(t.right() <= r.viewport.w - r.margin + 0.01f);
        CHECK(t.bottom() <= r.viewport.h - r.margin + 0.01f);
        // Never on top of the cursor, nor of its parent.
        CHECK_FALSE(t.contains(r.cursor_x, r.cursor_y));
        if (r.parent != nullptr) {
            CHECK_FALSE(t.intersects(*r.parent));
        }
        ++checked;
    }
    CHECK(checked == 3000);
}

TEST_CASE("place_tooltip: a third level also avoids the first one") {
    PlaceRequest root;
    root.size = {280, 120};
    root.viewport = {1280, 720};
    root.cursor_x = 900;
    root.cursor_y = 300;
    root.anchor = {880, 290, 120, 20};
    const RectF level0 = place_tooltip(root);

    PlaceRequest second = root;
    second.parent = &level0;
    second.anchor = {level0.x + 4, level0.y + 40, level0.w - 8, 20};
    second.cursor_x = second.anchor.x + 20;
    second.cursor_y = second.anchor.y + 10;
    const RectF level1 = place_tooltip(second);
    CHECK_FALSE(level1.intersects(level0));

    PlaceRequest third = second;
    third.parent = &level1;
    third.avoid = {level0};
    third.anchor = {level1.x + 4, level1.y + 40, level1.w - 8, 20};
    third.cursor_x = third.anchor.x + 20;
    third.cursor_y = third.anchor.y + 10;
    const RectF level2 = place_tooltip(third);
    CHECK_FALSE(level2.intersects(level1));
    CHECK_FALSE(level2.intersects(level0));
    CHECK(level2.x >= third.margin);
    CHECK(level2.right() <= 1280 - third.margin);
}

TEST_CASE("place_tooltip: a tooltip larger than the screen is clamped to the top-left corner") {
    PlaceRequest r;
    r.size = {900, 800};
    r.viewport = {640, 480};
    r.cursor_x = 300;
    r.cursor_y = 200;
    r.anchor = {290, 190, 40, 20};
    const RectF t = place_tooltip(r);
    CHECK(t.x == doctest::Approx(r.margin));
    CHECK(t.y == doctest::Approx(r.margin));
}

// ---------------------------------------------------------------- state machine

TEST_CASE("tooltip state: opens after the delay and closes after the grace period") {
    TooltipState s;
    run(s, hover("a", -1), 0.2f);
    CHECK(s.levels().empty());  // not yet
    run(s, hover("a", -1), 0.2f);
    REQUIRE(s.levels().size() == 1);
    CHECK(s.levels()[0].path == "a");

    const auto rev = s.revision();
    run(s, hover("a", -1), 1.0f);  // staying on the same target changes nothing
    CHECK(s.revision() == rev);
    CHECK(s.levels().size() == 1);

    run(s, hover("", -1), 0.1f);
    CHECK(s.levels().size() == 1);  // grace period
    run(s, hover("", -1), 0.3f);
    CHECK(s.levels().empty());
    CHECK(s.revision() > rev);
}

TEST_CASE("tooltip state: moving into the tooltip keeps it, nested levels open and close in order") {
    TooltipState s;
    run(s, hover("skill:0", -1), 0.5f);
    REQUIRE(s.levels().size() == 1);

    // The cursor crosses the gap and lands on the tooltip itself (inside level 0, no nested target).
    run(s, hover("", 0), 0.1f);
    run(s, hover("", 0), 1.0f);
    CHECK(s.levels().size() == 1);  // stays while the cursor is inside it

    // Hover a nested term: level 1 opens after the (shorter) nested delay.
    run(s, hover("skill:0/1", 0), 0.1f);
    CHECK(s.levels().size() == 1);
    run(s, hover("skill:0/1", 0), 0.2f);
    REQUIRE(s.levels().size() == 2);
    CHECK(s.levels()[1].path == "skill:0/1");

    // And one more level.
    run(s, hover("skill:0/1/0", 1), 0.3f);
    REQUIRE(s.levels().size() == 3);

    // No fourth level, however long the cursor stays.
    run(s, hover("skill:0/1/0/0", 2), 2.0f);
    CHECK(s.levels().size() == 3);

    // Back on level 1's own area: level 2 closes after the grace period, level 1 stays.
    run(s, hover("", 1), 0.1f);
    CHECK(s.levels().size() == 3);
    run(s, hover("", 1), 0.3f);
    CHECK(s.levels().size() == 2);

    // Leaving everything closes the rest.
    run(s, hover("", -1), 0.5f);
    CHECK(s.levels().empty());
}

TEST_CASE("tooltip state: hovering a sibling term replaces the nested level") {
    TooltipState s;
    run(s, hover("k", -1), 0.5f);
    run(s, hover("k/0", 0), 0.3f);
    REQUIRE(s.levels().size() == 2);
    CHECK(s.levels()[1].path == "k/0");
    run(s, hover("k/1", 0), 0.3f);
    REQUIRE(s.levels().size() == 2);
    CHECK(s.levels()[1].path == "k/1");
    // A different root target replaces everything.
    run(s, hover("other", -1), 0.3f);
    REQUIRE(s.levels().size() == 1);
    CHECK(s.levels()[0].path == "other");
}

TEST_CASE("tooltip state: pinning keeps tooltips while the cursor wanders; unpinning releases them") {
    TooltipState s;
    run(s, hover("k", -1), 0.5f);
    run(s, hover("k/0", 0), 0.3f);
    REQUIRE(s.levels().size() == 2);

    s.set_pinned(true);
    CHECK(s.pinned());
    run(s, hover("", -1), 5.0f);  // far away from everything
    CHECK(s.levels().size() == 2);
    run(s, hover("another", -1), 5.0f);  // hovering another target does not replace the pinned stack
    CHECK(s.levels().size() == 2);
    CHECK(s.levels()[0].path == "k");

    s.set_pinned(false);
    run(s, hover("", -1), 0.5f);
    CHECK(s.levels().empty());

    // Nothing open: pinning has no effect.
    s.set_pinned(true);
    CHECK_FALSE(s.pinned());
}

TEST_CASE("tooltip state: close_all drops everything at once and unpins") {
    TooltipState s;
    run(s, hover("k", -1), 0.5f);
    s.set_pinned(true);
    s.close_all();
    CHECK(s.levels().empty());
    CHECK_FALSE(s.pinned());
    // A pending target must not pop up right after.
    s.set_hover(hover("", -1));
    s.tick(0.05f);
    CHECK(s.levels().empty());
}

TEST_CASE(
    "explanation: the board layout of a tooltip (headline, lines, rows, sum row, footer, close button)") {
    Explanation e{"Казна рода", "+42", "Итоговое значение", {}};
    e.headline = "1 248";
    e.lines = {"Первая строка", "Вторая <строка>"};
    e.total_label = "В месяц";
    e.footer = "Подпись внизу";
    e.tone = TipTone::Warning;
    e.terms.push_back({"Доходы", "+68", "×1,20", {}});
    e.terms.push_back({"Содержание",
                       "\xE2\x88\x92"
                       "26",
                       "",
                       {}});
    const std::string rml = render_tooltip_rml(e, "t:0", 0, TooltipRender{{}, "icons", true});
    const auto has = [&](const char* needle) { return rml.find(needle) != std::string::npos; };
    CHECK(has("class=\"tip tip-float pinned\""));
    CHECK(has("data-tip-close=\"1\""));
    CHECK(has("icons/close@quiet.svg"));
    CHECK(has("class=\"tip-title warn\""));
    CHECK(has("class=\"tip-note\">Итоговое значение"));
    CHECK(has("class=\"tip-headline\">1 248"));
    CHECK(has("Вторая &lt;строка&gt;"));
    CHECK(has("class=\"tip-factor\">×1,20"));
    CHECK(has("tip-value neg\">\xE2\x88\x92"
              "26"));  // the true minus sign tints the value like a hyphen
    CHECK(has("class=\"tip-sum-label\">В месяц"));
    CHECK(has("tip-sum-value pos\">+42"));
    CHECK(has("class=\"tip-footer\">Подпись внизу"));
    CHECK_FALSE(has("class=\"tip-total\""));  // with a sum row the value is not repeated next to the title

    // Without a sum label the value stays next to the title; the wide variant and the unpinned box.
    Explanation plain{"Навык", "9", "", {}};
    plain.wide = true;
    const std::string plain_rml = render_tooltip_rml(plain, "s:0", 0);
    CHECK(plain_rml.find("class=\"tip-total\">9") != std::string::npos);
    CHECK(plain_rml.find("tip-wide") != std::string::npos);
    CHECK(plain_rml.find("pinned") == std::string::npos);
    CHECK(plain_rml.find("tip-sum") == std::string::npos);
}
