#pragma once

#include "pat/core/rng.hpp"
#include "pat/sim/economy/breakdown.hpp"
#include "pat/sim/events/program.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

// The virtual machine of the event engine (E11-T04, docs/dev/event-engine.md sections 7.1..7.6).
//
// The machine is decoupled from the world and from the event state tables by the VmHost interface:
// everything it reads (scope transitions, catalog values, predicates, lists, variables) goes
// through the host, and the only thing it consumes is the host's random stream. Everything it
// "writes" (effects over foreign state = future commands, operations over the state of the events
// themselves) goes into buffers of the RunResult: the machine never mutates anything. The caller
// commits the buffers only when the run succeeded; on a limit hit or an ABORT both buffers are
// empty (rollback), the draws already taken from the random stream stay spent (deterministic).
//
// Three modes (section 7.5): Run (the game), Trace (no draws, no effects; leaves are reported to a
// Tracer: the reason a trigger is false), Preview (no draws, no buffers; effect lines and groups
// "chance / branch / for each" go to a PreviewSink: the automatic tooltip). One evaluator serves
// all three, so what is shown is what is applied (section 13.4).

namespace pat::sim::events {

enum class Mode : std::uint8_t { Run, Trace, Preview };

/// What a program is: decides the default instruction budget and whether it may use EMIT/STATE.
enum class Kind : std::uint8_t {
    Trigger,  // pure: a boolean on the stack at the end
    Value,    // pure: an accumulator (ai_chance, weight, chance)
    Effect,   // may emit commands and state operations
};

enum class AbortReason : std::uint16_t {
    None = 0,
    Instr = 1,       // instruction budget exhausted
    Stack = 2,       // value stack overflow
    ScopeDepth = 3,  // scope stack overflow
    IterNest = 4,    // iterators nested deeper than kMaxIterNest
    Commands = 5,    // more than max_commands effects in one run
    StateOps = 6,    // more than max_state_ops operations in one run
    BadProgram = 7,  // malformed program: bad operand, stack underflow, jump out of range, ...
    Explicit = 8,    // the ABORT instruction; RunResult::abort_detail is its operand
    ForkDepth = 9,   // too many open random groups in Preview
};

[[nodiscard]] const char* abort_reason_name(AbortReason reason) noexcept;

// ---- effect buffers -------------------------------------------------------------------------------

/// A buffer of operations (effects over foreign state, or operations over event state): code, the
/// scope they apply to and the arguments in the order they were pushed. Built in program order;
/// byte-comparable between runs (RunResult::to_bytes).
class OpBuffer {
public:
    struct Entry {
        std::uint16_t code = 0;
        std::uint16_t argc = 0;
        std::uint32_t first = 0;  // index of the first argument in args()
        Scope target;

        [[nodiscard]] friend constexpr bool operator==(const Entry&, const Entry&) noexcept = default;
    };

    void add(std::uint16_t code, Scope target, std::span<const Word> args);
    void clear() noexcept;

    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
    [[nodiscard]] bool empty() const noexcept { return entries_.empty(); }
    [[nodiscard]] const Entry& entry(std::size_t i) const noexcept { return entries_[i]; }
    [[nodiscard]] std::span<const Word> args_of(std::size_t i) const noexcept {
        return std::span<const Word>(args_).subspan(entries_[i].first, entries_[i].argc);
    }
    [[nodiscard]] const std::vector<Entry>& entries() const noexcept { return entries_; }
    [[nodiscard]] const std::vector<Word>& args() const noexcept { return args_; }

