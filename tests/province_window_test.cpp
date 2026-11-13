// Province inspector (UI-D06): board geometry, snapshot presentation, tab and action input, and
// unavailable economic data on an offscreen WARP target with the game theme and fonts.

#include "pat/engine/dpi.hpp"
#include "pat/engine/renderer.hpp"
#include "pat/engine/ui.hpp"
#include "pat/ui/province_window.hpp"

#include <doctest/doctest.h>

#ifdef _WIN32

#include <RmlUi/Core.h>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "test_fonts.hpp"
#include "ui_layout_fixture.hpp"
#include "ui_loc_fixture.hpp"
#include "ui_theme_fixture.hpp"

namespace {

using namespace pat::engine;
using pat::ui::EconomyNumber;
using pat::ui::EconomyUnit;
using pat::ui::Explanation;
using pat::ui::ProvinceSnapshot;
using pat::ui::ProvinceWindow;

bool fonts_ok() {
    if (!pat::test::game_fonts_present()) {
        MESSAGE("Game fonts not found, skipping");
        return false;
    }
    return true;
}

EconomyNumber number(std::int64_t value, EconomyUnit unit, const char* label, const char* text) {
    EconomyNumber result;
    result.value = value;
    result.unit = unit;
    result.explanation = Explanation(label, text, "Snapshot explanation", {});
    return result;
}

ProvinceSnapshot snapshot() {
    ProvinceSnapshot s;
    s.economy.available = true;
    s.economy.id = 17;
    s.economy.name = "Pskov";
    s.economy.owner = "Testov dynasty";
    s.economy.population = number(24500, EconomyUnit::People, "Population", "24 500");
    s.economy.control = number(620, EconomyUnit::Permille, "Order", "62%");
    s.economy.income_last_month = number(10000, EconomyUnit::Kopecks, "Last month", "100 rub.");
    s.economy.projected_income = number(12345, EconomyUnit::Kopecks, "Current income", "123.45 rub.");
    s.economy.projected_income.explanation.terms.emplace_back(
        "Land dues", "150 rub.", "", std::vector<Explanation>{});
    s.economy.projected_income.explanation.terms.emplace_back(
        "Costs", "-26.55 rub.", "", std::vector<Explanation>{});
    pat::ui::EconomyGroupSnapshot group;
    group.key = "peasants";
    group.name = "Peasants";
    group.population = number(24500, EconomyUnit::People, "Peasants", "24 500");
    s.economy.groups.push_back(group);
    pat::ui::EconomyBuildingSnapshot building;
    building.key = "mill";
    building.name = "Water mill";
    building.upkeep = number(550, EconomyUnit::Kopecks, "Mill upkeep", "5.50 rub.");
    s.economy.buildings.push_back(building);
    s.subtitle = "North-western holding";
    s.income_change = number(2345, EconomyUnit::Kopecks, "Income change", "+23.45 rub.");
    s.order = "62 / 100";
    s.order_percent = 62;
    s.garrison = "Unavailable";
    s.garrison_explanation.label = "Garrison unavailable";
    s.garrison_explanation.lines = {"The military system has no garrison data."};
    s.state = "Winter adjustment";
    s.state_detail = "Trade income reduced";
    s.state_explanation = Explanation("Season", "Winter", "Current economic rule", {});
    s.owner_name = "Alexei Testov";
    s.owner_detail = "Head of the Testov dynasty";
    s.manage_explanation.label = "Management unavailable";
    s.manage_explanation.lines = {"Management is not implemented yet."};
    s.manage_explanation.tone = pat::ui::TipTone::Warning;
    return s;
}

struct Fixture {
    std::unique_ptr<IRenderer> renderer;
    std::unique_ptr<UiContext> ui;
    std::unique_ptr<ProvinceWindow> window;

