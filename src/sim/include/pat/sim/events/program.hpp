#pragma once

#include "pat/core/fixed.hpp"
#include "pat/core/result.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Bytecode of the event engine (E11-T04, docs/dev/event-engine.md section 7, ADR-0008 D3).
//
// A Program is the immutable compiled form of ONE block (a trigger, an effect list, a value
// accumulator). Instructions are 8 bytes: u16 op | u16 a | i32 b. The machine is a stack machine
// over 64-bit words: Fixed64 raw values (Q32.32), booleans (0/1), packed scopes and 64-bit keys.
// Types are checked by the compiler (E11-T06), not by the machine; the machine only enforces
// bounds and limits, so a malformed program can abort a run but never corrupt memory.
//
// Jump operands (JMP, JF, JT, JF_KEEP, JT_KEEP, SCOPE_ENTER, SCOPE_LOAD, ITER_BEGIN, ITER_NEXT,
// JTABLE) are relative to the NEXT instruction: target = pc + 1 + b.
//
// This header has no dependency on World, the command layer or any catalog: the catalogs (E11-T07
// .. T10) give the numeric codes of values, predicates, lists, effects and state operations their
// meaning through the VmHost interface (vm.hpp).

namespace pat::sim::events {

/// A machine word: Fixed64 raw bits, a boolean (0/1), a packed Scope or a 64-bit key.
using Word = std::int64_t;

[[nodiscard]] constexpr Word word_from_fixed(core::Fixed64 v) noexcept {
    return v.raw();
}
[[nodiscard]] constexpr core::Fixed64 fixed_from_word(Word w) noexcept {
    return core::Fixed64::from_raw(w);
}
/// A whole number as a word (the value of PUSH_INT).
[[nodiscard]] constexpr Word word_from_int(std::int32_t v) noexcept {
    return static_cast<Word>(v) * (Word{1} << 32);
}

/// Limits of the machine (docs/dev/event-engine.md section 7.4). `[предл.]` values.
namespace limits {
inline constexpr std::size_t kMaxStack = 32;
inline constexpr std::size_t kMaxScopeDepth = 8;
inline constexpr std::size_t kMaxIterNest = 3;
inline constexpr std::size_t kMaxFanout = 256;
inline constexpr std::uint32_t kMaxTriggerInstr = 4000;
inline constexpr std::uint32_t kMaxEffectInstr = 40000;
inline constexpr std::uint32_t kMaxCommandsPerRun = 64;
inline constexpr std::uint32_t kMaxStateOpsPerRun = 128;
inline constexpr std::size_t kMaxSavedScopes = 8;
inline constexpr std::size_t kMaxBranches = 8;
inline constexpr std::size_t kMaxEffectArgs = 8;
inline constexpr std::size_t kMaxForkDepth = 8;
inline constexpr std::size_t kMaxProgramBytes = 64 * 1024;
}  // namespace limits

// ---- scopes ---------------------------------------------------------------------------------------

/// Kind of a scope (docs/dev/event-engine.md section 5). Numeric values are stable.
enum class ScopeKind : std::uint8_t {
    None = 0,
    Character = 1,
    Dynasty = 2,
    Province = 3,
    Realm = 4,
    Church = 5,
    Army = 6,
    Scheme = 7,
    Plot = 8,
    Faith = 9,
    Treaty = 10,
};

/// A typed reference: (kind, id). `None` means "empty" (a transition found nothing).
struct Scope {
    ScopeKind kind = ScopeKind::None;
    std::uint32_t id = 0;

    [[nodiscard]] constexpr bool empty() const noexcept { return kind == ScopeKind::None; }
    /// kind in the high 32 bits, id in the low 32: ordering by word() is ordering by (kind, id).
    [[nodiscard]] constexpr Word word() const noexcept {
        return (static_cast<Word>(kind) << 32) | static_cast<Word>(id);
    }
    [[nodiscard]] static constexpr Scope from_word(Word w) noexcept {
        return Scope{static_cast<ScopeKind>((static_cast<std::uint64_t>(w) >> 32) & 0xFFU),
                     static_cast<std::uint32_t>(static_cast<std::uint64_t>(w) & 0xFFFFFFFFULL)};
    }
    [[nodiscard]] static constexpr Scope make(ScopeKind k, std::uint32_t i) noexcept { return Scope{k, i}; }

