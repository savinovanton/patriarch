#include "pat/sim/events/program.hpp"

#include "pat/core/rng.hpp"

#include <algorithm>
#include <limits>
#include <utility>

namespace pat::sim::events {

namespace {

struct OpInfo {
    Op op;
    const char* name;
    std::uint32_t wu;
};

// One row per assigned code, ascending by code (checked by a test).
constexpr std::array kOps{
    OpInfo{Op::kNop, "NOP", 0},
    OpInfo{Op::kPushInt, "PUSH_INT", 1},
    OpInfo{Op::kPushConst, "PUSH_CONST", 1},
    OpInfo{Op::kPushBool, "PUSH_BOOL", 1},
    OpInfo{Op::kDup, "DUP", 1},
    OpInfo{Op::kDrop, "DROP", 1},
    OpInfo{Op::kSwap, "SWAP", 1},
    OpInfo{Op::kAdd, "ADD", 1},
    OpInfo{Op::kSub, "SUB", 1},
    OpInfo{Op::kMul, "MUL", 2},
    OpInfo{Op::kDiv, "DIV", 2},
    OpInfo{Op::kNeg, "NEG", 1},
    OpInfo{Op::kMin, "MIN", 1},
    OpInfo{Op::kMax, "MAX", 1},
    OpInfo{Op::kClamp, "CLAMP", 1},
    OpInfo{Op::kTrunc, "TRUNC", 1},
    OpInfo{Op::kEq, "EQ", 1},
    OpInfo{Op::kNe, "NE", 1},
    OpInfo{Op::kLt, "LT", 1},
    OpInfo{Op::kLe, "LE", 1},
    OpInfo{Op::kGt, "GT", 1},
    OpInfo{Op::kGe, "GE", 1},
    OpInfo{Op::kNot, "NOT", 1},
    OpInfo{Op::kJmp, "JMP", 1},
    OpInfo{Op::kJf, "JF", 1},
    OpInfo{Op::kJt, "JT", 1},
    OpInfo{Op::kJfKeep, "JF_KEEP", 1},
    OpInfo{Op::kJtKeep, "JT_KEEP", 1},
    OpInfo{Op::kScopeEnter, "SCOPE_ENTER", 2},
    OpInfo{Op::kScopeLeave, "SCOPE_LEAVE", 1},
    OpInfo{Op::kScopeSave, "SCOPE_SAVE", 1},
    OpInfo{Op::kScopeLoad, "SCOPE_LOAD", 2},
    OpInfo{Op::kScopeRoot, "SCOPE_ROOT", 1},
    OpInfo{Op::kLoad, "LOAD", 2},
    OpInfo{Op::kPred, "PRED", 2},
    OpInfo{Op::kLoadVar, "LOAD_VAR", 3},
    OpInfo{Op::kIterBegin, "ITER_BEGIN", 4},
    OpInfo{Op::kIterNext, "ITER_NEXT", 2},
    OpInfo{Op::kIterEnd, "ITER_END", 1},
    OpInfo{Op::kIterOrder, "ITER_ORDER", 0},
    OpInfo{Op::kIterTake, "ITER_TAKE", 1},
    OpInfo{Op::kIterKeep, "ITER_KEEP", 1},
    OpInfo{Op::kIterRewind, "ITER_REWIND", 1},
    OpInfo{Op::kIterWeight, "ITER_WEIGHT", 1},
    OpInfo{Op::kIterPick, "ITER_PICK", 3},
    OpInfo{Op::kRandPermille, "RAND_PERMILLE", 2},
    OpInfo{Op::kRandPick, "RAND_PICK", 3},
    OpInfo{Op::kJTable, "JTABLE", 1},
    OpInfo{Op::kBranchEnd, "BRANCH_END", 0},
    OpInfo{Op::kHide, "HIDE", 0},
    OpInfo{Op::kEmit, "EMIT", 8},
    OpInfo{Op::kState, "STATE", 4},
    OpInfo{Op::kRet, "RET", 0},
    OpInfo{Op::kAbort, "ABORT", 0},
    OpInfo{Op::kStep, "STEP", 0},
};

const OpInfo* find_op(std::uint16_t code) noexcept {
    for (const OpInfo& info : kOps) {
        if (static_cast<std::uint16_t>(info.op) == code) {
            return &info;
        }
    }
    return nullptr;
}

void put_u32(std::vector<std::byte>& out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xFFU));
    }
}

