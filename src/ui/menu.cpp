#include "pat/ui/menu.hpp"

#include "pat/core/log.hpp"

#include <RmlUi/Core.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <system_error>
#include <utility>

namespace pat::ui {

namespace {

constexpr const char* kModelName = "menu";

using loc::Arg;

// Layout numbers of the main menu, read from boards 01 and 02 of data/ui/layout/menu_boards.pd (design
// pixels).
constexpr int kPrimaryHeight = 68;
constexpr int kItemHeight = 54;
constexpr int kOverviewColumns = 4;
constexpr int kOverviewLeft = 40;
constexpr int kOverviewTop = 260;
constexpr int kOverviewPitchX = 470;
constexpr int kOverviewPitchY = 214;

// Cards of "Новая игра" (board 03): three houses and "Основать свой род" in a grid of two, 510x236.
struct Place {
    int left;
    int top;
    int width;
};
constexpr std::array<Place, 4> kHousePlaces = {
    {{124, 340, 510}, {662, 340, 510}, {124, 600, 510}, {662, 600, 510}}};
// Cards of "Цель партии" (board 05): by the index of the goal.
constexpr std::array<Place, 5> kGoalPlaces = {
    {{128, 340, 506}, {666, 340, 506}, {128, 510, 506}, {666, 510, 506}, {128, 680, 1044}}};
constexpr int kProgressWidth = 1660;  // the track of the world screen
constexpr int kPreviewPercent = 68;   // what the overview shows for "Загрузка мира"

struct ItemRow {
    int index = 0;
    Rml::String id;
    Rml::String label;
    Rml::String note;
    Rml::String top;               // "521dp"
    Rml::String height;            // "68dp"
    Rml::String note_top = "0dp";  // relative to the item
    bool primary = false;
    bool enabled = true;
    bool note_save = false;  // the line under "Продолжить" is larger than the hint under a disabled entry
};

struct SaveRow {
    int index = 0;
    Rml::String name;
    Rml::String badge;
    Rml::String line1;
    Rml::String line2;
    Rml::String icon;
    bool ok = true;
};

struct SummaryRow {
    Rml::String house;
    Rml::String date;
    Rml::String kind;
    Rml::String head;
    Rml::String treasury;
};

struct HouseRow {
    int index = 0;
    Rml::String name;
    Rml::String line;
    Rml::String note;
    Rml::String badge;
    Rml::String icon;
    Rml::String left;
    Rml::String top;
    bool custom = false;  // the card "Основать свой род"
};

struct HouseSummary {
    Rml::String name;
    Rml::String head;
    Rml::String head_line;
    Rml::String members;
    Rml::String treasury;
    Rml::String prestige;
    Rml::String heir;
    Rml::String portrait = "none";  // decorator of the portrait
};

struct GoalRow {
    int index = 0;
    Rml::String title;
    Rml::String line;
    Rml::String icon;
    Rml::String left;
    Rml::String top;
    Rml::String width;
};

struct GoalSummary {
    Rml::String house;
    Rml::String head;
    Rml::String date;
    Rml::String goal;
    Rml::String mode;
    Rml::String treasury;
};

struct CustomFields {
    Rml::String surname;
    Rml::String house;
    Rml::String name;
    Rml::String patronymic;
    Rml::String motto;
    Rml::String look;
    Rml::String faith;
};

struct CustomSummary {
    Rml::String line1;
    Rml::String line2;
    Rml::String motto;
    Rml::String portrait = "none";
};

struct WorldView {
    Rml::String title;
    Rml::String house_line;
    Rml::String stage0;
    Rml::String stage1;
    Rml::String stage2;
    Rml::String percent;
    Rml::String bar = "0dp";  // width of the progress value, "1129dp"
    Rml::String enter;
    int stage = 0;
    bool ready = false;
};

struct CardRow {
    int index = 0;
    Rml::String number;
    Rml::String title;
    Rml::String caption;
    Rml::String left;
    Rml::String top;
    Rml::String hint;
    bool enabled = true;
};

/// What a card of the overview page opens.
enum class CardTarget { MainFirst, MainSave, Load, LoadEmpty, NewGame, Custom, World, Soon };

struct CardSpec {
    const char* key;
    CardTarget target;
};

// The eight cards of the board "Обзор стартовых экранов": the ones whose screens do not exist yet are "Soon".
constexpr std::array<CardSpec, 8> kCards = {{
    {"first", CardTarget::MainFirst},
    {"continue", CardTarget::MainSave},
    {"new", CardTarget::NewGame},
    {"custom", CardTarget::Custom},
    {"saves", CardTarget::Load},
    {"empty", CardTarget::LoadEmpty},
    {"settings", CardTarget::Soon},
    {"world", CardTarget::World},
}};

std::string dp(int value) {
    return std::to_string(value) + "dp";
}

const char* screen_name(MenuScreen screen) {
    switch (screen) {
        case MenuScreen::Main:
            return "main";
        case MenuScreen::Load:
            return "load";
        case MenuScreen::About:
            return "about";
        case MenuScreen::Overview:
            return "overview";
        case MenuScreen::NewGame:
            return "newgame";
        case MenuScreen::Custom:
            return "custom";
        case MenuScreen::Goal:
            return "goal";
        case MenuScreen::World:
            return "world";
    }
    return "main";
}

const char* dialog_name(MenuDialog dialog) {
    switch (dialog) {
        case MenuDialog::None:
            return "";
        case MenuDialog::Exit:
            return "exit";
        case MenuDialog::SaveUnavailable:
            return "unavailable";
    }
    return "";
}

}  // namespace

// ---- navigator -------------------------------------------------------------------------------

void MenuNavigator::reset(MenuScreen root) {
    stack_.assign(1, root);
    dialog_ = MenuDialog::None;
}

void MenuNavigator::push(MenuScreen screen) {
    if (stack_.back() != screen) {
        stack_.push_back(screen);
    }
}

bool MenuNavigator::pop() {
    if (stack_.size() <= 1) {
        return false;
    }
    stack_.pop_back();
    return true;
}

MenuBack MenuNavigator::back() {
    if (dialog_ != MenuDialog::None) {
        dialog_ = MenuDialog::None;
        return MenuBack::ClosedDialog;
    }
    if (pop()) {
        return MenuBack::WentBack;
    }
    if (top() == MenuScreen::Main) {
        dialog_ = MenuDialog::Exit;
        return MenuBack::AskedExit;
    }
    return MenuBack::Nothing;
}

// ---- placeholder data ------------------------------------------------------------------------

MenuSnapshot make_placeholder_menu(const loc::Localizer& loc) {
    MenuSave base;
    base.house = loc.format("ui.menu.placeholder.house");
    base.house_gen = loc.format("ui.menu.placeholder.house_gen");
    base.date = loc.format("ui.menu.placeholder.date");
    base.season = loc.format("ui.menu.placeholder.season");
    base.head = loc.format("ui.menu.placeholder.head");
    base.treasury = loc.format("ui.menu.placeholder.treasury");

    MenuSnapshot snapshot;
    snapshot.placeholder = true;
    MenuSave autosave = base;
    autosave.id = "autosave";
    autosave.kind = MenuSaveKind::Auto;
    MenuSave manual = base;
    manual.id = "manual-1";
    manual.kind = MenuSaveKind::Manual;
    manual.name = loc.format("ui.menu.placeholder.manual_name");
    MenuSave newer = base;
    newer.id = "newer";
    newer.kind = MenuSaveKind::Manual;
    newer.name = loc.format("ui.menu.placeholder.newer_name");
    newer.compatible = false;
    snapshot.saves = {autosave, manual, newer};
    return snapshot;
}

MenuNewGame make_placeholder_new_game(const loc::Localizer& loc) {
    struct HouseSpec {
        const char* id;
        const char* icon;
        int age;
        int members;
        const char* portrait;
    };
    constexpr std::array<HouseSpec, 3> specs = {
        {{"golitsyn", "shield", 47, 12, "portrait_head_of_dynasty.jpg"},
         {"sheremetev", "crown", 52, 9, "portrait_head_of_dynasty.jpg"},
         {"dolgorukov", "dynasty", 44, 14, "portrait_head_of_dynasty.jpg"}}};
    const auto key = [&](const char* id, const char* field) {
        return loc.format(std::string("ui.menu.placeholder.house.") + id + "." + field);
    };
    MenuNewGame data;
    data.placeholder = true;
    for (const HouseSpec& s : specs) {
        MenuHouse house;
        house.id = s.id;
        house.name = key(s.id, "name");
        house.name_gen = key(s.id, "name_gen");
        house.line = key(s.id, "line");
        house.badge = key(s.id, "badge");
        house.icon = s.icon;
        house.region = key(s.id, "region");
        house.head = key(s.id, "head");
        house.head_age = s.age;
        house.members = s.members;
        house.treasury = key(s.id, "treasury");
        house.prestige = key(s.id, "prestige");
        house.heir = key(s.id, "heir");
        house.portrait = s.portrait;
        data.houses.push_back(std::move(house));
    }
    constexpr std::array<std::array<const char*, 2>, 5> goals = {{{"survive", "shield"},
                                                                  {"office", "crown"},
                                                                  {"dynasty", "dynasty"},
                                                                  {"throne", "sword"},
                                                                  {"sandbox", "globe"}}};
    for (const auto& g : goals) {
        MenuGoal goal;
        goal.id = g[0];
        goal.icon = g[1];
        goal.title = loc.format(std::string("ui.menu.goal.") + g[0] + ".title");
        goal.line = loc.format(std::string("ui.menu.goal.") + g[0] + ".line");
        data.goals.push_back(std::move(goal));
    }
    data.default_goal = 2;  // "Величие рода"
    data.start_date = loc.format("ui.menu.placeholder.start_date");
    data.custom_defaults.surname = loc.format("ui.menu.placeholder.custom.surname");
    data.custom_defaults.house_name = loc.format("ui.menu.placeholder.custom.house");
    data.custom_defaults.name = loc.format("ui.menu.placeholder.custom.name");
    data.custom_defaults.patronymic = loc.format("ui.menu.placeholder.custom.patronymic");
    data.custom_defaults.motto = loc.format("ui.menu.placeholder.custom.motto");
    data.looks = {loc.format("ui.menu.placeholder.custom.look_a"),
                  loc.format("ui.menu.placeholder.custom.look_b")};
    data.faiths = {loc.format("ui.menu.placeholder.custom.faith_a"),
                   loc.format("ui.menu.placeholder.custom.faith_b")};
    data.custom_treasury = loc.format("ui.menu.placeholder.custom.treasury");
    data.custom_portrait = "portrait_ivan_golitsyn.jpg";
    return data;
}

// ---- window ----------------------------------------------------------------------------------

struct MenuWindow::Impl {
    engine::UiContext* ui = nullptr;
    const loc::Localizer* loc = nullptr;
    Rml::DataModelHandle handle;
    std::string source_url;
    MenuSnapshot snapshot;
    std::optional<MenuSnapshot> preview;  // the overview shows a screen with other data than the real ones
    MenuActions actions;
    MenuNavigator nav;
    bool visible = false;
    std::string pending_focus;

