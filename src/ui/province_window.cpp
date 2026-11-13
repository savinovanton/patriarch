#include "pat/ui/province_window.hpp"

#include <RmlUi/Core.h>

#include <algorithm>
#include <charconv>
#include <system_error>
#include <utility>
#include <vector>

namespace pat::ui {

namespace {

constexpr const char* kModelName = "province";

struct ProvinceTexts {
    Rml::String title;
    Rml::String close;
    Rml::String tab_overview;
    Rml::String tab_income;
    Rml::String tab_buildings;
    Rml::String income;
    Rml::String income_detail;
    Rml::String population;
    Rml::String order;
    Rml::String garrison;
    Rml::String owner;
    Rml::String manage;
    Rml::String unavailable;
    Rml::String income_factors;
    Rml::String income_last_month;
    Rml::String groups;
    Rml::String buildings;
    Rml::String buildings_empty;
    Rml::String building_upkeep;
    Rml::String slots;
    Rml::String free_slots;
};

struct DetailRow {
    Rml::String name;
    Rml::String value;
    Rml::String note;
    Rml::String tip;
};

bool valid_tab(std::string_view tab) {
    return tab == "overview" || tab == "income" || tab == "buildings";
}

const Explanation* indexed_explanation(std::string_view digits,
                                       const std::vector<EconomyGroupSnapshot>& groups) {
    std::size_t index = 0;
    const auto [end, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), index);
    return ec == std::errc{} && end == digits.data() + digits.size() && index < groups.size()
               ? &groups[index].population.explanation
               : nullptr;
}

const Explanation* indexed_explanation(std::string_view digits,
                                       const std::vector<EconomyBuildingSnapshot>& buildings) {
    std::size_t index = 0;
    const auto [end, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), index);
    return ec == std::errc{} && end == digits.data() + digits.size() && index < buildings.size()
               ? &buildings[index].upkeep.explanation
               : nullptr;
}

}  // namespace

struct ProvinceWindow::Impl {
    engine::UiContext* ui = nullptr;
    const loc::Localizer* loc = nullptr;
    Rml::DataModelHandle handle;
    std::string source_url;
    ProvinceSnapshot snapshot;
    std::function<void()> on_close;
    std::function<void()> on_manage;
    bool visible = false;

    ProvinceTexts t;
    Rml::String tab = "overview";
    Rml::String name;
    Rml::String subtitle;
    Rml::String income;
    Rml::String income_change;
    Rml::String population;
    Rml::String order;
    Rml::String order_width = "0%";
    Rml::String garrison;
    Rml::String state;
    Rml::String state_detail;
    Rml::String owner_name;
    Rml::String owner_detail;
    Rml::String income_last_month;
    Rml::String slots;
    Rml::String free_slots;
    Rml::String manage_reason;
    Rml::String close_icon;
    Rml::String manage_icon;
    Rml::String owner_portrait = "none";
    std::string portrait_decorator;
    std::vector<DetailRow> factors;
    std::vector<DetailRow> groups;
    std::vector<DetailRow> buildings;
    bool available = false;
    bool manage_enabled = false;
    bool change_positive = false;
    bool change_negative = false;
    bool has_buildings = false;

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

    void apply_visibility() const {
        if (Rml::ElementDocument* doc = find_document()) {
            if (visible) {
                doc->Show(Rml::ModalFlag::None, Rml::FocusFlag::None);
            } else {
                doc->Hide();
            }
        }
    }

    void refresh_chrome() {
        t.title = loc->format("ui.province_window.title");
        t.close = loc->format("ui.province_window.close");
        t.tab_overview = loc->format("ui.province_window.tab.overview");
        t.tab_income = loc->format("ui.province_window.tab.income");
        t.tab_buildings = loc->format("ui.province_window.tab.buildings");
        t.income = loc->format("ui.province_window.income");
        t.income_detail = loc->format("ui.province_window.income_detail");
        t.population = loc->format("ui.province_window.population");
        t.order = loc->format("ui.province_window.order");
        t.garrison = loc->format("ui.province_window.garrison");
        t.owner = loc->format("ui.province_window.owner");
        t.manage = loc->format("ui.province_window.manage");
        t.unavailable = loc->format("ui.province_window.unavailable");
        t.income_factors = loc->format("ui.province_window.income_factors");
        t.income_last_month = loc->format("ui.province_window.income_last_month");
        t.groups = loc->format("ui.province_window.groups");
        t.buildings = loc->format("ui.province_window.buildings");
        t.buildings_empty = loc->format("ui.province_window.buildings_empty");
        t.building_upkeep = loc->format("ui.province_window.building_upkeep");
        t.slots = loc->format("ui.province_window.slots");
        t.free_slots = loc->format("ui.province_window.free_slots");
        handle.DirtyVariable("t");
    }

