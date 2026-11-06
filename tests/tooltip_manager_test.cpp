// Nested tooltips over RmlUi (E3-T08) on an offscreen WARP target. Needs Arial (skipped without it).

#include "pat/engine/renderer.hpp"
#include "pat/engine/ui.hpp"
#include "pat/ui/character_window.hpp"
#include "pat/ui/tooltip_manager.hpp"

#include <doctest/doctest.h>

#ifdef _WIN32

#include <RmlUi/Core.h>

#include <filesystem>
#include <string>

namespace {

using namespace pat::engine;
using namespace pat::ui;

bool have_font() {
    return std::filesystem::exists("C:/Windows/Fonts/arial.ttf");
}

struct Fixture {
    std::filesystem::path dir;
    int width;
    int height;
    std::unique_ptr<IRenderer> renderer;
    std::unique_ptr<UiContext> ui;
    std::unique_ptr<CharacterWindow> window;
    std::unique_ptr<TooltipManager> tooltips;
    CharacterSnapshot snapshot;
    std::size_t skill_with_depth = 0;

    explicit Fixture(const char* name, int w = 1280, int h = 720, float scale = 1.0f) : width(w), height(h) {
        dir = std::filesystem::temp_directory_path() / name;
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);
        const std::filesystem::path source(PATRIARCH_SPIKE_UI_DIR);
        for (const char* f : {"character.rml", "character.rcss", "tooltip.rcss"}) {
            std::filesystem::copy_file(source / f, dir / f);
        }

        // A character with a skill that has a three-level breakdown (skill -> education -> mentor -> ...).
        for (std::uint32_t seed = 1; seed < 200; ++seed) {
            snapshot = make_test_character(seed, 10);
            for (std::size_t i = 0; i < kSkillCount; ++i) {
                for (const Explanation& term : snapshot.skill_explanations[i].terms) {
                    if (!term.terms.empty() && !term.terms.front().terms.empty()) {
                        skill_with_depth = i;
                        goto found;
                    }
                }
            }
        }
        FAIL("no test character with a three-level explanation");
    found:
        RendererDesc rd;
        rd.size = {w, h};
        rd.software = true;
        auto created = create_renderer(rd);
        REQUIRE(created.has_value());
        renderer = std::move(created).value();

        UiDesc ud;
        ud.size = {w, h};
        ud.ui_scale = scale;
        ud.font_files.emplace_back("C:/Windows/Fonts/arial.ttf");
        ud.font_files.emplace_back("C:/Windows/Fonts/times.ttf");
        auto ui_created = UiContext::create(*renderer, nullptr, ud);
        REQUIRE(ui_created.has_value());
        ui = std::move(ui_created).value();

        auto cw = CharacterWindow::create(*ui, dir / "character.rml");
        REQUIRE(cw.has_value());
        window = std::move(cw).value();
        window->set_snapshot(snapshot);

        auto tm = TooltipManager::create(*ui, dir);
        REQUIRE(tm.has_value());
        tooltips = std::move(tm).value();
        tooltips->set_provider([this](std::string_view key) { return window->explanation(key); });
        step(0.0f);
    }

    ~Fixture() {
        tooltips.reset();
        window.reset();
        ui.reset();
        renderer.reset();
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }

    /// One frame of `dt` seconds with the cursor wherever it was last put.
    void step(float dt) {
        window->update();
        tooltips->update(dt);
        ui->update();
    }

    void run(float seconds) {
        for (float t = 0.0f; t < seconds - 1e-4f; t += 0.05f) {
            step(0.05f);
        }
    }

    void move_cursor(float x, float y) {
        const Event e = event::MouseMove{x, y, 0, 0};
        ui->handle_event(e);
        tooltips->handle_event(e);
        cursor_x = x;
        cursor_y = y;
        step(0.0f);  // lets RmlUi work out the hovered element
    }

    void key(int scancode) {
        const Event e = event::Key{scancode, true, false, 0};
        ui->handle_event(e);
        tooltips->handle_event(e);
    }

    static Rml::Element* center_target(Rml::Element* el, float& x, float& y) {
        x = el->GetAbsoluteLeft() + el->GetOffsetWidth() * 0.5f;
        y = el->GetAbsoluteTop() + el->GetOffsetHeight() * 0.5f;
        return el;
    }

