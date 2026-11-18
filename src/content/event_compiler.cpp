#include "event_compiler.hpp"

#include "pat/core/rng.hpp"
#include "pat/data/schema.hpp"
#include "pat/sim/date.hpp"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <format>
#include <limits>
#include <utility>

namespace pat::content::events {

namespace ev = sim::events;
using ev::ArgKind;
using ev::CatalogEntry;
using ev::NameSpace;
using ev::Op;
using ev::ScopeKind;
using ev::ValueType;
using Label = ev::ProgramBuilder::Label;

// ---- helpers ------------------------------------------------------------------------------------------

namespace {

bool starts_with(std::string_view text, std::string_view prefix) {
    return text.size() >= prefix.size() && text.substr(0, prefix.size()) == prefix;
}

std::string
unknown_text(std::string_view what, std::string_view name, const std::vector<std::string>& known) {
    std::string text = std::format("unknown {} '{}'", what, name);
    if (const std::string hint = data::suggest_similar(name, known); !hint.empty()) {
        text += std::format(" (did you mean '{}'?)", hint);
    }
    return text;
}

std::string kinds_text(ev::ScopeMask mask) {
    if (mask == 0) {
        return "any scope";
    }
    std::string out;
    for (unsigned k = 1; k < 32; ++k) {
        if ((mask & (ev::ScopeMask{1} << k)) != 0) {
            if (!out.empty()) {
                out += " or ";
            }
            out += ev::scope_kind_name(static_cast<ScopeKind>(k));
        }
    }
    return out;
}

Op compare_op(data::Op op) {
    switch (op) {
        case data::Op::Eq:
            return Op::kEq;
        case data::Op::Ne:
            return Op::kNe;
        case data::Op::Lt:
            return Op::kLt;
        case data::Op::Le:
            return Op::kLe;
        case data::Op::Gt:
            return Op::kGt;
        case data::Op::Ge:
            return Op::kGe;
    }
    return Op::kEq;
}

const data::Entry* find_entry(const std::vector<data::Entry>& entries, std::string_view key) {
    for (const data::Entry& e : entries) {
        if (e.key_kind == data::ValueKind::Ident && e.key == key) {
            return &e;
        }
    }
    return nullptr;
}

// Lists that would iterate the whole world: forbidden (EV107, ADR-0002 P8).
bool is_world_list(std::string_view name) {
    static constexpr std::string_view kWorld[] = {"character",
                                                  "living_character",
                                                  "province",
                                                  "dynasty",
                                                  "landed_province",
                                                  "realm_province",
                                                  "player"};
    return std::ranges::find(kWorld, name) != std::end(kWorld);
}

constexpr std::string_view kAccumulatorKeys[] = {"base",
                                                 "value",
                                                 "add",
                                                 "subtract",
                                                 "multiply",
                                                 "divide",
                                                 "min",
                                                 "max",
                                                 "round",
                                                 "floor",
                                                 "ceiling",
                                                 "if"};

bool is_accumulator_key(std::string_view key) {
    return std::ranges::find(kAccumulatorKeys, key) != std::end(kAccumulatorKeys);
}

}  // namespace

// ---- NameTable ------------------------------------------------------------------------------------------

NameTable::NameTable(KeyFn fn) : fn_(fn ? std::move(fn) : KeyFn(ev::event_key)) {}

std::uint64_t NameTable::add(std::string_view name, std::string* clash) {
    const std::uint64_t key = fn_(name);
    const auto it = names_.find(key);
    if (it == names_.end()) {
        names_.emplace(key, std::string(name));
    } else if (it->second != name && clash != nullptr) {
        *clash = it->second;
    }
    return key;
}

std::uint64_t NameTable::key_of(std::string_view name) const {
    return fn_(name);
}

// ---- numbers and durations ---------------------------------------------------------------------------

std::optional<core::Fixed64> fixed_of(const data::Value& value) {
    core::Fixed64 out;
    if (value.kind == data::ValueKind::Int) {
        if (data::decimal_to_fixed(value.integer, 0, out)) {
            return out;
        }
    } else if (value.kind == data::ValueKind::Decimal) {
        if (data::decimal_to_fixed(value.integer, value.scale, out)) {
            return out;
        }
    }
    return std::nullopt;
}

std::optional<std::int64_t>
parse_duration(const data::Value& value, const ev::EventSettings& settings, std::vector<Diag>& diags) {
    const auto fail = [&](data::SourcePos pos, std::string text) {
        diags.push_back(Diag{pos, data::ErrorCode::EventBadArgument, std::move(text)});
        return std::optional<std::int64_t>();
    };
    if (value.kind == data::ValueKind::Int) {
        if (value.integer < 0 || value.integer > 100000) {
            return fail(value.pos, "a duration must be between 0 and 100000 days");
        }
        return value.integer;
    }
    if (value.kind != data::ValueKind::Block) {
        return fail(value.pos,
                    "expected a duration: { days = N  months = N  years = N } or a number of days");
    }
    std::int64_t days = 0;
    bool ok = true;
    bool any = false;
    for (const data::Entry& e : value.entries) {
        std::int64_t unit = 0;
        if (e.key == "days") {
            unit = 1;
        } else if (e.key == "months") {
            unit = settings.pulse_month_days;
        } else if (e.key == "years") {
            unit = settings.pulse_year_days;
        } else {
            diags.push_back(
                Diag{e.pos,
                     data::ErrorCode::EventUnknownEntry,
                     std::format("unknown entry '{}' in a duration (use days, months, years)", e.key)});
            ok = false;
            continue;
        }
        if (e.value.kind != data::ValueKind::Int || e.value.integer < 0 || e.value.integer > 100000) {
            diags.push_back(Diag{e.value.pos,
                                 data::ErrorCode::EventBadArgument,
                                 std::format("'{}' needs a whole number from 0 to 100000", e.key)});
            ok = false;
            continue;
        }
        days += e.value.integer * unit;
        any = true;
    }
    if (ok && !any) {
        return fail(value.pos, "an empty duration; write { days = N }, { months = N } or { years = N }");
    }
    if (!ok) {
        return std::nullopt;
    }
    if (days > 1000000) {
        return fail(value.pos, "the duration is longer than 1000000 days");
    }
    return days;
}

// ---- the compiler ---------------------------------------------------------------------------------------

namespace {

enum class Terminal : std::uint8_t { Scope, Value, Predicate, Variable, GlobalVariable };

constexpr unsigned kTermScope = 1;
constexpr unsigned kTermValue = 2;
constexpr unsigned kTermPredicate = 4;
constexpr unsigned kTermVariable = 8;

struct Seg {
    std::string text;
    std::uint32_t offset = 0;
};

struct PathText {
    std::vector<Seg> segs;
    data::SourcePos pos;
};

struct Step {
    enum class Kind : std::uint8_t { Root, This, Saved, Transition } kind = Kind::This;
    std::uint16_t arg = 0;
    ScopeKind to = ScopeKind::None;
};

struct Plan {
    std::vector<Step> steps;
    Terminal term = Terminal::Scope;
    const CatalogEntry* entry = nullptr;
    int suffix = -1;
    std::string var_name;
    ScopeKind end = ScopeKind::None;
    data::SourcePos term_pos;
};

PathText split_path(std::string_view text, data::SourcePos pos) {
    PathText out;
    out.pos = pos;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= text.size(); ++i) {
        if (i == text.size() || text[i] == '.' || text[i] == ':') {
            out.segs.push_back(
                Seg{std::string(text.substr(start, i - start)), static_cast<std::uint32_t>(start)});
            start = i + 1;
        }
    }
    return out;
}

data::SourcePos at(const PathText& p, std::size_t seg) {
    return data::SourcePos{p.pos.line, p.pos.column + p.segs[seg].offset};
}

}  // namespace

class RulesCompiler::Impl {
public:
    Impl(const CompileShared& shared,
         std::vector<Diag>& diags,
         std::uint32_t file,
         ScopeKind root,
         SavedScopes saved)
        : sh_(shared), diags_(diags), file_(file), root_(root), cur_(root), saved_(std::move(saved)) {}

    [[nodiscard]] const SavedScopes& saved() const noexcept { return saved_; }

    std::optional<ev::Program> trigger(const data::Value& block) {
        if (block.kind != data::ValueKind::Block) {
            err(block.pos, data::ErrorCode::EventBadArgument, "expected a block of conditions { ... }");
            return std::nullopt;
        }
        trigger_and(block.entries, nullptr);
        return finish();
    }

    std::optional<ev::Program> accumulator(const data::Value& value, std::uint16_t step_base) {
        step_code_ = step_base;
        operand(value, nullptr, true);
        return finish();
    }

    std::optional<ev::Program> effect(const data::Value& block) {
        if (block.kind != data::ValueKind::Block) {
            err(block.pos, data::ErrorCode::EventBadArgument, "expected a block of effects { ... }");
            return std::nullopt;
        }
        effect_block(block.entries);
        return finish();
    }

    std::optional<ev::Program> scope_value(const data::Value& value) {
        const std::optional<PathText> path = path_of_value(value);
        if (path) {
            if (const std::optional<Plan> plan = analyze(*path, kTermScope, "a scope", {})) {
                set_source(value.pos);
                emit_guarded(*plan, [&] { b_.emit(Op::kPushScope); }, [&] { b_.push_int(0); }, std::nullopt);
            }
        }
        return finish();
    }

private:
    // ---- plumbing ----

