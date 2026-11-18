#include "pat/core/fixed.hpp"
#include "pat/core/rng.hpp"
#include "pat/sim/events/program.hpp"
#include "pat/sim/events/vm.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "test_files.hpp"

// E11-T04: bytecode and virtual machine (docs/dev/event-engine.md section 7). The machine is tested
// on hand-assembled programs over a fake host: no World, no compiler, no catalogs.

using namespace pat::sim::events;
namespace economy = pat::sim::economy;
using pat::core::Fixed64;
using pat::core::Pcg32;

namespace {

constexpr std::uint16_t kHonor = 5;      // value: honor of the current scope (host map)
constexpr std::uint16_t kSumArgs = 900;  // value: the sum of its arguments
constexpr std::uint16_t kTransDynasty = 1;
constexpr std::uint16_t kTransSpouse = 2;
constexpr std::uint16_t kTransLoop = 3;  // character n -> character n+1 (for deep chains)
constexpr std::uint16_t kListMembers = 1;
constexpr std::uint16_t kListBig = 2;  // 300 elements
constexpr std::uint16_t kPredAdult = 1;
constexpr std::uint16_t kPredYes = 2;  // always true, counts calls
constexpr std::uint16_t kPredNo = 3;   // always false, counts calls

Scope ch(std::uint32_t id) {
    return Scope::make(ScopeKind::Character, id);
}
Scope dyn(std::uint32_t id) {
    return Scope::make(ScopeKind::Dynasty, id);
}
Word W(std::int32_t v) {
    return word_from_int(v);
}
Word WR(std::int64_t num, std::int64_t den) {
    return Fixed64::from_ratio(num, den).raw();
}

class FakeHost final : public VmHost {
public:
    explicit FakeHost(std::uint64_t seed = 1) : rng_(seed, 7) {
        values_[{ch(1).word(), kHonor}] = W(22);
        values_[{ch(2).word(), kHonor}] = W(40);
        values_[{ch(3).word(), kHonor}] = W(40);
        values_[{ch(4).word(), kHonor}] = W(10);
        values_[{dyn(10).word(), kHonor}] = W(55);
        for (std::uint32_t id = 1; id <= 4; ++id) {
            transitions_[{ch(id).word(), kTransDynasty}] = dyn(10);
        }
        lists_[{dyn(10).word(), kListMembers}] = {ch(3), ch(1), ch(2), ch(4)};
        std::vector<Scope> big;
        for (std::uint32_t i = 1; i <= 300; ++i) {
            big.push_back(ch(i));
        }
        lists_[{dyn(10).word(), kListBig}] = big;
        preds_[{ch(1).word(), kPredAdult}] = true;
        preds_[{ch(2).word(), kPredAdult}] = true;
    }

    Scope transition(Scope from, std::uint16_t code) override {
        ++transition_calls;
        if (code == kTransLoop) {
            return ch(from.id + 1);
        }
        const auto it = transitions_.find({from.word(), code});
        return it == transitions_.end() ? Scope{} : it->second;
    }
    Word load(Scope scope, std::uint16_t code, std::span<const Word> args) override {
        ++load_calls;
        last_args.assign(args.begin(), args.end());
        if (code == kSumArgs) {
            Word sum = 0;
            for (const Word a : args) {
                sum += a;
            }
            return sum;
        }
        const auto it = values_.find({scope.word(), code});
        return it == values_.end() ? 0 : it->second;
    }
    Word load_var(Scope scope, bool global, std::uint64_t key) override {
        const auto it = vars_.find({global ? -1 : scope.word(), key});
        return it == vars_.end() ? 0 : it->second;
    }
    bool predicate(Scope scope, std::uint16_t code, std::span<const Word> args) override {
        ++pred_calls;
        last_args.assign(args.begin(), args.end());
        if (code == kPredYes) {
            return true;
        }
        if (code == kPredNo) {
            return false;
        }
        const auto it = preds_.find({scope.word(), code});
        return it != preds_.end() && it->second;
    }
    bool list(Scope scope, std::uint16_t code, std::uint32_t limit, std::vector<Scope>& out) override {
        ++list_calls;
        const auto it = lists_.find({scope.word(), code});
        if (it == lists_.end()) {
            return false;
        }
        const std::size_t n = std::min<std::size_t>(limit, it->second.size());
        out.assign(it->second.begin(), it->second.begin() + static_cast<std::ptrdiff_t>(n));
        return it->second.size() > limit;
    }
    Pcg32& rng() override { return rng_; }

    [[nodiscard]] std::uint32_t total_calls() const {
        return transition_calls + load_calls + pred_calls + list_calls;
    }

    std::uint32_t transition_calls = 0;
    std::uint32_t load_calls = 0;
    std::uint32_t pred_calls = 0;
    std::uint32_t list_calls = 0;
    std::vector<Word> last_args;
    std::map<std::pair<Word, std::uint16_t>, Word> values_;
    std::map<std::pair<Word, std::uint16_t>, Scope> transitions_;
    std::map<std::pair<Word, std::uint16_t>, std::vector<Scope>> lists_;
    std::map<std::pair<Word, std::uint16_t>, bool> preds_;
    std::map<std::pair<Word, std::uint64_t>, Word> vars_;
    Pcg32 rng_;
};

Program build(const std::function<void(ProgramBuilder&)>& fn) {
    ProgramBuilder b;
    fn(b);
    auto result = b.finish();
    if (!result) {
        FAIL_CHECK("program rejected: " << result.error().message << " at pc " << result.error().pc);
        return Program{};
    }
    return std::move(result).value();
}

RunResult run(const Program& program,
              VmHost& host,
              Kind kind = Kind::Trigger,
              Mode mode = Mode::Run,
              Scope root = ch(1)) {
    Vm vm;
    RunOptions options;
    options.mode = mode;
    options.kind = kind;
    options.root = root;
    return vm.run(program, host, options);
}

RunResult eval(const std::function<void(ProgramBuilder&)>& fn, Kind kind = Kind::Trigger) {
    FakeHost host;
    return run(build(fn), host, kind);
}

RunResult eval2(Op op, Word a, Word b) {
    return eval([&](ProgramBuilder& p) {
        p.push_fixed(Fixed64::from_raw(a));
        p.push_fixed(Fixed64::from_raw(b));
        p.emit(op);
        p.emit(Op::kRet);
    });
}

constexpr Word kWMax = std::numeric_limits<Word>::max();
constexpr Word kWMin = std::numeric_limits<Word>::min();

// for_each-style iterator loop: ITER_BEGIN list; loop: ITER_NEXT; body; JMP loop; end: ITER_END
void each(ProgramBuilder& p, std::uint16_t list, const std::function<void(ProgramBuilder&)>& body) {
    const auto loop = p.new_label();
    const auto end = p.new_label();
    p.emit_jump(Op::kIterBegin, end, list);
    p.bind(loop);
    p.emit_jump(Op::kIterNext, end);
    body(p);
    p.emit_jump(Op::kJmp, loop);
    p.bind(end);
    p.emit(Op::kIterEnd);
}

using pat::test::read_text;

}  // namespace

// ---- the instruction table ----------------------------------------------------------------------

TEST_CASE("events vm: instructions are 8 bytes and codes are stable") {
    static_assert(sizeof(Instr) == 8);
    const std::array<std::pair<Op, std::uint16_t>, 56> codes = {{
        {Op::kNop, 0},         {Op::kPushInt, 1},     {Op::kPushConst, 2},     {Op::kPushBool, 3},
        {Op::kDup, 4},         {Op::kDrop, 5},        {Op::kSwap, 6},          {Op::kAdd, 10},
        {Op::kSub, 11},        {Op::kMul, 12},        {Op::kDiv, 13},          {Op::kNeg, 14},
        {Op::kMin, 15},        {Op::kMax, 16},        {Op::kClamp, 17},        {Op::kTrunc, 18},
        {Op::kEq, 20},         {Op::kNe, 21},         {Op::kLt, 22},           {Op::kLe, 23},
        {Op::kGt, 24},         {Op::kGe, 25},         {Op::kNot, 30},          {Op::kJmp, 31},
        {Op::kJf, 32},         {Op::kJt, 33},         {Op::kJfKeep, 34},       {Op::kJtKeep, 35},
        {Op::kScopeEnter, 40}, {Op::kScopeLeave, 41}, {Op::kScopeSave, 42},    {Op::kScopeLoad, 43},
        {Op::kScopeRoot, 44},  {Op::kPushScope, 45},  {Op::kLoad, 50},         {Op::kPred, 51},
        {Op::kLoadVar, 52},    {Op::kIterBegin, 60},  {Op::kIterNext, 61},     {Op::kIterEnd, 62},
        {Op::kIterOrder, 63},  {Op::kIterTake, 64},   {Op::kIterKeep, 65},     {Op::kIterRewind, 66},
        {Op::kIterWeight, 67}, {Op::kIterPick, 68},   {Op::kRandPermille, 70}, {Op::kRandPick, 71},
        {Op::kJTable, 72},     {Op::kBranchEnd, 73},  {Op::kHide, 74},         {Op::kEmit, 80},
        {Op::kState, 81},      {Op::kRet, 90},        {Op::kAbort, 91},        {Op::kStep, 92},
    }};
    std::set<std::string> names;
    for (const auto& [op, code] : codes) {
        CHECK(static_cast<std::uint16_t>(op) == code);
        CHECK(is_known_op(code));
        CHECK(op_name(code) != "?");
        names.insert(std::string(op_name(code)));
    }
    CHECK(names.size() == codes.size());  // mnemonics are unique
    for (const std::uint16_t unused :
         std::array<std::uint16_t, 12>{7, 8, 9, 19, 26, 46, 53, 69, 75, 82, 93, 1000}) {
        CHECK_FALSE(is_known_op(unused));
        CHECK(op_name(unused) == "?");
    }
    // Costs from the table of section 7.2.
    CHECK(op_base_wu(static_cast<std::uint16_t>(Op::kMul)) == 2);
    CHECK(op_base_wu(static_cast<std::uint16_t>(Op::kEmit)) == 8);
    CHECK(op_base_wu(static_cast<std::uint16_t>(Op::kState)) == 4);
    CHECK(op_base_wu(static_cast<std::uint16_t>(Op::kRet)) == 0);
}

TEST_CASE("events vm: stack operations and constants") {
    SUBCASE("push int, bool, const") {
        CHECK(eval([](ProgramBuilder& p) {
                  p.push_int(-7);
                  p.emit(Op::kRet);
              }).value == W(-7));
        CHECK(eval([](ProgramBuilder& p) {
                  p.push_bool(true);
                  p.emit(Op::kRet);
              }).value == 1);
        const Word third = WR(1, 3);
        CHECK(eval([&](ProgramBuilder& p) {
                  p.push_fixed(Fixed64::from_raw(third));
                  p.emit(Op::kRet);
              }).value == third);
    }
    SUBCASE("dup, drop, swap") {
        CHECK(eval([](ProgramBuilder& p) {
                  p.push_int(3);
                  p.emit(Op::kDup);
                  p.emit(Op::kAdd);
                  p.emit(Op::kRet);
              }).value == W(6));
        CHECK(eval([](ProgramBuilder& p) {
                  p.push_int(3);
                  p.push_int(9);
                  p.emit(Op::kDrop);
                  p.emit(Op::kRet);
              }).value == W(3));
        CHECK(eval([](ProgramBuilder& p) {
                  p.push_int(10);
                  p.push_int(4);
                  p.emit(Op::kSwap);
                  p.emit(Op::kSub);  // 4 - 10
                  p.emit(Op::kRet);
              }).value == W(-6));
    }
    SUBCASE("NOP does nothing and costs no work units") {
        const RunResult r = eval([](ProgramBuilder& p) {
            p.emit(Op::kNop);
            p.push_int(4);
            p.emit(Op::kNop);
            p.emit(Op::kRet);
        });
        CHECK(r.value == W(4));
        CHECK(r.instructions == 4);
        CHECK(r.wu == 1);  // only PUSH_INT counts
    }
    SUBCASE("an empty stack at RET has no value") {
        const RunResult r = eval([](ProgramBuilder& p) { p.emit(Op::kRet); });
        CHECK(r.ok);
        CHECK_FALSE(r.has_value);
        CHECK_FALSE(r.truthy());
    }
}

