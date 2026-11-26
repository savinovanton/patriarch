#include "pat/content/event_content.hpp"

#include "pat/data/content_schemas.hpp"
#include "pat/data/event_schemas.hpp"
#include "pat/data/schema.hpp"
#include "pat/sim/date.hpp"

#include <algorithm>
#include <cstddef>
#include <format>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "event_compiler.hpp"
#include "typed_accessors.hpp"

namespace pat::content {

namespace {

namespace ev = sim::events;
using data::ContentObject;
using data::ErrorCode;
using data::TypedBlock;
using data::TypedValue;
using events::Diag;

// Breakdown families of event-engine.md section 8.5.
constexpr std::uint16_t kAiChanceStep = 13000;
constexpr std::uint16_t kWeightStep = 13200;
constexpr std::uint16_t kChanceStep = 13300;

// ---- typed field readers -----------------------------------------------------------------------------

const TypedValue* field(const TypedBlock& block, std::string_view name) {
    return block.find(name);
}

std::optional<std::string> text_field(const TypedBlock& block, std::string_view name) {
    if (const TypedValue* v = field(block, name)) {
        if (const auto* s = v->get_if<std::string>()) {
            return *s;
        }
    }
    return std::nullopt;
}

std::optional<std::size_t> enum_field(const TypedBlock& block, std::string_view name) {
    if (const TypedValue* v = field(block, name)) {
        if (const auto* e = v->get_if<data::EnumValue>()) {
            return e->index;
        }
    }
    return std::nullopt;
}

std::optional<bool> bool_field(const TypedBlock& block, std::string_view name) {
    if (const TypedValue* v = field(block, name)) {
        if (const auto* b = v->get_if<bool>()) {
            return *b;
        }
    }
    return std::nullopt;
}

std::optional<std::int64_t> int_field(const TypedBlock& block, std::string_view name) {
    if (const TypedValue* v = field(block, name)) {
        if (const auto* i = v->get_if<std::int64_t>()) {
            return *i;
        }
    }
    return std::nullopt;
}

const data::Value* rules_field(const TypedBlock& block, std::string_view name) {
    if (const TypedValue* v = field(block, name)) {
        if (const auto* r = v->get_if<data::RulesValue>()) {
            return &r->value;
        }
    }
    return nullptr;
}

data::SourcePos pos_of(const TypedBlock& block, std::string_view name, data::SourcePos fallback) {
    return detail::key_pos(block, name, fallback);
}

ev::ScopeKind scope_of_index(std::size_t index) {
    switch (index) {
        case 0:
            return ev::ScopeKind::Character;
        case 1:
            return ev::ScopeKind::Dynasty;
        case 2:
            return ev::ScopeKind::Province;
        default:
            return ev::ScopeKind::Realm;
    }
}

// ---- the build context -----------------------------------------------------------------------------------

struct Build {
    const data::ContentRegistry& registry;
    const ev::Catalog& catalog;
    const EventBuildOptions& options;
    ErrorSink sink;
    ev::EventSettings settings;
    events::NameTable names;
    std::set<std::string, std::less<>> event_ids;
    std::set<std::string, std::less<>> modifier_ids;
    std::set<std::string, std::less<>> pools;
    std::set<std::string, std::less<>> event_refs;
    std::map<std::string, std::uint32_t> file_index;
    std::vector<std::string> files;

    Build(const data::ContentRegistry& r, const ev::Catalog& c, const EventBuildOptions& o)
        : registry(r), catalog(c), options(o), sink(r), names(o.key_function) {}

    std::uint32_t file_of(const ContentObject& object) {
        const auto it = file_index.find(object.file);
        if (it != file_index.end()) {
            return it->second;
        }
        const auto index = static_cast<std::uint32_t>(files.size());
        files.push_back(object.file);
        file_index.emplace(object.file, index);
        return index;
    }

    events::CompileShared shared() {
        events::CompileShared s;
        s.registry = &registry;
        s.catalog = &catalog;
        s.settings = &settings;
        s.names = &names;
        s.event_ids = &event_ids;
        s.modifier_ids = &modifier_ids;
        s.pools = &pools;
        s.event_refs = &event_refs;
        return s;
    }

    void flush(const ContentObject& object, std::vector<Diag>& diags) {
        for (Diag& d : diags) {
            sink.fail(object, d.pos, d.code, std::move(d.message));
        }
        diags.clear();
    }