    // ---- bound variables (names are used by the document)
    int rev = 0;
    Rml::String screen = "main";
    Rml::String dialog;
    Rml::String icons;
    Rml::String decor;
    Rml::String art_deco;
    Rml::String preview_deco;
    Rml::String lead;
    bool show_hint = false;
    std::vector<ItemRow> items;
    int sel = 0;
    Rml::String page_caption;
    Rml::String page_title;
    Rml::String page_subtitle;
    bool has_saves = false;
    std::vector<SaveRow> saves;
    int sel_save = 0;
    bool has_sel = false;
    SummaryRow sum;
    std::vector<CardRow> cards;

    // ---- new game, own house, goal, world (part 2)
    MenuNewGame ng;
    MenuCustomHouse cust;
    int sel_house = 0;
    int sel_goal = 0;
    bool goal_custom = false;  // the goal screen is about the own house
    MenuWorldKind world_kind = MenuWorldKind::NewGame;
    int percent = 0;
    int stage = 0;
    bool world_preview = false;  // the overview shows the world screen: no callbacks
    std::string world_house;     // "Род Голицыных · 1 января 1703"
    bool paged = false;          // the page frame (panel, title, back button) is shown
    Rml::String back_label;
    Rml::String next_label;
    Rml::String next_icon;
    bool next_ok = true;
    bool cust_ok = true;
    Rml::String chosen;
    Rml::String art_dir;
    bool has_house = false;
    std::vector<HouseRow> houses;
    HouseSummary hs;
    std::vector<GoalRow> goals;
    GoalSummary gs;
    CustomFields cf;
    CustomSummary cs;
    WorldView wv;

    /// The keys of the menu. Heard in the capture phase on the document, so that Esc also works while a text
    /// input (which keeps the keys it uses) holds the focus.
    struct KeyListener : Rml::EventListener {
        Impl* owner = nullptr;
        void ProcessEvent(Rml::Event& ev) override { owner->handle_key(ev); }
    } key_listener;

    void handle_key(Rml::Event& ev) {
        const auto key = static_cast<Rml::Input::KeyIdentifier>(
            ev.GetParameter<int>("key_identifier", static_cast<int>(Rml::Input::KI_UNKNOWN)));
        if (key == Rml::Input::KI_ESCAPE) {
            if (do_back() != MenuBack::Nothing) {
                ev.StopPropagation();
            }
        } else if ((key == Rml::Input::KI_UP || key == Rml::Input::KI_DOWN || key == Rml::Input::KI_LEFT ||
                    key == Rml::Input::KI_RIGHT) &&
                   nav.dialog() == MenuDialog::None &&
                   (nav.top() == MenuScreen::NewGame || nav.top() == MenuScreen::Goal)) {
            do_grid(key);
            ev.StopPropagation();
        } else if ((key == Rml::Input::KI_UP || key == Rml::Input::KI_DOWN) &&
                   nav.dialog() == MenuDialog::None &&
                   (nav.top() == MenuScreen::Main || nav.top() == MenuScreen::Load)) {
            do_move(key == Rml::Input::KI_UP ? -1 : 1);
            ev.StopPropagation();
        }
    }

    [[nodiscard]] const MenuSnapshot& data() const { return preview ? *preview : snapshot; }

    [[nodiscard]] Rml::ElementDocument* find_document() const {
        Rml::Context* ctx = ui->context();
        for (int i = 0; i < ctx->GetNumDocuments(); ++i) {
            Rml::ElementDocument* doc = ctx->GetDocument(i);
            if (doc != nullptr && doc->GetSourceURL() == source_url) {
                return doc;
            }
        }
        return nullptr;
    }

    [[nodiscard]] std::string text(std::string_view key, std::initializer_list<Arg> args = {}) const {
        return loc->format(key, args);
    }

    [[nodiscard]] std::string kind_label(MenuSaveKind kind) const {
        return text(kind == MenuSaveKind::Auto ? "ui.menu.kind.auto" : "ui.menu.kind.manual");
    }

    void call(const std::function<void()>& action, const char* what) const {
        if (action) {
            action();
        } else {
            PAT_LOG_INFO("menu", "{}: no handler yet (stub)", what);
        }
    }

    // ---- building the rows