    void err(data::SourcePos pos, data::ErrorCode code, std::string message) {
        diags_.push_back(Diag{pos, code, std::move(message)});
        ++errors_;
    }

    void set_source(data::SourcePos pos) { b_.set_source(file_, pos.line, pos.column); }

    std::optional<ev::Program> finish() {
        b_.emit(Op::kRet);
        if (errors_ != 0) {
            return std::nullopt;
        }
        auto built = b_.finish();
        if (!built) {
            const ev::ProgramError& e = built.error();
            err(data::SourcePos{},
                data::ErrorCode::EventLimit,
                e.code == ev::ProgramErrorCode::StackOverflow || e.code == ev::ProgramErrorCode::TooLong
                    ? std::format("the block is too complex: {} (limit {} values on the stack)",
                                  e.message,
                                  ev::limits::kMaxStack)
                    : std::format("internal compiler error: {} at instruction {}", e.message, e.pc));
            return std::nullopt;
        }
        return std::move(built).value();
    }

    void note_depth() {
        if (depth_ > ev::limits::kMaxScopeDepth && !depth_reported_) {
            depth_reported_ = true;
            err(source_,
                data::ErrorCode::EventLimit,
                std::format("scopes are nested deeper than {} levels", ev::limits::kMaxScopeDepth));
        }
    }

    // ---- paths ----

    std::optional<PathText> path_of_value(const data::Value& v) {
        if (v.kind == data::ValueKind::Ident) {
            return split_path(v.text, v.pos);
        }
        if (v.kind == data::ValueKind::Ref) {
            if (data::ref_prefix(v) != "scope") {
                err(v.pos,
                    data::ErrorCode::EventBadArgument,
                    std::format("unknown reference '{}': only 'scope:name' is allowed here", v.text));
                return std::nullopt;
            }
            return split_path(v.text, v.pos);
        }
        err(v.pos,
            data::ErrorCode::EventBadArgument,
            "expected a name or a path, for example scope:name or owner.honor");
        return std::nullopt;
    }

    int saved_slot(std::string_view name) const {
        for (std::size_t i = 0; i < saved_.slots.size(); ++i) {
            if (saved_.slots[i].name == name) {
                return static_cast<int>(i);
            }
        }
        return -1;
    }

    // Resolves a path: the scope steps and the terminal. Reports every problem; nullopt on error.
    std::optional<Plan> analyze(const PathText& p,
                                unsigned allowed,
                                std::string_view expecting,
                                const std::vector<std::string>& extra_names) {
        Plan plan;
        ScopeKind cur = cur_;
        std::size_t i = 0;
        const std::string& head = p.segs[0].text;
        if (head == "root") {
            plan.steps.push_back(Step{Step::Kind::Root, 0, root_});
            cur = root_;
            i = 1;
        } else if (head == "this") {
            plan.steps.push_back(Step{Step::Kind::This, 0, cur});
            i = 1;
        } else if (head == "scope") {
            if (p.segs.size() < 2 || p.segs[1].text.empty()) {
                err(at(p, 0), data::ErrorCode::EventBadArgument, "'scope' needs a name: scope:name");
                return std::nullopt;
            }
            const int slot = saved_slot(p.segs[1].text);
            if (slot < 0) {
                std::vector<std::string> known;
                for (const SavedScope& s : saved_.slots) {
                    known.push_back(s.name);
                }
                err(at(p, 1),
                    data::ErrorCode::EventUnresolvedReference,
                    unknown_text("saved scope", p.segs[1].text, known) +
                        "; save it first with save_scope_as, or use a scope the on-action provides");
                return std::nullopt;
            }
            plan.steps.push_back(Step{Step::Kind::Saved,
                                      static_cast<std::uint16_t>(slot),
                                      saved_.slots[static_cast<std::size_t>(slot)].kind});
            cur = saved_.slots[static_cast<std::size_t>(slot)].kind;
            i = 2;
        }
        while (i < p.segs.size()) {
            const std::string& seg = p.segs[i].text;
            const CatalogEntry* t = sh_.catalog->find(NameSpace::Transition, seg);
            if (t == nullptr) {
                break;
            }
            if (t->retired) {
                err(at(p, i),
                    data::ErrorCode::EventRetired,
                    std::format("transition '{}' is retired: {}", seg, t->retired_hint));
                return std::nullopt;
            }
            if (!ev::scope_allowed(t->from, cur)) {
                err(at(p, i),
                    data::ErrorCode::EventScopeMismatch,
                    std::format("transition '{}' starts from {}, but the current scope is {}",
                                seg,
                                kinds_text(t->from),
                                ev::scope_kind_name(cur)));
                return std::nullopt;
            }
            plan.steps.push_back(Step{Step::Kind::Transition, t->code, t->to});
            cur = t->to;
            ++i;
        }
        plan.end = cur;
        if (i == p.segs.size()) {
            plan.term = Terminal::Scope;
            plan.term_pos = at(p, p.segs.size() - 1);
            if ((allowed & kTermScope) == 0) {
                err(plan.term_pos,
                    data::ErrorCode::EventBadArgument,
                    std::format("this path ends in a scope ({}), but {} is expected here",
                                ev::scope_kind_name(cur),
                                expecting));
                return std::nullopt;
            }
            return plan;
        }
        const std::string& name = p.segs[i].text;
        plan.term_pos = at(p, i);
        const std::size_t remaining = p.segs.size() - i;
        if (name == "variable" || name == "global_variable") {
            if (remaining != 2 || p.segs[i + 1].text.empty()) {
                err(plan.term_pos,
                    data::ErrorCode::EventBadArgument,
                    std::format("'{}' needs a name: {}.name", name, name));
                return std::nullopt;
            }
            if ((allowed & kTermVariable) == 0) {
                err(plan.term_pos,
                    data::ErrorCode::EventBadArgument,
                    std::format("a variable is not allowed here, {} is expected", expecting));
                return std::nullopt;
            }
            plan.term = name == "variable" ? Terminal::Variable : Terminal::GlobalVariable;
            plan.var_name = p.segs[i + 1].text;
            return plan;
        }
        const CatalogEntry* value = sh_.catalog->find(NameSpace::Value, name);
        const CatalogEntry* pred = sh_.catalog->find(NameSpace::Predicate, name);
        const CatalogEntry* chosen = value != nullptr ? value : pred;
        if (chosen == nullptr) {
            // A transition of another scope kind is a scope mismatch, not an unknown name.
            if (const CatalogEntry* t = sh_.catalog->find(NameSpace::Transition, name)) {
                err(plan.term_pos,
                    data::ErrorCode::EventScopeMismatch,
                    std::format("transition '{}' starts from {}, but the current scope is {}",
                                name,
                                kinds_text(t->from),
                                ev::scope_kind_name(cur)));
                return std::nullopt;
            }
            std::vector<std::string> known = sh_.catalog->names(NameSpace::Transition);
            for (const std::string& n : sh_.catalog->names(NameSpace::Value)) {
                known.push_back(n);
            }
            for (const std::string& n : sh_.catalog->names(NameSpace::Predicate)) {
                known.push_back(n);
            }
            for (const std::string& n : extra_names) {
                known.push_back(n);
            }
            err(plan.term_pos, data::ErrorCode::EventUnknownName, unknown_text("name", name, known));
            return std::nullopt;
        }
        if (chosen->retired) {
            err(plan.term_pos,
                data::ErrorCode::EventRetired,
                std::format("'{}' is retired: {}", name, chosen->retired_hint));
            return std::nullopt;
        }
        if (!ev::scope_allowed(chosen->from, cur)) {
            err(plan.term_pos,
                data::ErrorCode::EventScopeMismatch,
                std::format(
                    "'{}' works on {}, but the current scope is {}; enter the right scope with a transition",
                    name,
                    kinds_text(chosen->from),
                    ev::scope_kind_name(cur)));
            return std::nullopt;
        }
        plan.entry = chosen;
        plan.term = chosen == value ? Terminal::Value : Terminal::Predicate;
        if (!chosen->suffixes.empty()) {
            if (remaining != 2) {
                err(plan.term_pos,
                    data::ErrorCode::EventBadArgument,
                    std::format("'{}' needs one of: {} (write {}.{})",
                                name,
                                join(chosen->suffixes),
                                name,
                                chosen->suffixes.front()));
                return std::nullopt;
            }
            const auto it = std::ranges::find(chosen->suffixes, p.segs[i + 1].text);
            if (it == chosen->suffixes.end()) {
                err(at(p, i + 1),
                    data::ErrorCode::EventBadArgument,
                    unknown_text(std::format("kind of '{}'", name), p.segs[i + 1].text, chosen->suffixes) +
                        std::format(" (allowed: {})", join(chosen->suffixes)));
                return std::nullopt;
            }
            plan.suffix = static_cast<int>(it - chosen->suffixes.begin());
        } else if (remaining != 1) {
            err(at(p, i + 1),
                data::ErrorCode::EventUnknownEntry,
                std::format("unexpected '{}' after '{}'", p.segs[i + 1].text, name));
            return std::nullopt;
        }
        const unsigned bit = plan.term == Terminal::Value ? kTermValue : kTermPredicate;
        if ((allowed & bit) == 0) {
            err(plan.term_pos,
                data::ErrorCode::EventBadArgument,
                std::format("'{}' is {}, but {} is expected here",
                            name,
                            plan.term == Terminal::Value ? "a value" : "a condition",
                            expecting));
            return std::nullopt;
        }
        return plan;
    }

