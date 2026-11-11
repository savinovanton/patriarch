#include "pat/ui/component_gallery.hpp"

#include <RmlUi/Core.h>

#include <system_error>

namespace pat::ui {

namespace {

constexpr const char* kModelName = "gallery";

// The texts of the page: member name == the last part of the key `ui.gallery.<name>`.
#define PAT_GALLERY_TEXTS(X)                                                                                 \
    X(eyebrow)                                                                                               \
    X(title)                                                                                                 \
    X(subtitle)                                                                                              \
    X(section_buttons)                                                                                       \
    X(section_states)                                                                                        \
    X(section_badges)                                                                                        \
    X(section_tabs)                                                                                          \
    X(section_cards)                                                                                         \
    X(section_progress)                                                                                      \
    X(section_fields)                                                                                        \
    X(section_panels)                                                                                        \
    X(btn_primary)                                                                                           \
    X(btn_secondary)                                                                                         \
    X(btn_disabled)                                                                                          \
    X(action)                                                                                                \
    X(state_default)                                                                                         \
    X(state_hover)                                                                                           \
    X(state_focus)                                                                                           \
    X(state_disabled)                                                                                        \
    X(reason)                                                                                                \
    X(badge_light)                                                                                           \
    X(badge_head)                                                                                            \
    X(badge_unavailable)                                                                                     \
    X(badge_pause)                                                                                           \
    X(tab_overview)                                                                                          \
    X(tab_income)                                                                                            \
    X(tab_buildings)                                                                                         \
    X(card_one_title)                                                                                        \
    X(card_one_detail)                                                                                       \
    X(card_two_title)                                                                                        \
    X(card_two_detail)                                                                                       \
    X(order)                                                                                                 \
    X(order_value)                                                                                           \
    X(row_gifts)                                                                                             \
    X(row_gifts_value)                                                                                       \
    X(row_road)                                                                                              \
    X(row_road_value)                                                                                        \
    X(row_total)                                                                                             \
    X(row_total_value)                                                                                       \
    X(search_hint)                                                                                           \
    X(chip_availability)                                                                                     \
    X(chip_faith)                                                                                            \
    X(panel_window)                                                                                          \
    X(panel_light)                                                                                           \
    X(panel_dark_label)                                                                                      \
    X(panel_dark)

struct GalleryTexts {
#define X(name) Rml::String name;
    PAT_GALLERY_TEXTS(X)
#undef X
};

}  // namespace

struct ComponentGallery::Impl {
    engine::UiContext* ui = nullptr;
    const loc::Localizer* loc = nullptr;
    Rml::DataModelHandle handle;
    std::string source_url;
    GalleryTexts t;
    Rml::String icons;  // directory of the icons relative to the document
    Rml::String tab = "overview";
    Rml::String search;
    int card = 1;
    int chip = 0;
    int presses = 0;
    int order_percent = 72;

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

    void refresh_texts() {
#define X(name) t.name = loc->format("ui.gallery." #name);
        PAT_GALLERY_TEXTS(X)
#undef X
        handle.DirtyVariable("t");
    }
};

ComponentGallery::ComponentGallery(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

ComponentGallery::~ComponentGallery() {
    if (impl_ != nullptr && impl_->ui != nullptr) {
        Rml::Context* ctx = impl_->ui->context();
        if (Rml::ElementDocument* doc = impl_->find_document()) {
            doc->Close();
        }
        ctx->Update();
        ctx->RemoveDataModel(kModelName);
    }
}

core::Result<std::unique_ptr<ComponentGallery>>
ComponentGallery::create(engine::UiContext& ui,
                         const std::filesystem::path& document,
                         const std::filesystem::path& icons,
                         const loc::Localizer& loc) {
    using R = core::Result<std::unique_ptr<ComponentGallery>>;
    auto impl = std::make_unique<Impl>();
    impl->ui = &ui;
    impl->loc = &loc;

    std::error_code ec;
    const std::filesystem::path rel = std::filesystem::relative(
        std::filesystem::absolute(icons), std::filesystem::absolute(document).parent_path(), ec);
    impl->icons = ec ? icons.generic_string() : rel.generic_string();

    Rml::DataModelConstructor ctor = ui.context()->CreateDataModel(kModelName);
    if (!ctor) {
        return R::err(core::Error{96, "cannot create the component gallery data model"});
    }
    if (auto texts = ctor.RegisterStruct<GalleryTexts>()) {
#define X(name) texts.RegisterMember(#name, &GalleryTexts::name);
        PAT_GALLERY_TEXTS(X)
#undef X
    }
    Impl* m = impl.get();
    ctor.Bind("t", &m->t);
    ctor.Bind("icons", &m->icons);
    ctor.Bind("tab", &m->tab);
    ctor.Bind("search", &m->search);
    ctor.Bind("card", &m->card);
    ctor.Bind("chip", &m->chip);
    ctor.Bind("order_percent", &m->order_percent);
    m->handle = ctor.GetModelHandle();
    ctor.BindEventCallback("press",
                           [m](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) { ++m->presses; });
    ctor.BindEventCallback("set_tab", [m](Rml::DataModelHandle h, Rml::Event&, const Rml::VariantList& args) {
        if (!args.empty()) {
            m->tab = args[0].Get<Rml::String>();
            h.DirtyVariable("tab");
        }
    });
    ctor.BindEventCallback("select_card",
                           [m](Rml::DataModelHandle h, Rml::Event&, const Rml::VariantList& args) {
                               if (!args.empty()) {
                                   m->card = args[0].Get<int>();
                                   h.DirtyVariable("card");
                               }
                           });
    ctor.BindEventCallback("toggle_chip",
                           [m](Rml::DataModelHandle h, Rml::Event&, const Rml::VariantList& args) {
                               if (!args.empty()) {
                                   const int chip = args[0].Get<int>();
                                   m->chip = m->chip == chip ? 0 : chip;
                                   h.DirtyVariable("chip");
                               }
                           });
    m->refresh_texts();

    Rml::ElementDocument* loaded = ui.load_document(document);
    if (loaded == nullptr) {
        ui.context()->RemoveDataModel(kModelName);
        m->ui = nullptr;
        return R::err(core::Error{97, "cannot load the component gallery " + document.generic_string()});
    }
    m->source_url = loaded->GetSourceURL();
    return R::ok(std::unique_ptr<ComponentGallery>(new ComponentGallery(std::move(impl))));
}

void ComponentGallery::relocalize() {
    impl_->refresh_texts();
}

void ComponentGallery::set_tab(std::string_view tab) {
    if (tab == "overview" || tab == "income" || tab == "buildings") {
        impl_->tab = std::string(tab);
        impl_->handle.DirtyVariable("tab");
    }
}

std::string_view ComponentGallery::tab() const {
    return impl_->tab;
}

int ComponentGallery::selected_card() const {
    return impl_->card;
}

int ComponentGallery::presses() const {
    return impl_->presses;
}

Rml::ElementDocument* ComponentGallery::document() const {
    return impl_->find_document();
}

void ComponentGallery::show(bool visible) {
    if (Rml::ElementDocument* doc = impl_->find_document()) {
        if (visible) {
            doc->Show(Rml::ModalFlag::None, Rml::FocusFlag::None);
        } else {
            doc->Hide();
        }
    }
}

}  // namespace pat::ui
