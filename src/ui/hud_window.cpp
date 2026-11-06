#include "pat/ui/hud_window.hpp"

#include <RmlUi/Core.h>

#include <array>
#include <system_error>

namespace pat::ui {

namespace {

constexpr const char* kModelName = "hud";

struct MenuEntry {
    const char* key;
    const char* name;
    const char* hotkey;
};

constexpr std::array<MenuEntry, HudWindow::kMenuCount> kMenu{{
    {"world", "Мир", "1"},
    {"realm", "Держава", "2"},
    {"holdings", "Владения", "3"},
    {"army", "Войско", "4"},
    {"church", "Церковь", "5"},
    {"economy", "Хозяйство", "6"},
    {"diplomacy", "Дипломатия", "7"},
    {"chronicle", "Летопись", "8"},
    {"system", "Меню", "Esc"},
}};

struct ResourceRow {
    Rml::String key;
    Rml::String label;
    Rml::String value;
    Rml::String delta;
    Rml::String tone;  // "pos", "neg" or "zero"
    Rml::String icon;  // path of the icon relative to the document
};

struct MenuRow {
    Rml::String key;
    Rml::String name;
    Rml::String hotkey;
    Rml::String icon;
};

}  // namespace

HudSnapshot make_test_hud() {
    HudSnapshot hud;
    hud.realm_name = "Московское царство";
    hud.realm_subtitle = "Государев двор";
    hud.resources = {
        {"treasury", "Казна", "12 480", "+84"},
        {"grain", "Запасы", "3 240", "+62"},
        {"army", "Войско", "18 600", "+240"},
        {"faith", "Вера", "72", "+2"},
        {"influence", "Влияние", "48", "+1"},
    };
    hud.bell_count = 2;
    hud.charter_count = 3;
    hud.embassy_count = 0;
    return hud;
}

struct HudWindow::Impl {
    engine::UiContext* ui = nullptr;
    Rml::DataModelHandle handle;
    std::string source_url;
    std::string assets_rel;  // olive assets directory relative to the document, '/' separators
    HudSnapshot snapshot;
    std::function<void(const std::string&)> on_menu;

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

    void refresh() {
        realm_name = snapshot.realm_name;
        realm_subtitle = snapshot.realm_subtitle;
        resources.clear();
        for (const HudResource& r : snapshot.resources) {
            ResourceRow row;
            row.key = r.key;
            row.label = r.label;
            row.value = r.value;
            row.delta = r.delta;
            row.tone = r.delta.empty() || r.delta == "0" ? "zero" : (r.delta.front() == '-' ? "neg" : "pos");
            row.icon = icon_path("res_" + r.key);
            resources.push_back(std::move(row));
        }
        bell = snapshot.bell_count;
        charter = snapshot.charter_count;
        embassy = snapshot.embassy_count;
        for (const char* name : {"realm_name", "realm_subtitle", "resources", "bell", "charter", "embassy"}) {
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
                                                           const std::filesystem::path& olive_assets) {
    using R = core::Result<std::unique_ptr<HudWindow>>;
    auto impl = std::make_unique<Impl>();
    impl->ui = &ui;

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
    }
    ctor.RegisterArray<std::vector<ResourceRow>>();
    if (auto item = ctor.RegisterStruct<MenuRow>()) {
        item.RegisterMember("key", &MenuRow::key);
        item.RegisterMember("name", &MenuRow::name);
        item.RegisterMember("hotkey", &MenuRow::hotkey);
        item.RegisterMember("icon", &MenuRow::icon);
    }
    ctor.RegisterArray<std::vector<MenuRow>>();

    Impl* m = impl.get();
    for (const MenuEntry& e : kMenu) {
        m->menu.push_back({e.key, e.name, e.hotkey, m->icon_path(std::string("menu_") + e.key)});
    }
    m->frame_src = m->assets_rel + "/frame.png";
    m->realm_icon = m->icon_path("menu_realm");
    m->bell_icon = m->icon_path("alert_bell");
    m->charter_icon = m->icon_path("alert_charter");
    m->embassy_icon = m->icon_path("alert_embassy");

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
    m->handle = ctor.GetModelHandle();
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

const HudSnapshot& HudWindow::snapshot() const {
    return impl_->snapshot;
}

std::string_view HudWindow::menu_key(int index) {
    return index >= 0 && index < kMenuCount ? kMenu[static_cast<std::size_t>(index)].key : std::string_view{};
}

std::string_view HudWindow::menu_name(int index) {
    return index >= 0 && index < kMenuCount ? kMenu[static_cast<std::size_t>(index)].name
                                            : std::string_view{};
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
