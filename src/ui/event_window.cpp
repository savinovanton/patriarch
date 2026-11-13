#include "pat/ui/event_window.hpp"

#include "pat/core/log.hpp"

#include <RmlUi/Core.h>

#include <algorithm>
#include <system_error>
#include <utility>

namespace pat::ui {

namespace {

constexpr const char* kModelName = "event";

// Layout numbers of board 03 ("Important event / Продолжение рода" at 484,154 of 1920x1080): the first option
// starts 386 dp below the top of the window and the options are 116 dp apart.
constexpr int kOptionTop = 386;
constexpr int kOptionPitch = 116;

using loc::Arg;

/// Captions and texts of the open event (one struct so that the document reads them as `t.name`).
struct EventTexts {
    Rml::String kind;
    Rml::String title;
    Rml::String subtitle;
    Rml::String text;
    Rml::String speaker;
    Rml::String role;
    Rml::String quote;
    Rml::String footer;
    Rml::String choose;
    Rml::String autopause;
};

struct OptionRow {
    int index = 0;
    Rml::String number;  // "1"
    Rml::String title;
    Rml::String detail;
    Rml::String top;  // "386dp"
    Rml::String tip;  // tooltip key ("event:0"), empty without consequences
    bool enabled = true;
    bool reason = false;  // the line below the title is the reason the option is unavailable
};

[[nodiscard]] bool has_content(const Explanation& e) {
    return !e.label.empty() || !e.terms.empty() || !e.lines.empty() || !e.headline.empty();
}

}  // namespace

EventSnapshot make_placeholder_event(const loc::Localizer& loc) {
    const auto text = [&](const char* key) { return loc.format(std::string("ui.event.placeholder.") + key); };
    EventSnapshot snapshot;
    snapshot.placeholder = true;
    EventView& ev = snapshot.event;
    ev.id = "placeholder.heir_of_age";
    ev.kind = "family";
    ev.category = "house";
    ev.title = text("title");
    ev.subtitle = text("subtitle");
    ev.text = text("text");
    ev.portrait = "portrait_head_of_dynasty.jpg";
    ev.speaker = text("speaker");
    ev.speaker_role = text("speaker_role");
    ev.quote = text("quote");
    ev.date = text("date");
    ev.autopause = true;
    ev.default_option = 0;

    const char* const ids[] = {"negotiate", "learn", "postpone"};
    for (const char* id : ids) {
        EventOptionView option;
        const std::string prefix = std::string(id) + ".";
        option.title = text((prefix + "title").c_str());
        option.detail = text((prefix + "detail").c_str());
        // The consequences of the option, shown before the choice (the principle of the design). Numbers are
        // those of the mockup, not balance.
        Explanation& tip = option.consequences;
        tip.label = text((prefix + "tip.title").c_str());
        tip.note = text((prefix + "tip.note").c_str());
        tip.footer = loc.format("ui.event.consequences_first");
        for (int i = 1; i <= 2; ++i) {
            const std::string term = "ui.event.placeholder." + prefix + "tip.term" + std::to_string(i);
            if (loc.resolves(term + ".label")) {
                tip.terms.push_back(
                    Explanation(loc.format(term + ".label"), loc.format(term + ".value"), std::string(), {}));
            }
        }
        ev.options.push_back(std::move(option));
    }
    return snapshot;
}

struct EventWindow::Impl {
    engine::UiContext* ui = nullptr;
    const loc::Localizer* loc = nullptr;
    Rml::DataModelHandle handle;
    std::string source_url;
    std::string icons_rel;
    std::string art_rel;
    EventSnapshot snapshot;
    std::function<void(const EventView&)> on_shown;
    std::function<void(std::size_t)> on_choice;
    bool visible = false;

    // ---- bound variables (names are used by the document)
    EventTexts t;
    std::vector<OptionRow> rows;
    Rml::String shield;
    Rml::String portrait = "none";  // decorator of the portrait: image(file cover)
    bool has_portrait = false;
    bool has_quote = false;
    bool autopause = false;
    int sel = 0;

    /// The keys of the window. Heard in the capture phase on the document so that nothing under the focus
    /// swallows them.
    struct KeyListener : Rml::EventListener {
        Impl* owner = nullptr;
        void ProcessEvent(Rml::Event& ev) override { owner->handle_key(ev); }
    } key_listener;

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