    void refresh_manage() {
        manage_enabled = available && snapshot.manage_available && static_cast<bool>(on_manage);
        manage_reason.clear();
        if (!manage_enabled) {
            const Explanation& reason = snapshot.manage_explanation;
            manage_reason = !reason.lines.empty() ? reason.lines.front() : reason.note;
            if (manage_reason.empty()) {
                manage_reason = loc->format("ui.province_window.manage_unavailable");
            }
        }
        handle.DirtyVariable("manage_enabled");
        handle.DirtyVariable("manage_reason");
    }

    void refresh_snapshot() {
        const ProvinceEconomySnapshot& economy = snapshot.economy;
        available = economy.available;
        name = economy.name.empty() ? loc->format("ui.province_window.no_selection") : economy.name;
        subtitle = snapshot.subtitle;
        // Economic values arrive already formatted by game, alongside their authoritative tooltips.
        income = available ? economy.projected_income.explanation.value : Rml::String{};
        income_change = available ? snapshot.income_change.explanation.value : Rml::String{};
        population = available ? economy.population.explanation.value : Rml::String{};
        order = available ? snapshot.order : Rml::String{};
        order_width = std::to_string(available ? std::clamp(snapshot.order_percent, 0, 100) : 0) + "%";
        garrison = available ? snapshot.garrison : Rml::String{};
        state = available ? snapshot.state : Rml::String{};
        state_detail = available ? snapshot.state_detail : Rml::String{};
        owner_name = snapshot.owner_name;
        owner_detail = snapshot.owner_detail;
        owner_portrait = snapshot.owner_portrait ? portrait_decorator : "none";
        income_last_month = available ? economy.income_last_month.explanation.value : Rml::String{};
        slots = available ? economy.slots.explanation.value : Rml::String{};
        free_slots = available ? economy.empty_slots.explanation.value : Rml::String{};
        change_positive = snapshot.income_change.value > 0;
        change_negative = snapshot.income_change.value < 0;

        factors.clear();
        groups.clear();
        buildings.clear();
        if (available) {
            for (std::size_t i = 0; i < economy.projected_income.explanation.terms.size(); ++i) {
                const Explanation& term = economy.projected_income.explanation.terms[i];
                factors.push_back(
                    {term.label, term.value, term.note, "province:income/" + std::to_string(i)});
            }
            for (std::size_t i = 0; i < economy.groups.size(); ++i) {
                const EconomyGroupSnapshot& group = economy.groups[i];
                groups.push_back({group.name,
                                  group.population.explanation.value,
                                  "",
                                  "province:group:" + std::to_string(i)});
            }
            for (std::size_t i = 0; i < economy.buildings.size(); ++i) {
                const EconomyBuildingSnapshot& building = economy.buildings[i];
                buildings.push_back({building.name,
                                     building.upkeep.explanation.value,
                                     "",
                                     "province:building:" + std::to_string(i)});
            }
        }
        has_buildings = !buildings.empty();
        refresh_manage();
        for (const char* variable :
             {"available",    "name",       "subtitle",        "income",          "income_change",
              "population",   "order",      "order_width",     "garrison",        "state",
              "state_detail", "owner_name", "owner_detail",    "owner_portrait",  "income_last_month",
              "slots",        "free_slots", "change_positive", "change_negative", "factors",
              "groups",       "buildings",  "has_buildings"}) {
            handle.DirtyVariable(variable);
        }
    }

    void select_tab(std::string_view next) {
        if (valid_tab(next) && tab != next) {
            tab = std::string(next);
            handle.DirtyVariable("tab");
        }
    }
};