TEST_CASE("events vm: arithmetic table") {
    SUBCASE("add, sub, mul, div on ordinary values") {
        CHECK(eval2(Op::kAdd, W(2), W(3)).value == W(5));
        CHECK(eval2(Op::kAdd, WR(3, 2), WR(9, 4)).value == WR(15, 4));
        CHECK(eval2(Op::kSub, W(2), W(5)).value == W(-3));
        CHECK(eval2(Op::kMul, W(2), W(3)).value == W(6));
        CHECK(eval2(Op::kMul, WR(3, 2), WR(3, 2)).value == WR(9, 4));
        CHECK(eval2(Op::kMul, W(-3), WR(1, 2)).value == WR(-3, 2));
        CHECK(eval2(Op::kDiv, W(6), W(3)).value == W(2));
        CHECK(eval2(Op::kDiv, W(1), W(3)).value == WR(1, 3));
        CHECK(eval2(Op::kDiv, W(-7), W(2)).value == WR(-7, 2));
    }
    SUBCASE("multiplication and division truncate toward zero") {
        CHECK(eval2(Op::kMul, 3, 3).value == 0);     // 9 / 2^32
        CHECK(eval2(Op::kMul, -3, 3).value == 0);    // no rounding to -1
        CHECK(eval2(Op::kDiv, 1, W(2)).value == 0);  // raw 1 / 2
        CHECK(eval2(Op::kDiv, -1, W(2)).value == 0);
        CHECK(eval2(Op::kDiv, W(-1), W(3)).value == -WR(1, 3));
    }
    SUBCASE("add and sub saturate and count") {
        RunResult r = eval2(Op::kAdd, kWMax, 1);
        CHECK(r.ok);
        CHECK(r.value == kWMax);
        CHECK(r.counters.saturations == 1);
        r = eval2(Op::kAdd, kWMin, -1);
        CHECK(r.value == kWMin);
        CHECK(r.counters.saturations == 1);
        r = eval2(Op::kSub, kWMin, 1);
        CHECK(r.value == kWMin);
        CHECK(r.counters.saturations == 1);
        r = eval2(Op::kSub, kWMax, -1);
        CHECK(r.value == kWMax);
        CHECK(r.counters.saturations == 1);
        r = eval2(Op::kAdd, kWMax, kWMin);  // exact, no saturation
        CHECK(r.value == -1);
        CHECK(r.counters.saturations == 0);
    }
    SUBCASE("mul saturates, the exact minimum does not") {
        RunResult r = eval2(Op::kMul, W(1 << 30), W(1 << 30));
        CHECK(r.value == kWMax);
        CHECK(r.counters.saturations == 1);
        r = eval2(Op::kMul, W(-(1 << 30)), W(1 << 30));
        CHECK(r.value == kWMin);
        CHECK(r.counters.saturations == 1);
        r = eval2(Op::kMul, kWMin, W(-1));  // +2^63 does not fit
        CHECK(r.value == kWMax);
        CHECK(r.counters.saturations == 1);
        r = eval2(Op::kMul, kWMin, W(1));  // exactly the minimum
        CHECK(r.value == kWMin);
        CHECK(r.counters.saturations == 0);
    }
    SUBCASE("division by zero is 0 and counted, overflow saturates") {
        RunResult r = eval2(Op::kDiv, W(5), 0);
        CHECK(r.ok);
        CHECK(r.value == 0);
        CHECK(r.counters.div_zero == 1);
        CHECK(r.counters.saturations == 0);
        r = eval2(Op::kDiv, W(1 << 30), 1);  // 2^30 / 2^-32 = 2^62 in units: does not fit
        CHECK(r.value == kWMax);
        CHECK(r.counters.saturations == 1);
        r = eval2(Op::kDiv, kWMin, W(-1));
        CHECK(r.value == kWMax);
        CHECK(r.counters.saturations == 1);
        r = eval2(Op::kDiv, kWMin, W(1));
        CHECK(r.value == kWMin);
        CHECK(r.counters.saturations == 0);
    }
    SUBCASE("neg, min, max, clamp, trunc") {
        const auto unary = [](Op op, Word a) {
            return eval([&](ProgramBuilder& p) {
                p.push_fixed(Fixed64::from_raw(a));
                p.emit(op);
                p.emit(Op::kRet);
            });
        };
        CHECK(unary(Op::kNeg, W(5)).value == W(-5));
        CHECK(unary(Op::kNeg, W(-5)).value == W(5));
        RunResult r = unary(Op::kNeg, kWMin);
        CHECK(r.value == kWMax);
        CHECK(r.counters.saturations == 1);
        CHECK(eval2(Op::kMin, W(3), W(-2)).value == W(-2));
        CHECK(eval2(Op::kMax, W(3), W(-2)).value == W(3));
        CHECK(unary(Op::kTrunc, WR(15, 4)).value == W(3));
        CHECK(unary(Op::kTrunc, WR(-15, 4)).value == W(-3));
        CHECK(unary(Op::kTrunc, WR(1, 2)).value == 0);
        CHECK(unary(Op::kTrunc, kWMin).value == kWMin);

        const auto clamp = [](Word x, Word lo, Word hi) {
            return eval([&](ProgramBuilder& p) {
                       p.push_fixed(Fixed64::from_raw(x));
                       p.push_fixed(Fixed64::from_raw(lo));
                       p.push_fixed(Fixed64::from_raw(hi));
                       p.emit(Op::kClamp);
                       p.emit(Op::kRet);
                   })
                .value;
        };
        CHECK(clamp(W(5), W(0), W(10)) == W(5));
        CHECK(clamp(W(-5), W(0), W(10)) == W(0));
        CHECK(clamp(W(50), W(0), W(10)) == W(10));
        CHECK(clamp(W(5), W(8), W(2)) == W(8));  // hi < lo: the lower bound wins
    }
}

TEST_CASE("events vm: comparisons and NOT") {
    const std::array<Word, 3> v = {W(-1), W(0), W(1)};
    for (const Word a : v) {
        for (const Word b : v) {
            CHECK(eval2(Op::kEq, a, b).value == (a == b ? 1 : 0));
            CHECK(eval2(Op::kNe, a, b).value == (a != b ? 1 : 0));
            CHECK(eval2(Op::kLt, a, b).value == (a < b ? 1 : 0));
            CHECK(eval2(Op::kLe, a, b).value == (a <= b ? 1 : 0));
            CHECK(eval2(Op::kGt, a, b).value == (a > b ? 1 : 0));
            CHECK(eval2(Op::kGe, a, b).value == (a >= b ? 1 : 0));
        }
    }
    const auto not_of = [](Word x) {
        return eval([&](ProgramBuilder& p) {
                   p.push_fixed(Fixed64::from_raw(x));
                   p.emit(Op::kNot);
                   p.emit(Op::kRet);
               })
            .value;
    };
    CHECK(not_of(0) == 1);
    CHECK(not_of(1) == 0);
    CHECK(not_of(W(5)) == 0);
}

TEST_CASE("events vm: jumps and short-circuit evaluation") {
    SUBCASE("JF and JT consume the value") {
        const auto pick = [](bool cond, Op jump) {
            return eval([&](ProgramBuilder& p) {
                       const auto other = p.new_label();
                       p.push_bool(cond);
                       p.emit_jump(jump, other);
                       p.push_int(1);
                       p.emit(Op::kRet);
                       p.bind(other);
                       p.push_int(2);
                       p.emit(Op::kRet);
                   })
                .value;
        };
        CHECK(pick(false, Op::kJf) == W(2));
        CHECK(pick(true, Op::kJf) == W(1));
        CHECK(pick(true, Op::kJt) == W(2));
        CHECK(pick(false, Op::kJt) == W(1));
    }
    SUBCASE("a counting loop terminates and counts instructions") {
        const RunResult r = eval([](ProgramBuilder& p) {
            const auto loop = p.new_label();
            p.push_int(3);
            p.bind(loop);
            p.push_int(1);
            p.emit(Op::kSub);
            p.emit(Op::kDup);
            p.emit_jump(Op::kJt, loop);
            p.emit(Op::kRet);
        });
        CHECK(r.ok);
        CHECK(r.value == 0);
        CHECK(r.instructions == 1 + 3 * 4 + 1);
    }
    SUBCASE("AND with JF_KEEP skips the second operand") {
        const auto both = [](std::uint16_t first, std::uint16_t second, FakeHost& host) {
            const Program prog = build([&](ProgramBuilder& p) {
                const auto end = p.new_label();
                p.emit(Op::kPred, first, 0);
                p.emit_jump(Op::kJfKeep, end);
                p.emit(Op::kDrop);
                p.emit(Op::kPred, second, 0);
                p.bind(end);
                p.emit(Op::kRet);
            });
            return run(prog, host);
        };
        FakeHost host;
        CHECK_FALSE(both(kPredNo, kPredYes, host).truthy());
        CHECK(host.pred_calls == 1);  // the second predicate was never asked
        host.pred_calls = 0;
        CHECK(both(kPredYes, kPredYes, host).truthy());
        CHECK(host.pred_calls == 2);
    }
    SUBCASE("OR with JT_KEEP skips the second operand") {
        FakeHost host;
        const Program prog = build([&](ProgramBuilder& p) {
            const auto end = p.new_label();
            p.emit(Op::kPred, kPredYes, 0);
            p.emit_jump(Op::kJtKeep, end);
            p.emit(Op::kDrop);
            p.emit(Op::kPred, kPredNo, 0);
            p.bind(end);
            p.emit(Op::kRet);
        });
        CHECK(run(prog, host).truthy());
        CHECK(host.pred_calls == 1);
    }
}

// ---- scopes and host reads ---------------------------------------------------------------------

TEST_CASE("events vm: scopes, transitions and saved scopes") {
    FakeHost host;
    SUBCASE("enter a transition, read there, leave, read at the start") {
        const Program prog = build([](ProgramBuilder& p) {
            const auto empty = p.new_label();
            const auto end = p.new_label();
            p.emit_jump(Op::kScopeEnter, empty, kTransDynasty);
            p.emit(Op::kLoad, kHonor, 0);  // dynasty honor 55
            p.emit(Op::kScopeLeave);
            p.emit(Op::kLoad, kHonor, 0);  // character honor 22
            p.emit(Op::kAdd);
            p.emit_jump(Op::kJmp, end);
            p.bind(empty);
            p.push_int(-1);
            p.bind(end);
            p.emit(Op::kRet);
        });
        const RunResult r = run(prog, host);
        CHECK(r.ok);
        CHECK(r.value == W(77));
    }
    SUBCASE("an empty transition jumps") {
        const Program prog = build([](ProgramBuilder& p) {
            const auto empty = p.new_label();
            const auto end = p.new_label();
            p.emit_jump(Op::kScopeEnter, empty, kTransSpouse);
            p.push_int(1);
            p.emit(Op::kScopeLeave);
            p.emit_jump(Op::kJmp, end);
            p.bind(empty);
            p.push_int(0);
            p.bind(end);
            p.emit(Op::kRet);
        });
        CHECK(run(prog, host).value == W(0));
    }
    SUBCASE("save, enter elsewhere, load the saved scope, and ROOT") {
        const Program prog = build([](ProgramBuilder& p) {
            const auto none = p.new_label();
            const auto end = p.new_label();
            p.emit(Op::kScopeSave, 3);
            p.emit_jump(Op::kScopeEnter, none, kTransDynasty);
            p.emit_jump(Op::kScopeLoad, none, 3);  // the saved character 1, inside the dynasty
            p.emit(Op::kLoad, kHonor, 0);          // 22
            p.emit(Op::kScopeLeave);
            p.emit(Op::kScopeRoot);
            p.emit(Op::kLoad, kHonor, 0);  // 22 again
            p.emit(Op::kAdd);
            p.emit_jump(Op::kJmp, end);
            p.bind(none);
            p.push_int(-1);
            p.bind(end);
            p.emit(Op::kRet);
        });
        CHECK(run(prog, host).value == W(44));
    }
    SUBCASE("an unsaved slot jumps") {
        const Program prog = build([](ProgramBuilder& p) {
            const auto none = p.new_label();
            p.emit_jump(Op::kScopeLoad, none, 5);
            p.push_int(1);
            p.emit(Op::kRet);
            p.bind(none);
            p.push_int(2);
            p.emit(Op::kRet);
        });
        CHECK(run(prog, host).value == W(2));
    }
    SUBCASE("scope depth is limited") {
        // root + 8 entries = 9 scopes > kMaxScopeDepth (8).
        Program prog;
        for (int i = 0; i < 8; ++i) {
            prog.code.push_back(make_instr(Op::kScopeEnter, kTransLoop, 100));
        }
        prog.code.push_back(make_instr(Op::kRet));
        const RunResult r = run(prog, host);
        CHECK_FALSE(r.ok);
        CHECK(r.reason == AbortReason::ScopeDepth);
        CHECK(r.abort_pc == 7);
    }
}