    void build_items() {
        struct Spec {
            const char* id;
            int top;
            int height;
        };
        const MenuSnapshot& d = data();
        const bool save = !d.saves.empty();
        std::vector<Spec> specs;
        if (save) {
            specs = {{"continue", 437, kPrimaryHeight},
                     {"new", 578, kItemHeight},
                     {"load", 650, kItemHeight},
                     {"settings", 722, kItemHeight},
                     {"about", 794, kItemHeight},
                     {"exit", 866, kItemHeight}};
        } else {
            specs = {{"new", 521, kPrimaryHeight},
                     {"load", 616, kItemHeight},
                     {"settings", 726, kItemHeight},
                     {"about", 798, kItemHeight},
                     {"exit", 870, kItemHeight}};
        }
        items.clear();
        for (const Spec& s : specs) {
            ItemRow row;
            row.index = static_cast<int>(items.size());
            row.id = s.id;
            row.label = text(std::string("ui.menu.item.") + s.id);
            row.top = dp(s.top);
            row.height = dp(s.height);
            row.primary = s.height == kPrimaryHeight;
            const std::string id = s.id;
            if (id == "continue") {
                const MenuSave& newest = d.saves.front();
                row.note = text("ui.menu.continue.line",
                                {Arg("house", newest.house),
                                 Arg("date", newest.date),
                                 Arg("kind", kind_label(newest.kind))});
                row.note_top = dp(84);
                row.note_save = true;
            } else if (id == "load" && !save) {
                row.enabled = false;
                row.note = text("ui.menu.item.no_saves");
                row.note_top = dp(61);
            }
            items.push_back(std::move(row));
        }
        if (sel < 0 || sel >= static_cast<int>(items.size()) ||
            !items[static_cast<std::size_t>(sel)].enabled) {
            sel = 0;
        }
        lead = text(save ? "ui.menu.main.lead.save" : "ui.menu.main.lead.first");
        show_hint = !save;
    }

    void build_saves() {
        const MenuSnapshot& d = data();
        saves.clear();
        for (const MenuSave& s : d.saves) {
            SaveRow row;
            row.index = static_cast<int>(saves.size());
            row.ok = s.compatible;
            row.name = s.name.empty() ? kind_label(s.kind) : s.name;
            row.icon = icons + (s.kind == MenuSaveKind::Auto ? "/scroll@green.svg" : "/quill@green.svg");
            if (s.compatible) {
                row.badge = text("ui.menu.load.compatible");
                row.line1 = text("ui.menu.load.row", {Arg("house", s.house), Arg("date", s.date)});
                row.line2 = text(s.kind == MenuSaveKind::Auto ? "ui.menu.load.last" : "ui.menu.kind.manual");
            } else {
                row.icon = icons + (s.kind == MenuSaveKind::Auto ? "/scroll@muted.svg" : "/quill@muted.svg");
                row.badge = text("ui.menu.load.blocked");
                row.line1 = text("ui.menu.load.newer");
                row.line2 = text("ui.menu.load.unavailable");
            }
            saves.push_back(std::move(row));
        }
        has_saves = !saves.empty();
        if (sel_save < 0 || sel_save >= static_cast<int>(saves.size())) {
            sel_save = 0;
        }
        build_summary();
    }

    void build_summary() {
        const MenuSnapshot& d = data();
        has_sel = sel_save >= 0 && static_cast<std::size_t>(sel_save) < d.saves.size();
        if (!has_sel) {
            sum = {};
            return;
        }
        const MenuSave& s = d.saves[static_cast<std::size_t>(sel_save)];
        sum.house = text("ui.menu.load.summary.house", {Arg("house", s.house_gen)});
        sum.date = text("ui.menu.load.summary.date", {Arg("date", s.date), Arg("season", s.season)});
        sum.kind = kind_label(s.kind);
        sum.head = s.head;
        sum.treasury = s.treasury;
    }

    void build_page() {
        const bool empty = data().saves.empty();
        paged = nav.top() != MenuScreen::Main && nav.top() != MenuScreen::Overview &&
                nav.top() != MenuScreen::World;
        back_label = text("ui.menu.button.main");
        switch (nav.top()) {
            case MenuScreen::About:
                page_caption = text("ui.menu.about.caption");
                page_title = text("ui.menu.about.title");
                page_subtitle = text("ui.menu.about.subtitle");
                break;
            case MenuScreen::NewGame:
                page_caption = text("ui.menu.newgame.caption");
                page_title = text("ui.menu.newgame.title");
                page_subtitle = text("ui.menu.newgame.subtitle");
                break;
            case MenuScreen::Custom:
                page_caption = text("ui.menu.custom.caption");
                page_title = text("ui.menu.custom.title");
                page_subtitle = text("ui.menu.custom.subtitle");
                back_label = text("ui.menu.button.to_houses");
                break;
            case MenuScreen::Goal:
                page_caption = text("ui.menu.goal.caption");
                page_title = text("ui.menu.goal.title");
                page_subtitle = text("ui.menu.goal.subtitle");
                back_label = text("ui.menu.button.back");
                break;
            case MenuScreen::World:
                page_caption =
                    text(wv.ready ? "ui.menu.world.caption.ready" : "ui.menu.world.caption.prepare");
                break;
            default:
                page_caption = text(empty ? "ui.menu.load.caption.empty" : "ui.menu.load.caption");
                page_title = text(empty ? "ui.menu.load.title.empty" : "ui.menu.load.title");
                page_subtitle = text(empty ? "ui.menu.load.subtitle.empty" : "ui.menu.load.subtitle");
                break;
        }
        const bool goal = nav.top() == MenuScreen::Goal;
        next_label = text(goal ? "ui.menu.button.start" : "ui.menu.button.next_goal");
        next_icon = icons + (goal ? "/play@gold.svg" : "/chevron@gold.svg");
        chosen = text("ui.menu.newgame.chosen");
        update_next();
    }

    void update_next() {
        switch (nav.top()) {
            case MenuScreen::NewGame:
                next_ok = has_house;
                break;
            case MenuScreen::Custom:
                next_ok = cust_ok;
                break;
            case MenuScreen::Goal:
                next_ok = !goals.empty();
                break;
            default:
                next_ok = true;
                break;
        }
    }

    // ---- new game, own house, goal, world

    [[nodiscard]] std::size_t house_count() const {
        return std::min(ng.houses.size(), kHousePlaces.size() - 1);
    }
    [[nodiscard]] std::size_t goal_count() const { return std::min(ng.goals.size(), kGoalPlaces.size()); }
    [[nodiscard]] const MenuHouse* house_at(int index) const {
        if (index < 0 || static_cast<std::size_t>(index) >= house_count()) {
            return nullptr;
        }
        return &ng.houses[static_cast<std::size_t>(index)];
    }

    [[nodiscard]] std::string art_image(const std::string& file) const {
        return "image(" + art_dir + "/" + file + " cover)";
    }

    void build_houses() {
        houses.clear();
        const std::size_t count = house_count();
        for (std::size_t i = 0; i < count; ++i) {
            const MenuHouse& h = ng.houses[i];
            HouseRow row;
            row.index = static_cast<int>(i);
            row.name = h.name;
            row.line = h.line;
            row.note = h.region;
            row.badge = h.badge;
            row.icon = icons + "/" + h.icon + "@green.svg";
            row.left = dp(kHousePlaces[i].left);
            row.top = dp(kHousePlaces[i].top);
            houses.push_back(std::move(row));
        }
        HouseRow custom;  // "Основать свой род" is always the last card
        custom.index = static_cast<int>(count);
        custom.name = text("ui.menu.newgame.custom.name");
        custom.line = text("ui.menu.newgame.custom.line");
        custom.note = text("ui.menu.newgame.custom.note");
        custom.badge = text("ui.menu.newgame.custom.badge");
        custom.icon = icons + "/quill@green.svg";
        custom.left = dp(kHousePlaces[count].left);
        custom.top = dp(kHousePlaces[count].top);
        custom.custom = true;
        houses.push_back(std::move(custom));
        has_house = count > 0;
        if (!has_house) {
            sel_house = -1;
        } else if (sel_house < 0 || static_cast<std::size_t>(sel_house) >= count) {
            sel_house = 0;
        }
        build_house_summary();
    }

    void build_house_summary() {
        const MenuHouse* h = house_at(sel_house);
        if (h == nullptr) {
            hs = {};
            return;
        }
        hs.name = text("ui.menu.newgame.house", {Arg("house", h->name_gen)});
        hs.head = h->head;
        hs.head_line =
            text("ui.menu.newgame.head_line", {Arg("age", text("ui.age.years", {Arg("n", h->head_age)}))});
        hs.members = text("ui.menu.newgame.members", {Arg("n", h->members)});
        hs.treasury = h->treasury;
        hs.prestige = h->prestige;
        hs.heir = h->heir;
        hs.portrait = art_image(h->portrait);
    }

