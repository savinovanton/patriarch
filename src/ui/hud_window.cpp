#include "pat/ui/hud_window.hpp"

#include "pat/ui/text.hpp"

#include <RmlUi/Core.h>

#include <algorithm>
#include <array>
#include <system_error>

namespace pat::ui {

namespace {

constexpr const char* kModelName = "hud";

using loc::Arg;

// Entries of the left navigation: the key (ui.nav.<key>) and the icon file (assets/ui/patriarch/icons).
struct NavEntry {
    const char* key;
    const char* icon;
};

constexpr std::array<NavEntry, HudWindow::kNavCount + 1> kNav{{
    {"dynasty", "dynasty"},
    {"holdings", "castle"},
    {"court", "crown"},
    {"treasury", "coin"},
    {"church", "church"},
    {"army", "sword"},
    {"diplomacy", "globe"},
    {"intrigue", "quill"},
    {"chronicle", "scroll"},
    {"system", "gear"},
}};

constexpr std::array<const char*, HudWindow::kModeCount> kModes{"political", "relief", "income", "faith"};

// Resource icons of the top bar by key.
const char* resource_icon(std::string_view key) {
    if (key == "treasury") {
        return "coin";
    }
    if (key == "prestige") {
        return "crown";
    }
    if (key == "influence") {
        return "people";
    }
    if (key == "piety") {
        return "church";
    }
    if (key == "militia") {
        return "sword";
    }
    return "diamond";
}

/// Captions of the frame (one struct so the document reads them as `t.name`).
struct HudTexts {
    Rml::String logo;
    Rml::String house;
    Rml::String map_title;
    Rml::String hints;
};

struct ResourceRow {
    Rml::String key;
    Rml::String label;
    Rml::String value;
    Rml::String note;
    Rml::String tone;     // "pos", "neg" or "zero": the colour of the note
    Rml::String icon;     // path of the icon relative to the document
    Rml::String tooltip;  // "Казна: 1 248 ₽ (+42 / мес.)"
};

struct NavRow {
    Rml::String key;
    Rml::String name;
    Rml::String icon;
    Rml::String hint;  // "Скоро" for an entry without a screen, empty otherwise
    bool available = false;
};

struct ModeRow {
    Rml::String key;
    Rml::String name;
    Rml::String hint;
    bool available = false;
};

}  // namespace

HudSnapshot make_test_hud() {
    HudSnapshot hud;
    hud.house = "Голицыных";
    hud.resources = {
        {"treasury", 1248, 42, 0, HudNote::Delta},
        {"prestige", 68, 3, 0, HudNote::Delta},
        {"influence", 64, 0, 0, HudNote::Caption},
        {"piety", 72, 0, 0, HudNote::Caption},
        {"militia", 1240, 0, 1600, HudNote::Capacity},
    };
    return hud;
}

struct HudWindow::Impl {
    engine::UiContext* ui = nullptr;
    const loc::Localizer* loc = nullptr;
    Rml::DataModelHandle handle;
    std::string source_url;
    std::string icons_rel;  // icon directory relative to the document, '/' separators
    HudSnapshot snapshot;
    std::function<void(const std::string&)> on_nav;
    std::function<void(const std::string&)> on_mode;

    HudTexts t;
    Rml::String icons;
    Rml::String logo_icon;
    Rml::String active = "";
    Rml::String mode = "political";
    std::vector<ResourceRow> resources;
    std::vector<NavRow> nav;  // the nine entries
    NavRow system;            // settings
    std::vector<ModeRow> modes;