void put_u64(std::vector<std::byte>& out, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        out.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xFFU));
    }
}

ProgramError error_at(ProgramErrorCode code, std::uint32_t pc, std::string message) {
    return ProgramError{code, pc, std::move(message)};
}

bool is_relative_jump(Op op) noexcept {
    switch (op) {
        case Op::kJmp:
        case Op::kJf:
        case Op::kJt:
        case Op::kJfKeep:
        case Op::kJtKeep:
        case Op::kScopeEnter:
        case Op::kScopeLoad:
        case Op::kIterBegin:
        case Op::kIterNext:
            return true;
        default:
            return false;
    }
}

struct StackEffect {
    std::int32_t need = 0;   // values the instruction takes from the stack
    std::int32_t delta = 0;  // net change of the depth
};

StackEffect stack_effect(const Instr& in) noexcept {
    const auto argc = in.b;
    switch (static_cast<Op>(in.op)) {
        case Op::kPushInt:
        case Op::kPushConst:
        case Op::kPushBool:
        case Op::kLoadVar:
            return {0, 1};
        case Op::kDup:
            return {1, 1};
        case Op::kDrop:
        case Op::kJf:
        case Op::kJt:
        case Op::kIterKeep:
        case Op::kIterWeight:
        case Op::kJTable:
            return {1, -1};
        case Op::kSwap:
            return {2, 0};
        case Op::kAdd:
        case Op::kSub:
        case Op::kMul:
        case Op::kDiv:
        case Op::kMin:
        case Op::kMax:
        case Op::kEq:
        case Op::kNe:
        case Op::kLt:
        case Op::kLe:
        case Op::kGt:
        case Op::kGe:
            return {2, -1};
        case Op::kNeg:
        case Op::kTrunc:
        case Op::kNot:
        case Op::kJfKeep:
        case Op::kJtKeep:
        case Op::kRandPermille:
        case Op::kStep:
            return {1, 0};
        case Op::kClamp:
            return {3, -2};
        case Op::kLoad:
        case Op::kPred:
            return {argc, 1 - argc};
        case Op::kRandPick:
            return {static_cast<std::int32_t>(in.a), 1 - static_cast<std::int32_t>(in.a)};
        case Op::kEmit:
        case Op::kState:
            return {argc, -argc};
        default:
            return {0, 0};
    }
}

}  // namespace

bool is_known_op(std::uint16_t code) noexcept {
    return find_op(code) != nullptr;
}

std::string_view op_name(std::uint16_t code) noexcept {
    const OpInfo* info = find_op(code);
    return info != nullptr ? std::string_view(info->name) : std::string_view("?");
}

std::uint32_t op_base_wu(std::uint16_t code) noexcept {
    const OpInfo* info = find_op(code);
    return info != nullptr ? info->wu : 0;
}

const SourcePos* Program::source_at(std::uint32_t pc) const noexcept {
    const SourcePos* best = nullptr;
    for (const SourcePos& pos : debug) {
        if (pos.pc > pc) {
            break;
        }
        best = &pos;
    }
    return best;
}

std::vector<std::byte> Program::to_bytes() const {
    std::vector<std::byte> out;
    out.reserve(8 + code.size() * 8 + consts.size() * 8);
    put_u32(out, static_cast<std::uint32_t>(code.size()));
    for (const Instr& in : code) {
        out.push_back(static_cast<std::byte>(in.op & 0xFFU));
        out.push_back(static_cast<std::byte>((in.op >> 8) & 0xFFU));
        out.push_back(static_cast<std::byte>(in.a & 0xFFU));
        out.push_back(static_cast<std::byte>((in.a >> 8) & 0xFFU));
        put_u32(out, static_cast<std::uint32_t>(in.b));
    }
    put_u32(out, static_cast<std::uint32_t>(consts.size()));
    for (const std::int64_t c : consts) {
        put_u64(out, static_cast<std::uint64_t>(c));
    }
    return out;
}

