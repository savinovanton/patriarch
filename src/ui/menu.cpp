#include "pat/ui/menu.hpp"

#include "pat/core/log.hpp"

#include <RmlUi/Core.h>

#include <algorithm>
#include <array>
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
enum class CardTarget { MainFirst, MainSave, Load, LoadEmpty, Soon };

struct CardSpec {
    const char* key;
    CardTarget target;
};

// The eight cards of the board "Обзор стартовых экранов": the ones whose screens do not exist yet are "Soon".
constexpr std::array<CardSpec, 8> kCards = {{
    {"first", CardTarget::MainFirst},
    {"continue", CardTarget::MainSave},
    {"new", CardTarget::Soon},
    {"custom", CardTarget::Soon},
    {"saves", CardTarget::Load},
    {"empty", CardTarget::LoadEmpty},
    {"settings", CardTarget::Soon},
    {"world", CardTarget::Soon},
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
        if (nav.top() == MenuScreen::About) {
            page_caption = text("ui.menu.about.caption");
            page_title = text("ui.menu.about.title");
            page_subtitle = text("ui.menu.about.subtitle");
        } else {
            page_caption = text(empty ? "ui.menu.load.caption.empty" : "ui.menu.load.caption");
            page_title = text(empty ? "ui.menu.load.title.empty" : "ui.menu.load.title");
            page_subtitle = text(empty ? "ui.menu.load.subtitle.empty" : "ui.menu.load.subtitle");
        }
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
        for (const char* name :
             {"rev",       "screen",    "dialog",   "icons",   "decor",        "art_deco",   "preview_deco",
              "lead",      "show_hint", "items",    "sel",     "page_caption", "page_title", "page_subtitle",
              "has_saves", "saves",     "sel_save", "has_sel", "sum",          "cards"}) {
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
                build_page();
                break;
            case MenuScreen::About:
                build_page();
                break;
            case MenuScreen::Overview:
                build_cards();
                break;
        }
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
                call(actions.new_game, "new game");
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
                call(actions.continue_game, "continue");
            } else if (id == "new") {
                call(actions.new_game, "new game");
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
        if (actions.load_save) {
            actions.load_save(s.id);
        } else {
            PAT_LOG_INFO("menu", "load save '{}': no handler yet (stub)", s.id);
        }
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
            case CardTarget::Soon:
                return;
        }
        enter_screen(kCards[index].target == CardTarget::Load || kCards[index].target == CardTarget::LoadEmpty
                         ? MenuScreen::Load
                         : MenuScreen::Main);
    }

    MenuBack do_back() {
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
    ctor.BindEventCallback("key", [m](Rml::DataModelHandle, Rml::Event& ev, const Rml::VariantList&) {
        const auto key = static_cast<Rml::Input::KeyIdentifier>(
            ev.GetParameter<int>("key_identifier", static_cast<int>(Rml::Input::KI_UNKNOWN)));
        if (key == Rml::Input::KI_ESCAPE) {
            if (m->do_back() != MenuBack::Nothing) {
                ev.StopPropagation();
            }
        } else if ((key == Rml::Input::KI_UP || key == Rml::Input::KI_DOWN) &&
                   m->nav.dialog() == MenuDialog::None &&
                   (m->nav.top() == MenuScreen::Main || m->nav.top() == MenuScreen::Load)) {
            m->do_move(key == Rml::Input::KI_UP ? -1 : 1);
            ev.StopPropagation();
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
    impl_->sel = 0;
    impl_->sel_save = 0;
    impl_->visible = true;
    if (Rml::ElementDocument* doc = impl_->find_document()) {
        doc->Show(Rml::ModalFlag::Modal, Rml::FocusFlag::Auto);
    }
    impl_->enter_screen(root);  // pushing the root again is a no-op for the stack; it computes the state
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
