#include "pat/ui/character_window.hpp"

#include "pat/core/log.hpp"

#include <RmlUi/Core.h>

#include <algorithm>
#include <cmath>

namespace pat::ui {

namespace {

constexpr const char* kModelName = "character";
constexpr int kBufferRows = 4;  // extra rows above and below the visible ones

struct SkillRow {
    Rml::String name;
    int value = 0;
    int max = kSkillMax;
    Rml::String width;  // CSS width of the bar fill, e.g. "65%"
};

struct RelationRow {
    int index = 0;
    Rml::String text;
};

/// UTF-8 aware first letter of a string (the portrait placeholder shows initials).
std::string first_letter(const std::string& s) {
    if (s.empty()) {
        return {};
    }
    std::size_t n = 1;
    const auto lead = static_cast<unsigned char>(s[0]);
    if (lead >= 0xF0) {
        n = 4;
    } else if (lead >= 0xE0) {
        n = 3;
    } else if (lead >= 0xC0) {
        n = 2;
    }
    return s.substr(0, std::min(n, s.size()));
}

std::string dp_string(float dp) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.0fdp", static_cast<double>(dp));
    return buf;
}

}  // namespace

struct CharacterWindow::Impl {
    engine::UiContext* ui = nullptr;
    Rml::DataModelHandle handle;
    std::string source_url;  // identifies the document even after a hot reload replaced it
    CharacterSnapshot snapshot;
    std::function<void(const std::string&)> on_command;

    // ---- bound variables (names are used by the document)
    Rml::String full_name;
    Rml::String initials;
    Rml::String role;
    Rml::String health;
    int age = 0;
    std::vector<SkillRow> skills;
    std::vector<Rml::String> traits;
    std::vector<Rml::String> family;
    Rml::String tab = "overview";
    Rml::String last_command;
    int relation_total = 0;
    std::vector<RelationRow> relation_rows;  // only the rows around the scroll position
    Rml::String pad_top = "0dp";
    Rml::String pad_bottom = "0dp";
    std::size_t first_row = 0;

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

    void refresh_snapshot_variables() {
        full_name = snapshot.full_name();
        initials = first_letter(snapshot.given_name) + first_letter(snapshot.surname);
        role = snapshot.role;
        health = snapshot.health;
        age = snapshot.age;
        skills.clear();
        for (std::size_t i = 0; i < kSkillCount; ++i) {
            SkillRow row;
            row.name = kSkillNames[i];
            row.value = snapshot.skills[i];
            row.width = std::to_string(snapshot.skills[i] * 100 / kSkillMax) + "%";
            skills.push_back(std::move(row));
        }
        traits.assign(snapshot.traits.begin(), snapshot.traits.end());
        family.assign(snapshot.family.begin(), snapshot.family.end());
        relation_total = static_cast<int>(snapshot.relations.size());
        rebuild_rows(first_row, count_wanted(), /*force=*/true);
        for (const char* name : {"full_name",
                                 "initials",
                                 "role",
                                 "health",
                                 "age",
                                 "skills",
                                 "traits",
                                 "family",
                                 "relation_total"}) {
            handle.DirtyVariable(name);
        }
    }

    [[nodiscard]] std::size_t count_wanted() const {
        Rml::ElementDocument* doc = find_document();
        Rml::Element* list = doc != nullptr ? doc->GetElementById("rel_list") : nullptr;
        const float dp = ui->context()->GetDensityIndependentPixelRatio();
        const float view = list != nullptr ? list->GetClientHeight() : 0.0f;
        const auto visible = static_cast<int>(std::ceil(view / (kRowHeightDp * dp)));
        return static_cast<std::size_t>(std::max(visible, 1) + 2 * kBufferRows);
    }

    void rebuild_rows(std::size_t first, std::size_t count, bool force) {
        const std::size_t total = snapshot.relations.size();
        first = std::min(first, total == 0 ? 0 : total - 1);
        count = std::min(count, total - first);
        if (!force && first == first_row && count == relation_rows.size()) {
            return;
        }
        first_row = first;
        relation_rows.clear();
        relation_rows.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            relation_rows.push_back({static_cast<int>(first + i), snapshot.relations[first + i]});
        }
        pad_top = dp_string(static_cast<float>(first) * kRowHeightDp);
        pad_bottom = dp_string(static_cast<float>(total - first - count) * kRowHeightDp);
        handle.DirtyVariable("relation_rows");
        handle.DirtyVariable("pad_top");
        handle.DirtyVariable("pad_bottom");
    }
};