    /// Moves the cursor to the centre of the n-th *shown* element with the given class in a document.
    void hover_class(Rml::ElementDocument* doc, const char* cls, std::size_t n, bool nested_only = false) {
        Rml::ElementList list;
        doc->GetElementsByClassName(list, cls);
        std::size_t seen = 0;
        for (Rml::Element* el : list) {
            if (!el->IsVisible(true) || (nested_only && !el->IsClassSet("nested"))) {
                continue;
            }
            if (seen++ == n) {
                float x = 0.0f;
                float y = 0.0f;
                center_target(el, x, y);
                move_cursor(x, y);
                return;
            }
        }
        FAIL("element not found");
    }

    std::string text_of(Rml::ElementDocument* doc) { return doc->GetInnerRML(); }

    float cursor_x = 0.0f;
    float cursor_y = 0.0f;
};

}  // namespace

TEST_CASE("TooltipManager: hovering a skill opens a generated tooltip after a delay") {
    if (!have_font()) {
        MESSAGE("Arial not found, skipping");
        return;
    }
    Fixture fx("patriarch_tip_open");
    fx.hover_class(fx.window->document(), "skill", fx.skill_with_depth);

    fx.run(0.2f);
    CHECK(fx.tooltips->open_levels() == 0);  // not yet
    fx.run(0.3f);
    REQUIRE(fx.tooltips->open_levels() == 1);

    // The tooltip is the generated breakdown of this very skill.
    const Explanation& e = fx.snapshot.skill_explanations[fx.skill_with_depth];
    Rml::ElementDocument* doc = fx.tooltips->level_document(0);
    REQUIRE(doc != nullptr);
    const std::string html = fx.text_of(doc);
    CHECK(html.find(e.label) != std::string::npos);
    for (const Explanation& term : e.terms) {
        CHECK(html.find(term.label) != std::string::npos);
    }
    Rml::ElementList rows;
    doc->GetElementsByClassName(rows, "tip-row");
    CHECK(rows.size() == e.terms.size());

    // Placed inside the screen and off the cursor.
    const RectF rect = fx.tooltips->level_rect(0);
    CHECK(rect.x >= 0.0f);
    CHECK(rect.y >= 0.0f);
    CHECK(rect.right() <= 1280.0f);
    CHECK(rect.bottom() <= 720.0f);
    CHECK_FALSE(rect.contains(fx.cursor_x, fx.cursor_y));
}

TEST_CASE("TooltipManager: terms open nested tooltips up to three levels, then the stack unwinds") {
    if (!have_font()) {
        return;
    }
    Fixture fx("patriarch_tip_nested");
    fx.hover_class(fx.window->document(), "skill", fx.skill_with_depth);
    fx.run(0.5f);
    REQUIRE(fx.tooltips->open_levels() == 1);

    // Move into the tooltip, onto a term that has terms of its own.
    Rml::ElementDocument* level0 = fx.tooltips->level_document(0);
    Rml::ElementList rows0;
    level0->GetElementsByClassName(rows0, "nested");
    REQUIRE(!rows0.empty());
    fx.hover_class(level0, "tip-row", 0, /*nested_only=*/true);
    fx.run(0.1f);
    CHECK(fx.tooltips->open_levels() == 1);  // the nested delay has not passed yet
    fx.run(0.3f);
    REQUIRE(fx.tooltips->open_levels() == 2);

    // Level 1 does not cover level 0, and is inside the screen.
    const RectF r0 = fx.tooltips->level_rect(0);
    const RectF r1 = fx.tooltips->level_rect(1);
    CHECK_FALSE(r0.intersects(r1));
    CHECK(r1.x >= 0.0f);
    CHECK(r1.right() <= 1280.0f);
    CHECK(r1.bottom() <= 720.0f);
    CHECK_FALSE(r1.contains(fx.cursor_x, fx.cursor_y));

    // Third level: the mentor's own breakdown.
    Rml::ElementDocument* level1 = fx.tooltips->level_document(1);
    fx.hover_class(level1, "tip-row", 0, /*nested_only=*/true);
    fx.run(0.5f);
    REQUIRE(fx.tooltips->open_levels() == 3);
    CHECK_FALSE(fx.tooltips->level_rect(2).intersects(fx.tooltips->level_rect(1)));

    // Moving the cursor back to level 0's own rows closes level 2 (and 1 follows once nothing keeps it).
    Rml::ElementDocument* level0_again = fx.tooltips->level_document(0);
    fx.hover_class(level0_again, "tip-head", 0);  // in level 0, on no term with a tooltip
    fx.run(0.6f);
    CHECK(fx.tooltips->open_levels() == 1);

    // Leaving everything closes the last level.
    fx.move_cursor(5.0f, 700.0f);
    fx.run(0.6f);
    CHECK(fx.tooltips->open_levels() == 0);
}

