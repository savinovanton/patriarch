#include "pat/ui/agenda_panel.hpp"

#include "pat/ui/text.hpp"

#include <RmlUi/Core.h>

#include <algorithm>
#include <system_error>

namespace pat::ui {

namespace {

constexpr const char* kModelName = "agenda";
constexpr std::size_t kMaxCards = 3;  // the panel of the mockup holds three cards

using loc::Arg;

/// Icon of a section chip (assets/ui/patriarch/icons): the icons of the chips on board 01 (dynasty, scroll
/// for the court, coin), the navigation icons for the rest.
const char* section_icon(std::string_view section) {
    if (section == "dynasty") {
        return "dynasty";
    }
    if (section == "court" || section == "chronicle") {
        return "scroll";
    }
    if (section == "treasury") {
        return "coin";
    }
    if (section == "holdings") {
        return "castle";
    }
    if (section == "church") {
        return "church";
    }
    if (section == "army") {
        return "sword";
    }
    if (section == "diplomacy") {
        return "globe";
    }
    return "quill";
}

struct AgendaTexts {
    Rml::String title;
    Rml::String subtitle;
    Rml::String journal;
    Rml::String empty;
};

struct AgendaRow {
    int index = 0;
    Rml::String number;  // "01"
    Rml::String title;
    Rml::String detail;
    Rml::String section;
    Rml::String section_name;
    Rml::String icon;
    bool attention = false;
};

}  // namespace

AgendaSnapshot make_placeholder_agenda() {
    AgendaSnapshot snapshot;
    snapshot.placeholder = true;
    snapshot.items = {
        {"ui.agenda.placeholder.heir.title", "ui.agenda.placeholder.heir.detail", 0, "dynasty", true},
        {"ui.agenda.placeholder.letter.title", "ui.agenda.placeholder.letter.detail", 0, "court", true},
        {"ui.agenda.placeholder.taxes.title", "ui.agenda.placeholder.taxes.detail", 12, "treasury", false},
    };
    return snapshot;
}

struct AgendaPanel::Impl {
    engine::UiContext* ui = nullptr;
    const loc::Localizer* loc = nullptr;
    Rml::DataModelHandle handle;
    std::string source_url;
    std::string icons_rel;
    AgendaSnapshot snapshot;
    std::function<void(const std::string&)> on_open;
    std::function<void()> on_journal;

    AgendaTexts t;
    Rml::String bell;
    Rml::String count_text;
    int count = 0;
    int shown = 0;
    std::vector<AgendaRow> rows;

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