CharacterWindow::CharacterWindow(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

CharacterWindow::~CharacterWindow() {
    if (impl_ != nullptr && impl_->ui != nullptr) {
        Rml::Context* ctx = impl_->ui->context();
        if (Rml::ElementDocument* doc = impl_->find_document()) {
            doc->Close();
        }
        ctx->Update();  // lets the context actually drop the closed document before the model goes
        ctx->RemoveDataModel(kModelName);
    }
}

core::Result<std::unique_ptr<CharacterWindow>>
CharacterWindow::create(engine::UiContext& ui, const std::filesystem::path& document) {
    using R = core::Result<std::unique_ptr<CharacterWindow>>;
    auto impl = std::make_unique<Impl>();
    impl->ui = &ui;

    Rml::DataModelConstructor ctor = ui.context()->CreateDataModel(kModelName);
    if (!ctor) {
        return R::err(core::Error{90, "cannot create the character data model"});
    }
    if (auto skill = ctor.RegisterStruct<SkillRow>()) {
        skill.RegisterMember("name", &SkillRow::name);
        skill.RegisterMember("value", &SkillRow::value);
        skill.RegisterMember("max", &SkillRow::max);
        skill.RegisterMember("width", &SkillRow::width);
    }
    ctor.RegisterArray<std::vector<SkillRow>>();
    if (auto relation = ctor.RegisterStruct<RelationRow>()) {
        relation.RegisterMember("index", &RelationRow::index);
        relation.RegisterMember("text", &RelationRow::text);
    }
    ctor.RegisterArray<std::vector<RelationRow>>();
    ctor.RegisterArray<std::vector<Rml::String>>();

    Impl* m = impl.get();
    ctor.Bind("full_name", &m->full_name);
    ctor.Bind("initials", &m->initials);
    ctor.Bind("role", &m->role);
    ctor.Bind("health", &m->health);
    ctor.Bind("age", &m->age);
    ctor.Bind("skills", &m->skills);
    ctor.Bind("traits", &m->traits);
    ctor.Bind("family", &m->family);
    ctor.Bind("tab", &m->tab);
    ctor.Bind("last_command", &m->last_command);
    ctor.Bind("relation_total", &m->relation_total);
    ctor.Bind("relation_rows", &m->relation_rows);
    ctor.Bind("pad_top", &m->pad_top);
    ctor.Bind("pad_bottom", &m->pad_bottom);
    m->handle = ctor.GetModelHandle();

    ctor.BindEventCallback("set_tab", [m](Rml::DataModelHandle h, Rml::Event&, const Rml::VariantList& args) {
        if (!args.empty()) {
            m->tab = args[0].Get<Rml::String>();
            h.DirtyVariable("tab");
        }
    });
    ctor.BindEventCallback("command", [m](Rml::DataModelHandle h, Rml::Event&, const Rml::VariantList& args) {
        if (args.empty()) {
            return;
        }
        const Rml::String id = args[0].Get<Rml::String>();
        m->last_command = "Команда: " + id + " (заглушка)";
        h.DirtyVariable("last_command");
        if (m->on_command) {
            m->on_command(id);
        }
    });

    Rml::ElementDocument* loaded = ui.load_document(document);
    if (loaded == nullptr) {
        ui.context()->RemoveDataModel(kModelName);
        m->ui = nullptr;
        return R::err(core::Error{91, "cannot load the character document " + document.generic_string()});
    }
    m->source_url = loaded->GetSourceURL();
    return R::ok(std::unique_ptr<CharacterWindow>(new CharacterWindow(std::move(impl))));
}

void CharacterWindow::set_snapshot(const CharacterSnapshot& snapshot) {
    impl_->snapshot = snapshot;
    impl_->refresh_snapshot_variables();
}

const CharacterSnapshot& CharacterWindow::snapshot() const {
    return impl_->snapshot;
}

void CharacterWindow::set_tab(std::string_view tab) {
    impl_->tab = std::string(tab);
    impl_->handle.DirtyVariable("tab");
}

std::string_view CharacterWindow::tab() const {
    return impl_->tab;
}

void CharacterWindow::set_command_handler(std::function<void(const std::string&)> handler) {
    impl_->on_command = std::move(handler);
}

void CharacterWindow::update() {
    Impl& m = *impl_;
    Rml::ElementDocument* doc = m.find_document();
    if (doc == nullptr || m.tab != "relations") {
        return;
    }
    Rml::Element* list = doc->GetElementById("rel_list");
    if (list == nullptr) {
        return;
    }
    const float dp = m.ui->context()->GetDensityIndependentPixelRatio();
    const float row_px = kRowHeightDp * dp;
    const auto top_row = static_cast<std::size_t>(std::max(0.0f, std::floor(list->GetScrollTop() / row_px)));
    const std::size_t first = top_row > static_cast<std::size_t>(kBufferRows) ? top_row - kBufferRows : 0;
    m.rebuild_rows(first, m.count_wanted(), /*force=*/false);
}

std::size_t CharacterWindow::rows_in_document() const {
    return impl_->relation_rows.size();
}

std::size_t CharacterWindow::first_row_in_document() const {
    return impl_->first_row;
}

Rml::ElementDocument* CharacterWindow::document() const {
    return impl_->find_document();
}

void CharacterWindow::show(bool visible) {
    Rml::ElementDocument* doc = impl_->find_document();
    if (doc == nullptr) {
        return;
    }
    if (visible) {
        doc->Show();
    } else {
        doc->Hide();
    }
}

}  // namespace pat::ui