std::uint64_t Program::hash() const noexcept {
    const std::vector<std::byte> bytes = to_bytes();
    return core::fnv1a64(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
}

// ---- validation -----------------------------------------------------------------------------------

core::Result<void, ProgramError> validate(Program& program) {
    using R = core::Result<void, ProgramError>;
    const std::size_t n = program.code.size();
    if (n == 0) {
        return R::err(error_at(ProgramErrorCode::FallsOffEnd, 0, "empty program"));
    }
    if (n * sizeof(Instr) > limits::kMaxProgramBytes) {
        return R::err(error_at(ProgramErrorCode::TooLong, 0, "program exceeds kMaxProgramBytes"));
    }
    const auto in_range = [&](std::int64_t target) {
        return target >= 0 && target < static_cast<std::int64_t>(n);
    };

    // Pass 1: operands of every instruction.
    for (std::size_t i = 0; i < n; ++i) {
        const Instr& in = program.code[i];
        const auto pc = static_cast<std::uint32_t>(i);
        if (!is_known_op(in.op)) {
            return R::err(error_at(ProgramErrorCode::BadOpcode, pc, "unknown opcode"));
        }
        const Op op = static_cast<Op>(in.op);
        const std::int64_t target = static_cast<std::int64_t>(i) + 1 + in.b;
        if (is_relative_jump(op) && !in_range(target)) {
            return R::err(error_at(ProgramErrorCode::BadJump, pc, "jump target outside the program"));
        }
        switch (op) {
            case Op::kPushConst:
                if (in.b < 0 || static_cast<std::size_t>(in.b) >= program.consts.size()) {
                    return R::err(error_at(ProgramErrorCode::BadConst, pc, "constant index out of range"));
                }
                break;
            case Op::kLoadVar:
                if (in.a > 1 || in.b < 0 || static_cast<std::size_t>(in.b) >= program.consts.size()) {
                    return R::err(error_at(ProgramErrorCode::BadConst, pc, "variable key out of range"));
                }
                break;
            case Op::kScopeSave:
            case Op::kScopeLoad:
                if (in.a >= limits::kMaxSavedScopes) {
                    return R::err(
                        error_at(ProgramErrorCode::BadOperand, pc, "saved scope slot out of range"));
                }
                break;
            case Op::kLoad:
            case Op::kPred:
            case Op::kEmit:
            case Op::kState:
                if (in.b < 0 || static_cast<std::size_t>(in.b) > limits::kMaxEffectArgs) {
                    return R::err(error_at(ProgramErrorCode::BadOperand, pc, "too many arguments"));
                }
                break;
            case Op::kIterBegin:
                if (static_cast<Op>(program.code[static_cast<std::size_t>(target)].op) != Op::kIterEnd) {
                    return R::err(
                        error_at(ProgramErrorCode::BadJump, pc, "ITER_BEGIN must point at ITER_END"));
                }
                break;
            case Op::kRandPick:
                if (in.a < 1 || in.a > limits::kMaxBranches) {
                    return R::err(error_at(ProgramErrorCode::BadOperand, pc, "RAND_PICK branch count"));
                }
                break;
            case Op::kJTable: {
                if (in.a < 1 || in.a > limits::kMaxBranches) {
                    return R::err(error_at(ProgramErrorCode::BadOperand, pc, "JTABLE branch count"));
                }
                for (std::int64_t k = 0; k < in.a; ++k) {
                    const std::int64_t entry = target + k;
                    if (!in_range(entry) ||
                        static_cast<Op>(program.code[static_cast<std::size_t>(entry)].op) != Op::kJmp) {
                        return R::err(error_at(ProgramErrorCode::BadTable, pc, "JTABLE entries must be JMP"));
                    }
                }
                break;
            }
            case Op::kHide:
                if (in.a > 1) {
                    return R::err(error_at(ProgramErrorCode::BadOperand, pc, "HIDE operand"));
                }
                break;
            default:
                break;
        }
    }

    // Pass 2: the value stack depth on every path (a worklist over the control-flow graph).
    std::vector<std::int32_t> depth(n, -1);
    std::vector<std::uint32_t> work;
    std::int32_t peak = 0;
    depth[0] = 0;
    work.push_back(0);

    core::Result<void, ProgramError> failure = R::ok();
    const auto visit = [&](std::int64_t target, std::int32_t d, std::uint32_t from) -> bool {
        if (!in_range(target)) {
            failure = R::err(error_at(ProgramErrorCode::FallsOffEnd, from, "execution runs off the end"));
            return false;
        }
        auto& slot = depth[static_cast<std::size_t>(target)];
        if (slot < 0) {
            slot = d;
            work.push_back(static_cast<std::uint32_t>(target));
        } else if (slot != d) {
            failure = R::err(error_at(ProgramErrorCode::StackMismatch,
                                      static_cast<std::uint32_t>(target),
                                      "stack depth differs between paths"));
            return false;
        }
        return true;
    };

    while (!work.empty()) {
        const std::uint32_t pc = work.back();
        work.pop_back();
        const Instr& in = program.code[pc];
        const Op op = static_cast<Op>(in.op);
        const std::int32_t d = depth[pc];
        const StackEffect fx = stack_effect(in);
        if (d < fx.need) {
            return R::err(error_at(ProgramErrorCode::StackUnderflow, pc, "stack underflow"));
        }
        const std::int32_t nd = d + fx.delta;
        peak = std::max({peak, d, nd});
        if (peak > static_cast<std::int32_t>(limits::kMaxStack)) {
            return R::err(error_at(ProgramErrorCode::StackOverflow, pc, "stack deeper than kMaxStack"));
        }
        const std::int64_t next = static_cast<std::int64_t>(pc) + 1;
        const std::int64_t target = next + in.b;
        bool good = true;
        switch (op) {
            case Op::kRet:
                if (d > 1) {
                    return R::err(
                        error_at(ProgramErrorCode::StackMismatch, pc, "RET with more than one value"));
                }
                break;
            case Op::kAbort:
                break;
            case Op::kJmp:
                good = visit(target, nd, pc);
                break;
            case Op::kJf:
            case Op::kJt:
            case Op::kJfKeep:
            case Op::kJtKeep:
            case Op::kScopeEnter:
            case Op::kScopeLoad:
            case Op::kIterNext:
                good = visit(next, nd, pc) && visit(target, nd, pc);
                break;
            case Op::kJTable:
                for (std::int64_t k = 0; good && k <= in.a; ++k) {
                    good = visit(target + k, nd, pc);
                }
                break;
            default:
                good = visit(next, nd, pc);
                break;
        }
        if (!good) {
            return failure;
        }
    }

    program.max_stack = static_cast<std::uint32_t>(peak);
    program.estimated_wu = estimate_cost(program, CatalogInfo{});
    return R::ok();
}

std::uint32_t estimate_cost(const Program& program, const CatalogInfo& catalog) {
    const std::size_t n = program.code.size();
    std::vector<std::uint64_t> mult(n, 1);
    std::vector<std::uint32_t> bound(n, 1);  // bound of the innermost enclosing iterator
    constexpr std::uint64_t kCap = std::numeric_limits<std::uint32_t>::max();

    for (std::size_t i = 0; i < n; ++i) {
        const Instr& in = program.code[i];
        if (static_cast<Op>(in.op) != Op::kIterBegin) {
            continue;
        }
        const std::int64_t end = static_cast<std::int64_t>(i) + 1 + in.b;
        if (end < 0 || end >= static_cast<std::int64_t>(n)) {
            continue;
        }
        const std::uint32_t b =
            std::min<std::uint32_t>(catalog.list_bound(in.a), static_cast<std::uint32_t>(limits::kMaxFanout));
        for (std::size_t k = i + 1; k < static_cast<std::size_t>(end); ++k) {
            mult[k] = std::min(mult[k] * std::max<std::uint32_t>(b, 1), kCap);
            bound[k] = b;
        }
    }

    std::uint64_t total = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const Instr& in = program.code[i];
        const Op op = static_cast<Op>(in.op);
        std::uint64_t cost = op_base_wu(in.op);
        switch (op) {
            case Op::kLoad:
            case Op::kPred:
                cost = catalog.cost(op, in.a);
                break;
            case Op::kIterBegin:
                cost = 4 + std::min<std::uint32_t>(catalog.list_bound(in.a),
                                                   static_cast<std::uint32_t>(limits::kMaxFanout));
                break;
            case Op::kIterOrder:
                cost = static_cast<std::uint64_t>(bound[i]) * (2 + catalog.cost(Op::kLoad, in.a));
                break;
            default:
                break;
        }
        total = std::min(total + cost * mult[i], kCap);
    }
    return static_cast<std::uint32_t>(total);
}