    [[nodiscard]] friend constexpr bool operator==(const Scope&, const Scope&) noexcept = default;
};

// ---- instructions ---------------------------------------------------------------------------------

/// Instruction codes. STABLE: never renumber or reuse (disassembler, golden programs, reports).
/// Codes up to 91 are the table of the specification; 45 (E11-T06: a scope as an argument), 65..68, 73,
/// 74 and 92 are additions made by E11-T04 (random_<list>, "ordered_" with a limit, the preview structure,
/// hidden_effect and the breakdown steps); see docs/dev/event-engine.md section 7.2.
enum class Op : std::uint16_t {
    kNop = 0,
    kPushInt = 1,
    kPushConst = 2,
    kPushBool = 3,
    kDup = 4,
    kDrop = 5,
    kSwap = 6,
    kAdd = 10,
    kSub = 11,
    kMul = 12,
    kDiv = 13,
    kNeg = 14,
    kMin = 15,
    kMax = 16,
    kClamp = 17,
    kTrunc = 18,
    kEq = 20,
    kNe = 21,
    kLt = 22,
    kLe = 23,
    kGt = 24,
    kGe = 25,
    kNot = 30,
    kJmp = 31,
    kJf = 32,
    kJt = 33,
    kJfKeep = 34,
    kJtKeep = 35,
    kScopeEnter = 40,
    kScopeLeave = 41,
    kScopeSave = 42,
    kScopeLoad = 43,
    kScopeRoot = 44,
    kPushScope = 45,
    kLoad = 50,
    kPred = 51,
    kLoadVar = 52,
    kIterBegin = 60,
    kIterNext = 61,
    kIterEnd = 62,
    kIterOrder = 63,
    kIterTake = 64,
    kIterKeep = 65,
    kIterRewind = 66,
    kIterWeight = 67,
    kIterPick = 68,
    kRandPermille = 70,
    kRandPick = 71,
    kJTable = 72,
    kBranchEnd = 73,
    kHide = 74,
    kEmit = 80,
    kState = 81,
    kRet = 90,
    kAbort = 91,
    kStep = 92,
};

struct Instr {
    std::uint16_t op = 0;
    std::uint16_t a = 0;
    std::int32_t b = 0;

    [[nodiscard]] friend constexpr bool operator==(const Instr&, const Instr&) noexcept = default;
};
static_assert(sizeof(Instr) == 8, "instructions are 8 bytes (event-engine.md section 7.1)");

[[nodiscard]] constexpr Instr make_instr(Op op, std::uint16_t a = 0, std::int32_t b = 0) noexcept {
    return Instr{static_cast<std::uint16_t>(op), a, b};
}

/// True when `code` is an assigned instruction code.
[[nodiscard]] bool is_known_op(std::uint16_t code) noexcept;
/// Mnemonic ("ADD", "ITER_NEXT"); "?" for an unknown code.
[[nodiscard]] std::string_view op_name(std::uint16_t code) noexcept;
/// Base cost of an instruction in work units (WU). LOAD and PRED cost 2 here; a catalog can say more.
[[nodiscard]] std::uint32_t op_base_wu(std::uint16_t code) noexcept;

// ---- catalog information --------------------------------------------------------------------------

enum class NameSpace : std::uint8_t {
    Transition,
    Value,
    Predicate,
    List,
    Effect,
    StateOp,
    Step,
};

/// What the machine tools (disassembler, cost estimate) may ask a catalog. The defaults are used
/// when no catalog exists yet; VmHost derives from this interface, E11-T07..T10 override it.
class CatalogInfo {
public:
    virtual ~CatalogInfo() = default;
    CatalogInfo() = default;
    CatalogInfo(const CatalogInfo&) = default;
    CatalogInfo& operator=(const CatalogInfo&) = default;

    /// WU of a LOAD or PRED with this code (2..8 by the catalog entry).
    [[nodiscard]] virtual std::uint32_t cost(Op op, std::uint16_t code) const {
        (void)code;
        return op_base_wu(static_cast<std::uint16_t>(op));
    }
    /// Upper bound of the number of elements of a list (the cost estimate multiplies by it).
    [[nodiscard]] virtual std::uint32_t list_bound(std::uint16_t code) const {
        (void)code;
        return 32;
    }
    /// Name of a catalog code for the disassembler; empty when unknown.
    [[nodiscard]] virtual std::string_view name(NameSpace space, std::uint16_t code) const {
        (void)space;
        (void)code;
        return {};
    }
};

// ---- program --------------------------------------------------------------------------------------

/// Where an instruction came from in the source block (debug table); filled by the compiler.
struct SourcePos {
    std::uint32_t pc = 0;
    std::uint32_t file = 0;
    std::uint32_t line = 0;
    std::uint32_t column = 0;