    [[nodiscard]] bool operator==(const OpBuffer&) const = default;

private:
    std::vector<Entry> entries_;
    std::vector<Word> args_;
};

// ---- host -----------------------------------------------------------------------------------------

/// The interface through which the machine reads the outside world. Read-only by contract: a host
/// must not change any state in these calls (the only exception is rng(), consumed by RAND_*).
/// The real host (E11-T05+) works over the World and the event tables; tests use a fake one.
class VmHost : public CatalogInfo {
public:
    /// Moves from a scope along a transition (section 5.1); an empty Scope when there is nothing.
    [[nodiscard]] virtual Scope transition(Scope from, std::uint16_t code) = 0;
    /// A catalog value over a scope, in the unit of the entry (Fixed64 raw, count, ppm, ...).
    [[nodiscard]] virtual Word load(Scope scope, std::uint16_t code, std::span<const Word> args) = 0;
    /// A variable of the scope or a global one, by 64-bit key; an absent variable reads as 0.
    [[nodiscard]] virtual Word load_var(Scope scope, bool global, std::uint64_t key) = 0;
    /// A catalog predicate over a scope.
    [[nodiscard]] virtual bool predicate(Scope scope, std::uint16_t code, std::span<const Word> args) = 0;
    /// Appends the elements of a list in the default order of the list (section 5.2), at most
    /// `limit` of them; returns true when the list had more and was cut.
    [[nodiscard]] virtual bool
    list(Scope scope, std::uint16_t code, std::uint32_t limit, std::vector<Scope>& out) = 0;
    /// The stream of the event being run (events.run / events.mtth / events.ai).
    [[nodiscard]] virtual core::Pcg32& rng() = 0;
};

// ---- Trace and Preview sinks ----------------------------------------------------------------------

/// A leaf of a trigger evaluated in Trace mode: a comparison or a predicate with its values.
struct TraceLeaf {
    std::uint32_t pc = 0;
    Op op = Op::kNop;
    std::uint16_t code = 0;  // predicate code (PRED), else 0
    bool result = false;
    Word v0 = 0;  // comparison: left operand; PRED: first argument; RAND_PERMILLE: ppm
    Word v1 = 0;  // comparison: right operand
};

/// Receives the leaves of a Trace run and the steps of accumulators (STEP instruction).
class Tracer {
public:
    virtual ~Tracer() = default;
    Tracer() = default;
    Tracer(const Tracer&) = default;
    Tracer& operator=(const Tracer&) = default;

    virtual void leaf(const TraceLeaf& leaf) = 0;
    /// One step of an accumulator: its code and the accumulator after the step (Fixed64 raw).
    virtual void step(std::uint16_t code, Word accumulator) = 0;
};

enum class PreviewLineKind : std::uint8_t { Effect, State };
enum class PreviewGroup : std::uint8_t {
    Chance,  // `random = { chance = N ... }`: value = ppm
    Branch,  // a branch of random_list: index = branch number, value = ppm share of 1 000 000
    Each,    // an iterator: index = list code, value = number of elements
};

/// Receives the consequences of an effect program in Preview mode: lines "effect + arguments" and
/// the structure around them. Groups nest and are always balanced (also when the run ends early).
class PreviewSink {
public:
    virtual ~PreviewSink() = default;
    PreviewSink() = default;
    PreviewSink(const PreviewSink&) = default;
    PreviewSink& operator=(const PreviewSink&) = default;

    virtual void line(PreviewLineKind kind, std::uint16_t code, Scope target, std::span<const Word> args) = 0;
    virtual void group_begin(PreviewGroup group, std::uint32_t index, std::uint32_t value) = 0;
    virtual void group_end() = 0;
    /// The iterator of the innermost Each group moved to this element (before its body runs).
    virtual void element(Scope scope) = 0;
    /// The innermost Each group is a random_<list>: one of `candidates` elements is picked.
    virtual void iter_pick(std::uint32_t candidates) = 0;
};

// ---- run ------------------------------------------------------------------------------------------

struct RunOptions {
    Mode mode = Mode::Run;
    Kind kind = Kind::Trigger;
    Scope root;                   // the scope the program starts in
    std::uint32_t max_instr = 0;  // 0: kMaxTriggerInstr (Trigger, Value) or kMaxEffectInstr (Effect)
    std::uint32_t max_commands = limits::kMaxCommandsPerRun;
    std::uint32_t max_state_ops = limits::kMaxStateOpsPerRun;
    Tracer* tracer = nullptr;        // Trace: leaves; any mode: accumulator steps
    PreviewSink* preview = nullptr;  // Preview: lines and groups
};

/// Counters of a run (section 7.3); they never abort the run.
struct RunCounters {
    std::uint32_t saturations = 0;  // arithmetic results clamped to the Fixed64 range
    std::uint32_t div_zero = 0;     // divisions by zero (result 0)
    std::uint32_t truncated = 0;    // lists cut at the fan-out limit
    std::uint32_t draws = 0;        // extractions from the random stream (Run only)

    [[nodiscard]] friend constexpr bool operator==(const RunCounters&, const RunCounters&) noexcept = default;
};

struct RunResult {
    bool ok = false;
    AbortReason reason = AbortReason::None;
    std::uint32_t abort_pc = 0;
    std::uint32_t abort_detail = 0;  // the operand of an explicit ABORT
    bool has_value = false;          // the program left a value on the stack
    Word value = 0;                  // the top of the stack at RET (a trigger: 0 or 1)
    std::uint32_t instructions = 0;  // executed (an instruction refused by the budget is not counted)
    std::uint32_t wu = 0;            // work units spent (telemetry, section 7.4)
    RunCounters counters;
    OpBuffer commands;   // Run: effects over foreign state, committed by the caller
    OpBuffer state_ops;  // Run: operations over the state of the events, committed by the caller