    void refresh() {
        t.title = text("ui.agenda.title");
        t.subtitle = text("ui.agenda.subtitle");
        t.journal = text("ui.agenda.journal");
        t.empty = text("ui.agenda.empty");
        handle.DirtyVariable("t");
        bell = icons_rel + "/bell@gold.svg";
        handle.DirtyVariable("bell");

        rows.clear();
        count = 0;
        for (const AgendaItem& item : snapshot.items) {
            if (item.attention) {
                ++count;
            }
            if (rows.size() >= kMaxCards) {
                continue;
            }
            AgendaRow row;
            row.index = static_cast<int>(rows.size());
            row.number = rows.size() + 1 < 10 ? "0" + std::to_string(rows.size() + 1)
                                              : std::to_string(rows.size() + 1);
            row.title = text(item.title_key);
            row.detail = text(item.detail_key, {Arg("n", item.count)});
            row.section = item.section;
            row.section_name = text("ui.nav." + item.section);
            row.icon = icons_rel + "/" + section_icon(item.section) + "@gold.svg";
            row.attention = item.attention;
            rows.push_back(std::move(row));
        }
        shown = static_cast<int>(rows.size());
        count_text = format_number(*loc, count);
        handle.DirtyVariable("rows");
        handle.DirtyVariable("shown");
        handle.DirtyVariable("count");
        handle.DirtyVariable("count_text");
    }
};

AgendaPanel::AgendaPanel(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

AgendaPanel::~AgendaPanel() {
    if (impl_ != nullptr && impl_->ui != nullptr) {
        Rml::Context* ctx = impl_->ui->context();
        if (Rml::ElementDocument* doc = impl_->find_document()) {
            doc->Close();
        }
        ctx->Update();
        ctx->RemoveDataModel(kModelName);
    }
}

core::Result<std::unique_ptr<AgendaPanel>> AgendaPanel::create(engine::UiContext& ui,
                                                               const std::filesystem::path& document,
                                                               const std::filesystem::path& icons,
                                                               const loc::Localizer& loc) {
    using R = core::Result<std::unique_ptr<AgendaPanel>>;
    auto impl = std::make_unique<Impl>();
    impl->ui = &ui;
    impl->loc = &loc;

    std::error_code ec;
    const std::filesystem::path rel = std::filesystem::relative(
        std::filesystem::absolute(icons), std::filesystem::absolute(document).parent_path(), ec);
    impl->icons_rel = ec ? icons.generic_string() : rel.generic_string();

    Rml::DataModelConstructor ctor = ui.context()->CreateDataModel(kModelName);
    if (!ctor) {
        return R::err(core::Error{98, "cannot create the agenda data model"});
    }
    if (auto texts = ctor.RegisterStruct<AgendaTexts>()) {
        texts.RegisterMember("title", &AgendaTexts::title);
        texts.RegisterMember("subtitle", &AgendaTexts::subtitle);
        texts.RegisterMember("journal", &AgendaTexts::journal);
        texts.RegisterMember("empty", &AgendaTexts::empty);
    }
    if (auto item = ctor.RegisterStruct<AgendaRow>()) {
        item.RegisterMember("index", &AgendaRow::index);
        item.RegisterMember("number", &AgendaRow::number);
        item.RegisterMember("title", &AgendaRow::title);
        item.RegisterMember("detail", &AgendaRow::detail);
        item.RegisterMember("section", &AgendaRow::section);
        item.RegisterMember("section_name", &AgendaRow::section_name);
        item.RegisterMember("icon", &AgendaRow::icon);
        item.RegisterMember("attention", &AgendaRow::attention);
    }
    ctor.RegisterArray<std::vector<AgendaRow>>();

    Impl* m = impl.get();
    ctor.Bind("t", &m->t);
    ctor.Bind("bell", &m->bell);
    ctor.Bind("rows", &m->rows);
    ctor.Bind("shown", &m->shown);
    ctor.Bind("count", &m->count);
    ctor.Bind("count_text", &m->count_text);
    m->handle = ctor.GetModelHandle();
    ctor.BindEventCallback("open", [m](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) {
        if (!args.empty() && m->on_open) {
            const int index = args[0].Get<int>();
            if (index >= 0 && static_cast<std::size_t>(index) < m->rows.size()) {
                m->on_open(m->rows[static_cast<std::size_t>(index)].section);
            }
        }
    });
    ctor.BindEventCallback("journal", [m](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
        if (m->on_journal) {
            m->on_journal();
        }
    });
    m->refresh();

    Rml::ElementDocument* loaded = ui.load_document(document);
    if (loaded == nullptr) {
        ui.context()->RemoveDataModel(kModelName);
        m->ui = nullptr;
        return R::err(core::Error{99, "cannot load the agenda panel " + document.generic_string()});
    }
    m->source_url = loaded->GetSourceURL();
    return R::ok(std::unique_ptr<AgendaPanel>(new AgendaPanel(std::move(impl))));
}

void AgendaPanel::set_snapshot(const AgendaSnapshot& snapshot) {
    impl_->snapshot = snapshot;
    impl_->refresh();
}

const AgendaSnapshot& AgendaPanel::snapshot() const {
    return impl_->snapshot;
}

void AgendaPanel::relocalize() {
    impl_->refresh();
}

void AgendaPanel::set_open_handler(std::function<void(const std::string&)> handler) {
    impl_->on_open = std::move(handler);
}

void AgendaPanel::set_journal_handler(std::function<void()> handler) {
    impl_->on_journal = std::move(handler);
}

void AgendaPanel::press_item(std::size_t index) {
    if (index < impl_->rows.size() && impl_->on_open) {
        impl_->on_open(impl_->rows[index].section);
    }
}

void AgendaPanel::press_journal() {
    if (impl_->on_journal) {
        impl_->on_journal();
    }
}

int AgendaPanel::attention_count() const {
    return impl_->count;
}

Rml::ElementDocument* AgendaPanel::document() const {
    return impl_->find_document();
}

void AgendaPanel::show(bool visible) {
    if (Rml::ElementDocument* doc = impl_->find_document()) {
        if (visible) {
            doc->Show(Rml::ModalFlag::None, Rml::FocusFlag::None);
        } else {
            doc->Hide();
        }
    }
}

}  // namespace pat::ui