    [[nodiscard]] bool can_pick(std::size_t index) const {
        return index < rows.size() && rows[index].enabled;
    }

    void refresh_chrome() {
        t.choose = loc->format("ui.event.choose");
        t.autopause = loc->format("ui.event.autopause");
        shield = icons_rel + "/shield@green.svg";
        handle.DirtyVariable("t");
        handle.DirtyVariable("shield");
    }

    void refresh_event(bool keep_selection) {
        const EventView& ev = snapshot.event;
        const int previous = sel;
        std::string kind_key = "ui.event.kind." + ev.kind;
        if (ev.kind.empty() || !loc->resolves(kind_key)) {
            kind_key = "ui.event.kind.other";
        }
        t.kind = loc->format(kind_key);
        t.title = ev.title;
        t.subtitle = ev.subtitle;
        t.text = ev.text;
        t.speaker = ev.speaker;
        t.role = ev.speaker_role;
        t.quote = ev.quote;
        if (ev.category.empty()) {
            t.footer = ev.date;
        } else {
            t.footer = loc->format(
                "ui.event.footer",
                {Arg("date", ev.date), Arg("category", loc->format("ui.event.category." + ev.category))});
        }
        has_portrait = !ev.portrait.empty();
        portrait = has_portrait ? "image(" + art_rel + "/" + ev.portrait + " cover)" : Rml::String("none");
        has_quote = !ev.quote.empty();
        autopause = ev.autopause;

        rows.clear();
        if (ev.options.size() > EventWindow::kMaxOptions) {
            PAT_LOG_WARN("event",
                         "event '{}' has {} options, the window shows {}",
                         ev.id,
                         ev.options.size(),
                         EventWindow::kMaxOptions);
        }
        for (const EventOptionView& option : ev.options) {
            if (rows.size() >= EventWindow::kMaxOptions) {
                break;
            }
            OptionRow row;
            row.index = static_cast<int>(rows.size());
            row.number = std::to_string(rows.size() + 1);
            row.title = option.title;
            row.detail = option.available || option.unavailable_reason.empty() ? option.detail
                                                                               : option.unavailable_reason;
            row.reason = !option.available && !option.unavailable_reason.empty();
            row.top = std::to_string(kOptionTop + row.index * kOptionPitch) + "dp";
            row.tip =
                has_content(option.consequences) ? "event:" + std::to_string(rows.size()) : std::string();
            row.enabled = option.available;
            rows.push_back(std::move(row));
        }
        sel = 0;
        if (keep_selection && can_pick(static_cast<std::size_t>(std::max(0, previous)))) {
            sel = previous;
        } else if (can_pick(ev.default_option)) {
            sel = static_cast<int>(ev.default_option);
        } else {
            for (std::size_t i = 0; i < rows.size(); ++i) {
                if (rows[i].enabled) {
                    sel = static_cast<int>(i);
                    break;
                }
            }
        }
        handle.DirtyVariable("t");
        handle.DirtyVariable("rows");
        handle.DirtyVariable("portrait");
        handle.DirtyVariable("has_portrait");
        handle.DirtyVariable("has_quote");
        handle.DirtyVariable("autopause");
        handle.DirtyVariable("sel");
    }

    void focus_option(std::size_t index) {
        Rml::ElementDocument* doc = find_document();
        if (doc == nullptr) {
            return;
        }
        if (Rml::Element* e = doc->GetElementById("event-opt-" + std::to_string(index))) {
            e->Focus();
        }
        // The ring of the focus shows after the keyboard was used; the digits are not among the keys the
        // focus visibility listens to.
        if (Rml::Element* root = ui->context()->GetRootElement()) {
            root->SetClass("kbd", true);
        }
    }

    void select(std::size_t index, bool focus) {
        if (!can_pick(index)) {
            return;
        }
        if (sel != static_cast<int>(index)) {
            sel = static_cast<int>(index);
            handle.DirtyVariable("sel");
        }
        if (focus) {
            focus_option(index);
        }
    }

    /// The next available option from the highlighted one in the direction `step` (no wrap-around).
    void step(int direction) {
        for (int i = sel + direction; i >= 0 && i < static_cast<int>(rows.size()); i += direction) {
            if (rows[static_cast<std::size_t>(i)].enabled) {
                select(static_cast<std::size_t>(i), true);
                return;
            }
        }
        select(static_cast<std::size_t>(sel), true);
    }

