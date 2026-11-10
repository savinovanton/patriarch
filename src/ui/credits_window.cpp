#include "pat/ui/credits_window.hpp"

#include <RmlUi/Core.h>

#include <utility>

namespace pat::ui {

namespace {

constexpr const char* kModelName = "credits";

struct ItemRow {
    Rml::String name;
    Rml::String meta;
    Rml::String text;
};

struct SectionRow {
    Rml::String title;
    std::vector<ItemRow> items;
};

/// Captions of the window (one struct so the document reads them as `t.name`).
struct CreditsTexts {
    Rml::String title;
    Rml::String close;
    Rml::String empty;
};

}  // namespace

struct CreditsWindow::Impl {
    engine::UiContext* ui = nullptr;
    const loc::Localizer* loc = nullptr;
    Rml::DataModelHandle handle;
    std::string source_url;  // identifies the document even after a hot reload replaced it
    CreditsSnapshot snapshot;
    std::function<void()> on_close;
    bool visible = false;

    // ---- bound variables (names are used by the document)
    CreditsTexts t;
    std::vector<SectionRow> sections;
    bool is_empty = true;

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

    void apply_visibility() {
        if (Rml::ElementDocument* doc = find_document()) {
            if (visible) {
                doc->Show();
            } else {
                doc->Hide();
            }
        }
    }

    void refresh_chrome() {
        t.title = loc->format("ui.credits.title");
        t.close = loc->format("ui.credits.close");
        t.empty = loc->format("ui.credits.empty");
        handle.DirtyVariable("t");
    }

    void refresh_snapshot() {
        sections.clear();
        for (const CreditsSection& section : snapshot.sections) {
            SectionRow row;
            row.title = section.title;
            for (const CreditsItem& item : section.items) {
                row.items.push_back({item.name, item.meta, item.text});
            }
            sections.push_back(std::move(row));
        }
        is_empty = sections.empty();
        handle.DirtyVariable("sections");
        handle.DirtyVariable("is_empty");
    }
};

CreditsWindow::CreditsWindow(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

CreditsWindow::~CreditsWindow() {
    if (impl_ != nullptr && impl_->ui != nullptr) {
        Rml::Context* ctx = impl_->ui->context();
        if (Rml::ElementDocument* doc = impl_->find_document()) {
            doc->Close();
        }
        ctx->Update();  // lets the context actually drop the closed document before the model goes
        ctx->RemoveDataModel(kModelName);
    }
}

core::Result<std::unique_ptr<CreditsWindow>> CreditsWindow::create(engine::UiContext& ui,
                                                                   const std::filesystem::path& document,
                                                                   const loc::Localizer& loc) {
    using R = core::Result<std::unique_ptr<CreditsWindow>>;
    auto impl = std::make_unique<Impl>();
    impl->ui = &ui;
    impl->loc = &loc;

    Rml::DataModelConstructor ctor = ui.context()->CreateDataModel(kModelName);
    if (!ctor) {
        return R::err(core::Error{96, "cannot create the credits data model"});
    }
    if (auto texts = ctor.RegisterStruct<CreditsTexts>()) {
        texts.RegisterMember("title", &CreditsTexts::title);
        texts.RegisterMember("close", &CreditsTexts::close);
        texts.RegisterMember("empty", &CreditsTexts::empty);
    }
    if (auto item = ctor.RegisterStruct<ItemRow>()) {
        item.RegisterMember("name", &ItemRow::name);
        item.RegisterMember("meta", &ItemRow::meta);
        item.RegisterMember("text", &ItemRow::text);
    }
    ctor.RegisterArray<std::vector<ItemRow>>();
    if (auto section = ctor.RegisterStruct<SectionRow>()) {
        section.RegisterMember("title", &SectionRow::title);
        section.RegisterMember("items", &SectionRow::items);
    }
    ctor.RegisterArray<std::vector<SectionRow>>();

    Impl* m = impl.get();
    ctor.Bind("t", &m->t);
    ctor.Bind("sections", &m->sections);
    ctor.Bind("is_empty", &m->is_empty);
    m->handle = ctor.GetModelHandle();

    ctor.BindEventCallback("close", [m](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
        m->visible = false;
        m->apply_visibility();
        if (m->on_close) {
            m->on_close();
        }
    });

    Rml::ElementDocument* loaded = ui.load_document(document);
    if (loaded == nullptr) {
        ui.context()->RemoveDataModel(kModelName);
        m->ui = nullptr;
        return R::err(core::Error{97, "cannot load the credits document " + document.generic_string()});
    }
    m->source_url = loaded->GetSourceURL();
    auto window = std::unique_ptr<CreditsWindow>(new CreditsWindow(std::move(impl)));
    window->impl_->refresh_chrome();
    window->impl_->refresh_snapshot();
    window->impl_->apply_visibility();
    return R::ok(std::move(window));
}

void CreditsWindow::set_snapshot(const CreditsSnapshot& snapshot) {
    impl_->snapshot = snapshot;
    impl_->refresh_snapshot();
}

const CreditsSnapshot& CreditsWindow::snapshot() const {
    return impl_->snapshot;
}

void CreditsWindow::relocalize() {
    impl_->refresh_chrome();
}

void CreditsWindow::set_close_handler(std::function<void()> handler) {
    impl_->on_close = std::move(handler);
}

bool CreditsWindow::visible() const {
    return impl_->visible;
}

void CreditsWindow::show(bool visible) {
    impl_->visible = visible;
    impl_->apply_visibility();
}

void CreditsWindow::toggle() {
    show(!impl_->visible);
}

Rml::ElementDocument* CreditsWindow::document() const {
    return impl_->find_document();
}

}  // namespace pat::ui