    static std::string join(const std::vector<std::string>& names) {
        std::string out;
        for (const std::string& n : names) {
            if (!out.empty()) {
                out += ", ";
            }
            out += n;
        }
        return out;
    }

    // Emits the scope steps, the terminal, the way back, and (when a step can find nothing) a fallback
    // reached with the scopes left. With `exit` the fallback jumps there afterwards.
    void emit_guarded(const Plan& plan,
                      const std::function<void()>& terminal,
                      const std::function<void()>& fallback,
                      std::optional<Label> exit) {
        std::vector<Label> empty_labels(1);  // index 0 unused: pushes are numbered from 1
        bool may_be_empty = false;
        int pushes = 0;
        for (const Step& s : plan.steps) {
            switch (s.kind) {
                case Step::Kind::This:
                    break;
                case Step::Kind::Root:
                    ++pushes;
                    ++depth_;
                    empty_labels.push_back(b_.new_label());
                    b_.emit(Op::kScopeRoot);
                    break;
                case Step::Kind::Saved:
                    ++pushes;
                    ++depth_;
                    empty_labels.push_back(b_.new_label());
                    may_be_empty = true;
                    b_.emit_jump(Op::kScopeLoad, empty_labels.back(), s.arg);
                    break;
                case Step::Kind::Transition:
                    ++pushes;
                    ++depth_;
                    empty_labels.push_back(b_.new_label());
                    may_be_empty = true;
                    b_.emit_jump(Op::kScopeEnter, empty_labels.back(), s.arg);
                    break;
            }
            note_depth();
        }
        const ScopeKind saved_cur = cur_;
        cur_ = plan.end;
        terminal();
        cur_ = saved_cur;
        for (int i = 0; i < pushes; ++i) {
            b_.emit(Op::kScopeLeave);
        }
        depth_ -= pushes;
        if (!may_be_empty) {
            return;
        }
        const Label skip = b_.new_label();
        b_.emit_jump(Op::kJmp, skip);
        for (int k = pushes; k >= 2; --k) {
            b_.bind(empty_labels[static_cast<std::size_t>(k)]);
            b_.emit(Op::kScopeLeave);
        }
        b_.bind(empty_labels[1]);
        fallback();
        if (exit) {
            b_.emit_jump(Op::kJmp, *exit);
        }
        b_.bind(skip);
    }

    // ---- triggers ----

    void trigger_and(const std::vector<data::Entry>& entries,
                     const std::function<bool(const data::Entry&)>& skip) {
        std::vector<const data::Entry*> list;
        for (const data::Entry& e : entries) {
            if (!skip || !skip(e)) {
                list.push_back(&e);
            }
        }
        if (list.empty()) {
            b_.push_bool(true);
            return;
        }
        const Label end = b_.new_label();
        for (std::size_t i = 0; i < list.size(); ++i) {
            if (i > 0) {
                b_.emit(Op::kDrop);
            }
            trigger_entry(*list[i]);
            if (i + 1 < list.size()) {
                b_.emit_jump(Op::kJfKeep, end);
            }
        }
        b_.bind(end);
    }

    void trigger_or(const std::vector<data::Entry>& entries) {
        if (entries.empty()) {
            b_.push_bool(false);
            return;
        }
        const Label end = b_.new_label();
        for (std::size_t i = 0; i < entries.size(); ++i) {
            if (i > 0) {
                b_.emit(Op::kDrop);
            }
            trigger_entry(entries[i]);
            if (i + 1 < entries.size()) {
                b_.emit_jump(Op::kJtKeep, end);
            }
        }
        b_.bind(end);
    }

    bool expect_block(const data::Entry& e) {
        if (e.value.kind != data::ValueKind::Block) {
            err(e.value.pos,
                data::ErrorCode::EventBadArgument,
                std::format("'{}' needs a block: {} = {{ ... }}", e.key, e.key));
            return false;
        }
        return true;
    }

    void trigger_entry(const data::Entry& e) {
        source_ = e.pos;
        set_source(e.pos);
        if (e.key_kind != data::ValueKind::Ident) {
            err(e.pos,
                data::ErrorCode::EventUnknownEntry,
                "a condition starts with a name, not a number or a date");
            b_.push_bool(true);
            return;
        }
        const std::string& key = e.key;
        if (key == "and" || key == "or" || key == "not" || key == "nor") {
            if (!expect_block(e)) {
                b_.push_bool(true);
                return;
            }
            if (key == "and") {
                trigger_and(e.value.entries, nullptr);
            } else if (key == "or") {
                trigger_or(e.value.entries);
            } else if (key == "not") {
                trigger_and(e.value.entries, nullptr);
                b_.emit(Op::kNot);
            } else {
                trigger_or(e.value.entries);
                b_.emit(Op::kNot);
            }
            return;
        }
        if (key == "exists") {
            trigger_exists(e);
            return;
        }
        if (key == "random_chance") {
            if (operand(e.value, nullptr, false)) {
                b_.push_int(10000);
                b_.emit(Op::kMul);
            }
            b_.emit(Op::kRandPermille, 0, 0);
            return;
        }
        if (starts_with(key, "any_") || starts_with(key, "all_")) {
            trigger_iterator(e);
            return;
        }
        if (key == "trigger_if" || key == "trigger_else" || key == "trigger_else_if") {
            err(e.pos,
                data::ErrorCode::EventForbidden,
                std::format("'{}' is not available in M1: use or / and / not", key));
            b_.push_bool(true);
            return;
        }
        if (key == "limit" || key == "count" || key == "if" || key == "else" || key == "else_if" ||
            starts_with(key, "count_")) {
            err(e.pos, data::ErrorCode::EventUnknownEntry, std::format("'{}' is not a condition here", key));
            b_.push_bool(true);
            return;
        }
        const PathText path = split_path(key, e.pos);
        const std::optional<Plan> plan =
            analyze(path, kTermScope | kTermValue | kTermPredicate | kTermVariable, "a condition", {});
        if (!plan) {
            b_.push_bool(true);
            return;
        }
        switch (plan->term) {
            case Terminal::Scope:
                trigger_scope_block(e, *plan);
                break;
            case Terminal::Predicate:
                trigger_predicate(e, *plan);
                break;
            case Terminal::Value:
            case Terminal::Variable:
            case Terminal::GlobalVariable:
                trigger_compare(e, *plan);
                break;
        }
    }

    void trigger_exists(const data::Entry& e) {
        const std::optional<PathText> path = path_of_value(e.value);
        std::optional<Plan> plan;
        if (path) {
            plan = analyze(*path, kTermScope, "a transition", {});
        }
        if (!plan) {
            b_.push_bool(true);
            return;
        }
        emit_guarded(*plan, [&] { b_.push_bool(true); }, [&] { b_.push_bool(false); }, std::nullopt);
    }

    void trigger_scope_block(const data::Entry& e, const Plan& plan) {
        if (e.value.kind != data::ValueKind::Block || e.op != data::Op::Eq) {
            err(e.pos,
                data::ErrorCode::EventBadArgument,
                std::format("'{}' is a scope: write {} = {{ conditions }}, or compare one of its values",
                            e.key,
                            e.key));
            b_.push_bool(true);
            return;
        }
        emit_guarded(
            plan, [&] { trigger_and(e.value.entries, nullptr); }, [&] { b_.push_bool(false); }, std::nullopt);
    }

    void trigger_predicate(const data::Entry& e, const Plan& plan) {
        const CatalogEntry& ce = *plan.entry;
        if (e.op != data::Op::Eq) {
            err(e.pos,
                data::ErrorCode::EventBadArgument,
                std::format("expected '=' after '{}', found '{}'", e.key, data::op_text(e.op)));
        }
        const bool negate = e.value.kind == data::ValueKind::Bool && e.value.integer == 0 &&
                            !(!ce.args.empty() && ce.args[0].kind == ArgKind::Bool);
        emit_guarded(
            plan,
            [&] {
                compile_args(ce, e.pos, e.value, nullptr);
                b_.emit(Op::kPred, ce.code, static_cast<std::int32_t>(ce.args.size()));
                if (negate) {
                    b_.emit(Op::kNot);
                }
            },
            [&] { b_.push_bool(false); },
            std::nullopt);
    }