ProvinceWindow::ProvinceWindow(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

ProvinceWindow::~ProvinceWindow() {
    if (impl_ != nullptr && impl_->ui != nullptr) {
        Rml::Context* ctx = impl_->ui->context();
        if (Rml::ElementDocument* doc = impl_->find_document()) {
            doc->Close();
        }
        ctx->Update();
        ctx->RemoveDataModel(kModelName);
    }
}

core::Result<std::unique_ptr<ProvinceWindow>> ProvinceWindow::create(engine::UiContext& ui,
                                                                     const std::filesystem::path& document,
                                                                     const std::filesystem::path& icons,
                                                                     const loc::Localizer& loc) {
    using R = core::Result<std::unique_ptr<ProvinceWindow>>;
    auto impl = std::make_unique<Impl>();
    impl->ui = &ui;
    impl->loc = &loc;

    std::error_code ec;
    const auto relative = std::filesystem::relative(
        std::filesystem::absolute(icons), std::filesystem::absolute(document).parent_path(), ec);
    const std::string icons_rel = ec ? icons.generic_string() : relative.generic_string();
    impl->close_icon = icons_rel + "/close@muted.svg";
    impl->manage_icon = icons_rel + "/castle@gold.svg";
    impl->portrait_decorator = "image(" + icons_rel + "/../art/portrait_head_of_dynasty.jpg cover)";

    Rml::DataModelConstructor ctor = ui.context()->CreateDataModel(kModelName);
    if (!ctor) {
        return R::err(core::Error{102, "cannot create the province data model"});
    }
    if (auto texts = ctor.RegisterStruct<ProvinceTexts>()) {
        texts.RegisterMember("title", &ProvinceTexts::title);
        texts.RegisterMember("close", &ProvinceTexts::close);
        texts.RegisterMember("tab_overview", &ProvinceTexts::tab_overview);
        texts.RegisterMember("tab_income", &ProvinceTexts::tab_income);
        texts.RegisterMember("tab_buildings", &ProvinceTexts::tab_buildings);
        texts.RegisterMember("income", &ProvinceTexts::income);
        texts.RegisterMember("income_detail", &ProvinceTexts::income_detail);
        texts.RegisterMember("population", &ProvinceTexts::population);
        texts.RegisterMember("order", &ProvinceTexts::order);
        texts.RegisterMember("garrison", &ProvinceTexts::garrison);
        texts.RegisterMember("owner", &ProvinceTexts::owner);
        texts.RegisterMember("manage", &ProvinceTexts::manage);
        texts.RegisterMember("unavailable", &ProvinceTexts::unavailable);
        texts.RegisterMember("income_factors", &ProvinceTexts::income_factors);
        texts.RegisterMember("income_last_month", &ProvinceTexts::income_last_month);
        texts.RegisterMember("groups", &ProvinceTexts::groups);
        texts.RegisterMember("buildings", &ProvinceTexts::buildings);
        texts.RegisterMember("buildings_empty", &ProvinceTexts::buildings_empty);
        texts.RegisterMember("building_upkeep", &ProvinceTexts::building_upkeep);
        texts.RegisterMember("slots", &ProvinceTexts::slots);
        texts.RegisterMember("free_slots", &ProvinceTexts::free_slots);
    }
    if (auto row = ctor.RegisterStruct<DetailRow>()) {
        row.RegisterMember("name", &DetailRow::name);
        row.RegisterMember("value", &DetailRow::value);
        row.RegisterMember("note", &DetailRow::note);
        row.RegisterMember("tip", &DetailRow::tip);
    }
    ctor.RegisterArray<std::vector<DetailRow>>();

    Impl* m = impl.get();
    ctor.Bind("t", &m->t);
    ctor.Bind("tab", &m->tab);
    ctor.Bind("name", &m->name);
    ctor.Bind("subtitle", &m->subtitle);
    ctor.Bind("income", &m->income);
    ctor.Bind("income_change", &m->income_change);
    ctor.Bind("population", &m->population);
    ctor.Bind("order", &m->order);
    ctor.Bind("order_width", &m->order_width);
    ctor.Bind("garrison", &m->garrison);
    ctor.Bind("state", &m->state);
    ctor.Bind("state_detail", &m->state_detail);
    ctor.Bind("owner_name", &m->owner_name);
    ctor.Bind("owner_detail", &m->owner_detail);
    ctor.Bind("owner_portrait", &m->owner_portrait);
    ctor.Bind("income_last_month", &m->income_last_month);
    ctor.Bind("slots", &m->slots);
    ctor.Bind("free_slots", &m->free_slots);
    ctor.Bind("manage_reason", &m->manage_reason);
    ctor.Bind("close_icon", &m->close_icon);
    ctor.Bind("manage_icon", &m->manage_icon);
    ctor.Bind("factors", &m->factors);
    ctor.Bind("groups", &m->groups);
    ctor.Bind("buildings", &m->buildings);
    ctor.Bind("available", &m->available);
    ctor.Bind("manage_enabled", &m->manage_enabled);
    ctor.Bind("change_positive", &m->change_positive);
    ctor.Bind("change_negative", &m->change_negative);
    ctor.Bind("has_buildings", &m->has_buildings);
    m->handle = ctor.GetModelHandle();

    ctor.BindEventCallback("tab", [m](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) {
        if (!args.empty()) {
            m->select_tab(args[0].Get<Rml::String>());
        }
    });
    ctor.BindEventCallback("close", [m](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
        m->visible = false;
        m->apply_visibility();
        if (m->on_close) {
            m->on_close();
        }
    });
    ctor.BindEventCallback("manage", [m](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
        if (m->manage_enabled && m->on_manage) {
            m->on_manage();
        }
    });

    Rml::ElementDocument* loaded = ui.load_document(document);
    if (loaded == nullptr) {
        ui.context()->RemoveDataModel(kModelName);
        m->ui = nullptr;
        return R::err(core::Error{103, "cannot load the province document " + document.generic_string()});
    }
    m->source_url = loaded->GetSourceURL();
    auto window = std::unique_ptr<ProvinceWindow>(new ProvinceWindow(std::move(impl)));
    window->relocalize();
    window->impl_->apply_visibility();
    return R::ok(std::move(window));
}

void ProvinceWindow::set_snapshot(const ProvinceSnapshot& snapshot) {
    impl_->snapshot = snapshot;
    impl_->refresh_snapshot();
}

const ProvinceSnapshot& ProvinceWindow::snapshot() const {
    return impl_->snapshot;
}

void ProvinceWindow::relocalize() {
    impl_->refresh_chrome();
    impl_->refresh_snapshot();
}

const Explanation* ProvinceWindow::explanation(std::string_view key) const {
    const SplitPath path = split_tooltip_path(key);
    const ProvinceSnapshot& snapshot = impl_->snapshot;
    const ProvinceEconomySnapshot& economy = snapshot.economy;
    const Explanation* root = nullptr;
    if (path.key == "province:income") {
        root = &economy.projected_income.explanation;
    } else if (path.key == "province:change") {
        root = &snapshot.income_change.explanation;
    } else if (path.key == "province:population") {
        root = &economy.population.explanation;
    } else if (path.key == "province:order") {
        root = &economy.control.explanation;
    } else if (path.key == "province:garrison") {
        root = &snapshot.garrison_explanation;
    } else if (path.key == "province:state") {
        root = &snapshot.state_explanation;
    } else if (path.key == "province:manage") {
        root = &snapshot.manage_explanation;
    } else if (path.key == "province:income-last") {
        root = &economy.income_last_month.explanation;
    } else if (path.key == "province:slots") {
        root = &economy.slots.explanation;
    } else if (path.key == "province:free-slots") {
        root = &economy.empty_slots.explanation;
    } else if (path.key.starts_with("province:group:")) {
        root =
            indexed_explanation(path.key.substr(std::string_view("province:group:").size()), economy.groups);
    } else if (path.key.starts_with("province:building:")) {
        root = indexed_explanation(path.key.substr(std::string_view("province:building:").size()),
                                   economy.buildings);
    }
    return root == nullptr ? nullptr : find_term(*root, path.terms);
}

void ProvinceWindow::set_tab(std::string_view tab) {
    impl_->select_tab(tab);
}

std::string_view ProvinceWindow::tab() const {
    return impl_->tab;
}

void ProvinceWindow::set_close_handler(std::function<void()> handler) {
    impl_->on_close = std::move(handler);
}

void ProvinceWindow::set_manage_handler(std::function<void()> handler) {
    impl_->on_manage = std::move(handler);
    impl_->refresh_manage();
}

Rml::ElementDocument* ProvinceWindow::document() const {
    return impl_->find_document();
}

void ProvinceWindow::show(bool visible) {
    impl_->visible = visible;
    impl_->apply_visibility();
}

bool ProvinceWindow::visible() const {
    return impl_->visible;
}

}  // namespace pat::ui