    void build_goals() {
        goals.clear();
        const std::size_t count = goal_count();
        for (std::size_t i = 0; i < count; ++i) {
            GoalRow row;
            row.index = static_cast<int>(i);
            row.title = ng.goals[i].title;
            row.line = ng.goals[i].line;
            row.icon = icons + "/" + ng.goals[i].icon + "@green.svg";
            row.left = dp(kGoalPlaces[i].left);
            row.top = dp(kGoalPlaces[i].top);
            row.width = dp(kGoalPlaces[i].width);
            goals.push_back(std::move(row));
        }
        if (sel_goal < 0 || static_cast<std::size_t>(sel_goal) >= count) {
            sel_goal = count == 0 ? -1 : static_cast<int>(std::min(ng.default_goal, count - 1));
        }
        build_goal_summary();
    }

    [[nodiscard]] std::string founder_name() const {
        return cust.patronymic.empty() ? cust.name : cust.name + " " + cust.patronymic;
    }

    void build_goal_summary() {
        gs = {};
        gs.date = ng.start_date;
        gs.mode = text("ui.menu.goal.mode");
        if (sel_goal >= 0 && static_cast<std::size_t>(sel_goal) < goal_count()) {
            gs.goal = ng.goals[static_cast<std::size_t>(sel_goal)].title;
        }
        if (goal_custom) {
            gs.house = text("ui.menu.newgame.house", {Arg("house", cust.house_name)});
            gs.head = text("ui.menu.goal.founder", {Arg("name", cust.name + " " + cust.surname)});
            gs.treasury = ng.custom_treasury;
        } else if (const MenuHouse* h = house_at(sel_house)) {
            gs.house = text("ui.menu.newgame.house", {Arg("house", h->name_gen)});
            gs.head = text("ui.menu.goal.head", {Arg("name", h->head)});
            gs.treasury = h->treasury;
        }
    }

    void build_custom() {
        cf.surname = cust.surname;
        cf.house = cust.house_name;
        cf.name = cust.name;
        cf.patronymic = cust.patronymic;
        cf.motto = cust.motto;
        const auto option = [](const std::vector<std::string>& list, int index) {
            return index >= 0 && static_cast<std::size_t>(index) < list.size()
                       ? list[static_cast<std::size_t>(index)]
                       : std::string();
        };
        cf.look = option(ng.looks, cust.look);
        cf.faith = option(ng.faiths, cust.faith);
        build_custom_summary();
    }

    void build_custom_summary() {
        cs.line1 = founder_name();
        cs.line2 = cust.surname;
        cs.motto = text("ui.menu.custom.motto", {Arg("motto", cust.motto)});
        cs.portrait = art_image(ng.custom_portrait);
        cust_ok = !cust.surname.empty() && !cust.house_name.empty() && !cust.name.empty();
    }

    [[nodiscard]] std::string default_house_line() const {
        if (const MenuHouse* h = house_at(sel_house)) {
            return text("ui.menu.world.house", {Arg("house", h->name_gen), Arg("date", ng.start_date)});
        }
        return {};
    }

    void build_world() {
        const bool restore = world_kind == MenuWorldKind::Restore;
        const char* kind = restore ? "restore" : "new";
        wv.ready = percent >= 100;
        wv.stage = std::clamp(stage, 0, 2);
        wv.title = text(wv.ready ? (restore ? "ui.menu.world.ready.restore" : "ui.menu.world.ready.new")
                                 : (restore ? "ui.menu.world.title.restore" : "ui.menu.world.title.new"));
        wv.house_line = world_house.empty() ? default_house_line() : world_house;
        wv.stage0 = text(std::string("ui.menu.world.stage.") + kind + ".s0");
        wv.stage1 = text(std::string("ui.menu.world.stage.") + kind + ".s1");
        wv.stage2 = text(std::string("ui.menu.world.stage.") + kind + ".s2");
        wv.percent = text("ui.menu.world.percent", {Arg("n", percent)});
        wv.bar = dp((kProgressWidth * std::clamp(percent, 0, 100) + 50) / 100);
        wv.enter = text(restore ? "ui.menu.world.enter.restore" : "ui.menu.world.enter.new");
        page_caption = text(wv.ready ? "ui.menu.world.caption.ready" : "ui.menu.world.caption.prepare");
    }

    void build_cards() {
        cards.clear();
        for (std::size_t i = 0; i < kCards.size(); ++i) {
            CardRow row;
            row.index = static_cast<int>(i);
            row.number = i + 1 < 10 ? "0" + std::to_string(i + 1) : std::to_string(i + 1);
            const std::string key = std::string("ui.menu.overview.card.") + kCards[i].key;
            row.title = text(key + ".title");
            row.caption = text(key + ".caption");
            row.left = dp(kOverviewLeft + static_cast<int>(i % kOverviewColumns) * kOverviewPitchX);
            row.top = dp(kOverviewTop + static_cast<int>(i / kOverviewColumns) * kOverviewPitchY);
            row.enabled = kCards[i].target != CardTarget::Soon;
            row.hint = row.enabled ? Rml::String() : text("ui.menu.overview.soon");
            cards.push_back(std::move(row));
        }
    }

    void dirty_all() {
        for (const char* name : {"rev",
                                 "screen",
                                 "dialog",
                                 "icons",
                                 "decor",
                                 "art_deco",
                                 "preview_deco",
                                 "lead",
                                 "show_hint",
                                 "items",
                                 "sel",
                                 "page_caption",
                                 "page_title",
                                 "page_subtitle",
                                 "has_saves",
                                 "saves",
                                 "sel_save",
                                 "has_sel",
                                 "sum",
                                 "cards",
                                 "paged",
                                 "back_label",
                                 "next_label",
                                 "next_icon",
                                 "next_ok",
                                 "cust_ok",
                                 "chosen",
                                 "has_house",
                                 "houses",
                                 "sel_house",
                                 "hs",
                                 "goals",
                                 "sel_goal",
                                 "gs",
                                 "cf",
                                 "cs",
                                 "wv"}) {
            handle.DirtyVariable(name);
        }
    }

    /// Rebuilds what the current screen shows from the data and the stack, and tells the document.
    void rebuild() {
        screen = screen_name(nav.top());
        dialog = dialog_name(nav.dialog());
        switch (nav.top()) {
            case MenuScreen::Main:
                build_items();
                break;
            case MenuScreen::Load:
                build_saves();
                break;
            case MenuScreen::About:
                break;
            case MenuScreen::Overview:
                build_cards();
                break;
            case MenuScreen::NewGame:
                build_houses();
                break;
            case MenuScreen::Custom:
                build_custom();
                break;
            case MenuScreen::Goal:
                build_goals();
                break;
            case MenuScreen::World:
                build_world();
                break;
        }
        build_page();
        dirty_all();
    }

    void focus(std::string id) { pending_focus = std::move(id); }

    /// The control that gets the keyboard focus on the current screen (or in the open dialog).
    void focus_default() {
        if (nav.dialog() == MenuDialog::Exit) {
            focus("dlg-stay");
            return;
        }
        if (nav.dialog() == MenuDialog::SaveUnavailable) {
            focus("dlg-back");
            return;
        }
        switch (nav.top()) {
            case MenuScreen::Main:
                if (!items.empty()) {
                    focus("mi-" + std::string(items[static_cast<std::size_t>(sel)].id));
                }
                break;
            case MenuScreen::Load:
                focus(has_saves ? "save-" + std::to_string(sel_save) : std::string("btn-new"));
                break;
            case MenuScreen::About:
                focus("btn-main");
                break;
            case MenuScreen::Overview:
                for (const CardRow& c : cards) {
                    if (c.enabled) {
                        focus("card-" + std::to_string(c.index));
                        break;
                    }
                }
                break;
            case MenuScreen::NewGame:
                focus("house-" + std::to_string(std::max(sel_house, 0)));
                break;
            case MenuScreen::Custom:
                focus("in-surname");
                break;
            case MenuScreen::Goal:
                focus("goal-" + std::to_string(std::max(sel_goal, 0)));
                break;
            case MenuScreen::World:
                focus(wv.ready ? "btn-enter" : "btn-cancel");
                break;
        }
    }

    void changed() {
        rebuild();
        focus_default();
    }

