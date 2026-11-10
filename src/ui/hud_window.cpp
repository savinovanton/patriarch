#include "pat/ui/hud_window.hpp"

#include "pat/ui/text.hpp"

#include <RmlUi/Core.h>

#include <array>
#include <system_error>

namespace pat::ui {

namespace {

constexpr const char* kModelName = "hud";

using loc::Arg;

// The display name of an entry is the string ui.menu.<key>; hotkeys are key names, not language text.
struct MenuEntry {
    const char* key;
    const char* hotkey;
};

constexpr std::array<MenuEntry, HudWindow::kMenuCount> kMenu{{
    {"world", "1"},
    {"realm", "2"},
    {"holdings", "3"},
    {"army", "4"},
    {"church", "5"},
    {"economy", "6"},
    {"diplomacy", "7"},
    {"chronicle", "8"},
    {"system", "Esc"},
}};

/// Captions of the panels and tabs (one struct so the document reads them as `t.name`).
struct HudTexts {
    Rml::String holdings_title;
    Rml::String armies_title;
    Rml::String affairs_title;
    Rml::String chronicle_title;
    Rml::String tab_cities;
    Rml::String tab_armies;
    Rml::String tab_dynasties;
};

struct ResourceRow {
    Rml::String key;
    Rml::String label;
    Rml::String value;
    Rml::String delta;
    Rml::String tone;     // "pos", "neg" or "zero"
    Rml::String icon;     // path of the icon relative to the document
    Rml::String tooltip;  // "Казна: 12 480 (+84)"
};

struct MenuRow {
    Rml::String key;
    Rml::String name;
    Rml::String hotkey;
    Rml::String icon;
};

struct EntryRow {
    Rml::String name;
    Rml::String value;
    Rml::String icon;
};

struct AffairRow {
    Rml::String title;
    Rml::String subtitle;
    Rml::String icon;
};

struct ChronicleRow {
    Rml::String time;
    Rml::String text;
};

constexpr std::size_t kMaxListRows = 6;        // rows of the holdings list that fit the panel
constexpr std::size_t kMaxAffairs = 3;         // cards of "Дела государства"
constexpr std::size_t kMaxChronicleLines = 4;  // lines of the chronicle box

}  // namespace

HudSnapshot make_test_hud(const loc::Localizer& loc) {
    constexpr HudNumber kThousands = HudNumber::Thousands;
    HudSnapshot hud;
    hud.realm_name = loc.format("ui.hud.realm.name");
    hud.realm_subtitle = loc.format("ui.hud.realm.subtitle");
    hud.resources = {
        {"treasury", 12480, 84},
        {"grain", 3240, 62},
        {"army", 18600, 240},
        {"faith", 72, 2},
        {"influence", 48, 1},
    };
    hud.bell_count = 2;
    hud.charter_count = 3;
    hud.embassy_count = 0;
    // Names below are placeholder content until the map and dynasties come from data.
    hud.cities = {{"Москва", 94, kThousands},
                  {"Тверь", 71, kThousands},
                  {"Новгород", 56, kThousands},
                  {"Псков", 38, kThousands},
                  {"Смоленск", 33, kThousands}};
    hud.armies = {{"Большой полк", 4800, HudNumber::Plain}};
    hud.dynasties = {{"Шереметевы", 18, HudNumber::Plain},
                     {"Репнины", 12, HudNumber::Plain},
                     {"Голицыны", 9, HudNumber::Plain}};
    hud.affairs = {{"bell", "ui.hud.affairs.council.title", "ui.hud.affairs.council.detail", 1},
                   {"embassy", "ui.hud.affairs.embassy.title", "ui.hud.affairs.embassy.detail", 0}};
    hud.chronicle = {{"11:42", "ui.hud.chronicle.embassy_arrived", 0},
                     {"11:30", "ui.hud.chronicle.taxes_collected", 0}};
    return hud;
}

struct HudWindow::Impl {
    engine::UiContext* ui = nullptr;
    const loc::Localizer* loc = nullptr;
    Rml::DataModelHandle handle;
    std::string source_url;
    std::string assets_rel;  // olive assets directory relative to the document, '/' separators
    HudSnapshot snapshot;
    std::function<void(const std::string&)> on_menu;