TEST_CASE("events vm: host reads pass their arguments in order") {
    FakeHost host;
    host.vars_[{ch(1).word(), 42}] = W(9);
    host.vars_[{-1, 42}] = W(100);
    SUBCASE("LOAD with arguments") {
        const RunResult r = eval([](ProgramBuilder& p) {
            p.push_int(1);
            p.push_int(20);
            p.push_int(300);
            p.emit(Op::kLoad, kSumArgs, 3);
            p.emit(Op::kRet);
        });
        CHECK(r.value == W(321));
        FakeHost h2;
        const Program prog = build([](ProgramBuilder& p) {
            p.push_int(1);
            p.push_int(2);
            p.emit(Op::kLoad, kSumArgs, 2);
            p.emit(Op::kRet);
        });
        (void)run(prog, h2);
        REQUIRE(h2.last_args.size() == 2);
        CHECK(h2.last_args[0] == W(1));  // first pushed, first argument
        CHECK(h2.last_args[1] == W(2));
    }
    SUBCASE("PRED with an argument") {
        const Program prog = build([](ProgramBuilder& p) {
            p.push_int(7);
            p.emit(Op::kPred, kPredAdult, 1);
            p.emit(Op::kRet);
        });
        CHECK(run(prog, host).truthy());
        REQUIRE(host.last_args.size() == 1);
        CHECK(host.last_args[0] == W(7));
        CHECK_FALSE(run(prog, host, Kind::Trigger, Mode::Run, ch(3)).truthy());
    }
    SUBCASE("variables: scope and global, by key from the pool") {
        ProgramBuilder b;
        const auto key = static_cast<std::int32_t>(b.constant(42));
        b.emit(Op::kLoadVar, 0, key);
        b.emit(Op::kLoadVar, 1, key);
        b.emit(Op::kAdd);
        b.emit(Op::kRet);
        const Program prog = std::move(b.finish()).value();
        CHECK(run(prog, host).value == W(109));
    }
}

// ---- iterators ---------------------------------------------------------------------------------

TEST_CASE("events vm: count_, any_ and all_ over a list") {
    FakeHost host;
    const Scope root = dyn(10);
    SUBCASE("count of members with honor >= 30") {
        const Program prog = build([](ProgramBuilder& p) {
            p.push_int(0);
            const auto loop = p.new_label();
            const auto end = p.new_label();
            p.emit_jump(Op::kIterBegin, end, kListMembers);
            p.bind(loop);
            p.emit_jump(Op::kIterNext, end);
            p.emit(Op::kLoad, kHonor, 0);
            p.push_int(30);
            p.emit(Op::kGe);
            const auto skip = p.new_label();
            p.emit_jump(Op::kJf, skip);
            p.push_int(1);
            p.emit(Op::kAdd);
            p.bind(skip);
            p.emit_jump(Op::kJmp, loop);
            p.bind(end);
            p.emit(Op::kIterEnd);
            p.emit(Op::kRet);
        });
        const RunResult r = run(prog, host, Kind::Value, Mode::Run, root);
        CHECK(r.ok);
        CHECK(r.value == W(2));
    }
    SUBCASE("any_ exits early and leaves the element scope") {
        const Program prog = build([](ProgramBuilder& p) {
            const auto loop = p.new_label();
            const auto none = p.new_label();
            const auto found = p.new_label();
            const auto out = p.new_label();
            p.emit_jump(Op::kIterBegin, none, kListMembers);
            p.bind(loop);
            p.emit_jump(Op::kIterNext, none);
            p.emit(Op::kLoad, kHonor, 0);
            p.push_int(40);
            p.emit(Op::kGe);
            p.emit_jump(Op::kJf, loop);
            p.bind(found);
            p.emit(Op::kIterEnd);
            p.push_int(1);
            p.emit_jump(Op::kJmp, out);
            p.bind(none);
            p.emit(Op::kIterEnd);
            p.push_int(0);
            p.bind(out);
            p.emit(Op::kLoad, kHonor, 0);  // back in the root scope: the dynasty, 55
            p.emit(Op::kAdd);
            p.emit(Op::kRet);
        });
        const RunResult r = run(prog, host, Kind::Value, Mode::Run, root);
        CHECK(r.value == W(1 + 55));
        // the elements are [3, 1, 2, 4]: the first one already matches (character 3, honor 40)
        CHECK(host.load_calls == 1 + 1);
    }
    SUBCASE("all_ is vacuously true for an empty list") {
        const Program prog = build([](ProgramBuilder& p) {
            const auto loop = p.new_label();
            const auto bad = p.new_label();
            const auto ok = p.new_label();
            p.emit_jump(Op::kIterBegin, ok, 99);  // list 99 does not exist: empty
            p.bind(loop);
            p.emit_jump(Op::kIterNext, ok);
            p.emit(Op::kPred, kPredNo, 0);
            p.emit_jump(Op::kJf, bad);
            p.emit_jump(Op::kJmp, loop);
            p.bind(bad);
            p.emit(Op::kIterEnd);
            p.push_bool(false);
            const auto out = p.new_label();
            p.emit_jump(Op::kJmp, out);
            p.bind(ok);
            p.emit(Op::kIterEnd);
            p.push_bool(true);
            p.bind(out);
            p.emit(Op::kRet);
        });
        CHECK(run(prog, host, Kind::Trigger, Mode::Run, root).truthy());
    }
}

TEST_CASE("events vm: every_, ordered_ and random_ iterators produce effects in order") {
    FakeHost host;
    const Scope root = dyn(10);
    SUBCASE("every_ walks the list in the host order and targets each element") {
        const Program prog = build([](ProgramBuilder& p) {
            each(p, kListMembers, [](ProgramBuilder& q) { q.emit(Op::kEmit, 7, 0); });
            p.emit(Op::kRet);
        });
        const RunResult r = run(prog, host, Kind::Effect, Mode::Run, root);
        REQUIRE(r.ok);
        REQUIRE(r.commands.size() == 4);
        CHECK(r.commands.entry(0).target == ch(3));
        CHECK(r.commands.entry(1).target == ch(1));
        CHECK(r.commands.entry(2).target == ch(2));
        CHECK(r.commands.entry(3).target == ch(4));
    }
    SUBCASE("ordered_ sorts by value descending, then by id, and takes N") {
        const Program prog = build([](ProgramBuilder& p) {
            const auto loop = p.new_label();
            const auto end = p.new_label();
            p.emit_jump(Op::kIterBegin, end, kListMembers);
            p.emit(Op::kIterOrder, kHonor);
            p.emit(Op::kIterTake, 0, 3);
            p.bind(loop);
            p.emit_jump(Op::kIterNext, end);
            p.emit(Op::kEmit, 7, 0);
            p.emit_jump(Op::kJmp, loop);
            p.bind(end);
            p.emit(Op::kIterEnd);
            p.emit(Op::kRet);
        });
        const RunResult r = run(prog, host, Kind::Effect, Mode::Run, root);
        REQUIRE(r.ok);
        REQUIRE(r.commands.size() == 3);
        CHECK(r.commands.entry(0).target == ch(2));  // 40, lower id first
        CHECK(r.commands.entry(1).target == ch(3));  // 40
        CHECK(r.commands.entry(2).target == ch(1));  // 22
    }
    SUBCASE("filter with ITER_KEEP, then ordered_ over what is left") {
        const Program prog = build([](ProgramBuilder& p) {
            const auto filter = p.new_label();
            const auto filtered = p.new_label();
            const auto loop = p.new_label();
            const auto end = p.new_label();
            p.emit_jump(Op::kIterBegin, end, kListMembers);
            p.bind(filter);
            p.emit_jump(Op::kIterNext, filtered);
            p.emit(Op::kLoad, kHonor, 0);
            p.push_int(30);
            p.emit(Op::kLt);  // keep honor < 30: characters 1 (22) and 4 (10)
            p.emit(Op::kIterKeep);
            p.emit_jump(Op::kJmp, filter);
            p.bind(filtered);
            p.emit(Op::kIterRewind);
            p.emit(Op::kIterOrder, kHonor);
            p.bind(loop);
            p.emit_jump(Op::kIterNext, end);
            p.emit(Op::kEmit, 7, 0);
            p.emit_jump(Op::kJmp, loop);
            p.bind(end);
            p.emit(Op::kIterEnd);
            p.emit(Op::kRet);
        });
        const RunResult r = run(prog, host, Kind::Effect, Mode::Run, root);
        REQUIRE(r.ok);
        REQUIRE(r.commands.size() == 2);
        CHECK(r.commands.entry(0).target == ch(1));
        CHECK(r.commands.entry(1).target == ch(4));
    }
    SUBCASE("random_<list> picks exactly one element with one draw") {
        // filter honor >= 30 (characters 3 and 2), weight 1 each, pick one, emit once
        const Program prog = build([](ProgramBuilder& p) {
            const auto filter = p.new_label();
            const auto filtered = p.new_label();
            const auto after = p.new_label();
            p.emit_jump(Op::kIterBegin, after, kListMembers);
            p.bind(filter);
            p.emit_jump(Op::kIterNext, filtered);
            p.emit(Op::kLoad, kHonor, 0);
            p.push_int(30);
            p.emit(Op::kGe);
            p.emit(Op::kIterKeep);
            p.emit_jump(Op::kJmp, filter);
            p.bind(filtered);
            p.emit(Op::kIterRewind);
            p.emit(Op::kIterPick);
            p.emit_jump(Op::kIterNext, after);
            p.emit(Op::kEmit, 7, 0);
            p.bind(after);
            p.emit(Op::kIterEnd);
            p.emit(Op::kRet);
        });
        std::set<std::uint32_t> seen;
        for (std::uint64_t seed = 1; seed <= 40; ++seed) {
            FakeHost h(seed);
            const RunResult r = run(prog, h, Kind::Effect, Mode::Run, root);
            REQUIRE(r.ok);
            REQUIRE(r.commands.size() == 1);
            CHECK(r.counters.draws == 1);
            seen.insert(r.commands.entry(0).target.id);
        }
        CHECK(seen == std::set<std::uint32_t>{2, 3});
    }
    SUBCASE("random_<list> with weights never picks a zero weight") {
        const Program prog = build([](ProgramBuilder& p) {
            const auto filter = p.new_label();
            const auto filtered = p.new_label();
            const auto after = p.new_label();
            p.emit_jump(Op::kIterBegin, after, kListMembers);
            p.bind(filter);
            p.emit_jump(Op::kIterNext, filtered);
            p.emit(Op::kLoad, kHonor, 0);  // weight = honor - 40: only characters 2 and 3 have 0
            p.push_int(40);
            p.emit(Op::kSub);
            p.emit(Op::kDup);
            p.emit(Op::kIterWeight);
            p.emit(Op::kDrop);
            p.emit_jump(Op::kJmp, filter);
            p.bind(filtered);
            p.emit(Op::kIterPick);
            p.emit_jump(Op::kIterNext, after);
            p.emit(Op::kEmit, 7, 0);
            p.bind(after);
            p.emit(Op::kIterEnd);
            p.emit(Op::kRet);
        });
        for (std::uint64_t seed = 1; seed <= 20; ++seed) {
            FakeHost h(seed);
            const RunResult r = run(prog, h, Kind::Effect, Mode::Run, root);
            REQUIRE(r.ok);
            // all weights are <= 0: nothing is picked, no element, no draw
            CHECK(r.commands.empty());
            CHECK(r.counters.draws == 0);
        }
    }
}