    // `value op number`, `path op number`, `variable.x op number`, `value = { args  value op number }`.
    void trigger_compare(const data::Entry& e, const Plan& plan) {
        const Label end = b_.new_label();
        data::Op op = e.op;
        const data::Value* rhs = &e.value;
        std::vector<const data::Entry*> cmps;
        const bool block_form = e.value.kind == data::ValueKind::Block;
        if (block_form) {
            if (e.op != data::Op::Eq) {
                err(e.pos,
                    data::ErrorCode::EventBadArgument,
                    "a value with arguments is written name = { ... value >= N }");
            }
            if (plan.entry == nullptr) {
                err(e.pos,
                    data::ErrorCode::EventBadArgument,
                    "a variable takes no arguments; compare it directly");
                b_.push_bool(true);
                return;
            }
        }
        emit_guarded(
            plan,
            [&] {
                if (block_form) {
                    compile_args(*plan.entry, e.pos, e.value, &cmps);
                } else if (plan.entry != nullptr) {
                    push_value_args(*plan.entry, e.pos, plan.suffix);
                }
                emit_value_terminal(plan);
            },
            [&] { b_.push_bool(false); },
            end);
        if (block_form) {
            if (cmps.size() != 1) {
                err(e.pos,
                    data::ErrorCode::EventBadArgument,
                    cmps.empty() ? "a value with arguments needs a comparison: value >= N"
                                 : "only one 'value' comparison is allowed in a block");
                b_.emit(Op::kDrop);
                b_.push_bool(true);
                b_.bind(end);
                return;
            }
            op = cmps[0]->op;
            rhs = &cmps[0]->value;
        } else if (e.value.kind == data::ValueKind::Bool) {
            err(e.value.pos,
                data::ErrorCode::EventBadArgument,
                std::format(
                    "'{}' is a number; compare it with a number (for example {} >= 1)", e.key, e.key));
        }
        Abandon abandon{1, end};
        if (!operand(*rhs, &abandon, false)) {
            b_.emit(Op::kDrop);
            b_.push_int(0);
        }
        b_.emit(compare_op(op));
        b_.bind(end);
    }

    // ---- iterators in triggers and accumulators ----

    const CatalogEntry* find_list(const data::Entry& e, std::size_t prefix) {
        const std::string name = e.key.substr(prefix);
        if (is_world_list(name)) {
            err(e.pos,
                data::ErrorCode::EventForbidden,
                std::format("'{}' would walk the whole world; use a list of the current scope "
                            "(living_member, child, sibling, owned_province, ...)",
                            e.key));
            return nullptr;
        }
        const CatalogEntry* list = sh_.catalog->find(NameSpace::List, name);
        if (list == nullptr) {
            err(e.pos,
                data::ErrorCode::EventUnknownName,
                unknown_text("list", name, sh_.catalog->names(NameSpace::List)));
            return nullptr;
        }
        if (list->retired) {
            err(e.pos,
                data::ErrorCode::EventRetired,
                std::format("list '{}' is retired: {}", name, list->retired_hint));
            return nullptr;
        }
        if (!ev::scope_allowed(list->from, cur_)) {
            err(e.pos,
                data::ErrorCode::EventScopeMismatch,
                std::format("list '{}' belongs to {}, but the current scope is {}",
                            name,
                            kinds_text(list->from),
                            ev::scope_kind_name(cur_)));
            return nullptr;
        }
        return list;
    }

    struct IterGuard {
        Impl& self;
        ScopeKind saved_cur;
        explicit IterGuard(Impl& s, ScopeKind inner) : self(s), saved_cur(s.cur_) {
            ++self.iter_nest_;
            ++self.depth_;
            self.cur_ = inner;
            if (self.iter_nest_ > ev::limits::kMaxIterNest) {
                self.err(self.source_,
                         data::ErrorCode::EventLimit,
                         std::format("iterators are nested deeper than {} levels", ev::limits::kMaxIterNest));
            }
            self.note_depth();
        }
        ~IterGuard() {
            --self.iter_nest_;
            --self.depth_;
            self.cur_ = saved_cur;
        }
        IterGuard(const IterGuard&) = delete;
        IterGuard& operator=(const IterGuard&) = delete;
    };

    // Reads the `count >= N` entries of an any_ block; the rest is the condition.
    struct CountSpec {
        bool present = false;
        data::Op op = data::Op::Ge;
        std::int64_t n = 1;
    };

    CountSpec read_count(const std::vector<data::Entry>& entries) {
        CountSpec spec;
        for (const data::Entry& e : entries) {
            if (e.key_kind == data::ValueKind::Ident && e.key == "count") {
                if (e.value.kind != data::ValueKind::Int || e.value.integer < 0 ||
                    e.value.integer > 1000000) {
                    err(e.value.pos,
                        data::ErrorCode::EventBadArgument,
                        "'count' needs a whole number, for example count >= 2");
                } else if (spec.present) {
                    err(e.pos, data::ErrorCode::EventBadArgument, "'count' is written once");
                } else {
                    spec.present = true;
                    spec.op = e.op;
                    spec.n = e.value.integer;
                }
            }
        }
        return spec;
    }

    static bool is_count_entry(const data::Entry& e) {
        return e.key_kind == data::ValueKind::Ident && e.key == "count";
    }

    void trigger_iterator(const data::Entry& e) {
        const bool any = starts_with(e.key, "any_");
        const CatalogEntry* list = find_list(e, 4);
        if (list == nullptr || !expect_block(e)) {
            b_.push_bool(true);
            return;
        }
        const CountSpec count = any ? read_count(e.value.entries) : CountSpec{};
        const bool counting = any && count.present;
        const Label end = b_.new_label();
        const Label out = b_.new_label();
        const Label loop = b_.new_label();
        if (counting) {
            b_.push_int(0);
        }
        {
            IterGuard guard(*this, list->to);
            b_.emit_jump(Op::kIterBegin, end, list->code);
            b_.bind(loop);
            b_.emit_jump(Op::kIterNext, end);
            trigger_and(e.value.entries, is_count_entry);
            if (any && !counting) {
                b_.emit_jump(Op::kJf, loop);
                b_.emit(Op::kIterEnd);
                b_.push_bool(true);
                b_.emit_jump(Op::kJmp, out);
            } else if (counting) {
                b_.emit_jump(Op::kJf, loop);
                b_.push_int(1);
                b_.emit(Op::kAdd);
                b_.emit_jump(Op::kJmp, loop);
            } else {
                const Label bad = b_.new_label();
                b_.emit_jump(Op::kJf, bad);
                b_.emit_jump(Op::kJmp, loop);
                b_.bind(bad);
                b_.emit(Op::kIterEnd);
                b_.push_bool(false);
                b_.emit_jump(Op::kJmp, out);
            }
        }
        b_.bind(end);
        b_.emit(Op::kIterEnd);
        if (counting) {
            b_.push_int(static_cast<std::int32_t>(count.n));
            b_.emit(compare_op(count.op));
        } else {
            b_.push_bool(!any);
        }
        b_.bind(out);
    }

    // ---- operands and accumulators ----

    struct Abandon {
        int drops;
        Label end;
    };

    // Pushes the number the value stands for. With `abandon`, an empty transition drops `drops` values,
    // pushes false and jumps to `end` (the whole comparison is false); otherwise it yields 0.
    bool operand(const data::Value& v, const Abandon* abandon, bool accumulator_top) {
        set_source(v.pos);
        source_ = v.pos;
        switch (v.kind) {
            case data::ValueKind::Int:
            case data::ValueKind::Decimal: {
                const std::optional<core::Fixed64> f = fixed_of(v);
                if (!f) {
                    err(v.pos,
                        data::ErrorCode::EventBadConstant,
                        std::format("the number {} is out of range",
                                    v.text.empty() ? std::to_string(v.integer) : v.text));
                    b_.push_int(0);
                    return false;
                }
                b_.push_fixed(*f);
                if (accumulator_top) {
                    b_.emit(Op::kStep, next_step());
                }
                return true;
            }
            case data::ValueKind::Date: {
                if (!data::is_valid_julian_date(v.year, v.month, v.day)) {
                    err(v.pos,
                        data::ErrorCode::EventBadConstant,
                        std::format("{} does not exist in the Julian calendar", v.text));
                    b_.push_int(0);
                    return false;
                }
                b_.push_int(sim::Date::from_ymd(v.year, v.month, v.day).days());
                return true;
            }
            case data::ValueKind::Ident:
            case data::ValueKind::Ref: {
                const std::optional<PathText> path = path_of_value(v);
                std::optional<Plan> plan;
                if (path) {
                    plan = analyze(*path, kTermValue | kTermVariable, "a number", {});
                }
                if (!plan) {
                    b_.push_int(0);
                    return false;
                }
                emit_operand_path(*plan, v.pos, abandon);
                if (accumulator_top) {
                    b_.emit(Op::kStep, next_step());
                }
                return true;
            }
            case data::ValueKind::Block:
                return operand_block(v, abandon, accumulator_top);
            case data::ValueKind::Bool:
            case data::ValueKind::String:
            case data::ValueKind::List:
                err(v.pos,
                    data::ErrorCode::EventBadArgument,
                    "expected a number, a path to a value, or a block of steps (base, add, multiply, ...)");
                b_.push_int(0);
                return false;
        }
        return false;
    }

    void emit_operand_path(const Plan& plan, data::SourcePos pos, const Abandon* abandon) {
        const auto fallback = [&] {
            if (abandon != nullptr) {
                for (int i = 0; i < abandon->drops; ++i) {
                    b_.emit(Op::kDrop);
                }
                b_.push_bool(false);
            } else {
                b_.push_int(0);
            }
        };
        emit_guarded(
            plan,
            [&] {
                if (plan.entry != nullptr) {
                    push_value_args(*plan.entry, pos, plan.suffix);
                }
                emit_value_terminal(plan);
            },
            fallback,
            abandon != nullptr ? std::optional<Label>(abandon->end) : std::nullopt);
    }