    HudTexts t;
    Rml::String realm_name;
    Rml::String realm_subtitle;
    std::vector<ResourceRow> resources;
    std::vector<MenuRow> menu;
    Rml::String active = "world";
    int bell = 0;
    int charter = 0;
    int embassy = 0;
    Rml::String frame_src;
    Rml::String realm_icon;
    Rml::String bell_icon;
    Rml::String charter_icon;
    Rml::String embassy_icon;
    Rml::String tab = "cities";
    Rml::String list_icon;
    std::vector<EntryRow> rows;  // the list of the active tab (at most kMaxListRows)
    int rows_total = 0;
    std::vector<EntryRow> army_rows;
    int army_total = 0;
    std::vector<AffairRow> affairs;
    int affairs_total = 0;
    std::vector<ChronicleRow> chronicle;
    int chronicle_total = 0;

    [[nodiscard]] std::string icon_path(const std::string& name) const {
        return assets_rel + "/icons/" + name + ".svg";
    }

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

    [[nodiscard]] std::string entry_value(const HudEntry& e) const {
        return e.format == HudNumber::Thousands ? format_thousands(*loc, e.value)
                                                : format_number(*loc, e.value);
    }

    // Captions and menu names: everything that does not depend on the snapshot.
    void refresh_chrome() {
        t.holdings_title = text("ui.hud.holdings.title");
        t.armies_title = text("ui.hud.armies.title");
        t.affairs_title = text("ui.hud.affairs.title");
        t.chronicle_title = text("ui.hud.chronicle.title");
        t.tab_cities = text("ui.hud.tab.cities");
        t.tab_armies = text("ui.hud.tab.armies");
        t.tab_dynasties = text("ui.hud.tab.dynasties");
        for (std::size_t i = 0; i < menu.size(); ++i) {
            menu[i].name = text("ui.menu." + std::string(kMenu[i].key));
        }
        handle.DirtyVariable("t");
        handle.DirtyVariable("menu");
    }

    void refresh() {
        refresh_chrome();
        realm_name = snapshot.realm_name;
        realm_subtitle = snapshot.realm_subtitle;
        resources.clear();
        for (const HudResource& r : snapshot.resources) {
            ResourceRow row;
            row.key = r.key;
            row.label = text("ui.res." + r.key);
            row.value = format_number(*loc, r.value);
            row.delta = format_delta(*loc, r.delta);
            row.tone = r.delta == 0 ? "zero" : (r.delta < 0 ? "neg" : "pos");
            row.icon = icon_path("res_" + r.key);
            row.tooltip =
                text("ui.res.tooltip", {Arg("label", row.label), Arg("n", r.value), Arg("delta", row.delta)});
            resources.push_back(std::move(row));
        }
        bell = snapshot.bell_count;
        charter = snapshot.charter_count;
        embassy = snapshot.embassy_count;
        refresh_lists();
        affairs.clear();
        affairs_total = static_cast<int>(snapshot.affairs.size());
        for (std::size_t i = 0; i < snapshot.affairs.size() && i < kMaxAffairs; ++i) {
            const HudAffair& a = snapshot.affairs[i];
            affairs.push_back({text(a.title_key, {Arg("n", a.count)}),
                               text(a.detail_key, {Arg("n", a.count)}),
                               icon_path("alert_" + a.kind)});
        }
        chronicle.clear();
        chronicle_total = static_cast<int>(snapshot.chronicle.size());
        for (std::size_t i = 0; i < snapshot.chronicle.size() && i < kMaxChronicleLines; ++i) {
            const HudChronicleLine& line = snapshot.chronicle[i];
            chronicle.push_back({line.time, text(line.key, {Arg("n", line.count)})});
        }
        for (const char* name : {"realm_name",
                                 "realm_subtitle",
                                 "resources",
                                 "bell",
                                 "charter",
                                 "embassy",
                                 "affairs",
                                 "affairs_total",
                                 "chronicle",
                                 "chronicle_total"}) {
            handle.DirtyVariable(name);
        }
    }

    void
    fill_rows(std::vector<EntryRow>& out, const std::vector<HudEntry>& in, const std::string& icon) const {
        out.clear();
        for (std::size_t i = 0; i < in.size() && i < kMaxListRows; ++i) {
            out.push_back({in[i].name, entry_value(in[i]), icon});
        }
    }