TEST_CASE("events vm: fan-out limit truncates and counts; nesting is limited") {
    FakeHost host;
    const Scope root = dyn(10);
    SUBCASE("a list of 300 is cut at kMaxFanout") {
        const Program prog = build([](ProgramBuilder& p) {
            p.push_int(0);
            each(p, kListBig, [](ProgramBuilder& q) {
                q.push_int(1);
                q.emit(Op::kAdd);
            });
            p.emit(Op::kRet);
        });
        const RunResult r = run(prog, host, Kind::Value, Mode::Run, root);
        CHECK(r.ok);
        CHECK(r.value == W(static_cast<std::int32_t>(limits::kMaxFanout)));
        CHECK(r.counters.truncated == 1);
    }
    SUBCASE("a host that returns too many is cut by the machine too") {
        class Greedy final : public VmHost {
        public:
            Scope transition(Scope, std::uint16_t) override { return {}; }
            Word load(Scope, std::uint16_t, std::span<const Word>) override { return 0; }
            Word load_var(Scope, bool, std::uint64_t) override { return 0; }
            bool predicate(Scope, std::uint16_t, std::span<const Word>) override { return false; }
            bool list(Scope, std::uint16_t, std::uint32_t, std::vector<Scope>& out) override {
                for (std::uint32_t i = 1; i <= 1000; ++i) {
                    out.push_back(ch(i));
                }
                return false;
            }
            Pcg32& rng() override { return rng_; }
            Pcg32 rng_{1, 1};
        } greedy;
        const Program prog = build([](ProgramBuilder& p) {
            p.push_int(0);
            each(p, 1, [](ProgramBuilder& q) {
                q.push_int(1);
                q.emit(Op::kAdd);
            });
            p.emit(Op::kRet);
        });
        const RunResult r = run(prog, greedy, Kind::Value, Mode::Run, root);
        CHECK(r.value == W(static_cast<std::int32_t>(limits::kMaxFanout)));
        CHECK(r.counters.truncated == 1);
    }
    SUBCASE("four nested iterators abort: iter_nest") {
        Program prog;
        for (int i = 0; i < 4; ++i) {
            prog.code.push_back(make_instr(Op::kIterBegin, kListMembers, 1000));
        }
        prog.code.push_back(make_instr(Op::kRet));
        const RunResult r = run(prog, host, Kind::Value, Mode::Run, root);
        CHECK_FALSE(r.ok);
        CHECK(r.reason == AbortReason::IterNest);
        CHECK(r.abort_pc == 3);
    }
    SUBCASE("three nested iterators are fine") {
        // every_member { enter dynasty; every_member { enter dynasty; every_member { +1 } } }
        const auto enter_dynasty = [](ProgramBuilder& q, const std::function<void(ProgramBuilder&)>& inner) {
            const auto skip = q.new_label();
            q.emit_jump(Op::kScopeEnter, skip, kTransDynasty);
            inner(q);
            q.emit(Op::kScopeLeave);
            q.bind(skip);
        };
        const Program prog = build([&](ProgramBuilder& p) {
            p.push_int(0);
            each(p, kListMembers, [&](ProgramBuilder& a) {
                enter_dynasty(a, [&](ProgramBuilder& a2) {
                    each(a2, kListMembers, [&](ProgramBuilder& b) {
                        enter_dynasty(b, [&](ProgramBuilder& b2) {
                            each(b2, kListMembers, [](ProgramBuilder& c) {
                                c.push_int(1);
                                c.emit(Op::kAdd);
                            });
                        });
                    });
                });
            });
            p.emit(Op::kRet);
        });
        const RunResult r = run(prog, host, Kind::Value, Mode::Run, root);
        CHECK(r.ok);
        CHECK(r.value == W(64));  // 4 x 4 x 4
    }
}

// ---- randomness --------------------------------------------------------------------------------

TEST_CASE("events vm: RAND_PERMILLE takes exactly one draw from the host stream") {
    const auto roll = [](std::int32_t ppm) {
        return build([&](ProgramBuilder& p) {
            p.push_int(ppm);
            p.emit(Op::kRandPermille, 0, 0);
            p.emit(Op::kRet);
        });
    };
    for (std::uint64_t seed = 1; seed <= 30; ++seed) {
        for (const std::int32_t ppm : {0, 1, 250000, 999999, 1000000, 2000000, -5}) {
            FakeHost host(seed);
            const RunResult r = run(roll(ppm), host);
            Pcg32 reference(seed, 7);
            const std::uint32_t draw = reference.bounded(1'000'000);
            REQUIRE(r.ok);
            CHECK(r.counters.draws == 1);
            CHECK(r.truthy() == (static_cast<std::int64_t>(draw) < ppm));
            // exactly one bounded() call: both streams are at the same position afterwards
            CHECK(host.rng_ == reference);
        }
    }
}

TEST_CASE("events vm: RAND_PICK is a weighted choice with one draw, none when no weight is positive") {
    const auto pick2 = [](std::int32_t w0, std::int32_t w1) {
        return build([&](ProgramBuilder& p) {
            p.push_int(w0);
            p.push_int(w1);
            p.emit(Op::kRandPick, 2, 0);
            p.emit(Op::kRet);
        });
    };
    SUBCASE("30 / 70 against a reference of the documented scaling") {
        int zeros = 0;
        for (std::uint64_t seed = 1; seed <= 400; ++seed) {
            FakeHost host(seed);
            const RunResult r = run(pick2(30, 70), host);
            REQUIRE(r.ok);
            CHECK(r.counters.draws == 1);
            // weights 30 and 70 (Q32.32): bit widths 39 and 2 -> shift 9; scaled are exact multiples
            Pcg32 reference(seed, 7);
            const std::uint64_t scaled0 = (static_cast<std::uint64_t>(30) << 32) >> 9;
            const std::uint64_t scaled1 = (static_cast<std::uint64_t>(70) << 32) >> 9;
            const std::uint64_t r0 = reference.bounded(static_cast<std::uint32_t>(scaled0 + scaled1));
            const int expected = r0 < scaled0 ? 0 : 1;
            CHECK(r.value == W(expected));
            CHECK(host.rng_ == reference);
            zeros += expected == 0 ? 1 : 0;
        }
        CHECK(zeros > 80);  // about 30% of 400
        CHECK(zeros < 160);
    }
    SUBCASE("a zero or negative weight is never picked") {
        for (std::uint64_t seed = 1; seed <= 50; ++seed) {
            FakeHost host(seed);
            CHECK(run(pick2(0, 5), host).value == W(1));
            CHECK(run(pick2(-3, 5), host).value == W(1));
            CHECK(run(pick2(7, 0), host).value == W(0));
        }
    }
    SUBCASE("no positive weight: -1 and the stream is untouched") {
        FakeHost host(9);
        const Pcg32 before = host.rng_;
        const RunResult r = run(pick2(0, -1), host);
        CHECK(r.value == W(-1));
        CHECK(r.counters.draws == 0);
        CHECK(host.rng_ == before);
    }
    SUBCASE("huge weights still fit the draw") {
        FakeHost host(3);
        const Program prog = build([](ProgramBuilder& p) {
            p.push_fixed(Fixed64::from_raw(kWMax));
            p.push_fixed(Fixed64::from_raw(kWMax));
            p.push_fixed(Fixed64::from_raw(1));
            p.emit(Op::kRandPick, 3, 0);
            p.emit(Op::kRet);
        });
        const RunResult r = run(prog, host);
        CHECK(r.ok);
        CHECK((r.value == W(0) || r.value == W(1)));  // the weight 1 scales to nothing
    }
}

namespace {

// random_list = { 30 = { EMIT 2 }  70 = { EMIT 3; EMIT 4 } } as the compiler lays it out:
// weights; RAND_PICK n; JTABLE n; JMP b0; JMP b1; JMP join (the "nothing picked" entry); branches
// each ending in BRANCH_END and a JMP to the join.
void emit_random_list(ProgramBuilder& p, std::int32_t w0 = 30, std::int32_t w1 = 70, std::int32_t w2 = -1) {
    const bool three = w2 >= 0;
    const auto b0 = p.new_label();
    const auto b1 = p.new_label();
    const auto b2 = p.new_label();
    const auto join = p.new_label();
    p.push_int(w0);
    p.push_int(w1);
    if (three) {
        p.push_int(w2);
    }
    p.emit(Op::kRandPick, three ? 3 : 2, 0);
    p.emit(Op::kJTable, three ? 3 : 2, 0);
    p.emit_jump(Op::kJmp, b0);
    p.emit_jump(Op::kJmp, b1);
    if (three) {
        p.emit_jump(Op::kJmp, b2);
    }
    p.emit_jump(Op::kJmp, join);
    p.bind(b0);
    p.emit(Op::kEmit, 2, 0);
    p.emit(Op::kBranchEnd);
    p.emit_jump(Op::kJmp, join);
    p.bind(b1);
    p.emit(Op::kEmit, 3, 0);
    p.emit(Op::kEmit, 4, 0);
    p.emit(Op::kBranchEnd);
    p.emit_jump(Op::kJmp, join);
    if (three) {
        p.bind(b2);
        p.emit(Op::kEmit, 5, 0);
        p.emit(Op::kBranchEnd);
        p.emit_jump(Op::kJmp, join);
    }
    p.bind(join);
}

Program random_list_program(std::int32_t w0 = 30, std::int32_t w1 = 70, std::int32_t w2 = -1) {
    return build([&](ProgramBuilder& p) {
        emit_random_list(p, w0, w1, w2);
        p.emit(Op::kRet);
    });
}

std::vector<std::uint16_t> codes_of(const OpBuffer& buffer) {
    std::vector<std::uint16_t> out;
    for (const auto& e : buffer.entries()) {
        out.push_back(e.code);
    }
    return out;
}

}  // namespace

TEST_CASE("events vm: random_list runs exactly one branch") {
    const Program prog = random_list_program();
    int first = 0;
    int second = 0;
    for (std::uint64_t seed = 1; seed <= 300; ++seed) {
        FakeHost host(seed);
        const RunResult r = run(prog, host, Kind::Effect);
        REQUIRE(r.ok);
        CHECK(r.counters.draws == 1);
        const auto codes = codes_of(r.commands);
        if (codes == std::vector<std::uint16_t>{2}) {
            ++first;
        } else {
            CHECK(codes == std::vector<std::uint16_t>{3, 4});
            ++second;
        }
    }
    CHECK(first + second == 300);
    CHECK(first > 60);
    CHECK(second > 150);
}

// ---- effects, limits, rollback -----------------------------------------------------------------

TEST_CASE("events vm: effects land in the buffers in program order with their scope and arguments") {
    FakeHost host;
    const Program prog = build([](ProgramBuilder& p) {
        const auto skip = p.new_label();
        p.push_int(5);
        p.push_int(-2);
        p.emit(Op::kEmit, 10, 2);
        p.emit_jump(Op::kScopeEnter, skip, kTransDynasty);
        p.push_int(8);
        p.emit(Op::kState, 20, 1);
        p.emit(Op::kEmit, 11, 0);
        p.emit(Op::kScopeLeave);
        p.bind(skip);
        p.emit(Op::kRet);
    });
    const RunResult r = run(prog, host, Kind::Effect);
    REQUIRE(r.ok);
    REQUIRE(r.commands.size() == 2);
    CHECK(r.commands.entry(0).code == 10);
    CHECK(r.commands.entry(0).target == ch(1));
    REQUIRE(r.commands.args_of(0).size() == 2);
    CHECK(r.commands.args_of(0)[0] == W(5));
    CHECK(r.commands.args_of(0)[1] == W(-2));
    CHECK(r.commands.entry(1).code == 11);
    CHECK(r.commands.entry(1).target == dyn(10));
    REQUIRE(r.state_ops.size() == 1);
    CHECK(r.state_ops.entry(0).code == 20);
    CHECK(r.state_ops.entry(0).target == dyn(10));
    CHECK(r.state_ops.args_of(0)[0] == W(8));
}

TEST_CASE("events vm: effects are refused in a trigger or a value program") {
    FakeHost host;
    const Program prog = build([](ProgramBuilder& p) {
        p.emit(Op::kEmit, 1, 0);
        p.emit(Op::kRet);
    });
    for (const Kind kind : {Kind::Trigger, Kind::Value}) {
        const RunResult r = run(prog, host, kind);
        CHECK_FALSE(r.ok);
        CHECK(r.reason == AbortReason::BadProgram);
    }
}