    explicit Fixture(PixelSize size = {1920, 1080}) {
        RendererDesc rd;
        rd.size = size;
        rd.software = true;
        auto created = create_renderer(rd);
        REQUIRE(created.has_value());
        renderer = std::move(created).value();
        UiDesc ud;
        ud.size = size;
        ud.ui_scale = design_ui_scale(size);
        ud.font_files = pat::test::game_font_files();
        auto ui_created = UiContext::create(*renderer, nullptr, ud);
        REQUIRE(ui_created.has_value());
        ui = std::move(ui_created).value();
        pat::test::install_game_theme(*ui);
        auto result =
            ProvinceWindow::create(*ui,
                                   std::filesystem::path(PATRIARCH_SPIKE_UI_DIR) / "province.rml",
                                   std::filesystem::path(PATRIARCH_ASSETS_DIR) / "ui" / "patriarch" / "icons",
                                   pat::test::ui_localizer());
        REQUIRE(result.has_value());
        window = std::move(result).value();
        CHECK_FALSE(window->visible());
        window->set_snapshot(snapshot());
        window->show(true);
        ui->update();
    }

    ~Fixture() {
        window.reset();
        ui.reset();
        renderer.reset();
    }

    Rml::Element* by_id(const char* id) const {
        REQUIRE(window->document() != nullptr);
        Rml::Element* e = window->document()->GetElementById(id);
        REQUIRE_MESSAGE(e != nullptr, "no element #" << id);
        return e;
    }

    void click(const char* id) {
        Rml::Element* e = by_id(id);
        REQUIRE(e->IsVisible(true));
        const float x = e->GetAbsoluteLeft() + e->GetOffsetWidth() * 0.5f;
        const float y = e->GetAbsoluteTop() + e->GetOffsetHeight() * 0.5f;
        ui->handle_event(event::MouseMove{x, y, 0, 0});
        ui->update();
        ui->handle_event(event::MouseButton{1, true, 1, x, y});
        ui->handle_event(event::MouseButton{1, false, 1, x, y});
        ui->update();
    }

    void key(Rml::Input::KeyIdentifier key) {
        ui->context()->ProcessKeyDown(key, 0);
        ui->context()->ProcessKeyUp(key, 0);
        ui->update();
    }
};

void check_rect(Rml::Element* e, const pat::test::Layer& layer, float scale) {
    REQUIRE(layer.found);
    CHECK(std::abs(e->GetAbsoluteLeft() - layer.x * scale) <= 1.0);
    CHECK(std::abs(e->GetAbsoluteTop() - layer.y * scale) <= 1.0);
    CHECK(std::abs(e->GetOffsetWidth() - layer.w * scale) <= 1.0);
    CHECK(std::abs(e->GetOffsetHeight() - layer.h * scale) <= 1.0);
}

}  // namespace

TEST_CASE("ProvinceWindow: board 01 geometry follows the design scale and leaves the map clickable") {
    if (!fonts_ok()) {
        return;
    }
    const auto layer = pat::test::find_layer("board_01_map_province.pd", "Province inspector / Новгород");
    for (const PixelSize size : {PixelSize{1920, 1080}, PixelSize{1280, 720}}) {
        INFO(size.w << "x" << size.h);
        Fixture fx(size);
        const float scale = design_ui_scale(size);
        check_rect(fx.by_id("province-inspector"), layer, scale);
        check_rect(fx.by_id("province-close-icon"),
                   pat::test::find_layer("board_01_map_province.pd", "Icon / close"),
                   scale);
        CHECK(fx.by_id("province-income-note")->GetOffsetWidth() >= 320.0f * scale);
        CHECK(fx.by_id("province-manage")->GetAbsoluteTop() +
                  fx.by_id("province-manage")->GetOffsetHeight() <=
              static_cast<float>(size.h));

        const float x = static_cast<float>(size.w) * 0.75f;
        const float y = static_cast<float>(size.h) * 0.5f;
        CHECK_FALSE(fx.ui->handle_event(event::MouseMove{x, y, 0, 0}));
        CHECK_FALSE(fx.ui->handle_event(event::MouseButton{1, true, 1, x, y}));
        CHECK_FALSE(fx.ui->handle_event(event::MouseButton{1, false, 1, x, y}));
    }
}