    void enter_screen(MenuScreen s) {
        if (s == MenuScreen::Main) {
            sel = 0;
        }
        if (s == MenuScreen::Load) {
            sel_save = 0;
            const MenuSnapshot& d = data();
            for (std::size_t i = 0; i < d.saves.size(); ++i) {
                if (d.saves[i].compatible) {
                    sel_save = static_cast<int>(i);
                    break;
                }
            }
        }
        nav.push(s);
        changed();
    }

    // ---- actions

    void select_item(int index, bool move_focus) {
        if (index < 0 || static_cast<std::size_t>(index) >= items.size() ||
            !items[static_cast<std::size_t>(index)].enabled || index == sel) {
            return;
        }
        sel = index;
        handle.DirtyVariable("sel");
        if (move_focus) {
            focus("mi-" + std::string(items[static_cast<std::size_t>(index)].id));
        }
    }

    void select_save(int index, bool move_focus) {
        if (index < 0 || static_cast<std::size_t>(index) >= saves.size() || index == sel_save) {
            return;
        }
        sel_save = index;
        build_summary();
        handle.DirtyVariable("sel_save");
        handle.DirtyVariable("sum");
        handle.DirtyVariable("has_sel");
        if (move_focus) {
            focus("save-" + std::to_string(index));
        }
    }

    void do_item(const std::string& id) {
        if (nav.dialog() != MenuDialog::None) {
            return;
        }
        if (nav.top() == MenuScreen::Load) {
            // "Новая игра" of the empty list of saves is not a row of the main menu.
            if (id == "new") {
                enter_screen(MenuScreen::NewGame);
            }
            return;
        }
        if (nav.top() != MenuScreen::Main) {
            return;
        }
        for (const ItemRow& row : items) {
            if (row.id != id || !row.enabled) {
                continue;
            }
            if (id == "continue") {
                if (!preview) {
                    call(actions.continue_game, "continue");
                }
                begin_restore(data().saves.front());
            } else if (id == "new") {
                enter_screen(MenuScreen::NewGame);
            } else if (id == "load") {
                enter_screen(MenuScreen::Load);
            } else if (id == "settings") {
                call(actions.open_settings, "settings");
            } else if (id == "about") {
                enter_screen(MenuScreen::About);
            } else if (id == "exit") {
                nav.open_dialog(MenuDialog::Exit);
                changed();
            }
            return;
        }
    }

    void do_load_selected() {
        if (nav.dialog() != MenuDialog::None || nav.top() != MenuScreen::Load || !has_sel) {
            return;
        }
        const MenuSave& s = data().saves[static_cast<std::size_t>(sel_save)];
        if (!s.compatible) {
            nav.open_dialog(MenuDialog::SaveUnavailable);
            changed();
            return;
        }
        if (preview) {
            // The overview shows the screens on demo data: nothing is loaded.
        } else if (actions.load_save) {
            actions.load_save(s.id);
        } else {
            PAT_LOG_INFO("menu", "load save '{}': no handler yet (stub)", s.id);
        }
        begin_restore(s);
    }

    /// The world screen for the restore of `save` (from "Загрузить историю" and "Продолжить").
    void begin_restore(const MenuSave& save) {
        world_kind = MenuWorldKind::Restore;
        percent = 0;
        stage = 0;
        world_preview = preview.has_value();
        world_house = text("ui.menu.world.house", {Arg("house", save.house_gen), Arg("date", save.date)});
        enter_screen(MenuScreen::World);
    }

    /// "Начать историю": hands the choice to the host and shows the preparation of the world.
    void do_start() {
        const MenuStart start = make_start();
        if (actions.start_game) {
            actions.start_game(start);
        } else {
            PAT_LOG_INFO("menu",
                         "start game '{}' with goal '{}': no handler yet (stub)",
                         start.house_id,
                         start.goal_id);
        }
        world_kind = MenuWorldKind::NewGame;
        percent = 0;
        stage = 0;
        world_preview = false;
        if (goal_custom) {
            world_house =
                text("ui.menu.world.house", {Arg("house", cust.house_name), Arg("date", ng.start_date)});
        } else {
            world_house.clear();  // the chosen house, from the data
        }
        enter_screen(MenuScreen::World);
    }

    [[nodiscard]] MenuStart make_start() const {
        MenuStart start;
        start.custom = goal_custom;
        if (goal_custom) {
            start.custom_house = cust;
        } else if (const MenuHouse* h = house_at(sel_house)) {
            start.house_id = h->id;
        }
        if (sel_goal >= 0 && static_cast<std::size_t>(sel_goal) < goal_count()) {
            start.goal_id = ng.goals[static_cast<std::size_t>(sel_goal)].id;
        }
        return start;
    }

    void do_next() {
        if (nav.dialog() != MenuDialog::None || !next_ok) {
            return;
        }
        switch (nav.top()) {
            case MenuScreen::NewGame:
                goal_custom = false;
                enter_screen(MenuScreen::Goal);
                break;
            case MenuScreen::Custom:
                goal_custom = true;
                enter_screen(MenuScreen::Goal);
                break;
            case MenuScreen::Goal:
                do_start();
                break;
            default:
                break;
        }
    }

    void do_enter() {
        if (nav.dialog() != MenuDialog::None || nav.top() != MenuScreen::World || !wv.ready ||
            world_preview) {
            return;
        }
        call(actions.enter_game, "enter game");
    }

    void select_house(int index, bool move_focus) {
        if (house_at(index) == nullptr || index == sel_house) {
            return;
        }
        sel_house = index;
        build_house_summary();
        handle.DirtyVariable("sel_house");
        handle.DirtyVariable("hs");
        if (move_focus) {
            focus("house-" + std::to_string(index));
        }
    }

    void do_house(int index) {
        if (nav.top() != MenuScreen::NewGame || nav.dialog() != MenuDialog::None) {
            return;
        }
        if (index == static_cast<int>(house_count())) {
            goal_custom = true;  // "Основать свой род" opens the form at once
            enter_screen(MenuScreen::Custom);
        } else {
            select_house(index, false);
        }
    }

    void select_goal(int index, bool move_focus) {
        if (index < 0 || static_cast<std::size_t>(index) >= goal_count() || index == sel_goal) {
            return;
        }
        sel_goal = index;
        build_goal_summary();
        handle.DirtyVariable("sel_goal");
        handle.DirtyVariable("gs");
        if (move_focus) {
            focus("goal-" + std::to_string(index));
        }
    }

    void set_world_progress(int new_percent, int new_stage) {
        new_percent = std::clamp(new_percent, 0, 100);
        new_stage = std::clamp(new_stage, 0, 2);
        if (new_percent == percent && new_stage == stage) {
            return;
        }
        percent = new_percent;
        stage = new_stage;
        const bool was_ready = wv.ready;
        build_world();
        handle.DirtyVariable("wv");
        handle.DirtyVariable("page_caption");
        if (wv.ready && !was_ready) {
            focus_default();  // the button of the ready state takes the focus
        }
    }

    void edit_field(const std::string& field, const std::string& value) {
        if (field == "surname") {
            cust.surname = value;
        } else if (field == "house") {
            cust.house_name = value;
        } else if (field == "name") {
            cust.name = value;
        } else if (field == "patronymic") {
            cust.patronymic = value;
        } else if (field == "motto") {
            cust.motto = value;
        } else {
            return;
        }
        build_custom_summary();
        update_next();
        handle.DirtyVariable("cs");
        handle.DirtyVariable("cust_ok");
        handle.DirtyVariable("next_ok");
    }

    void cycle_field(const std::string& field) {
        const auto next = [](int current, std::size_t count) {
            return count == 0 ? 0 : static_cast<int>((static_cast<std::size_t>(current) + 1) % count);
        };
        if (field == "look") {
            cust.look = next(cust.look, ng.looks.size());
        } else if (field == "faith") {
            cust.faith = next(cust.faith, ng.faiths.size());
        } else {
            return;
        }
        build_custom();
        handle.DirtyVariable("cf");
    }