// ---- assembler ------------------------------------------------------------------------------------

ProgramBuilder::Label ProgramBuilder::new_label() {
    label_pc_.push_back(-1);
    return static_cast<Label>(label_pc_.size() - 1);
}

void ProgramBuilder::bind(Label label) {
    PAT_ASSERT(label < label_pc_.size(), "unknown label");
    label_pc_[label] = static_cast<std::int64_t>(program_.code.size());
}

void ProgramBuilder::emit(Op op, std::uint16_t a, std::int32_t b) {
    if (has_pending_source_) {
        pending_source_.pc = static_cast<std::uint32_t>(program_.code.size());
        program_.debug.push_back(pending_source_);
        has_pending_source_ = false;
    }
    program_.code.push_back(make_instr(op, a, b));
}

void ProgramBuilder::emit_jump(Op op, Label target, std::uint16_t a) {
    fixups_.push_back(Fixup{static_cast<std::uint32_t>(program_.code.size()), target});
    emit(op, a, 0);
}

std::uint32_t ProgramBuilder::constant(std::int64_t raw) {
    for (std::size_t i = 0; i < program_.consts.size(); ++i) {
        if (program_.consts[i] == raw) {
            return static_cast<std::uint32_t>(i);
        }
    }
    program_.consts.push_back(raw);
    return static_cast<std::uint32_t>(program_.consts.size() - 1);
}