    bool choose(std::size_t index) {
        if (!visible || !can_pick(index)) {
            return false;
        }
        visible = false;
        if (Rml::ElementDocument* doc = find_document()) {
            doc->Hide();
        }
        if (on_choice) {
            on_choice(index);
        }
        return true;
    }

    void handle_key(Rml::Event& ev) {
        if (!visible) {
            return;
        }
        const auto key = static_cast<Rml::Input::KeyIdentifier>(
            ev.GetParameter<int>("key_identifier", static_cast<int>(Rml::Input::KI_UNKNOWN)));
        std::size_t digit = rows.size();
        if (key >= Rml::Input::KI_1 && key <= Rml::Input::KI_9) {
            digit = static_cast<std::size_t>(key - Rml::Input::KI_1);
        } else if (key >= Rml::Input::KI_NUMPAD1 && key <= Rml::Input::KI_NUMPAD9) {
            digit = static_cast<std::size_t>(key - Rml::Input::KI_NUMPAD1);
        }
        if (key == Rml::Input::KI_ESCAPE) {
            ev.StopPropagation();  // nothing is decided by Esc, and the game must not treat it as "quit"
                                   // either
        } else if (digit < rows.size()) {
            select(digit, true);
            ev.StopPropagation();
        } else if (key == Rml::Input::KI_UP || key == Rml::Input::KI_LEFT) {
            step(-1);
            ev.StopPropagation();
        } else if (key == Rml::Input::KI_DOWN || key == Rml::Input::KI_RIGHT) {
            step(1);
            ev.StopPropagation();
        }
    }
};

EventWindow::EventWindow(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

EventWindow::~EventWindow() {
    if (impl_ != nullptr && impl_->ui != nullptr) {
        Rml::Context* ctx = impl_->ui->context();
        if (Rml::ElementDocument* doc = impl_->find_document()) {
            doc->RemoveEventListener(Rml::EventId::Keydown, &impl_->key_listener, true);
            doc->Close();
        }
        ctx->Update();  // lets the context actually drop the closed document before the model goes
        ctx->RemoveDataModel(kModelName);
    }
}

core::Result<std::unique_ptr<EventWindow>> EventWindow::create(engine::UiContext& ui,
                                                               const std::filesystem::path& document,
                                                               const std::filesystem::path& icons,
                                                               const loc::Localizer& loc) {
    using R = core::Result<std::unique_ptr<EventWindow>>;
    auto impl = std::make_unique<Impl>();
    impl->ui = &ui;
    impl->loc = &loc;

    const auto relative = [&](const std::filesystem::path& target) {
        std::error_code ec;
        const std::filesystem::path rel = std::filesystem::relative(
            std::filesystem::absolute(target), std::filesystem::absolute(document).parent_path(), ec);
        return ec ? target.generic_string() : rel.generic_string();
    };
    impl->icons_rel = relative(icons);
    impl->art_rel = relative(icons.parent_path() / "art");

    Rml::DataModelConstructor ctor = ui.context()->CreateDataModel(kModelName);
    if (!ctor) {
        return R::err(core::Error{93, "cannot create the event data model"});
    }
    if (auto texts = ctor.RegisterStruct<EventTexts>()) {
        texts.RegisterMember("kind", &EventTexts::kind);
        texts.RegisterMember("title", &EventTexts::title);
        texts.RegisterMember("subtitle", &EventTexts::subtitle);
        texts.RegisterMember("text", &EventTexts::text);
        texts.RegisterMember("speaker", &EventTexts::speaker);
        texts.RegisterMember("role", &EventTexts::role);
        texts.RegisterMember("quote", &EventTexts::quote);
        texts.RegisterMember("footer", &EventTexts::footer);
        texts.RegisterMember("choose", &EventTexts::choose);
        texts.RegisterMember("autopause", &EventTexts::autopause);
    }
    if (auto row = ctor.RegisterStruct<OptionRow>()) {
        row.RegisterMember("index", &OptionRow::index);
        row.RegisterMember("number", &OptionRow::number);
        row.RegisterMember("title", &OptionRow::title);
        row.RegisterMember("detail", &OptionRow::detail);
        row.RegisterMember("top", &OptionRow::top);
        row.RegisterMember("tip", &OptionRow::tip);
        row.RegisterMember("enabled", &OptionRow::enabled);
        row.RegisterMember("reason", &OptionRow::reason);
    }
    ctor.RegisterArray<std::vector<OptionRow>>();

    Impl* m = impl.get();
    ctor.Bind("t", &m->t);
    ctor.Bind("rows", &m->rows);
    ctor.Bind("shield", &m->shield);
    ctor.Bind("portrait", &m->portrait);
    ctor.Bind("has_portrait", &m->has_portrait);
    ctor.Bind("has_quote", &m->has_quote);
    ctor.Bind("autopause", &m->autopause);
    ctor.Bind("sel", &m->sel);
    m->handle = ctor.GetModelHandle();

    // A mouseover or the focus (Tab, a digit) highlights the option; a click chooses it. The focus event of a
    // click comes first and changes nothing: the highlight is already there.
    ctor.BindEventCallback("hover", [m](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) {
        if (!args.empty() && m->visible) {
            m->select(static_cast<std::size_t>(std::max(0, args[0].Get<int>())), false);
        }
    });
    ctor.BindEventCallback("choose", [m](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) {
        if (!args.empty()) {
            m->choose(static_cast<std::size_t>(std::max(0, args[0].Get<int>())));
        }
    });
    m->refresh_chrome();

    Rml::ElementDocument* loaded = ui.load_document(document);
    if (loaded == nullptr) {
        ui.context()->RemoveDataModel(kModelName);
        m->ui = nullptr;
        return R::err(core::Error{92, "cannot load the event document " + document.generic_string()});
    }
    m->source_url = loaded->GetSourceURL();
    m->key_listener.owner = m;
    loaded->AddEventListener(Rml::EventId::Keydown, &m->key_listener, /*in_capture_phase=*/true);
    loaded->Hide();  // shown by show_event()
    return R::ok(std::unique_ptr<EventWindow>(new EventWindow(std::move(impl))));
}

void EventWindow::show_event(const EventSnapshot& snapshot) {
    Impl& m = *impl_;
    m.snapshot = snapshot;
    m.refresh_event(false);
    m.visible = true;
    if (Rml::ElementDocument* doc = m.find_document()) {
        // Modal: the mouse and Tab stay in this document. The focus starts on the document itself, so that
        // Enter does not choose anything before the player has pointed at an option.
        doc->Show(Rml::ModalFlag::Modal, Rml::FocusFlag::Document);
    }
    if (m.on_shown) {
        m.on_shown(m.snapshot.event);
    }
}

void EventWindow::close() {
    impl_->visible = false;
    if (Rml::ElementDocument* doc = impl_->find_document()) {
        doc->Hide();
    }
}

bool EventWindow::visible() const {
    return impl_->visible;
}

const EventSnapshot& EventWindow::snapshot() const {
    return impl_->snapshot;
}

void EventWindow::set_event_shown_handler(std::function<void(const EventView&)> handler) {
    impl_->on_shown = std::move(handler);
}

void EventWindow::set_choice_handler(std::function<void(std::size_t)> handler) {
    impl_->on_choice = std::move(handler);
}

void EventWindow::select(std::size_t index, bool focus) {
    impl_->select(index, focus);
}

std::size_t EventWindow::selected() const {
    return static_cast<std::size_t>(impl_->sel);
}

bool EventWindow::choose(std::size_t index) {
    return impl_->choose(index);
}

const Explanation* EventWindow::explanation(std::string_view key) const {
    constexpr std::string_view kPrefix = "event:";
    if (key.substr(0, kPrefix.size()) != kPrefix) {
        return nullptr;
    }
    std::size_t index = 0;
    const std::string_view digits = key.substr(kPrefix.size());
    if (digits.empty() || digits.size() > 3) {
        return nullptr;
    }
    for (const char c : digits) {
        if (c < '0' || c > '9') {
            return nullptr;
        }
        index = index * 10 + static_cast<std::size_t>(c - '0');
    }
    const auto& options = impl_->snapshot.event.options;
    if (index >= options.size() || index >= EventWindow::kMaxOptions ||
        !has_content(options[index].consequences)) {
        return nullptr;
    }
    return &options[index].consequences;
}

void EventWindow::relocalize() {
    impl_->refresh_chrome();
    impl_->refresh_event(true);
}

Rml::ElementDocument* EventWindow::document() const {
    return impl_->find_document();
}

}  // namespace pat::ui