    [[nodiscard]] std::string icon_path(const std::string& name, const char* colour) const {
        return icons_rel + "/" + name + "@" + colour + ".svg";
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

    NavRow* find_nav(std::string_view key) {
        if (key == "system") {
            return &system;
        }
        for (NavRow& row : nav) {
            if (row.key == key) {
                return &row;
            }
        }
        return nullptr;
    }

    ModeRow* find_mode(std::string_view key) {
        for (ModeRow& row : modes) {
            if (row.key == key) {
                return &row;
            }
        }
        return nullptr;
    }

    // Names, icons (gold for the active entry, quiet for the others) and the "soon" hints.
    void refresh_nav() {
        const std::string soon = text("ui.nav.soon");
        const auto fill = [&](NavRow& row, const NavEntry& entry) {
            row.name = text(std::string("ui.nav.") + entry.key);
            row.icon = icon_path(entry.icon, row.key == active ? "gold" : "quiet");
            row.hint = row.available ? std::string() : soon;
        };
        for (std::size_t i = 0; i < nav.size(); ++i) {
            fill(nav[i], kNav[i]);
        }
        fill(system, kNav[kNav.size() - 1]);
        const std::string mode_soon = text("ui.hud.mode.soon");
        for (ModeRow& row : modes) {
            row.name = text("ui.hud.mode." + row.key);
            row.hint = row.available ? std::string() : mode_soon;
        }
        handle.DirtyVariable("nav");
        handle.DirtyVariable("system");
        handle.DirtyVariable("modes");
    }

    void refresh() {
        t.logo = text("ui.hud.logo");
        t.house = text("ui.hud.house", {Arg("name", upper_text(snapshot.house))});
        t.map_title = text("ui.hud.map_title");
        t.hints = text("ui.hud.hints");
        handle.DirtyVariable("t");
        refresh_nav();

        resources.clear();
        for (const HudResource& r : snapshot.resources) {
            ResourceRow row;
            row.key = r.key;
            row.label = text("ui.res." + r.key);
            row.value = r.key == "treasury" ? text("ui.fmt.rouble", {Arg("n", r.value)})
                                            : format_number(*loc, r.value);
            row.tone = "zero";
            switch (r.note) {
                case HudNote::Delta:
                    row.note = text("ui.hud.res.delta", {Arg("delta", format_delta(*loc, r.delta))});
                    row.tone = r.delta == 0 ? "zero" : (r.delta < 0 ? "neg" : "pos");
                    break;
                case HudNote::Capacity:
                    row.note = text("ui.hud.res.capacity", {Arg("n", r.capacity)});
                    break;
                case HudNote::Caption:
                    row.note = text("ui.hud.res." + r.key + ".note");
                    break;
            }
            row.icon = icon_path(resource_icon(r.key), "gold");
            row.tooltip = text("ui.res.tooltip",
                               {Arg("label", row.label), Arg("value", row.value), Arg("note", row.note)});
            resources.push_back(std::move(row));
        }
        handle.DirtyVariable("resources");
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
                                                           const std::filesystem::path& icons,
                                                           const loc::Localizer& loc) {
    using R = core::Result<std::unique_ptr<HudWindow>>;
    auto impl = std::make_unique<Impl>();
    impl->ui = &ui;
    impl->loc = &loc;

    std::error_code ec;
    const std::filesystem::path rel = std::filesystem::relative(
        std::filesystem::absolute(icons), std::filesystem::absolute(document).parent_path(), ec);
    impl->icons_rel = ec ? icons.generic_string() : rel.generic_string();

    Rml::DataModelConstructor ctor = ui.context()->CreateDataModel(kModelName);
    if (!ctor) {
        return R::err(core::Error{94, "cannot create the HUD data model"});
    }
    if (auto res = ctor.RegisterStruct<ResourceRow>()) {
        res.RegisterMember("key", &ResourceRow::key);
        res.RegisterMember("label", &ResourceRow::label);
        res.RegisterMember("value", &ResourceRow::value);
        res.RegisterMember("note", &ResourceRow::note);
        res.RegisterMember("tone", &ResourceRow::tone);
        res.RegisterMember("icon", &ResourceRow::icon);
        res.RegisterMember("tooltip", &ResourceRow::tooltip);
    }
    ctor.RegisterArray<std::vector<ResourceRow>>();
    if (auto texts = ctor.RegisterStruct<HudTexts>()) {
        texts.RegisterMember("logo", &HudTexts::logo);
        texts.RegisterMember("house", &HudTexts::house);
        texts.RegisterMember("map_title", &HudTexts::map_title);
        texts.RegisterMember("hints", &HudTexts::hints);
    }
    if (auto item = ctor.RegisterStruct<NavRow>()) {
        item.RegisterMember("key", &NavRow::key);
        item.RegisterMember("name", &NavRow::name);
        item.RegisterMember("icon", &NavRow::icon);
        item.RegisterMember("hint", &NavRow::hint);
        item.RegisterMember("available", &NavRow::available);
    }
    ctor.RegisterArray<std::vector<NavRow>>();
    if (auto item = ctor.RegisterStruct<ModeRow>()) {
        item.RegisterMember("key", &ModeRow::key);
        item.RegisterMember("name", &ModeRow::name);
        item.RegisterMember("hint", &ModeRow::hint);
        item.RegisterMember("available", &ModeRow::available);
    }
    ctor.RegisterArray<std::vector<ModeRow>>();

    Impl* m = impl.get();
    for (int i = 0; i < kNavCount; ++i) {
        NavRow row;
        row.key = kNav[static_cast<std::size_t>(i)].key;
        m->nav.push_back(std::move(row));
    }
    m->system.key = "system";
    for (const char* key : kModes) {
        ModeRow row;
        row.key = key;
        row.available = std::string_view(key) == "political";
        m->modes.push_back(std::move(row));
    }
    m->icons = m->icons_rel;
    m->logo_icon = m->icon_path("shield", "gold");

    ctor.Bind("t", &m->t);
    ctor.Bind("logo_icon", &m->logo_icon);
    ctor.Bind("resources", &m->resources);
    ctor.Bind("nav", &m->nav);
    ctor.Bind("system", &m->system);
    ctor.Bind("active", &m->active);
    ctor.Bind("modes", &m->modes);
    ctor.Bind("mode", &m->mode);
    m->handle = ctor.GetModelHandle();
    ctor.BindEventCallback("nav", [m](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) {
        if (!args.empty() && m->on_nav) {
            if (NavRow* row = m->find_nav(args[0].Get<Rml::String>()); row != nullptr && row->available) {
                m->on_nav(row->key);
            }
        }
    });
    ctor.BindEventCallback("mode", [m](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) {
        if (!args.empty() && m->on_mode) {
            if (ModeRow* row = m->find_mode(args[0].Get<Rml::String>()); row != nullptr && row->available) {
                m->on_mode(row->key);
            }
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

std::string_view HudWindow::nav_key(int index) {
    return index >= 0 && index <= kNavCount ? kNav[static_cast<std::size_t>(index)].key : std::string_view{};
}

std::string HudWindow::nav_name(int index) const {
    if (index < 0 || index > kNavCount) {
        return {};
    }
    return index == kNavCount ? impl_->system.name : impl_->nav[static_cast<std::size_t>(index)].name;
}

void HudWindow::set_active_nav(std::string_view key) {
    if (impl_->active != key) {
        impl_->active = std::string(key);
        impl_->handle.DirtyVariable("active");
        impl_->refresh_nav();
    }
}

std::string_view HudWindow::active_nav() const {
    return impl_->active;
}

void HudWindow::set_nav_available(std::string_view key, bool available) {
    if (NavRow* row = impl_->find_nav(key); row != nullptr && row->available != available) {
        row->available = available;
        impl_->refresh_nav();
    }
}

bool HudWindow::nav_available(std::string_view key) const {
    const NavRow* row = impl_->find_nav(key);
    return row != nullptr && row->available;
}

void HudWindow::set_nav_handler(std::function<void(const std::string&)> handler) {
    impl_->on_nav = std::move(handler);
}

void HudWindow::press_nav(std::string_view key) {
    if (const NavRow* row = impl_->find_nav(key); row != nullptr && row->available && impl_->on_nav) {
        impl_->on_nav(row->key);
    }
}

std::string_view HudWindow::mode_key(int index) {
    return index >= 0 && index < kModeCount ? kModes[static_cast<std::size_t>(index)] : std::string_view{};
}

void HudWindow::set_map_mode(std::string_view key) {
    if (impl_->mode != key && impl_->find_mode(key) != nullptr) {
        impl_->mode = std::string(key);
        impl_->handle.DirtyVariable("mode");
    }
}

std::string_view HudWindow::map_mode() const {
    return impl_->mode;
}

void HudWindow::set_mode_available(std::string_view key, bool available) {
    if (ModeRow* row = impl_->find_mode(key); row != nullptr && row->available != available) {
        row->available = available;
        impl_->refresh_nav();
    }
}

bool HudWindow::mode_available(std::string_view key) const {
    for (const ModeRow& row : impl_->modes) {
        if (row.key == key) {
            return row.available;
        }
    }
    return false;
}

std::string_view HudWindow::next_mode() const {
    std::size_t current = 0;
    for (std::size_t i = 0; i < impl_->modes.size(); ++i) {
        if (impl_->modes[i].key == impl_->mode) {
            current = i;
        }
    }
    for (std::size_t step = 1; step <= impl_->modes.size(); ++step) {
        const ModeRow& row = impl_->modes[(current + step) % impl_->modes.size()];
        if (row.available) {
            return kModes[(current + step) % impl_->modes.size()];
        }
    }
    return impl_->mode;
}

void HudWindow::set_mode_handler(std::function<void(const std::string&)> handler) {
    impl_->on_mode = std::move(handler);
}

void HudWindow::press_mode(std::string_view key) {
    if (const ModeRow* row = impl_->find_mode(key); row != nullptr && row->available && impl_->on_mode) {
        impl_->on_mode(row->key);
    }
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