TEST_CASE("events vm: limits abort with a reason and roll back") {
    FakeHost host(5);
    SUBCASE("the instruction budget stops an endless loop") {
        const Program prog = build([](ProgramBuilder& p) {
            const auto loop = p.new_label();
            p.bind(loop);
            p.emit_jump(Op::kJmp, loop);
        });
        RunResult r = run(prog, host);
        CHECK_FALSE(r.ok);
        CHECK(r.reason == AbortReason::Instr);
        CHECK(r.instructions == limits::kMaxTriggerInstr);
        r = run(prog, host, Kind::Effect);
        CHECK(r.reason == AbortReason::Instr);
        CHECK(r.instructions == limits::kMaxEffectInstr);

        Vm vm;
        RunOptions options;
        options.max_instr = 25;
        r = vm.run(prog, host, options);
        CHECK(r.instructions == 25);
    }
    SUBCASE("more than kMaxCommandsPerRun effects abort and nothing is handed over") {
        const auto emitter = [](int count) {
            return build([&](ProgramBuilder& p) {
                for (int i = 0; i < count; ++i) {
                    p.emit(Op::kEmit, 1, 0);
                }
                p.emit(Op::kRet);
            });
        };
        RunResult r = run(emitter(static_cast<int>(limits::kMaxCommandsPerRun)), host, Kind::Effect);
        CHECK(r.ok);
        CHECK(r.commands.size() == limits::kMaxCommandsPerRun);
        r = run(emitter(static_cast<int>(limits::kMaxCommandsPerRun) + 1), host, Kind::Effect);
        CHECK_FALSE(r.ok);
        CHECK(r.reason == AbortReason::Commands);
        CHECK(r.commands.empty());
    }
    SUBCASE("more than kMaxStateOpsPerRun operations abort") {
        const Program prog = build([](ProgramBuilder& p) {
            for (std::size_t i = 0; i <= limits::kMaxStateOpsPerRun; ++i) {
                p.emit(Op::kState, 1, 0);
            }
            p.emit(Op::kRet);
        });
        const RunResult r = run(prog, host, Kind::Effect);
        CHECK_FALSE(r.ok);
        CHECK(r.reason == AbortReason::StateOps);
        CHECK(r.state_ops.empty());
    }
    SUBCASE("a lowered budget in the options is honoured") {
        const Program prog = build([](ProgramBuilder& p) {
            p.emit(Op::kEmit, 1, 0);
            p.emit(Op::kEmit, 1, 0);
            p.emit(Op::kEmit, 1, 0);
            p.emit(Op::kRet);
        });
        Vm vm;
        RunOptions options;
        options.kind = Kind::Effect;
        options.max_commands = 2;
        const RunResult r = vm.run(prog, host, options);
        CHECK(r.reason == AbortReason::Commands);
        CHECK(r.abort_pc == 2);
    }
    SUBCASE("the value stack is bounded") {
        Program prog;  // bypasses validate(): the machine must protect itself
        for (std::size_t i = 0; i <= limits::kMaxStack; ++i) {
            prog.code.push_back(make_instr(Op::kPushInt, 0, 1));
        }
        prog.code.push_back(make_instr(Op::kRet));
        const RunResult r = run(prog, host);
        CHECK_FALSE(r.ok);
        CHECK(r.reason == AbortReason::Stack);
        CHECK(r.abort_pc == limits::kMaxStack);
    }
    SUBCASE("explicit ABORT carries its operand") {
        const Program prog = build([](ProgramBuilder& p) { p.emit(Op::kAbort, 77); });
        const RunResult r = run(prog, host);
        CHECK(r.reason == AbortReason::Explicit);
        CHECK(r.abort_detail == 77);
    }
}

TEST_CASE("events vm: a malformed program aborts instead of corrupting memory") {
    FakeHost host;
    const auto raw = [&](std::initializer_list<Instr> code, std::vector<std::int64_t> consts = {}) {
        Program prog;
        prog.code.assign(code.begin(), code.end());
        prog.consts = std::move(consts);
        return run(prog, host);
    };
    CHECK(raw({make_instr(Op::kAdd), make_instr(Op::kRet)}).reason == AbortReason::BadProgram);
    CHECK(raw({make_instr(Op::kJmp, 0, 50)}).reason == AbortReason::BadProgram);
    CHECK(raw({make_instr(Op::kJmp, 0, -5)}).reason == AbortReason::BadProgram);
    CHECK(raw({make_instr(Op::kPushConst, 0, 3), make_instr(Op::kRet)}).reason == AbortReason::BadProgram);
    CHECK(raw({make_instr(Op::kPushInt, 0, 1)}).reason == AbortReason::BadProgram);  // runs off the end
    CHECK(raw({Instr{999, 0, 0}, make_instr(Op::kRet)}).reason == AbortReason::BadProgram);
    CHECK(raw({make_instr(Op::kScopeLeave), make_instr(Op::kRet)}).reason == AbortReason::BadProgram);
    CHECK(raw({make_instr(Op::kIterNext, 0, 0), make_instr(Op::kRet)}).reason == AbortReason::BadProgram);
    CHECK(raw({make_instr(Op::kLoad, 1, 99), make_instr(Op::kRet)}).reason == AbortReason::BadProgram);
    CHECK(raw({make_instr(Op::kScopeSave, 200), make_instr(Op::kRet)}).reason == AbortReason::BadProgram);
}

TEST_CASE("events vm: a failed run hands over nothing, keeps the draws spent, and leaves the host alone") {
    // roll twice, emit, change state, then run out of commands: everything but the draws is dropped
    const Program prog = build([](ProgramBuilder& p) {
        p.push_int(500000);
        p.emit(Op::kRandPermille, 0, 0);
        p.emit(Op::kDrop);
        p.push_int(500000);
        p.emit(Op::kRandPermille, 0, 0);
        p.emit(Op::kDrop);
        p.emit(Op::kEmit, 1, 0);
        p.emit(Op::kState, 2, 0);
        p.emit(Op::kEmit, 1, 0);
        p.emit(Op::kRet);
    });
    FakeHost host(11);
    Vm vm;
    RunOptions options;
    options.kind = Kind::Effect;
    options.max_commands = 1;
    const RunResult r = vm.run(prog, host, options);
    CHECK_FALSE(r.ok);
    CHECK(r.reason == AbortReason::Commands);
    CHECK(r.commands.empty());
    CHECK(r.state_ops.empty());
    CHECK_FALSE(r.has_value);
    CHECK(r.counters.draws == 2);
    Pcg32 reference(11, 7);
    (void)reference.bounded(1'000'000);
    (void)reference.bounded(1'000'000);
    CHECK(host.rng_ == reference);  // the two rolls stay spent, deterministically
    // and the same failure repeats exactly
    FakeHost again(11);
    CHECK(vm.run(prog, again, options).to_bytes() == r.to_bytes());
}

// ---- Trace -------------------------------------------------------------------------------------

TEST_CASE("events vm: Trace reports the leaves and the first false one") {
    // honor >= 30 and age predicate: the characters of FakeHost have honor 22 (character 1)
    const Program prog = build([](ProgramBuilder& p) {
        const auto end = p.new_label();
        p.emit(Op::kPred, kPredAdult, 0);
        p.emit_jump(Op::kJfKeep, end);
        p.emit(Op::kDrop);
        p.emit(Op::kLoad, kHonor, 0);
        p.push_int(30);
        p.emit(Op::kGe);
        p.bind(end);
        p.emit(Op::kRet);
    });
    FakeHost host;
    TraceRecorder tracer;
    Vm vm;
    RunOptions options;
    options.mode = Mode::Trace;
    options.root = ch(1);
    options.tracer = &tracer;
    const RunResult r = vm.run(prog, host, options);
    CHECK(r.ok);
    CHECK_FALSE(r.truthy());
    REQUIRE(tracer.leaves().size() == 2);
    CHECK(tracer.leaves()[0].op == Op::kPred);
    CHECK(tracer.leaves()[0].result);
    const TraceLeaf* reason = tracer.first_false();
    REQUIRE(reason != nullptr);
    CHECK(reason->op == Op::kGe);
    CHECK(reason->v0 == W(22));  // "now 22"
    CHECK(reason->v1 == W(30));  // "need 30"
    CHECK(reason->pc == 5);

    // the Run of the same program says the same thing
    FakeHost run_host;
    CHECK(run(prog, run_host).value == r.value);
}

TEST_CASE("events vm: Trace and Preview take no draws, produce no buffers and leave the host alone") {
    const Program effects = random_list_program();
    const Program chance = build([](ProgramBuilder& p) {
        p.push_int(250000);
        p.emit(Op::kRandPermille, 0, 0);
        p.emit(Op::kRet);
    });
    for (const Mode mode : {Mode::Trace, Mode::Preview}) {
        for (const Program* prog : {&effects, &chance}) {
            FakeHost host(21);
            const Pcg32 before = host.rng_;
            const auto calls_before = host.total_calls();
            const RunResult r = run(*prog, host, Kind::Effect, mode);
            CHECK(r.ok);
            CHECK(r.counters.draws == 0);
            CHECK(r.commands.empty());
            CHECK(r.state_ops.empty());
            CHECK(host.rng_ == before);
            CHECK(host.total_calls() == calls_before);  // no host reads were needed for these programs
        }
    }
}

TEST_CASE("events vm: Trace of a trigger equals Run when no dice are involved") {
    const Program prog = build([](ProgramBuilder& p) {
        const auto end = p.new_label();
        p.emit(Op::kLoad, kHonor, 0);
        p.push_int(20);
        p.emit(Op::kGt);
        p.emit_jump(Op::kJfKeep, end);
        p.emit(Op::kDrop);
        p.emit(Op::kPred, kPredAdult, 0);
        p.bind(end);
        p.emit(Op::kRet);
    });
    for (std::uint32_t id = 1; id <= 4; ++id) {
        FakeHost a;
        FakeHost b;
        const RunResult run_r = run(prog, a, Kind::Trigger, Mode::Run, ch(id));
        const RunResult trace_r = run(prog, b, Kind::Trigger, Mode::Trace, ch(id));
        CHECK(run_r.value == trace_r.value);
        CHECK(run_r.ok == trace_r.ok);
    }
}

TEST_CASE("events vm: Trace does not execute effects") {
    const Program prog = build([](ProgramBuilder& p) {
        p.emit(Op::kEmit, 1, 0);
        p.emit(Op::kState, 2, 0);
        p.push_bool(true);
        p.emit(Op::kRet);
    });
    FakeHost host;
    const RunResult r = run(prog, host, Kind::Effect, Mode::Trace);
    CHECK(r.ok);
    CHECK(r.commands.empty());
    CHECK(r.state_ops.empty());
    CHECK(r.truthy());
}

TEST_CASE("events vm: a random_chance leaf in Trace is reported with its chance and counts as true") {
    const Program prog = build([](ProgramBuilder& p) {
        p.push_int(125000);  // 12.5 %
        p.emit(Op::kRandPermille, 0, 0);
        p.emit(Op::kRet);
    });
    FakeHost host;
    TraceRecorder tracer;
    Vm vm;
    RunOptions options;
    options.mode = Mode::Trace;
    options.tracer = &tracer;
    const RunResult r = vm.run(prog, host, options);
    CHECK(r.truthy());
    REQUIRE(tracer.leaves().size() == 1);
    CHECK(tracer.leaves()[0].op == Op::kRandPermille);
    CHECK(tracer.leaves()[0].v0 == 125000);
}

// ---- Preview -----------------------------------------------------------------------------------

TEST_CASE("events vm: Preview of random_list lists every branch with its share") {
    FakeHost host;
    PreviewRecorder sink;
    Vm vm;
    RunOptions options;
    options.mode = Mode::Preview;
    options.kind = Kind::Effect;
    options.root = ch(1);
    options.preview = &sink;
    const RunResult r = vm.run(random_list_program(), host, options);
    REQUIRE(r.ok);
    const auto& e = sink.events();
    REQUIRE(e.size() == 7);
    CHECK(e[0].type == PreviewEvent::Type::GroupBegin);
    CHECK(e[0].group == PreviewGroup::Branch);
    CHECK(e[0].index == 0);
    CHECK(e[0].value == 300000);
    CHECK(e[1].type == PreviewEvent::Type::Line);
    CHECK(e[1].code == 2);
    CHECK(e[2].type == PreviewEvent::Type::GroupEnd);
    CHECK(e[3].type == PreviewEvent::Type::GroupBegin);
    CHECK(e[3].index == 1);
    CHECK(e[3].value == 700000);
    CHECK(e[4].code == 3);
    CHECK(e[5].code == 4);
    CHECK(e[6].type == PreviewEvent::Type::GroupEnd);
}

TEST_CASE("events vm: shares of a random_list add up to exactly 1 000 000") {
    for (const auto& weights : {std::array<std::int32_t, 3>{1, 1, 1}, {1, 2, 4}, {7, 0, 3}, {1, 1000, 3}}) {
        FakeHost host;
        PreviewRecorder sink;
        Vm vm;
        RunOptions options;
        options.mode = Mode::Preview;
        options.kind = Kind::Effect;
        options.preview = &sink;
        const RunResult r = vm.run(random_list_program(weights[0], weights[1], weights[2]), host, options);
        REQUIRE(r.ok);
        std::uint32_t sum = 0;
        int begins = 0;
        for (const PreviewEvent& ev : sink.events()) {
            if (ev.type == PreviewEvent::Type::GroupBegin) {
                REQUIRE(ev.group == PreviewGroup::Branch);
                sum += ev.value;
                ++begins;
            }
        }
        CHECK(sum == 1'000'000);
        int positive = 0;
        for (const std::int32_t w : weights) {
            positive += w > 0 ? 1 : 0;
        }
        CHECK(begins == positive);  // a branch with weight 0 is not shown
    }
}