    bool loc_missing(std::string_view key) const { return options.has_loc_key && !options.has_loc_key(key); }
};

// ---- settings ---------------------------------------------------------------------------------------------

void read_settings(Build& b) {
    const data::ContentTable* table = b.registry.table(data::kEventSettingsKind);
    if (table == nullptr || table->empty()) {
        return;
    }
    bool first = true;
    for (const ContentObject& object : table->objects()) {
        if (object.object.id != "main" || !first) {
            b.sink.fail(object,
                        object.object.pos,
                        ErrorCode::EventBadField,
                        first ? std::string("the only event_settings object is called \"main\"")
                              : std::string("there is only one event_settings object (\"main\")"));
            if (object.object.id != "main") {
                continue;
            }
        }
        if (!first) {
            continue;
        }
        first = false;
        const TypedBlock& body = object.object.body;
        ev::EventSettings& s = b.settings;
        const auto set = [&](std::string_view name, std::int32_t& out) {
            if (const auto v = int_field(body, name)) {
                out = static_cast<std::int32_t>(*v);
            }
        };
        set("modal_per_month", s.modal_per_month);
        set("modal_per_month_crisis", s.modal_per_month_crisis);
        set("crisis_phase_from", s.crisis_phase_from);
        set("defer_max", s.defer_max);
        set("defer_days", s.defer_days);
        set("max_chain_depth", s.max_chain_depth);
        set("max_timer_chain", s.max_timer_chain);
        set("abort_disable_months", s.abort_disable_months);
        set("fire_cap_per_pulse", s.fire_cap_per_pulse);
        set("pulse_month_days", s.pulse_month_days);
        set("pulse_year_days", s.pulse_year_days);
        set("notification_ring", s.notification_ring);
        set("history_ring", s.history_ring);
        if (const auto v = enum_field(body, "frequency_applies_to")) {
            s.frequency_applies_to_all = *v == 1;
        }
        if (const TypedValue* v = field(body, "frequency_choices")) {
            if (const auto* list = v->get_if<data::TypedList>()) {
                s.frequency_choices.clear();
                for (const TypedValue& item : *list) {
                    if (const auto* i = item.get_if<std::int64_t>()) {
                        s.frequency_choices.push_back(static_cast<std::int32_t>(*i));
                    }
                }
                if (s.frequency_choices.empty()) {
                    b.sink.fail(object,
                                pos_of(body, "frequency_choices", object.object.pos),
                                ErrorCode::EventBadField,
                                "frequency_choices must list at least one multiplier");
                }
            }
        }
        if (const TypedValue* v = field(body, "rumor_delay_days")) {
            if (const auto* list = v->get_if<data::TypedList>()) {
                const auto at = [&](std::size_t i) -> std::int32_t {
                    const auto* n = i < list->size() ? (*list)[i].get_if<std::int64_t>() : nullptr;
                    return n != nullptr ? static_cast<std::int32_t>(*n) : 0;
                };
                if (list->size() != 2 || at(0) > at(1)) {
                    b.sink.fail(
                        object,
                        pos_of(body, "rumor_delay_days", object.object.pos),
                        ErrorCode::EventBadField,
                        "rumor_delay_days is two numbers: the shortest and the longest delay, in days");
                } else {
                    s.rumor_delay_min_days = at(0);
                    s.rumor_delay_max_days = at(1);
                }
            }
        }
        if (const TypedValue* v = field(body, "limits")) {
            if (const auto* limits = v->get_if<TypedBlock>()) {
                const auto lower = [&](std::string_view name, std::uint32_t& out) {
                    if (const auto n = int_field(*limits, name)) {
                        out = std::min(out, static_cast<std::uint32_t>(*n));
                    }
                };
                lower("max_trigger_instr", s.max_trigger_instr);
                lower("max_effect_instr", s.max_effect_instr);
                lower("max_commands", s.max_commands);
                lower("max_state_ops", s.max_state_ops);
                lower("max_fanout", s.max_fanout);
            }
        }
    }
}

// ---- one event
// -----------------------------------------------------------------------------------------------

struct EventBuild {
    Build& b;
    const ContentObject& object;
    const TypedBlock& body;
    ev::EventDef def;
    std::vector<Diag> diags;
    std::uint32_t file = 0;
    events::SavedScopes saved;
    std::size_t program_bytes = 0;

    EventBuild(Build& build, const ContentObject& o)
        : b(build), object(o), body(o.object.body), file(build.file_of(o)) {}

    void error(data::SourcePos pos, ErrorCode code, std::string message) {
        diags.push_back(Diag{pos, code, std::move(message)});
    }

    data::SourcePos field_pos(std::string_view name) const { return pos_of(body, name, object.object.pos); }

    void adopt(const std::optional<ev::Program>& program) {
        if (program) {
            program_bytes += program->to_bytes().size();
        }
    }

    events::RulesCompiler compiler() {
        return events::RulesCompiler(b.shared(), diags, file, def.scope, saved);
    }