    // LOAD of a catalog value or LOAD_VAR of a variable; its arguments are already on the stack.
    void emit_value_terminal(const Plan& plan) {
        if (plan.term == Terminal::Variable || plan.term == Terminal::GlobalVariable) {
            std::string clash;
            const std::uint64_t key = sh_.names->add(plan.var_name, &clash);
            if (!clash.empty()) {
                err(plan.term_pos,
                    data::ErrorCode::EventKeyCollision,
                    std::format("the names '{}' and '{}' have the same 64-bit key; rename one",
                                plan.var_name,
                                clash));
            }
            const std::uint32_t index = b_.constant(static_cast<std::int64_t>(key));
            b_.emit(Op::kLoadVar,
                    plan.term == Terminal::GlobalVariable ? 1 : 0,
                    static_cast<std::int32_t>(index));
            return;
        }
        const CatalogEntry& ce = *plan.entry;
        const std::size_t argc = ce.args.size() + (ce.suffixes.empty() ? 0 : 1);
        b_.emit(Op::kLoad, ce.code, static_cast<std::int32_t>(argc));
    }

    // The suffix index and the default (or required-missing) arguments of a value used without a block.
    void push_value_args(const CatalogEntry& ce, data::SourcePos pos, int suffix) {
        if (!ce.suffixes.empty()) {
            b_.push_int(suffix);
        }
        for (const ev::ArgSpec& a : ce.args) {
            if (a.required) {
                err(pos,
                    data::ErrorCode::EventBadArgument,
                    std::format("'{}' needs the argument '{}': write {} = {{ {} = ... value >= N }}",
                                ce.name,
                                a.name,
                                ce.name,
                                a.name));
            }
            push_default(a);
        }
    }

    void push_default(const ev::ArgSpec& a) {
        switch (a.kind) {
            case ArgKind::Scope:
                b_.push_int(0);
                break;
            case ArgKind::Name:
            case ArgKind::Ref:
                b_.push_int(0);
                break;
            default:
                b_.push_fixed(a.default_value);
                break;
        }
    }

    std::uint16_t next_step() { return static_cast<std::uint16_t>(++step_code_); }

    bool operand_block(const data::Value& v, const Abandon* abandon, bool accumulator_top) {
        if (v.entries.empty()) {
            err(v.pos,
                data::ErrorCode::EventBadArgument,
                "an empty block; write a number or { base = N  add = ... }");
            b_.push_int(0);
            return false;
        }
        const data::Entry& first = v.entries.front();
        if (first.key_kind == data::ValueKind::Ident) {
            if (is_accumulator_key(first.key)) {
                accumulator_block(v.entries);
                return true;
            }
            if (starts_with(first.key, "count_")) {
                if (v.entries.size() != 1) {
                    err(v.entries[1].pos,
                        data::ErrorCode::EventUnknownEntry,
                        "write the count as its own block: { count_child = { ... } }");
                }
                count_operand(first);
                if (accumulator_top) {
                    b_.emit(Op::kStep, next_step());
                }
                return true;
            }
            if (const CatalogEntry* ce = sh_.catalog->find(NameSpace::Value, first.key)) {
                if (v.entries.size() != 1) {
                    err(v.entries[1].pos,
                        data::ErrorCode::EventUnknownEntry,
                        "a value with arguments is written as its own block: { name = { ... } }");
                }
                value_call_operand(*ce, first, abandon);
                if (accumulator_top) {
                    b_.emit(Op::kStep, next_step());
                }
                return true;
            }
        }
        // Unknown first key: report it with the accumulator keys and the catalog values as candidates.
        std::vector<std::string> known(std::begin(kAccumulatorKeys), std::end(kAccumulatorKeys));
        for (const std::string& n : sh_.catalog->names(NameSpace::Value)) {
            known.push_back(n);
        }
        err(first.pos, data::ErrorCode::EventUnknownEntry, unknown_text("entry", first.key, known));
        b_.push_int(0);
        return false;
    }

    void value_call_operand(const CatalogEntry& ce, const data::Entry& e, const Abandon* abandon) {
        (void)abandon;
        if (!ev::scope_allowed(ce.from, cur_)) {
            err(e.pos,
                data::ErrorCode::EventScopeMismatch,
                std::format("'{}' works on {}, but the current scope is {}",
                            ce.name,
                            kinds_text(ce.from),
                            ev::scope_kind_name(cur_)));
        }
        if (!ce.suffixes.empty()) {
            err(e.pos,
                data::ErrorCode::EventBadArgument,
                std::format("'{}' needs a kind: write {}.{}", ce.name, ce.name, ce.suffixes.front()));
        }
        compile_args(ce, e.pos, e.value, nullptr);
        b_.emit(Op::kLoad, ce.code, static_cast<std::int32_t>(ce.args.size()));
    }

    void count_operand(const data::Entry& e) {
        const CatalogEntry* list = find_list(e, 6);
        if (list == nullptr || !expect_block(e)) {
            b_.push_int(0);
            return;
        }
        const Label end = b_.new_label();
        const Label loop = b_.new_label();
        b_.push_int(0);
        {
            IterGuard guard(*this, list->to);
            b_.emit_jump(Op::kIterBegin, end, list->code);
            b_.bind(loop);
            b_.emit_jump(Op::kIterNext, end);
            const data::Entry* limit = find_entry(e.value.entries, "limit");
            for (const data::Entry& inner : e.value.entries) {
                if (&inner != limit) {
                    err(inner.pos,
                        data::ErrorCode::EventUnknownEntry,
                        std::format("unknown entry '{}' in a count (only 'limit' is allowed)", inner.key));
                }
            }
            if (limit != nullptr && limit->value.kind == data::ValueKind::Block) {
                trigger_and(limit->value.entries, nullptr);
            } else {
                b_.push_bool(true);
            }
            b_.emit_jump(Op::kJf, loop);
            b_.push_int(1);
            b_.emit(Op::kAdd);
            b_.emit_jump(Op::kJmp, loop);
        }
        b_.bind(end);
        b_.emit(Op::kIterEnd);
    }

    // The accumulator: entries run in order over one Fixed64 on the stack.
    void accumulator_block(const std::vector<data::Entry>& entries) {
        bool started = false;
        for (const data::Entry& e : entries) {
            source_ = e.pos;
            set_source(e.pos);
            if (e.key_kind != data::ValueKind::Ident) {
                err(e.pos, data::ErrorCode::EventUnknownEntry, "a step starts with a name (base, add, ...)");
                continue;
            }
            if (e.key == "base" || e.key == "value") {
                if (started) {
                    err(e.pos,
                        data::ErrorCode::EventUnknownEntry,
                        std::format("'{}' is allowed only as the first entry of a block", e.key));
                    continue;
                }
                started = true;
                operand(e.value, nullptr, true);
                continue;
            }
            if (!started) {
                b_.push_int(0);
                started = true;
            }
            accumulator_step(e);
        }
        if (!started) {
            b_.push_int(0);
        }
    }

    void accumulator_step(const data::Entry& e) {
        const std::string& key = e.key;
        if (key == "if") {
            if (!expect_block(e)) {
                return;
            }
            const data::Entry* limit = find_entry(e.value.entries, "limit");
            if (limit == nullptr || limit->value.kind != data::ValueKind::Block) {
                err(e.pos, data::ErrorCode::EventUnknownEntry, "'if' needs a limit = { conditions } block");
                return;
            }
            const Label skip = b_.new_label();
            trigger_and(limit->value.entries, nullptr);
            b_.emit_jump(Op::kJf, skip);
            for (const data::Entry& inner : e.value.entries) {
                if (&inner == limit) {
                    continue;
                }
                source_ = inner.pos;
                set_source(inner.pos);
                if (inner.key_kind == data::ValueKind::Ident &&
                    (inner.key == "base" || inner.key == "value")) {
                    err(inner.pos,
                        data::ErrorCode::EventUnknownEntry,
                        "'base' and 'value' are not allowed inside 'if'; use add");
                    continue;
                }
                if (inner.key_kind != data::ValueKind::Ident) {
                    err(inner.pos,
                        data::ErrorCode::EventUnknownEntry,
                        "a step starts with a name (add, subtract, ...)");
                    continue;
                }
                accumulator_step(inner);
            }
            b_.bind(skip);
            return;
        }
        if (key == "round") {
            if (e.value.kind != data::ValueKind::Bool || e.value.integer == 0) {
                err(e.value.pos, data::ErrorCode::EventBadArgument, "write round = yes");
            }
            b_.emit(Op::kTrunc);
            b_.emit(Op::kStep, next_step());
            return;
        }
        if (key == "floor" || key == "ceiling") {
            err(e.pos,
                data::ErrorCode::EventForbidden,
                std::format(
                    "'{}' is not available in M1 (the machine truncates toward zero: use round = yes)", key));
            return;
        }
        Op op = Op::kNop;
        if (key == "add") {
            op = Op::kAdd;
        } else if (key == "subtract") {
            op = Op::kSub;
        } else if (key == "multiply") {
            op = Op::kMul;
        } else if (key == "divide") {
            op = Op::kDiv;
        } else if (key == "min") {
            op = Op::kMin;
        } else if (key == "max") {
            op = Op::kMax;
        } else {
            std::vector<std::string> known(std::begin(kAccumulatorKeys), std::end(kAccumulatorKeys));
            err(e.pos, data::ErrorCode::EventUnknownEntry, unknown_text("step", key, known));
            return;
        }
        if (op == Op::kDiv) {
            if (const auto f = fixed_of(e.value); f && f->raw() == 0) {
                err(e.value.pos, data::ErrorCode::EventBadConstant, "division by the constant 0");
            }
        }
        if (operand(e.value, nullptr, false)) {
            b_.emit(op);
        } else {
            b_.emit(Op::kDrop);
            b_.emit(Op::kDrop);
            b_.push_int(0);
        }
        b_.emit(Op::kStep, next_step());
    }