    void do_card(std::size_t index) {
        if (nav.top() != MenuScreen::Overview || nav.dialog() != MenuDialog::None || index >= kCards.size()) {
            return;
        }
        const bool has_real = !snapshot.saves.empty();
        switch (kCards[index].target) {
            case CardTarget::MainFirst:
            case CardTarget::LoadEmpty:
                preview = MenuSnapshot{};
                break;
            case CardTarget::MainSave:
            case CardTarget::Load:
                if (has_real) {
                    preview.reset();
                } else {
                    preview = make_placeholder_menu(*loc);
                }
                break;
            case CardTarget::NewGame:
                enter_screen(MenuScreen::NewGame);
                return;
            case CardTarget::Custom:
                goal_custom = true;
                enter_screen(MenuScreen::Custom);
                return;
            case CardTarget::World:
                world_kind = MenuWorldKind::NewGame;
                percent = kPreviewPercent;
                stage = 1;
                world_preview = true;
                world_house.clear();
                enter_screen(MenuScreen::World);
                return;
            case CardTarget::Soon:
                return;
        }
        enter_screen(kCards[index].target == CardTarget::Load || kCards[index].target == CardTarget::LoadEmpty
                         ? MenuScreen::Load
                         : MenuScreen::Main);
    }

    MenuBack do_back() {
        if (nav.dialog() == MenuDialog::None && nav.top() == MenuScreen::World && !world_preview) {
            if (wv.ready) {
                return MenuBack::Held;  // the world is ready: only "Вступить в игру" leaves
            }
            call(actions.cancel_prepare, "cancel prepare");
        }
        const MenuBack result = nav.back();
        if (result == MenuBack::Nothing) {
            return result;
        }
        if (nav.top() == MenuScreen::Overview) {
            preview.reset();
        }
        changed();
        return result;
    }

    /// The keys of the grids of cards ("Новая игра", "Цель партии"): arrows move the focus, and the card that
    /// gets it is the chosen one. Two columns: Left and Right step by one, Up and Down by two.
    void do_grid(Rml::Input::KeyIdentifier key) {
        const bool houses_screen = nav.top() == MenuScreen::NewGame;
        const int count = houses_screen ? static_cast<int>(houses.size()) : static_cast<int>(goals.size());
        const int current = houses_screen ? std::max(sel_house, 0) : std::max(sel_goal, 0);
        if (count == 0 || nav.dialog() != MenuDialog::None) {
            return;
        }
        int focused = current;
        if (Rml::Element* f = ui->context()->GetFocusElement()) {
            const std::string prefix = houses_screen ? "house-" : "goal-";
            if (f->GetId().rfind(prefix, 0) == 0) {
                focused = std::atoi(f->GetId().c_str() + prefix.size());
            }
        }
        int next = focused;
        switch (key) {
            case Rml::Input::KI_LEFT:
                next = focused - 1;
                break;
            case Rml::Input::KI_RIGHT:
                next = focused + 1;
                break;
            case Rml::Input::KI_UP:
                next = focused - 2;
                break;
            default:
                next = focused + 2;
                break;
        }
        next = std::clamp(next, 0, count - 1);
        if (houses_screen) {
            select_house(next, true);
        } else {
            select_goal(next, true);
        }
        focus((houses_screen ? "house-" : "goal-") + std::to_string(next));
    }

    void do_move(int delta) {
        if (nav.dialog() != MenuDialog::None || delta == 0) {
            return;
        }
        if (nav.top() == MenuScreen::Main) {
            const int count = static_cast<int>(items.size());
            int next = sel;
            for (int step = 0; step < count; ++step) {
                next = ((next + delta) % count + count) % count;
                if (items[static_cast<std::size_t>(next)].enabled) {
                    select_item(next, true);
                    return;
                }
            }
        } else if (nav.top() == MenuScreen::NewGame || nav.top() == MenuScreen::Goal) {
            do_grid(delta < 0 ? Rml::Input::KI_UP : Rml::Input::KI_DOWN);
        } else if (nav.top() == MenuScreen::Load && has_saves) {
            const int count = static_cast<int>(saves.size());
            select_save(((sel_save + delta) % count + count) % count, true);
        }
    }