    std::optional<ev::Program> compile_trigger(const data::Value& v) {
        auto rc = compiler();
        auto program = rc.trigger(v);
        adopt(program);
        return program;
    }

    std::optional<ev::Program> compile_value(const data::Value& v, std::uint16_t family) {
        auto rc = compiler();
        auto program = rc.accumulator(v, family);
        adopt(program);
        return program;
    }

    // ---- scalar fields ----

    void read_scalars() {
        def.id = object.object.id;
        def.key = b.names.key_of(def.id);
        def.scope = scope_of_index(enum_field(body, "scope").value_or(0));
        def.type = static_cast<ev::EventType>(enum_field(body, "type").value_or(0));
        def.category = text_field(body, "category").value_or(std::string());
        def.image = text_field(body, "image").value_or(def.category);
        if (const TypedValue* v = field(body, "tags")) {
            if (const auto* list = v->get_if<data::TypedList>()) {
                for (const TypedValue& item : *list) {
                    if (const auto* s = item.get_if<std::string>()) {
                        def.tags.push_back(*s);
                    }
                }
            }
        }
        def.file = file;
        def.line = object.object.pos.line;
        def.column = object.object.pos.column;
        def.priority = static_cast<std::int32_t>(int_field(body, "priority").value_or(50));

        const bool silent_type = def.type == ev::EventType::Hidden || def.type == ev::EventType::Background;
        if (const auto d = enum_field(body, "display")) {
            def.display = static_cast<ev::Display>(*d);
        } else if (silent_type) {
            def.display = ev::Display::Silent;
        } else if (def.type == ev::EventType::Imperial) {
            def.display = ev::Display::Notification;
        } else {
            def.display = ev::Display::Modal;
        }
        if (const auto u = enum_field(body, "urgency")) {
            def.urgency = static_cast<ev::Urgency>(*u);
        } else {
            def.urgency = def.display == ev::Display::Modal ? ev::Urgency::Important : ev::Urgency::Info;
        }
        def.autopause = static_cast<ev::AutoPause>(enum_field(body, "autopause").value_or(0));
        def.demotable = bool_field(body, "demotable").value_or(true);
        def.rumor = bool_field(body, "rumor").value_or(false);
        def.survives_death = bool_field(body, "survives_death").value_or(false);
        def.single_option = bool_field(body, "single_option").value_or(false);
        def.group_key = text_field(body, "group_key").value_or(std::string());
        if (const auto o = enum_field(body, "once")) {
            def.once = *o == 0 ? ev::OnceMode::Scope : ev::OnceMode::Global;
        }
        def.review = static_cast<ev::ReviewStatus>(enum_field(body, "review").value_or(0));
        if (const auto h = enum_field(body, "history")) {
            def.history = *h == 0 ? ev::HistoryStatus::Unchecked : ev::HistoryStatus::Checked;
        }
        if (const TypedValue* v = field(body, "sources")) {
            if (const auto* list = v->get_if<data::TypedList>()) {
                for (const TypedValue& item : *list) {
                    if (const auto* s = item.get_if<std::string>()) {
                        def.sources.push_back(*s);
                    }
                }
            }
        }
        if (const auto cg = text_field(body, "cooldown_group")) {
            def.cooldown_group = intern(*cg, field_pos("cooldown_group"));
        }
        if (const auto pool = text_field(body, "pool")) {
            def.pool = intern(*pool, field_pos("pool"));
        }
    }

    std::uint64_t intern(const std::string& name, data::SourcePos pos) {
        std::string clash;
        const std::uint64_t key = b.names.add(name, &clash);
        if (!clash.empty()) {
            error(pos,
                  ErrorCode::EventKeyCollision,
                  std::format("the names '{}' and '{}' have the same 64-bit key; rename one", name, clash));
        }
        return key;
    }

    // ---- on-actions ----

