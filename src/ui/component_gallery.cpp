#include "pat/ui/component_gallery.hpp"

#include "pat/ui/explanation.hpp"

#include <RmlUi/Core.h>

#include <iterator>
#include <system_error>
#include <utility>
#include <vector>

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
    X(panel_dark)                                                                                            \
    X(section_tooltips)                                                                                      \
    X(tip_cost_title)                                                                                        \
    X(tip_cost_note)                                                                                         \
    X(tip_cost_gifts)                                                                                        \
    X(tip_cost_gifts_value)                                                                                  \
    X(tip_cost_road)                                                                                         \
    X(tip_cost_road_value)                                                                                   \
    X(tip_cost_total)                                                                                        \
    X(tip_cost_total_value)                                                                                  \
    X(tip_cost_footer)                                                                                       \
    X(tip_treasury_title)                                                                                    \
    X(tip_treasury_before)                                                                                   \
    X(tip_treasury_after)                                                                                    \
    X(tip_treasury_income)                                                                                   \
    X(tip_treasury_income_value)                                                                             \
    X(tip_treasury_upkeep)                                                                                   \
    X(tip_treasury_upkeep_value)                                                                             \
    X(tip_treasury_total)                                                                                    \
    X(tip_treasury_total_value)                                                                              \
    X(tip_reason_title)                                                                                      \
    X(tip_reason_text)                                                                                       \
    X(tip_reason_footer)                                                                                     \
    X(tip_skill_title)                                                                                       \
    X(tip_skill_note)                                                                                        \
    X(tip_skill_base)                                                                                        \
    X(tip_skill_education)                                                                                   \
    X(tip_skill_trait)                                                                                       \
    X(tip_skill_total)                                                                                       \
    X(tip_skill_hint)                                                                                        \
    X(tip_short_title)                                                                                       \
    X(tip_short_detail)                                                                                      \
    X(tip_short_total)                                                                                       \
    X(tip_short_footer)                                                                                      \
    X(page_components)                                                                                       \
    X(page_tips)

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
    Rml::String page = "components";
    Rml::String search;
    int card = 1;
    int chip = 0;
    int presses = 0;
    int order_percent = 72;
    std::vector<Explanation> tips;  // cost, treasury (before), treasury (after), reason, skill: the samples

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

    void build_tips() {
        tips.clear();
        Explanation cost{t.tip_cost_title, t.tip_cost_total_value, t.tip_cost_note, {}};
        cost.terms.push_back({t.tip_cost_gifts, t.tip_cost_gifts_value, "", {}});
        cost.terms.push_back({t.tip_cost_road, t.tip_cost_road_value, "", {}});
        cost.total_label = t.tip_cost_total;
        cost.footer = t.tip_cost_footer;
        tips.push_back(cost);

        Explanation treasury;
        treasury.label = t.tip_treasury_title;
        treasury.headline = t.tip_treasury_before;
        treasury.value = t.tip_treasury_total_value;
        treasury.total_label = t.tip_treasury_total;
        treasury.terms.push_back({t.tip_treasury_income, t.tip_treasury_income_value, "", {}});
        treasury.terms.push_back({t.tip_treasury_upkeep, t.tip_treasury_upkeep_value, "", {}});
        tips.push_back(treasury);
        treasury.headline = t.tip_treasury_after;
        tips.push_back(treasury);

        Explanation reason;
        reason.label = t.tip_reason_title;
        reason.lines.push_back(t.tip_reason_text);
        reason.footer = t.tip_reason_footer;
        tips.push_back(reason);

        Explanation skill{t.tip_skill_title, "14", t.tip_skill_note, {}};
        skill.terms.push_back({t.tip_skill_base, "+10", "", {}});
        skill.terms.push_back({t.tip_skill_education, "+3", "", {}});
        skill.terms.push_back({t.tip_skill_trait, "+1", "", {}});
        skill.total_label = t.tip_skill_total;
        skill.wide = true;
        tips.push_back(skill);

        Explanation shortage{t.tip_short_title, t.tip_cost_total_value, "", {}};
        shortage.tone = TipTone::Warning;
        shortage.lines.push_back(t.tip_short_detail);
        shortage.terms.push_back({t.tip_cost_gifts, t.tip_cost_gifts_value, "", {}});
        shortage.terms.push_back({t.tip_cost_road, t.tip_cost_road_value, "", {}});
        shortage.total_label = t.tip_short_total;
        shortage.footer = t.tip_short_footer;
        tips.push_back(shortage);
    }

    /// Fills the static tooltip samples of the page with the same markup the live tooltips use.
    void fill_tips() {
        static constexpr const char* kIds[] = {
            "tip-cost", "tip-before", "tip-after", "tip-reason", "tip-skill", "tip-shortage"};
        Rml::ElementDocument* doc = find_document();
        if (doc == nullptr) {
            return;
        }
        for (std::size_t i = 0; i < tips.size() && i < std::size(kIds); ++i) {
            if (Rml::Element* el = doc->GetElementById(kIds[i])) {
                el->SetClass("tip-wide", tips[i].wide);
                el->SetInnerRML(render_tooltip_body(tips[i], "", icons));
            }
        }
    }

    void refresh_texts() {
#define X(name) t.name = loc->format("ui.gallery." #name);
        PAT_GALLERY_TEXTS(X)
#undef X
        handle.DirtyVariable("t");
        build_tips();
        fill_tips();
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
    ctor.Bind("page", &m->page);
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
    ctor.BindEventCallback("set_page",
                           [m](Rml::DataModelHandle h, Rml::Event&, const Rml::VariantList& args) {
                               if (!args.empty()) {
                                   m->page = args[0].Get<Rml::String>();
                                   h.DirtyVariable("page");
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
    m->fill_tips();
    return R::ok(std::unique_ptr<ComponentGallery>(new ComponentGallery(std::move(impl))));
}

const Explanation* ComponentGallery::explanation(std::string_view key) const {
    static constexpr std::pair<std::string_view, std::size_t> kKeys[] = {{"gallery:cost", 0},
                                                                         {"gallery:treasury", 1},
                                                                         {"gallery:reason", 3},
                                                                         {"gallery:skill", 4},
                                                                         {"gallery:shortage", 5}};
    for (const auto& [name, index] : kKeys) {
        if (key == name && index < impl_->tips.size()) {
            return &impl_->tips[index];
        }
    }
    return nullptr;
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

void ComponentGallery::set_page(std::string_view page) {
    if (page == "components" || page == "tips") {
        impl_->page = std::string(page);
        impl_->handle.DirtyVariable("page");
    }
}

std::string_view ComponentGallery::page() const {
    return impl_->page;
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