namespace {

// Depth of nested groups never goes negative and ends at zero.
bool balanced(const std::vector<PreviewEvent>& events) {
    int depth = 0;
    for (const PreviewEvent& ev : events) {
        if (ev.type == PreviewEvent::Type::GroupBegin) {
            ++depth;
        } else if (ev.type == PreviewEvent::Type::GroupEnd) {
            if (--depth < 0) {
                return false;
            }
        }
    }
    return depth == 0;
}

RunResult preview(const Program& prog, FakeHost& host, PreviewRecorder& sink, Scope root = ch(1)) {
    Vm vm;
    RunOptions options;
    options.mode = Mode::Preview;
    options.kind = Kind::Effect;
    options.root = root;
    options.preview = &sink;
    return vm.run(prog, host, options);
}

// One effect program with everything the tooltip has to cover: plain effects, a state line, a
// random_list, a chance block, and an iterator.
Program combined_program() {
    return build([](ProgramBuilder& p) {
        const auto skip = p.new_label();
        p.push_int(5);
        p.emit(Op::kEmit, 1, 1);
        p.push_int(1);
        p.emit(Op::kState, 30, 1);
        emit_random_list(p, 30, 70);
        p.push_int(250000);
        p.emit(Op::kRandPermille, 0, 1);
        p.emit_jump(Op::kJf, skip);
        p.emit(Op::kEmit, 6, 0);
        p.emit(Op::kBranchEnd);
        p.bind(skip);
        const auto none = p.new_label();
        p.emit_jump(Op::kScopeEnter, none, kTransDynasty);
        each(p, kListMembers, [](ProgramBuilder& q) { q.emit(Op::kEmit, 7, 0); });
        p.emit(Op::kScopeLeave);
        p.bind(none);
        p.emit(Op::kRet);
    });
}

}  // namespace

TEST_CASE("events vm: Preview of a chance block, a hidden effect and a state line") {
    FakeHost host;
    PreviewRecorder sink;
    const Program prog = build([](ProgramBuilder& p) {
        const auto skip = p.new_label();
        p.push_int(250000);
        p.emit(Op::kRandPermille, 0, 1);
        p.emit_jump(Op::kJf, skip);
        p.emit(Op::kEmit, 6, 0);
        p.emit(Op::kBranchEnd);
        p.bind(skip);
        p.emit(Op::kHide, 1);
        p.emit(Op::kEmit, 8, 0);
        p.emit(Op::kHide, 0);
        p.push_int(3);
        p.emit(Op::kState, 9, 1);
        p.emit(Op::kRet);
    });
    const RunResult r = preview(prog, host, sink);
    REQUIRE(r.ok);
    const auto& e = sink.events();
    REQUIRE(e.size() == 4);
    CHECK(e[0].type == PreviewEvent::Type::GroupBegin);
    CHECK(e[0].group == PreviewGroup::Chance);
    CHECK(e[0].value == 250000);
    CHECK(e[1].type == PreviewEvent::Type::Line);
    CHECK(e[1].line_kind == PreviewLineKind::Effect);
    CHECK(e[1].code == 6);
    CHECK(e[2].type == PreviewEvent::Type::GroupEnd);
    CHECK(e[3].type == PreviewEvent::Type::Line);  // the hidden effect (8) is not listed
    CHECK(e[3].line_kind == PreviewLineKind::State);
    CHECK(e[3].code == 9);
    CHECK(e[3].args == std::vector<Word>{W(3)});

    // a zero chance shows nothing for the block
    sink.clear();
    const Program never = build([](ProgramBuilder& p) {
        const auto skip = p.new_label();
        p.push_int(0);
        p.emit(Op::kRandPermille, 0, 1);
        p.emit_jump(Op::kJf, skip);
        p.emit(Op::kEmit, 6, 0);
        p.emit(Op::kBranchEnd);
        p.bind(skip);
        p.emit(Op::kRet);
    });
    REQUIRE(preview(never, host, sink).ok);
    CHECK(sink.events().empty());
}

TEST_CASE("events vm: Preview of an iterator shows the group, each element and its effects") {
    FakeHost host;
    PreviewRecorder sink;
    const Program prog = build([](ProgramBuilder& p) {
        each(p, kListMembers, [](ProgramBuilder& q) { q.emit(Op::kEmit, 7, 0); });
        p.emit(Op::kRet);
    });
    REQUIRE(preview(prog, host, sink, dyn(10)).ok);
    const auto& e = sink.events();
    REQUIRE(e.size() == 2 + 4 * 2);
    CHECK(e.front().type == PreviewEvent::Type::GroupBegin);
    CHECK(e.front().group == PreviewGroup::Each);
    CHECK(e.front().index == kListMembers);
    CHECK(e.front().value == 4);
    CHECK(e[1].type == PreviewEvent::Type::Element);
    CHECK(e[1].scope == ch(3));
    CHECK(e[2].type == PreviewEvent::Type::Line);
    CHECK(e[2].scope == ch(3));
    CHECK(e.back().type == PreviewEvent::Type::GroupEnd);
    CHECK(balanced(e));
}

TEST_CASE("events vm: Preview of random_<list> says one of N candidates") {
    FakeHost host;
    PreviewRecorder sink;
    const Program prog = build([](ProgramBuilder& p) {
        const auto filter = p.new_label();
        const auto filtered = p.new_label();
        const auto after = p.new_label();
        p.emit_jump(Op::kIterBegin, after, kListMembers);
        p.bind(filter);
        p.emit_jump(Op::kIterNext, filtered);
        p.push_bool(true);
        p.emit(Op::kIterKeep);
        p.emit_jump(Op::kJmp, filter);
        p.bind(filtered);
        p.emit(Op::kIterRewind);
        p.emit(Op::kIterPick);
        p.emit_jump(Op::kIterNext, after);
        p.emit(Op::kEmit, 7, 0);
        p.bind(after);
        p.emit(Op::kIterEnd);
        p.emit(Op::kRet);
    });
    const Pcg32 before = host.rng_;
    REQUIRE(preview(prog, host, sink, dyn(10)).ok);
    CHECK(host.rng_ == before);
    int picks = 0;
    int lines = 0;
    for (const PreviewEvent& ev : sink.events()) {
        if (ev.type == PreviewEvent::Type::IterPick) {
            ++picks;
            CHECK(ev.index == 4);
        }
        lines += ev.type == PreviewEvent::Type::Line ? 1 : 0;
    }
    CHECK(picks == 1);
    CHECK(lines == 1);  // one representative, not four
    CHECK(balanced(sink.events()));
}

TEST_CASE("events vm: shown = applied: every command a Run can produce is among the Preview lines") {
    const Program prog = combined_program();
    FakeHost preview_host;
    PreviewRecorder sink;
    REQUIRE(preview(prog, preview_host, sink).ok);
    CHECK(balanced(sink.events()));
    std::set<std::uint16_t> shown;
    for (const PreviewEvent& ev : sink.events()) {
        if (ev.type == PreviewEvent::Type::Line) {
            shown.insert(ev.code);
        }
    }
    CHECK(shown == std::set<std::uint16_t>{1, 2, 3, 4, 6, 7, 30});

    std::set<std::uint16_t> produced;
    for (std::uint64_t seed = 1; seed <= 200; ++seed) {
        FakeHost host(seed);
        const RunResult r = run(prog, host, Kind::Effect, Mode::Run, ch(1));
        REQUIRE(r.ok);
        for (const auto& entry : r.commands.entries()) {
            CHECK(shown.contains(entry.code));
            produced.insert(entry.code);
        }
        for (const auto& entry : r.state_ops.entries()) {
            CHECK(shown.contains(entry.code));
            produced.insert(entry.code);
        }
        // a run takes one draw for the list and one for the chance block
        CHECK(r.counters.draws == 2);
    }
    CHECK(produced == shown);  // and every shown line can really happen
}

TEST_CASE("events vm: nested random groups stay balanced in Preview") {
    // chance block { random_list { EMIT 2 | EMIT 3, EMIT 4 } }
    const Program prog = build([](ProgramBuilder& p) {
        const auto skip = p.new_label();
        p.push_int(500000);
        p.emit(Op::kRandPermille, 0, 1);
        p.emit_jump(Op::kJf, skip);
        emit_random_list(p, 1, 1);
        p.emit(Op::kBranchEnd);
        p.bind(skip);
        p.emit(Op::kRet);
    });
    FakeHost host;
    PreviewRecorder sink;
    REQUIRE(preview(prog, host, sink).ok);
    CHECK(balanced(sink.events()));
    int groups = 0;
    for (const PreviewEvent& ev : sink.events()) {
        groups += ev.type == PreviewEvent::Type::GroupBegin ? 1 : 0;
    }
    CHECK(groups == 3);  // the chance block and its two branches
    // in Run the same program takes two draws when the chance hits and one when it misses
    for (std::uint64_t seed = 1; seed <= 20; ++seed) {
        FakeHost h(seed);
        const RunResult r = run(prog, h, Kind::Effect);
        REQUIRE(r.ok);
        CHECK((r.counters.draws == 1 || r.counters.draws == 2));
        CHECK((r.counters.draws == 2) == !r.commands.empty());
    }
}

TEST_CASE("events vm: Preview aborts under the same limits as Run") {
    FakeHost host;
    PreviewRecorder sink;
    const Program prog = build([](ProgramBuilder& p) {
        for (int i = 0; i < 3; ++i) {
            p.emit(Op::kEmit, 1, 0);
        }
        p.emit(Op::kRet);
    });
    Vm vm;
    RunOptions options;
    options.mode = Mode::Preview;
    options.kind = Kind::Effect;
    options.max_commands = 2;
    options.preview = &sink;
    const RunResult r = vm.run(prog, host, options);
    CHECK_FALSE(r.ok);
    CHECK(r.reason == AbortReason::Commands);
}

// ---- determinism -------------------------------------------------------------------------------

TEST_CASE("events vm: one program run twice gives the same bytes, also on a reused machine") {
    const Program prog = combined_program();
    Vm shared;
    for (std::uint64_t seed = 1; seed <= 50; ++seed) {
        FakeHost a(seed);
        FakeHost b(seed);
        FakeHost c(seed);
        RunOptions options;
        options.kind = Kind::Effect;
        options.root = ch(1);
        const RunResult first = shared.run(prog, a, options);
        Vm fresh;
        const RunResult second = fresh.run(prog, b, options);
        const RunResult third = shared.run(prog, c, options);
        REQUIRE(first.ok);
        CHECK(first.to_bytes() == second.to_bytes());
        CHECK(first.to_bytes() == third.to_bytes());
        CHECK(first.hash() == second.hash());
        CHECK(a.rng_ == b.rng_);
    }
}

TEST_CASE("events vm: golden result of a fixed program (guards the arithmetic and the draw order)") {
    // Pinned on purpose: a change of the saturation rules, of the weighted choice scaling or of the
    // order of draws changes this value, and with it every event of the game. Update only together
    // with docs/dev/event-engine.md section 7.3.
    FakeHost host(4242);
    const Program prog = combined_program();
    const RunResult r = run(prog, host, Kind::Effect);
    REQUIRE(r.ok);
    CHECK(prog.hash() == 0xAA9F0FD9490CBDDDULL);
    CHECK(r.hash() == 0x5F3F7E9D00EFBA28ULL);
}