    void read_on() {
        const data::Value* on = rules_field(body, "on");
        if (on == nullptr) {
            return;
        }
        std::vector<const data::Value*> items;
        if (on->kind == data::ValueKind::List) {
            for (const data::Value& item : on->items) {
                items.push_back(&item);
            }
        } else {
            items.push_back(on);
        }
        std::vector<ev::OnProvided> provided;
        bool first = true;
        for (const data::Value* item : items) {
            if (item->kind != data::ValueKind::Ident && item->kind != data::ValueKind::String) {
                error(item->pos,
                      ErrorCode::EventBadOnAction,
                      "an on-action is a name such as monthly_pulse or on_death");
                continue;
            }
            std::string name = item->text;
            std::string role;
            if (const auto dot = name.find('.'); dot != std::string::npos) {
                role = name.substr(dot + 1);
                name.resize(dot);
            }
            const ev::OnActionEntry* action = b.catalog.find_on_action(name);
            if (action == nullptr) {
                const std::string hint = data::suggest_similar(name, b.catalog.on_action_names());
                error(item->pos,
                      ErrorCode::EventBadOnAction,
                      std::format("unknown on-action '{}'{}",
                                  name,
                                  hint.empty() ? std::string() : std::format(" (did you mean '{}'?)", hint)));
                continue;
            }
            if (action->reserved) {
                error(item->pos,
                      ErrorCode::EventBadOnAction,
                      std::format(
                          "the on-action '{}' is reserved for a later epic and cannot be subscribed to yet",
                          name));
                continue;
            }
            ev::OnSub sub;
            sub.action = action->code;
            if (action->pulse != ev::PulseKind::None) {
                if (!role.empty()) {
                    error(item->pos,
                          ErrorCode::EventBadOnAction,
                          std::format("'{}' is a pulse and has no roles", name));
                    continue;
                }
                if (!ev::scope_allowed(action->pulse_scopes, def.scope)) {
                    error(item->pos,
                          ErrorCode::EventScopeMismatch,
                          std::format(
                              "'{}' does not run over {} scopes", name, ev::scope_kind_name(def.scope)));
                    continue;
                }
                pulses.insert(action->pulse);
            } else {
                std::size_t index = 0;
                if (!role.empty()) {
                    index = action->roles.size();
                    for (std::size_t i = 0; i < action->roles.size(); ++i) {
                        if (action->roles[i].name == role) {
                            index = i;
                        }
                    }
                    if (index == action->roles.size()) {
                        std::vector<std::string> known;
                        for (const ev::OnRole& r : action->roles) {
                            known.push_back(r.name);
                        }
                        const std::string hint = data::suggest_similar(role, known);
                        error(item->pos,
                              ErrorCode::EventBadOnAction,
                              std::format("'{}' has no role '{}'{}",
                                          name,
                                          role,
                                          hint.empty() ? std::string()
                                                       : std::format(" (did you mean '{}'?)", hint)));
                        continue;
                    }
                }
                if (action->roles.empty() || action->roles[index].kind != def.scope) {
                    error(item->pos,
                          ErrorCode::EventScopeMismatch,
                          std::format("'{}' runs on {} (role '{}'), but the event is for {}",
                                      name,
                                      action->roles.empty()
                                          ? "nothing"
                                          : std::string(ev::scope_kind_name(action->roles[index].kind)),
                                      action->roles.empty() ? "" : action->roles[index].name,
                                      ev::scope_kind_name(def.scope)));
                    continue;
                }
                sub.role = static_cast<std::uint8_t>(index);
            }
            def.on.push_back(sub);
            if (first) {
                provided = action->provides;
                first = false;
            } else {
                std::erase_if(provided, [&](const ev::OnProvided& p) {
                    return std::ranges::none_of(action->provides, [&](const ev::OnProvided& q) {
                        return q.name == p.name && q.kind == p.kind;
                    });
                });
            }
        }
        for (const ev::OnProvided& p : provided) {
            saved.slots.push_back(events::SavedScope{p.name, p.kind});
        }
        def.provided_saved = static_cast<std::uint8_t>(saved.slots.size());
    }

    /// `chain_scopes = { governor = character }`: the saved scopes a chain brings (a timer or an immediate
    /// `trigger_event` of another event keeps them by name, E11-T11); they follow the slots of the on-action
    /// and count as provided by the start.
    void read_chain_scopes() {
        const data::Value* block = rules_field(body, "chain_scopes");
        if (block == nullptr) {
            return;
        }
        if (block->kind != data::ValueKind::Block) {
            error(block->pos,
                  ErrorCode::EventBadArgument,
                  "chain_scopes is a block of names and kinds: chain_scopes = { governor = character }");
            return;
        }
        for (const data::Entry& e : block->entries) {
            const ev::ScopeKind kind = e.value.kind == data::ValueKind::Ident
                                           ? ev::scope_kind_from_name(e.value.text)
                                           : ev::ScopeKind::None;
            if (e.key_kind != data::ValueKind::Ident || kind == ev::ScopeKind::None) {
                error(e.pos,
                      ErrorCode::EventBadArgument,
                      "a chain scope is written name = character, dynasty, province or realm");
                continue;
            }
            if (std::ranges::any_of(saved.slots,
                                    [&](const events::SavedScope& s) { return s.name == e.key; })) {
                error(
                    e.pos, ErrorCode::EventBadArgument, std::format("the scope '{}' is named twice", e.key));
                continue;
            }
            if (saved.slots.size() >= ev::limits::kMaxSavedScopes) {
                error(e.pos,
                      ErrorCode::EventLimit,
                      std::format("an event saves at most {} scopes", ev::limits::kMaxSavedScopes));
                break;
            }
            saved.slots.push_back(events::SavedScope{e.key, kind});
        }
        def.provided_saved = static_cast<std::uint8_t>(saved.slots.size());
    }