    void do_dialog(const std::string& id) {
        if (nav.dialog() == MenuDialog::Exit) {
            if (id == "stay") {
                nav.close_dialog();
                changed();
            } else if (id == "exit") {
                nav.close_dialog();
                changed();
                call(actions.quit, "quit");
            }
        } else if (nav.dialog() == MenuDialog::SaveUnavailable && id == "back") {
            nav.close_dialog();
            changed();
        }
    }
};

MenuWindow::MenuWindow(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

MenuWindow::~MenuWindow() {
    if (impl_ != nullptr && impl_->ui != nullptr) {
        Rml::Context* ctx = impl_->ui->context();
        if (Rml::ElementDocument* doc = impl_->find_document()) {
            doc->Close();
        }
        ctx->Update();
        ctx->RemoveDataModel(kModelName);
    }
}

core::Result<std::unique_ptr<MenuWindow>> MenuWindow::create(engine::UiContext& ui,
                                                             const std::filesystem::path& document,
                                                             const std::filesystem::path& icons,
                                                             const loc::Localizer& loc) {
    using R = core::Result<std::unique_ptr<MenuWindow>>;
    auto impl = std::make_unique<Impl>();
    impl->ui = &ui;
    impl->loc = &loc;

    const auto relative = [&](const std::filesystem::path& target) {
        std::error_code ec;
        const std::filesystem::path rel = std::filesystem::relative(
            std::filesystem::absolute(target), std::filesystem::absolute(document).parent_path(), ec);
        return ec ? target.generic_string() : rel.generic_string();
    };
    const std::filesystem::path root = icons.parent_path();
    impl->icons = relative(icons);
    impl->decor = relative(root / "decor");
    const std::string art = relative(root / "art") + "/winter_terrain_concept.jpg";
    impl->art_deco = "image(" + art + " cover)";
    impl->preview_deco = impl->art_deco;
    impl->art_dir = relative(root / "art");

    Rml::DataModelConstructor ctor = ui.context()->CreateDataModel(kModelName);
    if (!ctor) {
        return R::err(core::Error{94, "cannot create the menu data model"});
    }
    if (auto item = ctor.RegisterStruct<ItemRow>()) {
        item.RegisterMember("index", &ItemRow::index);
        item.RegisterMember("id", &ItemRow::id);
        item.RegisterMember("label", &ItemRow::label);
        item.RegisterMember("note", &ItemRow::note);
        item.RegisterMember("top", &ItemRow::top);
        item.RegisterMember("height", &ItemRow::height);
        item.RegisterMember("note_top", &ItemRow::note_top);
        item.RegisterMember("primary", &ItemRow::primary);
        item.RegisterMember("enabled", &ItemRow::enabled);
        item.RegisterMember("note_save", &ItemRow::note_save);
    }
    ctor.RegisterArray<std::vector<ItemRow>>();
    if (auto save = ctor.RegisterStruct<SaveRow>()) {
        save.RegisterMember("index", &SaveRow::index);
        save.RegisterMember("name", &SaveRow::name);
        save.RegisterMember("badge", &SaveRow::badge);
        save.RegisterMember("line1", &SaveRow::line1);
        save.RegisterMember("line2", &SaveRow::line2);
        save.RegisterMember("icon", &SaveRow::icon);
        save.RegisterMember("ok", &SaveRow::ok);
    }
    ctor.RegisterArray<std::vector<SaveRow>>();
    if (auto summary = ctor.RegisterStruct<SummaryRow>()) {
        summary.RegisterMember("house", &SummaryRow::house);
        summary.RegisterMember("date", &SummaryRow::date);
        summary.RegisterMember("kind", &SummaryRow::kind);
        summary.RegisterMember("head", &SummaryRow::head);
        summary.RegisterMember("treasury", &SummaryRow::treasury);
    }
    if (auto card = ctor.RegisterStruct<CardRow>()) {
        card.RegisterMember("index", &CardRow::index);
        card.RegisterMember("number", &CardRow::number);
        card.RegisterMember("title", &CardRow::title);
        card.RegisterMember("caption", &CardRow::caption);
        card.RegisterMember("left", &CardRow::left);
        card.RegisterMember("top", &CardRow::top);
        card.RegisterMember("hint", &CardRow::hint);
        card.RegisterMember("enabled", &CardRow::enabled);
    }
    ctor.RegisterArray<std::vector<CardRow>>();
    if (auto house = ctor.RegisterStruct<HouseRow>()) {
        house.RegisterMember("index", &HouseRow::index);
        house.RegisterMember("name", &HouseRow::name);
        house.RegisterMember("line", &HouseRow::line);
        house.RegisterMember("note", &HouseRow::note);
        house.RegisterMember("badge", &HouseRow::badge);
        house.RegisterMember("icon", &HouseRow::icon);
        house.RegisterMember("left", &HouseRow::left);
        house.RegisterMember("top", &HouseRow::top);
        house.RegisterMember("custom", &HouseRow::custom);
    }
    ctor.RegisterArray<std::vector<HouseRow>>();
    if (auto house_sum = ctor.RegisterStruct<HouseSummary>()) {
        house_sum.RegisterMember("name", &HouseSummary::name);
        house_sum.RegisterMember("head", &HouseSummary::head);
        house_sum.RegisterMember("head_line", &HouseSummary::head_line);
        house_sum.RegisterMember("members", &HouseSummary::members);
        house_sum.RegisterMember("treasury", &HouseSummary::treasury);
        house_sum.RegisterMember("prestige", &HouseSummary::prestige);
        house_sum.RegisterMember("heir", &HouseSummary::heir);
        house_sum.RegisterMember("portrait", &HouseSummary::portrait);
    }
    if (auto goal = ctor.RegisterStruct<GoalRow>()) {
        goal.RegisterMember("index", &GoalRow::index);
        goal.RegisterMember("title", &GoalRow::title);
        goal.RegisterMember("line", &GoalRow::line);
        goal.RegisterMember("icon", &GoalRow::icon);
        goal.RegisterMember("left", &GoalRow::left);
        goal.RegisterMember("top", &GoalRow::top);
        goal.RegisterMember("width", &GoalRow::width);
    }
    ctor.RegisterArray<std::vector<GoalRow>>();
    if (auto goal_sum = ctor.RegisterStruct<GoalSummary>()) {
        goal_sum.RegisterMember("house", &GoalSummary::house);
        goal_sum.RegisterMember("head", &GoalSummary::head);
        goal_sum.RegisterMember("date", &GoalSummary::date);
        goal_sum.RegisterMember("goal", &GoalSummary::goal);
        goal_sum.RegisterMember("mode", &GoalSummary::mode);
        goal_sum.RegisterMember("treasury", &GoalSummary::treasury);
    }
    if (auto fields = ctor.RegisterStruct<CustomFields>()) {
        fields.RegisterMember("surname", &CustomFields::surname);
        fields.RegisterMember("house", &CustomFields::house);
        fields.RegisterMember("name", &CustomFields::name);
        fields.RegisterMember("patronymic", &CustomFields::patronymic);
        fields.RegisterMember("motto", &CustomFields::motto);
        fields.RegisterMember("look", &CustomFields::look);
        fields.RegisterMember("faith", &CustomFields::faith);
    }
    if (auto custom_sum = ctor.RegisterStruct<CustomSummary>()) {
        custom_sum.RegisterMember("line1", &CustomSummary::line1);
        custom_sum.RegisterMember("line2", &CustomSummary::line2);
        custom_sum.RegisterMember("motto", &CustomSummary::motto);
        custom_sum.RegisterMember("portrait", &CustomSummary::portrait);
    }
    if (auto world = ctor.RegisterStruct<WorldView>()) {
        world.RegisterMember("title", &WorldView::title);
        world.RegisterMember("house_line", &WorldView::house_line);
        world.RegisterMember("stage0", &WorldView::stage0);
        world.RegisterMember("stage1", &WorldView::stage1);
        world.RegisterMember("stage2", &WorldView::stage2);
        world.RegisterMember("percent", &WorldView::percent);
        world.RegisterMember("bar", &WorldView::bar);
        world.RegisterMember("enter", &WorldView::enter);
        world.RegisterMember("stage", &WorldView::stage);
        world.RegisterMember("ready", &WorldView::ready);
    }

    Impl* m = impl.get();
    ctor.Bind("rev", &m->rev);
    ctor.Bind("screen", &m->screen);
    ctor.Bind("dialog", &m->dialog);
    ctor.Bind("icons", &m->icons);
    ctor.Bind("decor", &m->decor);
    ctor.Bind("art_deco", &m->art_deco);
    ctor.Bind("preview_deco", &m->preview_deco);
    ctor.Bind("lead", &m->lead);
    ctor.Bind("show_hint", &m->show_hint);
    ctor.Bind("items", &m->items);
    ctor.Bind("sel", &m->sel);
    ctor.Bind("page_caption", &m->page_caption);
    ctor.Bind("page_title", &m->page_title);
    ctor.Bind("page_subtitle", &m->page_subtitle);
    ctor.Bind("has_saves", &m->has_saves);
    ctor.Bind("saves", &m->saves);
    ctor.Bind("sel_save", &m->sel_save);
    ctor.Bind("has_sel", &m->has_sel);
    ctor.Bind("sum", &m->sum);
    ctor.Bind("cards", &m->cards);
    ctor.Bind("paged", &m->paged);
    ctor.Bind("back_label", &m->back_label);
    ctor.Bind("next_label", &m->next_label);
    ctor.Bind("next_icon", &m->next_icon);
    ctor.Bind("next_ok", &m->next_ok);
    ctor.Bind("cust_ok", &m->cust_ok);
    ctor.Bind("chosen", &m->chosen);
    ctor.Bind("has_house", &m->has_house);
    ctor.Bind("houses", &m->houses);
    ctor.Bind("sel_house", &m->sel_house);
    ctor.Bind("hs", &m->hs);
    ctor.Bind("goals", &m->goals);
    ctor.Bind("sel_goal", &m->sel_goal);
    ctor.Bind("gs", &m->gs);
    ctor.Bind("cf", &m->cf);
    ctor.Bind("cs", &m->cs);
    ctor.Bind("wv", &m->wv);
    m->handle = ctor.GetModelHandle();

    // `{{ rev | tr('key') }}`: the arguments are the piped value (`rev`, only there as a dependency) and the
    // key.
    ctor.RegisterTransformFunc("tr", [m](const Rml::VariantList& args) -> Rml::Variant {
        if (args.size() < 2) {
            return Rml::Variant(Rml::String());
        }
        return Rml::Variant(Rml::String(m->loc->format(args[1].Get<Rml::String>())));
    });

    ctor.BindEventCallback("item", [m](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) {
        if (!args.empty()) {
            m->do_item(args[0].Get<Rml::String>());
        }
    });
    ctor.BindEventCallback("hover", [m](Rml::DataModelHandle, Rml::Event& ev, const Rml::VariantList& args) {
        if (!args.empty() && m->nav.dialog() == MenuDialog::None) {
            // A focus event comes from the keyboard (the focus is already there); a mouseover must move it.
            m->select_item(args[0].Get<int>(), ev.GetId() == Rml::EventId::Mouseover);
        }
    });
    ctor.BindEventCallback("pick", [m](Rml::DataModelHandle, Rml::Event& ev, const Rml::VariantList& args) {
        if (!args.empty() && m->nav.dialog() == MenuDialog::None) {
            m->select_save(args[0].Get<int>(), ev.GetId() != Rml::EventId::Focus);
        }
    });
    ctor.BindEventCallback("open_save", [m](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) {
        if (!args.empty()) {
            m->select_save(args[0].Get<int>(), false);
            m->do_load_selected();
        }
    });
    ctor.BindEventCallback(
        "load", [m](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) { m->do_load_selected(); });
    ctor.BindEventCallback("card", [m](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) {
        if (!args.empty()) {
            m->do_card(static_cast<std::size_t>(std::max(0, args[0].Get<int>())));
        }
    });
    ctor.BindEventCallback("back", [m](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
        if (m->nav.dialog() == MenuDialog::None) {
            m->do_back();
        }
    });
    ctor.BindEventCallback("dialog", [m](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) {
        if (!args.empty()) {
            m->do_dialog(args[0].Get<Rml::String>());
        }
    });
    ctor.BindEventCallback("house", [m](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) {
        if (!args.empty()) {
            m->do_house(args[0].Get<int>());
        }
    });
    ctor.BindEventCallback(
        "house_focus", [m](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) {
            // The card that gets the keyboard focus is the chosen house; the card "Основать свой род" only
            // opens on a press.
            if (!args.empty() && m->nav.top() == MenuScreen::NewGame && m->nav.dialog() == MenuDialog::None) {
                m->select_house(args[0].Get<int>(), false);
            }
        });
    ctor.BindEventCallback("goal", [m](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) {
        if (!args.empty() && m->nav.dialog() == MenuDialog::None) {
            m->select_goal(args[0].Get<int>(), false);
        }
    });
    ctor.BindEventCallback("next",
                           [m](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) { m->do_next(); });
    ctor.BindEventCallback(
        "enter", [m](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) { m->do_enter(); });
    ctor.BindEventCallback("edit", [m](Rml::DataModelHandle, Rml::Event& ev, const Rml::VariantList& args) {
        if (!args.empty() && m->nav.top() == MenuScreen::Custom) {
            m->edit_field(args[0].Get<Rml::String>(), ev.GetParameter<Rml::String>("value", Rml::String()));
        }
    });
    ctor.BindEventCallback("cycle", [m](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) {
        if (!args.empty() && m->nav.top() == MenuScreen::Custom && m->nav.dialog() == MenuDialog::None) {
            m->cycle_field(args[0].Get<Rml::String>());
        }
    });
    m->rebuild();

    Rml::ElementDocument* loaded = ui.load_document(document);
    if (loaded == nullptr) {
        ui.context()->RemoveDataModel(kModelName);
        m->ui = nullptr;
        return R::err(core::Error{95, "cannot load the menu document " + document.generic_string()});
    }
    m->source_url = loaded->GetSourceURL();
    m->key_listener.owner = m;
    loaded->AddEventListener(Rml::EventId::Keydown, &m->key_listener, /*in_capture_phase=*/true);
    loaded->Hide();  // shown by open()
    return R::ok(std::unique_ptr<MenuWindow>(new MenuWindow(std::move(impl))));
}

void MenuWindow::set_snapshot(const MenuSnapshot& snapshot) {
    impl_->snapshot = snapshot;
    impl_->rebuild();
}

const MenuSnapshot& MenuWindow::snapshot() const {
    return impl_->snapshot;
}

void MenuWindow::set_new_game(const MenuNewGame& data) {
    Impl& m = *impl_;
    m.ng = data;
    m.cust = data.custom_defaults;
    m.sel_house = 0;
    m.sel_goal =
        data.goals.empty() ? -1 : static_cast<int>(std::min(data.default_goal, data.goals.size() - 1));
    m.rebuild();
}

const MenuNewGame& MenuWindow::new_game() const {
    return impl_->ng;
}

void MenuWindow::set_actions(MenuActions actions) {
    impl_->actions = std::move(actions);
}

void MenuWindow::relocalize() {
    ++impl_->rev;
    impl_->rebuild();
}

void MenuWindow::open(MenuScreen root) {
    impl_->nav.reset(root);
    impl_->preview.reset();
    impl_->world_preview = false;
    impl_->sel = 0;
    impl_->sel_save = 0;
    impl_->visible = true;
    if (Rml::ElementDocument* doc = impl_->find_document()) {
        doc->Show(Rml::ModalFlag::Modal, Rml::FocusFlag::Auto);
    }
    impl_->enter_screen(root);  // pushing the root again is a no-op for the stack; it computes the state
}

void MenuWindow::open_world(MenuWorldKind kind, int percent, const std::string& house_line) {
    Impl& m = *impl_;
    m.world_kind = kind;
    m.percent = std::clamp(percent, 0, 100);
    m.stage = m.percent >= 100 ? 2 : (m.percent >= 34 ? 1 : 0);
    m.world_house = house_line;
    open(MenuScreen::World);
}

void MenuWindow::set_progress(int percent, int stage) {
    if (impl_->nav.top() == MenuScreen::World) {
        impl_->set_world_progress(percent, stage);
    }
}

void MenuWindow::set_custom_flow(bool custom) {
    impl_->goal_custom = custom;
    if (impl_->nav.top() == MenuScreen::Goal) {
        impl_->build_goal_summary();
        impl_->handle.DirtyVariable("gs");
    }
}

void MenuWindow::close() {
    impl_->visible = false;
    impl_->pending_focus.clear();
    if (Rml::ElementDocument* doc = impl_->find_document()) {
        doc->Hide();
    }
}

bool MenuWindow::visible() const {
    return impl_->visible;
}

void MenuWindow::update() {
    if (!impl_->visible || impl_->pending_focus.empty()) {
        return;
    }
    Rml::ElementDocument* doc = impl_->find_document();
    if (doc == nullptr) {
        return;
    }
    Rml::Element* target = doc->GetElementById(impl_->pending_focus);
    if (target == nullptr) {
        return;  // not created yet: the next update tries again
    }
    if (impl_->ui->context()->GetFocusElement() != target) {
        target->Focus();
    }
    impl_->pending_focus.clear();
}

MenuBack MenuWindow::press_back() {
    return impl_->do_back();
}

void MenuWindow::press_move(int delta) {
    impl_->do_move(delta);
}

void MenuWindow::press_item(const std::string& id) {
    impl_->do_item(id);
}

void MenuWindow::press_save(std::size_t index) {
    if (impl_->nav.top() == MenuScreen::Load && impl_->nav.dialog() == MenuDialog::None) {
        impl_->select_save(static_cast<int>(index), true);
    }
}

void MenuWindow::press_load() {
    impl_->do_load_selected();
}

void MenuWindow::press_card(std::size_t index) {
    impl_->do_card(index);
}

void MenuWindow::press_dialog(const std::string& id) {
    impl_->do_dialog(id);
}

void MenuWindow::press_house(std::size_t index) {
    impl_->do_house(static_cast<int>(index));
}

void MenuWindow::press_goal(std::size_t index) {
    if (impl_->nav.top() == MenuScreen::Goal && impl_->nav.dialog() == MenuDialog::None) {
        impl_->select_goal(static_cast<int>(index), true);
    }
}

void MenuWindow::press_next() {
    impl_->do_next();
}

void MenuWindow::press_enter() {
    impl_->do_enter();
}

void MenuWindow::set_field(const std::string& field, const std::string& value) {
    impl_->edit_field(field, value);
    impl_->build_custom();
    impl_->handle.DirtyVariable("cf");
}

void MenuWindow::press_cycle(const std::string& field) {
    impl_->cycle_field(field);
}

MenuStart MenuWindow::current_start() const {
    return impl_->make_start();
}

bool MenuWindow::next_enabled() const {
    return impl_->next_ok;
}

int MenuWindow::progress() const {
    return impl_->percent;
}

bool MenuWindow::world_ready() const {
    return impl_->wv.ready;
}

int MenuWindow::house_selected() const {
    return impl_->sel_house;
}

int MenuWindow::goal_selected() const {
    return impl_->sel_goal;
}

MenuScreen MenuWindow::screen() const {
    return impl_->nav.top();
}

MenuDialog MenuWindow::dialog() const {
    return impl_->nav.dialog();
}

std::size_t MenuWindow::depth() const {
    return impl_->nav.depth();
}

int MenuWindow::highlighted() const {
    switch (impl_->nav.top()) {
        case MenuScreen::Main:
            return impl_->sel;
        case MenuScreen::Load:
            return impl_->has_saves ? impl_->sel_save : -1;
        case MenuScreen::NewGame:
            return impl_->sel_house;
        case MenuScreen::Goal:
            return impl_->sel_goal;
        default:
            return -1;
    }
}

bool MenuWindow::has_save_variant() const {
    return !impl_->data().saves.empty();
}

Rml::ElementDocument* MenuWindow::document() const {
    return impl_->find_document();
}

}  // namespace pat::ui