    // ---- arguments ----

    // Pushes the arguments of a catalog entry in signature order. `v` is the right-hand side of
    // `name = v`: a block of named arguments, or one scalar for the main (first) argument. In a value
    // block, `value` entries are comparisons, handed back through `value_cmps`.
    void compile_args(const CatalogEntry& ce,
                      data::SourcePos pos,
                      const data::Value& v,
                      std::vector<const data::Entry*>* value_cmps) {
        const std::size_t n = ce.args.size();
        if (v.kind == data::ValueKind::Block) {
            std::vector<const data::Entry*> by_arg(n, nullptr);
            std::vector<std::string> names;
            for (const ev::ArgSpec& a : ce.args) {
                names.push_back(a.name);
            }
            for (const data::Entry& inner : v.entries) {
                if (value_cmps != nullptr && inner.key_kind == data::ValueKind::Ident &&
                    inner.key == "value") {
                    value_cmps->push_back(&inner);
                    continue;
                }
                std::size_t index = n;
                for (std::size_t i = 0; i < n; ++i) {
                    if (inner.key_kind == data::ValueKind::Ident && ce.args[i].name == inner.key) {
                        index = i;
                    }
                }
                if (index == n) {
                    err(inner.pos,
                        data::ErrorCode::EventUnknownEntry,
                        unknown_text(std::format("argument of '{}'", ce.name), inner.key, names) +
                            (n == 0 ? std::string(" ('") + ce.name + "' takes no arguments)"
                                    : std::string()));
                    continue;
                }
                if (by_arg[index] != nullptr) {
                    err(inner.pos,
                        data::ErrorCode::EventBadArgument,
                        std::format("the argument '{}' is written twice", inner.key));
                    continue;
                }
                if (inner.op != data::Op::Eq) {
                    err(inner.pos,
                        data::ErrorCode::EventBadArgument,
                        std::format(
                            "expected '=' after '{}', found '{}'", inner.key, data::op_text(inner.op)));
                }
                by_arg[index] = &inner;
            }
            if (!ce.suffixes.empty()) {
                b_.push_int(0);
            }
            for (std::size_t i = 0; i < n; ++i) {
                if (by_arg[i] != nullptr) {
                    compile_arg(ce.args[i], by_arg[i]->value);
                } else {
                    if (ce.args[i].required) {
                        err(pos,
                            data::ErrorCode::EventBadArgument,
                            std::format("'{}' needs the argument '{}'", ce.name, ce.args[i].name));
                    }
                    push_default(ce.args[i]);
                }
            }
            return;
        }
        if (!ce.suffixes.empty()) {
            b_.push_int(0);
        }
        const bool bool_flag =
            v.kind == data::ValueKind::Bool && (n == 0 || ce.args[0].kind != ArgKind::Bool);
        if (n == 0 && !bool_flag) {
            err(v.pos,
                data::ErrorCode::EventBadArgument,
                std::format("'{}' takes no arguments: write {} = yes", ce.name, ce.name));
            return;
        }
        for (std::size_t i = 0; i < n; ++i) {
            if (i == 0 && !bool_flag) {
                compile_arg(ce.args[0], v);
                continue;
            }
            if (ce.args[i].required) {
                err(pos,
                    data::ErrorCode::EventBadArgument,
                    std::format("'{}' needs the argument '{}': write {} = {{ ... }}",
                                ce.name,
                                ce.args[i].name,
                                ce.name));
            }
            push_default(ce.args[i]);
        }
    }

    void compile_arg(const ev::ArgSpec& a, const data::Value& v) {
        source_ = v.pos;
        set_source(v.pos);
        switch (a.kind) {
            case ArgKind::Number:
                if (!operand(v, nullptr, false)) {
                    // operand() already pushed a placeholder
                }
                return;
            case ArgKind::Bool:
                if (v.kind != data::ValueKind::Bool) {
                    err(v.pos,
                        data::ErrorCode::EventBadArgument,
                        std::format("'{}' needs yes or no", a.name));
                    b_.push_bool(false);
                    return;
                }
                b_.push_bool(v.integer != 0);
                return;
            case ArgKind::Scope:
                compile_scope_arg(a, v);
                return;
            case ArgKind::Name:
                compile_name_arg(a, v);
                return;
            case ArgKind::Ref:
                compile_ref_arg(a, v);
                return;
            case ArgKind::Enum: {
                if (v.kind != data::ValueKind::Ident) {
                    err(v.pos,
                        data::ErrorCode::EventBadArgument,
                        std::format("'{}' needs one of: {}", a.name, join(a.enum_values)));
                    b_.push_int(0);
                    return;
                }
                const auto it = std::ranges::find(a.enum_values, v.text);
                if (it == a.enum_values.end()) {
                    err(v.pos,
                        data::ErrorCode::EventBadArgument,
                        unknown_text(std::format("value of '{}'", a.name), v.text, a.enum_values) +
                            std::format(" (allowed: {})", join(a.enum_values)));
                    b_.push_int(0);
                    return;
                }
                b_.push_int(static_cast<std::int32_t>(it - a.enum_values.begin()));
                return;
            }
            case ArgKind::Duration: {
                const std::optional<std::int64_t> days = parse_duration(v, *sh_.settings, diags_);
                if (!days) {
                    ++errors_;
                    b_.push_int(0);
                    return;
                }
                b_.push_int(static_cast<std::int32_t>(
                    std::min<std::int64_t>(*days, std::numeric_limits<std::int32_t>::max())));
                return;
            }
            case ArgKind::Date:
                if (v.kind != data::ValueKind::Date) {
                    err(v.pos,
                        data::ErrorCode::EventBadArgument,
                        std::format("'{}' needs a date like 1703.05.16", a.name));
                    b_.push_int(0);
                    return;
                }
                if (!data::is_valid_julian_date(v.year, v.month, v.day)) {
                    err(v.pos,
                        data::ErrorCode::EventBadConstant,
                        std::format("{} does not exist in the Julian calendar", v.text));
                    b_.push_int(0);
                    return;
                }
                b_.push_int(sim::Date::from_ymd(v.year, v.month, v.day).days());
                return;
        }
    }

    void compile_scope_arg(const ev::ArgSpec& a, const data::Value& v) {
        const std::optional<PathText> path = path_of_value(v);
        std::optional<Plan> plan;
        if (path) {
            plan = analyze(*path, kTermScope, std::format("a scope for '{}'", a.name), {});
        }
        if (!plan) {
            b_.push_int(0);
            return;
        }
        if (a.scope_kind != ScopeKind::None && plan->end != a.scope_kind) {
            err(v.pos,
                data::ErrorCode::EventScopeMismatch,
                std::format("'{}' needs a {} scope, but this path leads to {}",
                            a.name,
                            ev::scope_kind_name(a.scope_kind),
                            ev::scope_kind_name(plan->end)));
        }
        emit_guarded(*plan, [&] { b_.emit(Op::kPushScope); }, [&] { b_.push_int(0); }, std::nullopt);
    }

    void compile_name_arg(const ev::ArgSpec& a, const data::Value& v) {
        if (v.kind != data::ValueKind::Ident && v.kind != data::ValueKind::String) {
            err(v.pos,
                data::ErrorCode::EventForbidden,
                std::format(
                    "'{}' must be a literal name: a computed name of a flag or a variable is not allowed",
                    a.name));
            b_.push_int(0);
            return;
        }
        const std::string& text = v.text;
        if (a.ref_kind == "event") {
            if (!sh_.event_ids->contains(text)) {
                std::vector<std::string> known(sh_.event_ids->begin(), sh_.event_ids->end());
                err(v.pos, data::ErrorCode::EventUnresolvedReference, unknown_text("event", text, known));
                b_.push_int(0);
                return;
            }
            sh_.event_refs->insert(text);
        } else if (a.ref_kind == "event_modifier") {
            if (!sh_.modifier_ids->contains(text)) {
                std::vector<std::string> known(sh_.modifier_ids->begin(), sh_.modifier_ids->end());
                err(v.pos,
                    data::ErrorCode::EventUnresolvedReference,
                    unknown_text("event modifier", text, known));
                b_.push_int(0);
                return;
            }
        } else if (a.ref_kind == "pool") {
            if (!sh_.pools->contains(text)) {
                std::vector<std::string> known(sh_.pools->begin(), sh_.pools->end());
                err(v.pos, data::ErrorCode::EventUnresolvedReference, unknown_text("pool", text, known));
                b_.push_int(0);
                return;
            }
        }
        std::string clash;
        const std::uint64_t key = sh_.names->add(text, &clash);
        if (!clash.empty()) {
            err(v.pos,
                data::ErrorCode::EventKeyCollision,
                std::format("the names '{}' and '{}' have the same 64-bit key; rename one", text, clash));
        }
        b_.emit(Op::kPushConst, 0, static_cast<std::int32_t>(b_.constant(static_cast<std::int64_t>(key))));
    }