    std::set<ev::PulseKind> pulses;

    // ---- timing: mean_time_to_happen, chance, cooldown, window ----

    void read_timing() {
        const data::Value* mtth = rules_field(body, "mean_time_to_happen");
        const data::Value* chance = rules_field(body, "chance");
        if (mtth != nullptr && chance != nullptr) {
            error(field_pos("chance"),
                  ErrorCode::EventBadField,
                  "mean_time_to_happen and chance cannot be used together");
        }
        if (mtth != nullptr) {
            read_mtth(*mtth);
        }
        if (chance != nullptr) {
            def.chance = compile_value(*chance, kChanceStep);
        }
        if (const data::Value* cooldown = rules_field(body, "cooldown")) {
            if (const auto days = events::parse_duration(*cooldown, b.settings, diags)) {
                def.cooldown_days = static_cast<std::int32_t>(*days);
            }
        }
        if (const TypedValue* w = field(body, "window")) {
            if (const auto* block = w->get_if<TypedBlock>()) {
                def.has_window = true;
                def.window_from = std::numeric_limits<std::int32_t>::min();
                def.window_to = std::numeric_limits<std::int32_t>::max();
                const auto date_of = [&](std::string_view name) -> std::optional<std::int32_t> {
                    if (const TypedValue* d = field(*block, name)) {
                        if (const auto* dv = d->get_if<data::DateValue>()) {
                            return sim::Date::from_ymd(dv->year, dv->month, dv->day).days();
                        }
                    }
                    return std::nullopt;
                };
                const auto from = date_of("from");
                const auto to = date_of("to");
                if (from) {
                    def.window_from = *from;
                }
                if (to) {
                    def.window_to = *to;
                }
                def.anchor = bool_field(*block, "anchor").value_or(false);
                if (from && to && *to < *from) {
                    error(pos_of(*block, "to", w->pos),
                          ErrorCode::EventBadField,
                          "the window ends before it starts (to < from)");
                }
                if (def.anchor && !from && !to) {
                    error(w->pos,
                          ErrorCode::EventBadField,
                          "a historical anchor needs a window with from or to");
                }
            }
        }
        if (def.pool != 0 && rules_field(body, "weight") == nullptr) {
            error(field_pos("pool"), ErrorCode::EventBadField, "an event in a pool needs a weight");
        }
    }

    void read_mtth(const data::Value& v) {
        if (v.kind != data::ValueKind::Block) {
            error(field_pos("mean_time_to_happen"),
                  ErrorCode::EventBadArgument,
                  "mean_time_to_happen is a duration block: { months = 24 }");
            return;
        }
        data::Value duration;
        duration.kind = data::ValueKind::Block;
        duration.pos = v.pos;
        std::vector<const data::Entry*> modifiers;
        for (const data::Entry& e : v.entries) {
            if (e.key_kind == data::ValueKind::Ident && e.key == "modifier") {
                modifiers.push_back(&e);
            } else {
                duration.entries.push_back(e);
            }
        }
        const auto days = events::parse_duration(duration, b.settings, diags);
        if (!days) {
            return;
        }
        if (*days <= 0) {
            error(v.pos, ErrorCode::EventBadConstant, "the mean time to happen must be longer than 0 days");
            return;
        }
        def.mtth_days = static_cast<std::int32_t>(*days);
        if (pulses.empty()) {
            error(field_pos("mean_time_to_happen"),
                  ErrorCode::EventBadField,
                  "mean_time_to_happen needs a pulse on-action (monthly_pulse, quarterly_pulse, "
                  "yearly_pulse, daily_pulse)");
        }
        for (const ev::PulseKind pulse : pulses) {
            def.mtth_ppm[static_cast<std::size_t>(pulse)] = ev::mtth_ppm(b.settings.pulse_days(pulse), *days);
        }
        for (const data::Entry* m : modifiers) {
            read_mtth_modifier(*m);
        }
    }

