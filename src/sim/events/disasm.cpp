#include "pat/sim/events/program.hpp"

#include <algorithm>
#include <format>
#include <string>
#include <vector>

namespace pat::sim::events {

namespace {

bool jumps(Op op) noexcept {
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

// The catalog namespace the `a` operand of an instruction belongs to (or nullopt).
bool catalog_space(Op op, NameSpace& space) noexcept {
    switch (op) {
        case Op::kScopeEnter:
            space = NameSpace::Transition;
            return true;
        case Op::kLoad:
        case Op::kIterOrder:
            space = NameSpace::Value;
            return true;
        case Op::kPred:
            space = NameSpace::Predicate;
            return true;
        case Op::kIterBegin:
            space = NameSpace::List;
            return true;
        case Op::kEmit:
            space = NameSpace::Effect;
            return true;
        case Op::kState:
            space = NameSpace::StateOp;
            return true;
        case Op::kStep:
            space = NameSpace::Step;
            return true;
        default:
            return false;
    }
}

}  // namespace

std::string disassemble(const Program& program, const CatalogInfo* catalog) {
    const std::size_t n = program.code.size();
    std::vector<bool> is_target(n + 1, false);
    for (std::size_t i = 0; i < n; ++i) {
        const Instr& in = program.code[i];
        const Op op = static_cast<Op>(in.op);
        std::int64_t base = static_cast<std::int64_t>(i) + 1 + in.b;
        if (jumps(op)) {
            if (base >= 0 && base <= static_cast<std::int64_t>(n)) {
                is_target[static_cast<std::size_t>(base)] = true;
            }
        } else if (op == Op::kJTable) {
            for (std::int64_t k = 0; k <= in.a; ++k) {
                if (base + k >= 0 && base + k <= static_cast<std::int64_t>(n)) {
                    is_target[static_cast<std::size_t>(base + k)] = true;
                }
            }
        }
    }

    std::string out = std::format("; {} instructions, {} constants, max stack {}, estimated {} WU\n",
                                  n,
                                  program.consts.size(),
                                  program.max_stack,
                                  program.estimated_wu);
    for (std::size_t i = 0; i < n; ++i) {
        const Instr& in = program.code[i];
        const Op op = static_cast<Op>(in.op);
        std::string line = std::format("{}{:04}  {:<13}", is_target[i] ? '>' : ' ', i, op_name(in.op));
        std::string note;

        switch (op) {
            case Op::kPushInt:
            case Op::kPushBool:
            case Op::kIterTake:
                line += std::format(" {}", in.b);
                break;
            case Op::kPushConst:
                line += std::format(" #{}", in.b);
                if (in.b >= 0 && static_cast<std::size_t>(in.b) < program.consts.size()) {
                    note = std::format("= {}",
                                       core::to_string(core::Fixed64::from_raw(
                                           program.consts[static_cast<std::size_t>(in.b)])));
                }
                break;
            case Op::kLoadVar:
                line += std::format(" {} #{}", in.a != 0 ? "global" : "scope", in.b);
                break;
            case Op::kScopeSave:
                line += std::format(" slot {}", in.a);
                break;
            case Op::kScopeLoad:
                line += std::format(" slot {} -> {:04}", in.a, static_cast<std::int64_t>(i) + 1 + in.b);
                break;
            case Op::kLoad:
            case Op::kPred:
            case Op::kEmit:
            case Op::kState:
                line += std::format(" {} argc {}", in.a, in.b);
                break;
            case Op::kScopeEnter:
            case Op::kIterBegin:
                line += std::format(" {} -> {:04}", in.a, static_cast<std::int64_t>(i) + 1 + in.b);
                break;
            case Op::kJTable:
                line += std::format(" n {} table {:04}", in.a, static_cast<std::int64_t>(i) + 1 + in.b);
                break;
            case Op::kRandPick:
            case Op::kIterOrder:
            case Op::kHide:
            case Op::kStep:
            case Op::kAbort:
                line += std::format(" {}", in.a);
                break;
            case Op::kRandPermille:
                line += std::format(" {}", in.b != 0 ? "block" : "plain");
                break;
            default:
                if (jumps(op)) {
                    line += std::format(" -> {:04}", static_cast<std::int64_t>(i) + 1 + in.b);
                }
                break;
        }

        NameSpace space{};
        if (catalog != nullptr && catalog_space(op, space)) {
            const std::string_view name = catalog->name(space, in.a);
            if (!name.empty()) {
                note = std::string(name);
            }
        }
        if (const SourcePos* pos = program.source_at(static_cast<std::uint32_t>(i));
            pos != nullptr && pos->pc == i) {
            note += std::format("{}@{}:{}:{}", note.empty() ? "" : "  ", pos->file, pos->line, pos->column);
        }
        if (!note.empty()) {
            while (line.size() < 40) {
                line += ' ';
            }
            line += "; " + note;
        } else {
            while (!line.empty() && line.back() == ' ') {
                line.pop_back();
            }
        }
        out += line;
        out += '\n';
    }
    return out;
}

}  // namespace pat::sim::events