TEST_CASE("ProvinceWindow: overview uses the snapshot values, owner and unavailable garrison") {
    if (!fonts_ok()) {
        return;
    }
    Fixture fx;
    const ProvinceSnapshot s = snapshot();
    CHECK(fx.by_id("province-name")->GetInnerRML() == s.economy.name);
    CHECK(fx.by_id("province-income")->GetInnerRML() == s.economy.projected_income.explanation.value);
    CHECK(fx.by_id("province-income-change")->GetInnerRML() == s.income_change.explanation.value);
    CHECK(fx.by_id("province-population")->GetInnerRML() == s.economy.population.explanation.value);
    CHECK(fx.by_id("province-order")->GetInnerRML() == s.order);
    CHECK(fx.by_id("province-garrison")->GetInnerRML() == s.garrison);
    CHECK(fx.by_id("province-state")->GetInnerRML() == s.state);
    CHECK(fx.by_id("province-state-detail")->GetInnerRML() == s.state_detail);
    CHECK(fx.by_id("province-owner-name")->GetInnerRML() == s.owner_name);
    CHECK(fx.by_id("province-owner-detail")->GetInnerRML() == s.owner_detail);
    CHECK(fx.by_id("province-order-fill")->GetOffsetWidth() ==
          doctest::Approx(fx.by_id("province-order-track")->GetClientWidth() * 0.62f).epsilon(0.01));

    ProvinceSnapshot next = s;
    next.economy.name = "Novgorod";
    next.economy.projected_income.explanation.value = "81 rub.";
    next.owner_name = "No living dynasty head";
    next.owner_detail = "Testov dynasty";
    next.order = "37 / 100";
    next.order_percent = 37;
    fx.window->set_snapshot(next);
    fx.ui->update();
    CHECK(fx.by_id("province-name")->GetInnerRML() == next.economy.name);
    CHECK(fx.by_id("province-income")->GetInnerRML() == "81 rub.");
    CHECK(fx.by_id("province-owner-name")->GetInnerRML() == next.owner_name);
    CHECK(fx.by_id("province-owner-detail")->GetInnerRML() == next.owner_detail);
    CHECK(fx.by_id("province-order-fill")->GetOffsetWidth() ==
          doctest::Approx(fx.by_id("province-order-track")->GetClientWidth() * 0.37f).epsilon(0.01));
}

TEST_CASE("ProvinceWindow: real tab clicks and keyboard activation switch pages and snapshot keeps tab") {
    if (!fonts_ok()) {
        return;
    }
    Fixture fx;
    CHECK(fx.window->tab() == "overview");
    CHECK(fx.by_id("province-income")->IsVisible(true));
    CHECK_FALSE(fx.by_id("province-income-page")->IsVisible(true));
    fx.click("province-tab-income");
    CHECK(fx.window->tab() == "income");
    CHECK(fx.by_id("province-income-page")->IsVisible(true));
    CHECK_FALSE(fx.by_id("province-overview-page")->IsVisible(true));
    fx.click("province-tab-buildings");
    CHECK(fx.window->tab() == "buildings");
    CHECK(fx.by_id("province-buildings-page")->IsVisible(true));
    CHECK_FALSE(fx.by_id("province-income-page")->IsVisible(true));

    ProvinceSnapshot next = snapshot();
    next.economy.name = "Updated holding";
    fx.window->set_snapshot(next);
    fx.ui->update();
    CHECK(fx.window->tab() == "buildings");
    CHECK(fx.by_id("province-buildings-page")->IsVisible(true));
    CHECK(fx.by_id("province-name")->GetInnerRML() == next.economy.name);
    fx.window->set_tab("unknown");
    CHECK(fx.window->tab() == "buildings");

    REQUIRE(fx.by_id("province-tab-overview")->Focus());
    fx.key(Rml::Input::KI_TAB);
    REQUIRE(fx.ui->context()->GetFocusElement() == fx.by_id("province-tab-income"));
    fx.key(Rml::Input::KI_RETURN);
    CHECK(fx.window->tab() == "income");
    CHECK(fx.by_id("province-income-page")->IsVisible(true));
    fx.click("province-tab-overview");
    CHECK(fx.window->tab() == "overview");
    CHECK(fx.by_id("province-income")->IsVisible(true));
}