    void read_mtth_modifier(const data::Entry& m) {
        if (m.value.kind != data::ValueKind::Block) {
            error(m.value.pos,
                  ErrorCode::EventBadArgument,
                  "a modifier is a block: modifier = { factor = 0.5  trigger = { ... } }");
            return;
        }
        const data::Entry* factor = nullptr;
        const data::Entry* trigger = nullptr;
        for (const data::Entry& e : m.value.entries) {
            if (e.key == "factor" && factor == nullptr) {
                factor = &e;
            } else if (e.key == "trigger" && trigger == nullptr) {
                trigger = &e;
            } else {
                error(e.pos,
                      ErrorCode::EventUnknownEntry,
                      std::format("unknown entry '{}' in a modifier (use factor and trigger)", e.key));
            }
        }
        if (factor == nullptr || trigger == nullptr) {
            error(m.pos, ErrorCode::EventUnknownEntry, "a modifier needs factor = N and trigger = { ... }");
            return;
        }
        const auto f = events::fixed_of(factor->value);
        if (!f || f->raw() <= 0 || f->raw() > (std::int64_t{1000} << 32)) {
            error(factor->value.pos,
                  ErrorCode::EventBadConstant,
                  "the factor must be greater than 0 and at most 1000");
            return;
        }
        ev::MtthModifier mod;
        mod.factor_permille = static_cast<std::int32_t>((f->raw() * 1000) >> 32);
        if (mod.factor_permille < 1) {
            error(factor->value.pos, ErrorCode::EventBadConstant, "the factor is too small (below 0.001)");
            return;
        }
        if (auto program = compile_trigger(trigger->value)) {
            mod.trigger = std::move(*program);
            def.mtth_modifiers.push_back(std::move(mod));
        }
    }

    // ---- programs, options, texts ----

    void compile_programs() {
        if (const data::Value* v = rules_field(body, "trigger")) {
            def.trigger = compile_trigger(*v);
        }
        if (const data::Value* v = rules_field(body, "weight")) {
            def.weight = compile_value(*v, kWeightStep);
        }
        if (const data::Value* v = rules_field(body, "immediate")) {
            auto rc = compiler();
            def.immediate = rc.effect(*v);
            adopt(def.immediate);
            saved = rc.saved();
        }
        if (const data::Value* v = rules_field(body, "speaker")) {
            auto rc = compiler();
            def.speaker = rc.scope_value(*v);
            adopt(def.speaker);
        }
        def.saved_names.clear();
        for (const events::SavedScope& s : saved.slots) {
            def.saved_names.push_back(s.name);
        }
        compile_options();
        if (const data::Value* v = rules_field(body, "after")) {
            auto rc = compiler();
            def.after = rc.effect(*v);
            adopt(def.after);
        }
        if (program_bytes > ev::limits::kMaxProgramBytes) {
            error(object.object.pos,
                  ErrorCode::EventLimit,
                  std::format("the programs of the event take {} bytes, the limit is {}",
                              program_bytes,
                              ev::limits::kMaxProgramBytes));
        }
    }

    void compile_options() {
        std::set<std::uint64_t> option_keys;
        std::size_t defaults = 0;
        for (const data::TypedField& f : body.fields) {
            if (f.name != "option") {
                continue;
            }
            const auto* block = f.value.get_if<TypedBlock>();
            if (block == nullptr) {
                continue;
            }
            ev::OptionDef option;
            option.name = f.label;
            option.key = b.names.key_of(f.label);
            if (!option_keys.insert(option.key).second) {
                error(f.pos,
                      ErrorCode::EventKeyCollision,
                      std::format("the option '{}' has the same 64-bit key as another option of this event",
                                  f.label));
            }
            const std::size_t base = saved.slots.size();
            const auto option_compiler = [&]() {
                return events::RulesCompiler(b.shared(), diags, file, def.scope, saved);
            };
            std::size_t local = 0;
            if (const data::Value* v = rules_field(*block, "trigger")) {
                auto rc = option_compiler();
                option.available = rc.trigger(*v);
                adopt(option.available);
                local = std::max(local, rc.saved().slots.size() - base);
            }
            if (const data::Value* v = rules_field(*block, "show_if")) {
                auto rc = option_compiler();
                option.visible = rc.trigger(*v);
                adopt(option.visible);
            }
            if (const data::Value* v = rules_field(*block, "ai_chance")) {
                auto rc = option_compiler();
                option.ai_chance = rc.accumulator(*v, kAiChanceStep);
                adopt(option.ai_chance);
            }
            if (const data::Value* v = rules_field(*block, "effect")) {
                auto rc = option_compiler();
                option.effect = rc.effect(*v);
                adopt(option.effect);
                local = std::max(local, rc.saved().slots.size() - base);
            }
            option.local_saved = static_cast<std::uint8_t>(local);
            option.is_default = bool_field(*block, "default").value_or(false);
            option.neutral = bool_field(*block, "neutral").value_or(false);
            defaults += option.is_default ? 1U : 0U;
            option.text_key =
                text_field(*block, "text").value_or(std::format("evt.{}.opt.{}", def.id, f.label));
            option.detail_key = text_field(*block, "detail").value_or(std::string());
            if (def.type != ev::EventType::Hidden && b.loc_missing(option.text_key)) {
                error(pos_of(*block, "text", f.pos),
                      ErrorCode::EventMissingText,
                      std::format("missing localization key '{}' (text of the option '{}')",
                                  option.text_key,
                                  f.label));
            }
            if (!option.detail_key.empty() && b.loc_missing(option.detail_key)) {
                error(pos_of(*block, "detail", f.pos),
                      ErrorCode::EventMissingText,
                      std::format("missing localization key '{}'", option.detail_key));
            }
            def.options.push_back(std::move(option));
        }
        if (defaults > 1) {
            error(object.object.pos,
                  ErrorCode::EventBadOptions,
                  "more than one option is marked default = yes");
        }
        const bool modal = def.display == ev::Display::Modal;
        if (def.type == ev::EventType::Hidden && !def.options.empty()) {
            error(field_pos("option"),
                  ErrorCode::EventBadOptions,
                  "a hidden event has no options: put its effects in immediate");
        } else if (def.options.size() > 4) {
            error(field_pos("option"),
                  ErrorCode::EventBadOptions,
                  std::format("an event has at most 4 options, this one has {}", def.options.size()));
        } else if (modal && def.options.empty() && def.type != ev::EventType::Hidden) {
            error(object.object.pos, ErrorCode::EventBadOptions, "a modal event needs at least one option");
        }
        if (def.single_option && def.options.size() != 1) {
            error(field_pos("single_option"),
                  ErrorCode::EventBadOptions,
                  "single_option = yes needs exactly one option");
        }
    }