    [[nodiscard]] bool truthy() const noexcept { return ok && has_value && value != 0; }
    [[nodiscard]] core::Fixed64 fixed() const noexcept { return fixed_from_word(value); }

    /// Canonical little-endian bytes of the whole result: equal runs give equal bytes (T04).
    [[nodiscard]] std::vector<std::byte> to_bytes() const;
    [[nodiscard]] std::uint64_t hash() const noexcept;
};

/// The machine. Owns reusable scratch memory (iterator lists), so create one per thread and reuse
/// it; run() allocates nothing once the scratch has grown.
class Vm {
public:
    [[nodiscard]] RunResult run(const Program& program, VmHost& host, const RunOptions& options);

    /// One element of an iterator list with the data the random_/ordered_ forms need.
    struct IterElem {
        Scope scope;
        Word weight = 0;
        bool keep = true;
    };

    struct Keyed {
        Word key = 0;
        IterElem elem;
    };

    /// Reusable memory of the machine (the "arena of the tick", section 7.1).
    struct Scratch {
        std::vector<IterElem> items;
        std::vector<Scope> listing;
        std::vector<Keyed> keyed;
    };

private:
    Scratch scratch_;
};

// ---- recorders ------------------------------------------------------------------------------------

/// A Tracer that stores everything it is told.
class TraceRecorder final : public Tracer {
public:
    struct Step {
        std::uint16_t code = 0;
        Word value = 0;
    };

    void leaf(const TraceLeaf& l) override { leaves_.push_back(l); }
    void step(std::uint16_t code, Word accumulator) override { steps_.push_back(Step{code, accumulator}); }

    [[nodiscard]] const std::vector<TraceLeaf>& leaves() const noexcept { return leaves_; }
    [[nodiscard]] const std::vector<Step>& steps() const noexcept { return steps_; }
    /// The first leaf that evaluated to false, or nullptr. For a trigger that is a conjunction of
    /// leaves this is the reason it is false (section 13.3); under `not`/`or` the compiler marks
    /// the reason itself (E11-T06/T13).
    [[nodiscard]] const TraceLeaf* first_false() const noexcept;
    void clear() noexcept {
        leaves_.clear();
        steps_.clear();
    }

private:
    std::vector<TraceLeaf> leaves_;
    std::vector<Step> steps_;
};

/// Builds the additive economy::Breakdown of an accumulator from its STEP records (section 13.2):
/// the root is the total, every step is a child with the delta it contributed, a final remainder
/// term absorbs whatever the steps did not account for. Values are converted to the Q16.16 raw
/// ("points") of economy::Unit::Points with saturation, so the tree is exactly additive.
class BreakdownRecorder final : public Tracer {
public:
    explicit BreakdownRecorder(std::uint16_t root_code, std::uint16_t remainder_code) noexcept
        : root_code_(root_code), remainder_code_(remainder_code) {}

    void leaf(const TraceLeaf& l) override { (void)l; }
    void step(std::uint16_t code, Word accumulator) override;

    /// The tree for the final value of the accumulator (the run's result).
    [[nodiscard]] economy::Breakdown build(core::Fixed64 total) const;
    [[nodiscard]] std::size_t step_count() const noexcept { return steps_.size(); }

private:
    std::uint16_t root_code_;
    std::uint16_t remainder_code_;
    std::vector<TraceRecorder::Step> steps_;
};

/// One event of a PreviewRecorder.
struct PreviewEvent {
    enum class Type : std::uint8_t { Line, GroupBegin, GroupEnd, Element, IterPick };

    Type type = Type::Line;
    PreviewLineKind line_kind = PreviewLineKind::Effect;
    PreviewGroup group = PreviewGroup::Chance;
    std::uint16_t code = 0;   // Line: effect/state code
    std::uint32_t index = 0;  // GroupBegin: index; IterPick: candidates
    std::uint32_t value = 0;  // GroupBegin: value
    Scope scope;              // Line: target; Element: the element
    std::vector<Word> args;   // Line: arguments

    [[nodiscard]] bool operator==(const PreviewEvent&) const = default;
};

/// A PreviewSink that stores the event stream (the formatter of E11-T13 may use it directly).
class PreviewRecorder final : public PreviewSink {
public:
    void line(PreviewLineKind kind, std::uint16_t code, Scope target, std::span<const Word> args) override;
    void group_begin(PreviewGroup group, std::uint32_t index, std::uint32_t value) override;
    void group_end() override;
    void element(Scope scope) override;
    void iter_pick(std::uint32_t candidates) override;

    [[nodiscard]] const std::vector<PreviewEvent>& events() const noexcept { return events_; }
    void clear() noexcept { events_.clear(); }

private:
    std::vector<PreviewEvent> events_;
};

}  // namespace pat::sim::events