TEST_CASE("ProvinceWindow: tooltip roots retain the authoritative snapshot explanations") {
    if (!fonts_ok()) {
        return;
    }
    Fixture fx;
    const ProvinceSnapshot& s = fx.window->snapshot();
    const Explanation* income = fx.window->explanation("province:income");
    REQUIRE(income != nullptr);
    CHECK(*income == s.economy.projected_income.explanation);
    CHECK(income->terms.size() == 2);
    CHECK(fx.by_id("province-income")->GetAttribute<Rml::String>("data-tip", "") == "province:income");
    const char* ids[] = {"province-income-change",
                         "province-population",
                         "province-order",
                         "province-garrison",
                         "province-state",
                         "province-manage"};
    const char* keys[] = {"province:change",
                          "province:population",
                          "province:order",
                          "province:garrison",
                          "province:state",
                          "province:manage"};
    for (std::size_t i = 0; i < std::size(ids); ++i) {
        INFO(keys[i]);
        CHECK(fx.by_id(ids[i])->GetAttribute<Rml::String>("data-tip", "") == keys[i]);
        CHECK(fx.window->explanation(keys[i]) != nullptr);
    }
    REQUIRE(fx.window->explanation("province:garrison") != nullptr);
    CHECK(*fx.window->explanation("province:garrison") == s.garrison_explanation);
    REQUIRE(fx.window->explanation("province:manage") != nullptr);
    CHECK(*fx.window->explanation("province:manage") == s.manage_explanation);
    CHECK(fx.window->explanation("province:group:0") != nullptr);
    CHECK(fx.window->explanation("province:building:0") != nullptr);
    CHECK(fx.window->explanation("province:group:1") == nullptr);
    CHECK(fx.window->explanation("province:building:1") == nullptr);
    CHECK(fx.window->explanation("province:unknown") == nullptr);
}

TEST_CASE("ProvinceWindow: disabled management ignores presses, enabled fires once and close hides") {
    if (!fonts_ok()) {
        return;
    }
    Fixture fx;
    int managed = 0;
    int closed = 0;
    fx.window->set_manage_handler([&] { ++managed; });
    fx.window->set_close_handler([&] { ++closed; });
    fx.ui->update();
    fx.click("province-manage");
    CHECK(managed == 0);

    ProvinceSnapshot enabled = snapshot();
    enabled.manage_available = true;
    fx.window->set_snapshot(enabled);
    fx.ui->update();
    fx.click("province-manage");
    CHECK(managed == 1);
    fx.window->set_manage_handler({});
    fx.ui->update();
    fx.click("province-manage");
    CHECK(managed == 1);

    fx.click("province-close");
    CHECK(closed == 1);
    CHECK_FALSE(fx.window->visible());
    CHECK_FALSE(fx.by_id("province-inspector")->IsVisible(true));
}

TEST_CASE(
    "ProvinceWindow: unavailable economy retains the name and hides stale economic values on every tab") {
    if (!fonts_ok()) {
        return;
    }
    Fixture fx;
    ProvinceSnapshot missing;
    missing.economy.name = "Holding without economic data";
    fx.window->set_snapshot(missing);
    fx.ui->update();
    CHECK(fx.window->visible());
    CHECK(fx.by_id("province-name")->GetInnerRML() == missing.economy.name);
    for (const char* tab : {"overview", "income", "buildings"}) {
        INFO(tab);
        fx.window->set_tab(tab);
        fx.ui->update();
        CHECK(fx.by_id("province-unavailable")->IsVisible(true));
        CHECK_FALSE(fx.by_id("province-unavailable")->GetInnerRML().empty());
        for (const char* id : {"province-income",
                               "province-income-change",
                               "province-population",
                               "province-order",
                               "province-income-page",
                               "province-buildings-page"}) {
            CHECK_FALSE(fx.by_id(id)->IsVisible(true));
        }
    }

    fx.window->set_snapshot(snapshot());
    fx.ui->update();
    CHECK(fx.window->tab() == "buildings");
    CHECK(fx.by_id("province-buildings-page")->IsVisible(true));
    CHECK_FALSE(fx.by_id("province-unavailable")->IsVisible(true));
}

#endif  // _WIN32