    void read_texts() {
        const bool shown = def.type != ev::EventType::Hidden && def.display != ev::Display::Silent;
        def.title_key = text_field(body, "title").value_or(std::format("evt.{}.title", def.id));
        def.desc_key = text_field(body, "desc").value_or(std::format("evt.{}.desc", def.id));
        def.subtitle_key = text_field(body, "subtitle").value_or(std::string());
        def.quote_key = text_field(body, "quote").value_or(std::string());
        const auto check =
            [&](const std::string& key, std::string_view what, std::string_view field_name, bool required) {
                (void)required;
                if (!key.empty() && b.loc_missing(key)) {
                    error(pos_of(body, field_name, object.object.pos),
                          ErrorCode::EventMissingText,
                          std::format("missing localization key '{}' ({} of the event)", key, what));
                }
            };
        if (shown) {
            check(def.title_key, "title", "title", true);
            check(def.desc_key, "text", "desc", true);
        }
        check(def.subtitle_key, "subtitle", "subtitle", false);
        check(def.quote_key, "quote", "quote", false);
        if (b.options.has_image && !def.image.empty() && !b.options.has_image(def.image)) {
            const std::string_view where = field(body, "image") != nullptr ? "image" : "category";
            error(pos_of(body, where, object.object.pos),
                  ErrorCode::EventUnresolvedReference,
                  std::format("unknown image pool '{}'", def.image));
        }
    }