void ProgramBuilder::push_fixed(core::Fixed64 v) {
    constexpr std::int64_t kOne = core::Fixed64::one_raw;
    const std::int64_t raw = v.raw();
    if (raw % kOne == 0 && raw / kOne >= std::numeric_limits<std::int32_t>::min() &&
        raw / kOne <= std::numeric_limits<std::int32_t>::max()) {
        push_int(static_cast<std::int32_t>(raw / kOne));
    } else {
        emit(Op::kPushConst, 0, static_cast<std::int32_t>(constant(raw)));
    }
}

void ProgramBuilder::set_source(std::uint32_t file, std::uint32_t line, std::uint32_t column) {
    pending_source_ = SourcePos{0, file, line, column};
    has_pending_source_ = true;
}

core::Result<Program, ProgramError> ProgramBuilder::finish() {
    using R = core::Result<Program, ProgramError>;
    for (const Fixup& fix : fixups_) {
        const std::int64_t target = label_pc_[fix.label];
        if (target < 0) {
            return R::err(error_at(ProgramErrorCode::UnboundLabel, fix.pc, "label was never bound"));
        }
        program_.code[fix.pc].b = static_cast<std::int32_t>(target - (static_cast<std::int64_t>(fix.pc) + 1));
    }
    if (auto checked = validate(program_); !checked) {
        return R::err(checked.error());
    }
    return R::ok(std::move(program_));
}

}  // namespace pat::sim::events
