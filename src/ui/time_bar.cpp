#include "pat/ui/time_bar.hpp"

#include <RmlUi/Core.h>

#include <system_error>
#include <vector>

namespace pat::ui {

namespace {

constexpr const char* kModelName = "timebar";

struct TimeTexts {
    Rml::String calendar;
    Rml::String paused;
    Rml::String hint;
    Rml::String pause_title;
};

struct SpeedRow {
    int n = 0;
    Rml::String title;
};

}  // namespace

struct TimeBar::Impl {
    engine::UiContext* ui = nullptr;
    const loc::Localizer* loc = nullptr;
    Rml::DataModelHandle handle;
    std::string source_url;
    TimeTexts t;
    Rml::String icons;
    Rml::String date_text;
    std::vector<SpeedRow> speeds;
    int speed = 0;
    int shown = 1;  // the level the segments light up: the running speed, or the last one while paused
    bool paused = true;
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

    void refresh_texts() {
        t.calendar = loc->format("ui.hud.calendar");
        t.paused = loc->format("ui.timebar.paused");
        t.hint = loc->format(paused ? "ui.timebar.hint_resume" : "ui.timebar.hint_pause");
        t.pause_title = loc->format("ui.timebar.pause");
        for (SpeedRow& row : speeds) {
            row.title = loc->format("ui.timebar.speed", {loc::Arg("n", row.n)});
        }
        handle.DirtyVariable("t");
        handle.DirtyVariable("speeds");
    }

    void press(int requested) {
        if (on_speed) {
            on_speed(requested);
        }
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

core::Result<std::unique_ptr<TimeBar>> TimeBar::create(engine::UiContext& ui,
                                                       const std::filesystem::path& document,
                                                       const std::filesystem::path& icons,
                                                       const loc::Localizer& loc) {
    using R = core::Result<std::unique_ptr<TimeBar>>;
    auto impl = std::make_unique<Impl>();
    impl->ui = &ui;
    impl->loc = &loc;

    std::error_code ec;
    const std::filesystem::path rel = std::filesystem::relative(
        std::filesystem::absolute(icons), std::filesystem::absolute(document).parent_path(), ec);
    impl->icons = ec ? icons.generic_string() : rel.generic_string();
    for (int n = 1; n <= 5; ++n) {
        impl->speeds.push_back({n, {}});
    }

    Rml::DataModelConstructor ctor = ui.context()->CreateDataModel(kModelName);
    if (!ctor) {
        return R::err(core::Error{92, "cannot create the time bar data model"});
    }
    if (auto texts = ctor.RegisterStruct<TimeTexts>()) {
        texts.RegisterMember("calendar", &TimeTexts::calendar);
        texts.RegisterMember("paused", &TimeTexts::paused);
        texts.RegisterMember("hint", &TimeTexts::hint);
        texts.RegisterMember("pause_title", &TimeTexts::pause_title);
    }
    if (auto row = ctor.RegisterStruct<SpeedRow>()) {
        row.RegisterMember("n", &SpeedRow::n);
        row.RegisterMember("title", &SpeedRow::title);
    }
    ctor.RegisterArray<std::vector<SpeedRow>>();
    Impl* m = impl.get();
    ctor.Bind("t", &m->t);
    ctor.Bind("icons", &m->icons);
    ctor.Bind("date_text", &m->date_text);
    ctor.Bind("speeds", &m->speeds);
    ctor.Bind("speed", &m->speed);
    ctor.Bind("shown", &m->shown);
    ctor.Bind("paused", &m->paused);
    m->handle = ctor.GetModelHandle();
    ctor.BindEventCallback("set_speed", [m](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) {
        if (!args.empty()) {
            m->press(args[0].Get<int>());
        }
    });
    ctor.BindEventCallback("toggle_pause", [m](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
        m->press(m->paused ? m->shown : 0);
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
    impl_->refresh_texts();
}

void TimeBar::set_date(const std::string& text) {
    if (impl_->date_text != text) {
        impl_->date_text = text;
        impl_->handle.DirtyVariable("date_text");
    }
}

void TimeBar::set_speed(int speed) {
    if (speed < 0 || speed > 5) {
        return;
    }
    if (impl_->speed != speed) {
        impl_->speed = speed;
        impl_->handle.DirtyVariable("speed");
    }
    const bool paused = speed == 0;
    if (!paused && impl_->shown != speed) {
        impl_->shown = speed;
        impl_->handle.DirtyVariable("shown");
    }
    if (impl_->paused != paused) {
        impl_->paused = paused;
        impl_->handle.DirtyVariable("paused");
        impl_->refresh_texts();  // the hint changes with the state
    }
}

int TimeBar::speed() const {
    return impl_->speed;
}

int TimeBar::resume_speed() const {
    return impl_->shown;
}

const std::string& TimeBar::date() const {
    return impl_->date_text;
}

void TimeBar::set_speed_handler(std::function<void(int)> handler) {
    impl_->on_speed = std::move(handler);
}

void TimeBar::press_speed(int speed) {
    if (speed >= 0 && speed <= 5) {
        impl_->press(speed);
    }
}

void TimeBar::toggle_pause() {
    impl_->press(impl_->paused ? impl_->shown : 0);
}

Rml::ElementDocument* TimeBar::document() const {
    return impl_->find_document();
}

}  // namespace pat::ui