    void compile_ref_arg(const ev::ArgSpec& a, const data::Value& v) {
        if (v.kind != data::ValueKind::Ident && v.kind != data::ValueKind::String) {
            err(v.pos,
                data::ErrorCode::EventBadArgument,
                std::format("'{}' needs the id of a {}", a.name, a.ref_kind));
            b_.push_int(0);
            return;
        }
        const data::ContentTable* table = sh_.registry->table(a.ref_kind);
        const data::ContentObject* object = table != nullptr ? table->find(v.text) : nullptr;
        if (object == nullptr) {
            std::vector<std::string> known;
            if (table != nullptr) {
                for (const data::ContentObject& o : table->objects()) {
                    known.push_back(o.object.id);
                }
            }
            err(v.pos, data::ErrorCode::EventUnresolvedReference, unknown_text(a.ref_kind, v.text, known));
            b_.push_int(0);
            return;
        }
        b_.push_int(static_cast<std::int32_t>(object->id.index()));
    }

    // ---- effects ----

    void effect_block(const std::vector<data::Entry>& entries) {
        for (std::size_t i = 0; i < entries.size();) {
            i = effect_entry(entries, i);
        }
    }

    std::size_t effect_entry(const std::vector<data::Entry>& entries, std::size_t i) {
        const data::Entry& e = entries[i];
        source_ = e.pos;
        set_source(e.pos);
        if (e.key_kind != data::ValueKind::Ident) {
            err(e.pos,
                data::ErrorCode::EventUnknownEntry,
                "an effect starts with a name, not a number or a date");
            return i + 1;
        }
        const std::string& key = e.key;
        if (key == "if") {
            return effect_if(entries, i);
        }
        if (key == "else_if" || key == "else") {
            err(e.pos, data::ErrorCode::EventUnknownEntry, std::format("'{}' without a preceding 'if'", key));
            return i + 1;
        }
        if (key == "limit") {
            err(e.pos,
                data::ErrorCode::EventUnknownEntry,
                "'limit' belongs inside if, every_, random_ or ordered_ blocks");
            return i + 1;
        }
        if (key == "random_list") {
            effect_random_list(e);
            return i + 1;
        }
        if (key == "random") {
            effect_random(e);
            return i + 1;
        }
        if (key == "hidden_effect") {
            if (expect_block(e)) {
                b_.emit(Op::kHide, 1);
                effect_block(e.value.entries);
                b_.emit(Op::kHide, 0);
            }
            return i + 1;
        }
        if (key == "save_scope_as") {
            effect_save_scope(e);
            return i + 1;
        }
        if (starts_with(key, "every_") || starts_with(key, "random_") || starts_with(key, "ordered_")) {
            effect_iterator(e);
            return i + 1;
        }
        const CatalogEntry* ce = sh_.catalog->find(NameSpace::Effect, key);
        if (ce == nullptr) {
            ce = sh_.catalog->find(NameSpace::StateOp, key);
        }
        if (ce != nullptr) {
            effect_call(*ce, e);
            return i + 1;
        }
        // A scope block: owner = { ... }, scope:x = { ... }, root.owner = { ... }.
        std::vector<std::string> extra = sh_.catalog->names(NameSpace::Effect);
        for (const std::string& n : sh_.catalog->names(NameSpace::StateOp)) {
            extra.push_back(n);
        }
        const PathText path = split_path(key, e.pos);
        const std::optional<Plan> plan = analyze(path, kTermScope, "an effect", extra);
        if (!plan) {
            return i + 1;
        }
        if (e.value.kind != data::ValueKind::Block || e.op != data::Op::Eq) {
            err(e.pos,
                data::ErrorCode::EventBadArgument,
                std::format("'{}' is a scope: write {} = {{ effects }}", key, key));
            return i + 1;
        }
        emit_guarded(*plan, [&] { effect_block(e.value.entries); }, [] {}, std::nullopt);
        return i + 1;
    }

    void effect_call(const CatalogEntry& ce, const data::Entry& e) {
        if (ce.retired) {
            err(e.pos,
                data::ErrorCode::EventRetired,
                std::format("effect '{}' is retired: {}", ce.name, ce.retired_hint));
            return;
        }
        if (!ev::scope_allowed(ce.from, cur_)) {
            err(e.pos,
                data::ErrorCode::EventScopeMismatch,
                std::format("effect '{}' works on {}, but the current scope is {}; enter the right scope "
                            "with a transition",
                            ce.name,
                            kinds_text(ce.from),
                            ev::scope_kind_name(cur_)));
        }
        if (e.op != data::Op::Eq) {
            err(e.pos,
                data::ErrorCode::EventBadArgument,
                std::format("expected '=' after '{}', found '{}'", e.key, data::op_text(e.op)));
        }
        compile_args(ce, e.pos, e.value, nullptr);
        const std::size_t argc = ce.args.size() + (ce.suffixes.empty() ? 0 : 1);
        b_.emit(
            ce.space == NameSpace::Effect ? Op::kEmit : Op::kState, ce.code, static_cast<std::int32_t>(argc));
    }

    void effect_save_scope(const data::Entry& e) {
        if (e.value.kind != data::ValueKind::Ident && e.value.kind != data::ValueKind::String) {
            err(e.value.pos,
                data::ErrorCode::EventBadArgument,
                "save_scope_as needs a name: save_scope_as = governor");
            return;
        }
        const std::string& name = e.value.text;
        int slot = saved_slot(name);
        if (slot >= 0) {
            if (saved_.slots[static_cast<std::size_t>(slot)].kind != cur_) {
                err(e.value.pos,
                    data::ErrorCode::EventScopeMismatch,
                    std::format("'{}' is already saved as a {} scope, but here the scope is {}",
                                name,
                                ev::scope_kind_name(saved_.slots[static_cast<std::size_t>(slot)].kind),
                                ev::scope_kind_name(cur_)));
                return;
            }
        } else {
            if (saved_.slots.size() >= ev::limits::kMaxSavedScopes) {
                err(e.value.pos,
                    data::ErrorCode::EventLimit,
                    std::format("an event saves at most {} scopes", ev::limits::kMaxSavedScopes));
                return;
            }
            saved_.slots.push_back(SavedScope{name, cur_});
            slot = static_cast<int>(saved_.slots.size() - 1);
        }
        b_.emit(Op::kScopeSave, static_cast<std::uint16_t>(slot));
    }

    std::size_t effect_if(const std::vector<data::Entry>& entries, std::size_t i) {
        const Label end = b_.new_label();
        std::size_t next = i;
        bool first = true;
        while (next < entries.size()) {
            const data::Entry& e = entries[next];
            const bool is_else = e.key_kind == data::ValueKind::Ident && e.key == "else";
            const bool is_if =
                e.key_kind == data::ValueKind::Ident && (first ? e.key == "if" : e.key == "else_if");
            if (!is_if && !(is_else && !first)) {
                break;
            }
            first = false;
            source_ = e.pos;
            set_source(e.pos);
            if (!expect_block(e)) {
                ++next;
                if (is_else) {
                    break;
                }
                continue;
            }
            const data::Entry* limit = find_entry(e.value.entries, "limit");
            if (is_else) {
                for (const data::Entry& inner : e.value.entries) {
                    if (&inner == limit) {
                        err(inner.pos,
                            data::ErrorCode::EventUnknownEntry,
                            "'else' has no limit; use else_if");
                    }
                }
                effect_block(e.value.entries);
                ++next;
                break;
            }
            if (limit == nullptr || limit->value.kind != data::ValueKind::Block) {
                err(e.pos,
                    data::ErrorCode::EventUnknownEntry,
                    std::format("'{}' needs a limit = {{ conditions }} block", e.key));
                ++next;
                continue;
            }
            const Label otherwise = b_.new_label();
            trigger_and(limit->value.entries, nullptr);
            b_.emit_jump(Op::kJf, otherwise);
            effect_block_skipping(e.value.entries, limit);
            b_.emit_jump(Op::kJmp, end);
            b_.bind(otherwise);
            ++next;
        }
        b_.bind(end);
        return next;
    }

    void effect_block_skipping(const std::vector<data::Entry>& entries, const data::Entry* skipped) {
        for (std::size_t i = 0; i < entries.size();) {
            if (&entries[i] == skipped) {
                ++i;
                continue;
            }
            i = effect_entry(entries, i);
        }
    }

    void effect_random(const data::Entry& e) {
        if (!expect_block(e)) {
            return;
        }
        const data::Entry* chance = find_entry(e.value.entries, "chance");
        if (chance == nullptr) {
            err(e.pos, data::ErrorCode::EventUnknownEntry, "'random' needs chance = N (percent)");
            return;
        }
        if (operand(chance->value, nullptr, false)) {
            b_.push_int(10000);
            b_.emit(Op::kMul);
        }
        const Label skip = b_.new_label();
        b_.emit(Op::kRandPermille, 0, 1);
        b_.emit_jump(Op::kJf, skip);
        effect_block_skipping(e.value.entries, chance);
        b_.emit(Op::kBranchEnd);
        b_.bind(skip);
    }