TEST_CASE("TooltipManager: Alt pins tooltips, Esc closes them") {
    if (!have_font()) {
        return;
    }
    Fixture fx("patriarch_tip_pin");
    fx.hover_class(fx.window->document(), "skill", 1);
    fx.run(0.5f);
    REQUIRE(fx.tooltips->open_levels() == 1);

    fx.key(226);  // left Alt
    CHECK(fx.tooltips->pinned());
    fx.move_cursor(5.0f, 700.0f);
    fx.run(2.0f);
    CHECK(fx.tooltips->open_levels() == 1);  // still there although the cursor is far away

    fx.key(226);  // unpin
    CHECK_FALSE(fx.tooltips->pinned());
    fx.run(0.6f);
    CHECK(fx.tooltips->open_levels() == 0);

    // Esc closes immediately.
    fx.hover_class(fx.window->document(), "skill", 2);
    fx.run(0.5f);
    REQUIRE(fx.tooltips->open_levels() == 1);
    fx.key(41);
    CHECK(fx.tooltips->open_levels() == 0);
}

TEST_CASE("TooltipManager: tooltips stay on screen near the edges and on a small screen") {
    if (!have_font()) {
        return;
    }
    // A small viewport forces the tooltip to flip and be clamped; the character window is right-aligned.
    Fixture fx("patriarch_tip_edges", 640, 400);
    Rml::ElementDocument* doc = fx.window->document();
    for (std::size_t i = 0; i < kSkillCount; ++i) {
        fx.hover_class(doc, "skill", i);
        fx.run(0.5f);
        REQUIRE(fx.tooltips->open_levels() >= 1);
        const RectF rect = fx.tooltips->level_rect(0);
        CHECK(rect.x >= 0.0f);
        CHECK(rect.y >= 0.0f);
        CHECK(rect.right() <= 640.0f);
        CHECK(rect.bottom() <= 400.0f);
        CHECK_FALSE(rect.contains(fx.cursor_x, fx.cursor_y));
        fx.move_cursor(2.0f, 2.0f);
        fx.run(0.6f);
        CHECK(fx.tooltips->open_levels() == 0);
    }
}

TEST_CASE("TooltipManager: no provider or unknown keys mean no tooltip") {
    if (!have_font()) {
        return;
    }
    Fixture fx("patriarch_tip_none");
    fx.tooltips->set_provider({});
    fx.hover_class(fx.window->document(), "skill", 0);
    fx.run(1.0f);
    CHECK(fx.tooltips->open_levels() == 0);

    fx.tooltips->set_provider([](std::string_view) -> const Explanation* { return nullptr; });
    fx.run(1.0f);
    CHECK(fx.tooltips->open_levels() == 0);

    // Elements without data-tip never open anything.
    fx.tooltips->set_provider([&](std::string_view key) { return fx.window->explanation(key); });
    fx.hover_class(fx.window->document(), "bar", 0);
    fx.run(1.0f);
    // (the skill row contains the bar, so it does have a tooltip: hovering inside a tipped element counts)
    CHECK(fx.tooltips->open_levels() <= 1);
    fx.move_cursor(2.0f, 2.0f);
    fx.run(0.6f);
    CHECK(fx.tooltips->open_levels() == 0);
}

TEST_CASE("TooltipManager: scaled UI keeps tooltips usable (200%)") {
    if (!have_font()) {
        return;
    }
    Fixture fx("patriarch_tip_scale", 1600, 900, 2.0f);
    fx.hover_class(fx.window->document(), "skill", 0);
    fx.run(0.6f);
    REQUIRE(fx.tooltips->open_levels() == 1);
    const RectF rect = fx.tooltips->level_rect(0);
    CHECK(rect.right() <= 1600.0f);
    CHECK(rect.bottom() <= 900.0f);
    CHECK_FALSE(rect.contains(fx.cursor_x, fx.cursor_y));
    // 280dp wide at 200% is 560 px.
    CHECK(rect.w == doctest::Approx(560.0f + 2 * 20.0f + 2 * 2.0f).epsilon(0.1));
}

#endif  // _WIN32
