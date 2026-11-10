#include "pat/ui/time_bar.hpp"

#include <RmlUi/Core.h>

namespace pat::ui {

namespace {
constexpr const char* kModelName = "timebar";
}

struct TimeBar::Impl {
    engine::UiContext* ui = nullptr;
    const loc::Localizer* loc = nullptr;
    Rml::DataModelHandle handle;
    std::string source_url;
    Rml::String pause_text;
    Rml::String date_text;
    int speed = 0;
    std::function<void(int)> on_speed;

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
};

TimeBar::TimeBar(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

TimeBar::~TimeBar() {
    if (impl_ != nullptr && impl_->ui != nullptr) {
        Rml::Context* ctx = impl_->ui->context();
        if (Rml::ElementDocument* doc = impl_->find_document()) {
            doc->Close();
        }
        ctx->Update();
        ctx->RemoveDataModel(kModelName);
    }
}

core::Result<std::unique_ptr<TimeBar>>
TimeBar::create(engine::UiContext& ui, const std::filesystem::path& document, const loc::Localizer& loc) {
    using R = core::Result<std::unique_ptr<TimeBar>>;
    auto impl = std::make_unique<Impl>();
    impl->ui = &ui;
    impl->loc = &loc;

    Rml::DataModelConstructor ctor = ui.context()->CreateDataModel(kModelName);
    if (!ctor) {
        return R::err(core::Error{92, "cannot create the time bar data model"});
    }
    Impl* m = impl.get();
    ctor.Bind("pause_text", &m->pause_text);
    ctor.Bind("date_text", &m->date_text);
    ctor.Bind("speed", &m->speed);
    m->handle = ctor.GetModelHandle();
    ctor.BindEventCallback("set_speed", [m](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) {
        if (!args.empty() && m->on_speed) {
            m->on_speed(args[0].Get<int>());
        }
    });

    Rml::ElementDocument* loaded = ui.load_document(document);
    if (loaded == nullptr) {
        ui.context()->RemoveDataModel(kModelName);
        m->ui = nullptr;
        return R::err(core::Error{93, "cannot load the time bar document " + document.generic_string()});
    }
    m->source_url = loaded->GetSourceURL();
    auto bar = std::unique_ptr<TimeBar>(new TimeBar(std::move(impl)));
    bar->relocalize();
    return R::ok(std::move(bar));
}

void TimeBar::relocalize() {
    impl_->pause_text = impl_->loc->format("ui.timebar.pause");
    impl_->handle.DirtyVariable("pause_text");
}

void TimeBar::set_date(const std::string& text) {
    if (impl_->date_text != text) {
        impl_->date_text = text;
        impl_->handle.DirtyVariable("date_text");
    }
}

void TimeBar::set_speed(int speed) {
    if (impl_->speed != speed) {
        impl_->speed = speed;
        impl_->handle.DirtyVariable("speed");
    }
}

int TimeBar::speed() const {
    return impl_->speed;
}

const std::string& TimeBar::date() const {
    return impl_->date_text;
}

void TimeBar::set_speed_handler(std::function<void(int)> handler) {
    impl_->on_speed = std::move(handler);
}

Rml::ElementDocument* TimeBar::document() const {
    return impl_->find_document();
}

}  // namespace pat::ui