    void effect_random_list(const data::Entry& e) {
        if (!expect_block(e)) {
            return;
        }
        const auto& branches = e.value.entries;
        if (branches.empty() || branches.size() > ev::limits::kMaxBranches) {
            err(e.pos,
                branches.empty() ? data::ErrorCode::EventBadArgument : data::ErrorCode::EventLimit,
                branches.empty()
                    ? std::string("random_list needs branches: weight = { effects }")
                    : std::format("random_list has at most {} branches", ev::limits::kMaxBranches));
            return;
        }
        std::vector<Label> bodies;
        for (const data::Entry& b : branches) {
            (void)b;
            bodies.push_back(b_.new_label());
        }
        const Label join = b_.new_label();
        // The weights, one per branch.
        for (const data::Entry& b : branches) {
            source_ = b.pos;
            set_source(b.pos);
            core::Fixed64 weight;
            if (b.key_kind != data::ValueKind::Int) {
                err(b.pos,
                    data::ErrorCode::EventUnknownEntry,
                    "a branch of random_list starts with its weight, for example 30 = { ... }");
                b_.push_int(0);
                continue;
            }
            std::int64_t number = 0;
            const auto parsed = std::from_chars(b.key.data(), b.key.data() + b.key.size(), number);
            if (parsed.ec != std::errc() || !data::decimal_to_fixed(number, 0, weight)) {
                err(b.pos, data::ErrorCode::EventBadConstant, "the weight is out of range");
            }
            b_.push_fixed(weight);
            if (b.value.kind != data::ValueKind::Block) {
                continue;
            }
            for (const data::Entry& inner : b.value.entries) {
                if (inner.key_kind == data::ValueKind::Ident && inner.key == "modifier") {
                    weight_modifier(inner);
                }
            }
        }
        const auto count = static_cast<std::uint16_t>(branches.size());
        b_.emit(Op::kRandPick, count, 0);
        b_.emit(Op::kJTable, count, 0);
        for (const Label body : bodies) {
            b_.emit_jump(Op::kJmp, body);
        }
        b_.emit_jump(Op::kJmp, join);
        for (std::size_t i = 0; i < branches.size(); ++i) {
            b_.bind(bodies[i]);
            if (branches[i].value.kind == data::ValueKind::Block) {
                for (std::size_t k = 0; k < branches[i].value.entries.size();) {
                    const data::Entry& inner = branches[i].value.entries[k];
                    if (inner.key_kind == data::ValueKind::Ident && inner.key == "modifier") {
                        ++k;
                        continue;
                    }
                    k = effect_entry(branches[i].value.entries, k);
                }
            } else {
                err(branches[i].value.pos,
                    data::ErrorCode::EventBadArgument,
                    "a branch needs a block of effects");
            }
            b_.emit(Op::kBranchEnd);
            b_.emit_jump(Op::kJmp, join);
        }
        b_.bind(join);
    }

    // modifier = { factor = 2  trigger = { ... } } multiplies the weight on the stack when the trigger holds.
    void weight_modifier(const data::Entry& m) {
        if (!expect_block(m)) {
            return;
        }
        const data::Entry* factor = find_entry(m.value.entries, "factor");
        const data::Entry* trig = find_entry(m.value.entries, "trigger");
        if (factor == nullptr || trig == nullptr || trig->value.kind != data::ValueKind::Block) {
            err(m.pos,
                data::ErrorCode::EventUnknownEntry,
                "a weight modifier needs factor = N and trigger = { ... }");
            return;
        }
        const Label skip = b_.new_label();
        trigger_and(trig->value.entries, nullptr);
        b_.emit_jump(Op::kJf, skip);
        if (operand(factor->value, nullptr, false)) {
            b_.emit(Op::kMul);
        } else {
            b_.emit(Op::kDrop);
        }
        b_.bind(skip);
    }

    void effect_iterator(const data::Entry& e) {
        const bool every = starts_with(e.key, "every_");
        const bool random = starts_with(e.key, "random_");
        const std::size_t prefix = every ? 6 : (random ? 7 : 8);
        const CatalogEntry* list = find_list(e, prefix);
        if (list == nullptr || !expect_block(e)) {
            return;
        }
        const auto& inner = e.value.entries;
        const data::Entry* limit = find_entry(inner, "limit");
        const data::Entry* weight = random ? find_entry(inner, "weight") : nullptr;
        const data::Entry* order_by = every || random ? nullptr : find_entry(inner, "order_by");
        const data::Entry* max_entry = every || random ? nullptr : find_entry(inner, "max");
        if (!every && !random && order_by == nullptr) {
            err(e.pos, data::ErrorCode::EventUnknownEntry, std::format("'{}' needs order_by = value", e.key));
            return;
        }
        std::uint16_t order_code = 0;
        std::int32_t take = 1;
        if (order_by != nullptr) {
            const CatalogEntry* value = order_by->value.kind == data::ValueKind::Ident
                                            ? sh_.catalog->find(NameSpace::Value, order_by->value.text)
                                            : nullptr;
            if (value == nullptr) {
                err(order_by->value.pos,
                    data::ErrorCode::EventUnknownName,
                    unknown_text("value", order_by->value.text, sh_.catalog->names(NameSpace::Value)));
                return;
            }
            if (!value->args.empty() || !value->suffixes.empty() ||
                !ev::scope_allowed(value->from, list->to)) {
                err(order_by->value.pos,
                    data::ErrorCode::EventBadArgument,
                    std::format("'{}' cannot order {} elements (it needs arguments or another scope)",
                                value->name,
                                ev::scope_kind_name(list->to)));
                return;
            }
            order_code = value->code;
        }
        if (max_entry != nullptr) {
            if (max_entry->value.kind != data::ValueKind::Int || max_entry->value.integer < 1 ||
                max_entry->value.integer > static_cast<std::int64_t>(ev::limits::kMaxFanout)) {
                err(max_entry->value.pos,
                    data::ErrorCode::EventBadArgument,
                    "'max' needs a whole number from 1 to 256");
            } else {
                take = static_cast<std::int32_t>(max_entry->value.integer);
            }
        }
        const auto is_meta = [&](const data::Entry& x) {
            return &x == limit || &x == weight || &x == order_by || &x == max_entry;
        };
        const Label end = b_.new_label();
        {
            IterGuard guard(*this, list->to);
            b_.emit_jump(Op::kIterBegin, end, list->code);
            if (every) {
                const Label loop = b_.new_label();
                b_.bind(loop);
                b_.emit_jump(Op::kIterNext, end);
                if (limit != nullptr && limit->value.kind == data::ValueKind::Block) {
                    trigger_and(limit->value.entries, nullptr);
                    b_.emit_jump(Op::kJf, loop);
                }
                for (std::size_t i = 0; i < inner.size();) {
                    i = is_meta(inner[i]) ? i + 1 : effect_entry(inner, i);
                }
                b_.emit_jump(Op::kJmp, loop);
            } else {
                const bool filtering = limit != nullptr || weight != nullptr;
                if (filtering) {
                    const Label filter = b_.new_label();
                    const Label filtered = b_.new_label();
                    b_.bind(filter);
                    b_.emit_jump(Op::kIterNext, filtered);
                    if (limit != nullptr && limit->value.kind == data::ValueKind::Block) {
                        trigger_and(limit->value.entries, nullptr);
                        b_.emit(Op::kIterKeep);
                    }
                    if (weight != nullptr) {
                        operand(weight->value, nullptr, false);
                        b_.emit(Op::kIterWeight);
                    }
                    b_.emit_jump(Op::kJmp, filter);
                    b_.bind(filtered);
                    if (limit != nullptr) {
                        b_.emit(Op::kIterRewind);
                    }
                }
                if (random) {
                    b_.emit(Op::kIterPick);
                    b_.emit_jump(Op::kIterNext, end);
                    for (std::size_t i = 0; i < inner.size();) {
                        i = is_meta(inner[i]) ? i + 1 : effect_entry(inner, i);
                    }
                } else {
                    const Label loop = b_.new_label();
                    b_.emit(Op::kIterOrder, order_code);
                    b_.emit(Op::kIterTake, 0, take);
                    b_.bind(loop);
                    b_.emit_jump(Op::kIterNext, end);
                    for (std::size_t i = 0; i < inner.size();) {
                        i = is_meta(inner[i]) ? i + 1 : effect_entry(inner, i);
                    }
                    b_.emit_jump(Op::kJmp, loop);
                }
            }
        }
        b_.bind(end);
        b_.emit(Op::kIterEnd);
    }

    // ---- state ----

    CompileShared sh_;
    std::vector<Diag>& diags_;
    std::uint32_t file_;
    ScopeKind root_;
    ScopeKind cur_;
    SavedScopes saved_;
    ev::ProgramBuilder b_;
    std::size_t errors_ = 0;
    std::size_t depth_ = 1;
    std::size_t iter_nest_ = 0;
    bool depth_reported_ = false;
    std::uint16_t step_code_ = 0;
    data::SourcePos source_;
};

RulesCompiler::RulesCompiler(const CompileShared& shared,
                             std::vector<Diag>& diags,
                             std::uint32_t file,
                             ScopeKind root,
                             SavedScopes saved)
    : impl_(std::make_unique<Impl>(shared, diags, file, root, std::move(saved))) {}

RulesCompiler::~RulesCompiler() = default;

std::optional<ev::Program> RulesCompiler::trigger(const data::Value& block) {
    return impl_->trigger(block);
}
std::optional<ev::Program> RulesCompiler::accumulator(const data::Value& value, std::uint16_t step_base) {
    return impl_->accumulator(value, step_base);
}
std::optional<ev::Program> RulesCompiler::effect(const data::Value& block) {
    return impl_->effect(block);
}
std::optional<ev::Program> RulesCompiler::scope_value(const data::Value& value) {
    return impl_->scope_value(value);
}
const SavedScopes& RulesCompiler::saved() const noexcept {
    return impl_->saved();
}

}  // namespace pat::content::events