    // The holdings list follows the active tab; the "Войска" block always shows the armies.
    void refresh_lists() {
        const std::vector<HudEntry>* source = &snapshot.cities;
        std::string icon = icon_path("menu_holdings");
        if (tab == "armies") {
            source = &snapshot.armies;
            icon = icon_path("menu_army");
        } else if (tab == "dynasties") {
            source = &snapshot.dynasties;
            icon = icon_path("menu_realm");
        }
        fill_rows(rows, *source, icon);
        rows_total = static_cast<int>(source->size());
        fill_rows(army_rows, snapshot.armies, icon_path("menu_army"));
        army_total = static_cast<int>(snapshot.armies.size());
        for (const char* name : {"tab", "rows", "rows_total", "army_rows", "army_total"}) {
            handle.DirtyVariable(name);
        }
    }
};

HudWindow::HudWindow(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

HudWindow::~HudWindow() {
    if (impl_ != nullptr && impl_->ui != nullptr) {
        Rml::Context* ctx = impl_->ui->context();
        if (Rml::ElementDocument* doc = impl_->find_document()) {
            doc->Close();
        }
        ctx->Update();
        ctx->RemoveDataModel(kModelName);
    }
}

core::Result<std::unique_ptr<HudWindow>> HudWindow::create(engine::UiContext& ui,
                                                           const std::filesystem::path& document,
                                                           const std::filesystem::path& olive_assets,
                                                           const loc::Localizer& loc) {
    using R = core::Result<std::unique_ptr<HudWindow>>;
    auto impl = std::make_unique<Impl>();
    impl->ui = &ui;
    impl->loc = &loc;

    std::error_code ec;
    const std::filesystem::path rel = std::filesystem::relative(
        std::filesystem::absolute(olive_assets), std::filesystem::absolute(document).parent_path(), ec);
    impl->assets_rel = ec ? olive_assets.generic_string() : rel.generic_string();

    Rml::DataModelConstructor ctor = ui.context()->CreateDataModel(kModelName);
    if (!ctor) {
        return R::err(core::Error{94, "cannot create the HUD data model"});
    }
    if (auto res = ctor.RegisterStruct<ResourceRow>()) {
        res.RegisterMember("key", &ResourceRow::key);
        res.RegisterMember("label", &ResourceRow::label);
        res.RegisterMember("value", &ResourceRow::value);
        res.RegisterMember("delta", &ResourceRow::delta);
        res.RegisterMember("tone", &ResourceRow::tone);
        res.RegisterMember("icon", &ResourceRow::icon);
        res.RegisterMember("tooltip", &ResourceRow::tooltip);
    }
    if (auto texts = ctor.RegisterStruct<HudTexts>()) {
        texts.RegisterMember("holdings_title", &HudTexts::holdings_title);
        texts.RegisterMember("armies_title", &HudTexts::armies_title);
        texts.RegisterMember("affairs_title", &HudTexts::affairs_title);
        texts.RegisterMember("chronicle_title", &HudTexts::chronicle_title);
        texts.RegisterMember("tab_cities", &HudTexts::tab_cities);
        texts.RegisterMember("tab_armies", &HudTexts::tab_armies);
        texts.RegisterMember("tab_dynasties", &HudTexts::tab_dynasties);
    }
    ctor.RegisterArray<std::vector<ResourceRow>>();
    if (auto item = ctor.RegisterStruct<MenuRow>()) {
        item.RegisterMember("key", &MenuRow::key);
        item.RegisterMember("name", &MenuRow::name);
        item.RegisterMember("hotkey", &MenuRow::hotkey);
        item.RegisterMember("icon", &MenuRow::icon);
    }
    ctor.RegisterArray<std::vector<MenuRow>>();
    if (auto entry = ctor.RegisterStruct<EntryRow>()) {
        entry.RegisterMember("name", &EntryRow::name);
        entry.RegisterMember("value", &EntryRow::value);
        entry.RegisterMember("icon", &EntryRow::icon);
    }
    ctor.RegisterArray<std::vector<EntryRow>>();
    if (auto affair = ctor.RegisterStruct<AffairRow>()) {
        affair.RegisterMember("title", &AffairRow::title);
        affair.RegisterMember("subtitle", &AffairRow::subtitle);
        affair.RegisterMember("icon", &AffairRow::icon);
    }
    ctor.RegisterArray<std::vector<AffairRow>>();
    if (auto line = ctor.RegisterStruct<ChronicleRow>()) {
        line.RegisterMember("time", &ChronicleRow::time);
        line.RegisterMember("text", &ChronicleRow::text);
    }
    ctor.RegisterArray<std::vector<ChronicleRow>>();

    Impl* m = impl.get();
    for (const MenuEntry& e : kMenu) {
        m->menu.push_back({e.key, {}, e.hotkey, m->icon_path(std::string("menu_") + e.key)});
    }
    m->frame_src = m->assets_rel + "/frame.png";
    m->realm_icon = m->icon_path("menu_realm");
    m->bell_icon = m->icon_path("alert_bell");
    m->charter_icon = m->icon_path("alert_charter");
    m->embassy_icon = m->icon_path("alert_embassy");

    ctor.Bind("t", &m->t);
    ctor.Bind("realm_name", &m->realm_name);
    ctor.Bind("realm_subtitle", &m->realm_subtitle);
    ctor.Bind("resources", &m->resources);
    ctor.Bind("menu", &m->menu);
    ctor.Bind("active", &m->active);
    ctor.Bind("bell", &m->bell);
    ctor.Bind("charter", &m->charter);
    ctor.Bind("embassy", &m->embassy);
    ctor.Bind("frame_src", &m->frame_src);
    ctor.Bind("realm_icon", &m->realm_icon);
    ctor.Bind("bell_icon", &m->bell_icon);
    ctor.Bind("charter_icon", &m->charter_icon);
    ctor.Bind("embassy_icon", &m->embassy_icon);
    ctor.Bind("tab", &m->tab);
    ctor.Bind("rows", &m->rows);
    ctor.Bind("rows_total", &m->rows_total);
    ctor.Bind("army_rows", &m->army_rows);
    ctor.Bind("army_total", &m->army_total);
    ctor.Bind("affairs", &m->affairs);
    ctor.Bind("affairs_total", &m->affairs_total);
    ctor.Bind("chronicle", &m->chronicle);
    ctor.Bind("chronicle_total", &m->chronicle_total);
    m->handle = ctor.GetModelHandle();
    ctor.BindEventCallback("set_tab", [m](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) {
        if (!args.empty()) {
            m->tab = args[0].Get<Rml::String>();
            m->refresh_lists();
        }
    });
    ctor.BindEventCallback("menu", [m](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) {
        if (!args.empty() && m->on_menu) {
            m->on_menu(args[0].Get<Rml::String>());
        }
    });

    Rml::ElementDocument* loaded = ui.load_document(document);
    if (loaded == nullptr) {
        ui.context()->RemoveDataModel(kModelName);
        m->ui = nullptr;
        return R::err(core::Error{95, "cannot load the HUD document " + document.generic_string()});
    }
    m->source_url = loaded->GetSourceURL();
    auto window = std::unique_ptr<HudWindow>(new HudWindow(std::move(impl)));
    window->set_snapshot(HudSnapshot{});
    return R::ok(std::move(window));
}

void HudWindow::set_snapshot(const HudSnapshot& snapshot) {
    impl_->snapshot = snapshot;
    impl_->refresh();
}

void HudWindow::relocalize() {
    impl_->refresh();
}

const HudSnapshot& HudWindow::snapshot() const {
    return impl_->snapshot;
}

std::string_view HudWindow::menu_key(int index) {
    return index >= 0 && index < kMenuCount ? kMenu[static_cast<std::size_t>(index)].key : std::string_view{};
}

std::string HudWindow::menu_name(int index) const {
    return index >= 0 && index < kMenuCount ? impl_->menu[static_cast<std::size_t>(index)].name
                                            : std::string{};
}

void HudWindow::set_active_menu(std::string_view key) {
    if (impl_->active != key) {
        impl_->active = std::string(key);
        impl_->handle.DirtyVariable("active");
    }
}

std::string_view HudWindow::active_menu() const {
    return impl_->active;
}

void HudWindow::set_tab(std::string_view tab) {
    if (tab != "cities" && tab != "armies" && tab != "dynasties") {
        return;
    }
    if (impl_->tab != tab) {
        impl_->tab = std::string(tab);
        impl_->refresh_lists();
    }
}

std::string_view HudWindow::tab() const {
    return impl_->tab;
}

void HudWindow::set_menu_handler(std::function<void(const std::string&)> handler) {
    impl_->on_menu = std::move(handler);
}

Rml::ElementDocument* HudWindow::document() const {
    return impl_->find_document();
}

void HudWindow::show(bool visible) {
    if (Rml::ElementDocument* doc = impl_->find_document()) {
        if (visible) {
            doc->Show(Rml::ModalFlag::None, Rml::FocusFlag::None);
        } else {
            doc->Hide();
        }
    }
}

}  // namespace pat::ui