TEST_CASE("events vm: canonical bytes of a Program and of a RunResult are pinned byte for byte") {
    // Program::hash and RunResult::hash feed EventSet::hash (key 21 of the save summary), so the
    // exact byte layout is part of the format: little-endian, fixed widths, no padding.
    const auto bytes_of = [](std::initializer_list<unsigned> values) {
        std::vector<std::byte> out;
        for (const unsigned v : values) {
            out.push_back(static_cast<std::byte>(v));
        }
        return out;
    };

    Program program;
    program.code.push_back(Instr{0x0102, 0x0304, 0x05060708});
    program.consts.push_back(-2);
    CHECK(program.to_bytes() == bytes_of({0x01, 0x00, 0x00, 0x00,  // code count
                                          0x02, 0x01, 0x04, 0x03,  // op, a
                                          0x08, 0x07, 0x06, 0x05,  // b
                                          0x01, 0x00, 0x00, 0x00,  // const count
                                          0xFE, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}));

    RunResult result;
    result.ok = true;
    result.reason = AbortReason::Explicit;
    result.abort_pc = 1;
    result.abort_detail = 2;
    result.has_value = true;
    result.value = -1;
    result.instructions = 3;
    result.wu = 4;
    result.counters = RunCounters{5, 6, 7, 8};
    const std::array<Word, 2> args{0x0A, -1};
    result.commands.add(0x1234, Scope{ScopeKind::Province, 9}, args);
    CHECK(result.to_bytes() == bytes_of({0x45, 0x56, 0x52, 0x31,                          // "EVR1"
                                         0x01,                                            // ok
                                         0x08, 0x00, 0x00, 0x00,                          // reason
                                         0x01, 0x00, 0x00, 0x00,                          // abort_pc
                                         0x02, 0x00, 0x00, 0x00,                          // abort_detail
                                         0x01,                                            // has_value
                                         0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,  // value
                                         0x03, 0x00, 0x00, 0x00,                          // instructions
                                         0x04, 0x00, 0x00, 0x00,                          // wu
                                         0x05, 0x00, 0x00, 0x00,                          // saturations
                                         0x06, 0x00, 0x00, 0x00,                          // div_zero
                                         0x07, 0x00, 0x00, 0x00,                          // truncated
                                         0x08, 0x00, 0x00, 0x00,                          // draws
                                         0x01, 0x00, 0x00, 0x00,                          // commands: count
                                         0x34, 0x12, 0x00, 0x00,                          // code
                                         0x03, 0x00, 0x00, 0x00,                          // target kind
                                         0x09, 0x00, 0x00, 0x00,                          // target id
                                         0x02, 0x00, 0x00, 0x00,                          // argc
                                         0x0A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // arg 0
                                         0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,  // arg 1
                                         0x00, 0x00, 0x00, 0x00}));                       // state_ops: count
}

// ---- program: validation, disassembler, cost -----------------------------------------------------

TEST_CASE("events program: validate rejects malformed programs with a precise error") {
    const auto code_of = [](Program prog) {
        const auto result = validate(prog);
        REQUIRE_FALSE(result.has_value());
        return result.error().code;
    };
    const auto make = [](std::initializer_list<Instr> code, std::vector<std::int64_t> consts = {}) {
        Program prog;
        prog.code.assign(code.begin(), code.end());
        prog.consts = std::move(consts);
        return prog;
    };
    CHECK(code_of(make({})) == ProgramErrorCode::FallsOffEnd);
    CHECK(code_of(make({Instr{999, 0, 0}, make_instr(Op::kRet)})) == ProgramErrorCode::BadOpcode);
    CHECK(code_of(make({make_instr(Op::kJmp, 0, 50)})) == ProgramErrorCode::BadJump);
    CHECK(code_of(make({make_instr(Op::kPushConst, 0, 0), make_instr(Op::kRet)})) ==
          ProgramErrorCode::BadConst);
    CHECK(code_of(make({make_instr(Op::kAdd), make_instr(Op::kRet)})) == ProgramErrorCode::StackUnderflow);
    CHECK(code_of(make({make_instr(Op::kPushInt, 0, 1)})) == ProgramErrorCode::FallsOffEnd);
    CHECK(code_of(make({make_instr(Op::kLoad, 1, 9), make_instr(Op::kRet)})) == ProgramErrorCode::BadOperand);
    CHECK(code_of(make({make_instr(Op::kScopeSave, 8), make_instr(Op::kRet)})) ==
          ProgramErrorCode::BadOperand);
    CHECK(code_of(make({make_instr(Op::kRandPick, 9, 0), make_instr(Op::kRet)})) ==
          ProgramErrorCode::BadOperand);
    CHECK(code_of(
              make({make_instr(Op::kPushInt, 0, 1), make_instr(Op::kPushInt, 0, 2), make_instr(Op::kRet)})) ==
          ProgramErrorCode::StackMismatch);  // RET with two values
    CHECK(code_of(make({make_instr(Op::kJTable, 2, 0), make_instr(Op::kRet), make_instr(Op::kRet)})) ==
          ProgramErrorCode::BadTable);  // the entries are not JMP
    CHECK(code_of(make(
              {make_instr(Op::kIterBegin, 1, 1), make_instr(Op::kIterNext, 0, 0), make_instr(Op::kRet)})) ==
          ProgramErrorCode::BadJump);  // must point at ITER_END

    SUBCASE("stack depth must agree on every path") {
        ProgramBuilder b;
        const auto other = b.new_label();
        b.push_bool(true);
        b.emit_jump(Op::kJf, other);
        b.push_int(1);  // one path leaves a value, the other does not
        b.bind(other);
        b.emit(Op::kRet);
        const auto result = b.finish();
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == ProgramErrorCode::StackMismatch);
    }
    SUBCASE("stack deeper than kMaxStack") {
        ProgramBuilder b;
        for (std::size_t i = 0; i <= limits::kMaxStack; ++i) {
            b.push_int(1);
        }
        b.emit(Op::kRet);
        const auto result = b.finish();
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == ProgramErrorCode::StackOverflow);
    }
    SUBCASE("an unbound label") {
        ProgramBuilder b;
        const auto never = b.new_label();
        b.emit_jump(Op::kJmp, never);
        const auto result = b.finish();
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == ProgramErrorCode::UnboundLabel);
    }
    SUBCASE("a program over kMaxProgramBytes") {
        Program big;
        for (std::size_t i = 0; i < limits::kMaxProgramBytes / sizeof(Instr) + 1; ++i) {
            big.code.push_back(make_instr(Op::kNop));
        }
        big.code.push_back(make_instr(Op::kRet));
        CHECK(code_of(big) == ProgramErrorCode::TooLong);
    }
}

TEST_CASE("events program: validate computes max_stack and the cost estimate") {
    const Program prog = build([](ProgramBuilder& p) {
        p.push_int(1);
        p.push_int(2);
        p.push_int(3);
        p.emit(Op::kAdd);
        p.emit(Op::kAdd);
        p.emit(Op::kRet);
    });
    CHECK(prog.max_stack == 3);
    CHECK(prog.estimated_wu == 3 + 1 + 1);

    class Costly final : public CatalogInfo {
    public:
        std::uint32_t cost(Op, std::uint16_t) const override { return 4; }
        std::uint32_t list_bound(std::uint16_t) const override { return 10; }
    } costly;
    const Program loop = build([](ProgramBuilder& p) {
        each(p, 1, [](ProgramBuilder& q) { q.emit(Op::kEmit, 7, 0); });
        p.emit(Op::kRet);
    });
    // ITER_BEGIN 4 + 10, then per element NEXT 2 + EMIT 8 + JMP 1, then ITER_END 1, RET 0
    CHECK(estimate_cost(loop, costly) == 14 + 10 * 11 + 1);
    // the default catalog assumes 32 elements per list
    CHECK(loop.estimated_wu == 4 + 32 + 32 * 11 + 1);

    // a nested loop multiplies
    const Program nested = build([](ProgramBuilder& p) {
        each(p, 1, [](ProgramBuilder& a) { each(a, 1, [](ProgramBuilder& b) { b.emit(Op::kEmit, 7, 0); }); });
        p.emit(Op::kRet);
    });
    CHECK(estimate_cost(nested, costly) > 10 * 10 * 8);
    // the run itself never exceeds the estimate
    FakeHost host;
    const RunResult r = run(loop, host, Kind::Effect, Mode::Run, dyn(10));
    CHECK(r.ok);
    CHECK(r.wu <= loop.estimated_wu);
}

TEST_CASE("events program: the disassembler lists offsets, targets, constants and names") {
    class Names final : public CatalogInfo {
    public:
        std::string_view name(NameSpace space, std::uint16_t code) const override {
            if (space == NameSpace::Value && code == kHonor) {
                return "honor";
            }
            if (space == NameSpace::Effect && code == 7) {
                return "add_prestige";
            }
            return {};
        }
    } names;
    ProgramBuilder b;
    const auto skip = b.new_label();
    b.set_source(2, 14, 9);
    b.emit(Op::kLoad, kHonor, 0);
    b.push_int(30);
    b.emit(Op::kGe);
    b.emit_jump(Op::kJf, skip);
    b.push_fixed(Fixed64::from_ratio(5, 2));
    b.emit(Op::kEmit, 7, 1);
    b.bind(skip);
    b.emit(Op::kRet);
    const Program prog = std::move(b.finish()).value();
    const std::string text = disassemble(prog, &names);
    CHECK(text.find("PUSH_CONST") != std::string::npos);
    CHECK(text.find("-> 0006") != std::string::npos);
    CHECK(text.find("honor") != std::string::npos);
    CHECK(text.find("add_prestige") != std::string::npos);
    CHECK(text.find("2.5000") != std::string::npos);
    CHECK(text.find("@2:14:9") != std::string::npos);
    CHECK(text.find(">0006  RET") != std::string::npos);
    // without a catalog the codes stay numeric
    CHECK(disassemble(prog).find("honor") == std::string::npos);
    INFO(text);
    const std::string expected = "; 7 instructions, 1 constants, max stack 2, estimated 14 WU\n"
                                 " 0000  LOAD          5 argc 0           ; honor  @2:14:9\n"
                                 " 0001  PUSH_INT      30\n"
                                 " 0002  GE\n"
                                 " 0003  JF            -> 0006\n"
                                 " 0004  PUSH_CONST    #0                 ; = 2.5000\n"
                                 " 0005  EMIT          7 argc 1           ; add_prestige\n"
                                 ">0006  RET\n";
    CHECK(text == expected);
}

TEST_CASE("events program: bytes, hash and the debug table") {
    ProgramBuilder a;
    a.set_source(1, 3, 4);
    a.push_int(1);
    a.emit(Op::kRet);
    const Program pa = std::move(a.finish()).value();
    ProgramBuilder b;
    b.push_int(1);
    b.emit(Op::kRet);
    const Program pb = std::move(b.finish()).value();
    CHECK(pa.hash() == pb.hash());  // the debug table is not part of the identity
    CHECK(pa.to_bytes() == pb.to_bytes());
    ProgramBuilder c;
    c.push_int(2);
    c.emit(Op::kRet);
    CHECK(std::move(c.finish()).value().hash() != pa.hash());

    REQUIRE(pa.source_at(0) != nullptr);
    CHECK(pa.source_at(0)->line == 3);
    CHECK(pa.source_at(1)->column == 4);  // the last record at or before the pc
    CHECK(pb.source_at(0) == nullptr);

    // a constant is pooled once
    ProgramBuilder d;
    d.push_fixed(Fixed64::from_ratio(1, 3));
    d.push_fixed(Fixed64::from_ratio(1, 3));
    d.emit(Op::kAdd);
    d.emit(Op::kRet);
    CHECK(std::move(d.finish()).value().consts.size() == 1);
}

// ---- breakdown steps ---------------------------------------------------------------------------

TEST_CASE("events vm: accumulator steps build an additive Breakdown") {
    // ai_chance = { base = 20  add = honor / 5  subtract = 15  min = 0  max = 100 }, a STEP after each
    const Program prog = build([](ProgramBuilder& p) {
        p.push_int(20);
        p.emit(Op::kStep, 13001);
        p.emit(Op::kLoad, kHonor, 0);
        p.push_int(5);
        p.emit(Op::kDiv);
        p.emit(Op::kAdd);
        p.emit(Op::kStep, 13002);
        p.push_int(15);
        p.emit(Op::kSub);
        p.emit(Op::kStep, 13003);
        p.push_int(0);
        p.push_int(100);
        p.emit(Op::kClamp);
        p.emit(Op::kRet);
    });
    FakeHost host;
    BreakdownRecorder recorder(13000, 13099);
    Vm vm;
    RunOptions options;
    options.kind = Kind::Value;
    options.root = ch(1);
    options.tracer = &recorder;
    const RunResult r = vm.run(prog, host, options);
    REQUIRE(r.ok);
    CHECK(recorder.step_count() == 3);
    const economy::Breakdown tree = recorder.build(r.fixed());
    CHECK(tree.is_consistent());
    CHECK(tree.root().code == 13000);
    CHECK(tree.node(1).code == 13001);
    CHECK(tree.node(1).value == 20 * 65536);  // base 20, Q16.16 points
    CHECK(tree.node(2).code == 13002);        // + 22 / 5 = 4.4
    CHECK(tree.node(3).code == 13003);        // - 15
    CHECK(tree.node(3).value == -15 * 65536);
    CHECK(tree.total() == (r.value >> 16));
    // the steps are reported in Run too and do not change the result
    FakeHost plain_host;
    CHECK(run(prog, plain_host, Kind::Value).value == r.value);

    // a clamp that no step accounts for becomes the remainder term
    const Program clamped = build([](ProgramBuilder& p) {
        p.push_int(250);
        p.emit(Op::kStep, 13001);
        p.push_int(0);
        p.push_int(100);
        p.emit(Op::kClamp);
        p.emit(Op::kRet);
    });
    BreakdownRecorder rec2(13000, 13099);
    options.tracer = &rec2;
    const RunResult r2 = vm.run(clamped, host, options);
    const economy::Breakdown tree2 = rec2.build(r2.fixed());
    CHECK(tree2.is_consistent());
    CHECK(tree2.total() == 100 * 65536);
    CHECK(tree2.node(2).code == 13099);
    CHECK(tree2.node(2).value == -150 * 65536);

    // more steps than the tree can hold fold into the remainder and the tree stays additive
    ProgramBuilder many;
    many.push_int(0);
    for (int i = 0; i < 40; ++i) {
        many.push_int(1);
        many.emit(Op::kAdd);
        many.emit(Op::kStep, static_cast<std::uint16_t>(13100 + i));
    }
    many.emit(Op::kRet);
    const Program long_prog = std::move(many.finish()).value();
    BreakdownRecorder rec3(13000, 13099);
    options.tracer = &rec3;
    const RunResult r3 = vm.run(long_prog, host, options);
    const economy::Breakdown tree3 = rec3.build(r3.fixed());
    CHECK(tree3.is_consistent());
    CHECK(tree3.total() == 40 * 65536);
}