    [[nodiscard]] friend constexpr bool operator==(const SourcePos&, const SourcePos&) noexcept = default;
};

struct Program {
    std::vector<Instr> code;
    std::vector<std::int64_t> consts;  // raw Fixed64 values and 64-bit keys
    std::vector<SourcePos> debug;      // ascending by pc
    std::uint32_t max_stack = 0;       // proven by validate()
    std::uint32_t estimated_wu = 0;    // static worst case with the default CatalogInfo

    /// The debug record of the last instruction at or before `pc`, or nullptr.
    [[nodiscard]] const SourcePos* source_at(std::uint32_t pc) const noexcept;
    /// Canonical bytes of the code and constants (the debug table is not part of them).
    [[nodiscard]] std::vector<std::byte> to_bytes() const;
    /// FNV-1a of to_bytes(): identifies a compiled program in tests and reports.
    [[nodiscard]] std::uint64_t hash() const noexcept;
};

enum class ProgramErrorCode : std::uint8_t {
    BadOpcode,
    BadOperand,
    BadJump,
    BadConst,
    BadTable,
    StackUnderflow,
    StackOverflow,
    StackMismatch,
    FallsOffEnd,
    TooLong,
    UnboundLabel,
};

struct ProgramError {
    ProgramErrorCode code = ProgramErrorCode::BadOpcode;
    std::uint32_t pc = 0;
    std::string message;
};

/// Static checks every compiled program must pass: known opcodes, operands in range, jump targets
/// inside the program, the value stack consistent on every path and within kMaxStack, every path
/// ending in RET or ABORT. Fills max_stack and estimated_wu. The machine does not require it (it
/// checks at run time), but the compiler and tests use it as the "valid program" definition.
[[nodiscard]] core::Result<void, ProgramError> validate(Program& program);

/// Static worst-case cost in WU: every instruction counted once per iteration of its enclosing
/// iterators (each multiplied by the catalog's list bound); both branches of a condition counted.
[[nodiscard]] std::uint32_t estimate_cost(const Program& program, const CatalogInfo& catalog);

/// Text listing for debugging: offset, mnemonic, operands, jump targets, catalog names, source.
[[nodiscard]] std::string disassemble(const Program& program, const CatalogInfo* catalog = nullptr);

// ---- assembler ------------------------------------------------------------------------------------

/// Builds programs by hand (tests, and the compiler of E11-T06 later): labels, constant pool with
/// de-duplication, debug positions. finish() resolves jumps and runs validate().
class ProgramBuilder {
public:
    using Label = std::uint32_t;

    [[nodiscard]] Label new_label();
    /// Binds the label to the next emitted instruction.
    void bind(Label label);

    void emit(Op op, std::uint16_t a = 0, std::int32_t b = 0);
    /// Emits a jumping instruction whose operand b is resolved to `target`.
    void emit_jump(Op op, Label target, std::uint16_t a = 0);

    void push_int(std::int32_t v) { emit(Op::kPushInt, 0, v); }
    void push_bool(bool v) { emit(Op::kPushBool, 0, v ? 1 : 0); }
    /// PUSH_INT when the value is a whole number that fits, otherwise PUSH_CONST.
    void push_fixed(core::Fixed64 v);
    /// Index of a raw constant in the pool (added when new).
    [[nodiscard]] std::uint32_t constant(std::int64_t raw);

    /// The next instruction gets this position in the debug table.
    void set_source(std::uint32_t file, std::uint32_t line, std::uint32_t column);

    [[nodiscard]] std::size_t size() const noexcept { return program_.code.size(); }

    [[nodiscard]] core::Result<Program, ProgramError> finish();

private:
    struct Fixup {
        std::uint32_t pc;
        Label label;
    };

    Program program_;
    std::vector<std::int64_t> label_pc_;  // -1 while unbound
    std::vector<Fixup> fixups_;
    bool has_pending_source_ = false;
    SourcePos pending_source_;
};

}  // namespace pat::sim::events