    ev::EventDef build() {
        read_scalars();
        read_on();
        read_chain_scopes();
        read_timing();
        compile_programs();
        read_texts();
        return std::move(def);
    }
};

// ---- modifiers
// -------------------------------------------------------------------------------------------------

std::optional<ev::EventModifierDef> build_modifier(Build& b, const ContentObject& object) {
    const TypedBlock& body = object.object.body;
    ev::EventModifierDef m;
    m.id = object.object.id;
    m.key = b.names.key_of(m.id);
    m.scope = scope_of_index(enum_field(body, "scope").value_or(0));
    m.stack = static_cast<ev::StackMode>(enum_field(body, "stack").value_or(0));
    std::vector<Diag> diags;
    if (const data::Value* d = rules_field(body, "duration")) {
        if (const auto days = events::parse_duration(*d, b.settings, diags)) {
            m.duration_days = static_cast<std::int32_t>(*days);
        }
    }
    if (const TypedValue* v = field(body, "values")) {
        if (const auto* map = v->get_if<data::TypedMap>()) {
            for (const data::TypedField& entry : map->entries) {
                std::string clash;
                const std::uint64_t key = b.names.add(entry.name, &clash);
                if (!clash.empty()) {
                    diags.push_back(
                        Diag{entry.pos,
                             ErrorCode::EventKeyCollision,
                             std::format("the names '{}' and '{}' have the same 64-bit key; rename one",
                                         entry.name,
                                         clash)});
                }
                if (const auto* n = entry.value.get_if<core::Fixed64>()) {
                    m.values.emplace_back(key, *n);
                }
            }
            std::ranges::sort(m.values, [](const auto& x, const auto& y) { return x.first < y.first; });
        }
    }
    m.tooltip_key = text_field(body, "tooltip").value_or(std::format("modifier.{}", m.id));
    if (b.loc_missing(m.tooltip_key)) {
        diags.push_back(
            Diag{pos_of(body, "tooltip", object.object.pos),
                 ErrorCode::EventMissingText,
                 std::format("missing localization key '{}' (name of the modifier)", m.tooltip_key)});
    }
    const bool failed = !diags.empty();
    b.flush(object, diags);
    return failed ? std::nullopt : std::optional<ev::EventModifierDef>(std::move(m));
}

}  // namespace

EventSetResult build_event_set(const data::ContentRegistry& registry,
                               const ev::Catalog& catalog,
                               const EventBuildOptions& options) {
    Build b(registry, catalog, options);
    read_settings(b);

    const data::ContentTable* events_table = registry.table(data::kEventKind);
    const data::ContentTable* modifiers_table = registry.table(data::kEventModifierKind);

    // Names first: every event, modifier and pool must be known before any block refers to it.
    const auto register_id = [&](const ContentObject& object) {
        std::string clash;
        (void)b.names.add(object.object.id, &clash);
        if (!clash.empty()) {
            b.sink.fail(object,
                        object.object.pos,
                        ErrorCode::EventKeyCollision,
                        std::format("the id '{}' has the same 64-bit key as '{}'; rename one of them",
                                    object.object.id,
                                    clash));
        }
    };
    if (events_table != nullptr) {
        for (const ContentObject& object : events_table->objects()) {
            b.event_ids.insert(object.object.id);
            register_id(object);
            if (const auto pool = text_field(object.object.body, "pool")) {
                b.pools.insert(*pool);
            }
        }
    }
    if (modifiers_table != nullptr) {
        for (const ContentObject& object : modifiers_table->objects()) {
            b.modifier_ids.insert(object.object.id);
            register_id(object);
        }
    }

    ev::EventSet set;
    if (modifiers_table != nullptr) {
        for (const ContentObject& object : modifiers_table->objects()) {
            if (auto m = build_modifier(b, object)) {
                set.modifiers.push_back(std::move(*m));
            }
        }
    }
    std::vector<const ContentObject*> chain_checks;  // events with `once` and no `on`
    if (events_table != nullptr) {
        for (const ContentObject& object : events_table->objects()) {
            EventBuild eb(b, object);
            ev::EventDef def = eb.build();
            const bool failed = !eb.diags.empty();
            if (def.once != ev::OnceMode::None && def.on.empty()) {
                chain_checks.push_back(&object);
            }
            b.flush(object, eb.diags);
            if (!failed) {
                set.events.push_back(std::move(def));
            }
        }
    }
    for (const ContentObject* object : chain_checks) {
        if (!b.event_refs.contains(object->object.id)) {
            b.sink.fail(
                *object,
                pos_of(object->object.body, "once", object->object.pos),
                ErrorCode::EventBadField,
                "'once' needs an on-action or a chain: no 'on' is written and no effect starts this event");
        }
    }

    if (!b.sink.empty()) {
        return EventSetResult::err(b.sink.take());
    }
    std::ranges::sort(set.events, [](const ev::EventDef& x, const ev::EventDef& y) { return x.key < y.key; });
    std::ranges::sort(set.modifiers, [](const ev::EventModifierDef& x, const ev::EventModifierDef& y) {
        return x.key < y.key;
    });
    set.settings = b.settings;
    set.files = b.files;
    for (const auto& [key, name] : b.names.names()) {
        set.key_names.emplace_back(key, name);
    }
    return EventSetResult::ok(std::move(set));
}

std::uint64_t event_content_hash(const data::ContentRegistry& registry) {
    return registry.content_hash_excluding(data::kEventKinds);
}

EventContentResult load_event_content(std::span<const data::ContentSource* const> roots,
                                      const ev::Catalog& catalog,
                                      const EventBuildOptions& options) {
    auto loaded = data::load_content(roots, data::content_schemas());
    if (!loaded.ok()) {
        return EventContentResult::err(std::move(loaded.errors));
    }
    auto set = build_event_set(loaded.registry, catalog, options);
    if (!set) {
        return EventContentResult::err(std::move(set).error());
    }
    return EventContentResult::ok({std::move(set).value(), event_content_hash(loaded.registry)});
}

EventContentResult load_event_content(const data::ContentSource& root,
                                      const ev::Catalog& catalog,
                                      const EventBuildOptions& options) {
    const data::ContentSource* roots[] = {&root};
    return load_event_content(roots, catalog, options);
}

}  // namespace pat::content