// ---- grep test: no float, no unordered containers, no C rand -------------------------------------

TEST_CASE("events vm: the module has no floating point, unordered containers, C rand or clocks") {
    namespace fs = std::filesystem;
    const fs::path root(PATRIARCH_SOURCE_DIR);
    const std::regex banned(
        R"(\b(float|double|unordered_\w+|rand|srand|random_device|mt19937|chrono|system_clock|steady_clock|time_t)\b)");
    int files = 0;
    for (const char* dir : {"src/sim/events", "src/sim/include/pat/sim/events"}) {
        REQUIRE(fs::exists(root / dir));
        for (const auto& entry : fs::directory_iterator(root / dir)) {
            const std::string ext = entry.path().extension().string();
            if (ext != ".cpp" && ext != ".hpp") {
                continue;
            }
            ++files;
            std::istringstream in(read_text(entry.path()));
            std::string line;
            int number = 0;
            while (std::getline(in, line)) {
                ++number;
                const auto comment = line.find("//");
                if (comment != std::string::npos) {
                    line.erase(comment);
                }
                std::smatch m;
                if (std::regex_search(line, m, banned)) {
                    FAIL_CHECK(entry.path().filename().string() << ":" << number << ": " << m.str());
                }
            }
        }
    }
    CHECK(files >= 5);
}

// ---- random programs against a reference evaluator -----------------------------------------------

namespace {

struct Expr;
using ExprPtr = std::unique_ptr<Expr>;

struct Expr {
    enum class Type { Const, Add, Sub, Mul, Div, Neg, Min, Max, Clamp, Trunc, Select };
    Type type = Type::Const;
    Word value = 0;
    Op cmp = Op::kLt;
    ExprPtr a;
    ExprPtr b;
    ExprPtr c;
    ExprPtr d;
};

Word random_constant(Pcg32& rng) {
    static const std::array<Word, 22> pool = {
        0,         1,         -1,        W(1),         W(-1),         W(2),       W(-3),         WR(1, 3),
        WR(-7, 2), W(1000),   W(-1000),  W(1 << 20),   W(-(1 << 20)), W(1 << 30), W(-(1 << 30)), kWMax,
        kWMin,     kWMax - 1, kWMin + 1, WR(1, 65536), WR(-1, 65536), 2};
    if (rng.bounded(5) == 0) {
        return static_cast<Word>(rng.next_u64());
    }
    return pool[rng.bounded(static_cast<std::uint32_t>(pool.size()))];
}

ExprPtr random_expr(Pcg32& rng, int depth) {
    auto e = std::make_unique<Expr>();
    if (depth == 0 || rng.bounded(5) == 0) {
        e->type = Expr::Type::Const;
        e->value = random_constant(rng);
        return e;
    }
    static const std::array<Expr::Type, 10> types = {Expr::Type::Add,
                                                     Expr::Type::Sub,
                                                     Expr::Type::Mul,
                                                     Expr::Type::Div,
                                                     Expr::Type::Neg,
                                                     Expr::Type::Min,
                                                     Expr::Type::Max,
                                                     Expr::Type::Clamp,
                                                     Expr::Type::Trunc,
                                                     Expr::Type::Select};
    e->type = types[rng.bounded(static_cast<std::uint32_t>(types.size()))];
    e->a = random_expr(rng, depth - 1);
    switch (e->type) {
        case Expr::Type::Neg:
        case Expr::Type::Trunc:
            break;
        case Expr::Type::Clamp:
            e->b = random_expr(rng, depth - 1);
            e->c = random_expr(rng, depth - 1);
            break;
        case Expr::Type::Select: {
            static const std::array<Op, 6> cmps = {Op::kEq, Op::kNe, Op::kLt, Op::kLe, Op::kGt, Op::kGe};
            e->cmp = cmps[rng.bounded(6)];
            e->b = random_expr(rng, depth - 1);
            e->c = random_expr(rng, depth - 1);
            e->d = random_expr(rng, depth - 1);
            break;
        }
        default:
            e->b = random_expr(rng, depth - 1);
            break;
    }
    return e;
}

void compile_expr(ProgramBuilder& p, const Expr& e) {
    switch (e.type) {
        case Expr::Type::Const:
            p.push_fixed(Fixed64::from_raw(e.value));
            return;
        case Expr::Type::Neg:
            compile_expr(p, *e.a);
            p.emit(Op::kNeg);
            return;
        case Expr::Type::Trunc:
            compile_expr(p, *e.a);
            p.emit(Op::kTrunc);
            return;
        case Expr::Type::Clamp:
            compile_expr(p, *e.a);
            compile_expr(p, *e.b);
            compile_expr(p, *e.c);
            p.emit(Op::kClamp);
            return;
        case Expr::Type::Select: {
            const auto otherwise = p.new_label();
            const auto end = p.new_label();
            compile_expr(p, *e.a);
            compile_expr(p, *e.b);
            p.emit(e.cmp);
            p.emit_jump(Op::kJf, otherwise);
            compile_expr(p, *e.c);
            p.emit_jump(Op::kJmp, end);
            p.bind(otherwise);
            compile_expr(p, *e.d);
            p.bind(end);
            return;
        }
        default:
            break;
    }
    compile_expr(p, *e.a);
    compile_expr(p, *e.b);
    switch (e.type) {
        case Expr::Type::Add:
            p.emit(Op::kAdd);
            break;
        case Expr::Type::Sub:
            p.emit(Op::kSub);
            break;
        case Expr::Type::Mul:
            p.emit(Op::kMul);
            break;
        case Expr::Type::Div:
            p.emit(Op::kDiv);
            break;
        case Expr::Type::Min:
            p.emit(Op::kMin);
            break;
        default:
            p.emit(Op::kMax);
            break;
    }
}

// Reference: the saturating operations of pat::core::Fixed64 and plain integer logic; the
// division checks the overflow condition |a| / 2^31 < |b| itself.
Word reference_eval(const Expr& e) {
    const auto fx = [](Word w) { return Fixed64::from_raw(w); };
    const auto mag = [](Word w) {
        return w < 0 ? 0ULL - static_cast<std::uint64_t>(w) : static_cast<std::uint64_t>(w);
    };
    switch (e.type) {
        case Expr::Type::Const:
            return e.value;
        case Expr::Type::Add:
            return saturating_add(fx(reference_eval(*e.a)), fx(reference_eval(*e.b))).raw();
        case Expr::Type::Sub:
            return saturating_sub(fx(reference_eval(*e.a)), fx(reference_eval(*e.b))).raw();
        case Expr::Type::Mul:
            return saturating_mul(fx(reference_eval(*e.a)), fx(reference_eval(*e.b))).raw();
        case Expr::Type::Div: {
            const Word a = reference_eval(*e.a);
            const Word b = reference_eval(*e.b);
            if (b == 0) {
                return 0;
            }
            if ((mag(a) >> 31) >= mag(b)) {
                return ((a < 0) != (b < 0)) ? kWMin : kWMax;
            }
            return (fx(a) / fx(b)).raw();
        }
        case Expr::Type::Neg: {
            const Word a = reference_eval(*e.a);
            return a == kWMin ? kWMax : -a;
        }
        case Expr::Type::Min:
            return std::min(reference_eval(*e.a), reference_eval(*e.b));
        case Expr::Type::Max:
            return std::max(reference_eval(*e.a), reference_eval(*e.b));
        case Expr::Type::Clamp: {
            const Word x = reference_eval(*e.a);
            const Word lo = reference_eval(*e.b);
            const Word hi = reference_eval(*e.c);
            return x < lo ? lo : (x > hi ? (hi < lo ? lo : hi) : x);  // hi < lo: lo wins, as documented
        }
        case Expr::Type::Trunc:
            return (reference_eval(*e.a) / Fixed64::one_raw) * Fixed64::one_raw;
        case Expr::Type::Select: {
            const Word l = reference_eval(*e.a);
            const Word r = reference_eval(*e.b);
            bool hit = false;
            switch (e.cmp) {
                case Op::kEq:
                    hit = l == r;
                    break;
                case Op::kNe:
                    hit = l != r;
                    break;
                case Op::kLt:
                    hit = l < r;
                    break;
                case Op::kLe:
                    hit = l <= r;
                    break;
                case Op::kGt:
                    hit = l > r;
                    break;
                default:
                    hit = l >= r;
                    break;
            }
            return hit ? reference_eval(*e.c) : reference_eval(*e.d);
        }
    }
    return 0;
}

}  // namespace

TEST_CASE("events vm: random valid programs agree with the reference evaluator, in Run and in Trace") {
    Pcg32 rng(2026, 11);
    Vm vm;
    FakeHost host;
    int saturated_runs = 0;
    for (int i = 0; i < 1500; ++i) {
        const ExprPtr tree = random_expr(rng, 1 + static_cast<int>(rng.bounded(5)));
        const Program prog = build([&](ProgramBuilder& p) {
            compile_expr(p, *tree);
            p.emit(Op::kRet);
        });
        const Word expected = reference_eval(*tree);
        RunOptions options;
        options.kind = Kind::Value;
        const RunResult r = vm.run(prog, host, options);
        REQUIRE(r.ok);
        REQUIRE(r.has_value);
        CHECK(r.value == expected);
        saturated_runs += r.counters.saturations > 0 ? 1 : 0;
        options.mode = Mode::Trace;
        const RunResult t = vm.run(prog, host, options);
        CHECK(t.value == expected);
        CHECK(t.counters == r.counters);
        CHECK(r.counters.draws == 0);
        if (r.value != expected) {
            break;  // one failing program is enough of a report
        }
    }
    CHECK(saturated_runs > 50);  // the generator does reach the saturation paths
}

// ---- benchmark (skipped by default: run with --no-skip -tc="events vm: bench*") -------------------

TEST_CASE("events vm: bench: one trigger of about 100 instructions" * doctest::skip()) {
    // honor >= 1 and honor >= 2 and ... : a chain of 20 comparisons joined by AND with short circuit
    ProgramBuilder b;
    const auto end = b.new_label();
    for (int i = 1; i <= 20; ++i) {
        b.emit(Op::kLoad, kHonor, 0);
        b.push_int(i);
        b.emit(Op::kGe);
        if (i < 20) {
            b.emit_jump(Op::kJfKeep, end);
            b.emit(Op::kDrop);
        }
    }
    b.bind(end);
    b.emit(Op::kRet);
    const Program prog = std::move(b.finish()).value();
    FakeHost host;
    Vm vm;
    RunOptions options;
    options.root = ch(1);
    constexpr int kSamples = 20000;
    std::vector<std::int64_t> ns;
    ns.reserve(kSamples);
    std::uint32_t instructions = 0;
    for (int i = 0; i < kSamples; ++i) {
        const auto t0 = std::chrono::steady_clock::now();
        const RunResult r = vm.run(prog, host, options);
        const auto t1 = std::chrono::steady_clock::now();
        instructions = r.instructions;
        ns.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
    }
    std::sort(ns.begin(), ns.end());
    MESSAGE("instructions " << instructions << ", p50 " << ns[kSamples / 2] << " ns, p99 "
                            << ns[kSamples * 99 / 100] << " ns, max " << ns.back() << " ns");
}
